import Foundation
import CoreGraphics
@testable import FLXCore

// Headless renderer — the same engine the app uses, driven from the shell.
// Useful for smoke-testing a RAW end to end, batch work, and generating the
// comparison images in the README.
//
//   flxrender <input> <output.jpg> [--film STOCK] [--paper STOCK]
//             [--exposure EV] [--brightness L] [--density STOPS] [--grade R]
//             [--shadows L] [--highlights L] [--toe V] [--shoulder V]
//             [--auto-brightness] [--auto-contrast] [--filter NAME] [--size PX] [--recipe file.json]

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

var args = Array(CommandLine.arguments.dropFirst())
guard args.count >= 2 else {
    fail("""
    usage: flxrender <input.nef|dng> <output.jpg|tif|png> [options]
      --film STOCK             film profile (default kodak_portra_400)
      --paper STOCK            print profile (default: the film's reference paper)
      --exposure EV            negative exposure (turns auto exposure off)
      --brightness L           midtone lightness shift, ±0.2; positive is lighter
                               (turns auto brightness off)
      --density STOPS          print exposure; positive is darker
      --grade R                contrast, ISO-R (115 neutral, lower is harder;
                               turns auto contrast off)
      --shadows V              lighten/darken the dark half, -1...1
      --highlights V           lighten/darken the light half, -1...1
      --toe V / --shoulder V   endpoint roll-off
      --auto-brightness        meter the midtones toward mid-grey
      --auto-contrast          fit the frame's textured range onto the paper
      --no-auto                disable auto exposure
      --size PX                longest edge (default: native)
      --recipe FILE            load a saved recipe, then apply the flags above
      --wb MODE                "Film reference" (default), "As shot", Daylight, Tungsten
      --filter NAME            camera filter for a black-and-white film:
                               blue, green, yellow, orange or red
      --list                   print the available stocks and exit
    """)
}

if args.contains("--dump-recipe") {
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
    var sample = Recipe()
    var mask = LocalMask()
    mask.name = "Example burn"
    mask.shape = .oval
    mask.points = [CGPoint(x: 0.5, y: 0.5), CGPoint(x: 0.75, y: 0.5), CGPoint(x: 0.5, y: 0.75)]
    mask.stops = 0.75
    sample.masks = [mask]
    print(String(data: try! encoder.encode(sample), encoding: .utf8)!)
    exit(0)
}

if args.contains("--list") {
    print("film stocks:")
    for e in PackLibrary.shared.filmStocks { print("  \(e.stock.padding(toLength: 30, withPad: " ", startingAt: 0)) \(e.name)") }
    print("print stocks:")
    for e in PackLibrary.shared.printStocks { print("  \(e.stock.padding(toLength: 30, withPad: " ", startingAt: 0)) \(e.name)") }
    exit(0)
}

let inputURL = URL(fileURLWithPath: args.removeFirst())
let outputURL = URL(fileURLWithPath: args.removeFirst())

var recipe = Recipe()
var maxDimension: Int?

func option(_ name: String) -> String? {
    guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
    return args[i + 1]
}
func flag(_ name: String) -> Bool { args.contains(name) }

