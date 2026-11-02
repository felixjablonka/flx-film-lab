// flxmotion: one frame of a clip through the Mac app's own Motion pipeline
// (FramePipeline, LookBaker, AVFoundation's decode), as display light: the
// reference flxport-motion --linear is compared with.
//
//   flxmotion <movie> <out.f32> [--at SECONDS] [--width PX] [--film STOCK] [--exposure EV] [--bloom] [--glow]

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
guard args.count == 2 else { fail("usage: flxmotion <movie> <out.f32> [--at SECONDS] [--width PX] [--film STOCK] [--exposure EV] [--bloom] [--glow]") }
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

    // the frame showing at `at`
    let asset = AVURLAsset(url: url)
    guard let track = try await asset.loadTracks(withMediaType: .video).first else { fail("no video track") }
    let reader = try AVAssetReader(asset: asset)
    reader.timeRange = CMTimeRange(start: CMTime(seconds: at, preferredTimescale: 60000),
                                   duration: CMTime(seconds: 1, preferredTimescale: 600))
    let output = AVAssetReaderTrackOutput(track: track, outputSettings: info.decodeAttributes)
    output.alwaysCopiesSampleData = false
    reader.add(output)
    guard reader.startReading(), let sample = output.copyNextSampleBuffer(),
          let frame = CMSampleBufferGetImageBuffer(sample) else { fail("no frame at \(at) s") }
    let time = CMSampleBufferGetPresentationTimeStamp(sample).seconds

    let baker = try LookBaker()
    let lut = try baker.bake(recipe: look.recipe, size: 65, input: format.lutInput, output: .bt1886)
    let pipeline = try FramePipeline()
    pipeline.setLUT(lut)
    let w = width, h = Int((Double(width) * Double(CVPixelBufferGetHeight(frame)) / Double(CVPixelBufferGetWidth(frame))).rounded())
    let pixels = try pipeline.renderLinear(frame, width: w, height: h, effects: look.effectsAsPrinted,
                                           settings: settings, mode: .graded)
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
