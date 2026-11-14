import Foundation
import AVFoundation
import QuartzCore
@testable import FLXCore

// Drives ClipModel the way the app does — open, grade, open another, go back,
// fail to open, play, export, export a LUT, relaunch — and checks the state
// after each step. Uses generated N-Log clips; the display link is simulated
// by ticking the player by hand. The player is muted.

var failures = 0
func check(_ ok: Bool, _ message: String) {
    print("   \(ok ? "ok  " : "FAIL") \(message)")
    if !ok { failures += 1 }
}

@MainActor
func waitUntil(_ m: ClipModel, timeout: Double = 15, _ condition: () -> Bool) async -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
        m.playback.tick(hostTime: CACurrentMediaTime())
        if condition() { return true }
        try? await Task.sleep(for: .milliseconds(15))
    }
    return condition()
}

/// Where the tests keep their libraries, never in the app's own.
let scratch = FileManager.default.temporaryDirectory.appendingPathComponent("flx-motionapptest-scratch-\(getpid())")

/// A library of its own for each model.
@MainActor func scratchLibrary() -> ImageLibrary {
    let library = ImageLibrary(directory: scratch.appendingPathComponent("library-\(UUID().uuidString)"))
    library.saveDelay = .milliseconds(50)
    return library
}

@MainActor
func settled(_ m: ClipModel) async -> Bool {
    try? await Task.sleep(for: .milliseconds(100))
    return await waitUntil(m) { !m.isOpening && !m.isMetering && m.bakedRecipe == m.effectiveRecipe
        && m.bakedFormat == m.format && !m.isExporting }
}

