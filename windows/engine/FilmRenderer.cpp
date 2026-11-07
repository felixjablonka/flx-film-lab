#include "FilmRenderer.h"
#include "PrintCurves.h"

#include "Filters.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFloat16>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace {


// The stage-2 kernels' uniform blocks, whole vec4s as the shaders declare them.
struct ScatterBlock {
    Vec4 tailWeight;
    Vec4 amount;
};
struct AccumulateBlock {
    Vec4 weight;
    IVec4 reset;
};
struct HalationBlock {
    Vec4 strength;
    IVec4 renormalize;
};
struct CouplersBlock {
    Vec4 matrix[3];
};
struct GrainLayerBlock {
    Vec4 tableTop;
    Vec4 scale;                     // pixelArea, sizeSquared, amount
    uint32_t ids[4];                // seed, monochrome, layer, samples
    uint32_t origin[4];             // where the pixels lie in the frame or the shaped print
};
struct ConvolveBlock {
    IVec4 params;                   // radius, axis, first layer
};

} // namespace

FilmRenderer::FilmRenderer(QRhi *rhi, Blur blur)
    : GpuGraph(rhi), m_blurKind(blur)
{
}

FilmRenderer::~FilmRenderer() = default;








namespace {

// FNV-1a over bytes, 64 bits.
struct Hash {
    uint64_t h = 0xcbf29ce484222325ull;
    void bytes(const void *data, size_t n)
    {
        const auto *p = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < n; ++i)
            h = (h ^ p[i]) * 0x100000001b3ull;
    }
    template<typename T>
    void value(const T &v) { bytes(&v, sizeof v); }
    void floats(const std::vector<float> &v) { bytes(v.data(), v.size() * 4); }
};

} // namespace

// Everything the film stage reads, from the exposure to the grain.
uint64_t FilmRenderer::filmKey(const RenderSetup &s) const
{
    Hash h;
    h.value(m_size.width());
    h.value(m_size.height());
    h.value(m_pitch);
    h.value(s.film);
    h.bytes(s.gradients.data(), s.gradients.size() * sizeof(Vec4));
    h.value(s.develop);
    h.floats(s.curves.floats);
    h.floats(s.curves0.floats);
    h.floats(s.edgeScale.floats);
    h.floats(s.tcLut.floats);
    h.value(s.tcLut.width);
    h.bytes(s.couplerMatrix, sizeof s.couplerMatrix);
    h.value(s.couplerDiffusionUm);
    const HalationParams &a = s.halation;
    h.value(a.active);
    h.bytes(a.scatterCoreUm, sizeof a.scatterCoreUm);
    h.bytes(a.scatterTailUm, sizeof a.scatterTailUm);
    h.bytes(a.scatterTailWeight, sizeof a.scatterTailWeight);
    h.bytes(a.halationStrength, sizeof a.halationStrength);
    h.bytes(a.halationFirstSigmaUm, sizeof a.halationFirstSigmaUm);
    h.value(a.bounces);
    h.value(a.bounceDecay);
    h.value(a.renormalize);
    h.bytes(s.grain.tableDensityMax, sizeof s.grain.tableDensityMax);
    h.bytes(s.grain.cloudUm, sizeof s.grain.cloudUm);
    h.value(s.grain.samples);
    h.floats(s.selwyn.floats);
    const RecipeParams &r = s.recipe;
    for (double v : {r.halationAmount, r.halationScale, r.scatterAmount, r.scatterScale, r.couplersDiffusion,
                     r.grainAmount, r.grainScale, r.frameLongMM, r.frameShortMM})
        h.value(v);
    h.value(r.cameraSharpness);
    h.value(s.stages.halation);
    h.value(s.stages.couplers);
    h.value(s.stages.grain);
    h.value(s.shape.active.x);          // the grain on the frame, or on a shaped print
    if (s.part) {                       // a part of a frame: where its box lies
        h.value(s.part->frameWidth);
        h.value(s.part->frameHeight);
        h.value(s.part->boxX);
        h.value(s.part->boxY);
    }
    h.value(s.fileSeed);
    h.value(s.monochrome);
    const QByteArray stock = s.stock.toUtf8();
    h.bytes(stock.constData(), size_t(stock.size()));
    return h.h;
}

