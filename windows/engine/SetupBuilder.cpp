#include "SetupBuilder.h"

#include "FilmMath.h"
#include "Filters.h"
#include "Lights.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

// agx balances the whole print chain against an 18% grey card.
constexpr float kMidgrayValue = 0.184f;
// Where the darkest and lightest textured tones should print, paper black 0 to white 1.
constexpr float kAutoShadowTarget = 0.07f;
constexpr float kAutoHighlightTarget = 0.93f;
constexpr float kAutoBrightnessStrength = 0.4f;
constexpr float kBrightnessLimit = 0.2f;

Vec4 vec4(float3 v) { return {v.x, v.y, v.z, 0}; }

// An 18% grey under the light a camera filter is made for, as the decode
// presents it to this film (RenderSetupBuilder.midGrey): neutral in the
// film's own light, and in daylight for the 85 and 85B on a tungsten film.
// The print time is set on it, as the filter factor is, so a daylight frame
// through an 85B prints as light as the bare film in its own light.
float3 midGrey(const ProfilePack &film, const LensFilterRef *filter)
{
    const std::optional<Light> light =
        filter && filter->light != film.referenceIlluminant ? chromaticityLight(filter->light) : std::nullopt;
    if (!light)
        return float3::all(kMidgrayValue);
    const float2 xy = light->xy;
    return film.rgbToXYZMatrix().inverse() * (float3{xy.x / xy.y, 1, (1 - xy.x - xy.y) / xy.y} * kMidgrayValue);
}

void putColumns(const Mat3 &m, Vec4 (&out)[3])
{
    for (int c = 0; c < 3; ++c)
        out[c] = vec4(m.c[c]);
}

Dump table(std::vector<float> values, int width, int height, int channels)
{
    Dump d;
    d.width = width;
    d.height = height;
    d.channels = channels;
    d.floats = std::move(values);
    return d;
}

float2 tri2quad(float2 xy)
{
    const float y = xy.y / std::max(1 - xy.x, 1e-10f);
    const float x = (1 - xy.x) * (1 - xy.x);
    return {std::min(std::max(x, 0.0f), 1.0f), std::min(std::max(y, 0.0f), 1.0f)};
}

float2 intoTriangle(float2 xy, float2 white)
{
    const float2 d{xy.x - white.x, xy.y - white.y};
    float t = 1;
    const float s = d.x + d.y;
    if (s > 0)
        t = std::min(t, (1 - white.x - white.y) / s);
    if (d.x < 0)
        t = std::min(t, -white.x / d.x);
    if (d.y < 0)
        t = std::min(t, -white.y / d.y);
    const float k = std::max(t, 0.0f);
    return {white.x + d.x * k, white.y + d.y * k};
}

float percentileOf(const std::vector<float> &sorted, float q)
{
    const float position = q * float(sorted.size() - 1);
    const size_t i = size_t(position);
    const float t = position - float(i);
    return sorted[i] * (1 - t) + sorted[std::min(i + 1, sorted.size() - 1)] * t;
}

} // namespace

float2 tcCoordinate(float3 xyz, float b, const Mat3 &matrix, bool monochrome)
{
    float2 xy{xyz.x / std::max(b, 1e-10f), xyz.y / std::max(b, 1e-10f)};
    if (monochrome) {
        const float3 w = matrix * float3{1, 1, 1};
        const float sum = w.x + w.y + w.z;
        xy = intoTriangle(xy, {w.x / sum, w.y / sum});
    }
    return tri2quad(xy);
}

// Mitchell–Netravali cubic with reflected edges, as sampleTcLut in the kernel.
float3 sampleTcLutCPU(const std::vector<float> &lut, int L, float2 tc)
{
    auto mitchell = [](float t) {
        const float B = 1.0f / 3, C = 1.0f / 3;
        const float x = std::abs(t), x2 = x * x, x3 = x2 * x;
        if (x < 1)
            return (1.0f / 6) * ((12 - 9 * B - 6 * C) * x3 + (-18 + 12 * B + 6 * C) * x2 + (6 - 2 * B));
        if (x < 2)
            return (1.0f / 6) * ((-B - 6 * C) * x3 + (6 * B + 30 * C) * x2 + (-12 * B - 48 * C) * x + (8 * B + 24 * C));
        return 0.0f;
    };
    auto safeIndex = [L](int i) { return i < 0 ? -i : (i >= L ? 2 * (L - 1) - i : i); };
    int base[2];
    float frac[2];
    const float coord[2] = {std::min(std::max(tc.x, 0.0f), 1.0f) * float(L - 1),
                            std::min(std::max(tc.y, 0.0f), 1.0f) * float(L - 1)};
    for (int d = 0; d < 2; ++d) {
        if (coord[d] >= float(L - 1)) {
            base[d] = L - 2;
            frac[d] = 1;
        } else {
            base[d] = int(std::floor(coord[d]));
            frac[d] = coord[d] - float(base[d]);
        }
    }
    float3 acc;
    float weightSum = 0;
    for (int i = 0; i < 4; ++i) {
        const int xi = safeIndex(base[0] - 1 + i);
        const float wx = mitchell(frac[0] + 1 - float(i));
        for (int j = 0; j < 4; ++j) {
            const int yj = safeIndex(base[1] - 1 + j);
            const float w = wx * mitchell(frac[1] + 1 - float(j));
            weightSum += w;
            const size_t o = (size_t(xi) * L + yj) * 3;
            acc += w * float3{lut[o], lut[o + 1], lut[o + 2]};
        }
    }
    return weightSum != 0 ? acc / weightSum : acc;
}

