#pragma once

#include "Packs.h"
#include "Recipe.h"

#include <optional>
#include <vector>

// The print stage's tone controls, baked into the 2D transfer LUT
// print_and_scan reads: ToneCurve.swift, in float as it is there.
namespace tonecurve {

// Lightness of the print against channel density: 1 at paper white, 0 at
// paper black, linear in CIE L* between, from the medium's own dyes.
struct LightnessScale {
    float densityWhite = 0, densityBlack = 1;
    std::vector<float> forward;     // lightness at evenly spaced density, white -> black
    std::vector<float> inverse;     // density at evenly spaced lightness, 0 -> 1

    LightnessScale() = default;
    LightnessScale(const ProfilePack &medium, const SharedData &shared, const std::string &viewingIlluminant,
                   float densityWhite, float densityBlack, int samples = 513);
    float lightness(float v) const;
    float density(float t) const;
};

struct Geometry {
    float densityMin = 0, densityMax = 0, range = 0;
    float scale = 1;                // range relative to NegPy's 2.3 reference paper
    float anchor = 0;               // density a mid-grey scene prints at
    LightnessScale lightness;
    float midGrey() const { return lightness.lightness(anchor); }
};

// Brightness, Shadows and Highlights as one curve on lightness.
struct LightnessCurve {
    std::vector<float> table;
    static float brightnessProfile(float t, float midGrey);
    static std::optional<LightnessCurve> make(float brightness, float shadows, float highlights, float midGrey);
    float apply(float t) const;
};

Geometry geometry(const ProfilePack &paper, const SharedData &shared);
Geometry transparencyGeometry(const ProfilePack &film, const SharedData &shared);

// The paper's own curve for a neutral tone at a grade, before any control.
float neutralPaperDensity(const ProfilePack &paper, float logExposure, float grade, float softest = kGradeMax);

struct ToneLut {
    std::vector<float> data;        // (17, 512, 4)
    Geometry geometry;
    float axisStart = 0, axisStep = 0;
};
ToneLut buildLUT(const ProfilePack &paper, const Recipe &recipe, float softest, const SharedData &shared);
ToneLut buildTransparencyLUT(const ProfilePack &film, const Recipe &recipe, const SharedData &shared);

// Brightness, Shadows and Highlights as one density change for all three
// layers, by mean density.
struct NeutralShift {
    float densityMax = 0;
    std::vector<float> values;
};
std::optional<NeutralShift> neutralShift(const Geometry &g, const Recipe &recipe);

// The paper's curve as the render prints it, each layer's density along the
// log exposure axis: the curve plot's (ToneCurve.plotSamples).
struct PlotSample {
    float logExposure = 0;
    float density[3] = {};
};
std::vector<PlotSample> plotSamples(const ProfilePack &paper, const Recipe &recipe, const SharedData &shared,
                                    int count = 192);

} // namespace tonecurve
