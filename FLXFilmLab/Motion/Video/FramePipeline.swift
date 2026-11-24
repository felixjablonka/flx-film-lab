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

    /// Must match `GrainLightUniforms` in motion.metal.
    struct GrainLightUniforms {
        var tableTop: SIMD3<Float>
        var pixelArea: Float
        var sizeSquared: Float
        var amount: Float
        var samples: UInt32
        var kept0: SIMD3<Float>
        var kept1: SIMD3<Float>
        var kept2: SIMD3<Float>
        var monochrome: UInt32
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
    /// The grain's kernels, made when a clip first has grain: the engine's
    /// own three (flx.metal), `grain_density` and `grain_light`.
    private struct GrainKernels {
        var density, layer, convolve, convolveAdd, light: MTLComputePipelineState
    }
    private var grainKernels: GrainKernels?
    private let densityFunction: MTLFunction
    private let lightFunction: MTLFunction
    /// The frame as print density, two scratch fields (the first also takes
    /// the effects' light), the grainy density, at the last grained size.
    private var grainTextures: [MTLTexture] = []

    init(device: MTLDevice? = nil) throws {
        guard let device = device ?? MTLCreateSystemDefaultDevice(),
              let queue = device.makeCommandQueue() else { throw PipelineError.noDevice }
        self.device = device
        self.queue = queue

        let library: MTLLibrary
        do {
            library = try ShaderLibrary.make("motion", device: device,
                                             sourcePath: "FLXFilmLab/Motion/Resources/Shaders/motion.metal",
                                             options: nil) { try Self.shaderSource() }
        } catch {
            throw PipelineError.shader("\(error)")
        }
        guard let function = library.makeFunction(name: "grade_frame") else {
            throw PipelineError.shader("grade_frame not found")
        }
        gradePipeline = try device.makeComputePipelineState(function: function)
        guard let light = library.makeFunction(name: "grain_light"),
              let density = library.makeFunction(name: "grain_density") else {
            throw PipelineError.shader("grain_light or grain_density not found")
        }
        lightFunction = light
        densityFunction = density

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
    /// display light: graded, then Glow's effects, then the frame's grain.
    /// Encoded on `commandBuffer`.
    func image(_ source: CVPixelBuffer, width: Int, height: Int, effects: GlowLook, grain: FrameGrain? = nil,
               settings: InputSettings, mode: Mode, commandBuffer: MTLCommandBuffer) throws -> CIImage {
        let reading = try Self.reading(of: source, settings: settings)
        guard let target = gradedTarget(width: width, height: height) else { throw PipelineError.texture }
        if mode == .graded, let grain, grain.settings.isVisible {
            return try grainedImage(source, reading: reading, target: target, effects: effects, grain: grain,
                                    commandBuffer: commandBuffer)
        }
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

    /// The graded frame with Glow's effects and then its grain. The grain is
    /// drawn in Metal on a texture of the finished light, and a Core Image
    /// render must not come before Metal passes in one command buffer: at 4K
    /// its work ran after them, and every exported frame came out black (the
    /// smaller preview, and frames rendered again, hid it). So with no effect
    /// on, Core Image is not used at all; with one, the grade is committed on
    /// its own, Core Image renders the effects on a command buffer of its own,
    /// and the grain is drawn once that is done.
    private func grainedImage(_ source: CVPixelBuffer, reading: Reading, target: MTLTexture, effects: GlowLook,
                              grain: FrameGrain, commandBuffer: MTLCommandBuffer) throws -> CIImage {
        let width = target.width, height = target.height
        let textures = try grainTargets(width: width, height: height)
        var lit = target
        if effects.bloom.isActive || effects.glow.isActive || effects.halation.isActive {
            guard let first = queue.makeCommandBuffer() else { throw PipelineError.texture }
            try encodeGrade(source, reading: reading, into: target, mode: .graded, commandBuffer: first)
            first.commit()
            guard let graded = CIImage(mtlTexture: target, options: [.colorSpace: Self.linearSRGB]) else {
                throw PipelineError.texture
            }
            let glowed = GlowPipeline.apply(effects, to: graded, longEdge: CGFloat(max(width, height)))
            let destination = CIRenderDestination(mtlTexture: textures[1], commandBuffer: nil)
            destination.colorSpace = nil
            destination.isFlipped = false
            _ = try ciContext.startTask(toRender: glowed, to: destination).waitUntilCompleted()
            lit = textures[1]
        } else {
            try encodeGrade(source, reading: reading, into: target, mode: .graded, commandBuffer: commandBuffer)
        }
        let out = try grained(lit, grain: grain, textures: textures, commandBuffer: commandBuffer)
        guard let image = CIImage(mtlTexture: out, options: [.colorSpace: Self.linearSRGB]) else {
            throw PipelineError.texture
        }
        return image
    }

    // MARK: - Grain

    /// `image` with the frame's grain, laid on after the look and the effects,
    /// as the last thing the film adds. The frame is read as the print's
    /// density, and each record's grain drawn as Stills draws the negative's
    /// (`Renderer.drawGrain`): Gaussian, its variance the negative's Selwyn
    /// coefficient over the pixel's area on the gauge's frame, then its dye
    /// clouds; `grain_light` mixes the records as the print does and returns
    /// to light. A smaller frame has larger pixels on the film, so the
    /// preview shows at its size the grain the export shows at full size.
    private func grained(_ lit: MTLTexture, grain: FrameGrain, textures t: [MTLTexture],
                         commandBuffer: MTLCommandBuffer) throws -> MTLTexture {
        let k = try madeGrainKernels()
        let width = lit.width, height = lit.height
        let clean = t[0], scratch1 = t[1], scratch2 = t[2], grainy = t[3]
        encode(commandBuffer, k.density, width: width, height: height) { e in
            e.setTexture(lit, index: 0)
            e.setTexture(clean, index: 1)
        }

        let table = grain.table
        let pitch = Renderer.pixelSizeUm(width: width, height: height, frame: grain.settings.gauge.frameMM)
        let size = Float(grain.settings.size), amount = Float(grain.settings.amount)
        var selwyn = table.selwynUm2
        var kept: [SIMD3<Float>] = []
        for layer in 0..<3 {
            var uniforms = Renderer.GrainLayerUniforms(tableTop: table.tableDensityMax, pixelArea: pitch * pitch,
                                                       sizeSquared: size * size, amount: amount, seed: grain.seed,
                                                       monochrome: table.monochrome ? 1 : 0, layer: UInt32(layer),
                                                       samples: UInt32(table.samples))
            encode(commandBuffer, k.layer, width: width, height: height) { e in
                e.setTexture(clean, index: 0)
                e.setTexture(scratch1, index: 1)
                e.setBytes(&uniforms, length: MemoryLayout<Renderer.GrainLayerUniforms>.stride, index: 0)
                e.setBytes(&selwyn, length: MemoryLayout<Float>.stride * selwyn.count, index: 1)
            }
            // each record's clouds for this layer, in pixels, padded to one radius
            let kernels = (0..<3).map {
                Renderer.cloudFilter(sigma: Double(table.cloudUm[$0 * 3 + layer] * size / pitch))
            }
            kept.append(SIMD3((0..<3).map { c -> Float in
                let squares = kernels[c].reduce(0) { $0 + $1 * $1 }
                return Float(squares * squares)
            }))
            let radius = kernels.map { $0.count / 2 }.max() ?? 0
            var taps = (0...(2 * radius)).map { i in
                SIMD4<Float>((0..<3).map { c -> Float in
                    let kernel = kernels[c], j = i - radius + kernel.count / 2
                    return j >= 0 && j < kernel.count ? Float(kernel[j]) : 0
                } + [0])
            }
            var first = SIMD2<Int32>(Int32(radius), 0)
            encode(commandBuffer, k.convolve, width: width, height: height) { e in
                e.setTexture(scratch1, index: 0)
                e.setTexture(scratch2, index: 1)
                e.setBytes(&taps, length: MemoryLayout<SIMD4<Float>>.stride * taps.count, index: 0)
                e.setBytes(&first, length: MemoryLayout<SIMD2<Int32>>.stride, index: 1)
            }
            var second = SIMD3<Int32>(Int32(radius), 1, layer == 0 ? 1 : 0)
            encode(commandBuffer, k.convolveAdd, width: width, height: height) { e in
                e.setTexture(scratch2, index: 0)
                e.setTexture(clean, index: 1)
                e.setTexture(grainy, index: 2)
                e.setBytes(&taps, length: MemoryLayout<SIMD4<Float>>.stride * taps.count, index: 0)
                e.setBytes(&second, length: MemoryLayout<SIMD3<Int32>>.stride, index: 1)
            }
        }
        var light = GrainLightUniforms(tableTop: table.tableDensityMax, pixelArea: pitch * pitch,
                                       sizeSquared: size * size, amount: amount, samples: UInt32(table.samples),
                                       kept0: kept[0], kept1: kept[1], kept2: kept[2],
                                       monochrome: table.monochrome ? 1 : 0)
        var response = table.response
        encode(commandBuffer, k.light, width: width, height: height) { e in
            e.setTexture(grainy, index: 0)
            e.setTexture(clean, index: 1)
            e.setTexture(scratch1, index: 2)
            e.setBytes(&light, length: MemoryLayout<GrainLightUniforms>.stride, index: 0)
            e.setBytes(&selwyn, length: MemoryLayout<Float>.stride * selwyn.count, index: 1)
            e.setBytes(&response, length: MemoryLayout<Float>.stride * response.count, index: 2)
        }
        return scratch1
    }

    private func madeGrainKernels() throws -> GrainKernels {
        if let grainKernels { return grainKernels }
        let library: MTLLibrary
        do {
            let options = MTLCompileOptions()
            options.mathMode = .fast
            library = try ShaderLibrary.make("flx", device: device, sourcePath: "FLXFilmLab/Resources/Shaders/flx.metal",
                                             options: options) { try PackLibrary.shared.shaderSource() }
        } catch {
            throw PipelineError.shader("\(error)")
        }
        func pipeline(_ name: String) throws -> MTLComputePipelineState {
            guard let function = library.makeFunction(name: name) else { throw PipelineError.shader("\(name) not found") }
            return try device.makeComputePipelineState(function: function)
        }
        let made = GrainKernels(density: try device.makeComputePipelineState(function: densityFunction),
                                layer: try pipeline("film_grain_layer"), convolve: try pipeline("grain_convolve"),
                                convolveAdd: try pipeline("grain_convolve_add"),
                                light: try device.makeComputePipelineState(function: lightFunction))
        grainKernels = made
        return made
    }

    /// The density, two scratch fields, and the grainy density, which the
    /// three layers are added into in full precision: a half-float texture's
    /// writes round towards zero on this GPU, half a step at each of the three
    /// adds, which darkened the grainy negative by 0.0007 density, brightening
    /// every pixel by 0.16% (half floats step 0.0005 at a density of 0.77).
    private func grainTargets(width: Int, height: Int) throws -> [MTLTexture] {
        if let first = grainTextures.first, first.width == width, first.height == height { return grainTextures }
        var made: [MTLTexture] = []
        for format in [MTLPixelFormat.rgba16Float, .rgba16Float, .rgba16Float, .rgba32Float] {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: width,
                                                             height: height, mipmapped: false)
            // a render target too: Core Image renders the effects' light into the second
            d.usage = [.shaderRead, .shaderWrite, .renderTarget]
            d.storageMode = .private
            if let t = device.makeTexture(descriptor: d) { made.append(t) }
        }
        guard made.count == 4 else { throw PipelineError.texture }
        grainTextures = made
        return made
    }

    private func encode(_ cb: MTLCommandBuffer, _ pipeline: MTLComputePipelineState, width: Int, height: Int,
                        _ body: (MTLComputeCommandEncoder) -> Void) {
        guard let e = cb.makeComputeCommandEncoder() else { return }
        e.setComputePipelineState(pipeline)
        body(e)
        let w = pipeline.threadExecutionWidth
        let h = max(pipeline.maxTotalThreadsPerThreadgroup / w, 1)
        e.dispatchThreadgroups(MTLSize(width: (width + w - 1) / w, height: (height + h - 1) / h, depth: 1),
                               threadsPerThreadgroup: MTLSize(width: w, height: h, depth: 1))
        e.endEncoding()
    }

    // MARK: - Destinations

    /// Export: the full-size frame into `destination`, a 64RGBAHalf buffer,
    /// encoded for `display` and tagged Rec.709. Waits for the GPU.
    func renderExport(_ source: CVPixelBuffer, into destination: CVPixelBuffer, effects: GlowLook,
                      grain: FrameGrain? = nil, settings: InputSettings, display: LookLUT.Output) throws {
        guard let cb = queue.makeCommandBuffer() else { throw PipelineError.texture }
        let width = CVPixelBufferGetWidth(source), height = CVPixelBufferGetHeight(source)
        let image = try self.image(source, width: width, height: height, effects: effects, grain: grain,
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
                       padding: CGFloat, background: CIColor, effects: GlowLook, grain: FrameGrain? = nil,
                       settings: InputSettings, mode: Mode, commandBuffer: MTLCommandBuffer) throws {
        let placement = Placement(source: CGSize(width: CVPixelBufferGetWidth(source),
                                                 height: CVPixelBufferGetHeight(source)),
                                  quarterTurns: quarterTurns,
                                  destination: CGSize(width: texture.width, height: texture.height),
                                  padding: padding)
        let image = try self.image(source, width: placement.gradedWidth, height: placement.gradedHeight,
                                   effects: effects, grain: grain, settings: settings, mode: mode,
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

    /// The graded frame at `width` x `height` as an 8-bit sRGB picture, turned
    /// `quarterTurns` clockwise as the track is played: a sidebar's thumbnail.
    func thumbnail(_ source: CVPixelBuffer, width: Int, height: Int, effects: GlowLook,
                   settings: InputSettings, quarterTurns: Int) throws -> CGImage? {
        guard let cb = queue.makeCommandBuffer() else { throw PipelineError.texture }
        var image = try self.image(source, width: width, height: height, effects: effects,
                                   settings: settings, mode: .graded, commandBuffer: cb)
        cb.commit()
        cb.waitUntilCompleted()
        // Row 0 is y = 0 here, and a picture's top row is its highest y in
        // Core Image's own frame: flipped first, then turned as an EXIF
        // orientation turns a photo upright.
        image = image.oriented(.downMirrored)
        let turns: [CGImagePropertyOrientation] = [.up, .right, .down, .left]
        image = image.oriented(turns[((quarterTurns % 4) + 4) % 4])
        return ciContext.createCGImage(image, from: image.extent, format: .RGBA8,
                                       colorSpace: CGColorSpace(name: CGColorSpace.sRGB))
    }

    /// Display light at `width` x `height`, read back to the CPU: RGBA floats,
    /// row 0 at the top. For checks.
    func renderLinear(_ source: CVPixelBuffer, width: Int, height: Int, effects: GlowLook,
                      grain: FrameGrain? = nil, settings: InputSettings, mode: Mode) throws -> [Float] {
        guard let cb = queue.makeCommandBuffer() else { throw PipelineError.texture }
        let image = try self.image(source, width: width, height: height, effects: effects, grain: grain,
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
