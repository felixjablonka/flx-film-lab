import Foundation
import simd

/// Turns a `Recipe` into everything the GPU needs that is not a per-pixel
/// uniform. This is the host-side half of the port: the parts of agx's
/// pipeline that are per-*image* rather than per-pixel — the enlarger's
/// filtered illuminant, the mid-grey exposure normalisation, the pre-coupler
/// curve inversion, the paper transfer LUT.
enum RenderSetupBuilder {

    /// agx uses an 18% grey card as the reference the whole print chain is
    /// balanced against (`printing._compute_exposure_factor_midgray`).
    static let midgrayValue: Float = 0.184

    /// `exposureEV` is the exposure the negative actually receives — the
    /// metered value plus the user's trim when Auto Exposure is on. It travels
    /// in the setup so the film stage cannot be handed a different number than
    /// the meters read; `nil` falls back to the recipe's own slider.
    static func build(recipe: Recipe, exposureEV: Float? = nil) throws -> RenderSetup {
        let library = PackLibrary.shared
        let film = try library.pack(recipe.filmStock)
        let paper = try library.pack(recipe.printStock)
        let shared = library.shared_

        // A positive stock printed onto RA-4 paper comes out inverted. That is
        // exactly what would happen in a lab, and exactly what nobody wants, so
        // slides default to being read directly off the light table.
        let scanFilm = recipe.scanFilm ?? film.isPositive

        // --- Enlarger -------------------------------------------------------
        // Neutral filtration comes from agx's measured table for this exact
        // film + paper + lamp; the shifts are the user's colour timing on top.
        let enlargerIlluminantName = paper.referenceIlluminant
        let neutral = try neutralFiltration(film: film, paper: paper)

        let source = shared.illuminant(enlargerIlluminantName)
        let printIlluminant = FilmMath.enlargerIlluminant(
            source: source,
            dichroics: shared.dichroicFilters,
            cc: SIMD3(neutral.x,
                      neutral.y + Float(recipe.filterShiftM),
                      neutral.z + Float(recipe.filterShiftY)))

        let preflashIlluminant = FilmMath.enlargerIlluminant(
            source: source,
            dichroics: shared.dichroicFilters,
            cc: SIMD3(neutral.x,
                      neutral.y + Float(recipe.preflashM),
                      neutral.z + Float(recipe.preflashY)))

        // --- Mid-grey balance ----------------------------------------------
        // Push an 18% grey through the film stage on the CPU, then normalise
        // the print exposure by the geometric mean of what the paper sees. This
        // is what makes a grey card print grey whichever stock is loaded.
        let negativeExposureEV = exposureEV ?? Float(recipe.exposureEV)
        let midgrayDensitySpectral = try filmDensitySpectral(
            film: film, rgb: SIMD3(repeating: midgrayValue), exposureEV: 0,
            gamma: Float(recipe.densityCurveGamma),
            lensFilter: film.lensFilter(recipe.lensFilter))

        let paperSensitivity = paper.sensitivity
        // Normalise the print exposure against an 18% grey at the *reference*
        // exposure, not the user's.
        //
        // agx's default does the opposite (`print_exposure_compensation`), which
        // re-times the enlarger for whatever the negative exposure was. That is
        // the darkroom reflex — a denser negative gets a longer print time — and
        // it makes the Exposure control invisible: it changes the film's
        // response but not the picture's brightness, so the slider looks broken.
        //
        // Holding the print time fixed is also what agx itself does under
        // `lut_mode`, which is what the parity references are generated with, so
        // this costs nothing there. Exposure now does both halves of what
        // over- or under-exposing film does: the negative sits somewhere else on
        // its curve *and* the print comes out lighter or darker.
        let exposureFactor = FilmMath.midgrayExposureFactor(
            densitySpectralMidgray: midgrayDensitySpectral,
            printIlluminant: printIlluminant,
            paperSensitivity: paperSensitivity,
            valid: film.spectralValid)

        let preflashRaw = FilmMath.preflashRaw(
            baseDensity: film.baseDensity,
            valid: film.spectralValid,
            preflashIlluminant: preflashIlluminant,
            paperSensitivity: paperSensitivity,
            exposure: Float(recipe.preflashExposure))

        // --- Spectral tables -------------------------------------------------
        var tables = [Float](repeating: 0, count: Constants.spectralTableFloats)
        func put(_ values: [Float], at offset: Int, count: Int) {
            for i in 0..<min(count, values.count) { tables[offset + i] = values[i] }
        }
        put(film.channelDensity, at: Constants.offFilmChannelDensity, count: Constants.wavelengths * 3)
        put(film.baseDensity, at: Constants.offFilmBaseDensity, count: Constants.wavelengths)
        put(film.spectralValid, at: Constants.offFilmValid, count: Constants.wavelengths)
        put(printIlluminant, at: Constants.offPrintIlluminant, count: Constants.wavelengths)
        put(paperSensitivity, at: Constants.offPaperSensitivity, count: Constants.wavelengths * 3)
        // In transparency mode the "print" slots carry the film's own dyes, so
        // the scan stage reads the developed negative instead of a print.
        let scanned = scanFilm ? film : paper
        put(scanned.channelDensity, at: Constants.offPrintChannelDensity, count: Constants.wavelengths * 3)
        put(scanned.baseDensity, at: Constants.offPrintBaseDensity, count: Constants.wavelengths)
        put(scanned.spectralValid, at: Constants.offPrintValid, count: Constants.wavelengths)
        // Colour balance has to act somewhere. Printing, it winds the
        // enlarger's dichroics. Reading a transparency there is no enlarger, so
        // the filters go on the viewing light instead — which is what putting a
        // filter on a light table actually does, and keeps the control
        // spectral rather than a per-channel fudge.
        //
        // With the opposite sign: yellow in the enlarger makes a print *cooler*
        // (the negative-positive inversion), yellow on a light table makes a
        // slide warmer. The shifts are print-equivalent filtration, so they are
        // negated here and Temperature and Tint move the picture the same way
        // in both modes.
        let viewingLight = scanFilm
            ? FilmMath.enlargerIlluminant(
                source: shared.illuminant(scanned.viewingIlluminant),
                dichroics: shared.dichroicFilters,
                cc: SIMD3(0, -Float(recipe.filterShiftM), -Float(recipe.filterShiftY)))
            : shared.illuminant(scanned.viewingIlluminant)
        put(viewingLight, at: Constants.offViewingIlluminant, count: Constants.wavelengths)
        put(shared.cmfs, at: Constants.offCMFs, count: Constants.wavelengths * 3)

        // --- Transparency white point -----------------------------------------
        // Clear film base becomes the light table's own white, the way an eye
        // or a scanner white-points on it: a von Kries adaptation from the
        // base to the illuminant, in cone space, so the base's tint goes too.
        //
        // The target has to be the illuminant's white rather than XYZ (1,1,1).
        // The output matrices adapt *from the illuminant*, so an equal-energy
        // white comes out blue-magenta — which is what every slide did, by
        // about b* -10 at mid-grey.
        //
        // The base is measured under the *unfiltered* light, so Temperature
        // and Tint (filters on the light table) still tint the slide instead
        // of being white-pointed straight back out.
        var scanAdaptation = matrix_identity_float3x3
        if scanFilm && recipe.normalizeToBase {
            let neutralLight = shared.illuminant(scanned.viewingIlluminant)
            var baseXYZ = SIMD3<Float>(repeating: 0)
            for l in 0..<Constants.wavelengths {
                guard scanned.spectralValid[l] != 0 else { continue }
                let base = scanned.baseDensity.isEmpty ? 0 : scanned.baseDensity[l]
                let light = pow(10, -base) * neutralLight[l]
                baseXYZ.x += light * shared.cmfs[l * 3 + 0]
                baseXYZ.y += light * shared.cmfs[l * 3 + 1]
                baseXYZ.z += light * shared.cmfs[l * 3 + 2]
            }
            baseXYZ /= shared.cmfNormalization(viewing: scanned.viewingIlluminant)
            scanAdaptation = FilmMath.vonKriesAdaptation(
                from: baseXYZ, to: shared.illuminantXYZ(viewing: scanned.viewingIlluminant))
        }

        // --- DIR couplers ----------------------------------------------------
        let couplerDefaults = film.defaults.dirCouplers
        let couplerMatrix = couplerDefaults.map {
            FilmMath.dirCouplerMatrix($0, amount: Float(recipe.couplersAmount))
        } ?? simd_float3x3(diagonal: SIMD3(repeating: 0))

        let normalizedCurves = film.normalizedDensityCurves
        // The couplers act on the min-subtracted curves, so D-max has to be the
        // normalized one too — otherwise a positive stock's silver density
        // (D_max - D) is offset by the base density.
        let normalizedDensityMax = SIMD3<Float>(film.densityMax[0] - film.densityMin[0],
                                                film.densityMax[1] - film.densityMin[1],
                                                film.densityMax[2] - film.densityMin[2])
        // Both on the finer grid the coupler pass reads (Constants.couplerOversampling).
        let fine = FilmMath.oversampled(curves: normalizedCurves, logExposure: film.logExposure)
        let curvesBefore = (recipe.couplersEnabled && couplerDefaults != nil)
            ? FilmMath.densityCurvesBeforeCouplers(curves: fine.curves,
                                                   logExposure: fine.logExposure,
                                                   matrix: couplerMatrix,
                                                   densityMax: [normalizedDensityMax.x,
                                                                normalizedDensityMax.y,
                                                                normalizedDensityMax.z],
                                                   isPositive: film.isPositive)
            : fine.curves

        let edgeScale: [Float]
        if recipe.couplersEnabled, let reference = couplerDefaults?.edgeGainReference {
            edgeScale = FilmMath.edgeGainScale(curves: fine.curves, logExposure: fine.logExposure,
                                               matrix: couplerMatrix, reference: reference)
        } else {
            edgeScale = []
        }

        // --- Tone stage -------------------------------------------------------
        let toneLUT: [Float]
        let geometry: ToneCurve.Geometry
        let toneAxis: (start: Float, step: Float)
        if scanFilm {
            let built = ToneCurve.buildTransparencyLUT(film: film, recipe: recipe)
            toneLUT = built.data
            geometry = built.geometry
            toneAxis = built.axis
        } else {
            let built = ToneCurve.buildLUT(paper: paper, recipe: recipe, softest: film.softestGrade)
            toneLUT = built.data
            geometry = built.geometry
            toneAxis = (paper.logExposureStart,
                        (paper.logExposure[paper.logExposure.count - 1] - paper.logExposureStart)
                            / Float(Constants.toneLutWidth - 1))
        }

        let toneShift = ToneCurve.neutralShift(geometry: geometry, recipe: recipe)

        // --- Uniforms ---------------------------------------------------------
        let develop = DevelopUniforms(
            logE0: film.logExposureStart,
            logEStep: film.logExposureStep,
            gammaFactor: SIMD3(repeating: Float(recipe.densityCurveGamma)),
            densityMax: normalizedDensityMax,
            couplersAmount: Float(recipe.couplersAmount),
            isPositive: film.isPositive ? 1 : 0,
            edgeCap: edgeScale.isEmpty ? 0 : 1,
            diffusionFraction: couplerDefaults?.diffusionFraction ?? 1)

        let print = PrintUniforms(
            // The transparency adaptation rides in the output matrix. Viewing
            // glare is the only thing added between the two, and it is off for
            // a transparency.
            xyzToRGB: shared.xyzToRGBMatrix(viewing: scanned.viewingIlluminant,
                                            output: recipe.outputColorSpace) * scanAdaptation,
            exposureFactorMidgray: exposureFactor,
            preflashRaw: preflashRaw,
            // Density is a print-exposure change in stops, exactly like moving
            // the enlarger timer — so it rides the paper curve rather than
            // scaling the output.
            printExposure: pow(2, Float(recipe.density)),
            cmfNormalization: shared.cmfNormalization(viewing: scanned.viewingIlluminant),
            toneLogE0: toneAxis.start,
            toneLogEStep: toneAxis.step,
            toneLutWidth: Int32(Constants.toneLutWidth),
            gradeSpanMin: Constants.localGradeMin,
            gradeSpanMax: Constants.localGradeMax,
            hasDodgeBurn: recipe.masks.contains(where: { $0.enabled }) ? 1 : 0,
            // agx applies viewing glare to prints only — a transparency is lit
            // from behind, so there is no print surface to scatter off.
            glareAmount: (recipe.glareEnabled && !scanFilm) ? Float(recipe.glareAmount) / 100 : 0,
            illuminantXYZ: shared.illuminantXYZ(viewing: scanned.viewingIlluminant),
            gamutCompress: recipe.gamutCompress ? 1 : 0,
            // agx OutputGamutCompressSpec.knee as retuned upstream (experimental
            // branch, July 2026): only the last 5% before the gamut edge rolls
            // off, so in-gamut colour is left alone. The original (0, 1, 6)
            // took chroma off every saturated colour — a fifth to a half of
            // the pixels in an ordinary frame moved by more than 2 dE.
            gamutThreshold: 0.95,
            gamutLimit: 1.0,
            gamutPower: 1.6,
            scanFilm: scanFilm ? 1 : 0,
            // The D65 entry adapts D65 to the space's own white, so its inverse
            // takes output RGB back to the D65-relative XYZ Oklab is built on —
            // including for ProPhoto's D50.
            gamutRGBToLMS: FilmMath.oklabM1
                * shared.xyzToRGBMatrix(viewing: "D65", output: recipe.outputColorSpace).inverse,
            gamutLMSToRGB: shared.xyzToRGBMatrix(viewing: "D65", output: recipe.outputColorSpace)
                * FilmMath.oklabM1.inverse,
            toneShiftMax: toneShift?.densityMax ?? 0,
            toneShiftCount: Int32(toneShift?.values.count ?? 0))

        return RenderSetup(film: film,
                           paper: paper,
                           toneGeometry: geometry,
                           printUniforms: print,
                           developUniforms: develop,
                           couplerMatrix: couplerMatrix,
                           curvesBeforeCouplers: curvesBefore,
                           edgeScale: edgeScale,
                           normalizedCurves: normalizedCurves,
                           spectralTables: tables,
                           toneLUT: toneLUT,
                           neutralFilters: neutral,
                           exposureEV: negativeExposureEV,
                           lensFilter: film.lensFilter(recipe.lensFilter),
                           exposureGradients: Array(recipe.gradients.filter(\.isActive)
                                                        .prefix(Constants.maxExposureGradients)),
                           toneShift: toneShift?.values ?? [])
    }

