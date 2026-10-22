import Foundation
import simd

/// CPU-side model evaluation: everything that is cheap enough to redo on the
/// host and would otherwise cost a shader branch. All of it runs in well under
/// a millisecond, so a slider move rebuilds these and re-dispatches.
enum FilmMath {

    // MARK: - Primitives

    static func normalCDF(_ z: Float) -> Float {
        0.5 * (1 + erf(z / Float(2.0.squareRoot())))
    }

    static func softplus(_ t: Float) -> Float {
        // stable form: log(1 + e^t) without overflowing for large t
        max(t, 0) + log1p(exp(-abs(t)))
    }

    static func sigmoid(_ t: Float) -> Float {
        1 / (1 + exp(-t))
    }

    // MARK: - Density curves

    /// agx `morph_curves._evaluate_channel_density` with the coupled-gamma
    /// morph folded in: sigma' = sigma / gamma, mu' = mu / gamma, amplitude fixed.
    /// That scaling is what makes gamma a true contrast control — it stretches
    /// the curve along log-exposure without moving D(0) or D_max.
    static func paperDensity(model: CurveModel,
                             channel: Int,
                             logExposure x: Float,
                             gamma: Float,
                             isPositive: Bool) -> Float {
        var total: Float = 0
        let g = max(gamma, 1e-4)
        for layer in 0..<model.nLayers {
            let (mu, amplitude, sigma) = model[channel: channel, layer: layer]
            let sigmaMorphed = max(sigma / g, 0.05)   // SIGMA_FLOOR in morph_curves
            let muMorphed = mu / g
            var z = (x - muMorphed) / sigmaMorphed
            if isPositive { z = -z }                  // agx `_signed_z`
            total += amplitude * normalCDF(z)
        }
        return total
    }

    /// agx `couplers.compute_dir_couplers_matrix`. Row = donor layer that
    /// releases inhibitor, column = receiving layer whose exposure is cut.
    static func dirCouplerMatrix(_ d: DirCouplerDefaults, amount: Float) -> simd_float3x3 {
        var m = [[Float]](repeating: [0, 0, 0], count: 3)
        for i in 0..<3 { m[i][i] = d.gammaSameLayerRGB[i] * d.inhibitionSameLayer }
        m[0][1] += d.gammaInterlayerRtoGB[0] * d.inhibitionInterLayer
        m[0][2] += d.gammaInterlayerRtoGB[1] * d.inhibitionInterLayer
        m[1][0] += d.gammaInterlayerGtoRB[0] * d.inhibitionInterLayer
        m[1][2] += d.gammaInterlayerGtoRB[1] * d.inhibitionInterLayer
        m[2][0] += d.gammaInterlayerBtoRG[0] * d.inhibitionInterLayer
        m[2][1] += d.gammaInterlayerBtoRG[1] * d.inhibitionInterLayer
        for i in 0..<3 { for j in 0..<3 { m[i][j] *= amount } }
        // simd_float3x3 is column-major; keep [donor][receiver] indexing by
        // storing donor as the column, which is how the shader reads it back.
        return simd_float3x3(columns: (SIMD3(m[0][0], m[0][1], m[0][2]),
                                       SIMD3(m[1][0], m[1][1], m[1][2]),
                                       SIMD3(m[2][0], m[2][1], m[2][2])))
    }

