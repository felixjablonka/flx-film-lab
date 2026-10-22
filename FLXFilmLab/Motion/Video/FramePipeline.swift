import Foundation
import Metal
import QuartzCore
import CoreImage
import CoreVideo
import simd

/// One frame, from decoded Y'CbCr to the finished picture.
///
/// The grade kernel reads the frame's planes directly — no colour management,
/// no format conversion — turns them into the log format's own values (code
/// values for N-Log and V-Log, the 0...1 signal for Apple Log) and looks those
/// up in the print LUT baked for that format. Glow's diffusion, glow and halation then run on the
/// print in Core Image, on the same command buffer. Preview and export share
/// all of it and differ only in size and destination.
///
/// Core Image keeps a texture's row 0 at its y = 0, so the frame reads upside
/// down in Core Image's own terms but every texture-to-texture render keeps the
/// rows where they were. Destinations are always textures with `isFlipped`
/// off; a pixel-buffer destination would flip by default.
final class FramePipeline {

    enum Mode: Int32 {
        case graded = 0
        /// The untouched log footage, the way QuickTime shows a log file.
        case log = 1
        /// R'G'B' code values / 1023, for checking the decode.
        case codes = 2
    }

    /// Must match `GradeUniforms` in motion.metal.
    struct GradeUniforms {
        var lumaSize: SIMD2<Float>
        var chromaSize: SIMD2<Float>
        var chromaScale: SIMD2<Float>
        var chromaSiting: SIMD2<Float>
        var texelMax: Float
        var codeStep: Float
        var yBlack: Float
        var yWhite: Float
        var cMid: Float
        var cRange: Float
        var kr: Float
        var kb: Float
        var fullRange: Int32
        var lutSize: Int32
        var mode: Int32
        var taps: Int32
        var onSignal: Int32
    }

    /// How one frame's samples are read: its own layout and tags, with the
    /// settings' overrides applied.
    struct Reading: Equatable {
        var bitDepth: Int
        var chroma: ClipInfo.Chroma
        var fullRange: Bool
        var kr: Float
        var kb: Float
        var matrixName: String
        var chromaSiting: SIMD2<Float>
        var format: LogFormat
    }

    enum PipelineError: Error, LocalizedError {
        case noDevice
        case shader(String)
        case unsupportedFormat(OSType)
        case texture
        case noLUT

        var errorDescription: String? {
            switch self {
            case .noDevice: return "No Metal device available."
            case .shader(let m): return "The frame shader failed to compile: \(m)"
            case .unsupportedFormat(let f): return "Frames arrived in an unsupported pixel format (\(Self.fourCC(f)))."
            case .texture: return "Could not map a video frame to a Metal texture."
            case .noLUT: return "The print look has not been baked yet."
            }
        }

        static func fourCC(_ f: OSType) -> String {
            String(bytes: [24, 16, 8, 0].map { UInt8((f >> $0) & 0xFF) }, encoding: .ascii) ?? "\(f)"
        }
    }

    static let linearSRGB = CGColorSpace(name: CGColorSpace.extendedLinearSRGB)!

    let device: MTLDevice
    let queue: MTLCommandQueue
    let ciContext: CIContext
    private let gradePipeline: MTLComputePipelineState
    private let textureCache: CVMetalTextureCache
    private let encodeKernel: CIColorKernel
    private var lutTexture: MTLTexture?
    private(set) var lutSize = 0
    private var gradedTexture: MTLTexture?

    init(device: MTLDevice? = nil) throws {
        guard let device = device ?? MTLCreateSystemDefaultDevice(),
              let queue = device.makeCommandQueue() else { throw PipelineError.noDevice }
        self.device = device
        self.queue = queue

        let library: MTLLibrary
        do {
            library = try device.makeLibrary(source: try Self.shaderSource(), options: nil)
        } catch {
            throw PipelineError.shader("\(error)")
        }
        guard let function = library.makeFunction(name: "grade_frame") else {
            throw PipelineError.shader("grade_frame not found")
        }
        gradePipeline = try device.makeComputePipelineState(function: function)

        var cache: CVMetalTextureCache?
        CVMetalTextureCacheCreate(nil, nil, device, nil, &cache)
        guard let cache else { throw PipelineError.texture }
        textureCache = cache

        ciContext = CIContext(mtlCommandQueue: queue, options: [
            .workingColorSpace: Self.linearSRGB,
            .workingFormat: CIFormat.RGBAh,
            .cacheIntermediates: false,
            .name: "FLX Film Lab Motion",
        ])

        let encodeSource = """
        #include <CoreImage/CoreImage.h>
        using namespace metal;
        extern "C" { namespace coreimage {
        // Display light to the export's transfer curve.
        [[stitchable]] float4 flxEncodePower(sample_t s, float inverseGamma) {
            return float4(powr(max(s.rgb, 0.0f), float3(inverseGamma)), s.a);
        }
        }}
        """
        guard let kernel = try CIKernel.kernels(withMetalString: encodeSource)
                .first(where: { $0.name == "flxEncodePower" }) as? CIColorKernel else {
            throw PipelineError.shader("flxEncodePower")
        }
        encodeKernel = kernel
    }