    // MARK: - Neutral filtration

    /// The filtration that prints this film's mid-grey neutral on this paper.
    ///
    /// - Black-and-white paper: none. A silver paper prints neutral through any
    ///   filtration, which only changes how much light it gets, and the
    ///   mid-grey normalisation takes that back out.
    /// - A pair in agx's measured table: its values.
    /// - Any other negative, which agx's table does not cover: a black-and-white
    ///   one on colour paper, or a colour stock built here from a datasheet.
    ///   Solved, for what agx's values achieve — a mid-grey that prints
    ///   neutral. With agx's colour-negative default instead, a neutral
    ///   black-and-white negative on RA-4 paper came out strongly cyan-blue:
    ///   there is no orange mask for that filtration to cancel. On the colour
    ///   pairs the table does cover, the solve lands within 0.3 of agx's
    ///   magenta and 1.7 of its yellow, which leaves mid-grey a shade cooler.
    static func neutralFiltration(film: ProfilePack, paper: ProfilePack) throws -> SIMD3<Float> {
        if paper.isMonochrome { return SIMD3(0, 0, 0) }
        let shared = PackLibrary.shared.shared_
        if let measured = shared.neutralFilters(paper: paper.stock,
                                                illuminant: paper.referenceIlluminant,
                                                film: film.stock) {
            return measured
        }
        guard !film.isPositive else { return SIMD3(0, 65, 55) }   // agx EnlargerParams defaults
        let key = film.stock + "|" + paper.stock
        neutralLock.lock()
        if let solved = solvedNeutral[key] { neutralLock.unlock(); return solved }
        neutralLock.unlock()
        let solved = try solveNeutralFiltration(film: film, paper: paper)
        neutralLock.lock()
        solvedNeutral[key] = solved
        neutralLock.unlock()
        return solved
    }