std::vector<float> SetupBuilder::filmDensitySpectral(const ProfilePack &film, float3 rgb, float exposureEV,
                                                     float gamma, const LensFilterRef *lensFilter)
{
    const std::vector<float> &lut = m_packs.tcLut(film, lensFilter);
    const Mat3 matrix = film.rgbToXYZMatrix();
    const float3 xyz = matrix * rgb;
    float b = xyz.x + xyz.y + xyz.z;
    if (!std::isfinite(b))
        b = 0;
    float3 raw = sampleTcLutCPU(lut, film.tcLutSize, tcCoordinate(xyz, b, matrix, film.isMonochrome())) * b;
    raw *= std::pow(2.0f, exposureEV);
    const float3 logRaw{std::log10(std::max(raw.x, 0.0f) + 1e-10f), std::log10(std::max(raw.y, 0.0f) + 1e-10f),
                        std::log10(std::max(raw.z, 0.0f) + 1e-10f)};
    const float3 cmy = filmmath::sampleCurves(film.densityCurves, logRaw, film.logExposureStart(),
                                              film.logExposureStep(), float3::all(gamma));
    return filmmath::densitySpectral(cmy, film.channelDensity, film.baseDensity, film.spectralValid);
}

// The (a*, b*) a mid-grey prints at against the viewing light.
float2 SetupBuilder::printedMidGrey(const ProfilePack &film, const ProfilePack &paper,
                                    const std::vector<float> &midDensity, float3 filters)
{
    const SharedData &shared = m_packs.shared();
    const std::vector<float> lamp = filmmath::enlargerIlluminant(shared.illuminant(paper.referenceIlluminant),
                                                                 shared.dichroicFilters, filters);
    float3 raw;
    for (int l = 0; l < kWavelengths; ++l) {
        if (film.spectralValid[size_t(l)] == 0)
            continue;
        const float light = std::pow(10.0f, -midDensity[size_t(l)]) * lamp[size_t(l)];
        const float3 sensitivity{paper.sensitivity[size_t(l) * 3], paper.sensitivity[size_t(l) * 3 + 1],
                                 paper.sensitivity[size_t(l) * 3 + 2]};
        raw += light * sensitivity;
    }
    raw = max(raw, float3::all(1e-20f));
    const float mean = (std::log10(raw.x) + std::log10(raw.y) + std::log10(raw.z)) / 3;
    const float3 logRaw = float3{std::log10(raw.x), std::log10(raw.y), std::log10(raw.z)} - float3::all(mean);
    float3 cmy;
    for (int c = 0; c < 3; ++c)
        cmy[c] = filmmath::paperDensity(paper.curveModel, c, logRaw[c], 1, paper.isPositive());
    const std::vector<float> &viewing = shared.illuminant(paper.viewingIlluminant);
    float3 xyz;
    for (int l = 0; l < kWavelengths; ++l) {
        if (paper.spectralValid[size_t(l)] == 0)
            continue;
        const float c = cmy.x * paper.channelDensity[size_t(l) * 3];
        const float m = cmy.y * paper.channelDensity[size_t(l) * 3 + 1];
        const float y = cmy.z * paper.channelDensity[size_t(l) * 3 + 2];
        const float base = paper.baseDensity.empty() ? 0 : paper.baseDensity[size_t(l)];
        const float d = c + m + y + base;
        const float light = std::pow(10.0f, -d) * viewing[size_t(l)];
        xyz += light * float3{shared.cmfs[size_t(l) * 3], shared.cmfs[size_t(l) * 3 + 1], shared.cmfs[size_t(l) * 3 + 2]};
    }
    xyz /= shared.cmfNormalization(paper.viewingIlluminant);
    const float3 t = xyz / shared.illuminantXYZ(paper.viewingIlluminant);
    auto f = [](float v) { return v > 0.008856f ? std::cbrt(v) : 7.787f * v + 16.0f / 116; };
    return {500 * (f(t.x) - f(t.y)), 200 * (f(t.y) - f(t.z))};
}

// Magenta and yellow that print the film's mid-grey neutral: Newton on a 2x2
// numerical Jacobian of the print's (a*, b*), from agx's defaults.
float3 SetupBuilder::solveNeutralFiltration(const ProfilePack &film, const ProfilePack &paper)
{
    const std::vector<float> mid = filmDensitySpectral(film, float3::all(kMidgrayValue), 0, 1, nullptr);
    float2 my{65, 55};
    for (int iteration = 0; iteration < 40; ++iteration) {
        const float2 f = printedMidGrey(film, paper, mid, {0, my.x, my.y});
        if (length(f) < 0.005f)
            break;
        const float step = 0.5f;
        const float2 fm = printedMidGrey(film, paper, mid, {0, my.x + step, my.y});
        const float2 fy = printedMidGrey(film, paper, mid, {0, my.x, my.y + step});
        const float2 dm{(fm.x - f.x) / step, (fm.y - f.y) / step};
        const float2 dy{(fy.x - f.x) / step, (fy.y - f.y) / step};
        // the Jacobian's columns dm and dy, and its inverse
        const float det = dm.x * dy.y - dy.x * dm.y;
        if (!(std::abs(det) > 1e-12f))
            break;
        const float2 delta{(dy.y * f.x - dy.x * f.y) / det, (-dm.y * f.x + dm.x * f.y) / det};
        my = {std::min(std::max(my.x - delta.x, 0.0f), 400.0f), std::min(std::max(my.y - delta.y, 0.0f), 400.0f)};
    }
    return {0, my.x, my.y};
}

