import Foundation
import simd

/// A print look as a 3D lookup table: log footage (N-Log, V-Log, Apple Log,
/// Apple Log 2, F-Log, F-Log2, Canon Log 2 and 3, S-Log3 or I-Log) in, display
/// RGB out.
///
/// Everything the engine does per pixel — film exposure, development, the
/// couplers' colour effect, the enlarger, the paper and its tone controls, the
/// scan — is a fixed function of the pixel's own colour, so it can be sampled
/// once on a lattice and interpolated. What depends on the neighbourhood
/// (halation, scatter, the couplers' diffusion, grain, dodge and burn) or on
/// the whole frame (the meters) cannot, and `LookBaker.perPixel` switches it
/// off before baking.
struct LookLUT {

    /// What a lattice coordinate (0...1 per channel) means on the way in.
    ///
    /// Every curve but Apple Log is defined on the code values the camera
    /// records, code / 1023 with the footroom in; each has a video-levels
    /// twin, (code - 64) / 876, for a video-range file as DaVinci Resolve and
    /// most editors hand it to a LUT.
    enum Input: String, CaseIterable, Codable {
        /// N-Log normalised code value, code / 1023, as recorded. What the
        /// Motion mode reads off a decoded frame.
        case nlogCode = "N-Log code values"
        /// The video-levels signal, (code - 64) / 876: how DaVinci Resolve
        /// and most editors hand a video-range clip to a LUT.
        case nlogVideoLevels = "N-Log video levels"
        /// The Apple Log signal, 0...1. Apple defines the curve on the signal
        /// rather than on code values, so this is what an app hands the LUT
        /// whichever range the file is in.
        case appleLog = "Apple Log"
        /// Apple Log 2 from iPhone 17 Pro and later: Apple Log's curve on the
        /// same 0...1 signal, in Apple Wide Gamut primaries.
        case appleLog2 = "Apple Log 2"
        /// Panasonic V-Log / V-Gamut, code value / 1023: the full range a
        /// Lumix camera's Real Time LUT takes.
        case vlog = "Panasonic V-Log"
        /// V-Log from a video-range file as DaVinci Resolve hands it to a
        /// LUT: (code - 64) / 876.
        case vlogVideoLevels = "Panasonic V-Log (video levels)"
        /// Fujifilm F-Log and F-Log2, in F-Gamut (BT.2020).
        case flog = "Fujifilm F-Log"
        case flogVideoLevels = "Fujifilm F-Log (video levels)"
        case flog2 = "Fujifilm F-Log2"
        case flog2VideoLevels = "Fujifilm F-Log2 (video levels)"
        /// Canon Log 2 and Canon Log 3, in Cinema Gamut.
        case clog2 = "Canon Log 2"
        case clog2VideoLevels = "Canon Log 2 (video levels)"
        case clog3 = "Canon Log 3"
        case clog3VideoLevels = "Canon Log 3 (video levels)"
        /// Sony S-Log3, in S-Gamut3.Cine or in S-Gamut3.
        case slog3Cine = "Sony S-Log3 (S-Gamut3.Cine)"
        case slog3CineVideoLevels = "Sony S-Log3 (S-Gamut3.Cine, video levels)"
        case slog3 = "Sony S-Log3 (S-Gamut3)"
        case slog3VideoLevels = "Sony S-Log3 (S-Gamut3, video levels)"
        /// Insta360 I-Log, in BT.2020.
        case ilog = "Insta360 I-Log"
        case ilogVideoLevels = "Insta360 I-Log (video levels)"

        /// The lattice coordinate is the video-levels signal rather than
        /// code / 1023.
        var onVideoLevels: Bool {
            switch self {
            case .nlogVideoLevels, .vlogVideoLevels, .flogVideoLevels, .flog2VideoLevels, .clog2VideoLevels,
                 .clog3VideoLevels, .slog3CineVideoLevels, .slog3VideoLevels, .ilogVideoLevels: return true
            default: return false
            }
        }

        /// Scene-linear reflectance for one channel of a lattice coordinate,
        /// in the footage's own primaries.
        func linear(_ v: Double) -> Double {
            let code = onVideoLevels ? (64 + 876 * v) / 1023 : v
            switch self {
            case .nlogCode, .nlogVideoLevels: return NLog.linear(code: code)
            case .appleLog, .appleLog2: return AppleLog.linear(signal: v)
            case .vlog, .vlogVideoLevels: return VLog.linear(code: code)
            case .flog, .flogVideoLevels: return FLog.flog.linear(code: code)
            case .flog2, .flog2VideoLevels: return FLog.flog2.linear(code: code)
            case .clog2, .clog2VideoLevels: return CanonLog.linear2(code: code)
            case .clog3, .clog3VideoLevels: return CanonLog.linear3(code: code)
            case .slog3Cine, .slog3CineVideoLevels, .slog3, .slog3VideoLevels: return SLog3.linear(code: code)
            case .ilog, .ilogVideoLevels: return ILog.linear(code: code)
            }
        }

