import Foundation
import Metal
import MetalPerformanceShaders
import CoreGraphics
import CoreImage
import simd

// Uniform layouts. These must stay byte-identical to the structs in flx.metal;
// Metal aligns float3 to 16 bytes, so the padding below is load-bearing.

struct FilmUniforms {
    var rgbToXYZ: simd_float3x3
    var exposureScale: Float
    var tcLutSize: Int32
    var intoTriangle: Int32 = 0
    var gradientCount: Int32 = 0
}

/// One exposure gradient as `film_expose` reads it: its handles in pixels of
/// the frame being rendered (a linear gradient's two, an oval's centre and
/// axis ends), the compensation in EV, and an oval's feather and side.
/// `ExposureGradientU` in flx.metal.
struct ExposureGradientUniform {
    var a: SIMD2<Float> = .zero
    var b: SIMD2<Float> = .zero
    var c: SIMD2<Float> = .zero
    var ev: Float = 0
    var kind: Int32 = 0
    var feather: Float = 0
    var inverted: Int32 = 0
}

struct DevelopUniforms {
    var logE0: Float
    var logEStep: Float
    var pad0: Float = 0
    var pad1: Float = 0
    var gammaFactor: SIMD3<Float>
    var densityMax: SIMD3<Float>
    var couplersAmount: Float
    var isPositive: Int32
    var edgeCap: Float = 0     // 1: hold the edge effect (`edgeScale`)
    var diffusionFraction: Float = 1   // share of the inhibitor that spreads
}

struct PrintUniforms {
    var xyzToRGB: simd_float3x3
    var exposureFactorMidgray: SIMD3<Float>
    var preflashRaw: SIMD3<Float>
    var printExposure: Float
    var cmfNormalization: Float
    var toneLogE0: Float
    var toneLogEStep: Float
    var toneLutWidth: Int32
    var gradeSpanMin: Float
    var gradeSpanMax: Float
    var hasDodgeBurn: Int32
    var glareAmount: Float
    var pad0: Float = 0
    var pad1: Float = 0
    var pad2: Float = 0
    var illuminantXYZ: SIMD3<Float>
    var gamutCompress: Int32
    var gamutThreshold: Float
    var gamutLimit: Float
    var gamutPower: Float
    var scanFilm: Int32
    var pad3: Float = 0
    var pad4: Float = 0
    var pad5: Float = 0
    var gamutRGBToLMS: simd_float3x3
    var gamutLMSToRGB: simd_float3x3
    var toneShiftMax: Float = 0
    var toneShiftCount: Int32 = 0
    var pad6: Float = 0
    var pad7: Float = 0
}

/// A `GeometryMap` as the kernels read it: a print pixel's position to the
/// frame's. `GeometryUniforms` in flx.metal; the default is the frame as it is.
struct GeometryUniforms {
    var column0 = SIMD2<Float>(1, 0)
    var column1 = SIMD2<Float>(0, 1)
    var offset = SIMD2<Float>.zero
    var sourceSize = SIMD2<Float>.zero
    var active: Int32 = 0
    var pad: Int32 = 0

    init() {}

    init(_ map: GeometryMap) {
        let t = map.outputToSource
        column0 = SIMD2(Float(t.a), Float(t.b))
        column1 = SIMD2(Float(t.c), Float(t.d))
        offset = SIMD2(Float(t.tx), Float(t.ty))
        sourceSize = SIMD2(Float(map.sourceSize.width), Float(map.sourceSize.height))
        active = map.isIdentity ? 0 : 1
    }

    /// The map from the pixels of a part of the print that starts at `part`
    /// (in the print's pixels) to those of an image that starts at `held`
    /// in the frame: the loupe's part and box (`Renderer.setWindow`).
    init(_ map: GeometryMap, part: CGPoint, held: SIMD2<Float> = .zero) {
        self.init(map)
        offset += column0 * Float(part.x) + column1 * Float(part.y) - held
    }
}

/// Everything the film stage reads. Equal keys mean the developed negative
/// from the last render can be reused as it is.
struct FilmStageKey: Equatable {
    var film: String
    var lensFilter: String?
    var exposureEV: Float
    var exposureGradients: [ExposureGradient]
    var densityCurveGamma: Double
    var couplers: [Double]
    var halation: [Double]
    var grain: [Double]
    var cameraSharpness: Bool
    var filmFormat: FilmFormat
    /// An instant print's processing temperature, which picks its curves.
    var processingTemperature: Double?
    /// The grain is drawn on the frame, unless the print is shaped: then it
    /// is drawn on the print's own pixels (`Renderer.render`, step 2d).
    var grainOnFrame: Bool

    init(setup: RenderSetup, recipe: Recipe, grainOnFrame: Bool = true) {
        film = setup.film.stock
        lensFilter = setup.lensFilter?.id
        exposureEV = setup.exposureEV
        exposureGradients = setup.exposureGradients
        densityCurveGamma = recipe.densityCurveGamma
        couplers = [recipe.couplersEnabled ? 1 : 0, recipe.couplersAmount, recipe.couplersDiffusion]
        halation = [recipe.halationEnabled ? 1 : 0, recipe.halationAmount, recipe.halationScale,
                    recipe.scatterAmount, recipe.scatterScale]
        grain = [recipe.grainEnabled ? 1 : 0, recipe.grainAmount, recipe.grainScale]
        cameraSharpness = recipe.keepsCameraSharpness
        filmFormat = recipe.filmFormat
        processingTemperature = setup.film.processing == nil ? nil : recipe.processingTemperature
        self.grainOnFrame = grainOnFrame
    }
}

/// Everything the render depends on that is *not* a per-pixel uniform:
/// resolved profiles, the spectral tables, and the tone LUT. Recomputed on the
/// CPU whenever the recipe changes, which costs well under a millisecond.
struct RenderSetup {
    var film: ProfilePack
    var paper: ProfilePack
    var toneGeometry: ToneCurve.Geometry
    var printUniforms: PrintUniforms
    var developUniforms: DevelopUniforms
    var couplerMatrix: simd_float3x3
    /// On the coupler pass's finer grid, `Constants.couplerCurveSamples` long.
    var curvesBeforeCouplers: [Float]
    /// `FilmMath.edgeGainScale` on the same grid, or empty where the stock's
    /// edge effect is agx's.
    var edgeScale: [Float] = []
    var normalizedCurves: [Float]
    var spectralTables: [Float]
    var toneLUT: [Float]
    var neutralFilters: SIMD3<Float>
    /// What the negative is exposed at, metered value and trim included. The
    /// film stage reads it from here rather than from the recipe, whose slider
    /// is only the trim when Auto Exposure is on.
    var exposureEV: Float
    /// The camera filter this black-and-white film is shot through, if any.
    var lensFilter: LensFilterRef? = nil
    /// The graduated exposure masks that do anything.
    var exposureGradients: [ExposureGradient] = []
    /// Brightness / Shadows / Highlights as a neutral density shift by mean
    /// density (`ToneCurve.NeutralShift`); empty when they are all at zero.
    var toneShift: [Float] = []
}

enum RendererError: Error, LocalizedError {
    case noDevice
    case shaderCompilation(String)
    case missingFunction(String)
    case textureAllocation

    var errorDescription: String? {
        switch self {
        case .noDevice: return "No Metal device available."
        case .shaderCompilation(let m): return "Metal shader compilation failed:\n\(m)"
        case .missingFunction(let n): return "Shader function '\(n)' not found."
        case .textureAllocation: return "Could not allocate a render texture."
        }
    }
}

final class Renderer {
    let device: MTLDevice
    private let queue: MTLCommandQueue
    private let filmExpose: MTLComputePipelineState
    private let filmLog: MTLComputePipelineState
    private let scatterMix: MTLComputePipelineState
    private let halationAdd: MTLComputePipelineState
    private let accumulateScaled: MTLComputePipelineState
    private let filmGrainLayer: MTLComputePipelineState
    private let filmDevelop: MTLComputePipelineState
    private let couplersAmount: MTLComputePipelineState
    private let filmDevelopCouplers: MTLComputePipelineState
    private let printAndScan: MTLComputePipelineState
    private let grainRestoreMean: MTLComputePipelineState
    private let grainMirror: MTLComputePipelineState
    private let grainEvenMean: MTLComputePipelineState
    private let grainConvolve: MTLComputePipelineState
    private let grainConvolveAdd: MTLComputePipelineState
    private let encodeDisplay: MTLComputePipelineState
    private let applyGeometry: MTLComputePipelineState
    private let applyCurves: MTLComputePipelineState
    private let histogramRGB: MTLComputePipelineState

    // Per-image textures
    private var sourceTexture: MTLTexture?
    private var rawLinear: MTLTexture?
    private var rawLinearB: MTLTexture?
    private var scratchA: MTLTexture?
    private var scratchB: MTLTexture?
    private var logEFilm: MTLTexture?
    private var cmyFilm: MTLTexture?
    private var cmyFilmB: MTLTexture?
    private var correction: MTLTexture?
    private var correctionBlurred: MTLTexture?
    private var linearOut: MTLTexture?
    private var displayOut: MTLTexture?
    private var dodgeBurnTexture: MTLTexture?
    private var dummyDodgeBurn: MTLTexture?
    /// Whichever of the two CMY buffers holds the developed negative after the
    /// optional coupler pass — the `cmy_film` tap.
    private var developedCmy: MTLTexture?
    /// The same negative before grain, when grain is on.
    private var developedGrainFree: MTLTexture?
    /// What the developed negative in `developedCmy` was made from.
    private var filmStageKey: FilmStageKey?