@MainActor
func run() async throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("flx-motionapptest-\(getpid())")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }
    let a = dir.appendingPathComponent("A.mov"), b = dir.appendingPathComponent("B.mov")
    let c = dir.appendingPathComponent("C.mov"), bad = dir.appendingPathComponent("broken.mov")
    try TestClip.write(to: a, width: 640, height: 360, frames: 24, fps: 24)
    try TestClip.write(to: b, width: 480, height: 270, frames: 12, fps: 25, audio: false)
    try TestClip.write(to: c, width: 320, height: 180, frames: 6, fps: 30, audio: false)
    try Data("not a movie".utf8).write(to: bad)
    UserDefaults.standard.removeObject(forKey: ClipModel.lookDefaultsKey)

    print("a fresh start")
    let m = ClipModel(library: scratchLibrary())
    m.playback.player.isMuted = true
    check(await settled(m) && m.look.recipe.filmStock == "kodak_portra_800"
          && m.look.recipe.printStock == "kodak_portra_endura",
          "starts on Portra 800 printed on Portra Endura, with the look baked")
    check(!m.look.effects.bloom.enabled && !m.look.effects.glow.enabled && !m.look.effects.halation.enabled,
          "with every effect off")

    print("open, read and grade")
    m.open(a)
    check(await settled(m) && m.clip?.url == a, "A opens")
    check(await waitUntil(m) { m.reading != nil }, "its first frame arrives without pressing play")
    if let r = m.reading {
        check(r.bitDepth == 10 && !r.fullRange && r.matrixName == "BT.2020",
              "read as \(r.bitDepth)-bit, \(r.fullRange ? "full" : "video") range, \(r.matrixName)")
    }

    print("auto exposure")
    check(m.look.recipe.autoExposure, "is on for a first clip")
    check(await settled(m) && m.meteredExposure == 0 && m.bakedRecipe?.exposureEV == 0,
          "a negative prints as shot: the meter reads 0 EV for Portra 800")
    m.look.recipe.filmStock = "kodak_ektachrome_100"
    check(await settled(m) && (m.meteredExposure ?? 0) <= -1.5 && m.bakedRecipe?.exposureEV == m.meteredExposure,
          String(format: "a slide is pulled down to hold the clip's highlights (%.2f EV)", m.meteredExposure ?? 0))
    m.look.recipe.filmStock = "kodak_portra_800"
    check(await settled(m) && m.meteredExposure == 0 && !m.isMetering, "back on the negative, without metering again")

    // Moving the slider takes over from the meter, as in the inspector.
    m.look.recipe.autoExposure = false
    m.look.recipe.exposureEV = 1
    m.look.recipe.brightness = 0.05
    m.look.effects.bloom.enabled = true
    check(await settled(m) && m.bakedRecipe?.exposureEV == 1, "an edit re-bakes the look")
    for i in 1...20 { m.look.recipe.shadows = Double(i) / 40 }
    check(await settled(m) && m.bakedRecipe?.shadows == 0.5, "a drag of 20 ticks ends on the last value")

    print("playback")
    m.playback.play()
    let started = m.playback.time
    try? await Task.sleep(for: .milliseconds(400))
    // Generous: the first run after a build compiles shaders cold.
    _ = await waitUntil(m, timeout: 5) { m.playback.time > started + 0.2 }
    check(m.playback.isPlaying && m.playback.time > started + 0.2,
          String(format: "plays (%.2f s in)", m.playback.time))
    m.playback.pause()
    m.playback.seek(to: 0.5)
    check(await waitUntil(m) { abs(m.playback.time - 0.5) < 0.05 } && !m.playback.isPlaying, "seeks while paused")

    print("another clip")
    m.open(b)
    check(await settled(m) && m.clip?.url == b, "B opens")
    check(m.look.recipe.exposureEV == 0 && m.look.recipe.brightness == 0 && m.look.recipe.shadows == 0,
          "starting from fresh exposure and tone")
    check(m.look.recipe.filmStock == "kodak_portra_800" && m.look.effects.bloom.enabled,
          "but with A's film and effects")
    m.look.recipe.filmStock = "kodak_ektar_100"
    m.look.recipe.printStock = "kodak_supra_endura"
    _ = await settled(m)

    m.open(a)
    check(await settled(m) && m.clip?.url == a, "back to A")
    check(m.look.recipe.exposureEV == 1 && m.look.recipe.shadows == 0.5 && m.look.recipe.filmStock == "kodak_portra_800",
          "with A's grade exactly as it was left")

    m.open(c)
    check(await settled(m) && m.clip?.url == c && m.look.recipe.filmStock == "kodak_portra_800"
          && m.look.recipe.exposureEV == 0,
          "a new clip C carries the look of the clip before it (A), not B's")

    print("a file that will not open")
    m.open(bad)
    _ = await settled(m)
    check(m.clip?.url == c && m.errorMessage != nil, "leaves C on screen and says why")

    print("pick a black-and-white film")
    m.selectFilm("kodak_tri_x_400")
    check(m.look.recipe.filmStock == "kodak_tri_x_400" && m.look.recipe.printStock == "ilford_multigrade_rc_deluxe",
          "Tri-X moves the print onto its own paper (\(m.look.recipe.printStock))")
    check(await settled(m) && m.bakedRecipe?.filmStock == "kodak_tri_x_400", "and bakes")
    m.look.recipe.lensFilter = "red"
    check(await settled(m) && m.bakedRecipe?.lensFilter == "red", "a camera filter re-bakes the look")
    let tint = m.look.effectsAsPrinted.halation.tint
    check(tint.r == tint.g && tint.g == tint.b && tint.r > 0.4,
          String(format: "halation prints grey on it (%.2f %.2f %.2f)", tint.r, tint.g, tint.b))
    m.selectFilm("ilford_hp5_plus")
    check(m.look.recipe.printStock == "ilford_multigrade_rc_deluxe" && m.look.recipe.lensFilter == "red",
          "another black-and-white film keeps the paper and the filter")
    m.look.recipe.printStock = "kodak_portra_endura"
    m.selectFilm("kodak_tri_x_400")
    check(m.look.recipe.printStock == "kodak_portra_endura",
          "a silver film already on colour paper, chosen deliberately, stays there")
    m.look.recipe.printStock = "ilford_multigrade_rc_deluxe"
    m.selectFilm("kodak_portra_800")
    check(m.look.recipe.printStock == "kodak_portra_endura", "back to colour film, back to its colour paper")
    check(m.look.effectsAsPrinted == m.look.effects, "where halation is Glow's own red again")
    m.look.recipe.printStock = "ilford_multigrade_rc_deluxe"
    check(m.look.effectsAsPrinted.halation.tint.r == m.look.effectsAsPrinted.halation.tint.b,
          "and grey once more for colour film printed on silver paper")
    m.look.recipe.printStock = "kodak_portra_endura"
    m.look.recipe.lensFilter = nil
    _ = await settled(m)

    print("pick a motion picture film")
    m.selectFilm("kodak_exr_200t")
    check(m.look.recipe.printStock == "fujifilm_fcp_3519",
          "EXR 200T moves the print onto F-CP 3519 (\(m.look.recipe.printStock))")
    m.selectFilm("kodak_portra_800")
    check(m.look.recipe.printStock == "kodak_portra_endura", "and a still film back onto its own paper")
    _ = await settled(m)

    print("export")
    let out = dir.appendingPathComponent("C graded.mov")
    m.look.export.codec = .hevc10
    m.export(to: out)
    check(m.isExporting, "is under way")
    m.open(a)
    check(await settled(m) && FileManager.default.fileExists(atPath: out.path) && m.clip?.url == a,
          "writes the file, while another clip opens in the meantime")
    check(m.status.hasPrefix("Exported"), "and says so: \(m.status)")
    let exported = try await ClipInfo.load(out)
    check(exported.codecName == "HEVC" && exported.bitDepth == 10 && Int(exported.frameRate.rounded()) == 30,
          "as \(exported.summary)")
    let items = (try? await AVURLAsset(url: out).load(.metadata)) ?? []
    func text(_ identifier: AVMetadataIdentifier) async -> String? {
        guard let item = items.first(where: { $0.identifier == identifier }) else { return nil }
        return try? await item.load(.stringValue)
    }
    let software = await text(.quickTimeMetadataSoftware), described = await text(.quickTimeMetadataDescription)
    check(software == "FLX Film Lab" && described == "Film: Kodak Portra 800; paper: Kodak Professional Portra Endura",
          "and names FLX Film Lab, the film and the paper in its metadata: \(described ?? "-")")

    print("export a LUT")
    let cube = dir.appendingPathComponent("look.cube")
    m.exportLUT(to: cube)
    check(await waitUntil(m) { FileManager.default.fileExists(atPath: cube.path) }, "writes the .cube")
    let text = (try? String(contentsOf: cube, encoding: .utf8)) ?? ""
    check(text.contains("LUT_3D_SIZE 65") && text.contains("video levels") && text.contains("BT.1886"),
          "65 points, N-Log video levels in, BT.1886 out")
    check(text.contains("TITLE \"Kodak Portra 800 on Kodak Professional Portra Endura\"")
              && text.contains("(Motion): Kodak Portra 800 printed on Kodak Professional Portra Endura"),
          "titled with the film on its paper")
    let negative = m.look
    m.selectFilm("fujifilm_velvia_50")
    check(m.printDescription == ("Fujifilm Velvia 50", "Fujifilm Velvia 50 on a light table"),
          "a slide's names the light table, not a paper: \(m.printDescription)")
    m.selectFilm("polaroid_553")
    check(m.printDescription == ("Polaroid 553", "Polaroid 553, an instant print processed at 70 F"),
          "an instant print's, the print it is: \(m.printDescription)")
    m.look = negative
    _ = await settled(m)

    print("log formats")
    check(m.format == .nlog && m.bakedFormat == .nlog, "the synthetic Nikon clips are read as N-Log")
    let iphone = dir.appendingPathComponent("iPhone.mov")
    try TestClip.write(to: iphone, width: 320, height: 180, frames: 6, fps: 30, audio: false, format: .appleLog)
    m.open(iphone)
    check(await settled(m) && m.clip?.url == iphone && m.format == .appleLog && m.bakedFormat == .appleLog,
          "an iPhone clip is read as Apple Log from its own tag, and the look baked for it")
    _ = await waitUntil(m) { m.reading != nil }
    let card = TestClip.Layout(width: 320, height: 180).rect(TestClip.grey)
    m.probe(at: CGPoint(x: (card.x0 + card.x1) / 2, y: (card.y0 + card.y1) / 2))
    check(m.probeText?.hasPrefix("Apple Log ") == true && m.probeText?.contains("0.0 stops") == true,
          "the probe reads its grey card in Apple Log: \(m.probeText ?? "nothing")")
    m.probe(at: nil)
    m.look.input.format = .vlog
    check(await settled(m) && m.bakedFormat == .vlog && m.reading?.format == .vlog,
          "choosing V-Log by hand re-bakes the look and re-reads the frame")
    let vcube = dir.appendingPathComponent("vlog.cube")
    m.exportLUT(to: vcube)
    _ = await waitUntil(m) { FileManager.default.fileExists(atPath: vcube.path) }
    check(((try? String(contentsOf: vcube, encoding: .utf8)) ?? "").contains("Panasonic V-Log, V-Gamut primaries, video levels"),
          "and Export LUT takes V-Log the way Resolve reads this video-range clip")
    m.open(a)
    check(await settled(m) && m.look.input.format == nil && m.format == .nlog,
          "the next clip is read from its own tags again: N-Log")
    m.open(iphone)
    check(await settled(m) && m.format == .vlog, "going back to a clip keeps the choice made for it")
    m.look.input.format = nil
    check(await settled(m) && m.format == .appleLog, "and From the file goes back to its tag")
    m.open(a)
    _ = await settled(m)

    print("relaunch")
    // A look saved before the clip meter had Auto exposure forced off.
    var old = MotionLook.initial
    old.recipe.autoExposure = false
    old.version = nil
    UserDefaults.standard.set(try JSONEncoder().encode(old), forKey: ClipModel.lookDefaultsKey)
    check(ClipModel(library: scratchLibrary()).look.recipe.autoExposure, "a look saved by the previous version gets Auto exposure back")

    m.look.effects.halation.enabled = true
    _ = await settled(m)
    let again = ClipModel(library: scratchLibrary())
    check(await settled(again) && again.look.recipe.filmStock == m.look.recipe.filmStock
          && again.look.effects.halation.enabled && again.look.export.codec == .hevc10,
          "the next launch keeps the look")
    check(again.look.recipe.exposureEV == 0, "but not one clip's exposure")
    UserDefaults.standard.removeObject(forKey: ClipModel.lookDefaultsKey)
}