        /// The footage's primaries to the engine's BT.2020, where they differ.
        /// N-Log, Apple Log and I-Log record in BT.2020, F-Log and F-Log2 in
        /// F-Gamut, which is BT.2020.
        var gamut: simd_double3x3? {
            switch self {
            case .appleLog2: return AppleLog.wideGamutToBT2020
            case .vlog, .vlogVideoLevels: return VLog.toBT2020
            case .clog2, .clog2VideoLevels, .clog3, .clog3VideoLevels: return CanonLog.cinemaGamutToBT2020
            case .slog3Cine, .slog3CineVideoLevels: return SLog3.sGamut3CineToBT2020
            case .slog3, .slog3VideoLevels: return SLog3.sGamut3ToBT2020
            default: return nil
            }
        }

        /// Scene-linear BT.2020 light for a lattice coordinate.
        func scene(_ p: SIMD3<Double>) -> SIMD3<Double> {
            let light = SIMD3(linear(p.x), linear(p.y), linear(p.z))
            return gamut.map { $0 * light } ?? light
        }

        var cubeComment: String {
            let levels = onVideoLevels ? "video levels ((code - 64) / 876)" : "code value / 1023 (full range)"
            switch self {
            case .nlogCode: return "Nikon N-Log, BT.2020 primaries, code value / 1023 (data levels)"
            case .nlogVideoLevels: return "Nikon N-Log, BT.2020 primaries, video levels ((code - 64) / 876)"
            case .appleLog: return "Apple Log (iPhone 15 Pro and later), BT.2020 primaries, signal 0-1"
            case .appleLog2: return "Apple Log 2 (iPhone 17 Pro and later), Apple Wide Gamut primaries, signal 0-1"
            case .vlog, .vlogVideoLevels: return "Panasonic V-Log, V-Gamut primaries, \(levels)"
            case .flog, .flogVideoLevels: return "Fujifilm F-Log, F-Gamut (BT.2020) primaries, \(levels)"
            case .flog2, .flog2VideoLevels: return "Fujifilm F-Log2, F-Gamut (BT.2020) primaries, \(levels)"
            case .clog2, .clog2VideoLevels: return "Canon Log 2, Cinema Gamut primaries, \(levels)"
            case .clog3, .clog3VideoLevels: return "Canon Log 3, Cinema Gamut primaries, \(levels)"
            case .slog3Cine, .slog3CineVideoLevels: return "Sony S-Log3, S-Gamut3.Cine primaries, \(levels)"
            case .slog3, .slog3VideoLevels: return "Sony S-Log3, S-Gamut3 primaries, \(levels)"
            case .ilog, .ilogVideoLevels: return "Insta360 I-Log, BT.2020 primaries, \(levels)"
            }
        }
    }

    /// How the output values are encoded. The primaries are Rec.709 either way.
    enum Output: String, CaseIterable, Codable {
        /// A 2.4 power: what a BT.1886 Rec.709 reference display decodes.
        case bt1886 = "Rec.709 (BT.1886)"
        /// A 1.961 power: how macOS and iOS decode Rec.709-tagged video —
        /// QuickTime, Safari, Photos. Measured with VideoToolbox, not assumed.
        case appleDisplay = "Rec.709 (Apple displays)"
        /// Display-referred linear light.
        case linear = "Linear"
        /// The sRGB curve: what a JPEG or HEIF is decoded with, and between
        /// the two video curves, so one table serves a camera's photos and
        /// its movies.
        case sRGB = "sRGB"

        /// The power the curve is, or for sRGB the one it approximates. The
        /// export's encode kernel uses it; it is only ever handed the two
        /// Rec.709 cases.
        var gamma: Double {
            switch self {
            case .bt1886: return 2.4
            case .appleDisplay: return 1.961
            case .linear: return 1
            case .sRGB: return 2.2
            }
        }

        func encode(_ light: Double) -> Double {
            let l = max(light, 0)
            if self == .sRGB { return l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1 / 2.4) - 0.055 }
            return pow(l, 1 / gamma)
        }

