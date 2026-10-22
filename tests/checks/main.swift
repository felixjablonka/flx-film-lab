import Foundation
import CoreGraphics
import ImageIO
import simd
@testable import FLXCore

// Invariant checks for the paths the parity harness cannot see.
//
// Parity runs agx's own configuration — exposure 0, no light-table white
// balance, no gamut compression, sRGB out — so it stayed green while the app's
// defaults were wrong in each of those places. Each check here pins one of
// them down with a property that has to hold whatever the stocks look like.

var failures = 0
func check(_ ok: Bool, _ message: String) {
    print("   \(ok ? "ok  " : "FAIL") \(message)")
    if !ok { failures += 1 }
}

func lab(_ c: SIMD3<Float>) -> SIMD3<Float> {
    let r = max(c.x, 0), g = max(c.y, 0), b = max(c.z, 0)
    let x = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b
    let y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b
    let z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b
    func f(_ t: Float) -> Float { t > 0.008856 ? cbrt(t) : (7.787 * t + 16.0 / 116.0) }
    let fx = f(x / 0.95047), fy = f(y), fz = f(z / 1.08883)
    return SIMD3(116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz))
}

/// One row of scene greys, `stops` relative to 18%.
func greyWedge(_ stops: [Float]) -> [Float] {
    var px = [Float](repeating: 1, count: stops.count * 4)
    for (i, s) in stops.enumerated() {
        let v = RenderSetupBuilder.midgrayValue * pow(2, s)
        px[i * 4] = v; px[i * 4 + 1] = v; px[i * 4 + 2] = v
    }
    return px
}

func render(_ renderer: Renderer, _ recipe: Recipe, exposureEV: Float? = nil) throws -> [SIMD3<Float>] {
    let setup = try RenderSetupBuilder.build(recipe: recipe, exposureEV: exposureEV)
    _ = try renderer.render(setup: setup, recipe: recipe)
    let out = renderer.readLinear()!
    return (0..<(out.count / 4)).map { SIMD3(out[$0 * 4], out[$0 * 4 + 1], out[$0 * 4 + 2]) }
}

func baseRecipe(film: String) throws -> Recipe {
    var r = Recipe()
    r.filmStock = film
    if let target = try PackLibrary.shared.pack(film).targetPrint { r.printStock = target }
    r.autoExposure = false
    r.grainEnabled = false
    r.halationEnabled = false
    return r
}

let renderer = try Renderer()
let stops: [Float] = [-2, -1, 0, 1, 2, 8]   // the last one is clear film / paper white
renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
let mid = 2, clear = 5

print("exposure reaches the film stage")
do {
    let r = try baseRecipe(film: "kodak_portra_400")
    _ = try render(renderer, r, exposureEV: 0)
    let at0 = renderer.readTap("log_e_film")!
    _ = try render(renderer, r, exposureEV: 1.5)
    let at15 = renderer.readTap("log_e_film")!
    let shift = at15[mid * 4 + 1] - at0[mid * 4 + 1]
    check(abs(shift - 1.5 * log10(2)) < 1e-4,
          String(format: "+1.5 EV moves mid-grey log exposure by %.4f (want %.4f)", shift, 1.5 * log10(2)))
}

print("slides are white-balanced on their base")
for entry in PackLibrary.shared.filmStocks where entry.type == "positive" {
    let r = try baseRecipe(film: entry.stock)
    let out = try render(renderer, r).map(lab)
    let g = out[mid], w = out[clear]
    check(abs(g.y) < 3 && abs(g.z) < 3,
          String(format: "%@ mid-grey a* %+.1f b* %+.1f", entry.stock, g.y, g.z))
    check(w.x > 99 && abs(w.y) < 1 && abs(w.z) < 1,
          String(format: "%@ clear base L* %.1f a* %+.1f b* %+.1f", entry.stock, w.x, w.y, w.z))
}

print("graduated exposure changes what the film receives, and only where it covers")
// Where the gradient is at full strength the film must receive exactly what
// that much more or less exposure gives it, where it has faded out exactly
// what it got without one, and in between what the CPU copy of the profile
// says.
do {
    let size = 64
    var px = [Float](repeating: 1, count: size * size * 4)
    for i in 0..<(size * size) {
        px[i * 4] = RenderSetupBuilder.midgrayValue
        px[i * 4 + 1] = RenderSetupBuilder.midgrayValue
        px[i * 4 + 2] = RenderSetupBuilder.midgrayValue
    }
    renderer.setImage(px, width: size, height: size)
    var r = try baseRecipe(film: "fujifilm_provia_100f")
    _ = try render(renderer, r, exposureEV: 0)
    let plain = renderer.readTap("log_e_film")!
    let decade = log10(Float(2))
    for ev in [-2.0, 1.0] {
        var g = ExposureGradient()
        g.points = [CGPoint(x: 0.3, y: 0.25), CGPoint(x: 0.3, y: 0.75)]
        g.exposureEV = ev
        r.gradients = [g]
        _ = try render(renderer, r, exposureEV: 0)
        let graded = renderer.readTap("log_e_film")!
        var worst: Float = 0
        for row in 0..<size {
            let o = (row * size + size / 2) * 4 + 1
            let want = g.exposureEV(atX: Float(size / 2) + 0.5, y: Float(row) + 0.5,
                                    width: Float(size), height: Float(size)) * decade
            worst = max(worst, abs((graded[o] - plain[o]) - want))
        }
        let top = graded[(size / 2) * 4 + 1] - plain[(size / 2) * 4 + 1]
        let bottom = graded[((size - 1) * size + size / 2) * 4 + 1] - plain[((size - 1) * size + size / 2) * 4 + 1]
        check(abs(top - Float(ev) * decade) < 1e-4 && abs(bottom) < 1e-5 && worst < 1e-4,
              String(format: "%+.0f EV: full side %+.4f (want %+.4f), faded side %+.5f, worst row off by %.5f",
                     ev, top, Float(ev) * decade, bottom, worst))
    }

    // A stack adds up: one from the top at -2 EV crossed with one from the
    // left at +1 EV, checked over the whole frame.
    var fromTop = ExposureGradient()
    fromTop.points = [CGPoint(x: 0.5, y: 0.2), CGPoint(x: 0.5, y: 0.7)]
    fromTop.exposureEV = -2
    var fromLeft = ExposureGradient()
    fromLeft.points = [CGPoint(x: 0.1, y: 0.5), CGPoint(x: 0.8, y: 0.6)]
    fromLeft.exposureEV = 1
    r.gradients = [fromTop, fromLeft]
    _ = try render(renderer, r, exposureEV: 0)
    let stacked = renderer.readTap("log_e_film")!
    var worstStack: Float = 0
    for y in stride(from: 0, to: size, by: 3) {
        for x in stride(from: 0, to: size, by: 3) {
            let o = (y * size + x) * 4 + 1
            let want = ExposureGradient.exposureEV(r.gradients, atX: Float(x) + 0.5, y: Float(y) + 0.5,
                                                   width: Float(size), height: Float(size)) * decade
            worstStack = max(worstStack, abs((stacked[o] - plain[o]) - want))
        }
    }
    check(worstStack < 1e-4, String(format: "two stacked gradients add up: worst pixel off by %.5f", worstStack))

    // Saved recipes from before the gradients existed have no such key.
    let encoded = try JSONEncoder().encode(Recipe())
    var dict = try JSONSerialization.jsonObject(with: encoded) as! [String: Any]
    dict.removeValue(forKey: "exposureGradients")
    let old = try JSONDecoder().decode(Recipe.self, from: JSONSerialization.data(withJSONObject: dict))
    let roundTrip = try JSONDecoder().decode(Recipe.self, from: JSONEncoder().encode(r))
    check(old.gradients.isEmpty && roundTrip.gradients == r.gradients,
          "a recipe saved without gradients loads, and one with them round-trips")
    renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
}

