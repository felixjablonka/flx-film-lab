#include "ToneCurve.h"

#include "FilmMath.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace tonecurve {

namespace {

// NegPy's toe, shoulder and snap geometry is quoted against a 2.3 D-max paper.
constexpr float kNegpyReferenceRange = 2.3f;
constexpr float kToeHeight = 0.90f;
constexpr float kShoulderHeight = 0.35f;
constexpr float kSnapWidth = 0.6f;
constexpr float kPi = std::numbers::pi_v<float>;

// LightnessCurve's constants
constexpr int kCurveSamples = 1024;
constexpr float kSlopeFloor = 0.05f;
constexpr float kMidGap = 0.02f;
constexpr float kBandPeak = 0.12f;
constexpr float kMargin = 0.85f;
constexpr float kBandCeiling = 0.18f;

constexpr int kNeutralShiftSamples = 1024;

float clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

float smoothstep(float a, float b, float x)
{
    const float t = std::min(std::max((x - a) / (b - a), 0.0f), 1.0f);
    return t * t * (3 - 2 * t);
}

// Which band of the print a tone belongs to: (shadow, highlight).
std::pair<float, float> zoneWeights(float t, float m)
{
    return {1 - smoothstep(0.12f, m - 0.05f, t), smoothstep(m + 0.05f, 0.88f, t)};
}

// A smooth, lopsided bump: a quarter sine squared up each side.
float band(float t, float a, float p, float b)
{
    if (!(t > a && t < b))
        return 0;
    const float half = t < p ? (t - a) / (p - a) : (b - t) / (b - p);
    const float s = std::sin(kPi / 2 * half);
    return s * s;
}

float shadowProfile(float t, float m) { return band(t, 0, kBandPeak, m - kMidGap); }
float highlightProfile(float t, float m) { return band(t, m + kMidGap, 1 - kBandPeak, 1); }

// An amplitude below 2w / pi cannot fold a quarter-sine-squared edge of width w.
float limit(float w) { return std::min(kMargin * 2 * w / kPi, kBandCeiling); }

float paperDensityRaw(const ProfilePack &paper, int channel, float x, float gamma)
{
    return filmmath::paperDensity(paper.curveModel, channel, x, gamma, paper.isPositive());
}

float ccComponent(const CMYOffset &o, int channel)
{
    return channel == 0 ? float(o.cyan) : (channel == 1 ? float(o.magenta) : float(o.yellow));
}

// Steps 2-6 of the stack, on each dye layer's developed density.
float applyControls(float start, const Recipe &recipe, const Geometry &g, int c, float gradeR)
{
    float v = start;
    auto weights = [&](float density) { return zoneWeights(g.lightness.lightness(density), g.midGrey()); };

    // 2 — split contrast, pivoting on the midtone anchor
    if (recipe.shadowGrade != 0 || recipe.highlightGrade != 0) {
        const auto [shadow, highlight] = weights(v);
        const float kShadow = gradeR / std::max(gradeR + float(recipe.shadowGrade), 1.0f) - 1;
        const float kHighlight = gradeR / std::max(gradeR + float(recipe.highlightGrade), 1.0f) - 1;
        v += (kShadow * shadow + kHighlight * highlight) * (v - g.anchor);
    }
    // 3 — regional colour timing
    const float shadowCC = ccComponent(recipe.shadowCMY, c);
    const float highlightCC = ccComponent(recipe.highlightCMY, c);
    if (shadowCC != 0 || highlightCC != 0) {
        const auto [shadow, highlight] = weights(v);
        v += shadowCC / 100 * shadow + highlightCC / 100 * highlight;
    }
    // 4 — snap: anchor-preserving midtone gamma
    const float snap = float(recipe.snap) + float(recipe.snapTrim[c]);
    if (snap != 0) {
        const float w = kSnapWidth * g.scale;
        v += snap * w * std::tanh((v - g.anchor) / w);
    }
    // 5 — shoulder: hold highlights back from paper white
    const float shoulder = float(recipe.shoulder) + float(recipe.shoulderTrim[c]);
    if (shoulder != 0) {
        const float lift = shoulder * kShoulderHeight * g.scale;
        const float reach = std::max(float(recipe.shoulderWidth), 0.05f) * 0.5f * g.range;
        const float d = v - g.densityMin;
        v = g.densityMin + std::max(d + lift * std::exp(-std::max(d, 0.0f) / reach), 0.0f);
    }
    // 6 — toe: lift shadow detail out of paper black
    const float toe = float(recipe.toe) + float(recipe.toeTrim[c]);
    if (toe != 0) {
        const float lift = toe * kToeHeight * g.scale;
        const float reach = std::max(float(recipe.toeWidth), 0.05f) * 0.5f * g.range;
        const float d = g.densityMax - v;
        v = g.densityMax - std::max(d + lift * std::exp(-std::max(d, 0.0f) / reach), 0.0f);
    }
    return std::max(v, 0.0f);
}

float density(const ProfilePack &paper, const Recipe &recipe, const Geometry &g, int c, float x, float localGrade,
              float softest)
{
    // 1 — grade: slope as 1/R through the morph's sigma' = sigma / gamma
    const float gradeR = clampf(float(recipe.grade) + localGrade + float(recipe.gradeTrim[c]), kGradeMin, softest);
    const float gamma = kGradeReference / gradeR;
    return applyControls(paperDensityRaw(paper, c, x, gamma), recipe, g, c, gradeR);
}

bool anyMaskGrade(const Recipe &recipe)
{
    return std::any_of(recipe.masks.begin(), recipe.masks.end(),
                       [](const LocalMask &m) { return m.enabled && m.grade != 0; });
}

float localGradeOfRow(bool masks, int row)
{
    return masks ? kLocalGradeMin + (kLocalGradeMax - kLocalGradeMin) * float(row) / float(kToneLutHeight - 1) : 0;
}

} // namespace

