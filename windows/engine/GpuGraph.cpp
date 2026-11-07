#include "GpuGraph.h"

#include "Filters.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFloat16>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace {

constexpr int kGroup = 16;          // local_size_x and _y of every kernel

struct ConvolveBlock {
    IVec4 params;                   // radius, axis
};
struct EffectBlock {
    Vec4 a, b;
    IVec4 flags;
};

QShader shader(const char *kernel)
{
    QFile f(QStringLiteral(":/shaders/%1.comp.qsb").arg(QLatin1StringView(kernel)));
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error(QStringLiteral("no shader %1").arg(QLatin1StringView(kernel)).toStdString());
    return QShader::fromSerialized(f.readAll());
}

} // namespace

void GpuGraph::fail(const QString &what)
{
    throw std::runtime_error(what.toStdString());
}

GpuGraph::GpuGraph(QRhi *rhi)
    : m_rhi(rhi)
{
    if (!rhi->isFeatureSupported(QRhi::Compute))
        fail(QStringLiteral("this GPU backend has no compute shaders"));
    m_nearest = rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    m_linear = rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                               QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    m_kept.emplace_back(m_nearest);
    m_kept.emplace_back(m_linear);
    if (!m_nearest->create() || !m_linear->create())
        fail(QStringLiteral("cannot make the samplers"));
}

GpuGraph::~GpuGraph()
{
    if (m_pending)
        m_pending->release();
    if (m_batch)
        m_batch->release();
}

void GpuGraph::beginLayout()
{
    m_passes.clear();
    m_uniforms.clear();
    for (auto &c : m_storageCache)
        c.used = false;
    for (auto &c : m_uniformCache)
        c.used = false;
    for (auto &c : m_textureCache)
        c.used = false;
    for (auto &c : m_bindingsCache)
        c.used = false;
    m_storageNext = m_uniformNext = m_textureNext = 0;
    m_batch = m_pending ? m_pending : m_rhi->nextResourceUpdateBatch();
    m_pending = nullptr;
}

// The uploads go with the next frame's; what this layout didn't use of the
// last one's goes, the bindings first, as they refer to the rest.
void GpuGraph::endLayout()
{
    m_pending = m_batch;
    m_batch = nullptr;
    retireUnused();
}

void GpuGraph::clearLayout()
{
    m_passes.clear();
    m_bindingsCache.clear();
}

void GpuGraph::retireUnused()
{
    std::erase_if(m_bindingsCache, [](const CachedBindings &c) { return !c.used; });
    auto trim = [](auto &cache, size_t next) {
        if (cache.size() > next)
            cache.resize(next);
    };
    trim(m_storageCache, m_storageNext);
    trim(m_uniformCache, m_uniformNext);
    trim(m_textureCache, m_textureNext);
    m_retired.clear();
}

// A layout's small texture: the last layout's at this place where it has the
// same format and size.
QRhiTexture *GpuGraph::texture(QRhiTexture::Format format, QSize size, bool loadStore)
{
    const size_t i = m_textureNext++;
    if (i < m_textureCache.size()) {
        QRhiTexture *t = m_textureCache[i].resource.get();
        if (t->format() == format && t->pixelSize() == size
            && bool(t->flags() & QRhiTexture::UsedWithLoadStore) == loadStore) {
            m_textureCache[i].used = true;
            return t;
        }
        m_retired.push_back(std::move(m_textureCache[i].resource));
    } else {
        m_textureCache.emplace_back();
    }
    auto t = std::unique_ptr<QRhiTexture>(
        m_rhi->newTexture(format, size, 1, loadStore ? QRhiTexture::UsedWithLoadStore : QRhiTexture::Flags()));
    noteTexture("graph", size, format);
    if (!t->create())
        fail(QStringLiteral("cannot make a texture"));
    m_textureCache[i] = {std::move(t), true};
    return m_textureCache[i].resource.get();
}

QRhiBuffer *GpuGraph::storage(const void *data, size_t bytes)
{
    // padded: QRhi makes no empty buffers
    bytes = std::max<size_t>(bytes, 16);
    const size_t i = m_storageNext++;
    QRhiBuffer *b = nullptr;
    if (i < m_storageCache.size() && m_storageCache[i].resource->size() == quint32(bytes)) {
        b = m_storageCache[i].resource.get();
        m_storageCache[i].used = true;
    } else {
        if (i < m_storageCache.size())
            m_retired.push_back(std::move(m_storageCache[i].resource));
        else
            m_storageCache.emplace_back();
        auto made = std::unique_ptr<QRhiBuffer>(
            m_rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, quint32(bytes)));
        if (!made->create())
            fail(QStringLiteral("cannot make a buffer"));
        b = made.get();
        m_storageCache[i] = {std::move(made), true};
    }
    std::vector<char> padded(bytes, 0);
    if (data)
        std::memcpy(padded.data(), data, std::min(bytes, padded.size()));
    m_batch->uploadStaticBuffer(b, padded.data());
    return b;
}