print("Temperature and Tint move print and slide the same way")
for film in ["kodak_portra_400", "fujifilm_provia_100f"] {
    var r = try baseRecipe(film: film)
    let neutral = lab(try render(renderer, r)[mid])
    r.filterShiftY = Controls.temperature.toPhysical(50)
    let warm = lab(try render(renderer, r)[mid])
    r.filterShiftY = 0
    r.filterShiftM = Controls.tint.toPhysical(50)
    let magenta = lab(try render(renderer, r)[mid])
    check(warm.z - neutral.z > 5, String(format: "%@ Temperature +50: b* %+.1f", film, warm.z - neutral.z))
    check(magenta.y - neutral.y > 5, String(format: "%@ Tint +50: a* %+.1f", film, magenta.y - neutral.y))
}

print("gamut compression leaves in-gamut colour alone and keeps the rest inside")
do {
    var r = try baseRecipe(film: "kodak_portra_400")
    r.gamutCompress = false
    let plain = try render(renderer, r)
    r.gamutCompress = true
    let compressed = try render(renderer, r)
    let worst = zip(plain, compressed).map { simd_length($0 - $1) }.max() ?? 0
    check(worst < 1e-5, String(format: "neutral wedge unchanged (max |diff| %.2g)", worst))

    // Saturated scene colours through Velvia overshoot sRGB by a wide margin.
    var px: [Float] = []
    for hue in 0..<24 {
        let a = Float(hue) / 24 * 2 * .pi
        let c = SIMD3<Float>(cos(a), cos(a - 2.094), cos(a + 2.094)) * 0.5 + 0.5
        let v = simd_max(c * c * c, SIMD3(repeating: 0.002)) * 0.4
        px += [v.x, v.y, v.z, 1]
    }
    renderer.setImage(px, width: 24, height: 1)
    var v = try baseRecipe(film: "fujifilm_velvia_50")
    v.gamutCompress = false
    let raw = try render(renderer, v)
    v.gamutCompress = true
    let inside = try render(renderer, v)
    let outsideBefore = raw.filter { $0.min() < 0 || $0.max() > 1 }.count
    let outsideAfter = inside.filter { $0.min() < -1e-4 || $0.max() > 1 + 1e-4 }.count
    check(outsideBefore > 0 && outsideAfter == 0,
          "\(outsideBefore)/24 saturated patches outside sRGB before, \(outsideAfter) after")

    // Pulled in along their own hue line, not clipped per channel.
    func oklabHue(_ c: SIMD3<Float>) -> Float {
        let l = 0.4122214708 * c.x + 0.5363325363 * c.y + 0.0514459929 * c.z
        let m = 0.2119034982 * c.x + 0.6806995451 * c.y + 0.1073969566 * c.z
        let s = 0.0883024619 * c.x + 0.2817188376 * c.y + 0.6299787005 * c.z
        let lc = cbrt(l), mc = cbrt(m), sc = cbrt(s)
        let a = 1.9779984951 * lc - 2.4285922050 * mc + 0.4505937099 * sc
        let b = 0.0259040371 * lc + 0.7827717662 * mc - 0.8086757660 * sc
        return atan2(b, a) * 180 / .pi
    }
    var worstHue: Float = 0
    for (before, after) in zip(raw, inside) where before.min() < 0 || before.max() > 1 {
        var d = abs(oklabHue(before) - oklabHue(after))
        if d > 180 { d = 360 - d }
        worstHue = max(worstHue, d)
    }
    check(worstHue < 0.5, String(format: "out-of-gamut patches keep their hue (worst %.2f deg)", worstHue))
    renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
}

print("Brightness, Shadows and Highlights stay in their own band")
do {
    let fine = stride(from: Float(-7), through: 5, by: 0.25).map { $0 }
    renderer.setImage(greyWedge(fine), width: fine.count, height: 1)
    let midIndex = fine.firstIndex(of: 0)!
    let base = try baseRecipe(film: "kodak_portra_400")
    let neutral = try render(renderer, base).map { lab($0).x }
    func change(_ apply: (inout Recipe) -> Void) throws -> [Float] {
        var r = base
        apply(&r)
        return zip(try render(renderer, r).map { lab($0).x }, neutral).map { $0 - $1 }
    }
    let ends: (([Float]) -> Float) = { max(abs($0.first!), abs($0.last!)) }

    let brighter = try change { $0.brightness = Controls.brightness.toPhysical(100) }
    check(brighter[midIndex] > 12 && ends(brighter) < 1,
          String(format: "Brightness +100: mid-grey %+.1f L*, paper black/white within %.2f", brighter[midIndex], ends(brighter)))
    // Shadows and Highlights act hardest near their own end of the scale,
    // not at the quarter tones, and leave mid-grey and the endpoints alone.
    func peak(_ d: [Float]) -> (change: Float, at: Float) {
        let i = d.indices.max(by: { abs(d[$0]) < abs(d[$1]) })!
        return (d[i], neutral[i])
    }
    for (label, value, apply) in [
        ("Shadows +100", Float(1), { (r: inout Recipe) in r.shadows = Controls.shadows.toPhysical(100) }),
        ("Shadows -100", Float(1), { (r: inout Recipe) in r.shadows = Controls.shadows.toPhysical(-100) }),
        ("Highlights +100", Float(0), { (r: inout Recipe) in r.highlights = Controls.highlights.toPhysical(100) }),
        ("Highlights -100", Float(0), { (r: inout Recipe) in r.highlights = Controls.highlights.toPhysical(-100) }),
    ] as [(String, Float, (inout Recipe) -> Void)] {
        let d = try change(apply)
        let p = peak(d)
        let nearItsEnd = value == 1 ? p.at < 25 : p.at > 75
        // The darkest tone a negative can print (its base, L* ~4) sits a
        // little above the paper's own black, inside the shadow band's foot.
        check(abs(p.change) > 4 && nearItsEnd && abs(d[midIndex]) < 0.5 && ends(d) < 3,
              String(format: "%@: peak %+.1f L* at L* %.0f, mid-grey %+.2f, ends within %.2f",
                     label, p.change, p.at, d[midIndex], ends(d)))
    }

    // No combination may fold the curve back on itself (a tone reversal).
    var worstDrop: Float = 0
    for b in [-100.0, 0, 100] { for s in [-100.0, 0, 100] { for h in [-100.0, 0, 100] { for grade in [50.0, 115, 180] {
        var r = base
        r.brightness = Controls.brightness.toPhysical(b)
        r.shadows = Controls.shadows.toPhysical(s)
        r.highlights = Controls.highlights.toPhysical(h)
        r.grade = grade
        let l = try render(renderer, r).map { lab($0).x }
        for i in 1..<l.count { worstDrop = max(worstDrop, l[i - 1] - l[i]) }
    }}}}
    check(worstDrop < 0.05, String(format: "81 extreme combinations stay monotone (worst step back %.3f L*)", worstDrop))
    renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
}

