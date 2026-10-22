import Foundation
import AVFoundation
import VideoToolbox

/// Writes a graded copy of a clip.
///
/// Every frame goes through the same `FramePipeline` the preview uses, at full
/// size; the audio is copied sample for sample, untouched. The file is a
/// QuickTime movie tagged Rec.709, written with the curve the chosen displays
/// decode (`ExportSettings.display`), and keeps the source's rotation and
/// metadata.
final class ClipExporter: @unchecked Sendable {

    struct Options {
        var codec: ExportCodec
        var display: LookLUT.Output
        /// nil exports the whole clip.
        var timeRange: CMTimeRange?
    }

    enum ExportError: Error, LocalizedError {
        case cannotRead(String)
        case cannotWrite(String)
        case cancelled

        var errorDescription: String? {
            switch self {
            case .cannotRead(let m): return "Could not read the clip: \(m)"
            case .cannotWrite(let m): return "Could not write the export: \(m)"
            case .cancelled: return "Export cancelled."
            }
        }
    }

    private let lock = NSLock()
    private var cancelled = false
    private var failure: Error?

    func cancel() { lock.withLock { cancelled = true } }
    private var isCancelled: Bool { lock.withLock { cancelled } }
    private func fail(_ error: Error) { lock.withLock { if failure == nil { failure = error }; cancelled = true } }

    static func videoSettings(codec: ExportCodec, width: Int, height: Int,
                              frameRate: Double, sourceDataRate: Float) -> [String: Any] {
        var settings: [String: Any] = [
            AVVideoWidthKey: width,
            AVVideoHeightKey: height,
            AVVideoColorPropertiesKey: [
                AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
                AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
                AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2,
            ],
        ]
        switch codec {
        case .proRes422HQ:
            settings[AVVideoCodecKey] = AVVideoCodecType.proRes422HQ
        case .hevc10:
            settings[AVVideoCodecKey] = AVVideoCodecType.hevc
            // A graded image carries more fine detail than flat log footage,
            // so give it a little more than the source had.
            let bitRate = min(max(Double(sourceDataRate) * 1.25, 30e6), 300e6)
            var compression: [String: Any] = [
                kVTCompressionPropertyKey_ProfileLevel as String: kVTProfileLevel_HEVC_Main10_AutoLevel,
                AVVideoAverageBitRateKey: bitRate,
            ]
            if frameRate > 0 { compression[AVVideoExpectedSourceFrameRateKey] = frameRate }
            settings[AVVideoCompressionPropertiesKey] = compression
        }
        return settings
    }

    /// Export `source` graded with `look` (its print already baked into
    /// `lut`) to `destination`. `progress` runs on an arbitrary queue.
    func export(source: URL, to destination: URL, look: MotionLook, lut: LookLUT,
                options: Options, progress: @escaping @Sendable (Double) -> Void) async throws {
        let info = try await ClipInfo.load(source)
        let asset = AVURLAsset(url: source)
        guard let videoTrack = try await asset.loadTracks(withMediaType: .video).first else {
            throw ExportError.cannotRead("no video track")
        }
        let audioTracks = try await asset.loadTracks(withMediaType: .audio)
        let metadata = (try? await asset.load(.metadata)) ?? []

        let pipeline = try FramePipeline()
        pipeline.setLUT(lut)

        let reader: AVAssetReader
        do { reader = try AVAssetReader(asset: asset) } catch { throw ExportError.cannotRead("\(error)") }
        if let range = options.timeRange { reader.timeRange = range }
        let videoOutput = AVAssetReaderTrackOutput(track: videoTrack, outputSettings: info.decodeAttributes)
        videoOutput.alwaysCopiesSampleData = false
        reader.add(videoOutput)

        try? FileManager.default.removeItem(at: destination)
        let writer: AVAssetWriter
        do { writer = try AVAssetWriter(outputURL: destination, fileType: .mov) } catch {
            throw ExportError.cannotWrite("\(error)")
        }
        let width = Int(info.size.width), height = Int(info.size.height)
        let videoInput = AVAssetWriterInput(
            mediaType: .video,
            outputSettings: Self.videoSettings(codec: options.codec, width: width, height: height,
                                               frameRate: info.frameRate, sourceDataRate: info.dataRate))
        videoInput.transform = info.transform
        videoInput.expectsMediaDataInRealTime = false
        let adaptor = AVAssetWriterInputPixelBufferAdaptor(
            assetWriterInput: videoInput,
            sourcePixelBufferAttributes: [
                kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_64RGBAHalf,
                kCVPixelBufferWidthKey as String: width,
                kCVPixelBufferHeightKey as String: height,
                kCVPixelBufferMetalCompatibilityKey as String: true,
                kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any](),
            ])
        guard writer.canAdd(videoInput) else { throw ExportError.cannotWrite("the video settings were refused") }
        writer.add(videoInput)

