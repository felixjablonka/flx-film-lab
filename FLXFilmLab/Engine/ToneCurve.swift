import Foundation
import simd

/// The print stage's tone controls, baked into a 2D transfer LUT.
///
/// The substrate is agx's fitted paper characteristic curve (a sum of scaled
/// normal CDFs, one per emulsion sub-layer) reshaped by its coupled-gamma
/// morph. On top of that sit NegPy's darkroom controls — split grade, toe and
/// shoulder, regional colour — and the photographic ones, Brightness, Shadows
/// and Highlights, all reformulated so that **every one of them is exactly
/// identity at its default**. That matters twice over: the app at default
/// settings reproduces agx bit-for-bit, which is what the parity tests assert,
/// and a photographer can trust that a slider at zero is doing nothing.
///
/// Controls that are about *how light a tone looks* — which band of the print
/// counts as shadows, where the midtones are — are defined on the print's
/// lightness (`LightnessScale`), not on density. Density is logarithmic, so on
/// an RA-4 paper the shadows take up three quarters of it: weights placed in
/// density reached deep into the midtones from the shadow side while barely
/// clearing paper white on the other.
///
/// NegPy layers its controls on a straight line it draws itself, so it can use
/// softplus knees for the endpoints. Here the paper curve already has its own
/// toe and shoulder, so the same softplus would compress a second time even at
/// zero. The endpoint controls are instead exponential-reach lifts of the
/// paper-black ceiling and paper-white floor: same intent (height plus how far
/// the roll-off reaches), identity at zero by construction.
///
/// The second LUT axis is local grade, so a dodge/burn mask can print through a
/// different contrast filter without needing a LUT of its own.
enum ToneCurve {

    /// NegPy's toe, shoulder and snap geometry is quoted against a 2.3 D-max
    /// paper. Papers here carry their own range, so the constants are rescaled
    /// to mean the same thing on any stock.
    private static let negpyReferenceRange: Float = 2.3
    private static let toeHeight: Float = 0.90
    private static let shoulderHeight: Float = 0.35
    private static let snapWidth: Float = 0.6

    struct Geometry {
        var densityMin: Float
        var densityMax: Float
        var range: Float
        var scale: Float          // range relative to NegPy's reference paper
        var anchor: Float         // density a mid-grey scene prints at
        var lightness: LightnessScale
        /// Where the anchor sits on the lightness scale.
        var midGrey: Float { lightness.lightness(anchor) }
    }

    // MARK: - Lightness

    /// Lightness of the print as a function of channel density: 1 at paper
    /// white, 0 at paper black, linear in CIE L* between — computed from the
    /// medium's own dyes under its own viewing light, for a neutral tone.
    struct LightnessScale {
        let densityWhite: Float
        let densityBlack: Float
        private let forward: [Float]   // lightness at evenly spaced density, white -> black
        private let inverse: [Float]   // density at evenly spaced lightness, 0 -> 1

        init(medium: ProfilePack, viewingIlluminant: String,
             densityWhite: Float, densityBlack: Float, samples n: Int = 513) {
            self.densityWhite = densityWhite
            self.densityBlack = max(densityBlack, densityWhite + 1e-3)
            let shared = PackLibrary.shared.shared_
            let light = shared.illuminant(viewingIlluminant)
            let step = (self.densityBlack - densityWhite) / Float(n - 1)

            var luminance = [Float](repeating: 0, count: n)
            for i in 0..<n {
                let v = densityWhite + step * Float(i)
                var y: Float = 0
                for l in 0..<Constants.wavelengths where medium.spectralValid[l] != 0 {
                    let dyes = medium.channelDensity[l * 3] + medium.channelDensity[l * 3 + 1]
                             + medium.channelDensity[l * 3 + 2]
                    let base = medium.baseDensity.isEmpty ? 0 : medium.baseDensity[l]
                    y += pow(10, -(v * dyes + base)) * light[l] * shared.cmfs[l * 3 + 1]
                }
                luminance[i] = y
            }
            let white = max(luminance[0], 1e-20)
            func lstar(_ y: Float) -> Float {
                let r = y / white
                return r > 0.008856 ? 116 * cbrt(r) - 16 : 903.3 * r
            }
            let black = lstar(luminance[n - 1])
            let span = max(100 - black, 1e-3)
            let forward = luminance.map { (lstar($0) - black) / span }
            self.forward = forward

            var inverse = [Float](repeating: 0, count: n)
            var i = n - 2
            for k in 0..<n {
                let t = Float(k) / Float(n - 1)
                while i > 0 && forward[i] < t { i -= 1 }
                let t0 = forward[i], t1 = forward[i + 1]
                let f = t0 > t1 ? min(max((t0 - t) / (t0 - t1), 0), 1) : 0
                inverse[k] = densityWhite + step * (Float(i) + f)
            }
            self.inverse = inverse
        }

