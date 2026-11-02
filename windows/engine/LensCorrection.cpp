#include "LensCorrection.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace {

// ---- Reading the raw files ----------------------------------------------

// Bytes from a file, or from a value already read.
class Source {
public:
    virtual ~Source() = default;
    virtual bool read(uint64_t at, void *to, size_t n) = 0;
};

class FileSource : public Source {
public:
    explicit FileSource(const std::string &path) : in(std::filesystem::path(path), std::ios::binary) {}
    bool read(uint64_t at, void *to, size_t n) override
    {
        in.clear();
        in.seekg(std::streamoff(at));
        in.read(static_cast<char *>(to), std::streamsize(n));
        return bool(in);
    }

private:
    std::ifstream in;
};

class MemorySource : public Source {
public:
    explicit MemorySource(const std::vector<uint8_t> &bytes) : bytes(bytes) {}
    bool read(uint64_t at, void *to, size_t n) override
    {
        if (at > bytes.size() || n > bytes.size() - at)
            return false;
        std::memcpy(to, bytes.data() + at, n);
        return true;
    }

private:
    const std::vector<uint8_t> &bytes;
};

uint16_t get16(const uint8_t *p, bool little) { return little ? uint16_t(p[0] | p[1] << 8) : uint16_t(p[1] | p[0] << 8); }
uint32_t get32(const uint8_t *p, bool little)
{
    return little ? uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24
                  : uint32_t(p[3]) | uint32_t(p[2]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[0]) << 24;
}
uint64_t get64(const uint8_t *p, bool little)
{
    const uint64_t a = get32(p, little), b = get32(p + 4, little);
    return little ? a | b << 32 : b | a << 32;
}

// The values of a TIFF structure's directories, by tag: IFD0's chain and
// every directory below it (SubIFDs, the EXIF IFD, and entries of type IFD,
// as Fujifilm's raw section and Olympus's makernote have them). Offsets
// count from `base`; any magic number is taken (Panasonic's, Olympus's).
struct TiffValue {
    uint16_t type = 0;
    uint32_t count = 0;
    std::vector<uint8_t> bytes;         // empty where the value is too large to read
};

class Tiff {
public:
    Tiff(Source &src, uint64_t base) : src(src), base(base)
    {
        uint8_t header[8];
        if (!src.read(base, header, 8) || header[0] != header[1] || (header[0] != 'I' && header[0] != 'M'))
            return;
        little = header[0] == 'I';
        walk(get32(header + 4, little));
    }
    Tiff(Source &src, uint64_t base, bool little, uint32_t first) : little(little), src(src), base(base) { walk(first); }

    bool little = true;
    std::vector<std::map<uint16_t, TiffValue>> ifds;

    const TiffValue *find(uint16_t tag) const
    {
        for (const auto &ifd : ifds)
            if (const auto it = ifd.find(tag); it != ifd.end() && !it->second.bytes.empty())
                return &it->second;
        return nullptr;
    }
    // the i-th number of a value, rationals divided
    double number(const TiffValue &v, uint32_t i) const
    {
        static const int size[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};
        const size_t n = v.type < 14 ? size_t(size[v.type]) : 0;
        if (!n || i >= v.count || (size_t(i) + 1) * n > v.bytes.size())
            return 0;
        const uint8_t *p = v.bytes.data() + size_t(i) * n;
        switch (v.type) {
        case 1: case 7: return p[0];
        case 6: return int8_t(p[0]);
        case 3: return get16(p, little);
        case 8: return int16_t(get16(p, little));
        case 4: case 13: return get32(p, little);
        case 9: return int32_t(get32(p, little));
        case 5: { const uint32_t d = get32(p + 4, little); return d ? double(get32(p, little)) / d : 0; }
        case 10: { const int32_t d = int32_t(get32(p + 4, little)); return d ? double(int32_t(get32(p, little))) / d : 0; }
        case 11: { const uint32_t b = get32(p, little); float f; std::memcpy(&f, &b, 4); return f; }
        case 12: { const uint64_t b = get64(p, little); double f; std::memcpy(&f, &b, 8); return f; }
        default: return 0;
        }
    }

private:
    Source &src;
    uint64_t base = 0;
    std::set<uint32_t> seen;

    void walk(uint32_t offset, int depth = 0)
    {
        for (int chain = 0; offset && chain < 8 && depth < 6 && seen.insert(offset).second; ++chain) {
            uint8_t countBytes[2];
            if (!src.read(base + offset, countBytes, 2))
                return;
            const int count = get16(countBytes, little);
            if (count <= 0 || count > 2000)
                return;
            std::vector<uint8_t> entries(size_t(count) * 12 + 4);
            if (!src.read(base + offset + 2, entries.data(), entries.size()))
                return;
            static const int size[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};
            std::map<uint16_t, TiffValue> ifd;
            std::vector<uint32_t> below;
            for (int i = 0; i < count; ++i) {
                const uint8_t *e = &entries[size_t(i) * 12];
                TiffValue v;
                const uint16_t tag = get16(e, little);
                v.type = get16(e + 2, little);
                v.count = get32(e + 4, little);
                const uint64_t bytes = v.type < 14 ? uint64_t(size[v.type]) * v.count : 0;
                if (bytes && bytes <= (8u << 20)) {
                    v.bytes.resize(size_t(bytes));
                    if (bytes <= 4)
                        std::memcpy(v.bytes.data(), e + 8, size_t(bytes));
                    else if (!src.read(base + get32(e + 8, little), v.bytes.data(), size_t(bytes)))
                        v.bytes.clear();
                }
                if (tag == 0x014a || tag == 0x8769 || v.type == 13)        // SubIFDs, EXIF, IFDs
                    for (uint32_t k = 0; k < v.count && k < 16 && (size_t(k) + 1) * 4 <= v.bytes.size(); ++k)
                        below.push_back(get32(&v.bytes[size_t(k) * 4], little));
                ifd[tag] = std::move(v);
            }
            ifds.push_back(std::move(ifd));
            for (uint32_t b : below)
                walk(b, depth + 1);
            offset = get32(&entries[size_t(count) * 12], little);
        }
    }
};

