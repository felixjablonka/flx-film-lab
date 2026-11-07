import Foundation
import CoreGraphics
import ImageIO
@testable import FLXCore

// Drives DocumentModel the way the app does — open, edit, open another,
// go back, fail to open, switch fast, export mid-load — and checks the state
// after each step. Uses generated images, so it runs anywhere.

var failures = 0
func check(_ ok: Bool, _ message: String) {
    print("   \(ok ? "ok  " : "FAIL") \(message)")
    if !ok { failures += 1 }
}

@MainActor
func waitUntil(timeout: Double = 15, _ condition: () -> Bool) async -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
        if condition() { return true }
        try? await Task.sleep(for: .milliseconds(20))
    }
    return condition()
}

/// Nothing decoding, nothing rendering, nothing pending.
@MainActor
func settle(_ m: DocumentModel) async -> Bool {
    // Give a just-scheduled decode its settle delay before judging.
    try? await Task.sleep(for: .milliseconds(200))
    return await waitUntil { !m.isDecoding && !m.isRendering && m.pendingURL == nil }
}

func writeImage(_ url: URL, width: Int, height: Int, colour: (Float, Float, Float)) throws {
    var px = [Float](repeating: 1, count: width * height * 4)
    for y in 0..<height { for x in 0..<width {
        let ramp = 0.03 + 0.5 * Float(x) / Float(width) * (0.6 + 0.4 * Float(y) / Float(height))
        let o = (y * width + x) * 4
        px[o] = ramp * colour.0; px[o + 1] = ramp * colour.1; px[o + 2] = ramp * colour.2
    }}
    try Exporter.write(linear: px, width: width, height: height, to: url, format: .tiff16, colorSpace: "sRGB")
}

func size(_ image: CGImage?) -> String { image.map { "\($0.width)x\($0.height)" } ?? "none" }

/// Mean of the display image's pixels in rows `rows` (fractions of the height), as 0...1 luminance.
func meanLuminance(_ image: CGImage?, rows: ClosedRange<Double>) -> Double {
    guard let image else { return -1 }
    let w = image.width, h = image.height
    var buffer = [UInt8](repeating: 0, count: w * h * 4)
    guard let ctx = CGContext(data: &buffer, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                              space: CGColorSpaceCreateDeviceRGB(),
                              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return -1 }
    ctx.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
    var sum = 0.0, n = 0.0
    // A bitmap context's memory starts at the top row, like the fractions.
    for y in Int(rows.lowerBound * Double(h))..<max(Int(rows.upperBound * Double(h)), 1) {
        let row = min(y, h - 1) * w * 4
        for x in 0..<w {
            let o = row + x * 4
            sum += (0.2126 * Double(buffer[o]) + 0.7152 * Double(buffer[o + 1]) + 0.0722 * Double(buffer[o + 2])) / 255
            n += 1
        }
    }
    return sum / max(n, 1)
}

/// A small JPEG carrying a camera's metadata, written with ImageIO directly so
/// the export code under test has no hand in making it.
func writePhoto(_ url: URL, width: Int, height: Int, properties: [CFString: Any]) throws {
    let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0,
                            space: CGColorSpace(name: CGColorSpace.sRGB)!,
                            bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    context.setFillColor(CGColor(red: 0.6, green: 0.5, blue: 0.4, alpha: 1))
    context.fill(CGRect(x: 0, y: 0, width: width, height: height))
    let destination = CGImageDestinationCreateWithURL(url as CFURL, "public.jpeg" as CFString, 1, nil)!
    CGImageDestinationAddImage(destination, context.makeImage()!, properties as CFDictionary)
    guard CGImageDestinationFinalize(destination) else { throw CocoaError(.fileWriteUnknown) }
}

