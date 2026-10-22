import AVFoundation
import CoreVideo

/// What a clip is, read once from its track and format description.
struct ClipInfo {
    enum Chroma: String { case c420 = "4:2:0", c422 = "4:2:2", c444 = "4:4:4" }

    enum LoadError: Error, LocalizedError {
        case noVideo(URL)
        var errorDescription: String? {
            switch self {
            case .noVideo(let url): return "\(url.lastPathComponent) has no video track."
            }
        }
    }

    let url: URL
    /// Encoded pixels, before the track's rotation.
    let size: CGSize
    let frameRate: Double
    let duration: CMTime
    let transform: CGAffineTransform
    let codecName: String
    let bitDepth: Int?
    let chroma: Chroma
    /// nil when the file does not say.
    let fullRange: Bool?
    let matrixTag: String?
    let primariesTag: String?
    let transferTag: String?
    /// What the file says about the camera: log tags, maker, model.
    let camera: CameraTags
    /// Core Video's log tag, which Apple sets on footage recorded in Apple Log.
    var logTransferTag: String? { camera.logTransfer }
    let hasAudio: Bool
    /// Bits per second.
    let dataRate: Float

    static func load(_ url: URL) async throws -> ClipInfo {
        let asset = AVURLAsset(url: url)
        guard let track = try await asset.loadTracks(withMediaType: .video).first else {
            throw LoadError.noVideo(url)
        }
        let (naturalSize, frameRate, transform, formats, dataRate) = try await track.load(
            .naturalSize, .nominalFrameRate, .preferredTransform, .formatDescriptions, .estimatedDataRate)
        let duration = try await asset.load(.duration)
        let audio = try await asset.loadTracks(withMediaType: .audio)

        var codecName = "video"
        var bitDepth: Int?
        var chroma = Chroma.c420
        var fullRange: Bool?
        var matrix: String?, primaries: String?, transfer: String?, logTransfer: String?
        if let format = formats.first {
            let subtype = CMFormatDescriptionGetMediaSubType(format)
            (codecName, bitDepth, chroma) = Self.codec(subtype)
            let ext = CMFormatDescriptionGetExtensions(format) as? [String: Any] ?? [:]
            if let bits = ext["BitsPerComponent"] as? Int { bitDepth = bits }
            fullRange = ext[kCMFormatDescriptionExtension_FullRangeVideo as String] as? Bool
            matrix = ext[kCMFormatDescriptionExtension_YCbCrMatrix as String] as? String
            primaries = ext[kCMFormatDescriptionExtension_ColorPrimaries as String] as? String
            transfer = ext[kCMFormatDescriptionExtension_TransferFunction as String] as? String
            logTransfer = ext["LogTransferFunction"] as? String      // kCVImageBufferLogTransferFunctionKey
            // HEVC says exactly what it carries in its decoder configuration.
            if let atoms = ext[kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms as String] as? [String: Any],
               let hvcC = atoms["hvcC"] as? Data, hvcC.count > 18 {
                let bytes = [UInt8](hvcC)
                switch bytes[16] & 0x3 {
                case 2: chroma = .c422
                case 3: chroma = .c444
                default: chroma = .c420
                }
                bitDepth = Int(bytes[17] & 0x7) + 8
            }
        }
        let camera = await Self.cameraTags(asset, url: url, logTransfer: logTransfer)
        return ClipInfo(url: url, size: naturalSize, frameRate: Double(frameRate), duration: duration,
                        transform: transform, codecName: codecName, bitDepth: bitDepth, chroma: chroma,
                        fullRange: fullRange, matrixTag: matrix, primariesTag: primaries,
                        transferTag: transfer, camera: camera, hasAudio: !audio.isEmpty, dataRate: dataRate)
    }

    /// The log format the file says it is in, and on what grounds (`LogFormat.detect`).
    var detection: (format: LogFormat, basis: LogFormat.Basis) { LogFormat.detect(camera) }
    var detectedFormat: LogFormat { detection.format }

    /// The camera, from the file's metadata: its make and model, a Lumix
    /// camera's 'PANA' user-data block (or Panasonic or Lumix named
    /// anywhere), and a Sony camera's NonRealTimeMeta XML. A Nikon Z writes
    /// its own block ('NCTG') and no make.
    private static func cameraTags(_ asset: AVURLAsset, url: URL, logTransfer: String?) async -> CameraTags {
        var tags = CameraTags(logTransfer: logTransfer)
        let pana = 0x5041_4E41            // 'PANA'
        for item in (try? await asset.load(.metadata)) ?? [] {
            if let key = item.key as? NSNumber, key.intValue == pana { tags.panasonic = true }
            guard let value = try? await item.load(.stringValue) else { continue }
            if item.commonKey == .commonKeyMake, tags.make == nil { tags.make = value }
            if item.commonKey == .commonKeyModel, tags.model == nil { tags.model = value }
            if value.localizedCaseInsensitiveContains("panasonic") || value.localizedCaseInsensitiveContains("lumix") {
                tags.panasonic = true
            }
            if value.contains("CaptureGammaEquation") { readSonyXML(value, into: &tags) }
        }
        if tags.gamma == nil, let xml = sonyXML(url) { readSonyXML(xml, into: &tags) }
        return tags
    }

