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

/// One exposure gradient as `film_expose` reads it: both handles in pixels of
/// the frame being rendered, and the compensation in EV. `ExposureGradientU`
/// in flx.metal.
struct ExposureGradientUniform {
    var a: SIMD2<Float> = .zero
    var b: SIMD2<Float> = .zero
    var ev: Float = 0
    var pad0: Float = 0
    var pad1: Float = 0
    var pad2: Float = 0
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
    var filmFormat: FilmFormat

    init(setup: RenderSetup, recipe: Recipe) {
        film = setup.film.stock
        lensFilter = setup.lensFilter?.id
        exposureEV = setup.exposureEV
        exposureGradients = setup.exposureGradients
        densityCurveGamma = recipe.densityCurveGamma
        couplers = [recipe.couplersEnabled ? 1 : 0, recipe.couplersAmount, recipe.couplersDiffusion]
        halation = [recipe.halationEnabled ? 1 : 0, recipe.halationAmount, recipe.halationScale,
                    recipe.scatterAmount, recipe.scatterScale]
        grain = [recipe.grainEnabled ? 1 : 0, recipe.grainAmount, recipe.grainScale]
        filmFormat = recipe.filmFormat
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
    private let filmGrain: MTLComputePipelineState
    private let filmGrainApply: MTLComputePipelineState
    private let filmDevelop: MTLComputePipelineState
    private let couplersAmount: MTLComputePipelineState
    private let filmDevelopCouplers: MTLComputePipelineState
    private let printAndScan: MTLComputePipelineState
    private let grainRestoreMean: MTLComputePipelineState
    private let encodeDisplay: MTLComputePipelineState

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
    private var displayStaging: MTLTexture?

    // Cached uploads
    private var tcLutTexture: MTLTexture?
    private var tcLutStock: String?
    private var toneLutTexture: MTLTexture?

    private(set) var imageSize: CGSize = .zero
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
            if #available(macOS 15.0, *) {
                options.mathMode = .fast
            } else {
                options.fastMathEnabled = true
            }
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
        filmGrain = try pipeline("film_grain")
        filmGrainApply = try pipeline("film_grain_apply")
        filmDevelop = try pipeline("film_develop")
        couplersAmount = try pipeline("couplers_amount")
        filmDevelopCouplers = try pipeline("film_develop_couplers")
        printAndScan = try pipeline("print_and_scan")
        grainRestoreMean = try pipeline("grain_restore_mean")
        encodeDisplay = try pipeline("encode_display")

