import Foundation
import simd

/// Sony S-Log3, in S-Gamut3.Cine or S-Gamut3, what Alpha, FX and Venice
/// cameras record log in.
///
/// From Sony's *Technical Summary for S-Gamut3.Cine/S-Log3 and
/// S-Gamut3/S-Log3*, with the constants as colour-science gives them
/// (`log_encoding_SLog3`): scene reflectance straight to a 10-bit code
/// value, a straight line below reflectance 0.01125 and a base-10 log above.
/// Like N-Log, defined on the code values the camera records: scene black
/// at 95, an 18% grey at 420, a 90% white at 598, and code 1023 is 7.7 stops
/// over grey.
///
/// Sony pairs it with two gamuts, both wider than BT.2020 in places (both
/// blues lie outside the spectral locus): S-Gamut3, the wider, and
/// S-Gamut3.Cine, a little narrower, which most cameras offer first.
enum SLog3 {
    /// Normalised code value (code / 1023) to scene-linear reflectance.
    static func linear(code v: Double) -> Double {
        v >= 171.2102946929 / 1023 ? pow(10, (v * 1023 - 420) / 261.5) * (0.18 + 0.01) - 0.01
                                   : (v * 1023 - 95) * 0.01125 / (171.2102946929 - 95)
    }

    /// Scene-linear reflectance to normalised code value.
    static func code(linear x: Double) -> Double {
        x >= 0.01125 ? (420 + log10((x + 0.01) / (0.18 + 0.01)) * 261.5) / 1023
                     : (x * (171.2102946929 - 95) / 0.01125 + 95) / 1023
    }

    /// The two gamuts' chromaticities, D65, from Sony's technical summary.
    static let sGamut3 = (red: SIMD2(0.730, 0.280), green: SIMD2(0.140, 0.855), blue: SIMD2(0.100, -0.050))
    static let sGamut3Cine = (red: SIMD2(0.766, 0.275), green: SIMD2(0.225, 0.800), blue: SIMD2(0.089, -0.087))

    static let sGamut3ToBT2020 = Primaries.toBT2020(red: sGamut3.red, green: sGamut3.green, blue: sGamut3.blue)
    static let sGamut3CineToBT2020 = Primaries.toBT2020(red: sGamut3Cine.red, green: sGamut3Cine.green,
                                                        blue: sGamut3Cine.blue)
}