print("Auto Contrast fits the frame instead of crushing it")
do {
    // A textured scene spanning `stops` stops around mid-grey: a ramp with a
    // fine checker on top, so the texture gate lets every part of it vote.
    func scene(stops span: Float) -> (pixels: [Float], width: Int, height: Int) {
        let w = 512, h = 128
        var px = [Float](repeating: 1, count: w * h * 4)
        for y in 0..<h { for x in 0..<w {
            let ramp = (Float(x) / Float(w - 1) - 0.5) * span
            let checker: Float = ((x / 4 + y / 4) % 2 == 0) ? 0.3 : -0.3
            let v = RenderSetupBuilder.midgrayValue * pow(2, ramp + checker)
            let o = (y * w + x) * 4
            px[o] = v; px[o + 1] = v; px[o + 2] = v
        }}
        return (px, w, h)
    }
    func crushedShare(_ r: Recipe, _ s: (pixels: [Float], width: Int, height: Int)) throws -> Float {
        renderer.setImage(s.pixels, width: s.width, height: s.height)
        let out = try render(renderer, r)
        return Float(out.filter { lab($0).x < 6 }.count) / Float(out.count)
    }
    var r = try baseRecipe(film: "kodak_portra_400")
    let wide = scene(stops: 13)
    let atReference = try crushedShare(r, wide)
    r.autoGrade = true
    let auto = try RenderSetupBuilder.resolveAuto(recipe: r, pixels: wide.pixels, width: wide.width,
                                                  height: wide.height, exposureEV: 0).recipe
    let fitted = try crushedShare(auto, wide)
    check(auto.grade > 140 && fitted < atReference / 2,
          String(format: "13-stop scene: R%.0f, crushed %.0f%% -> %.0f%% of the frame", auto.grade, atReference * 100, fitted * 100))

    let flat = scene(stops: 2)
    let flatGrade = try RenderSetupBuilder.resolveAuto(recipe: r, pixels: flat.pixels, width: flat.width,
                                                       height: flat.height, exposureEV: 0).recipe.grade
    check(flatGrade >= 92, String(format: "2-stop scene hardens only to R%.0f (never past R92)", flatGrade))
    renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
}

print("grain adds texture, not a colour shift, at any resolution")
do {
    // A colourful gradient with grain as loud per pixel as in a full-size
    // export: 4x the grain size at 1600 px puts as few silver grains in each
    // pixel as an 6400 px export has. With grain on, the frame must average to
    // the grain-free colour — otherwise the export drifts from the preview.
    // The bottom quarter is a saturated teal whose red prints dark: where
    // taking the grain's average back out once pushed pixels below zero, and
    // every file clipped them back up.
    let w = 1600, h = 800, tealFrom = h * 3 / 4
    var px = [Float](repeating: 1, count: w * h * 4)
    for y in 0..<h { for x in 0..<w {
        let u = Float(x) / Float(w - 1), v = Float(y) / Float(h - 1)
        let tone = RenderSetupBuilder.midgrayValue * pow(2, (u - 0.5) * 6)
        let o = (y * w + x) * 4
        if y >= tealFrom {
            px[o] = tone * 0.04; px[o + 1] = tone; px[o + 2] = tone * 1.1
        } else {
            px[o] = tone * (0.7 + 0.6 * v); px[o + 1] = tone; px[o + 2] = tone * (1.3 - 0.6 * v)
        }
    }}
    renderer.setImage(px, width: w, height: h)
    var r = try baseRecipe(film: "kodak_portra_400")
    r.grainScale = 4
    r.grainEnabled = false
    let clean = try render(renderer, r)
    r.grainEnabled = true
    let grainy = try render(renderer, r)
    // Compare what an eye sees: the colour of the averaged light, region by
    // region — not the average of per-pixel L*a*b*, which a cube root biases
    // for any noisy image. The grainy side is clipped at zero, as a file is.
    var shift = SIMD3<Float>.zero
    let bands = 8
    for rows in [0..<tealFrom, tealFrom..<h] { for b in 0..<bands {
        var sumClean = SIMD3<Float>.zero, sumGrainy = SIMD3<Float>.zero
        for y in rows { for x in (b * w / bands)..<((b + 1) * w / bands) {
            sumClean += clean[y * w + x]; sumGrainy += simd_max(grainy[y * w + x], .zero)
        }}
        let count = Float(rows.count * w / bands)
        let local = lab(sumGrainy / count) - lab(sumClean / count)
        if simd_reduce_max(simd_abs(local)) > simd_reduce_max(simd_abs(shift)) { shift = local }
    }}
    let spread = zip(grainy, clean).map { simd_length(lab($0) - lab($1)) }.reduce(0, +) / Float(clean.count)
    let negative = grainy.filter { simd_reduce_min($0) < 0 }.count
    check(simd_reduce_max(simd_abs(shift)) < 0.3 && spread > 1 && negative == 0,
          String(format: "export-size grain: worst regional shift L* %+.2f a* %+.2f b* %+.2f, grain texture %.1f dE per pixel, %d pixels below zero",
                 shift.x, shift.y, shift.z, spread, negative))
    renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
}

