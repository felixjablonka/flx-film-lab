import Foundation

/// Per-channel trim, for crossover correction: one dye layer moved relative to
/// the shared curve. NegPy's "Global / R / G / B" selector on the Tone page.
struct RGBTrim: Codable, Equatable {
    var red: Double = 0
    var green: Double = 0
    var blue: Double = 0

    static let zero = RGBTrim()
    var isZero: Bool { red == 0 && green == 0 && blue == 0 }
    subscript(i: Int) -> Double {
        get { i == 0 ? red : (i == 1 ? green : blue) }
        set { if i == 0 { red = newValue } else if i == 1 { green = newValue } else { blue = newValue } }
    }
}

/// Subtractive filtration, in Kodak CC units. Positive c/m/y removes that
/// colour's complement, exactly like winding a dichroic head.
struct CMYOffset: Codable, Equatable {
    var cyan: Double = 0
    var magenta: Double = 0
    var yellow: Double = 0

    static let zero = CMYOffset()
    var isZero: Bool { cyan == 0 && magenta == 0 && yellow == 0 }
}

enum FilmFormat: String, Codable, CaseIterable, Identifiable {
    case thirtyFiveMM = "35 mm"
    case mediumFormat = "120"
    var id: String { rawValue }

    /// Long edge of the frame. A 6x6 negative is roughly 2.5x the area of a
    /// 35 mm one, which is why the same emulsion looks so much finer on 120.
    var frameWidthMM: Double {
        switch self {
        case .thirtyFiveMM: return 36
        case .mediumFormat: return 56
        }
    }
}

enum MaskShape: String, Codable, CaseIterable {
    case polygon    // N points, Catmull-Rom smoothed — a cut card
    case oval       // 3 points: centre and two axis ends, so tilt and shear are free
    case gradient   // 2 points: the graduated card edge
}

/// A darkroom mask. Vertices live in normalised image coordinates so they
/// survive resizing between the proxy and full-resolution renders.
struct LocalMask: Codable, Equatable, Identifiable {
    var id = UUID()
    var name: String = "Mask"
    var shape: MaskShape = .polygon
    var points: [CGPoint] = []
    var stops: Double = 0          // +burn / -dodge, NegPy's ±2
    var feather: Double = 0.04     // sigma as a fraction of the short side
    var grade: Double = 0          // ISO-R points off the frame's grade; negative = harder
    var inverted: Bool = false
    var enabled: Bool = true
}

/// Exposure compensation that fades across the frame, given to the film when
/// the picture is taken: the capture-side counterpart of Dodge & burn's
/// gradient, which works on the print.
///
/// Because it changes what the film receives, a darkened sky moves down the
/// film's own curve, off a slide's clear base or a negative's shoulder, and
/// keeps the detail a burn could not bring back. It changes nothing else: the
/// meters read the frame as if it were not there. Several stack, each adding
/// its own compensation where it covers.
struct ExposureGradient: Codable, Equatable, Identifiable {
    var id = UUID()
    var name: String = "Gradient"
    /// The darkroom gradient's two handles, in normalised image coordinates:
    /// the full compensation at and behind the first, none at and past the
    /// second. Their spacing is the transition, close together for a hard edge.
    var points: [CGPoint] = [CGPoint(x: 0.5, y: 0.3), CGPoint(x: 0.5, y: 0.55)]
    /// Stops of exposure on the covered side, like the Exposure slider:
    /// positive gives the film more light, negative less.
    var exposureEV: Double = -1
    var enabled: Bool = true

    var isActive: Bool { enabled && exposureEV != 0 && points.count >= 2 }

    /// The compensation at `p`, a pixel position in a `width` x `height`
    /// frame: the darkroom gradient's smoothstep along the handles. The
    /// film-exposure kernel (`gradientEV` in flx.metal) computes the same.
    func exposureEV(atX x: Float, y: Float, width: Float, height: Float) -> Float {
        guard isActive else { return 0 }
        let ax = Float(points[0].x) * width, ay = Float(points[0].y) * height
        let dx = Float(points[1].x) * width - ax, dy = Float(points[1].y) * height - ay
        let lengthSquared = dx * dx + dy * dy
        guard lengthSquared > 1e-9 else { return 0 }
        let t = min(max(((x - ax) * dx + (y - ay) * dy) / lengthSquared, 0), 1)
        return Float(exposureEV) * (1 - t * t * (3 - 2 * t))
    }

