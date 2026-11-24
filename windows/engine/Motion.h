#pragma once

#include "Clip.h"
#include "GpuGraph.h"
#include "LookLUT.h"
#include "Packs.h"
#include "PrintGrain.h"
#include "Recipe.h"

#include <QJsonObject>
#include <QSize>

#include <memory>
#include <optional>
#include <string>
#include <vector>

// The Motion mode's look and frame pipeline (MotionLook.swift,
// FramePipeline.swift, ClipMeter.swift).

// How a clip's Y'CbCr is read: each setting follows the file unless it is
// overridden, for footage whose tags are missing or wrong (InputSettings).
struct InputSettings {
    enum class Levels { Auto, Video, Full } levels = Levels::Auto;
    enum class Matrix { Auto, BT2020, BT709, BT601 } matrix = Matrix::Auto;
    std::optional<LogFormat> format;    // none reads the clip's own
    bool operator==(const InputSettings &) const = default;
};
std::string levelsName(InputSettings::Levels l);   // "From the file", "Video range", "Full range"
std::string matrixName(InputSettings::Matrix m);   // "From the file", "BT.2020", ...

// The codec Export writes. Its names are the values looks keep, as the
// Mac's: ProRes's is still its old one, "ProRes 422 HQ", whichever flavour
// is chosen (ExportSettings::proRes).
enum class ExportCodec { ProRes422HQ, HEVC10 };
std::string exportCodecName(ExportCodec c);       // "ProRes 422 HQ", "HEVC 10-bit"
// The family, as the toolbar names it: its compression is beside it.
std::string exportCodecTitle(ExportCodec c);      // "ProRes 422", "HEVC 10-bit"

// ProRes 422's flavours, from the most compressed to the least. ProRes takes
// no data rate: each flavour has Apple's target for a frame size and rate,
// and a clip comes out near it (ProResFlavour).
enum class ProResFlavour { Proxy, LT, Standard, HQ };
std::vector<ProResFlavour> proResFlavours();
std::string proResFlavourName(ProResFlavour f);   // "Proxy", "LT", "422", "HQ": what looks keep
std::string proResFlavourTitle(ProResFlavour f);  // "ProRes 422 LT", "ProRes 422", ...
// Apple's target data rate, Mbit/s (Apple ProRes white paper): 45, 102, 147
// and 220 at 1920 x 1080 and 29.97 frames a second, growing with the pixels
// and the frames: 182, 409, 589 and 884 at 4K.
double proResMegabits(ProResFlavour f, int width, int height, double frameRate);

// HEVC's data rate: four levels from HEVC's own limits for the clip's size and
// frame rate (HEVCLevels.h), or a rate of its own
// (ExportSettings::hevcCustomMegabits) (HEVCLevel). Standard is the most the
// HEVC level the clip needs allows, High tier: 100 Mbit/s for 4K up to 30
// fps, 160 up to 60 and 240 up to 120; for HD 30, 50 and 100. High and
// Maximum are the next two levels' limits, Smaller half of Standard: 50,
// 100, 160 and 240 Mbit/s for 4K at 30 fps. Cameras' own HEVC sits on the
// same steps (a Sony FX5's 4K is 30, 50 or 100 Mbit/s at 24 fps and 45, 75
// or 150 at 60; a Lumix S1II's 150 at 30, 200 at 60 and 300 at 120), and
// nothing in HEVC favours a rate between them. At 4K 30 fps the four keep
// about 50%, 60%, 70% and 75% of the film's grain, measured on the Mac.
enum class HEVCLevel { Smaller, Standard, High, Maximum, Custom };
std::vector<HEVCLevel> hevcLevels();
std::string hevcLevelName(HEVCLevel l);           // "Smaller", ..., "Custom"
// the level's rate for a clip `width` x `height` at `frameRate`, Mbit/s;
// none under Custom
std::optional<double> hevcLevelMegabits(HEVCLevel l, int width, int height, double frameRate);

