#include "FilmMath.h"

#include <cmath>
#include <limits>

namespace filmmath {

float normalCDF(float z)
{
    return 0.5f * (1 + std::erf(z / float(std::sqrt(2.0))));
}

float paperDensity(const CurveModel &model, int channel, float x, float gamma, bool isPositive)
{
    float total = 0;
    const float g = std::max(gamma, 1e-4f);
    for (int layer = 0; layer < model.nLayers; ++layer) {
        const size_t i = size_t(channel) * model.nLayers + layer;
        const float mu = model.centers[i], amplitude = model.amplitudes[i], sigma = model.sigmas[i];
        const float sigmaMorphed = std::max(sigma / g, 0.05f);     // SIGMA_FLOOR in morph_curves
        const float muMorphed = mu / g;
        float z = (x - muMorphed) / sigmaMorphed;
        if (isPositive)
            z = -z;                                                 // agx _signed_z
        total += amplitude * normalCDF(z);
    }
    return total;
}

Mat3 dirCouplerMatrix(const DirCouplerDefaults &d, float amount)
{
    float m[3][3] = {};
    for (int i = 0; i < 3; ++i)
        m[i][i] = d.gammaSameLayerRGB[i] * d.inhibitionSameLayer;
    m[0][1] += d.gammaInterlayerRtoGB[0] * d.inhibitionInterLayer;
    m[0][2] += d.gammaInterlayerRtoGB[1] * d.inhibitionInterLayer;
    m[1][0] += d.gammaInterlayerGtoRB[0] * d.inhibitionInterLayer;
    m[1][2] += d.gammaInterlayerGtoRB[1] * d.inhibitionInterLayer;
    m[2][0] += d.gammaInterlayerBtoRG[0] * d.inhibitionInterLayer;
    m[2][1] += d.gammaInterlayerBtoRG[1] * d.inhibitionInterLayer;
    for (auto &row : m)
        for (float &v : row)
            v *= amount;
    // donor as the column, as the shader reads it
    return Mat3::columns({m[0][0], m[0][1], m[0][2]}, {m[1][0], m[1][1], m[1][2]}, {m[2][0], m[2][1], m[2][2]});
}

Curves oversampled(const std::vector<float> &curves, const std::vector<float> &logExposure, int k)
{
    const int n = int(logExposure.size());
    if (n <= 1 || k <= 1)
        return {curves, logExposure};
    const int m = (n - 1) * k + 1;
    Curves out{std::vector<float>(size_t(m) * 3), std::vector<float>(size_t(m))};
    for (int j = 0; j < m; ++j) {
        const int i = std::min(j / k, n - 2);
        const float t = float(j - i * k) / float(k);
        out.logExposure[size_t(j)] = logExposure[size_t(i)] + (logExposure[size_t(i) + 1] - logExposure[size_t(i)]) * t;
        for (int c = 0; c < 3; ++c)
            out.curves[size_t(j) * 3 + c] = curves[size_t(i) * 3 + c]
                + (curves[(size_t(i) + 1) * 3 + c] - curves[size_t(i) * 3 + c]) * t;
    }
    return out;
}

std::vector<float> densityCurvesBeforeCouplers(const std::vector<float> &curves, const std::vector<float> &logExposure,
                                               const Mat3 &matrix, float3 densityMax, bool isPositive)
{
    const size_t n = logExposure.size();
    std::vector<float> silver(n * 3);
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c)
            silver[i * 3 + c] = isPositive ? (densityMax[c] - curves[i * 3 + c]) : curves[i * 3 + c];
    // couplers_amount_curves = density_curves_silver @ matrix, donor -> receiver
    std::vector<float> shiftedAxis(n * 3);
    for (size_t i = 0; i < n; ++i) {
        const float3 s{silver[i * 3], silver[i * 3 + 1], silver[i * 3 + 2]};
        for (int m = 0; m < 3; ++m) {
            const float amount = s.x * matrix.c[0][m] + s.y * matrix.c[1][m] + s.z * matrix.c[2][m];
            shiftedAxis[i * 3 + m] = logExposure[i] - amount;
        }
    }
    // each channel resampled back onto the original axis
    std::vector<float> out(n * 3), xs(n), ys(n);
    for (int c = 0; c < 3; ++c) {
        for (size_t i = 0; i < n; ++i) {
            xs[i] = shiftedAxis[i * 3 + c];
            ys[i] = curves[i * 3 + c];
        }
        for (size_t i = 0; i < n; ++i)
            out[i * 3 + c] = interpolate(logExposure[i], xs, ys);
    }
    return out;
}

