#pragma once

#include "Mat3.h"

#include <vector>

// The decode's detail steps, after the demosaic and the colour transform, as
// raw developers default to them and as the Mac app has Apple's decoder do
// in principle: false colour suppressed, colour noise reduced, the capture's
// softness sharpened, and no luminance noise reduction (the film's grain is
// the texture). Each works on linear Rec.2020, w x h, and is the port's own
// implementation of a published method. The noise they adapt to is the
// picture's own (measureNoise).

// The picture's noise: the variance of its luminance against the level,
// a Y + b (shot noise and read noise), fitted to the median variance of the
// fine detail, a pixel less its 3x3 mean, at each level. Most of a picture is
// smooth enough at any level that the median there is the noise's; where a
// picture is all texture, it reads more noise than there is, and the steps
// act more gently.
struct Noise {
    double a = 0, b = 0;
    float variance(float y) const;
};
// Measured once, after the colour transform: of the luminance at full size,
// and of its 2x2 means, which the colour noise is reduced at.
struct PictureNoise {
    Noise full, half;
};
PictureNoise measureNoise(const std::vector<float3> &rgb, int w, int h);
// What a fit takes, from every fourth pixel of a luminance plane each way
// (from (1, 1), short of the last row and column): its 3x3 mean and its
// distance from it. A GPU gathers the same and the CPU fits them.
constexpr int kNoiseStep = 4;
void noiseSamples(const std::vector<float> &y, int w, int h, std::vector<float2> &samples);
Noise fitNoise(std::vector<float2> &samples);

// A pixel without light amid well-lit ones, as the demosaic leaves beside a
// point of light (its interpolation's negative lobes): it takes its eight
// neighbours' median, channel by channel. The colour steps would otherwise
// take its false colour and leave it black. Where the neighbours' median is
// within four times the noise, the shadows' noise, nothing is touched.
void repairOvershoot(std::vector<float3> &rgb, int w, int h, const Noise &noise);

// False colour from the demosaic, moiré's colour: the colour differences
// R - G and B - G each through a 3x3 median, once (Freeman's method, as
// dcraw's median filter).
void suppressFalseColour(std::vector<float3> &rgb, int w, int h);

// Colour noise: the chroma of the cube roots of the channels smoothed by a
// guided filter (He, Sun and Tang) whose guide is the luminance, so that the
// chroma keeps to the luminance's edges and is evened out between them, over
// some 10 x 10 pixels on a 24 MP picture. Its threshold for an edge is three
// times the noise at the level.
void reduceColourNoise(std::vector<float3> &rgb, int w, int h, const Noise &half);

// Capture sharpening: Richardson-Lucy deconvolution of the luminance by a
// Gaussian blur of sigma `radius` px, `iterations` times, kept within each
// pixel's 3x3 neighbours' range (no halos), applied as a ratio to all three
// channels so the colour stays. Masked off where the local detail is within
// three times the noise at the level, so noise is not sharpened.
void sharpenCapture(std::vector<float3> &rgb, int w, int h, const Noise &noise, float radius = 0.7f,
                    int iterations = 10);
// Its Gaussian's taps, from -radius, for the GPU's
std::vector<float> sharpeningKernel(float radius);