    /// The text of the first box at the top of the file, other than the
    /// media data, that holds a Sony NonRealTimeMeta XML: Sony writes the
    /// curve and gamut it recorded in there (`CaptureGammaEquation`,
    /// `CaptureColorPrimaries`), and the camera (`Device`). Only box headers
    /// are read on the way, so a long clip costs no more than a short one.
    static func sonyXML(_ url: URL) -> String? {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? handle.close() }
        guard let end = try? handle.seekToEnd() else { return nil }
        var offset: UInt64 = 0
        while offset + 8 <= end {
            guard (try? handle.seek(toOffset: offset)) != nil,
                  let header = try? handle.read(upToCount: 16), header.count >= 8 else { return nil }
            let bytes = [UInt8](header)
            var size = bytes[0..<4].reduce(UInt64(0)) { $0 << 8 | UInt64($1) }
            let type = String(decoding: bytes[4..<8], as: UTF8.self)
            if size == 1, bytes.count >= 16 { size = bytes[8..<16].reduce(UInt64(0)) { $0 << 8 | UInt64($1) } }
            if size == 0 { size = end - offset }
            guard size >= 8 else { return nil }
            if !["mdat", "free", "skip", "wide"].contains(type), size <= 64 << 20,
               (try? handle.seek(toOffset: offset)) != nil,
               let data = try? handle.read(upToCount: Int(size)) {
                let text = String(decoding: data, as: UTF8.self)
                if text.contains("CaptureGammaEquation") { return text }
            }
            offset += size
        }
        return nil
    }

    /// The curve, gamut and camera out of a Sony NonRealTimeMeta XML.
    static func readSonyXML(_ xml: String, into tags: inout CameraTags) {
        func attribute(after marker: String, _ name: String) -> String? {
            guard let at = xml.range(of: marker) else { return nil }
            let tail = xml[at.upperBound...]
            guard let open = tail.range(of: name + "=\""),
                  let close = tail[open.upperBound...].firstIndex(of: "\"") else { return nil }
            return String(tail[open.upperBound..<close])
        }
        tags.gamma = attribute(after: "\"CaptureGammaEquation\"", "value") ?? tags.gamma
        tags.primaries = attribute(after: "\"CaptureColorPrimaries\"", "value") ?? tags.primaries
        if tags.make == nil { tags.make = attribute(after: "<Device", "manufacturer") }
        if tags.model == nil { tags.model = attribute(after: "<Device", "modelName") }
    }

    private static func codec(_ subtype: FourCharCode) -> (String, Int?, Chroma) {
        switch subtype {
        case kCMVideoCodecType_HEVC, FourCharCode(0x6865_7631): return ("HEVC", nil, .c420)   // 'hvc1', 'hev1'
        case kCMVideoCodecType_H264: return ("H.264", 8, .c420)
        case kCMVideoCodecType_AppleProRes422HQ: return ("ProRes 422 HQ", 10, .c422)
        case kCMVideoCodecType_AppleProRes422: return ("ProRes 422", 10, .c422)
        case kCMVideoCodecType_AppleProRes422LT: return ("ProRes 422 LT", 10, .c422)
        case kCMVideoCodecType_AppleProRes422Proxy: return ("ProRes 422 Proxy", 10, .c422)
        case kCMVideoCodecType_AppleProRes4444: return ("ProRes 4444", 12, .c444)
        case kCMVideoCodecType_AppleProRes4444XQ: return ("ProRes 4444 XQ", 12, .c444)
        default: return ("video", nil, .c420)
        }
    }

    /// The decoder's own layout, so frames arrive as recorded rather than
    /// converted: same chroma subsampling, same range, 10 bits where there are
    /// more than 8.
    var decodePixelFormat: OSType {
        let full = fullRange ?? false
        switch chroma {
        case .c420:
            if (bitDepth ?? 8) > 8 {
                return full ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
                            : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
            }
            return full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                        : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        case .c422:
            return full ? kCVPixelFormatType_422YpCbCr10BiPlanarFullRange
                        : kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange
        case .c444:
            return full ? kCVPixelFormatType_444YpCbCr10BiPlanarFullRange
                        : kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange
        }
    }

    /// Decoded frames as the pipeline wants them.
    var decodeAttributes: [String: Any] {
        [kCVPixelBufferPixelFormatTypeKey as String: decodePixelFormat,
         kCVPixelBufferMetalCompatibilityKey as String: true,
         kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any]()]
    }

    /// Clockwise quarter turns the track asks for on playback.
    var quarterTurns: Int {
        let degrees = atan2(Double(transform.b), Double(transform.a)) * 180 / .pi
        return (Int((degrees / 90).rounded()) % 4 + 4) % 4
    }

    /// As shown: rotated by the track's transform.
    var displaySize: CGSize {
        quarterTurns % 2 == 1 ? CGSize(width: size.height, height: size.width) : size
    }

    var durationSeconds: Double { duration.seconds.isFinite ? duration.seconds : 0 }

    var summary: String {
        var parts = ["\(Int(size.width))×\(Int(size.height))"]
        parts.append(frameRate.rounded() == frameRate ? "\(Int(frameRate)) fps" : String(format: "%.3f fps", frameRate))
        parts.append(bitDepth.map { "\(codecName) \($0)-bit" } ?? codecName)
        parts.append(chroma.rawValue)
        if let fullRange { parts.append(fullRange ? "full range" : "video range") }
        if let matrixTag { parts.append(Self.matrixName(matrixTag)) }
        return parts.joined(separator: " · ")
    }

    static func matrixName(_ tag: String) -> String {
        switch tag {
        case String(kCVImageBufferYCbCrMatrix_ITU_R_2020): return "BT.2020"
        case String(kCVImageBufferYCbCrMatrix_ITU_R_709_2): return "BT.709"
        case String(kCVImageBufferYCbCrMatrix_ITU_R_601_4): return "BT.601"
        default: return tag
        }
    }
}
