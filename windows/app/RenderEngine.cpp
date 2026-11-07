#include "RenderEngine.h"

#include "Exporter.h"
#include "FilmRenderer.h"
#include "Filters.h"
#include "GpuDecoder.h"
#include "Provenance.h"
#include "version.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <rhi/qrhi.h>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX            // windows.h's min and max macros would hide std::min and std::max
#endif
#include <dxgi1_4.h>
#include <wrl/client.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

QColorSpace outputColorSpace(const std::string &name)
{
    using P = QColorSpace::Primaries;
    using T = QColorSpace::TransferFunction;
    if (name == "Display P3")
        return QColorSpace(P::DciP3D65, T::SRgb);
    // the sRGB curve on Rec.2020's primaries, as the pixels are encoded
    // (encode_display), not the profile with a 2.4 power
    if (name == "ITU-R BT.2020")
        return QColorSpace(P::Bt2020, T::SRgb);
    if (name == "ProPhoto RGB")
        return QColorSpace(P::ProPhotoRgb, T::Gamma, 1.80078125f);
    return QColorSpace(QColorSpace::SRgb);
}

namespace {

[[noreturn]] void fail(const QString &what)
{
    throw std::runtime_error(what.toStdString());
}


// The files the RAW decoder does not read: pictures already rendered, read
// colour-managed into the working space, linear Rec.2020, as the Mac reads
// them with Core Image.
bool isRendered(const QString &path)
{
    static const QStringList kinds{"jpg", "jpeg", "png", "tif", "tiff", "bmp", "webp", "heic", "heif"};
    return kinds.contains(QFileInfo(path).suffix().toLower());
}

DecodedPicture readRendered(const QString &path, int maxDimension)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull())
        fail(QStringLiteral("cannot read %1: %2").arg(QFileInfo(path).fileName(), reader.errorString()));
    if (!image.colorSpace().isValid())
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    if (maxDimension > 0 && std::max(image.width(), image.height()) > maxDimension)
        image = image.scaled(QSize(maxDimension, maxDimension), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    image = image.convertToFormat(QImage::Format_RGBA32FPx4);
    image.convertToColorSpace(QColorSpace(QColorSpace::Primaries::Bt2020, QColorSpace::TransferFunction::Linear));
    DecodedPicture picture;
    picture.pixels.width = image.width();
    picture.pixels.height = image.height();
    picture.pixels.channels = 4;
    picture.pixels.floats.resize(picture.pixels.count());
    for (int y = 0; y < image.height(); ++y)
        std::memcpy(&picture.pixels.floats[size_t(y) * image.width() * 4], image.constScanLine(y),
                    size_t(image.width()) * 16);
    for (size_t i = 3; i < picture.pixels.floats.size(); i += 4)
        picture.pixels.floats[i] = 1;
    return picture;
}

} // namespace

// A GPU device for compute: Metal on a Mac; Direct3D 12 on Windows, else
// Direct3D 11, else Direct3D 11's software rasteriser.
std::unique_ptr<QRhi> makeDevice()
{
#if defined(Q_OS_MACOS)
    QRhiMetalInitParams metal;
    if (QRhi *rhi = QRhi::create(QRhi::Metal, &metal))
        return std::unique_ptr<QRhi>(rhi);
#elif defined(Q_OS_WIN)
    QRhiD3D12InitParams d3d12;
    if (QRhi *rhi = QRhi::create(QRhi::D3D12, &d3d12))
        return std::unique_ptr<QRhi>(rhi);
    QRhiD3D11InitParams d3d11;
    if (QRhi *rhi = QRhi::create(QRhi::D3D11, &d3d11))
        return std::unique_ptr<QRhi>(rhi);
    if (QRhi *rhi = QRhi::create(QRhi::D3D11, &d3d11, QRhi::PreferSoftwareRenderer))
        return std::unique_ptr<QRhi>(rhi);
#endif
    return nullptr;
}

RenderEngine::RenderEngine(AppData data)
    : m_data(std::move(data))
{
}

// The GPU memory the app may still take, in bytes: the budget Windows gives
// it on the adapter it computes on, less what it holds; -1 where that is not
// known (a Mac's memory, shared with the CPU). FLX_GPU_BUDGET_MB caps the
// budget, to try a smaller card.
qint64 gpuMemoryFree(QRhi *rhi)
{
    qint64 budget = -1, used = 0;
#if defined(Q_OS_WIN)
    LUID luid{};
    bool known = false;
    if (rhi->backend() == QRhi::D3D12) {
        const auto *h = static_cast<const QRhiD3D12NativeHandles *>(rhi->nativeHandles());
        luid.LowPart = h->adapterLuidLow;
        luid.HighPart = h->adapterLuidHigh;
        known = true;
    } else if (rhi->backend() == QRhi::D3D11) {
        const auto *h = static_cast<const QRhiD3D11NativeHandles *>(rhi->nativeHandles());
        luid.LowPart = h->adapterLuidLow;
        luid.HighPart = h->adapterLuidHigh;
        known = true;
    }
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (known && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))
        && SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))
        && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        budget = qint64(info.Budget);
        used = qint64(info.CurrentUsage);
    }