        func decode(_ value: Double) -> Double {
            let v = max(value, 0)
            if self == .sRGB { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4) }
            return pow(v, gamma)
        }

        var cubeComment: String {
            switch self {
            case .bt1886: return "Rec.709 primaries, BT.1886 (2.4 power) for a Rec.709 reference display"
            case .appleDisplay: return "Rec.709 primaries, 1.961 power (how macOS and iOS decode Rec.709 video)"
            case .linear: return "Rec.709 primaries, display-referred linear light"
            case .sRGB: return "Rec.709 / sRGB primaries, sRGB curve (photos exactly, video between 1.961 and 2.4)"
            }
        }
    }

    let size: Int
    let input: Input
    let output: Output
    /// RGB triples, red fastest, then green, then blue: the .cube order, and
    /// the memory order of a 3D texture indexed (r, g, b).
    let values: [Float]

    func value(_ r: Int, _ g: Int, _ b: Int) -> SIMD3<Float> {
        let i = (r + size * (g + size * b)) * 3
        return SIMD3(values[i], values[i + 1], values[i + 2])
    }

    /// Tetrahedral interpolation, the same split the shader uses: the cell is
    /// cut along its main diagonal into six tetrahedra, picked by the order of
    /// the fractional coordinates.
    func sample(_ p: SIMD3<Float>) -> SIMD3<Float> {
        let n = Float(size - 1)
        let q = simd_clamp(p, SIMD3(repeating: 0), SIMD3(repeating: 1)) * n
        let i = SIMD3<Int>(min(Int(q.x), size - 2), min(Int(q.y), size - 2), min(Int(q.z), size - 2))
        let f = q - SIMD3<Float>(Float(i.x), Float(i.y), Float(i.z))
        func c(_ dr: Int, _ dg: Int, _ db: Int) -> SIMD3<Float> { value(i.x + dr, i.y + dg, i.z + db) }
        let c000 = c(0, 0, 0), c111 = c(1, 1, 1)
        if f.x > f.y {
            if f.y > f.z {
                let c100 = c(1, 0, 0), c110 = c(1, 1, 0)
                return c000 + f.x * (c100 - c000) + f.y * (c110 - c100) + f.z * (c111 - c110)
            } else if f.x > f.z {
                let c100 = c(1, 0, 0), c101 = c(1, 0, 1)
                return c000 + f.x * (c100 - c000) + f.z * (c101 - c100) + f.y * (c111 - c101)
            } else {
                let c001 = c(0, 0, 1), c101 = c(1, 0, 1)
                return c000 + f.z * (c001 - c000) + f.x * (c101 - c001) + f.y * (c111 - c101)
            }
        } else {
            if f.z > f.y {
                let c001 = c(0, 0, 1), c011 = c(0, 1, 1)
                return c000 + f.z * (c001 - c000) + f.y * (c011 - c001) + f.x * (c111 - c011)
            } else if f.z > f.x {
                let c010 = c(0, 1, 0), c011 = c(0, 1, 1)
                return c000 + f.y * (c010 - c000) + f.z * (c011 - c010) + f.x * (c111 - c011)
            } else {
                let c010 = c(0, 1, 0), c110 = c(1, 1, 0)
                return c000 + f.y * (c010 - c000) + f.x * (c110 - c010) + f.z * (c111 - c110)
            }
        }
    }

    /// The values padded to RGBA, for a 3D texture.
    var rgba: [Float] {
        var out = [Float](repeating: 1, count: size * size * size * 4)
        for i in 0..<(size * size * size) {
            out[i * 4] = values[i * 3]
            out[i * 4 + 1] = values[i * 3 + 1]
            out[i * 4 + 2] = values[i * 3 + 2]
        }
        return out
    }

    /// The Adobe / Resolve .cube text. Plain ASCII: some readers reject
    /// anything else in the header. `headerLines` go straight under the title,
    /// where a Lumix camera looks for its base Photo Style.
    /// `domain` writes DOMAIN_MIN/MAX, which only restate the 0...1 default;
    /// a camera's own reader may not expect them.
    func cubeText(title: String, headerLines: [String] = [], comments: [String] = [], domain: Bool = true) -> String {
        var text = "TITLE \"\(title.filter { $0.isASCII && $0 != "\"" })\"\n"
        for line in headerLines { text += line.filter(\.isASCII) + "\n" }
        for line in comments + ["Input: \(input.cubeComment)", "Output: \(output.cubeComment)"] {
            text += "# \(line.filter(\.isASCII))\n"
        }
        text += "LUT_3D_SIZE \(size)\n"
        if domain { text += "DOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n" }
        // A 65-point table is 275k lines; String(format:) per line takes a
        // second, snprintf into one buffer a few milliseconds.
        var bytes = [UInt8](repeating: 0, count: size * size * size * 30 + 1)
        var used = 0
        bytes.withUnsafeMutableBufferPointer { buffer in
            buffer.baseAddress!.withMemoryRebound(to: CChar.self, capacity: buffer.count) { base in
                for i in 0..<(size * size * size) {
                    // Display values: 0...1, whatever rounding left a slide's clear base at.
                    let row = (0..<3).map { min(max(Double(values[i * 3 + $0]), 0), 1) }
                    used += Int(withVaList(row) { vsnprintf(base + used, buffer.count - used, "%.6f %.6f %.6f\n", $0) })
                }
            }
        }
        return text + String(decoding: bytes[0..<used], as: UTF8.self)
    }
}

