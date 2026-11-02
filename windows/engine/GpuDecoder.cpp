#include "GpuDecoder.h"

#include "RawDetail.h"

#include <QElapsedTimer>
#include <QFile>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace {

constexpr int kGroup = 16;          // local_size_x and _y of every kernel
constexpr auto kCompute = QRhiShaderResourceBinding::ComputeStage;

// The kernels' uniform blocks, whole vec4s as they declare them.
struct F4 {
    float x = 0, y = 0, z = 0, w = 0;
};
struct I4 {
    int32_t x = 0, y = 0, z = 0, w = 0;
};
struct PatternBlock {
    I4 cfa;
};
struct PatternXBlock {
    int32_t cfa[36];
    I4 size;
};
struct HighlightsBlock {
    F4 balance, light;
    I4 block;
};
struct ColourBlock {
    F4 balance, light;
    F4 matrix[3];
    F4 gain;
    I4 block;
};
struct SamplesBlock {
    I4 grid;
};
struct NoiseBlock {
    F4 noise;
};
struct BoxBlock {
    I4 params;
};
struct BlurBlock {
    float taps[8];
    I4 params;
};
struct LensBlock {
    F4 k, shape, centre;
    F4 planes[6];
    I4 model;
};
struct TurnBlock {
    I4 orientation;
};
struct ScaleBlock {
    I4 params;
};

[[noreturn]] void fail(const QString &what)
{
    throw std::runtime_error(what.toStdString());
}

QShader shader(const char *kernel)
{
    QFile f(QStringLiteral(":/shaders/%1.comp.qsb").arg(QLatin1StringView(kernel)));
    if (!f.open(QIODevice::ReadOnly))
        fail(QStringLiteral("no shader %1").arg(QLatin1StringView(kernel)));
    return QShader::fromSerialized(f.readAll());
}

F4 f4(float3 v, float w = 0) { return {v.x, v.y, v.z, w}; }
F4 noiseOf(const Noise &n) { return {float(n.a), float(n.b), 0, 0}; }

// How many noise samples a plane of `n` pixels has along an axis: from 1, in
// steps, short of the last
int sampleCount(int n) { return n >= 3 ? (n - 3) / kNoiseStep + 1 : 0; }

} // namespace

GpuDecoder::GpuDecoder(QRhi *rhi)
    : m_rhi(rhi)
{
    if (!rhi->isFeatureSupported(QRhi::Compute))
        fail("this GPU backend has no compute shaders");
}

GpuDecoder::~GpuDecoder() = default;

template<typename T>
T *GpuDecoder::own(T *resource)
{
    m_resources.emplace_back(resource);
    return resource;
}

QRhiTexture *GpuDecoder::texture(QRhiTexture::Format format, QSize size, bool loadStore)
{
    QRhiTexture *t = own(m_rhi->newTexture(format, size, 1,
                                           loadStore ? QRhiTexture::UsedWithLoadStore : QRhiTexture::Flags()));
    if (!t->create())
        fail(QStringLiteral("cannot make a texture of %1 x %2").arg(size.width()).arg(size.height()));
    return t;
}

QRhiBuffer *GpuDecoder::storage(const void *data, size_t bytes)
{
    QRhiBuffer *b = own(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::StorageBuffer, quint32(std::max<size_t>(bytes, 16))));
    if (!b->create())
        fail("cannot make a buffer");
    if (!m_updates)
        m_updates = m_rhi->nextResourceUpdateBatch();
    std::vector<char> padded(std::max<size_t>(bytes, 16));
    std::memcpy(padded.data(), data, bytes);
    m_updates->uploadStaticBuffer(b, padded.data());
    return b;
}

// Dynamic, as QRhi's Direct3D backends require of every uniform buffer, and
// written at the start of the frame that uses it.
template<typename Block>
QRhiBuffer *GpuDecoder::uniform(const Block &block)
{
    QRhiBuffer *b = own(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof block));
    if (!b->create())
        fail("cannot make a uniform buffer");
    const char *bytes = reinterpret_cast<const char *>(&block);
    m_uniforms.push_back({b, std::vector<char>(bytes, bytes + sizeof block)});
    return b;
}