#else
    Q_UNUSED(rhi);
#endif
    if (const qint64 cap = qEnvironmentVariableIntValue("FLX_GPU_BUDGET_MB"); cap > 0)
        budget = budget < 0 ? cap << 20 : std::min(budget, cap << 20);
    return budget < 0 ? -1 : std::max<qint64>(budget - used, 0);
}

RenderEngine::~RenderEngine()
{
    // the GPU's resources before its device
    m_loupe.renderer.reset();
    m_renderer.reset();
    m_decoder.reset();
}

void RenderEngine::ensureDevice()
{
    if (m_rhi)
        return;
    m_packs = std::make_unique<PackLibrary>(m_data.packs);
    m_builder = std::make_unique<SetupBuilder>(*m_packs);
    m_rhi = makeDevice();
    if (!m_rhi)
        fail("This computer has no GPU the app can compute on.");
    m_decoder = std::make_unique<GpuDecoder>(m_rhi.get());
    m_renderer = std::make_unique<FilmRenderer>(m_rhi.get(), FilmRenderer::Blur::Own);
    m_renderer->setSkipsUnchangedFilm(true);
    m_renderer->setMeasuresHistogram(true);
}

QString RenderEngine::backendName() const
{
    return m_rhi ? QString::fromLatin1(m_rhi->backendName()) : QString();
}

DecodedPicture RenderEngine::decode(const DecodeKey &key)
{
    ensureDevice();
    if (isRendered(key.path))
        return readRendered(key.path, key.maxDimension);
    DecodeOptions options;
    options.whiteBalance = key.whiteBalance;
    options.reference = key.reference;
    options.temperature = float(key.temperature);
    options.tint = float(key.tint);
    options.maxDimension = key.maxDimension;
    options.appleColour = m_data.appleColour.toStdString();
    options.lensfunDir = m_data.lensfun.toStdString();
    options.camerasXml = m_data.camerasXml.toStdString();
    QElapsedTimer clock;
    clock.start();
    DecodePlan plan = planDecode(QFileInfo(key.path).absoluteFilePath().toStdString(), options);
    const double planned = double(clock.nsecsElapsed()) / 1e6;
    double gpu = 0;
    // The GPU's decode holds about 150 bytes a pixel of the mosaic at once,
    // 7 GB for a 45 MP sensor: where the card has not that much free, the
    // CPU decodes it, by the same arithmetic.
    const qint64 need = qint64(plan.width) * plan.height * 160, free = gpuMemoryFree(m_rhi.get());
    const bool onCPU = free >= 0 && need > free * 8 / 10;
    DecodedPicture picture = onCPU ? decodePlan(std::move(plan)) : m_decoder->decode(std::move(plan), &gpu);
    if (onCPU && qEnvironmentVariableIsSet("FLX_TIMING"))
        std::fprintf(stderr, "decode on the CPU: it needs %lld MB of the GPU's, which has %lld MB free\n",
                     need >> 20, free >> 20);
    if (qEnvironmentVariableIsSet("FLX_TIMING"))
        std::fprintf(stderr, "decode %s: plan %.0f ms, pixels %.0f ms (GPU frames %.0f ms)\n",
                     qPrintable(QFileInfo(key.path).fileName()), planned, double(clock.nsecsElapsed()) / 1e6 - planned,
                     gpu * 1e3);
    return picture;
}

RenderEngine::Source RenderEngine::setPicture(DecodedPicture &&picture, const QString &path)
{
    ensureDevice();
    m_picture = std::move(picture.pixels);
    m_fileName = QFileInfo(path).fileName().toStdString();
    m_renderer->setPicture(m_picture.floats.data(), m_picture.width, m_picture.height);
    m_exposureReading.reset();
    m_printReading.reset();
    m_meteredArea.reset();
    m_lastMasks.reset();
    m_coverages.clear();
    m_renderer->setDodgeBurn({}, 0, 0);
    ++m_sourceID;
    QString camera = QString::fromStdString(picture.mosaic.make + " " + picture.mosaic.model).trimmed();
    return {m_sourceID, m_picture.width, m_picture.height, camera};
}

