#pragma once

#include "Clip.h"
#include "GpuGraph.h"
#include "LookLUT.h"
#include "Packs.h"
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

enum class ExportCodec { ProRes422HQ, HEVC10 };
std::string exportCodecName(ExportCodec c);       // "ProRes 422 HQ", "HEVC 10-bit"

struct ExportSettings {
    ExportCodec codec = ExportCodec::ProRes422HQ;
    // The displays the file is encoded for, all tagged Rec.709 and differing
    // in the curve the values are written with: Windows displays (sRGB's
    // curve, as Windows shows video on the screen), Apple displays (a 1.961
    // power, as macOS and iOS decode), or a BT.1886 monitor (2.4).
    LutOutput display = LutOutput::SRGB;
    bool operator==(const ExportSettings &) const = default;
};

// Everything that decides how a clip looks and is written out.
struct MotionLook {
    Recipe recipe;
    GlowLook effects;
    InputSettings input;
    ExportSettings exportSettings;
    int version = 2;

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

    // The frame graded into `size`: RGBA, 8 bits, sRGB, for the screen.
    // Throws std::runtime_error.
    std::vector<uint8_t> renderScreen(const VideoFrame &frame, const FrameReading &reading, QSize size, Mode mode,
                                      const EffectsParams &effects, double *seconds = nullptr);
    // Display light at `size`, RGBA float, row 0 at the top: for checks.
    std::vector<float> renderLinear(const VideoFrame &frame, const FrameReading &reading, QSize size, Mode mode,
                                    const EffectsParams &effects);
    // The frame at its own size for an export: Y'CbCr, BT.709, video levels,
    // 10 bits, `chroma` subsampled, the light written with `display`'s curve.
    void renderExport(const VideoFrame &frame, const FrameReading &reading, const EffectsParams &effects,
                      LutOutput display, Chroma chroma, std::vector<uint16_t> planes[3]);

private:
    QRhiTexture *grade(const VideoFrame &frame, const FrameReading &reading, QSize size, Mode mode);

    std::unique_ptr<QRhiTexture> m_lut;
    std::vector<float> m_lutData;
    int m_lutSize = 0;
    bool m_lutDirty = false;
};