void GpuGraph::upload(QRhiTexture *t, const void *data, size_t bytes)
{
    m_batch->uploadTexture(t, QRhiTextureUploadDescription(
        {0, 0, QRhiTextureSubresourceUploadDescription(data, quint32(bytes))}));
}

QRhiShaderResourceBinding GpuGraph::input(int binding, QRhiTexture *texture) const
{
    return B::sampledTexture(binding, kCompute, texture, m_nearest);
}

void GpuGraph::pass(const char *kernel, std::initializer_list<B> bindings)
{
    // the last layout's bindings, where they bind the same
    QRhiShaderResourceBindings *srb = nullptr;
    const std::vector<B> wanted(bindings);
    for (CachedBindings &c : m_bindingsCache) {
        if (!c.used && c.kernel == kernel && c.bindings == wanted) {
            c.used = true;
            srb = c.srb.get();
            break;
        }
    }
    if (!srb) {
        auto made = std::unique_ptr<QRhiShaderResourceBindings>(m_rhi->newShaderResourceBindings());
        made->setBindings(bindings);
        if (!made->create())
            fail(QStringLiteral("cannot bind %1's resources").arg(QLatin1StringView(kernel)));
        srb = made.get();
        m_bindingsCache.push_back({kernel, wanted, std::move(made), true});
    }
    // Built once and kept, with a layout of its own: the bindings it was
    // built with go with their layout.
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
    m_passes.push_back({pipeline, srb, m_passSize});
}

QRhiTexture *GpuGraph::blurAll(QRhiTexture *from, QRhiTexture *to, QRhiTexture *temp, float sigma)
{
    if (!(sigma >= 0.3f))
        return from;
    const std::vector<float> taps = gaussianTaps(sigma);
    QRhiBuffer *w = storage(taps.data(), taps.size() * sizeof(float));
    const int radius = int(taps.size() / 2);
    pass("effects_blur", {input(0, from), B::imageStore(1, kCompute, temp, 0), B::bufferLoad(2, kCompute, w),
                          B::uniformBuffer(3, kCompute, uniform(ConvolveBlock{{radius, 0, 0, 0}}))});
    pass("effects_blur", {input(0, temp), B::imageStore(1, kCompute, to, 0), B::bufferLoad(2, kCompute, w),
                          B::uniformBuffer(3, kCompute, uniform(ConvolveBlock{{radius, 1, 0, 0}}))});
    return to;
}

QRhiTexture *GpuGraph::applyEffects(QRhiTexture *print, const EffectsParams &e, QRhiTexture::Format working)
{
    const QSize size = print->pixelSize();
    if (!m_effectOut || m_effectOut->pixelSize() != size || m_effect[0]->format() != working) {
        m_effectResources.clear();
        auto make = [&](QRhiTexture::Format format) {
            QRhiTexture *t = m_rhi->newTexture(format, size, 1, QRhiTexture::UsedWithLoadStore);
            noteTexture("effects", size, format);
            m_effectResources.emplace_back(t);
            if (!t->create())
                fail(QStringLiteral("cannot make a texture for the effects"));
            return t;
        };
        for (QRhiTexture *&t : m_effect)
            t = make(working);
        m_effectOut = make(QRhiTexture::RGBA32F);
    }
    const bool lights = e.glow || e.halation;
    QRhiTexture **t = m_effect;
    QRhiTexture *current = print;
    if (e.bloom) {
        pass("effects_bloom_select", {input(0, print), B::imageStore(1, kCompute, t[0], 0),
                                      B::uniformBuffer(2, kCompute, uniform(EffectBlock{{e.bloomT0, e.bloomT1, 0, 0}, {}, {}}))});
        QRhiTexture *layer = blurAll(t[0], t[1], t[2], e.bloomSigma);
        QRhiTexture *out = !lights && !(e.haze > 0) ? m_effectOut : t[3];
        pass("effects_bloom_composite",
             {input(0, print), input(1, layer), B::imageStore(2, kCompute, out, 0),
              B::uniformBuffer(3, kCompute, uniform(EffectBlock{{e.bloomAmount, 0, 0, 0}, {}, {e.bloomMode, 0, 0, 0}}))});
        current = out;
    }
    if (e.haze > 0) {
        // energy-conserving scatter: a share of all light spread wide, lifting the shadows
        QRhiTexture *veil = blurAll(current, t[1], t[2], e.hazeSigma);
        QRhiTexture *out = !lights ? m_effectOut : (current == t[3] ? t[0] : t[3]);
        pass("effects_mix", {input(0, current), input(1, veil), B::imageStore(2, kCompute, out, 0),
                             B::uniformBuffer(3, kCompute, uniform(EffectBlock{{e.haze, 0, 0, 0}, {}, {}}))});
        current = out;
    }
    if (!lights)
        return current;
    // the working textures the bloomed print does not hold: one each for
    // glow's source, the blurs' first axis, glow's layer, halation's source
    // and halation's layer
    std::vector<QRhiTexture *> f;
    for (QRhiTexture *x : m_effect)
        if (x != current)
            f.push_back(x);
    QRhiTexture *glowLayer = print, *halationLayer = print;
    if (e.glow) {
        pass("effects_glow_select", {input(0, print), B::imageStore(1, kCompute, f[0], 0),
                                     B::uniformBuffer(2, kCompute, uniform(EffectBlock{{e.glowT0, 1, 0, 0}, {}, {}}))});
        glowLayer = blurAll(f[0], f[2], f[1], e.glowSigma);
    }
    if (e.halation) {
        pass("effects_halation_select",
             {input(0, print), B::imageStore(1, kCompute, f[3], 0),
              B::uniformBuffer(2, kCompute, uniform(EffectBlock{{e.halationT0, e.halationT1, 0, 0},
                                                                {e.tint[0], e.tint[1], e.tint[2], 0}, {}}))});
        halationLayer = blurAll(f[3], f[4], f[1], e.halationSigma);
    }
    pass("effects_light_combine",
         {input(0, current), input(1, print), input(2, glowLayer), input(3, halationLayer),
          B::imageStore(4, kCompute, m_effectOut, 0),
          B::uniformBuffer(5, kCompute, uniform(EffectBlock{{e.glow ? e.glowAmount : 0, e.halation ? e.halationAmount : 0,
                                                             e.halationT0, e.halationT1},
                                                            {},
                                                            {e.glow ? 1 : 0, e.halation ? 1 : 0, e.haloOnly ? 1 : 0, 0}}))});
    return m_effectOut;
}