    /// This picture's own grain. Each frame of film has its own grains, so
    /// the app seeds the grain from the picture's file (`grainSeed(for:)`),
    /// and the film stock adds its own: two stocks never draw the same grain,
    /// and the same file on the same stock always does. 0 for a picture that
    /// has no file.
    var grainSeed: UInt32 = 0 {
        didSet { if grainSeed != oldValue { filmStageKey = nil } }
    }

    /// A picture's grain seed: its file's name, so the export and every later
    /// session draw the same grain for it.
    static func grainSeed(for url: URL) -> UInt32 { fnv1a(url.lastPathComponent) }

    private static func fnv1a(_ text: String) -> UInt32 {
        var h: UInt32 = 0x811C_9DC5
        for byte in text.utf8 { h = (h ^ UInt32(byte)) &* 0x0100_0193 }
        return h
    }
    private var displayStaging: MTLTexture?
    /// A shaped print's own textures, at its own size: the negative turned,
    /// levelled and cropped, the same with its grain, two scratch textures,
    /// the print, and its copy for the screen.
    private var shapedNegative: MTLTexture?
    private var shapedGrainy: MTLTexture?
    private var shapedScratchA: MTLTexture?
    private var shapedScratchB: MTLTexture?
    private var geometryLinear: MTLTexture?
    private var geometryDisplay: MTLTexture?
    /// What the last render finished with: the print as cut, linear and for
    /// the screen, and whether the screen's copy has empty corners.
    private var finishedLinear: MTLTexture?
    private var finishedDisplay: MTLTexture?
    private var displayHasAlpha = false
    /// The last render's size: the frame's, or its crop's.
    private(set) var outputSize: CGSize = .zero

    // Cached uploads
    private var tcLutTexture: MTLTexture?
    private var tcLutStock: String?
    private var toneLutTexture: MTLTexture?

    /// The curves' table on the GPU (`PrintCurves.table`), and the curves
    /// it was sampled from.
    private var curveBuffer: MTLBuffer?
    private var curveBufferCurves: PrintCurves?

    /// Measure the finished print's histogram in each render: the preview's
    /// renderer does, an export has no use for it.
    var measuresHistogram = false
    /// The last render's histogram (`histogram_rgb`), 256 bins of red, then
    /// green, then blue: `after` of the print as finished, `before` of it as
    /// the curves were given it, when there are any.
    private(set) var histogram: (after: [UInt32], before: [UInt32]?)?
    private var histogramBuffers: [MTLBuffer] = []

    /// The frame's size: the image's own, or, while a box of a frame is
    /// held for the loupe (`setWindow`), the whole frame's, which sets the
    /// pitch and every size in fractions of the frame.
    private(set) var imageSize: CGSize = .zero
    /// Where the image held starts in its frame: (0, 0), or the loupe's box.
    /// The frame's pixels, not the box's, seed the grain and place the
    /// gradients and the masks.
    private var windowOrigin = SIMD2<Int>(0, 0)
    /// A frame held whole at full size, on the GPU, for the loupe to take
    /// boxes of (`holdFrame`, `setWindow`).
    private var heldFrame: MTLTexture?
    /// What the last render made of the print, in the print's pixels: all of
    /// it, or the loupe's part.
    private(set) var madeRect: CGRect = .zero
    /// Keep a copy of the print as it was before Bloom & Glow
    /// (`printBeforeEffects`): the loupe's effects take the light from
    /// beyond its part from the preview's.
    var keepsPrintBeforeEffects = false
    private(set) var printBeforeEffects: MTLTexture?
    private var effectsContext: CIContext?
    /// Allocated the first time an image is rendered with effects on.
    private var effectsOut: MTLTexture?
    private var intermediateFormat: MTLPixelFormat = .rgba32Float

    init() throws {
        guard let device = MTLCreateSystemDefaultDevice() else { throw RendererError.noDevice }
        self.device = device
        guard let queue = device.makeCommandQueue() else { throw RendererError.noDevice }
        self.queue = queue

        let source = try PackLibrary.shared.shaderSource()
        let library: MTLLibrary
        do {
            let options = MTLCompileOptions()
            options.mathMode = .fast
            library = try device.makeLibrary(source: source, options: options)
        } catch {
            throw RendererError.shaderCompilation("\(error)")
        }

        func pipeline(_ name: String) throws -> MTLComputePipelineState {
            guard let fn = library.makeFunction(name: name) else { throw RendererError.missingFunction(name) }
            return try device.makeComputePipelineState(function: fn)
        }
        filmExpose = try pipeline("film_expose")
        filmLog = try pipeline("film_log")
        scatterMix = try pipeline("scatter_mix")
        halationAdd = try pipeline("halation_add")
        accumulateScaled = try pipeline("accumulate_scaled")
        filmGrainLayer = try pipeline("film_grain_layer")
        filmDevelop = try pipeline("film_develop")
        couplersAmount = try pipeline("couplers_amount")
        filmDevelopCouplers = try pipeline("film_develop_couplers")
        printAndScan = try pipeline("print_and_scan")
        grainRestoreMean = try pipeline("grain_restore_mean")
        grainMirror = try pipeline("grain_mirror")
        grainEvenMean = try pipeline("grain_even_mean")
        grainConvolve = try pipeline("grain_convolve")
        grainConvolveAdd = try pipeline("grain_convolve_add")
        encodeDisplay = try pipeline("encode_display")
        applyGeometry = try pipeline("apply_geometry")
        applyCurves = try pipeline("apply_curves")
        histogramRGB = try pipeline("histogram_rgb")

        dummyDodgeBurn = makeTexture(width: 1, height: 1, format: .rg32Float, write: false)
    }

    // MARK: - Image lifecycle

    func setImage(_ pixels: [Float], width: Int, height: Int) {
        imageSize = CGSize(width: width, height: height)
        windowOrigin = SIMD2(0, 0)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba32Float, width: width, height: height, mipmapped: false)
        descriptor.usage = [.shaderRead, .shaderWrite]
        descriptor.storageMode = .private
        sourceTexture = device.makeTexture(descriptor: descriptor)