LightnessScale::LightnessScale(const ProfilePack &medium, const SharedData &shared,
                               const std::string &viewingIlluminant, float white, float black, int n)
    : densityWhite(white), densityBlack(std::max(black, white + 1e-3f))
{
    const std::vector<float> &light = shared.illuminant(viewingIlluminant);
    const float step = (densityBlack - densityWhite) / float(n - 1);
    std::vector<float> luminance(size_t(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        const float v = densityWhite + step * float(i);
        float y = 0;
        for (int l = 0; l < kWavelengths; ++l) {
            if (medium.spectralValid[size_t(l)] == 0)
                continue;
            const float dyes = medium.channelDensity[size_t(l) * 3] + medium.channelDensity[size_t(l) * 3 + 1]
                + medium.channelDensity[size_t(l) * 3 + 2];
            const float base = medium.baseDensity.empty() ? 0 : medium.baseDensity[size_t(l)];
            y += std::pow(10.0f, -(v * dyes + base)) * light[size_t(l)] * shared.cmfs[size_t(l) * 3 + 1];
        }
        luminance[size_t(i)] = y;
    }
    const float whiteY = std::max(luminance[0], 1e-20f);
    auto lstar = [&](float y) {
        const float r = y / whiteY;
        return r > 0.008856f ? 116 * std::cbrt(r) - 16 : 903.3f * r;
    };
    const float blackL = lstar(luminance[size_t(n) - 1]);
    const float span = std::max(100 - blackL, 1e-3f);
    forward.resize(size_t(n));
    for (int i = 0; i < n; ++i)
        forward[size_t(i)] = (lstar(luminance[size_t(i)]) - blackL) / span;

    inverse.resize(size_t(n));
    int i = n - 2;
    for (int k = 0; k < n; ++k) {
        const float t = float(k) / float(n - 1);
        while (i > 0 && forward[size_t(i)] < t)
            --i;
        const float t0 = forward[size_t(i)], t1 = forward[size_t(i) + 1];
        const float f = t0 > t1 ? std::min(std::max((t0 - t) / (t0 - t1), 0.0f), 1.0f) : 0;
        inverse[size_t(k)] = densityWhite + step * (float(i) + f);
    }
}

float LightnessScale::lightness(float v) const
{
    const int n = int(forward.size());
    const float pos = (v - densityWhite) / (densityBlack - densityWhite) * float(n - 1);
    const int i = std::min(std::max(int(std::floor(pos)), 0), n - 2);
    return forward[size_t(i)] + (forward[size_t(i) + 1] - forward[size_t(i)]) * (pos - float(i));
}

float LightnessScale::density(float t) const
{
    const int n = int(inverse.size());
    const float pos = std::min(std::max(t, 0.0f), 1.0f) * float(n - 1);
    const int i = std::min(int(std::floor(pos)), n - 2);
    return inverse[size_t(i)] + (inverse[size_t(i) + 1] - inverse[size_t(i)]) * (pos - float(i));
}

float LightnessCurve::brightnessProfile(float t, float m)
{
    if (!(t > 0 && t < 1))
        return 0;
    const float half = t < m ? t / m : (1 - t) / (1 - m);
    const float s = std::sin(kPi / 2 * half);
    return s * s * s;
}

std::optional<LightnessCurve> LightnessCurve::make(float brightness, float shadows, float highlights, float midGrey)
{
    if (brightness == 0 && shadows == 0 && highlights == 0)
        return std::nullopt;
    const float m = std::min(std::max(midGrey, 0.3f), 0.7f);
    const float shadowAmplitude = shadows * (shadows > 0 ? limit(m - kMidGap - kBandPeak) : limit(kBandPeak));
    const float highlightAmplitude = highlights * (highlights > 0 ? limit(kBandPeak) : limit(1 - kBandPeak - m - kMidGap));
    const int n = kCurveSamples;
    const float dt = 1 / float(n);
    std::vector<float> f(size_t(n) + 1), g(size_t(n) + 1, 0.0f);
    for (int k = 0; k <= n; ++k) {
        const float t = float(k) * dt;
        f[size_t(k)] = t + brightness * brightnessProfile(t, m) + shadowAmplitude * shadowProfile(t, m)
            + highlightAmplitude * highlightProfile(t, m);
    }
    for (int k = 0; k < n; ++k)
        g[size_t(k) + 1] = g[size_t(k)] + std::max(f[size_t(k) + 1] - f[size_t(k)], kSlopeFloor * dt);
    const float end = g[size_t(n)];
    LightnessCurve curve;
    curve.table.resize(g.size());
    for (size_t k = 0; k < g.size(); ++k)
        curve.table[k] = g[k] / end;
    return curve;
}

float LightnessCurve::apply(float t) const
{
    if (!(t > 0 && t < 1))
        return t;
    const float pos = t * float(table.size() - 1);
    const int i = std::min(int(pos), int(table.size()) - 2);
    return table[size_t(i)] + (table[size_t(i) + 1] - table[size_t(i)]) * (pos - float(i));
}

Geometry geometry(const ProfilePack &paper, const SharedData &shared)
{
    float anchor = 0;
    for (int c = 0; c < 3; ++c)
        anchor += paperDensityRaw(paper, c, 0, 1);
    anchor /= 3;
    const float dMin = paper.densityMin.empty() ? 0 : *std::min_element(paper.densityMin.begin(), paper.densityMin.end());
    const float dMax = paper.densityMax.empty() ? 2.3f : *std::max_element(paper.densityMax.begin(), paper.densityMax.end());
    Geometry g;
    g.densityMin = dMin;
    g.densityMax = dMax;
    g.range = std::max(dMax - dMin, 1e-3f);
    g.scale = g.range / kNegpyReferenceRange;
    g.anchor = anchor;
    g.lightness = LightnessScale(paper, shared, paper.viewingIlluminant, dMin, dMax);
    return g;
}

Geometry transparencyGeometry(const ProfilePack &film, const SharedData &shared)
{
    const std::vector<float> curves = film.normalizedDensityCurves();
    float anchor = 0;
    for (int c = 0; c < 3; ++c)
        anchor += filmmath::sampleCurves(curves, {0, 0, 0}, film.logExposureStart(), film.logExposureStep(),
                                         {1, 1, 1})[c];
    anchor /= 3;
    float dMax = 2.3f;
    for (int c = 0; c < 3; ++c)
        dMax = c == 0 ? film.densityMax[0] - film.densityMin[0]
                      : std::max(dMax, film.densityMax[size_t(c)] - film.densityMin[size_t(c)]);
    Geometry g;
    g.densityMin = 0;
    g.densityMax = dMax;
    g.range = std::max(dMax, 1e-3f);
    g.scale = g.range / kNegpyReferenceRange;
    g.anchor = anchor;
    g.lightness = LightnessScale(film, shared, film.viewingIlluminant, 0, dMax);
    return g;
}

float neutralPaperDensity(const ProfilePack &paper, float x, float grade, float softest)
{
    const float gamma = kGradeReference / clampf(grade, kGradeMin, softest);
    float d = 0;
    for (int c = 0; c < 3; ++c)
        d += paperDensityRaw(paper, c, x, gamma);
    return d / 3;
}

ToneLut buildLUT(const ProfilePack &paper, const Recipe &recipe, float softest, const SharedData &shared)
{
    ToneLut lut;
    lut.geometry = geometry(paper, shared);
    const float logE0 = paper.logExposureStart();
    const float logE1 = paper.logExposure.back();
    const float step = (logE1 - logE0) / float(kToneLutWidth - 1);
    lut.axisStart = logE0;
    lut.axisStep = step;
    lut.data.assign(size_t(kToneLutWidth) * kToneLutHeight * 4, 0.0f);
    const bool masks = anyMaskGrade(recipe);
    for (int row = 0; row < kToneLutHeight; ++row) {
        const float localGrade = localGradeOfRow(masks, row);
        for (int col = 0; col < kToneLutWidth; ++col) {
            const float x = logE0 + float(col) * step;
            const size_t base = (size_t(row) * kToneLutWidth + col) * 4;
            for (int c = 0; c < 3; ++c)
                lut.data[base + c] = density(paper, recipe, lut.geometry, c, x, localGrade, softest);
            lut.data[base + 3] = 1;
        }
    }
    return lut;
}

ToneLut buildTransparencyLUT(const ProfilePack &film, const Recipe &recipe, const SharedData &shared)
{
    ToneLut lut;
    lut.geometry = transparencyGeometry(film, shared);
    const Geometry &g = lut.geometry;
    // The axis starts exactly at zero density, never below it.
    const float start = 0;
    const float end = g.densityMax * 1.25f;
    const float step = (end - start) / float(kToneLutWidth - 1);
    lut.axisStart = start;
    lut.axisStep = step;
    lut.data.assign(size_t(kToneLutWidth) * kToneLutHeight * 4, 0.0f);
    const bool masks = anyMaskGrade(recipe);
    for (int row = 0; row < kToneLutHeight; ++row) {
        const float localGrade = localGradeOfRow(masks, row);
        for (int col = 0; col < kToneLutWidth; ++col) {
            const float d = start + float(col) * step;
            const size_t base = (size_t(row) * kToneLutWidth + col) * 4;
            for (int c = 0; c < 3; ++c) {
                const float gradeR = clampf(float(recipe.grade) + localGrade + float(recipe.gradeTrim[c]), kGradeMin,
                                            film.softestGrade());
                const float gain = kGradeReference / gradeR;
                lut.data[base + c] = applyControls(g.anchor + (d - g.anchor) * gain, recipe, g, c, gradeR);
            }
            lut.data[base + 3] = 1;
        }
    }
    return lut;
}

std::optional<NeutralShift> neutralShift(const Geometry &g, const Recipe &recipe)
{
    const auto curve = LightnessCurve::make(float(recipe.brightness), float(recipe.shadows), float(recipe.highlights),
                                            g.midGrey());
    if (!curve)
        return std::nullopt;
    NeutralShift shift;
    shift.densityMax = g.densityMax * 1.25f;
    shift.values.assign(size_t(kNeutralShiftSamples), 0.0f);
    for (int i = 0; i < kNeutralShiftSamples; ++i) {
        const float v = shift.densityMax * float(i) / float(kNeutralShiftSamples - 1);
        const float t = g.lightness.lightness(v);
        if (!(t > 0 && t < 1))
            continue;
        shift.values[size_t(i)] = g.lightness.density(curve->apply(t)) - v;
    }
    return shift;
}

std::vector<PlotSample> plotSamples(const ProfilePack &paper, const Recipe &recipe, const SharedData &shared, int count)
{
    const Geometry g = geometry(paper, shared);
    const auto shift = neutralShift(g, recipe);
    const float logE0 = paper.logExposureStart(), logE1 = paper.logExposure.back();
    std::vector<PlotSample> samples(size_t(std::max(count, 2)));
    for (size_t i = 0; i < samples.size(); ++i) {
        PlotSample &p = samples[i];
        p.logExposure = logE0 + (logE1 - logE0) * float(i) / float(samples.size() - 1);
        for (int c = 0; c < 3; ++c)
            p.density[c] = density(paper, recipe, g, c, p.logExposure, 0, kGradeMax);
        if (shift && shift->values.size() >= 2) {
            // NeutralShift.sample, at the mean density
            const size_t n = shift->values.size();
            const float v = (p.density[0] + p.density[1] + p.density[2]) / 3;
            const float pos = std::min(std::max(v / shift->densityMax, 0.0f), 1.0f) * float(n - 1);
            const size_t j = std::min(size_t(pos), n - 2);
            const float add = shift->values[j] + (shift->values[j + 1] - shift->values[j]) * (pos - float(j));
            for (float &d : p.density)
                d += add;
        }
    }
    return samples;
}

} // namespace tonecurve