        var audio: [(output: AVAssetReaderTrackOutput, input: AVAssetWriterInput)] = []
        for track in audioTracks {
            let hint = try? await track.load(.formatDescriptions).first
            let output = AVAssetReaderTrackOutput(track: track, outputSettings: nil)
            output.alwaysCopiesSampleData = false
            let input = AVAssetWriterInput(mediaType: .audio, outputSettings: nil, sourceFormatHint: hint)
            input.expectsMediaDataInRealTime = false
            guard reader.canAdd(output), writer.canAdd(input) else { continue }
            reader.add(output)
            writer.add(input)
            audio.append((output, input))
        }
        writer.metadata = metadata

        guard reader.startReading() else {
            throw ExportError.cannotRead(reader.error?.localizedDescription ?? "unknown error")
        }
        guard writer.startWriting() else {
            throw ExportError.cannotWrite(writer.error?.localizedDescription ?? "unknown error")
        }

        // The session starts where the first frame is, so a clip whose
        // timestamps do not begin at zero keeps them.
        let first = videoOutput.copyNextSampleBuffer()
        let start = first.map(CMSampleBufferGetPresentationTimeStamp) ?? options.timeRange?.start ?? .zero
        writer.startSession(atSourceTime: start)
        let span = (options.timeRange?.duration ?? info.duration).seconds

        let group = DispatchGroup()
        let videoQueue = DispatchQueue(label: "FLXFilmLab.motion.export.video")
        var pending = first
        var videoDone = false
        group.enter()
        videoInput.requestMediaDataWhenReady(on: videoQueue) { [self] in
            guard !videoDone else { return }
            func finish() {
                videoDone = true
                videoInput.markAsFinished()
                group.leave()
            }
            while videoInput.isReadyForMoreMediaData {
                if isCancelled { finish(); return }
                guard let sample = pending ?? videoOutput.copyNextSampleBuffer() else { finish(); return }
                pending = nil
                guard let frame = CMSampleBufferGetImageBuffer(sample) else { continue }
                let time = CMSampleBufferGetPresentationTimeStamp(sample)
                do {
                    guard let pool = adaptor.pixelBufferPool else {
                        throw ExportError.cannotWrite("no pixel buffer pool")
                    }
                    var buffer: CVPixelBuffer?
                    CVPixelBufferPoolCreatePixelBuffer(nil, pool, &buffer)
                    guard let buffer else { throw ExportError.cannotWrite("out of frame buffers") }
                    try pipeline.renderExport(frame, into: buffer, effects: look.effects,
                                              settings: look.input, display: options.display)
                    guard adaptor.append(buffer, withPresentationTime: time) else {
                        throw ExportError.cannotWrite(writer.error?.localizedDescription ?? "a frame was refused")
                    }
                } catch {
                    fail(error)
                    finish()
                    return
                }
                if span > 0 { progress(min(max((time - start).seconds / span, 0), 1)) }
            }
        }

        for (index, pair) in audio.enumerated() {
            let queue = DispatchQueue(label: "FLXFilmLab.motion.export.audio.\(index)")
            var done = false
            group.enter()
            pair.input.requestMediaDataWhenReady(on: queue) { [self] in
                guard !done else { return }
                func finish() {
                    done = true
                    pair.input.markAsFinished()
                    group.leave()
                }
                while pair.input.isReadyForMoreMediaData {
                    if isCancelled { finish(); return }
                    guard let sample = pair.output.copyNextSampleBuffer() else { finish(); return }
                    if !pair.input.append(sample) {
                        fail(ExportError.cannotWrite(writer.error?.localizedDescription ?? "audio was refused"))
                        finish()
                        return
                    }
                }
            }
        }

        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            group.notify(queue: .global()) { continuation.resume() }
        }

        if isCancelled {
            reader.cancelReading()
            writer.cancelWriting()
            try? FileManager.default.removeItem(at: destination)
            throw lock.withLock { failure } ?? ExportError.cancelled
        }
        if reader.status == .failed {
            writer.cancelWriting()
            try? FileManager.default.removeItem(at: destination)
            throw ExportError.cannotRead(reader.error?.localizedDescription ?? "unknown error")
        }
        await writer.finishWriting()
        guard writer.status == .completed else {
            throw ExportError.cannotWrite(writer.error?.localizedDescription ?? "unknown error")
        }
        progress(1)
    }
}
