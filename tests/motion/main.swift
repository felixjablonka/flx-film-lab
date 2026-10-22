import Foundation
import AVFoundation
import CoreVideo
import CoreImage
import Metal
import simd
@testable import FLXCore

// The Motion mode's video path, end to end, on synthetic N-Log footage: that the
// decode reads back the exact code values written, that a graded frame is the
// engine's print of those values, that the exported files decode — the way
// QuickTime decodes them — to what the preview shows, and that audio, frame
// order, rotation and timing survive.

var failures = 0
func check(_ ok: Bool, _ message: String) {
    print("   \(ok ? "ok  " : "FAIL") \(message)")
    if !ok { failures += 1 }
}

// MARK: - Helpers

/// Mean RGB over a rectangle of an RGBA float image.
func mean(_ px: [Float], width: Int, _ r: (x0: Int, y0: Int, x1: Int, y1: Int)) -> SIMD3<Double> {
    var sum = SIMD3<Double>(repeating: 0)
    var n = 0
    for y in r.y0..<r.y1 {
        for x in r.x0..<r.x1 {
            let o = (y * width + x) * 4
            sum += SIMD3(Double(px[o]), Double(px[o + 1]), Double(px[o + 2]))
            n += 1
        }
    }
    return sum / Double(max(n, 1))
}

/// Mean R'G'B' code values over a rectangle of a decoded frame, read on the
/// CPU off its Y'CbCr planes (`FramePipeline.withCodes`): what the file holds,
/// whatever the encoder lost.
func meanCodes(_ buffer: CVPixelBuffer, reading: FramePipeline.Reading,
               _ r: (x0: Int, y0: Int, x1: Int, y1: Int)) -> SIMD3<Double> {
    FramePipeline.withCodes(of: buffer, reading: reading) { sampler -> SIMD3<Double> in
        var sum = SIMD3<Double>(repeating: 0)
        var n = 0
        for y in r.y0..<r.y1 {
            for x in r.x0..<r.x1 {
                if let c = sampler.codes(x, y) { sum += c; n += 1 }
            }
        }
        return sum / Double(max(n, 1))
    } ?? SIMD3(repeating: .nan)
}

