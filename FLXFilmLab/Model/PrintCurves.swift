import Foundation

/// A control point of a curve: `x` in, `y` out, both 0...1.
struct CurvePoint: Codable, Equatable, Hashable {
    var x: Double
    var y: Double
}

/// Point curves on the finished print, as a scan is curved in an editor: one
/// for all three channels together, and one for each.
///
/// They act on the print as its file encodes it: the output space's own
/// transfer curve, the values the histogram shows and the file holds. They
/// come after everything else, Bloom & Glow included. The film, the paper and
/// the scan are left as they are, and the meters read the print before the
/// curves, so an auto never chases them. `rgb` acts first, then each
/// channel's own curve acts on what it gives.
struct PrintCurves: Codable, Equatable {
    enum Channel: String, CaseIterable, Identifiable {
        case rgb, red, green, blue
        var id: String { rawValue }
    }

    var rgb = PrintCurves.straight
    var red = PrintCurves.straight
    var green = PrintCurves.straight
    var blue = PrintCurves.straight

    /// The curve that changes nothing: black to black, white to white.
    static let straight = [CurvePoint(x: 0, y: 0), CurvePoint(x: 1, y: 1)]

    subscript(_ channel: Channel) -> [CurvePoint] {
        get {
            switch channel {
            case .rgb: return rgb
            case .red: return red
            case .green: return green
            case .blue: return blue
            }
        }
        set {
            switch channel {
            case .rgb: rgb = newValue
            case .red: red = newValue
            case .green: green = newValue
            case .blue: blue = newValue
            }
        }
    }

    var isIdentity: Bool { Channel.allCases.allSatisfy { Self.isStraight(self[$0]) } }

    /// True when the curve through `points` gives every value back as it was:
    /// its points on the diagonal, from black to white.
    static func isStraight(_ points: [CurvePoint]) -> Bool {
        let p = sanitized(points)
        guard let first = p.first, let last = p.last else { return true }
        return first.x == 0 && last.x == 1 && p.allSatisfy { abs($0.y - $0.x) < 1e-9 }
    }

    /// The points in order of `x`, without two at the same input, and kept
    /// to 0...1. Fewer than two make the straight curve.
    static func sanitized(_ points: [CurvePoint]) -> [CurvePoint] {
        var out: [CurvePoint] = []
        for p in points.sorted(by: { $0.x < $1.x }) {
            let q = CurvePoint(x: min(max(p.x, 0), 1), y: min(max(p.y, 0), 1))
            guard q.x.isFinite, q.y.isFinite else { continue }
            if let last = out.last, q.x - last.x < 1e-6 { out[out.count - 1] = q } else { out.append(q) }
        }
        return out.count >= 2 ? out : straight
    }

    /// The curve through a channel's points: a monotone cubic, Fritsch and
    /// Butland's (PCHIP), so it never overshoots them. A curve pulled up does
    /// not run past white between its points, and a rising curve never turns
    /// back on itself. Beyond its end points it is flat: they set where black
    /// and white fall.
    struct Spline {
        private let xs: [Double]
        private let ys: [Double]
        private let slopes: [Double]

        init(_ points: [CurvePoint]) {
            let p = PrintCurves.sanitized(points)
            let xs = p.map(\.x), ys = p.map(\.y)
            self.xs = xs
            self.ys = ys
            let n = xs.count
            let h = (0..<(n - 1)).map { xs[$0 + 1] - xs[$0] }
            let d = (0..<(n - 1)).map { (ys[$0 + 1] - ys[$0]) / h[$0] }
            var m = [Double](repeating: 0, count: n)
            if n == 2 {
                m = [d[0], d[0]]
            } else {
                // Inside: the weighted harmonic mean of the secants on either
                // side, or flat where they disagree in sign (a peak or a dip).
                for k in 1..<(n - 1) where d[k - 1] * d[k] > 0 {
                    let w1 = 2 * h[k] + h[k - 1], w2 = h[k] + 2 * h[k - 1]
                    m[k] = (w1 + w2) / (w1 / d[k - 1] + w2 / d[k])
                }
                m[0] = Self.endSlope(h[0], h[1], d[0], d[1])
                m[n - 1] = Self.endSlope(h[n - 2], h[n - 3], d[n - 2], d[n - 3])
            }
            slopes = m
        }

        /// The one-sided three-point slope at an end, held to the shape of
        /// the points (SciPy's PCHIP end condition).
        private static func endSlope(_ h0: Double, _ h1: Double, _ d0: Double, _ d1: Double) -> Double {
            let m = ((2 * h0 + h1) * d0 - h0 * d1) / (h0 + h1)
            if m * d0 <= 0 { return 0 }
            if d0 * d1 < 0, abs(m) > abs(3 * d0) { return 3 * d0 }
            return m
        }

        func callAsFunction(_ x: Double) -> Double {
            guard let x0 = xs.first, let x1 = xs.last else { return x }
            if x <= x0 { return ys[0] }
            if x >= x1 { return ys[ys.count - 1] }
            var k = 0
            while k < xs.count - 2, x >= xs[k + 1] { k += 1 }
            let h = xs[k + 1] - xs[k], t = (x - xs[k]) / h
            let t2 = t * t, t3 = t2 * t
            let y = (2 * t3 - 3 * t2 + 1) * ys[k] + (t3 - 2 * t2 + t) * h * slopes[k]
                + (-2 * t3 + 3 * t2) * ys[k + 1] + (t3 - t2) * h * slopes[k + 1]
            return min(max(y, 0), 1)
        }
    }

    /// Samples per channel in `table()`. Read with linear interpolation, the
    /// table is within a 16-bit file's step of the curves it samples.
    static let tableSize = 4096

    /// What the print's red, green and blue become: `rgb`, then each
    /// channel's own curve, sampled at `tableSize` evenly spaced values,
    /// red's samples, then green's, then blue's.
    func table() -> [Float] {
        let all = Spline(rgb)
        let own = [Spline(red), Spline(green), Spline(blue)]
        var out = [Float](repeating: 0, count: 3 * Self.tableSize)
        for i in 0..<Self.tableSize {
            let v = all(Double(i) / Double(Self.tableSize - 1))
            for c in 0..<3 { out[c * Self.tableSize + i] = Float(own[c](v)) }
        }
        return out
    }
}

/// A print's histogram: how many of its pixels hold each value an 8-bit file
/// would, per channel (`histogram_rgb`), red's 256 bins, then green's, then
/// blue's.
struct PrintHistogram: Equatable {
    /// The finished print's.
    var after: [UInt32]
    /// The print as the curves were given it, when there are curves; without
    /// them it is the finished print's.
    var before: [UInt32]?

    /// What the curves are given.
    var input: [UInt32] { before ?? after }

    /// What one channel's own curve is given: `before`, through the curve for
    /// all three, to the nearest of the 256 values.
    func input(of channel: PrintCurves.Channel, curves: PrintCurves?) -> [UInt32] {
        let given = input
        guard channel != .rgb, let curves, !PrintCurves.isStraight(curves.rgb), given.count == 768 else {
            return given
        }
        let all = PrintCurves.Spline(curves.rgb)
        let to = (0..<256).map { Int((all(Double($0) / 255) * 255).rounded()) }
        var out = [UInt32](repeating: 0, count: 768)
        for c in 0..<3 {
            for i in 0..<256 { out[c * 256 + to[i]] += given[c * 256 + i] }
        }
        return out
    }
}
