#pragma once

#include "Mat3.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// A raw file unpacked: the sensor's mosaic and what the decode needs to know
// about it. The decoder that fills it is the only part that depends on which
// library read the file; everything after (RawDecoder) is the port's own.
// RawSpeed first, LibRaw for what it does not read (CR3 above all).
struct RawMosaic {
    std::string decoder;            // "rawspeed" or "libraw"
    std::string make, model;
    int width = 0, height = 0;      // the mosaic, as unpacked
    std::vector<uint16_t> data;     // width x height raw values
    // The colour filter pattern from the mosaic's origin, 0 R 1 G 2 B,
    // [row][column]: Bayer's 2x2 or X-Trans's 6x6, cfaSize on a side.
    int cfaSize = 2;
    int cfa[6][6] = {{0, 1}, {1, 2}};
    // Black, per position of the 2x2 pattern, and white, in raw values.
    float black[2][2] = {};
    float white = 0;
    // The image area inside the mosaic: the decoder's own crop.
    int cropLeft = 0, cropTop = 0, cropWidth = 0, cropHeight = 0;
    // XYZ (D65) -> camera, the Adobe ColorMatrix for daylight.
    Mat3 xyzToCamera = Mat3::identity();
    float3 asShotNeutral{1, 1, 1};  // the camera's white balance, as camera RGB of a neutral
    int orientation = 1;            // EXIF
    // The lens, as the file names it, and the image's crop factor where the
    // file gives a 35 mm equivalent focal length (0 where not): for the lens
    // correction (LensCorrection)
    std::string lensMaker, lensModel;
    float focalLength = 0, cropFactor = 0;
    // What the exposure Apple gives a file can depend on, as LibRaw reads the
    // metadata: NAN or -1 where the file has none
    struct Exposure {
        float iso = 0;
        float fujiRawExposureShift = NAN;   // Fujifilm's RawExposureBias (ExpoMidPointShift), EV
        int fujiDynamicRange = -1;          // its development dynamic range: 100, 200, 400
        int canonHighlightTonePriority = -1;
        float dngBaselineExposure = NAN;
    } exposure;
    // What the file says about the photograph, for an export to carry, as
    // LibRaw reads it: zero or empty where the file has none
    struct Photo {
        float shutter = 0, aperture = 0;    // seconds, f-number
        int focalLength35 = 0;              // mm
        // EXIF's LensInfo: the lens's focal range and its widest apertures there
        float minFocal = 0, maxFocal = 0, maxApertureAtMin = 0, maxApertureAtMax = 0;
        long long taken = 0;                // when, seconds since 1970 as LibRaw reads the date (local time)
        std::string artist, description;
        bool gps = false;                   // a position read
        float latitude[3] = {}, longitude[3] = {}, altitude = 0;   // degrees, minutes, seconds; m
        char latitudeRef = 0, longitudeRef = 0, altitudeRef = 0, gpsStatus = 0;
    } photo;
};

// Each throws std::runtime_error naming the file and the reason. RawSpeed
// needs its camera list, data/cameras.xml, and refuses cameras not in it.
RawMosaic unpackWithRawSpeed(const std::string &path, const std::string &camerasXml);
RawMosaic unpackWithLibRaw(const std::string &path);
// The metadata RawSpeed reads none of (orientation, the exposure's, the lens), from
// LibRaw's parse of the file alone; left as they are where it cannot read it.
void readMetadataWithLibRaw(const std::string &path, RawMosaic &m);
