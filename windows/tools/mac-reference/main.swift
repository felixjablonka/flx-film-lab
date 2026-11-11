// flxref — the Mac app's own engine, at the release pinned in mac/, rendering
// a picture and writing out everything the port needs to render the same: the
// decoded input, the render setup (uniforms, tables, LUTs) and the stages in
// between (taps). The port renders that setup (tools/port-render) and the two
// are compared stage by stage (tools/compare).
//
//   flxref <raw> <outdir> [--recipe FILE] [--film STOCK] [--paper STOCK] [--size PX]
//          [--format 35mm|120] [--plain] [--no-halation] [--no-couplers] [--no-grain]
//
// --plain turns halation, the DIR couplers and grain off; the --no- flags one
// of them each. Run it from mac/, where the engine finds its packs and
// shaders; tools/mac-reference/build.sh compiles it with the engine's
// sources, so it sees the engine's internals as the app does.
//
// The Gaussian blurs the render uses are Metal Performance Shaders', whose
// kernel above a sigma of 1 is an approximation of Apple's own. So for every
// sigma the render blurs with, the blur of one bright pixel is written out
// too: the port can render with those kernels, to tell what its own blur
// changes from what everything else does.

import CoreGraphics
import Foundation
import Metal
import MetalPerformanceShaders
import simd

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

var args = Array(CommandLine.arguments.dropFirst())
guard args.count >= 2 else {
    fail("usage: flxref <raw> <outdir> [--recipe FILE] [--film STOCK] [--paper STOCK] [--size PX] [--format 35mm|120] [--plain] [--no-halation] [--no-couplers] [--no-grain]")
}
let inputURL = URL(fileURLWithPath: args.removeFirst())
let outDir = URL(fileURLWithPath: args.removeFirst())

func option(_ name: String) -> String? {
    guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
    return args[i + 1]
}

// The dump format, shared with the port (engine/Dump.h) and tools/compare: a
// 32-byte header — "FLXD", version 1, width, height, channels, type (0
// float32, 1 uint8), two zeros — then the values, row-major, little-endian.
func write(_ name: String, width: Int, height: Int, channels: Int, type: UInt32, payload: Data) throws {
    var header = Data("FLXD".utf8)
    for v in [UInt32(1), UInt32(width), UInt32(height), UInt32(channels), type, 0, 0] {
        withUnsafeBytes(of: v.littleEndian) { header.append(contentsOf: $0) }
    }
    try (header + payload).write(to: outDir.appendingPathComponent(name))
}

func write(_ name: String, width: Int, height: Int, channels: Int, floats: [Float]) throws {
    precondition(floats.count == width * height * channels, "\(name): \(floats.count) values")
    try write(name, width: width, height: height, channels: channels, type: 0,
              payload: floats.withUnsafeBytes { Data($0) })
}

func columns(_ m: simd_float3x3) -> [Float] {
    [m.columns.0.x, m.columns.0.y, m.columns.0.z,
     m.columns.1.x, m.columns.1.y, m.columns.1.z,
     m.columns.2.x, m.columns.2.y, m.columns.2.z]
}

func array(_ v: SIMD3<Float>) -> [Float] { [v.x, v.y, v.z] }

/// Renderer.mean, in Float as it sums.
func mean(_ values: [Float]) -> Float { values.isEmpty ? 0 : values.reduce(0, +) / Float(values.count) }

/// Renderer.fnv1a, which is private to it.
func fnv1a(_ text: String) -> UInt32 {
    var h: UInt32 = 0x811C_9DC5
    for byte in text.utf8 { h = (h ^ UInt32(byte)) &* 0x0100_0193 }
    return h
}

