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
    // Start from the shipped auto preferences.
    for key in ["prefersAutoExposure", "prefersAutoBrightness", "prefersAutoContrast"] {
        UserDefaults.standard.removeObject(forKey: key)
    }

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

    print("a preference for an auto carries to new images")
    m.rememberAutoPreference(\.autoGrade, isOn: true)
    var rb = m.recipe
    rb.autoGrade = true
    rb.shadows = 0.3
    m.recipe = rb
    _ = await settle(m)
    m.open(url: c)
    check(await settle(m) && m.fileURL == c, "C is on screen")
    check(m.recipe.autoGrade && m.meteredGrade != nil,
          "C opens with Auto Contrast on, metered at R\(m.meteredGrade.map { String(format: "%.0f", $0) } ?? "-")")

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
    check(stocks(paperGroups, "Motion picture").contains("kodak_2383"),
          "2383 print film sits under Motion picture among the papers")
    check(cine.contains("fujifilm_eterna_500t") && !negatives.contains("fujifilm_eterna_500t"),
          "Fujifilm's Eterna stocks sit under Motion picture too")

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
          "FLX Film Lab as the software, and the export's own size")

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
}

try await run()
print(failures == 0 ? "\nall app checks passed" : "\n\(failures) app check(s) failed")
exit(failures == 0 ? 0 : 1)
