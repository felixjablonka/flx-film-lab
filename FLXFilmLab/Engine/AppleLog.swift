import Foundation
import simd

/// Apple Log, the log curve iPhone 15 Pro and later record in.
///
/// From Apple's *Apple Log Profile White Paper* (2023), with the constants as
/// colour-science (`log_encoding_AppleLogProfile`) and the ACES input
/// transform `IDT.Apple.AppleLog_BT2020` give them: scene reflectance R
/// (0.18 is an 18% grey card) to an encoded value P, a square law from R0 up
/// to Rt and a base-2 log above.
///
/// Unlike N-Log, which Nikon defines on 10-bit code values, P is the signal
/// itself, normalised to 0...1 — what an editor hands a LUT once it has
/// scaled the file's own range. An 18% grey sits at P = 0.488, scene black at
/// 0.150, and P = 1 is reflectance 12.0, 6.1 stops over grey.
///
/// Apple Log's primaries are BT.2020, the engine's working space, so decoding
/// to linear is again the whole input transform.
///
/// Apple Log 2, which iPhone 17 Pro and later can record instead (Settings >
/// Camera > Formats > ProRes Log Video Encoding), keeps this curve but records
/// in Apple Wide Gamut, wider than BT.2020, so its decode adds a matrix.
enum AppleLog {
    static let r0 = -0.05641088
    static let rt = 0.01
    static let sigma = 47.28711236
    static let beta = 0.00964052
    static let gamma = 0.08550479
    static let delta = 0.69336945
    /// Where the square law meets the log, as an encoded value.
    static let pt = sigma * (rt - r0) * (rt - r0)

    /// Encoded value P to scene-linear reflectance.
    static func linear(signal p: Double) -> Double {
        if p >= pt { return pow(2, (p - delta) / gamma) - beta }
        if p >= 0 { return sqrt(p / sigma) + r0 }
        return r0
    }

    /// Scene-linear reflectance to encoded value P.
    static func signal(linear r: Double) -> Double {
        if r >= rt { return gamma * log2(r + beta) + delta }
        if r >= r0 { return sigma * (r - r0) * (r - r0) }
        return 0
    }

    /// Apple Wide Gamut's chromaticities, from the *Apple Log 2 Profile White
    /// Paper* (2025) as OpenColorIO's built-in `APPLE_LOG2` transform gives
    /// them. The blue primary lies outside the spectral locus, like V-Gamut's.
    /// White is D65.
    static let wideGamutRed = SIMD2(0.725, 0.301)
    static let wideGamutGreen = SIMD2(0.221, 0.814)
    static let wideGamutBlue = SIMD2(0.068, -0.076)
    static let d65 = Primaries.d65

    /// Linear Apple Wide Gamut to CIE XYZ (white Y = 1). Apple publishes the
    /// primaries but no matrix, so it is derived from them.
    static let wideGamutToXYZ = Primaries.rgbToXYZ(red: wideGamutRed, green: wideGamutGreen, blue: wideGamutBlue)

    /// Linear Apple Wide Gamut to linear BT.2020, both derived the same way
    /// from their primaries. Both are D65, so no adaptation.
    static let wideGamutToBT2020 = Primaries.toBT2020(red: wideGamutRed, green: wideGamutGreen, blue: wideGamutBlue)
}
