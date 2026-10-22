import Foundation
import simd

/// Camera gamuts by their published primaries, into the engine's BT.2020.
///
/// Apple, Canon and Sony publish the chromaticities of the primaries their
/// log footage records in; the matrices follow from them. Every gamut here is
/// D65, as BT.2020 is, so none needs a chromatic adaptation.
enum Primaries {
    static let d65 = SIMD2(0.3127, 0.3290)
    static let bt2020 = (red: SIMD2(0.708, 0.292), green: SIMD2(0.170, 0.797), blue: SIMD2(0.131, 0.046))

    /// RGB to CIE XYZ for the primaries and white given as xy, scaled so that
    /// RGB (1, 1, 1) is the white at Y = 1.
    static func rgbToXYZ(red: SIMD2<Double>, green: SIMD2<Double>,
                         blue: SIMD2<Double>, white: SIMD2<Double> = d65) -> simd_double3x3 {
        func xyz(_ c: SIMD2<Double>) -> SIMD3<Double> { SIMD3(c.x / c.y, 1, (1 - c.x - c.y) / c.y) }
        let primaries = simd_double3x3(columns: (xyz(red), xyz(green), xyz(blue)))
        let s = primaries.inverse * xyz(white)
        return simd_double3x3(columns: (primaries.columns.0 * s.x, primaries.columns.1 * s.y,
                                        primaries.columns.2 * s.z))
    }

    static let bt2020ToXYZ = rgbToXYZ(red: bt2020.red, green: bt2020.green, blue: bt2020.blue)

    /// Linear RGB in a D65 gamut with these primaries to linear BT.2020.
    static func toBT2020(red: SIMD2<Double>, green: SIMD2<Double>, blue: SIMD2<Double>) -> simd_double3x3 {
        bt2020ToXYZ.inverse * rgbToXYZ(red: red, green: green, blue: blue)
    }
}