struct ExportSettings {
    ExportCodec codec = ExportCodec::ProRes422HQ;
    // The displays the file is encoded for, all tagged Rec.709 and differing
    // in the curve the values are written with: Windows displays (sRGB's
    // curve, as Windows shows video on the screen), Apple displays (a 1.961
    // power, as macOS and iOS decode), or a BT.1886 monitor (2.4).
    LutOutput display = LutOutput::SRGB;
    // ProRes's flavour; none, HQ, all there was before. Optional, as are the
    // two below, so that a look saved before reads as it did.
    std::optional<ProResFlavour> proRes;
    std::optional<HEVCLevel> hevcLevel;         // none, Standard
    std::optional<double> hevcCustomMegabits;   // HEVC's rate under Custom; none, 100
    // The rates HEVC can be set to, Mbit/s: up to the most HEVC's highest
    // level, 6.2, allows, 800 at High tier.
    static constexpr double minMegabits = 1, maxMegabits = 800;

    ProResFlavour flavour() const { return proRes.value_or(ProResFlavour::HQ); }
    HEVCLevel level() const { return hevcLevel.value_or(HEVCLevel::Standard); }
    double customMegabits() const;
    // The rate a clip `width` x `height` at `frameRate` is written at,
    // Mbit/s: HEVC's as set, ProRes's Apple's target for its flavour.
    double megabits(int width, int height, double frameRate) const;
    // HEVC's rate at `level` for a clip of that size and frame rate.
    double megabits(HEVCLevel level, int width, int height, double frameRate) const;
    // The compression alone, as the toolbar shows it: "HQ", "High", or a
    // rate of its own, "120 Mbit/s".
    std::string compressionTitle() const;
    // The codec and its compression: "ProRes 422 LT", "HEVC 10-bit, High".
    std::string title() const;
    // A data rate as the app writes it: "207 Mbit/s", "8.5 Mbit/s".
    static std::string rate(double megabits);
    bool operator==(const ExportSettings &) const = default;
};

// The film gauge a clip is taken to be shot on (FilmGauge): the frame its
// grain is measured against, as the film format is for a photograph. The
// same emulsion shows coarser grain on 16 mm than on 35 mm, and finer on
// 70 mm, because the smaller frame is enlarged more to fill the same screen.
enum class FilmGauge { Sixteen, ThirtyFive, Seventy };
std::vector<FilmGauge> filmGauges();
std::string filmGaugeName(FilmGauge g);           // "16 mm", "35 mm", "70 mm"
// The camera aperture, long side by short, mm: Super 16, 12.52 x 7.41;
// Super 35, the 4-perf full aperture, 24.89 x 18.66; and 65 mm 5-perf,
// 52.48 x 23.01, the negative a 70 mm print is made from. A frame is laid on
// it as large as it fits (pixelSizeUm), so a 16:9 clip spans Super 16's and
// Super 35's width, and 65 mm's height.
std::pair<double, double> filmGaugeFrameMM(FilmGauge g);

// Grain on a clip (MotionGrain): laid on each finished frame, after the
// look's LUT and Glow's effects, every frame with grain of its own, as every
// frame of film is a new piece of emulsion. Amount and Size are the Stills
// mode's.
struct MotionGrain {
    bool enabled = false;
    FilmGauge gauge = FilmGauge::ThirtyFive;
    double amount = 1;
    double size = 1;
    bool isVisible() const { return enabled && amount > 0; }
    bool operator==(const MotionGrain &) const = default;
};

// One frame's grain (FrameGrain): the look's table, the clip's settings, and
// what makes each frame's grain its own, the clip's seed and the frame's
// number.
struct FrameGrain {
    const PrintGrainTable *table = nullptr;
    MotionGrain settings;
    uint32_t clipSeed = 0;
    int frame = 0;
    // The grains of frame `frame` of this clip, on this stock: the same in
    // the preview, paused or playing, and in every export.
    uint32_t seed() const;
    // A clip's seed: its file's name, hashed, as a photograph's is.
    static uint32_t clipSeedFor(const std::string &path);
    // The frame shown `seconds` into a clip of `frameRate`.
    static int frameNumber(double seconds, double frameRate);
};

// Everything that decides how a clip looks and is written out.
struct MotionLook {
    Recipe recipe;
    GlowLook effects;
    // Grain on the finished frames; none, none at all, as in a look saved
    // before clips had any.
    std::optional<MotionGrain> grain;
    InputSettings input;
    ExportSettings exportSettings;
    int version = 2;