const Dump &RenderEngine::meteredArea(const FrameGeometry &upright)
{
    if (m_meteredArea && m_meteredArea->geometry == upright)
        return m_meteredArea->pixels;
    const GeometryMap map(upright, m_picture.width, m_picture.height);
    if (map.isIdentity)
        return m_picture;
    MeteredArea area{upright, {}};
    area.pixels.channels = 4;
    area.pixels.floats = map.resample(m_picture.floats, std::max(m_picture.width, m_picture.height),
                                      area.pixels.width, area.pixels.height);
    m_meteredArea = std::move(area);
    return m_meteredArea->pixels;
}

std::pair<Recipe, Metered> RenderEngine::resolve(const Recipe &recipe, const std::optional<FrameGeometry> &metering)
{
    Metered metered;
    if (m_picture.floats.empty())
        return {recipe, metered};
    Recipe working = recipe;
    const FrameGeometry geometry = metering.value_or(recipe.geometry).upright();
    if (recipe.autoExposure) {
        if (!m_exposureReading || m_exposureReading->stock != recipe.filmStock
            || m_exposureReading->geometry != geometry)
            m_exposureReading = ExposureReading{
                recipe.filmStock, geometry,
                m_builder->autoExposureEV(meteredArea(geometry), m_packs->pack(recipe.filmStock))};
        working.exposureEV = double(m_exposureReading->ev);
        metered.exposureEV = working.exposureEV;
    }
    if (!working.autoDensity && !working.autoGrade)
        return {working, metered};
    const PrintReadingKey key{working.filmStock, working.printStock, working.lensFilter, working.scanFilm,
                              working.exposureEV, working.filterShiftY, working.filterShiftM,
                              working.densityCurveGamma, geometry};
    if (!m_printReading || m_printReading->first != key)
        m_printReading.emplace(key, m_builder->readPrint(working, meteredArea(geometry),
                                                         float(working.exposureEV)));
    if (!m_printReading->second)
        return {working, metered};
    Recipe resolved = m_builder->applyAuto(working, *m_printReading->second);
    if (resolved.autoDensity)
        metered.brightness = resolved.brightness;
    if (resolved.autoGrade)
        metered.grade = resolved.grade;
    return {resolved, metered};
}

BuiltSetup RenderEngine::setupFor(const Recipe &resolved, int width, int height, const std::string &fileName)
{
    BuiltSetup built = m_builder->build(resolved, float(resolved.exposureEV), width, height);
    // the effects' sizes follow the frame, so the preview blooms as the export does
    built.setup.effects = effectsFor(resolved.effects, std::max(width, height));
    built.setup.fileSeed = fnv1a(fileName);
    built.setup.grainSeed = fnv1a(resolved.filmStock) ^ (built.setup.fileSeed * 0x9E3779B9u);
    return built;
}

void RenderEngine::updateMasks(const std::vector<LocalMask> &masks)
{
    if (m_lastMasks && *m_lastMasks == masks)
        return;
    m_lastMasks = masks;
    const maskraster::Size size = maskraster::mapSize(m_picture.width, m_picture.height);
    std::map<std::string, Coverage> kept;
    std::vector<std::pair<const LocalMask *, const std::vector<float> *>> drawn;
    for (const LocalMask &mask : masks) {
        if (!maskraster::isActive(mask))
            continue;
        const maskraster::Geometry geometry(mask);
        auto it = m_coverages.find(mask.id);
        if (it != m_coverages.end() && it->second.geometry == geometry)
            kept.emplace(mask.id, std::move(it->second));
        else
            kept.emplace(mask.id, Coverage{geometry, maskraster::coverage(mask, size.width, size.height)});
    }
    m_coverages = std::move(kept);
    for (const LocalMask &mask : masks)
        if (const auto it = m_coverages.find(mask.id); it != m_coverages.end() && maskraster::isActive(mask))
            drawn.emplace_back(&mask, &it->second.alpha);
    if (drawn.empty())
        m_renderer->setDodgeBurn({}, 0, 0);
    else
        m_renderer->setDodgeBurn(maskraster::combine(drawn, size.width, size.height), size.width, size.height);
}

// GeometryUniforms(map): the print's pixel to the frame's.
void RenderEngine::shape(RenderSetup &s, const GeometryMap &map, bool coverage)
{
    const Affine &t = map.outputToSource;
    s.shape.columns = {float(t.a), float(t.b), float(t.c), float(t.d)};
    s.shape.offsetSize = {float(t.tx), float(t.ty), float(map.sourceWidth), float(map.sourceHeight)};
    s.shape.active = {map.isIdentity ? 0 : 1, 0, 0, 0};
    s.encode.columns = s.shape.columns;
    s.encode.offsetSize = s.shape.offsetSize;
    s.encode.active = s.shape.active;
    s.encode.modes.y = coverage && !map.isIdentity ? 1 : 0;
    s.outputWidth = map.outputWidth();
    s.outputHeight = map.outputHeight();
    s.stages.shaped = !map.isIdentity;
}