// A radial warp from a function of the output radius (in units of radius),
// as a table out to `to`.
void tabulate(Distortion &d, double to, const std::function<double(double)> &scale)
{
    constexpr int n = 4096;
    d.model = Distortion::Profile;
    d.profileStep = float(to / (n - 1));
    d.profile.resize(n);
    for (int i = 0; i < n; ++i)
        d.profile[size_t(i)] = float(scale(i * double(d.profileStep)));
}

// Piecewise linear through (x, y), held flat beyond the ends.
double piecewise(const std::vector<double> &x, const std::vector<double> &y, double at)
{
    if (at <= x.front())
        return y.front();
    for (size_t i = 1; i < x.size(); ++i)
        if (at <= x[i])
            return y[i - 1] + (y[i] - y[i - 1]) * (at - x[i - 1]) / (x[i] - x[i - 1]);
    return y.back();
}

// -- Nikon: NikonNEFInfo's DistortionInfo
//
// In a NEF, the raw IFD's NikonNEFInfo (tag 0xc7d5) is a small TIFF of its
// own after "Nikon\0" and four bytes. Its entry 0x0005, 84 bytes, is the
// distortion data, for the lens at the focal length of the picture:
//   0x00  "0100"
//   0x04  0 without data (lenses the camera knows nothing of), 1 where the
//         correction is optional (third-party lenses, F-mount lenses on the
//         FTZ adapter), 3 where it is required (Nikon's Z lenses)
//   0x08  the polynomial's radius 1 in mm (88474/4096, 21.6: a full frame's
//         half-diagonal, that of the sensor's whole readout)
//   0x14, 0x1c, 0x24  the coefficients of r^4, r^3 and r^2 (exiftool's
//         RadialDistortionCoefficient1-3)
//   0x40  c0, the picture's scale-up      (all signed, over 2^20)
// The source radius at an output radius r is r (1 + k2 r^2 + k3 r^3 +
// k4 r^4) / (1 + c0): Apple's decoder reads them so, measured on Z,
// third-party and FTZ-adapted lenses to 0.1-0.3 per mille of r, where no
// other order or set of powers comes within 1.
constexpr double kNikonFullFrame = 21.6;     // mm, a full frame's half-diagonal as Nikon's data has it

bool readNikon(const Tiff &tiff, const LensInfo &lens, double halfDiagonal, float crop, Distortion &d)
{
    const TiffValue *info = tiff.find(0xc7d5);
    if (!info || info->bytes.size() < 18 || std::memcmp(info->bytes.data(), "Nikon\0", 6) != 0)
        return false;
    const std::vector<uint8_t> inner(info->bytes.begin() + 10, info->bytes.end());
    MemorySource memory(inner);
    const Tiff nef(memory, 0);
    const TiffValue *v = nullptr;
    for (const auto &ifd : nef.ifds)
        if (const auto it = ifd.find(0x0005); it != ifd.end() && it->second.bytes.size() >= 0x48)
            v = &it->second;
    if (!v)
        return false;
    const uint8_t *p = v->bytes.data();
    auto rational = [&](size_t o) {
        const int32_t num = int32_t(get32(p + o, nef.little)), den = int32_t(get32(p + o + 4, nef.little));
        return den ? double(num) / den : 0.0;
    };
    const int mode = p[4];
    const double radiusMM = rational(0x08), c0 = rational(0x40);
    const double k2 = rational(0x24), k3 = rational(0x1c), k4 = rational(0x14);
    if (mode == 0 || radiusMM <= 0 || (c0 == 0 && k2 == 0 && k3 == 0 && k4 == 0))
        return false;
    d.model = Distortion::Nikon;
    d.k[0] = float(c0);
    d.k[1] = float(k2);
    d.k[2] = float(k3);
    d.k[3] = float(k4);
    // the sensor's readout is a frame of the crop factor, to Apple's decoder
    // (0.1 per mille of the radius on a Z f and a Z 7)
    const double sensor = lens.sensorWidth ? 0.5 * std::hypot(double(lens.sensorWidth), double(lens.sensorHeight))
                                           : halfDiagonal;
    d.radius = float(sensor * radiusMM / (kNikonFullFrame / crop));
    d.from = "Nikon's data";
    return true;
}

// -- Sony: the raw IFD's DistortionCorrParams (0x7037)
//
// A count n (11 or 16), then n values: the change of the radius, over 2^14,
// at n radii evenly from the centre to the corner, linear in between. 0.1-0.5
// per mille of the radius from Apple's decoder's reading, on an RX100 VII
// and an A7 IV. The camera's own setting (0x7036) is for its JPEGs; the
// numbers are the lens's whatever it says.
bool readSony(const Tiff &tiff, double halfDiagonal, Distortion &d)
{
    const TiffValue *params = tiff.find(0x7037);
    if (!params || params->count < 3)
        return false;
    const uint32_t n = std::min(uint32_t(tiff.number(*params, 0)), params->count - 1);
    if (n < 2)
        return false;
    std::vector<double> x, y;
    bool any = false;
    for (uint32_t i = 0; i < n; ++i) {
        x.push_back(double(i) / (n - 1));
        y.push_back(tiff.number(*params, i + 1) / 16384);
        any |= y.back() != 0;
    }
    if (!any)
        return false;
    d.radius = float(halfDiagonal);
    tabulate(d, 2, [&](double u) { return 1 + piecewise(x, y, u); });
    d.from = "Sony's data";
    return true;
}

