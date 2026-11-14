#include "RawDecoder.h"

#include "AppleColour.h"
#include "Lights.h"
#include "RawDetail.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

// Rows split over the cores.
void parallelRows(int rows, const std::function<void(int, int)> &body)
{
    const int threads = std::max(1, int(std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    const int chunk = (rows + threads - 1) / threads;
    for (int t = 0; t < threads; ++t) {
        const int begin = t * chunk, end = std::min(rows, begin + chunk);
        if (begin < end)
            pool.emplace_back(body, begin, end);
    }
    for (std::thread &t : pool)
        t.join();
}

// A light set by temperature and tint, Core Image's scale: its chromaticity
// as Robertson's isotemperature lines put it, which are Core Image's at the
// daylights and a little off them below 4000 K. Only a decode without
// Apple's profile takes the chromaticity; the profiles take the temperature.
Light lightAt(float temperature, float tint) { return {xyOfTemperature(temperature, tint), temperature, tint}; }

// The illuminants RawLoader balances for: by chromaticity where it gives one,
// otherwise by temperature (RawLoader.referenceChromaticity, referenceTemperature);
// a name it does not know as D55's 5503 K, its default.
Light referenceLight(const std::string &illuminant)
{
    if (const std::optional<Light> light = chromaticityLight(illuminant))
        return *light;
    static const std::map<std::string, float> kelvin = {
        {"T", 2856}, {"TH-KG3", 3400}, {"TH-KG3-L", 3400}, {"K75P", 5400}};
    const auto it = kelvin.find(illuminant);
    return lightAt(it == kelvin.end() ? 5503 : it->second, 0);
}

// The light a white balance other than As shot sets: the rest of RawLoader's
// white balances are Daylight D55 and D65 and Tungsten 3200 K, set by their
// chromaticities, and Tungsten 2850 K at tint 0. The camera's own RGB is
// balanced for D55.
Light lightOf(const DecodeOptions &options)
{
    if (options.cameraRGB || options.whiteBalance == "daylight-d55")
        return referenceLight("D55");
    if (options.whiteBalance == "daylight")
        return referenceLight("D65");
    if (options.whiteBalance == "tungsten-3200")
        return referenceLight("BB3200");
    if (options.whiteBalance == "tungsten")
        return lightAt(2850, 0);
    if (options.whiteBalance == "custom")
        return lightAt(options.temperature, options.tint);
    return referenceLight(options.reference);
}

// The name of the app's own white balance a profile has measured, or "".
std::string fixedWhiteBalance(const DecodeOptions &options)
{
    if (options.whiteBalance == "daylight-d55")
        return "D55";
    if (options.whiteBalance == "daylight")
        return "D65";
    if (options.whiteBalance == "tungsten-3200")
        return "BB3200";
    if (options.whiteBalance == "tungsten")
        return "Tungsten";
    if (options.whiteBalance != "film")
        return "";
    if (options.reference == "TH-KG3-L")
        return "TH-KG3";
    for (const char *name : {"D50", "D55", "D65", "T", "BB3200", "TH-KG3", "K75P"})
        if (options.reference == name)
            return name;
    return "";
}

float3 xyzOf(float2 xy) { return {xy.x / xy.y, 1, (1 - xy.x - xy.y) / xy.y}; }

// Bradford adaptation from one white to another.
Mat3 bradford(float3 from, float3 to)
{
    const Mat3 b = Mat3::rows({0.8951f, 0.2664f, -0.1614f}, {-0.7502f, 1.7135f, 0.0367f}, {0.0389f, -0.0685f, 1.0296f});
    return b.inverse() * Mat3::diagonal((b * to) / (b * from)) * b;
}

// XYZ (D65) -> linear Rec.2020.
const Mat3 kXYZToRec2020 = Mat3::rows({1.7166512f, -0.3556708f, -0.2533663f},
                                      {-0.6666844f, 1.6164812f, 0.0157685f},
                                      {0.0176399f, -0.0427706f, 0.9421031f});

// Malvar, He and Cutler's demosaic: bilinear interpolation corrected by the
// Laplacian of the channel that is there, 5x5 filters.
void demosaicMHC(const std::vector<float> &mosaic, int w, int h, const int (&cfa)[2][2], std::vector<float3> &out)
{
    out.assign(size_t(w) * h, {});
    auto at = [&](int x, int y) {
        // mirrored at the edges, onto the same colour
        if (x < 0) x = -x; else if (x >= w) x = 2 * (w - 1) - x;
        if (y < 0) y = -y; else if (y >= h) y = 2 * (h - 1) - y;
        return mosaic[size_t(y) * w + x];
    };
    parallelRows(h, [&](int begin, int end) {
        for (int y = begin; y < end; ++y) {
            for (int x = 0; x < w; ++x) {
                const int c = cfa[y & 1][x & 1];
                const float v = at(x, y);
                const float n = at(x, y - 1), s = at(x, y + 1), e = at(x + 1, y), wv = at(x - 1, y);
                const float nn = at(x, y - 2), ss = at(x, y + 2), ee = at(x + 2, y), ww = at(x - 2, y);
                const float ne = at(x + 1, y - 1), nw = at(x - 1, y - 1), se = at(x + 1, y + 1), sw = at(x - 1, y + 1);
                float3 rgb;
                if (c == 1) {
                    // green: red and blue from the row and the column
                    const float row = (5 * v + 4 * (e + wv) - (ne + nw + se + sw) - (ee + ww) + 0.5f * (nn + ss)) / 8;
                    const float column = (5 * v + 4 * (n + s) - (ne + nw + se + sw) - (nn + ss) + 0.5f * (ee + ww)) / 8;
                    const bool redInRow = cfa[y & 1][(x + 1) & 1] == 0;
                    rgb = {redInRow ? row : column, v, redInRow ? column : row};
                } else {
                    const float green = (4 * v + 2 * (n + s + e + wv) - (nn + ss + ee + ww)) / 8;
                    const float other = (6 * v + 2 * (ne + nw + se + sw) - 1.5f * (nn + ss + ee + ww)) / 8;
                    rgb = c == 0 ? float3{v, green, other} : float3{other, green, v};
                }
                out[size_t(y) * w + x] = rgb;
            }
        }
    });
}

// Any colour filter pattern, X-Trans's 6x6 above all: green from the greens
// around, then red and blue as differences from green, from the nearest
// pixels of their colour. Softer than MHC on Bayer and than Markesteijn's on
// X-Trans, but true in colour.
void demosaicPattern(const std::vector<float> &mosaic, int w, int h, const int (&cfa)[6][6], int n,
                     std::vector<float3> &out)
{
    out.assign(size_t(w) * h, {});
    auto colour = [&](int x, int y) { return cfa[y % n][x % n]; };
    std::vector<float> green(size_t(w) * h);
    parallelRows(h, [&](int begin, int end) {
        for (int y = begin; y < end; ++y)
            for (int x = 0; x < w; ++x) {
                if (colour(x, y) == 1) {
                    green[size_t(y) * w + x] = mosaic[size_t(y) * w + x];
                    continue;
                }
                float sum = 0, weight = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int sx = x + dx, sy = y + dy;
                        if ((dx || dy) && sx >= 0 && sx < w && sy >= 0 && sy < h && colour(sx, sy) == 1) {
                            const float k = dx && dy ? 1.0f : 2.0f;
                            sum += k * mosaic[size_t(sy) * w + sx];
                            weight += k;
                        }
                    }
                green[size_t(y) * w + x] = weight > 0 ? sum / weight : 0;
            }
    });
    parallelRows(h, [&](int begin, int end) {
        for (int y = begin; y < end; ++y)
            for (int x = 0; x < w; ++x) {
                float3 &p = out[size_t(y) * w + x];
                const float g = green[size_t(y) * w + x];
                p.y = g;
                for (int c : {0, 2}) {
                    if (colour(x, y) == c) {
                        p[c] = mosaic[size_t(y) * w + x];
                        continue;
                    }
                    float sum = 0, weight = 0;
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx) {
                            const int sx = x + dx, sy = y + dy;
                            if (sx >= 0 && sx < w && sy >= 0 && sy < h && colour(sx, sy) == c) {
                                const float k = 1.0f / float(dx * dx + dy * dy);
                                const size_t i = size_t(sy) * w + sx;
                                sum += k * (mosaic[i] - green[i]);
                                weight += k;
                            }
                        }
                    p[c] = g + (weight > 0 ? sum / weight : 0);
                }
            }
    });
}

} // namespace

