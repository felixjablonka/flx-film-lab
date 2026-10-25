import Foundation
@testable import FLXCore

// Bakes a film-and-paper look into a .cube LUT for log footage: Nikon N-Log,
// Apple Log or Apple Log 2 from an iPhone, Panasonic V-Log, Fujifilm F-Log or
// F-Log2, Canon Log 2 or 3, Sony S-Log3, or Insta360 I-Log.
//
//   flxlut <output.cube> [--film STOCK] [--paper STOCK] [--filter NAME] [--size 33|65]
//          [--input code|video-levels|applelog|applelog2|vlog|flog|flog2|clog2|clog3|
//                   slog3|slog3-sgamut3|ilog, each but the Apple Logs also as ...-video-levels]
//          [--output bt1886|apple|srgb|linear]
//          [--exposure EV] [--brightness L] [--grade R] [--shadows V]
//          [--highlights V] [--density STOPS] [--yellow CC] [--magenta CC]
//          [--recipe file.json]

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

var args = Array(CommandLine.arguments.dropFirst())

if args.contains("--list") {
    print("film stocks:")
    for e in PackLibrary.shared.filmStocks { print("  \(e.stock.padding(toLength: 30, withPad: " ", startingAt: 0)) \(e.name)") }
    print("print stocks:")
    for e in PackLibrary.shared.printStocks { print("  \(e.stock.padding(toLength: 30, withPad: " ", startingAt: 0)) \(e.name)") }
    exit(0)
}

guard let outputPath = args.first, !outputPath.hasPrefix("--") else {
    fail("""
    usage: flxlut <output.cube> [options]
      --film STOCK         film profile (default kodak_portra_800)
      --paper STOCK        print profile (default: the film's reference paper)
      --filter NAME        camera filter: blue, green, yellow, orange or red for a
                           black-and-white film, 81c or 812 (Tiffen's warming
                           filters) for a slide
      --size N             lattice points per axis (default 65; 33 for monitors)
      --input MODE         the footage, and how the app hands it to the LUT:
                             code          Nikon N-Log, code value / 1023: a Zf writes
                                           full-range HEVC, which Resolve reads as data
                                           levels (default)
                             video-levels  Nikon N-Log from a video-range file,
                                           (code - 64) / 876
                             applelog      Apple Log from an iPhone 15 Pro or later
                             applelog2     Apple Log 2 / Apple Wide Gamut, which iPhone
                                           17 Pro and later can record instead
                             vlog         Panasonic V-Log / V-Gamut, code value / 1023:
                                           the full range a Lumix Real Time LUT takes;
                                           adds the #LUMIXPHOTOSTYLE VLOG header line
                             vlog-video-levels  V-Log from a video-range file, as
                                           Resolve hands it: (code - 64) / 876
                             flog          Fujifilm F-Log / F-Gamut, code value / 1023
                             flog2         Fujifilm F-Log2 / F-Gamut, code value / 1023
                             clog2         Canon Log 2 / Cinema Gamut, code value / 1023
                             clog3         Canon Log 3 / Cinema Gamut, code value / 1023
                             slog3         Sony S-Log3 / S-Gamut3.Cine, code value / 1023
                             slog3-sgamut3  Sony S-Log3 / S-Gamut3, code value / 1023
                             ilog          Insta360 I-Log / BT.2020, code value / 1023
                             (each)-video-levels  that curve from a video-range file,
                                           as Resolve hands it: (code - 64) / 876
      --output MODE        bt1886   2.4 power, for a Rec.709 reference display (default)
                           apple    1.961 power, how macOS and iOS decode Rec.709 video
                           srgb     the sRGB curve: photos exactly, video in between
                           linear   display-referred linear
      --title TEXT         the TITLE line (default: film on paper)
      --exposure EV        negative exposure
      --brightness L       midtone lightness shift, ±0.2; positive is lighter
      --grade R            contrast, ISO-R (115 neutral, lower is harder)
      --shadows V          lighten/darken the dark half, -1...1
      --highlights V       lighten/darken the light half, -1...1
      --shadow-grade R     split grade for the shadows, ISO-R points (negative is harder)
      --highlight-grade R  split grade for the highlights, ISO-R points (positive is softer)
      --density STOPS      print exposure; positive is darker
      --yellow CC          enlarger yellow filtration on top of neutral (cooler print)
      --magenta CC         enlarger magenta filtration on top of neutral (greener print)
      --recipe FILE        load a recipe (flxrender --dump-recipe), then apply the flags
      --list               print the available stocks and exit

    Halation, grain, coupler diffusion, masks, bloom and glow and the meters
    depend on more than one pixel, so a LUT cannot carry them; they are
    switched off.
    """)
}
args.removeFirst()

func option(_ name: String) -> String? {
    guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
    return args[i + 1]
}

var recipe = Recipe()
recipe.filmStock = "kodak_portra_800"
recipe.printStock = "kodak_portra_endura"
if let path = option("--recipe") {
    do {
        recipe = try JSONDecoder().decode(Recipe.self, from: Data(contentsOf: URL(fileURLWithPath: path)))
    } catch {
        fail("could not read recipe: \(error)")
    }
}
if let v = option("--film") { recipe = recipe.loadingFilm(v) }   // as the app switches film
if let v = option("--paper") { recipe.printStock = v }
else if option("--film") != nil,
        let film = try? PackLibrary.shared.pack(recipe.filmStock),
        let target = film.targetPrint,
        PackLibrary.shared.printStocks.contains(where: { $0.stock == target }) {
    recipe.printStock = target
}
if let v = option("--filter") { recipe.lensFilter = v == "none" ? nil : v }
if let v = option("--exposure").flatMap(Double.init) { recipe.exposureEV = v }
if let v = option("--brightness").flatMap(Double.init) { recipe.brightness = v }
if let v = option("--grade").flatMap(Double.init) { recipe.grade = v }
if let v = option("--shadows").flatMap(Double.init) { recipe.shadows = v }
if let v = option("--highlights").flatMap(Double.init) { recipe.highlights = v }
if let v = option("--shadow-grade").flatMap(Double.init) { recipe.shadowGrade = v }
if let v = option("--highlight-grade").flatMap(Double.init) { recipe.highlightGrade = v }
if let v = option("--density").flatMap(Double.init) { recipe.density = v }
if let v = option("--yellow").flatMap(Double.init) { recipe.filterShiftY = v }
if let v = option("--magenta").flatMap(Double.init) { recipe.filterShiftM = v }