// -- Fujifilm: the raw section's GeometricDistortionParams (0xf00b)
//
// 2n + 1 numbers: one of no use here (the half-diagonal in pixels over n),
// then n radii (of the half-diagonal, the last beyond the corner) and the
// change of the radius there, in per cent: held from the centre out to the
// first radius, linear between. All nought: no correction. 0.15 per mille of
// the radius from Apple's decoder's reading, on an X-T4, but for the scale,
// which the numbers do not give.
bool readFujifilm(Source &file, double halfDiagonal, Distortion &d)
{
    uint8_t header[0x6c];
    if (!file.read(0, header, sizeof header) || std::memcmp(header, "FUJIFILMCCD-RAW", 15) != 0)
        return false;
    const Tiff raw(file, get32(header + 0x64, false));
    const TiffValue *v = raw.find(0xf00b);
    if (!v || v->count < 5 || v->count % 2 == 0)
        return false;
    const uint32_t n = (v->count - 1) / 2;
    std::vector<double> x, y;
    bool any = false;
    for (uint32_t i = 0; i < n; ++i) {
        x.push_back(raw.number(*v, 1 + i));
        y.push_back(raw.number(*v, 1 + n + i) / 100);
        any |= y.back() != 0;
    }
    if (!any)
        return false;
    d.radius = float(halfDiagonal);
    tabulate(d, 2, [&](double u) { return 1 + piecewise(x, y, u); });
    d.from = "Fujifilm's data";
    return true;
}

// -- Panasonic: IFD0's DistortionInfo (0x0119), 16 signed 16-bit numbers
//
// exiftool's names: DistortionParam02, 04, 08 and 11 (over 2^15), and the
// scale at 5 (1 / (1 + value / 2^15)). The polynomial runs from the source to
// the output: the output radius is scale s (1 + p8 s^2 + p4 s^4 + p11 s^6)
// for a source radius s, both of the half-diagonal. 0.13-0.14 per mille of
// the radius from Apple's decoder's reading, on an LX100 II, a ZS200 and a
// G9; no other set of the numbers comes within 0.3.
bool readPanasonic(const Tiff &tiff, double halfDiagonal, Distortion &d)
{
    const TiffValue *v = tiff.find(0x0119);
    if (!v || v->bytes.size() < 32)
        return false;
    auto p = [&](int i) { return double(int16_t(get16(v->bytes.data() + 2 * i, tiff.little))) / 32768; };
    const double p4 = p(4), p8 = p(8), p11 = p(11), scale = 1 / (1 + p(5));
    if (p4 == 0 && p8 == 0 && p11 == 0)
        return false;
    // the output radius of source radii on a fine grid, increasing as long as
    // the polynomial is of use, and read back for the output radii
    std::vector<double> source, output;
    for (int i = 0; i <= 8192; ++i) {
        const double s = 2.0 * i / 8192, o = scale * s * (1 + s * s * (p8 + s * s * (p4 + s * s * p11)));
        if (!output.empty() && o <= output.back())
            break;
        source.push_back(s);
        output.push_back(o);
    }
    if (output.size() < 2)
        return false;
    d.radius = float(halfDiagonal);
    tabulate(d, 2, [&](double u) {
        if (u <= 0)
            return 1 / (scale * 1.0);
        const size_t i = std::min(size_t(std::lower_bound(output.begin(), output.end(), u) - output.begin()), output.size() - 1);
        const size_t a = i ? i - 1 : 0, b = i ? i : 1;
        const double s = source[a] + (source[b] - source[a]) * (u - output[a]) / (output[b] - output[a]);
        return s / u;
    });
    d.from = "Panasonic's data";
    return true;
}

// -- Olympus, OM System: the makernote's ImageProcessing IFD, 0x150a
//
// Four numbers k1 k2 k3 s: the source radius at an output radius r is
// s r (1 + k1 q^2 + k2 q^4 + k3 q^6), q = s r over the half-diagonal of the
// sensor's 4:3 frame (wider than a picture cropped to 3:2 or 16:9).
// 0.09-0.11 per mille of the radius from Apple's decoder's reading, on an
// E-M10 IV and a TG-6 (the TG-6's picture a 3:2 crop).
bool readOlympus(const Tiff &tiff, int w, int h, Distortion &d)
{
    const TiffValue *note = tiff.find(0x927c);
    if (!note || note->bytes.size() < 16)
        return false;
    const uint8_t *b = note->bytes.data();
    size_t order;
    if (std::memcmp(b, "OLYMPUS\0", 8) == 0)
        order = 8;
    else if (note->bytes.size() > 20 && std::memcmp(b, "OM SYSTEM\0\0\0", 12) == 0)
        order = 12;
    else
        return false;
    if (b[order] != b[order + 1] || (b[order] != 'I' && b[order] != 'M'))
        return false;
    MemorySource memory(note->bytes);
    const Tiff makernote(memory, 0, b[order] == 'I', uint32_t(order + 4));
    const TiffValue *v = makernote.find(0x150a);
    if (!v || v->count < 4)
        return false;
    const double k1 = makernote.number(*v, 0), k2 = makernote.number(*v, 1), k3 = makernote.number(*v, 2),
                 s = makernote.number(*v, 3);
    if (s <= 0 || (k1 == 0 && k2 == 0 && k3 == 0 && s == 1))
        return false;
    d.radius = float(0.625 * std::max(double(std::max(w, h)), std::min(w, h) * 4.0 / 3));
    tabulate(d, 2, [&](double u) {
        const double q = s * u;
        return s * (1 + q * q * (k1 + q * q * (k2 + q * q * k3)));
    });
    d.from = "Olympus's data";
    return true;
}