/// MPSImageGaussianBlur's kernel at `sigma`, edge mode clamp as Renderer
/// uses it: the blur of one bright pixel in a square of zeros, through its
/// centre, divided by the centre's own weight on the other axis (the blur is
/// two passes, one per axis). Also how far the square's blur is from the
/// product of that kernel with itself.
func mpsKernel(sigma: Float, device: MTLDevice, queue: MTLCommandQueue) -> (taps: [Float], separability: Float) {
    let reach = Int((6 * sigma).rounded(.up)) + 8
    let n = 2 * reach + 1
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba32Float, width: n, height: n, mipmapped: false)
    d.usage = [.shaderRead, .shaderWrite]
    d.storageMode = .shared
    let source = device.makeTexture(descriptor: d)!, blurred = device.makeTexture(descriptor: d)!
    var pixels = [Float](repeating: 0, count: n * n * 4)
    pixels[(reach * n + reach) * 4] = 1
    source.replace(region: MTLRegionMake2D(0, 0, n, n), mipmapLevel: 0, withBytes: pixels, bytesPerRow: n * 16)
    let blur = MPSImageGaussianBlur(device: device, sigma: sigma)
    blur.edgeMode = .clamp
    let cb = queue.makeCommandBuffer()!
    blur.encode(commandBuffer: cb, sourceTexture: source, destinationTexture: blurred)
    cb.commit()
    cb.waitUntilCompleted()
    blurred.getBytes(&pixels, bytesPerRow: n * 16, from: MTLRegionMake2D(0, 0, n, n), mipmapLevel: 0)
    let at = { (x: Int, y: Int) in pixels[(y * n + x) * 4] }
    let centre = at(reach, reach).squareRoot()
    var taps = (0..<n).map { at($0, reach) / centre }
    while taps.count > 1, abs(taps.first!) < 1e-12, abs(taps.last!) < 1e-12 { taps.removeFirst(); taps.removeLast() }
    let r = taps.count / 2
    var worst: Float = 0
    for y in -r...r { for x in -r...r { worst = max(worst, abs(at(reach + x, reach + y) - taps[x + r] * taps[y + r])) } }
    return (taps, worst)
}

// The recipe, as flxrender makes it: a saved one, if given, with the flags on top.
var recipe = Recipe()
if let path = option("--recipe") {
    do { recipe = try JSONDecoder().decode(Recipe.self, from: Data(contentsOf: URL(fileURLWithPath: path))) }
    catch { fail("cannot read \(path): \(error)") }
}
if let v = option("--film") { recipe = recipe.loadingFilm(v) }
if let v = option("--paper") {
    recipe.printStock = v
} else if option("--film") != nil,
          let film = try? PackLibrary.shared.pack(recipe.filmStock),
          let target = film.targetPrint,
          PackLibrary.shared.printStocks.contains(where: { $0.stock == target }) {
    recipe.printStock = target
}
if let v = option("--format") {
    guard let format = FilmFormat(rawValue: v == "35mm" ? "35 mm" : v) else { fail("--format is 35mm or 120") }
    recipe.filmFormat = format
}
let plain = args.contains("--plain")
if plain || args.contains("--no-halation") { recipe.halationEnabled = false }
if plain || args.contains("--no-couplers") { recipe.couplersEnabled = false }
if plain || args.contains("--no-grain") { recipe.grainEnabled = false }
let maxDimension = option("--size").flatMap(Int.init)