print("grain is softened the way agx softens it")
do {
    // agx blurs the drawn grain by `blur` pixels (0.65 for every stock), which
    // correlates neighbouring pixels' grain by about 0.5; grain drawn without
    // it is not correlated at all, and twice as loud per pixel.
    let w = 512, h = 512, grey = RenderSetupBuilder.midgrayValue
    var px = [Float](repeating: 1, count: w * h * 4)
    for i in 0..<(w * h) { px[i * 4] = grey; px[i * 4 + 1] = grey; px[i * 4 + 2] = grey }
    renderer.setImage(px, width: w, height: h)
    var r = try baseRecipe(film: "kodak_portra_400")
    r.grainEnabled = true
    _ = try render(renderer, r)
    let film = renderer.readTap("cmy_film")!
    var correlations: [Double] = []
    for c in 0..<3 {
        var mean = 0.0
        for i in 0..<(w * h) { mean += Double(film[i * 4 + c]) }
        mean /= Double(w * h)
        var v0 = 0.0, vx = 0.0, vy = 0.0
        for y in 1..<(h - 2) { for x in 1..<(w - 2) {
            let d = Double(film[(y * w + x) * 4 + c]) - mean
            v0 += d * d
            vx += d * (Double(film[(y * w + x + 1) * 4 + c]) - mean)
            vy += d * (Double(film[((y + 1) * w + x) * 4 + c]) - mean)
        }}
        correlations += [vx / v0, vy / v0]
    }
    let lo = correlations.min()!, hi = correlations.max()!
    check(lo > 0.35 && hi < 0.7,
          String(format: "neighbouring pixels' grain correlates by %.2f to %.2f (agx's 0.65 px softening: about 0.5)", lo, hi))
    renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
}