std::optional<RenderEngine::Output> RenderEngine::render(const Recipe &recipe, int expected, bool wholeFrame,
                                                         const std::optional<FrameGeometry> &metering)
{
    if (expected != m_sourceID || m_picture.floats.empty())
        return std::nullopt;
    QElapsedTimer clock;
    clock.start();
    Output out;
    std::tie(out.resolved, out.metered) = resolve(recipe, metering);
    const double tResolve = double(clock.nsecsElapsed()) / 1e6;
    out.map = GeometryMap(out.resolved.geometry, m_picture.width, m_picture.height, wholeFrame);
    BuiltSetup built = setupFor(out.resolved, m_picture.width, m_picture.height, m_fileName);
    // while the crop is drawn, the corners levelling leaves empty are transparent
    shape(built.setup, out.map, wholeFrame);
    updateMasks(out.resolved.masks);
    const double tBuild = double(clock.nsecsElapsed()) / 1e6;
    m_renderer->load(built.setup);
    const double tLoad = double(clock.nsecsElapsed()) / 1e6;
    std::vector<uint8_t> display;
    FilmRenderer::Histogram histogram;
    const double gpu = m_renderer->render(&display, nullptr, &histogram) * 1e3;
    out.histogram = PrintHistogram{std::move(histogram.after), std::move(histogram.before)};
    const double tRender = double(clock.nsecsElapsed()) / 1e6;
    const QSize size = m_renderer->outputSize();
    const int w = size.width(), h = size.height();
    const bool transparent = built.setup.encode.modes.y != 0;
    QImage image(w, h, transparent ? QImage::Format_RGBA8888_Premultiplied : QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y)
        std::memcpy(image.scanLine(y), &display[size_t(y) * w * 4], size_t(w) * 4);
    image.setColorSpace(outputColorSpace(out.resolved.outputColorSpace));
    // the screen's colour is sRGB's: what isn't, comes into it
    if (out.resolved.outputColorSpace != "sRGB")
        image.convertToColorSpace(QColorSpace(QColorSpace::SRgb));
    out.image = image.convertToFormat(transparent ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);
    out.milliseconds = double(clock.nsecsElapsed()) / 1e6;
    if (qEnvironmentVariableIsSet("FLX_GPU_STATS")) {
        const QRhiStats st = m_rhi->statistics();
        std::fprintf(stderr, "rhi: %d blocks, %d allocations, %.0f MB used, %.0f MB unused, %.0f MB in all; free %lld MB\n",
                     st.blockCount, st.allocCount, st.usedBytes / 1e6, st.unusedBytes / 1e6, st.totalUsageBytes / 1e6,
                     gpuMemoryFree(m_rhi.get()) >> 20);
    }
    out.breakdown = QStringLiteral("resolve %1, build %2, load %3, frame %4 (GPU+readback %5), image %6")
                        .arg(tResolve, 0, 'f', 1).arg(tBuild - tResolve, 0, 'f', 1).arg(tLoad - tBuild, 0, 'f', 1)
                        .arg(tRender - tLoad, 0, 'f', 1).arg(gpu, 0, 'f', 1).arg(out.milliseconds - tRender, 0, 'f', 1);
    return out;
}