// -- DNG: the raw IFD's OpcodeList3 (0xc74e), its WarpRectilinear (opcode 1)
//
// As the DNG specification has it: from the optical centre (cx, cy, of the
// active area, 0 to 1), in units of m, the distance to the active area's
// farthest corner, a pixel (x, y) at radius r comes from
//     (kr0 + kr1 r^2 + kr2 r^4 + kr3 r^6) (x, y)
//     + (2 kt0 x y + kt1 (r^2 + 2 x^2), 2 kt1 x y + kt0 (r^2 + 2 y^2))
// with one set of coefficients for all planes, or one each. Opcode lists
// are big-endian, whatever the file. (Apple's decoder warps a Leica Q's
// picture more than the specification has it, by up to a fifth.)
bool readDng(const Tiff &tiff, const LensInfo &lens, int w, int h, Distortion &d)
{
    // the raw image's IFD: NewSubfileType 0, with the opcode list
    const std::map<uint16_t, TiffValue> *raw = nullptr;
    for (const auto &ifd : tiff.ifds) {
        const auto type = ifd.find(0x00fe);
        if (ifd.count(0xc74e) && (type == ifd.end() || tiff.number(type->second, 0) == 0))
            raw = &ifd;
    }
    if (!raw)
        return false;
    const std::vector<uint8_t> &list = raw->at(0xc74e).bytes;
    auto be32 = [&](size_t at) { return get32(list.data() + at, false); };
    auto be64 = [&](size_t at) {
        const uint64_t bits = get64(list.data() + at, false);
        double v;
        std::memcpy(&v, &bits, 8);
        return v;
    };
    if (list.size() < 4)
        return false;
    const uint32_t count = be32(0);
    size_t at = 4;
    for (uint32_t i = 0; i < count && at + 16 <= list.size(); ++i) {
        const uint32_t id = be32(at), size = be32(at + 12);
        const size_t params = at + 16;
        at = params + size;
        if (id != 1 || at > list.size() || size < 4)
            continue;
        const uint32_t planes = be32(params);
        if (planes == 0 || planes > 4 || size < 4 + planes * 48 + 16)
            return false;
        d.planes.clear();
        bool identity = true;
        for (uint32_t p = 0; p < planes; ++p) {
            Distortion::Plane plane;
            for (int k = 0; k < 4; ++k)
                plane.kr[k] = be64(params + 4 + p * 48 + 8 * size_t(k));
            for (int k = 0; k < 2; ++k)
                plane.kt[k] = be64(params + 4 + p * 48 + 32 + 8 * size_t(k));
            identity &= plane.kr[0] == 1 && plane.kr[1] == 0 && plane.kr[2] == 0 && plane.kr[3] == 0 &&
                        plane.kt[0] == 0 && plane.kt[1] == 0;
            d.planes.push_back(plane);
        }
        if (identity)
            return false;
        const double ncx = be64(params + 4 + planes * 48), ncy = be64(params + 4 + planes * 48 + 8);
        // the active area, in the mosaic: top, left, bottom, right
        double top = 0, left = 0, bottom = lens.sensorHeight, right = lens.sensorWidth;
        if (const auto it = raw->find(0xc68d); it != raw->end() && it->second.count >= 4) {
            top = tiff.number(it->second, 0);
            left = tiff.number(it->second, 1);
            bottom = tiff.number(it->second, 2);
            right = tiff.number(it->second, 3);
        }
        if (right <= left || bottom <= top) {
            left = lens.cropLeft;
            top = lens.cropTop;
            right = left + w;
            bottom = top + h;
        }
        // in the image's pixels, pixel i centred on i
        d.cx = left + ncx * (right - left) - lens.cropLeft - 0.5;
        d.cy = top + ncy * (bottom - top) - lens.cropTop - 0.5;
        double m = 0;
        for (double x : {left, right})
            for (double y : {top, bottom})
                m = std::max(m, std::hypot(x - lens.cropLeft - 0.5 - d.cx, y - lens.cropTop - 0.5 - d.cy));
        d.model = Distortion::DngWarp;
        d.radius = float(m);
        d.from = "the DNG's WarpRectilinear";
        return true;
    }
    return false;
}

// ---- Lensfun -------------------------------------------------------------

struct LfCalibration {
    float focal = 0;
    Distortion::Model model = Distortion::None;
    float k[3] = {};
};

struct LfLens {
    std::vector<std::string> makers, models, mounts;
    float cropFactor = 1, aspectRatio = 1.5f;
    bool rectilinear = true;
    bool fixed = false;                      // a compact camera's own ("fixed lens")
    std::vector<LfCalibration> distortion;   // by focal length
};

struct LfCamera {
    std::vector<std::string> makers, models;
    std::string mount;
    float cropFactor = 1;
};

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
bool letter(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }

// "4.0" as "4", "2.80" as "2.8"
std::string tidyNumber(std::string n)
{
    if (n.find('.') != std::string::npos) {
        while (!n.empty() && n.back() == '0')
            n.pop_back();
        if (!n.empty() && n.back() == '.')
            n.pop_back();
    }
    return n;
}

// A lens's name as words to compare, the cameras' ways of writing it and
// Lensfun's made one: lower case; "EF24-70mm" as "ef 24-70mm", "24-70 mm" as
// "24-70mm", "16-55mmF2.8" as "16-55mm f/2.8"; an aperture "F4.0", "f4" or
// "f/4" as "f/4", "f/4-6.3" keeping both ends, and letters after it a word
// of their own (Canon's "F4L" as "f/4 l", as the cameras write it).
std::vector<std::string> words(const std::string &name)
{
    std::string s = lower(name);
    for (char &c : s)
        if (c == ',' || c == '(' || c == ')' || c == '[' || c == ']' || c == '|')
            c = ' ';
    // an aperture straight after the focal length: "mmf2.8"
    for (size_t i = 3; i + 1 < s.size(); ++i)
        if (s[i] == 'f' && s[i - 1] == 'm' && s[i - 2] == 'm' && digit(s[i - 3]) && (digit(s[i + 1]) || s[i + 1] == '/'))
            s.insert(i++, 1, ' ');
    std::vector<std::string> split;
    for (size_t i = 0; i < s.size();) {
        while (i < s.size() && s[i] == ' ')
            ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ' ')
            ++j;
        if (j > i) {
            std::string w = s.substr(i, j - i);
            // a series' letters before the focal length: "ef24-70mm", "ef-s18-55mm"
            size_t p = 0;
            while (p < w.size() && (letter(w[p]) || w[p] == '-'))
                ++p;
            if (p > 0 && p < w.size() && digit(w[p]) && w.find("mm", p) != std::string::npos && w.substr(0, p) != "f") {
                split.push_back(w.substr(0, p));
                w = w.substr(p);
            }
            split.push_back(w);
        }
        i = j;
    }
    std::vector<std::string> out;
    for (std::string w : split) {
        if (w == "mm" && !out.empty() && digit(out.back().back())) {
            out.back() += "mm";
            continue;
        }
        if (w.size() > 1 && w[0] == 'f' && (digit(w[1]) || (w[1] == '/' && w.size() > 2 && digit(w[2])))) {
            std::string rest = w.substr(w[1] == '/' ? 2 : 1), aperture = "f/";
            size_t p = 0;
            for (bool first = true;; first = false) {
                size_t q = p;
                while (q < rest.size() && (digit(rest[q]) || rest[q] == '.'))
                    ++q;
                if (q == p)
                    break;
                aperture += (first ? "" : "-") + tidyNumber(rest.substr(p, q - p));
                p = q;
                if (p + 1 < rest.size() && rest[p] == '-' && digit(rest[p + 1]))
                    ++p;
                else
                    break;
            }
            out.push_back(aperture);
            if (p < rest.size())
                out.push_back(rest.substr(p));
            continue;
        }
        out.push_back(w);
    }
    return out;
}

bool isSpec(const std::string &w)        // a focal length or an aperture
{
    return (w.size() > 2 && w[0] == 'f' && w[1] == '/' && digit(w[2])) ||
           (digit(w[0]) && w.size() > 2 && w.compare(w.size() - 2, 2, "mm") == 0);
}

float aspectOf(const std::string &s)
{
    const size_t colon = s.find(':');
    if (colon != std::string::npos) {
        const float a = std::stof(s.substr(0, colon)), b = std::stof(s.substr(colon + 1));
        return b > 0 ? std::max(a, b) / std::min(a, b) : 1.5f;
    }
    const float v = std::stof(s);
    return v > 0 ? std::max(v, 1 / v) : 1.5f;
}

class Lensfun {
public:
    static const Lensfun &load(const std::string &dir)
    {
        static std::mutex lock;
        static std::map<std::string, std::unique_ptr<Lensfun>> loaded;
        const std::scoped_lock hold(lock);
        auto &db = loaded[dir];
        if (!db) {
            db = std::make_unique<Lensfun>();
            std::error_code error;
            for (const auto &entry : std::filesystem::directory_iterator(std::filesystem::path(dir), error))
                if (entry.path().extension() == ".xml")
                    db->read(entry.path());
        }
        return *db;
    }

    const LfCamera *camera(const std::string &maker, const std::string &model) const
    {
        const std::string mk = lower(maker), md = lower(model);
        for (const LfCamera &c : cameras) {
            bool makerMatches = false, modelMatches = false;
            for (const std::string &m : c.makers)
                makerMatches |= mk.rfind(lower(m), 0) == 0 || lower(m).rfind(mk, 0) == 0;
            for (const std::string &m : c.models)
                modelMatches |= lower(m) == md || lower(m) == mk + " " + md || lower(m) == lower(c.makers.front()) + " " + md;
            if (makerMatches && modelMatches)
                return &c;
        }
        return nullptr;
    }