/// ⌘Z and ⇧⌘Z on a clip's look, as in Stills: one step per change, named
/// resets, a history for each clip, and how files are written left out.
@MainActor
func undoRedo() async throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("flx-motionundo-\(getpid())")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer {
        try? FileManager.default.removeItem(at: dir)
        UserDefaults.standard.removeObject(forKey: ClipModel.lookDefaultsKey)
    }
    let a = dir.appendingPathComponent("A.mov"), b = dir.appendingPathComponent("B.mov")
    try TestClip.write(to: a, width: 320, height: 180, frames: 6, fps: 24, audio: false)
    try TestClip.write(to: b, width: 320, height: 180, frames: 6, fps: 25, audio: false)
    UserDefaults.standard.removeObject(forKey: ClipModel.lookDefaultsKey)
    let m = ClipModel(library: scratchLibrary())
    m.playback.player.isMuted = true
    m.history.mouseIsDown = { false }
    m.history.joinInterval = 0
    func pause() async { try? await Task.sleep(for: .milliseconds(80)) }
    let names = { m.history.stacks.undo.map(\.name) }

    print("undo and redo")
    m.open(a)
    check(await settled(m) && m.clip?.url == a && m.history.undoName == nil, "a clip opens with nothing to undo")
    m.look.recipe.brightness = 0.1
    await pause()
    m.look.effects.glow.enabled = true
    await pause()
    check(names() == ["Brightness", "Effects"], "each change is a step: \(names())")
    m.look.export.codec = .hevc10
    await pause()
    check(names() == ["Brightness", "Effects"], "how files are written is not")
    m.history.undo()
    check(!m.look.effects.glow.enabled && m.look.export.codec == .hevc10,
          "⌘Z turns the glow off again and leaves the codec as chosen")
    check(await settled(m) && m.bakedRecipe == m.effectiveRecipe, "and the look is baked again")
    m.history.redo()
    check(m.look.effects.glow.enabled && m.history.redoName == nil, "⇧⌘Z turns it back on")
    m.resetTone()
    check(m.history.undoName == "Reset Tone" && m.look.recipe.brightness == 0, "a reset is a step called by its name")
    m.history.undo()
    check(m.look.recipe.brightness == 0.1, "which ⌘Z undoes")

    m.open(b)
    check(await settled(m) && m.clip?.url == b && m.history.undoName == nil, "the next clip has its own history")
    m.look.recipe.shadows = 0.2
    await pause()
    m.open(a)
    check(await settled(m) && names() == ["Brightness", "Effects"] && m.history.redoName == "Reset Tone",
          "going back to the first, its history is as it was: \(names())")
    m.open(a)
    check(await settled(m) && names() == ["Brightness", "Effects"], "and opening it again keeps it")
    m.history.undo()
    m.history.undo()
    check(m.look.recipe.brightness == 0 && !m.look.effects.glow.enabled, "undone to how it opened")
    m.open(b)
    check(await settled(m) && names() == ["Shadows"], "the other clip kept its own")
}