@MainActor
func run() async throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("flx-apptest-\(getpid())")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }
    let a = dir.appendingPathComponent("A.tif"), b = dir.appendingPathComponent("B.tif")
    let c = dir.appendingPathComponent("C.tif"), bad = dir.appendingPathComponent("broken.tif")
    try writeImage(a, width: 600, height: 400, colour: (1.2, 1.0, 0.8))
    try writeImage(b, width: 300, height: 450, colour: (0.8, 1.0, 1.2))
    try writeImage(c, width: 500, height: 500, colour: (1.0, 1.0, 1.0))
    try Data("not an image".utf8).write(to: bad)

    let m = DocumentModel()

    print("open an image")
    m.open(url: a)
    check(await settle(m) && m.fileURL == a && size(m.displayImage) == "600x400",
          "A is on screen at \(size(m.displayImage))")

    print("edit it")
    var r = m.recipe
    r.filmStock = "kodak_ektar_100"
    r.brightness = 0.1
    var mask = LocalMask()
    mask.shape = .oval
    mask.points = [CGPoint(x: 0.5, y: 0.5), CGPoint(x: 0.7, y: 0.5), CGPoint(x: 0.5, y: 0.7)]
    mask.stops = 0.5
    r.masks = [mask]
    var gradient = ExposureGradient()
    gradient.exposureEV = -1
    r.gradients = [gradient]
    m.recipe = r
    m.whiteBalance = .custom
    m.customTemperature = 4000
    check(await settle(m) && m.resolvedRecipe?.brightness == 0.1 && m.resolvedRecipe?.masks.count == 1
              && m.resolvedRecipe?.gradients.count == 1,
          "A renders with its edits (brightness, one mask, a gradient, custom white balance)")

    print("drop a different image")
    m.open(url: b)
    check(m.fileURL == a && m.pendingURL == b && !m.canExport,
          "while B decodes, A stays on screen and export waits")
    check(await settle(m), "B finishes loading")
    check(m.fileURL == b && size(m.displayImage) == "300x450", "B is on screen at \(size(m.displayImage))")
    check(m.recipe.filmStock == "kodak_ektar_100", "B keeps the look: film is still Ektar 100")
    check(m.recipe.brightness == 0 && m.recipe.masks.isEmpty && m.recipe.gradients.isEmpty,
          "B starts without A's adjustments (brightness \(m.recipe.brightness), \(m.recipe.masks.count) masks, \(m.recipe.gradients.count) gradients)")
    check(m.whiteBalance == .filmReference, "B starts at film-reference white balance, not A's 4000 K")

    print("a new image starts with the default autos, whatever the last one had")
    var rb = m.recipe
    rb.autoExposure = false
    rb.autoDensity = true
    rb.autoGrade = true
    rb.shadows = 0.3
    m.recipe = rb
    _ = await settle(m)
    m.open(url: c)
    check(await settle(m) && m.fileURL == c, "C is on screen")
    check(m.recipe.autoExposure && !m.recipe.autoDensity && !m.recipe.autoGrade && m.meteredExposureEV != nil,
          "C opens with Auto Exposure on, metered at \(m.meteredExposureEV.map { String(format: "%+.2f EV", $0) } ?? "-"), and Auto Brightness and Auto Contrast off, though B had them the other way round")

    print("go back to the first image")
    m.open(url: a)
    check(await settle(m) && m.fileURL == a && size(m.displayImage) == "600x400", "A is back on screen")
    check(m.recipe.brightness == 0.1 && m.recipe.masks.count == 1 && m.recipe.masks.first?.id == mask.id
              && m.recipe.gradients.first?.id == gradient.id,
          "A's own edits are restored, mask and gradient included")
    check(m.whiteBalance == .custom && m.customTemperature == 4000, "A's custom 4000 K white balance is restored")
    m.open(url: b)
    check(await settle(m) && m.recipe.shadows == 0.3 && m.recipe.autoGrade,
          "B's edits survived the round trip too (shadows \(m.recipe.shadows))")

    print("graduated exposure changes its own side of the frame and nothing else")
    // With every auto on: the meters read the frame as if the gradient were
    // not there, so neither the exposure nor the print moves outside it.
    m.open(url: c)
    _ = await settle(m)
    let cBefore = m.recipe
    var rc = m.recipe
    rc.autoExposure = true
    rc.autoGrade = true
    rc.autoDensity = true
    rc.gradients = []
    m.recipe = rc
    _ = await settle(m)
    let meteredBare = (m.meteredExposureEV, m.meteredGrade, m.meteredBrightness)
    let topBare = meanLuminance(m.displayImage, rows: 0.0...0.2)
    let bottomBare = meanLuminance(m.displayImage, rows: 0.8...1.0)
    for ev in [-2.0, 1.0] {
        var g = ExposureGradient()
        g.points = [CGPoint(x: 0.5, y: 0.3), CGPoint(x: 0.5, y: 0.6)]
        g.exposureEV = ev
        rc.gradients = [g]
        m.recipe = rc
        _ = await settle(m)
        let top = meanLuminance(m.displayImage, rows: 0.0...0.2)
        let bottom = meanLuminance(m.displayImage, rows: 0.8...1.0)
        let metered = (m.meteredExposureEV, m.meteredGrade, m.meteredBrightness)
        check((ev < 0 ? top < topBare - 0.1 : top > topBare + 0.05) && abs(bottom - bottomBare) < 0.002
                  && metered == meteredBare,
              String(format: "%+.0f EV: top %.3f -> %.3f, bottom %.3f -> %.3f, meters unchanged: %@",
                     ev, topBare, top, bottomBare, bottom, metered == meteredBare ? "yes" : "no"))
    }
    // Two -1 EV gradients over the same side print exactly like one at -2 EV.
    var single = ExposureGradient()
    single.points = [CGPoint(x: 0.5, y: 0.3), CGPoint(x: 0.5, y: 0.6)]
    single.exposureEV = -2
    rc.gradients = [single]
    m.recipe = rc
    _ = await settle(m)
    let topSingle = meanLuminance(m.displayImage, rows: 0.0...0.2)
    var half = single
    half.exposureEV = -1
    var otherHalf = half
    otherHalf.id = UUID()
    rc.gradients = [half, otherHalf]
    m.recipe = rc
    _ = await settle(m)
    let topStacked = meanLuminance(m.displayImage, rows: 0.0...0.2)
    let bottomStacked = meanLuminance(m.displayImage, rows: 0.8...1.0)
    check(abs(topStacked - topSingle) < 0.002 && abs(bottomStacked - bottomBare) < 0.002
              && (m.meteredExposureEV, m.meteredGrade, m.meteredBrightness) == meteredBare,
          String(format: "two -1 EV gradients stack to one -2 EV: top %.3f against %.3f, bottom %.3f",
                 topStacked, topSingle, bottomStacked))
    m.recipe = cBefore
    _ = await settle(m)

    print("drop a file that is not an image")
    m.open(url: a)
    _ = await settle(m)
    m.open(url: bad)
    _ = await settle(m)
    check(m.fileURL == a && size(m.displayImage) == "600x400" && m.recipe.brightness == 0.1,
          "A stays on screen with its edits")
    check(m.errorMessage?.contains("broken.tif") == true, "and the error says which file: \(m.errorMessage ?? "none")")
    check(m.canExport, "export still works for A")

    print("switch quickly")
    m.open(url: b)
    m.open(url: c)
    check(await settle(m) && m.fileURL == c && size(m.displayImage) == "500x500",
          "the last image asked for wins (C, \(size(m.displayImage)))")
    m.open(url: b)
    m.open(url: c)
    check(await settle(m) && m.fileURL == c, "going to B and straight back leaves C on screen")

    print("drag a slider fast")
    for i in 0..<60 {
        var rr = m.recipe
        rr.brightness = Double(i) / 300
        m.recipe = rr
    }
    check(await settle(m) && m.resolvedRecipe?.brightness == 59.0 / 300,
          "the frame on screen is the last value, not a queued one")

    print("pick a black-and-white film")
    var start = m.recipe
    start.filmStock = "kodak_portra_400"
    start.printStock = "kodak_portra_endura"
    m.recipe = start
    m.selectFilm("kodak_tri_x_400")
    check(m.recipe.filmStock == "kodak_tri_x_400" && m.recipe.printStock == "ilford_multigrade_rc_deluxe",
          "Tri-X moves the print onto its own paper (\(m.recipe.printStock))")
    m.selectFilm("ilford_hp5_plus")
    check(m.recipe.printStock == "ilford_multigrade_rc_deluxe", "another black-and-white film keeps the paper")
    var onColour = m.recipe
    onColour.printStock = "kodak_portra_endura"
    m.recipe = onColour
    m.selectFilm("kodak_tri_x_400")
    check(m.recipe.printStock == "kodak_portra_endura",
          "a silver film already on colour paper, chosen deliberately, stays there")
    m.recipe.printStock = "ilford_multigrade_rc_deluxe"
    m.selectFilm("kodak_portra_400")
    check(m.recipe.printStock == "kodak_portra_endura", "back to colour film, back to its colour paper")
    check(await settle(m) && m.displayImage != nil, "and it renders")

    print("pick a motion picture film")
    m.selectFilm("kodak_exr_200t")
    check(m.recipe.printStock == "fujifilm_fcp_3519", "EXR 200T moves the print onto F-CP 3519 (\(m.recipe.printStock))")
    m.selectFilm("fujifilm_eterna_500t")
    check(m.recipe.printStock == "fujifilm_fcp_3510", "Eterna 500T onto its own, F-CP 3510")
    m.selectFilm("kodak_5251_50t")
    check(m.recipe.printStock == "kodak_5385", "5251 onto its companion print film, 5385 (\(m.recipe.printStock))")
    m.recipe.printStock = "kodak_2393"
    m.selectFilm("kodak_vision3_250d")
    check(m.recipe.printStock == "kodak_2383", "Vision3 250D onto 2383, from a print film chosen by hand too")
    m.selectFilm("kodak_portra_400")
    check(m.recipe.printStock == "kodak_portra_endura", "a still film leaves the print film for its own paper")
    m.recipe.printStock = "kodak_2383"
    m.selectFilm("kodak_gold_200")
    check(m.recipe.printStock == "kodak_2383", "a print film chosen for a still film stays")
    m.selectFilm("kodak_portra_400")
    m.recipe.printStock = "kodak_portra_endura"
    check(await settle(m) && m.displayImage != nil, "and it renders")

    print("pick a slide after setting \"Print on paper\" by hand")
    // Set by hand on a negative, it stayed pinned, and every slide picked
    // afterwards printed on RA-4 paper, as a negative.
    var pinned = m.recipe
    pinned.filmStock = "kodak_portra_400"
    pinned.scanFilm = false
    m.recipe = pinned
    m.selectFilm("fujifilm_velvia_50")
    check(m.isTransparency && m.recipe.scanFilm == nil, "the slide goes on the light table, as its stock does")
    var printedSlide = m.recipe
    printedSlide.scanFilm = false
    m.recipe = printedSlide
    m.selectFilm("kodak_ektachrome_100")
    check(!m.isTransparency, "slides printed on paper by choice: another slide is printed too")
    m.selectFilm("kodak_portra_400")
    check(!m.isTransparency && m.recipe.scanFilm == nil, "and a negative after it prints, as its stock does")
    check(await settle(m) && m.displayImage != nil, "and it renders")

    print("a camera filter goes when the new film has none like it")
    var filtered = m.recipe
    filtered.filmStock = "kodak_tri_x_400"
    filtered.printStock = "ilford_multigrade_rc_deluxe"
    filtered.lensFilter = "red"
    m.recipe = filtered
    m.selectFilm("fujifilm_velvia_50")
    check(m.recipe.lensFilter == nil, "Tri-X's red filter is not carried to a slide")
    var warmed = m.recipe
    warmed.lensFilter = "812"
    m.recipe = warmed
    m.selectFilm("fujifilm_provia_100f")
    check(m.recipe.lensFilter == "812", "a slide's warming filter stays on another slide")
    m.selectFilm("kodak_portra_400")
    check(m.recipe.lensFilter == "812", "and on a colour negative, which takes Tiffen's filters too")
    check(await settle(m) && m.displayImage != nil, "and it renders")
    var converted = m.recipe
    converted.lensFilter = "85b"
    m.recipe = converted
    m.selectFilm("kodak_exr_200t")
    check(m.recipe.lensFilter == "85b", "an 85B stays on a motion picture stock")
    m.selectFilm("kodak_tri_x_400")
    check(m.recipe.lensFilter == nil, "and goes on black and white, whose filters are Wratten's")
    check(await settle(m) && m.displayImage != nil, "and it renders")

    print("Phoenix opens a little softer, at Contrast -30 on its own scale")
    // Its curves print 2.2 times as hard as Portra 400's; it opens at R200.5
    // (Felix's pick), and Reset goes back there. A grade left at a film's own
    // default moves with the film; one set by hand stays.
    var neutral = m.recipe
    neutral.filmStock = "kodak_portra_400"
    neutral.grade = Double(Constants.gradeReference)
    m.recipe = neutral
    m.selectFilm("harman_phoenix_200")
    check(m.recipe.grade == 200.5, String(format: "an untouched grade moves to Phoenix's R%.1f", m.recipe.grade))
    m.selectFilm("kodak_gold_200")
    check(m.recipe.grade == Double(Constants.gradeReference), "and back to R115 on the next film")
    var handSet = m.recipe
    handSet.grade = 130
    m.recipe = handSet
    m.selectFilm("harman_phoenix_200")
    check(m.recipe.grade == 130, "a grade set by hand stays")
    m.resetTone()
    check(m.recipe.grade == 200.5 && m.recipe.carryingLook().grade == 200.5,
          "Reset, and the next image, open Phoenix at R200.5")
    check(await settle(m) && m.displayImage != nil, "and it renders")
    m.selectFilm("kodak_portra_400")

    print("motion picture stocks have a group of their own")
    func stocks(_ groups: [(title: String, entries: [PackIndexEntry])], _ title: String) -> [String] {
        groups.first { $0.title == title }?.entries.map { $0.stock } ?? []
    }
    let filmGroups = PackIndexEntry.groups(PackLibrary.shared.filmStocks, films: true)
    let titles = filmGroups.map { $0.title }
    check(titles == ["Colour negative", "Motion picture", "Colour slide", "Black and white"],
          "films: \(titles.joined(separator: ", "))")
    let cine = stocks(filmGroups, "Motion picture"), negatives = stocks(filmGroups, "Colour negative")
    check(cine.contains("kodak_vision3_500t") && !negatives.contains("kodak_vision3_500t")
          && negatives.contains("kodak_portra_400"),
          "Vision3 sits under Motion picture, Portra under Colour negative (\(cine.count) motion picture stocks)")
    let paperGroups = PackIndexEntry.groups(PackLibrary.shared.printStocks, films: false)
    let printFilms = stocks(paperGroups, "Motion picture")
    check(["kodak_2383", "kodak_2393", "fujifilm_fcp_3519", "fujifilm_fcp_3510", "fujifilm_eterna_cp_3513di",
           "kodak_5385"].allSatisfy(printFilms.contains),
          "Kodak's and Fujifilm's print films sit under Motion picture among the papers (\(printFilms.count))")
    check(cine.contains("fujifilm_eterna_500t") && !negatives.contains("fujifilm_eterna_500t"),
          "Fujifilm's Eterna stocks sit under Motion picture too")
    let eterna = try PackLibrary.shared.pack("fujifilm_eterna_500t").targetPrint
    let exr = try PackLibrary.shared.pack("kodak_exr_200t").targetPrint
    check(eterna == "fujifilm_fcp_3510" && exr == "fujifilm_fcp_3519",
          "Fujifilm's motion picture negatives print on F-CP 3510, Kodak's EXR on F-CP 3519 (\(eterna ?? "-"), \(exr ?? "-"))")

    print("a file opened from Finder goes to the mode that reads it")
    let routes: [(String, AppMode)] = [("clip.mov", .motion), ("clip.mp4", .motion), ("frame.NEF", .stills),
                                       ("frame.dng", .stills), ("print.tif", .stills), ("photo.jpg", .stills)]
    for (name, want) in routes {
        let got = AppMode.opening(URL(fileURLWithPath: "/tmp/\(name)"))
        check(got == want, "\(name) opens in \(got.rawValue)")
    }

    print("export while a new image loads")
    let out = dir.appendingPathComponent("out.tif")
    m.open(url: a)
    await m.export(to: out, format: .tiff16)
    check(!FileManager.default.fileExists(atPath: out.path), "export refuses mid-load instead of mixing images")
    _ = await settle(m)
    await m.export(to: out, format: .tiff16)
    let written = CGImageSourceCreateWithURL(out as CFURL, nil).flatMap { CGImageSourceCreateImageAtIndex($0, 0, nil) }
    check(written?.width == 600 && written?.height == 400, "after loading, A exports at \(size(written))")

    print("export keeps the photograph's metadata")
    // Stored 300x200 and marked to be turned a quarter (orientation 6), as a
    // camera held upright writes it.
    let shot = dir.appendingPathComponent("camera.jpg")
    try writePhoto(shot, width: 300, height: 200, properties: [
        kCGImagePropertyOrientation: 6,
        kCGImagePropertyTIFFDictionary: [
            kCGImagePropertyTIFFMake: "NIKON CORPORATION", kCGImagePropertyTIFFModel: "NIKON Z f",
            kCGImagePropertyTIFFOrientation: 6, kCGImagePropertyTIFFDateTime: "2026:10:14 13:15:13",
            kCGImagePropertyTIFFSoftware: "Ver.02.00"],
        kCGImagePropertyExifDictionary: [
            kCGImagePropertyExifDateTimeOriginal: "2026:10:14 13:15:13",
            kCGImagePropertyExifSubsecTimeOriginal: "28",
            kCGImagePropertyExifOffsetTimeOriginal: "+02:00",
            kCGImagePropertyExifLensMake: "NIKON",
            kCGImagePropertyExifLensModel: "NIKKOR Z 24-200mm f/4-6.3 VR",
            kCGImagePropertyExifFNumber: 9, kCGImagePropertyExifExposureTime: 0.002,
            kCGImagePropertyExifISOSpeedRatings: [100], kCGImagePropertyExifFocalLength: 24],
        kCGImagePropertyGPSDictionary: [
            kCGImagePropertyGPSLatitude: 48.1374, kCGImagePropertyGPSLatitudeRef: "N",
            kCGImagePropertyGPSLongitude: 11.5755, kCGImagePropertyGPSLongitudeRef: "E",
            kCGImagePropertyGPSStatus: "A"],
    ])
    m.open(url: shot)
    _ = await settle(m)
    let print1 = dir.appendingPathComponent("print.jpg")
    await m.export(to: print1, format: .jpeg)
    let meta = RawLoader.metadata(of: print1) ?? [:]
    let exif = meta[kCGImagePropertyExifDictionary] as? [CFString: Any] ?? [:]
    let tiff = meta[kCGImagePropertyTIFFDictionary] as? [CFString: Any] ?? [:]
    let gps = meta[kCGImagePropertyGPSDictionary] as? [CFString: Any] ?? [:]
    check(exif[kCGImagePropertyExifDateTimeOriginal] as? String == "2026:10:14 13:15:13"
          && exif[kCGImagePropertyExifSubsecTimeOriginal] as? String == "28"
          && exif[kCGImagePropertyExifOffsetTimeOriginal] as? String == "+02:00",
          "when it was taken, to the hundredth and with the time zone")
    check(tiff[kCGImagePropertyTIFFMake] as? String == "NIKON CORPORATION"
          && tiff[kCGImagePropertyTIFFModel] as? String == "NIKON Z f", "the camera")
    check(exif[kCGImagePropertyExifLensModel] as? String == "NIKKOR Z 24-200mm f/4-6.3 VR"
          && exif[kCGImagePropertyExifLensMake] as? String == "NIKON", "the lens")
    check(exif[kCGImagePropertyExifFNumber] as? Double == 9 && exif[kCGImagePropertyExifExposureTime] as? Double == 0.002
          && exif[kCGImagePropertyExifISOSpeedRatings] as? [Int] == [100]
          && exif[kCGImagePropertyExifFocalLength] as? Double == 24, "aperture, shutter, ISO and focal length")
    check((gps[kCGImagePropertyGPSLatitude] as? Double).map { abs($0 - 48.1374) < 1e-4 } == true
          && (gps[kCGImagePropertyGPSLongitude] as? Double).map { abs($0 - 11.5755) < 1e-4 } == true,
          "where it was taken")
    let upright = CGImageSourceCreateWithURL(print1 as CFURL, nil).flatMap { CGImageSourceCreateImageAtIndex($0, 0, nil) }
    check(meta[kCGImagePropertyOrientation] as? Int == 1 && upright?.width == 200 && upright?.height == 300,
          "upright pixels, marked as needing no turn (\(size(upright)))")
    check(tiff[kCGImagePropertyTIFFSoftware] as? String == "FLX Film Lab"
          && exif[kCGImagePropertyExifPixelXDimension] as? Int == 200
          && exif[kCGImagePropertyExifPixelYDimension] as? Int == 300,
          "FLX Film Lab as the software (its version in the app), and the export's own size")
    let filmName = (try? PackLibrary.shared.pack(m.recipe.filmStock))?.name ?? "?"
    let paperName = (try? PackLibrary.shared.pack(m.recipe.printStock))?.name ?? "?"
    let said = "Film: \(filmName); paper: \(paperName)"
    check(exif[kCGImagePropertyExifUserComment] as? String == said
          && tiff[kCGImagePropertyTIFFImageDescription] as? String == said,
          "the film and the paper, in the user comment and the description: \(exif[kCGImagePropertyExifUserComment] ?? "-")")

    // A camera's own comment and description stay, the film after the comment.
    let commented = dir.appendingPathComponent("commented.jpg")
    try writePhoto(commented, width: 300, height: 200, properties: [
        kCGImagePropertyTIFFDictionary: [kCGImagePropertyTIFFImageDescription: "Harbour, early"],
        kCGImagePropertyExifDictionary: [kCGImagePropertyExifUserComment: "Roll 12"],
    ])
    m.open(url: commented)
    _ = await settle(m)
    var bw = m.recipe
    bw = bw.loadingFilm("kodak_tri_x_400")
    bw.lensFilter = "red"
    m.recipe = bw
    _ = await settle(m)
    for format in [Exporter.Format.jpeg, .tiff16, .png] {
        let out = dir.appendingPathComponent("commented print.\(format.utType.preferredFilenameExtension ?? "img")")
        await m.export(to: out, format: format)
        let meta = RawLoader.metadata(of: out) ?? [:]
        let exif = meta[kCGImagePropertyExifDictionary] as? [CFString: Any] ?? [:]
        let tiff = meta[kCGImagePropertyTIFFDictionary] as? [CFString: Any] ?? [:]
        let comment = exif[kCGImagePropertyExifUserComment] as? String ?? "-"
        let iptc = meta[kCGImagePropertyIPTCDictionary] as? [CFString: Any] ?? [:]
        let description = tiff[kCGImagePropertyTIFFImageDescription] ?? iptc[kCGImagePropertyIPTCCaptionAbstract]
        let filter = (try? PackLibrary.shared.pack("kodak_tri_x_400"))?.lensFilter("red")?.designation ?? "?"
        check(comment.hasPrefix("Roll 12\nFilm: Kodak Tri-X 400; camera filter: \(filter); paper: ")
              && description as? String == "Harbour, early"
              && tiff[kCGImagePropertyTIFFSoftware] as? String == "FLX Film Lab",
              "\(format.utType.preferredFilenameExtension ?? "?"): the camera's comment and description stay, the film, its filter and the paper follow: \(comment.replacingOccurrences(of: "\n", with: " / "))")
    }
    var slide = m.recipe.loadingFilm("fujifilm_provia_100f")
    slide.scanFilm = nil
    check(Exporter.Provenance(recipe: slide).description == "Film: Fujifilm Provia 100F; viewed on a light table",
          "a slide viewed as one names the light table, not a paper: \(Exporter.Provenance(recipe: slide).description)")

    // A manual lens Nikon names only in its maker notes, and the GPS block a
    // Leica M writes without a fix.
    let manual = dir.appendingPathComponent("manual.jpg")
    try writePhoto(manual, width: 300, height: 200, properties: [
        kCGImagePropertyExifAuxDictionary: [kCGImagePropertyExifAuxLensModel: "90.0 mm f/2.8"],
        kCGImagePropertyGPSDictionary: [
            kCGImagePropertyGPSLatitude: 0, kCGImagePropertyGPSLatitudeRef: "N",
            kCGImagePropertyGPSLongitude: 0, kCGImagePropertyGPSLongitudeRef: "E",
            kCGImagePropertyGPSStatus: "V"],
    ])
    m.open(url: manual)
    _ = await settle(m)
    let print2 = dir.appendingPathComponent("manual print.jpg")
    await m.export(to: print2, format: .jpeg)
    let manualMeta = RawLoader.metadata(of: print2) ?? [:]
    check((manualMeta[kCGImagePropertyExifDictionary] as? [CFString: Any])?[kCGImagePropertyExifLensModel] as? String
          == "90.0 mm f/2.8", "a manual lens is named in the standard lens tag")
    check(manualMeta[kCGImagePropertyGPSDictionary] == nil, "a GPS block without a fix is left out, not placed at 0°, 0°")

    print("copy one image's settings and paste them onto another")
    check(!m.canPasteSettings, "nothing to paste before anything is copied")
    m.open(url: a)
    _ = await settle(m)
    var source = m.recipe
    source.filmStock = "fujifilm_pro_400h"
    source.printStock = "fujifilm_crystal_archive_typeii"
    source.brightness = 0.07
    source.autoExposure = false
    source.exposureEV = 0.6
    var sourceMask = LocalMask()
    sourceMask.shape = .oval
    sourceMask.points = [CGPoint(x: 0.3, y: 0.3), CGPoint(x: 0.4, y: 0.3), CGPoint(x: 0.3, y: 0.4)]
    source.masks = [sourceMask]
    m.recipe = source
    m.whiteBalance = .custom
    m.customTemperature = 3800
    _ = await settle(m)
    m.copySettings()
    m.open(url: b)
    _ = await settle(m)
    var own = LocalMask()
    own.shape = .oval
    own.points = [CGPoint(x: 0.6, y: 0.6), CGPoint(x: 0.7, y: 0.6), CGPoint(x: 0.6, y: 0.7)]
    var target = m.recipe
    target.masks = [own]
    m.recipe = target
    _ = await settle(m)
    m.pasteSettings()
    check(await settle(m) && m.fileURL == b && m.recipe.filmStock == "fujifilm_pro_400h"
              && m.recipe.printStock == "fujifilm_crystal_archive_typeii" && m.recipe.brightness == 0.07
              && !m.recipe.autoExposure && m.recipe.exposureEV == 0.6,
          "B takes A's film, paper, sliders and autos")
    check(m.whiteBalance == .custom && m.customTemperature == 3800, "and its white balance, 3800 K")
    check(m.recipe.masks.map(\.id) == [own.id], "but keeps its own mask, not A's")
    check(m.displayImage != nil && m.resolvedRecipe?.filmStock == "fujifilm_pro_400h", "and renders with them")

    print("crop, turn and level a print")
    m.open(url: a)
    _ = await settle(m)
    var auto = m.recipe
    auto.autoExposure = true
    auto.masks = []
    m.recipe = auto
    m.setCropAspect(.fiveFour)
    check(await settle(m) && size(m.displayImage) == "500x400", "5:4 on A's 3:2 frame is \(size(m.displayImage))")
    m.swapCropOrientation()
    check(await settle(m) && size(m.displayImage) == "320x400", "stood upright, \(size(m.displayImage))")
    m.turn(clockwise: true)
    check(await settle(m) && size(m.displayImage) == "400x320",
          "turned a quarter, the crop turns with the picture: \(size(m.displayImage))")
    let cropped = dir.appendingPathComponent("cropped.tif")
    await m.export(to: cropped, format: .tiff16)
    let croppedImage = CGImageSourceCreateWithURL(cropped as CFURL, nil).flatMap { CGImageSourceCreateImageAtIndex($0, 0, nil) }
    check(croppedImage?.width == 400 && croppedImage?.height == 320, "and exports so: \(size(croppedImage))")

    // A brightens to the right, so a crop of its right side meters darker.
    m.isCropping = true
    check(await settle(m) && size(m.displayImage) == "400x600", "cropping shows the whole turned frame, \(size(m.displayImage))")
    let before = m.meteredExposureEV
    m.editGeometry { g, f in
        g.setAspect(.free, frame: f)
        g.setCrop(CGRect(x: 0, y: 400, width: 400, height: 200), frame: f)   // the frame's right side, turned
    }
    check(await settle(m) && m.meteredExposureEV == before, "the meters hold their reading while the crop is drawn")
    m.endCropping(keep: true)
    check(await settle(m) && size(m.displayImage) == "400x200" && (m.meteredExposureEV ?? 0) < (before ?? 0) - 0.1,
          String(format: "and read only the crop once it is done: %+.2f EV, from %+.2f",
                 m.meteredExposureEV ?? .nan, before ?? .nan))
    let kept = m.recipe.geometry
    m.isCropping = true
    m.setStraighten(20)
    m.endCropping(keep: false)
    check(await settle(m) && m.recipe.geometry == kept && size(m.displayImage) == "400x200",
          "Cancel goes back to the crop there was")

    var oval = LocalMask()
    oval.shape = .oval
    oval.points = m.framePoints([CGPoint(x: 0.5, y: 0.5), CGPoint(x: 0.72, y: 0.5), CGPoint(x: 0.5, y: 0.72)])
    check(abs(oval.points[0].x - 5.0 / 6) < 1e-9 && abs(oval.points[0].y - 0.5) < 1e-9,
          String(format: "a new mask starts in the middle of the print, on the frame at (%.3f, %.3f)",
                 oval.points[0].x, oval.points[0].y))

    m.copySettings()
    m.open(url: c)
    _ = await settle(m)
    m.pasteSettings()
    check(await settle(m) && m.recipe.frameGeometry == nil && size(m.displayImage) == "500x500",
          "pasted settings leave C uncropped, \(size(m.displayImage))")
    let d = dir.appendingPathComponent("D.tif")
    try writeImage(d, width: 640, height: 480, colour: (1.0, 1.0, 1.0))
    m.open(url: a)
    _ = await settle(m)
    m.open(url: d)
    check(await settle(m) && m.recipe.frameGeometry == nil && size(m.displayImage) == "640x480",
          "an image opened for the first time starts uncropped, \(size(m.displayImage))")
    m.open(url: a)
    check(await settle(m) && size(m.displayImage) == "400x200", "and going back to A, its crop is still there")

    print("save settings as a preset, and put it on an image in a later session")
    // The suite's own folder, and deletes that do not go to the Trash.
    var store = PresetStore(directory: dir.appendingPathComponent("Presets"))
    store.discard = { try FileManager.default.removeItem(at: $0) }
    let first = DocumentModel(presetStore: store)
    check(first.presets.isEmpty, "no presets to begin with")
    first.open(url: b)
    _ = await settle(first)
    var look = first.recipe
    look.filmStock = "kodak_ektar_100"
    look.printStock = "kodak_portra_endura"
    look.brightness = 0.05
    look.autoGrade = true
    var dodge = LocalMask()
    dodge.shape = .oval
    dodge.points = [CGPoint(x: 0.4, y: 0.4), CGPoint(x: 0.5, y: 0.4), CGPoint(x: 0.4, y: 0.5)]
    look.masks = [dodge]
    first.recipe = look
    first.whiteBalance = .custom
    first.customTemperature = 4200
    first.setCropAspect(.fourThree)
    _ = await settle(first)
    first.savePreset(named: "  Ektar, warm  ")
    let file = dir.appendingPathComponent("Presets/Ektar, warm.json")
    check(first.presets.map(\.name) == ["Ektar, warm"] && FileManager.default.fileExists(atPath: file.path),
          "saved under its name, as a file: \(first.presets.map(\.name))")
    check(first.presets.first.map { $0.recipe.masks.isEmpty && $0.recipe.frameGeometry == nil } == true,
          "without the mask or the crop")

    let later = DocumentModel(presetStore: store)
    check(later.presets.map(\.name) == ["Ektar, warm"], "a new session finds it")
    later.open(url: c)
    _ = await settle(later)
    var withBurn = later.recipe
    var burn = LocalMask()
    burn.shape = .oval
    burn.points = [CGPoint(x: 0.7, y: 0.7), CGPoint(x: 0.8, y: 0.7), CGPoint(x: 0.7, y: 0.8)]
    withBurn.masks = [burn]
    later.recipe = withBurn
    later.turn(clockwise: true)
    _ = await settle(later)
    later.applyPreset(later.presets[0])
    check(await settle(later) && later.recipe.filmStock == "kodak_ektar_100"
              && later.recipe.printStock == "kodak_portra_endura" && later.recipe.brightness == 0.05
              && later.recipe.autoGrade && later.whiteBalance == .custom && later.customTemperature == 4200,
          "C takes the preset's film, paper, sliders, autos and white balance")
    check(later.recipe.masks.map(\.id) == [burn.id] && later.recipe.geometry.quarterTurns == 1
              && later.resolvedRecipe?.filmStock == "kodak_ektar_100",
          "keeps its own mask and its turn, and renders with the preset")
    later.recipe.brightness = 0.1
    later.savePreset(named: "Ektar, warm")
    let files = (try? FileManager.default.contentsOfDirectory(atPath: dir.appendingPathComponent("Presets").path)) ?? []
    check(later.presets.count == 1 && later.presets[0].recipe.brightness == 0.1 && files.count == 1,
          "saving under the same name replaces it")
    later.deletePreset(later.presets[0])
    check(later.presets.isEmpty && !FileManager.default.fileExists(atPath: file.path), "and Delete takes it away")

    print("the preview size is kept between launches")
    // Preferences of the suite's own, not the app's.
    let suite = "flx-apptest-\(getpid())"
    let prefs = UserDefaults(suiteName: suite)!
    defer { prefs.removePersistentDomain(forName: suite) }
    let launched = DocumentModel(presetStore: store, defaults: prefs)
    check(launched.proxyMaxDimension == 2560, "2560 to begin with")
    let big = dir.appendingPathComponent("big.tif")
    try writeImage(big, width: 4600, height: 3000, colour: (1.0, 1.0, 1.0))
    launched.open(url: big)
    check(await settle(launched) && size(launched.displayImage) == "2560x1669", "so a large image previews at \(size(launched.displayImage))")
    launched.proxyMaxDimension = 4096
    check(await settle(launched) && size(launched.displayImage) == "4096x2671", "and at \(size(launched.displayImage)) once 4096 is chosen")
    let smaller = dir.appendingPathComponent("smaller.tif")
    try writeImage(smaller, width: 3000, height: 2000, colour: (1.0, 1.0, 1.0))
    launched.open(url: smaller)
    check(await settle(launched) && size(launched.displayImage) == "3000x2000",
          "a file smaller than that previews at its own size, \(size(launched.displayImage)), never enlarged")
    let relaunched = DocumentModel(presetStore: store, defaults: prefs)
    check(relaunched.proxyMaxDimension == 4096, "which is still the choice after a relaunch")
    prefs.set(5000, forKey: "previewSize")
    check(DocumentModel(presetStore: store, defaults: prefs).proxyMaxDimension == 2560,
          "and a size the app does not offer goes back to 2560")
}

