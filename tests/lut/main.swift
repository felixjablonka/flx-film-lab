import Foundation
import simd
@testable import FLXCore

// Checks for the LUT baker: each log curve against its maker's published values,
// that the lattice comes out in the order a .cube and a 3D texture expect,
// that exposure in the LUT is scene exposure, and how far interpolating the
// LUT strays from rendering every pixel through the engine.

var failures = 0
func check(_ ok: Bool, _ message: String) {
    print("   \(ok ? "ok  " : "FAIL") \(message)")
    if !ok { failures += 1 }
}

func nlog(_ linear: SIMD3<Double>) -> SIMD3<Float> {
    SIMD3(Float(NLog.code(linear: linear.x)), Float(NLog.code(linear: linear.y)), Float(NLog.code(linear: linear.z)))
}

// MARK: - The curve

print("N-Log curve (Nikon N-Log Specification 1.0.0)")
check(abs(NLog.code(linear: 0.18) - 0.36366777) < 1e-7,
      String(format: "18%% grey encodes to %.8f (colour-science: 0.36366777)", NLog.code(linear: 0.18)))
check(abs(NLog.greyCode - 372) < 0.5,
      String(format: "18%% grey records at code %.1f (Nikon's primer: around 372, 35%% IRE)", NLog.greyCode))
let blackCode = NLog.code(linear: 0) * 1023
check(abs(blackCode - 127.2) < 0.1, String(format: "scene black records at code %.1f", blackCode))
let top = NLog.linear(code: 1)
check(abs(top - 14.776) < 0.01,
      String(format: "code 1023 is reflectance %.3f, %.2f stops over grey", top, log2(top / 0.18)))
var worstRoundTrip = 0.0
var monotonic = true
var previous = -Double.infinity
for code in 0...1023 {
    let v = Double(code) / 1023
    let y = NLog.linear(code: v)
    worstRoundTrip = max(worstRoundTrip, abs(NLog.code(linear: y) - v) * 1023)
    if y <= previous { monotonic = false }
    previous = y
}
check(monotonic, "decoding rises with every code value, across the cube-root / log break")
check(worstRoundTrip < 0.25,
      String(format: "decode then encode returns every code value (worst %.3f codes)", worstRoundTrip))
let greyVideo = LookLUT.Input.nlogVideoLevels.linear((372 - 64) / 876.0)
check(abs(greyVideo - 0.18) < 0.002,
      String(format: "video levels: 35%% signal decodes to %.4f", greyVideo))

print("Apple Log curve (Apple Log Profile White Paper, 2023)")
check(abs(AppleLog.signal(linear: 0.18) - 0.48827245852686763) < 1e-9,
      String(format: "18%% grey encodes to %.8f (colour-science: 0.48827246)", AppleLog.signal(linear: 0.18)))
check(abs(AppleLog.linear(signal: 0.48827245852686763) - 0.18) < 1e-9, "and decodes back to 0.18")
check(abs(AppleLog.signal(linear: 0) - 0.1505) < 0.0005,
      String(format: "scene black sits at %.4f", AppleLog.signal(linear: 0)))
let appleTop = AppleLog.linear(signal: 1)
check(abs(appleTop - 11.99) < 0.02,
      String(format: "the top of the signal is reflectance %.2f, %.2f stops over grey", appleTop, log2(appleTop / 0.18)))
var appleWorst = 0.0
var appleRising = true
var applePrevious = -Double.infinity
for i in 0...1023 {
    let p = Double(i) / 1023
    let r = AppleLog.linear(signal: p)
    appleWorst = max(appleWorst, abs(AppleLog.signal(linear: r) - p))
    if r < applePrevious { appleRising = false }
    applePrevious = r
}
check(appleRising, "decoding never falls, across the square-law / log break")
check(appleWorst < 1e-9, String(format: "decode then encode returns every signal value (worst %.1e)", appleWorst))

// MARK: - The bake

var recipe = Recipe()
recipe.filmStock = "kodak_portra_800"
recipe.printStock = "kodak_portra_endura"
recipe.autoExposure = false

let baker = try LookBaker()
let output = LookLUT.Output.bt1886

print("lattice order")
do {
    let n = 17
    let lut = try baker.bake(recipe: recipe, size: n, input: .nlogCode, output: output)
    let picks: [(Int, Int, Int)] = [(16, 0, 0), (0, 16, 0), (0, 0, 16), (3, 9, 14), (12, 2, 7), (8, 8, 8)]
    let points = picks.map { SIMD3(Float($0.0) / 16, Float($0.1) / 16, Float($0.2) / 16) }
    let direct = try baker.direct(points, recipe: recipe, input: .nlogCode, output: output)
    var worst: Float = 0
    for (k, p) in picks.enumerated() {
        worst = max(worst, simd_reduce_max(simd_abs(lut.value(p.0, p.1, p.2) - direct[k])))
    }
    check(worst < 1e-5, String(format: "lattice points match the engine rendered directly (max %.1e) — red fastest, blue slowest", worst))
    let red = lut.value(16, 0, 0), blue = lut.value(0, 0, 16)
    check(red.x > red.z && blue.z > blue.x, "the red corner prints red and the blue corner blue")
}