print("black-and-white stocks print in silver")
do {
    let bw = ["kodak_tri_x_400", "kodak_tri_x_400_push2", "ilford_hp5_plus", "fujifilm_neopan_1600", "agfa_apx_25"]
    let paper = "ilford_multigrade_rc_deluxe"

    // A colourful chart, hue around and exposure across, grain on: on a
    // silver paper every pixel must come out neutral, grain included —
    // the three channels are one layer, drawn from one random field.
    let w = 256, h = 128
    var chart = [Float](repeating: 1, count: w * h * 4)
    for y in 0..<h { for x in 0..<w {
        let hue = Float(x) / Float(w) * 2 * .pi
        let v = RenderSetupBuilder.midgrayValue * pow(2, (Float(y) / Float(h) - 0.5) * 6)
        let o = (y * w + x) * 4
        chart[o] = v * (1 + 0.8 * cos(hue)); chart[o + 1] = v * (1 + 0.8 * cos(hue - 2.094))
        chart[o + 2] = v * (1 + 0.8 * cos(hue + 2.094))
    }}
    renderer.setImage(chart, width: w, height: h)
    for film in bw {
        var r = try baseRecipe(film: film)
        r.grainEnabled = true
        r.halationEnabled = true
        let out = try render(renderer, r)
        let worst = out.map { simd_length(SIMD2(lab($0).y, lab($0).z)) }.max() ?? .infinity
        check(r.printStock == paper && worst < 0.1,
              String(format: "%@ on %@: worst pixel chroma %.3f, grain, halation and edge effects on", film, r.printStock, worst))
    }

    // Sharpness as Kodak measures it (F-4017, Tri-X 400 in D-76): a small
    // sinusoidal exposure, its modulation in the print against a large-area
    // step of the same size. Above 100% at coarse detail is development's
    // edge effect; without it, and with a colour film's scatter, Tri-X once
    // read 36% at 24 cycles/mm where Kodak measures 101%.
    do {
        let side = 6048, rows = 8, m = 0.04
        let pitchMM = Double(Renderer.pixelSizeUm(width: side, height: rows, filmFormatMM: 36)) / 1000
        func logLuminance(_ r: Recipe, _ pattern: (Int) -> Double) throws -> [Double] {
            var px = [Float](repeating: 1, count: side * rows * 4)
            for x in 0..<side {
                let v = RenderSetupBuilder.midgrayValue * Float(pow(10, pattern(x)))
                for yy in 0..<rows { let o = (yy * side + x) * 4; px[o] = v; px[o + 1] = v; px[o + 2] = v }
            }
            renderer.setImage(px, width: side, height: rows)
            let out = try render(renderer, r)
            return (0..<side).map { log10(Double(simd_dot(out[(rows / 2) * side + $0], SIMD3(0.2126, 0.7152, 0.0722))) + 1e-9) }
        }
        func response(_ r: Recipe, at f: Double, offset: Double = 0) throws -> Double {
            let large = (try logLuminance(r) { _ in offset + m }[side / 2] - logLuminance(r) { _ in offset - m }[side / 2]) / 2
            let w = 2 * Double.pi * f * pitchMM
            let l = try logLuminance(r) { offset + m * sin(w * (Double($0) + 0.5)) }
            let span = (side / 5)..<(4 * side / 5)
            let mean = span.map { l[$0] }.reduce(0, +) / Double(span.count)
            var s = 0.0, c = 0.0, s2 = 0.0, c2 = 0.0
            for x in span {
                let a = w * (Double(x) + 0.5)
                s += (l[x] - mean) * sin(a); c += (l[x] - mean) * cos(a); s2 += sin(a) * sin(a); c2 += cos(a) * cos(a)
            }
            return 100 * hypot(s / s2, c / c2) / large
        }
        let kodak: [(f: Double, response: Double)] = [(8, 112.0), (24, 100.6), (50, 54.7)]
        for film in bw where film != "agfa_apx_25" {
            var r = try baseRecipe(film: film)
            r.halationEnabled = true          // the scatter lives in the halation stage
            let got = try kodak.map { try response(r, at: $0.f) }
            check(zip(got, kodak).allSatisfy { abs($0 - $1.response) < 5 },
                  String(format: "%@: response %.0f%% at 8, %.0f%% at 24, %.0f%% at 50 cycles/mm (Kodak's Tri-X 112, 101, 55)",
                         film, got[0], got[1], got[2]))
            var plain = r
            plain.couplersEnabled = false
            let flat = try response(plain, at: 8)
            check(flat < 100 && got[0] > flat + 10,
                  String(format: "%@: without edge effects %.0f%% at 8 cycles/mm, below 100%% as scatter alone gives", film, flat))
        }

        // Agfa publishes APX 25's own curve, which it is fitted to instead. Its
        // scatter core is half a pixel at this pitch, too fine to be drawn
        // exactly, so finer than 30 cycles/mm the frame renders it sharper than
        // Agfa measures (77% at 50 against 71).
        do {
            var r = try baseRecipe(film: "agfa_apx_25")
            r.halationEnabled = true
            let agfa: [(f: Double, response: Double)] = [(5, 105.0), (20, 99.0), (30, 90.0)]
            let got = try agfa.map { try response(r, at: $0.f) }
            check(zip(got, agfa).allSatisfy { abs($0 - $1.response) < 5 },
                  String(format: "agfa_apx_25: response %.0f%% at 5, %.0f%% at 20, %.0f%% at 30 cycles/mm (Agfa's 105, 99, 90)",
                         got[0], got[1], got[2]))
        }

        // Fujifilm measures no edge effect for Provia 400X (AF3-0213E). On
        // 100F's couplers and its own steeper curve it read 200% at 10
        // cycles/mm two stops down, a bright rim along dark mid-tone edges.
        print("Provia 400X is as sharp as Fujifilm measures it")
        var provia = try baseRecipe(film: "fujifilm_provia_400x")
        provia.halationEnabled = true
        let fuji: [(f: Double, response: Double)] = [(5, 100.6), (10, 92.2), (20, 73.9)]
        let atGrey = try fuji.map { try response(provia, at: $0.f) }
        check(zip(atGrey, fuji).allSatisfy { abs($0 - $1.response) < 6 },
              String(format: "Provia 400X: response %.0f%% at 5, %.0f%% at 10, %.0f%% at 20 cycles/mm (Fujifilm's 101, 92, 74)",
                     atGrey[0], atGrey[1], atGrey[2]))
        let down = try [5.0, 10, 15].map { try response(provia, at: $0, offset: -0.6) }
        check(down.allSatisfy { $0 < 104 },
              String(format: "Provia 400X two stops down: %.0f%% at 5, %.0f%% at 10, %.0f%% at 15 cycles/mm, no edge rim",
                     down[0], down[1], down[2]))
        renderer.setImage(chart, width: w, height: h)
    }
    // A colour negative on the silver paper is neutral too.
    do {
        var r = try baseRecipe(film: "kodak_portra_400")
        r.printStock = paper
        let worst = try render(renderer, r).map { simd_length(SIMD2(lab($0).y, lab($0).z)) }.max() ?? .infinity
        check(worst < 0.1, String(format: "Portra 400 on Multigrade: worst pixel chroma %.3f", worst))
    }

    // On colour paper there is no table entry for a silver negative; the
    // solved filtration has to print its mid-grey neutral.
    renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
    for film in bw {
        var r = try baseRecipe(film: film)
        r.printStock = "kodak_portra_endura"
        let g = lab(try render(renderer, r)[mid])
        check(abs(g.y) < 0.5 && abs(g.z) < 0.5,
              String(format: "%@ on Portra Endura: mid-grey a* %+.2f b* %+.2f", film, g.y, g.z))
    }

    // Panchromatic, and bluer than the eye: an unfiltered silver film renders
    // a blue sky lighter and a red darker than their luminance, which is
    // what yellow and red filters are for. Two patches of equal luminance.
    let y = RenderSetupBuilder.midgrayValue
    let redPatch = SIMD3<Float>(0.8, 0.1, 0.1), bluePatch = SIMD3<Float>(0.1, 0.2, 0.8)
    func scaled(_ c: SIMD3<Float>) -> SIMD3<Float> { c * (y / simd_dot(c, SIMD3(0.2627, 0.6780, 0.0593))) }
    let rr = scaled(redPatch), bb = scaled(bluePatch)
    renderer.setImage([rr.x, rr.y, rr.z, 1, bb.x, bb.y, bb.z, 1], width: 2, height: 1)
    for film in bw {
        let out = try render(renderer, try baseRecipe(film: film)).map { lab($0).x }
        check(out[1] > out[0] + 2,
              String(format: "%@: equal-luminance blue prints L* %.1f, red L* %.1f", film, out[1], out[0]))
    }

    // A colour outside the xy triangle (negative Z: demosaicing noise in a
    // deep red shadow, here the inside of an ear) once landed on the
    // LUT's clamped edge, and Tri-X printed it lighter than the skin around
    // it. Scaled up five stops so the difference shows: it must not print
    // lighter than a grey of the same luminance.
    let ear = SIMD3<Float>(0.0131, 0.0004, -0.0024) * 32
    let earLuminance = simd_dot(ear, SIMD3(0.2627, 0.6780, 0.0593))
    renderer.setImage([ear.x, ear.y, ear.z, 1, earLuminance, earLuminance, earLuminance, 1], width: 2, height: 1)
    for film in bw {
        let out = try render(renderer, try baseRecipe(film: film)).map { lab($0).x }
        check(out[0] < out[1] + 2,
              String(format: "%@: a chromaticity outside the triangle prints L* %.1f, a grey of its luminance %.1f", film, out[0], out[1]))
    }

    // Camera filters. Exposure opens up by the filter factor, so a grey card
    // prints the same through every one; a red filter lightens red and
    // darkens blue, a blue filter the reverse.
    renderer.setImage([y, y, y, 1, rr.x, rr.y, rr.z, 1, bb.x, bb.y, bb.z, 1], width: 3, height: 1)
    for film in bw {
        var r = try baseRecipe(film: film)
        let plain = try render(renderer, r).map { lab($0).x }
        var greys: [Float] = []
        for filter in try PackLibrary.shared.pack(film).lensFilters ?? [] {
            r.lensFilter = filter.id
            let out = try render(renderer, r).map { lab($0).x }
            greys.append(abs(out[0] - plain[0]))
            if filter.id == "red" {
                check(out[1] > plain[1] + 3 && out[2] < plain[2] - 3,
                      String(format: "%@ through red: red L* %.1f -> %.1f, blue %.1f -> %.1f", film, plain[1], out[1], plain[2], out[2]))
            }
            if filter.id == "blue" {
                check(out[1] < plain[1] - 3 && out[2] > plain[2] + 3,
                      String(format: "%@ through blue: red L* %.1f -> %.1f, blue %.1f -> %.1f", film, plain[1], out[1], plain[2], out[2]))
            }
        }
        check(greys.count == 5 && (greys.max() ?? 1) < 0.5,
              String(format: "%@: a grey card prints within %.2f L* through all %d filters", film, greys.max() ?? 0, greys.count))
    }
    // Kodak F-4017's daylight filter factors for Tri-X 400: No. 8 yellow 2,
    // No. 25 red 8, No. 47 blue 6. (No. 58 green: 6; the model gives 9.9.)
    let factors = Dictionary(uniqueKeysWithValues: (try PackLibrary.shared.pack("kodak_tri_x_400").lensFilters ?? []).map { ($0.id, $0.factor) })
    check(abs((factors["red"] ?? 0) / 8 - 1) < 0.15 && abs((factors["blue"] ?? 0) / 6 - 1) < 0.15
          && abs((factors["yellow"] ?? 0) / 2 - 1) < 0.25,
          String(format: "Tri-X filter factors: yellow %.1f (Kodak 2), red %.1f (8), blue %.1f (6)",
                 factors["yellow"] ?? 0, factors["red"] ?? 0, factors["blue"] ?? 0))
    // Fujifilm AF3-608E's daylight factors for Neopan 1600: No. 8 yellow 2,
    // No. 21 orange 4 (the app's orange is the deeper No. 22), No. 25 red 8.
    let neopan = Dictionary(uniqueKeysWithValues: (try PackLibrary.shared.pack("fujifilm_neopan_1600").lensFilters ?? []).map { ($0.id, $0.factor) })
    check(abs((neopan["red"] ?? 0) / 8 - 1) < 0.15 && abs((neopan["yellow"] ?? 0) / 2 - 1) < 0.35
          && (neopan["orange"] ?? 0) > 4,
          String(format: "Neopan 1600 filter factors: yellow %.1f (Fujifilm 2), orange %.1f (No. 21: 4), red %.1f (8)",
                 neopan["yellow"] ?? 0, neopan["orange"] ?? 0, neopan["red"] ?? 0))

    // Grain sized to the datasheet: diffuse RMS granularity at net density
    // 1.0 through a 48 um aperture (Kodak F-4017: Tri-X 17; Ilford and Fujifilm
    // publish none, see bake_packs.BW_RMS_GRANULARITY). A flat field at
    // that density, read off the developed negative, averaged over squares of
    // the aperture's area — at the 6 um pitch the bake calibrates at, a 6000 px
    // export of a 35 mm frame.
    for (film, rms) in [("kodak_tri_x_400", 17.0), ("kodak_tri_x_400_push2", 19.0), ("ilford_hp5_plus", 17.0),
                        ("fujifilm_neopan_1600", 23.0), ("agfa_apx_25", 7.0)] {
        let pack = try PackLibrary.shared.pack(film)
        let net = pack.densityCurves.enumerated().filter { $0.offset % 3 == 0 }.map { $0.element - pack.densityMin[0] }
        let logE = pack.logExposure
        guard let i = net.firstIndex(where: { $0 >= 1.0 }) else { check(false, "\(film) never reaches density 1"); continue }
        let t = (1.0 - net[i - 1]) / (net[i] - net[i - 1])
        let e = logE[i - 1] + t * (logE[i] - logE[i - 1])
        let v = RenderSetupBuilder.midgrayValue * pow(10, e)
        let side = 6000, rows = 600
        renderer.setImage([Float](repeating: v, count: side * rows * 4).enumerated().map { $0.offset % 4 == 3 ? 1 : $0.element },
                          width: side, height: rows)
        var r = try baseRecipe(film: film)
        r.grainEnabled = true
        _ = try render(renderer, r)
        let negative = renderer.readTap("cmy_film")!
        let pitch = Renderer.pixelSizeUm(width: side, height: rows, filmFormatMM: r.filmFormatMM)
        let box = max(Int((sqrt(Float.pi) * 24 / pitch).rounded()), 1)
        var means: [Double] = []
        var y0 = 0
        while y0 + box <= rows {
            var x0 = 0
            while x0 + box <= side {
                var sum = 0.0
                for yy in y0..<(y0 + box) { for xx in x0..<(x0 + box) { sum += Double(negative[(yy * side + xx) * 4]) } }
                means.append(sum / Double(box * box))
                x0 += box
            }
            y0 += box
        }
        let mean = means.reduce(0, +) / Double(means.count)
        let sd = sqrt(means.map { ($0 - mean) * ($0 - mean) }.reduce(0, +) / Double(means.count - 1))
        // the box is not exactly the aperture's area: granularity goes as 1/sqrt(area)
        let granularity = 1000 * sd * Double(Float(box) * pitch) / Double(sqrt(Float.pi) * 24)
        check(abs(granularity - rms) < 0.1 * rms,
              String(format: "%@: RMS granularity %.1f at net density %.2f (target %.0f)", film, granularity, mean, rms))
    }
    renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
}