        /// 1 at paper white, 0 at paper black; extrapolated beyond either.
        func lightness(_ v: Float) -> Float {
            let n = forward.count
            let pos = (v - densityWhite) / (densityBlack - densityWhite) * Float(n - 1)
            let i = min(max(Int(pos.rounded(.down)), 0), n - 2)
            return forward[i] + (forward[i + 1] - forward[i]) * (pos - Float(i))
        }

        func density(_ t: Float) -> Float {
            let n = inverse.count
            let pos = min(max(t, 0), 1) * Float(n - 1)
            let i = min(Int(pos.rounded(.down)), n - 2)
            return inverse[i] + (inverse[i + 1] - inverse[i]) * (pos - Float(i))
        }
    }

    /// Brightness, Shadows and Highlights as one curve on lightness.
    ///
    /// Each control moves its own band and nothing else. Brightness peaks at
    /// mid-grey and fades to nothing at paper black and white. Shadows peaks
    /// close to black — the deep shadows, around L* 15 — and fades out just
    /// short of mid-grey; Highlights mirrors it near white. None of them moves
    /// paper black or paper white themselves — that is what Blacks and Whites
    /// are for. The bands are placed around where this paper actually prints an
    /// 18% grey, not around a nominal middle.
    ///
    /// Because each band peaks so near an end that stays fixed, it can lift a
    /// deep shadow a long way but darken it only a little before it would run
    /// into black (and mirrored for highlights). So `shadows` and `highlights`
    /// run from -1 to 1 as a fraction of the most each direction can do while
    /// the curve stays smooth and monotone, and the two directions are not the
    /// same size in lightness. Combined at extremes the controls' slopes could
    /// still cancel and fold the curve back on itself, so the slope is floored
    /// and the curve renormalised: tones compress there instead of reversing.
    struct LightnessCurve {
        private let table: [Float]
        private static let samples = 1024
        private static let slopeFloor: Float = 0.05
        /// Gap either side of mid-grey that Shadows and Highlights leave alone.
        private static let midGap: Float = 0.02
        /// How far from black (white) the Shadows (Highlights) band peaks.
        static let bandPeak: Float = 0.12
        /// Share of the fold-over limit a band uses at full travel.
        private static let margin: Float = 0.85
        /// Largest lightness move either band makes, in either direction.
        private static let bandCeiling: Float = 0.18

        /// Brightness's reach: 1 at mid-grey, easing to 0 at either end.
        static func brightnessProfile(_ t: Float, midGrey m: Float) -> Float {
            guard t > 0, t < 1 else { return 0 }
            let half = t < m ? t / m : (1 - t) / (1 - m)
            let s = sin(Float.pi / 2 * half)
            return s * s * s
        }

        /// 0 at paper black, 1 at `bandPeak`, back to 0 just short of mid-grey.
        static func shadowProfile(_ t: Float, midGrey m: Float) -> Float {
            band(t, from: 0, peak: bandPeak, to: m - midGap)
        }

        /// The mirror image: 0 just past mid-grey, 1 near white, 0 at paper white.
        static func highlightProfile(_ t: Float, midGrey m: Float) -> Float {
            band(t, from: m + midGap, peak: 1 - bandPeak, to: 1)
        }

        /// A smooth, lopsided bump: a quarter sine squared up each side.
        private static func band(_ t: Float, from a: Float, peak p: Float, to b: Float) -> Float {
            guard t > a, t < b else { return 0 }
            let half = t < p ? (t - a) / (p - a) : (b - t) / (b - p)
            let s = sin(Float.pi / 2 * half)
            return s * s
        }