print("grey card")
let lut65 = try baker.bake(recipe: recipe, size: 65, input: .nlogCode, output: output)
do {
    let grey = SIMD3<Float>(repeating: Float(NLog.code(linear: 0.18)))
    let g = lab(lut65.sample(grey), output)
    let d = lab(try baker.direct([grey], recipe: recipe, input: .nlogCode, output: output)[0], output)
    print(String(format: "      Portra 800 on Portra Endura: L* %.1f  a* %+.2f  b* %+.2f", g.x, g.y, g.z))
    check(abs(g.y) < 2 && abs(g.z) < 2, "an 18% grey card prints neutral")
    check(g.x > 40 && g.x < 60, "and near the middle of the scale")
    check(deltaE2000(g, d) < 0.25, String(format: "the LUT's grey is the engine's, off the lattice (dE %.3f)", deltaE2000(g, d)))
}

print("neutral scale")
do {
    let stops = stride(from: -7.0, through: 6.0, by: 0.25).map { $0 }
    let points = stops.map { SIMD3<Float>(repeating: Float(NLog.code(linear: 0.18 * pow(2, $0)))) }
    let direct = try baker.direct(points, recipe: recipe, input: .nlogCode, output: output).map { lab($0, output) }
    var rising = true
    for i in 1..<direct.count where direct[i].x <= direct[i - 1].x - 1e-6 { rising = false }
    check(rising, "lightness never falls as the scene gets brighter, -7 to +6 stops")
    let chroma = direct.map { hypot($0.y, $0.z) }
    let worst = chroma.indices.max(by: { chroma[$0] < chroma[$1] })!
    print(String(format: "      paper black L* %.1f, +6 stops L* %.1f; most colourful grey C* %.2f at %+.2f stops",
                 direct.first!.x, direct.last!.x, chroma[worst], stops[worst]))
    check(chroma[worst] < 6, "greys stay near neutral from end to end (film crossover is allowed some colour)")
}

print("exposure is scene exposure")
do {
    var plus = recipe
    plus.exposureEV = 1
    let scene: [SIMD3<Double>] = [SIMD3(0.18, 0.18, 0.18), SIMD3(0.05, 0.03, 0.02),
                                  SIMD3(0.30, 0.20, 0.15), SIMD3(0.04, 0.10, 0.25), SIMD3(1.2, 1.1, 1.0)]
    let once = try baker.direct(scene.map(nlog), recipe: plus, input: .nlogCode, output: output)
    let doubled = try baker.direct(scene.map { nlog($0 * 2) }, recipe: recipe, input: .nlogCode, output: output)
    var worst: Float = 0
    for i in scene.indices { worst = max(worst, simd_reduce_max(simd_abs(once[i] - doubled[i]))) }
    check(worst < 2e-4, String(format: "+1 EV prints what twice the light prints at 0 EV (max %.1e)", worst))
}

print("interpolation error, dE2000 against the engine per pixel")
do {
    var rng = SplitMix(state: 0xA6C5_7D10)
    // Scene-like colours: anything inside Rec.709, from 7 stops under grey to
    // 5 over, which covers real footage including saturated light.
    var scene: [SIMD3<Float>] = []
    for _ in 0..<4096 {
        let c709 = SIMD3(rng.next(), rng.next(), rng.next())
        let level = 0.18 * pow(2, -7 + 12 * rng.next()) / max(c709.max(), 1e-3)
        scene.append(nlog(rec709To2020(c709 * level)))
    }
    // Every code value combination, most of which no camera records.
    var cube: [SIMD3<Float>] = []
    for _ in 0..<4096 { cube.append(SIMD3(Float(rng.next()), Float(rng.next()), Float(rng.next()))) }

    let lut33 = try baker.bake(recipe: recipe, size: 33, input: .nlogCode, output: output)
    let lut65b = try baker.bake(recipe: recipe, size: 65, input: .nlogCode, output: output)
    let sceneDirect = try baker.direct(scene, recipe: recipe, input: .nlogCode, output: output)
    let cubeDirect = try baker.direct(cube, recipe: recipe, input: .nlogCode, output: output)

    func errors(_ lut: LookLUT, _ points: [SIMD3<Float>], _ truth: [SIMD3<Float>]) -> Errors {
        Errors(values: points.indices.map { deltaE2000(lab(truth[$0], output), lab(lut.sample(points[$0]), output)) })
    }
    let s65 = errors(lut65b, scene, sceneDirect), s33 = errors(lut33, scene, sceneDirect)
    let c65 = errors(lut65b, cube, cubeDirect), c33 = errors(lut33, cube, cubeDirect)
    print("      scene colours  65^3: \(s65.summary)")
    print("      scene colours  33^3: \(s33.summary)")
    print("      whole cube     65^3: \(c65.summary)")
    print("      whole cube     33^3: \(c33.summary)")
    check(s65.percentile(0.99) < 0.5 && s65.largest < 1.5, "65^3 is indistinguishable on scene colours (p99 < 0.5, max < 1.5)")
    check(s33.percentile(0.99) < 1.5, "33^3 is fine for monitoring (p99 < 1.5)")
}

