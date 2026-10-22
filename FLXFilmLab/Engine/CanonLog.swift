import Foundation
import simd

/// Canon Log 2 and Canon Log 3, what Cinema EOS and EOS R cameras record
/// log in.
///
/// From Canon's input transforms for the EOS C300 Mark II (version 202007,
/// which carry the curves' v1.2), with the constants as colour-science gives
/// them (`log_encoding_CanonLog2_v1_2`, `_CanonLog3_v1_2`). The v1.2 curves
/// take linear light scaled so that a 90% white is 1, reflectance / 0.9, to a
/// normalised 10-bit code value: defined, like N-Log, on the code values the
/// camera records. An 18% grey records at code 407 in Canon Log 2 and 351 in
/// Canon Log 3, scene black at 95 and 128; code 1023 is 8.4 and 6.4 stops over
/// grey. Both continue below black, mirrored, so noise in the blacks decodes
/// to small negative light.
///
/// Both default to Cinema Gamut, wider than BT.2020 (its green and blue lie
/// outside the spectral locus), so decoding takes a matrix into the engine's
/// BT.2020 as well. A Cinema EOS camera can record Canon Log 3 in BT.2020 or
/// BT.709 instead; footage shot that way is not this.
enum CanonLog {
    /// Normalised code value (code / 1023) to scene-linear reflectance.
    static func linear2(code v: Double) -> Double {
        let x = v < 0.092864125 ? -(pow(10, (0.092864125 - v) / 0.24136077) - 1) / 87.09937546
                                : (pow(10, (v - 0.092864125) / 0.24136077) - 1) / 87.09937546
        return x * 0.9
    }

    /// Scene-linear reflectance to normalised code value.
    static func code2(linear r: Double) -> Double {
        let x = r / 0.9
        return x < 0 ? -(0.24136077 * log10(-x * 87.09937546 + 1)) + 0.092864125
                     : 0.24136077 * log10(x * 87.09937546 + 1) + 0.092864125
    }

    static func linear3(code v: Double) -> Double {
        let x: Double
        if v < 0.097465473 {
            x = -(pow(10, (0.12783901 - v) / 0.36726845) - 1) / 14.98325
        } else if v <= 0.15277891 {
            x = (v - 0.12512219) / 1.9754798
        } else {
            x = (pow(10, (v - 0.12240537) / 0.36726845) - 1) / 14.98325
        }
        return x * 0.9
    }

    /// Where Canon Log 3's straight middle segment starts and ends, on its
    /// own linear scale (reflectance / 0.9).
    static let cut3 = (low: linear3(code: 0.097465473) / 0.9, high: linear3(code: 0.15277891) / 0.9)

    static func code3(linear r: Double) -> Double {
        let x = r / 0.9
        if x < cut3.low { return -0.36726845 * log10(-x * 14.98325 + 1) + 0.12783901 }
        if x <= cut3.high { return 1.9754798 * x + 0.12512219 }
        return 0.36726845 * log10(x * 14.98325 + 1) + 0.12240537
    }

    /// Cinema Gamut's chromaticities, D65, as colour-science gives them from
    /// Canon's EOS C500 firmware update (2014).
    static let cinemaGamut = (red: SIMD2(0.74, 0.27), green: SIMD2(0.17, 1.14), blue: SIMD2(0.08, -0.10))

    /// Linear Cinema Gamut to linear BT.2020.
    static let cinemaGamutToBT2020 = Primaries.toBT2020(red: cinemaGamut.red, green: cinemaGamut.green,
                                                        blue: cinemaGamut.blue)
}
