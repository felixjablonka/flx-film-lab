#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

// The log encodings the Motion mode reads (NLog.swift, VLog.swift,
// AppleLog.swift, FLog.swift, CanonLog.swift, SLog3.swift, ILog.swift,
// Primaries.swift, MotionLook.swift's LogFormat, LookLUT.swift's Input and
// Output): each curve from code value to scene-linear reflectance and back,
// and each camera gamut into the engine's BT.2020.

using Vec3d = std::array<double, 3>;

// A 3x3 matrix of doubles, rows.
struct Mat3d {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Vec3d operator*(const Vec3d &v) const;
    Mat3d operator*(const Mat3d &o) const;
    Mat3d inverse() const;
};

namespace primaries {
// RGB to CIE XYZ for primaries and white given as xy, scaled so RGB (1, 1, 1)
// is the white at Y = 1; and linear RGB with those primaries (D65) to BT.2020.
Mat3d rgbToXYZ(std::array<double, 2> red, std::array<double, 2> green, std::array<double, 2> blue,
               std::array<double, 2> white = {0.3127, 0.3290});
Mat3d toBT2020(std::array<double, 2> red, std::array<double, 2> green, std::array<double, 2> blue);
} // namespace primaries

// The curves, on normalised code values (code / 1023) or, for Apple Log, the
// 0-1 signal.
namespace logcurve {
double nlogLinear(double code);
double nlogCode(double linear);
double vlogLinear(double code);
double vlogCode(double linear);
double appleLogLinear(double signal);
double appleLogSignal(double linear);
double flogLinear(double code);
double flog2Linear(double code);
double clog2Linear(double code);
double clog3Linear(double code);
double slog3Linear(double code);
double ilogLinear(double code);
} // namespace logcurve

// What a lattice coordinate (0-1 per channel) means on the way into a LUT
// (LookLUT.Input): every curve but Apple Log on code / 1023, and each with a
// video-levels twin, (code - 64) / 876, as editors hand a video-range clip
// to a LUT.
enum class LutInput {
    NLogCode, NLogVideoLevels, AppleLog, AppleLog2, VLog, VLogVideoLevels, FLog, FLogVideoLevels, FLog2,
    FLog2VideoLevels, CLog2, CLog2VideoLevels, CLog3, CLog3VideoLevels, SLog3Cine, SLog3CineVideoLevels, SLog3,
    SLog3VideoLevels, ILog, ILogVideoLevels
};
std::string lutInputName(LutInput input);          // the Mac's raw value
bool onVideoLevels(LutInput input);
// scene-linear reflectance for one channel, in the footage's own primaries
double lutInputLinear(LutInput input, double v);
// the footage's primaries to BT.2020, where they differ
std::optional<Mat3d> lutInputGamut(LutInput input);
// scene-linear BT.2020 light for a lattice coordinate
Vec3d lutInputScene(LutInput input, const Vec3d &p);
std::string lutInputComment(LutInput input);

// How a LUT's output values are encoded (LookLUT.Output); Rec.709 primaries.
enum class LutOutput { BT1886, AppleDisplay, Linear, SRGB };
std::string lutOutputName(LutOutput output);
double lutOutputGamma(LutOutput output);
double lutOutputEncode(LutOutput output, double light);
double lutOutputDecode(LutOutput output, double value);
std::string lutOutputComment(LutOutput output);

// What a clip's metadata says about the camera (CameraTags).
struct CameraTags {
    std::string logTransfer;        // Apple's log tag
    bool panasonic = false;         // a 'PANA' block, or Panasonic or Lumix named
    std::string make, model;
    std::string gamma, primaries;   // a Sony camera's CaptureGammaEquation and CaptureColorPrimaries
    bool operator==(const CameraTags &) const = default;
};

// The log formats a clip can be in (LogFormat).
enum class LogFormat { NLog, VLog, AppleLog, AppleLog2, FLog, FLog2, CLog2, CLog3, SLog3Cine, SLog3, ILog };
const std::vector<LogFormat> &logFormats();
std::string logFormatName(LogFormat f);            // "Nikon N-Log", ...
std::string logFormatShortName(LogFormat f);       // "N-Log", ...
std::string logFormatCurveName(LogFormat f);
std::string logFormatGrey(LogFormat f);            // where an 18% grey records: "code 372"
bool onCodeValues(LogFormat f);
LutInput lutInputOf(LogFormat f);
// a table for other software, taking the clip as DaVinci Resolve hands it to
// a LUT: data levels for a full-range file, video levels otherwise
LutInput exportInputOf(LogFormat f, bool fullRange);
// scene-linear BT.2020 light for a pixel's code values (0-1023)
Vec3d logScene(LogFormat f, const Vec3d &codes, bool fullRange);

enum class DetectionBasis { Tagged, Make, Assumed };
std::string detectionBasisName(DetectionBasis b);  // "from its tags", ...
// What a clip says it is (LogFormat.detect).
std::pair<LogFormat, DetectionBasis> detectLogFormat(const CameraTags &tags);