        /// The steepest a quarter-sine-squared edge of width `w` gets is
        /// pi / (2w), so an amplitude below 2w / pi cannot fold the curve.
        private static func limit(edge w: Float) -> Float {
            min(margin * 2 * w / Float.pi, bandCeiling)
        }

        init?(brightness: Float, shadows: Float, highlights: Float, midGrey: Float) {
            guard brightness != 0 || shadows != 0 || highlights != 0 else { return nil }
            let m = min(max(midGrey, 0.3), 0.7)
            // Lifting the shadows steepens the long edge toward mid-grey;
            // darkening them steepens the short edge up from black.
            let shadowAmplitude = shadows * (shadows > 0
                ? Self.limit(edge: m - Self.midGap - Self.bandPeak)
                : Self.limit(edge: Self.bandPeak))
            // Brightening the highlights steepens the short edge down to white;
            // darkening them, the long edge toward mid-grey.
            let highlightAmplitude = highlights * (highlights > 0
                ? Self.limit(edge: Self.bandPeak)
                : Self.limit(edge: 1 - Self.bandPeak - m - Self.midGap))
            let n = Self.samples
            let dt = 1 / Float(n)
            var f = [Float](repeating: 0, count: n + 1)
            for k in 0...n {
                let t = Float(k) * dt
                f[k] = t + brightness * Self.brightnessProfile(t, midGrey: m)
                         + shadowAmplitude * Self.shadowProfile(t, midGrey: m)
                         + highlightAmplitude * Self.highlightProfile(t, midGrey: m)
            }
            var g = [Float](repeating: 0, count: n + 1)
            for k in 0..<n {
                g[k + 1] = g[k] + max(f[k + 1] - f[k], Self.slopeFloor * dt)
            }
            let end = g[n]
            table = g.map { $0 / end }
        }

        init?(recipe: Recipe, geometry g: Geometry) {
            self.init(brightness: Float(recipe.brightness),
                      shadows: Float(recipe.shadows),
                      highlights: Float(recipe.highlights),
                      midGrey: g.midGrey)
        }

        func apply(_ t: Float) -> Float {
            guard t > 0, t < 1 else { return t }
            let pos = t * Float(table.count - 1)
            let i = min(Int(pos), table.count - 2)
            return table[i] + (table[i + 1] - table[i]) * (pos - Float(i))
        }
    }

    /// Which band of the print a tone belongs to, for split contrast and
    /// regional colour. Mid-grey belongs to neither.
    static func zoneWeights(lightness t: Float, midGrey m: Float) -> (shadow: Float, highlight: Float) {
        (1 - smoothstep(0.12, m - 0.05, t), smoothstep(m + 0.05, 0.88, t))
    }

    private static func smoothstep(_ a: Float, _ b: Float, _ x: Float) -> Float {
        let t = min(max((x - a) / (b - a), 0), 1)
        return t * t * (3 - 2 * t)
    }

    // MARK: - Paper

    /// Zone geometry for a paper. The anchor is the density at log-exposure 0,
    /// which is where the enlarger's mid-grey normalisation puts an 18% scene —
    /// and which the gamma morph leaves fixed (scaling both mu and sigma by
    /// 1/gamma cancels at x = 0), so it does not drift as grade changes.
    static func geometry(paper: ProfilePack) -> Geometry {
        var anchor: Float = 0
        for c in 0..<3 {
            anchor += paperDensityRaw(paper: paper, channel: c, x: 0, gamma: 1)
        }
        anchor /= 3

        let dMin = paper.densityMin.min() ?? 0
        let dMax = paper.densityMax.max() ?? 2.3
        let range = max(dMax - dMin, 1e-3)
        return Geometry(densityMin: dMin, densityMax: dMax, range: range,
                        scale: range / negpyReferenceRange, anchor: anchor,
                        lightness: LightnessScale(medium: paper,
                                                  viewingIlluminant: paper.viewingIlluminant,
                                                  densityWhite: dMin, densityBlack: dMax))
    }

    private static func paperDensityRaw(paper: ProfilePack, channel: Int, x: Float, gamma: Float) -> Float {
        FilmMath.paperDensity(model: paper.curveModel, channel: channel,
                              logExposure: x, gamma: gamma, isPositive: paper.isPositive)
    }

