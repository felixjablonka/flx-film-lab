#pragma once

#include "Mat3.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

// Apple's camera profiles: how macOS's RAW decoder turns each camera's own
// RGB into colour, measured from Apple's output per camera (tools/calibrate)
// and kept in engine/data/apple-colour.json. At a white balance Apple's
// colour is a 3x3 transform of the camera's RGB,
//
//     Rec.2020 = C diag(1 / w) camera
//
// with w the camera's neutral for the light (its largest channel 1) and C the
// colour. A profile holds the transform measured at each of the app's own
// white balances, and over a grid of temperatures and tints, which the others
// interpolate. On most cameras C follows a two-illuminant DNG profile,
// blended by 1/T and the same at any tint, but the grid assumes no form, and
// it interpolates the transforms whole: at some lights a camera has no
// neutral (the grey takes a negative channel) while the transform stays
// smooth. The camera's RGB is its raw values less black, over white less
// black, as the profile's decoder read them.
struct AppleProfile {
    std::string make, model, decoder;
    float baselineExposure = 0;          // Apple's for the camera, EV: in its transforms already
    // The ISO values Apple gives another exposure (tools/calibrate/iso_exposure.py)
    struct IsoExposure {
        int from = 0, to = 0;
        float exposure = 0;
    };
    std::vector<IsoExposure> isoExposure;
    // Apple's exposure for a file at `iso`: the camera's, or the exception's
    float exposureAt(float iso) const;
    float black = 0, white = 1;          // the decoder's levels the colour was measured with,
    int mosaicWidth = 0, mosaicHeight = 0;   // in this mode
    struct Mode {
        int mosaicWidth = 0, mosaicHeight = 0;
        int crop[4] = {};                // Apple's image area in the mosaic: left, top, width, height
    };
    std::vector<Mode> modes;             // the camera's modes measured: their mosaic sizes and Apple's crops

    // Apple's crop for a mosaic of this size, if that mode was measured
    const int *cropFor(int mosaicWidth, int mosaicHeight) const;
    std::vector<float> gridTemperatures, gridTints;
    std::vector<Mat3> gridTransforms;    // [temperature][tint]
    struct Fixed {
        float temperature = 0, tint = 0; // as Apple reads the white balance
        Mat3 transform;                  // camera -> Rec.2020, measured
    };
    // the app's own white balances: D50 D55 D65 T BB3200 TH-KG3 K75P Tungsten, BB3200 in tables measured since
    // the app had it
    std::map<std::string, Fixed> fixed;

    // Apple's colour at a temperature and tint (Core Image's scale): camera
    // RGB -> linear Rec.2020
    Mat3 transform(float temperature, float tint) const;
    // ... for a camera neutral of its own, as for As shot: the grid's at the
    // temperature and tint where the grid has that neutral, with the
    // camera's own in place of the grid's
    Mat3 transformForNeutral(float3 neutral, float *temperature = nullptr, float *tint = nullptr) const;
};

// The camera RGB a transform takes to grey, largest channel 1. At lights a
// camera cannot balance for, a channel of it is zero or below.
float3 neutralOf(const Mat3 &transform);

class AppleColour {
public:
    // The table at the path, read once; empty when there is none.
    static const AppleColour &table(const std::string &path);
    // The profile for a camera as a decoder names it, if one was measured.
    const AppleProfile *find(const std::string &make, const std::string &model) const;
    size_t size() const { return profiles.size(); }

private:
    std::map<std::string, AppleProfile> profiles;  // "make|model"
};

// A temperature and tint as Apple and the DNG SDK read them (Robertson's
// isotemperature lines, tint -3000 to one unit of uv off the locus) -> xy.
float2 xyOfTemperature(double temperature, double tint);