// A Gaussian blur, edges clamped, as two passes of grain_convolve with the
// same kernel for all three channels: MPSImageGaussianBlur on the Mac.
void FilmRenderer::blur(QRhiTexture *from, QRhiTexture *to, float sigma)
{
    // the first axis into a texture the size the passes run over
    QRhiTexture *temp = m_passSize == m_size ? m_blurTemp : m_shapedBlurTemp;
    std::vector<float> taps;
    if (m_blurKind == Blur::Measured) {
        for (const MpsKernel &k : m_setup->mpsKernels)
            if (std::abs(k.sigma - sigma) <= 1e-6f * std::max(1.0f, sigma))
                taps = k.taps;
        if (taps.empty())
            fail(QStringLiteral("no measured MPS kernel at sigma %1").arg(sigma));
    } else {
        taps = gaussianTaps(sigma);
    }
    m_blurs.push_back({sigma, int(taps.size())});
    std::vector<Vec4> weights;
    for (float t : taps)
        weights.push_back({t, t, t, 0});
    QRhiBuffer *w = storage(weights.data(), weights.size() * sizeof(Vec4));
    const int radius = int(taps.size() / 2);
    pass("grain_convolve", {input(0, from), B::imageStore(1, kCompute, temp, 0), B::bufferLoad(2, kCompute, w),
                            B::uniformBuffer(3, kCompute, uniform(ConvolveBlock{{radius, 0, 0, 0}}))});
    pass("grain_convolve", {input(0, temp), B::imageStore(1, kCompute, to, 0), B::bufferLoad(2, kCompute, w),
                            B::uniformBuffer(3, kCompute, uniform(ConvolveBlock{{radius, 1, 0, 0}}))});
}

// 1b — agx model/diffusion.apply_halation_um: scatter first (core plus
// tail), then the back-reflection's bounces (Renderer.encodeHalation).
QRhiTexture *FilmRenderer::halation(QRhiTexture *source, QRhiTexture *destination,
                                    QRhiTexture *scratch1, QRhiTexture *scratch2)
{
    const HalationParams &h = m_setup->halation;
    const RecipeParams &r = m_setup->recipe;
    QRhiTexture *current = source;

    // scatter, none with Camera sharpness
    const float scatterAmount = r.cameraSharpness ? 0 : float(r.scatterAmount);
    const float coreSigma = meanOf(h.scatterCoreUm, 3) * float(r.scatterScale) / m_pitch;
    const float tailLambda = meanOf(h.scatterTailUm, 3) * float(r.scatterScale) / m_pitch;
    if (scatterAmount > 0 && (coreSigma > 0.05f || tailLambda > 0.05f)) {
        blur(current, scratch1, std::max(coreSigma, 0.05f));
        // the exponential tail's dominant Gaussian, sigma 1.52 lambda
        blur(current, scratch2, std::max(tailLambda * 1.5236f, 0.05f));
        const ScatterBlock u{{h.scatterTailWeight[0], h.scatterTailWeight[1], h.scatterTailWeight[2], 0},
                             {scatterAmount, 0, 0, 0}};
        pass("scatter_mix", {input(0, current), input(1, scratch1), input(2, scratch2),
                             B::imageStore(3, kCompute, destination, 0), B::uniformBuffer(4, kCompute, uniform(u))});
        current = destination;
    }

    // back-reflection
    const float amount = float(r.halationAmount);
    const Vec4 strength{h.halationStrength[0] * amount, h.halationStrength[1] * amount,
                        h.halationStrength[2] * amount, 0};
    const float sigmaH = meanOf(h.halationFirstSigmaUm, 3) * float(r.halationScale) / m_pitch;
    const int bounces = std::max(h.bounces, 1);
    if (!(std::max({strength.x, strength.y, strength.z}) > 0 && sigmaH > 0.05f))
        return current;
    std::vector<float> decay;
    float decaySum = 0;
    for (int k = 0; k < bounces; ++k) {
        decay.push_back(std::pow(h.bounceDecay, float(k)));
        decaySum += decay.back();
    }
    if (decaySum > 0)
        for (float &d : decay)
            d /= decaySum;

    // Accumulated in scratch1, never in the texture halation_add writes: with
    // the scatter left out (Camera sharpness) `current` is the source, and
    // Direct3D 11 cannot read a texture through one binding and write it
    // through another in a dispatch: it unbinds the read, which reads zero.
    QRhiTexture *accum = scratch1;
    QRhiTexture *blurTarget = scratch2;
    for (int k = 0; k < bounces; ++k) {
        blur(current, blurTarget, std::max(sigmaH * std::sqrt(float(k + 1)), 0.05f));
        const AccumulateBlock u{{decay[k], 0, 0, 0}, {k == 0 ? 1 : 0, 0, 0, 0}};
        pass("accumulate_scaled", {input(0, blurTarget), B::imageLoadStore(1, kCompute, accum, 0),
                                   B::uniformBuffer(2, kCompute, uniform(u))});
    }
    QRhiTexture *result = current == destination ? scratch2 : destination;
    const HalationBlock u{strength, {h.renormalize ? 1 : 0, 0, 0, 0}};
    pass("halation_add", {input(0, current), input(1, accum), B::imageStore(2, kCompute, result, 0),
                          B::uniformBuffer(3, kCompute, uniform(u))});
    return result;
}