    bool operator==(const MotionLook &) const = default;
    static MotionLook initial();
    // a clip opened for the first time: this look with the dials for one clip
    // (exposure, tone, colour) back at their defaults, its format read from it
    MotionLook carryingLook(double defaultGrade) const;
};
// A black-and-white film, or any film printed on black-and-white paper.
bool printsInBlackAndWhite(const Recipe &recipe, PackLibrary &packs);
// Glow's effects as this film and paper print them. Glow's halation is a
// colour film's: the light scattered back off the base re-exposes the
// red-sensitive bottom layer, so the halo is red. In a black-and-white
// picture it is grey, as bright as the red one, so Amount keeps its meaning.
GlowLook effectsAsPrinted(const MotionLook &look, PackLibrary &packs);
QJsonObject motionLookToJson(const MotionLook &look);
MotionLook motionLookFromJson(const QJsonObject &o);

// How one frame's samples are read: its own layout and tags, with the
// settings' overrides (FramePipeline.Reading).
struct FrameReading {
    int bitDepth = 10;
    Chroma chroma = Chroma::C420;
    bool fullRange = false;
    double kr = 0.2627, kb = 0.0593;
    std::string matrixName;
    double sitingX = 0, sitingY = 0.5;  // where chroma sample 0 sits, in luma pixels
    LogFormat format = LogFormat::NLog;
    bool operator==(const FrameReading &) const = default;
};
FrameReading readingOf(const VideoFrame &frame, const InputSettings &settings, LogFormat format);
// The R'G'B' code values (0-1023) at a pixel, nearest chroma: for the probe
// under the pointer and the clip meter.
std::optional<Vec3d> codesAt(const VideoFrame &frame, const FrameReading &reading, int x, int y);

// The clip meter: in each of a spread of frames, the level only the brightest
// 2% of the picture exceed; the middle of those across the clip, as scene
// luminance. And the exposure that keeps it inside a film's headroom, never
// above 0 nor below -2 (ClipMeter).
std::optional<double> clipHighlight(const std::string &path, const InputSettings &settings, LogFormat format,
                                    int frames = 9);
double clipExposure(double highlight, double headroomStops);

// One frame, from decoded Y'CbCr to the finished picture: graded through the
// look, Glow's effects on it, out for the screen or for an export.
class FramePipeline : public GpuGraph
{
public:
    explicit FramePipeline(QRhi *rhi);

    enum class Mode { Graded = 0, Log = 1, Codes = 2 };

    void setLUT(const LookLUT &lut);
    bool hasLUT() const { return m_lutSize > 0; }

    // The frame graded into `size`: RGBA, 8 bits, sRGB, for the screen. With
    // `grain`, its grain last, graded frames only. Throws std::runtime_error.
    std::vector<uint8_t> renderScreen(const VideoFrame &frame, const FrameReading &reading, QSize size, Mode mode,
                                      const EffectsParams &effects, double *seconds = nullptr,
                                      const FrameGrain *grain = nullptr);
    // Display light at `size`, RGBA float, row 0 at the top: for checks.
    std::vector<float> renderLinear(const VideoFrame &frame, const FrameReading &reading, QSize size, Mode mode,
                                    const EffectsParams &effects, const FrameGrain *grain = nullptr);
    // The frame at its own size for an export: Y'CbCr, BT.709, video levels,
    // 10 bits, `chroma` subsampled, the light written with `display`'s curve.
    void renderExport(const VideoFrame &frame, const FrameReading &reading, const EffectsParams &effects,
                      LutOutput display, Chroma chroma, std::vector<uint16_t> planes[3],
                      const FrameGrain *grain = nullptr);

private:
    QRhiTexture *grade(const VideoFrame &frame, const FrameReading &reading, QSize size, Mode mode);
    // The frame's grain on `lit`, laid on after the look and the effects, as
    // the last thing the film adds (FramePipeline.grained): the frame read as
    // the print's density, each record's grain drawn as Stills draws the
    // negative's (Gaussian, its variance the negative's Selwyn coefficient
    // over the pixel's area on the gauge's frame, then its dye clouds), and
    // motion_grain_light mixing the records as the print does and returning
    // to light. A smaller frame has larger pixels on the film, so the preview
    // shows at its size the grain the export shows at full size.
    QRhiTexture *grained(QRhiTexture *lit, const FrameGrain &grain);

    std::unique_ptr<QRhiTexture> m_lut;
    std::vector<float> m_lutData;
    int m_lutSize = 0;
    bool m_lutDirty = false;
};
