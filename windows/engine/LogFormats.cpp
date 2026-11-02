#include "LogFormats.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

// MARK: - Mat3d

Vec3d Mat3d::operator*(const Vec3d &v) const
{
    Vec3d out{};
    for (int r = 0; r < 3; ++r)
        out[size_t(r)] = m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2];
    return out;
}

Mat3d Mat3d::operator*(const Mat3d &o) const
{
    Mat3d out;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out.m[r][c] = m[r][0] * o.m[0][c] + m[r][1] * o.m[1][c] + m[r][2] * o.m[2][c];
    return out;
}

Mat3d Mat3d::inverse() const
{
    const auto &a = m;
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                     + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    Mat3d out;
    out.m[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
    out.m[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
    out.m[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
    out.m[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
    out.m[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
    out.m[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
    out.m[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
    out.m[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
    out.m[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
    return out;
}

// MARK: - Primaries

namespace primaries {

Mat3d rgbToXYZ(std::array<double, 2> red, std::array<double, 2> green, std::array<double, 2> blue,
               std::array<double, 2> white)
{
    auto xyz = [](std::array<double, 2> c) { return Vec3d{c[0] / c[1], 1, (1 - c[0] - c[1]) / c[1]}; };
    const Vec3d r = xyz(red), g = xyz(green), b = xyz(blue);
    Mat3d p;
    for (int i = 0; i < 3; ++i) {
        p.m[i][0] = r[size_t(i)];
        p.m[i][1] = g[size_t(i)];
        p.m[i][2] = b[size_t(i)];
    }
    const Vec3d s = p.inverse() * xyz(white);
    Mat3d out;
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 3; ++c)
            out.m[i][c] = p.m[i][c] * s[size_t(c)];
    return out;
}

Mat3d toBT2020(std::array<double, 2> red, std::array<double, 2> green, std::array<double, 2> blue)
{
    static const Mat3d bt2020ToXYZ = rgbToXYZ({0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046});
    return bt2020ToXYZ.inverse() * rgbToXYZ(red, green, blue);
}

} // namespace primaries

// MARK: - The curves

namespace logcurve {

// Nikon N-Log (N-Log Specification Document 1.0.0)
namespace {
const double kNa = 650.0 / 1023, kNb = 0.0075, kNc = 150.0 / 1023, kNd = 619.0 / 1023;
const double kNCutLinear = 0.328, kNCutCode = 452.0 / 1023;
}
double nlogLinear(double v) { return v < kNCutCode ? std::pow(v / kNa, 3) - kNb : std::exp((v - kNd) / kNc); }
double nlogCode(double y) { return y < kNCutLinear ? kNa * std::cbrt(y + kNb) : kNc * std::log(y) + kNd; }

// Panasonic V-Log (V-Log/V-Gamut Reference Manual)
double vlogLinear(double v) { return v < 0.181 ? (v - 0.125) / 5.6 : std::pow(10, (v - 0.598206) / 0.241514) - 0.00873; }
double vlogCode(double x) { return x < 0.01 ? 5.6 * x + 0.125 : 0.241514 * std::log10(x + 0.00873) + 0.598206; }

// Apple Log (Apple Log Profile White Paper)
namespace {
const double kAr0 = -0.05641088, kArt = 0.01, kAsigma = 47.28711236, kAbeta = 0.00964052, kAgamma = 0.08550479,
             kAdelta = 0.69336945;
const double kApt = kAsigma * (kArt - kAr0) * (kArt - kAr0);
}
double appleLogLinear(double p)
{
    if (p >= kApt)
        return std::pow(2, (p - kAdelta) / kAgamma) - kAbeta;
    if (p >= 0)
        return std::sqrt(p / kAsigma) + kAr0;
    return kAr0;
}
double appleLogSignal(double r)
{
    if (r >= kArt)
        return kAgamma * std::log2(r + kAbeta) + kAdelta;
    if (r >= kAr0)
        return kAsigma * (r - kAr0) * (r - kAr0);
    return 0;
}

// Fujifilm F-Log and F-Log2 (their data sheets)
namespace {
struct FCurve {
    double cut2, a, b, c, d, e, f;
    double linear(double v) const { return v < cut2 ? (v - f) / e : std::pow(10, (v - d) / c) / a - b / a; }
};
const FCurve kFLog{0.100537775223865, 0.555556, 0.009468, 0.344676, 0.790453, 8.735631, 0.092864};
const FCurve kFLog2{0.100686685370811, 5.555556, 0.064829, 0.245281, 0.384316, 8.799461, 0.092864};
}
double flogLinear(double v) { return kFLog.linear(v); }
double flog2Linear(double v) { return kFLog2.linear(v); }

// Canon Log 2 and 3, v1.2 (reflectance / 0.9 in the curves)
double clog2Linear(double v)
{
    const double x = v < 0.092864125 ? -(std::pow(10, (0.092864125 - v) / 0.24136077) - 1) / 87.09937546
                                     : (std::pow(10, (v - 0.092864125) / 0.24136077) - 1) / 87.09937546;
    return x * 0.9;
}
double clog3Linear(double v)
{
    double x;
    if (v < 0.097465473)
        x = -(std::pow(10, (0.12783901 - v) / 0.36726845) - 1) / 14.98325;
    else if (v <= 0.15277891)
        x = (v - 0.12512219) / 1.9754798;
    else
        x = (std::pow(10, (v - 0.12240537) / 0.36726845) - 1) / 14.98325;
    return x * 0.9;
}

// Sony S-Log3 (Technical Summary for S-Gamut3.Cine/S-Log3)
double slog3Linear(double v)
{
    return v >= 171.2102946929 / 1023 ? std::pow(10, (v * 1023 - 420) / 261.5) * (0.18 + 0.01) - 0.01
                                      : (v * 1023 - 95) * 0.01125 / (171.2102946929 - 95);
}

// Insta360 I-Log (10-bit I-Log White Paper)
double ilogLinear(double v)
{
    return v < 0.154402 ? (v - 0.09055934) / 5.77837328 : std::pow(10, (v - 0.623992) / 0.280055) - 0.01;
}

} // namespace logcurve

// MARK: - The gamuts

namespace {

// V-Gamut to XYZ (Panasonic's reference matrix), then XYZ to BT.2020
Mat3d vGamutToBT2020()
{
    Mat3d toXYZ;
    const double v[3][3] = {{0.679644, 0.152211, 0.118600}, {0.260686, 0.774894, -0.035580},
                            {-0.009310, -0.004612, 1.102980}};
    Mat3d xyzTo2020;
    const double x[3][3] = {{1.7166511880, -0.3556707838, -0.2533662814},
                            {-0.6666843518, 1.6164812366, 0.0157685458},
                            {0.0176398574, -0.0427706133, 0.9421031212}};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            toXYZ.m[r][c] = v[r][c];
            xyzTo2020.m[r][c] = x[r][c];
        }
    return xyzTo2020 * toXYZ;
}

const Mat3d &gamutOf(int which)
{
    static const Mat3d appleWide = primaries::toBT2020({0.725, 0.301}, {0.221, 0.814}, {0.068, -0.076});
    static const Mat3d vGamut = vGamutToBT2020();
    static const Mat3d cinema = primaries::toBT2020({0.74, 0.27}, {0.17, 1.14}, {0.08, -0.10});
    static const Mat3d sGamut3Cine = primaries::toBT2020({0.766, 0.275}, {0.225, 0.800}, {0.089, -0.087});
    static const Mat3d sGamut3 = primaries::toBT2020({0.730, 0.280}, {0.140, 0.855}, {0.100, -0.050});
    switch (which) {
    case 0: return appleWide;
    case 1: return vGamut;
    case 2: return cinema;
    case 3: return sGamut3Cine;
    default: return sGamut3;
    }
}

} // namespace

// MARK: - LutInput

std::string lutInputName(LutInput i)
{
    switch (i) {
    case LutInput::NLogCode: return "N-Log code values";
    case LutInput::NLogVideoLevels: return "N-Log video levels";
    case LutInput::AppleLog: return "Apple Log";
    case LutInput::AppleLog2: return "Apple Log 2";
    case LutInput::VLog: return "Panasonic V-Log";
    case LutInput::VLogVideoLevels: return "Panasonic V-Log (video levels)";
    case LutInput::FLog: return "Fujifilm F-Log";
    case LutInput::FLogVideoLevels: return "Fujifilm F-Log (video levels)";
    case LutInput::FLog2: return "Fujifilm F-Log2";
    case LutInput::FLog2VideoLevels: return "Fujifilm F-Log2 (video levels)";
    case LutInput::CLog2: return "Canon Log 2";
    case LutInput::CLog2VideoLevels: return "Canon Log 2 (video levels)";
    case LutInput::CLog3: return "Canon Log 3";
    case LutInput::CLog3VideoLevels: return "Canon Log 3 (video levels)";
    case LutInput::SLog3Cine: return "Sony S-Log3 (S-Gamut3.Cine)";
    case LutInput::SLog3CineVideoLevels: return "Sony S-Log3 (S-Gamut3.Cine, video levels)";
    case LutInput::SLog3: return "Sony S-Log3 (S-Gamut3)";
    case LutInput::SLog3VideoLevels: return "Sony S-Log3 (S-Gamut3, video levels)";
    case LutInput::ILog: return "Insta360 I-Log";
    case LutInput::ILogVideoLevels: return "Insta360 I-Log (video levels)";
    }
    return {};
}

bool onVideoLevels(LutInput i)
{
    switch (i) {
    case LutInput::NLogVideoLevels:
    case LutInput::VLogVideoLevels:
    case LutInput::FLogVideoLevels:
    case LutInput::FLog2VideoLevels:
    case LutInput::CLog2VideoLevels:
    case LutInput::CLog3VideoLevels:
    case LutInput::SLog3CineVideoLevels:
    case LutInput::SLog3VideoLevels:
    case LutInput::ILogVideoLevels: return true;
    default: return false;
    }
}

double lutInputLinear(LutInput i, double v)
{
    const double code = onVideoLevels(i) ? (64 + 876 * v) / 1023 : v;
    switch (i) {
    case LutInput::NLogCode:
    case LutInput::NLogVideoLevels: return logcurve::nlogLinear(code);
    case LutInput::AppleLog:
    case LutInput::AppleLog2: return logcurve::appleLogLinear(v);
    case LutInput::VLog:
    case LutInput::VLogVideoLevels: return logcurve::vlogLinear(code);
    case LutInput::FLog:
    case LutInput::FLogVideoLevels: return logcurve::flogLinear(code);
    case LutInput::FLog2:
    case LutInput::FLog2VideoLevels: return logcurve::flog2Linear(code);
    case LutInput::CLog2:
    case LutInput::CLog2VideoLevels: return logcurve::clog2Linear(code);
    case LutInput::CLog3:
    case LutInput::CLog3VideoLevels: return logcurve::clog3Linear(code);
    case LutInput::SLog3Cine:
    case LutInput::SLog3CineVideoLevels:
    case LutInput::SLog3:
    case LutInput::SLog3VideoLevels: return logcurve::slog3Linear(code);
    case LutInput::ILog:
    case LutInput::ILogVideoLevels: return logcurve::ilogLinear(code);
    }
    return v;
}

std::optional<Mat3d> lutInputGamut(LutInput i)
{
    switch (i) {
    case LutInput::AppleLog2: return gamutOf(0);
    case LutInput::VLog:
    case LutInput::VLogVideoLevels: return gamutOf(1);
    case LutInput::CLog2:
    case LutInput::CLog2VideoLevels:
    case LutInput::CLog3:
    case LutInput::CLog3VideoLevels: return gamutOf(2);
    case LutInput::SLog3Cine:
    case LutInput::SLog3CineVideoLevels: return gamutOf(3);
    case LutInput::SLog3:
    case LutInput::SLog3VideoLevels: return gamutOf(4);
    default: return std::nullopt;
    }
}

Vec3d lutInputScene(LutInput i, const Vec3d &p)
{
    const Vec3d light{lutInputLinear(i, p[0]), lutInputLinear(i, p[1]), lutInputLinear(i, p[2])};
    const auto gamut = lutInputGamut(i);
    return gamut ? *gamut * light : light;
}

std::string lutInputComment(LutInput i)
{
    const std::string levels = onVideoLevels(i) ? "video levels ((code - 64) / 876)" : "code value / 1023 (full range)";
    switch (i) {
    case LutInput::NLogCode: return "Nikon N-Log, BT.2020 primaries, code value / 1023 (data levels)";
    case LutInput::NLogVideoLevels: return "Nikon N-Log, BT.2020 primaries, video levels ((code - 64) / 876)";
    case LutInput::AppleLog: return "Apple Log (iPhone 15 Pro and later), BT.2020 primaries, signal 0-1";
    case LutInput::AppleLog2: return "Apple Log 2 (iPhone 17 Pro and later), Apple Wide Gamut primaries, signal 0-1";
    case LutInput::VLog:
    case LutInput::VLogVideoLevels: return "Panasonic V-Log, V-Gamut primaries, " + levels;
    case LutInput::FLog:
    case LutInput::FLogVideoLevels: return "Fujifilm F-Log, F-Gamut (BT.2020) primaries, " + levels;
    case LutInput::FLog2:
    case LutInput::FLog2VideoLevels: return "Fujifilm F-Log2, F-Gamut (BT.2020) primaries, " + levels;
    case LutInput::CLog2:
    case LutInput::CLog2VideoLevels: return "Canon Log 2, Cinema Gamut primaries, " + levels;
    case LutInput::CLog3:
    case LutInput::CLog3VideoLevels: return "Canon Log 3, Cinema Gamut primaries, " + levels;
    case LutInput::SLog3Cine:
    case LutInput::SLog3CineVideoLevels: return "Sony S-Log3, S-Gamut3.Cine primaries, " + levels;
    case LutInput::SLog3:
    case LutInput::SLog3VideoLevels: return "Sony S-Log3, S-Gamut3 primaries, " + levels;
    case LutInput::ILog:
    case LutInput::ILogVideoLevels: return "Insta360 I-Log, BT.2020 primaries, " + levels;
    }
    return {};
}

// MARK: - LutOutput

std::string lutOutputName(LutOutput o)
{
    switch (o) {
    case LutOutput::BT1886: return "Rec.709 (BT.1886)";
    case LutOutput::AppleDisplay: return "Rec.709 (Apple displays)";
    case LutOutput::Linear: return "Linear";
    case LutOutput::SRGB: return "sRGB";
    }
    return {};
}

double lutOutputGamma(LutOutput o)
{
    switch (o) {
    case LutOutput::BT1886: return 2.4;
    case LutOutput::AppleDisplay: return 1.961;
    case LutOutput::Linear: return 1;
    case LutOutput::SRGB: return 2.2;
    }
    return 2.4;
}

double lutOutputEncode(LutOutput o, double light)
{
    const double l = std::max(light, 0.0);
    if (o == LutOutput::SRGB)
        return l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1 / 2.4) - 0.055;
    return std::pow(l, 1 / lutOutputGamma(o));
}

double lutOutputDecode(LutOutput o, double value)
{
    const double v = std::max(value, 0.0);
    if (o == LutOutput::SRGB)
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    return std::pow(v, lutOutputGamma(o));
}

std::string lutOutputComment(LutOutput o)
{
    switch (o) {
    case LutOutput::BT1886: return "Rec.709 primaries, BT.1886 (2.4 power) for a Rec.709 reference display";
    case LutOutput::AppleDisplay: return "Rec.709 primaries, 1.961 power (how macOS and iOS decode Rec.709 video)";
    case LutOutput::Linear: return "Rec.709 primaries, display-referred linear light";
    case LutOutput::SRGB: return "Rec.709 / sRGB primaries, sRGB curve (photos exactly, video between 1.961 and 2.4)";
    }
    return {};
}

// MARK: - LogFormat

const std::vector<LogFormat> &logFormats()
{
    static const std::vector<LogFormat> all{LogFormat::NLog,  LogFormat::VLog,      LogFormat::AppleLog,
                                            LogFormat::AppleLog2, LogFormat::FLog,  LogFormat::FLog2,
                                            LogFormat::CLog2, LogFormat::CLog3,     LogFormat::SLog3Cine,
                                            LogFormat::SLog3, LogFormat::ILog};
    return all;
}

std::string logFormatName(LogFormat f)
{
    switch (f) {
    case LogFormat::NLog: return "Nikon N-Log";
    case LogFormat::VLog: return "Panasonic V-Log";
    case LogFormat::AppleLog: return "Apple Log";
    case LogFormat::AppleLog2: return "Apple Log 2";
    case LogFormat::FLog: return "Fujifilm F-Log";
    case LogFormat::FLog2: return "Fujifilm F-Log2";
    case LogFormat::CLog2: return "Canon Log 2";
    case LogFormat::CLog3: return "Canon Log 3";
    case LogFormat::SLog3Cine: return "Sony S-Log3 · S-Gamut3.Cine";
    case LogFormat::SLog3: return "Sony S-Log3 · S-Gamut3";
    case LogFormat::ILog: return "Insta360 I-Log";
    }
    return {};
}

std::string logFormatShortName(LogFormat f)
{
    switch (f) {
    case LogFormat::NLog: return "N-Log";
    case LogFormat::VLog: return "V-Log";
    case LogFormat::AppleLog: return "Apple Log";
    case LogFormat::AppleLog2: return "Apple Log 2";
    case LogFormat::FLog: return "F-Log";
    case LogFormat::FLog2: return "F-Log2";
    case LogFormat::CLog2: return "Canon Log 2";
    case LogFormat::CLog3: return "Canon Log 3";
    case LogFormat::SLog3Cine: return "S-Log3 · S-Gamut3.Cine";
    case LogFormat::SLog3: return "S-Log3 · S-Gamut3";
    case LogFormat::ILog: return "I-Log";
    }
    return {};
}

std::string logFormatCurveName(LogFormat f)
{
    return f == LogFormat::SLog3Cine || f == LogFormat::SLog3 ? "S-Log3" : logFormatShortName(f);
}

std::string logFormatGrey(LogFormat f)
{
    switch (f) {
    case LogFormat::NLog: return "code 372";
    case LogFormat::VLog: return "code 433";
    case LogFormat::AppleLog:
    case LogFormat::AppleLog2: return "signal 0.488";
    case LogFormat::FLog: return "code 470";
    case LogFormat::FLog2: return "code 400";
    case LogFormat::CLog2: return "code 407";
    case LogFormat::CLog3: return "code 351";
    case LogFormat::SLog3Cine:
    case LogFormat::SLog3: return "code 420";
    case LogFormat::ILog: return "code 432";
    }
    return {};
}

bool onCodeValues(LogFormat f)
{
    return f != LogFormat::AppleLog && f != LogFormat::AppleLog2;
}

LutInput lutInputOf(LogFormat f)
{
    switch (f) {
    case LogFormat::NLog: return LutInput::NLogCode;
    case LogFormat::VLog: return LutInput::VLog;
    case LogFormat::AppleLog: return LutInput::AppleLog;
    case LogFormat::AppleLog2: return LutInput::AppleLog2;
    case LogFormat::FLog: return LutInput::FLog;
    case LogFormat::FLog2: return LutInput::FLog2;
    case LogFormat::CLog2: return LutInput::CLog2;
    case LogFormat::CLog3: return LutInput::CLog3;
    case LogFormat::SLog3Cine: return LutInput::SLog3Cine;
    case LogFormat::SLog3: return LutInput::SLog3;
    case LogFormat::ILog: return LutInput::ILog;
    }
    return LutInput::NLogCode;
}

LutInput exportInputOf(LogFormat f, bool fullRange)
{
    if (fullRange)
        return lutInputOf(f);
    switch (f) {
    case LogFormat::NLog: return LutInput::NLogVideoLevels;
    case LogFormat::VLog: return LutInput::VLogVideoLevels;
    case LogFormat::AppleLog:
    case LogFormat::AppleLog2: return lutInputOf(f);
    case LogFormat::FLog: return LutInput::FLogVideoLevels;
    case LogFormat::FLog2: return LutInput::FLog2VideoLevels;
    case LogFormat::CLog2: return LutInput::CLog2VideoLevels;
    case LogFormat::CLog3: return LutInput::CLog3VideoLevels;
    case LogFormat::SLog3Cine: return LutInput::SLog3CineVideoLevels;
    case LogFormat::SLog3: return LutInput::SLog3VideoLevels;
    case LogFormat::ILog: return LutInput::ILogVideoLevels;
    }
    return lutInputOf(f);
}

Vec3d logScene(LogFormat f, const Vec3d &codes, bool fullRange)
{
    Vec3d v{};
    for (size_t c = 0; c < 3; ++c)
        v[c] = onCodeValues(f) || fullRange ? codes[c] / 1023 : (codes[c] - 64) / 876;
    return lutInputScene(lutInputOf(f), v);
}

std::string detectionBasisName(DetectionBasis b)
{
    switch (b) {
    case DetectionBasis::Tagged: return "from its tags";
    case DetectionBasis::Make: return "guessed from the camera";
    case DetectionBasis::Assumed: return "assumed";
    }
    return {};
}

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return s;
}

} // namespace

// Apple tags its log footage, and a Sony camera writes the curve and gamut
// into its own XML. Otherwise the maker decides: a Lumix records V-Log; a
// Fujifilm F-Log2 if it has it, else F-Log; a Canon most often Canon Log 3; a
// Sony S-Log3 in S-Gamut3.Cine; an Insta360 I-Log; an iPhone Apple Log.
// Anything else is taken for N-Log, which a Nikon Z writes untagged.
std::pair<LogFormat, DetectionBasis> detectLogFormat(const CameraTags &tags)
{
    if (tags.logTransfer == "com.apple.apple-wide-gamut.apple-log")
        return {LogFormat::AppleLog2, DetectionBasis::Tagged};
    if (tags.logTransfer == "com.apple.rec2020.apple-log")
        return {LogFormat::AppleLog, DetectionBasis::Tagged};
    const std::string gamma = lower(tags.gamma);
    if (gamma.find("s-log3") != std::string::npos) {
        const bool cine = gamma.find("cine") != std::string::npos
                       || lower(tags.primaries).find("cine") != std::string::npos;
        return {cine ? LogFormat::SLog3Cine : LogFormat::SLog3, DetectionBasis::Tagged};
    }
    static const std::set<std::string> fLog2Cameras{"X-H2S", "X-H2",      "X-T5",       "X-S20",
                                                    "X100VI", "X-T50",    "X-M5",       "X-E5",
                                                    "GFX100 II", "GFX100S II", "GFX100RF", "X-T30 III"};
    const std::string make = lower(tags.make);
    if (tags.panasonic || make.find("panasonic") != std::string::npos || make.find("lumix") != std::string::npos)
        return {LogFormat::VLog, DetectionBasis::Make};
    if (make.find("fujifilm") != std::string::npos)
        return {fLog2Cameras.count(upper(tags.model)) ? LogFormat::FLog2 : LogFormat::FLog, DetectionBasis::Make};
    if (make.find("canon") != std::string::npos)
        return {LogFormat::CLog3, DetectionBasis::Make};
    if (make.find("sony") != std::string::npos)
        return {LogFormat::SLog3Cine, DetectionBasis::Make};
    if (make.find("insta360") != std::string::npos)
        return {LogFormat::ILog, DetectionBasis::Make};
    // An iPhone records log only in Apple Log; a file that lost Apple's tag
    // (re-wrapped by another program) still names its maker
    if (make == "apple")
        return {LogFormat::AppleLog, DetectionBasis::Make};
    return {LogFormat::NLog, DetectionBasis::Assumed};
}