// 2c — the grain, per emulsion layer, fast, medium and slow, each spread over
// its own dye clouds and laid on the clean density (Renderer.drawGrain).
void FilmRenderer::drawGrain(QRhiTexture *clean, QRhiTexture *target, QRhiTexture *scratch1, QRhiTexture *scratch2,
                             Origin origin)
{
    const RenderSetup &s = *m_setup;
    const float size = float(s.recipe.grainScale);
    const uint32_t seed = fnv1a(s.stock.toStdString()) ^ (s.fileSeed * 0x9E3779B9u);
    QRhiBuffer *selwyn = storage(s.selwyn.floats.data(), s.selwyn.floats.size() * 4);
    for (uint32_t layer = 0; layer < 3; ++layer) {
        const GrainLayerBlock u{{s.grain.tableDensityMax[0], s.grain.tableDensityMax[1], s.grain.tableDensityMax[2], 0},
                                {m_pitch * m_pitch, size * size, float(s.recipe.grainAmount), 0},
                                {seed, s.monochrome ? 1u : 0u, layer, uint32_t(s.grain.samples)},
                                {origin.x, origin.y, 0, 0}};
        pass("film_grain_layer", {input(0, clean), B::imageStore(1, kCompute, scratch1, 0),
                                  B::uniformBuffer(2, kCompute, uniform(u)), B::bufferLoad(3, kCompute, selwyn)});
        // each record's clouds for this layer, in pixels, padded to one radius
        std::vector<double> kernels[3];
        size_t radius = 0;
        for (int c = 0; c < 3; ++c) {
            kernels[c] = cloudFilter(double(s.grain.cloudUm[c * 3 + layer] * size / m_pitch));
            radius = std::max(radius, kernels[c].size() / 2);
        }
        std::vector<Vec4> taps;
        for (size_t i = 0; i <= 2 * radius; ++i) {
            float v[3];
            for (int c = 0; c < 3; ++c) {
                const long j = long(i) - long(radius) + long(kernels[c].size() / 2);
                v[c] = j >= 0 && j < long(kernels[c].size()) ? float(kernels[c][size_t(j)]) : 0;
            }
            taps.push_back({v[0], v[1], v[2], 0});
        }
        QRhiBuffer *w = storage(taps.data(), taps.size() * sizeof(Vec4));
        pass("grain_convolve", {input(0, scratch1), B::imageStore(1, kCompute, scratch2, 0), B::bufferLoad(2, kCompute, w),
                                B::uniformBuffer(3, kCompute, uniform(ConvolveBlock{{int(radius), 0, 0, 0}}))});
        pass("grain_convolve_add", {input(0, scratch2), input(1, clean), B::imageLoadStore(2, kCompute, target, 0),
                                    B::bufferLoad(3, kCompute, w),
                                    B::uniformBuffer(4, kCompute, uniform(ConvolveBlock{{int(radius), 1, layer == 0 ? 1 : 0, 0}}))});
    }
}