print(".cube file")
do {
    let lut = try baker.bake(recipe: recipe, size: 17, input: .nlogVideoLevels, output: output)
    let text = lut.cubeText(title: "Kodak Portra 800 on Kodak Portra Endura", comments: ["test"])
    let lines = text.split(separator: "\n", omittingEmptySubsequences: false)
    check(lines.contains("LUT_3D_SIZE 17"), "declares its size")
    let rows = lines.compactMap { line -> SIMD3<Float>? in
        let parts = line.split(separator: " ")
        guard parts.count == 3, let r = Float(parts[0]), let g = Float(parts[1]), let b = Float(parts[2]) else { return nil }
        return SIMD3(r, g, b)
    }
    check(rows.count == 17 * 17 * 17, "carries \(rows.count) rows (want \(17 * 17 * 17))")
    var worst: Float = 0
    for i in 0..<min(rows.count, 17 * 17 * 17) {
        worst = max(worst, simd_reduce_max(simd_abs(rows[i] - SIMD3(lut.values[i * 3], lut.values[i * 3 + 1], lut.values[i * 3 + 2]))))
    }
    check(worst <= 1e-6, String(format: "reads back to within %.1e (six decimals)", worst))
    check(text.allSatisfy(\.isASCII), "is plain ASCII")
}

print("only per-pixel controls bake")
do {
    var spatial = recipe
    spatial.halationEnabled = true
    spatial.grainEnabled = true
    spatial.couplersDiffusion = 1
    spatial.autoGrade = true
    var mask = LocalMask()
    mask.shape = .oval
    mask.points = [CGPoint(x: 0.5, y: 0.5), CGPoint(x: 0.7, y: 0.5), CGPoint(x: 0.5, y: 0.7)]
    mask.stops = 1
    spatial.masks = [mask]
    var effects = GlowLook.off
    effects.bloom.enabled = true
    effects.glow.enabled = true
    spatial.effects = effects
    let plain = try baker.bake(recipe: recipe, size: 17, input: .nlogCode, output: output)
    let withSpatial = try baker.bake(recipe: spatial, size: 17, input: .nlogCode, output: output)
    check(plain.values == withSpatial.values, "halation, grain, coupler diffusion, masks, bloom, glow and the meters leave the LUT unchanged")
}

print("speed")
do {
    let fresh = try LookBaker()
    var t = CFAbsoluteTimeGetCurrent()
    _ = try fresh.bake(recipe: recipe, size: 65)
    let first = (CFAbsoluteTimeGetCurrent() - t) * 1000
    var r = recipe
    r.brightness = 0.05
    t = CFAbsoluteTimeGetCurrent()
    _ = try fresh.bake(recipe: r, size: 65)
    let printOnly = (CFAbsoluteTimeGetCurrent() - t) * 1000
    r.exposureEV = 0.5
    t = CFAbsoluteTimeGetCurrent()
    _ = try fresh.bake(recipe: r, size: 65)
    let film = (CFAbsoluteTimeGetCurrent() - t) * 1000
    print(String(format: "      65^3: first bake %.0f ms, print control %.0f ms, exposure %.0f ms", first, printOnly, film))
    check(printOnly < 150 && film < 250, "re-baking after a slider move is interactive")
}

print("Apple Log: Portra 800 on Portra Endura, Fujifilm C200 on Crystal Archive Type II")
do {
    var portra = Recipe()
    portra.filmStock = "kodak_portra_800"
    portra.printStock = "kodak_portra_endura"
    var c200 = Recipe()
    c200.filmStock = "fujifilm_c200"
    c200.printStock = "fujifilm_crystal_archive_typeii"
    let apple = LookLUT.Output.appleDisplay
    var rng = SplitMix(state: 0x1F0E_5A55)
    var scene: [SIMD3<Double>] = []
    for _ in 0..<4096 {
        let c709 = SIMD3(rng.next(), rng.next(), rng.next())
        scene.append(rec709To2020(c709 * (0.18 * pow(2, -7 + 12 * rng.next()) / max(c709.max(), 1e-3))))
    }
    func appleLog(_ c: SIMD3<Double>) -> SIMD3<Float> {
        SIMD3(Float(AppleLog.signal(linear: c.x)), Float(AppleLog.signal(linear: c.y)), Float(AppleLog.signal(linear: c.z)))
    }
    let points = scene.map(appleLog)
    for (name, look) in [("Portra 800 / Endura", portra), ("C200 / Crystal Archive II", c200)] {
        let lut65 = try baker.bake(recipe: look, size: 65, input: .appleLog, output: apple)
        let lut33 = try baker.bake(recipe: look, size: 33, input: .appleLog, output: apple)
        let grey = lab(lut65.sample(SIMD3(repeating: Float(AppleLog.signal(linear: 0.18)))), apple)
        print(String(format: "      %@: grey card L* %.1f a* %+.2f b* %+.2f", name, grey.x, grey.y, grey.z))
        check(abs(grey.y) < 2 && abs(grey.z) < 2 && grey.x > 40 && grey.x < 60,
              "\(name): an 18% grey card prints a neutral mid-grey")
        let truth = try baker.direct(points, recipe: look, input: .appleLog, output: apple)
        let e65 = Errors(values: points.indices.map { deltaE2000(lab(truth[$0], apple), lab(lut65.sample(points[$0]), apple)) })
        let e33 = Errors(values: points.indices.map { deltaE2000(lab(truth[$0], apple), lab(lut33.sample(points[$0]), apple)) })
        print("      65^3: \(e65.summary)   33^3: \(e33.summary)")
        check(e65.percentile(0.99) < 0.5 && e33.percentile(0.99) < 1.5,
              "\(name): interpolates as closely from Apple Log as from N-Log")
        // The same scene through either camera's curve prints the same, up to
        // the engine's float32 arithmetic along the two decode paths.
        let viaNikon = try baker.direct(scene.map(nlog), recipe: look, input: .nlogCode, output: apple)
        let worst = points.indices.map { deltaE2000(lab(truth[$0], apple), lab(viaNikon[$0], apple)) }.max() ?? 0
        check(worst < 0.05, String(format: "\(name): the same scene prints the same from Apple Log and N-Log (dE %.3f)", worst))
    }
}

