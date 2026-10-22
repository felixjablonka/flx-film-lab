import Foundation
import simd
@testable import FLXCore

// Colour maths shared by the LUT and video checks.

func lab(linear c: SIMD3<Double>) -> SIMD3<Double> {
    let r = max(c.x, 0), g = max(c.y, 0), b = max(c.z, 0)
    let x = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b
    let y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b
    let z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b
    func f(_ t: Double) -> Double { t > 0.008856 ? cbrt(t) : (7.787 * t + 16.0 / 116.0) }
    let fx = f(x / 0.95047), fy = f(y), fz = f(z / 1.08883)
    return SIMD3(116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz))
}

func lab(_ encoded: SIMD3<Float>, _ output: LookLUT.Output) -> SIMD3<Double> {
    lab(linear: SIMD3(output.decode(Double(encoded.x)),
                      output.decode(Double(encoded.y)),
                      output.decode(Double(encoded.z))))
}

/// CIEDE2000 (Sharma, Wu & Dalal 2005).
func deltaE2000(_ p: SIMD3<Double>, _ q: SIMD3<Double>) -> Double {
    let c1 = hypot(p.y, p.z), c2 = hypot(q.y, q.z)
    let cBar7 = pow((c1 + c2) / 2, 7)
    let g = 0.5 * (1 - sqrt(cBar7 / (cBar7 + pow(25.0, 7))))
    let a1 = (1 + g) * p.y, a2 = (1 + g) * q.y
    let c1p = hypot(a1, p.z), c2p = hypot(a2, q.z)
    func hue(_ a: Double, _ b: Double) -> Double {
        if a == 0 && b == 0 { return 0 }
        let h = atan2(b, a) * 180 / .pi
        return h >= 0 ? h : h + 360
    }
    let h1 = hue(a1, p.z), h2 = hue(a2, q.z)
    var dh = 0.0
    if c1p * c2p != 0 {
        dh = h2 - h1
        if dh > 180 { dh -= 360 } else if dh < -180 { dh += 360 }
    }
    let dL = q.x - p.x, dC = c2p - c1p
    let dH = 2 * sqrt(c1p * c2p) * sin(dh * .pi / 360)
    let lBar = (p.x + q.x) / 2, cBarP = (c1p + c2p) / 2
    var hBar = h1 + h2
    if c1p * c2p != 0 {
        if abs(h1 - h2) <= 180 { hBar = (h1 + h2) / 2 }
        else if h1 + h2 < 360 { hBar = (h1 + h2 + 360) / 2 }
        else { hBar = (h1 + h2 - 360) / 2 }
    }
    let rad = Double.pi / 180
    let t = 1 - 0.17 * cos((hBar - 30) * rad) + 0.24 * cos(2 * hBar * rad)
        + 0.32 * cos((3 * hBar + 6) * rad) - 0.20 * cos((4 * hBar - 63) * rad)
    let dTheta = 30 * exp(-pow((hBar - 275) / 25, 2))
    let cBarP7 = pow(cBarP, 7)
    let rc = 2 * sqrt(cBarP7 / (cBarP7 + pow(25.0, 7)))
    let sl = 1 + 0.015 * pow(lBar - 50, 2) / sqrt(20 + pow(lBar - 50, 2))
    let sc = 1 + 0.045 * cBarP
    let sh = 1 + 0.015 * cBarP * t
    let rt = -sin(2 * dTheta * rad) * rc
    return sqrt(pow(dL / sl, 2) + pow(dC / sc, 2) + pow(dH / sh, 2) + rt * (dC / sc) * (dH / sh))
}

struct Errors {
    var values: [Double]
    var mean: Double { values.reduce(0, +) / Double(Swift.max(values.count, 1)) }
    func percentile(_ p: Double) -> Double {
        let s = values.sorted()
        return s.isEmpty ? 0 : s[min(s.count - 1, Int(Double(s.count - 1) * p))]
    }
    var largest: Double { values.max() ?? 0 }
    var summary: String { String(format: "mean %.3f  p99 %.3f  max %.3f", mean, percentile(0.99), largest) }
}

/// Deterministic, so a failure reproduces.
struct SplitMix {
    var state: UInt64
    mutating func next() -> Double {
        state &+= 0x9E37_79B9_7F4A_7C15
        var z = state
        z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
        z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
        return Double((z ^ (z >> 31)) >> 11) / Double(1 << 53)
    }
}

/// Linear Rec.709 to linear Rec.2020, for building scene colours from familiar ones.
func rec709To2020(_ c: SIMD3<Double>) -> SIMD3<Double> {
    SIMD3(0.627404 * c.x + 0.329283 * c.y + 0.043313 * c.z,
          0.069097 * c.x + 0.919540 * c.y + 0.011362 * c.z,
          0.016391 * c.x + 0.088013 * c.y + 0.895595 * c.z)
}
