import Foundation
import simd

/// A look's grain as its print shows it, by the print's own density: what the
/// frame pipeline lays on a finished frame (`FramePipeline.grained`).
///
/// The grain is the negative's: each emulsion layer's Selwyn coefficient by
/// the negative's density, from the stock's own data (`GrainDefaults`), and
/// its dye clouds' size. The print sees it through its spectral response: a
/// change in one layer's density moves each of the print's channels, its own
/// most, the others through the dyes' overlapping absorptions. So the look's
/// grey scale is rendered as a ramp of light through the engine
/// (`LookBaker.printGrain`), once as it is and once with each layer of the
/// negative a little denser and a little thinner (`Renderer.negativeOffset`),
/// and read off each step: the negative's densities, the print's, and how
/// the print's channels answer each layer. Each pixel's grain is then each
/// layer's own, drawn at the negative density that prints the pixel's tone,
/// mixed as the print mixes them. Where the print is clipped, the paper's
/// white or its deepest black, the answer and the grain go to nothing, as on
/// a print. A colour film on black-and-white paper mixes its three layers'
/// grains into one neutral grain; a black-and-white film has one record.
struct PrintGrainTable: Equatable {
    /// Each record's Selwyn coefficient, OD² µm², [record][sample][layer],
    /// laid out as `GrainDefaults.selwynUm2`, by the print density of the
    /// channel that record prints (the red channel's for the cyan layer).
    var selwynUm2: [Float]
    /// How the print's density answers each record's, d D_print / d D_negative,
    /// [channel][sample][record], by the channel's own print density.
    var response: [Float]
    /// The print density at each channel's last sample.
    var tableDensityMax: SIMD3<Float>
    var samples: Int
    /// The negative's dye clouds or silver clumps, µm, [record][layer].
    var cloudUm: [Float]
    /// One record for the three channels: a black-and-white film.
    var monochrome: Bool
    /// The stock's part of every frame's seed, as in Stills.
    var stockSeed: UInt32

    /// The grey ramp the table is read from: scene light from 10 stops below
    /// an 18% grey to 10 above, `count` steps.
    static func greyRamp(count: Int) -> [Float] {
        var px = [Float](repeating: 1, count: count * 4)
        for i in 0..<count {
            let stops = -10 + 20 * Double(i) / Double(count - 1)
            let light = Float(0.18 * pow(2, stops))
            px[i * 4] = light
            px[i * 4 + 1] = light
            px[i * 4 + 2] = light
        }
        return px
    }

    /// A layer's coefficient at a negative density, as `film_grain_layer`
    /// reads it.
    static func negativeSelwyn(_ grain: GrainDefaults, record c: Int, layer: Int, density: Float) -> Float {
        let n = grain.tableSamples
        let top = max(grain.tableDensityMax[c], 1e-4)
        let x = min(max(max(density, 0) / top, 0), 1) * Float(n - 1)
        let i = min(Int(x), n - 2)
        let t = x - Float(i)
        let row = c * n
        let a = grain.selwynUm2[(row + i) * 3 + layer], b = grain.selwynUm2[(row + i + 1) * 3 + layer]
        return a + (b - a) * t
    }

    /// The table from the ramp's renders, RGBA each, `count` steps: the
    /// developed negative's net density per record, the print's light, and
    /// for each record the print's light with that record `step` denser and
    /// `step` thinner.
    static func build(negative: [Float], print: [Float], denser: [[Float]], thinner: [[Float]], step: Float,
                      count: Int, grain: GrainDefaults, monochromeFilm: Bool, stockSeed: UInt32,
                      samples m: Int = 64) -> PrintGrainTable {
        func density(_ light: [Float], _ i: Int, _ c: Int) -> Float { -log10(max(light[i * 4 + c], 1e-6)) }
        var selwyn = [Float](repeating: 0, count: 3 * m * 3)
        var response = [Float](repeating: 0, count: 3 * m * 3)
        var top = SIMD3<Float>(repeating: 0.05)
        for c in 0..<3 {
            // Each step: the print's density in this channel, how it answers
            // each record, and this record's own coefficients.
            var points: [(density: Float, answer: SIMD3<Float>, s: SIMD3<Float>)] = []
            for i in 0..<count {
                var answer = SIMD3<Float>(repeating: 0)
                for r in 0..<3 {
                    answer[r] = (density(denser[r], i, c) - density(thinner[r], i, c)) / (2 * step)
                }
                var s = SIMD3<Float>(repeating: 0)
                for layer in 0..<3 {
                    s[layer] = negativeSelwyn(grain, record: c, layer: layer, density: negative[i * 4 + c])
                }
                points.append((density(print, i, c), answer, s))
            }
            points.sort { $0.density < $1.density }
            guard let highest = points.last?.density else { continue }
            top[c] = min(max(highest, 0.05), 5)
            // On an even scale of print density: between the two steps around
            // each sample, none past the ramp's ends.
            var k = 0
            for j in 0..<m {
                let d = Float(j) / Float(m - 1) * top[c]
                while k + 1 < points.count - 1, points[k + 1].density < d { k += 1 }
                let a = points[k], b = points[min(k + 1, points.count - 1)]
                var t: Float = 0
                if d >= b.density || b.density - a.density < 1e-6 {
                    t = d <= a.density ? 0 : 1
                } else if d > a.density {
                    t = (d - a.density) / (b.density - a.density)
                }
                let s = a.s + (b.s - a.s) * t, answer = a.answer + (b.answer - a.answer) * t
                for n in 0..<3 {
                    selwyn[(c * m + j) * 3 + n] = max(s[n], 0)
                    response[(c * m + j) * 3 + n] = answer[n]
                }
            }
        }
        return PrintGrainTable(selwynUm2: selwyn, response: response, tableDensityMax: top, samples: m,
                               cloudUm: grain.cloudUm, monochrome: monochromeFilm, stockSeed: stockSeed)
    }
}
