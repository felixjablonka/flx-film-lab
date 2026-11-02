#pragma once

#include "Mat3.h"

#include <optional>
#include <string>

// A light as Core Image takes a white balance: its chromaticity, and the
// temperature and tint (Core Image's scale) Apple's colour for it is that of.
struct Light {
    float2 xy;
    float temperature = 0, tint = 0;
};

// A light RawLoader sets by its chromaticity (RawLoader.referenceChromaticity):
// the CIE daylights, and the Planckian 3200 K, where agx and the CIE put it.
// CIE A and the printing lamps it sets by temperature. With each, the
// temperature and tint Core Image reads it as: CIRAWFilter's
// neutralTemperature and neutralTint once neutralChromaticity is set, the
// same for every camera (measured on macOS 27). Robertson's isotemperature
// lines, the DNG SDK's, read the daylights the same to the hundredth, but the
// Planckian 3200 K as 3199.8 K, tint +0.2: Core Image's own 3200 K at tint 0
// sits 14 K off it.
inline std::optional<Light> chromaticityLight(const std::string &light)
{
    if (light == "D50")
        return Light{{0.34567f, 0.35850f}, 5001.80f, 9.601f};
    if (light == "D55")
        return Light{{0.33242f, 0.34743f}, 5501.50f, 10.011f};
    if (light == "D65")
        return Light{{0.31271f, 0.32902f}, 6503.01f, 9.786f};
    if (light == "BB3200")
        return Light{{0.42336f, 0.39895f}, 3186.04f, -0.403f};
    return std::nullopt;
}