    /// Density curves (n x 3, interleaved) and their axis on a grid `by` times
    /// finer, linearly interpolated, so the curves themselves do not change.
    ///
    /// The pre-coupler curves are built on this grid. Inverting the couplers
    /// squeezes the axis by 1 - b, b the share of a layer's contrast its
    /// inhibition takes; with strong couplers on a slide's steep shadows b
    /// nears 0.9, and resampled back onto agx's 256 samples the inverted curve
    /// loses its shape: the developed slope then steps by up to a third from
    /// one sample to the next where the sheet's is smooth.
    static func oversampled(curves: [Float], logExposure: [Float],
                            by k: Int = Constants.couplerOversampling) -> (curves: [Float], logExposure: [Float]) {
        let n = logExposure.count
        guard n > 1, k > 1 else { return (curves, logExposure) }
        let m = (n - 1) * k + 1
        var axis = [Float](repeating: 0, count: m)
        var out = [Float](repeating: 0, count: m * 3)
        for j in 0..<m {
            let i = min(j / k, n - 2)
            let t = Float(j - i * k) / Float(k)
            axis[j] = logExposure[i] + (logExposure[i + 1] - logExposure[i]) * t
            for c in 0..<3 {
                out[j * 3 + c] = curves[i * 3 + c] + (curves[(i + 1) * 3 + c] - curves[i * 3 + c]) * t
            }
        }
        return (out, axis)
    }

    /// agx `couplers.compute_density_curves_before_dir_couplers`.
    ///
    /// The datasheet curves already include the couplers' effect, so developing
    /// against them and then applying inhibition again would double-count.
    /// This inverts that: the curve the emulsion would have had without couplers.
    static func densityCurvesBeforeCouplers(curves: [Float],
                                            logExposure: [Float],
                                            matrix: simd_float3x3,
                                            densityMax: [Float],
                                            isPositive: Bool) -> [Float] {
        let n = logExposure.count
        var silver = [Float](repeating: 0, count: n * 3)
        for i in 0..<n {
            for c in 0..<3 {
                silver[i * 3 + c] = isPositive ? (densityMax[c] - curves[i * 3 + c]) : curves[i * 3 + c]
            }
        }

        // couplers_amount_curves = density_curves_silver @ matrix, donor -> receiver
        var shiftedAxis = [Float](repeating: 0, count: n * 3)
        for i in 0..<n {
            let s = SIMD3(silver[i * 3], silver[i * 3 + 1], silver[i * 3 + 2])
            for m in 0..<3 {
                let amount = s.x * matrix[0][m] + s.y * matrix[1][m] + s.z * matrix[2][m]
                shiftedAxis[i * 3 + m] = logExposure[i] - amount
            }
        }

        // Resample each channel back onto the original axis.
        //
        // agx writes the positive branch as `-np.interp(x, xp, -fp)`. That pair
        // of negations cancels — np.interp is linear in fp — so both stock types
        // take the same interpolation here. Negating the *axis* instead (which
        // is what the shape of that expression suggests) reverses the sort order
        // and breaks the search.
        var out = [Float](repeating: 0, count: n * 3)
        var xs = [Float](repeating: 0, count: n)
        var ys = [Float](repeating: 0, count: n)
        for c in 0..<3 {
            for i in 0..<n {
                xs[i] = shiftedAxis[i * 3 + c]
                ys[i] = curves[i * 3 + c]
            }
            for i in 0..<n {
                out[i * 3 + c] = interpolate(x: logExposure[i], xs: xs, ys: ys)
            }
        }
        return out
    }