    // The lens the file's name for it names, among those that fit the
    // camera's mount and were calibrated on a frame as large as the camera's
    // at least (a calibration does not reach beyond its frame). The names
    // must have the same focal lengths and apertures and the same other
    // words, but for the lens maker's name, one at least: an adapted lens
    // the camera knows only as "28mm f/2" is none of Lensfun's. Of the lenses
    // that match, the one calibrated on the frame nearest the camera's, on
    // the camera's own mount before an adapter's.
    const LfLens *lens(const std::string &mount, float crop, const std::string &name, std::string &matched) const
    {
        std::set<std::string> fileSpecs, fileOthers;
        for (const std::string &w : words(name))
            (isSpec(w) ? fileSpecs : fileOthers).insert(w);
        if (fileSpecs.empty() || fileOthers.empty())
            return nullptr;
        std::set<std::string> mounts;
        if (const auto it = compat.find(mount); it != compat.end())
            mounts.insert(it->second.begin(), it->second.end());
        const LfLens *best = nullptr;
        bool bestNative = false;
        for (const LfLens &l : lenses) {
            if (!l.rectilinear || l.distortion.empty() || l.cropFactor > crop * 1.01f)
                continue;
            bool native = false, adapted = false;
            for (const std::string &m : l.mounts) {
                native |= m == mount;
                adapted |= mounts.count(m) > 0;
            }
            if (!mount.empty() && !native && !adapted)
                continue;
            std::set<std::string> maker;
            for (const std::string &m : l.makers)
                for (const std::string &w : words(m))
                    maker.insert(w);
            for (const std::string &model : l.models) {
                std::set<std::string> specs, others;
                for (const std::string &w : words(model))
                    (isSpec(w) ? specs : others).insert(w);
                if (specs != fileSpecs)
                    continue;
                bool same = true, shared = false;
                for (const std::string &w : fileOthers) {
                    const bool there = others.count(w) > 0;
                    shared |= there;
                    same &= there || maker.count(w) > 0;
                }
                for (const std::string &w : others)
                    same &= fileOthers.count(w) > 0 || maker.count(w) > 0;
                if (!same || !shared)
                    continue;
                if (!best || (native && !bestNative) ||
                    (native == bestNative && l.cropFactor > best->cropFactor)) {
                    best = &l;
                    bestNative = native;
                    matched = model;
                }
                break;
            }
        }
        return best;
    }

    // A compact camera's lens, which the files of most name not at all: the
    // fixed lens on the camera's own mount, calibrated on the frame nearest
    // the picture's; not one calibrated with a converter in front ("Ricoh GR
    // III & compatibles + GW-4"), which the file would not say.
    const LfLens *fixedLens(const std::string &mount, float crop, std::string &matched) const
    {
        const LfLens *best = nullptr;
        for (const LfLens &l : lenses) {
            const std::string name = lower(l.models.front());
            if (!l.fixed || !l.rectilinear || l.distortion.empty() || l.cropFactor > crop * 1.01f ||
                std::find(l.mounts.begin(), l.mounts.end(), mount) == l.mounts.end() ||
                name.find(" + ") != std::string::npos || name.find("with ") != std::string::npos ||
                name.find("converter") != std::string::npos)
                continue;
            if (!best || l.cropFactor > best->cropFactor) {
                best = &l;
                matched = l.models.front();
            }
        }
        return best;
    }

private:
    std::vector<LfCamera> cameras;
    std::vector<LfLens> lenses;
    std::map<std::string, std::vector<std::string>> compat;

    void read(const std::filesystem::path &path)
    {
        pugi::xml_document doc;
        if (!doc.load_file(path.c_str()))
            return;
        const pugi::xml_node root = doc.child("lensdatabase");
        for (const pugi::xml_node m : root.children("mount"))
            for (const pugi::xml_node c : m.children("compat"))
                compat[m.child_value("name")].push_back(c.child_value());
        for (const pugi::xml_node c : root.children("camera")) {
            LfCamera cam;
            for (const pugi::xml_node n : c.children("maker"))
                cam.makers.push_back(n.child_value());
            for (const pugi::xml_node n : c.children("model"))
                cam.models.push_back(n.child_value());
            cam.mount = c.child_value("mount");
            if (const char *cf = c.child_value("cropfactor"); *cf)
                cam.cropFactor = std::stof(cf);
            if (!cam.makers.empty() && !cam.models.empty())
                cameras.push_back(std::move(cam));
        }
        for (const pugi::xml_node n : root.children("lens")) {
            LfLens l;
            for (const pugi::xml_node m : n.children("maker"))
                l.makers.push_back(m.child_value());
            for (const pugi::xml_node m : n.children("model")) {
                l.models.push_back(m.child_value());
                l.fixed |= std::string(m.attribute("lang").as_string()) == "en" && l.models.back() == "fixed lens";
            }
            for (const pugi::xml_node m : n.children("mount"))
                l.mounts.push_back(m.child_value());
            if (const char *cf = n.child_value("cropfactor"); *cf)
                l.cropFactor = std::stof(cf);
            if (const char *ar = n.child_value("aspect-ratio"); *ar)
                l.aspectRatio = aspectOf(ar);
            if (const char *type = n.child_value("type"); *type)
                l.rectilinear = std::string(type) == "rectilinear";
            for (const pugi::xml_node d : n.child("calibration").children("distortion")) {
                LfCalibration cal;
                cal.focal = d.attribute("focal").as_float();
                const std::string model = d.attribute("model").as_string();
                if (model == "ptlens") {
                    cal.model = Distortion::PTLens;
                    cal.k[0] = d.attribute("a").as_float();
                    cal.k[1] = d.attribute("b").as_float();
                    cal.k[2] = d.attribute("c").as_float();
                } else if (model == "poly3") {
                    cal.model = Distortion::Poly3;
                    cal.k[0] = d.attribute("k1").as_float();
                } else if (model == "poly5") {
                    cal.model = Distortion::Poly5;
                    cal.k[0] = d.attribute("k1").as_float();
                    cal.k[1] = d.attribute("k2").as_float();
                } else {
                    continue;
                }
                l.distortion.push_back(cal);
            }
            std::sort(l.distortion.begin(), l.distortion.end(),
                      [](const LfCalibration &a, const LfCalibration &b) { return a.focal < b.focal; });
            if (!l.makers.empty() && !l.models.empty())
                lenses.push_back(std::move(l));
        }
    }
};