if let path = option("--recipe") {
    recipe = try JSONDecoder().decode(Recipe.self, from: Data(contentsOf: URL(fileURLWithPath: path)))
}
if let v = option("--film") { recipe = recipe.loadingFilm(v) }   // as the app switches film
if let v = option("--paper") { recipe.printStock = v }
else if option("--film") != nil,
        let film = try? PackLibrary.shared.pack(recipe.filmStock),
        let target = film.targetPrint,
        PackLibrary.shared.printStocks.contains(where: { $0.stock == target }) {
    recipe.printStock = target
}
if flag("--auto-brightness") { recipe.autoDensity = true }
if flag("--auto-contrast") { recipe.autoGrade = true }
// A value given explicitly is the user's, so it takes over from its meter —
// the same as moving the slider in the app.
if let v = option("--exposure").flatMap(Double.init) { recipe.exposureEV = v; recipe.autoExposure = false }
if let v = option("--brightness").flatMap(Double.init) { recipe.brightness = v; recipe.autoDensity = false }
if let v = option("--density").flatMap(Double.init) { recipe.density = v }
if let v = option("--grade").flatMap(Double.init) { recipe.grade = v; recipe.autoGrade = false }
if let v = option("--shadows").flatMap(Double.init) { recipe.shadows = v }
if let v = option("--highlights").flatMap(Double.init) { recipe.highlights = v }
if let v = option("--toe").flatMap(Double.init) { recipe.toe = v }
if let v = option("--filter") { recipe.lensFilter = v == "none" ? nil : v }
if let v = option("--shoulder").flatMap(Double.init) { recipe.shoulder = v }
if let v = option("--size").flatMap(Int.init) { maxDimension = v }
if flag("--no-auto") { recipe.autoExposure = false }
let whiteBalanceMode: RawLoader.WhiteBalance =
    RawLoader.WhiteBalance(rawValue: option("--wb") ?? "") ?? .filmReference

let format: Exporter.Format
switch outputURL.pathExtension.lowercased() {
case "tif", "tiff": format = .tiff16
case "png": format = .png
default: format = .jpeg
}

let clock = CFAbsoluteTimeGetCurrent()
do {
    let referenceIlluminant = (try? PackLibrary.shared.pack(recipe.filmStock))?.referenceIlluminant ?? "D55"
    let decoded = try RawLoader.load(
        url: inputURL,
        options: RawLoader.Options(whiteBalance: whiteBalanceMode,
                                   filmReference: referenceIlluminant,
                                   maxDimension: maxDimension))
    let decodeTime = CFAbsoluteTimeGetCurrent() - clock
    print(String(format: "decoded  %d x %d  (%.2f s)", decoded.width, decoded.height, decodeTime))

    let renderer = try Renderer()
    renderer.setImage(decoded.pixels, width: decoded.width, height: decoded.height)

    // As the app does: the meters read only what the crop keeps.
    let area = recipe.geometry.metered(decoded.pixels, width: decoded.width, height: decoded.height)
    if recipe.autoExposure, let film = try? PackLibrary.shared.pack(recipe.filmStock) {
        let metered = RenderSetupBuilder.autoExposureEV(pixels: area.pixels,
                                                        width: area.width,
                                                        height: area.height,
                                                        film: film)
        recipe.exposureEV = Double(metered)
        print(String(format: "metered  %+.2f EV", metered))
    }
    let exposureEV = Float(recipe.exposureEV)

    let auto = try RenderSetupBuilder.resolveAuto(recipe: recipe, pixels: area.pixels,
                                                  width: area.width, height: area.height,
                                                  exposureEV: exposureEV)
    let resolved = auto.recipe
    if auto.metering != nil {
        if recipe.autoDensity { print(String(format: "midtones %+.3f", resolved.brightness)) }
        if recipe.autoGrade { print(String(format: "grade    %.0f R", resolved.grade)) }
    }
    let setup = try RenderSetupBuilder.build(recipe: resolved, exposureEV: exposureEV)
    let masks = resolved.masks.filter { $0.enabled }
    if !masks.isEmpty {
        renderer.setDodgeBurnMap(
            MaskRasterizer.rasterize(masks: masks, width: decoded.width, height: decoded.height))
    }

    let renderStart = CFAbsoluteTimeGetCurrent()
    _ = try renderer.render(setup: setup, recipe: resolved)
    let renderTime = CFAbsoluteTimeGetCurrent() - renderStart
    print(String(format: "rendered %@ on %@  (%.3f s)",
                 setup.film.name, setup.paper.name, renderTime))

    guard let linear = renderer.readLinear() else { fail("render produced no pixels") }
    try Exporter.write(linear: linear, width: Int(renderer.outputSize.width),
                       height: Int(renderer.outputSize.height),
                       to: outputURL, format: format, colorSpace: resolved.outputColorSpace,
                       metadata: RawLoader.metadata(of: inputURL))
    print("wrote    \(outputURL.path)")
} catch {
    fail("error: \(error)")
}
