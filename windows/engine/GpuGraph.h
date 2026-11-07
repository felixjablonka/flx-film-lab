#pragma once

#include "Setup.h"

#include <rhi/qrhi.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// What the GPU stages share (the film renderer, Motion's frame pipeline): a
// layout of compute passes, laid out again for each setup and kept for the
// next where it asks for the same: the kernels, built once; buffers and small
// textures by their place in the order the layout makes them and their size;
// bindings by kernel and what they bind. A layout's uploads go with the next
// frame. And Glow's effects, which both lay on their finished picture.

// FLX_GPU_STATS: each texture of 10 MB or more as it is made, to see where
// the GPU's memory goes.
inline void noteTexture(const char *what, QSize size, QRhiTexture::Format format)
{
    static const bool on = qEnvironmentVariableIsSet("FLX_GPU_STATS");
    const int bytes = format == QRhiTexture::RGBA32F ? 16 : format == QRhiTexture::RGBA16F ? 8
        : format == QRhiTexture::R32F ? 4 : format == QRhiTexture::RGBA8 ? 4 : 4;
    const double mb = double(size.width()) * size.height() * bytes / 1e6;
    if (on && mb >= 10)
        std::fprintf(stderr, "texture %s %dx%d %.0f MB\n", what, size.width(), size.height(), mb);
}

class GpuGraph
{
public:
    explicit GpuGraph(QRhi *rhi);
    virtual ~GpuGraph();

protected:
    struct Pass {
        QRhiComputePipeline *pipeline;
        QRhiShaderResourceBindings *bindings;
        QSize size;                 // the pixels it runs over
    };
    struct Uniform {
        QRhiBuffer *buffer;
        std::vector<char> data;
    };
    using B = QRhiShaderResourceBinding;
    static constexpr auto kCompute = QRhiShaderResourceBinding::ComputeStage;

    // Laying out passes: everything the last layout made goes unused until
    // asked for again, and what isn't goes at the end.
    void beginLayout();
    void endLayout();
    // The passes and bindings gone, as when the textures they bind go.
    void clearLayout();

    QRhiTexture *texture(QRhiTexture::Format format, QSize size, bool loadStore);
    QRhiBuffer *storage(const void *data, size_t bytes);
    template<typename Block>
    QRhiBuffer *uniform(const Block &block);
    void upload(QRhiTexture *texture, const void *data, size_t bytes);
    // a texture a kernel only reads, by texelFetch
    B input(int binding, QRhiTexture *texture) const;
    // a kernel run once a pixel of m_passSize
    void pass(const char *kernel, std::initializer_list<B> bindings);

    // a Gaussian over all four channels, through `temp`; none below 0.3
    QRhiTexture *blurAll(QRhiTexture *from, QRhiTexture *to, QRhiTexture *temp, float sigma);
    // Glow's bloom, haze, glow and halation on `print`, in textures of its
    // size (`working` the format of all but the result, which is float32);
    // the print with them (GlowPipeline.apply)
    QRhiTexture *applyEffects(QRhiTexture *print, const EffectsParams &e, QRhiTexture::Format working);

    // Runs the passes from `first`, the layout's uploads before them and the
    // readbacks after, of textures and of storage buffers; the wall time, in
    // seconds.
    double run(const std::vector<std::pair<QRhiTexture *, QRhiReadbackResult *>> &readbacks, size_t first = 0,
               const std::vector<std::pair<QRhiBuffer *, QRhiReadbackResult *>> &buffers = {});
    // a float readback as float32, from half precision where it is
    std::vector<float> floats(const QRhiReadbackResult &r, QRhiTexture *texture) const;

    [[noreturn]] static void fail(const QString &what);

    QRhi *m_rhi;
    QRhiSampler *m_nearest = nullptr, *m_linear = nullptr;
    QRhiResourceUpdateBatch *m_batch = nullptr;     // during a layout
    QSize m_passSize;                               // what the passes laid out next run over
    std::vector<Pass> m_passes;
    std::vector<std::unique_ptr<QRhiResource>> m_kept; // the kernels, their layouts, the samplers

private:
    void retireUnused();

    template<typename R>
    struct Cached {
        std::unique_ptr<R> resource;
        bool used = false;
    };
    std::vector<Cached<QRhiBuffer>> m_storageCache, m_uniformCache;
    std::vector<Cached<QRhiTexture>> m_textureCache;
    struct CachedBindings {
        std::string kernel;
        std::vector<QRhiShaderResourceBinding> bindings;
        std::unique_ptr<QRhiShaderResourceBindings> srb;
        bool used = false;
    };
    std::vector<CachedBindings> m_bindingsCache;
    size_t m_storageNext = 0, m_uniformNext = 0, m_textureNext = 0;
    std::vector<std::unique_ptr<QRhiResource>> m_retired;   // replaced during a layout
    QRhiResourceUpdateBatch *m_pending = nullptr;           // a layout's uploads, for the next frame
    std::map<std::string, QRhiComputePipeline *> m_pipelines;
    std::vector<Uniform> m_uniforms;
    // the effects' textures at the print's size: six working, and the result
    // in float32, as the Mac's effectsOut
    std::vector<std::unique_ptr<QRhiResource>> m_effectResources;
    QRhiTexture *m_effect[6] = {}, *m_effectOut = nullptr;
};

// Dynamic, as QRhi's Direct3D backends require of every uniform buffer, and
// written at the start of each frame.
template<typename Block>
QRhiBuffer *GpuGraph::uniform(const Block &block)
{
    const size_t i = m_uniformNext++;
    QRhiBuffer *b = nullptr;
    if (i < m_uniformCache.size() && m_uniformCache[i].resource->size() == quint32(sizeof block)) {
        b = m_uniformCache[i].resource.get();
        m_uniformCache[i].used = true;
    } else {
        if (i < m_uniformCache.size())
            m_retired.push_back(std::move(m_uniformCache[i].resource));
        else
            m_uniformCache.emplace_back();
        auto made = std::unique_ptr<QRhiBuffer>(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof block));
        if (!made->create())
            fail(QStringLiteral("cannot make a uniform buffer"));
        b = made.get();
        m_uniformCache[i] = {std::move(made), true};
    }
    const char *bytes = reinterpret_cast<const char *>(&block);
    m_uniforms.push_back({b, std::vector<char>(bytes, bytes + sizeof block)});
    return b;
}