float3 SetupBuilder::neutralFiltration(const ProfilePack &film, const ProfilePack &paper)
{
    if (paper.isMonochrome())
        return {0, 0, 0};
    if (const auto measured = m_packs.shared().neutralFilters(paper.stock, paper.referenceIlluminant, film.stock))
        return *measured;
    if (film.isPositive())
        return {0, 65, 55};                         // agx EnlargerParams defaults
    return solveNeutralFiltration(film, paper);
}

BuiltSetup SetupBuilder::build(const Recipe &recipe, float exposureEV, int width, int height)
{
    const ProfilePack &film = m_packs.pack(recipe.filmStock);
    const ProfilePack &paper = m_packs.pack(recipe.printStock);
    const SharedData &shared = m_packs.shared();
    if (!film.rgbToXYZ)
        throw std::runtime_error(recipe.filmStock + " is not a film for the camera");

    // A slide printed onto RA-4 paper comes out inverted: by default slides
    // are read off the light table.
    const bool scanFilm = recipe.scanFilm.value_or(film.isPositive());
    const LensFilterRef *lensFilter = film.lensFilter(recipe.lensFilter);

    // the enlarger
    const float3 neutral = neutralFiltration(film, paper);
    const std::vector<float> &source = shared.illuminant(paper.referenceIlluminant);
    const std::vector<float> printIlluminant = filmmath::enlargerIlluminant(
        source, shared.dichroicFilters,
        {neutral.x, neutral.y + float(recipe.filterShiftM), neutral.z + float(recipe.filterShiftY)});
    const std::vector<float> preflashIlluminant = filmmath::enlargerIlluminant(
        source, shared.dichroicFilters,
        {neutral.x, neutral.y + float(recipe.preflashM), neutral.z + float(recipe.preflashY)});

    // mid-grey balance, at the reference exposure
    const std::vector<float> midgray = filmDensitySpectral(film, midGrey(film, lensFilter), 0,
                                                           float(recipe.densityCurveGamma), lensFilter);
    const float3 exposureFactor = filmmath::midgrayExposureFactor(midgray, printIlluminant, paper.sensitivity,
                                                                  film.spectralValid);
    const float3 preflash = filmmath::preflashRaw(film.baseDensity, film.spectralValid, preflashIlluminant,
                                                  paper.sensitivity, float(recipe.preflashExposure));

    // the spectral tables
    std::vector<float> tables(size_t(kSpectralTableFloats), 0.0f);
    auto put = [&](const std::vector<float> &values, int offset, int count) {
        for (int i = 0; i < std::min(count, int(values.size())); ++i)
            tables[size_t(offset + i)] = values[size_t(i)];
    };
    put(film.channelDensity, kOffFilmChannelDensity, kWavelengths * 3);
    put(film.baseDensity, kOffFilmBaseDensity, kWavelengths);
    put(film.spectralValid, kOffFilmValid, kWavelengths);
    put(printIlluminant, kOffPrintIlluminant, kWavelengths);
    put(paper.sensitivity, kOffPaperSensitivity, kWavelengths * 3);
    // reading a transparency, the "print" slots carry the film's own dyes
    const ProfilePack &scanned = scanFilm ? film : paper;
    put(scanned.channelDensity, kOffPrintChannelDensity, kWavelengths * 3);
    put(scanned.baseDensity, kOffPrintBaseDensity, kWavelengths);
    put(scanned.spectralValid, kOffPrintValid, kWavelengths);
    // a transparency's colour balance acts on the light table, with the opposite sign
    const std::vector<float> viewingLight = scanFilm
        ? filmmath::enlargerIlluminant(shared.illuminant(scanned.viewingIlluminant), shared.dichroicFilters,
                                       {0, -float(recipe.filterShiftM), -float(recipe.filterShiftY)})
        : shared.illuminant(scanned.viewingIlluminant);
    put(viewingLight, kOffViewingIlluminant, kWavelengths);
    put(shared.cmfs, kOffCMFs, kWavelengths * 3);

    // a transparency's clear base white-pointed to the light table's own white
    Mat3 scanAdaptation = Mat3::identity();
    if (scanFilm && recipe.normalizeToBase) {
        const std::vector<float> &neutralLight = shared.illuminant(scanned.viewingIlluminant);
        float3 baseXYZ;
        for (int l = 0; l < kWavelengths; ++l) {
            if (scanned.spectralValid[size_t(l)] == 0)
                continue;
            const float base = scanned.baseDensity.empty() ? 0 : scanned.baseDensity[size_t(l)];
            const float light = std::pow(10.0f, -base) * neutralLight[size_t(l)];
            baseXYZ.x += light * shared.cmfs[size_t(l) * 3 + 0];
            baseXYZ.y += light * shared.cmfs[size_t(l) * 3 + 1];
            baseXYZ.z += light * shared.cmfs[size_t(l) * 3 + 2];
        }
        baseXYZ /= shared.cmfNormalization(scanned.viewingIlluminant);
        scanAdaptation = filmmath::vonKriesAdaptation(baseXYZ, shared.illuminantXYZ(scanned.viewingIlluminant));
    }

    // the DIR couplers
    const Mat3 couplerMatrix = film.dirCouplers ? filmmath::dirCouplerMatrix(*film.dirCouplers, float(recipe.couplersAmount))
                                                : Mat3::diagonal({0, 0, 0});
    const std::vector<float> normalizedCurves = film.normalizedDensityCurves();
    const float3 normalizedDensityMax{film.densityMax[0] - film.densityMin[0], film.densityMax[1] - film.densityMin[1],
                                      film.densityMax[2] - film.densityMin[2]};
    const filmmath::Curves fine = filmmath::oversampled(normalizedCurves, film.logExposure);
    const std::vector<float> curvesBefore = recipe.couplersEnabled && film.dirCouplers
        ? filmmath::densityCurvesBeforeCouplers(fine.curves, fine.logExposure, couplerMatrix, normalizedDensityMax,
                                                film.isPositive())
        : fine.curves;
    std::vector<float> edgeScale;
    if (recipe.couplersEnabled && film.dirCouplers && film.dirCouplers->edgeGainReference)
        edgeScale = filmmath::edgeGainScale(fine.curves, fine.logExposure, couplerMatrix,
                                            *film.dirCouplers->edgeGainReference);

    // the tone stage
    const tonecurve::ToneLut tone = scanFilm ? tonecurve::buildTransparencyLUT(film, recipe, shared)
                                             : tonecurve::buildLUT(paper, recipe, film.softestGrade(), shared);
    const auto toneShift = tonecurve::neutralShift(tone.geometry, recipe);

    BuiltSetup built;
    built.resolved = recipe;
    built.exposureEV = exposureEV;
    built.neutralFilters = neutral;
    built.toneGeometry = tone.geometry;
    RenderSetup &s = built.setup;
    s.width = width;
    s.height = height;

    // film_expose
    putColumns(film.rgbToXYZMatrix(), s.film.rgbToXYZ);
    s.film.scale = {std::pow(2.0f, exposureEV), 0, 0, 0};
    for (const ExposureGradient &g : recipe.gradients) {
        if (!g.isActive() || s.gradients.size() / 2 >= size_t(kMaxExposureGradients))
            continue;
        s.gradients.push_back({float(g.ax) * float(width), float(g.ay) * float(height),
                               float(g.bx) * float(width), float(g.by) * float(height)});
        s.gradients.push_back({float(g.exposureEV), 0, 0, 0});
    }
    s.film.counts = {film.tcLutSize ? film.tcLutSize : 192, film.isMonochrome() ? 1 : 0,
                     int32_t(s.gradients.size() / 2), 0};
    if (s.gradients.empty())
        s.gradients.resize(2);

    // filming.develop
    s.develop.axis = {film.logExposureStart(), film.logExposureStep(), edgeScale.empty() ? 0.0f : 1.0f,
                      film.dirCouplers ? film.dirCouplers->diffusionFraction.value_or(1.0f) : 1.0f};
    s.develop.gammaFactor = vec4(float3::all(float(recipe.densityCurveGamma)));
    s.develop.densityMax = vec4(normalizedDensityMax);
    s.develop.couplers = {float(recipe.couplersAmount), film.isPositive() ? 1.0f : 0.0f, 0, 0};

    // print_and_scan
    const bool dodgeBurn = std::any_of(recipe.masks.begin(), recipe.masks.end(), [](const LocalMask &m) { return m.enabled; });
    putColumns(shared.xyzToRGBMatrix(scanned.viewingIlluminant, recipe.outputColorSpace) * scanAdaptation, s.print.xyzToRGB);
    s.print.exposureFactorMidgray = vec4(exposureFactor);
    s.print.preflashRaw = vec4(preflash);
    s.print.exposure = {std::pow(2.0f, float(recipe.density)), shared.cmfNormalization(scanned.viewingIlluminant),
                        tone.axisStart, tone.axisStep};
    s.print.grades = {kLocalGradeMin, kLocalGradeMax,
                      recipe.glareEnabled && !scanFilm ? float(recipe.glareAmount) / 100 : 0.0f,
                      toneShift ? toneShift->densityMax : 0.0f};
    s.print.illuminantXYZ = vec4(shared.illuminantXYZ(scanned.viewingIlluminant));
    // agx OutputGamutCompressSpec.knee as retuned upstream: the last 5% before the edge rolls off
    s.print.gamut = {0.95f, 1.0f, 1.6f, 0};
    s.print.flags = {kToneLutWidth, dodgeBurn ? 1 : 0, recipe.gamutCompress ? 1 : 0, scanFilm ? 1 : 0};
    s.print.flags2 = {toneShift ? int32_t(toneShift->values.size()) : 0, 0, 0, 0};
    const Mat3 d65 = shared.xyzToRGBMatrix("D65", recipe.outputColorSpace);
    putColumns(filmmath::oklabM1 * d65.inverse(), s.print.gamutRGBToLMS);
    putColumns(d65 * filmmath::oklabM1.inverse(), s.print.gamutLMSToRGB);

    // the frame as it is, and the screen's encoding
    s.shape.columns = {1, 0, 0, 1};
    s.encode.modes = {recipe.outputColorSpace == "ProPhoto RGB" ? 2 : 1, 0, 0, 0};
    if (recipe.curves && !recipe.curves->isIdentity())
        s.printCurves = recipe.curves->table();
    s.encode.columns = s.shape.columns;

    // the tables and LUTs
    s.spectral = table(tables, kSpectralTableFloats, 1, 1);
    s.curves = table(normalizedCurves, kCurveSamples, 1, 3);
    s.curves0 = table(curvesBefore, int(curvesBefore.size() / 3), 1, 3);
    s.edgeScale = edgeScale.empty() ? table({1, 1, 1}, 1, 1, 3) : table(edgeScale, int(edgeScale.size() / 3), 1, 3);
    s.toneShift = toneShift ? table(toneShift->values, int(toneShift->values.size()), 1, 1) : table({0, 0}, 2, 1, 1);
    s.toneLut = table(tone.data, kToneLutWidth, kToneLutHeight, 4);
    s.tcLut = table(m_packs.tcLut(film, lensFilter), film.tcLutSize, film.tcLutSize, 3);

    // the film stage's other inputs
    s.stock = QString::fromStdString(film.stock);
    s.monochrome = film.isMonochrome();
    s.recipe.halationAmount = recipe.halationAmount;
    s.recipe.halationScale = recipe.halationScale;
    s.recipe.scatterAmount = recipe.scatterAmount;
    s.recipe.scatterScale = recipe.scatterScale;
    s.recipe.couplersDiffusion = recipe.couplersDiffusion;
    s.recipe.grainAmount = recipe.grainAmount;
    s.recipe.grainScale = recipe.grainScale;
    s.recipe.cameraSharpness = recipe.keepsCameraSharpness();
    s.recipe.frameLongMM = recipe.frameLongMM();
    s.recipe.frameShortMM = recipe.frameShortMM();
    if (film.halation) {
        const HalationDefaults &h = *film.halation;
        s.halation.active = h.active;
        std::copy(std::begin(h.scatterCoreUm), std::end(h.scatterCoreUm), s.halation.scatterCoreUm);
        std::copy(std::begin(h.scatterTailUm), std::end(h.scatterTailUm), s.halation.scatterTailUm);
        std::copy(std::begin(h.scatterTailWeight), std::end(h.scatterTailWeight), s.halation.scatterTailWeight);
        std::copy(std::begin(h.halationStrength), std::end(h.halationStrength), s.halation.halationStrength);
        std::copy(std::begin(h.halationFirstSigmaUm), std::end(h.halationFirstSigmaUm), s.halation.halationFirstSigmaUm);
        s.halation.bounces = h.bounces;
        s.halation.bounceDecay = h.bounceDecay;
        s.halation.renormalize = h.renormalize;
    }
    s.couplerDiffusionUm = film.dirCouplers ? film.dirCouplers->diffusionSizeUm : 0;
    putColumns(couplerMatrix, s.couplerMatrix);
    if (film.grain) {
        std::copy(std::begin(film.grain->tableDensityMax), std::end(film.grain->tableDensityMax), s.grain.tableDensityMax);
        std::copy(std::begin(film.grain->cloudUm), std::end(film.grain->cloudUm), s.grain.cloudUm);
        s.grain.samples = film.grain->tableSamples();
        s.selwyn = table(film.grain->selwynUm2, 3, 3 * s.grain.samples, 1);
    }
    s.pixelSizeUm = pixelSizeUm(width, height, recipe.frameLongMM(), recipe.frameShortMM());
    s.stages.halation = recipe.halationEnabled && film.halation && film.halation->active;
    s.stages.couplers = recipe.couplersEnabled && recipe.couplersAmount > 0 && film.dirCouplers.has_value();
    s.stages.grain = recipe.grainEnabled && recipe.grainAmount > 0 && film.grain.has_value();
    s.stages.shaped = recipe.shaped;
    s.stages.effects = recipe.effects && recipe.effects->isActive();
    s.stages.dodgeBurn = dodgeBurn;
    return built;
}