/// A copy of a 10-bit frame with each sample's top bits repeated in the six
/// low bits of its 16-bit word, as some decoders write them; the format only
/// promises the ten high bits.
func fillingLowBits(_ buffer: CVPixelBuffer) -> CVPixelBuffer {
    var copy: CVPixelBuffer?
    CVPixelBufferCreate(nil, CVPixelBufferGetWidth(buffer), CVPixelBufferGetHeight(buffer),
                        CVPixelBufferGetPixelFormatType(buffer),
                        [kCVPixelBufferMetalCompatibilityKey: true,
                         kCVPixelBufferIOSurfacePropertiesKey: [String: Any]()] as CFDictionary, &copy)
    CVBufferPropagateAttachments(buffer, copy!)
    CVPixelBufferLockBaseAddress(buffer, .readOnly)
    CVPixelBufferLockBaseAddress(copy!, [])
    for plane in 0..<CVPixelBufferGetPlaneCount(buffer) {
        let words = CVPixelBufferGetWidthOfPlane(buffer, plane) * (plane == 0 ? 1 : 2)
        let from = CVPixelBufferGetBaseAddressOfPlane(buffer, plane)!, to = CVPixelBufferGetBaseAddressOfPlane(copy!, plane)!
        let fromRow = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane), toRow = CVPixelBufferGetBytesPerRowOfPlane(copy!, plane)
        for y in 0..<CVPixelBufferGetHeightOfPlane(buffer, plane) {
            let a = from.advanced(by: y * fromRow).assumingMemoryBound(to: UInt16.self)
            let b = to.advanced(by: y * toRow).assumingMemoryBound(to: UInt16.self)
            for x in 0..<words {
                let v = a[x] >> 6
                b[x] = v << 6 | v >> 4
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(copy!, [])
    CVPixelBufferUnlockBaseAddress(buffer, .readOnly)
    return copy!
}

/// A 64RGBAHalf pixel buffer as RGBA floats.
func floats(_ buffer: CVPixelBuffer) -> [Float] {
    CVPixelBufferLockBaseAddress(buffer, .readOnly)
    defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
    let w = CVPixelBufferGetWidth(buffer), h = CVPixelBufferGetHeight(buffer)
    let base = CVPixelBufferGetBaseAddress(buffer)!, row = CVPixelBufferGetBytesPerRow(buffer)
    var out = [Float](repeating: 0, count: w * h * 4)
    for y in 0..<h {
        let p = base.advanced(by: y * row).assumingMemoryBound(to: Float16.self)
        for x in 0..<(w * 4) { out[y * w * 4 + x] = Float(p[x]) }
    }
    return out
}

/// Every frame of a file's video track, decoded with `settings`.
func frames(_ url: URL, _ settings: [String: Any], limit: Int = .max) throws -> [CVPixelBuffer] {
    let asset = AVURLAsset(url: url)
    let track = asset.tracks(withMediaType: .video)[0]
    let reader = try AVAssetReader(asset: asset)
    let output = AVAssetReaderTrackOutput(track: track, outputSettings: settings)
    reader.add(output)
    reader.startReading()
    var out: [CVPixelBuffer] = []
    while out.count < limit, let sample = output.copyNextSampleBuffer() {
        if let b = CMSampleBufferGetImageBuffer(sample) { out.append(b) }
    }
    return out
}

/// What QuickTime sees: VideoToolbox converting the file to linear light by
/// its own tags.
let quickTimeLinear: [String: Any] = [
    kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_64RGBAHalf,
    AVVideoColorPropertiesKey: [AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
                                AVVideoTransferFunctionKey: AVVideoTransferFunction_Linear,
                                AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2],
]
/// The encoded values as written, only converted from Y'CbCr to RGB.
let encodedRGB: [String: Any] = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_64RGBAHalf]

func formatInfo(_ url: URL) -> (codec: FourCharCode, tags: [String], bits: Int?) {
    let track = AVURLAsset(url: url).tracks(withMediaType: .video)[0]
    let fd = track.formatDescriptions.first as! CMFormatDescription
    let ext = CMFormatDescriptionGetExtensions(fd) as? [String: Any] ?? [:]
    let tags = [kCMFormatDescriptionExtension_ColorPrimaries, kCMFormatDescriptionExtension_TransferFunction,
                kCMFormatDescriptionExtension_YCbCrMatrix].map { ext[$0 as String] as? String ?? "-" }
    var bits = ext["BitsPerComponent"] as? Int
    if let atoms = ext[kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms as String] as? [String: Any],
       let hvcC = atoms["hvcC"] as? Data, hvcC.count > 18 {
        bits = Int([UInt8](hvcC)[17] & 0x7) + 8
    }
    return (CMFormatDescriptionGetMediaSubType(fd), tags, bits)
}

let rec709Tags = ["ITU_R_709_2", "ITU_R_709_2", "ITU_R_709_2"]

func luminance(_ c: SIMD3<Double>) -> Double { 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z }

/// The highest value a progress callback reported, from whichever queue.
final class Highest: @unchecked Sendable {
    private let lock = NSLock()
    private var value = 0.0
    func record(_ v: Double) { lock.withLock { value = max(value, v) } }
    var seen: Double { lock.withLock { value } }
}

/// A frame that is grey except for its top-left quadrant, which is red.
func quadrantFrame(width: Int, height: Int) -> CVPixelBuffer {
    let frame = TestClip.makeFrame(width: width, height: height)
    let grey = TestClip.ycbcr(TestClip.codes(TestClip.greyStops(0)))
    let red = TestClip.ycbcr(TestClip.codes(SIMD3(0.5, 0.02, 0.02)))
    CVPixelBufferLockBaseAddress(frame, [])
    let yBase = CVPixelBufferGetBaseAddressOfPlane(frame, 0)!, yRow = CVPixelBufferGetBytesPerRowOfPlane(frame, 0)
    let cBase = CVPixelBufferGetBaseAddressOfPlane(frame, 1)!, cRow = CVPixelBufferGetBytesPerRowOfPlane(frame, 1)
    for y in 0..<height {
        let row = yBase.advanced(by: y * yRow).assumingMemoryBound(to: UInt16.self)
        for x in 0..<width { row[x] = UInt16(((x < width / 2 && y < height / 2) ? red : grey).x.rounded()) << 6 }
    }
    for y in 0..<(height / 2) {
        let row = cBase.advanced(by: y * cRow).assumingMemoryBound(to: UInt16.self)
        for x in 0..<(width / 2) {
            let v = (x < width / 4 && y < height / 4) ? red : grey
            row[2 * x] = UInt16(v.y.rounded()) << 6
            row[2 * x + 1] = UInt16(v.z.rounded()) << 6
        }
    }
    CVPixelBufferUnlockBaseAddress(frame, [])
    return frame
}

func timeIt(_ runs: Int, _ body: () throws -> Void) rethrows -> Double {
    try body()   // warm up: shader compilation, texture allocation
    let t = CFAbsoluteTimeGetCurrent()
    for _ in 0..<runs { try body() }
    return (CFAbsoluteTimeGetCurrent() - t) * 1000 / Double(runs)
}

// MARK: - Run

func run() async throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("flx-motiontest-\(getpid())")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }

    let width = 1280, height = 720, frameCount = 36
    let fps: Int32 = 24
    let clipURL = dir.appendingPathComponent("nlog.mov")
    try TestClip.write(to: clipURL, width: width, height: height, frames: frameCount, fps: fps)
    let layout = TestClip.Layout(width: width, height: height)
    let indices = Array(TestClip.patches.indices)

    print("the clip, as the Motion mode reads it")
    let info = try await ClipInfo.load(clipURL)
    print("      \(info.summary)")
    check(info.size == CGSize(width: width, height: height) && Int(info.frameRate.rounded()) == Int(fps),
          "size and frame rate")
    check(info.bitDepth == 10 && info.chroma == .c420 && info.fullRange == false,
          "10-bit 4:2:0 video range, from the HEVC configuration")
    check(info.matrixTag == (kCVImageBufferYCbCrMatrix_ITU_R_2020 as String) && info.hasAudio,
          "BT.2020 matrix tag, and an audio track")
    check(info.decodePixelFormat == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
          "decoded as recorded: 10-bit 4:2:0 video range")

    let look = MotionLook.initial
    let baker = try LookBaker()
    let lut = try baker.bake(recipe: look.recipe)
    let pipeline = try FramePipeline()
    pipeline.setLUT(lut)
    var noEffects = GlowLook()
    noEffects.bloom.enabled = false

    let source = try frames(clipURL, info.decodeAttributes)
    check(source.count == frameCount, "all \(frameCount) frames decode")
    let frame0 = source[0]

    print("decode: Y'CbCr back to N-Log code values")
    let reading = try FramePipeline.reading(of: frame0, settings: InputSettings())
    check(reading.bitDepth == 10 && !reading.fullRange && reading.matrixName == "BT.2020",
          "read as 10-bit, video range, BT.2020 (\(reading.matrixName))")
    let codesImage = try pipeline.renderLinear(frame0, width: width, height: height, effects: noEffects,
                                               settings: InputSettings(), mode: .codes)
    // Two questions, kept apart: does the grade read the planes exactly, and
    // does the file still hold what was written? The second is the encoder's.
    // A Mac's media engine keeps every patch to rounding; the software
    // encoders of a CI runner, which has none, lose more. So the checks from
    // here on hold the grade to what the file holds.
    var worstRead = 0.0, worstCode = 0.0
    var decoded: [SIMD3<Double>] = [], recorded: [SIMD3<Double>] = []
    for i in indices {
        let got = mean(codesImage, width: width, layout.core(i)) * 1023
        let held = meanCodes(frame0, reading: reading, layout.core(i))
        decoded.append(got)
        recorded.append(held)
        worstRead = max(worstRead, simd_reduce_max(simd_abs(got - held)))
        worstCode = max(worstCode, simd_reduce_max(simd_abs(held - TestClip.patchCodes(i, frame: 0))))
    }
    // The CPU takes the nearest chroma sample and the kernel interpolates,
    // which moves a patch's mean only at its rim.
    check(worstRead < 0.25, String(format: "the grade reads the planes as the CPU does (within %.3f codes)", worstRead))
    // A decoder may fill a sample's six low bits with copies of its top bits,
    // as the software decoder on GitHub's runners does: the grade has to read
    // the ten high bits alone, as the CPU does.
    let filled = fillingLowBits(frame0)
    let readsAlike = try FramePipeline.reading(of: filled, settings: InputSettings()) == reading
    let filledCodes = try pipeline.renderLinear(filled, width: width, height: height, effects: noEffects,
                                                settings: InputSettings(), mode: .codes)
    var worstFilled = 0.0
    for i in indices {
        let got = mean(filledCodes, width: width, layout.core(i)) * 1023
        worstFilled = max(worstFilled, simd_reduce_max(simd_abs(got - recorded[i])))
    }
    check(readsAlike && worstFilled < 0.25,
          String(format: "and ignores what a decoder leaves in the low six bits (within %.3f codes)", worstFilled))
    // Rounding Y', Cb and Cr to whole 10-bit codes alone moves B' by up to
    // 0.5 + 1.88 x 0.5 x 876/896 = 1.42 codes, R' by up to 1.22. A lossier
    // encoder adds its own; a wrong matrix or range would be tens of codes.
    check(worstCode < 4, String(format: "every patch reads back within %.2f codes of what was written (Y'CbCr rounding alone allows 1.42)", worstCode))

    // The probe takes the nearest chroma sample where the kernel
    // interpolates, so at one pixel they differ by a share of the step
    // between two chroma rows.
    var worstProbe = 0.0
    for i in indices {
        let r = layout.rect(i)
        let (x, y) = ((r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2)
        if let got = FramePipeline.codes(at: x, y, of: frame0, reading: reading) {
            let o = (y * width + x) * 4
            let graded = SIMD3(Double(codesImage[o]), Double(codesImage[o + 1]), Double(codesImage[o + 2])) * 1023
            worstProbe = max(worstProbe, simd_reduce_max(simd_abs(got - graded)))
        } else {
            worstProbe = .infinity
        }
    }
    check(worstProbe < 0.5, String(format: "the pointer probe reads the code values the grade reads (within %.2f)", worstProbe))

    print("grade: each patch is the engine's print of it")
    let graded = try pipeline.renderLinear(frame0, width: width, height: height, effects: noEffects,
                                           settings: InputSettings(), mode: .graded)
    let truth = try baker.direct(indices.map { SIMD3<Float>(recorded[$0] / 1023) },
                                 recipe: look.recipe, input: .nlogCode, output: .linear)
    var engineErrors: [Double] = [], cpuErrors: [Double] = []
    var gradedPatches: [SIMD3<Double>] = []
    for i in indices {
        let got = mean(graded, width: width, layout.core(i))
        gradedPatches.append(got)
        let t = truth[i]
        engineErrors.append(deltaE2000(lab(linear: got), lab(linear: SIMD3(Double(t.x), Double(t.y), Double(t.z)))))
        let cpu = lut.sample(SIMD3<Float>(decoded[i] / 1023))
        cpuErrors.append(deltaE2000(lab(linear: got), lab(cpu, .bt1886)))
    }
    let engineStats = Errors(values: engineErrors), cpuStats = Errors(values: cpuErrors)
    print("      against the engine per pixel: \(engineStats.summary)")
    check(cpuStats.largest < 0.05, String(format: "the GPU lookup is the CPU's (max dE %.3f)", cpuStats.largest))
    check(engineStats.largest < 1.0, "and within dE 1 of rendering every pixel through the engine")
    let greyLab = lab(linear: gradedPatches[TestClip.grey])
    print(String(format: "      grey card: L* %.1f a* %+.2f b* %+.2f", greyLab.x, greyLab.y, greyLab.z))
    check(abs(greyLab.y) < 2 && abs(greyLab.z) < 2, "the grey card prints neutral")
    let red = gradedPatches[TestClip.redMarker], blue = gradedPatches[TestClip.blueMarker]
    check(red.x > 3 * red.z && blue.z > 3 * blue.x, "top-left is red and bottom-right blue: not flipped")

    print("levels override")
    var forcedFull = InputSettings()
    forcedFull.levels = .full
    let fullCodes = try pipeline.renderLinear(frame0, width: width, height: height, effects: noEffects,
                                              settings: forcedFull, mode: .codes)
    let greyAsFull = mean(fullCodes, width: width, layout.core(TestClip.grey)).x * 1023
    let expected = (TestClip.ycbcr(TestClip.patchCodes(TestClip.grey, frame: 0)).x)   // Y' as a full-range code
    check(abs(greyAsFull - expected) < 1.5,
          String(format: "forcing full range reads grey's Y' %.1f as code %.1f", expected, greyAsFull))

    print("clip meter")
    guard let highlight = try await ClipMeter.highlight(of: info, settings: InputSettings()) else {
        check(false, "meters the clip")
        return
    }
    // The specular patch (5.8 stops over grey) is under 2% of the chart, so it
    // may burn out; the +5-stop grey next brightest is what gets protected.
    check(abs(log2(highlight / 0.18) - 5.0) < 0.1,
          String(format: "lets the small specular burn and protects the next brightest tone (%.2f stops over grey)",
                 log2(highlight / 0.18)))
    var readings: [String: Float] = [:]
    for stock in ["kodak_portra_800", "kodak_vision3_250d", "kodak_ektachrome_100", "fujifilm_velvia_50"] {
        readings[stock] = ClipMeter.exposure(highlight: highlight, film: try PackLibrary.shared.pack(stock))
    }
    print("      " + readings.sorted { $0.key < $1.key }.map { String(format: "%@ %+.2f", $0.key, $0.value) }.joined(separator: ", "))
    check(readings["kodak_portra_800"] == 0 && readings["kodak_vision3_250d"] == 0,
          "negatives hold it with room to spare and print as shot")
    check(readings["kodak_ektachrome_100"] == -2 && readings["fujifilm_velvia_50"] == -2,
          "slides are pulled down, by two stops at most")
    let modest = 0.18 * pow(2, 2.5) as Float
    let e100 = ClipMeter.exposure(highlight: modest, film: try PackLibrary.shared.pack("kodak_ektachrome_100"))
    let velvia = ClipMeter.exposure(highlight: modest, film: try PackLibrary.shared.pack("fujifilm_velvia_50"))
    check(e100 < 0 && velvia < e100,
          String(format: "highlights at +2.5 stops: Ektachrome %+.2f EV, Velvia (shorter latitude) further, %+.2f", e100, velvia))
    check(ClipMeter.exposure(highlight: 0.18 * 2, film: try PackLibrary.shared.pack("kodak_ektachrome_100")) == 0,
          "a clip whose highlights fit is left alone, even on a slide")

    print("export: ProRes 422 HQ for Apple displays")
    let prores = dir.appendingPathComponent("prores.mov")
    let progressSeen = Highest()
    try await ClipExporter().export(source: clipURL, to: prores, look: look, lut: lut,
                                    options: .init(codec: .proRes422HQ, display: .appleDisplay, timeRange: nil),
                                    progress: { progressSeen.record($0) })
    let proresFormat = formatInfo(prores)
    check(proresFormat.codec == kCMVideoCodecType_AppleProRes422HQ && proresFormat.tags == rec709Tags,
          "ProRes 422 HQ, tagged Rec.709 (\(proresFormat.tags.joined(separator: "/")))")
    check(progressSeen.seen == 1, "progress reaches 100%")
    let seenByQuickTime = try frames(prores, quickTimeLinear)
    check(seenByQuickTime.count == frameCount, "all \(seenByQuickTime.count) frames written")
    let qt0 = floats(seenByQuickTime[0])
    var qtErrors: [Double] = []
    for i in indices {
        qtErrors.append(deltaE2000(lab(linear: mean(qt0, width: width, layout.core(i))), lab(linear: gradedPatches[i])))
    }
    let qtStats = Errors(values: qtErrors)
    print("      decoded the way QuickTime decodes it, against the preview: \(qtStats.summary)")
    // The bound is the codec's: on a Mac's media engine the export stays
    // within 0.3 of the preview, through a CI runner's software path within
    // 0.9. A wrong transfer curve is several times that in the shadows.
    check(qtStats.largest < 1.0, "QuickTime shows what the preview shows")

    // Frame order: the marker patch steps by 20 codes a frame, so each written
    // frame's marker has to be nearer its own frame's than either neighbour's.
    // The frames checked are ones whose step prints clearly, dE 2.6 to 4.8;
    // from frame 22 on, the print's shoulder squeezes a step under dE 1, about
    // what a software codec loses.
    func previewMarker(_ k: Int) throws -> SIMD3<Double> {
        let frame = try pipeline.renderLinear(source[k], width: width, height: height, effects: noEffects,
                                              settings: InputSettings(), mode: .graded)
        return mean(frame, width: width, layout.core(TestClip.frameMarker))
    }
    func marker(_ got: SIMD3<Double>, isFrame k: Int) throws -> (error: Double, neighbour: Double) {
        let error = deltaE2000(lab(linear: got), lab(linear: try previewMarker(k)))
        let neighbour = try [k - 1, k + 1].filter { source.indices.contains($0) }
            .map { deltaE2000(lab(linear: got), lab(linear: try previewMarker($0))) }.min()!
        return (error, neighbour)
    }
    var inOrder = true
    var orderErrors: [Double] = [], nearest = Double.infinity
    for k in [3, 9, 12, frameCount - 1] {
        let m = try marker(mean(floats(seenByQuickTime[k]), width: width, layout.core(TestClip.frameMarker)),
                           isFrame: k)
        inOrder = inOrder && m.error < m.neighbour
        orderErrors.append(m.error)
        nearest = min(nearest, m.neighbour)
    }
    check(inOrder, String(format: "frames stay in order (marker dE %.2f at worst, a neighbour's %.2f at nearest)",
                          orderErrors.max()!, nearest))

    let sourceAsset = AVURLAsset(url: clipURL), exported = AVURLAsset(url: prores)
    let sourceAudio = try await sourceAsset.loadTracks(withMediaType: .audio)
    let exportedAudio = try await exported.loadTracks(withMediaType: .audio)
    check(sourceAudio.count == 1 && exportedAudio.count == 1, "the audio track comes along")
    if let a = sourceAudio.first, let b = exportedAudio.first {
        let (ra, rb) = (try await a.load(.timeRange), try await b.load(.timeRange))
        let fa = try await a.load(.formatDescriptions).first, fb = try await b.load(.formatDescriptions).first
        let asbdA = fa.flatMap { CMAudioFormatDescriptionGetStreamBasicDescription($0)?.pointee }
        let asbdB = fb.flatMap { CMAudioFormatDescriptionGetStreamBasicDescription($0)?.pointee }
        check(abs(ra.duration.seconds - rb.duration.seconds) < 0.001 && asbdA?.mFormatID == asbdB?.mFormatID
              && asbdA?.mSampleRate == asbdB?.mSampleRate,
              String(format: "untouched: %.3f s of %.0f Hz audio both sides", rb.duration.seconds, asbdB?.mSampleRate ?? 0))
    }
    let (vs, ve) = (try await sourceAsset.loadTracks(withMediaType: .video)[0].load(.timeRange),
                    try await exported.loadTracks(withMediaType: .video)[0].load(.timeRange))
    check(vs.start == ve.start && abs(vs.duration.seconds - ve.duration.seconds) < 1e-6,
          String(format: "same timing: %.3f s from %.3f s", ve.duration.seconds, ve.start.seconds))

    print("export: HEVC 10-bit for Rec.709 (BT.1886) displays")
    let hevc = dir.appendingPathComponent("hevc.mov")
    try await ClipExporter().export(source: clipURL, to: hevc, look: look, lut: lut,
                                    options: .init(codec: .hevc10, display: .bt1886, timeRange: nil),
                                    progress: { _ in })
    let hevcFormat = formatInfo(hevc)
    check(hevcFormat.codec == kCMVideoCodecType_HEVC && hevcFormat.bits == 10 && hevcFormat.tags == rec709Tags,
          "HEVC Main10, tagged Rec.709 (\(hevcFormat.bits ?? 0)-bit)")
    let hevcFrames = try frames(hevc, encodedRGB, limit: 1)
    let h0 = floats(hevcFrames[0])
    var hevcErrors: [Double] = []
    for i in indices {
        let v = mean(h0, width: width, layout.core(i))
        let light = SIMD3(pow(max(v.x, 0), 2.4), pow(max(v.y, 0), 2.4), pow(max(v.z, 0), 2.4))
        hevcErrors.append(deltaE2000(lab(linear: light), lab(linear: gradedPatches[i])))
    }
    let hevcStats = Errors(values: hevcErrors)
    print("      written values decoded with a 2.4 power, against the preview: \(hevcStats.summary)")
    check(hevcStats.largest < 1.0, "a BT.1886 display shows what the preview shows")

    print("partial export")
    let part = dir.appendingPathComponent("part.mov")
    let range = CMTimeRange(start: CMTime(value: 12, timescale: fps), duration: CMTime(value: 6, timescale: fps))
    try await ClipExporter().export(source: clipURL, to: part, look: look, lut: lut,
                                    options: .init(codec: .proRes422HQ, display: .appleDisplay, timeRange: range),
                                    progress: { _ in })
    let partFrames = try frames(part, quickTimeLinear)
    let partMarker = mean(floats(partFrames[0]), width: width, layout.core(TestClip.frameMarker))
    check(partFrames.count == 6, "a 6-frame range writes 6 frames (\(partFrames.count))")
    let start = try marker(partMarker, isFrame: 12)
    check(start.error < start.neighbour,
          String(format: "starting at frame 12 (marker dE %.2f, frame 11's or 13's %.2f)", start.error, start.neighbour))

    print("cancelling")
    let cancelled = dir.appendingPathComponent("cancelled.mov")
    let exporter = ClipExporter()
    var threw = false
    do {
        try await exporter.export(source: clipURL, to: cancelled, look: look, lut: lut,
                                  options: .init(codec: .proRes422HQ, display: .appleDisplay, timeRange: nil),
                                  progress: { if $0 > 0.2 { exporter.cancel() } })
    } catch ClipExporter.ExportError.cancelled {
        threw = true
    }
    check(threw && !FileManager.default.fileExists(atPath: cancelled.path),
          "stops, says so, and leaves no partial file")

    print("effects")
    func withEffects(_ configure: (inout GlowLook) -> Void, size: (Int, Int) = (width, height)) throws -> [Float] {
        var fx = noEffects
        configure(&fx)
        return try pipeline.renderLinear(frame0, width: size.0, height: size.1, effects: fx,
                                         settings: InputSettings(), mode: .graded)
    }
    let bloom = try withEffects { $0.bloom.enabled = true }
    let spec = layout.rect(TestClip.highlight)
    // The ground just outside the specular patch, where bloom should spill.
    let ring = (x0: spec.x1 + 2, y0: spec.y0, x1: spec.x1 + 14, y1: spec.y1)
    let before = luminance(mean(graded, width: width, ring)), after = luminance(mean(bloom, width: width, ring))
    check(after > before * 1.15, String(format: "diffusion spills light off the highlight (%.3f -> %.3f)", before, after))
    var untouched = 0.0
    for i in indices where i != TestClip.highlight {
        untouched = max(untouched, deltaE2000(lab(linear: mean(bloom, width: width, layout.core(i))), lab(linear: gradedPatches[i])))
    }
    check(untouched < 0.1, String(format: "and leaves the other patches alone (dE %.3f)", untouched))
    let halation = try withEffects { $0.halation.enabled = true; $0.halation.amount = 100 }
    let tint = mean(halation, width: width, ring) - mean(graded, width: width, ring)
    check(tint.x > 0 && tint.x > 3 * tint.z, String(format: "halation glows red (+%.4f R, +%.4f B)", tint.x, tint.z))
    // The preview renders smaller frames; its effects must be the export's, scaled.
    let halfBloom = try withEffects({ $0.bloom.enabled = true }, size: (width / 2, height / 2))
    let halfGraded = try pipeline.renderLinear(frame0, width: width / 2, height: height / 2, effects: noEffects,
                                               settings: InputSettings(), mode: .graded)
    let halfRing = (x0: ring.x0 / 2, y0: ring.y0 / 2, x1: ring.x1 / 2, y1: ring.y1 / 2)
    let gainFull = luminance(mean(bloom, width: width, ring)) - before
    let gainHalf = luminance(mean(halfBloom, width: width / 2, halfRing)) - luminance(mean(halfGraded, width: width / 2, halfRing))
    check(abs(gainHalf - gainFull) < 0.2 * gainFull,
          String(format: "a half-size preview blooms like the full-size export (+%.4f vs +%.4f)", gainHalf, gainFull))

    print("black and white")
    var bw = MotionLook.initial
    bw.recipe = bw.recipe.loadingFilm("kodak_tri_x_400")
    check(bw.recipe.printStock == "ilford_multigrade_rc_deluxe", "Tri-X loads with its own paper, Multigrade")
    pipeline.setLUT(try baker.bake(recipe: bw.recipe))
    bw.effects = noEffects
    bw.effects.halation.enabled = true
    bw.effects.halation.amount = 100
    let silver = try pipeline.renderLinear(frame0, width: width, height: height, effects: noEffects,
                                           settings: InputSettings(), mode: .graded)
    let silverHalation = try pipeline.renderLinear(frame0, width: width, height: height, effects: bw.effectsAsPrinted,
                                                   settings: InputSettings(), mode: .graded)
    func chroma(_ c: SIMD3<Double>) -> Double { let l = lab(linear: c); return hypot(l.y, l.z) }
    let worstChroma = indices.map { chroma(mean(silver, width: width, layout.core($0))) }.max()!
    check(worstChroma < 0.1, String(format: "every patch prints neutral in silver (chroma %.3f at most)", worstChroma))
    let greyGlow = mean(silverHalation, width: width, ring) - mean(silver, width: width, ring)
    check(luminance(greyGlow) > 0.2 * luminance(tint) && chroma(mean(silverHalation, width: width, ring)) < 0.1,
          String(format: "halation glows grey, not red (+%.4f R, +%.4f B)", greyGlow.x, greyGlow.z))
    pipeline.setLUT(lut)

    // Through the bake, as the clip's frames see it: a red filter holds back
    // blue and passes red, and opens up so the grey card prints the same.
    let probes = [TestClip.grey, TestClip.redMarker, TestClip.blueMarker]
        .map { SIMD3<Float>(TestClip.patchCodes($0, frame: 0) / 1023) }
    func lightness(_ recipe: Recipe) throws -> [Double] {
        try baker.direct(probes, recipe: recipe, input: .nlogCode, output: .linear)
            .map { lab(linear: SIMD3(Double($0.x), Double($0.y), Double($0.z))).x }
    }
    var redFilter = bw.recipe
    redFilter.lensFilter = "red"
    let open = try lightness(bw.recipe), filtered = try lightness(redFilter)
    check(abs(filtered[0] - open[0]) < 1.5 && filtered[1] > open[1] + 5 && filtered[2] < open[2] - 5,
          String(format: "a red filter: grey L* %.1f -> %.1f, red %.1f -> %.1f, blue %.1f -> %.1f",
                 open[0], filtered[0], open[1], filtered[1], open[2], filtered[2]))

    print("other log formats")
    // The same chart recorded the way a Lumix (V-Log) and an iPhone (Apple Log,
    // Apple Log 2) record it: each graded patch has to be the engine's print
    // of the light it records, and an 18% grey has to print alike whichever
    // camera recorded it.
    let nlogGrey = lab(linear: gradedPatches[TestClip.grey]).x
    for format in [LogFormat.vlog, .appleLog, .appleLog2, .flog, .flog2, .clog2, .clog3, .slog3Cine, .slog3] {
        let url = dir.appendingPathComponent("\(format.shortName).mov")
        try TestClip.write(to: url, width: 640, height: 360, frames: 2, fps: 24, audio: false, format: format,
                           metadata: TestClip.camera(format))
        let clip = try await ClipInfo.load(url)
        let small = TestClip.Layout(width: 640, height: 360)
        let decodedFrame = try frames(url, clip.decodeAttributes, limit: 1)[0]
        var settings = InputSettings()
        settings.format = format
        pipeline.setLUT(try baker.bake(recipe: look.recipe, input: format.lutInput))
        let out = try pipeline.renderLinear(decodedFrame, width: 640, height: 360, effects: noEffects,
                                            settings: settings, mode: .graded)
        // What the file holds, not what was written, as for N-Log above.
        let held = try FramePipeline.reading(of: decodedFrame, settings: settings)
        let coordinates = indices.map { i -> SIMD3<Float> in
            let c = meanCodes(decodedFrame, reading: held, small.core(i))
            return SIMD3<Float>(format.onCodeValues ? c / 1023 : (c - 64) / 876)
        }
        let truth = try baker.direct(coordinates, recipe: look.recipe, input: format.lutInput, output: .linear)
        let errors = indices.map { i in
            deltaE2000(lab(linear: mean(out, width: 640, small.core(i))),
                       lab(linear: SIMD3(Double(truth[i].x), Double(truth[i].y), Double(truth[i].z))))
        }
        let worst = Errors(values: errors).largest
        let grey = lab(linear: mean(out, width: 640, small.core(TestClip.grey))).x
        check(worst < 1.0, String(format: "%@: every patch is the engine's print of the light it records (worst dE %.2f)",
                                  format.shortName, worst))
        check(abs(grey - nlogGrey) < 1,
              String(format: "%@: an 18%% grey card prints at L* %.1f, as it does from N-Log (%.1f)", format.shortName, grey, nlogGrey))
        // A Canon camera's file names only the camera, and Canon Log 3 is what
        // most record: Canon Log 2 is read as that until it is chosen.
        let (detected, basis) = clip.detection
        let expected = format == .clog2 ? LogFormat.clog3 : format
        check(detected == expected,
              "\(format.shortName): read as \(detected.shortName), \(basis.rawValue)"
              + (format == .clog2 ? " (Canon Log 2 has to be chosen)" : ""))
    }
    pipeline.setLUT(lut)
    // Read wrongly, footage is far off: Apple Log taken for N-Log.
    do {
        let url = dir.appendingPathComponent("Apple Log.mov")
        let clip = try await ClipInfo.load(url)
        let decodedFrame = try frames(url, clip.decodeAttributes, limit: 1)[0]
        let out = try pipeline.renderLinear(decodedFrame, width: 640, height: 360, effects: noEffects,
                                            settings: InputSettings(), mode: .graded)
        let grey = lab(linear: mean(out, width: 640, TestClip.Layout(width: 640, height: 360).core(TestClip.grey))).x
        check(abs(grey - nlogGrey) > 5, String(format: "and the format matters: Apple Log read as N-Log prints that grey at L* %.1f", grey))
    }

    print("preview placement")
    // Where the red quadrant lands in a 900 x 900 preview: its centre, as a
    // fraction of the canvas.
    let quadrant = quadrantFrame(width: 640, height: 360)
    func redCentre(turns: Int) throws -> CGPoint {
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba16Float, width: 900, height: 900, mipmapped: false)
        d.usage = [.shaderRead, .shaderWrite]
        d.storageMode = .shared
        let texture = pipeline.device.makeTexture(descriptor: d)!
        let cb = pipeline.queue.makeCommandBuffer()!
        try pipeline.renderPreview(quadrant, into: texture, quarterTurns: turns, padding: 10,
                                   background: CIColor(red: 0, green: 0, blue: 0), effects: noEffects,
                                   settings: InputSettings(), mode: .graded, commandBuffer: cb)
        cb.commit()
        cb.waitUntilCompleted()
        var half = [Float16](repeating: 0, count: 900 * 900 * 4)
        half.withUnsafeMutableBytes { texture.getBytes($0.baseAddress!, bytesPerRow: 900 * 8, from: MTLRegionMake2D(0, 0, 900, 900), mipmapLevel: 0) }
        var sum = CGPoint.zero, n = 0.0
        for y in 0..<900 {
            for x in 0..<900 {
                let o = (y * 900 + x) * 4
                let r = Float(half[o]), b = Float(half[o + 2])
                if r > 0.03 && r > 4 * b { sum.x += CGFloat(x); sum.y += CGFloat(y); n += 1 }
            }
        }
        return CGPoint(x: sum.x / n / 900, y: sum.y / n / 900)
    }
    let upright = try redCentre(turns: 0)
    check(upright.x < 0.5 && upright.y < 0.5,
          String(format: "upright, the red quadrant shows top-left (%.2f, %.2f)", upright.x, upright.y))
    let turned = try redCentre(turns: 1)
    check(turned.x > 0.5 && turned.y < 0.5,
          String(format: "turned a quarter clockwise, it shows top-right (%.2f, %.2f)", turned.x, turned.y))

    print("rotated clips")
    let rotatedURL = dir.appendingPathComponent("rotated.mov")
    try TestClip.write(to: rotatedURL, width: 640, height: 360, frames: 4, fps: fps,
                       transform: CGAffineTransform(a: 0, b: 1, c: -1, d: 0, tx: 360, ty: 0), audio: false)
    let rotatedOut = dir.appendingPathComponent("rotated-out.mov")
    try await ClipExporter().export(source: rotatedURL, to: rotatedOut, look: look, lut: lut,
                                    options: .init(codec: .proRes422HQ, display: .appleDisplay, timeRange: nil),
                                    progress: { _ in })
    let rotatedInfo = try await ClipInfo.load(rotatedURL), rotatedOutInfo = try await ClipInfo.load(rotatedOut)
    check(rotatedInfo.quarterTurns == 1 && rotatedOutInfo.quarterTurns == 1 && !rotatedOutInfo.hasAudio,
          "a portrait clip stays portrait (\(rotatedOutInfo.displaySize.width)x\(rotatedOutInfo.displaySize.height))")

    print("speed, 3840x2160")
    let uhd = TestClip.makeFrame(width: 3840, height: 2160)
    var out: CVPixelBuffer?
    CVPixelBufferCreate(nil, 3840, 2160, kCVPixelFormatType_64RGBAHalf,
                        [kCVPixelBufferMetalCompatibilityKey: true, kCVPixelBufferIOSurfacePropertiesKey: [String: Any]()] as CFDictionary, &out)
    let plain = try timeIt(10) {
        try pipeline.renderExport(uhd, into: out!, effects: noEffects, settings: InputSettings(), display: .appleDisplay)
    }
    var all = noEffects
    all.bloom.enabled = true
    all.glow.enabled = true
    all.halation.enabled = true
    let busy = try timeIt(10) {
        try pipeline.renderExport(uhd, into: out!, effects: all, settings: InputSettings(), display: .appleDisplay)
    }
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba16Float, width: 2400, height: 1400, mipmapped: false)
    d.usage = [.shaderRead, .shaderWrite]
    d.storageMode = .private
    let drawable = pipeline.device.makeTexture(descriptor: d)!
    let preview = try timeIt(20) {
        let cb = pipeline.queue.makeCommandBuffer()!
        try pipeline.renderPreview(uhd, into: drawable, quarterTurns: 0, padding: 20, background: CIColor(red: 0.01, green: 0.01, blue: 0.01),
                                   effects: all, settings: InputSettings(), mode: .graded, commandBuffer: cb)
        cb.commit()
        cb.waitUntilCompleted()
    }
    print(String(format: "      export frame %.1f ms plain, %.1f ms with diffusion, glow and halation; preview frame %.1f ms with all three",
                 plain, busy, preview))
    // A shared CI machine's virtual GPU is no measure of the app's speed: there
    // the timings are reported, not judged.
    if ProcessInfo.processInfo.environment["CI"] == nil {
        check(busy < 60 && preview < 30, "fast enough to play 4K at 30 fps and export faster than real time")
    } else {
        print("   note speed not judged on CI")
    }
}

try await run()
print(failures == 0 ? "\nall video checks pass" : "\n\(failures) video check(s) failed")
exit(failures == 0 ? 0 : 1)
