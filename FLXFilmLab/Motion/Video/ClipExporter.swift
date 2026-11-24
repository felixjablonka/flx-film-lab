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
        /// ProRes's flavour, HEVC's level and its rate under Custom, as a
        /// look's `ExportSettings` keep them.
        var proRes: ProResFlavour = .hq
        var hevcLevel: HEVCLevel = .standard
        var hevcCustomMegabits: Double = 100

        init(codec: ExportCodec, display: LookLUT.Output, timeRange: CMTimeRange?) {
            self.codec = codec
            self.display = display
            self.timeRange = timeRange
        }

        /// As a look's export settings say.
        init(_ export: ExportSettings, timeRange: CMTimeRange? = nil) {
            self.init(codec: export.codec, display: export.display, timeRange: timeRange)
            proRes = export.flavour
            hevcLevel = export.level
            hevcCustomMegabits = export.customMegabits
        }

        var settings: ExportSettings {
            ExportSettings(codec: codec, display: display, proRes: proRes, hevcLevel: hevcLevel,
                           hevcCustomMegabits: hevcCustomMegabits)
        }
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

    /// The writer's settings for a clip `width` x `height` at `frameRate`:
    /// ProRes at its flavour, HEVC 10-bit at its data rate (`ExportSettings`).
    static func videoSettings(_ options: Options, width: Int, height: Int, frameRate: Double) -> [String: Any] {
        var settings: [String: Any] = [
            AVVideoWidthKey: width,
            AVVideoHeightKey: height,
            AVVideoColorPropertiesKey: [
                AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
                AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
                AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2,
            ],
        ]
        switch options.codec {
        case .proRes422HQ:
            switch options.proRes {
            case .proxy: settings[AVVideoCodecKey] = AVVideoCodecType.proRes422Proxy
            case .lt: settings[AVVideoCodecKey] = AVVideoCodecType.proRes422LT
            case .standard: settings[AVVideoCodecKey] = AVVideoCodecType.proRes422
            case .hq: settings[AVVideoCodecKey] = AVVideoCodecType.proRes422HQ
            }
        case .hevc10:
            settings[AVVideoCodecKey] = AVVideoCodecType.hevc
            let bitRate = options.settings.megabits(width: width, height: height, frameRate: frameRate) * 1_000_000
            var compression: [String: Any] = [
                kVTCompressionPropertyKey_ProfileLevel as String: kVTProfileLevel_HEVC_Main10_AutoLevel,
                AVVideoAverageBitRateKey: bitRate,
            ]
            if frameRate > 0 { compression[AVVideoExpectedSourceFrameRateKey] = frameRate }
            settings[AVVideoCompressionPropertiesKey] = compression
        }
        return settings
    }

    /// The source's metadata, with FLX Film Lab and its version in place of
    /// the camera's software, and the comment Settings asks for (the film,
    /// filter and paper or print film) in place of its description. With no
    /// comment asked for, the camera's description stays.
    static func metadata(_ source: [AVMetadataItem], provenance: Exporter.Provenance) -> [AVMetadataItem] {
        var replaced: Set<AVMetadataIdentifier> = [.quickTimeMetadataSoftware, .quickTimeUserDataSoftware,
                                                   .commonIdentifierSoftware]
        var written = [(AVMetadataIdentifier.quickTimeMetadataSoftware, provenance.software)]
        if !provenance.description.isEmpty {
            replaced.formUnion([.quickTimeMetadataDescription, .quickTimeUserDataDescription, .commonIdentifierDescription])
            written.append((.quickTimeMetadataDescription, provenance.description))
        }
        var items = source.filter { item in item.identifier.map { !replaced.contains($0) } ?? true }
        for (identifier, value) in written {
            let item = AVMutableMetadataItem()
            item.identifier = identifier
            item.value = value as NSString
            item.dataType = kCMMetadataBaseDataType_UTF8 as String
            items.append(item)
        }
        return items
    }

    /// Export `source` graded with `look` (its print already baked into
    /// `lut`) to `destination`. `progress` runs on an arbitrary queue.
    func export(source: URL, to destination: URL, look: MotionLook, lut: LookLUT, grain: ClipGrain? = nil,
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
            outputSettings: Self.videoSettings(options, width: width, height: height, frameRate: info.frameRate))
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
        writer.metadata = Self.metadata(metadata, provenance: Exporter.Provenance(recipe: look.recipe))

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
                    // each frame its own grains, numbered by its time as the preview's are
                    let frameGrain = grain?.frame(FrameGrain.frameNumber(at: time.seconds, frameRate: info.frameRate))
                    try pipeline.renderExport(frame, into: buffer, effects: look.effects, grain: frameGrain,
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
        // VideoToolbox labels HEVC Main tier, climbing levels with the rate:
        // the file says the lowest level and tier that hold it instead.
        if options.codec == .hevc10 {
            let rate = options.settings.megabits(width: width, height: height, frameRate: info.frameRate)
            _ = try? HEVCLevels.relabel(destination, as: HEVCLevels.label(width: width, height: height,
                                                                      frameRate: info.frameRate, megabits: rate))
        }
        progress(1)
    }
}