void RenderEngine::exportTo(const Recipe &recipe, const DecodeKey &key, const QString &path, ExportFormat format)
{
    ensureDevice();
    // exactly the values on screen, metered on the same decode
    const Recipe resolved = resolve(recipe).first;
    DecodeKey full = key;
    full.maxDimension = 0;
    DecodedPicture picture = decode(full);
    const int w = picture.pixels.width, h = picture.pixels.height;
    std::vector<float> linear;
    QSize out;
    // The whole print at once where the card holds it; else in tiles, each
    // printed as part of the whole (printPart), which come out as the whole
    // print's pixels there.
    const GeometryMap whole(resolved.geometry, w, h);
    const qint64 framePixels = qint64(w) * h, printPixels = qint64(whole.outputWidth()) * whole.outputHeight();
    const qint64 inter = framePixels > 12'000'000 ? 8 : 16;
    qint64 need = framePixels * (16 + 11 * inter + 16 + 4);
    if (!whole.isIdentity)
        need += printPixels * (5 * inter + 16 + 4);
    if (resolved.effects && resolved.effects->isActive())
        need += printPixels * (6 * inter + 16);
    const qint64 free = gpuMemoryFree(m_rhi.get());
    // FLX_EXPORT_TILES: in tiles whatever the card, for the tests
    if ((free >= 0 && need > free * 8 / 10) || qEnvironmentVariableIsSet("FLX_EXPORT_TILES")) {
        const int pw = whole.outputWidth(), ph = whole.outputHeight();
        // a box as large as half of what is free holds, at about 250 bytes
        // a pixel for the box, a shaped part and the effects
        const int most = free < 0 ? 4096 : std::clamp(int(std::sqrt(double(free) * 0.5 / 250)), 1024, 4096);
        const int tile = std::clamp(most / 2 / 256 * 256, 512, 2048);
        if (qEnvironmentVariableIsSet("FLX_TIMING"))
            std::fprintf(stderr, "export in %d px tiles: the whole print needs %lld MB of the GPU's, which has %lld MB free\n",
                         tile, need >> 20, free >> 20);
        linear.assign(size_t(pw) * ph * 4, 0);
        PartRenderer tiles;
        const std::string fileName = QFileInfo(key.path).fileName().toStdString();
        for (int ty = 0; ty < ph; ty += tile)
            for (int tx = 0; tx < pw; tx += tile) {
                const QRect window(tx, ty, std::min(tile, pw - tx), std::min(tile, ph - ty));
                std::vector<float> part;
                printPart(tiles, picture.pixels, fileName, resolved, window, most, nullptr, &part);
                for (int y = 0; y < window.height(); ++y)
                    std::memcpy(&linear[(size_t(ty + y) * pw + size_t(tx)) * 4], &part[size_t(y) * window.width() * 4],
                                size_t(window.width()) * 16);
            }
        picture.pixels.floats = {};
        out = QSize(pw, ph);
    } else {
        FilmRenderer renderer(m_rhi.get(), FilmRenderer::Blur::Own);
        renderer.setPicture(picture.pixels.floats.data(), w, h);
        picture.pixels.floats = {};
        BuiltSetup built = setupFor(resolved, w, h, QFileInfo(key.path).fileName().toStdString());
        const GeometryMap map(resolved.geometry, w, h);
        shape(built.setup, map, false);
        // the masks at the full size's map, which is as large as the preview's
        const maskraster::Size size = maskraster::mapSize(w, h);
        std::vector<std::vector<float>> coverages;
        std::vector<std::pair<const LocalMask *, const std::vector<float> *>> drawn;
        coverages.reserve(resolved.masks.size());
        for (const LocalMask &mask : resolved.masks)
            if (maskraster::isActive(mask))
                coverages.push_back(maskraster::coverage(mask, size.width, size.height));
        size_t next = 0;
        for (const LocalMask &mask : resolved.masks)
            if (maskraster::isActive(mask))
                drawn.emplace_back(&mask, &coverages[next++]);
        if (!drawn.empty())
            renderer.setDodgeBurn(maskraster::combine(drawn, size.width, size.height), size.width, size.height);
        renderer.load(built.setup);
        renderer.render(nullptr, &linear);
        out = renderer.outputSize();
    }
    // what the source says about the photograph
    const RawMosaic &m = picture.mosaic;
    ExportMetadata metadata;
    metadata.make = QString::fromStdString(m.make);
    metadata.model = QString::fromStdString(m.model);
    metadata.lensMake = QString::fromStdString(m.lensMaker);
    metadata.lensModel = QString::fromStdString(m.lensModel);
    // A lens the file does not name (a manual or adapted one, its focal
    // length and aperture entered in the camera) from its LensInfo, as
    // ImageIO names it and the Mac's export writes it: "28.0 mm f/2.0".
    if (metadata.lensModel.isEmpty() && m.photo.minFocal > 0) {
        const auto number = [](float v) { return QString::number(v, 'f', 1); };
        QString name = number(m.photo.minFocal);
        if (m.photo.maxFocal > m.photo.minFocal)
            name += QLatin1Char('-') + number(m.photo.maxFocal);
        name += QStringLiteral(" mm");
        if (m.photo.maxApertureAtMin > 0) {
            name += QStringLiteral(" f/") + number(m.photo.maxApertureAtMin);
            if (m.photo.maxApertureAtMax > m.photo.maxApertureAtMin)
                name += QLatin1Char('-') + number(m.photo.maxApertureAtMax);
        }
        metadata.lensModel = name;
    }
    metadata.artist = QString::fromStdString(m.photo.artist);
    metadata.description = QString::fromStdString(m.photo.description);
    metadata.shutter = m.photo.shutter;
    metadata.aperture = m.photo.aperture;
    metadata.focalLength = m.focalLength;
    metadata.focalLength35 = m.photo.focalLength35;
    metadata.iso = int(std::lround(m.exposure.iso));
    metadata.taken = m.photo.taken;
    metadata.gps = m.photo.gps;
    for (int i = 0; i < 3; ++i) {
        metadata.latitude[i] = m.photo.latitude[i];
        metadata.longitude[i] = m.photo.longitude[i];
    }
    metadata.altitude = m.photo.altitude;
    metadata.latitudeRef = m.photo.latitudeRef;
    metadata.longitudeRef = m.photo.longitudeRef;
    metadata.altitudeRef = m.photo.altitudeRef;
    metadata.gpsStatus = m.photo.gpsStatus;
    const Provenance made = provenance(resolved, *m_packs, FLX_VERSION_TEXT);
    metadata.software = QString::fromStdString(made.software);
    metadata.provenance = QString::fromStdString(made.description);
    writeExport(linear, out.width(), out.height(), path, format, resolved.outputColorSpace, metadata);
}