    static func shaderSource() throws -> String {
        let candidates: [URL] = [
            Bundle.main.resourceURL?.appendingPathComponent("Shaders/motion.metal"),
            URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
                .appendingPathComponent("FLXFilmLab/Motion/Resources/Shaders/motion.metal"),
        ].compactMap { $0 }
        for url in candidates {
            if let source = try? String(contentsOf: url, encoding: .utf8) { return source }
        }
        throw PipelineError.shader("Shaders/motion.metal is missing")
    }

    // MARK: - The look

    /// Upload a baked look. The values are the LUT's 2.4-power encoding, as
    /// `LookBaker` writes them by default.
    func setLUT(_ lut: LookLUT) {
        let n = lut.size
        if lutTexture == nil || lutSize != n {
            let d = MTLTextureDescriptor()
            d.textureType = .type3D
            d.pixelFormat = .rgba32Float
            d.width = n
            d.height = n
            d.depth = n
            d.usage = [.shaderRead]
            d.storageMode = .shared
            lutTexture = device.makeTexture(descriptor: d)
            lutSize = n
        }
        let rgba = lut.rgba
        rgba.withUnsafeBytes { raw in
            lutTexture?.replace(region: MTLRegionMake3D(0, 0, 0, n, n, n), mipmapLevel: 0, slice: 0,
                                withBytes: raw.baseAddress!, bytesPerRow: n * 16, bytesPerImage: n * n * 16)
        }
    }

    var hasLUT: Bool { lutTexture != nil }

    // MARK: - Reading a frame

    static func reading(of buffer: CVPixelBuffer, settings: InputSettings) throws -> Reading {
        let format = CVPixelBufferGetPixelFormatType(buffer)
        let layout: (bits: Int, chroma: ClipInfo.Chroma, full: Bool)
        switch format {
        case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange: layout = (10, .c420, false)
        case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange: layout = (10, .c420, true)
        case kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange: layout = (10, .c422, false)
        case kCVPixelFormatType_422YpCbCr10BiPlanarFullRange: layout = (10, .c422, true)
        case kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange: layout = (10, .c444, false)
        case kCVPixelFormatType_444YpCbCr10BiPlanarFullRange: layout = (10, .c444, true)
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange: layout = (8, .c420, false)
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange: layout = (8, .c420, true)
        default: throw PipelineError.unsupportedFormat(format)
        }

        var fullRange = layout.full
        switch settings.levels {
        case .auto: break
        case .video: fullRange = false
        case .full: fullRange = true
        }

        let tag = CVBufferCopyAttachment(buffer, kCVImageBufferYCbCrMatrixKey, nil) as? String
        let matrix: (kr: Float, kb: Float, name: String)
        switch settings.matrix {
        case .bt2020: matrix = (0.2627, 0.0593, "BT.2020")
        case .bt709: matrix = (0.2126, 0.0722, "BT.709")
        case .bt601: matrix = (0.299, 0.114, "BT.601")
        case .auto:
            switch tag {
            case String(kCVImageBufferYCbCrMatrix_ITU_R_709_2): matrix = (0.2126, 0.0722, "BT.709")
            case String(kCVImageBufferYCbCrMatrix_ITU_R_601_4): matrix = (0.299, 0.114, "BT.601")
            case String(kCVImageBufferYCbCrMatrix_ITU_R_2020): matrix = (0.2627, 0.0593, "BT.2020")
            // Untagged: log footage is usually BT.2020-encoded, so that is
            // the likelier guess.
            default: matrix = (0.2627, 0.0593, "BT.2020 (assumed)")
            }
        }

        // Where chroma samples sit relative to luma, in luma pixels. HEVC and
        // H.264 default to "left": co-sited horizontally, between rows.
        let location = CVBufferCopyAttachment(buffer, kCVImageBufferChromaLocationTopFieldKey, nil) as? String
        var siting: SIMD2<Float>
        switch location {
        case String(kCVImageBufferChromaLocation_Center): siting = SIMD2(0.5, 0.5)
        case String(kCVImageBufferChromaLocation_TopLeft): siting = SIMD2(0, 0)
        case String(kCVImageBufferChromaLocation_Top): siting = SIMD2(0.5, 0)
        case String(kCVImageBufferChromaLocation_BottomLeft): siting = SIMD2(0, 1)
        case String(kCVImageBufferChromaLocation_Bottom): siting = SIMD2(0.5, 1)
        default: siting = SIMD2(0, 0.5)
        }
        switch layout.chroma {
        case .c420: break
        case .c422: siting.y = 0
        case .c444: siting = SIMD2(0, 0)
        }

        return Reading(bitDepth: layout.bits, chroma: layout.chroma, fullRange: fullRange,
                       kr: matrix.kr, kb: matrix.kb, matrixName: matrix.name, chromaSiting: siting,
                       format: settings.format ?? .nlog)
    }