print("Apple Log 2 and Apple Wide Gamut (iPhone 17 Pro and later)")
do {
    let white = AppleLog.wideGamutToBT2020 * SIMD3<Double>(1, 1, 1)
    check(simd_reduce_max(simd_abs(white - 1)) < 1e-9,
          String(format: "Apple Wide Gamut white is BT.2020 white (%.6f %.6f %.6f)", white.x, white.y, white.z))
    func xy(_ c: SIMD3<Double>) -> SIMD2<Double> {
        let xyz = AppleLog.wideGamutToXYZ * c
        return SIMD2(xyz.x, xyz.y) / (xyz.x + xyz.y + xyz.z)
    }
    let corners = [xy(SIMD3(1, 0, 0)), xy(SIMD3(0, 1, 0)), xy(SIMD3(0, 0, 1))]
    let published = [AppleLog.wideGamutRed, AppleLog.wideGamutGreen, AppleLog.wideGamutBlue]
    check(zip(corners, published).allSatisfy { simd_distance($0, $1) < 1e-9 },
          String(format: "the matrix puts the primaries where Apple does: blue (%.3f, %.3f)", corners[2].x, corners[2].y))

    // OpenColorIO's own test of its built-in Apple Log 2 transform
    // (tests/cpu/transforms/BuiltinTransform_tests.cpp): the signal
    // (0.5, 0.4, 0.3) to ACES2065-1, adapted by Bradford from D65 to the ACES white.
    let acesWhite = SIMD2(0.32168, 0.33767)
    let ap0ToXYZ = Primaries.rgbToXYZ(red: SIMD2(0.7347, 0.2653), green: SIMD2(0, 1),
                                     blue: SIMD2(0.0001, -0.0770), white: acesWhite)
    let bradford = simd_double3x3(rows: [SIMD3(0.8951, 0.2664, -0.1614),
                                         SIMD3(-0.7502, 1.7135, 0.0367),
                                         SIMD3(0.0389, -0.0685, 1.0296)])
    func whiteXYZ(_ c: SIMD2<Double>) -> SIMD3<Double> { SIMD3(c.x / c.y, 1, (1 - c.x - c.y) / c.y) }
    let gain = (bradford * whiteXYZ(acesWhite)) / (bradford * whiteXYZ(AppleLog.d65))
    let adapt = bradford.inverse * simd_double3x3(diagonal: gain) * bradford
    let signal = SIMD3(0.5, 0.4, 0.3)
    let light = SIMD3(AppleLog.linear(signal: signal.x), AppleLog.linear(signal: signal.y), AppleLog.linear(signal: signal.z))
    let aces = ap0ToXYZ.inverse * adapt * AppleLog.wideGamutToXYZ * light
    let ocio = SIMD3(0.160302015, 0.091223177, 0.026405713)
    check(simd_reduce_max(simd_abs(aces - ocio)) < 2e-6,
          String(format: "signal (0.5, 0.4, 0.3) is ACES (%.6f %.6f %.6f), as in OpenColorIO's test", aces.x, aces.y, aces.z))

    // The same scene recorded in Apple Log 2 prints what it prints from N-Log.
    var portra = Recipe()
    portra.filmStock = "kodak_portra_800"
    portra.printStock = "kodak_portra_endura"
    let apple = LookLUT.Output.appleDisplay
    let toWideGamut = AppleLog.wideGamutToBT2020.inverse
    var rng = SplitMix(state: 0xA1E_2025)
    var scene: [SIMD3<Double>] = []
    for _ in 0..<4096 {
        let c709 = SIMD3(rng.next(), rng.next(), rng.next())
        scene.append(rec709To2020(c709 * (0.18 * pow(2, -7 + 12 * rng.next()) / max(c709.max(), 1e-3))))
    }
    func appleLog2(_ c: SIMD3<Double>) -> SIMD3<Float> {
        let w = toWideGamut * c
        return SIMD3(Float(AppleLog.signal(linear: w.x)), Float(AppleLog.signal(linear: w.y)), Float(AppleLog.signal(linear: w.z)))
    }
    let points = scene.map(appleLog2)
    let truth = try baker.direct(points, recipe: portra, input: .appleLog2, output: apple)
    let viaNikon = try baker.direct(scene.map(nlog), recipe: portra, input: .nlogCode, output: apple)
    let same = Errors(values: points.indices.map { deltaE2000(lab(truth[$0], apple), lab(viaNikon[$0], apple)) })
    print("      Apple Log 2 against N-Log, same scene: \(same.summary)")
    check(same.largest < 0.05, String(format: "the same scene prints the same from Apple Log 2 and N-Log (dE %.3f)", same.largest))
    let lut33 = try baker.bake(recipe: portra, size: 33, input: .appleLog2, output: apple)
    let e33 = Errors(values: points.indices.map { deltaE2000(lab(truth[$0], apple), lab(lut33.sample(points[$0]), apple)) })
    print("      33^3 from Apple Log 2: \(e33.summary)")
    check(e33.percentile(0.99) < 1.5, "a 33-point table is close enough to shoot with")
    // An Apple Log table reads Apple Wide Gamut as BT.2020.
    let logTable = try baker.bake(recipe: portra, size: 33, input: .appleLog, output: apple)
    let misread = Errors(values: points.indices.map { deltaE2000(lab(truth[$0], apple), lab(logTable.sample(points[$0]), apple)) })
    print("      Log 2 footage through an Apple Log table: \(misread.summary)")
}

