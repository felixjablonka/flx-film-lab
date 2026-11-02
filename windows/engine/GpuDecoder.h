#pragma once

#include "RawDecoder.h"

#include <rhi/qrhi.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// A decode's pixels on the GPU (QRhi compute), the same steps by the same
// arithmetic as decodePlan on the CPU: the demosaic, the highlights, the
// colour, the detail steps, the lens's distortion, turning upright and
// scaling. What needs the whole picture the CPU works out between the GPU's
// frames, from small readbacks: the highlights' block colours and the
// noise's fit.
class GpuDecoder
{
public:
    explicit GpuDecoder(QRhi *rhi);
    ~GpuDecoder();

    // The plan's pixels, read back into the result's. `seconds`: the wall
    // time of the GPU's frames. Throws std::runtime_error.
    DecodedPicture decode(DecodePlan &&plan, double *seconds = nullptr);

private:
    struct Pass {
        QRhiComputePipeline *pipeline;
        QRhiShaderResourceBindings *bindings;
        int groupsX, groupsY;
    };
    struct Uniform {
        QRhiBuffer *buffer;
        std::vector<char> data;
    };
    using B = QRhiShaderResourceBinding;

    template<typename T>
    T *own(T *resource);
    QRhiTexture *texture(QRhiTexture::Format format, QSize size, bool loadStore);
    QRhiBuffer *storage(const void *data, size_t bytes);
    template<typename Block>
    QRhiBuffer *uniform(const Block &block);
    B input(int binding, QRhiTexture *texture) const;
    B output(int binding, QRhiTexture *texture) const;
    void upload(QRhiTexture *texture, const void *data, size_t bytes);
    // a kernel run once a pixel of `size`, or over `groupsX` x `groupsY` groups
    void pass(const char *kernel, std::initializer_list<B> bindings, QSize size);
    void passGroups(const char *kernel, std::initializer_list<B> bindings, int groupsX, int groupsY);
    // the passes so far in a frame, the uploads before them and the
    // readbacks after; then the GPU's done
    void run(const std::vector<std::pair<QRhiTexture *, QRhiReadbackResult *>> &readbacks);

    // A blur of a single plane along one axis (raw_blur), its second axis
    // optionally ending a Richardson-Lucy step against `aux`.
    void blurAxis(QRhiTexture *from, QRhiTexture *to, const std::vector<float> &taps, int axis, int mode,
                  QRhiTexture *aux);
    // A box mean over (2r + 1)^2, in two passes through `scratch`.
    void boxMean(QRhiTexture *from, QRhiTexture *to, QRhiTexture *scratch, int radius);

    QRhi *m_rhi;
    QRhiSampler *m_nearest = nullptr;
    std::vector<std::unique_ptr<QRhiResource>> m_resources;     // a decode's
    std::vector<std::unique_ptr<QRhiResource>> m_kept;          // the kernels, their layouts, the sampler
    std::map<std::string, QRhiComputePipeline *> m_pipelines;   // built once and kept
    QRhiResourceUpdateBatch *m_updates = nullptr;               // the next frame's uploads
    std::vector<Uniform> m_uniforms;                             // the next frame's uniforms
    std::vector<Pass> m_passes;                                  // the next frame's
    double m_seconds = 0;
};