// Meter a frame the way a printer reads a negative: block averages on a
// coarse grid, through the film and the enlarger, the textured parts voting.
std::optional<SetupBuilder::FrameMetering> SetupBuilder::meterFrame(const Dump &picture, const ProfilePack &film,
                                                                    const BuiltSetup &probe, float exposureEV,
                                                                    bool scanFilm)
{
    const int width = picture.width, height = picture.height;
    const std::vector<float> &px = picture.floats;
    if (width <= 0 || height <= 0 || px.size() < size_t(width) * height * 4)
        return std::nullopt;
    const std::vector<float> &lut = m_packs.tcLut(film, film.lensFilter(probe.resolved.lensFilter));
    const int gridLongEdge = 128;
    const int cell = std::max(std::max(width, height) / gridLongEdge, 1);
    const int gw = std::max(width / cell, 1), gh = std::max(height / cell, 1);
    std::vector<float3> grid(size_t(gw) * gh);
    for (int gy = 0; gy < gh; ++gy) {
        for (int y = gy * cell; y < std::min((gy + 1) * cell, height); ++y) {
            size_t o = size_t(y) * width * 4;
            for (int gx = 0; gx < gw; ++gx) {
                float3 sum;
                for (int k = 0; k < cell; ++k) {
                    if (!(o + 2 < px.size()))
                        continue;
                    sum += float3{px[o], px[o + 1], px[o + 2]};
                    o += 4;
                }
                grid[size_t(gy) * gw + gx] += sum;
            }
        }
    }
    const float perCell = 1 / float(cell * cell);

    const Mat3 matrix = film.rgbToXYZMatrix();
    const float exposureScale = std::pow(2.0f, exposureEV);
    const std::vector<float> curves = film.normalizedDensityCurves();
    const std::vector<float> &tables = probe.setup.spectral.floats;
    const float3 factor{probe.setup.print.exposureFactorMidgray.x, probe.setup.print.exposureFactorMidgray.y,
                        probe.setup.print.exposureFactorMidgray.z};
    std::vector<float> values(grid.size());
    for (size_t i = 0; i < grid.size(); ++i) {
        const float3 xyz = matrix * (grid[i] * perCell);
        float b = xyz.x + xyz.y + xyz.z;
        if (!std::isfinite(b))
            b = 0;
        const float2 tc = tcCoordinate(xyz, b, matrix, film.isMonochrome());
        const float3 raw = max(sampleTcLutCPU(lut, film.tcLutSize, tc) * b * exposureScale, float3::all(0));
        const float3 logRaw{std::log10(raw.x + 1e-10f), std::log10(raw.y + 1e-10f), std::log10(raw.z + 1e-10f)};
        const float3 cmy = filmmath::sampleCurves(curves, logRaw, film.logExposureStart(), film.logExposureStep(),
                                                  float3::all(1));
        if (scanFilm) {
            values[i] = (cmy.x + cmy.y + cmy.z) / 3;
            continue;
        }
        float3 printRaw;
        for (int l = 0; l < kWavelengths; ++l) {
            if (tables[size_t(kOffFilmValid + l)] == 0)
                continue;
            const float d = cmy.x * tables[size_t(kOffFilmChannelDensity + l * 3 + 0)]
                + cmy.y * tables[size_t(kOffFilmChannelDensity + l * 3 + 1)]
                + cmy.z * tables[size_t(kOffFilmChannelDensity + l * 3 + 2)] + tables[size_t(kOffFilmBaseDensity + l)];
            const float light = std::pow(10.0f, -d) * tables[size_t(kOffPrintIlluminant + l)];
            printRaw += light * float3{tables[size_t(kOffPaperSensitivity + l * 3 + 0)],
                                       tables[size_t(kOffPaperSensitivity + l * 3 + 1)],
                                       tables[size_t(kOffPaperSensitivity + l * 3 + 2)]};
        }
        printRaw *= factor;
        values[i] = std::log10(std::max((printRaw.x + printRaw.y + printRaw.z) / 3, 0.0f) + 1e-10f);
    }

    // the texture gate over 2x2 sectors of cells
    std::vector<float> votes;
    votes.reserve(values.size());
    const int sw = gw / 2, sh = gh / 2;
    int active = 0;
    for (int sy = 0; sy < sh; ++sy) {
        for (int sx = 0; sx < sw; ++sx) {
            const size_t i = size_t(2 * sy) * gw + 2 * sx;
            const float q[4] = {values[i], values[i + 1], values[i + gw], values[i + gw + 1]};
            if (*std::max_element(q, q + 4) - *std::min_element(q, q + 4) > 0.05f) {
                votes.insert(votes.end(), q, q + 4);
                ++active;
            }
        }
    }
    const float fraction = float(active) / float(std::max(sw * sh, 1));
    if (fraction < 0.05f)
        votes = values;
    if (votes.size() < 8)
        return std::nullopt;
    std::sort(votes.begin(), votes.end());
    const float lo = percentileOf(votes, 0.05f), hi = percentileOf(votes, 0.95f);
    float windowSum = 0;
    int windowCount = 0;
    for (float v : votes) {
        if (v >= lo && v <= hi) {
            windowSum += v;
            ++windowCount;
        }
    }
    const float windowMean = windowCount == 0 ? 0.5f * (lo + hi) : windowSum / float(windowCount);
    FrameMetering m;
    m.shadow = percentileOf(votes, 0.98f);
    m.highlight = percentileOf(votes, 0.02f);
    m.middle = 0.5f * (windowMean + 0.5f * (lo + hi));
    m.texturedFraction = fraction;
    m.isDensity = scanFilm;
    return m;
}

