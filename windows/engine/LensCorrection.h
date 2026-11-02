#pragma once

#include "Mat3.h"

#include <string>
#include <vector>

// Lens distortion, corrected from what the port can read, as open-source
// raw developers do: the camera's own correction data where its form is
// known, else the Lensfun database (third_party/lensfun/data/db, CC BY-SA
// 3.0), whose models the port evaluates itself. Only the distortion, as
// Apple's decoder in the Mac app corrects it: no vignetting, no lateral
// chromatic aberration (but where a DNG's warp has one per colour).
//
// The camera's own data: Nikon's, Sony's, Fujifilm's, Panasonic's and
// Olympus's (OM System's), each read as checked against Apple's decoder on
// public samples, and DNG's WarpRectilinear opcode as the DNG specification
// has it.

struct LensInfo {
    std::string cameraMaker, cameraModel;
    std::string lensMaker, lensModel;   // as the file names the lens
    float focalLength = 0;              // mm
    float cropFactor = 0;               // the image's, where the file gives it (0: the camera's, from Lensfun)
    int sensorWidth = 0, sensorHeight = 0;  // the mosaic as read out, in pixels
    int cropLeft = 0, cropTop = 0;      // the image area's corner in the mosaic
};

struct Distortion {
    // the output radius -> the source radius: r_s = r * scale(r)
    enum Model { None, Nikon, Poly3, Poly5, PTLens, Profile, DngWarp } model = None;
    float k[4] = {};                    // Nikon: c0 k2 k3 k4; poly3: k1; poly5: k1 k2; ptlens: a b c
    float radius = 1;                   // the models' radius 1, in pixels of the image
    // The model scaled up by: to fill the frame where it has no scale of its
    // own (Lensfun's, Fujifilm's), else as far as keeps the frame's edge
    // inside the picture
    float zoom = 1;
    // Profile: scale() at radii 0, step, 2 step... of `radius`, linear in
    // between, the last beyond
    std::vector<float> profile;
    float profileStep = 1;
    // DngWarp: kr0-kr3, kt0 and kt1 of each plane (one for all, or red,
    // green and blue), the optical centre in the image's pixels; radius is
    // the specification's m
    struct Plane { double kr[4], kt[2]; };
    std::vector<Plane> planes;
    double cx = 0, cy = 0;
    std::string from;                   // "Nikon's data", "Lensfun: <lens>"...
    float scale(float r) const;         // r in pixels; not for DngWarp
};

// The distortion of the lens a raw file was taken with, for its image area of
// w x h pixels in the sensor's frame; none where nothing describes it.
// cameraData false skips the camera's own data, for Lensfun's alone.
Distortion distortionFor(const std::string &rawPath, const LensInfo &lens, int w, int h,
                         const std::string &lensfunDir, bool cameraData = true);

// Each output pixel from the source position the distortion gives, bicubic,
// clamped at the edges.
void correctDistortion(std::vector<float3> &rgb, int w, int h, const Distortion &d);
