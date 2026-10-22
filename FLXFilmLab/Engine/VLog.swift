import Foundation
import simd

/// Panasonic V-Log and V-Gamut, what Lumix cameras record V-Log in.
///
/// From Panasonic's *V-Log/V-Gamut Reference Manual* (2014), with the
/// constants as colour-science gives them (`log_encoding_VLog`): scene
/// reflectance (0.18 is an 18% grey card) to a normalised 10-bit code value,
/// a straight line below reflectance 0.01 and a base-10 log above. Like
/// N-Log, the curve is defined on code values: an 18% grey records at code
/// 433 (42% IRE), scene black at 128. The top of the code range is reflectance
/// 46, eight stops over grey, well past where a Lumix sensor clips.
///
/// Unlike N-Log and Apple Log, V-Log does not use BT.2020: its primaries are
/// V-Gamut, wider still (the blue primary lies outside the spectral locus).
/// Decoding takes a matrix into the engine's BT.2020 working space as well. A
/// colour outside BT.2020 comes out with a negative component, which the film
/// exposure takes as it is: the film sees the same XYZ either way.
enum VLog {
    static let cut1 = 0.01
    static let cut2 = 0.181
    static let b = 0.00873
    static let c = 0.241514
    static let d = 0.598206

    /// Normalised code value (code / 1023) to scene-linear reflectance.
    static func linear(code v: Double) -> Double {
        v < cut2 ? (v - 0.125) / 5.6 : pow(10, (v - d) / c) - b
    }

    /// Scene-linear reflectance to normalised code value.
    static func code(linear x: Double) -> Double {
        x < cut1 ? 5.6 * x + 0.125 : c * log10(x + b) + d
    }

    /// V-Gamut to CIE XYZ, D65: Panasonic's reference matrix.
    static let toXYZ = simd_double3x3(rows: [
        SIMD3(0.679644, 0.152211, 0.118600),
        SIMD3(0.260686, 0.774894, -0.035580),
        SIMD3(-0.009310, -0.004612, 1.102980),
    ])

    /// CIE XYZ to BT.2020, D65 (ITU-R BT.2020's primaries).
    static let xyzToBT2020 = simd_double3x3(rows: [
        SIMD3(1.7166511880, -0.3556707838, -0.2533662814),
        SIMD3(-0.6666843518, 1.6164812366, 0.0157685458),
        SIMD3(0.0176398574, -0.0427706133, 0.9421031212),
    ])

    /// Linear V-Gamut to linear BT.2020; both are D65, so no adaptation.
    static let toBT2020 = xyzToBT2020 * toXYZ
}
