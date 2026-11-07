#pragma once

#include "Dump.h"

#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <vector>

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
};
struct IVec4 {
    int32_t x = 0, y = 0, z = 0, w = 0;
};

// The uniform blocks, laid out as the shaders declare them: whole vec4s only,
// which std140, Direct3D's constant buffers and Metal all lay out alike.
struct FilmBlock {
    Vec4 rgbToXYZ[3];               // columns
    Vec4 scale;                     // x: 2^exposureEV
    IVec4 counts;                   // tcLutSize, intoTriangle, gradient count
};
struct DevelopBlock {
    Vec4 axis;                      // logE0, logEStep, edgeCap, diffusionFraction
    Vec4 gammaFactor;
    Vec4 densityMax;
    Vec4 couplers;                  // couplersAmount, isPositive
};
struct PrintBlock {
    Vec4 xyzToRGB[3];
    Vec4 exposureFactorMidgray;
    Vec4 preflashRaw;
    Vec4 exposure;                  // printExposure, cmfNormalization, toneLogE0, toneLogEStep
    Vec4 grades;                    // gradeSpanMin, gradeSpanMax, glareAmount, toneShiftMax
    Vec4 illuminantXYZ;
    Vec4 gamut;                     // threshold, limit, power
    IVec4 flags;                    // toneLutWidth, hasDodgeBurn, gamutCompress, scanFilm
    IVec4 flags2;                   // toneShiftCount
    Vec4 gamutRGBToLMS[3];
    Vec4 gamutLMSToRGB[3];
};
struct ShapeBlock {
    Vec4 columns;                   // column0.xy, column1.xy
    Vec4 offsetSize;                // offset.xy, sourceSize.xy
    IVec4 active;
};
struct EncodeBlock {
    IVec4 modes;                    // encoding, coverage
    Vec4 columns;
    Vec4 offsetSize;
    IVec4 active;
};

// The stock's halation and scatter (HalationDefaults in ProfilePack.swift).
struct HalationParams {
    bool active = false;
    float scatterCoreUm[3] = {};
    float scatterTailUm[3] = {};
    float scatterTailWeight[3] = {};
    float halationStrength[3] = {};
    float halationFirstSigmaUm[3] = {};
    int bounces = 1;
    float bounceDecay = 0;
    bool renormalize = false;
};

// The stock's grain (GrainDefaults); its Selwyn table is RenderSetup::selwyn.
struct GrainParams {
    float tableDensityMax[3] = {};
    float cloudUm[9] = {};          // [record][layer]
    int samples = 0;
};

// The recipe's values the film stage reads beyond the uniforms.
struct RecipeParams {
    double halationAmount = 1, halationScale = 1, scatterAmount = 1, scatterScale = 1;
    double couplersDiffusion = 1, grainAmount = 1, grainScale = 1;
    bool cameraSharpness = false;
    double frameLongMM = 84, frameShortMM = 56;
};

// Glow's effects as the print stage runs them, their sizes in pixels of the
// frame's long edge (Renderer.applyEffects, GlowPipeline.apply).
struct EffectsParams {
    bool bloom = false;             // the highlights blended back
    float bloomAmount = 0, bloomSigma = 0, bloomT0 = 0, bloomT1 = 1;
    int bloomMode = 0;              // 0 lighten, 1 screen, 2 add
    float haze = 0, hazeSigma = 0;  // the veil's share; 0 for none
    bool glow = false;
    float glowAmount = 0, glowSigma = 0, glowT0 = 0.5f;
    bool halation = false;
    float halationAmount = 0, halationSigma = 0, halationT0 = 0, halationT1 = 1;
    float tint[3] = {1, 0.3f, 0.05f};
    bool haloOnly = false;
    bool active() const { return bloom || haze > 0 || glow || halation; }
};

// MPSImageGaussianBlur's kernel at one sigma, as flxref measured it.
struct MpsKernel {
    float sigma = 0;
    std::vector<float> taps;        // odd length, centred
};

// One render's setup: the picture, the uniforms and the tables and LUTs its
// kernels read, as the Mac engine set it up (tools/mac-reference, read by
// loadSetup) or as the port's SetupBuilder makes it.
struct RenderSetup {
    QString directory;              // the dump's, for one read from a dump
    QJsonObject json;               // its setup.json
    int width = 0;
    int height = 0;

    FilmBlock film;
    std::vector<Vec4> gradients;    // two vec4 per gradient; one zero pair when none
    DevelopBlock develop;
    PrintBlock print;
    ShapeBlock shape;               // the frame as it is, or the print's shape (shape.active.x 1)
    EncodeBlock encode;
    int outputWidth = 0, outputHeight = 0;  // the print's size when shaped
    EffectsParams effects;
    // The curves' table (PrintCurves::table: red's 4096 samples, then
    // green's, then blue's), laid on the print last; empty without curves.
    std::vector<float> printCurves;

    // A part of a frame, the loupe's (Renderer.render(part:) on the Mac): the
    // picture set is a box of the frame, and the print made is a part of the
    // whole print. Everything that depends on where a pixel is, is worked
    // out in the frame: the film's scale, the grain, the gradients, the
    // masks. Without it the picture is the whole frame.
    struct Part {
        int frameWidth = 0, frameHeight = 0;
        int boxX = 0, boxY = 0;             // the box, in the frame
        int printX = 0, printY = 0;         // the part made, in the whole print
        ShapeBlock held;                    // a shaped part's pixel to the box's, for apply_geometry
    };
    std::optional<Part> part;

    Dump input;                     // RGBA float32, linear working RGB
    Dump spectral, curves, toneShift, toneLut, tcLut;

    // The film stage's other inputs.
    QString stock;
    bool monochrome = false;
    RecipeParams recipe;
    HalationParams halation;        // halation.active false when the stock has none
    float couplerDiffusionUm = 0;
    Vec4 couplerMatrix[3];          // columns
    Dump curves0, edgeScale;        // the coupler pass's curves, and its edge-gain scale
    GrainParams grain;
    Dump selwyn;                    // empty when the stock has no grain
    uint32_t fileSeed = 0;          // Renderer.grainSeed: the picture's file name, hashed
    uint32_t grainSeed = 0;         // what flxref's engine drew the grain with, to check ours
    float pixelSizeUm = 0;          // what the Mac engine worked the pitch out to, to check ours
    std::vector<MpsKernel> mpsKernels;

    // Which of Renderer.render's stages run.
    struct Stages {
        bool halation = false, couplers = false, grain = false;
        bool shaped = false, effects = false, dodgeBurn = false;
    } stages;
};

// Reads the directory flxref wrote. Throws std::runtime_error.
RenderSetup loadSetup(const QString &directory);
