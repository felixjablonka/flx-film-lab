import Foundation
import AVFoundation
import CoreVideo
import VideoToolbox
import simd
@testable import FLXCore

/// Synthetic log footage with exact 10-bit code values: flat patches of known
/// scene colours on a mid-grey ground, written as video-range BT.2020 Y'CbCr
/// in HEVC Main10, the way a Nikon Z records N-Log (or, given a format, the
/// way a Lumix or an iPhone records its log). Until a real clip arrives, this
/// is what the video checks run on.
enum TestClip {
    struct Patch {
        let name: String
        /// Scene-linear, BT.2020 primaries, 0.18 = grey card.
        let linear: SIMD3<Double>
    }

    static let columns = 8
    static let rows = 5
    static let redMarker = 0
    static let blueMarker = 39
    /// Changes level every frame, so frame order and timing can be checked.
    static let frameMarker = 15
    static let highlight = 12
    static let grey = 6

    static func srgb(_ r: Double, _ g: Double, _ b: Double) -> SIMD3<Double> {
        func lin(_ v: Double) -> Double {
            let c = v / 255
            return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4)
        }
        return rec709To2020(SIMD3(lin(r), lin(g), lin(b)))
    }

    static func rec709To2020(_ c: SIMD3<Double>) -> SIMD3<Double> {
        SIMD3(0.627404 * c.x + 0.329283 * c.y + 0.043313 * c.z,
              0.069097 * c.x + 0.919540 * c.y + 0.011362 * c.z,
              0.016391 * c.x + 0.088013 * c.y + 0.895595 * c.z)
    }

    static func greyStops(_ s: Double) -> SIMD3<Double> { SIMD3(repeating: 0.18 * pow(2, s)) }

    static let patches: [Patch] = {
        var p: [Patch] = [Patch(name: "red marker", linear: SIMD3(0.5, 0.02, 0.02))]
        for s in -5...1 { p.append(Patch(name: "grey \(s) stops", linear: greyStops(Double(s)))) }
        for s in 2...5 { p.append(Patch(name: "grey +\(s) stops", linear: greyStops(Double(s)))) }
        p.append(Patch(name: "specular", linear: greyStops(5.8)))
        p.append(Patch(name: "black", linear: SIMD3(repeating: 0)))
        p.append(Patch(name: "deep shadow", linear: greyStops(-7)))
        p.append(Patch(name: "frame marker", linear: greyStops(0)))
        let checker: [(String, Double, Double, Double)] = [
            ("dark skin", 115, 82, 68), ("light skin", 194, 150, 130), ("blue sky", 98, 122, 157),
            ("foliage", 87, 108, 67), ("blue flower", 133, 128, 177), ("bluish green", 103, 189, 170),
            ("orange", 214, 126, 44), ("purplish blue", 80, 91, 166), ("moderate red", 193, 90, 99),
            ("purple", 94, 60, 108), ("yellow green", 157, 188, 64), ("orange yellow", 224, 163, 46),
            ("blue", 56, 61, 150), ("green", 70, 148, 73), ("red", 175, 54, 60),
            ("yellow", 231, 199, 31), ("magenta", 187, 86, 149), ("cyan", 8, 133, 161),
        ]
        for c in checker { p.append(Patch(name: c.0, linear: srgb(c.1, c.2, c.3))) }
        for (name, c) in [("2020 red", SIMD3(0.36, 0.0, 0.0)), ("2020 green", SIMD3(0.0, 0.36, 0.0)),
                          ("2020 blue", SIMD3(0.0, 0.0, 0.36)), ("2020 cyan", SIMD3(0.0, 0.3, 0.3)),
                          ("2020 magenta", SIMD3(0.3, 0.0, 0.3))] {
            p.append(Patch(name: name, linear: c))
        }
        p.append(Patch(name: "blue marker", linear: SIMD3(0.02, 0.02, 0.5)))
        precondition(p.count == columns * rows)
        return p
    }()

    /// R'G'B' code values, 0...1023, as a video-range file in `format` stores
    /// them: every curve but Apple Log on its own code values, Apple Log's
    /// 0...1 signal over the video levels. The scene is BT.2020; the cameras
    /// record in their own primaries.
    static func codes(_ linear: SIMD3<Double>, format: LogFormat = .nlog) -> SIMD3<Double> {
        func each(_ c: SIMD3<Double>, _ f: (Double) -> Double) -> SIMD3<Double> { SIMD3(f(c.x), f(c.y), f(c.z)) }
        let own = format.lutInput.gamut.map { $0.inverse * linear } ?? linear
        switch format {
        case .nlog: return each(own) { NLog.code(linear: $0) } * 1023
        case .vlog: return each(own) { VLog.code(linear: $0) } * 1023
        case .appleLog, .appleLog2: return 64 + 876 * each(own) { AppleLog.signal(linear: $0) }
        case .flog: return each(own) { FLog.flog.code(linear: $0) } * 1023
        case .flog2: return each(own) { FLog.flog2.code(linear: $0) } * 1023
        case .clog2: return each(own) { CanonLog.code2(linear: $0) } * 1023
        case .clog3: return each(own) { CanonLog.code3(linear: $0) } * 1023
        case .slog3Cine, .slog3: return each(own) { SLog3.code(linear: $0) } * 1023
        case .ilog: return each(own) { ILog.code(linear: $0) } * 1023
        }
    }

    /// The camera metadata a clip in `format` carries in these tests: its
    /// maker and model, and for S-Gamut3 the NonRealTimeMeta XML a Sony
    /// camera writes, which names the curve and gamut.
    static func camera(_ format: LogFormat) -> [AVMetadataItem] {
        func item(_ id: AVMetadataIdentifier, _ value: String) -> AVMetadataItem {
            let m = AVMutableMetadataItem()
            m.identifier = id
            m.value = value as NSString
            m.dataType = kCMMetadataBaseDataType_UTF8 as String
            return m
        }
        func camera(_ make: String, _ model: String) -> [AVMetadataItem] {
            [item(.quickTimeMetadataMake, make), item(.quickTimeMetadataModel, model)]
        }
        switch format {
        case .vlog: return camera("Panasonic", "DC-S9")
        case .flog: return camera("FUJIFILM", "X-T4")
        case .flog2: return camera("FUJIFILM", "X-T5")
        case .clog2: return camera("Canon", "EOS C70")
        case .clog3: return camera("Canon", "EOS R5")
        case .slog3Cine: return camera("Sony", "ILCE-7SM3")
        case .ilog: return camera("Insta360", "Luna Ultra")
        case .slog3:
            return [item(.quickTimeMetadataComment, """
                <?xml version="1.0" encoding="UTF-8"?>
                <NonRealTimeMeta xmlns="urn:schemas-professionalDisc:nonRealTimeMeta:ver.2.20">
                <Device manufacturer="Sony" modelName="ILME-FX3"/>
                <AcquisitionRecord><Group name="CameraUnitMetadataSet">
                <Item name="CaptureGammaEquation" value="s-log3"/>
                <Item name="CaptureColorPrimaries" value="S-Gamut3"/>
                </Group></AcquisitionRecord></NonRealTimeMeta>
                """)]
        default: return []
        }
    }

    static func frameMarkerCode(_ frame: Int) -> Double { 180 + 20 * Double(frame % 30) }

    static func patchCodes(_ index: Int, frame: Int, format: LogFormat = .nlog) -> SIMD3<Double> {
        index == frameMarker ? SIMD3(repeating: frameMarkerCode(frame)) : codes(patches[index].linear, format: format)
    }

    /// Video-range Y'CbCr, BT.2020 non-constant luminance, from R'G'B' codes.
    static func ycbcr(_ code: SIMD3<Double>) -> SIMD3<Double> {
        let kr = 0.2627, kb = 0.0593, kg = 1 - kr - kb
        let e = (code - 64) / 876
        let y = kr * e.x + kg * e.y + kb * e.z
        let cb = (e.z - y) / (2 * (1 - kb)), cr = (e.x - y) / (2 * (1 - kr))
        return SIMD3(64 + 876 * y, 512 + 896 * cb, 512 + 896 * cr)
    }

    struct Layout {
        let width: Int
        let height: Int
        var cellWidth: Int { (width / TestClip.columns) & ~1 }
        var cellHeight: Int { (height / TestClip.rows) & ~1 }
        var inset: Int { max(cellWidth, cellHeight) / 10 & ~1 }

        /// The patch's flat area, even-aligned so 4:2:0 chroma stays inside it.
        func rect(_ index: Int) -> (x0: Int, y0: Int, x1: Int, y1: Int) {
            let col = index % TestClip.columns, row = index / TestClip.columns
            return (col * cellWidth + inset, row * cellHeight + inset,
                    (col + 1) * cellWidth - inset, (row + 1) * cellHeight - inset)
        }

        /// The middle half of the patch, clear of compression at its edges.
        func core(_ index: Int, scale: Double = 1) -> (x0: Int, y0: Int, x1: Int, y1: Int) {
            let r = rect(index)
            let w = r.x1 - r.x0, h = r.y1 - r.y0
            return (Int(Double(r.x0 + w / 4) * scale), Int(Double(r.y0 + h / 4) * scale),
                    Int(Double(r.x1 - w / 4) * scale), Int(Double(r.y1 - h / 4) * scale))
        }

        func patch(at x: Int, _ y: Int) -> Int? {
            let col = x / cellWidth, row = y / cellHeight
            guard col < TestClip.columns, row < TestClip.rows else { return nil }
            let i = row * TestClip.columns + col
            let r = rect(i)
            return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1 ? i : nil
        }
    }

    static func fill(_ buffer: CVPixelBuffer, layout: Layout, frame: Int, format: LogFormat = .nlog) {
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        let background = ycbcr(codes(greyStops(0), format: format))
        var table = [SIMD3<Double>](repeating: background, count: patches.count)
        for i in patches.indices { table[i] = ycbcr(patchCodes(i, frame: frame, format: format)) }
        let yBase = CVPixelBufferGetBaseAddressOfPlane(buffer, 0)!
        let yRow = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0)
        let cBase = CVPixelBufferGetBaseAddressOfPlane(buffer, 1)!
        let cRow = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1)
        func word(_ v: Double) -> UInt16 { UInt16(min(max(v.rounded(), 4), 1019)) << 6 }
        for y in 0..<layout.height {
            let row = yBase.advanced(by: y * yRow).assumingMemoryBound(to: UInt16.self)
            for x in 0..<layout.width {
                row[x] = word((layout.patch(at: x, y).map { table[$0] } ?? background).x)
            }
        }
        for y in 0..<(layout.height / 2) {
            let row = cBase.advanced(by: y * cRow).assumingMemoryBound(to: UInt16.self)
            for x in 0..<(layout.width / 2) {
                let v = layout.patch(at: 2 * x, 2 * y).map { table[$0] } ?? background
                row[2 * x] = word(v.y)
                row[2 * x + 1] = word(v.z)
            }
        }
    }

    static func makeFrame(width: Int, height: Int, frame: Int = 0) -> CVPixelBuffer {
        var buffer: CVPixelBuffer?
        CVPixelBufferCreate(nil, width, height, kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
                            [kCVPixelBufferMetalCompatibilityKey: true,
                             kCVPixelBufferIOSurfacePropertiesKey: [String: Any]()] as CFDictionary, &buffer)
        fill(buffer!, layout: Layout(width: width, height: height), frame: frame)
        CVBufferSetAttachment(buffer!, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_2020, .shouldPropagate)
        CVBufferSetAttachment(buffer!, kCVImageBufferChromaLocationTopFieldKey, kCVImageBufferChromaLocation_Left, .shouldPropagate)
        return buffer!
    }

    /// `count` stereo frames of the 440 Hz tone at 48 kHz, starting at frame
    /// `start`, interleaved. Its own function with explicit types: inline in the
    /// writer's closure it was more than Xcode 16's type checker would solve.
    static func tone(from start: Int, count: Int) -> [Int16] {
        var pcm = [Int16](repeating: 0, count: count * 2)
        for i in 0..<count {
            let phase: Double = 2 * Double.pi * 440 * Double(start + i) / 48000
            let v = Int16(8000 * sin(phase))
            pcm[2 * i] = v
            pcm[2 * i + 1] = v
        }
        return pcm
    }

    /// Write `frames` frames at `fps`, with a 440 Hz stereo tone unless
    /// `audio` is off. Apple Log is tagged the way an iPhone tags it.
    static func write(to url: URL, width: Int, height: Int, frames: Int, fps: Int32,
                      transform: CGAffineTransform = .identity, audio: Bool = true,
                      format: LogFormat = .nlog, metadata: [AVMetadataItem] = []) throws {
        try? FileManager.default.removeItem(at: url)
        let writer = try AVAssetWriter(outputURL: url, fileType: .mov)
        writer.metadata = metadata
        let video = AVAssetWriterInput(mediaType: .video, outputSettings: [
            AVVideoCodecKey: AVVideoCodecType.hevc, AVVideoWidthKey: width, AVVideoHeightKey: height,
            AVVideoColorPropertiesKey: [AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_2020,
                                        AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
                                        AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_2020],
            AVVideoCompressionPropertiesKey: [
                kVTCompressionPropertyKey_ProfileLevel as String: kVTProfileLevel_HEVC_Main10_AutoLevel,
                AVVideoAverageBitRateKey: 120_000_000]])
        video.transform = transform
        video.expectsMediaDataInRealTime = false
        let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: video, sourcePixelBufferAttributes: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
            kCVPixelBufferWidthKey as String: width, kCVPixelBufferHeightKey as String: height])
        writer.add(video)

        var asbd = AudioStreamBasicDescription(
            mSampleRate: 48000, mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
            mBytesPerPacket: 4, mFramesPerPacket: 1, mBytesPerFrame: 4, mChannelsPerFrame: 2,
            mBitsPerChannel: 16, mReserved: 0)
        var audioFormat: CMAudioFormatDescription?
        CMAudioFormatDescriptionCreate(allocator: nil, asbd: &asbd, layoutSize: 0, layout: nil,
                                       magicCookieSize: 0, magicCookie: nil, extensions: nil,
                                       formatDescriptionOut: &audioFormat)
        var sound: AVAssetWriterInput?
        if audio {
            let input = AVAssetWriterInput(mediaType: .audio, outputSettings: [
                AVFormatIDKey: kAudioFormatLinearPCM, AVSampleRateKey: 48000, AVNumberOfChannelsKey: 2,
                AVLinearPCMBitDepthKey: 16, AVLinearPCMIsFloatKey: false, AVLinearPCMIsBigEndianKey: false,
                AVLinearPCMIsNonInterleaved: false], sourceFormatHint: audioFormat)
            input.expectsMediaDataInRealTime = false
            writer.add(input)
            sound = input
        }

        guard writer.startWriting() else { throw writer.error ?? CocoaError(.fileWriteUnknown) }
        writer.startSession(atSourceTime: .zero)
        let layout = Layout(width: width, height: height)
        let totalSamples = Int(48000 * Double(frames) / Double(fps))

        // One queue per track, each fed whenever the writer asks: feeding both
        // from one loop deadlocks once the encoder holds frames back.
        let group = DispatchGroup()
        var frame = 0
        group.enter()
        video.requestMediaDataWhenReady(on: DispatchQueue(label: "testclip.video")) {
            while video.isReadyForMoreMediaData {
                guard frame < frames else {
                    video.markAsFinished()
                    group.leave()
                    return
                }
                var buffer: CVPixelBuffer?
                CVPixelBufferPoolCreatePixelBuffer(nil, adaptor.pixelBufferPool!, &buffer)
                fill(buffer!, layout: layout, frame: frame, format: format)
                if format == .appleLog || format == .appleLog2, #available(macOS 14.2, *) {
                    let tag = format == .appleLog ? LogFormat.appleLogTag : LogFormat.appleLog2Tag
                    CVBufferSetAttachment(buffer!, kCVImageBufferLogTransferFunctionKey, tag as CFString, .shouldPropagate)
                }
                adaptor.append(buffer!, withPresentationTime: CMTime(value: CMTimeValue(frame), timescale: fps))
                frame += 1
            }
        }
        if let sound, let audioFormat {
            var written = 0
            group.enter()
            sound.requestMediaDataWhenReady(on: DispatchQueue(label: "testclip.audio")) {
                while sound.isReadyForMoreMediaData {
                    guard written < totalSamples else {
                        sound.markAsFinished()
                        group.leave()
                        return
                    }
                    let n = min(1024, totalSamples - written)
                    let pcm = tone(from: written, count: n)
                    var block: CMBlockBuffer?
                    CMBlockBufferCreateWithMemoryBlock(allocator: nil, memoryBlock: nil, blockLength: n * 4,
                                                       blockAllocator: nil, customBlockSource: nil,
                                                       offsetToData: 0, dataLength: n * 4, flags: 0,
                                                       blockBufferOut: &block)
                    pcm.withUnsafeBytes { raw in
                        _ = CMBlockBufferReplaceDataBytes(with: raw.baseAddress!, blockBuffer: block!,
                                                          offsetIntoDestination: 0, dataLength: n * 4)
                    }
                    var sample: CMSampleBuffer?
                    CMAudioSampleBufferCreateReadyWithPacketDescriptions(
                        allocator: nil, dataBuffer: block!, formatDescription: audioFormat, sampleCount: n,
                        presentationTimeStamp: CMTime(value: CMTimeValue(written), timescale: 48000),
                        packetDescriptions: nil, sampleBufferOut: &sample)
                    sound.append(sample!)
                    written += n
                }
            }
        }
        group.wait()
        let done = DispatchSemaphore(value: 0)
        writer.finishWriting { done.signal() }
        done.wait()
        guard writer.status == .completed else { throw writer.error ?? CocoaError(.fileWriteUnknown) }
    }
}