    /// The paper's own curve for a neutral tone at a given grade, before any
    /// tone control — what the meters reason about.
    static func neutralPaperDensity(paper: ProfilePack, logExposure x: Float, grade: Float) -> Float {
        let gamma = Constants.gradeReference / clamp(grade, Constants.gradeMin, Constants.gradeMax)
        var d: Float = 0
        for c in 0..<3 { d += paperDensityRaw(paper: paper, channel: c, x: x, gamma: gamma) }
        return d / 3
    }

    /// Density for one channel at one log-exposure, with the full control stack
    /// applied. `localGrade` is the per-pixel ISO-R offset from a mask.
    static func density(paper: ProfilePack,
                        recipe: Recipe,
                        geometry g: Geometry,
                        channel c: Int,
                        logExposure x: Float,
                        localGrade: Float) -> Float {
        // 1 — Grade. Contrast as an ISO range value; slope goes as 1/R, and the
        //     morph's sigma' = sigma/gamma turns that into a real curve stretch.
        let gradeR = clamp(Float(recipe.grade) + localGrade + Float(recipe.gradeTrim[c]),
                           Constants.gradeMin, Constants.gradeMax)
        let gamma = Constants.gradeReference / gradeR
        let v = paperDensityRaw(paper: paper, channel: c, x: x, gamma: gamma)
        return applyControls(v, recipe: recipe, geometry: g, channel: c, gradeR: gradeR)
    }

    /// Steps 2-6 of the stack: everything that acts on each dye layer's
    /// developed density, and so applies whether the substrate is a paper
    /// curve or a transparency. Brightness, Shadows and Highlights come after,
    /// on all three layers at once (`neutralShift`).
    static func applyControls(_ start: Float, recipe: Recipe, geometry g: Geometry,
                              channel c: Int, gradeR: Float) -> Float {
        var v = start

        // Weights are read off the running density, so each block sees the
        // previous block's result.
        func weights(_ v: Float) -> (shadow: Float, highlight: Float) {
            zoneWeights(lightness: g.lightness.lightness(v), midGrey: g.midGrey)
        }

        // 2 — Split contrast: change the slope inside one zone, pivoting on the
        //     midtone anchor so the rest of the scale does not shift.
        //
        //     Rotating about the *zone centre* instead — the obvious reading of
        //     a split-grade print — barely works: the lever arm `(v - z)` is
        //     almost nothing exactly where the control is supposed to bite.
        //     Pivoting on the anchor gives the lever arm the width of the whole
        //     tonal scale, and the zone weight keeps it to its own end.
        if recipe.shadowGrade != 0 || recipe.highlightGrade != 0 {
            let w = weights(v)
            let kShadow = gradeR / max(gradeR + Float(recipe.shadowGrade), 1) - 1
            let kHighlight = gradeR / max(gradeR + Float(recipe.highlightGrade), 1) - 1
            v += (kShadow * w.shadow + kHighlight * w.highlight) * (v - g.anchor)
        }

        // 3 — Regional colour timing. A dichroic filter can only shift a whole
        //     layer in parallel; this is the part it cannot do — a shadow cast
        //     corrected without touching the highlights.
        let shadowCC = ccComponent(recipe.shadowCMY, channel: c)
        let highlightCC = ccComponent(recipe.highlightCMY, channel: c)
        if shadowCC != 0 || highlightCC != 0 {
            let w = weights(v)
            v += shadowCC / 100 * w.shadow + highlightCC / 100 * w.highlight
        }

        // 4 — Snap: anchor-preserving midtone gamma, easing to zero toward both
        //     ends, like a real paper's continuously varying gamma.
        let snap = Float(recipe.snap) + Float(recipe.snapTrim[c])
        if snap != 0 {
            let w = snapWidth * g.scale
            v += snap * w * tanh((v - g.anchor) / w)
        }

        // 5 — Shoulder: hold highlights back from paper white. `lift` is how
        //     far the floor rises, `reach` how far into the scale it carries.
        let shoulder = Float(recipe.shoulder) + Float(recipe.shoulderTrim[c])
        if shoulder != 0 {
            let lift = shoulder * shoulderHeight * g.scale
            let reach = max(Float(recipe.shoulderWidth), 0.05) * 0.5 * g.range
            let d = v - g.densityMin
            v = g.densityMin + max(d + lift * exp(-max(d, 0) / reach), 0)
        }

        // 6 — Toe: lift shadow detail out of paper black, or (negative)
        //     deepen the blacks by pulling the ceiling down onto them.
        let toe = Float(recipe.toe) + Float(recipe.toeTrim[c])
        if toe != 0 {
            let lift = toe * toeHeight * g.scale
            let reach = max(Float(recipe.toeWidth), 0.05) * 0.5 * g.range
            let d = g.densityMax - v
            v = g.densityMax - max(d + lift * exp(-max(d, 0) / reach), 0)
        }

        return max(v, 0)
    }

