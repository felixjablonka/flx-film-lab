import Foundation
import CoreGraphics

/// Rasterises dodge/burn masks into the two-plane map the print kernel reads:
/// plane 0 is summed print exposure in stops, plane 1 is summed local grade in
/// ISO-R points.
///
/// Following NegPy: a mask changes *print exposure*, so it rides the whole
/// characteristic curve — a burn rolls into paper black the way a real one
/// does, instead of clipping like a brightness overlay would. Local grade is
/// carried separately because burning through a harder filter is a different
/// operation from burning longer.
///
/// The map is deliberately low-resolution. Feathered masks are smooth by
/// construction, so a 1024-pixel map sampled bilinearly is indistinguishable
/// from a full-resolution one and costs a fraction of the time — which matters
/// because this runs on the CPU while a slider is moving.
enum MaskRasterizer {

    static let mapMaxDimension = 1024

    /// The rasterised map together with the size it was actually rasterised at.
    ///
    /// These travel as one value on purpose. The map is deliberately smaller
    /// than the frame, and an earlier version returned a bare array that every
    /// caller then uploaded using the *image* dimensions — a buffer overrun on
    /// any frame wider than the cap, which crashed the app the moment a mask
    /// was added.
    struct Map {
        var data: [Float]     // interleaved RG pairs: stops, ISO-R points
        var width: Int
        var height: Int
    }

    static func mapSize(width: Int, height: Int) -> (width: Int, height: Int) {
        let longEdge = max(width, height)
        guard longEdge > mapMaxDimension else { return (max(width, 1), max(height, 1)) }
        let scale = Double(mapMaxDimension) / Double(longEdge)
        return (max(Int(Double(width) * scale), 1), max(Int(Double(height) * scale), 1))
    }

    static func rasterize(masks: [LocalMask], width: Int, height: Int) -> Map {
        let size = mapSize(width: width, height: height)
        let coverages = masks.filter(isActive).map { ($0, coverage(of: $0, mapWidth: size.width, mapHeight: size.height)) }
        return combine(coverages, mapWidth: size.width, mapHeight: size.height)
    }

    /// Whether a mask contributes anything at all.
    static func isActive(_ mask: LocalMask) -> Bool {
        mask.enabled && (mask.stops != 0 || mask.grade != 0)
    }

    /// What decides a mask's coverage — everything except how strongly it acts.
    /// Two masks with equal geometry cover exactly the same pixels, so the app
    /// keeps each coverage while only a mask's stops or grade move.
    struct Geometry: Equatable {
        var shape: MaskShape
        var points: [CGPoint]
        var feather: Double
        var inverted: Bool

        init(_ mask: LocalMask) {
            shape = mask.shape
            points = mask.points
            feather = mask.feather
            inverted = mask.inverted
        }
    }

    /// One mask's coverage, 0...1, at map resolution: shape, feather, invert.
    static func coverage(of mask: LocalMask, mapWidth w: Int, mapHeight h: Int) -> [Float] {
        var alpha = [Float](repeating: 0, count: w * h)
        fill(&alpha, mask: mask, width: w, height: h)
        if mask.shape != .gradient && mask.feather > 0 {
            // The gradient mask's handle spacing *is* its softness, so it
            // is the one shape feather does not apply to.
            let sigma = Float(mask.feather) * Float(min(w, h))
            boxBlur(&alpha, width: w, height: h, sigma: sigma)
        }
        if mask.inverted {
            for i in 0..<alpha.count { alpha[i] = 1 - alpha[i] }
        }
        return alpha
    }

    /// Sum coverages into the two-plane map, each weighted by its own stops
    /// and grade.
    static func combine(_ coverages: [(mask: LocalMask, coverage: [Float])],
                        mapWidth w: Int, mapHeight h: Int) -> Map {
        var out = [Float](repeating: 0, count: w * h * 2)
        out.withUnsafeMutableBufferPointer { o in
            for (mask, alpha) in coverages where alpha.count == w * h {
                let stops = Float(mask.stops)
                let grade = Float(mask.grade)
                alpha.withUnsafeBufferPointer { a in
                    for i in 0..<(w * h) {
                        o[i * 2 + 0] += a[i] * stops
                        o[i * 2 + 1] += a[i] * grade
                    }
                }
            }
        }
        return Map(data: out, width: w, height: h)
    }

    // MARK: - Shapes

    private static func fill(_ alpha: inout [Float], mask: LocalMask, width w: Int, height h: Int) {
        for i in 0..<alpha.count { alpha[i] = 0 }
        let points = mask.points.map { CGPoint(x: $0.x * CGFloat(w), y: $0.y * CGFloat(h)) }
        switch mask.shape {
        case .polygon:
            guard points.count >= 3 else { return }
            fillPolygon(&alpha, outline: smoothClosedPolyline(points), width: w, height: h)
        case .oval:
            guard points.count >= 3 else { return }
            fillPolygon(&alpha, outline: ovalOutline(points), width: w, height: h)
        case .gradient:
            guard points.count >= 2 else { return }
            fillGradient(&alpha, from: points[0], to: points[1], width: w, height: h)
        }
    }

