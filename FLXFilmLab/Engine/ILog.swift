import Foundation

/// Insta360 I-Log, what Insta360 cameras such as the Luna Ultra record log in.
///
/// From Insta360's *10-bit I-Log White Paper* (June 2026): scene reflectance
/// (0.18 is an 18% grey card) to a normalised 10-bit code value, a straight
/// line up to reflectance 0.01104854 and a base-10 log above. Like N-Log it is
/// defined on the code values the camera records: scene black at code 93, an
/// 18% grey at 432, and code 1023 at 2200% reflectance, 6.9 stops over grey.
///
/// I-Log records in BT.2020, the engine's working space, so decoding to
/// linear is the whole input transform. Insta360 tags its clips BT.709 all
/// the same, so the Motion mode knows them by the camera's make.
enum ILog {
    static let alpha = 5.77837328, beta = 0.09055934
    static let delta = 0.623992, eta = 0.280055, theta = 0.01
    /// Where the line meets the log: in reflectance, and as a code value.
    static let cutLinear = 0.01104854, cutCode = 0.154402

    /// Normalised code value (code / 1023) to scene-linear reflectance.
    static func linear(code v: Double) -> Double {
        v < cutCode ? (v - beta) / alpha : pow(10, (v - delta) / eta) - theta
    }

    /// Scene-linear reflectance to normalised code value.
    static func code(linear x: Double) -> Double {
        x <= cutLinear ? alpha * x + beta : delta + eta * log10(x + theta)
    }
}