let size = option("--size").flatMap(Int.init) ?? 65
guard (2...129).contains(size) else { fail("--size must be between 2 and 129") }

let input: LookLUT.Input
switch option("--input") ?? "code" {
case "video-levels", "video", "legal": input = .nlogVideoLevels
case "code", "data", "full": input = .nlogCode
case "applelog", "apple-log", "iphone": input = .appleLog
case "applelog2", "apple-log-2", "log2": input = .appleLog2
case "vlog", "v-log", "lumix", "panasonic": input = .vlog
case "vlog-video-levels", "vlog-video": input = .vlogVideoLevels
case "flog", "f-log": input = .flog
case "flog-video-levels": input = .flogVideoLevels
case "flog2", "f-log2": input = .flog2
case "flog2-video-levels": input = .flog2VideoLevels
case "clog2", "c-log2", "canon-log-2": input = .clog2
case "clog2-video-levels": input = .clog2VideoLevels
case "clog3", "c-log3", "canon-log-3": input = .clog3
case "clog3-video-levels": input = .clog3VideoLevels
case "slog3", "s-log3", "slog3-cine": input = .slog3Cine
case "slog3-video-levels", "slog3-cine-video-levels": input = .slog3CineVideoLevels
case "slog3-sgamut3", "s-log3-s-gamut3": input = .slog3
case "slog3-sgamut3-video-levels": input = .slog3VideoLevels
case "ilog", "i-log", "insta360": input = .ilog
case "ilog-video-levels": input = .ilogVideoLevels
case let other: fail("unknown --input \(other)")
}
let output: LookLUT.Output
switch option("--output") ?? "bt1886" {
case "bt1886", "rec709", "2.4": output = .bt1886
case "apple", "mac": output = .appleDisplay
case "srgb", "photo": output = .sRGB
case "linear": output = .linear
case let other: fail("unknown --output \(other)")
}

do {
    let film = try PackLibrary.shared.pack(recipe.filmStock)
    let paper = try PackLibrary.shared.pack(recipe.printStock)
    let filter = film.lensFilter(recipe.lensFilter)
    if option("--filter") != nil, recipe.lensFilter != nil, filter == nil {
        let names = film.lensFilters?.map(\.id).joined(separator: ", ")
        fail("\(film.name) has no \(recipe.lensFilter!) filter" + (names.map { "; it has \($0)" } ?? "; only black-and-white and slide stocks take one"))
    }
    let start = CFAbsoluteTimeGetCurrent()
    let baker = try LookBaker()
    let lut = try baker.bake(recipe: recipe, size: size, input: input, output: output)
    let bakeTime = CFAbsoluteTimeGetCurrent() - start

    // A slide is read off a light table unless it is printed on purpose.
    let lightTable = recipe.scanFilm ?? film.isPositive
    let rendering = lightTable ? "\(film.name) on a light table" : "\(film.name) printed on \(paper.name)"
    var comments = ["Made with FLX Film Lab (flxlut): \(rendering)"]
    if let filter {
        comments.append(String(format: "Camera filter: %@, %.1fx (exposure opened %.1f stops)",
                               filter.designation, filter.factor, log2(max(filter.factor, 1))))
    }
    comments.append(String(format: "Exposure %+.2f EV, brightness %+.3f, grade R%.0f, shadows %+.2f, highlights %+.2f, print exposure %+.2f",
                           recipe.exposureEV, recipe.brightness, recipe.grade,
                           recipe.shadows, recipe.highlights, recipe.density))
    if recipe.shadowGrade != 0 || recipe.highlightGrade != 0 {
        comments.append(String(format: "Split grade: shadows %+.0f, highlights %+.0f ISO-R points",
                               recipe.shadowGrade, recipe.highlightGrade))
    }
    if recipe.filterShiftY != 0 || recipe.filterShiftM != 0 {
        comments.append(String(format: "Filtration on top of neutral: %+.0fY %+.0fM", recipe.filterShiftY, recipe.filterShiftM))
    }
    comments.append("Per-pixel only: halation, grain, coupler diffusion and masks are not in a LUT")
    // A Lumix camera reads its base Photo Style from the line under the title;
    // without it (or if it is wrong) it uses V-Log anyway.
    let header = input == .vlog ? ["#LUMIXPHOTOSTYLE VLOG"] : []
    let title = option("--title") ?? "\(film.name)\(filter.map { " (\($0.name) filter)" } ?? "")"
        + (lightTable ? "" : " on \(paper.name)")
    let text = lut.cubeText(title: title, headerLines: header, comments: comments, domain: input != .vlog)
    try text.write(to: URL(fileURLWithPath: outputPath), atomically: true, encoding: .utf8)

    print("\(rendering)\(filter.map { " (\($0.name) filter)" } ?? ""), \(size)^3, \(input.rawValue) -> \(output.rawValue)")
    print(String(format: "baked in %.0f ms", bakeTime * 1000))
    print("wrote \(outputPath)")
} catch {
    fail("error: \(error)")
}