    /// What a stack adds up to at `p`: each gradient's compensation, summed.
    static func exposureEV(_ stack: [ExposureGradient], atX x: Float, y: Float,
                           width: Float, height: Float) -> Float {
        stack.reduce(0) { $0 + $1.exposureEV(atX: x, y: y, width: width, height: height) }
    }
}

struct Recipe: Codable, Equatable {
    // MARK: Stocks
    var filmStock: String = "kodak_portra_400"
    var printStock: String = "kodak_portra_endura"

    // MARK: Negative exposure
    /// Each auto flag means "this value is the meter's". While it is on, the
    /// meter's reading replaces the stored value when rendering, and the
    /// slider shows that reading; touching the slider stores the new value
    /// and turns the flag off. The stored value is only ever the user's.
    var exposureEV: Double = 0
    var autoExposure: Bool = true
    /// The camera filter in front of a black-and-white film ("red", ...), or
    /// none. Exposure is opened up by its filter factor, so it moves colours,
    /// not the picture's brightness. Ignored for stocks without filters.
    var lensFilter: String?
    /// Graduated exposure compensation, at most `Constants.maxExposureGradients`.
    /// Optional so a recipe saved before they existed still loads; read and
    /// write them through `gradients`.
    var exposureGradients: [ExposureGradient]? = nil
    var gradients: [ExposureGradient] {
        get { exposureGradients ?? [] }
        set { exposureGradients = newValue.isEmpty ? nil : newValue }
    }

    // MARK: The print's shape
    /// Turns, mirror, levelling and crop (`FrameGeometry`). Optional so a
    /// recipe saved before it existed still loads; read and write it through
    /// `geometry`. Like the masks, it belongs to its frame: pasted settings
    /// and a newly opened image leave it out.
    var frameGeometry: FrameGeometry? = nil
    var geometry: FrameGeometry {
        get { frameGeometry ?? .identity }
        set { frameGeometry = newValue == .identity ? nil : newValue }
    }

    // MARK: Print — brightness and contrast
    /// Midtone brightness, as a shift in the print's lightness at mid-grey, in
    /// fractions of the range from paper black to paper white. Positive is
    /// lighter. It fades to nothing at both ends, so paper black and paper
    /// white stay where the paper puts them.
    var brightness: Double = 0
    /// Print exposure in stops — the enlarger timer, which moves every tone,
    /// endpoints included. NegPy calls this Density; more light = darker print.
    var density: Double = 0
    /// Auto Brightness (the stored flag predates the rename).
    var autoDensity: Bool = false
    /// Contrast as an ISO range value. 115 is neutral, lower is harder.
    var grade: Double = Double(Constants.gradeReference)
    var autoGrade: Bool = false
    /// Zone-local contrast — the split-grade print. ISO-R points, negative = harder.
    var shadowGrade: Double = 0
    var highlightGrade: Double = 0

    // MARK: Print — how shadows and highlights render
    /// Lightness moves confined to one band of the print — the dark half for
    /// shadows, the light half for highlights — strongest near black (white)
    /// and fading to nothing at mid-grey and at paper black / paper white.
    /// -1...1, as a share of the most the band can move in that direction
    /// (`ToneCurve.LightnessCurve`); positive is lighter.
    var shadows: Double = 0
    var highlights: Double = 0
    /// Endpoint roll-off. Positive toe lifts shadow detail out of paper black,
    /// positive shoulder holds highlights back from paper white.
    var toe: Double = 0                  // ±1
    var shoulder: Double = 0             // ±1
    /// How far each roll-off reaches into the tonal scale.
    var toeWidth: Double = 1             // 0.1...5
    var shoulderWidth: Double = 1
    /// Anchor-preserving midtone gamma.
    var snap: Double = 0                 // ±0.5

    // Per-layer trims (crossover correction)
    var gradeTrim = RGBTrim.zero         // ±30 ISO-R points
    var toeTrim = RGBTrim.zero           // ±1
    var shoulderTrim = RGBTrim.zero      // ±1
    var snapTrim = RGBTrim.zero          // ±0.5

    // MARK: Colour timing
    /// Global timing moves the real dichroic filters, so it is a physical
    /// change to the printing light rather than a tint on the output.
    var filterShiftY: Double = 0
    var filterShiftM: Double = 0
    /// Regional timing acts in density space with the same mid-sparing weights
    /// as Zone Density, which a filter shift cannot do — a filter can only move
    /// a whole layer in parallel.
    var shadowCMY = CMYOffset.zero
    var highlightCMY = CMYOffset.zero