    /// The R'G'B' code values (0...1023) the grade kernel reads, worked out on
    /// the CPU with nearest chroma: for the preview's probe under the pointer
    /// and the clip meter. `body` runs with the frame locked; the sampler must
    /// not outlive it.
    static func withCodes<T>(of buffer: CVPixelBuffer, reading: Reading,
                             _ body: (CodeSampler) -> T) -> T? {
        guard CVPixelBufferGetPlaneCount(buffer) == 2 else { return nil }
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        guard let luma = CVPixelBufferGetBaseAddressOfPlane(buffer, 0),
              let chroma = CVPixelBufferGetBaseAddressOfPlane(buffer, 1) else { return nil }
        return body(CodeSampler(buffer: buffer, reading: reading, luma: luma, chroma: chroma))
    }

    static func codes(at x: Int, _ y: Int, of buffer: CVPixelBuffer, reading: Reading) -> SIMD3<Double>? {
        withCodes(of: buffer, reading: reading) { $0.codes(x, y) } ?? nil
    }

    struct CodeSampler {
        let width: Int
        let height: Int
        private let reading: Reading
        private let luma: UnsafeMutableRawPointer
        private let chroma: UnsafeMutableRawPointer
        private let lumaRow: Int
        private let chromaRow: Int
        private let tenBit: Bool
        private let levels: (black: Double, white: Double, mid: Double, range: Double)

