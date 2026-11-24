// flxmotion: one frame of a clip through the Mac app's own Motion pipeline
// (FramePipeline, LookBaker, AVFoundation's decode), as display light: the
// reference flxport-motion --linear is compared with.
//
//   flxmotion <movie> <out.f32> [--at SECONDS] [--width PX] [--film STOCK] [--exposure EV] [--bloom] [--glow]
//             [--grain 16|35|70] [--grain-amount A] [--grain-size S]

import AVFoundation
import Foundation
@testable import FLXCore

func fail(_ message: String) -> Never {
    FileHandle.standardError.write((message + "\n").data(using: .utf8)!)
    exit(1)
}

var args = Array(CommandLine.arguments.dropFirst())
func option(_ name: String) -> String? {
    guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
    let v = args[i + 1]
    args.removeSubrange(i...(i + 1))
    return v
}
func flag(_ name: String) -> Bool {
    guard let i = args.firstIndex(of: name) else { return false }
    args.remove(at: i)
    return true
}
let at = Double(option("--at") ?? "1") ?? 1
let width = Int(option("--width") ?? "1920") ?? 1920
let film = option("--film") ?? "kodak_portra_800"
let exposure = option("--exposure").flatMap(Double.init) ?? 0
let bloom = flag("--bloom"), glow = flag("--glow")
let grainGauge = option("--grain")
let grainAmount = option("--grain-amount").flatMap(Double.init) ?? 1
let grainSize = option("--grain-size").flatMap(Double.init) ?? 1
guard args.count == 2 else { fail("usage: flxmotion <movie> <out.f32> [--at SECONDS] [--width PX] [--film STOCK] [--exposure EV] [--bloom] [--glow] [--grain 16|35|70] [--grain-amount A] [--grain-size S]") }
let url = URL(fileURLWithPath: args[0])

do {
    let info = try await ClipInfo.load(url)
    let format = info.detectedFormat
    var look = MotionLook.initial
    look.recipe = look.recipe.loadingFilm(film)
    if let target = (try? PackLibrary.shared.pack(film))?.targetPrint { look.recipe.printStock = target }
    look.recipe.exposureEV = exposure
    look.effects.bloom.enabled = bloom
    look.effects.glow.enabled = glow
    var settings = look.input
    settings.format = format

    // the frame nearest `at`, as the port's ClipReader.frameAt takes it. Read
    // from a second before: a reader's first sample in a time range is stamped
    // with the range's start, not its own time, which would number the
    // frame's grain wrongly.
    let asset = AVURLAsset(url: url)
    guard let track = try await asset.loadTracks(withMediaType: .video).first else { fail("no video track") }
    let reader = try AVAssetReader(asset: asset)
    let from = max(at - 1, 0)
    reader.timeRange = CMTimeRange(start: CMTime(seconds: from, preferredTimescale: 60000),
                                   duration: CMTime(seconds: at - from + 1, preferredTimescale: 60000))
    let output = AVAssetReaderTrackOutput(track: track, outputSettings: info.decodeAttributes)
    output.alwaysCopiesSampleData = false
    reader.add(output)
    guard reader.startReading() else { fail("cannot read the clip") }
    var nearest: (sample: CMSampleBuffer, time: Double)?
    var first = true
    while let sample = output.copyNextSampleBuffer() {
        defer { first = false }
        guard CMSampleBufferGetImageBuffer(sample) != nil else { continue }
        let t = CMSampleBufferGetPresentationTimeStamp(sample).seconds
        if first && from > 0 { continue }
        if let n = nearest, abs(n.time - at) <= abs(t - at) { break }
        nearest = (sample, t)
    }
    guard let chosen = nearest, let frame = CMSampleBufferGetImageBuffer(chosen.sample) else { fail("no frame at \(at) s") }
    let time = chosen.time

    let baker = try LookBaker()
    let lut = try baker.bake(recipe: look.recipe, size: 65, input: format.lutInput, output: .bt1886)
    let pipeline = try FramePipeline()
    pipeline.setLUT(lut)
    let w = width, h = Int((Double(width) * Double(CVPixelBufferGetHeight(frame)) / Double(CVPixelBufferGetWidth(frame))).rounded())
    // the look's grain, the frame's own grains, numbered by its time as the app's are
    var frameGrain: FrameGrain?
    if let gauge = grainGauge {
        var grain = MotionGrain()
        grain.enabled = true
        grain.gauge = gauge == "16" ? .sixteen : gauge == "70" ? .seventy : .thirtyFive
        grain.amount = grainAmount
        grain.size = grainSize
        guard let table = try baker.printGrain(recipe: look.recipe) else { fail("the film has no grain data") }
        frameGrain = ClipGrain(table: table, settings: grain, clip: url)
            .frame(FrameGrain.frameNumber(at: time, frameRate: info.frameRate))
    }
    let pixels = try pipeline.renderLinear(frame, width: w, height: h, effects: look.effectsAsPrinted,
                                           grain: frameGrain, settings: settings, mode: .graded)
    reader.cancelReading()

    // the dump format tools/compare reads: "FLXD", version 1, width, height, channels, type 0, two zeros
    var data = Data("FLXD".utf8)
    for v in [UInt32(1), UInt32(w), UInt32(h), UInt32(4), UInt32(0), UInt32(0), UInt32(0)] {
        withUnsafeBytes(of: v.littleEndian) { data.append(contentsOf: $0) }
    }
    pixels.withUnsafeBytes { data.append(contentsOf: $0) }
    try data.write(to: URL(fileURLWithPath: args[1]))
    print(String(format: "%@: %@ read as %@, frame %.3f s at %dx%d -> %@", url.lastPathComponent, info.summary,
                 format.rawValue, time, w, h, args[1]))
} catch {
    fail("error: \(error)")
}