    private static var solvedNeutral: [String: SIMD3<Float>] = [:]
    private static let neutralLock = NSLock()

    /// Magenta and yellow (no cyan, as in agx's table) that print this film's
    /// mid-grey neutral: Newton on a 2x2 numerical Jacobian of the print's
    /// (a*, b*), from agx's defaults. Exact for a black-and-white negative,
    /// which has no couplers or other spatial effect to leave out; a colour
    /// negative's couplers are pre-compensated out of a neutral grey, so it
    /// holds there too.
    static func solveNeutralFiltration(film: ProfilePack, paper: ProfilePack) throws -> SIMD3<Float> {
        let mid = try filmDensitySpectral(film: film, rgb: SIMD3(repeating: midgrayValue),
                                          exposureEV: 0, gamma: 1)
        var my = SIMD2<Float>(65, 55)
        for _ in 0..<40 {
            let f = printedMidGrey(film: film, paper: paper, midDensity: mid, filters: SIMD3(0, my.x, my.y))
            if simd_length(f) < 0.005 { break }
            let step: Float = 0.5
            let dm = (printedMidGrey(film: film, paper: paper, midDensity: mid,
                                     filters: SIMD3(0, my.x + step, my.y)) - f) / step
            let dy = (printedMidGrey(film: film, paper: paper, midDensity: mid,
                                     filters: SIMD3(0, my.x, my.y + step)) - f) / step
            let jacobian = simd_float2x2(columns: (dm, dy))
            guard abs(jacobian.determinant) > 1e-12 else { break }
            my = simd_clamp(my - jacobian.inverse * f, SIMD2(repeating: 0), SIMD2(repeating: 400))
        }
        return SIMD3(0, my.x, my.y)
    }