    /// Kodak CC units for the dye layer that records `channel`.
    private static func ccComponent(_ offset: CMYOffset, channel: Int) -> Float {
        switch channel {
        case 0: return Float(offset.cyan)
        case 1: return Float(offset.magenta)
        default: return Float(offset.yellow)
        }
    }

    /// Build the transfer LUT: width samples along print log-exposure,
    /// height slices along local grade, RGBA32Float (alpha unused).
    static func buildLUT(paper: ProfilePack, recipe: Recipe) -> (data: [Float], geometry: Geometry) {
        let g = geometry(paper: paper)
        let w = Constants.toneLutWidth
        let h = Constants.toneLutHeight
        let logE0 = paper.logExposureStart
        let logE1 = paper.logExposure[paper.logExposure.count - 1]
        let step = (logE1 - logE0) / Float(w - 1)

        var data = [Float](repeating: 0, count: w * h * 4)
        let anyMaskGrade = recipe.masks.contains { $0.enabled && $0.grade != 0 }

        for row in 0..<h {
            let localGrade: Float = anyMaskGrade
                ? Constants.localGradeMin
                    + (Constants.localGradeMax - Constants.localGradeMin) * Float(row) / Float(h - 1)
                : 0
            for col in 0..<w {
                let x = logE0 + Float(col) * step
                let base = (row * w + col) * 4
                for c in 0..<3 {
                    data[base + c] = density(paper: paper, recipe: recipe, geometry: g,
                                             channel: c, logExposure: x, localGrade: localGrade)
                }
                data[base + 3] = 1
            }
        }
        return (data, g)
    }

    // MARK: - Transparency

    /// Zone geometry for a slide viewed directly, with no print stage.
    /// The anchor is the density a mid-grey scene develops to, which the film
    /// exposure normalisation puts at log-exposure 0 — the same construction as
    /// the paper, so the same controls mean the same thing.
    static func transparencyGeometry(film: ProfilePack) -> Geometry {
        var anchor: Float = 0
        for c in 0..<3 {
            anchor += FilmMath.sampleCurves(film.normalizedDensityCurves,
                                            at: SIMD3(repeating: 0),
                                            logE0: film.logExposureStart,
                                            step: film.logExposureStep,
                                            gamma: SIMD3(repeating: 1))[c]
        }
        anchor /= 3

        let dMax = (0..<3).map { film.densityMax[$0] - film.densityMin[$0] }.max() ?? 2.3
        let range = max(dMax, 1e-3)
        return Geometry(densityMin: 0, densityMax: dMax, range: range,
                        scale: range / negpyReferenceRange, anchor: anchor,
                        lightness: LightnessScale(medium: film,
                                                  viewingIlluminant: film.viewingIlluminant,
                                                  densityWhite: 0, densityBlack: dMax))
    }

