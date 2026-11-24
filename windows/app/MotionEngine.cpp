#include "MotionEngine.h"
#include "DisplayColour.h"

#include "Provenance.h"
#include "version.h"

#include "RenderEngine.h"

#include <QColorSpace>
#include <QElapsedTimer>
#include <rhi/qrhi.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

MotionEngine::MotionEngine(QString packsDir)
    : m_packsDir(std::move(packsDir))
{
}

MotionEngine::~MotionEngine()
{
    // the pipeline and the baker's textures go before their device
    m_pipeline.reset();
    m_baker.reset();
    m_rhi.reset();
}

void MotionEngine::ensureDevice()
{
    if (m_rhi)
        return;
    m_packs = std::make_unique<PackLibrary>(m_packsDir);
    m_rhi = makeDevice();
    if (!m_rhi)
        throw std::runtime_error("This computer has no GPU the app can compute on.");
    m_baker = std::make_unique<LookBaker>(m_rhi.get(), *m_packs);
    m_pipeline = std::make_unique<FramePipeline>(m_rhi.get());
}

ClipInfo MotionEngine::open(const QString &path, int id)
{
    ensureDevice();
    const std::string p = path.toStdString();
    ClipInfo info = readClipInfo(p);
    auto reader = std::make_unique<ClipReader>(p);
    if (qEnvironmentVariableIsSet("FLX_TIMING"))
        std::fprintf(stderr, "%s: decoded by %s\n", qPrintable(path), reader->decoder().c_str());
    m_reader = std::move(reader);
    m_clip = id;
    // another clip draws grains of its own
    m_clipSeed = FrameGrain::clipSeedFor(p);
    m_frameRate = info.frameRate;
    m_frame.reset();
    m_shown.reset();
    return info;
}

std::optional<MotionEngine::Output> MotionEngine::render(const Request &request)
{
    ensureDevice();
    if (request.clip != m_clip || !m_reader || request.size.isEmpty())
        return std::nullopt;
    QElapsedTimer clock;
    clock.start();
    Output out;
    out.clip = m_clip;

    // a new look only when the print or the format it reads moved
    if (!m_baked || m_baked->first != request.recipe || m_baked->second != request.format) {
        m_pipeline->setLUT(m_baker->bake(request.recipe, 65, lutInputOf(request.format), LutOutput::BT1886));
        m_baked.emplace(request.recipe, request.format);
    }
    out.baked = m_baked->first;
    out.bakedFormat = m_baked->second;
    // the grain's table with it, for the same recipe, while the look has grain
    const bool grained = request.mode == FramePipeline::Mode::Graded && request.grain && request.grain->isVisible();
    if (grained && (!m_grainBasis || *m_grainBasis != request.recipe)) {
        m_grainTable = m_baker->printGrain(request.recipe);
        m_grainBasis = request.recipe;
    }

    // the frame showing at the time asked for, decoded only when it is another
    const double half = m_reader->frameDuration() / 2;
    if (!m_frame || std::abs(request.time - m_frame->time) >= half) {
        auto frame = m_reader->frameAt(request.time);
        if (!frame && !m_frame)
            throw std::runtime_error("The clip has no frame there.");
        if (frame)
            m_frame = std::make_shared<const VideoFrame>(std::move(*frame));
    }
    out.frame = m_frame;
    out.reading = readingOf(*m_frame, request.input, request.format);

    // the same frame from the same settings is the image already on screen
    Request shown = request;
    shown.time = m_frame->time;
    if (m_shown && m_shown->clip == shown.clip && m_shown->time == shown.time && m_shown->recipe == shown.recipe
        && m_shown->format == shown.format && m_shown->input == shown.input && m_shown->effects == shown.effects
        && m_shown->mode == shown.mode && m_shown->size == shown.size && m_shown->screen == shown.screen
        && m_shown->grain == shown.grain)
        return out;
    const double prepared = double(clock.nsecsElapsed()) / 1e6;
    clock.restart();
    // sizes are fractions of the long edge, so the preview's smaller frame
    // blooms exactly as the full-size export does
    const EffectsParams fx = effectsFor(request.effects, std::max(request.size.width(), request.size.height()));
    // each frame its own grains, by its number: the same paused or playing, and in the export
    std::optional<FrameGrain> grain;
    if (grained && m_grainTable)
        grain = FrameGrain{&*m_grainTable, *request.grain, m_clipSeed,
                           FrameGrain::frameNumber(m_frame->time, m_frameRate)};
    std::vector<uint8_t> rgba = m_pipeline->renderScreen(*m_frame, out.reading, request.size, request.mode, fx,
                                                         nullptr, grain ? &*grain : nullptr);
    QImage image(request.size, QImage::Format_RGBA8888);
    for (int y = 0; y < request.size.height(); ++y)
        std::memcpy(image.scanLine(y), rgba.data() + size_t(y) * request.size.width() * 4,
                    size_t(request.size.width()) * 4);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    // for the screen: the monitor's colour where it has a profile (DisplayColour)
    out.shown = displaycolour::toScreen(image);
    out.image = std::move(image);
    // the grade alone, as the Mac's preview times its draw; the bake and the
    // decode before it only with FLX_TIMING
    out.milliseconds = double(clock.nsecsElapsed()) / 1e6;
    if (qEnvironmentVariableIsSet("FLX_TIMING"))
        std::fprintf(stderr, "frame %.3f s: bake and decode %.1f ms, grade %.1f ms\n", m_frame->time, prepared,
                     out.milliseconds);
    m_shown = shown;
    return out;
}

std::string MotionEngine::cube(const Recipe &recipe, LutInput input, const std::string &title,
                               const std::vector<std::string> &comments)
{
    ensureDevice();
    return m_baker->bake(recipe, 65, input, LutOutput::BT1886).cubeText(title, comments);
}

std::string runClipExport(const QString &packsDir, const QString &source, const QString &destination,
                          const MotionLook &look, LogFormat format, const std::function<bool(double)> &progress,
                          const ExportComment &comment)
{
    std::unique_ptr<QRhi> rhi = makeDevice();
    if (!rhi)
        throw std::runtime_error("This computer has no GPU the app can compute on.");
    PackLibrary packs(packsDir);
    std::string encoder;
    {
        LookBaker baker(rhi.get(), packs);
        const LookLUT lut = baker.bake(look.recipe, 65, lutInputOf(format), LutOutput::BT1886);
        // the grain the clip is exported with: its look's, made for the recipe it prints with
        std::optional<PrintGrainTable> grainTable;
        if (look.grain && look.grain->isVisible())
            grainTable = baker.printGrain(look.recipe);
        ClipExportOptions options(look.exportSettings);
        options.grainTable = grainTable ? &*grainTable : nullptr;
        options.grain = look.grain;
        const Provenance made = provenance(look.recipe, packs, FLX_VERSION_TEXT, comment);
        options.software = made.software;
        options.description = made.description;
        encoder = exportClip(rhi.get(), source.toStdString(), destination.toStdString(), lut, look.effects, look.input,
                             format, options, progress);
    }
    return encoder;
}
