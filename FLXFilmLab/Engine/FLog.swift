import Foundation

/// Fujifilm F-Log and F-Log2, what Fujifilm X and GFX cameras record log in.
///
/// From Fujifilm's *F-Log Data Sheet* and *F-Log2 Data Sheet*, with the
/// constants as colour-science gives them (`log_encoding_FLog`, `_FLog2`):
/// scene reflectance (0.18 is an 18% grey card) to a normalised 10-bit code
/// value, a straight line below reflectance 0.00089 and a base-10 log above.
/// Like N-Log, both are defined on the code values the camera records,
/// footroom included. Scene black records at code 95 in either; an 18% grey
/// at 470 in F-Log and at 400 in F-Log2, which spends its codes on more
/// stops: code 1023 is 5.3 stops over grey in F-Log and 8.3 in F-Log2.
///
/// Both record in F-Gamut, which is BT.2020, the engine's working space, so
/// decoding to linear is the whole input transform.
enum FLog {
    struct Curve {
        let cut1, cut2, a, b, c, d, e, f: Double

        /// Normalised code value (code / 1023) to scene-linear reflectance.
        func linear(code v: Double) -> Double {
            v < cut2 ? (v - f) / e : pow(10, (v - d) / c) / a - b / a
        }

        /// Scene-linear reflectance to normalised code value.
        func code(linear x: Double) -> Double {
            x < cut1 ? e * x + f : c * log10(a * x + b) + d
        }
    }

    static let flog = Curve(cut1: 0.00089, cut2: 0.100537775223865, a: 0.555556, b: 0.009468,
                            c: 0.344676, d: 0.790453, e: 8.735631, f: 0.092864)
    static let flog2 = Curve(cut1: 0.000889, cut2: 0.100686685370811, a: 5.555556, b: 0.064829,
                             c: 0.245281, d: 0.384316, e: 8.799461, f: 0.092864)
}