    /// Catmull-Rom through the control points. The outline bows out past its own
    /// vertices by design — a cut card has no sharp corners either.
    private static func smoothClosedPolyline(_ points: [CGPoint], samplesPerSegment: Int = 12) -> [CGPoint] {
        let n = points.count
        guard n >= 3 else { return points }
        var out: [CGPoint] = []
        out.reserveCapacity(n * samplesPerSegment)
        for i in 0..<n {
            let p0 = points[(i - 1 + n) % n]
            let p1 = points[i]
            let p2 = points[(i + 1) % n]
            let p3 = points[(i + 2) % n]
            for s in 0..<samplesPerSegment {
                let t = CGFloat(s) / CGFloat(samplesPerSegment)
                let t2 = t * t, t3 = t2 * t
                let x = 0.5 * ((2 * p1.x) + (-p0.x + p2.x) * t
                        + (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2
                        + (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * t3)
                let y = 0.5 * ((2 * p1.y) + (-p0.y + p2.y) * t
                        + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2
                        + (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3)
                out.append(CGPoint(x: x, y: y))
            }
        }
        return out
    }

    /// Three control points (centre, and the ends of two axes) define an affine
    /// frame, so a tilted or sheared oval needs no extra parameters.
    private static func ovalOutline(_ points: [CGPoint], samples: Int = 64) -> [CGPoint] {
        let c = points[0]
        let u = CGPoint(x: points[1].x - c.x, y: points[1].y - c.y)
        let v = CGPoint(x: points[2].x - c.x, y: points[2].y - c.y)
        return (0..<samples).map { i in
            let theta = 2 * CGFloat.pi * CGFloat(i) / CGFloat(samples)
            return CGPoint(x: c.x + u.x * cos(theta) + v.x * sin(theta),
                           y: c.y + u.y * cos(theta) + v.y * sin(theta))
        }
    }

    /// The graduated card edge: full effect at and behind `a`, none at and past `b`.
    private static func fillGradient(_ alpha: inout [Float], from a: CGPoint, to b: CGPoint,
                                     width w: Int, height h: Int) {
        let dx = b.x - a.x, dy = b.y - a.y
        let lengthSquared = dx * dx + dy * dy
        guard lengthSquared > 1e-9 else { return }
        for y in 0..<h {
            for x in 0..<w {
                let px = CGFloat(x) + 0.5 - a.x
                let py = CGFloat(y) + 0.5 - a.y
                var t = (px * dx + py * dy) / lengthSquared
                t = min(max(t, 0), 1)
                let smooth = t * t * (3 - 2 * t)
                alpha[y * w + x] = Float(1 - smooth)
            }
        }
    }

    /// Even-odd scanline fill.
    private static func fillPolygon(_ alpha: inout [Float], outline: [CGPoint], width w: Int, height h: Int) {
        let n = outline.count
        guard n >= 3 else { return }
        var crossings: [CGFloat] = []
        crossings.reserveCapacity(16)
        for y in 0..<h {
            let scan = CGFloat(y) + 0.5
            crossings.removeAll(keepingCapacity: true)
            var j = n - 1
            for i in 0..<n {
                let a = outline[j], b = outline[i]
                if (a.y > scan) != (b.y > scan) {
                    let t = (scan - a.y) / (b.y - a.y)
                    crossings.append(a.x + t * (b.x - a.x))
                }
                j = i
            }
            guard crossings.count >= 2 else { continue }
            crossings.sort()
            var k = 0
            while k + 1 < crossings.count {
                let x0 = max(Int(crossings[k].rounded()), 0)
                let x1 = min(Int(crossings[k + 1].rounded()), w)
                if x1 > x0 {
                    for x in x0..<x1 { alpha[y * w + x] = 1 }
                }
                k += 2
            }
        }
    }

    /// Three box passes approximate a Gaussian closely enough for a feather and
    /// run in time independent of the radius.
    private static func boxBlur(_ data: inout [Float], width w: Int, height h: Int, sigma: Float) {
        guard sigma > 0.5 else { return }
        let radius = max(Int((sigma * 1.2).rounded()), 1)
        var scratch = [Float](repeating: 0, count: data.count)
        data.withUnsafeMutableBufferPointer { d in
            scratch.withUnsafeMutableBufferPointer { s in
                for _ in 0..<3 {
                    boxPassHorizontal(d, into: s, width: w, height: h, radius: radius)
                    boxPassVertical(s, into: d, width: w, height: h, radius: radius)
                }
            }
        }
    }

    private static func boxPassHorizontal(_ src: UnsafeMutableBufferPointer<Float>,
                                          into dst: UnsafeMutableBufferPointer<Float>,
                                          width w: Int, height h: Int, radius: Int) {
        let norm = 1 / Float(2 * radius + 1)
        for y in 0..<h {
            let row = y * w
            var sum: Float = 0
            for i in -radius...radius { sum += src[row + min(max(i, 0), w - 1)] }
            for x in 0..<w {
                dst[row + x] = sum * norm
                sum -= src[row + min(max(x - radius, 0), w - 1)]
                sum += src[row + min(max(x + radius + 1, 0), w - 1)]
            }
        }
    }

    /// Row by row, carrying one running sum per column: the same filter as a
    /// column walk, but reading memory in order, which is several times faster.
    private static func boxPassVertical(_ src: UnsafeMutableBufferPointer<Float>,
                                        into dst: UnsafeMutableBufferPointer<Float>,
                                        width w: Int, height h: Int, radius: Int) {
        let norm = 1 / Float(2 * radius + 1)
        var sums = [Float](repeating: 0, count: w)
        sums.withUnsafeMutableBufferPointer { sum in
            for i in -radius...radius {
                let row = min(max(i, 0), h - 1) * w
                for x in 0..<w { sum[x] += src[row + x] }
            }
            for y in 0..<h {
                let out = y * w
                for x in 0..<w { dst[out + x] = sum[x] * norm }
                let leaving = min(max(y - radius, 0), h - 1) * w
                let entering = min(max(y + radius + 1, 0), h - 1) * w
                for x in 0..<w { sum[x] += src[entering + x] - src[leaving + x] }
            }
        }
    }
}