// MARK: - The loupe

void RenderEngine::holdFullFrame(DecodedPicture &&picture, const QString &path)
{
    m_full = std::move(picture.pixels);
    m_fullFileName = QFileInfo(path).fileName().toStdString();
    m_loupe.box = {};
    m_loupe.masks.reset();
}

void RenderEngine::releaseFullFrame()
{
    m_full = {};
    m_loupe = {};
}

namespace {

// A blur's reach, as FilmRenderer::blur and GpuGraph::blurAll lay it: the
// radius of its taps; none below what each skips.
int blurReach(float sigma, float least)
{
    return sigma >= least ? int(gaussianTaps(sigma).size() / 2) : 0;
}

// How far the render reaches from a pixel: in the frame, through the film
// stage (scatter and halation, the couplers' diffusion, the grain's clouds
// when drawn on the frame, a shaped print's bicubic read), and in the print,
// after it (the grain's clouds on a shaped print, the grain's mean, Bloom &
// Glow). Every one is a finite kernel, so a part with these margins prints
// as the whole print does there.
struct Reach {
    int film = 0, print = 0;
};

Reach reachOf(const RenderSetup &s)
{
    const float pitch = s.pixelSizeUm;
    const RecipeParams &r = s.recipe;
    const HalationParams &h = s.halation;
    Reach reach;
    reach.film = 2;
    if (s.stages.halation) {
        const float scatterAmount = r.cameraSharpness ? 0 : float(r.scatterAmount);
        const float core = meanOf(h.scatterCoreUm, 3) * float(r.scatterScale) / pitch;
        const float tail = meanOf(h.scatterTailUm, 3) * float(r.scatterScale) / pitch;
        int scatter = 0;
        if (scatterAmount > 0 && (core > 0.05f || tail > 0.05f))
            scatter = std::max(blurReach(std::max(core, 0.05f), 0), blurReach(std::max(tail * 1.5236f, 0.05f), 0));
        const float sigmaH = meanOf(h.halationFirstSigmaUm, 3) * float(r.halationScale) / pitch;
        int halation = 0;
        if (sigmaH > 0.05f)    // its bounces blur what the scatter made, the widest last
            halation = blurReach(std::max(sigmaH * std::sqrt(float(std::max(h.bounces, 1))), 0.05f), 0);
        reach.film += scatter + halation;
    }
    if (s.stages.couplers && !r.cameraSharpness && s.couplerDiffusionUm > 0) {
        const float sigma = s.couplerDiffusionUm * float(r.couplersDiffusion) / pitch;
        if (sigma > 0.4f)
            reach.film += blurReach(sigma, 0);
    }
    int grain = 0;
    if (s.stages.grain)
        for (int i = 0; i < 9; ++i)
            grain = std::max(grain, int(cloudFilter(double(s.grain.cloudUm[i] * float(r.grainScale) / pitch)).size() / 2));
    if (s.stages.shaped)
        reach.print += grain;
    else
        reach.film += grain;
    if (s.stages.grain)
        reach.print += blurReach(3, 0);         // the grain's mean, restored over 3 px
    const EffectsParams &e = s.effects;
    if (e.active()) {
        const int bloom = e.bloom ? blurReach(e.bloomSigma, 0.3f) : 0;
        const int haze = e.haze > 0 ? blurReach(e.hazeSigma, 0.3f) : 0;
        const int glow = e.glow ? blurReach(e.glowSigma, 0.3f) : 0;
        const int halation = e.halation ? blurReach(e.halationSigma, 0.3f) : 0;
        reach.print += std::max({bloom + haze, glow, halation});
    }
    return reach;
}

// [lo, hi) widened by `margin` on both sides, on a grid of `align` so the box
// keeps its size as the loupe moves, within [0, limit) and at most `most`
// wide: its start and its length.
std::pair<int, int> span(double lo, double hi, int margin, int limit, int align, int most)
{
    const double from = std::floor(lo) - margin, to = std::ceil(hi) + margin;
    // one step of the grid more than the margins need, so a start snapped
    // down onto it still covers them
    int size = int(std::ceil((to - from + align) / align)) * align;
    int start = int(std::floor(from / align)) * align;
    if (size > most) {
        // past the most, the margins shrink alike on both sides, the window kept whole
        size = std::max(most / align * align, int(std::ceil((hi - lo) / align)) * align + 2 * align);
        start = int(std::floor(((lo + hi) / 2 - size / 2.0) / align)) * align;
    }
    if (size >= limit)
        return {0, limit};
    start = std::clamp(start, 0, limit - size);
    return {start, size};
}

} // namespace