print("slides are as sharp as their sheets, and grey")
// Each slide's scatter and spreading inhibition are fitted to its datasheet's
// MTF (tools/bake/slide_mtf.py), one stop under mid-grey: the green layer's
// density modulation for a small sinusoidal exposure against a large-area
// step of the same size. Deeper down the edge effect must not run away — on
// agx's couplers Provia 400X read 200% at 10 cycles/mm two stops down — and
// every slide's grey wedge must view grey.
do {
    let sheets: [(film: String, mtf: [Double])] = [      // 5, 10, 20, 30 cycles/mm, green
        ("fujifilm_provia_100f", [115, 113, 92, 70]), ("fujifilm_provia_400x", [101, 92, 74, 61]),
        ("fujifilm_astia_100f", [102, 93, 74, 58]),
        ("fujifilm_velvia_50", [117, 122, 105, 81]), ("fujifilm_velvia_100", [109, 106, 84, 66]),
        ("kodak_ektachrome_100", [97, 91, 81, 63]), ("kodak_kodachrome_25", [131, 138, 110, 79]),
        ("kodak_kodachrome_64", [119, 123, 93, 69]), ("kodak_kodachrome_200", [119, 123, 93, 69]),
    ]
    let frequencies = [5.0, 10, 20, 30]
    let side = 6048, rows = 8, m = 0.02
    let pitchMM = Double(Renderer.pixelSizeUm(width: side, height: rows, filmFormatMM: 36)) / 1000
    func green(_ r: Recipe, ev: Double, _ pattern: (Int) -> Double) throws -> [Double] {
        var px = [Float](repeating: 1, count: side * rows * 4)
        for x in 0..<side {
            let v = RenderSetupBuilder.midgrayValue * Float(pow(2, ev) * pow(10, pattern(x)))
            for yy in 0..<rows { let o = (yy * side + x) * 4; px[o] = v; px[o + 1] = v; px[o + 2] = v }
        }
        renderer.setImage(px, width: side, height: rows)
        _ = try render(renderer, r)
        let density = renderer.readTap("cmy_film")!
        return (0..<side).map { Double(density[((rows / 2) * side + $0) * 4 + 1]) }
    }
    func responses(_ r: Recipe, ev: Double) throws -> [Double] {
        let large = abs(try green(r, ev: ev) { _ in m }[side / 2] - green(r, ev: ev) { _ in -m }[side / 2]) / 2
        return try frequencies.map { f in
            let w = 2 * Double.pi * f * pitchMM
            let l = try green(r, ev: ev) { m * sin(w * (Double($0) + 0.5)) }
            let span = (side / 5)..<(4 * side / 5)
            let mean = span.map { l[$0] }.reduce(0, +) / Double(span.count)
            var s = 0.0, c = 0.0, s2 = 0.0, c2 = 0.0
            for x in span {
                let a = w * (Double(x) + 0.5)
                s += (l[x] - mean) * sin(a); c += (l[x] - mean) * cos(a); s2 += sin(a) * sin(a); c2 += cos(a) * cos(a)
            }
            return 100 * hypot(s / s2, c / c2) / large
        }
    }
    for (film, sheet) in sheets {
        var r = try baseRecipe(film: film)
        r.halationEnabled = true          // the scatter lives in the halation stage
        let got = try responses(r, ev: -1)
        check(zip(got, sheet).allSatisfy { abs($0 - $1) < 6 },
              String(format: "%@ one stop down: %.0f%%, %.0f%%, %.0f%%, %.0f%% at 5, 10, 20, 30 cycles/mm (sheet %.0f, %.0f, %.0f, %.0f)",
                     film, got[0], got[1], got[2], got[3], sheet[0], sheet[1], sheet[2], sheet[3]))
        let deeper = try [-2.0, -3].map { try responses(r, ev: $0) }
        let rise = deeper.flatMap { zip($0, got).map { $0 - $1 } }.max() ?? .infinity
        check(rise < 20, String(format: "%@ two and three stops down: at most %+.0f points over one stop down", film, rise))
    }
    let wedge: [Float] = [-3, -2, -1, 0, 1, 2]
    renderer.setImage(greyWedge(wedge), width: wedge.count, height: 1)
    // Astia 100F's sheet draws green 0.03-0.07 below red where the density is
    // 2 to 3, and three stops down its grey views a* -3.1 as drawn.
    let drawn: [String: Float] = ["fujifilm_astia_100f": 3.5]
    for (film, _) in sheets {
        let out = try render(renderer, try baseRecipe(film: film)).map(lab)
        let a = out.map { abs($0.y) }.max() ?? .infinity, b = out.map { abs($0.z) }.max() ?? .infinity
        let limit = drawn[film] ?? 3
        check(a < limit && b < limit, String(format: "%@: grey from -3 to +2 stops within |a*| %.1f |b*| %.1f", film, a, b))
    }
    renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
}