print("V-Log and V-Gamut (Panasonic V-Log/V-Gamut Reference Manual)")
do {
    check(abs(VLog.code(linear: 0.18) - 0.4233114) < 1e-6,
          String(format: "18%% grey encodes to %.7f (colour-science: 0.4233114)", VLog.code(linear: 0.18)))
    check(abs(VLog.code(linear: 0.18) * 1023 - 433) < 0.5,
          String(format: "and records at code %.1f, 42%% IRE", VLog.code(linear: 0.18) * 1023))
    check(abs(VLog.code(linear: 0) * 1023 - 127.9) < 0.1, String(format: "scene black at code %.1f", VLog.code(linear: 0) * 1023))
    var worst = 0.0, rising = true, previous = -Double.infinity
    for code in 0...1023 {
        let v = Double(code) / 1023, y = VLog.linear(code: v)
        worst = max(worst, abs(VLog.code(linear: y) - v) * 1023)
        if y <= previous { rising = false }
        previous = y
    }
    check(rising && worst < 1e-9, "decodes rising and round-trips every code value")
    let white = VLog.toBT2020 * SIMD3<Double>(1, 1, 1)
    check(simd_reduce_max(simd_abs(white - 1)) < 2e-3, String(format: "V-Gamut white is BT.2020 white (%.4f %.4f %.4f)", white.x, white.y, white.z))
    func xy(_ c: SIMD3<Double>) -> SIMD2<Double> { let xyz = VLog.toXYZ * c; return SIMD2(xyz.x, xyz.y) / (xyz.x + xyz.y + xyz.z) }
    let r = xy(SIMD3(1, 0, 0)), b = xy(SIMD3(0, 0, 1))
    check(simd_distance(r, SIMD2(0.730, 0.280)) < 2e-3 && simd_distance(b, SIMD2(0.100, -0.030)) < 2e-3,
          String(format: "primaries where Panasonic puts them: red (%.3f, %.3f), blue (%.3f, %.3f)", r.x, r.y, b.x, b.y))
    // A video-range file as Resolve hands it to a LUT: (code - 64) / 876.
    let grey = LookLUT.Input.vlogVideoLevels.linear((VLog.code(linear: 0.18) * 1023 - 64) / 876)
    check(abs(grey - 0.18) < 1e-9, String(format: "video levels read the 18%% grey back as %.6f", grey))
}