/// A folder of clips in the sidebar: listed, stepped through, a look kept
/// between launches, flags, Paste Look to Selected and Export Selected.
@MainActor
func clipFolders() async throws {
    let fm = FileManager.default
    let dir = scratch.appendingPathComponent("Clips")
    try fm.createDirectory(at: dir, withIntermediateDirectories: true)
    let names = ["A.mov", "B.mov", "C.mov"]
    let urls = names.map { dir.appendingPathComponent($0) }
    for (i, url) in urls.enumerated() {
        try TestClip.write(to: url, width: 320 + 32 * i, height: 180, frames: 6, fps: 24, audio: false)
    }
    try Data("notes".utf8).write(to: dir.appendingPathComponent("notes.txt"))
    try Exporter.write(linear: [Float](repeating: 0.2, count: 8 * 8 * 4), width: 8, height: 8,
                       to: dir.appendingPathComponent("still.tif"), format: .tiff16, colorSpace: "sRGB")
    let (a, b, c) = (urls[0], urls[1], urls[2])
    let libraryDir = scratch.appendingPathComponent("clips-library")
    func model() -> ClipModel {
        let library = ImageLibrary(directory: libraryDir)
        library.saveDelay = .milliseconds(50)
        let m = ClipModel(library: library)
        m.playback.player.isMuted = true
        return m
    }
    func written(_ m: ClipModel) async {
        try? await Task.sleep(for: .milliseconds(1000))
        m.library.save(wait: true)
        m.library.finishWriting()
    }
    func short(_ stock: String) -> String {
        let name = (try? PackLibrary.shared.pack(stock))?.name ?? stock
        return ["Kodak ", "Fujifilm ", "Ilford "].reduce(name) { $0.replacingOccurrences(of: $1, with: "") }
    }

    print("open a folder of clips")
    let m = model()
    m.openFolder(dir)
    check(await settled(m) && m.browser.items.map(\.name) == names && m.clip?.url == a && m.browser.selection == [a],
          "its clips by name, not its picture or notes, the first on screen: \(m.browser.items.map(\.name))")
    m.showNeighbour(1)
    check(await settled(m) && m.clip?.url == b && m.browser.current == b, "⌘↓ steps to the next")

    print("a look of its own, kept")
    m.selectFilm("kodak_vision3_500t")
    await written(m)
    check(m.browser.edited == [b] && m.library.record(for: b)?.look?.recipe.filmStock == "kodak_vision3_500t"
              && m.library.record(for: a) == nil,
          "a clip changed keeps its look, one only looked at none")

    print("flags")
    m.browser.selection = [c]
    m.flagSelection(.reject)
    m.browser.selection = [a]
    m.flagSelection(.pick)
    check(m.browser.flags[c] == .reject && m.browser.flags[a] == .pick, "X rejects and P picks")
    await written(m)

    print("kept between launches")
    let m2 = model()
    m2.openFolder(dir)
    check(await settled(m2) && m2.browser.edited == [b] && m2.browser.flags[c] == .reject && m2.browser.flags[a] == .pick,
          "a new session finds the look and the flags")
    m2.open(b)
    check(await settled(m2) && m2.look.recipe.filmStock == "kodak_vision3_500t", "B opens with its own look")

    print("copy and paste a look")
    // A different film from the one the session opened A with, the last one used.
    m2.selectFilm("kodak_vision3_250d")
    _ = await settled(m2)
    m2.copyLook()
    m2.browser.selection = [a, c]
    m2.pasteLookToSelected()
    await written(m2)
    check(m2.library.record(for: a)?.look?.recipe.filmStock == "kodak_vision3_250d" && m2.library.record(for: c)?.look == nil,
          "Paste Look to Selected: the pick gets it, the reject not: A \(m2.library.record(for: a)?.look?.recipe.filmStock ?? "none"), C \(m2.library.record(for: c)?.look?.recipe.filmStock ?? "none"); for batch \(m2.browser.selectedForBatch.map(\.lastPathComponent)), copied \(m2.copiedLook?.grade.recipe.filmStock ?? "nothing"), on screen \(m2.clip?.url.lastPathComponent ?? "-")")
    m2.open(a)
    check(await settled(m2) && m2.look.recipe.filmStock == "kodak_vision3_250d" && m2.history.undoName == "Paste Look",
          "the paste a step of A's own history: \(m2.look.recipe.filmStock), \(m2.history.undoName ?? "nothing to undo")")

    print("export the selection")
    let d = dir.appendingPathComponent("D.mov")
    try TestClip.write(to: d, width: 320, height: 180, frames: 6, fps: 24, audio: false)
    m2.openFolder(dir)
    m2.selectFilm("fujifilm_eterna_500t")
    _ = await settled(m2)
    let out = scratch.appendingPathComponent("clips-out")
    try fm.createDirectory(at: out, withIntermediateDirectories: true)
    m2.browser.selection = [b, c, d]
    m2.exportSelected(to: out)
    check(m2.browser.batch?.total == 2, "two to write: the reject left out")
    check(await waitUntil(m2, timeout: 120) { m2.browser.batch == nil }, "the batch finishes")
    let made = ((try? fm.contentsOfDirectory(atPath: out.path)) ?? []).sorted()
    let expected = ["B \(short("kodak_vision3_250d")).mov", "D \(short("fujifilm_eterna_500t")).mov"].sorted()
    check(made == expected, "each with its own look, the one never opened with the look on screen: \(made)")
    let tracks = (try? await AVURLAsset(url: out.appendingPathComponent(expected[0])).loadTracks(withMediaType: .video)) ?? []
    check(tracks.count == 1, "a clip that plays")
}