    /// How much of the diffused inhibitor's edge effect to keep, per layer and
    /// log exposure, so fine detail never gains more over a large area than it
    /// does at `reference`.
    ///
    /// A layer whose inhibition takes a share b of its own contrast (the
    /// coupler matrix times the slope of every donor's curve) keeps 1 / (1 - b)
    /// times a large area's contrast in detail finer than the inhibitor's
    /// reach. agx's b grows with the curve's slope, so on a slide's steep
    /// shadows a datasheet-sized edge effect at density 1 becomes a bright rim
    /// at density 2 (Provia 400X's halo). Scaling the diffused part of the
    /// inhibitor by (G_ref - 1) / (G - 1) wherever the gain G passes G_ref holds
    /// the edge there; uniform areas, where the diffused and local inhibitor are
    /// the same, are untouched.
    static func edgeGainScale(curves: [Float], logExposure: [Float], matrix: simd_float3x3,
                              reference: Float) -> [Float] {
        let n = logExposure.count
        guard n > 2 else { return [Float](repeating: 1, count: n * 3) }
        // |dS/dlogE| per donor layer, S the silver: D for a negative, D_max - D for
        // a slide, the same slope either way.
        var slope = [Float](repeating: 0, count: n * 3)
        for i in 0..<n {
            let lo = max(i - 1, 0), hi = min(i + 1, n - 1)
            let dx = logExposure[hi] - logExposure[lo]
            for c in 0..<3 where dx > 0 {
                slope[i * 3 + c] = abs(curves[hi * 3 + c] - curves[lo * 3 + c]) / dx
            }
        }
        func gain(_ i: Int, _ m: Int) -> Float {
            let b = slope[i * 3] * matrix[0][m] + slope[i * 3 + 1] * matrix[1][m] + slope[i * 3 + 2] * matrix[2][m]
            return 1 / max(1 - b, 0.05)
        }
        var out = [Float](repeating: 1, count: n * 3)
        for m in 0..<3 {
            var g = [Float](repeating: 1, count: n)
            for i in 0..<n { g[i] = gain(i, m) }
            let ref = interpolate(x: reference, xs: logExposure, ys: g)
            for i in 0..<n where g[i] > ref {
                out[i * 3 + m] = ref > 1 ? (ref - 1) / (g[i] - 1) : 0
            }
        }
        return out
    }

    /// np.interp: linear, clamped to the endpoint values, non-uniform x allowed.
    static func interpolate(x: Float, xs: [Float], ys: [Float]) -> Float {
        let n = xs.count
        if n == 0 { return 0 }
        if x <= xs[0] { return ys[0] }
        if x >= xs[n - 1] { return ys[n - 1] }
        var lo = 0, hi = n - 1
        while hi - lo > 1 {
            let mid = (lo + hi) / 2
            if xs[mid] <= x { lo = mid } else { hi = mid }
        }
        let dx = xs[hi] - xs[lo]
        if dx == 0 { return ys[hi] }
        let t = (x - xs[lo]) / dx
        return ys[lo] + t * (ys[hi] - ys[lo])
    }

    /// Linear interpolation on the uniform log-exposure grid, per channel,
    /// with agx's endpoint clamping. Mirrors `interpCurves` in flx.metal.
    static func sampleCurves(_ curves: [Float], at x: SIMD3<Float>,
                             logE0: Float, step: Float, gamma: SIMD3<Float>) -> SIMD3<Float> {
        var out = SIMD3<Float>(repeating: 0)
        for c in 0..<3 {
            var pos = (x[c] * gamma[c] - logE0) / step
            pos = min(max(pos, 0), Float(Constants.curveSamples - 1))
            let i0 = Int(pos)
            let i1 = min(i0 + 1, Constants.curveSamples - 1)
            let t = pos - Float(i0)
            out[c] = curves[i0 * 3 + c] * (1 - t) + curves[i1 * 3 + c] * t
        }
        return out
    }

    // MARK: - Enlarger

    /// agx `color_filters.DichroicFilters.apply_cc`. CC units are density x100,
    /// so 100 units is one full density of filtration.
    static func enlargerIlluminant(source: [Float],
                                   dichroics: [Float],
                                   cc: SIMD3<Float>) -> [Float] {
        let transmittance = SIMD3<Float>(pow(10, -cc.x / 100),
                                         pow(10, -cc.y / 100),
                                         pow(10, -cc.z / 100))
        var out = [Float](repeating: 0, count: Constants.wavelengths)
        for l in 0..<Constants.wavelengths {
            var total: Float = 1
            for i in 0..<3 {
                let f = dichroics[l * 3 + i]
                total *= 1 - (1 - f) * (1 - transmittance[i])
            }
            out[l] = source[l] * total
        }
        return out
    }