        // Upload through a shared staging texture; the working copy is private
        // so the GPU reads it at full bandwidth.
        let staging = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba32Float, width: width, height: height, mipmapped: false)
        staging.usage = [.shaderRead]
        staging.storageMode = .shared
        guard let stagingTexture = device.makeTexture(descriptor: staging),
              let target = sourceTexture else { return }
        pixels.withUnsafeBytes { raw in
            stagingTexture.replace(region: MTLRegionMake2D(0, 0, width, height),
                                   mipmapLevel: 0,
                                   withBytes: raw.baseAddress!,
                                   bytesPerRow: width * 16)
        }
        if let cb = queue.makeCommandBuffer(), let blit = cb.makeBlitCommandEncoder() {
            blit.copy(from: stagingTexture, sourceSlice: 0, sourceLevel: 0,
                      sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                      sourceSize: MTLSize(width: width, height: height, depth: 1),
                      to: target, destinationSlice: 0, destinationLevel: 0,
                      destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
            blit.endEncoding()
            cb.commit()
            cb.waitUntilCompleted()
        }

        // Full-resolution work needs ten image-sized buffers. At 45 MP that is
        // 7 GB in float32, so past a threshold the intermediates drop to half
        // precision — plenty for linear light and density, and the difference
        // is far below the grain. The proxy and the parity harness stay in
        // float32.
        intermediateFormat = (width * height > 12_000_000) ? .rgba16Float : .rgba32Float
        allocateIntermediates(width: width, height: height)
        // The masks were drawn for the image before; a box of the frame
        // (`setWindow`) keeps the frame's.
        dodgeBurnTexture = nil
    }

    /// The film stage's image-sized textures, at `width` x `height`, in
    /// `intermediateFormat`; the print's are made on first use.
    private func allocateIntermediates(width: Int, height: Int) {
        let format = intermediateFormat
        rawLinear = makeTexture(width: width, height: height, format: format)
        rawLinearB = makeTexture(width: width, height: height, format: format)
        scratchA = makeTexture(width: width, height: height, format: format)
        scratchB = makeTexture(width: width, height: height, format: format)
        logEFilm = makeTexture(width: width, height: height, format: format)
        cmyFilm = makeTexture(width: width, height: height, format: format)
        cmyFilmB = makeTexture(width: width, height: height, format: format)
        correction = makeTexture(width: width, height: height, format: format)
        correctionBlurred = makeTexture(width: width, height: height, format: format)
        // The print at the frame's size is made on first use: a shaped print
        // is made in textures of its own size instead.
        linearOut = nil
        displayOut = nil
        effectsOut = nil
        filmStageKey = nil
        developedCmy = nil
        developedGrainFree = nil
        displayStaging = nil
        shapedNegative = nil
        shapedGrainy = nil
        shapedScratchA = nil
        shapedScratchB = nil
        geometryLinear = nil
        geometryDisplay = nil
        finishedLinear = nil
        finishedDisplay = nil
        outputSize = .zero
    }

    private func makeTexture(width: Int, height: Int, format: MTLPixelFormat, write: Bool = true) -> MTLTexture? {
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: max(width, 1), height: max(height, 1), mipmapped: false)
        d.usage = write ? [.shaderRead, .shaderWrite] : [.shaderRead]
        d.storageMode = .private
        return device.makeTexture(descriptor: d)
    }

    // MARK: - Render

    /// Render the print `recipe` makes, in the shape it gives the print.
    /// `wholeFrame` keeps the whole frame, turned and levelled, for drawing
    /// the crop on.
    @discardableResult
    func render(setup: RenderSetup, recipe: Recipe, wholeFrame: Bool = false, part: Part? = nil) throws -> MTLTexture? {
        guard let source = sourceTexture,
              let raw = rawLinear,
              let rawB = rawLinearB,
              let scratch1 = scratchA,
              let scratch2 = scratchB,
              let logE = logEFilm,
              var cmy = cmyFilm,
              let cmyAlt = cmyFilmB,
              let corr = correction,
              let corrBlurred = correctionBlurred else { return nil }

        try ensureTcLut(for: setup.film, lensFilter: setup.lensFilter)
        uploadToneLUT(setup.toneLUT)
        uploadTables(setup)
        guard let tcLut = tcLutTexture, let toneLut = toneLutTexture,
              let spectralBuffer = spectralBuffer,
              let curvesBuffer = curvesBuffer,
              let curves0Buffer = curves0Buffer,
              let toneShiftBuffer = toneShiftBuffer else { return nil }

        let width = source.width, height = source.height
        guard let cb = queue.makeCommandBuffer() else { return nil }

        // Turned, levelled or cropped, the print is made at its own size from
        // the negative given its shape (step 2d). The shape is the frame's;
        // for the loupe, the image held is a box of it (`setWindow`).
        let frameWidth = Int(imageSize.width), frameHeight = Int(imageSize.height)
        let map = GeometryMap(recipe.geometry, sourceWidth: frameWidth, sourceHeight: frameHeight,
                              wholeFrame: wholeFrame)
        let shaped = !map.isIdentity
        let origin = SIMD2<Float>(Float(windowOrigin.x), Float(windowOrigin.y))
        // What is made of the print: all of it, or the loupe's part, which,
        // unshaped, is the box held.
        let made: CGRect
        if let part {
            made = shaped ? part.rect
                : CGRect(x: windowOrigin.x, y: windowOrigin.y, width: width, height: height)
        } else {
            made = CGRect(x: 0, y: 0, width: map.outputWidth, height: map.outputHeight)
        }

        // The film stage — exposure, halation, development, couplers, grain —
        // depends on none of the print controls. When only those moved, the
        // developed negative from the last render is reused: the print stage
        // alone is about a quarter of the work.
        let filmKey = FilmStageKey(setup: setup, recipe: recipe, grainOnFrame: !shaped)
        var grainFree: MTLTexture?
        if filmKey == filmStageKey, let developed = developedCmy {
            cmy = developed
            grainFree = developedGrainFree
        } else {
            // 1 — filming.expose
            // The exposure gradients change what the film receives, so they act
            // on the exposure itself, before halation spreads it in the film.
            var gradients = setup.exposureGradients.map { g in
                let pixel = { (p: CGPoint) in
                    SIMD2(Float(p.x) * Float(frameWidth), Float(p.y) * Float(frameHeight)) - origin
                }
                return ExposureGradientUniform(
                    a: pixel(g.points[0]), b: pixel(g.points[1]),
                    c: g.isOval ? pixel(g.points[2]) : .zero,
                    ev: Float(g.exposureEV),
                    kind: g.isOval ? 1 : 0,
                    feather: Float(g.featherAmount),
                    inverted: g.isInverted ? 1 : 0)
            }
            var filmUniforms = FilmUniforms(rgbToXYZ: setup.film.rgbToXYZMatrix,
                                            exposureScale: pow(2, setup.exposureEV),
                                            tcLutSize: Int32(setup.film.tcLut?.size ?? 192),
                                            intoTriangle: setup.film.isMonochrome ? 1 : 0,
                                            gradientCount: Int32(gradients.count))
            if gradients.isEmpty { gradients = [ExposureGradientUniform()] }   // setBytes needs a length
            encode(cb, filmExpose, width: width, height: height) { e in
                e.setTexture(source, index: 0)
                e.setTexture(raw, index: 1)
                e.setTexture(tcLut, index: 2)
                e.setBytes(&filmUniforms, length: MemoryLayout<FilmUniforms>.stride, index: 0)
                e.setBytes(&gradients, length: MemoryLayout<ExposureGradientUniform>.stride * gradients.count,
                           index: 1)
            }

            // 1b — scatter and halation, on linear irradiance before the curve
            var exposed = raw
            if recipe.halationEnabled, let halation = setup.film.defaults.halation, halation.active {
                exposed = encodeHalation(cb, source: raw, destination: rawB,
                                         scratch1: scratch1, scratch2: scratch2,
                                         halation: halation, recipe: recipe,
                                         frame: setup.film.frame(recipe.filmFormat),
                                         width: width, height: height)
            }

            encode(cb, filmLog, width: width, height: height) { e in
                e.setTexture(exposed, index: 0)
                e.setTexture(logE, index: 1)
            }

            // 2 — filming.develop, against the stock's min-subtracted curves
            var developUniforms = setup.developUniforms
            encode(cb, filmDevelop, width: width, height: height) { e in
                e.setTexture(logE, index: 0)
                e.setTexture(cmy, index: 1)
                e.setBuffer(curvesBuffer, offset: 0, index: 0)
                e.setBytes(&developUniforms, length: MemoryLayout<DevelopUniforms>.stride, index: 1)
            }

            // 2b — DIR couplers: measure the inhibitor released, optionally let it
            //      diffuse, then re-develop against the pre-coupler curves.
            //      A black-and-white stock has no couplers; its pack runs
            //      development's adjacency effect here instead, as the one
            //      layer's own inhibition (tools/bake/bw_mtf.py).
            if recipe.couplersEnabled && recipe.couplersAmount > 0 && setup.film.defaults.dirCouplers != nil {
                var matrix = setup.couplerMatrix
                encode(cb, couplersAmount, width: width, height: height) { e in
                    e.setTexture(cmy, index: 0)
                    e.setTexture(corr, index: 1)
                    e.setBytes(&matrix, length: MemoryLayout<simd_float3x3>.stride, index: 0)
                    e.setBytes(&developUniforms, length: MemoryLayout<DevelopUniforms>.stride, index: 1)
                }

                // Letting the inhibitor diffuse is what turns DIR couplers into a
                // local-contrast and edge-sharpness effect rather than a pure
                // per-pixel saturation boost.
                var correctionSource = corr
                let sigma = couplerDiffusionPixels(setup: setup, recipe: recipe)
                if sigma > 0.4 {
                    let blur = MPSImageGaussianBlur(device: device, sigma: sigma)
                    blur.edgeMode = .clamp
                    blur.encode(commandBuffer: cb, sourceTexture: corr, destinationTexture: corrBlurred)
                    correctionSource = corrBlurred
                }

                encode(cb, filmDevelopCouplers, width: width, height: height) { e in
                    e.setTexture(logE, index: 0)
                    e.setTexture(correctionSource, index: 1)
                    e.setTexture(cmyAlt, index: 2)
                    e.setTexture(corr, index: 3)            // the inhibitor where it was released
                    e.setBuffer(curves0Buffer, offset: 0, index: 0)
                    e.setBytes(&developUniforms, length: MemoryLayout<DevelopUniforms>.stride, index: 1)
                    e.setBuffer(edgeScaleBuffer, offset: 0, index: 2)
                }
                cmy = cmyAlt
            }

            // 2c — grain (`drawGrain`), on the frame unless the print is shaped.
            if !shaped, recipe.grainEnabled, recipe.grainAmount > 0, let grain = setup.film.defaults.grain {
                let target = (cmy === cmyAlt) ? cmyFilm! : cmyAlt
                drawGrain(cb, grain, on: cmy, into: target, scratch: scratch1, scratch2,
                          setup: setup, recipe: recipe,
                          origin: SIMD2(UInt32(windowOrigin.x), UInt32(windowOrigin.y)))
                grainFree = cmy
                cmy = target
            }
            filmStageKey = filmKey
            developedGrainFree = grainFree
        }
        developedCmy = cmy

        // 2d — the print's shape: the developed negative turned, mirrored,
        //      levelled and cropped, as the enlarger's easel and the trimmer
        //      would shape the print, and the rest made at the print's own
        //      size. Exposure, halation and the couplers have acted on the
        //      whole frame, and the pixels keep the frame's size, so a crop
        //      enlarges the grain as cropping a print does. The grain is drawn
        //      after, on the print's own pixels: drawn on the frame and
        //      levelled with it, it was softened wherever the print's pixels
        //      fall between the frame's, and showed a faint grid, every 23
        //      pixels at 2.5°.
        var negative = cmy
        let printed: MTLTexture, screen: MTLTexture
        var printScratch = scratch1, printScratchB = scratch2, grainyBlur = rawB
        if shaped {
            let w = Int(made.width), h = Int(made.height)
            if geometryLinear?.width != w || geometryLinear?.height != h {
                shapedNegative = makeTexture(width: w, height: h, format: intermediateFormat)
                shapedGrainy = makeTexture(width: w, height: h, format: intermediateFormat)
                shapedScratchA = makeTexture(width: w, height: h, format: intermediateFormat)
                shapedScratchB = makeTexture(width: w, height: h, format: intermediateFormat)
                geometryLinear = makeTexture(width: w, height: h, format: .rgba32Float)
                geometryDisplay = makeTexture(width: w, height: h, format: .bgra8Unorm)
            }
            guard let shapedNeg = shapedNegative, let grainy = shapedGrainy, let a = shapedScratchA,
                  let b = shapedScratchB, let shapedPrint = geometryLinear, let shapedScreen = geometryDisplay else {
                throw RendererError.textureAllocation
            }
            // From the part's pixels to the image held: the frame, or the box.
            var uniforms = part == nil ? GeometryUniforms(map) : GeometryUniforms(map, part: made.origin, held: origin)
            encode(cb, applyGeometry, width: w, height: h) { e in
                e.setTexture(cmy, index: 0)
                e.setTexture(shapedNeg, index: 1)
                e.setBytes(&uniforms, length: MemoryLayout<GeometryUniforms>.stride, index: 0)
            }
            negative = shapedNeg
            grainFree = nil
            if recipe.grainEnabled, recipe.grainAmount > 0, let grain = setup.film.defaults.grain {
                drawGrain(cb, grain, on: shapedNeg, into: grainy, scratch: a, b, setup: setup, recipe: recipe,
                          origin: SIMD2(UInt32(made.minX), UInt32(made.minY)))
                negative = grainy
                grainFree = shapedNeg
            }
            printed = shapedPrint
            screen = shapedScreen
            // The grainy negative is printed before its texture is needed again.
            printScratch = a
            printScratchB = b
            grainyBlur = grainy
        } else {
            if linearOut == nil {
                linearOut = makeTexture(width: width, height: height, format: .rgba32Float)
                displayOut = makeTexture(width: width, height: height, format: .bgra8Unorm)
            }
            guard let frame = linearOut, let frameScreen = displayOut else { throw RendererError.textureAllocation }
            printed = frame
            screen = frameScreen
        }

        // 3 — printing.expose + printing.develop + scanning.scan, fused
        var printUniforms = setup.printUniforms
        var shape = GeometryUniforms(map)
        if part != nil {
            // The loupe's part finds the masks, drawn on the frame, through
            // where it lies in the frame.
            shape = GeometryUniforms(map, part: made.origin)
            shape.active = 1
        }
        let db = dodgeBurnTexture ?? dummyDodgeBurn
        func printNegative(_ negative: MTLTexture, into destination: MTLTexture) {
            encode(cb, printAndScan, width: destination.width, height: destination.height) { e in
                e.setTexture(negative, index: 0)
                e.setTexture(destination, index: 1)
                e.setTexture(toneLut, index: 2)
                e.setTexture(db, index: 3)
                e.setBuffer(spectralBuffer, offset: 0, index: 0)
                e.setBytes(&printUniforms, length: MemoryLayout<PrintUniforms>.stride, index: 1)
                e.setBuffer(toneShiftBuffer, offset: 0, index: 2)
                e.setBytes(&shape, length: MemoryLayout<GeometryUniforms>.stride, index: 3)
            }
        }
        printNegative(negative, into: printed)

        // 3b — grain as texture only.
        //
        // Grain is drawn per pixel, so its noise per pixel depends on how many
        // silver grains a pixel holds: a full-resolution export has 8-16x
        // fewer per pixel than the preview, and 3-4x the noise. Printing and
        // scanning are nonlinear, so that extra noise shifted the export's
        // average colour away from the preview's (b* -1 to -2.4, the shadows
        // of one portrait 19% bluer). It also moved both away from the
        // measured curves, which are large-area densities with the grain
        // already in them.
        //
        // So the shift is taken out, and only the shift. The grain's mirror —
        // the clean density less the grain — is printed too. Grain is
        // symmetric, so in the mean of the two prints its texture, the odd
        // part of the print's response, cancels, and what is left is the
        // grain-free print and the even part: the shift. Each pixel is scaled
        // so that its neighbourhood of that mean holds the grain-free light.
        // The neighbourhood is a 3 px Gaussian, wide enough to average the
        // shift's own noise, narrow enough to follow the image. Scaling
        // against the grainy print's own neighbourhood, as 1.0-1.1 did, took
        // the grain's coarse structure out with the shift: the print kept 40%
        // of the negative's grain at 0.1 mm. It is a gain, not an offset:
        // subtracting the shift left some of a full-size frame's red pixels
        // below zero, in its teals, and the file clipped them. The average is
        // of light: what the eye adds up, and what macOS Preview appears to
        // average when it fits an image to its window. The film-stage scratch
        // textures are free by now.
        if let grainFree {
            encode(cb, grainMirror, width: grainFree.width, height: grainFree.height) { e in
                e.setTexture(grainFree, index: 0)
                e.setTexture(negative, index: 1)
                e.setTexture(printScratchB, index: 2)
            }
            printNegative(printScratchB, into: printScratch)
            encode(cb, grainEvenMean, width: printed.width, height: printed.height) { e in
                e.setTexture(printed, index: 0)
                e.setTexture(printScratch, index: 1)
                e.setTexture(printScratchB, index: 2)
            }
            blur(cb, from: printScratchB, to: grainyBlur, sigma: 3)
            printNegative(grainFree, into: printScratch)
            blur(cb, from: printScratch, to: printScratchB, sigma: 3)
            encode(cb, grainRestoreMean, width: printed.width, height: printed.height) { e in
                e.setTexture(printed, index: 0)
                e.setTexture(grainyBlur, index: 1)
                e.setTexture(printScratchB, index: 2)
            }
        }

        // 3c — bloom and glow: Glow's effects (FLXFilmLab/Effects), a look laid
        //      on the finished print in linear light. Their sizes are
        //      fractions of the frame's long edge, so the preview blooms as the
        //      full-size export does, and a crop enlarges them as it enlarges
        //      the grain. Core Image encodes into this same command buffer.
        if let effects = recipe.effects, effects.isActive {
            if keepsPrintBeforeEffects { keep(printed, cb) }
            let whole = CGSize(width: map.outputWidth, height: map.outputHeight)
            applyEffects(effects, to: printed, longEdge: max(frameWidth, frameHeight),
                         part: part.map { (made, whole, $0.beyond) }, commandBuffer: cb)
        }

        // 3d — the curves (`PrintCurves`), last of all, on the print as its
        //      file encodes it. The histogram is taken of the finished print,
        //      and, for the curves' graph, of the print they were given.
        var encoding: Int32 = recipe.outputColorSpace == "ProPhoto RGB" ? 2 : 1
        var coverage: Int32 = wholeFrame && shaped ? 1 : 0
        let curves = recipe.curves.flatMap { $0.isIdentity ? nil : $0 }
        let bins = measuresHistogram ? histogramBins(curves == nil ? 1 : 2) : []
        if bins.count > 1 {
            measureHistogram(cb, of: printed, into: bins[1], encoding: encoding, coverage: coverage, shape: shape)
        }
        if let curves, let table = curveTable(curves) {
            var encodingSize = SIMD2<Int32>(encoding, Int32(PrintCurves.tableSize))
            encode(cb, applyCurves, width: printed.width, height: printed.height) { e in
                e.setTexture(printed, index: 0)
                e.setBuffer(table, offset: 0, index: 0)
                e.setBytes(&encodingSize, length: MemoryLayout<SIMD2<Int32>>.stride, index: 1)
            }
        }
        if let after = bins.first {
            measureHistogram(cb, of: printed, into: after, encoding: encoding, coverage: coverage, shape: shape)
        }

        // 4 — output encoding for the screen. While the crop is drawn, the
        //     corners levelling leaves empty are transparent.
        encode(cb, encodeDisplay, width: printed.width, height: printed.height) { e in
            e.setTexture(printed, index: 0)
            e.setTexture(screen, index: 1)
            e.setBytes(&encoding, length: MemoryLayout<Int32>.stride, index: 0)
            e.setBytes(&coverage, length: MemoryLayout<Int32>.stride, index: 1)
            e.setBytes(&shape, length: MemoryLayout<GeometryUniforms>.stride, index: 2)
        }

        cb.commit()
        cb.waitUntilCompleted()
        if let after = bins.first {
            func read(_ buffer: MTLBuffer) -> [UInt32] {
                Array(UnsafeBufferPointer(start: buffer.contents().assumingMemoryBound(to: UInt32.self), count: 768))
            }
            histogram = (read(after), bins.count > 1 ? read(bins[1]) : nil)
        }
        finishedLinear = printed
        finishedDisplay = screen
        madeRect = made
        displayHasAlpha = coverage == 1
        outputSize = CGSize(width: printed.width, height: printed.height)
        return screen
    }

    /// 2c — grain, in the density domain where development happens: per
    /// emulsion layer, fast, medium and slow, each drawn at the strength the
    /// stock's datasheet gives it and spread over its own dye clouds, then
    /// laid on the clean density (`film_grain_layer`). Only the grain is
    /// spread, not the image under it, so turning grain on costs no sharpness.
    /// Its scale is the film's: the pitch is the frame's (`imageSize`), on the
    /// frame and on a shaped print alike, so a crop enlarges the grain as it
    /// enlarges the picture. Grain size scales the clouds, and with them the
    /// Selwyn coefficient, which grows with a cloud's area.
    private func drawGrain(_ cb: MTLCommandBuffer, _ grain: GrainDefaults, on clean: MTLTexture,
                           into target: MTLTexture, scratch scratch1: MTLTexture, _ scratch2: MTLTexture,
                           setup: RenderSetup, recipe: Recipe, origin: SIMD2<UInt32> = .zero) {
        let width = clean.width, height = clean.height
        let pitch = Self.pixelSizeUm(width: Int(imageSize.width), height: Int(imageSize.height),
                                     frame: setup.film.frame(recipe.filmFormat))
        let size = Float(recipe.grainScale)
        let top = grain.tableDensityMax
        var table = grain.selwynUm2
        for layer in 0..<3 {
            var uniforms = GrainLayerUniforms(tableTop: SIMD3(top[0], top[1], top[2]),
                                              pixelArea: pitch * pitch, sizeSquared: size * size,
                                              amount: Float(recipe.grainAmount),
                                              seed: Self.fnv1a(setup.film.stock) ^ (grainSeed &* 0x9E37_79B9),
                                              monochrome: setup.film.isMonochrome ? 1 : 0,
                                              layer: UInt32(layer), samples: UInt32(grain.tableSamples),
                                              origin: origin)
            encode(cb, filmGrainLayer, width: width, height: height) { e in
                e.setTexture(clean, index: 0)
                e.setTexture(scratch1, index: 1)
                e.setBytes(&uniforms, length: MemoryLayout<GrainLayerUniforms>.stride, index: 0)
                e.setBytes(&table, length: MemoryLayout<Float>.stride * table.count, index: 1)
            }
            // each record's clouds for this layer, in pixels, padded to one radius
            let kernels = (0..<3).map {
                Self.cloudFilter(sigma: Double(grain.cloudUm[$0 * 3 + layer] * size / pitch))
            }
            let radius = kernels.map { $0.count / 2 }.max() ?? 0
            var taps = (0...(2 * radius)).map { i in
                SIMD4<Float>((0..<3).map { c -> Float in
                    let k = kernels[c], j = i - radius + k.count / 2
                    return j >= 0 && j < k.count ? Float(k[j]) : 0
                } + [0])
            }
            var first = SIMD2<Int32>(Int32(radius), 0)
            encode(cb, grainConvolve, width: width, height: height) { e in
                e.setTexture(scratch1, index: 0)
                e.setTexture(scratch2, index: 1)
                e.setBytes(&taps, length: MemoryLayout<SIMD4<Float>>.stride * taps.count, index: 0)
                e.setBytes(&first, length: MemoryLayout<SIMD2<Int32>>.stride, index: 1)
            }
            var second = SIMD3<Int32>(Int32(radius), 1, layer == 0 ? 1 : 0)
            encode(cb, grainConvolveAdd, width: width, height: height) { e in
                e.setTexture(scratch2, index: 0)
                e.setTexture(clean, index: 1)
                e.setTexture(target, index: 2)
                e.setBytes(&taps, length: MemoryLayout<SIMD4<Float>>.stride * taps.count, index: 0)
                e.setBytes(&second, length: MemoryLayout<SIMD3<Int32>>.stride, index: 1)
            }
        }
    }

    // MARK: - Part of a frame, for the loupe

    /// The part of the print to make, for the loupe: `rect`, in the print's
    /// own pixels at the frame's size, and the whole print at any size (the
    /// preview's) before its effects, for their blurs to gather light from
    /// beyond the part, as they do in the whole.
    struct Part {
        var rect: CGRect
        var beyond: CIImage?
    }

    /// Keep the frame at full size on the GPU, for the loupe's boxes. It is
    /// sent up a band at a time, so the copy on its way takes little more
    /// memory than the band: about 16 bytes a pixel stay, 720 MB at 45 MP.
    func holdFrame(_ pixels: [Float], width: Int, height: Int) {
        heldFrame = nil
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba32Float, width: width, height: height, mipmapped: false)
        d.usage = [.shaderRead]
        d.storageMode = .private
        let rows = min(512, height)
        let s = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba32Float, width: width, height: rows, mipmapped: false)
        s.usage = [.shaderRead]
        s.storageMode = .shared
        guard pixels.count >= width * height * 4, let frame = device.makeTexture(descriptor: d),
              let staging = device.makeTexture(descriptor: s) else { return }
        pixels.withUnsafeBytes { raw in
            var y0 = 0
            while y0 < height {
                let n = min(rows, height - y0)
                staging.replace(region: MTLRegionMake2D(0, 0, width, n), mipmapLevel: 0,
                                withBytes: raw.baseAddress!.advanced(by: y0 * width * 16), bytesPerRow: width * 16)
                guard let cb = queue.makeCommandBuffer(), let blit = cb.makeBlitCommandEncoder() else { return }
                blit.copy(from: staging, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                          sourceSize: MTLSize(width: width, height: n, depth: 1),
                          to: frame, destinationSlice: 0, destinationLevel: 0,
                          destinationOrigin: MTLOrigin(x: 0, y: y0, z: 0))
                blit.endEncoding()
                cb.commit()
                cb.waitUntilCompleted()
                y0 += n
            }
        }
        heldFrame = frame
    }

    /// Lets the frame held for the loupe go, and the box taken from it.
    func releaseFrame() {
        heldFrame = nil
        sourceTexture = nil
        rawLinear = nil; rawLinearB = nil; scratchA = nil; scratchB = nil
        logEFilm = nil; cmyFilm = nil; cmyFilmB = nil; correction = nil; correctionBlurred = nil
        linearOut = nil; displayOut = nil; effectsOut = nil
        shapedNegative = nil; shapedGrainy = nil; shapedScratchA = nil; shapedScratchB = nil
        geometryLinear = nil; geometryDisplay = nil; finishedLinear = nil; finishedDisplay = nil
        displayStaging = nil; developedCmy = nil; developedGrainFree = nil; filmStageKey = nil
    }

    var holdsFrame: Bool { heldFrame != nil }

    /// Take the box `x, y, width, height` of the frame held (`holdFrame`) as
    /// the image to render: `render(…, part:)` then makes part of the print
    /// exactly as the whole would be there, given a box that holds that
    /// part and every stage's reach around it (`reach`). The textures stay
    /// while the box keeps its size; the negative is developed again only
    /// when the box moves.
    @discardableResult
    func setWindow(x: Int, y: Int, width: Int, height: Int) -> Bool {
        guard let frame = heldFrame, width > 0, height > 0,
              x >= 0, y >= 0, x + width <= frame.width, y + height <= frame.height else { return false }
        if sourceTexture?.width != width || sourceTexture?.height != height || rawLinear == nil {
            sourceTexture = makeTexture(width: width, height: height, format: .rgba32Float)
            // Half or full precision as the whole frame's export would be.
            intermediateFormat = (frame.width * frame.height > 12_000_000) ? .rgba16Float : .rgba32Float
            allocateIntermediates(width: width, height: height)
        }
        if windowOrigin != SIMD2(x, y) { filmStageKey = nil }
        imageSize = CGSize(width: frame.width, height: frame.height)
        windowOrigin = SIMD2(x, y)
        guard let box = sourceTexture, let cb = queue.makeCommandBuffer(),
              let blit = cb.makeBlitCommandEncoder() else { return false }
        blit.copy(from: frame, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: x, y: y, z: 0),
                  sourceSize: MTLSize(width: width, height: height, depth: 1),
                  to: box, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
        blit.endEncoding()
        cb.commit()
        cb.waitUntilCompleted()
        return true
    }

    /// How far each stage of the print reaches past a pixel, in pixels of a
    /// frame at full size: what a part of the print must be made with around
    /// it to come out as the whole print does there (the loupe).
    struct Reach: Equatable {
        /// On the frame, before the print is shaped: scatter, halation, the
        /// couplers' diffusion; and, unshaped, the grain and its mean too.
        var film: Int
        /// On a shaped print's own pixels: its grain, and the 3 px its mean
        /// is restored over.
        var print: Int
        /// What the box's corner in the frame must be a multiple of. Metal's
        /// Gaussian blur makes a wide blur on a coarser grid that starts at
        /// the texture's corner, so a box whose corner is off that grid
        /// blurs a little differently (1e-3 in the file's values near a
        /// highlight, for halation 4x as wide). On the grid, the part comes
        /// out as the whole does to the float.
        var align: Int
    }

    /// The reach of `recipe` on a frame `frameWidth` x `frameHeight`: four
    /// sigmas of a blur, all the taps of a filter. Bloom & Glow reach further
    /// than a part can hold; they take the light beyond it from the preview
    /// (`Part.beyond`).
    static func reach(setup: RenderSetup, recipe: Recipe, frameWidth: Int, frameHeight: Int,
                      shaped: Bool) -> Reach {
        let pitch = pixelSizeUm(width: frameWidth, height: frameHeight, frame: setup.film.frame(recipe.filmFormat))
        func mean(_ v: [Float]) -> Float { v.isEmpty ? 0 : v.reduce(0, +) / Float(v.count) }
        var film: Float = 0, widest: Float = 0
        if recipe.halationEnabled, let h = setup.film.defaults.halation, h.active {
            let scatter = recipe.keepsCameraSharpness ? 0 : Float(recipe.scatterAmount)
            let core = mean(h.scatterCoreUm) * Float(recipe.scatterScale) / pitch
            let tail = mean(h.scatterTailUm) * Float(recipe.scatterScale) / pitch
            if scatter > 0 && (core > 0.05 || tail > 0.05) {
                let sigma = max(core, tail * 1.5236, 0.05)
                film += 4 * sigma
                widest = max(widest, sigma)
            }
            let strength = h.halationStrength.max() ?? 0
            let sigma = mean(h.halationFirstSigmaUm) * Float(recipe.halationScale) / pitch
            if strength * Float(recipe.halationAmount) > 0, sigma > 0.05 {
                let last = sigma * Float(max(h.halationNBounces, 1)).squareRoot()
                film += 4 * last
                widest = max(widest, last)
            }
        }
        if recipe.couplersEnabled, recipe.couplersAmount > 0, let dc = setup.film.defaults.dirCouplers,
           !recipe.keepsCameraSharpness, dc.diffusionSizeUm > 0 {
            let sigma = dc.diffusionSizeUm * Float(recipe.couplersDiffusion) / pitch
            if sigma > 0.4 {
                film += 4 * sigma
                widest = max(widest, sigma)
            }
        }
        var grain: Float = 0
        if recipe.grainEnabled, recipe.grainAmount > 0, let g = setup.film.defaults.grain {
            let size = Float(recipe.grainScale)
            let taps = g.cloudUm.map { cloudFilter(sigma: Double($0 * size / pitch)).count / 2 }.max() ?? 0
            grain = Float(taps) + 12
        }
        // Blurs this narrow are made on the texture's own grid; wider ones
        // were found to need the corner on a grid of 8 at a sigma of 19 px
        // and of 32 at 50 px: twice the sigma, to the next power of two.
        var align = 1
        if widest >= 3 { while Float(align) < 2 * widest && align < 256 { align *= 2 } }
        // The shaping reads two pixels each way; one more for the rounding.
        return shaped ? Reach(film: Int((film + 3).rounded(.up)), print: Int((grain + 2).rounded(.up)), align: align)
                      : Reach(film: Int((film + grain + 2).rounded(.up)), print: 0, align: align)
    }

    /// What the loupe makes for a window of the print (in the print's pixels
    /// at full size): the part of the print, the window with the reach on the
    /// print around it, and the box of the frame the part is made from, the
    /// part's image there with the reach on the frame around it and its
    /// corner on the blurs' grid. Both are kept inside the print and the
    /// frame by moving them, not by cutting them, so their sizes, and the
    /// textures made for them, hold as the window moves (except against the
    /// frame's far edges). Unshaped, the part is the box.
    static func loupeRegions(window: CGRect, map: GeometryMap, reach: Reach) -> (part: CGRect, box: CGRect) {
        let print = CGRect(x: 0, y: 0, width: map.outputWidth, height: map.outputHeight)
        let fw = Int(map.sourceSize.width), fh = Int(map.sourceSize.height)
        /// `lo..<hi` of `0..<limit`, from a multiple of `align`, in a size
        /// that depends on the length alone.
        func span(_ lo: CGFloat, _ hi: CGFloat, limit: Int, align: Int) -> (start: Int, size: Int) {
            let from = lo.rounded(.down)
            let wanted = Int((hi - from - 1e-6).rounded(.up))
            var size = ((wanted + align - 1) / align) * align + (align > 1 ? align : 0)
            guard size < limit else { return (0, limit) }
            var start = Int((from / CGFloat(align)).rounded(.down)) * align
            start = max(start, 0)
            if start + size > limit {
                start = max(((limit - size) / align) * align, 0)
                size = limit - start
            }
            return (start, size)
        }
        func box(around r: CGRect, by margin: Int) -> CGRect {
            let x = span(r.minX - CGFloat(margin), r.maxX + CGFloat(margin), limit: fw, align: reach.align)
            let y = span(r.minY - CGFloat(margin), r.maxY + CGFloat(margin), limit: fh, align: reach.align)
            return CGRect(x: x.start, y: y.start, width: x.size, height: y.size)
        }
        let window = window.integral
        guard !map.isIdentity else {
            let b = box(around: window, by: reach.film)
            return (b, b)
        }
        let m = CGFloat(reach.print)
        let px = span(window.minX - m, window.maxX + m, limit: map.outputWidth, align: 1)
        let py = span(window.minY - m, window.maxY + m, limit: map.outputHeight, align: 1)
        let part = CGRect(x: px.start, y: py.start, width: px.size, height: py.size).intersection(print)
        let corners = [CGPoint(x: part.minX, y: part.minY), CGPoint(x: part.maxX, y: part.minY),
                       CGPoint(x: part.minX, y: part.maxY), CGPoint(x: part.maxX, y: part.maxY)]
            .map { $0.applying(map.outputToSource) }
        let x0 = corners.map(\.x).min()!, x1 = corners.map(\.x).max()!
        let y0 = corners.map(\.y).min()!, y1 = corners.map(\.y).max()!
        return (part, box(around: CGRect(x: x0, y: y0, width: x1 - x0, height: y1 - y0), by: reach.film + 1))
    }

    /// A copy of the print before its effects, for the loupe's.
    private func keep(_ printed: MTLTexture, _ cb: MTLCommandBuffer) {
        if printBeforeEffects?.width != printed.width || printBeforeEffects?.height != printed.height {
            printBeforeEffects = makeTexture(width: printed.width, height: printed.height, format: .rgba32Float)
        }
        guard let copy = printBeforeEffects, let blit = cb.makeBlitCommandEncoder() else { return }
        blit.copy(from: printed, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                  sourceSize: MTLSize(width: printed.width, height: printed.height, depth: 1),
                  to: copy, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
        blit.endEncoding()
    }

    // MARK: - Curves and histogram

    /// The curves' table, sampled again only when the curves change.
    private func curveTable(_ curves: PrintCurves) -> MTLBuffer? {
        if curveBufferCurves != curves || curveBuffer == nil {
            let table = curves.table()
            curveBuffer = table.withUnsafeBytes {
                device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared)
            }
            curveBufferCurves = curves
        }
        return curveBuffer
    }

    /// `count` histograms' bins, emptied, kept from one render to the next.
    private func histogramBins(_ count: Int) -> [MTLBuffer] {
        while histogramBuffers.count < count {
            guard let buffer = device.makeBuffer(length: 768 * MemoryLayout<UInt32>.stride,
                                                 options: .storageModeShared) else { return [] }
            histogramBuffers.append(buffer)
        }
        let buffers = Array(histogramBuffers.prefix(count))
        for buffer in buffers { memset(buffer.contents(), 0, buffer.length) }
        return buffers
    }

    private func measureHistogram(_ cb: MTLCommandBuffer, of print: MTLTexture, into bins: MTLBuffer,
                                  encoding: Int32, coverage: Int32, shape: GeometryUniforms) {
        var encoding = encoding, coverage = coverage, shape = shape
        encode(cb, histogramRGB, width: print.width, height: print.height) { e in
            e.setTexture(print, index: 0)
            e.setBuffer(bins, offset: 0, index: 0)
            e.setBytes(&encoding, length: MemoryLayout<Int32>.stride, index: 1)
            e.setBytes(&coverage, length: MemoryLayout<Int32>.stride, index: 2)
            e.setBytes(&shape, length: MemoryLayout<GeometryUniforms>.stride, index: 3)
        }
    }

    // MARK: - Effects

    /// Core Image's working space for the effects. The print is in the output
    /// space's linear primaries; tagging both ends with the same space leaves
    /// it there — nothing is converted — and the effects' own thresholds only
    /// use the sRGB transfer curve, which every output space but ProPhoto shares.
    private static let effectsSpace = CGColorSpace(name: CGColorSpace.extendedLinearSRGB)!

    /// Runs Glow's pipeline over the print and writes the result back. Float
    /// throughout, so a 16-bit TIFF keeps its precision. `longEdge` is the
    /// frame's, which sets the effects' sizes.
    private func applyEffects(_ look: GlowLook, to linear: MTLTexture, longEdge: Int,
                              part: (rect: CGRect, whole: CGSize, beyond: CIImage?)? = nil,
                              commandBuffer cb: MTLCommandBuffer) {
        if effectsContext == nil {
            effectsContext = CIContext(mtlCommandQueue: queue, options: [
                .workingColorSpace: Self.effectsSpace,
                .workingFormat: CIFormat.RGBAf,
                .cacheIntermediates: false,
                .name: "FLX Film Lab effects",
            ])
        }
        if effectsOut?.width != linear.width || effectsOut?.height != linear.height {
            effectsOut = makeTexture(width: linear.width, height: linear.height, format: .rgba32Float)
        }
        guard let context = effectsContext, let out = effectsOut,
              let image = CIImage(mtlTexture: linear, options: [.colorSpace: Self.effectsSpace]) else { return }
        var input = image, bounds = image.extent
        if let part {
            // The loupe's part, laid where it is in the whole print, over the
            // whole print as the preview made it: the effects' blurs reach
            // far past the part (5% of the frame's long edge, the veil 8%),
            // and gather the light from all of it, as in the export. Without
            // the preview's, the part's edges are carried out.
            let whole = CGRect(origin: .zero, size: part.whole)
            let placed = image.transformed(by: CGAffineTransform(translationX: part.rect.minX, y: part.rect.minY))
            var beyond = placed.clampedToExtent()
            if let far = part.beyond, far.extent.width > 0, far.extent.height > 0 {
                beyond = far.transformed(by: CGAffineTransform(scaleX: whole.width / far.extent.width,
                                                               y: whole.height / far.extent.height))
            }
            input = placed.composited(over: beyond).cropped(to: whole)
            bounds = placed.extent
        }
        let result = GlowPipeline.apply(look, to: input, longEdge: CGFloat(longEdge))
        context.render(result, to: out, commandBuffer: cb, bounds: bounds, colorSpace: Self.effectsSpace)
        guard let blit = cb.makeBlitCommandEncoder() else { return }
        blit.copy(from: out, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                  sourceSize: MTLSize(width: linear.width, height: linear.height, depth: 1),
                  to: linear, destinationSlice: 0, destinationLevel: 0,
                  destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
        blit.endEncoding()
    }

    // MARK: - Halation and grain

    struct ScatterUniforms {
        var tailWeight: SIMD3<Float>
        var amount: Float
        var pad: Float = 0
    }

    struct HalationUniforms {
        var strength: SIMD3<Float>
        var renormalize: Int32
    }

    struct GrainLayerUniforms {
        var tableTop: SIMD3<Float>
        var pixelArea: Float
        var sizeSquared: Float
        var amount: Float
        var seed: UInt32
        var monochrome: UInt32
        var layer: UInt32
        var samples: UInt32
        /// Where the texture's first pixel is in the frame (or the print):
        /// the grain is seeded by the frame's pixels, so the loupe's part
        /// draws the grains the whole does.
        var origin = SIMD2<UInt32>.zero
    }

    /// agx `model/diffusion.apply_halation_um`: scatter first (energy-preserving
    /// core + exponential tail), then additive back-reflection.
    ///
    /// One departure from the reference: agx blurs each channel with its own
    /// scatter sigma. The three differ by under 30% and land well under a pixel
    /// at any real output size, so this uses their mean and saves four blur
    /// passes. Back-reflection — the visible part, the red bloom — is exact,
    /// including its per-channel strength and the sqrt(k) bounce widths.
    private func encodeHalation(_ cb: MTLCommandBuffer,
                                source: MTLTexture,
                                destination: MTLTexture,
                                scratch1: MTLTexture,
                                scratch2: MTLTexture,
                                halation: HalationDefaults,
                                recipe: Recipe,
                                frame: (long: Double, short: Double),
                                width: Int, height: Int) -> MTLTexture {
        let pixelSizeUm = Self.pixelSizeUm(width: Int(imageSize.width), height: Int(imageSize.height),
                                           frame: frame)
        var current = source

        // --- scatter --- (none with Camera sharpness: the file's own detail)
        let scatterAmount = recipe.keepsCameraSharpness ? 0 : Float(recipe.scatterAmount)
        let coreSigma = mean(halation.scatterCoreUm) * Float(recipe.scatterScale) / pixelSizeUm
        let tailLambda = mean(halation.scatterTailUm) * Float(recipe.scatterScale) / pixelSizeUm
        if scatterAmount > 0 && (coreSigma > 0.05 || tailLambda > 0.05) {
            blur(cb, from: current, to: scratch1, sigma: max(coreSigma, 0.05))
            // The exponential tail is a fitted three-Gaussian mixture in agx;
            // its dominant component (a = 0.65, sigma = 1.52 lambda) carries the
            // shape, and the two wings are within the blur's own error here.
            blur(cb, from: current, to: scratch2, sigma: max(tailLambda * 1.5236, 0.05))
            var uniforms = ScatterUniforms(
                tailWeight: SIMD3(halation.scatterTailWeight[0],
                                  halation.scatterTailWeight[1],
                                  halation.scatterTailWeight[2]),
                amount: scatterAmount)
            encode(cb, scatterMix, width: width, height: height) { e in
                e.setTexture(current, index: 0)
                e.setTexture(scratch1, index: 1)
                e.setTexture(scratch2, index: 2)
                e.setTexture(destination, index: 3)
                e.setBytes(&uniforms, length: MemoryLayout<ScatterUniforms>.stride, index: 0)
            }
            current = destination
        }

        // --- back-reflection halation ---
        let strength = SIMD3<Float>(halation.halationStrength[0],
                                    halation.halationStrength[1],
                                    halation.halationStrength[2]) * Float(recipe.halationAmount)
        let sigmaH = mean(halation.halationFirstSigmaUm) * Float(recipe.halationScale) / pixelSizeUm
        let bounces = max(halation.halationNBounces, 1)
        guard strength.max() > 0, sigmaH > 0.05 else { return current }

        var decay = (1...bounces).map { pow(halation.halationBounceDecay, Float($0 - 1)) }
        let decaySum = decay.reduce(0, +)
        if decaySum > 0 { decay = decay.map { $0 / decaySum } }

        let accum = (current === destination) ? scratch1 : destination
        let blurTarget = scratch2
        for (index, weight) in decay.enumerated() {
            let sigmaK = max(sigmaH * sqrt(Float(index + 1)), 0.05)
            blur(cb, from: current, to: blurTarget, sigma: sigmaK)
            var w = weight
            var reset: Int32 = index == 0 ? 1 : 0
            encode(cb, accumulateScaled, width: width, height: height) { e in
                e.setTexture(blurTarget, index: 0)
                e.setTexture(accum, index: 1)
                e.setBytes(&w, length: MemoryLayout<Float>.stride, index: 0)
                e.setBytes(&reset, length: MemoryLayout<Int32>.stride, index: 1)
            }
        }

        let result = (current === destination) ? scratch2 : destination
        var uniforms = HalationUniforms(strength: strength,
                                        renormalize: halation.halationRenormalize ? 1 : 0)
        encode(cb, halationAdd, width: width, height: height) { e in
            e.setTexture(current, index: 0)
            e.setTexture(accum, index: 1)
            e.setTexture(result, index: 2)
            e.setBytes(&uniforms, length: MemoryLayout<HalationUniforms>.stride, index: 0)
        }
        return result
    }

    /// The filter that spreads a layer's per-pixel noise over its clouds of
    /// `sigma` pixels, one axis of it. The film's grain is the clouds over a
    /// continuous film, and each pixel averages it over its own area, so
    /// neighbouring pixels share some of it: for Gaussian clouds, pixels n
    /// apart covary by a triangle (two pixel-wide boxes) convolved with the
    /// Gaussian at sigma times root 2. The filter is the square root of that
    /// covariance, in frequency, so the noise has it exactly, whatever the
    /// pitch: spreading each pixel's noise by the clouds alone, sampled at the
    /// pixels, missed some of the sharing, and a 12 MP frame seen at the
    /// preview's size had 13% less grain than a 45 MP one. It sums to one, so
    /// the grain over large areas is Selwyn's, and it is a single tap for
    /// clouds much smaller than a pixel.
    static func cloudFilter(sigma: Double) -> [Double] {
        guard sigma > 0.02 else { return [1] }
        // Psi(z) = integral of the normal CDF: z Phi(z) + phi(z)
        func psi(_ z: Double) -> Double {
            z * 0.5 * (1 + erf(z / 2.0.squareRoot())) + exp(-z * z / 2) / (2 * Double.pi).squareRoot()
        }
        let s = sigma * 2.0.squareRoot()
        let reach = Int((4 * s + 2).rounded(.up))
        // Pixels n apart covary by (triangle * Gaussian)(n): a box by (a box by
        // the Gaussian), whose second integral gives it in closed form
        func covariance(_ n: Int) -> Double {
            let x = Double(n)
            return s * (psi((x + 1) / s) - 2 * psi(x / s) + psi((x - 1) / s))
        }
        let c = (-reach...reach).map(covariance)
        // the square root in frequency, on a grid wide enough not to wrap
        let m = 8 * reach + 8
        var spectrum = [Double](repeating: 0, count: m)
        for k in 0..<m {
            let w = 2 * Double.pi * Double(k) / Double(m)
            var sum = 0.0
            for (i, v) in c.enumerated() { sum += v * cos(w * Double(i - reach)) }
            spectrum[k] = max(sum, 0).squareRoot()
        }
        var h = (-reach...reach).map { n -> Double in
            var sum = 0.0
            for k in 0..<m { sum += spectrum[k] * cos(2 * Double.pi * Double(k) * Double(n) / Double(m)) }
            return sum / Double(m)
        }
        let total = h.reduce(0, +)
        h = h.map { $0 / total }
        // trim taps that carry nothing
        while h.count > 1, let first = h.first, let last = h.last, abs(first) < 1e-5, abs(last) < 1e-5 {
            h.removeFirst(); h.removeLast()
        }
        return h
    }

    private func mean(_ values: [Float]) -> Float {
        values.isEmpty ? 0 : values.reduce(0, +) / Float(values.count)
    }

    private func blur(_ cb: MTLCommandBuffer, from: MTLTexture, to: MTLTexture, sigma: Float) {
        let kernel = MPSImageGaussianBlur(device: device, sigma: sigma)
        kernel.edgeMode = .clamp
        kernel.encode(commandBuffer: cb, sourceTexture: from, destinationTexture: to)
    }

    /// How far the inhibitor spreads, in pixels. Not at all with Camera
    /// sharpness: acting where it is released, the couplers keep their colour
    /// and tone and draw no edges, so the file's detail comes through.
    private func couplerDiffusionPixels(setup: RenderSetup, recipe: Recipe) -> Float {
        guard !recipe.keepsCameraSharpness,
              let dc = setup.film.defaults.dirCouplers, dc.diffusionSizeUm > 0 else { return 0 }
        let pixelSizeUm = Self.pixelSizeUm(width: Int(imageSize.width),
                                           height: Int(imageSize.height),
                                           frame: setup.film.frame(recipe.filmFormat))
        return dc.diffusionSizeUm * Float(recipe.couplersDiffusion) / pixelSizeUm
    }

    /// How much film a pixel covers, in um: the pitch that converts every
    /// micrometre-scale effect to pixels. The image is laid on the format's
    /// frame as large as it fits, as a camera of its shape would fill the
    /// film: a 3:2 image fills a 3:2 frame (agx's `ResizingService` spans the
    /// long edge), a 4:3 one — Fuji GFX, Micro Four Thirds — spans the short
    /// side, 24 mm on 35 mm and 56 mm on 120, and one wider than 3:2 spans
    /// the long side, inside the frame either way.
    static func pixelSizeUm(width: Int, height: Int, format: FilmFormat) -> Float {
        pixelSizeUm(width: width, height: height, frame: format.frameMM)
    }

    /// The same on a frame of `frame` mm: an instant print's own
    /// (`ProfilePack.frame`).
    static func pixelSizeUm(width: Int, height: Int, frame: (long: Double, short: Double)) -> Float {
        let long = Float(max(width, height)), short = Float(min(width, height))
        guard short > 0 else { return 10 }
        return min(Float(frame.long) * 1000 / long, Float(frame.short) * 1000 / short)
    }

    private func encode(_ cb: MTLCommandBuffer,
                        _ pipeline: MTLComputePipelineState,
                        width: Int, height: Int,
                        _ body: (MTLComputeCommandEncoder) -> Void) {
        guard let e = cb.makeComputeCommandEncoder() else { return }
        e.setComputePipelineState(pipeline)
        body(e)
        let w = pipeline.threadExecutionWidth
        let h = max(pipeline.maxTotalThreadsPerThreadgroup / w, 1)
        let threadsPerGroup = MTLSize(width: w, height: h, depth: 1)
        let groups = MTLSize(width: (width + w - 1) / w, height: (height + h - 1) / h, depth: 1)
        e.dispatchThreadgroups(groups, threadsPerThreadgroup: threadsPerGroup)
        e.endEncoding()
    }

    // MARK: - LUT and table uploads

    /// The spectral tables are 5.8 KB, past `setBytes`' 4 KB ceiling, so they
    /// live in a real buffer. The curve tables ride along for consistency.
    private var spectralBuffer: MTLBuffer?
    private var curvesBuffer: MTLBuffer?
    private var curves0Buffer: MTLBuffer?
    private var edgeScaleBuffer: MTLBuffer?
    private var toneShiftBuffer: MTLBuffer?

    private func uploadTables(_ setup: RenderSetup) {
        func upload(_ values: [Float], into buffer: inout MTLBuffer?) {
            let byteCount = values.count * MemoryLayout<Float>.size
            if buffer == nil || buffer!.length < byteCount {
                buffer = device.makeBuffer(length: byteCount, options: .storageModeShared)
            }
            guard let buffer else { return }
            values.withUnsafeBytes { raw in
                buffer.contents().copyMemory(from: raw.baseAddress!, byteCount: byteCount)
            }
        }
        upload(setup.spectralTables, into: &spectralBuffer)
        upload(setup.normalizedCurves, into: &curvesBuffer)
        upload(setup.curvesBeforeCouplers, into: &curves0Buffer)
        upload(setup.edgeScale.isEmpty ? [1, 1, 1] : setup.edgeScale, into: &edgeScaleBuffer)
        // Always bound, even when the kernel will not read it.
        upload(setup.toneShift.isEmpty ? [0, 0] : setup.toneShift, into: &toneShiftBuffer)
    }

    private func ensureTcLut(for film: ProfilePack, lensFilter: LensFilterRef?) throws {
        let key = lensFilter?.file ?? film.stock
        guard tcLutStock != key else { return }
        let (values, size) = try PackLibrary.shared.tcLut(for: film, lensFilter: lensFilter)
        // Pad RGB -> RGBA so the texture read is one fetch.
        var rgba = [Float](repeating: 1, count: size * size * 4)
        for i in 0..<(size * size) {
            rgba[i * 4 + 0] = values[i * 3 + 0]
            rgba[i * 4 + 1] = values[i * 3 + 1]
            rgba[i * 4 + 2] = values[i * 3 + 2]
        }
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba32Float, width: size, height: size, mipmapped: false)
        d.usage = [.shaderRead]
        d.storageMode = .shared
        guard let texture = device.makeTexture(descriptor: d) else { throw RendererError.textureAllocation }
        rgba.withUnsafeBytes { raw in
            texture.replace(region: MTLRegionMake2D(0, 0, size, size), mipmapLevel: 0,
                            withBytes: raw.baseAddress!, bytesPerRow: size * 16)
        }
        tcLutTexture = texture
        tcLutStock = key
    }

    private func uploadToneLUT(_ data: [Float]) {
        let w = Constants.toneLutWidth, h = Constants.toneLutHeight
        if toneLutTexture == nil {
            let d = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .rgba32Float, width: w, height: h, mipmapped: false)
            d.usage = [.shaderRead]
            d.storageMode = .shared
            toneLutTexture = device.makeTexture(descriptor: d)
        }
        data.withUnsafeBytes { raw in
            toneLutTexture?.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0,
                                    withBytes: raw.baseAddress!, bytesPerRow: w * 16)
        }
    }

    func setDodgeBurnMap(_ map: MaskRasterizer.Map) {
        let width = map.width, height = map.height
        let data = map.data
        guard width > 0, height > 0, data.count == width * height * 2 else {
            dodgeBurnTexture = nil
            return
        }
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rg32Float, width: width, height: height, mipmapped: false)
        d.usage = [.shaderRead]
        d.storageMode = .shared
        guard let texture = device.makeTexture(descriptor: d) else { return }
        data.withUnsafeBytes { raw in
            texture.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0,
                            withBytes: raw.baseAddress!, bytesPerRow: width * 8)
        }
        dodgeBurnTexture = texture
    }

    func clearDodgeBurn() { dodgeBurnTexture = nil }

    // MARK: - Readback

    /// Linear working-space pixels of the last render, `outputSize`, for
    /// export and for the parity harness.
    func readLinear() -> [Float]? {
        guard let linear = finishedLinear ?? linearOut else { return nil }
        return readback(linear)
    }

    func makeDisplayImage(colorSpaceName: String) -> CGImage? {
        guard let display = finishedDisplay ?? displayOut else { return nil }
        let w = display.width, h = display.height
        // A private texture cannot be read directly; stage through a shared
        // one, kept between frames rather than allocated for every one.
        if displayStaging?.width != w || displayStaging?.height != h {
            let d = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: display.pixelFormat, width: w, height: h, mipmapped: false)
            d.usage = [.shaderRead]
            d.storageMode = .shared
            displayStaging = device.makeTexture(descriptor: d)
        }
        guard let staging = displayStaging,
              let cb = queue.makeCommandBuffer(),
              let blit = cb.makeBlitCommandEncoder() else { return nil }
        blit.copy(from: display, sourceSlice: 0, sourceLevel: 0,
                  sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                  sourceSize: MTLSize(width: w, height: h, depth: 1),
                  to: staging, destinationSlice: 0, destinationLevel: 0,
                  destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
        blit.endEncoding()
        cb.commit()
        cb.waitUntilCompleted()
        var bytes = [UInt8](repeating: 0, count: w * h * 4)
        bytes.withUnsafeMutableBytes { raw in
            staging.getBytes(raw.baseAddress!, bytesPerRow: w * 4,
                             from: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0)
        }
        let cs = Self.cgColorSpace(named: colorSpaceName)
        guard let provider = CGDataProvider(data: Data(bytes) as CFData) else { return nil }
        // Blue, green, red, alpha in memory: the order Core Animation draws
        // from. Given red first, it redrew every frame into its own order on
        // the main thread, a sixth of that thread's time while a slider moved.
        let alpha: CGImageAlphaInfo = displayHasAlpha ? .premultipliedFirst : .noneSkipFirst
        return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32,
                       bytesPerRow: w * 4, space: cs,
                       bitmapInfo: CGBitmapInfo(rawValue: alpha.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false,
                       intent: .defaultIntent)
    }

    static func cgColorSpace(named: String) -> CGColorSpace {
        switch named {
        case "Display P3": return CGColorSpace(name: CGColorSpace.displayP3) ?? CGColorSpaceCreateDeviceRGB()
        // Not `itur_2020`: on macOS that profile carries a 2.4 power curve,
        // while the pixels are encoded with the sRGB curve (`encode_display`,
        // `Exporter.transferFunction`). Tagged that way, mid-grey decoded 0.2
        // stops dark and the deep shadows more than two.
        case "ITU-R BT.2020": return CGColorSpace(name: CGColorSpace.itur_2020_sRGBGamma) ?? CGColorSpaceCreateDeviceRGB()
        case "ProPhoto RGB": return CGColorSpace(name: CGColorSpace.rommrgb) ?? CGColorSpaceCreateDeviceRGB()
        default: return CGColorSpace(name: CGColorSpace.sRGB) ?? CGColorSpaceCreateDeviceRGB()
        }
    }

    private func stagingCopy(of texture: MTLTexture) -> MTLTexture? {
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: texture.pixelFormat, width: texture.width, height: texture.height, mipmapped: false)
        d.usage = [.shaderRead]
        d.storageMode = .shared
        guard let staging = device.makeTexture(descriptor: d),
              let cb = queue.makeCommandBuffer(),
              let blit = cb.makeBlitCommandEncoder() else { return nil }
        blit.copy(from: texture, sourceSlice: 0, sourceLevel: 0,
                  sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                  sourceSize: MTLSize(width: texture.width, height: texture.height, depth: 1),
                  to: staging, destinationSlice: 0, destinationLevel: 0,
                  destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
        blit.endEncoding()
        cb.commit()
        cb.waitUntilCompleted()
        return staging
    }

    private func readback(_ texture: MTLTexture) -> [Float]? {
        guard let staging = stagingCopy(of: texture) else { return nil }
        let w = texture.width, h = texture.height
        // Past 12 MP the intermediates are half precision (`setImage`).
        if texture.pixelFormat == .rgba16Float {
            var half = [Float16](repeating: 0, count: w * h * 4)
            half.withUnsafeMutableBytes { raw in
                staging.getBytes(raw.baseAddress!, bytesPerRow: w * 8,
                                 from: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0)
            }
            return half.map(Float.init)
        }
        var out = [Float](repeating: 0, count: w * h * 4)
        out.withUnsafeMutableBytes { raw in
            staging.getBytes(raw.baseAddress!, bytesPerRow: w * 16,
                             from: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0)
        }
        return out
    }

    /// Intermediate taps, for the parity harness.
    func readTap(_ tap: String) -> [Float]? {
        switch tap {
        case "log_e_film": return logEFilm.flatMap(readback)
        case "cmy_film":   return (developedCmy ?? cmyFilm).flatMap(readback)
        case "rgb_out":    return linearOut.flatMap(readback)
        case "shaped_cmy": return shapedNegative.flatMap(readback)
        default: return nil
        }
    }
}