void RenderEngine::printPart(PartRenderer &part, const Dump &full, const std::string &fileName, const Recipe &resolved,
                             const QRect &window, int most, std::vector<uint8_t> *display, std::vector<float> *linear)
{
    const int W = full.width, H = full.height;
    const GeometryMap map(resolved.geometry, W, H);
    const int pw = map.outputWidth(), ph = map.outputHeight();
    const int wx = window.x(), wy = window.y(), ww = window.width(), wh = window.height();

    BuiltSetup built = setupFor(resolved, W, H, fileName);
    RenderSetup &s = built.setup;
    shape(s, map, false);
    const Reach reach = reachOf(s);
    constexpr int kAlign = 64;
    QRect box, made;
    if (map.isIdentity) {
        const auto [bx, bw] = span(wx, wx + ww, reach.film + reach.print, W, kAlign, most);
        const auto [by, bh] = span(wy, wy + wh, reach.film + reach.print, H, kAlign, most);
        box = made = QRect(bx, by, bw, bh);
    } else {
        const auto [px, pwide] = span(wx, wx + ww, reach.print, pw, kAlign, most);
        const auto [py, phigh] = span(wy, wy + wh, reach.print, ph, kAlign, most);
        made = QRect(px, py, pwide, phigh);
        // the part's footprint in the frame
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (const PointD corner : {PointD{double(px), double(py)}, PointD{double(px + pwide), double(py)},
                                    PointD{double(px), double(py + phigh)}, PointD{double(px + pwide), double(py + phigh)}}) {
            const PointD f = map.outputToSource.apply(corner);
            x0 = std::min(x0, f.x), y0 = std::min(y0, f.y), x1 = std::max(x1, f.x), y1 = std::max(y1, f.y);
        }
        const auto [bx, bw] = span(x0, x1, reach.film, W, kAlign, most + 2 * reach.film);
        const auto [by, bh] = span(y0, y1, reach.film, H, kAlign, most + 2 * reach.film);
        box = QRect(bx, by, bw, bh);
    }

    if (!part.renderer) {
        part.renderer = std::make_unique<FilmRenderer>(m_rhi.get(), FilmRenderer::Blur::Own);
        part.renderer->setSkipsUnchangedFilm(true);
    }
    if (box != part.box) {
        std::vector<float> pixels(size_t(box.width()) * box.height() * 4);
        for (int y = 0; y < box.height(); ++y)
            std::memcpy(&pixels[size_t(y) * box.width() * 4],
                        &full.floats[(size_t(box.y() + y) * W + size_t(box.x())) * 4], size_t(box.width()) * 16);
        part.renderer->setPicture(pixels.data(), box.width(), box.height(), qint64(W) * H);
        part.box = box;
    }
    // the masks at the full size's map, as the export draws them
    if (!part.masks || *part.masks != resolved.masks) {
        const maskraster::Size size = maskraster::mapSize(W, H);
        std::vector<std::vector<float>> coverages;
        std::vector<std::pair<const LocalMask *, const std::vector<float> *>> drawn;
        coverages.reserve(resolved.masks.size());
        for (const LocalMask &mask : resolved.masks)
            if (maskraster::isActive(mask))
                coverages.push_back(maskraster::coverage(mask, size.width, size.height));
        size_t next = 0;
        for (const LocalMask &mask : resolved.masks)
            if (maskraster::isActive(mask))
                drawn.emplace_back(&mask, &coverages[next++]);
        if (drawn.empty())
            part.renderer->setDodgeBurn({}, 0, 0);
        else
            part.renderer->setDodgeBurn(maskraster::combine(drawn, size.width, size.height), size.width, size.height);
        part.masks = resolved.masks;
    }

    // the setup of the whole print, for this part of it
    for (size_t i = 0; i + 1 < s.gradients.size(); i += 2) {
        s.gradients[i].x -= float(box.x());
        s.gradients[i].y -= float(box.y());
        s.gradients[i].z -= float(box.x());
        s.gradients[i].w -= float(box.y());
    }
    s.width = box.width();
    s.height = box.height();
    RenderSetup::Part p;
    p.frameWidth = W;
    p.frameHeight = H;
    p.boxX = box.x();
    p.boxY = box.y();
    p.printX = made.x();
    p.printY = made.y();
    if (map.isIdentity) {
        // the print is the box: the masks found in the frame through where it lies
        s.shape.columns = {1, 0, 0, 1};
        s.shape.offsetSize = {float(box.x()), float(box.y()), float(W), float(H)};
        s.shape.active = {0, 1, 0, 0};
        s.outputWidth = box.width();
        s.outputHeight = box.height();
    } else {
        // the part's pixel to the frame's: the whole print's, moved to where the part lies
        const Affine &t = map.outputToSource;
        s.shape.offsetSize.x += float(t.a * made.x() + t.c * made.y());
        s.shape.offsetSize.y += float(t.b * made.x() + t.d * made.y());
        p.held = s.shape;
        p.held.offsetSize.x -= float(box.x());
        p.held.offsetSize.y -= float(box.y());
        s.outputWidth = made.width();
        s.outputHeight = made.height();
    }
    s.encode.columns = s.shape.columns;
    s.encode.offsetSize = s.shape.offsetSize;
    s.encode.active = s.shape.active;
    s.encode.modes.y = 0;
    s.part = p;
    part.renderer->load(s);
    std::vector<uint8_t> screen;
    std::vector<float> print;
    part.renderer->render(display ? &screen : nullptr, linear ? &print : nullptr);
    const QSize out = part.renderer->outputSize();
    const int ox = wx - made.x(), oy = wy - made.y();
    if (ox < 0 || oy < 0 || ox + ww > out.width() || oy + wh > out.height())
        fail("the window is not inside what was printed for it");
    if (display) {
        display->resize(size_t(ww) * wh * 4);
        for (int y = 0; y < wh; ++y)
            std::memcpy(&(*display)[size_t(y) * ww * 4], &screen[(size_t(oy + y) * out.width() + size_t(ox)) * 4],
                        size_t(ww) * 4);
    }
    if (linear) {
        linear->resize(size_t(ww) * wh * 4);
        for (int y = 0; y < wh; ++y)
            std::memcpy(&(*linear)[size_t(y) * ww * 4], &print[(size_t(oy + y) * out.width() + size_t(ox)) * 4],
                        size_t(ww) * 16);
    }
}