print("Panasonic S9: V-Log in, sRGB out, 33 points")
do {
    let rec709ToXYZ = simd_double3x3(rows: [SIMD3(0.4124564, 0.3575761, 0.1804375),
                                            SIMD3(0.2126729, 0.7151522, 0.0721750),
                                            SIMD3(0.0193339, 0.1191920, 0.9503041)])
    let toVGamut = VLog.toXYZ.inverse * rec709ToXYZ
    /// A scene colour given in linear Rec.709, as the S9 records it.
    func vlog(_ c709: SIMD3<Double>, stops: Double = 0) -> SIMD3<Float> {
        let v = toVGamut * (c709 * pow(2, stops))
        return SIMD3(Float(VLog.code(linear: v.x)), Float(VLog.code(linear: v.y)), Float(VLog.code(linear: v.z)))
    }
    let srgb = LookLUT.Output.sRGB
    func recipe(_ film: String, _ paper: String?, _ change: (inout Recipe) -> Void = { _ in }) -> Recipe {
        var r = Recipe()
        r.filmStock = film
        if let paper { r.printStock = paper }
        change(&r)
        return r
    }
    let grey = SIMD3<Double>(repeating: 0.18)
    for (name, look) in [("Ektar 100", recipe("kodak_ektar_100", "kodak_portra_endura")),
                         ("Portra 800", recipe("kodak_portra_800", "kodak_portra_endura")),
                         ("C200", recipe("fujifilm_c200", "fujifilm_crystal_archive_typeii")),
                         ("Velvia 50", recipe("fujifilm_velvia_50", nil))] {
        let lut = try baker.bake(recipe: look, size: 33, input: .vlog, output: srgb)
        let g = lab(lut.sample(vlog(grey)), srgb)
        check(abs(g.y) < 2.5 && abs(g.z) < 2.5 && g.x > 40 && g.x < 62,
              String(format: "%@: a grey card at code 433 prints a neutral mid-grey (L* %.1f a* %+.1f b* %+.1f)", name, g.x, g.y, g.z))
    }

    // Tri-X: silver is neutral by construction; filters move colours, not grey.
    let red = SIMD3<Double>(0.40, 0.04, 0.03), blue = SIMD3<Double>(0.03, 0.06, 0.40), green = SIMD3<Double>(0.05, 0.30, 0.05)
    var trix: [String: (grey: Double, red: Double, blue: Double, green: Double, chroma: Double)] = [:]
    for filter in [nil, "blue", "green", "yellow", "red"] as [String?] {
        let lut = try baker.bake(recipe: recipe("kodak_tri_x_400", "ilford_multigrade_rc_deluxe") { $0.lensFilter = filter },
                                 size: 33, input: .vlog, output: srgb)
        let patches = [grey, red, blue, green].map { lab(lut.sample(vlog($0)), srgb) }
        trix[filter ?? "none"] = (patches[0].x, patches[1].x, patches[2].x, patches[3].x,
                                  patches.map { hypot($0.y, $0.z) }.max()!)
    }
    let none = trix["none"]!
    print("      Tri-X L*:  filter   grey   red   blue  green")
    for name in ["none", "blue", "green", "yellow", "red"] {
        let t = trix[name]!
        print(String(format: "                 %-7@ %5.1f %5.1f %6.1f %6.1f", name as NSString, t.grey, t.red, t.blue, t.green))
    }
    check(trix.values.allSatisfy { $0.chroma < 0.5 }, "Tri-X prints every colour as a neutral grey")
    check(trix.values.allSatisfy { abs($0.grey - none.grey) < 1.5 }, "each filter's factor keeps the grey card where it was")
    let r = trix["red"]!, y = trix["yellow"]!, b = trix["blue"]!, gr = trix["green"]!
    check(r.red > none.red + 5 && r.blue < none.blue - 5, "red filter: reds lighter, blues (sky) much darker")
    check(y.blue < none.blue - 2 && y.blue > r.blue, "yellow filter: blues darker, less than with red")
    check(b.blue > none.blue + 3 && b.red < none.red - 3, "blue filter: blues lighter, reds darker")
    check(gr.green > none.green + 3 && gr.red < none.red, "green filter: greens (foliage) lighter, reds darker")

    // Max DR: the camera at -1 EV records scene stop s at s - 1. The LUT's +1 EV
    // and softer print put midtones back and roll the extra stop into paper white.
    // The S9's sensor (the Zf's and S5's) holds about +6 stops over grey at either
    // native V-Log ISO, so shot at -1 EV its clip is scene +7.
    let normal = try baker.bake(recipe: recipe("kodak_portra_800", "kodak_portra_endura"), size: 33, input: .vlog, output: srgb)
    let maxDR = try baker.bake(recipe: recipe("kodak_portra_800", "kodak_portra_endura") {
        $0.exposureEV = 1; $0.grade = 134; $0.shadowGrade = -16 }, size: 33, input: .vlog, output: srgb)
    let plusOne = try baker.bake(recipe: recipe("kodak_portra_800", "kodak_portra_endura") { $0.exposureEV = 1 },
                                 size: 33, input: .vlog, output: srgb)
    let midtones = stride(from: -2.0, through: 1.0, by: 0.5).map {
        abs(lab(maxDR.sample(vlog(grey, stops: $0 - 1)), srgb).x - lab(normal.sample(vlog(grey, stops: $0)), srgb).x)
    }.max()!
    func l(_ lut: LookLUT, _ scene: Double) -> Double { lab(lut.sample(vlog(grey, stops: scene - 1)), srgb).x }
    let spread = l(maxDR, 5.5) - l(maxDR, 3.5), plain = l(plusOne, 5.5) - l(plusOne, 3.5)
    check(midtones < 2.5, String(format: "Max DR shot at -1 EV: midtones within %.1f L* of the normal LUT shot as metered", midtones))
    check(l(maxDR, 7) > 91.5 && l(maxDR, 7) >= l(maxDR, 5.5),
          String(format: "the S9's clip (scene +7) lands on paper white, L* %.1f", l(maxDR, 7)))
    let upper = l(maxDR, 5) - l(maxDR, 2), upperNormal = lab(normal.sample(vlog(grey, stops: 5)), srgb).x
        - lab(normal.sample(vlog(grey, stops: 2)), srgb).x
    check(upper > upperNormal + 2,
          String(format: "scene +2 to +5 spreads over %.1f L* (normal table: %.1f)", upper, upperNormal))
    // Paper has a fixed range, so more stops in it means the existing ones
    // a little tighter: the softer print buys about twice the gradation.
    check(spread > 1.6 * plain && spread > 3,
          String(format: "scene +3.5 to +5.5 keeps %.1f L* of gradation (+1 EV alone: %.1f)", spread, plain))

    // Interpolation from V-Log, scene colours inside Rec.709 from -7 to +5 stops.
    var rng = SplitMix(state: 0x5E9_1234)
    var points: [SIMD3<Float>] = []
    for _ in 0..<4096 {
        let c = SIMD3(rng.next(), rng.next(), rng.next())
        points.append(vlog(c * (0.18 * pow(2, -7 + 12 * rng.next()) / max(c.max(), 1e-3))))
    }
    let portra = recipe("kodak_portra_800", "kodak_portra_endura")
    let lut33 = try baker.bake(recipe: portra, size: 33, input: .vlog, output: srgb)
    let truth = try baker.direct(points, recipe: portra, input: .vlog, output: srgb)
    let e = Errors(values: points.indices.map { deltaE2000(lab(truth[$0], srgb), lab(lut33.sample(points[$0]), srgb)) })
    print("      33^3 from V-Log: \(e.summary)")
    check(e.percentile(0.99) < 1.5, "a 33-point table is close enough for a camera's own rendering")
}

