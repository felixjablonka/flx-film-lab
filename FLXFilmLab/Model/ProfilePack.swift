import Foundation
import simd

// Data baked by tools/bake/bake_packs.py from agx-emulsion profiles.
// Flat arrays keep the JSON small and map straight onto Metal buffers; the
// accessors below name the shapes so call sites don't have to do index math.

struct CurveModel: Decodable {
    let modelType: String
    let nChannels: Int
    let nLayers: Int
    let centers: [Float]        // (nChannels, nLayers) row-major
    let amplitudes: [Float]
    let sigmas: [Float]

    subscript(channel c: Int, layer l: Int) -> (center: Float, amplitude: Float, sigma: Float) {
        let i = c * nLayers + l
        return (centers[i], amplitudes[i], sigmas[i])
    }
}

struct DirCouplerDefaults: Decodable {
    let active: Bool
    let amount: Float
    let inhibitionSameLayer: Float
    let inhibitionInterLayer: Float
    let gammaSameLayerRGB: [Float]
    let gammaInterlayerRtoGB: [Float]
    let gammaInterlayerGtoRB: [Float]
    let gammaInterlayerBtoRG: [Float]
    let diffusionSizeUm: Float
    let diffusionTailUm: Float
    let diffusionTailWeight: Float
    /// Where the edge effect is held (log exposure on the pack's axis), for a
    /// stock whose sharpness is fitted to its datasheet's MTF there:
    /// `FilmMath.edgeGainScale`. Absent, the inhibitor's reach is agx's.
    let edgeGainReference: Float?
    /// The share of the inhibitor that spreads before it acts; the rest acts
    /// where it was released. The spread part draws the edge effect, the whole
    /// sets the inter-image colour, so a stock fitted to its sheet keeps both:
    /// edges from its MTF, colour from its statement. Absent, all of it (agx).
    let diffusionFraction: Float?
}

struct HalationDefaults: Decodable {
    let active: Bool
    let scatterCoreUm: [Float]
    let scatterTailUm: [Float]
    let scatterTailWeight: [Float]
    let halationStrength: [Float]
    let halationFirstSigmaUm: [Float]
    let halationNBounces: Int
    let halationBounceDecay: Float
    let halationRenormalize: Bool
    let boostRange: Float
    let protectEV: Float
}

/// A camera filter a film can be shot through, with its own exposure LUT
/// baked from the filter's transmittance (tools/bake/lens_filters.py): Kodak's
/// Wratten filters for black and white, Tiffen's warming filters for slides.
struct LensFilterRef: Decodable, Hashable, Identifiable {
    let id: String
    let name: String
    /// The Wratten number, for Kodak's filters; a Tiffen filter's name is its
    /// own designation.
    let wratten: String?
    let file: String
    /// How much the filter cuts a mid-grey's exposure on this film. The LUT
    /// already opens up by it, as a photographer would.
    let factor: Float

    /// "Red, Wratten 25", or a Tiffen filter's own name, "Tiffen 81C". Plain
    /// ASCII, as it goes into a .cube's comments.
    var designation: String { wratten.map { "\(name), Wratten \($0)" } ?? name }
}

struct GrainDefaults: Decodable {
    let particleAreaUm2: Float
    let particleScale: [Float]
    let densityMin: [Float]
    let uniformity: [Float]
    let blur: Float
    let microStructure: [Float]
}

struct StockDefaults: Decodable {
    let dirCouplers: DirCouplerDefaults?
    let halation: HalationDefaults?
    let grain: GrainDefaults?
}

struct TcLutRef: Decodable {
    let file: String
    let size: Int
    let channels: Int
    let format: String
}

struct ProfilePack: Decodable {
    let stock: String
    let name: String
    let type: String            // negative | positive
    let support: String         // film | paper
    let stage: String           // filming | printing
    let antihalation: String
    /// "color", or "bw" for a single silver layer carried as three identical
    /// channels (tools/bake/build_bw_profiles.py).
    let channelModel: String?
    let targetPrint: String?
    let referenceIlluminant: String
    let viewingIlluminant: String

    let logExposure: [Float]            // (256,)
    let densityCurves: [Float]          // (256, 3) row-major
    let densityCurvesLayers: [Float]    // (256, 3, 3)
    let densityMax: [Float]
    let densityMin: [Float]

    let sensitivity: [Float]            // (81, 3) linear, zero where unmeasured
    let channelDensity: [Float]         // (81, 3)
    let baseDensity: [Float]            // (81,) or empty
    let spectralValid: [Float]          // (81,)

    let curveModel: CurveModel
    let defaults: StockDefaults
    let rgbToXYZ: [Float]?              // filming stocks only, (3, 3) row-major
    let tcLut: TcLutRef?
    /// Black-and-white films only.
    let lensFilters: [LensFilterRef]?
    /// The softest paper grade, ISO R, that Contrast and Auto Contrast may
    /// reach with this film: absent for all but a film far harder than a
    /// colour negative (HARMAN Phoenix 200), whose softest grade is scaled by
    /// how much harder (`tools/bake/bake_packs.py`, `softest_grade`).
    let gradeSoftest: Float?
    /// The grade, ISO R, this film opens at, where it is not R115: Phoenix
    /// 200's, a little softer than its curves print (Felix's pick).
    let gradeDefault: Float?