    /// The (a*, b*) a mid-grey prints at, against the viewing light: the CPU
    /// half of `print_and_scan` for one neutral patch at the default tone
    /// controls — enlarger, mid-grey normalisation, paper curve, scan.
    static func printedMidGrey(film: ProfilePack, paper: ProfilePack,
                               midDensity: [Float], filters: SIMD3<Float>) -> SIMD2<Float> {
        let shared = PackLibrary.shared.shared_
        let lamp = FilmMath.enlargerIlluminant(source: shared.illuminant(paper.referenceIlluminant),
                                               dichroics: shared.dichroicFilters, cc: filters)
        var raw = SIMD3<Float>(repeating: 0)
        for l in 0..<Constants.wavelengths where film.spectralValid[l] != 0 {
            raw += pow(10, -midDensity[l]) * lamp[l]
                * SIMD3(paper.sensitivity[l * 3], paper.sensitivity[l * 3 + 1], paper.sensitivity[l * 3 + 2])
        }
        raw = simd_max(raw, SIMD3(repeating: 1e-20))
        let mean = (log10(raw.x) + log10(raw.y) + log10(raw.z)) / 3
        let logRaw = SIMD3(log10(raw.x), log10(raw.y), log10(raw.z)) - mean
        let cmy = SIMD3<Float>((0..<3).map {
            FilmMath.paperDensity(model: paper.curveModel, channel: $0, logExposure: logRaw[$0],
                                  gamma: 1, isPositive: paper.isPositive)
        })
        let viewing = shared.illuminant(paper.viewingIlluminant)
        var xyz = SIMD3<Float>(repeating: 0)
        for l in 0..<Constants.wavelengths where paper.spectralValid[l] != 0 {
            let d = cmy.x * paper.channelDensity[l * 3] + cmy.y * paper.channelDensity[l * 3 + 1]
                + cmy.z * paper.channelDensity[l * 3 + 2] + (paper.baseDensity.isEmpty ? 0 : paper.baseDensity[l])
            xyz += pow(10, -d) * viewing[l] * SIMD3(shared.cmfs[l * 3], shared.cmfs[l * 3 + 1], shared.cmfs[l * 3 + 2])
        }
        xyz /= shared.cmfNormalization(viewing: paper.viewingIlluminant)
        let t = xyz / shared.illuminantXYZ(viewing: paper.viewingIlluminant)
        func f(_ v: Float) -> Float { v > 0.008856 ? cbrt(v) : 7.787 * v + 16 / 116 }
        return SIMD2(500 * (f(t.x) - f(t.y)), 200 * (f(t.y) - f(t.z)))
    }