double GpuGraph::run(const std::vector<std::pair<QRhiTexture *, QRhiReadbackResult *>> &readbacks, size_t first,
                     const std::vector<std::pair<QRhiBuffer *, QRhiReadbackResult *>> &buffers)
{
    QElapsedTimer clock;
    clock.start();
    QRhiCommandBuffer *cb = nullptr;
    if (m_rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
        fail(QStringLiteral("cannot start a frame"));
    QRhiResourceUpdateBatch *uniforms = m_pending ? m_pending : m_rhi->nextResourceUpdateBatch();
    m_pending = nullptr;
    for (const Uniform &u : m_uniforms)
        uniforms->updateDynamicBuffer(u.buffer, 0, quint32(u.data.size()), u.data.data());

    // Each kernel reads what the one before it wrote. Direct3D 11 refuses a
    // texture as a kernel's input while it is still bound as the last one's
    // output, and reads zeros instead; QRhi binds the new inputs before it
    // unbinds the old outputs, so there each kernel gets a pass of its own,
    // which unbinds everything at its end.
    const bool passPerKernel = m_rhi->backend() == QRhi::D3D11;
    cb->beginComputePass(uniforms);
    for (size_t i = first; i < m_passes.size(); ++i) {
        if (passPerKernel && i > first) {
            cb->endComputePass();
            cb->beginComputePass();
        }
        cb->setComputePipeline(m_passes[i].pipeline);
        cb->setShaderResources(m_passes[i].bindings);
        const QSize &n = m_passes[i].size;
        cb->dispatch((n.width() + kGroup - 1) / kGroup, (n.height() + kGroup - 1) / kGroup, 1);
    }
    QRhiResourceUpdateBatch *readback = nullptr;
    if (!readbacks.empty() || !buffers.empty()) {
        readback = m_rhi->nextResourceUpdateBatch();
        for (const auto &[texture, result] : readbacks)
            readback->readBackTexture(QRhiReadbackDescription(texture), result);
        for (const auto &[buffer, result] : buffers)
            readback->readBackBuffer(buffer, 0, buffer->size(), result);
    }
    cb->endComputePass(readback);
    m_rhi->endOffscreenFrame();
    // Direct3D 11's offscreen frames return before the GPU is done.
    m_rhi->finish();
    return double(clock.nsecsElapsed()) / 1e9;
}

std::vector<float> GpuGraph::floats(const QRhiReadbackResult &r, QRhiTexture *texture) const
{
    const QRhiTexture::Format format = texture->format();
    const size_t pixels = size_t(texture->pixelSize().width()) * texture->pixelSize().height();
    const size_t bytes = format == QRhiTexture::RGBA16F ? pixels * 8 : pixels * 16;
    if (size_t(r.data.size()) != bytes)
        fail(QStringLiteral("a float readback has %1 bytes, expected %2").arg(r.data.size()).arg(bytes));
    std::vector<float> out(pixels * 4);
    if (format == QRhiTexture::RGBA16F)
        qFloatFromFloat16(out.data(), reinterpret_cast<const qfloat16 *>(r.data.constData()), qsizetype(pixels * 4));
    else
        std::memcpy(out.data(), r.data.constData(), bytes);
    return out;
}