print("colour negatives built from datasheets print a full, grey range")
// Each one's mid-grey is placed on its own sheet (metered at its rating, at
// camera stop 0, or a decade over its speed point), and its filtration solved
// so mid-grey prints grey. Reala 500D, metered at EI 500 on its sheet's axis,
// once printed every shadow at L* 27: the axis does not place that rating.
do {
    let negatives = ["fujifilm_superia_1600", "fujifilm_superia_reala_100", "agfa_optima_ii_100", "agfa_portrait_xps_160",
                     "fujifilm_eterna_vivid_160t", "fujifilm_eterna_500t", "fujifilm_reala_500d"]
    let wedge: [Float] = [-3, -2, -1, 0, 1, 2]
    renderer.setImage(greyWedge(wedge), width: wedge.count, height: 1)
    for film in negatives {
        let out = try render(renderer, try baseRecipe(film: film)).map(lab)
        let rising = zip(out, out.dropFirst()).allSatisfy { $0.x < $1.x }
        check(rising && out[0].x < 15 && out[5].x > 75 && abs(out[3].y) < 1 && abs(out[3].z) < 1,
              String(format: "%@: L* %.0f to %.0f from -3 to +2 stops, mid-grey a* %+.1f b* %+.1f",
                     film, out[0].x, out[5].x, out[3].y, out[3].z))
    }
    renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
}

print("bloom and glow land on the print where the light is")
do {
    // A bright square on a dark ground. Glow's effects run in Core Image on
    // the renderer's own texture; a round trip through Core Image can flip an
    // image, and the sizes must be fractions of the frame, not pixels.
    func scene(_ w: Int, _ h: Int) -> [Float] {
        var px = [Float](repeating: 0, count: w * h * 4)
        for i in 0..<(w * h) { px[i * 4] = 0.03; px[i * 4 + 1] = 0.03; px[i * 4 + 2] = 0.03; px[i * 4 + 3] = 1 }
        let x0 = w / 5, y0 = h / 5, side = w / 40
        for y in y0..<(y0 + side) { for x in x0..<(x0 + side) {
            let o = (y * w + x) * 4
            px[o] = 40; px[o + 1] = 40; px[o + 2] = 40
        }}
        return px
    }
    func luminance(_ out: [SIMD3<Float>], _ w: Int, _ x: Int, _ y: Int) -> Float {
        simd_dot(out[y * w + x], SIMD3(0.2126, 0.7152, 0.0722))
    }
    var bloom = GlowLook.off
    bloom.bloom.enabled = true
    bloom.glow.enabled = true
    var r = Recipe()
    r.autoExposure = false
    r.grainEnabled = false
    r.halationEnabled = false

    let w = 600, h = 400
    renderer.setImage(scene(w, h), width: w, height: h)
    let plain = try render(renderer, r)
    r.effects = .off
    check(try render(renderer, r) == plain, "all effects off renders exactly as no effects")
    r.effects = bloom
    let small = try render(renderer, r)
    var cx = 0.0, cy = 0.0, total = 0.0
    for y in 0..<h { for x in 0..<w {
        let added = Double(luminance(small, w, x, y) - luminance(plain, w, x, y))
        if added > 0 { cx += added * Double(x); cy += added * Double(y); total += added }
    }}
    let centre = (Double(w / 5 + w / 80), Double(h / 5 + w / 80))
    check(total > 0 && abs(cx / total - centre.0) < 3 && abs(cy / total - centre.1) < 3,
          String(format: "the added light is centred on the highlight (%.1f, %.1f), not mirrored (%.0f, %.0f)",
                 cx / total, cy / total, centre.0, centre.1))

    // Three times the size: the same bloom three times as wide.
    let W = w * 3, H = h * 3
    renderer.setImage(scene(W, H), width: W, height: H)
    r.effects = nil
    let plainBig = try render(renderer, r)
    r.effects = bloom
    let big = try render(renderer, r)
    // The bloom's profile beside the square, at distances that scale with
    // the frame (its sigma here is a few pixels), against the strongest it gets.
    var profile: [(Float, Float)] = []
    // (A small pixel x covers big pixels 3x...3x+2, so it is matched with 3x+1.)
    for d in stride(from: 1, through: 10, by: 1) {
        let x = w / 5 + w / 40 + d, y = h / 5 + w / 80
        let a = luminance(small, w, x, y) - luminance(plain, w, x, y)
        let b = luminance(big, W, 3 * x + 1, 3 * y + 1) - luminance(plainBig, W, 3 * x + 1, 3 * y + 1)
        profile.append((a, b))
    }
    let peak = profile.map { max($0.0, $0.1) }.max() ?? 0
    let worst = profile.map { abs($0.0 - $0.1) }.max() ?? .infinity
    check(peak > 0.005 && worst < 0.1 * peak,
          String(format: "a frame three times the size blooms the same: profile within %.1f%% of its peak", 100 * worst / max(peak, 1e-9)))
    renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
}