    /// Transfer LUT for transparency mode, indexed in *density* rather than
    /// print log-exposure. The substrate is the identity — the developed film
    /// already is the picture — with Grade acting as a gain about the anchor,
    /// which is what push/pull development does to a slide.
    static func buildTransparencyLUT(film: ProfilePack, recipe: Recipe)
        -> (data: [Float], geometry: Geometry, axis: (start: Float, step: Float)) {
        let g = transparencyGeometry(film: film)
        let w = Constants.toneLutWidth
        let h = Constants.toneLutHeight
        // The axis starts exactly at zero density. It must not extend below:
        // the control stack clamps its output at zero, so texels for negative
        // density would all store zero and then bleed into the interpolation
        // just above it — a 0.002 density error right where a slide's
        // highlights live.
        let start: Float = 0
        let end = g.densityMax * 1.25
        let step = (end - start) / Float(w - 1)

        var data = [Float](repeating: 0, count: w * h * 4)
        let anyMaskGrade = recipe.masks.contains { $0.enabled && $0.grade != 0 }

        for row in 0..<h {
            let localGrade: Float = anyMaskGrade
                ? Constants.localGradeMin
                    + (Constants.localGradeMax - Constants.localGradeMin) * Float(row) / Float(h - 1)
                : 0
            for col in 0..<w {
                let d = start + Float(col) * step
                let base = (row * w + col) * 4
                for c in 0..<3 {
                    let gradeR = clamp(Float(recipe.grade) + localGrade + Float(recipe.gradeTrim[c]),
                                       Constants.gradeMin, Constants.gradeMax)
                    let gain = Constants.gradeReference / gradeR
                    let v = g.anchor + (d - g.anchor) * gain
                    data[base + c] = applyControls(v, recipe: recipe, geometry: g,
                                                   channel: c, gradeR: gradeR)
                }
                data[base + 3] = 1
            }
        }
        return (data, g, (start, step))
    }

    /// Samples for the inspector's characteristic-curve plot: the same LUT the
    /// GPU reads plus the same neutral shift, so the plot cannot disagree with
    /// the render.
    static func plotSamples(paper: ProfilePack, recipe: Recipe, count: Int = 192)
        -> [(logExposure: Float, density: SIMD3<Float>)] {
        let g = geometry(paper: paper)
        let shift = neutralShift(geometry: g, recipe: recipe)
        let logE0 = paper.logExposureStart
        let logE1 = paper.logExposure[paper.logExposure.count - 1]
        return (0..<count).map { i in
            let x = logE0 + (logE1 - logE0) * Float(i) / Float(count - 1)
            var d = SIMD3<Float>(repeating: 0)
            for c in 0..<3 {
                d[c] = density(paper: paper, recipe: recipe, geometry: g,
                               channel: c, logExposure: x, localGrade: 0)
            }
            if let shift { d += shift.sample((d.x + d.y + d.z) / 3) }
            return (x, d)
        }
    }

    // MARK: - Neutral shift

    /// Brightness, Shadows and Highlights, as a density change applied to all
    /// three dye layers alike, looked up by the pixel's mean density — a
    /// table the print kernel samples after the per-layer LUT.
    ///
    /// Moving the layers together is what keeps hue. The same curve applied to
    /// each layer on its own lifts a skin tone's yellow and magenta (near
    /// mid-grey) far more than its cyan (near white): +0.2 Brightness turned a
    /// face 15 degrees toward yellow. This is how a luminance curve works in
    /// an editor, and how a print-exposure change works on paper.
    struct NeutralShift {
        let densityMax: Float
        let values: [Float]       // density change at evenly spaced mean density

        func sample(_ v: Float) -> Float {
            let n = values.count
            let pos = min(max(v / densityMax, 0), 1) * Float(n - 1)
            let i = min(Int(pos), n - 2)
            return values[i] + (values[i + 1] - values[i]) * (pos - Float(i))
        }
    }

    static let neutralShiftSamples = 1024

    static func neutralShift(geometry g: Geometry, recipe: Recipe) -> NeutralShift? {
        guard let curve = LightnessCurve(recipe: recipe, geometry: g) else { return nil }
        let n = neutralShiftSamples
        let densityMax = g.densityMax * 1.25
        var values = [Float](repeating: 0, count: n)
        for i in 0..<n {
            let v = densityMax * Float(i) / Float(n - 1)
            let t = g.lightness.lightness(v)
            guard t > 0, t < 1 else { continue }
            values[i] = g.lightness.density(curve.apply(t)) - v
        }
        return NeutralShift(densityMax: densityMax, values: values)
    }

    private static func clamp(_ v: Float, _ lo: Float, _ hi: Float) -> Float { min(max(v, lo), hi) }
}