        dummyDodgeBurn = makeTexture(width: 1, height: 1, format: .rg32Float, write: false)
    }

    // MARK: - Image lifecycle

    func setImage(_ pixels: [Float], width: Int, height: Int) {
        imageSize = CGSize(width: width, height: height)
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
        let format: MTLPixelFormat = (width * height > 12_000_000) ? .rgba16Float : .rgba32Float
        intermediateFormat = format

        rawLinear = makeTexture(width: width, height: height, format: format)
        rawLinearB = makeTexture(width: width, height: height, format: format)
        scratchA = makeTexture(width: width, height: height, format: format)
        scratchB = makeTexture(width: width, height: height, format: format)
        logEFilm = makeTexture(width: width, height: height, format: format)
        cmyFilm = makeTexture(width: width, height: height, format: format)
        cmyFilmB = makeTexture(width: width, height: height, format: format)
        correction = makeTexture(width: width, height: height, format: format)
        correctionBlurred = makeTexture(width: width, height: height, format: format)
        linearOut = makeTexture(width: width, height: height, format: .rgba32Float)
        displayOut = makeTexture(width: width, height: height, format: .rgba8Unorm)
        dodgeBurnTexture = nil
        effectsOut = nil
        filmStageKey = nil
        developedCmy = nil
        developedGrainFree = nil
        displayStaging = nil
    }

    private func makeTexture(width: Int, height: Int, format: MTLPixelFormat, write: Bool = true) -> MTLTexture? {
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: max(width, 1), height: max(height, 1), mipmapped: false)
        d.usage = write ? [.shaderRead, .shaderWrite] : [.shaderRead]
        d.storageMode = .private
        return device.makeTexture(descriptor: d)
    }

    // MARK: - Render

    @discardableResult
    func render(setup: RenderSetup, recipe: Recipe) throws -> MTLTexture? {
        guard let source = sourceTexture,
              let raw = rawLinear,
              let rawB = rawLinearB,
              let scratch1 = scratchA,
              let scratch2 = scratchB,
              let logE = logEFilm,
              var cmy = cmyFilm,
              let cmyAlt = cmyFilmB,
              let corr = correction,
              let corrBlurred = correctionBlurred,
              let linear = linearOut,
              let display = displayOut else { return nil }

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

        // The film stage — exposure, halation, development, couplers, grain —
        // depends on none of the print controls. When only those moved, the
        // developed negative from the last render is reused: the print stage
        // alone is about a quarter of the work.
        let filmKey = FilmStageKey(setup: setup, recipe: recipe)
        var grainFree: MTLTexture?
        if filmKey == filmStageKey, let developed = developedCmy {
            cmy = developed
            grainFree = developedGrainFree
        } else {
            // 1 — filming.expose
            // The exposure gradients change what the film receives, so they act
            // on the exposure itself, before halation spreads it in the film.
            var gradients = setup.exposureGradients.map { g in
                ExposureGradientUniform(
                    a: SIMD2(Float(g.points[0].x) * Float(width), Float(g.points[0].y) * Float(height)),
                    b: SIMD2(Float(g.points[1].x) * Float(width), Float(g.points[1].y) * Float(height)),
                    ev: Float(g.exposureEV))
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

            // 2c — grain, in the density domain where development actually happens.
            //      agx softens the drawn particles with a Gaussian of `blur`
            //      pixels (0.65 for every stock), which takes the noise per
            //      pixel down to under half; without it the grain was twice as
            //      loud as agx's at any size. Only the grain is softened here,
            //      not the image under it, so turning grain on costs no
            //      sharpness. agx skips blurs under 0.4 px, and so does this.
            if recipe.grainEnabled, recipe.grainAmount > 0, let grain = setup.film.defaults.grain {
                let target = (cmy === cmyAlt) ? cmyFilm! : cmyAlt
                var uniforms = grainUniforms(grain: grain, setup: setup, recipe: recipe)
                encode(cb, filmGrain, width: width, height: height) { e in
                    e.setTexture(cmy, index: 0)
                    e.setTexture(scratch1, index: 1)
                    e.setBytes(&uniforms, length: MemoryLayout<GrainUniforms>.stride, index: 0)
                }
                var deviation = scratch1
                if grain.blur > 0.4 {
                    blur(cb, from: scratch1, to: scratch2, sigma: grain.blur)
                    deviation = scratch2
                }
                encode(cb, filmGrainApply, width: width, height: height) { e in
                    e.setTexture(cmy, index: 0)
                    e.setTexture(deviation, index: 1)
                    e.setTexture(target, index: 2)
                }
                grainFree = cmy
                cmy = target
            }
            filmStageKey = filmKey
            developedGrainFree = grainFree
        }
        developedCmy = cmy

        // 3 — printing.expose + printing.develop + scanning.scan, fused
        var printUniforms = setup.printUniforms
        let db = dodgeBurnTexture ?? dummyDodgeBurn
        func printNegative(_ negative: MTLTexture, into destination: MTLTexture) {
            encode(cb, printAndScan, width: width, height: height) { e in
                e.setTexture(negative, index: 0)
                e.setTexture(destination, index: 1)
                e.setTexture(toneLut, index: 2)
                e.setTexture(db, index: 3)
                e.setBuffer(spectralBuffer, offset: 0, index: 0)
                e.setBytes(&printUniforms, length: MemoryLayout<PrintUniforms>.stride, index: 1)
                e.setBuffer(toneShiftBuffer, offset: 0, index: 2)
            }
        }
        printNegative(cmy, into: linear)

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
        // So the grain-free print is rendered as well, and each pixel is scaled
        // so that its neighbourhood holds the grain-free light: grain keeps
        // its texture, and every region averages to the grain-free colour at
        // any size. The neighbourhood is a 3 px Gaussian — wide enough to
        // average per-pixel grain, narrow enough to follow the image. It is a
        // gain, not an offset: subtracting the grain's local average left up
        // to 15% of a full-size frame's red pixels below zero, in its teals,
        // and the file clipped them, lifting that red by up to 1.5 stops. The
        // average is of light: what the eye adds up, and what macOS Preview
        // appears to average when it fits an image to its window. The
        // film-stage scratch textures are free by now.
        if let grainFree {
            printNegative(grainFree, into: scratch1)
            blur(cb, from: linear, to: rawB, sigma: 3)
            blur(cb, from: scratch1, to: scratch2, sigma: 3)
            encode(cb, grainRestoreMean, width: width, height: height) { e in
                e.setTexture(linear, index: 0)
                e.setTexture(rawB, index: 1)
                e.setTexture(scratch2, index: 2)
            }
        }

        // 3c — bloom and glow: Glow's effects (FLXFilmLab/Effects), a look laid
        //      on the finished print in linear light. Their sizes are
        //      fractions of the long edge, so the preview blooms as the
        //      full-size export does. Core Image encodes into this same
        //      command buffer.
        if let effects = recipe.effects, effects.isActive {
            applyEffects(effects, to: linear, commandBuffer: cb)
        }

        // 4 — output encoding for the screen
        var encoding: Int32 = recipe.outputColorSpace == "ProPhoto RGB" ? 2 : 1
        encode(cb, encodeDisplay, width: width, height: height) { e in
            e.setTexture(linear, index: 0)
            e.setTexture(display, index: 1)
            e.setBytes(&encoding, length: MemoryLayout<Int32>.stride, index: 0)
        }

        cb.commit()
        cb.waitUntilCompleted()
        return display
    }

    // MARK: - Effects

    /// Core Image's working space for the effects. The print is in the output
    /// space's linear primaries; tagging both ends with the same space leaves
    /// it there — nothing is converted — and the effects' own thresholds only
    /// use the sRGB transfer curve, which every output space but ProPhoto shares.
    private static let effectsSpace = CGColorSpace(name: CGColorSpace.extendedLinearSRGB)!

    /// Runs Glow's pipeline over the print and writes the result back. Float
    /// throughout, so a 16-bit TIFF keeps its precision.
    private func applyEffects(_ look: GlowLook, to linear: MTLTexture, commandBuffer cb: MTLCommandBuffer) {
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
        let result = GlowPipeline.apply(look, to: image,
                                        longEdge: CGFloat(max(linear.width, linear.height)))
        context.render(result, to: out, commandBuffer: cb, bounds: image.extent, colorSpace: Self.effectsSpace)
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

    struct GrainUniforms {
        var particlesPerPixel: SIMD3<Float>
        var densityMin: SIMD3<Float>
        var densityMax: SIMD3<Float>
        var uniformity: SIMD3<Float>
        var amount: Float
        var seed: UInt32
        var monochrome: UInt32 = 0
        var pad1: Float = 0
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
                                width: Int, height: Int) -> MTLTexture {
        let pixelSizeUm = Self.pixelSizeUm(width: width, height: height,
                                           filmFormatMM: recipe.filmFormatMM)
        var current = source

        // --- scatter ---
        let scatterAmount = Float(recipe.scatterAmount)
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

    /// agx `model/grain.apply_grain_to_density`, single-layer path.
    private func grainUniforms(grain: GrainDefaults, setup: RenderSetup, recipe: Recipe) -> GrainUniforms {
        let pixelSizeUm = Self.pixelSizeUm(width: Int(imageSize.width),
                                           height: Int(imageSize.height),
                                           filmFormatMM: recipe.filmFormatMM)
        let pixelArea = pixelSizeUm * pixelSizeUm
        let scale = Float(recipe.grainScale)
        var particles = SIMD3<Float>(repeating: 0)
        var densityMin = SIMD3<Float>(repeating: 0)
        var uniformity = SIMD3<Float>(repeating: 0)
        for c in 0..<3 {
            // Larger particles mean fewer per pixel, which is what makes grain
            // coarser rather than merely louder.
            let particleArea = max(grain.particleAreaUm2 * grain.particleScale[c] * scale * scale, 1e-6)
            particles[c] = pixelArea / particleArea
            densityMin[c] = grain.densityMin[c]
            uniformity[c] = grain.uniformity[c]
        }
        let normalizedMax = SIMD3<Float>(setup.film.densityMax[0] - setup.film.densityMin[0],
                                         setup.film.densityMax[1] - setup.film.densityMin[1],
                                         setup.film.densityMax[2] - setup.film.densityMin[2])
        return GrainUniforms(particlesPerPixel: particles,
                             densityMin: densityMin,
                             densityMax: normalizedMax + densityMin,
                             uniformity: uniformity,
                             amount: Float(recipe.grainAmount),
                             seed: 0x5EED_1234,
                             monochrome: setup.film.isMonochrome ? 1 : 0)
    }

    private func mean(_ values: [Float]) -> Float {
        values.isEmpty ? 0 : values.reduce(0, +) / Float(values.count)
    }

    private func blur(_ cb: MTLCommandBuffer, from: MTLTexture, to: MTLTexture, sigma: Float) {
        let kernel = MPSImageGaussianBlur(device: device, sigma: sigma)
        kernel.edgeMode = .clamp
        kernel.encode(commandBuffer: cb, sourceTexture: from, destinationTexture: to)
    }

    private func couplerDiffusionPixels(setup: RenderSetup, recipe: Recipe) -> Float {
        guard let dc = setup.film.defaults.dirCouplers, dc.diffusionSizeUm > 0 else { return 0 }
        let pixelSizeUm = Self.pixelSizeUm(width: Int(imageSize.width),
                                           height: Int(imageSize.height),
                                           filmFormatMM: recipe.filmFormatMM)
        return dc.diffusionSizeUm * Float(recipe.couplersDiffusion) / pixelSizeUm
    }

    /// agx `ResizingService`: the film format spans the long edge, so the
    /// pixel pitch is what converts every micrometre-scale effect to pixels.
    static func pixelSizeUm(width: Int, height: Int, filmFormatMM: Double) -> Float {
        let longEdge = Float(max(width, height))
        guard longEdge > 0 else { return 10 }
        return Float(filmFormatMM) * 1000 / longEdge
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

    /// Linear working-space pixels, for export and for the parity harness.
    func readLinear() -> [Float]? {
        guard let linear = linearOut else { return nil }
        return readback(linear)
    }

    func makeDisplayImage(colorSpaceName: String) -> CGImage? {
        guard let display = displayOut else { return nil }
        let w = display.width, h = display.height
        // A private texture cannot be read directly; stage through a shared
        // one, kept between frames rather than allocated for every one.
        if displayStaging == nil {
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
        return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32,
                       bytesPerRow: w * 4, space: cs,
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
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
        default: return nil
        }
    }
}
