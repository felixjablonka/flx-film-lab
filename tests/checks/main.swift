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
    // The sheets' MTF and granularity are measured on 35 mm, and the checks
    // that compare against them work out their pixel pitch for it.
    r.filmFormat = .thirtyFiveMM
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

    // HARMAN Phoenix 200 prints 2.2 times as hard as Portra 400 about
    // mid-grey, so its grades reach R400 (`gradeSoftest`) where every other
    // film's stop at R180, with the Contrast slider mapped as it always was.
    // A 6-stop scene loses its shadows on Phoenix at R115; Auto Contrast goes
    // past R180 to keep them.
    let phoenix = try PackLibrary.shared.pack("harman_phoenix_200")
    let usualRange = try PackLibrary.shared.filmStocks.filter { $0.stock != phoenix.stock }
        .allSatisfy { try PackLibrary.shared.pack($0.stock).softestGrade == Constants.gradeMax }
    check(phoenix.softestGrade == 400 && usualRange,
          String(format: "Phoenix's softest grade R%.0f; every other film's R%.0f", phoenix.softestGrade, Constants.gradeMax))
    let usual = Controls.contrast, phoenixScale = Controls.contrast(softest: 400)
    let unchanged = stride(from: -100.0, through: 100, by: 10).allSatisfy {
        abs(usual.toPhysical($0) - (Double(Constants.gradeReference) - $0 * 0.65)) < 1e-9
            && abs(usual.fromPhysical(usual.toPhysical($0)) - $0) < 1e-9
    }
    check(unchanged && phoenixScale.toPhysical(-100) == 400 && phoenixScale.toPhysical(100) == usual.toPhysical(100)
          && abs(phoenixScale.fromPhysical(phoenixScale.toPhysical(-37)) + 37) < 1e-9,
          String(format: "Contrast -100 is R%.0f on Phoenix, R%.0f elsewhere; the hard half the same",
                 phoenixScale.toPhysical(-100), usual.toPhysical(-100)))
    // It opens at Contrast -30 on that scale (Felix's pick); every other film at 0.
    let usualDefault = try PackLibrary.shared.filmStocks.filter { $0.stock != phoenix.stock }
        .allSatisfy { try PackLibrary.shared.pack($0.stock).defaultGrade == Constants.gradeReference }
    check(abs(phoenixScale.fromPhysical(Double(phoenix.defaultGrade)) + 30) < 1e-9 && usualDefault,
          String(format: "Phoenix opens at R%.1f, Contrast %.0f on its scale; every other film at R115",
                 phoenix.defaultGrade, phoenixScale.fromPhysical(Double(phoenix.defaultGrade))))
    var p = try baseRecipe(film: phoenix.stock)
    let six = scene(stops: 6)
    let phoenixReference = try crushedShare(p, six)
    p.autoGrade = true
    let phoenixAuto = try RenderSetupBuilder.resolveAuto(recipe: p, pixels: six.pixels, width: six.width,
                                                         height: six.height, exposureEV: 0).recipe
    let phoenixFitted = try crushedShare(phoenixAuto, six)
    check(phoenixAuto.grade > 180 && phoenixFitted < phoenixReference / 2,
          String(format: "harman_phoenix_200, 6-stop scene: R%.0f, crushed %.0f%% -> %.0f%% of the frame",
                 phoenixAuto.grade, phoenixReference * 100, phoenixFitted * 100))
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

print("a picture is laid on the film frame as large as it fits")
do {
    // 36 x 24 mm on 35 mm, 84 x 56 mm (6x9) on 120: a 3:2 picture fills the
    // frame, a 4:3 one (Fuji GFX, Micro Four Thirds) spans its short side, a
    // square one makes a 6x6 on 120, and one wider than 3:2 spans the long side.
    let cases: [(String, Int, Int, FilmFormat, Double)] = [
        ("3:2 Zf on 35 mm", 6048, 4032, .thirtyFiveMM, 36.0 / 6048),
        ("3:2 Zf, upright, on 35 mm", 4032, 6048, .thirtyFiveMM, 36.0 / 6048),
        ("3:2 Zf on 120", 6048, 4032, .mediumFormat, 84.0 / 6048),
        ("4:3 Micro Four Thirds on 35 mm", 5184, 3888, .thirtyFiveMM, 24.0 / 3888),
        ("4:3 GFX on 120", 11648, 8736, .mediumFormat, 56.0 / 8736),
        ("square on 120", 4000, 4000, .mediumFormat, 56.0 / 4000),
        ("16:9 on 35 mm", 6000, 3375, .thirtyFiveMM, 36.0 / 6000),
    ]
    for (name, w, h, format, mmPerPixel) in cases {
        let pitch = Double(Renderer.pixelSizeUm(width: w, height: h, format: format))
        let frame = format.frameMM
        let long = Double(max(w, h)) * pitch / 1000, short = Double(min(w, h)) * pitch / 1000
        check(abs(pitch / (1000 * mmPerPixel) - 1) < 1e-5 && long <= frame.long * (1 + 1e-5) && short <= frame.short * (1 + 1e-5),
              String(format: "%@: %.2f um a pixel, %.1f x %.1f mm on the film", name, pitch, long, short))
    }
}

