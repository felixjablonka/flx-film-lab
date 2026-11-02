#pragma once

#include "Mat3.h"
#include "Packs.h"

#include <vector>

// The film model's CPU side: FilmMath.swift, function for function, in float
// as it is there.
namespace filmmath {

float normalCDF(float z);

// agx morph_curves._evaluate_channel_density with the coupled-gamma morph:
// sigma' = sigma / gamma, mu' = mu / gamma.
float paperDensity(const CurveModel &model, int channel, float logExposure, float gamma, bool isPositive);

// agx couplers.compute_dir_couplers_matrix: column = donor layer, row = receiver.
Mat3 dirCouplerMatrix(const DirCouplerDefaults &d, float amount);

struct Curves {
    std::vector<float> curves;      // (n, 3)
    std::vector<float> logExposure;
};
// The curves on a grid `by` times finer, linearly interpolated.
Curves oversampled(const std::vector<float> &curves, const std::vector<float> &logExposure,
                   int by = kCouplerOversampling);

// agx couplers.compute_density_curves_before_dir_couplers.
std::vector<float> densityCurvesBeforeCouplers(const std::vector<float> &curves, const std::vector<float> &logExposure,
                                               const Mat3 &matrix, float3 densityMax, bool isPositive);

// How much of the diffused inhibitor's edge effect to keep (edgeGainScale).
std::vector<float> edgeGainScale(const std::vector<float> &curves, const std::vector<float> &logExposure,
                                 const Mat3 &matrix, float reference);

// np.interp: linear, clamped to the endpoint values.
float interpolate(float x, const std::vector<float> &xs, const std::vector<float> &ys);

// Linear interpolation on the uniform 256-sample log-exposure grid, per channel.
float3 sampleCurves(const std::vector<float> &curves, float3 x, float logE0, float step, float3 gamma);

// agx color_filters.DichroicFilters.apply_cc, CC units density x 100.
std::vector<float> enlargerIlluminant(const std::vector<float> &source, const std::vector<float> &dichroics, float3 cc);

std::vector<float> densitySpectral(float3 cmy, const std::vector<float> &channelDensity,
                                   const std::vector<float> &baseDensity, const std::vector<float> &valid);

// agx printing._compute_exposure_factor_midgray.
float3 midgrayExposureFactor(const std::vector<float> &densitySpectralMidgray, const std::vector<float> &printIlluminant,
                             const std::vector<float> &paperSensitivity, const std::vector<float> &valid);

extern const Mat3 oklabM1;          // D65-relative XYZ -> LMS
extern const Mat3 cat02;

Mat3 vonKriesAdaptation(float3 source, float3 target);

// agx printing._compute_raw_preflash.
float3 preflashRaw(const std::vector<float> &baseDensity, const std::vector<float> &valid,
                   const std::vector<float> &preflashIlluminant, const std::vector<float> &paperSensitivity,
                   float exposure);

} // namespace filmmath