int highlightBlock(int w, int h) { return std::max(8, std::max(w, h) / 256); }

std::vector<float> highlightColours(const std::vector<float> &sums, int bw, int bh)
{
    std::vector<float3> colour(size_t(bw) * bh);
    std::vector<char> known(colour.size());
    for (size_t k = 0; k < colour.size(); ++k)
        if (sums[k * 4 + 3] > 16) {
            colour[k] = float3{sums[k * 4], sums[k * 4 + 1], sums[k * 4 + 2]} / sums[k * 4 + 3];
            known[k] = 1;
        }
    for (bool missing = true; missing;) {
        missing = false;
        std::vector<float3> next = colour;
        std::vector<char> nextKnown = known;
        bool filled = false;
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                const size_t k = size_t(by) * bw + bx;
                if (known[k])
                    continue;
                float3 total;
                int n = 0;
                const int nx[4] = {bx - 1, bx + 1, bx, bx}, ny[4] = {by, by, by - 1, by + 1};
                for (int i = 0; i < 4; ++i)
                    if (nx[i] >= 0 && nx[i] < bw && ny[i] >= 0 && ny[i] < bh && known[size_t(ny[i]) * bw + nx[i]]) {
                        total += colour[size_t(ny[i]) * bw + nx[i]];
                        ++n;
                    }
                if (n) {
                    next[k] = total / float(n);
                    nextKnown[k] = 1;
                    filled = true;
                } else {
                    missing = true;
                }
            }
        colour.swap(next);
        known.swap(nextKnown);
        if (!filled)
            break;                      // no unclipped pixel anywhere: neutral it is
    }
    std::vector<float> out(colour.size() * 4);
    for (size_t k = 0; k < colour.size(); ++k) {
        out[k * 4] = colour[k].x;
        out[k * 4 + 1] = colour[k].y;
        out[k * 4 + 2] = colour[k].z;
        out[k * 4 + 3] = known[k] ? 1.0f : 0.0f;
    }
    return out;
}