print("Camera sharpness keeps the file's detail and the film's colour")
do {
    // With the switch on, no scatter in the emulsion and no development edge
    // effects: a small sinusoidal exposure across a 24 MP frame comes out
    // at its large-area modulation at every frequency up to the pixels'
    // limit, and a flat colour prints exactly as it does with the film's own
    // sharpness (both act only on detail). Halation stays on.
    let side = 6048, rows = 8, m = 0.04
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
    func response(_ r: Recipe, cyclesPerPixel f: Double) throws -> Double {
        let large = (try logLuminance(r) { _ in m }[side / 2] - logLuminance(r) { _ in -m }[side / 2]) / 2
        let w = 2 * Double.pi * f
        let l = try logLuminance(r) { m * sin(w * (Double($0) + 0.5)) }
        let span = (side / 5)..<(4 * side / 5)
        let mean = span.map { l[$0] }.reduce(0, +) / Double(span.count)
        var s = 0.0, c = 0.0, s2 = 0.0, c2 = 0.0
        for x in span {
            let a = w * (Double(x) + 0.5)
            s += (l[x] - mean) * sin(a); c += (l[x] - mean) * cos(a); s2 += sin(a) * sin(a); c2 += cos(a) * cos(a)
        }
        return 100 * hypot(s / s2, c / c2) / large
    }
    for film in ["kodak_portra_400", "fujifilm_provia_100f", "kodak_tri_x_400"] {
        var r = try baseRecipe(film: film)
        r.halationEnabled = true
        r.keepsCameraSharpness = true
        let got = try [0.05, 0.1, 0.2, 0.3, 0.45].map { try response(r, cyclesPerPixel: $0) }
        let worst = got.map { abs($0 - 100) }.max() ?? .infinity
        check(worst < 3, String(format: "%@: %.0f%%, %.0f%%, %.0f%%, %.0f%%, %.0f%% of the file's detail at 0.05 to 0.45 cycles a pixel",
                                film, got[0], got[1], got[2], got[3], got[4]))
        // a flat colour, with the switch on and off
        renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
        let sharp = try render(renderer, r).map(lab)
        r.keepsCameraSharpness = false
        let filmic = try render(renderer, r).map(lab)
        let shift = zip(sharp, filmic).map { simd_length($0 - $1) }.max() ?? .infinity
        check(shift < 0.05, String(format: "%@: a flat grey wedge prints the same with it on, %.3f dE apart", film, shift))
    }
    // Saved before the switch existed, a recipe still loads, as it off;
    // saved with it on, it comes back on.
    var plain = try JSONSerialization.jsonObject(with: JSONEncoder().encode(Recipe())) as! [String: Any]
    plain.removeValue(forKey: "cameraSharpness")
    let old = try? JSONDecoder().decode(Recipe.self, from: JSONSerialization.data(withJSONObject: plain))
    var on = Recipe()
    on.keepsCameraSharpness = true
    let back = try? JSONDecoder().decode(Recipe.self, from: JSONEncoder().encode(on))
    check(old?.keepsCameraSharpness == false && back?.keepsCameraSharpness == true,
          "a recipe saved without the switch loads with it off; one saved with it on comes back on")
    renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
}

print("each stock, and each picture, draws its own grain")
do {
    // The grain was seeded by pixel alone, so every stock drew the same
    // grain, only scaled: HP5, Tri-X and Neopan 1600 side by side looked like
    // one film. The stock seeds it now, and the picture's file
    // (`Renderer.grainSeed`): the same file on the same stock always grains
    // the same, anything else differently. A flat grey, the developed
    // negative's grain compared pixel by pixel.
    let w = 600, h = 200, grey = RenderSetupBuilder.midgrayValue
    var px = [Float](repeating: 1, count: w * h * 4)
    for i in 0..<(w * h) { px[i * 4] = grey; px[i * 4 + 1] = grey; px[i * 4 + 2] = grey }
    renderer.setImage(px, width: w, height: h)
    func grain(_ film: String, seed: UInt32) throws -> [Double] {
        renderer.grainSeed = seed
        var r = try baseRecipe(film: film)
        r.grainEnabled = true
        _ = try render(renderer, r)
        let negative = renderer.readTap("cmy_film")!
        let values = (0..<(w * h)).map { Double(negative[$0 * 4]) }
        let mean = values.reduce(0, +) / Double(values.count)
        return values.map { $0 - mean }
    }
    func correlation(_ a: [Double], _ b: [Double]) -> Double {
        zip(a, b).map(*).reduce(0, +) / sqrt(a.map { $0 * $0 }.reduce(0, +) * b.map { $0 * $0 }.reduce(0, +))
    }
    let trix = try grain("kodak_tri_x_400", seed: 7), again = try grain("kodak_tri_x_400", seed: 7)
    let hp5 = try grain("ilford_hp5_plus", seed: 7), neopan = try grain("fujifilm_neopan_1600", seed: 7)
    let otherPicture = try grain("kodak_tri_x_400", seed: 8)
    let across = [correlation(trix, hp5), correlation(trix, neopan), correlation(hp5, neopan)].map(abs).max()!
    check(correlation(trix, again) > 0.9999 && across < 0.05 && abs(correlation(trix, otherPicture)) < 0.05,
          String(format: "the same stock and picture grain alike (%.4f); Tri-X, HP5 and Neopan 1600 at most %.3f alike; another picture %.3f",
                 correlation(trix, again), across, correlation(trix, otherPicture)))
    renderer.grainSeed = 0
    renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
}