do {
    try FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: true)
    // The recipe as given, before the meters: the port starts from it.
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
    try encoder.encode(recipe).write(to: outDir.appendingPathComponent("recipe.json"))
    let film = try PackLibrary.shared.pack(recipe.filmStock)
    let decoded = try RawLoader.load(url: inputURL,
                                     options: RawLoader.Options(whiteBalance: .filmReference,
                                                                filmReference: film.referenceIlluminant,
                                                                maxDimension: maxDimension))
    let width = decoded.width, height = decoded.height

    let renderer = try Renderer()
    renderer.setImage(decoded.pixels, width: width, height: height)
    renderer.grainSeed = Renderer.grainSeed(for: inputURL)
    let area = recipe.geometry.metered(decoded.pixels, width: width, height: height)
    if recipe.autoExposure {
        recipe.exposureEV = Double(RenderSetupBuilder.autoExposureEV(pixels: area.pixels, width: area.width,
                                                                     height: area.height, film: film))
    }
    let exposureEV = Float(recipe.exposureEV)
    let resolved = try RenderSetupBuilder.resolveAuto(recipe: recipe, pixels: area.pixels, width: area.width,
                                                      height: area.height, exposureEV: exposureEV).recipe
    let setup = try RenderSetupBuilder.build(recipe: resolved, exposureEV: exposureEV)
    // Dodge & burn, as the app's export draws it (DocumentModel.export).
    let masks = resolved.masks.filter { $0.enabled }
    if !masks.isEmpty {
        renderer.setDodgeBurnMap(MaskRasterizer.rasterize(masks: masks, width: width, height: height))
    }

    let start = CFAbsoluteTimeGetCurrent()
    _ = try renderer.render(setup: setup, recipe: resolved)
    let renderSeconds = CFAbsoluteTimeGetCurrent() - start

    // What film_expose gets, as Renderer.render makes it.
    let tc = try PackLibrary.shared.tcLut(for: setup.film, lensFilter: setup.lensFilter)
    // As ExposureGradientUniform: a, b, c (an oval's; zero for a linear one),
    // ev, kind, feather, inverted.
    let gradients = setup.exposureGradients.map { g -> [Float] in
        let c = g.isOval ? [Float(g.points[2].x) * Float(width), Float(g.points[2].y) * Float(height)] : [0, 0]
        return [Float(g.points[0].x) * Float(width), Float(g.points[0].y) * Float(height),
                Float(g.points[1].x) * Float(width), Float(g.points[1].y) * Float(height)] + c
            + [Float(g.exposureEV), g.isOval ? 1 : 0, Float(g.featherAmount), g.isInverted ? 1 : 0]
    }
    let d = setup.developUniforms, p = setup.printUniforms
    let halationOn = resolved.halationEnabled && (setup.film.defaults.halation?.active ?? false)
    let couplersOn = resolved.couplersEnabled && resolved.couplersAmount > 0 && setup.film.defaults.dirCouplers != nil
    let grainOn = resolved.grainEnabled && resolved.grainAmount > 0 && setup.film.defaults.grain != nil

    // The sigmas Renderer.render blurs with, worked out as it does
    // (encodeHalation, couplerDiffusionPixels, step 3b).
    // an instant print's own frame, or the format's (ProfilePack.frame)
    let frame = setup.film.frame(resolved.filmFormat)
    let pitch = Renderer.pixelSizeUm(width: width, height: height, frame: frame)
    var sigmas: [Float] = []
    if halationOn, let h = setup.film.defaults.halation {
        let scatterAmount = resolved.keepsCameraSharpness ? 0 : Float(resolved.scatterAmount)
        let core = mean(h.scatterCoreUm) * Float(resolved.scatterScale) / pitch
        let tail = mean(h.scatterTailUm) * Float(resolved.scatterScale) / pitch
        if scatterAmount > 0 && (core > 0.05 || tail > 0.05) { sigmas += [max(core, 0.05), max(tail * 1.5236, 0.05)] }
        let strength = SIMD3<Float>(h.halationStrength[0], h.halationStrength[1], h.halationStrength[2]) * Float(resolved.halationAmount)
        let sigmaH = mean(h.halationFirstSigmaUm) * Float(resolved.halationScale) / pitch
        if strength.max() > 0, sigmaH > 0.05 {
            for k in 0..<max(h.halationNBounces, 1) { sigmas.append(max(sigmaH * Float(k + 1).squareRoot(), 0.05)) }
        }
    }
    if couplersOn, !resolved.keepsCameraSharpness, let dc = setup.film.defaults.dirCouplers, dc.diffusionSizeUm > 0 {
        let s = dc.diffusionSizeUm * Float(resolved.couplersDiffusion) / pitch
        if s > 0.4 { sigmas.append(s) }
    }
    if grainOn { sigmas.append(3) }
    let device = MTLCreateSystemDefaultDevice()!, queue = device.makeCommandQueue()!
    let mpsKernels: [[String: Any]] = sigmas.map { s in
        let k = mpsKernel(sigma: s, device: device, queue: queue)
        return ["sigma": s, "taps": k.taps, "separability": k.separability]
    }

    var stock: [String: Any] = ["monochrome": setup.film.isMonochrome]
    if let h = setup.film.defaults.halation {
        stock["halation"] = [
            "active": h.active, "scatterCoreUm": h.scatterCoreUm, "scatterTailUm": h.scatterTailUm,
            "scatterTailWeight": h.scatterTailWeight, "halationStrength": h.halationStrength,
            "halationFirstSigmaUm": h.halationFirstSigmaUm, "halationNBounces": h.halationNBounces,
            "halationBounceDecay": h.halationBounceDecay, "halationRenormalize": h.halationRenormalize,
        ]
    }
    if let dc = setup.film.defaults.dirCouplers { stock["dirCouplers"] = ["diffusionSizeUm": dc.diffusionSizeUm] }
    if let g = setup.film.defaults.grain {
        stock["grain"] = ["tableDensityMax": g.tableDensityMax, "cloudUm": g.cloudUm, "tableSamples": g.tableSamples]
        try write("selwyn.f32", width: 3, height: 3 * g.tableSamples, channels: 1, floats: g.selwynUm2)
    }
    let json: [String: Any] = [
        "recipe": [
            "halationAmount": resolved.halationAmount, "halationScale": resolved.halationScale,
            "scatterAmount": resolved.scatterAmount, "scatterScale": resolved.scatterScale,
            "couplersDiffusion": resolved.couplersDiffusion,
            "grainAmount": resolved.grainAmount, "grainScale": resolved.grainScale,
            "cameraSharpness": resolved.keepsCameraSharpness,
            "frameMM": [frame.long, frame.short],
        ],
        "stock": stock,
        "pixelSizeUm": pitch,
        "grainSeed": ["file": renderer.grainSeed, "stock": fnv1a(setup.film.stock),
                      "seed": fnv1a(setup.film.stock) ^ (renderer.grainSeed &* 0x9E37_79B9)],
        "mpsKernels": mpsKernels,
        "macRelease": ProcessInfo.processInfo.environment["FLX_MAC_RELEASE"] ?? "unknown",
        "input": inputURL.lastPathComponent,
        "width": width, "height": height,
        "film": setup.film.stock, "paper": setup.paper.stock,
        "plain": plain,
        "stages": [
            "halation": halationOn,
            "couplers": couplersOn,
            "grain": grainOn,
            "shaped": !GeometryMap(resolved.geometry, sourceWidth: width, sourceHeight: height, wholeFrame: false).isIdentity,
            "effects": resolved.effects?.isActive ?? false,
            "curves": !(resolved.curves?.isIdentity ?? true),
            "dodgeBurn": !resolved.masks.filter(\.enabled).isEmpty,
        ],
        "exposureEV": exposureEV,
        // What the meters and the setup resolved the recipe to.
        "resolved": [
            "exposureEV": exposureEV, "brightness": resolved.brightness, "grade": resolved.grade,
            "neutralFilters": array(setup.neutralFilters),
            "toneGeometry": [
                "densityMin": setup.toneGeometry.densityMin, "densityMax": setup.toneGeometry.densityMax,
                "range": setup.toneGeometry.range, "scale": setup.toneGeometry.scale,
                "anchor": setup.toneGeometry.anchor, "midGrey": setup.toneGeometry.midGrey,
            ],
        ],
        "filmUniforms": [
            "rgbToXYZ": columns(setup.film.rgbToXYZMatrix),
            "exposureScale": pow(2, setup.exposureEV),
            "tcLutSize": setup.film.tcLut?.size ?? 192,
            "intoTriangle": setup.film.isMonochrome ? 1 : 0,
        ],
        "gradients": gradients,
        "developUniforms": [
            "logE0": d.logE0, "logEStep": d.logEStep,
            "gammaFactor": array(d.gammaFactor), "densityMax": array(d.densityMax),
            "couplersAmount": d.couplersAmount, "isPositive": d.isPositive,
            "edgeCap": d.edgeCap, "diffusionFraction": d.diffusionFraction,
        ],
        "couplerMatrix": columns(setup.couplerMatrix),
        "printUniforms": [
            "xyzToRGB": columns(p.xyzToRGB),
            "exposureFactorMidgray": array(p.exposureFactorMidgray),
            "preflashRaw": array(p.preflashRaw),
            "printExposure": p.printExposure, "cmfNormalization": p.cmfNormalization,
            "toneLogE0": p.toneLogE0, "toneLogEStep": p.toneLogEStep, "toneLutWidth": p.toneLutWidth,
            "gradeSpanMin": p.gradeSpanMin, "gradeSpanMax": p.gradeSpanMax,
            "hasDodgeBurn": p.hasDodgeBurn, "glareAmount": p.glareAmount,
            "illuminantXYZ": array(p.illuminantXYZ),
            "gamutCompress": p.gamutCompress, "gamutThreshold": p.gamutThreshold,
            "gamutLimit": p.gamutLimit, "gamutPower": p.gamutPower,
            "scanFilm": p.scanFilm,
            "gamutRGBToLMS": columns(p.gamutRGBToLMS), "gamutLMSToRGB": columns(p.gamutLMSToRGB),
            "toneShiftMax": p.toneShiftMax, "toneShiftCount": p.toneShiftCount,
        ],
        "encoding": resolved.outputColorSpace == "ProPhoto RGB" ? 2 : 1,
        "outputColorSpace": resolved.outputColorSpace,
        "renderSeconds": renderSeconds,
    ]
    let jsonData = try JSONSerialization.data(withJSONObject: json, options: [.prettyPrinted, .sortedKeys])
    try jsonData.write(to: outDir.appendingPathComponent("setup.json"))

    // The tables, as Renderer.uploadTables binds them.
    try write("spectral.f32", width: setup.spectralTables.count, height: 1, channels: 1, floats: setup.spectralTables)
    try write("curves.f32", width: Constants.curveSamples, height: 1, channels: 3, floats: setup.normalizedCurves)
    try write("curves0.f32", width: setup.curvesBeforeCouplers.count / 3, height: 1, channels: 3,
              floats: setup.curvesBeforeCouplers)
    let edge = setup.edgeScale.isEmpty ? [Float](repeating: 1, count: 3) : setup.edgeScale
    try write("edgeScale.f32", width: edge.count / 3, height: 1, channels: 3, floats: edge)
    let shift = setup.toneShift.isEmpty ? [Float](repeating: 0, count: 2) : setup.toneShift
    try write("toneShift.f32", width: shift.count, height: 1, channels: 1, floats: shift)
    if let curves = resolved.curves, !curves.isIdentity {
        try write("printCurves.f32", width: PrintCurves.tableSize, height: 3, channels: 1, floats: curves.table())
    }
    try write("toneLut.f32", width: Constants.toneLutWidth, height: Constants.toneLutHeight, channels: 4,
              floats: setup.toneLUT)
    try write("tcLut.f32", width: tc.size, height: tc.size, channels: 3, floats: tc.values)

    // The picture, and the stages the engine lets a test read.
    try write("input.f32", width: width, height: height, channels: 4, floats: decoded.pixels)
    for tap in ["log_e_film", "cmy_film"] {
        guard let values = renderer.readTap(tap) else { fail("no \(tap) tap") }
        try write("\(tap).f32", width: width, height: height, channels: 4, floats: values)
    }
    // The print at its own size: the frame's, or a shaped print's.
    let outWidth = Int(renderer.outputSize.width), outHeight = Int(renderer.outputSize.height)
    guard let linear = renderer.readLinear() else { fail("no linear print") }
    try write("rgb_out.f32", width: outWidth, height: outHeight, channels: 4, floats: linear)
    guard let display = renderer.makeDisplayImage(colorSpaceName: resolved.outputColorSpace),
          var bytes = display.dataProvider?.data as Data? else { fail("no display image") }
    // Red first, as the port's: from 1.4.0 the Mac's screen image holds blue,
    // green, red, alpha, the order Core Animation draws from.
    if display.bitmapInfo.contains(.byteOrder32Little) {
        bytes.withUnsafeMutableBytes { raw in
            let p = raw.bindMemory(to: UInt8.self)
            for i in stride(from: 0, to: p.count - 3, by: 4) { p.swapAt(i, i + 2) }
        }
    }
    try write("display.u8", width: outWidth, height: outHeight, channels: 4, type: 1, payload: bytes)

    print(String(format: "%@: %d x %d, %@ on %@, %+.2f EV, render %.3f s -> %@",
                 inputURL.lastPathComponent, width, height, setup.film.stock, setup.paper.stock,
                 exposureEV, renderSeconds, outDir.path))
} catch {
    fail("error: \(error)")
}