    /// agx `filming._simple_rgb_to_density_spectral`: one RGB triple all the way
    /// to the negative's spectral density. Uses the stock's own (not
    /// min-subtracted) curves, matching `develop_simple`.
    static func filmDensitySpectral(film: ProfilePack,
                                    rgb: SIMD3<Float>,
                                    exposureEV: Float,
                                    gamma: Float,
                                    lensFilter: LensFilterRef? = nil) throws -> [Float] {
        let (lut, size) = try PackLibrary.shared.tcLut(for: film, lensFilter: lensFilter)
        let xyz = film.rgbToXYZMatrix * rgb
        var b = xyz.x + xyz.y + xyz.z
        if !b.isFinite { b = 0 }
        let tc = tcCoordinate(xyz: xyz, b: b, matrix: film.rgbToXYZMatrix, monochrome: film.isMonochrome)
        var raw = sampleTcLutCPU(lut, size: size, tc: tc) * b
        raw *= pow(2, exposureEV)
        let logRaw = SIMD3<Float>(log10(max(raw.x, 0) + 1e-10),
                                  log10(max(raw.y, 0) + 1e-10),
                                  log10(max(raw.z, 0) + 1e-10))
        let cmy = FilmMath.sampleCurves(film.densityCurves, at: logRaw,
                                        logE0: film.logExposureStart,
                                        step: film.logExposureStep,
                                        gamma: SIMD3(repeating: gamma))
        return FilmMath.densitySpectral(cmy: cmy,
                                        channelDensity: film.channelDensity,
                                        baseDensity: film.baseDensity,
                                        valid: film.spectralValid)
    }

    static func tri2quad(_ xy: SIMD2<Float>) -> SIMD2<Float> {
        let y = xy.y / max(1 - xy.x, 1e-10)
        let x = (1 - xy.x) * (1 - xy.x)
        return simd_clamp(SIMD2(x, y), SIMD2(repeating: 0), SIMD2(repeating: 1))
    }

    /// For a black-and-white stock, a chromaticity outside the xy triangle (a
    /// negative X, Y or Z) is pulled back along the line to white onto the
    /// triangle's edge rather than clamped onto the LUT's; colour stocks keep
    /// agx's clamp (see `intoTriangle` in flx.metal, which this mirrors).
    static func intoTriangle(_ xy: SIMD2<Float>, white: SIMD2<Float>) -> SIMD2<Float> {
        let d = xy - white
        var t: Float = 1
        let s = d.x + d.y
        if s > 0 { t = min(t, (1 - white.x - white.y) / s) }
        if d.x < 0 { t = min(t, -white.x / d.x) }
        if d.y < 0 { t = min(t, -white.y / d.y) }
        return white + d * max(t, 0)
    }

    /// The LUT coordinate for one XYZ under a stock's input matrix.
    static func tcCoordinate(xyz: SIMD3<Float>, b: Float, matrix: simd_float3x3,
                             monochrome: Bool) -> SIMD2<Float> {
        var xy = SIMD2(xyz.x, xyz.y) / max(b, 1e-10)
        if monochrome {
            let w = matrix * SIMD3<Float>(repeating: 1)
            xy = intoTriangle(xy, white: SIMD2(w.x, w.y) / (w.x + w.y + w.z))
        }
        return tri2quad(xy)
    }

    /// Mitchell–Netravali cubic with reflected edges — the CPU twin of
    /// `sampleTcLut` in flx.metal, kept in sync by construction.
    static func sampleTcLutCPU(_ lut: [Float], size L: Int, tc: SIMD2<Float>) -> SIMD3<Float> {
        func mitchell(_ t: Float) -> Float {
            let B: Float = 1.0 / 3, C: Float = 1.0 / 3
            let x = abs(t), x2 = x * x, x3 = x2 * x
            if x < 1 { return (1.0 / 6) * ((12 - 9 * B - 6 * C) * x3 + (-18 + 12 * B + 6 * C) * x2 + (6 - 2 * B)) }
            if x < 2 { return (1.0 / 6) * ((-B - 6 * C) * x3 + (6 * B + 30 * C) * x2 + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) }
            return 0
        }
        func safeIndex(_ i: Int) -> Int {
            if i < 0 { return -i }
            if i >= L { return 2 * (L - 1) - i }
            return i
        }
        var base = SIMD2<Int>(0, 0)
        var frac = SIMD2<Float>(0, 0)
        let coord = simd_clamp(tc, SIMD2(repeating: 0), SIMD2(repeating: 1)) * Float(L - 1)
        for d in 0..<2 {
            if coord[d] >= Float(L - 1) { base[d] = L - 2; frac[d] = 1 }
            else { base[d] = Int(coord[d].rounded(.down)); frac[d] = coord[d] - Float(base[d]) }
        }
        var acc = SIMD3<Float>(repeating: 0)
        var weightSum: Float = 0
        for i in 0..<4 {
            let xi = safeIndex(base.x - 1 + i)
            let wx = mitchell(frac.x + 1 - Float(i))
            for j in 0..<4 {
                let yj = safeIndex(base.y - 1 + j)
                let w = wx * mitchell(frac.y + 1 - Float(j))
                weightSum += w
                let o = (xi * L + yj) * 3
                acc += w * SIMD3(lut[o], lut[o + 1], lut[o + 2])
            }
        }
        return weightSum != 0 ? acc / weightSum : acc
    }

    // MARK: - Print metering

    /// What the meters read off a frame, in the tone LUT's input units: print
    /// log-exposure when printing, developed density when reading a
    /// transparency. Either way, more is darker.
    struct FrameMetering {
        /// The darkest textured tone (98th percentile) and the lightest (2nd).
        var shadow: Float
        var highlight: Float
        /// A robust middle: the mean of the 5-95% window averaged with that
        /// window's midpoint — NegPy's metered tone.
        var middle: Float
        /// Share of the frame's sectors that carried texture and so voted.
        var texturedFraction: Float
        var isDensity: Bool
        var range: Float { max(shadow - highlight, 1e-3) }
    }