// 3 — printing.expose + printing.develop + scanning.scan, fused.
void FilmRenderer::printNegative(QRhiTexture *negative, QRhiTexture *into)
{
    pass("print_and_scan", {input(0, negative), B::imageStore(1, kCompute, into, 0), input(2, m_toneLut),
                            B::sampledTexture(3, kCompute, m_dodgeBurn, m_linear), B::bufferLoad(4, kCompute, m_spectral),
                            B::uniformBuffer(5, kCompute, m_printU), B::bufferLoad(6, kCompute, m_toneShift),
                            B::uniformBuffer(7, kCompute, m_shapeU)});
}

void FilmRenderer::setPicture(const float *rgba, int width, int height, qint64 framePixels)
{
    const QSize size(width, height);
    // a part of a frame works in the frame's precision, so it prints as the whole does
    const QRhiTexture::Format intermediate =
        (framePixels > 0 ? framePixels : qint64(width) * height) > 12'000'000 ? QRhiTexture::RGBA16F : QRhiTexture::RGBA32F;
    if (size != m_size || !m_source || intermediate != m_intermediate) {
        m_pictureResources.clear();
        m_size = size;
        auto make = [&](QRhiTexture::Format format, bool loadStore) {
            noteTexture("picture", m_size, format);
            QRhiTexture *t = m_rhi->newTexture(format, m_size, 1,
                                               loadStore ? QRhiTexture::UsedWithLoadStore : QRhiTexture::Flags());
            m_pictureResources.emplace_back(t);
            if (!t->create())
                fail("cannot make a texture the picture's size");
            return t;
        };
        // Full-resolution work needs a dozen image-sized textures, 8 GB at
        // 45 MP in float32: past 12 MP they drop to half precision, as the
        // Mac's do, which is plenty for linear light and density.
        m_intermediate = intermediate;
        m_source = make(QRhiTexture::RGBA32F, false);
        m_raw = make(m_intermediate, true);
        m_rawB = make(m_intermediate, true);
        m_scratchA = make(m_intermediate, true);
        m_scratchB = make(m_intermediate, true);
        m_logEMap = make(m_intermediate, true);
        m_cmy = make(m_intermediate, true);
        m_cmyB = make(m_intermediate, true);
        m_correction = make(m_intermediate, true);
        m_correctionBlurred = make(m_intermediate, true);
        m_printedMap = make(QRhiTexture::RGBA32F, true);    // the print in float32, as the Mac's
        m_displayMap = make(QRhiTexture::RGBA8, true);
        m_blurTemp = make(m_intermediate, true);    // the blur's first axis
        // the passes laid out for the last picture's textures go with them
        clearLayout();
    }
    m_renderedFilmKey.reset();
    QRhiResourceUpdateBatch *batch = m_rhi->nextResourceUpdateBatch();
    batch->uploadTexture(m_source, QRhiTextureUploadDescription(
        {0, 0, QRhiTextureSubresourceUploadDescription(rgba, quint32(size_t(width) * height * 16))}));
    QRhiCommandBuffer *cb = nullptr;
    if (m_rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
        fail("cannot start a frame");
    cb->resourceUpdate(batch);
    m_rhi->endOffscreenFrame();
}

void FilmRenderer::setDodgeBurn(std::vector<float> map, int width, int height)
{
    if (map.size() != size_t(width) * height * 4)
        map.clear();
    m_dodgeBurnMap = std::move(map);
    m_dodgeBurnSize = QSize(width, height);
    m_dodgeBurnDirty = true;
}



// A shaped print's textures at its size, made again only when it changes.
void FilmRenderer::ensureShaped(QSize size)
{
    if (m_shapedDisplay && m_shapedDisplay->pixelSize() == size)
        return;
    m_shapedResources.clear();
    auto make = [&](QRhiTexture::Format format) {
        noteTexture("shaped", size, format);
        QRhiTexture *t = m_rhi->newTexture(format, size, 1, QRhiTexture::UsedWithLoadStore);
        m_shapedResources.emplace_back(t);
        if (!t->create())
            fail("cannot make a texture the print's size");
        return t;
    };
    m_shapedNegative = make(m_intermediate);
    m_shapedGrainy = make(m_intermediate);
    m_shapedScratchA = make(m_intermediate);
    m_shapedScratchB = make(m_intermediate);
    m_shapedPrinted = make(QRhiTexture::RGBA32F);
    m_shapedDisplay = make(QRhiTexture::RGBA8);
    m_shapedBlurTemp = make(m_intermediate);    // the blur's first axis, at the print's size
    // the bindings of the last size's textures match none of this load's,
    // and go at its end with the rest it didn't use
}

void FilmRenderer::load(const RenderSetup &s)
{
    if (!s.input.floats.empty())
        setPicture(s.input.floats.data(), s.width, s.height);
    if (!m_source || m_size != QSize(s.width, s.height))
        fail("the setup is not for the picture set");
    const bool shaped = s.shape.active.x != 0;
    m_passSize = m_size;
    beginLayout();
    m_blurs.clear();
    m_setup = &s;
    m_passSize = m_size;

    // The film's scale, and the grain's seed, as the Mac engine had them:
    // a part's scale is its frame's.
    const int frameWidth = s.part ? s.part->frameWidth : s.width, frameHeight = s.part ? s.part->frameHeight : s.height;
    m_pitch = pixelSizeUm(frameWidth, frameHeight, s.recipe.frameLongMM, s.recipe.frameShortMM);
    if (std::abs(m_pitch - s.pixelSizeUm) > 1e-6f * s.pixelSizeUm)
        fail(QStringLiteral("pixel pitch %1 um, the Mac's %2").arg(m_pitch).arg(s.pixelSizeUm));
    if (s.stages.grain && (fnv1a(s.stock.toStdString()) ^ (s.fileSeed * 0x9E3779B9u)) != s.grainSeed)
        fail("the grain seed is not the Mac's");


    // The LUTs.
    auto upload = [&](QRhiTexture *t, const void *data, size_t bytes) {
        m_batch->uploadTexture(t, QRhiTextureUploadDescription(
            {0, 0, QRhiTextureSubresourceUploadDescription(data, quint32(bytes))}));
    };
    // RGB padded to RGBA, so a fetch is one texel (Renderer.ensureTcLut).
    const int L = s.tcLut.width;
    std::vector<float> tc(size_t(L) * L * 4, 1.0f);
    for (size_t i = 0; i < size_t(L) * L; ++i)
        std::memcpy(&tc[i * 4], &s.tcLut.floats[i * 3], 12);
    QRhiTexture *tcLut = texture(QRhiTexture::RGBA32F, QSize(L, L), false);
    upload(tcLut, tc.data(), tc.size() * 4);
    m_toneLut = texture(QRhiTexture::RGBA32F, QSize(s.toneLut.width, s.toneLut.height), false);
    upload(m_toneLut, s.toneLut.floats.data(), s.toneLut.floats.size() * 4);
    // Dodge & burn's map, or without masks one empty texel, as Renderer's
    // dummyDodgeBurn.
    if (m_dodgeBurnMap.empty()) {
        if (!m_noDodgeBurn) {
            m_noDodgeBurn.reset(m_rhi->newTexture(QRhiTexture::RGBA32F, QSize(1, 1)));
            if (!m_noDodgeBurn->create())
                fail("cannot make a texture");
            static const float none[4] = {0, 0, 0, 0};
            upload(m_noDodgeBurn.get(), none, sizeof none);
        }
        m_dodgeBurn = m_noDodgeBurn.get();
    } else {
        if (!m_dodgeBurnTexture || m_dodgeBurnTexture->pixelSize() != m_dodgeBurnSize) {
            m_dodgeBurnTexture.reset(m_rhi->newTexture(QRhiTexture::RGBA32F, m_dodgeBurnSize));
            if (!m_dodgeBurnTexture->create())
                fail("cannot make the dodge & burn map");
            m_dodgeBurnDirty = true;
        }
        if (m_dodgeBurnDirty)
            upload(m_dodgeBurnTexture.get(), m_dodgeBurnMap.data(), m_dodgeBurnMap.size() * 4);
        m_dodgeBurnDirty = false;
        m_dodgeBurn = m_dodgeBurnTexture.get();
    }

    QRhiTexture *source = m_source, *raw = m_raw, *rawB = m_rawB, *scratchA = m_scratchA, *scratchB = m_scratchB,
                *logE = m_logEMap, *cmy = m_cmy, *cmyB = m_cmyB, *correction = m_correction,
                *correctionBlurred = m_correctionBlurred, *printed = m_printedMap, *display = m_displayMap;

    // The tables and uniforms.
    QRhiBuffer *gradients = storage(s.gradients.data(), s.gradients.size() * sizeof(Vec4));
    QRhiBuffer *curves = storage(s.curves.floats.data(), s.curves.floats.size() * 4);
    QRhiBuffer *curves0 = storage(s.curves0.floats.data(), s.curves0.floats.size() * 4);
    QRhiBuffer *edgeScale = storage(s.edgeScale.floats.data(), s.edgeScale.floats.size() * 4);
    m_spectral = storage(s.spectral.floats.data(), s.spectral.floats.size() * 4);
    m_toneShift = storage(s.toneShift.floats.data(), s.toneShift.floats.size() * 4);
    QRhiBuffer *filmU = uniform(s.film);
    QRhiBuffer *developU = uniform(s.develop);
    m_printU = uniform(s.print);
    m_shapeU = uniform(s.shape);
    QRhiBuffer *encodeU = uniform(s.encode);

    // 1 — filming.expose
    pass("film_expose", {input(0, source), B::imageStore(1, kCompute, raw, 0), input(2, tcLut),
                         B::uniformBuffer(3, kCompute, filmU), B::bufferLoad(4, kCompute, gradients)});
    // 1b — scatter and halation, on linear irradiance before the curve
    QRhiTexture *exposed = raw;
    if (s.stages.halation)
        exposed = halation(raw, rawB, scratchA, scratchB);
    pass("film_log", {input(0, exposed), B::imageStore(1, kCompute, logE, 0)});

    // 2 — filming.develop
    pass("film_develop", {input(0, logE), B::imageStore(1, kCompute, cmy, 0), B::bufferLoad(2, kCompute, curves),
                          B::uniformBuffer(3, kCompute, developU)});
    QRhiTexture *negative = cmy;
    // 2b — the DIR couplers: the inhibitor released, its diffusion, and the
    // development against the pre-coupler curves
    if (s.stages.couplers) {
        const CouplersBlock couplers{{s.couplerMatrix[0], s.couplerMatrix[1], s.couplerMatrix[2]}};
        pass("couplers_amount", {input(0, cmy), B::imageStore(1, kCompute, correction, 0),
                                 B::uniformBuffer(2, kCompute, uniform(couplers)), B::uniformBuffer(3, kCompute, developU)});
        QRhiTexture *inhibitor = correction;
        // how far the inhibitor spreads, in pixels; not at all with Camera sharpness
        const float sigma = s.recipe.cameraSharpness || !(s.couplerDiffusionUm > 0)
            ? 0 : s.couplerDiffusionUm * float(s.recipe.couplersDiffusion) / m_pitch;
        if (sigma > 0.4f) {
            blur(correction, correctionBlurred, sigma);
            inhibitor = correctionBlurred;
        }
        pass("film_develop_couplers", {input(0, logE), input(1, inhibitor), B::imageStore(2, kCompute, cmyB, 0),
                                       input(3, correction), B::bufferLoad(4, kCompute, curves0),
                                       B::uniformBuffer(5, kCompute, developU), B::bufferLoad(6, kCompute, edgeScale)});
        negative = cmyB;
    }
    // 2c — grain, on the frame unless the print is shaped
    QRhiTexture *grainFree = nullptr;
    if (s.stages.grain && !shaped) {
        QRhiTexture *target = negative == cmyB ? cmy : cmyB;
        drawGrain(negative, target, scratchA, scratchB,
                  s.part ? Origin{uint32_t(s.part->boxX), uint32_t(s.part->boxY)} : Origin{0, 0});
        grainFree = negative;
        negative = target;
    }
    // the film stage ends here: the print's passes never write its results
    m_filmPasses = m_passes.size();
    m_filmKey = filmKey(s);

    // 2d — the print's shape: the developed negative turned, mirrored,
    //      levelled and cropped, and the rest made at the print's own size.
    //      Exposure, halation and the couplers have acted on the whole frame,
    //      and the pixels keep the frame's size, so a crop enlarges the grain
    //      as cropping a print does; the grain is drawn after, on the print's
    //      own pixels (Renderer.render).
    QRhiTexture *printScratch = scratchA, *printScratchB = scratchB, *grainyBlur = rawB;
    if (shaped) {
        const QSize out(std::max(s.outputWidth, 1), std::max(s.outputHeight, 1));
        ensureShaped(out);
        m_passSize = out;
        // a part's pixels from where they lie in the box held
        pass("apply_geometry", {input(0, negative), B::imageStore(1, kCompute, m_shapedNegative, 0),
                                B::uniformBuffer(2, kCompute, s.part ? uniform(s.part->held) : m_shapeU)});
        negative = m_shapedNegative;
        grainFree = nullptr;
        if (s.stages.grain) {
            drawGrain(m_shapedNegative, m_shapedGrainy, m_shapedScratchA, m_shapedScratchB,
                      s.part ? Origin{uint32_t(s.part->printX), uint32_t(s.part->printY)} : Origin{0, 0});
            negative = m_shapedGrainy;
            grainFree = m_shapedNegative;
        }
        printed = m_shapedPrinted;
        display = m_shapedDisplay;
        // the grainy negative is printed before its texture is needed again
        printScratch = m_shapedScratchA;
        printScratchB = m_shapedScratchB;
        grainyBlur = m_shapedGrainy;
    }

    // 3 — the print
    printNegative(negative, printed);
    // 3b — grain as texture only: the grain's mirror printed too, and each
    // pixel scaled so its neighbourhood of the two prints' mean holds the
    // grain-free light (Renderer.render)
    if (grainFree) {
        pass("grain_mirror", {input(0, grainFree), input(1, negative), B::imageStore(2, kCompute, printScratchB, 0)});
        printNegative(printScratchB, printScratch);
        pass("grain_even_mean", {input(0, printed), input(1, printScratch), B::imageStore(2, kCompute, printScratchB, 0)});
        blur(printScratchB, grainyBlur, 3);
        printNegative(grainFree, printScratch);
        blur(printScratch, printScratchB, 3);
        pass("grain_restore_mean", {B::imageLoadStore(0, kCompute, printed, 0), input(1, grainyBlur),
                                    input(2, printScratchB)});
    }
    // 3c — bloom and glow, laid on the finished print in linear light, their
    //      sizes the frame's, so the preview blooms as the export does
    if (s.effects.active())
        printed = applyEffects(printed, s.effects, m_intermediate);
    // 3d — the curves (PrintCurves), last of all, on the print as its file
    //      encodes it, so the export carries them as the screen does. The
    //      histogram is taken of the finished print, and, for the curves'
    //      graph, of the print they were given.
    m_binsAfter = m_binsBefore = nullptr;
    const auto histogram = [&](QRhiBuffer *&bins) {
        bins = storage(nullptr, 768 * sizeof(uint32_t));
        pass("histogram_rgb", {input(0, printed), B::bufferLoadStore(1, kCompute, bins),
                               B::uniformBuffer(2, kCompute, encodeU)});
    };
    if (m_measuresHistogram && !s.printCurves.empty())
        histogram(m_binsBefore);
    if (!s.printCurves.empty()) {
        struct CurvesBlock {
            IVec4 modes;            // encoding, the table's samples a channel
        } curves{{s.encode.modes.x, PrintCurves::tableSize, 0, 0}};
        QRhiBuffer *table = storage(s.printCurves.data(), s.printCurves.size() * sizeof(float));
        pass("apply_curves", {B::imageLoadStore(0, kCompute, printed, 0), B::bufferLoad(1, kCompute, table),
                              B::uniformBuffer(2, kCompute, uniform(curves))});
    }
    if (m_measuresHistogram)
        histogram(m_binsAfter);
    // 4 — the screen's encoding
    pass("encode_display", {input(0, printed), B::imageStore(1, kCompute, display, 0),
                            B::uniformBuffer(2, kCompute, encodeU)});

    m_logE = logE;
    m_negative = negative;
    m_printed = printed;
    m_display = display;

    m_setup = nullptr;
    endLayout();
}



double FilmRenderer::render(Taps *taps)
{
    QRhiReadbackResult logE, cmy, rgbOut, display;
    std::vector<std::pair<QRhiTexture *, QRhiReadbackResult *>> readbacks;
    if (taps)
        readbacks = {{m_logE, &logE}, {m_negative, &cmy}, {m_printed, &rgbOut}, {m_display, &display}};
    const double seconds = runPasses(readbacks);
    if (taps) {
        const size_t pixels = size_t(m_size.width()) * m_size.height();
        taps->logE = floats(logE, m_logE);
        taps->cmy = floats(cmy, m_negative);
        taps->rgbOut = floats(rgbOut, m_printed);
        if (size_t(display.data.size()) != pixels * 4)
            fail(QStringLiteral("the display readback has %1 bytes, expected %2").arg(display.data.size()).arg(pixels * 4));
        taps->display.assign(display.data.constData(), display.data.constData() + pixels * 4);
    }
    return seconds;
}

double FilmRenderer::render(std::vector<uint8_t> *display, std::vector<float> *linear, Histogram *histogram)
{
    QRhiReadbackResult screen, print, after, before;
    std::vector<std::pair<QRhiTexture *, QRhiReadbackResult *>> readbacks;
    std::vector<std::pair<QRhiBuffer *, QRhiReadbackResult *>> buffers;
    if (display)
        readbacks.push_back({m_display, &screen});
    if (linear)
        readbacks.push_back({m_printed, &print});
    if (histogram && m_binsAfter) {
        buffers.push_back({m_binsAfter, &after});
        if (m_binsBefore)
            buffers.push_back({m_binsBefore, &before});
    }
    const double seconds = runPasses(readbacks, buffers);
    const auto bins = [](const QRhiReadbackResult &r) {
        std::vector<uint32_t> out(768, 0);
        if (size_t(r.data.size()) >= out.size() * sizeof(uint32_t))
            std::memcpy(out.data(), r.data.constData(), out.size() * sizeof(uint32_t));
        return out;
    };
    if (histogram) {
        *histogram = {};
        if (m_binsAfter)
            histogram->after = bins(after);
        if (m_binsBefore)
            histogram->before = bins(before);
    }
    if (display) {
        const size_t pixels = size_t(m_display->pixelSize().width()) * m_display->pixelSize().height();
        if (size_t(screen.data.size()) != pixels * 4)
            fail(QStringLiteral("the display readback has %1 bytes, expected %2").arg(screen.data.size()).arg(pixels * 4));
        display->assign(screen.data.constData(), screen.data.constData() + pixels * 4);
    }
    if (linear)
        *linear = floats(print, m_printed);
    return seconds;
}

// the film stage again only where its inputs moved
double FilmRenderer::runPasses(const std::vector<std::pair<QRhiTexture *, QRhiReadbackResult *>> &readbacks,
                               const std::vector<std::pair<QRhiBuffer *, QRhiReadbackResult *>> &buffers)
{
    const size_t first = m_skipsUnchangedFilm && m_renderedFilmKey == m_filmKey ? m_filmPasses : 0;
    const double seconds = run(readbacks, first, buffers);
    m_renderedFilmKey = m_filmKey;
    return seconds;
}