/// The sidebar's picture of a clip with a look of its own: its frame graded
/// as the preview grades it, turned as it plays, kept on disk.
@MainActor
func gradedThumbnails() async throws {
    let fm = FileManager.default
    let dir = scratch.appendingPathComponent("Graded")
    try fm.createDirectory(at: dir, withIntermediateDirectories: true)
    let e = dir.appendingPathComponent("E.mov"), r = dir.appendingPathComponent("R.mov")
    try TestClip.write(to: e, width: 320, height: 180, frames: 30, fps: 24, audio: false)
    try TestClip.write(to: r, width: 320, height: 180, frames: 30, fps: 24,
                       transform: CGAffineTransform(rotationAngle: .pi / 2), audio: false)

    /// RGBA, 8 bits, sRGB, row 0 at the top.
    func pixels(_ image: CGImage) -> [UInt8] {
        var p = [UInt8](repeating: 0, count: image.width * image.height * 4)
        p.withUnsafeMutableBytes { raw in
            let ctx = CGContext(data: raw.baseAddress, width: image.width, height: image.height, bitsPerComponent: 8,
                                bytesPerRow: image.width * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
            ctx.draw(image, in: CGRect(x: 0, y: 0, width: image.width, height: image.height))
        }
        return p
    }
    func meanDifference(_ a: [UInt8], _ b: [UInt8]) -> Double {
        guard a.count == b.count, !a.isEmpty else { return .infinity }
        var sum = 0.0
        for i in a.indices where i % 4 != 3 { sum += abs(Double(a[i]) - Double(b[i])) }
        return sum / Double(a.count / 4 * 3)
    }
    /// Which corner patch is the red marker's and which the blue's, as shown.
    func markers(_ image: CGImage) -> String {
        let p = pixels(image), w = image.width, h = image.height
        let cols = w > h ? Double(TestClip.columns) : Double(TestClip.rows)
        let rows = w > h ? Double(TestClip.rows) : Double(TestClip.columns)
        func mean(_ fx: Double, _ fy: Double) -> SIMD3<Double> {
            let cx = Int(fx * Double(w)), cy = Int(fy * Double(h))
            var s = SIMD3<Double>(repeating: 0), n = 0.0
            for y in max(0, cy - 2)...min(h - 1, cy + 2) {
                for x in max(0, cx - 2)...min(w - 1, cx + 2) {
                    let i = (y * w + x) * 4
                    s += SIMD3(Double(p[i]), Double(p[i + 1]), Double(p[i + 2]))
                    n += 1
                }
            }
            return s / n
        }
        let corners = [("top left", 0.5 / cols, 0.5 / rows), ("top right", 1 - 0.5 / cols, 0.5 / rows),
                       ("bottom left", 0.5 / cols, 1 - 0.5 / rows), ("bottom right", 1 - 0.5 / cols, 1 - 0.5 / rows)]
            .map { ($0.0, mean($0.1, $0.2)) }
        let red = corners.max { $0.1.x - $0.1.z < $1.1.x - $1.1.z }!.0
        let blue = corners.max { $0.1.z - $0.1.x < $1.1.z - $1.1.x }!.0
        return "red \(red), blue \(blue)"
    }
    func encode(_ v: Float) -> UInt8 {
        let c = Double(min(max(v, 0), 1))
        return UInt8(((c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1 / 2.4) - 0.055) * 255).rounded())
    }

    let m = ClipModel(library: scratchLibrary())
    m.playback.player.isMuted = true
    m.openFolder(dir)
    _ = await settled(m)
    let store = m.browser.thumbnails
    m.browser.requestThumbnail(e)
    m.browser.requestThumbnail(r)
    check(await waitUntil(m) { store.images[e] != nil && store.images[r] != nil }, "the log frames come first")
    guard let logE = store.images[e], let logR = store.images[r] else { return }

    print("a look of its own: the frame graded")
    m.selectFilm(m.look.recipe.filmStock == "kodak_vision3_250d" ? "kodak_vision3_500t" : "kodak_vision3_250d")
    _ = await settled(m)
    let started = Date()
    check(await waitUntil(m, timeout: 30) { store.images[e].map { $0 !== logE } ?? false },
          "E's thumbnail is graded once its look settles")
    print("      (in \(String(format: "%.2f", Date().timeIntervalSince(started))) s from the look settling)")
    guard let gradedE = store.images[e] else { return }
    check(gradedE.width == logE.width && gradedE.height == logE.height && meanDifference(pixels(gradedE), pixels(logE)) > 8,
          "graded, the size of the log frame: \(gradedE.width)x\(gradedE.height) vs \(logE.width)x\(logE.height), "
              + "differs by \(String(format: "%.1f", meanDifference(pixels(gradedE), pixels(logE)))) codes")
    check(markers(gradedE) == markers(logE), "upright, as the log frame: \(markers(gradedE)) vs \(markers(logE))")

    // The same frame graded with the look as the model holds it: the meter's
    // exposure, the format it reads the clip in, the effects it prints.
    let info = try await ClipInfo.load(e)
    let asset = AVURLAsset(url: e)
    let reader = try AVAssetReader(asset: asset)
    reader.timeRange = CMTimeRange(start: CMTime(seconds: min(1, info.durationSeconds / 10), preferredTimescale: 600),
                                   duration: CMTime(seconds: 1, preferredTimescale: 600))
    let output = AVAssetReaderTrackOutput(track: try await asset.loadTracks(withMediaType: .video)[0],
                                          outputSettings: info.decodeAttributes)
    reader.add(output)
    if reader.startReading(), let sample = output.copyNextSampleBuffer(), let frame = CMSampleBufferGetImageBuffer(sample) {
        let pipeline = try FramePipeline()
        pipeline.setLUT(try await LookBakeEngine().bake(m.effectiveRecipe, input: m.format.lutInput))
        let light = try pipeline.renderLinear(frame, width: gradedE.width, height: gradedE.height,
                                              effects: m.look.effectsAsPrinted, settings: m.readingSettings, mode: .graded)
        let reference = light.enumerated().map { $0.offset % 4 == 3 ? 255 : encode($0.element) }
        let off = meanDifference(pixels(gradedE), reference)
        check(off < 1.5, "as the preview grades the clip: \(String(format: "%.2f", off)) codes off on average")
    } else {
        check(false, "read E's frame for the reference")
    }
    reader.cancelReading()

    print("kept on disk")
    // Written in the background once it is made.
    let key = m.browser.ownGrade(e).map { ThumbnailStore.printKey(e, grade: $0) } ?? ""
    check(await waitUntil(m, timeout: 3) { ThumbnailStore.loadCached(key) != nil }, "in the thumbnail cache")
    let fresh = ThumbnailStore(kind: .clips)
    fresh.requestClip(e, grade: m.browser.ownGrade(e))
    check(fresh.images[e].map { meanDifference(pixels($0), pixels(gradedE)) < 3 } ?? false,
          "a new session shows it at once, from there: "
              + (fresh.images[e].map { String(format: "%.2f codes off", meanDifference(pixels($0), pixels(gradedE))) } ?? "none"))

    print("pasted onto a clip turned as it plays")
    // Brighter than the look R would carry from the one on screen, which is
    // the film without E's tone, so the paste gives R a look of its own.
    m.look.recipe.brightness = 0.15
    _ = await settled(m)
    m.copyLook()
    m.browser.selection = [r]
    m.pasteLookToSelected()
    check(await waitUntil(m, timeout: 30) { store.images[r].map { $0 !== logR } ?? false }, "R's thumbnail is graded")
    if let gradedR = store.images[r] {
        check(gradedR.width == logR.width && gradedR.height == logR.height && gradedR.height > gradedR.width,
              "standing, the size of its log frame: \(gradedR.width)x\(gradedR.height) vs \(logR.width)x\(logR.height)")
        check(markers(gradedR) == markers(logR), "turned as it plays: \(markers(gradedR)) vs \(markers(logR))")
    }

    print("with no look of its own")
    let plain = ThumbnailStore(kind: .clips)
    plain.requestClip(e, grade: nil)
    check(await waitUntil(m) { plain.images[e] != nil } && plain.images[e].map { meanDifference(pixels($0), pixels(logE)) < 1 } ?? false,
          "and with no look of its own, the log frame")
}

ThumbnailStore.cacheDirectoryOverride = scratch.appendingPathComponent("thumbnails")
try await run()
try await undoRedo()
try await clipFolders()
try await gradedThumbnails()
try? FileManager.default.removeItem(at: scratch)
print(failures == 0 ? "\nall app checks pass" : "\n\(failures) app check(s) failed")
exit(failures == 0 ? 0 : 1)
