#pragma once

#include "Mat3.h"
#include "Packs.h"
#include "Recipe.h"
#include "Setup.h"
#include "ToneCurve.h"

#include <optional>
#include <string>

// RenderSetupBuilder.swift: a recipe and a picture to everything the render
// reads, and the meters behind Auto Exposure, Auto Brightness and Auto
// Contrast.

struct BuiltSetup {
    RenderSetup setup;              // as the Mac's dumps carry it, but the picture
    Recipe resolved;                // the recipe after the meters
    float exposureEV = 0;           // what the negative is exposed at
    float3 neutralFilters;          // the enlarger's neutral filtration, c m y
    tonecurve::Geometry toneGeometry;
};

// Glow's look as the print stage runs it, its sizes scaled to the frame's
// long edge (GlowPipeline.apply).
EffectsParams effectsFor(const std::optional<GlowLook> &look, int longEdge);

// The film picker's choice (Recipe.loadingFilm): "Print on paper" set by hand
// is a choice for one kind of film; a grade left at the last film's default
// moves to this one's; crossing between colour and black and white moves the
// print to the film's own paper, and a motion picture film onto its own print
// film; a camera filter the new film is not baked for goes.
Recipe loadingFilm(const Recipe &recipe, const std::string &stock, PackLibrary &packs);

class SetupBuilder
{
public:
    struct FrameMetering {
        float shadow = 0, highlight = 0, middle = 0, texturedFraction = 0;
        bool isDensity = false;
        float range() const { return std::max(shadow - highlight, 1e-3f); }
    };
    // What the print meters read (RenderSetupBuilder.PrintReading): the
    // frame through the exposure the negative receives.
    struct PrintReading {
        BuiltSetup probe;
        FrameMetering metering;
        bool scanFilm = false;
    };

    explicit SetupBuilder(PackLibrary &packs) : m_packs(packs) {}

    // As flxrender renders a picture: Auto Exposure metered on it, Auto
    // Brightness and Auto Contrast read off it through that exposure, then
    // the setup. `fileName` seeds the grain. Throws std::runtime_error.
    BuiltSetup prepare(Recipe recipe, const Dump &picture, const std::string &fileName);

    // RenderSetupBuilder.build: the setup for `recipe` with the negative
    // exposed at `exposureEV`, for a picture of `width` x `height`.
    BuiltSetup build(const Recipe &recipe, float exposureEV, int width, int height);

    // RenderSetupBuilder.autoExposureEV.
    float autoExposureEV(const Dump &picture, const ProfilePack &film);

    // RenderSetupBuilder.readPrint and applyAuto: the print meters' reading
    // of the picture at `exposureEV`, none for a frame without texture; and
    // the recipe with Auto Contrast's and Auto Brightness's values in, for
    // those that are on.
    std::optional<PrintReading> readPrint(const Recipe &recipe, const Dump &picture, float exposureEV);
    Recipe applyAuto(const Recipe &recipe, const PrintReading &reading);

    PackLibrary &packs() { return m_packs; }
    // How many stops of highlight the stock holds above a mid-grey exposure.
    float highlightHeadroomStops(const ProfilePack &film);

private:

    float3 neutralFiltration(const ProfilePack &film, const ProfilePack &paper);
    float3 solveNeutralFiltration(const ProfilePack &film, const ProfilePack &paper);
    float2 printedMidGrey(const ProfilePack &film, const ProfilePack &paper, const std::vector<float> &midDensity,
                          float3 filters);
    std::vector<float> filmDensitySpectral(const ProfilePack &film, float3 rgb, float exposureEV, float gamma,
                                           const LensFilterRef *lensFilter);
    std::optional<FrameMetering> meterFrame(const Dump &picture, const ProfilePack &film, const BuiltSetup &probe,
                                            float exposureEV, bool scanFilm);
    double autoGrade(const FrameMetering &m, const BuiltSetup &probe, const ProfilePack &film,
                     const ProfilePack &paper, bool scanFilm);
    double autoBrightness(const FrameMetering &m, const BuiltSetup &probe, const ProfilePack &film,
                          const ProfilePack &paper, float grade, bool scanFilm);
    float logExposureAtNeutralDensity(const ProfilePack &paper, float target);

    PackLibrary &m_packs;
};

// The CPU twins of film_expose's LUT lookup.
float3 sampleTcLutCPU(const std::vector<float> &lut, int size, float2 tc);
float2 tcCoordinate(float3 xyz, float b, const Mat3 &matrix, bool monochrome);