print("F-Log, F-Log2, Canon Log 2 and 3, S-Log3 and I-Log (each maker's document)")
do {
    // colour-science 0.4.7: the 18% grey's normalised code value, and each
    // gamut's matrix into BT.2020 (`matrix_RGB_to_RGB`). colour-science has
    // no I-Log: its grey is the white paper's own table, to its six decimals.
    struct Case {
        let name: String
        let input: LookLUT.Input, video: LookLUT.Input
        let encode: (Double) -> Double
        let grey: Double, black: Double
        let primaries: (red: SIMD2<Double>, green: SIMD2<Double>, blue: SIMD2<Double>)?
        let toBT2020: [Double]?
        var greyFrom = "colour-science"
        var greyTolerance = 1e-9
    }
    let cases = [
        Case(name: "F-Log", input: .flog, video: .flogVideoLevels, encode: FLog.flog.code(linear:),
             grey: 0.4593184587, black: 95.00, primaries: nil, toBT2020: nil),
        Case(name: "F-Log2", input: .flog2, video: .flog2VideoLevels, encode: FLog.flog2.code(linear:),
             grey: 0.3910072419, black: 95.00, primaries: nil, toBT2020: nil),
        Case(name: "Canon Log 2", input: .clog2, video: .clog2VideoLevels, encode: CanonLog.code2(linear:),
             grey: 0.3982546926, black: 95.00, primaries: CanonLog.cinemaGamut,
             toBT2020: [1.1387361045, -0.0267685345, -0.1119675701, -0.0552075941, 1.3155732943, -0.2603657002,
                        -0.0076594111, -0.2576976173, 1.2653570284]),
        Case(name: "Canon Log 3", input: .clog3, video: .clog3VideoLevels, encode: CanonLog.code3(linear:),
             grey: 0.3433893704, black: 128.00, primaries: CanonLog.cinemaGamut, toBT2020: nil),
        Case(name: "S-Log3 (S-Gamut3.Cine)", input: .slog3Cine, video: .slog3CineVideoLevels, encode: SLog3.code(linear:),
             grey: 0.4105571848, black: 95.00, primaries: SLog3.sGamut3Cine,
             toBT2020: [0.9600463439, 0.1195329788, -0.0795793226, -0.0522394796, 1.2643057472, -0.2120662678,
                        -0.0288405067, -0.0595209682, 1.0883614749]),
        Case(name: "S-Log3 (S-Gamut3)", input: .slog3, video: .slog3VideoLevels, encode: SLog3.code(linear:),
             grey: 0.4105571848, black: 95.00, primaries: SLog3.sGamut3,
             toBT2020: [1.1188568767, -0.0598319380, -0.0590249386, -0.0331200219, 1.1857373958, -0.1526173739,
                        -0.0082452407, -0.0270378968, 1.0352831375]),
        Case(name: "I-Log", input: .ilog, video: .ilogVideoLevels, encode: ILog.code(linear:),
             grey: 0.422003, black: 92.64, primaries: nil, toBT2020: nil,
             greyFrom: "Insta360's white paper", greyTolerance: 5e-7),
    ]
    check(abs(ILog.code(linear: 0) - 0.0905593) < 5e-8 && abs(ILog.code(linear: 22) - 1) < 1e-6,
          "I-Log: black and 2200% encode as the white paper's table has them, codes 93 and 1023")
    var portra = Recipe()
    portra.filmStock = "kodak_portra_800"
    portra.printStock = "kodak_portra_endura"
    let apple = LookLUT.Output.appleDisplay
    var rng = SplitMix(state: 0xF106_C106)
    var scene: [SIMD3<Double>] = []
    for _ in 0..<2048 {
        let c709 = SIMD3(rng.next(), rng.next(), rng.next())
        scene.append(rec709To2020(c709 * (0.18 * pow(2, -7 + 12 * rng.next()) / max(c709.max(), 1e-3))))
    }
    let viaNikon = try baker.direct(scene.map(nlog), recipe: portra, input: .nlogCode, output: apple)
    for c in cases {
        check(abs(c.encode(0.18) - c.grey) < c.greyTolerance,
              String(format: "%@: 18%% grey encodes to %.10f (%@: %.10f), code %.0f", c.name, c.encode(0.18), c.greyFrom,
                     c.grey, c.encode(0.18) * 1023))
        check(abs(c.encode(0) * 1023 - c.black) < 0.01, String(format: "%@: scene black at code %.2f", c.name, c.encode(0) * 1023))
        var worst = 0.0, rising = true, previous = -Double.infinity
        for code in 0...1023 {
            let v = Double(code) / 1023, y = c.input.linear(v)
            worst = max(worst, abs(c.encode(y) - v) * 1023)
            if y <= previous { rising = false }
            previous = y
        }
        check(rising && worst < 1e-6, String(format: "%@: decodes rising and round-trips every code value (%.1e)", c.name, worst))
        let greyVideo = c.video.linear((c.encode(0.18) * 1023 - 64) / 876)
        check(abs(greyVideo - 0.18) < 1e-9, String(format: "%@: video levels read the 18%% grey back as %.6f", c.name, greyVideo))
        let toBT2020 = c.input.gamut ?? matrix_identity_double3x3
        let white = toBT2020 * SIMD3<Double>(1, 1, 1)
        check(simd_reduce_max(simd_abs(white - 1)) < 1e-9, "\(c.name): its white is BT.2020 white")
        if let primaries = c.primaries {
            let toXYZ = Primaries.bt2020ToXYZ * toBT2020
            func xy(_ rgb: SIMD3<Double>) -> SIMD2<Double> { let v = toXYZ * rgb; return SIMD2(v.x, v.y) / (v.x + v.y + v.z) }
            let corners = [xy(SIMD3(1, 0, 0)), xy(SIMD3(0, 1, 0)), xy(SIMD3(0, 0, 1))]
            check(zip(corners, [primaries.red, primaries.green, primaries.blue]).allSatisfy { simd_distance($0, $1) < 1e-9 },
                  String(format: "%@: primaries where the maker puts them, blue (%.3f, %.3f)", c.name, corners[2].x, corners[2].y))
        }
        if let reference = c.toBT2020 {
            let rows = (0..<3).flatMap { r in (0..<3).map { k in toBT2020[k][r] } }
            check(zip(rows, reference).allSatisfy { abs($0 - $1) < 1e-8 }, "\(c.name): the matrix into BT.2020 is colour-science's")
        }
        // The same scene through this camera's curve and gamut prints what it
        // prints through N-Log.
        let fromBT2020 = toBT2020.inverse
        let points = scene.map { light -> SIMD3<Float> in
            let own = fromBT2020 * light
            return SIMD3(Float(c.encode(own.x)), Float(c.encode(own.y)), Float(c.encode(own.z)))
        }
        let truth = try baker.direct(points, recipe: portra, input: c.input, output: apple)
        let same = Errors(values: points.indices.map { deltaE2000(lab(truth[$0], apple), lab(viaNikon[$0], apple)) })
        check(same.largest < 0.05, String(format: "%@: the same scene prints the same as from N-Log (dE %.3f)", c.name, same.largest))
        let lut65 = try baker.bake(recipe: portra, size: 65, input: c.input, output: apple)
        let e65 = Errors(values: points.indices.map { deltaE2000(lab(truth[$0], apple), lab(lut65.sample(points[$0]), apple)) })
        check(e65.percentile(0.99) < 1.0, "\(c.name): the app's 65-point table follows the engine (\(e65.summary))")
        // Canon Log 3's straight shadow segment meets its log about three
        // stops under grey, inside one cell of a 33-point lattice: the table
        // strays most there (p99 3.2 from five to three stops under, 0.7 above
        // grey), as any 33-point Canon Log 3 table would.
        let lut33 = try baker.bake(recipe: portra, size: 33, input: c.input, output: apple)
        let e33 = Errors(values: points.indices.map { deltaE2000(lab(truth[$0], apple), lab(lut33.sample(points[$0]), apple)) })
        check(e33.percentile(0.99) < (c.input == .clog3 ? 3.0 : 1.5),
              "\(c.name): a 33-point table is close enough to shoot with (\(e33.summary))")
    }
}

print(failures == 0 ? "\nall LUT checks pass" : "\n\(failures) LUT check(s) failed")
exit(failures == 0 ? 0 : 1)