namespace {

// Clipped highlights, rebuilt as Apple's decoder does where it is clear what
// it does: a pixel whose three channels reached the sensor's white is the
// neutral at its brightest channel's level (Apple's, to 0.02 ΔE on a Zf); one
// with two there takes the third channel's level for them where that is
// higher; one with one there, that channel from the colour of the unclipped
// pixels around (Apple's rebuild of those is closer still, and begins below
// the white). All in the camera's RGB balanced for the light, where its
// neutral is equal channels: `balance` is what the mosaic was balanced with,
// `light` the light's neutral.
void reconstructHighlights(std::vector<float3> &rgb, int w, int h, float3 balance, float3 light)
{
    constexpr float kClipped = 0.98f, kUnclipped = 0.75f;   // of the sensor's white
    // the unclipped colour in blocks, empty blocks filled from their neighbours
    const int block = highlightBlock(w, h);
    const int bw = (w + block - 1) / block, bh = (h + block - 1) / block;
    std::vector<float> sums(size_t(bw) * bh * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float3 s = rgb[size_t(y) * w + x] / balance;
            if (std::max(s.x, std::max(s.y, s.z)) < kUnclipped && std::min(s.x, std::min(s.y, s.z)) > 0.01f) {
                float *v = &sums[(size_t(y / block) * bw + x / block) * 4];
                const float3 c = s / light;
                v[0] += c.x;
                v[1] += c.y;
                v[2] += c.z;
                v[3] += 1;
            }
        }
    const std::vector<float> colours = highlightColours(sums, bw, bh);
    std::vector<float3> colour(size_t(bw) * bh);
    std::vector<char> known(colour.size());
    for (size_t k = 0; k < colour.size(); ++k) {
        colour[k] = {colours[k * 4], colours[k * 4 + 1], colours[k * 4 + 2]};
        known[k] = colours[k * 4 + 3] > 0;
    }
    parallelRows(h, [&](int begin, int end) {
        for (int y = begin; y < end; ++y)
            for (int x = 0; x < w; ++x) {
                float3 &p = rgb[size_t(y) * w + x];
                const float3 s = p / balance;
                const bool clipped[3] = {s.x >= kClipped, s.y >= kClipped, s.z >= kClipped};
                const int n = int(clipped[0]) + int(clipped[1]) + int(clipped[2]);
                if (!n)
                    continue;
                float3 b = s / light;
                if (n == 3) {
                    b = float3::all(std::max(b.x, std::max(b.y, b.z)));
                } else if (n == 2) {
                    const int u = !clipped[0] ? 0 : !clipped[1] ? 1 : 2;
                    for (int c = 0; c < 3; ++c)
                        if (clipped[c])
                            b[c] = std::max(b[c], b[u]);
                } else {
                    const int c = clipped[0] ? 0 : clipped[1] ? 1 : 2;
                    const size_t k = size_t(y / block) * bw + x / block;
                    const float3 m = known[k] ? colour[k] : float3{1, 1, 1};
                    float estimate = 0;
                    for (int o = 0; o < 3; ++o)
                        if (o != c)
                            estimate += 0.5f * b[o] * m[c] / std::max(m[o], 1e-6f);
                    b[c] = std::max(b[c], estimate);
                }
                p = b * light * balance;
            }
    });
}

// Lanczos-3 resampling to `ow` x `oh`, one axis at a time, edges clamped.
// `offset` moves the output's pixels along the source, in output pixels.
std::vector<float> lanczosWeights(int source, int target, double scale, std::vector<int> &first, int &taps,
                                  double offset = 0)
{
    const double support = 3 / std::min(scale, 1.0);
    taps = int(std::ceil(support)) * 2 + 1;
    std::vector<float> weights(size_t(target) * taps);
    first.assign(size_t(target), 0);
    auto lanczos = [](double t) {
        if (t == 0) return 1.0;
        if (std::abs(t) >= 3) return 0.0;
        const double p = std::numbers::pi * t;
        return 3 * std::sin(p) * std::sin(p / 3) / (p * p);
    };
    const double k = std::min(scale, 1.0);
    for (int o = 0; o < target; ++o) {
        const double centre = (o + 0.5 + offset) / scale - 0.5;
        const int start = int(std::floor(centre)) - taps / 2;
        first[size_t(o)] = start;
        double total = 0;
        for (int i = 0; i < taps; ++i)
            total += lanczos((start + i - centre) * k);
        for (int i = 0; i < taps; ++i)
            weights[size_t(o) * taps + i] = float(lanczos((start + i - centre) * k) / total);
    }
    (void)source;
    return weights;
}

std::vector<float3> scaleDown(const std::vector<float3> &in, int w, int h, int ow, int oh, double scale)
{
    std::vector<int> firstX, firstY;
    int tapsX = 0, tapsY = 0;
    const std::vector<float> wx = lanczosWeights(w, ow, scale, firstX, tapsX);
    // Core Image counts rows from the bottom: where the scaled height has a
    // fraction, RawLoader's crop to whole pixels drops it at the top
    const std::vector<float> wy = lanczosWeights(h, oh, scale, firstY, tapsY, h * scale - oh);
    std::vector<float3> rows(size_t(ow) * h), out(size_t(ow) * oh);
    parallelRows(h, [&](int begin, int end) {
        for (int y = begin; y < end; ++y)
            for (int o = 0; o < ow; ++o) {
                float3 sum;
                for (int i = 0; i < tapsX; ++i) {
                    const int x = std::min(std::max(firstX[size_t(o)] + i, 0), w - 1);
                    sum += wx[size_t(o) * tapsX + i] * in[size_t(y) * w + x];
                }
                rows[size_t(y) * ow + o] = sum;
            }
    });
    parallelRows(oh, [&](int begin, int end) {
        for (int o = begin; o < end; ++o)
            for (int x = 0; x < ow; ++x) {
                float3 sum;
                for (int i = 0; i < tapsY; ++i) {
                    const int y = std::min(std::max(firstY[size_t(o)] + i, 0), h - 1);
                    sum += wy[size_t(o) * tapsY + i] * rows[size_t(y) * ow + x];
                }
                out[size_t(o) * ow + x] = sum;
            }
    });
    return out;
}

} // namespace