// Lensfun's interpolation between calibrated focal lengths: each term times
// its focal length, a cubic Hermite spline through the four nearest with
// Catmull-Rom tangents (one-sided where a neighbour is missing), over the
// focal length; the nearest calibration beyond the range.
float hermite(float y1, float y2, float y3, float y4, bool have1, bool have4, float t)
{
    const float tg2 = have1 ? (y3 - y1) * 0.5f : y3 - y2;
    const float tg3 = have4 ? (y4 - y2) * 0.5f : y3 - y2;
    const float t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * y2 + (t3 - 2 * t2 + t) * tg2 + (-2 * t3 + 3 * t2) * y3 + (t3 - t2) * tg3;
}

LfCalibration interpolate(const std::vector<LfCalibration> &cal, float focal)
{
    if (focal <= cal.front().focal)
        return cal.front();
    if (focal >= cal.back().focal)
        return cal.back();
    size_t i = 0;
    while (cal[i + 1].focal <= focal)
        ++i;
    if (cal[i].focal == focal)
        return cal[i];
    const LfCalibration *s[4] = {i > 0 ? &cal[i - 1] : nullptr, &cal[i], &cal[i + 1],
                                 i + 2 < cal.size() ? &cal[i + 2] : nullptr};
    const float t = (focal - s[1]->focal) / (s[2]->focal - s[1]->focal);
    LfCalibration out = *s[1];
    out.focal = focal;
    for (int k = 0; k < 3; ++k) {
        auto y = [&](int j) { return s[j] ? s[j]->k[k] * s[j]->focal : 0.0f; };
        out.k[k] = hermite(y(0), y(1), y(2), y(3), s[0] != nullptr, s[3] != nullptr, t) / focal;
    }
    return out;
}

} // namespace

float Distortion::scale(float r) const
{
    const float u = r / (radius * zoom);
    float s = 1;
    switch (model) {
    case Nikon:  s = (1 + u * u * (k[1] + u * (k[2] + u * k[3]))) / (1 + k[0]); break;
    case Poly3:  s = 1 - k[0] + k[0] * u * u; break;
    case Poly5:  s = 1 + k[0] * u * u + k[1] * u * u * u * u; break;
    case PTLens: s = k[0] * u * u * u + k[1] * u * u + k[2] * u + 1 - k[0] - k[1] - k[2]; break;
    case Profile: {
        const float x = u / profileStep;
        const size_t i = size_t(std::max(0.0f, x));
        if (i + 1 >= profile.size())
            s = profile.back();
        else
            s = profile[i] + (profile[i + 1] - profile[i]) * (x - float(i));
        break;
    }
    default:     break;
    }
    return s / zoom;
}

namespace {

// The least scale-up (or the most down) at which every pixel on the frame's
// edge comes from inside the picture: the source radius over the output
// radius at most 1 from the nearest edge out to the corners.
float fillZoom(Distortion d, int w, int h)
{
    const float nearest = 0.5f * float(std::min(w, h) - 1), corner = 0.5f * std::hypot(float(w - 1), float(h - 1));
    auto worst = [&](float zoom) {
        d.zoom = zoom;
        float most = 0;
        for (int i = 0; i <= 256; ++i)
            most = std::max(most, d.scale(nearest + (corner - nearest) * float(i) / 256));
        return most;
    };
    float lo = 0.5f, hi = 2;
    if (worst(lo) <= 1 || worst(hi) > 1)
        return 1;                       // no sensible scale: left as the model has it
    for (int i = 0; i < 40; ++i) {
        const float mid = 0.5f * (lo + hi);
        (worst(mid) > 1 ? lo : hi) = mid;
    }
    return hi;
}

} // namespace