        init(buffer: CVPixelBuffer, reading: Reading, luma: UnsafeMutableRawPointer, chroma: UnsafeMutableRawPointer) {
            width = CVPixelBufferGetWidth(buffer)
            height = CVPixelBufferGetHeight(buffer)
            self.reading = reading
            self.luma = luma
            self.chroma = chroma
            lumaRow = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0)
            chromaRow = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1)
            tenBit = reading.bitDepth > 8
            let unit: Double = tenBit ? 4 : 1
            let top: Double = tenBit ? 1023 : 255
            levels = reading.fullRange ? (0, top, tenBit ? 512 : 128, top)
                                       : (16 * unit, 235 * unit, 128 * unit, 224 * unit)
        }

        private func sample(_ base: UnsafeMutableRawPointer, _ row: Int, _ index: Int) -> Double {
            let p = base.advanced(by: row)
            return tenBit ? Double(p.assumingMemoryBound(to: UInt16.self)[index] >> 6)
                          : Double(p.assumingMemoryBound(to: UInt8.self)[index])
        }

        func codes(_ x: Int, _ y: Int) -> SIMD3<Double>? {
            guard x >= 0, y >= 0, x < width, y < height else { return nil }
            let cx = reading.chroma == .c444 ? x : x / 2
            let cy = reading.chroma == .c420 ? y / 2 : y
            let yCode = sample(luma, y * lumaRow, x)
            let cb = sample(chroma, cy * chromaRow, 2 * cx), cr = sample(chroma, cy * chromaRow, 2 * cx + 1)
            let kr = Double(reading.kr), kb = Double(reading.kb)
            let ey = (yCode - levels.black) / (levels.white - levels.black)
            let ecb = (cb - levels.mid) / levels.range, ecr = (cr - levels.mid) / levels.range
            let r = ey + 2 * (1 - kr) * ecr, b = ey + 2 * (1 - kb) * ecb
            let g = (ey - kr * r - kb * b) / (1 - kr - kb)
            let e = SIMD3(r, g, b)
            return reading.fullRange ? e * 1023 : 64 + 876 * e
        }
    }

    private func planeTexture(_ buffer: CVPixelBuffer, plane: Int,
                              format: MTLPixelFormat) throws -> (MTLTexture, CVMetalTexture) {
        let w = CVPixelBufferGetWidthOfPlane(buffer, plane)
        let h = CVPixelBufferGetHeightOfPlane(buffer, plane)
        var cvTexture: CVMetalTexture?
        CVMetalTextureCacheCreateTextureFromImage(nil, textureCache, buffer, nil, format, w, h, plane, &cvTexture)
        guard let cvTexture, let texture = CVMetalTextureGetTexture(cvTexture) else { throw PipelineError.texture }
        return (texture, cvTexture)
    }

    /// A whole (single-plane) pixel buffer as a texture.
    func texture(for buffer: CVPixelBuffer, format: MTLPixelFormat) throws -> (MTLTexture, CVMetalTexture) {
        var cvTexture: CVMetalTexture?
        CVMetalTextureCacheCreateTextureFromImage(nil, textureCache, buffer, nil, format,
                                                  CVPixelBufferGetWidth(buffer), CVPixelBufferGetHeight(buffer),
                                                  0, &cvTexture)
        guard let cvTexture, let texture = CVMetalTextureGetTexture(cvTexture) else { throw PipelineError.texture }
        return (texture, cvTexture)
    }

    // MARK: - Grading

    /// Encode the grade kernel: `source` read as `reading`, written to
    /// `target` at whatever size that is.
    func encodeGrade(_ source: CVPixelBuffer, reading: Reading, into target: MTLTexture,
                     mode: Mode, commandBuffer: MTLCommandBuffer) throws {
        guard let lut = lutTexture else { throw PipelineError.noLUT }
        let tenBit = reading.bitDepth > 8
        let (luma, lumaRef) = try planeTexture(source, plane: 0, format: tenBit ? .r16Unorm : .r8Unorm)
        let (chroma, chromaRef) = try planeTexture(source, plane: 1, format: tenBit ? .rg16Unorm : .rg8Unorm)

        // Native code values: 10 bits sit in the top of 16-bit words, and only
        // they count (`codeTexel` in motion.metal), as for the CPU's `codes`.
        let unit: Float = tenBit ? 4 : 1
        let levels: (yBlack: Float, yWhite: Float, cMid: Float, cRange: Float) = reading.fullRange
            ? (0, tenBit ? 1023 : 255, tenBit ? 512 : 128, tenBit ? 1023 : 255)
            : (16 * unit, 235 * unit, 128 * unit, 224 * unit)

        let scale = Float(source.width) / Float(target.width)
        var uniforms = GradeUniforms(
            lumaSize: SIMD2(Float(luma.width), Float(luma.height)),
            chromaSize: SIMD2(Float(chroma.width), Float(chroma.height)),
            chromaScale: SIMD2(Float(chroma.width) / Float(luma.width), Float(chroma.height) / Float(luma.height)),
            chromaSiting: reading.chromaSiting,
            texelMax: tenBit ? 65535 : 255, codeStep: tenBit ? 64 : 1,
            yBlack: levels.yBlack, yWhite: levels.yWhite, cMid: levels.cMid, cRange: levels.cRange,
            kr: reading.kr, kb: reading.kb,
            fullRange: reading.fullRange ? 1 : 0,
            lutSize: Int32(lutSize),
            mode: mode.rawValue,
            taps: scale > 2.5 ? 3 : (scale > 1.3 ? 2 : 1),
            onSignal: reading.format.onCodeValues ? 0 : 1)

        guard let encoder = commandBuffer.makeComputeCommandEncoder() else { throw PipelineError.texture }
        encoder.setComputePipelineState(gradePipeline)
        encoder.setTexture(luma, index: 0)
        encoder.setTexture(chroma, index: 1)
        encoder.setTexture(lut, index: 2)
        encoder.setTexture(target, index: 3)
        encoder.setBytes(&uniforms, length: MemoryLayout<GradeUniforms>.stride, index: 0)
        let w = gradePipeline.threadExecutionWidth
        let h = max(gradePipeline.maxTotalThreadsPerThreadgroup / w, 1)
        encoder.dispatchThreadgroups(MTLSize(width: (target.width + w - 1) / w, height: (target.height + h - 1) / h, depth: 1),
                                     threadsPerThreadgroup: MTLSize(width: w, height: h, depth: 1))
        encoder.endEncoding()
        // The planes have to outlive the GPU's use of them.
        commandBuffer.addCompletedHandler { _ in _ = (lumaRef, chromaRef) }
    }

    private func gradedTarget(width: Int, height: Int) -> MTLTexture? {
        if let t = gradedTexture, t.width == width, t.height == height { return t }
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba32Float, width: width,
                                                         height: height, mipmapped: false)
        d.usage = [.shaderRead, .shaderWrite]
        d.storageMode = .private
        gradedTexture = device.makeTexture(descriptor: d)
        return gradedTexture
    }

    /// The finished frame at `width` x `height`, in source orientation and
    /// display light: graded, then Glow's effects. Encoded on `commandBuffer`.
    func image(_ source: CVPixelBuffer, width: Int, height: Int, effects: GlowLook,
               settings: InputSettings, mode: Mode, commandBuffer: MTLCommandBuffer) throws -> CIImage {
        let reading = try Self.reading(of: source, settings: settings)
        guard let target = gradedTarget(width: width, height: height) else { throw PipelineError.texture }
        try encodeGrade(source, reading: reading, into: target, mode: mode, commandBuffer: commandBuffer)
        guard var image = CIImage(mtlTexture: target, options: [.colorSpace: Self.linearSRGB]) else {
            throw PipelineError.texture
        }
        if mode == .graded {
            // Sizes are fractions of the long edge, so the preview's smaller
            // frame blooms exactly as the full-size export does.
            image = GlowPipeline.apply(effects, to: image, longEdge: CGFloat(max(width, height)))
        }
        return image
    }

    // MARK: - Destinations

    /// Export: the full-size frame into `destination`, a 64RGBAHalf buffer,
    /// encoded for `display` and tagged Rec.709. Waits for the GPU.
    func renderExport(_ source: CVPixelBuffer, into destination: CVPixelBuffer, effects: GlowLook,
                      settings: InputSettings, display: LookLUT.Output) throws {
        guard let cb = queue.makeCommandBuffer() else { throw PipelineError.texture }
        let width = CVPixelBufferGetWidth(source), height = CVPixelBufferGetHeight(source)
        let image = try self.image(source, width: width, height: height, effects: effects,
                                   settings: settings, mode: .graded, commandBuffer: cb)
        guard let encoded = encodeKernel.apply(extent: image.extent,
                                               arguments: [image, Float(1 / display.gamma)]) else {
            throw PipelineError.texture
        }
        let (texture, ref) = try self.texture(for: destination, format: .rgba16Float)
        let target = CIRenderDestination(mtlTexture: texture, commandBuffer: cb)
        target.colorSpace = nil
        target.isFlipped = false
        _ = try ciContext.startTask(toRender: encoded, to: target)
        cb.commit()
        cb.waitUntilCompleted()
        _ = ref
        Self.tagRec709(destination)
    }

    static func tagRec709(_ buffer: CVPixelBuffer) {
        CVBufferSetAttachment(buffer, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, .shouldPropagate)
        CVBufferSetAttachment(buffer, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, .shouldPropagate)
        CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, .shouldPropagate)
    }

    /// Where a frame lands in a destination of `size`: fitted with `padding`
    /// all round, rotated by the track's quarter turns.
    struct Placement {
        var gradedWidth: Int
        var gradedHeight: Int
        var transform: CGAffineTransform
        var frame: CGRect

        init(source: CGSize, quarterTurns: Int, destination: CGSize, padding: CGFloat) {
            let turned = quarterTurns % 2 == 1 ? CGSize(width: source.height, height: source.width) : source
            let fit = max(min((destination.width - 2 * padding) / turned.width,
                              (destination.height - 2 * padding) / turned.height), 1e-3)
            // Never grade at more than the source's own resolution; scale up
            // after, if the window is bigger than the clip.
            let gradeScale = min(fit, 1)
            gradedWidth = max(Int((source.width * gradeScale).rounded()), 1)
            gradedHeight = max(Int((source.height * gradeScale).rounded()), 1)
            let shown = CGSize(width: turned.width * fit, height: turned.height * fit)
            frame = CGRect(x: ((destination.width - shown.width) / 2).rounded(),
                           y: ((destination.height - shown.height) / 2).rounded(),
                           width: shown.width, height: shown.height)
            // Core Image's y runs down the frame here (row 0 is y = 0), so a
            // positive angle turns the picture clockwise as seen.
            var t = CGAffineTransform(rotationAngle: CGFloat(quarterTurns) * .pi / 2)
            let rotated = CGRect(x: 0, y: 0, width: gradedWidth, height: gradedHeight).applying(t)
            t = t.concatenating(CGAffineTransform(translationX: -rotated.minX, y: -rotated.minY))
            let k = fit / gradeScale
            t = t.concatenating(CGAffineTransform(scaleX: k, y: k))
            t = t.concatenating(CGAffineTransform(translationX: frame.minX, y: frame.minY))
            transform = t
        }
    }

    /// Preview: the frame fitted into `texture` (a drawable, in extended linear
    /// sRGB) over `background`. Encodes onto `commandBuffer`; the caller
    /// presents and commits.
    func renderPreview(_ source: CVPixelBuffer, into texture: MTLTexture, quarterTurns: Int,
                       padding: CGFloat, background: CIColor, effects: GlowLook,
                       settings: InputSettings, mode: Mode, commandBuffer: MTLCommandBuffer) throws {
        let placement = Placement(source: CGSize(width: CVPixelBufferGetWidth(source),
                                                 height: CVPixelBufferGetHeight(source)),
                                  quarterTurns: quarterTurns,
                                  destination: CGSize(width: texture.width, height: texture.height),
                                  padding: padding)
        let image = try self.image(source, width: placement.gradedWidth, height: placement.gradedHeight,
                                   effects: effects, settings: settings, mode: mode,
                                   commandBuffer: commandBuffer)
        let canvas = CGRect(x: 0, y: 0, width: texture.width, height: texture.height)
        let composed = image.transformed(by: placement.transform)
            .composited(over: CIImage(color: background).cropped(to: canvas))
            .cropped(to: canvas)
        let target = CIRenderDestination(mtlTexture: texture, commandBuffer: commandBuffer)
        target.colorSpace = Self.linearSRGB
        target.isFlipped = false
        _ = try ciContext.startTask(toRender: composed, to: target)
    }

    /// Paint all of `texture` (a drawable) with `color`: the preview with no frame yet.
    func fill(_ texture: MTLTexture, with color: CIColor, commandBuffer: MTLCommandBuffer) throws {
        let target = CIRenderDestination(mtlTexture: texture, commandBuffer: commandBuffer)
        target.colorSpace = Self.linearSRGB
        let canvas = CGRect(x: 0, y: 0, width: texture.width, height: texture.height)
        _ = try ciContext.startTask(toRender: CIImage(color: color).cropped(to: canvas), to: target)
    }

    /// Display light at `width` x `height`, read back to the CPU: RGBA floats,
    /// row 0 at the top. For checks.
    func renderLinear(_ source: CVPixelBuffer, width: Int, height: Int, effects: GlowLook,
                      settings: InputSettings, mode: Mode) throws -> [Float] {
        guard let cb = queue.makeCommandBuffer() else { throw PipelineError.texture }
        let image = try self.image(source, width: width, height: height, effects: effects,
                                   settings: settings, mode: mode, commandBuffer: cb)
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba32Float, width: width,
                                                         height: height, mipmapped: false)
        d.usage = [.shaderRead, .shaderWrite]
        d.storageMode = .shared
        guard let out = device.makeTexture(descriptor: d) else { throw PipelineError.texture }
        let target = CIRenderDestination(mtlTexture: out, commandBuffer: cb)
        target.colorSpace = nil
        target.isFlipped = false
        _ = try ciContext.startTask(toRender: image, to: target)
        cb.commit()
        cb.waitUntilCompleted()
        var pixels = [Float](repeating: 0, count: width * height * 4)
        pixels.withUnsafeMutableBytes { raw in
            out.getBytes(raw.baseAddress!, bytesPerRow: width * 16,
                         from: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0)
        }
        return pixels
    }
}

private extension CVPixelBuffer {
    var width: Int { CVPixelBufferGetWidth(self) }
}
