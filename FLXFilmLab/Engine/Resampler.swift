import Foundation

/// The finished print made smaller, for an export with a long edge chosen
/// (`ExportSize`).
///
/// The print is made at full size, as the loupe shows it, and then shrunk in
/// linear light, where light adds up: fine detail, grain and colour keep the
/// brightness they have at full size. Shrunk after the tone curve instead,
/// as most editors do it, the branches of a tree against the sky came out up
/// to 2 L* darker, and a grainy black-and-white portrait 0.15 L* darker all
/// over.
///
/// The filter is Lanczos-3, stretched by the reduction, so that nothing finer
/// than the new pixels aliases. Its negative lobes ring at a hard edge, and in
/// linear light the dark side of an edge rang below black and was clipped
/// channel by channel into coloured specks. So each channel of a pixel is held
/// to the range of the light under the filter's main lobe: an edge stays as
/// crisp, and none of the ringing is left. Simply averaging the light each
/// pixel covers rings nowhere either, but it is softer, and at a slight
/// reduction its weights repeat every few pixels and the grain's strength
/// with them.
enum Resampler {
    /// The size of a `width` × `height` print exported with its long edge
    /// `longEdge` pixels, the short edge in proportion, to the nearest pixel.
    /// Its own size when `longEdge` is nil or not smaller: a print is never
    /// enlarged.
    static func size(width: Int, height: Int, longEdge: Int?) -> (width: Int, height: Int) {
        let long = max(width, height)
        guard let longEdge, longEdge > 0, longEdge < long else { return (width, height) }
        let short = { (side: Int) in max(1, Int((Double(side) * Double(longEdge) / Double(long)).rounded())) }
        return width >= height ? (longEdge, short(height)) : (short(width), longEdge)
    }

    /// The Lanczos window of three lobes, sinc(x) sinc(x/3).
    static func lanczos3(_ x: Double) -> Double {
        let x = abs(x)
        if x < 1e-12 { return 1 }
        if x >= 3 { return 0 }
        let px = Double.pi * x
        return 3 * sin(px) * sin(px / 3) / (px * px)
    }

    /// One direction of the filter, from `n` samples to `m`. Output sample
    /// `j` is centred on input position (j + ½) n/m and reads `count[j]`
    /// samples from `first[j]`, weighted by `weights` (`stride` to a sample,
    /// summing to 1); a sample beyond the edge counts as the edge's own. It
    /// is held within the light of samples `low[j]`...`high[j]`, those
    /// whose centres lie under the main lobe.
    struct Axis {
        var first: [Int] = []
        var count: [Int] = []
        var weights: [Float] = []
        var stride = 0
        var low: [Int] = []
        var high: [Int] = []

        init(from n: Int, to m: Int) {
            let scale = Double(n) / Double(m)
            let reach = 3 * scale
            stride = Int((2 * reach).rounded(.up)) + 2
            weights = [Float](repeating: 0, count: stride * m)
            for j in 0..<m {
                let centre = (Double(j) + 0.5) * scale
                let a = Int((centre - reach - 0.5).rounded(.up))
                let b = Int((centre + reach - 0.5).rounded(.down))
                let lo = max(a, 0), hi = min(b, n - 1)
                var w = [Double](repeating: 0, count: hi - lo + 1)
                for i in a...b {
                    w[min(max(i, 0), n - 1) - lo] += Resampler.lanczos3((Double(i) + 0.5 - centre) / scale)
                }
                let sum = w.reduce(0, +)
                for k in w.indices { weights[j * stride + k] = Float(w[k] / sum) }
                first.append(lo)
                count.append(w.count)
                let la = Int((centre - scale - 0.5).rounded(.up))
                let lb = Int((centre + scale - 0.5).rounded(.down))
                let lobeLow = min(max(la, 0), n - 1)
                low.append(lobeLow)
                high.append(max(min(max(lb, 0), n - 1), lobeLow))
            }
        }
    }

    /// `pixels`, RGBA floats in linear light, `width` × `height`, shrunk to
    /// `size`, no larger on either side.
    static func shrink(_ pixels: [Float], width: Int, height: Int, to size: (width: Int, height: Int)) -> [Float] {
        let (w, h) = size
        precondition(w <= width && h <= height && w > 0 && h > 0, "the resampler only shrinks")
        if w == width && h == height { return pixels }
        let across = Axis(from: width, to: w), down = Axis(from: height, to: h)
        var out = [Float](repeating: 0, count: w * h * 4)
        pixels.withUnsafeBufferPointer { source in
            out.withUnsafeMutableBufferPointer { target in
                across.weights.withUnsafeBufferPointer { acrossWeights in
                    down.weights.withUnsafeBufferPointer { downWeights in
                        let src = UnsafeRawPointer(source.baseAddress!).assumingMemoryBound(to: SIMD4<Float>.self)
                        let dst = UnsafeMutableRawPointer(target.baseAddress!).assumingMemoryBound(to: SIMD4<Float>.self)
                        let rowsPerTask = 4
                        DispatchQueue.concurrentPerform(iterations: (h + rowsPerTask - 1) / rowsPerTask) { task in
                            // Down the columns first, into three rows the full
                            // width: the weighted light, and the darkest and
                            // lightest under the main lobe.
                            let column = UnsafeMutablePointer<SIMD4<Float>>.allocate(capacity: width * 3)
                            defer { column.deallocate() }
                            let light = column, darkest = column + width, lightest = column + 2 * width
                            for y in (task * rowsPerTask)..<min(h, (task + 1) * rowsPerTask) {
                                light.update(repeating: .zero, count: width)
                                for k in 0..<down.count[y] {
                                    let weight = downWeights[y * down.stride + k]
                                    let row = src + (down.first[y] + k) * width
                                    for x in 0..<width { light[x] += weight * row[x] }
                                }
                                darkest.update(from: src + down.low[y] * width, count: width)
                                lightest.update(from: src + down.low[y] * width, count: width)
                                if down.high[y] > down.low[y] {
                                    for r in (down.low[y] + 1)...down.high[y] {
                                        let row = src + r * width
                                        for x in 0..<width {
                                            darkest[x] = pointwiseMin(darkest[x], row[x])
                                            lightest[x] = pointwiseMax(lightest[x], row[x])
                                        }
                                    }
                                }
                                // Then along the row.
                                let line = dst + y * w
                                for x in 0..<w {
                                    var sum = SIMD4<Float>.zero
                                    let first = across.first[x], weights = x * across.stride
                                    for k in 0..<across.count[x] { sum += acrossWeights[weights + k] * light[first + k] }
                                    var lo = darkest[across.low[x]], hi = lightest[across.low[x]]
                                    if across.high[x] > across.low[x] {
                                        for i in (across.low[x] + 1)...across.high[x] {
                                            lo = pointwiseMin(lo, darkest[i])
                                            hi = pointwiseMax(hi, lightest[i])
                                        }
                                    }
                                    line[x] = pointwiseMin(pointwiseMax(sum, lo), hi)
                                }
                            }
                        }
                    }
                }
            }
        }
        return out
    }
}