    /// Spectral density of the developed negative for one CMY density triple.
    static func densitySpectral(cmy: SIMD3<Float>,
                                channelDensity: [Float],
                                baseDensity: [Float],
                                valid: [Float]) -> [Float] {
        var out = [Float](repeating: 0, count: Constants.wavelengths)
        for l in 0..<Constants.wavelengths {
            guard valid[l] != 0 else { out[l] = .infinity; continue }
            out[l] = cmy.x * channelDensity[l * 3 + 0]
                   + cmy.y * channelDensity[l * 3 + 1]
                   + cmy.z * channelDensity[l * 3 + 2]
                   + (baseDensity.isEmpty ? 0 : baseDensity[l])
        }
        return out
    }

    /// agx `printing._compute_exposure_factor_midgray`: normalise the print
    /// exposure so an 18% grey scene lands at the same place on the paper curve
    /// whatever film is loaded. Uses the geometric mean, so it balances colour
    /// as well as level.
    static func midgrayExposureFactor(densitySpectralMidgray: [Float],
                                      printIlluminant: [Float],
                                      paperSensitivity: [Float],
                                      valid: [Float]) -> SIMD3<Float> {
        var raw = SIMD3<Float>(repeating: 0)
        for l in 0..<Constants.wavelengths {
            guard valid[l] != 0 else { continue }
            let light = pow(10, -densitySpectralMidgray[l]) * printIlluminant[l]
            raw.x += light * paperSensitivity[l * 3 + 0]
            raw.y += light * paperSensitivity[l * 3 + 1]
            raw.z += light * paperSensitivity[l * 3 + 2]
        }
        raw = simd_max(raw, SIMD3(repeating: 1e-10))
        let geoMean = exp((log(raw.x) + log(raw.y) + log(raw.z)) / 3)
        return SIMD3(repeating: 1 / geoMean)
    }

    /// Oklab's first stage, D65-relative XYZ -> LMS (Ottosson's M1).
    static let oklabM1 = simd_float3x3(rows: [SIMD3(0.8189330101, 0.3618667424, -0.1288597137),
                                              SIMD3(0.0329845436, 0.9293118715, 0.0361456387),
                                              SIMD3(0.0482003018, 0.2643662691, 0.6338517070)])

    /// CAT02 cone-response matrix, the adaptation colour-science folds into
    /// the scan stage's XYZ -> RGB matrices.
    static let cat02 = simd_float3x3(rows: [SIMD3(0.7328, 0.4296, -0.1624),
                                            SIMD3(-0.7036, 1.6975, 0.0061),
                                            SIMD3(0.0030, 0.0136, 0.9834)])

    /// Von Kries adaptation in CAT02 cone space: the XYZ -> XYZ matrix that
    /// maps `source` onto `target`, scaling everything else the way an eye
    /// adapting from one white to the other would.
    static func vonKriesAdaptation(from source: SIMD3<Float>, to target: SIMD3<Float>) -> simd_float3x3 {
        let gain = (cat02 * target) / simd_max(cat02 * source, SIMD3(repeating: 1e-6))
        return cat02.inverse * simd_float3x3(diagonal: gain) * cat02
    }

    /// agx `printing._compute_raw_preflash`: a uniform pre-exposure through the
    /// negative's base only, which is how a darkroom printer holds highlights.
    static func preflashRaw(baseDensity: [Float],
                            valid: [Float],
                            preflashIlluminant: [Float],
                            paperSensitivity: [Float],
                            exposure: Float) -> SIMD3<Float> {
        guard exposure > 0 else { return SIMD3(repeating: 0) }
        var raw = SIMD3<Float>(repeating: 0)
        for l in 0..<Constants.wavelengths {
            guard valid[l] != 0 else { continue }
            let base = baseDensity.isEmpty ? 0 : baseDensity[l]
            let light = pow(10, -base) * preflashIlluminant[l]
            raw.x += light * paperSensitivity[l * 3 + 0]
            raw.y += light * paperSensitivity[l * 3 + 1]
            raw.z += light * paperSensitivity[l * 3 + 2]
        }
        return raw * exposure
    }
}