QRhiShaderResourceBinding GpuDecoder::input(int binding, QRhiTexture *texture) const
{
    return B::sampledTexture(binding, kCompute, texture, m_nearest);
}

QRhiShaderResourceBinding GpuDecoder::output(int binding, QRhiTexture *texture) const
{
    return B::imageStore(binding, kCompute, texture, 0);
}

void GpuDecoder::upload(QRhiTexture *texture, const void *data, size_t bytes)
{
    if (!m_updates)
        m_updates = m_rhi->nextResourceUpdateBatch();
    QRhiTextureSubresourceUploadDescription description(data, quint32(bytes));
    m_updates->uploadTexture(texture, QRhiTextureUploadDescription({0, 0, description}));
}

void GpuDecoder::pass(const char *kernel, std::initializer_list<B> bindings, QSize size)
{
    passGroups(kernel, bindings, (size.width() + kGroup - 1) / kGroup, (size.height() + kGroup - 1) / kGroup);
}

void GpuDecoder::passGroups(const char *kernel, std::initializer_list<B> bindings, int groupsX, int groupsY)
{
    QRhiShaderResourceBindings *srb = own(m_rhi->newShaderResourceBindings());
    srb->setBindings(bindings);
    if (!srb->create())
        fail(QStringLiteral("cannot bind %1's resources").arg(QLatin1StringView(kernel)));
    // built once and kept, with a layout of its own: the bindings it was
    // built with go with their decode
    QRhiComputePipeline *&pipeline = m_pipelines[kernel];
    if (!pipeline) {
        QRhiShaderResourceBindings *layout = m_rhi->newShaderResourceBindings();
        m_kept.emplace_back(layout);
        layout->setBindings(bindings);
        if (!layout->create())
            fail(QStringLiteral("cannot lay out %1's resources").arg(QLatin1StringView(kernel)));
        pipeline = m_rhi->newComputePipeline();
        m_kept.emplace_back(pipeline);
        pipeline->setShaderStage({QRhiShaderStage::Compute, shader(kernel)});
        pipeline->setShaderResourceBindings(layout);
        if (!pipeline->create())
            fail(QStringLiteral("cannot build %1").arg(QLatin1StringView(kernel)));
    }
    m_passes.push_back({pipeline, srb, groupsX, groupsY});
}