QImage RenderEngine::renderLoupe(const Recipe &recipe, QPointF centre, int width, int height)
{
    if (m_full.floats.empty() || width <= 0 || height <= 0)
        return {};
    ensureDevice();
    // the values the export would print with, metered on the preview
    const Recipe resolved = resolve(recipe).first;
    const GeometryMap map(resolved.geometry, m_full.width, m_full.height);
    const int pw = map.outputWidth(), ph = map.outputHeight();
    const int ww = std::min(width, pw), wh = std::min(height, ph);
    const int wx = std::clamp(int(std::lround(centre.x() * pw - ww / 2.0)), 0, pw - ww);
    const int wy = std::clamp(int(std::lround(centre.y() * ph - wh / 2.0)), 0, ph - wh);
    // A box at most this wide on the GPU, a third of what is free: Bloom &
    // Glow at their widest reach furthest, and past it their farthest light
    // is the box's edge.
    const qint64 free = gpuMemoryFree(m_rhi.get());
    const int most = free < 0 ? 3072 : std::clamp(int(std::sqrt(double(free) / 3 / 250)), 1024, 3072);
    std::vector<uint8_t> display;
    printPart(m_loupe, m_full, m_fullFileName, resolved, QRect(wx, wy, ww, wh), std::max(most, std::max(ww, wh) + 256),
              &display, nullptr);
    QImage image(ww, wh, QImage::Format_RGBA8888);
    for (int y = 0; y < wh; ++y)
        std::memcpy(image.scanLine(y), &display[size_t(y) * ww * 4], size_t(ww) * 4);
    image.setColorSpace(outputColorSpace(resolved.outputColorSpace));
    if (resolved.outputColorSpace != "sRGB")
        image.convertToColorSpace(QColorSpace(QColorSpace::SRgb));
    return image.convertToFormat(QImage::Format_RGB32);
}