    // MARK: Enlarger
    var preflashExposure: Double = 0
    var preflashY: Double = 0
    var preflashM: Double = 0

    // MARK: Film rendering
    var densityCurveGamma: Double = 1.0
    var couplersEnabled: Bool = true
    var couplersAmount: Double = 1.0
    var couplersDiffusion: Double = 1.0   // scales the stock's diffusion_size_um
    var halationEnabled: Bool = true
    var halationAmount: Double = 1.0
    var halationScale: Double = 1.0
    var scatterAmount: Double = 1.0
    var scatterScale: Double = 1.0
    var grainEnabled: Bool = true
    var grainAmount: Double = 1.0
    var grainScale: Double = 1.0
    var glareEnabled: Bool = true
    var glareAmount: Double = 0.03

    // MARK: Local
    var masks: [LocalMask] = []

    // MARK: Effects
    /// Glow's bloom and glow (FLXFilmLab/Effects, shared with the Motion mode), on
    /// the finished print. `nil` until first touched, which is all off.
    var effects: GlowLook?

    // MARK: Output
    /// Skip the print stage and read the developed film directly, the way a
    /// slide is viewed on a light table. `nil` means follow the stock: a
    /// positive stock printed onto RA-4 paper inverts, which is physically
    /// correct and almost never what you want.
    var scanFilm: Bool? = nil
    /// Transparency only: white-balance the scan on the bare film base.
    ///
    /// agx's own `scan_film` reports absolute transmittance, so a base density
    /// of 0.1 leaves the clear base at 79% and the whole slide reads flat. A
    /// light table, a projector and a scanner all take the base as their white
    /// point — which is why a projected slide looks contrasty and the raw
    /// transmittance does not. Off reproduces agx exactly; the parity harness
    /// turns it off for that reason.
    var normalizeToBase: Bool = true
    var outputColorSpace: String = "sRGB"
    var gamutCompress: Bool = true
    /// Film format drives the pixel pitch, which is what sets the scale of
    /// grain, halation and coupler diffusion. Only the two formats anyone
    /// actually shoots: a 35 mm frame and a 6x6 on 120.
    var filmFormat: FilmFormat = .thirtyFiveMM
    var filmFormatMM: Double { filmFormat.frameWidthMM }

    /// The recipe a newly opened image starts from: this one's look — film,
    /// paper, how it is printed, the film character and the output — with
    /// everything dialled for one particular frame (exposure, tone, colour,
    /// masks) back at its default. So are the autos, whatever the last image
    /// had: Auto Exposure on, Auto Brightness and Auto Contrast off.
    func carryingLook() -> Recipe {
        var r = Recipe()
        r.filmStock = filmStock
        r.grade = r.defaultGrade
        r.printStock = printStock
        r.lensFilter = lensFilter
        r.scanFilm = scanFilm
        r.normalizeToBase = normalizeToBase
        r.densityCurveGamma = densityCurveGamma
        r.couplersEnabled = couplersEnabled
        r.couplersAmount = couplersAmount
        r.couplersDiffusion = couplersDiffusion
        r.halationEnabled = halationEnabled
        r.halationAmount = halationAmount
        r.halationScale = halationScale
        r.scatterAmount = scatterAmount
        r.scatterScale = scatterScale
        r.grainEnabled = grainEnabled
        r.grainAmount = grainAmount
        r.grainScale = grainScale
        r.glareEnabled = glareEnabled
        r.glareAmount = glareAmount
        r.effects = effects
        r.filmFormat = filmFormat
        r.outputColorSpace = outputColorSpace
        r.gamutCompress = gamutCompress
        return r
    }

    /// True when every tone control sits at its identity value. The engine uses
    /// this for the parity tests: at defaults the output must match agx.
    var toneIsNeutral: Bool {
        density == 0 && brightness == 0
            && grade == Double(Constants.gradeReference)
            && shadowGrade == 0 && highlightGrade == 0
            && shadows == 0 && highlights == 0
            && toe == 0 && shoulder == 0 && snap == 0
            && gradeTrim.isZero && toeTrim.isZero && shoulderTrim.isZero && snapTrim.isZero
            && shadowCMY.isZero && highlightCMY.isZero
    }
}

extension GlowLook {
    /// Everything off: FLX Film Lab's starting point. (Glow's own default has
    /// bloom on.)
    static var off: GlowLook {
        var look = GlowLook()
        look.bloom.enabled = false
        return look
    }

    var isActive: Bool { bloom.isActive || glow.isActive || halation.isActive }
}
