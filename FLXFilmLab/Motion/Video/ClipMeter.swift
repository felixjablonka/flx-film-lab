import Foundation
import AVFoundation
import CoreVideo

/// Meters a clip for the film's highlight latitude.
///
/// Log footage keeps six or more stops above an 18% grey; a slide keeps about
/// one and a half before it runs to clear film. Printed at the exposure the camera
/// recorded, every bright wall a slide cannot hold burns out, and the frame
/// reads flat and washed. A slide shooter meters for the highlights that
/// matter and lets a lamp or a window burn out, and so does this: in each of a
/// spread of the clip's frames it takes the level only the brightest 2% of the
/// picture exceeds, takes the middle of those across the clip so one shot of a
/// window does not darken all the others, and places that inside the stock's
/// own headroom the way the Stills mode's meter does (`RenderSetupBuilder
/// .autoExposureEV`: same headroom, half-stop margin and partial pull).
///
/// It only ever pulls exposure down. The Stills mode's meter also pulls the median
/// toward grey, which suits a RAW whose exposure is arbitrary; a log clip was
/// exposed on purpose, so that half is left out. Negatives have stops of
/// headroom to spare, so for them the reading is zero and the clip prints as
/// shot. One reading covers the whole clip, so exposure never moves within it,
/// and it never pulls more than two stops.
enum ClipMeter {
    /// The share of each frame allowed to burn out.
    static let frameHighlight: Float = 0.98

    /// The clip's highlight, as scene luminance (linear, 0.18 = grey card):
    /// the median, across `frames` frames spread over the clip, of the level
    /// 98% of each frame stays below.
    static func highlight(of info: ClipInfo, settings: InputSettings, frames: Int = 9) async throws -> Float? {
        let asset = AVURLAsset(url: info.url)
        guard let track = try await asset.loadTracks(withMediaType: .video).first else { return nil }
        let duration = info.durationSeconds
        var perFrame: [Float] = []
        for k in 0..<max(frames, 1) {
            try Task.checkCancellation()
            let t = duration > 0 ? duration * (Double(k) + 0.5) / Double(frames) : 0
            let reader = try AVAssetReader(asset: asset)
            reader.timeRange = CMTimeRange(start: CMTime(seconds: t, preferredTimescale: 600),
                                           duration: CMTime(seconds: 1, preferredTimescale: 600))
            let output = AVAssetReaderTrackOutput(track: track, outputSettings: info.decodeAttributes)
            output.alwaysCopiesSampleData = false
            reader.add(output)
            guard reader.startReading(), let sample = output.copyNextSampleBuffer(),
                  let frame = CMSampleBufferGetImageBuffer(sample) else {
                reader.cancelReading()
                continue
            }
            var luminances = sampleLuminances(frame, settings: settings)
            reader.cancelReading()
            guard luminances.count > 32 else { continue }
            luminances.sort()
            perFrame.append(luminances[Int(Float(luminances.count - 1) * frameHighlight)])
        }
        guard !perFrame.isEmpty else { return nil }
        perFrame.sort()
        return perFrame[perFrame.count / 2]
    }

    /// Scene luminance on a sparse grid, about 300 samples across the frame.
    static func sampleLuminances(_ frame: CVPixelBuffer, settings: InputSettings) -> [Float] {
        guard let reading = try? FramePipeline.reading(of: frame, settings: settings) else { return [] }
        return FramePipeline.withCodes(of: frame, reading: reading) { sampler -> [Float] in
            let step = max(sampler.width / 300, 1)
            var out: [Float] = []
            out.reserveCapacity((sampler.width / step + 1) * (sampler.height / step + 1))
            for y in stride(from: step / 2, to: sampler.height, by: step) {
                for x in stride(from: step / 2, to: sampler.width, by: step) {
                    guard let c = sampler.codes(x, y) else { continue }
                    // BT.2020 light, whatever the format's own primaries.
                    let light = reading.format.scene(codes: c, fullRange: reading.fullRange)
                    let luminance = 0.2627 * light.x + 0.6780 * light.y + 0.0593 * light.z
                    if luminance.isFinite && luminance > 0 { out.append(Float(luminance)) }
                }
            }
            return out
        } ?? []
    }

    /// The exposure that keeps `highlight` inside `film`'s headroom, never
    /// above zero: what Auto exposure sets the slider to.
    static func exposure(highlight: Float, film: ProfilePack) -> Float {
        guard highlight > 0 else { return 0 }
        let headroom = RenderSetupBuilder.highlightHeadroomStops(film: film) - 0.5
        let fromHighlight = headroom - log2(highlight / RenderSetupBuilder.midgrayValue)
        guard fromHighlight.isFinite else { return 0 }
        return max(min(fromHighlight, 0) * 0.7, -2)
    }
}