Distortion distortionFor(const std::string &rawPath, const LensInfo &lens, int w, int h, const std::string &lensfunDir,
                         bool cameraData)
{
    Distortion d;
    const Lensfun &db = Lensfun::load(lensfunDir);
    const LfCamera *camera = db.camera(lens.cameraMaker, lens.cameraModel);
    // the image's crop factor: the camera's, as Lensfun has it, unless the
    // file's 35 mm equivalent focal length says another (a crop mode, or a
    // camera Lensfun does not know)
    float crop = camera ? camera->cropFactor : 0;
    if (lens.cropFactor > 0 && (crop <= 0 || std::abs(lens.cropFactor / crop - 1) > 0.1f))
        crop = lens.cropFactor;
    if (crop <= 0)
        crop = 1;
    const double halfDiagonal = 0.5 * std::hypot(double(w) + 1, double(h) + 1);

    // the camera's own data, where its form is known
    if (cameraData) {
        FileSource file(rawPath);
        const Tiff tiff(file, 0);
        bool found = false;
        if (!tiff.ifds.empty()) {
            for (auto reader : {+[](const Tiff &t, const LensInfo &l, int w, int h, double, float, Distortion &d) {
                                    return readDng(t, l, w, h, d); },
                                +[](const Tiff &t, const LensInfo &l, int, int, double hd, float c, Distortion &d) {
                                    return readNikon(t, l, hd, c, d); },
                                +[](const Tiff &t, const LensInfo &, int, int, double hd, float, Distortion &d) {
                                    return readSony(t, hd, d); },
                                +[](const Tiff &t, const LensInfo &, int, int, double hd, float, Distortion &d) {
                                    return readPanasonic(t, hd, d); },
                                +[](const Tiff &t, const LensInfo &, int w, int h, double, float, Distortion &d) {
                                    return readOlympus(t, w, h, d); }}) {
                Distortion attempt;
                if (reader(tiff, lens, w, h, halfDiagonal, crop, attempt)) {
                    d = attempt;
                    found = true;
                    break;
                }
            }
        } else {
            found = readFujifilm(file, halfDiagonal, d);
        }
        if (found) {
            // Fujifilm's numbers give no scale: filled, as Lensfun's. The
            // others keep their own, scaled up only as far as keeps every
            // pixel of the frame's edge from outside the picture. DNG's warp
            // as the specification has it.
            if (d.from == "Fujifilm's data")
                d.zoom = fillZoom(d, w, h);
            else if (d.model != Distortion::DngWarp)
                d.zoom = std::max(1.0f, fillZoom(d, w, h));
            return d;
        }
    }
    // else Lensfun's calibration of the lens, at the focal length; its radius
    // 1 is half the short side of the frame it was calibrated on. A camera
    // Lensfun does not know takes the lens from any mount; a compact its own.
    if (lens.focalLength <= 0)
        return d;
    std::string matched;
    const LfLens *l = lens.lensModel.empty() ? nullptr : db.lens(camera ? camera->mount : "", crop, lens.lensModel, matched);
    if (!l && camera)
        l = db.fixedLens(camera->mount, crop, matched);
    if (!l)
        return d;
    const LfCalibration c = interpolate(l->distortion, lens.focalLength);
    d.model = c.model;
    std::copy(c.k, c.k + 3, d.k);
    d.radius = float(halfDiagonal * (crop / l->cropFactor) / std::hypot(1.0, double(l->aspectRatio)));
    d.zoom = fillZoom(d, w, h);
    d.from = "Lensfun: " + matched;
    return d;
}

void correctDistortion(std::vector<float3> &rgb, int w, int h, const Distortion &d)
{
    if (d.model == Distortion::None || (d.model == Distortion::DngWarp && d.planes.empty()))
        return;
    std::vector<float3> out(rgb.size());
    const float cx = 0.5f * float(w - 1), cy = 0.5f * float(h - 1);
    auto weights = [](float t, float k[4]) {
        // Catmull-Rom (cubic convolution, a = -0.5)
        const float t2 = t * t, t3 = t2 * t;
        k[0] = -0.5f * t3 + t2 - 0.5f * t;
        k[1] = 1.5f * t3 - 2.5f * t2 + 1;
        k[2] = -1.5f * t3 + 2 * t2 + 0.5f * t;
        k[3] = 0.5f * t3 - 0.5f * t2;
    };
    auto sample = [&](float sx, float sy) {
        const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
        float kx[4], ky[4];
        weights(sx - float(x0), kx);
        weights(sy - float(y0), ky);
        float3 sum;
        for (int j = 0; j < 4; ++j) {
            const int yy = std::clamp(y0 - 1 + j, 0, h - 1);
            float3 row;
            for (int i = 0; i < 4; ++i)
                row += rgb[size_t(yy) * w + std::clamp(x0 - 1 + i, 0, w - 1)] * kx[i];
            sum += row * ky[j];
        }
        return sum;
    };
    // DNG's warp, per plane, from its own centre and radius
    auto dng = [&](int x, int y) {
        const double m = d.radius, dx = (x - d.cx) / m, dy = (y - d.cy) / m, r2 = dx * dx + dy * dy;
        float3 v;
        for (int c = 0; c < 3; ++c) {
            if (c > 0 && d.planes.size() == 1)
                break;
            const Distortion::Plane &p = d.planes[std::min(size_t(c), d.planes.size() - 1)];
            const double f = p.kr[0] + r2 * (p.kr[1] + r2 * (p.kr[2] + r2 * p.kr[3]));
            const double sx = f * dx + 2 * p.kt[0] * dx * dy + p.kt[1] * (r2 + 2 * dx * dx);
            const double sy = f * dy + 2 * p.kt[1] * dx * dy + p.kt[0] * (r2 + 2 * dy * dy);
            const float3 s = sample(float(d.cx + m * sx), float(d.cy + m * sy));
            if (d.planes.size() == 1)
                return s;
            v[c] = s[c];
        }
        return v;
    };
    const int threads = std::max(1, int(std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    const int chunk = (h + threads - 1) / threads;
    for (int part = 0; part < threads; ++part) {
        const int begin = part * chunk, end = std::min(h, begin + chunk);
        if (begin >= end)
            continue;
        pool.emplace_back([&, begin, end] {
            for (int y = begin; y < end; ++y)
                for (int x = 0; x < w; ++x) {
                    if (d.model == Distortion::DngWarp) {
                        out[size_t(y) * w + x] = dng(x, y);
                        continue;
                    }
                    const float dx = float(x) - cx, dy = float(y) - cy;
                    const float s = d.scale(std::sqrt(dx * dx + dy * dy));
                    out[size_t(y) * w + x] = sample(cx + dx * s, cy + dy * s);
                }
        });
    }
    for (std::thread &t : pool)
        t.join();
    rgb.swap(out);
}