/// The 8-bit pixels of `image` in `window` (in its pixels, top row first),
/// drawn into an sRGB context.
func pixels(_ image: CGImage, window: CGRect) -> [UInt8] {
    let w = Int(window.width), h = Int(window.height)
    var buffer = [UInt8](repeating: 0, count: w * h * 4)
    let ctx = CGContext(data: &buffer, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    ctx.interpolationQuality = .none
    ctx.draw(image, in: CGRect(x: -window.minX, y: -(CGFloat(image.height) - window.maxY),
                               width: CGFloat(image.width), height: CGFloat(image.height)))
    return buffer
}

@MainActor
func loupe() async throws {
    print("the loupe shows the print at 100%, as the export has it")
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("flx-apptest-loupe-\(getpid())")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }
    // Larger than the preview, with fine texture and points of light, so the
    // loupe's part has grain, halation and couplers reaching across it.
    let w = 4600, h = 3000
    var px = [Float](repeating: 1, count: w * h * 4)
    var state: UInt32 = 99
    for y in 0..<h { for x in 0..<w {
        state = state &* 1664525 &+ 1013904223
        let n = Float(state >> 8) / 16777216
        let v = 0.03 + 0.4 * Float(x) / Float(w) + ((x / 37 + y / 53) % 2 == 0 ? 0.1 : 0) + 0.05 * n
        let o = (y * w + x) * 4
        px[o] = v * 1.1; px[o + 1] = v; px[o + 2] = v * 0.9
        if (x - 1400) * (x - 1400) + (y - 1800) * (y - 1800) < 64 { px[o] = 1; px[o + 1] = 1; px[o + 2] = 1 }
    }}
    let big = dir.appendingPathComponent("big.tif")
    try Exporter.write(linear: px, width: w, height: h, to: big, format: .tiff16, colorSpace: "sRGB")

    let m = DocumentModel(presetStore: PresetStore(directory: dir.appendingPathComponent("presets")),
                          defaults: UserDefaults(suiteName: "flx-apptest-loupe-\(getpid())")!)
    m.open(url: big)
    let settled = await settle(m)
    check(settled && size(m.displayImage).hasPrefix("2560x"),
          "a picture larger than the preview (\(size(m.displayImage)) on screen)")
    m.setLoupeSize(width: 400, height: 300)
    m.loupeOn = true
    check(await waitUntil(timeout: 60) { !m.loupeBusy && m.loupeImage != nil } && size(m.loupeImage) == "400x300",
          "the loupe reads it at full size and shows a part of it (\(size(m.loupeImage)))")

    func compare(_ name: String, centre: CGPoint) async throws {
        m.moveLoupe(to: centre)
        _ = await settle(m)
        _ = await waitUntil(timeout: 30) { !m.loupeBusy }
        let out = dir.appendingPathComponent("export \(name).tif")
        await m.export(to: out, format: .tiff16)
        guard let shown = m.loupeImage,
              let file = CGImageSourceCreateWithURL(out as CFURL, nil).flatMap({ CGImageSourceCreateImageAtIndex($0, 0, nil) })
        else { check(false, "\(name): the loupe and the export both made a picture"); return }
        let ow = file.width, oh = file.height
        let x = min(max(Int((centre.x * CGFloat(ow) - 200).rounded()), 0), ow - 400)
        let y = min(max(Int((centre.y * CGFloat(oh) - 150).rounded()), 0), oh - 300)
        let a = pixels(shown, window: CGRect(x: 0, y: 0, width: 400, height: 300))
        let b = pixels(file, window: CGRect(x: x, y: y, width: 400, height: 300))
        var worst = 0, mean = 0.0
        for i in 0..<a.count where i % 4 != 3 {
            let d = abs(Int(a[i]) - Int(b[i]))
            worst = max(worst, d)
            mean += Double(d)
        }
        check(worst <= 1, String(format: "%@: the loupe's pixels are the export's there, to %d of 255 (mean %.3f)",
                                 name, worst, mean / Double(a.count * 3 / 4)))
    }
    try await compare("as it comes", centre: CGPoint(x: 0.31, y: 0.6))
    try await compare("at a corner", centre: CGPoint(x: 0.99, y: 0.02))
    var shaped = m.recipe
    var g = FrameGeometry()
    g.turn(clockwise: false)
    g.setStraighten(-1.8)
    shaped.geometry = g
    var curves = PrintCurves()
    curves.rgb = [CurvePoint(x: 0, y: 0.03), CurvePoint(x: 0.4, y: 0.33), CurvePoint(x: 0.75, y: 0.82), CurvePoint(x: 1, y: 1)]
    shaped.curves = curves
    m.recipe = shaped
    try await compare("turned, levelled and curved", centre: CGPoint(x: 0.45, y: 0.4))
    m.loupeOn = false
    check(m.loupeImage == nil && !m.loupeBusy, "put away, the loupe lets the picture at full size go")
}