print("the raw decode keeps the darkest light the sensor recorded")
// Apple subtracts a "shadow bias" (3.0 on Nikon Z, 5.0 on a Leica M) unless
// it is set to none, which put this frame's foliage below zero: no Exposure
// could lift it. On the sensor it sits a median 8 DN over black. Uses a
// sample raw that is not in the repository, so it is skipped without one.
do {
    let sample = URL(fileURLWithPath: "Test Files/Test RAW files/DSC_0871.NEF")
    if FileManager.default.fileExists(atPath: sample.path) {
        let d = try RawLoader.load(url: sample, options: RawLoader.Options(whiteBalance: .daylight, maxDimension: 2000))
        var y: [Float] = []
        for row in (d.height / 10)..<(d.height * 9 / 10) {
            for x in 0..<(d.width / 4) {
                let o = (row * d.width + x) * 4
                y.append(0.2627 * d.pixels[o] + 0.6780 * d.pixels[o + 1] + 0.0593 * d.pixels[o + 2])
            }
        }
        y.sort()
        let below = Float(y.filter { $0 <= 0 }.count) / Float(y.count)
        check(y[y.count / 2] > 0 && below < 0.01,
              String(format: "DSC_0871's dark left side decodes at %.5f, %.2f%% at or below zero", y[y.count / 2], 100 * below))
    } else {
        print("   skip DSC_0871.NEF not present")
    }
}

print("daylight white balances are the CIE daylights, not lamps of the same temperature")
// Core Image reads a temperature with tint 0 as a blackbody, and puts D55 and
// D65 at tint +10. Balanced for "5503 K", a daylight frame decoded green-yellow
// (a* -1.9, b* +1.5 on a mid-grey). Each mode is compared with the same frame
// balanced on the temperature and tint Core Image itself reports for that
// daylight's chromaticity. Same sample raw as above.
do {
    let sample = URL(fileURLWithPath: "Test Files/Test RAW files/DSC_0871.NEF")
    if FileManager.default.fileExists(atPath: sample.path) {
        func meanRatio(_ a: RawLoader.Decoded, _ b: RawLoader.Decoded) -> SIMD3<Double> {
            var sum = SIMD3<Double>(repeating: 0), n = 0.0
            for i in stride(from: 0, to: a.pixels.count, by: 4) {
                let pa = SIMD3(Double(a.pixels[i]), Double(a.pixels[i + 1]), Double(a.pixels[i + 2]))
                let pb = SIMD3(Double(b.pixels[i]), Double(b.pixels[i + 1]), Double(b.pixels[i + 2]))
                guard pa.min() > 0.01, pb.min() > 0.01, pa.max() < 0.9, pb.max() < 0.9 else { continue }
                sum += pa / pb; n += 1
            }
            return sum / max(n, 1)
        }
        for (mode, label, temperature, tint) in [(RawLoader.WhiteBalance.filmReference, "Film reference (D55)", 5501.0, 10.01),
                                                 (.daylight, "Daylight (D65)", 6503.0, 9.79)] {
            let decoded = try RawLoader.load(url: sample, options: RawLoader.Options(whiteBalance: mode, filmReference: "D55",
                                                                                      maxDimension: 600))
            let reference = try RawLoader.load(url: sample, options: RawLoader.Options(whiteBalance: .custom, temperature: temperature,
                                                                                        tint: tint, maxDimension: 600))
            let r = meanRatio(decoded, reference)
            check(abs(r.x - 1) < 0.002 && abs(r.y - 1) < 0.002 && abs(r.z - 1) < 0.002,
                  String(format: "%@ decodes like %.0f K tint %+.2f: channel ratios %.4f %.4f %.4f",
                         label, temperature, tint, r.x, r.y, r.z))
        }
    } else {
        print("   skip DSC_0871.NEF not present")
    }
}

print("exported files decode back to what was rendered")
do {
    let levels: [Float] = [0.01, 0.05, 0.184, 0.5, 0.9]
    var px: [Float] = []
    for l in levels { px += [l, l, l, 1] }
    let linearTwin: [String: CFString] = [
        "sRGB": CGColorSpace.extendedLinearSRGB,
        "Display P3": CGColorSpace.extendedLinearDisplayP3,
        "ITU-R BT.2020": CGColorSpace.extendedLinearITUR_2020,
    ]
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("flx-checks-\(getpid())")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }
    for (space, twin) in linearTwin.sorted(by: { $0.key < $1.key }) {
        let url = dir.appendingPathComponent("ramp.tif")
        try Exporter.write(linear: px, width: levels.count, height: 1, to: url,
                           format: .tiff16, colorSpace: space)
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
            check(false, "\(space): could not read the export back")
            continue
        }
        var back = [Float](repeating: 0, count: levels.count * 4)
        let context = CGContext(data: &back, width: levels.count, height: 1, bitsPerComponent: 32,
                                bytesPerRow: levels.count * 16, space: CGColorSpace(name: twin)!,
                                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
                                    | CGBitmapInfo.floatComponents.rawValue
                                    | CGBitmapInfo.byteOrder32Little.rawValue)!
        context.draw(image, in: CGRect(x: 0, y: 0, width: levels.count, height: 1))
        let worst = levels.indices.map { abs(log2(back[$0 * 4 + 1] / levels[$0])) }.max() ?? .infinity
        check(worst < 0.02, String(format: "%@ 16-bit TIFF round trip within %.3f stops", space, worst))
    }
}

print(failures == 0 ? "\nall checks passed" : "\n\(failures) check(s) failed")
exit(failures == 0 ? 0 : 1)