std::vector<float> edgeGainScale(const std::vector<float> &curves, const std::vector<float> &logExposure,
                                 const Mat3 &matrix, float reference)
{
    const int n = int(logExposure.size());
    if (n <= 2)
        return std::vector<float>(size_t(n) * 3, 1.0f);
    // |dS/dlogE| per donor layer
    std::vector<float> slope(size_t(n) * 3, 0.0f);
    for (int i = 0; i < n; ++i) {
        const int lo = std::max(i - 1, 0), hi = std::min(i + 1, n - 1);
        const float dx = logExposure[size_t(hi)] - logExposure[size_t(lo)];
        for (int c = 0; c < 3; ++c)
            if (dx > 0)
                slope[size_t(i) * 3 + c] = std::abs(curves[size_t(hi) * 3 + c] - curves[size_t(lo) * 3 + c]) / dx;
    }
    auto gain = [&](int i, int m) {
        const float b = slope[size_t(i) * 3] * matrix.c[0][m] + slope[size_t(i) * 3 + 1] * matrix.c[1][m]
            + slope[size_t(i) * 3 + 2] * matrix.c[2][m];
        return 1 / std::max(1 - b, 0.05f);
    };
    std::vector<float> out(size_t(n) * 3, 1.0f);
    for (int m = 0; m < 3; ++m) {
        std::vector<float> g(size_t(n), 1.0f);
        for (int i = 0; i < n; ++i)
            g[size_t(i)] = gain(i, m);
        const float ref = interpolate(reference, logExposure, g);
        for (int i = 0; i < n; ++i)
            if (g[size_t(i)] > ref)
                out[size_t(i) * 3 + m] = ref > 1 ? (ref - 1) / (g[size_t(i)] - 1) : 0;
    }
    return out;
}

float interpolate(float x, const std::vector<float> &xs, const std::vector<float> &ys)
{
    const int n = int(xs.size());
    if (n == 0)
        return 0;
    if (x <= xs[0])
        return ys[0];
    if (x >= xs[size_t(n) - 1])
        return ys[size_t(n) - 1];
    int lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (xs[size_t(mid)] <= x)
            lo = mid;
        else
            hi = mid;
    }
    const float dx = xs[size_t(hi)] - xs[size_t(lo)];
    if (dx == 0)
        return ys[size_t(hi)];
    const float t = (x - xs[size_t(lo)]) / dx;
    return ys[size_t(lo)] + t * (ys[size_t(hi)] - ys[size_t(lo)]);
}

float3 sampleCurves(const std::vector<float> &curves, float3 x, float logE0, float step, float3 gamma)
{
    float3 out;
    for (int c = 0; c < 3; ++c) {
        float pos = (x[c] * gamma[c] - logE0) / step;
        pos = std::min(std::max(pos, 0.0f), float(kCurveSamples - 1));
        const int i0 = int(pos);
        const int i1 = std::min(i0 + 1, kCurveSamples - 1);
        const float t = pos - float(i0);
        out[c] = curves[size_t(i0) * 3 + c] * (1 - t) + curves[size_t(i1) * 3 + c] * t;
    }
    return out;
}