/// Samples the engine onto a lattice.
///
/// Keeps its own renderer and lattice between bakes, so a re-bake after a
/// slider move costs one render of the lattice: the image is uploaded once per
/// lattice, and the renderer reuses the developed negative when only print
/// controls changed.
final class LookBaker {

    enum BakeError: Error, LocalizedError {
        case noPixels
        var errorDescription: String? { "The engine produced no pixels for the lattice." }
    }

    let renderer: Renderer
    private var latticeKey: (size: Int, input: LookLUT.Input)?

    init(renderer: Renderer? = nil) throws {
        self.renderer = try renderer ?? Renderer()
    }

    /// The recipe as a LUT can carry it: every per-pixel control as set, and
    /// nothing that depends on neighbouring pixels or on the frame as a whole.
    /// The meters are off, so their stored values are what bakes.
    static func perPixel(_ recipe: Recipe) -> Recipe {
        var r = recipe
        r.autoExposure = false
        r.autoDensity = false
        r.autoGrade = false
        r.halationEnabled = false
        r.grainEnabled = false
        r.couplersDiffusion = 0
        r.masks = []
        r.exposureGradients = nil     // they cover part of the frame
        r.effects = nil               // bloom and glow spread light between pixels
        r.frameGeometry = nil         // the lattice is not a picture
        r.outputColorSpace = "sRGB"   // Rec.709 primaries; the encoding is the LUT's own
        return r
    }

    /// The lattice as an image of `size * size` by `size` pixels, laid out so
    /// the pixel order is the LUT order: red fastest, then green, then blue.
    static func lattice(size: Int, input: LookLUT.Input) -> [Float] {
        let n = size
        var px = [Float](repeating: 1, count: n * n * n * 4)
        let axis = (0..<n).map { input.linear(Double($0) / Double(n - 1)) }
        let gamut = input.gamut
        for b in 0..<n {
            for g in 0..<n {
                for r in 0..<n {
                    let o = (r + n * (g + n * b)) * 4
                    var light = SIMD3(axis[r], axis[g], axis[b])
                    if let gamut { light = gamut * light }
                    px[o] = Float(light.x)
                    px[o + 1] = Float(light.y)
                    px[o + 2] = Float(light.z)
                }
            }
        }
        return px
    }

    func bake(recipe: Recipe, size: Int = 65,
              input: LookLUT.Input = .nlogCode,
              output: LookLUT.Output = .bt1886) throws -> LookLUT {
        let r = Self.perPixel(recipe)
        if latticeKey?.size != size || latticeKey?.input != input {
            renderer.setImage(Self.lattice(size: size, input: input), width: size * size, height: size)
            latticeKey = (size, input)
        }
        let setup = try RenderSetupBuilder.build(recipe: r)
        _ = try renderer.render(setup: setup, recipe: r)
        guard let linear = renderer.readLinear() else { throw BakeError.noPixels }
        let count = size * size * size
        var values = [Float](repeating: 0, count: count * 3)
        for i in 0..<count {
            for c in 0..<3 {
                values[i * 3 + c] = Float(output.encode(Double(linear[i * 4 + c])))
            }
        }
        return LookLUT(size: size, input: input, output: output, values: values)
    }

    /// The engine's own answer for arbitrary inputs (lattice coordinates, not
    /// linear), in the LUT's output encoding: the reference a LUT is checked
    /// against. Replaces the lattice, so the next bake uploads it again.
    func direct(_ points: [SIMD3<Float>], recipe: Recipe,
                input: LookLUT.Input, output: LookLUT.Output) throws -> [SIMD3<Float>] {
        let r = Self.perPixel(recipe)
        let width = min(max(points.count, 1), 1024)
        let height = (points.count + width - 1) / width
        var px = [Float](repeating: 1, count: width * height * 4)
        for (i, p) in points.enumerated() {
            let light = input.scene(SIMD3(Double(p.x), Double(p.y), Double(p.z)))
            px[i * 4] = Float(light.x)
            px[i * 4 + 1] = Float(light.y)
            px[i * 4 + 2] = Float(light.z)
        }
        renderer.setImage(px, width: width, height: height)
        latticeKey = nil
        let setup = try RenderSetupBuilder.build(recipe: r)
        _ = try renderer.render(setup: setup, recipe: r)
        guard let linear = renderer.readLinear() else { throw BakeError.noPixels }
        return (0..<points.count).map { i in
            SIMD3(Float(output.encode(Double(linear[i * 4]))),
                  Float(output.encode(Double(linear[i * 4 + 1]))),
                  Float(output.encode(Double(linear[i * 4 + 2]))))
        }
    }
}
