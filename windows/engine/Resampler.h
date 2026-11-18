#pragma once

#include <utility>
#include <vector>

// The finished print made smaller, for an export with a long edge chosen
// (the Mac's Resampler.swift, step for step).
//
// The print is made at full size and shrunk in linear light, where light adds
// up, so fine detail, grain and colour keep their brightness. The filter is
// Lanczos-3, stretched by the reduction; each channel of a pixel is held to
// the range of the light under the filter's main lobe, which keeps an edge
// crisp and none of the ringing that would clip, in linear light, into dark
// outlines and coloured specks.
namespace resampler {

// The size of a width x height print exported with its long edge longEdge
// pixels, the short edge in proportion, to the nearest pixel. Its own size
// when longEdge is 0 (the full size) or not smaller: never enlarged.
std::pair<int, int> exportSize(int width, int height, int longEdge);

// sinc(x) sinc(x/3), the window of three lobes.
double lanczos3(double x);

// RGBA floats in linear light, width x height, shrunk to toWidth x toHeight,
// no larger on either side.
std::vector<float> shrink(const std::vector<float> &rgba, int width, int height, int toWidth, int toHeight);

} // namespace resampler