void lanczosTaps(int target, double scale, double offset, std::vector<int> &first, std::vector<float> &weights,
                 int &taps)
{
    weights = lanczosWeights(0, target, scale, first, taps, offset);
}

DecodePlan planDecode(const std::string &path, const DecodeOptions &options)
{
    DecodePlan plan;
    plan.options = options;
    DecodedPicture &result = plan.result;
    RawMosaic m;
    if (options.decoder == "libraw") {
        m = unpackWithLibRaw(path);
    } else if (options.decoder == "rawspeed") {
        m = unpackWithRawSpeed(path, options.camerasXml);
    } else {
        try {
            m = unpackWithRawSpeed(path, options.camerasXml);
        } catch (const std::exception &rawspeed) {
            try {
                m = unpackWithLibRaw(path);
            } catch (const UnreadableFormat &) {
                throw;      // said plainly, without RawSpeed's view of it
            } catch (const std::exception &libraw) {
                throw std::runtime_error(std::string(rawspeed.what()) + "; " + libraw.what());
            }
        }
    }

    // Apple's profile for the camera, where one was measured. Its crop is
    // that of the mode it was measured in: only for a mosaic of that size
    const AppleProfile *profile = nullptr;
    if (!options.cameraRGB && !options.appleColour.empty())
        profile = AppleColour::table(options.appleColour).find(m.make, m.model);
    result.profiled = profile != nullptr;
    {
        std::string extension = path.size() > 4 ? path.substr(path.size() - 4) : std::string();
        for (char &ch : extension)
            ch = char(std::tolower(static_cast<unsigned char>(ch)));
        result.dng = extension == ".dng";
        // The exposure Apple gives this file: a DNG's own; else its camera's,
        // or the one Apple gives the file's ISO where that differs (the D700
        // at ISO 100, the Z f at 90): the profile's own is in its transforms,
        // and the gain below adds the difference. Without a profile, the
        // options' where they name one.
        const bool dngOwn = result.dng && !std::isnan(m.exposure.dngBaselineExposure);
        const float apple = dngOwn ? m.exposure.dngBaselineExposure
                            : profile ? profile->exposureAt(m.exposure.iso)
                                      : 0.0f;
        result.baselineExposure = profile || std::isnan(options.exposureEV) ? apple : options.exposureEV;
        result.decodedExposure = options.cameraRGB ? 0 : result.baselineExposure;
    }

    // the image area: as given, else Apple's crop for the camera's mode,
    // else the decoder's
    const int *crop = profile ? profile->cropFor(m.width, m.height) : nullptr;
    const int left = options.cropWidth ? options.cropLeft : crop ? crop[0] : m.cropLeft;
    const int top = options.cropHeight ? options.cropTop : crop ? crop[1] : m.cropTop;
    const int w = options.cropWidth ? options.cropWidth : crop ? crop[2] : m.cropWidth;
    const int h = options.cropHeight ? options.cropHeight : crop ? crop[3] : m.cropHeight;
    if (left < 0 || top < 0 || left + w > m.width || top + h > m.height)
        throw std::runtime_error(path + ": the crop lies outside the mosaic");
    result.cropLeft = left;
    result.cropTop = top;
    result.cropWidth = w;
    result.cropHeight = h;
    // the pattern from the image area's corner, with all three colours
    const int n = m.cfaSize;
    bool colours[3] = {};
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            colours[std::clamp(m.cfa[y][x], 0, 2)] = true;
    if (!colours[0] || !colours[1] || !colours[2])
        throw std::runtime_error(path + ": the colour filter pattern lacks a colour (" + m.decoder + " misreads it)");
    int cfa[6][6] = {};
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            cfa[y][x] = m.cfa[(top + y) % n][(left + x) % n];

    // The white balance: the camera's neutral under the light, the RGB a
    // neutral surface gives, which the balance takes to equal channels.
    // With Apple's profile: its transform for the light, camera RGB ->
    // Rec.2020, and the neutral it has. Without: the light's chromaticity,
    // and the neutral through the camera's daylight matrix. The camera's own
    // RGB is balanced for daylight through that matrix.
    float3 neutral, lightNeutral;
    Mat3 apple;
    float2 white{0.33242f, 0.34743f};
    if (profile) {
        const std::string name = fixedWhiteBalance(options);
        if (const auto it = profile->fixed.find(name); it != profile->fixed.end()) {
            apple = it->second.transform;
            neutral = neutralOf(apple);
            result.temperature = it->second.temperature;
            result.tint = it->second.tint;
        } else if (options.whiteBalance == "as-shot") {
            neutral = m.asShotNeutral;
            apple = profile->transformForNeutral(neutral, &result.temperature, &result.tint);
        } else {
            // the grid at the light as Core Image takes it: Custom, or a
            // light the profile has no transform measured for, as a 3200 K
            // lamp in a table measured before the app had it
            const Light light = lightOf(options);
            result.temperature = light.temperature;
            result.tint = light.tint;
            apple = profile->transform(result.temperature, result.tint);
            neutral = neutralOf(apple);
        }
        lightNeutral = neutral;
        // A light the camera can hardly balance for has a neutral with a
        // channel near or below zero, and a balance from it would starve the
        // demosaic of that channel (Malvar-He-Cutler mixes them): it then
        // demosaics with daylight's. Any balance gives the same colour, as
        // the transform undoes it.
        const float top = std::max(neutral.x, std::max(neutral.y, neutral.z));
        if (std::min(neutral.x, std::min(neutral.y, neutral.z)) < 0.05f * top) {
            const auto daylight = profile->fixed.find("D55");
            neutral = daylight != profile->fixed.end() ? neutralOf(daylight->second.transform) : float3{1, 1, 1};
        }
        // the profile's camera RGB is the raw over its decoder's range: where
        // another decoder reads the mode its colour was measured in, with
        // another white (RawSpeed's measured one, LibRaw's bit depth), the
        // raw is put over that range. Another mode has its own range already.
        if (m.decoder != profile->decoder && m.width == profile->mosaicWidth && m.height == profile->mosaicHeight) {
            const float levels = (m.white - m.black[0][0]) / (profile->white - profile->black);
            for (float3 &column : apple.c)
                column *= levels;
        }
    } else {
        if (options.whiteBalance != "as-shot" || options.cameraRGB)
            white = lightOf(options).xy;
        if (options.whiteBalance == "as-shot" && !options.cameraRGB) {
            neutral = m.asShotNeutral;
            const float3 xyz = m.xyzToCamera.inverse() * neutral;
            white = {xyz.x / (xyz.x + xyz.y + xyz.z), xyz.y / (xyz.x + xyz.y + xyz.z)};
        } else {
            neutral = m.xyzToCamera * xyzOf(white);
        }
    }
    if (!profile)
        lightNeutral = neutral;
    {
        // the light's neutral as a colour, largest channel 1, for the highlights
        const float top = std::max(lightNeutral.x, std::max(lightNeutral.y, lightNeutral.z));
        lightNeutral = max(lightNeutral / top, float3::all(1e-3f));
    }
    result.whiteBalance = {neutral.y / neutral.x, 1, neutral.y / neutral.z};

    // black and white, the balance, into float
    std::vector<float> &mosaic = plan.mosaic;
    mosaic.resize(size_t(w) * h);
    const float range = 1 / (m.white - m.black[0][0]);
    parallelRows(h, [&](int begin, int end) {
        for (int y = begin; y < end; ++y)
            for (int x = 0; x < w; ++x) {
                const int mx = left + x, my = top + y;
                const float v = (float(m.data[size_t(my) * m.width + mx]) - m.black[my & 1][mx & 1]) * range;
                mosaic[size_t(y) * w + x] = v * result.whiteBalance[cfa[y % n][x % n]];
            }
    });
    result.mosaic = m;
    result.mosaic.data.clear();
    m.data.clear();
    m.data.shrink_to_fit();
    plan.width = w;
    plan.height = h;
    plan.cfaSize = n;
    std::memcpy(plan.cfa, cfa, sizeof cfa);
    plan.lightNeutral = lightNeutral;

    // white-balanced camera RGB -> Rec.2020: Apple's transform, the balance
    // undone; without a profile, XYZ under the light -> D65 -> Rec.2020
    const Mat3 unbalance = Mat3::diagonal({neutral.x / neutral.y, 1, neutral.z / neutral.y});
    if (profile) {
        result.cameraToRec2020 = apple * unbalance;
    } else {
        const Mat3 cameraToXYZ = m.xyzToCamera.inverse() * unbalance;
        result.cameraToRec2020 = kXYZToRec2020 * bradford(xyzOf(white), xyzOf({0.31271f, 0.32902f})) * cameraToXYZ;
    }
    plan.toRec2020 = options.cameraRGB ? Mat3::identity() : result.cameraToRec2020;
    // a profile has Apple's exposure for the camera in it; the file's may differ
    plan.gain = options.cameraRGB ? 1.0f
                : profile         ? std::exp2(result.baselineExposure - profile->baselineExposure)
                                  : std::exp2(result.baselineExposure);

    // the lens's distortion, in the sensor's frame, about the image area's centre
    if (options.lensCorrection && !options.cameraRGB) {
        const LensInfo lens{m.make, m.model, m.lensMaker, m.lensModel, m.focalLength, m.cropFactor, m.width, m.height,
                            left, top};
        result.distortion = distortionFor(path, lens, w, h, options.lensfunDir, options.cameraLensData);
    }

    // turned upright, then scaled down as RawLoader does: the long side to
    // maxDimension, the rest rounded down
    plan.orientation = options.upright ? m.orientation : 1;
    const bool quarter = plan.orientation == 6 || plan.orientation == 8;
    plan.turnedWidth = quarter ? h : w;
    plan.turnedHeight = quarter ? w : h;
    plan.outWidth = plan.turnedWidth;
    plan.outHeight = plan.turnedHeight;
    if (options.maxDimension > 0 && std::max(plan.turnedWidth, plan.turnedHeight) > options.maxDimension) {
        plan.scale = double(options.maxDimension) / std::max(plan.turnedWidth, plan.turnedHeight);
        plan.outWidth = int(std::floor(plan.turnedWidth * plan.scale));
        plan.outHeight = int(std::floor(plan.turnedHeight * plan.scale));
    }
    return plan;
}