/// ⌘Z and ⇧⌘Z, through the model as the Edit menu drives it: one step per
/// gesture, a double-click reset joined to its clicks, named resets and
/// pastes, a history for each image, and a cancelled crop leaving none.
@MainActor
func undoRedo() async throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("flx-undotest-\(getpid())")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    let suite = "flx-undotest-\(getpid())"
    let defaults = UserDefaults(suiteName: suite)!
    defer {
        try? FileManager.default.removeItem(at: dir)
        defaults.removePersistentDomain(forName: suite)
    }
    let a = dir.appendingPathComponent("A.tif"), b = dir.appendingPathComponent("B.tif")
    try writeImage(a, width: 600, height: 400, colour: (1.2, 1.0, 0.8))
    try writeImage(b, width: 300, height: 450, colour: (0.8, 1.0, 1.2))
    let m = DocumentModel(presetStore: PresetStore(directory: dir.appendingPathComponent("Presets")),
                          defaults: defaults)
    var mouseDown = false
    m.history.mouseIsDown = { mouseDown }
    m.history.joinInterval = 0.3
    /// After an action, as long as the app's run loop takes to close its step.
    func pause(_ ms: Int = 150) async { try? await Task.sleep(for: .milliseconds(ms)) }
    /// A slider dragged through `values`, from press to release, as the
    /// inspector's binding sets it.
    func drag(_ values: [Double], _ set: (inout Recipe, Double) -> Void) async {
        mouseDown = true
        for v in values {
            var r = m.recipe
            set(&r, v)
            m.recipe = r
            await pause(40)
        }
        mouseDown = false
        await pause()
    }
    let names = { m.history.stacks.undo.map(\.name) }

    print("undo and redo")
    m.open(url: a)
    check(await settle(m) && m.history.undoName == nil && m.history.redoName == nil,
          "an image opens with nothing to undo or redo")

    mouseDown = true
    for ev in [0.3, 0.6, 0.9] {
        var r = m.recipe
        r.exposureEV = ev
        r.autoExposure = false
        m.recipe = r
        await pause(40)
    }
    check(m.history.undoName == nil, "while the slider is held, the step stays open")
    mouseDown = false
    await pause()
    check(names() == ["Exposure"], "a slider dragged from press to release is one step: \(names())")
    m.history.undo()
    check(m.recipe.exposureEV == 0 && m.recipe.autoExposure && m.history.undoName == nil
              && m.history.redoName == "Exposure",
          "⌘Z puts it back where it was before the drag, its auto on again")
    check(await settle(m) && m.resolvedRecipe?.autoExposure == true, "and the print follows")
    m.history.redo()
    check(m.recipe.exposureEV == 0.9 && !m.recipe.autoExposure && m.history.undoName == "Exposure"
              && m.history.redoName == nil, "⇧⌘Z does it again")

    m.recipe.grainEnabled = false
    await pause()
    m.recipe.halationEnabled = false
    await pause()
    check(names() == ["Exposure", "Grain", "Halation"], "each click is a step of its own: \(names())")

    // A double-click on a slider: the slider takes both clicks as moves, and
    // the reset follows the second.
    await drag([0.2]) { $0.brightness = $1 }
    await drag([0.25]) { $0.brightness = $1 }
    m.recipe.brightness = 0
    await pause()
    check(names() == ["Exposure", "Grain", "Halation"],
          "a double-click that resets a slider already at its default leaves nothing to undo")
    await drag([0.3, 0.4]) { $0.brightness = $1 }
    try? await Task.sleep(for: .milliseconds(400))
    await drag([0.2]) { $0.brightness = $1 }
    await drag([0.25]) { $0.brightness = $1 }
    m.recipe.brightness = 0
    await pause()
    check(names() == ["Exposure", "Grain", "Halation", "Brightness", "Brightness"]
              && m.history.stacks.undo.last?.before.recipe.brightness == 0.4,
          "a double-click reset is one step with its clicks: \(names())")
    m.history.undo()
    check(m.recipe.brightness == 0.4, "so ⌘Z takes the slider back to where it was before both clicks")
    m.history.redo()

    m.turn(clockwise: true)
    m.history.finish()
    m.turn(clockwise: true)
    await pause()
    check(names().suffix(2) == ["Turn", "Turn"], "two quick turns by key are two steps: \(names().suffix(2))")
    m.history.undo()
    check(m.recipe.geometry.quarterTurns == 1, "so ⌘Z undoes one of them")
    m.history.undo()
    m.resetTone()
    check(m.history.undoName == "Brightness", "a reset that changes nothing leaves nothing to undo")
    m.recipe.highlights = -0.2
    await pause()
    m.resetTone()
    check(m.history.undoName == "Reset Tone" && m.recipe.highlights == 0 && !m.recipe.grainEnabled,
          "a reset is a step of its own, called by its name")
    m.resetExposure()
    check(names().suffix(2) == ["Reset Tone", "Reset Exposure"] && m.recipe.autoExposure,
          "Reset Exposure is one step though it sets four things")
    m.history.undo()
    check(m.recipe.exposureEV == 0.9 && !m.recipe.autoExposure, "and one ⌘Z undoes all four")

    m.whiteBalance = .custom
    await pause(400)
    mouseDown = true
    m.customTemperature = 4500
    await pause(40)
    m.customTemperature = 4000
    mouseDown = false
    await pause()
    check(names().suffix(2) == ["White Balance", "White Balance"],
          "choosing a white balance and dragging its temperature are two steps")
    _ = await settle(m)
    m.history.undo()
    m.history.undo()
    check(m.whiteBalance == .filmReference && m.customTemperature == 5500,
          "undone, the white balance is the film's again")
    check(await settle(m) && m.whiteBalanceSummary.hasPrefix("D55") && m.displayImage != nil,
          "and the picture is decoded for it again: \(m.whiteBalanceSummary)")
    check(m.history.redoName == "White Balance", "with both to redo")
    m.recipe.snap = 0.1
    await pause()
    check(m.history.redoName == nil && m.history.undoName == "Midtone Contrast",
          "a new change after undoing leaves nothing to redo: \(m.history.undoName ?? "-")")

    var mask = LocalMask()
    mask.shape = .oval
    mask.points = [CGPoint(x: 0.5, y: 0.5), CGPoint(x: 0.7, y: 0.5), CGPoint(x: 0.5, y: 0.7)]
    m.recipe.masks.append(mask)
    await pause()
    mouseDown = true
    for dx in [0.01, 0.02, 0.05] {
        m.recipe.masks[0].points[1].x = 0.7 + dx
        await pause(40)
    }
    mouseDown = false
    await pause()
    m.recipe.masks.removeAll()
    await pause()
    check(names().suffix(3) == ["Add Mask", "Mask", "Delete Mask"], "masks: \(names().suffix(3))")
    m.history.undo()
    check(m.recipe.masks.first?.points[1].x == 0.7 + 0.05, "undoing the delete brings the mask back as it was")
    m.history.undo()
    check(m.recipe.masks.first?.points[1].x == 0.7, "undoing the drag puts its point back")
    m.history.undo()
    check(m.recipe.masks.isEmpty, "and undoing the add takes it away")
    m.history.redo()
    m.history.redo()
    m.history.redo()

    mouseDown = true
    m.recipe.toe = 0.2
    m.history.undo()
    mouseDown = false
    check(m.recipe.toe == 0 && m.history.redoName == "Blacks", "⌘Z during a drag undoes the drag so far")
    m.history.redo()

    defaults.set(Vocabulary.darkroom.rawValue, forKey: "vocabulary")
    m.recipe.density = 0.1
    await pause()
    check(m.history.undoName == "Density", "the Edit menu uses the darkroom names when the controls do")
    defaults.set(Vocabulary.photographic.rawValue, forKey: "vocabulary")
    m.recipe.density = 0.2
    await pause(400)
    m.recipe.density = 0.3
    await pause()
    check(m.history.undoName == "Print Exposure", "and the photographic ones otherwise")

    print("undo with several images")
    let aSteps = names()
    m.open(url: b)
    check(await settle(m) && m.fileURL == b && m.history.undoName == nil, "B starts with nothing to undo")
    m.recipe.shadows = 0.3
    await pause()
    m.open(url: a)
    check(await settle(m) && names() == aSteps, "A keeps its own history: \(names().count) steps")
    m.history.undo()
    check(m.recipe.density == 0.2, "and ⌘Z on A undoes A's last change")
    m.history.redo()
    m.copySettings()
    m.open(url: b)
    check(await settle(m) && names() == ["Shadows"], "B keeps its own: \(names())")
    m.pasteSettings()
    check(await settle(m) && m.history.undoName == "Paste Settings" && m.recipe.density == 0.3,
          "a paste is one step, Paste Settings")
    m.history.undo()
    check(await settle(m) && m.recipe.density == 0 && m.recipe.shadows == 0.3 && m.recipe.autoExposure
              && m.whiteBalance == .filmReference,
          "and undoing it gives B its own settings back")

    print("undo while cropping")
    m.open(url: a)
    _ = await settle(m)
    m.recipe.glareEnabled = false
    await pause()
    m.history.undo()
    let last = m.history.undoName, redo = m.history.redoName
    m.isCropping = true
    m.setStraighten(2)
    await pause()
    m.editGeometry { g, f in g.setCrop(CGRect(x: 100, y: 50, width: 300, height: 200), frame: f) }
    await pause()
    check(names().suffix(2) == ["Straighten", "Crop"], "each change to the crop is a step: \(names().suffix(2))")
    m.history.undo()
    check(m.recipe.geometry.straighten == 2 && m.recipe.geometry.crop == nil && m.isCropping,
          "⌘Z while cropping undoes the last change to the crop")
    m.history.redo()
    m.endCropping(keep: false)
    check(m.recipe.geometry == .identity && m.history.undoName == last && m.history.redoName == redo,
          "a cancelled crop leaves no steps behind: \(last ?? "-") to undo and \(redo ?? "-") to redo, as before")
    m.isCropping = true
    m.setStraighten(1.5)
    await pause()
    m.recipe.shadows = 0.2
    await pause()
    m.endCropping(keep: false)
    check(m.recipe.geometry == .identity && m.recipe.shadows == 0.2 && m.history.undoName == "Cancel Crop",
          "with another change made meanwhile, going back is a step of its own")
    m.history.undo()
    check(m.recipe.geometry.straighten == 1.5, "which ⌘Z undoes")
    m.history.undo()
    m.history.undo()
    m.isCropping = true
    m.setStraighten(-1)
    await pause()
    m.endCropping(keep: true)
    check(m.history.undoName == "Straighten", "Done keeps the crop's steps")
    m.history.undo()
    check(await settle(m) && m.recipe.geometry == .identity && size(m.displayImage) == "600x400",
          "and ⌘Z afterwards undoes them: \(size(m.displayImage))")
}

try await undoRedo()
try await loupe()
try await run()
print(failures == 0 ? "\nall app checks passed" : "\n\(failures) app check(s) failed")
exit(failures == 0 ? 0 : 1)