void GpuDecoder::run(const std::vector<std::pair<QRhiTexture *, QRhiReadbackResult *>> &readbacks)
{
    QElapsedTimer clock;
    clock.start();
    QRhiCommandBuffer *cb = nullptr;
    if (m_rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
        fail("cannot start a frame");
    QRhiResourceUpdateBatch *updates = m_updates ? m_updates : m_rhi->nextResourceUpdateBatch();
    m_updates = nullptr;
    for (const Uniform &u : m_uniforms)
        updates->updateDynamicBuffer(u.buffer, 0, quint32(u.data.size()), u.data.data());
    m_uniforms.clear();
    // Each kernel reads what the one before it wrote. Direct3D 11 refuses a
    // texture as a kernel's input while it is still bound as the last one's
    // output, and reads zeros instead; there each kernel gets a pass of its
    // own, which unbinds everything at its end.
    const bool passPerKernel = m_rhi->backend() == QRhi::D3D11;
    cb->beginComputePass(updates);
    for (size_t i = 0; i < m_passes.size(); ++i) {
        if (passPerKernel && i > 0) {
            cb->endComputePass();
            cb->beginComputePass();
        }
        cb->setComputePipeline(m_passes[i].pipeline);
        cb->setShaderResources(m_passes[i].bindings);
        cb->dispatch(m_passes[i].groupsX, m_passes[i].groupsY, 1);
    }
    m_passes.clear();
    QRhiResourceUpdateBatch *back = nullptr;
    if (!readbacks.empty()) {
        back = m_rhi->nextResourceUpdateBatch();
        for (const auto &[texture, result] : readbacks)
            back->readBackTexture(QRhiReadbackDescription(texture), result);
    }
    cb->endComputePass(back);
    m_rhi->endOffscreenFrame();
    // Direct3D 11's offscreen frames return before the GPU is done.
    m_rhi->finish();
    m_seconds += double(clock.nsecsElapsed()) / 1e9;
}

void GpuDecoder::blurAxis(QRhiTexture *from, QRhiTexture *to, const std::vector<float> &taps, int axis, int mode,
                          QRhiTexture *aux)
{
    BlurBlock block{};
    if (taps.size() > 8 || taps.size() % 2 == 0)
        fail("a blur of more than 7 taps, or of an even number");
    std::copy(taps.begin(), taps.end(), block.taps);
    block.params = {int32_t(taps.size() / 2), axis, mode, 0};
    pass("raw_blur", {input(0, from), input(1, aux ? aux : from), output(2, to), B::uniformBuffer(3, kCompute, uniform(block))},
         to->pixelSize());
}

void GpuDecoder::boxMean(QRhiTexture *from, QRhiTexture *to, QRhiTexture *scratch, int radius)
{
    pass("raw_box", {input(0, from), output(1, scratch), B::uniformBuffer(2, kCompute, uniform(BoxBlock{{radius, 0, 0, 0}}))},
         scratch->pixelSize());
    pass("raw_box", {input(0, scratch), output(1, to), B::uniformBuffer(2, kCompute, uniform(BoxBlock{{radius, 1, 0, 0}}))},
         to->pixelSize());
}

DecodedPicture GpuDecoder::decode(DecodePlan &&plan, double *seconds)
{
    // whatever a decode that failed left
    m_passes.clear();
    m_uniforms.clear();
    m_resources.clear();
    if (m_updates) {
        m_updates->release();
        m_updates = nullptr;
    }
    m_seconds = 0;
    m_nearest = own(m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                      QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    if (!m_nearest->create())
        fail("cannot make a sampler");
    DecodedPicture result = std::move(plan.result);
    const DecodeOptions &options = plan.options;
    const int w = plan.width, h = plan.height;
    const QSize full(w, h);
    constexpr auto RGBA = QRhiTexture::RGBA32F, R = QRhiTexture::R32F;

    // 1 — the demosaic, and the highlights' block sums
    QRhiTexture *mosaic = texture(R, full, false);
    upload(mosaic, plan.mosaic.data(), plan.mosaic.size() * sizeof(float));
    plan.mosaic = std::vector<float>();
    QRhiTexture *a = texture(RGBA, full, true), *b = texture(RGBA, full, true);
    if (plan.cfaSize == 2) {
        const PatternBlock pattern{{plan.cfa[0][0], plan.cfa[0][1], plan.cfa[1][0], plan.cfa[1][1]}};
        pass("raw_demosaic_mhc", {input(0, mosaic), output(1, a), B::uniformBuffer(2, kCompute, uniform(pattern))}, full);
    } else {
        PatternXBlock pattern{};
        for (int i = 0; i < 36; ++i)
            pattern.cfa[i] = plan.cfa[i / 6][i % 6];
        pattern.size = {plan.cfaSize, 0, 0, 0};
        QRhiBuffer *u = uniform(pattern);
        QRhiTexture *green = texture(R, full, true);
        pass("raw_demosaic_green", {input(0, mosaic), output(1, green), B::uniformBuffer(2, kCompute, u)}, full);
        pass("raw_demosaic_colours", {input(0, mosaic), input(1, green), output(2, a), B::uniformBuffer(3, kCompute, u)},
             full);
    }
    const bool highlights = !options.cameraRGB;
    const int block = highlightBlock(w, h);
    const int bw = (w + block - 1) / block, bh = (h + block - 1) / block;
    const float3 balance = result.whiteBalance;
    QRhiReadbackResult sums;
    if (highlights) {
        QRhiTexture *blocks = texture(RGBA, QSize(bw, bh), true);
        const HighlightsBlock u{f4(balance), f4(plan.lightNeutral), {block, 0, 0, 0}};
        passGroups("raw_highlight_blocks", {input(0, a), output(1, blocks), B::uniformBuffer(2, kCompute, uniform(u))}, bw, bh);
        run({{blocks, &sums}});
    } else {
        run({});
    }
    std::vector<float> colours(size_t(bw) * bh * 4);
    if (highlights) {
        if (size_t(sums.data.size()) != colours.size() * sizeof(float))
            fail("the highlights' readback has the wrong size");
        std::vector<float> s(colours.size());
        std::memcpy(s.data(), sums.data.constData(), s.size() * sizeof(float));
        colours = highlightColours(s, bw, bh);
    }
    QRhiTexture *blockColours = texture(RGBA, QSize(bw, bh), false);
    upload(blockColours, colours.data(), colours.size() * sizeof(float));

    // 2 — the colour, and the noise's samples
    ColourBlock colour{f4(balance), f4(plan.lightNeutral), {}, {plan.gain, 0, 0, 0}, {block, highlights ? 1 : 0, 0, 0}};
    for (int c = 0; c < 3; ++c)
        colour.matrix[c] = f4(plan.toRec2020.c[c]);
    pass("raw_colour", {input(0, a), input(1, blockColours), output(2, b), B::uniformBuffer(3, kCompute, uniform(colour))},
         full);
    const bool repair = options.falseColour || options.colourNoiseReduction;
    const bool detail = !options.cameraRGB && (repair || options.sharpening);
    PictureNoise noise;
    const QSize halfSize((w + 1) / 2, (h + 1) / 2);
    const QSize fullGrid(sampleCount(w), sampleCount(h)), halfGrid(sampleCount(halfSize.width()), sampleCount(halfSize.height()));
    if (detail && !fullGrid.isEmpty() && !halfGrid.isEmpty()) {
        QRhiTexture *fullSamples = texture(RGBA, fullGrid, true), *halfSamples = texture(RGBA, halfGrid, true);
        pass("raw_noise_samples", {input(0, b), output(1, fullSamples),
                                   B::uniformBuffer(2, kCompute, uniform(SamplesBlock{{1, kNoiseStep, w, h}}))}, fullGrid);
        pass("raw_noise_samples", {input(0, b), output(1, halfSamples),
                                   B::uniformBuffer(2, kCompute, uniform(SamplesBlock{{2, kNoiseStep, halfSize.width(),
                                                                                       halfSize.height()}}))}, halfGrid);
        QRhiReadbackResult fullBack, halfBack;
        run({{fullSamples, &fullBack}, {halfSamples, &halfBack}});
        auto fit = [](const QRhiReadbackResult &r, QSize grid) {
            const size_t n = size_t(grid.width()) * grid.height();
            if (size_t(r.data.size()) != n * 16)
                fail("the noise's readback has the wrong size");
            std::vector<float2> samples(n);
            const float *f = reinterpret_cast<const float *>(r.data.constData());
            for (size_t i = 0; i < n; ++i)
                samples[i] = {f[i * 4], f[i * 4 + 1]};
            return fitNoise(samples);
        };
        noise.full = fit(fullBack, fullGrid);
        noise.half = fit(halfBack, halfGrid);
        result.noise[0] = noise.full.a;
        result.noise[1] = noise.full.b;
        result.noise[2] = noise.half.a;
        result.noise[3] = noise.half.b;
    }

    // 3 — the detail steps, the lens, turned upright, scaled
    QRhiTexture *current = b, *spare = a;
    auto done = [&] { std::swap(current, spare); };
    if (detail && repair) {
        pass("raw_repair", {input(0, current), output(1, spare),
                            B::uniformBuffer(2, kCompute, uniform(NoiseBlock{noiseOf(noise.full)}))}, full);
        done();
    }
    if (detail && options.falseColour) {
        pass("raw_false_colour", {input(0, current), output(1, spare)}, full);
        done();
    }
    if (detail && options.colourNoiseReduction) {
        // as RawDetail's: a window of 5 x 5 at half size on a picture 6000 wide
        const int r = std::max(1, int(std::lround(2.0 * std::max(w, h) / 6000)));
        QRhiTexture *guide = texture(R, full, true);
        pass("raw_cnr_guide", {input(0, current), output(1, guide)}, full);
        QRhiTexture *first = texture(RGBA, halfSize, true), *second = texture(RGBA, halfSize, true);
        QRhiTexture *scratch = texture(RGBA, halfSize, true), *meanFirst = texture(RGBA, halfSize, true);
        QRhiTexture *meanSecond = texture(RGBA, halfSize, true), *ab = texture(RGBA, halfSize, true);
        QRhiTexture *meanAb = texture(RGBA, halfSize, true);
        pass("raw_cnr_half", {input(0, current), input(1, guide), output(2, first), output(3, second)}, halfSize);
        boxMean(first, meanFirst, scratch, r);
        boxMean(second, meanSecond, scratch, r);
        pass("raw_cnr_ab", {input(0, meanFirst), input(1, meanSecond), output(2, ab),
                            B::uniformBuffer(3, kCompute, uniform(NoiseBlock{noiseOf(noise.half)}))}, halfSize);
        boxMean(ab, meanAb, scratch, r);
        pass("raw_cnr_apply", {input(0, meanAb), input(1, guide), output(2, spare)}, full);
        done();
    }
    if (detail && options.sharpening) {
        QRhiTexture *observed = texture(R, full, true), *luminance = texture(R, full, true);
        QRhiTexture *estimate = texture(R, full, true), *next = texture(R, full, true);
        QRhiTexture *scratch = texture(R, full, true), *ratio = texture(R, full, true);
        QRhiTexture *mean = texture(R, full, true), *energy = texture(R, full, true);
        pass("raw_sharp_luma", {input(0, current), output(1, observed), output(2, luminance), output(3, estimate)}, full);
        // Richardson-Lucy, as sharpenCapture's defaults
        const std::vector<float> gauss = sharpeningKernel(0.7f), box3 = {1 / 3.0f, 1 / 3.0f, 1 / 3.0f};
        for (int k = 0; k < 10; ++k) {
            blurAxis(estimate, scratch, gauss, 0, 0, nullptr);
            blurAxis(scratch, ratio, gauss, 1, 1, observed);
            blurAxis(ratio, scratch, gauss, 0, 0, nullptr);
            blurAxis(scratch, next, gauss, 1, 2, estimate);
            std::swap(estimate, next);
        }
        blurAxis(observed, scratch, box3, 0, 0, nullptr);
        blurAxis(scratch, mean, box3, 1, 0, nullptr);
        pass("raw_sharp_energy", {input(0, observed), input(1, mean), output(2, ratio)}, full);
        blurAxis(ratio, scratch, box3, 0, 0, nullptr);
        blurAxis(scratch, energy, box3, 1, 0, nullptr);
        pass("raw_sharp_apply", {input(0, current), input(1, observed), input(2, luminance), input(3, estimate),
                                 input(4, mean), input(5, energy), output(6, spare),
                                 B::uniformBuffer(7, kCompute, uniform(NoiseBlock{noiseOf(noise.full)}))}, full);
        done();
    }
    const Distortion &lens = result.distortion;
    if (lens.model != Distortion::None && !(lens.model == Distortion::DngWarp && lens.planes.empty())) {
        LensBlock u{};
        u.k = {lens.k[0], lens.k[1], lens.k[2], lens.k[3]};
        u.shape = {lens.radius, lens.zoom, lens.profileStep, 0};
        u.centre = {float(lens.cx), float(lens.cy), 0, 0};
        for (size_t p = 0; p < lens.planes.size() && p < 3; ++p) {
            const Distortion::Plane &plane = lens.planes[p];
            u.planes[2 * p] = {float(plane.kr[0]), float(plane.kr[1]), float(plane.kr[2]), float(plane.kr[3])};
            u.planes[2 * p + 1] = {float(plane.kt[0]), float(plane.kt[1]), 0, 0};
        }
        // Distortion::Model numbers the models as the kernel does
        u.model = {int32_t(lens.model), int32_t(lens.profile.size()), int32_t(std::min<size_t>(lens.planes.size(), 3)), 0};
        const float none = 1;
        QRhiBuffer *profile = lens.profile.empty() ? storage(&none, sizeof none)
                                                   : storage(lens.profile.data(), lens.profile.size() * sizeof(float));
        pass("raw_lens", {input(0, current), output(1, spare), B::bufferLoad(2, kCompute, profile),
                          B::uniformBuffer(3, kCompute, uniform(u))}, full);
        done();
    }
    QRhiTexture *turned = current;
    if (plan.orientation == 3 || plan.orientation == 6 || plan.orientation == 8) {
        turned = texture(RGBA, QSize(plan.turnedWidth, plan.turnedHeight), true);
        pass("raw_turn", {input(0, current), output(1, turned),
                          B::uniformBuffer(2, kCompute, uniform(TurnBlock{{plan.orientation, 0, 0, 0}}))},
             turned->pixelSize());
    }
    QRhiTexture *final = turned;
    if (plan.scale < 1) {
        // RawLoader's: Core Image drops a scaled height's fraction of a row at
        // the top (scaleDown)
        std::vector<int> firstX, firstY;
        std::vector<float> weightsX, weightsY;
        int tapsX = 0, tapsY = 0;
        lanczosTaps(plan.outWidth, plan.scale, 0, firstX, weightsX, tapsX);
        lanczosTaps(plan.outHeight, plan.scale, plan.turnedHeight * plan.scale - plan.outHeight, firstY, weightsY, tapsY);
        QRhiTexture *rows = texture(RGBA, QSize(plan.outWidth, plan.turnedHeight), true);
        final = texture(RGBA, QSize(plan.outWidth, plan.outHeight), true);
        pass("raw_lanczos", {input(0, turned), output(1, rows),
                             B::bufferLoad(2, kCompute, storage(weightsX.data(), weightsX.size() * sizeof(float))),
                             B::bufferLoad(3, kCompute, storage(firstX.data(), firstX.size() * sizeof(int))),
                             B::uniformBuffer(4, kCompute, uniform(ScaleBlock{{tapsX, 0, 0, 0}}))}, rows->pixelSize());
        pass("raw_lanczos", {input(0, rows), output(1, final),
                             B::bufferLoad(2, kCompute, storage(weightsY.data(), weightsY.size() * sizeof(float))),
                             B::bufferLoad(3, kCompute, storage(firstY.data(), firstY.size() * sizeof(int))),
                             B::uniformBuffer(4, kCompute, uniform(ScaleBlock{{tapsY, 1, 0, 0}}))}, final->pixelSize());
    }
    QRhiReadbackResult picture;
    run({{final, &picture}});
    const size_t pixels = size_t(plan.outWidth) * plan.outHeight;
    if (size_t(picture.data.size()) != pixels * 16)
        fail(QStringLiteral("the picture's readback has %1 bytes, expected %2").arg(picture.data.size()).arg(pixels * 16));
    Dump &out = result.pixels;
    out.width = plan.outWidth;
    out.height = plan.outHeight;
    out.channels = 4;
    out.floats.resize(pixels * 4);
    std::memcpy(out.floats.data(), picture.data.constData(), pixels * 16);

    m_passes.clear();
    m_resources.clear();
    if (seconds)
        *seconds = m_seconds;
    return result;
}