// Where the paper, at the reference grade, reaches a neutral density.
float SetupBuilder::logExposureAtNeutralDensity(const ProfilePack &paper, float target)
{
    float lo = paper.logExposureStart(), hi = paper.logExposure.back();
    for (int i = 0; i < 40; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (tonecurve::neutralPaperDensity(paper, mid, kGradeReference) < target)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5f * (lo + hi);
}

// Auto Contrast: fit the frame's textured range onto the paper.
double SetupBuilder::autoGrade(const FrameMetering &m, const BuiltSetup &probe, const ProfilePack &film,
                               const ProfilePack &paper, bool scanFilm)
{
    const tonecurve::Geometry &g = probe.toneGeometry;
    const float darkest = g.lightness.density(kAutoShadowTarget);
    const float lightest = g.lightness.density(kAutoHighlightTarget);
    const float span = scanFilm ? darkest - lightest
                                : logExposureAtNeutralDensity(paper, darkest) - logExposureAtNeutralDensity(paper, lightest);
    if (!(span > 1e-3f))
        return double(kGradeReference);
    const float ratio = span / m.range();
    const float slope = ratio < 1 ? std::min(ratio * 1.1f, 1.0f) : std::min(0.7f + 0.3f * ratio, 1.25f);
    return double(std::min(std::max(kGradeReference / slope, kGradeMin), film.softestGrade()));
}

// Auto Brightness: the frame's middle tone partway to where an 18% grey prints.
double SetupBuilder::autoBrightness(const FrameMetering &m, const BuiltSetup &probe, const ProfilePack &film,
                                    const ProfilePack &paper, float grade, bool scanFilm)
{
    const tonecurve::Geometry &g = probe.toneGeometry;
    const float middleDensity = scanFilm
        ? g.anchor + (m.middle - g.anchor) * kGradeReference / std::max(grade, 1.0f)
        : tonecurve::neutralPaperDensity(paper, m.middle, grade, film.softestGrade());
    const float tMiddle = g.lightness.lightness(middleDensity);
    const float want = kAutoBrightnessStrength * (g.midGrey() - tMiddle);
    const float reach = std::max(
        tonecurve::LightnessCurve::brightnessProfile(std::min(std::max(tMiddle, 0.0f), 1.0f), g.midGrey()), 0.6f);
    return double(std::min(std::max(want / reach, -kBrightnessLimit), kBrightnessLimit));
}

// How many stops of highlight the stock holds above a mid-grey exposure.
float SetupBuilder::highlightHeadroomStops(const ProfilePack &film)
{
    const std::vector<float> curves = film.normalizedDensityCurves();
    const int n = kCurveSamples;
    std::vector<float> mean(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        mean[size_t(i)] = (curves[size_t(i) * 3] + curves[size_t(i) * 3 + 1] + curves[size_t(i) * 3 + 2]) / 3;
    const float lo = *std::min_element(mean.begin(), mean.end()), hi = *std::max_element(mean.begin(), mean.end());
    if (!(hi > lo))
        return 4;
    const float target = film.isPositive() ? lo + 0.05f * (hi - lo) : lo + 0.95f * (hi - lo);
    float limit = film.logExposure[size_t(n) - 1];
    for (int i = 0; i < n; ++i) {
        if (film.isPositive() ? mean[size_t(i)] <= target : mean[size_t(i)] >= target) {
            limit = film.logExposureStart() + float(i) * film.logExposureStep();
            break;
        }
    }
    return std::max(limit / std::log10(2.0f), 1.0f);
}

// Meter the negative exposure: the highlights inside the film's headroom, never
// further than the median asks, and only part of the way.
float SetupBuilder::autoExposureEV(const Dump &picture, const ProfilePack &film)
{
    const int width = picture.width, height = picture.height;
    if (width <= 0 || height <= 0)
        return 0;
    const Mat3 m = film.rgbToXYZMatrix();
    const size_t total = size_t(width) * height;
    const size_t stride = std::max(total / 200000, size_t(1));
    std::vector<float> luminances;
    luminances.reserve(total / stride + 1);
    for (size_t i = 0; i < total; i += stride) {
        const size_t o = i * 4;
        const float y = m.c[0][1] * picture.floats[o] + m.c[1][1] * picture.floats[o + 1] + m.c[2][1] * picture.floats[o + 2];
        if (std::isfinite(y) && y > 0)
            luminances.push_back(y);
    }
    if (luminances.size() <= 32)
        return 0;
    std::sort(luminances.begin(), luminances.end());
    const float median = percentileOf(luminances, 0.50f);
    const float highlight = percentileOf(luminances, 0.995f);
    const float fromMedian = -std::log2(median / kMidgrayValue);
    const float headroom = highlightHeadroomStops(film) - 0.5f;
    const float fromHighlight = headroom - std::log2(highlight / kMidgrayValue);
    float ev = std::min(fromMedian, fromHighlight);
    if (!std::isfinite(ev))
        return 0;
    ev *= 0.7f;
    return std::min(std::max(ev, -4.0f), 4.0f);
}

std::optional<SetupBuilder::PrintReading> SetupBuilder::readPrint(const Recipe &recipe, const Dump &picture,
                                                                  float exposureEV)
{
    // the print meters read through the exposure the negative receives
    PrintReading reading{build(recipe, exposureEV, picture.width, picture.height), {}, false};
    reading.scanFilm = reading.probe.setup.print.flags.w != 0;
    const auto metering = meterFrame(picture, m_packs.pack(recipe.filmStock), reading.probe, exposureEV,
                                     reading.scanFilm);
    if (!metering)
        return std::nullopt;
    reading.metering = *metering;
    return reading;
}

Recipe SetupBuilder::applyAuto(const Recipe &recipe, const PrintReading &reading)
{
    const ProfilePack &film = m_packs.pack(recipe.filmStock);
    const ProfilePack &paper = m_packs.pack(recipe.printStock);
    Recipe resolved = recipe;
    if (recipe.autoGrade)
        resolved.grade = autoGrade(reading.metering, reading.probe, film, paper, reading.scanFilm);
    if (recipe.autoDensity)
        resolved.brightness = autoBrightness(reading.metering, reading.probe, film, paper, float(resolved.grade),
                                             reading.scanFilm);
    return resolved;
}

BuiltSetup SetupBuilder::prepare(Recipe recipe, const Dump &picture, const std::string &fileName)
{
    if (recipe.shaped)
        throw std::runtime_error("the port has no turned, levelled or cropped prints yet");
    const ProfilePack &film = m_packs.pack(recipe.filmStock);
    // the meters read what the crop keeps: here the whole frame
    if (recipe.autoExposure)
        recipe.exposureEV = double(autoExposureEV(picture, film));
    const float exposureEV = float(recipe.exposureEV);

    Recipe resolved = recipe;
    if (recipe.autoDensity || recipe.autoGrade)
        if (const auto reading = readPrint(recipe, picture, exposureEV))
            resolved = applyAuto(recipe, *reading);
    BuiltSetup built = build(resolved, exposureEV, picture.width, picture.height);
    built.setup.fileSeed = fnv1a(fileName);
    built.setup.grainSeed = fnv1a(film.stock) ^ (built.setup.fileSeed * 0x9E3779B9u);
    return built;
}

EffectsParams effectsFor(const std::optional<GlowLook> &look, int longEdge)
{
    EffectsParams e;
    if (!look || !look->isActive())
        return e;
    const GlowLook::Bloom &b = look->bloom;
    if (b.isActive()) {
        const double sigma = blurSigmaFraction(b.size) * longEdge;
        if (b.amount > 0) {
            e.bloom = true;
            e.bloomAmount = float(b.amount / 100);
            e.bloomSigma = float(sigma);
            const double t0 = std::clamp(b.threshold / 100, 0.0, 1.0);
            e.bloomT0 = float(t0);
            e.bloomT1 = float(t0 + (1 - t0) * std::clamp(b.softness, 0.0, 100.0) / 100);
            e.bloomMode = b.blend == "screen" ? 1 : b.blend == "add" ? 2 : 0;
        }
        if (b.haze > 0) {
            e.haze = float(b.haze / 100 * 0.35);
            e.hazeSigma = float(std::min(sigma * 5, longEdge * 0.08));
        }
    }
    if (look->glow.isActive()) {
        e.glow = true;
        e.glowAmount = float(look->glow.amount / 100);
        e.glowSigma = float(blurSigmaFraction(look->glow.size) * longEdge);
        e.glowT0 = float(look->glow.threshold / 100);
    }
    const GlowLook::Halation &h = look->halation;
    if (h.isActive()) {
        e.halation = true;
        e.halationAmount = float(h.amount / 100);
        e.halationSigma = float(blurSigmaFraction(h.size) * longEdge);
        // masked on linear light, as in NegPy; the slider is display brightness
        const double t = std::clamp(h.threshold / 100, 0.0, 1.0);
        e.halationT0 = float(t <= 0.04045 ? t / 12.92 : std::pow((t + 0.055) / 1.055, 2.4));
        e.halationT1 = 1;
        for (int c = 0; c < 3; ++c)
            e.tint[c] = float(h.tint[c]);
        e.haloOnly = h.haloOnly;
    }
    return e;
}

Recipe loadingFilm(const Recipe &recipe, const std::string &stock, PackLibrary &packs)
{
    Recipe r = recipe;
    if (stock == r.filmStock)
        return r;
    auto tryPack = [&](const std::string &name) -> const ProfilePack * {
        try {
            return &packs.pack(name);
        } catch (const std::exception &) {
            return nullptr;
        }
    };
    const ProfilePack *film = tryPack(stock);
    const ProfilePack *previous = tryPack(r.filmStock);
    r.filmStock = stock;
    if (film && previous) {
        if (film->isPositive() != previous->isPositive())
            r.scanFilm.reset();
        if (r.grade == double(previous->defaultGrade()))
            r.grade = double(film->defaultGrade());
        const ProfilePack *paper = tryPack(r.printStock);
        const auto &papers = packs.paperEntries();
        if (paper && !film->targetPrint.empty()
            && std::any_of(papers.begin(), papers.end(),
                           [&](const PackIndexEntry &e) { return e.stock == film->targetPrint; })) {
            const bool otherColour = film->isMonochrome() != previous->isMonochrome()
                                     && film->isMonochrome() != paper->isMonochrome();
            // a motion picture film always onto its own print film, as a lab
            // prints it, and a still film picked after one off the print film
            // onto its own paper; a print film chosen for a still film stays
            const bool printFilm = film->isMotionPicture() || (previous->isMotionPicture() && paper->isMotionPicture());
            if (otherColour || printFilm)
                r.printStock = film->targetPrint;
        }
    }
    if (r.lensFilter && (!film || !film->lensFilter(r.lensFilter)))
        r.lensFilter.reset();
    return r;
}
