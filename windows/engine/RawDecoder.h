#pragma once

#include "Dump.h"
#include "LensCorrection.h"
#include "RawUnpack.h"

#include <string>

// The port's RAW decode: a raw file to the engine's input, linear Rec.2020
// with the white balanced for the film's reference light, as the Mac app's
// RawLoader makes it with Apple's decoder (CIRAWFilter). The mosaic is
// unpacked by RawSpeed or LibRaw (RawUnpack); the rest is the port's own.

struct DecodeOptions {
    // RawLoader.Options: "film" (the film's reference light), "as-shot",
    // "daylight-d55", "daylight" (D65), "tungsten-3200", "tungsten" (2850 K)
    // or "custom" (temperature and tint, Core Image's)
    std::string whiteBalance = "film";
    std::string reference = "D55";  // the film's reference illuminant (RawLoader.Options.filmReference)
    float temperature = 5500, tint = 0;
    int maxDimension = 0;           // the long side, scaled down with Lanczos; 0 keeps the full size
    // Apple's crop of the mosaic: the image area inside it, when known. All
    // zero: the decoder's own.
    int cropLeft = 0, cropTop = 0, cropWidth = 0, cropHeight = 0;
    float exposureEV = 0;           // without a profile: Apple's baseline exposure, for comparing
    // Apple's camera profiles (AppleColour): their colour, crop and exposure
    // for every camera measured; the camera's daylight matrix for the rest
    std::string appleColour;
    // The camera's own RGB, balanced for daylight (D55 through Adobe's
    // matrix) but not converted: what Apple's colour is measured against.
    bool cameraRGB = false;
    bool upright = true;            // turned as the camera was held; false leaves the sensor's frame
    // The lens's distortion corrected, as RawLoader has Apple's decoder do
    // (LensCorrection): from the camera's data, else from Lensfun's database
    // in lensfunDir; not in the camera's own RGB
    bool lensCorrection = true;
    bool cameraLensData = true;     // false: Lensfun's alone, for testing it
    std::string lensfunDir;
    // The detail steps (RawDetail), on as RawLoader leaves Apple's: false
    // colour suppressed, colour noise reduced, the capture sharpened; never
    // luminance noise reduction. None in the camera's own RGB.
    bool falseColour = true, colourNoiseReduction = true, sharpening = true;
    // "rawspeed", "libraw", or "" for RawSpeed with LibRaw where it fails
    std::string decoder;
    std::string camerasXml;         // RawSpeed's camera list
};

struct DecodedPicture {
    Dump pixels;                    // RGBA float32
    RawMosaic mosaic;               // its data released
    float3 whiteBalance;            // what each camera channel was multiplied by
    int cropLeft = 0, cropTop = 0, cropWidth = 0, cropHeight = 0;   // of the mosaic, as decoded
    Mat3 cameraToRec2020;           // white-balanced camera RGB -> linear Rec.2020
    bool profiled = false;          // with Apple's profile for the camera
    float temperature = 0, tint = 0;    // the white balance as Apple reads it, where profiled
    Distortion distortion;          // the lens's, as corrected: None where it was not
    double noise[4] = {};           // the detail steps' noise, a Y + b: a and b at full size, then at half
};

// A decode laid out: all that comes before the pixels are worked on, on the
// CPU (the unpacking, the image area, the white balance, the colour
// transform, the lens's distortion, the size), and the image area's mosaic
// with black and white taken off and the balance applied. Its pixels are
// then worked the same way on the CPU (decodePlan) or the GPU (GpuDecoder):
// the demosaic, the highlights, the colour, the detail steps, the lens,
// turning upright and scaling.
struct DecodePlan {
    DecodedPicture result;          // all but the pixels
    DecodeOptions options;
    std::vector<float> mosaic;      // width x height
    int width = 0, height = 0;      // of the image area
    int cfaSize = 2;
    int cfa[6][6] = {};             // from the image area's corner
    float3 lightNeutral;            // the light's neutral, largest channel 1, for the highlights
    Mat3 toRec2020 = Mat3::identity();
    float gain = 1;
    int orientation = 1;            // the EXIF turn applied, 1 for none
    int turnedWidth = 0, turnedHeight = 0;      // after turning
    int outWidth = 0, outHeight = 0;            // after scaling
    double scale = 1;               // the Lanczos scale, 1 for none
};

// What the CPU and the GPU decode share: the highlights' block size, and
// their blocks' colours from the sums of their unclipped pixels' colour and
// their counts (four floats a block), those without filled from their
// neighbours (four floats a block, the last 1 where known); RawLoader's
// Lanczos weights, each output pixel's first source pixel and taps.
int highlightBlock(int w, int h);
std::vector<float> highlightColours(const std::vector<float> &sums, int bw, int bh);
void lanczosTaps(int target, double scale, double offset, std::vector<int> &first, std::vector<float> &weights,
                 int &taps);

DecodePlan planDecode(const std::string &path, const DecodeOptions &options);
DecodedPicture decodePlan(DecodePlan &&plan);   // on the CPU
DecodedPicture decodeRaw(const std::string &path, const DecodeOptions &options);