DecodedPicture decodePlan(DecodePlan &&plan)
{
    DecodedPicture result = std::move(plan.result);
    const DecodeOptions &options = plan.options;
    const int w = plan.width, h = plan.height, n = plan.cfaSize;
    std::vector<float3> rgb;
    if (n == 2) {
        const int bayer[2][2] = {{plan.cfa[0][0], plan.cfa[0][1]}, {plan.cfa[1][0], plan.cfa[1][1]}};
        demosaicMHC(plan.mosaic, w, h, bayer, rgb);
    } else {
        demosaicPattern(plan.mosaic, w, h, plan.cfa, n, rgb);
    }
    plan.mosaic = std::vector<float>();
    if (!options.cameraRGB)
        reconstructHighlights(rgb, w, h, result.whiteBalance, plan.lightNeutral);
    const Mat3 toRec2020 = plan.toRec2020;
    const float gain = plan.gain;
    parallelRows(h, [&](int begin, int end) {
        for (int y = begin; y < end; ++y)
            for (int x = 0; x < w; ++x) {
                float3 &p = rgb[size_t(y) * w + x];
                p = toRec2020 * p * gain;
            }
    });

    // the detail steps, on the demosaic's grid before the lens's warp, with
    // the noise measured once, here
    const bool repair = options.falseColour || options.colourNoiseReduction;
    if (!options.cameraRGB && (repair || options.sharpening)) {
        const PictureNoise noise = measureNoise(rgb, w, h);
        result.noise[0] = noise.full.a;
        result.noise[1] = noise.full.b;
        result.noise[2] = noise.half.a;
        result.noise[3] = noise.half.b;
        if (repair)
            repairOvershoot(rgb, w, h, noise.full);
        if (options.falseColour)
            suppressFalseColour(rgb, w, h);
        if (options.colourNoiseReduction)
            reduceColourNoise(rgb, w, h, noise.half);
        if (options.sharpening)
            sharpenCapture(rgb, w, h, noise.full);
    }

    correctDistortion(rgb, w, h, result.distortion);

    // upright
    const int ow = plan.turnedWidth, oh = plan.turnedHeight;
    if (plan.orientation != 1) {
        std::vector<float3> turned(rgb.size());
        const int orientation = plan.orientation;
        parallelRows(oh, [&](int begin, int end) {
            for (int y = begin; y < end; ++y)
                for (int x = 0; x < ow; ++x) {
                    int sx = x, sy = y;
                    if (orientation == 3) { sx = w - 1 - x; sy = h - 1 - y; }
                    else if (orientation == 6) { sx = y; sy = h - 1 - x; }       // 90 clockwise
                    else if (orientation == 8) { sx = w - 1 - y; sy = x; }       // 90 counter-clockwise
                    turned[size_t(y) * ow + x] = rgb[size_t(sy) * w + sx];
                }
        });
        rgb.swap(turned);
    }
    if (plan.scale < 1)
        rgb = scaleDown(rgb, ow, oh, plan.outWidth, plan.outHeight, plan.scale);

    Dump &out = result.pixels;
    out.width = plan.outWidth;
    out.height = plan.outHeight;
    out.channels = 4;
    out.floats.resize(size_t(out.width) * out.height * 4);
    for (size_t i = 0; i < rgb.size(); ++i) {
        out.floats[i * 4 + 0] = rgb[i].x;
        out.floats[i * 4 + 1] = rgb[i].y;
        out.floats[i * 4 + 2] = rgb[i].z;
        out.floats[i * 4 + 3] = 1;
    }
    return result;
}

DecodedPicture decodeRaw(const std::string &path, const DecodeOptions &options)
{
    return decodePlan(planDecode(path, options));
}
