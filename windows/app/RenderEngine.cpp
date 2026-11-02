#include "RenderEngine.h"

#include "Exporter.h"
#include "FilmRenderer.h"
#include "Filters.h"
#include "GpuDecoder.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <rhi/qrhi.h>

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

RenderEngine::~RenderEngine()
{
    // the GPU's resources before its device
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
    DecodedPicture picture = m_decoder->decode(std::move(plan), &gpu);
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
    const double gpu = m_renderer->render(&display, nullptr) * 1e3;
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
    {
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
    writeExport(linear, out.width(), out.height(), path, format, resolved.outputColorSpace, metadata);
}