print("grain is the film's size, not the pixels'")
do {
    // Grain is drawn at the film's scale: seen at the same size, a full-size
    // export and the preview must carry the same grain, and the finished
    // print must keep the negative's coarse grain — the mottle a print shows
    // at arm's length. 1.0-1.1 drew it per pixel with agx's fixed 0.65 px
    // softening, so the export seen at the preview's size had a third more
    // grain than the preview, and the colour safety step took 40% of the
    // grain out at 0.1 mm, leaving a fizz. A flat mid-grey, read in L*.
    let grey = RenderSetupBuilder.midgrayValue
    func flat(_ w: Int, _ h: Int) -> [Float] {
        var px = [Float](repeating: 1, count: w * h * 4)
        for i in 0..<(w * h) { px[i * 4] = grey; px[i * 4 + 1] = grey; px[i * 4 + 2] = grey }
        return px
    }
    // the standard deviation of L* averaged over squares of `side` pixels
    func boxed(_ l: [Float], _ w: Int, _ h: Int, _ side: Int) -> Double {
        var means: [Double] = []
        var y = 0
        while y + side <= h {
            var x = 0
            while x + side <= w {
                var sum = 0.0
                for yy in y..<(y + side) { for xx in x..<(x + side) { sum += Double(l[yy * w + xx]) } }
                means.append(sum / Double(side * side))
                x += side
            }
            y += side
        }
        let m = means.reduce(0, +) / Double(means.count)
        return sqrt(means.map { ($0 - m) * ($0 - m) }.reduce(0, +) / Double(means.count - 1))
    }
    for film in ["kodak_portra_400", "fujifilm_provia_100f", "kodak_tri_x_400"] {
        var r = try baseRecipe(film: film)
        r.grainEnabled = true
        // a 35 mm frame 6000 px wide (6 um pixels) and 2000 px wide (18 um)
        renderer.setImage(flat(6000, 360), width: 6000, height: 360)
        let big = try render(renderer, r).map { lab($0).x }
        let bigNegative = renderer.readTap("cmy_film")!.enumerated().filter { $0.offset % 4 == 1 }.map(\.element)
        renderer.setImage(flat(2000, 120), width: 2000, height: 120)
        let small = try render(renderer, r).map { lab($0).x }
        let atSmall = boxed(small, 2000, 120, 1), bigAtSmall = boxed(big, 6000, 360, 3)
        check(abs(bigAtSmall / atSmall - 1) < 0.1,
              String(format: "%@: grain at 18 um, export %.2f L* against preview %.2f", film, bigAtSmall, atSmall))
        // the print's grain at 0.1 mm against 24 um, as the negative's
        let print = boxed(big, 6000, 360, 16) / boxed(big, 6000, 360, 4)
        let negative = boxed(bigNegative, 6000, 360, 16) / boxed(bigNegative, 6000, 360, 4)
        check(abs(print / negative - 1) < 0.15,
              String(format: "%@: from 24 um to 0.1 mm the print keeps %.0f%% of its grain, the negative %.0f%%",
                     film, 100 * print, 100 * negative))
    }
    renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
}