std::vector<float> enlargerIlluminant(const std::vector<float> &source, const std::vector<float> &dichroics, float3 cc)
{
    const float3 transmittance{std::pow(10.0f, -cc.x / 100), std::pow(10.0f, -cc.y / 100), std::pow(10.0f, -cc.z / 100)};
    std::vector<float> out(kWavelengths);
    for (int l = 0; l < kWavelengths; ++l) {
        float total = 1;
        for (int i = 0; i < 3; ++i) {
            const float f = dichroics[size_t(l) * 3 + i];
            total *= 1 - (1 - f) * (1 - transmittance[i]);
        }
        out[size_t(l)] = source[size_t(l)] * total;
    }
    return out;
}

std::vector<float> densitySpectral(float3 cmy, const std::vector<float> &channelDensity,
                                   const std::vector<float> &baseDensity, const std::vector<float> &valid)
{
    std::vector<float> out(kWavelengths);
    for (int l = 0; l < kWavelengths; ++l) {
        if (valid[size_t(l)] == 0) {
            out[size_t(l)] = std::numeric_limits<float>::infinity();
            continue;
        }
        out[size_t(l)] = cmy.x * channelDensity[size_t(l) * 3 + 0] + cmy.y * channelDensity[size_t(l) * 3 + 1]
            + cmy.z * channelDensity[size_t(l) * 3 + 2] + (baseDensity.empty() ? 0 : baseDensity[size_t(l)]);
    }
    return out;
}

float3 midgrayExposureFactor(const std::vector<float> &densitySpectralMidgray, const std::vector<float> &printIlluminant,
                             const std::vector<float> &paperSensitivity, const std::vector<float> &valid)
{
    float3 raw;
    for (int l = 0; l < kWavelengths; ++l) {
        if (valid[size_t(l)] == 0)
            continue;
        const float light = std::pow(10.0f, -densitySpectralMidgray[size_t(l)]) * printIlluminant[size_t(l)];
        raw.x += light * paperSensitivity[size_t(l) * 3 + 0];
        raw.y += light * paperSensitivity[size_t(l) * 3 + 1];
        raw.z += light * paperSensitivity[size_t(l) * 3 + 2];
    }
    raw = max(raw, float3::all(1e-10f));
    const float geoMean = std::exp((std::log(raw.x) + std::log(raw.y) + std::log(raw.z)) / 3);
    return float3::all(1 / geoMean);
}

const Mat3 oklabM1 = Mat3::rows({0.8189330101f, 0.3618667424f, -0.1288597137f},
                                {0.0329845436f, 0.9293118715f, 0.0361456387f},
                                {0.0482003018f, 0.2643662691f, 0.6338517070f});

const Mat3 cat02 = Mat3::rows({0.7328f, 0.4296f, -0.1624f},
                              {-0.7036f, 1.6975f, 0.0061f},
                              {0.0030f, 0.0136f, 0.9834f});

Mat3 vonKriesAdaptation(float3 source, float3 target)
{
    const float3 gain = (cat02 * target) / max(cat02 * source, float3::all(1e-6f));
    return cat02.inverse() * Mat3::diagonal(gain) * cat02;
}

float3 preflashRaw(const std::vector<float> &baseDensity, const std::vector<float> &valid,
                   const std::vector<float> &preflashIlluminant, const std::vector<float> &paperSensitivity,
                   float exposure)
{
    if (!(exposure > 0))
        return {};
    float3 raw;
    for (int l = 0; l < kWavelengths; ++l) {
        if (valid[size_t(l)] == 0)
            continue;
        const float base = baseDensity.empty() ? 0 : baseDensity[size_t(l)];
        const float light = std::pow(10.0f, -base) * preflashIlluminant[size_t(l)];
        raw.x += light * paperSensitivity[size_t(l) * 3 + 0];
        raw.y += light * paperSensitivity[size_t(l) * 3 + 1];
        raw.z += light * paperSensitivity[size_t(l) * 3 + 2];
    }
    return raw * exposure;
}

} // namespace filmmath