    /// Lightness the darkest and lightest textured tones should print at, as
    /// fractions of the way from paper black to paper white: a black that still
    /// holds detail, and a white that is still tone rather than bare paper.
    static let autoShadowTarget: Float = 0.07
    static let autoHighlightTarget: Float = 0.93
    /// How much of the way Auto Brightness pulls the frame's middle tone to
    /// mid-grey. Partway — Auto Exposure has already done most of the work on
    /// the negative — so a low-key or high-key frame keeps its mood. At 0.6 a
    /// stormy lake came out as a grey midday.
    static let autoBrightnessStrength: Float = 0.4

    /// Meter a frame the way a printer reads a negative: block averages on a
    /// coarse grid, each pushed through the film and the enlarger, and only the
    /// parts of the frame that carry texture get a vote.
    ///
    /// The texture gate is NegPy's (after Boyack & Juenger, US 5,724,456): a
    /// 2x2 sector of cells votes only if its cells differ by more than 0.05 in
    /// log exposure. Otherwise sky, a black backdrop or a flat wall decides
    /// the grade of the whole print. If almost nothing passes, everything votes.
    ///
    /// Print exposure (the enlarger timer) is left at unity so the reading does
    /// not chase that control.
    static func meterFrame(pixels: [Float], width: Int, height: Int,
                           setup: RenderSetup, exposureEV: Float, scanFilm: Bool,
                           gridLongEdge: Int = 128) -> FrameMetering? {
        guard width > 0, height > 0, pixels.count >= width * height * 4 else { return nil }
        let film = setup.film
        guard let (lut, lutSize) = try? PackLibrary.shared.tcLut(for: film, lensFilter: setup.lensFilter) else { return nil }

        // Block averages in linear light: the mean of the light, which is what
        // a spot meter or a densitometer aperture would see.
        let cell = max(max(width, height) / gridLongEdge, 1)
        let gw = max(width / cell, 1), gh = max(height / cell, 1)
        var grid = [SIMD3<Float>](repeating: .zero, count: gw * gh)
        pixels.withUnsafeBufferPointer { px in
            for gy in 0..<gh {
                for y in (gy * cell)..<min((gy + 1) * cell, height) {
                    var o = y * width * 4
                    for gx in 0..<gw {
                        var sum = SIMD3<Float>.zero
                        for _ in 0..<cell where o + 2 < px.count {
                            sum += SIMD3(px[o], px[o + 1], px[o + 2])
                            o += 4
                        }
                        grid[gy * gw + gx] += sum
                    }
                }
            }
        }
        let perCell = 1 / Float(cell * cell)

        let matrix = film.rgbToXYZMatrix
        let exposureScale = pow(2, exposureEV) as Float
        let curves = film.normalizedDensityCurves
        let tables = setup.spectralTables
        let factor = setup.printUniforms.exposureFactorMidgray
        var values = [Float](repeating: 0, count: gw * gh)
        for i in 0..<grid.count {
            let xyz = matrix * (grid[i] * perCell)
            var b = xyz.x + xyz.y + xyz.z
            if !b.isFinite { b = 0 }
            let tc = tcCoordinate(xyz: xyz, b: b, matrix: matrix, monochrome: film.isMonochrome)
            let raw = simd_max(sampleTcLutCPU(lut, size: lutSize, tc: tc) * b * exposureScale,
                               SIMD3(repeating: 0))
            let logRaw = SIMD3(log10(raw.x + 1e-10), log10(raw.y + 1e-10), log10(raw.z + 1e-10))
            let cmy = FilmMath.sampleCurves(curves, at: logRaw,
                                            logE0: film.logExposureStart,
                                            step: film.logExposureStep, gamma: SIMD3(repeating: 1))
            if scanFilm {
                // Transparency: the developed film is what the tone stage
                // shapes, so meter its density directly.
                values[i] = (cmy.x + cmy.y + cmy.z) / 3
                continue
            }
            var printRaw = SIMD3<Float>(repeating: 0)
            for l in 0..<Constants.wavelengths where tables[Constants.offFilmValid + l] != 0 {
                let d = cmy.x * tables[Constants.offFilmChannelDensity + l * 3 + 0]
                      + cmy.y * tables[Constants.offFilmChannelDensity + l * 3 + 1]
                      + cmy.z * tables[Constants.offFilmChannelDensity + l * 3 + 2]
                      + tables[Constants.offFilmBaseDensity + l]
                let light = pow(10, -d) * tables[Constants.offPrintIlluminant + l]
                printRaw += light * SIMD3(tables[Constants.offPaperSensitivity + l * 3 + 0],
                                          tables[Constants.offPaperSensitivity + l * 3 + 1],
                                          tables[Constants.offPaperSensitivity + l * 3 + 2])
            }
            printRaw *= factor
            values[i] = log10(max((printRaw.x + printRaw.y + printRaw.z) / 3, 0) + 1e-10)
        }

        // Texture gate over 2x2 sectors of cells.
        var votes: [Float] = []
        votes.reserveCapacity(values.count)
        let sw = gw / 2, sh = gh / 2
        var active = 0
        for sy in 0..<sh {
            for sx in 0..<sw {
                let i = 2 * sy * gw + 2 * sx
                let q = [values[i], values[i + 1], values[i + gw], values[i + gw + 1]]
                if (q.max() ?? 0) - (q.min() ?? 0) > 0.05 {
                    votes += q
                    active += 1
                }
            }
        }
        let fraction = Float(active) / Float(max(sw * sh, 1))
        if fraction < 0.05 { votes = values }
        guard votes.count >= 8 else { return nil }
        votes.sort()
        func percentile(_ q: Float) -> Float {
            let position = q * Float(votes.count - 1)
            let i = Int(position)
            let t = position - Float(i)
            return votes[i] * (1 - t) + votes[min(i + 1, votes.count - 1)] * t
        }
        let lo = percentile(0.05), hi = percentile(0.95)
        let window = votes.filter { $0 >= lo && $0 <= hi }
        let windowMean = window.isEmpty ? 0.5 * (lo + hi) : window.reduce(0, +) / Float(window.count)
        return FrameMetering(shadow: percentile(0.98), highlight: percentile(0.02),
                             middle: 0.5 * (windowMean + 0.5 * (lo + hi)),
                             texturedFraction: fraction, isDensity: scanFilm)
    }