print("black-and-white stocks print in silver")
do {
    let bw = ["kodak_tri_x_400", "kodak_tri_x_400_push2", "ilford_hp5_plus", "fujifilm_neopan_1600", "agfa_apx_25",
              "kodak_t_max_100"]
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
        let pitchMM = Double(Renderer.pixelSizeUm(width: side, height: rows, format: .thirtyFiveMM)) / 1000
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
        for film in bw where !["agfa_apx_25", "kodak_t_max_100"].contains(film) {
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
        // Kodak F-4016 gives T-Max 100 its own curve (D-76), far sharper than
        // Tri-X's: 109% at 5 cycles/mm, 114% at 20 and still 112% at 30.
        do {
            var r = try baseRecipe(film: "kodak_t_max_100")
            r.halationEnabled = true
            let kodak: [(f: Double, response: Double)] = [(5, 109.0), (20, 114.0), (30, 112.0)]
            let got = try kodak.map { try response(r, at: $0.f) }
            check(zip(got, kodak).allSatisfy { abs($0 - $1.response) < 5 },
                  String(format: "kodak_t_max_100: response %.0f%% at 5, %.0f%% at 20, %.0f%% at 30 cycles/mm (Kodak's 109, 114, 112)",
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

    // Grain sized to the datasheet, every stock: diffuse RMS granularity
    // through a 48 um aperture, at the density the sheet reads it at — a
    // visual density of 1.0 over the minimum (Fujifilm, Agfa; Kodak's colour
    // negatives through their Print Grain Index), a gross visual 1.0 (Kodak's
    // slides), a net silver density of 1.0 (black-and-white); see
    // tools/bake/grain.py. A flat field of the neutral that develops to it,
    // read off the developed negative and averaged over squares of the
    // aperture's area, at 6 um pixels; the records weighted by how much
    // each moves the visual density there.
    for entry in PackLibrary.shared.filmStocks {
        let pack = try PackLibrary.shared.pack(entry.stock)
        guard let sheet = pack.defaults.grain?.sheet else { check(false, "\(entry.stock) has no grain"); continue }
        let v = RenderSetupBuilder.midgrayValue * pow(10, Float(sheet.logExposure))
        let side = 6000, rows = 288
        renderer.setImage([Float](repeating: v, count: side * rows * 4).enumerated().map { $0.offset % 4 == 3 ? 1 : $0.element },
                          width: side, height: rows)
        var r = try baseRecipe(film: entry.stock)
        r.grainEnabled = true
        _ = try render(renderer, r)
        let negative = renderer.readTap("cmy_film")!
        let pitch = Renderer.pixelSizeUm(width: side, height: rows, format: r.filmFormat)
        let box = max(Int((sqrt(Float.pi) * 24 / pitch).rounded()), 1)
        var variance = 0.0
        var densities = [Double](repeating: 0, count: 3)
        for c in 0..<3 {
            var means: [Double] = []
            var y0 = 0
            while y0 + box <= rows {
                var x0 = 0
                while x0 + box <= side {
                    var sum = 0.0
                    for yy in y0..<(y0 + box) { for xx in x0..<(x0 + box) { sum += Double(negative[(yy * side + xx) * 4 + c]) } }
                    means.append(sum / Double(box * box))
                    x0 += box
                }
                y0 += box
            }
            let mean = means.reduce(0, +) / Double(means.count)
            densities[c] = mean
            // the box is not exactly the aperture's area: granularity goes as 1/sqrt(area)
            let sd = sqrt(means.map { ($0 - mean) * ($0 - mean) }.reduce(0, +) / Double(means.count - 1))
                * Double(Float(box) * pitch) / Double(sqrt(Float.pi) * 24)
            variance += pow(sheet.weights[c] * sd, 2)
        }
        let granularity = 1000 * sqrt(variance)
        check(abs(granularity - sheet.rms) < 0.1 * sheet.rms,
              String(format: "%@: RMS granularity %.1f at %@ density 1.0 (sheet %.1f; densities %.2f %.2f %.2f, the bake's %.2f %.2f %.2f)",
                     entry.stock, granularity, sheet.basis, sheet.rms, densities[0], densities[1], densities[2],
                     sheet.point[0], sheet.point[1], sheet.point[2]))
    }
    renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
}

print("slides take Tiffen's warming filters")
do {
    // Tiffen's 81C and 812, read off its catalogue's transmission curves. One
    // filter factor opens up for the green-sensitive layer, so a grey card
    // keeps its lightness while it warms: the 81C a plain amber, the 812 with
    // half the yellow and nearer pink, as the two look on film. Tiffen rounds
    // both factors to a third of a stop; its curves pass about two thirds of
    // the green, half a stop.
    let slides = PackLibrary.shared.filmStocks.filter { $0.type == "positive" }.map(\.stock)
    let g = RenderSetupBuilder.midgrayValue
    renderer.setImage([g, g, g, 1], width: 1, height: 1)
    func hue(_ c: SIMD3<Float>) -> Float { atan2(c.z, c.y) * 180 / .pi }
    for film in slides {
        let filters = try PackLibrary.shared.pack(film).lensFilters ?? []
        var r = try baseRecipe(film: film)
        let plain = lab(try render(renderer, r)[0])
        var casts: [String: SIMD3<Float>] = [:]
        for f in filters {
            r.lensFilter = f.id
            casts[f.id] = lab(try render(renderer, r)[0]) - plain
        }
        let amber = casts["81c"] ?? .zero, pinker = casts["812"] ?? .zero
        let opened = filters.map { log2($0.factor) }
        check(filters.map(\.id) == ["81c", "812"] && opened.allSatisfy { $0 > 0.3 && $0 < 0.7 }
              && abs(amber.x) < 2 && abs(pinker.x) < 2 && amber.z > 7 && pinker.z > 3 && pinker.z < amber.z
              && hue(amber) > hue(pinker) + 5,
              String(format: "%@: a grey through the 81C a* %+.1f b* %+.1f, through the 812 a* %+.1f b* %+.1f, %.0f° nearer pink; L* within %.1f, opened %.2f and %.2f stops",
                     film, amber.y, amber.z, pinker.y, pinker.z, hue(amber) - hue(pinker),
                     max(abs(amber.x), abs(pinker.x)), opened.first ?? 0, opened.last ?? 0))
    }
    renderer.setImage(greyWedge(stops), width: stops.count, height: 1)
}

print("slides, and Vericolor III, are as sharp as their sheets, and grey")
// Each slide's scatter and spreading inhibition are fitted to its datasheet's
// MTF (tools/bake/slide_mtf.py), and so are Vericolor III's, the one colour
// negative whose sheet plots one, one stop under mid-grey: the green layer's
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
        ("kodak_vericolor_iii_160", [114, 114, 95, 76]),
    ]
    let frequencies = [5.0, 10, 20, 30]
    let side = 6048, rows = 8, m = 0.02
    let pitchMM = Double(Renderer.pixelSizeUm(width: side, height: rows, format: .thirtyFiveMM)) / 1000
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
                     "fujifilm_eterna_vivid_160t", "fujifilm_eterna_500t", "fujifilm_reala_500d", "kodak_royal_gold_100",
                     "kodak_vericolor_iii_160", "harman_phoenix_200"]
    let wedge: [Float] = [-3, -2, -1, 0, 1, 2]
    renderer.setImage(greyWedge(wedge), width: wedge.count, height: 1)
    for film in negatives {
        let out = try render(renderer, try baseRecipe(film: film)).map(lab)
        let rising = zip(out, out.dropFirst()).allSatisfy { $0.x < $1.x }
        check(rising && out[0].x < 15 && out[5].x > 75 && abs(out[3].y) < 1 && abs(out[3].z) < 1,
              String(format: "%@: L* %.0f to %.0f from -3 to +2 stops, mid-grey a* %+.1f b* %+.1f",
                     film, out[0].x, out[5].x, out[3].y, out[3].z))
    }
    // HARMAN Phoenix 200 runs without DIR couplers (its curves are too steep
    // for agx's), and without an orange mask needs about twice the usual
    // yellow: the bake solves its filtration for every colour paper, where the
    // engine's own solve, started at agx's default, misses it on most.
    let phoenix = try PackLibrary.shared.pack("harman_phoenix_200")
    check(phoenix.defaults.dirCouplers == nil, "harman_phoenix_200 carries no DIR couplers")
    for entry in PackLibrary.shared.printStocks where entry.channelModel != "bw" {
        var r = try baseRecipe(film: "harman_phoenix_200")
        r.printStock = entry.stock
        let grey = try render(renderer, r).map(lab)[3]
        check(abs(grey.y) < 1 && abs(grey.z) < 1,
              String(format: "harman_phoenix_200 on %@: mid-grey a* %+.1f b* %+.1f", entry.stock, grey.y, grey.z))
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

print("the print is turned, mirrored, levelled and cropped as it is, pixel for pixel")
do {
    // Lopsided in every direction, so a turn or a mirror the wrong way shows.
    // The expressions here are kept short: GitHub's older compiler gave up
    // type-checking the long ones.
    let w = 48, h = 32
    var px = [Float](repeating: 1, count: w * h * 4)
    for y in 0..<h { for x in 0..<w {
        let o = (y * w + x) * 4
        let fx = Float(x) / Float(w), fy = Float(y) / Float(h)
        px[o] = 0.04 + 0.4 * fx
        px[o + 1] = 0.04 + 0.3 * fy
        px[o + 2] = 0.06 + 0.3 * fx * fy
    }}
    renderer.setImage(px, width: w, height: h)
    let plain = try baseRecipe(film: "kodak_portra_400")
    let base = try render(renderer, plain)
    func shaped(_ recipe: Recipe? = nil, _ edit: (inout FrameGeometry, CGSize) -> Void) throws
        -> (pixels: [SIMD3<Float>], width: Int, height: Int) {
        var r = recipe ?? plain
        var g = r.geometry
        edit(&g, g.turnedSize(width: Double(w), height: Double(h)))
        r.geometry = g
        let out = try render(renderer, r)
        return (out, Int(renderer.outputSize.width), Int(renderer.outputSize.height))
    }
    /// Whether the print `p`, `pw` x `ph`, holds at each pixel the pixel of
    /// `q`, `qw` wide, that `from` names: pixels moved, none changed.
    func moved(_ p: [SIMD3<Float>], _ pw: Int, _ ph: Int, from q: [SIMD3<Float>], _ qw: Int,
               _ from: (Int, Int) -> (Int, Int)) -> Bool {
        for y in 0..<ph {
            for x in 0..<pw {
                let (qx, qy) = from(x, y)
                if p[y * pw + x] != q[qy * qw + qx] { return false }
            }
        }
        return true
    }

    var turned4 = FrameGeometry.identity
    for _ in 0..<4 { turned4.turn(clockwise: true) }
    var mirrored2 = FrameGeometry.identity
    mirrored2.flip(horizontal: true)
    mirrored2.flip(horizontal: true)
    var none = plain
    none.geometry = turned4
    check(none.frameGeometry == nil && turned4 == .identity && mirrored2 == .identity,
          "four quarter turns, or two mirrors, are no change at all, and are stored as none")

    let right = try shaped { g, _ in g.turn(clockwise: true) }
    let rightTurned = moved(right.pixels, h, w, from: base, w) { x, y in (y, h - 1 - x) }
    check(right.width == h && right.height == w && rightTurned,
          "a quarter turn to the right is the frame turned, exactly (\(right.width)x\(right.height))")
    let left = try shaped { g, _ in g.turn(clockwise: false) }
    check(moved(left.pixels, h, w, from: base, w) { x, y in (w - 1 - y, x) }, "and to the left")
    let across = try shaped { g, _ in g.flip(horizontal: true) }
    check(moved(across.pixels, w, h, from: base, w) { x, y in (w - 1 - x, y) }, "a mirror left to right is exact")
    let down = try shaped { g, _ in g.turn(clockwise: true); g.flip(horizontal: false) }
    check(moved(down.pixels, h, w, from: base, w) { x, y in (w - 1 - y, h - 1 - x) },
          "and top to bottom, of the turned frame")

    // A burn on the frame's left side stays on it: turned, the print is the
    // unturned print turned, burn and all.
    var burn = LocalMask()
    burn.shape = .polygon
    burn.points = [CGPoint(x: 0, y: 0), CGPoint(x: 0.3, y: 0), CGPoint(x: 0.3, y: 1), CGPoint(x: 0, y: 1)]
    burn.stops = 1
    var burnRecipe = plain
    burnRecipe.masks = [burn]
    renderer.setDodgeBurnMap(MaskRasterizer.rasterize(masks: [burn], width: w, height: h))
    let burnt = try render(renderer, burnRecipe)
    let burntRight = try shaped(burnRecipe) { g, _ in g.turn(clockwise: true) }
    renderer.clearDodgeBurn()
    let middle = w * (h / 2)
    let darker = burnt[middle + 2].y < base[middle + 2].y
    let untouched = burnt[middle + w - 3] == base[middle + w - 3]
    check(darker && untouched, "(the burn darkens the frame's left side, and only that)")
    check(moved(burntRight.pixels, h, w, from: burnt, w) { x, y in (y, h - 1 - x) },
          "a burn stays where it was drawn on the frame: turned, the print is the burnt print turned")

    let crop = CGRect(x: 10, y: 6, width: 20, height: 14)
    let cut = try shaped { g, f in g.setAspect(.free, frame: f); g.setCrop(crop, frame: f) }
    let cropped = moved(cut.pixels, 20, 14, from: base, w) { x, y in (10 + x, 6 + y) }
    check(cut.width == 20 && cut.height == 14 && cropped,
          "a crop is those pixels of the print, at their own size (\(cut.width)x\(cut.height))")

    let developed = renderer.readTap("cmy_film")!
    let level = try shaped { g, _ in g.setStraighten(9) }
    let map = GeometryMap(FrameGeometry(straighten: 9), sourceWidth: w, sourceHeight: h)
    let largest = FrameGeometry.largestCentred(ratio: Double(w) / Double(h), frame: CGSize(width: w, height: h),
                                               degrees: 9)
    // Every pixel's centre at least half a pixel inside the levelled frame.
    var inside = true
    for y in 0..<level.height {
        for x in 0..<level.width {
            let s = CGPoint(x: Double(x) + 0.5, y: Double(y) + 0.5).applying(map.outputToSource)
            let margin: CGFloat = min(min(s.x, s.y), min(CGFloat(w) - s.x, CGFloat(h) - s.y))
            if margin < 0.5 { inside = false }
        }
    }
    let largestWidth = Int(largest.width), largestHeight = Int(largest.height)
    let wideEnough = (largestWidth - 2...largestWidth).contains(level.width)
    let tallEnough = (largestHeight - 2...largestHeight).contains(level.height)
    check(wideEnough && tallEnough && inside,
          "levelled 9°, the print is the largest crop of the frame's shape inside the picture, " +
          "nothing empty (\(level.width)x\(level.height))")
    // The developed negative, sampled where each levelled pixel comes from as
    // apply_geometry does: Catmull-Rom, kept within the four nearest pixels.
    let shapedNegative = renderer.readTap("shaped_cmy")!
    func weights(_ t: Float) -> [Float] {
        let t2 = t * t, t3 = t2 * t
        let w0: Float = (2 * t2 - t3 - t) / 2
        let w1: Float = (3 * t3 - 5 * t2 + 2) / 2
        let w2: Float = (4 * t2 - 3 * t3 + t) / 2
        let w3: Float = (t3 - t2) / 2
        return [w0, w1, w2, w3]
    }
    func density(_ x: Int, _ y: Int) -> SIMD3<Float> {
        let o = (min(max(y, 0), h - 1) * w + min(max(x, 0), w - 1)) * 4
        return SIMD3(developed[o], developed[o + 1], developed[o + 2])
    }
    var worst: Float = 0
    for y in 0..<level.height {
        for x in 0..<level.width {
            let s = CGPoint(x: Double(x) + 0.5, y: Double(y) + 0.5).applying(map.outputToSource)
            let px = Float(s.x) - 0.5, py = Float(s.y) - 0.5
            let fx = Int(px.rounded(.down)), fy = Int(py.rounded(.down))
            let wx = weights(px - Float(fx)), wy = weights(py - Float(fy))
            var want = SIMD3<Float>.zero
            for j in 0..<4 {
                for i in 0..<4 {
                    let weight: Float = wx[i] * wy[j]
                    want += weight * density(fx - 1 + i, fy - 1 + j)
                }
            }
            let a = density(fx, fy), b = density(fx + 1, fy), c = density(fx, fy + 1), d = density(fx + 1, fy + 1)
            let lowest = simd_min(simd_min(a, b), simd_min(c, d))
            let highest = simd_max(simd_max(a, b), simd_max(c, d))
            want = simd_clamp(want, lowest, highest)
            let o = (y * level.width + x) * 4
            let got = SIMD3<Float>(shapedNegative[o], shapedNegative[o + 1], shapedNegative[o + 2])
            let scale = simd_max(abs(want), SIMD3<Float>(repeating: 1e-3))
            let error: SIMD3<Float> = abs(got - want) / scale
            worst = max(worst, error.max())
        }
    }
    check(worst < 1e-4, String(format: "and the negative it is printed from is the frame's, levelled, within %.4f%%",
                               worst * 100))
    var whole = plain
    whole.geometry = FrameGeometry(straighten: 9)
    _ = try renderer.render(setup: try RenderSetupBuilder.build(recipe: whole), recipe: whole, wholeFrame: true)
    let screen = renderer.makeDisplayImage(colorSpaceName: "sRGB")!
    let bytes = CFDataGetBytePtr(screen.dataProvider!.data)!
    let centre = ((h / 2) * w + w / 2) * 4 + 3
    let emptyCorner = bytes[3] == 0, fullCentre = bytes[centre] == 255
    check(screen.width == w && screen.alphaInfo == .premultipliedLast && emptyCorner && fullCentre,
          "drawing the crop, the whole frame shows, its corners empty where levelling leaves them")
}

print("levelled, the print's grain is drawn on its own pixels")
do {
    // A flat grey, so nothing but the grain varies. Levelled 2.5°, the print's
    // pixels fall at every position between the frame's; grain drawn on the
    // frame and levelled with it came out 37% weaker halfway between pixels
    // than on them, a faint grid every 23 pixels.
    let w = 1200, h = 800
    var px = [Float](repeating: 1, count: w * h * 4)
    for i in 0..<(w * h) { px[i * 4] = 0.06; px[i * 4 + 1] = 0.06; px[i * 4 + 2] = 0.06 }
    renderer.setImage(px, width: w, height: h)
    var r = Recipe()
    r.autoExposure = false
    r.halationEnabled = false
    var g = FrameGeometry()
    g.setStraighten(2.5)
    r.geometry = g
    let out = try render(renderer, r)
    let ow = Int(renderer.outputSize.width), oh = Int(renderer.outputSize.height)
    let map = GeometryMap(g, sourceWidth: w, sourceHeight: h)
    var sum = [Double](repeating: 0, count: 4), squares = sum, count = sum
    for y in 2..<(oh - 2) {
        for x in 2..<(ow - 2) {
            var m: Float = 0
            for j in -1...1 { for i in -1...1 { m += out[(y + j) * ow + x + i].y } }
            let grain = Double(out[y * ow + x].y - m / 9)
            let s = CGPoint(x: Double(x) + 0.5, y: Double(y) + 0.5).applying(map.outputToSource)
            let sx = Double(s.x) - 0.5, sy = Double(s.y) - 0.5
            let fx = sx - sx.rounded(.down), fy = sy - sy.rounded(.down)
            // On the frame's pixels, or halfway between them, in each direction.
            var bin = 0
            if abs(fx - 0.5) < 0.25 { bin += 1 }
            if abs(fy - 0.5) < 0.25 { bin += 2 }
            sum[bin] += grain
            squares[bin] += grain * grain
            count[bin] += 1
        }
    }
    var deviation: [Double] = []
    for bin in 0..<4 {
        let mean = sum[bin] / count[bin]
        deviation.append(sqrt(squares[bin] / count[bin] - mean * mean))
    }
    let spread = (deviation.max() ?? 0) / max(deviation.min() ?? 1, 1e-12)
    check(spread < 1.05, String(format: "as strong on the frame's pixels as between them: %.1f%% apart", (spread - 1) * 100))
}

print("the crop holds its shape, and stays inside the picture")
do {
    let proxy = CGSize(width: 2048, height: 1365)
    func close(_ a: Double, _ b: Double, within tolerance: Double) -> Bool { abs(a - b) < tolerance }
    func ratio(_ r: CGRect) -> Double { Double(r.width) / Double(r.height) }
    var g = FrameGeometry()
    g.setAspect(.fiveFour, frame: proxy)
    g.swapOrientation(frame: proxy)
    let upright = g.cropRect(frame: proxy)
    let fullHeight = close(Double(upright.height), Double(proxy.height), within: 1e-6)
    check(fullHeight && close(ratio(upright), 0.8, within: 1e-9),
          String(format: "5:4 stood upright is the frame's full height at 4:5 (%.0f x %.0f)",
                 upright.width, upright.height))
    var moved = g
    let leftward = upright.offsetBy(dx: -300, dy: 0)
    moved.setCrop(leftward.scaled(by: 0.6, about: leftward.centre), frame: proxy)
    let full = GeometryMap(moved, sourceWidth: 6048, sourceHeight: 4032)
    let fullRatio = Double(full.outputWidth) / Double(full.outputHeight)
    let onePixel = 0.5 / Double(full.outputHeight)
    check(abs(fullRatio - 0.8) <= onePixel,
          "drawn on the proxy, it is 4:5 in whole pixels at full size too (\(full.outputWidth)x\(full.outputHeight))")
    for aspect in [CropAspect.threeTwo, .fourThree, .panoramic] {
        var a = FrameGeometry()
        a.setAspect(aspect, frame: proxy)
        a.setStraighten(-14)
        let r = a.cropRect(frame: proxy)
        let kept = close(ratio(r), aspect.longOverShort!, within: 1e-9)
        check(a.contains(r, frame: proxy) && kept,
              "\(aspect.rawValue), levelled -14°, keeps its proportions inside the picture")
    }
    var custom = FrameGeometry()
    custom.customWidth = 7
    custom.customHeight = 5
    custom.setAspect(.custom, frame: proxy)
    let seven = custom.cropRect(frame: proxy)
    custom.swapOrientation(frame: proxy)
    let five = custom.cropRect(frame: proxy)
    custom.turn(clockwise: true)
    let turnedFrame = custom.turnedSize(width: proxy.width, height: proxy.height)
    let turnedFive = custom.cropRect(frame: turnedFrame)
    let laidDown = close(ratio(seven), 1.4, within: 1e-9)
    let stoodUp = close(ratio(five), 5.0 / 7.0, within: 1e-9)
    let readsAgain = custom.customWidth == 7 && custom.customHeight == 5
    let turnedBack = close(ratio(turnedFive), 1.4, within: 1e-9)
    check(laidDown && stoodUp && readsAgain && turnedBack,
          "a custom 7:5 stands up as 5:7, and turned with its picture it reads 7:5 again")

    var drawn = FrameGeometry()
    drawn.setAspect(.free, frame: proxy)
    let corner = CGRect(x: 40, y: 30, width: 900, height: 700)
    drawn.setCrop(corner, frame: proxy)
    drawn.setStraighten(12)
    let shrunk = drawn.cropRect(frame: proxy)
    drawn.setStraighten(0)
    let back = drawn.cropRect(frame: proxy)
    let shrank = shrunk.width < corner.width && drawn.contains(shrunk, frame: proxy)
    let sameCorner = close(Double(back.minX), Double(corner.minX), within: 1e-6)
        && close(Double(back.maxY), Double(corner.maxY), within: 1e-6)
    let sameSize = close(Double(back.width), Double(corner.width), within: 1e-6)
        && close(Double(back.height), Double(corner.height), within: 1e-6)
    check(shrank && sameCorner && sameSize, "levelling shrinks a crop as far as it must, and levelling back gives it back")
    var turned = drawn
    turned.turn(clockwise: true)
    turned.flip(horizontal: true)
    check(turned.upright == drawn, "a crop turned and mirrored with its picture is still the same crop")
}

print("the meters read only what the crop keeps")
do {
    // Dark on the left, light on the right.
    let w = 120, h = 80
    var px = [Float](repeating: 1, count: w * h * 4)
    for y in 0..<h { for x in 0..<w {
        let side: Float = x < w / 2 ? 0.01 : 0.3
        let ripple: Float = 1 + 0.2 * Float(y % 7) / 7
        let o = (y * w + x) * 4
        px[o] = side * ripple; px[o + 1] = side * ripple; px[o + 2] = side * ripple
    }}
    let film = try PackLibrary.shared.pack("kodak_portra_400")
    func ev(_ g: FrameGeometry) -> Float {
        let area = g.metered(px, width: w, height: h)
        return RenderSetupBuilder.autoExposureEV(pixels: area.pixels, width: area.width, height: area.height, film: film)
    }
    var lightHalf = FrameGeometry()
    lightHalf.setAspect(.free, frame: CGSize(width: w, height: h))
    lightHalf.setCrop(CGRect(x: 60, y: 0, width: 60, height: 80), frame: CGSize(width: w, height: h))
    var half: [Float] = []
    for y in 0..<h {
        let row = y * w * 4
        half.append(contentsOf: px[(row + 60 * 4)..<(row + w * 4)])
    }
    let alone = RenderSetupBuilder.autoExposureEV(pixels: half, width: 60, height: h, film: film)
    let cropped = ev(lightHalf), whole = ev(.identity)
    check(cropped == alone && cropped < whole - 0.5,
          String(format: "cropped to the light half, they read it as if it were the whole frame: %+.2f EV, not %+.2f",
                 cropped, whole))
    var turned = lightHalf
    turned.turn(clockwise: true)
    turned.flip(horizontal: false)
    check(ev(turned) == cropped, "turning or mirroring the print does not move them")
    var quarter = FrameGeometry.identity
    quarter.turn(clockwise: false)
    check(ev(quarter) == whole, "nor does turning a print that is not cropped")
}

print(failures == 0 ? "\nall checks passed" : "\n\(failures) check(s) failed")
exit(failures == 0 ? 0 : 1)
