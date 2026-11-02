#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The film stage's CPU-side arithmetic, from Renderer.swift, in the same
// float or double order so the numbers come out the same.

// How much film a pixel covers, in um (Renderer.pixelSizeUm): the picture
// laid on the format's frame, long side by short, as large as it fits.
float pixelSizeUm(int width, int height, double frameLongMM, double frameShortMM);

// Renderer.mean: a sum in float, over the count.
float meanOf(const float *values, int count);

// FNV-1a over UTF-8: the grain's seeds (Renderer.fnv1a).
uint32_t fnv1a(const std::string &text);

// The filter that spreads a grain layer's per-pixel noise over its clouds of
// `sigma` pixels, one axis of it: the square root, in frequency, of the
// pixel-averaged covariance (Renderer.cloudFilter). Sums to one.
std::vector<double> cloudFilter(double sigma);

// The port's Gaussian blur, one axis, edges clamped by the kernel that
// applies it. It follows Metal Performance Shaders' MPSImageGaussianBlur,
// which the Mac blurs with, as measured: below a sigma of 1, an exact
// sampled Gaussian over round(3.5 sigma) pixels each side, rounded half up;
// from 1, Apple's own approximation, within 1e-3 of a pixel-integrated
// Gaussian, which is what this uses there, over 4 sigma. Sums to one.
std::vector<float> gaussianTaps(float sigma);
