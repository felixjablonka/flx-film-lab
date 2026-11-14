#pragma once

#include <cstddef>
#include <cstdint>

// Apple's default rendering of a RAW file, as macOS shows it in Preview and
// Photos, and as the Mac app's Original (\) shows it: from the decode with
// the camera's own white balance, linear Rec.2020 with Apple's exposure in
// it, as the port's decode makes it.
//
// Measured on the Mac (2026-11-12), from Core Image's CIRAWFilter at its
// defaults against its linear decode, over 15 pictures from a Nikon Z 7, Z f
// and D700, a Ricoh GR IIIx, a Leica M (Typ 240), a Canon EOS 400D and a
// Fujifilm X-T3:
//  - its tone curve (boost 1, shadow boost 0.9) works on each channel of
//    linear Rec.2020 alike and is the same for every camera: one table;
//  - before it, its shadow bias takes s * 2^baselineExposure off the linear
//    values and scales them by 1 / (1 - s), s being its bias over 1000: 5 for
//    DNG files, 3 for the cameras' own formats (Nikon, Canon, Fujifilm);
//  - its gamut mapping, sharpening and noise reduction are left out: at the
//    preview's size they change next to nothing.
// The model against Apple's own rendering, as an sRGB screen shows both:
// ΔE2000 0.15-0.5 median per picture, 0.6-3 at the 95th percentile; 1.7 median
// on the most saturated picture, a tungsten-lit portrait.
namespace applerendering {

// Apple's tone curve, on a linear value.
float toneCurve(float linear);
// Apple's default shadow bias, over 1000.
float shadowBias(bool dng);
// `pixels` RGBA float (linear Rec.2020, white balanced as shot) to RGBA8 in
// sRGB, as Apple renders it. `baselineExposure` is the exposure Apple gives
// the file, EV, and `decodedExposure` the one the decode put in the pixels:
// they differ where the decode has no profile for the camera and the file is
// a DNG, whose own exposure Apple applies. A picture already rendered (`raw`
// false) is its own original: only encoded.
void toSRGB8(const float *rgba, size_t pixels, float baselineExposure, float decodedExposure, bool dng, uint8_t *out,
             bool raw = true);

} // namespace applerendering
