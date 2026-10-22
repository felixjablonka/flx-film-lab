import Foundation

/// Nikon N-Log, the log curve Nikon Z cameras record video in.
///
/// From Nikon's N-Log Specification Document 1.0.0 (2018), as colour-science
/// implements it (`log_encoding_NLog`): scene reflectance (0.18 is an 18% grey
/// card) to a 10-bit code value, a cube root up to reflectance 0.328 (code
/// 452) and a natural log above it.
///
/// The curve is defined on 10-bit code values. Nikon's N-Log primer puts an
/// 18% grey at "a 10-bit code value of around 372" (35% on a video-range
/// waveform). Scene black lands on code 127; the top of the code range, 1023,
/// is reflectance 14.8, 6.4 stops over grey. A Zf writes those code values
/// as they are into full-range 10-bit HEVC (tagged BT.709, firmware 2.00),
/// so in its files code 372 is still grey and the brightest highlights reach
/// 1023.
///
/// Nikon lists N-Log's colour space as BT.2020, which is the engine's own
/// working space, so decoding to linear is the whole input transform.
enum NLog {
    static let a = 650.0 / 1023
    static let b = 0.0075
    static let c = 150.0 / 1023
    static let d = 619.0 / 1023
    /// Where the two halves meet, as reflectance and as normalised code value.
    static let cutLinear = 0.328
    static let cutCode = 452.0 / 1023

    /// Normalised code value (code / 1023) to scene-linear reflectance.
    static func linear(code v: Double) -> Double {
        v < cutCode ? pow(v / a, 3) - b : exp((v - d) / c)
    }

    /// Scene-linear reflectance to normalised code value.
    static func code(linear y: Double) -> Double {
        y < cutLinear ? a * cbrt(y + b) : c * log(y) + d
    }

    /// The 10-bit code value an 18% grey card records at.
    static var greyCode: Double { code(linear: 0.18) * 1023 }
}
