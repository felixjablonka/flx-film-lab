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
    let m = ClipModel()
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

    print("export a LUT")
    let cube = dir.appendingPathComponent("look.cube")
    m.exportLUT(to: cube)
    check(await waitUntil(m) { FileManager.default.fileExists(atPath: cube.path) }, "writes the .cube")
    let text = (try? String(contentsOf: cube, encoding: .utf8)) ?? ""
    check(text.contains("LUT_3D_SIZE 65") && text.contains("video levels") && text.contains("BT.1886"),
          "65 points, N-Log video levels in, BT.1886 out")

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
    check(ClipModel().look.recipe.autoExposure, "a look saved by the previous version gets Auto exposure back")

    m.look.effects.halation.enabled = true
    _ = await settled(m)
    let again = ClipModel()
    check(await settled(again) && again.look.recipe.filmStock == m.look.recipe.filmStock
          && again.look.effects.halation.enabled && again.look.export.codec == .hevc10,
          "the next launch keeps the look")
    check(again.look.recipe.exposureEV == 0, "but not one clip's exposure")
    UserDefaults.standard.removeObject(forKey: ClipModel.lookDefaultsKey)
}

try await run()
print(failures == 0 ? "\nall app checks pass" : "\n\(failures) app check(s) failed")
exit(failures == 0 ? 0 : 1)