    /// One reading of a frame for the print meters, with the setup it was
    /// taken through. Taking it is the expensive part (the whole frame through
    /// film and enlarger); applying it is cheap, so the app keeps a reading
    /// while only the controls downstream of it move.
    struct PrintReading {
        var probe: RenderSetup
        var metering: FrameMetering
        var scanFilm: Bool
    }

    static func readPrint(recipe: Recipe, pixels: [Float], width: Int, height: Int,
                          exposureEV: Float) throws -> PrintReading? {
        // Meter at the exposure the negative actually receives. Exposure comes
        // first and is authoritative — it decides where the scene sits on the
        // film — and the print meters then read that result and correct what
        // is left.
        let probe = try build(recipe: recipe, exposureEV: exposureEV)
        let scanFilm = probe.printUniforms.scanFilm != 0
        guard let metering = meterFrame(pixels: pixels, width: width, height: height,
                                        setup: probe, exposureEV: exposureEV,
                                        scanFilm: scanFilm) else { return nil }
        return PrintReading(probe: probe, metering: metering, scanFilm: scanFilm)
    }

    /// The Auto Brightness and Auto Contrast toggles, resolved against a
    /// reading: with either on, the meter's value replaces the stored one. The
    /// app shows that value on the slider; moving the slider stores a value of
    /// its own and turns the toggle off.
    static func applyAuto(recipe: Recipe, reading: PrintReading) -> Recipe {
        var resolved = recipe
        if recipe.autoGrade {
            resolved.grade = autoGrade(reading.metering, setup: reading.probe, scanFilm: reading.scanFilm)
        }
        if recipe.autoDensity {
            resolved.brightness = autoBrightness(reading.metering, setup: reading.probe,
                                                 grade: Float(resolved.grade), scanFilm: reading.scanFilm)
        }
        return resolved
    }

    /// Read and apply in one go.
    static func resolveAuto(recipe: Recipe, pixels: [Float], width: Int, height: Int,
                            exposureEV: Float)
        throws -> (recipe: Recipe, metering: FrameMetering?) {
        guard recipe.autoDensity || recipe.autoGrade else { return (recipe, nil) }
        guard let reading = try readPrint(recipe: recipe, pixels: pixels, width: width,
                                          height: height, exposureEV: exposureEV) else {
            return (recipe, nil)
        }
        return (applyAuto(recipe: recipe, reading: reading), reading.metering)
    }

    /// Auto Contrast: fit the frame onto the paper.
    ///
    /// The textured range, darkest tone to lightest, has to land between a
    /// black that still holds detail and a white that is still tone. Where the
    /// negative carries more range than that, the grade softens until it fits
    /// (a tenth of overflow allowed) — that is what kept shadows from being
    /// crushed. Where the frame is flatter than the paper, the grade hardens,
    /// but only a third of the way and never past R92: a flat scene is allowed
    /// to stay a little flat, rather than being pushed until it turns garish.
    ///
    /// The earlier meter compared the frame's 10-90% spread with the paper's
    /// whole useful range, which reads every ordinary scene as flat; it went as
    /// hard as R61 on a portrait and never once looked at where the shadows
    /// landed.
    static func autoGrade(_ m: FrameMetering, setup: RenderSetup, scanFilm: Bool) -> Double {
        let reference = Constants.gradeReference
        let g = setup.toneGeometry
        let darkest = g.lightness.density(autoShadowTarget)
        let lightest = g.lightness.density(autoHighlightTarget)
        let span: Float
        if scanFilm {
            span = darkest - lightest     // the substrate is density in, density out
        } else {
            span = logExposure(paper: setup.paper, atNeutralDensity: darkest)
                 - logExposure(paper: setup.paper, atNeutralDensity: lightest)
        }
        guard span > 1e-3 else { return Double(reference) }
        // Grade scales the curve along its input about the anchor, so a slope
        // factor f is simply grade R115 / f.
        let ratio = span / m.range
        let slope: Float = ratio < 1 ? min(ratio * 1.1, 1) : min(0.7 + 0.3 * ratio, 1.25)
        return Double(min(max(reference / slope, Constants.gradeMin), setup.film.softestGrade))
    }

