#pragma once

#include "GpuGraph.h"
#include "Setup.h"

#include <rhi/qrhi.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The film engine on QRhi: Renderer.render for a frame as it is (not turned,
// levelled or cropped, without dodge & burn or Glow's effects): filming.expose,
// scatter and halation, the log, filming.develop, the DIR couplers, the grain,
// the fused print and scan, the grain's mean correction and the screen's
// encoding. It renders a setup the Mac engine made (RenderSetup), so what
// differs from the Mac's render is the port's doing alone.
class FilmRenderer : public GpuGraph
{
public:
    // Whose Gaussian blur to use: the port's own (gaussianTaps), or the
    // kernels flxref measured from Metal Performance Shaders at the same
    // sigmas, which tells what the port's blur changes from what the rest
    // of it does.
    enum class Blur { Own, Measured };

    FilmRenderer(QRhi *rhi, Blur blur);
    ~FilmRenderer() override;

    struct Taps {
        std::vector<float> logE;        // log_e_film, RGBA float32
        std::vector<float> cmy;         // cmy_film: the developed negative, with its grain
        std::vector<float> rgbOut;      // rgb_out: the linear print
        std::vector<uint8_t> display;   // the screen's 8-bit image, RGBA
    };
    struct BlurUse {
        float sigma;
        int taps;
    };

    // The picture the renders after it are of: RGBA float32, linear working
    // RGB, and the image-sized textures for it, kept until the next. Past
    // 12 MP the intermediates are half precision, as the Mac's
    // (Renderer.setImage). Throws std::runtime_error.
    // A part of a frame (the loupe's box) gives its frame's pixel count, so
    // it works in the frame's precision.
    void setPicture(const float *rgba, int width, int height, qint64 framePixels = 0);

    // Uploads the setup's tables and LUTs, and lays out every pass of the
    // render, for the picture set (or the setup's own, when it carries one).
    // The kernels, once built, are kept. Throws std::runtime_error.
    void load(const RenderSetup &setup);

    // Runs the passes once, reading the stages back into `taps` when it is
    // given. Returns the frame's wall time, in seconds.
    double render(Taps *taps);
    // The print's histogram (histogram_rgb): how many of its pixels hold each
    // of the 256 values an 8-bit file would, red's 256, then green's, then
    // blue's; of the finished print, and of what the curves are given when
    // there are curves.
    struct Histogram {
        std::vector<uint32_t> after, before;
    };
    // Runs them, reading back the screen's 8-bit image (RGBA), the linear
    // print (RGBA float32) and the histogram, each where asked for; the
    // histogram only when measured (setMeasuresHistogram).
    double render(std::vector<uint8_t> *display, std::vector<float> *linear, Histogram *histogram = nullptr);
    // Whether the passes laid out from now on measure the histogram: the
    // preview's do.
    void setMeasuresHistogram(bool measures) { m_measuresHistogram = measures; }

    QSize size() const { return m_size; }
    // the print's: the frame's, or a shaped print's own
    QSize outputSize() const { return m_display ? m_display->pixelSize() : m_size; }

    // The app's renders: where the film stage's inputs are the same as the
    // last render's (only the print's controls moved), it is not run again,
    // as the Mac's renderer skips it. Off, every render runs every pass.
    void setSkipsUnchangedFilm(bool skips) { m_skipsUnchangedFilm = skips; }

    // Dodge & burn's map (MaskRasterizer: RGBA, stops and local grade, on the
    // frame), or none when empty. Uploaded with the next load, only when it
    // has changed.
    void setDodgeBurn(std::vector<float> map, int width, int height);

    const std::vector<BlurUse> &blurs() const { return m_blurs; }
    int passCount() const { return int(m_passes.size()); }

private:
    uint64_t filmKey(const RenderSetup &s) const;
    double runPasses(const std::vector<std::pair<QRhiTexture *, QRhiReadbackResult *>> &readbacks,
                     const std::vector<std::pair<QRhiBuffer *, QRhiReadbackResult *>> &buffers = {});

    // The stages of Renderer.render that take more than one pass.
    void blur(QRhiTexture *from, QRhiTexture *to, float sigma);
    QRhiTexture *halation(QRhiTexture *source, QRhiTexture *destination, QRhiTexture *scratch1, QRhiTexture *scratch2);
    // where the grain's pixels lie in the frame, or in the shaped print
    struct Origin {
        uint32_t x, y;
    };
    void drawGrain(QRhiTexture *clean, QRhiTexture *target, QRhiTexture *scratch1, QRhiTexture *scratch2,
                   Origin origin);
    void printNegative(QRhiTexture *negative, QRhiTexture *into);

    Blur m_blurKind;
    const RenderSetup *m_setup = nullptr;           // during load()
    // the film stage's passes, and what its inputs hash to
    bool m_skipsUnchangedFilm = false;
    bool m_measuresHistogram = false;
    QRhiBuffer *m_binsAfter = nullptr, *m_binsBefore = nullptr;     // the last layout's
    size_t m_filmPasses = 0;
    uint64_t m_filmKey = 0;
    std::optional<uint64_t> m_renderedFilmKey;
    std::vector<std::unique_ptr<QRhiResource>> m_pictureResources;  // the picture's
    std::vector<BlurUse> m_blurs;
    QSize m_size;
    float m_pitch = 0;
    QRhiTexture *m_blurTemp = nullptr;
    // What every print pass reads.
    QRhiTexture *m_toneLut = nullptr, *m_dodgeBurn = nullptr;
    QRhiBuffer *m_spectral = nullptr, *m_toneShift = nullptr, *m_printU = nullptr, *m_shapeU = nullptr;
    // The picture's textures.
    QRhiTexture *m_source = nullptr, *m_raw = nullptr, *m_rawB = nullptr, *m_scratchA = nullptr,
                *m_scratchB = nullptr, *m_logEMap = nullptr, *m_cmy = nullptr, *m_cmyB = nullptr,
                *m_correction = nullptr, *m_correctionBlurred = nullptr, *m_printedMap = nullptr,
                *m_displayMap = nullptr;
    QRhiTexture::Format m_intermediate = QRhiTexture::RGBA32F;
    // A shaped print's own textures, at its size: the negative shaped, the
    // same with its grain, two scratch textures, the print and its copy for
    // the screen.
    std::vector<std::unique_ptr<QRhiResource>> m_shapedResources;
    QRhiTexture *m_shapedNegative = nullptr, *m_shapedGrainy = nullptr, *m_shapedScratchA = nullptr,
                *m_shapedScratchB = nullptr, *m_shapedPrinted = nullptr, *m_shapedDisplay = nullptr,
                *m_shapedBlurTemp = nullptr;
    // dodge & burn's map, kept until it changes; one empty texel without masks
    std::unique_ptr<QRhiTexture> m_dodgeBurnTexture, m_noDodgeBurn;
    std::vector<float> m_dodgeBurnMap;
    QSize m_dodgeBurnSize;
    bool m_dodgeBurnDirty = false;
    // The taps.
    QRhiTexture *m_logE = nullptr, *m_negative = nullptr, *m_printed = nullptr, *m_display = nullptr;

    void ensureShaped(QSize size);
};