    var isPositive: Bool { type == "positive" }
    /// `gradeSoftest`, or the grade range's own soft end.
    var softestGrade: Float { gradeSoftest ?? Constants.gradeMax }
    /// `gradeDefault`, or the neutral R115.
    var defaultGrade: Float { gradeDefault ?? Constants.gradeReference }
    var isFilmStage: Bool { stage == "filming" }
    /// One silver layer: the picture is neutral whatever the filtration, and
    /// there are no dyes for couplers or colour controls to act on.
    var isMonochrome: Bool { channelModel == "bw" }

    /// The filter a recipe names, if this stock can be shot through it.
    func lensFilter(_ id: String?) -> LensFilterRef? {
        guard let id else { return nil }
        return lensFilters?.first { $0.id == id }
    }

    /// Uniform axis, so downstream code can index instead of searching.
    var logExposureStart: Float { logExposure[0] }
    var logExposureStep: Float { (logExposure[logExposure.count - 1] - logExposure[0]) / Float(logExposure.count - 1) }

    var rgbToXYZMatrix: simd_float3x3 {
        guard let m = rgbToXYZ, m.count == 9 else { return matrix_identity_float3x3 }
        // JSON is row-major; simd_float3x3(columns:) wants columns.
        return simd_float3x3(columns: (SIMD3(m[0], m[3], m[6]),
                                       SIMD3(m[1], m[4], m[7]),
                                       SIMD3(m[2], m[5], m[8])))
    }

    /// agx `develop()` works on curves shifted so each channel's minimum is zero.
    var normalizedDensityCurves: [Float] {
        var out = densityCurves
        for c in 0..<3 {
            let m = densityMin[c]
            for i in 0..<Constants.curveSamples { out[i * 3 + c] -= m }
        }
        return out
    }

}

struct PackIndexEntry: Decodable, Identifiable, Hashable {
    let stock: String
    let name: String
    let type: String
    let support: String
    let targetPrint: String?
    let channelModel: String?
    let use: String?                // agx's "still" or "cine"
    var id: String { stock }
    var isMonochrome: Bool { channelModel == "bw" }
    var isMotionPicture: Bool { use == "cine" }
}

extension PackIndexEntry {
    /// The stock pickers' sections, in the order they are listed: films by
    /// kind (motion picture negatives apart from still ones), papers by
    /// colour, motion picture print film and silver.
    static func groups(_ entries: [PackIndexEntry], films: Bool) -> [(title: String, entries: [PackIndexEntry])] {
        let monochrome = entries.filter(\.isMonochrome)
        let colour = entries.filter { !$0.isMonochrome }
        let still = colour.filter { !$0.isMotionPicture }
        let cine = colour.filter(\.isMotionPicture)
        let parts: [(String, [PackIndexEntry])] = films
            ? [("Colour negative", still.filter { $0.type != "positive" }),
               ("Motion picture", cine),
               ("Colour slide", still.filter { $0.type == "positive" }),
               ("Black and white", monochrome)]
            : [("Colour", still), ("Motion picture", cine), ("Black and white", monochrome)]
        return parts.filter { !$0.1.isEmpty }.map { (title: $0.0, entries: $0.1) }
    }
}

struct PackIndex: Decodable {
    let filming: [PackIndexEntry]
    let printing: [PackIndexEntry]
}

struct SharedData: Decodable {
    let wavelengths: [Float]
    let nWavelengths: Int
    let cmfs: [Float]                                   // (81, 3)
    let illuminants: [String: [Float]]                  // name -> (81,)
    let dichroicFilters: [Float]                        // (81, 3) in CMY order
    let neutralPrintFilters: [String: [String: [String: [Float]]]]  // paper -> illuminant -> film -> [c, m, y]
    let workingColorSpace: String
    let xyzToRGB: [String: [String: [Float]]]           // illuminant -> space -> (3, 3) row-major
    let outputColorSpaces: [String]

    func illuminant(_ name: String) -> [Float] {
        illuminants[name] ?? illuminants["D50"] ?? [Float](repeating: 1, count: Constants.wavelengths)
    }

    func xyzToRGBMatrix(viewing: String, output: String) -> simd_float3x3 {
        guard let m = xyzToRGB[viewing]?[output], m.count == 9 else { return matrix_identity_float3x3 }
        return simd_float3x3(columns: (SIMD3(m[0], m[3], m[6]),
                                       SIMD3(m[1], m[4], m[7]),
                                       SIMD3(m[2], m[5], m[8])))
    }

    func cmfNormalization(viewing: String) -> Float {
        let illum = illuminant(viewing)
        var sum: Float = 0
        for l in 0..<Constants.wavelengths { sum += illum[l] * cmfs[l * 3 + 1] }
        return sum
    }

    func illuminantXYZ(viewing: String) -> SIMD3<Float> {
        let illum = illuminant(viewing)
        var xyz = SIMD3<Float>(repeating: 0)
        for l in 0..<Constants.wavelengths {
            xyz.x += illum[l] * cmfs[l * 3 + 0]
            xyz.y += illum[l] * cmfs[l * 3 + 1]
            xyz.z += illum[l] * cmfs[l * 3 + 2]
        }
        return xyz / cmfNormalization(viewing: viewing)
    }

    /// The Y/M/C values that print a neutral 18% grey for this film + paper.
    /// agx `params_builder.apply_database_neutral_print_filters`.
    func neutralFilters(paper: String, illuminant: String, film: String) -> SIMD3<Float>? {
        guard let v = neutralPrintFilters[paper]?[illuminant]?[film], v.count == 3 else { return nil }
        return SIMD3(v[0], v[1], v[2])   // c, m, y
    }
}