    /// Auto Brightness: pull the frame's middle tone partway to where an 18%
    /// grey prints, as a move of the same midtone curve the Brightness slider
    /// drives — so paper black and paper white stay put, as they do with the
    /// slider.
    static func autoBrightness(_ m: FrameMetering, setup: RenderSetup, grade: Float,
                               scanFilm: Bool) -> Double {
        let g = setup.toneGeometry
        let reference = Constants.gradeReference
        let middleDensity: Float = scanFilm
            ? g.anchor + (m.middle - g.anchor) * reference / max(grade, 1)
            : ToneCurve.neutralPaperDensity(paper: setup.paper, logExposure: m.middle, grade: grade,
                                            softest: setup.film.softestGrade)
        let tMiddle = g.lightness.lightness(middleDensity)
        let want = autoBrightnessStrength * (g.midGrey - tMiddle)
        // The curve moves a tone by its profile there. Compensate for that only
        // down to 60% of mid-grey's own reach, so a frame that is nearly all
        // shadow is not lifted by a huge midtone push.
        let reach = max(ToneCurve.LightnessCurve.brightnessProfile(min(max(tMiddle, 0), 1),
                                                                   midGrey: g.midGrey), 0.6)
        return Double(min(max(want / reach, -brightnessLimit), brightnessLimit))
    }

    /// Largest midtone move either the slider or the meter asks for.
    static let brightnessLimit: Float = 0.2

    /// Where the paper, at the reference grade, reaches a neutral density.
    static func logExposure(paper: ProfilePack, atNeutralDensity target: Float) -> Float {
        var lo = paper.logExposureStart
        var hi = paper.logExposure[paper.logExposure.count - 1]
        for _ in 0..<40 {
            let mid = 0.5 * (lo + hi)
            if ToneCurve.neutralPaperDensity(paper: paper, logExposure: mid,
                                             grade: Constants.gradeReference) < target {
                lo = mid
            } else {
                hi = mid
            }
        }
        return 0.5 * (lo + hi)
    }

    /// How many stops of highlight the stock holds above a mid-grey exposure.
    ///
    /// Mid-grey sits at log-exposure zero by construction, so this is just how
    /// far up the curve there is before it runs out — the film's own headroom,
    /// which is what a photographer is protecting when they meter for
    /// highlights.
    static func highlightHeadroomStops(film: ProfilePack) -> Float {
        let curves = film.normalizedDensityCurves
        let n = Constants.curveSamples
        var mean = [Float](repeating: 0, count: n)
        for i in 0..<n { mean[i] = (curves[i * 3] + curves[i * 3 + 1] + curves[i * 3 + 2]) / 3 }
        let lo = mean.min() ?? 0, hi = mean.max() ?? 1
        guard hi > lo else { return 4 }
        // For a positive stock density falls with exposure, so "used up" is the
        // low-density end either way.
        let target = film.isPositive ? lo + 0.05 * (hi - lo) : lo + 0.95 * (hi - lo)
        var limit = film.logExposure[n - 1]
        for i in 0..<n where (film.isPositive ? mean[i] <= target : mean[i] >= target) {
            limit = film.logExposureStart + Float(i) * film.logExposureStep
            break
        }
        return max(limit / log10(2.0 as Float), 1)
    }

    /// Meter the negative exposure.
    ///
    /// The old version was agx's centre-weighted average aimed at 18% grey,
    /// which normalises every scene to mid-grey — a night shot came out looking
    /// like noon, and it fought Auto Density, which is also trying to set
    /// brightness. The two jobs are different: *this* decides where the scene
    /// sits on the film's latitude, and Auto Density decides how bright the
    /// print is.
    ///
    /// So this meters like someone shooting the stock would: place the
    /// highlights inside the film's headroom, never push further than the
    /// median asks for, and then only go part of the way — a frame that was
    /// deliberately exposed dark stays dark.
    static func autoExposureEV(pixels: [Float], width: Int, height: Int, film: ProfilePack) -> Float {
        guard width > 0, height > 0 else { return 0 }
        let m = film.rgbToXYZMatrix

        // Sample rather than sweep: percentiles are stable well below full
        // resolution, and this runs on every reload.
        let total = width * height
        let stride = max(total / 200_000, 1)
        var luminances: [Float] = []
        luminances.reserveCapacity(total / stride + 1)
        var i = 0
        while i < total {
            let o = i * 4
            let rgb = SIMD3(pixels[o], pixels[o + 1], pixels[o + 2])
            let y = m[0][1] * rgb.x + m[1][1] * rgb.y + m[2][1] * rgb.z
            if y.isFinite && y > 0 { luminances.append(y) }
            i += stride
        }
        guard luminances.count > 32 else { return 0 }
        luminances.sort()

        func percentile(_ q: Float) -> Float {
            let position = q * Float(luminances.count - 1)
            let idx = Int(position)
            let t = position - Float(idx)
            return luminances[idx] * (1 - t) + luminances[min(idx + 1, luminances.count - 1)] * t
        }

        let median = percentile(0.50)
        let highlight = percentile(0.995)

        // Where the median alone would put mid-grey.
        let fromMedian = -log2(median / midgrayValue)
        // The most exposure that still keeps the brightest tones on the curve,
        // holding back half a stop so the very top does not sit on the shoulder.
        let headroom = highlightHeadroomStops(film: film) - 0.5
        let fromHighlight = headroom - log2(highlight / midgrayValue)

        var ev = min(fromMedian, fromHighlight)
        guard ev.isFinite else { return 0 }

        // Partial pull, the same principle as Auto Density: correct toward a
        // good exposure without erasing the one the photographer chose.
        ev *= 0.7
        return min(max(ev, -4), 4)
    }
}
