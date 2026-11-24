import Foundation
import simd

/// The log encodings the Motion mode reads.
///
/// All but Apple Log are defined on the 10-bit code values the camera
/// records, footroom included: a video-range file keeps its 64 offset. Apple
/// Log is defined on the 0...1 signal once the file's own levels are taken
/// out. The two agree for a full-range file and differ for a video-range one,
/// so the grade kernel hands the baked LUT whichever the format is defined on.
enum LogFormat: String, Codable, CaseIterable, Identifiable {
    case nlog = "Nikon N-Log"
    case vlog = "Panasonic V-Log"
    case appleLog = "Apple Log"
    case appleLog2 = "Apple Log 2"
    case flog = "Fujifilm F-Log"
    case flog2 = "Fujifilm F-Log2"
    case clog2 = "Canon Log 2"
    case clog3 = "Canon Log 3"
    case slog3Cine = "Sony S-Log3 · S-Gamut3.Cine"
    case slog3 = "Sony S-Log3 · S-Gamut3"
    case ilog = "Insta360 I-Log"
    var id: String { rawValue }

    var shortName: String {
        switch self {
        case .nlog: return "N-Log"
        case .vlog: return "V-Log"
        case .appleLog: return "Apple Log"
        case .appleLog2: return "Apple Log 2"
        case .flog: return "F-Log"
        case .flog2: return "F-Log2"
        case .clog2: return "Canon Log 2"
        case .clog3: return "Canon Log 3"
        case .slog3Cine: return "S-Log3 · S-Gamut3.Cine"
        case .slog3: return "S-Log3 · S-Gamut3"
        case .ilog: return "I-Log"
        }
    }

    /// The curve alone, whichever gamut it records in.
    var curveName: String {
        switch self {
        case .slog3Cine, .slog3: return "S-Log3"
        default: return shortName
        }
    }

    var onCodeValues: Bool { self != .appleLog && self != .appleLog2 }

    /// What the grade kernel hands the baked LUT.
    var lutInput: LookLUT.Input {
        switch self {
        case .nlog: return .nlogCode
        case .vlog: return .vlog
        case .appleLog: return .appleLog
        case .appleLog2: return .appleLog2
        case .flog: return .flog
        case .flog2: return .flog2
        case .clog2: return .clog2
        case .clog3: return .clog3
        case .slog3Cine: return .slog3Cine
        case .slog3: return .slog3
        case .ilog: return .ilog
        }
    }

    /// A table for other software, taking the clip the way DaVinci Resolve
    /// hands it to a LUT: data levels for a full-range file, video levels
    /// otherwise. Apple Log is defined on the signal either way.
    func exportInput(fullRange: Bool) -> LookLUT.Input {
        if fullRange { return lutInput }
        switch self {
        case .nlog: return .nlogVideoLevels
        case .vlog: return .vlogVideoLevels
        case .appleLog, .appleLog2: return lutInput
        case .flog: return .flogVideoLevels
        case .flog2: return .flog2VideoLevels
        case .clog2: return .clog2VideoLevels
        case .clog3: return .clog3VideoLevels
        case .slog3Cine: return .slog3CineVideoLevels
        case .slog3: return .slog3VideoLevels
        case .ilog: return .ilogVideoLevels
        }
    }

    /// Where an 18% grey card records.
    var grey: String {
        switch self {
        case .nlog: return "code 372"
        case .vlog: return "code 433"
        case .appleLog, .appleLog2: return "signal 0.488"
        case .flog: return "code 470"
        case .flog2: return "code 400"
        case .clog2: return "code 407"
        case .clog3: return "code 351"
        case .slog3Cine, .slog3: return "code 420"
        case .ilog: return "code 432"
        }
    }

    /// Scene-linear BT.2020 light for a pixel's R'G'B' code values (0...1023)
    /// in a file of the given range.
    func scene(codes: SIMD3<Double>, fullRange: Bool) -> SIMD3<Double> {
        let v = onCodeValues || fullRange ? codes / 1023 : (codes - 64) / 876
        return lutInput.scene(v)
    }

    /// Core Video's tags for footage recorded in Apple Log (`kCVImageBuffer
    /// LogTransferFunction_AppleLog` and `_AppleLog2`, spelt out because the
    /// second needs macOS 26).
    static let appleLogTag = "com.apple.rec2020.apple-log"
    static let appleLog2Tag = "com.apple.apple-wide-gamut.apple-log"

    /// How a format was read off a clip.
    enum Basis: String {
        /// The file names its log curve: Apple's Core Video tag, or the
        /// `CaptureGammaEquation` in a Sony camera's metadata.
        case tagged = "from its tags"
        /// The file names only the camera's maker, which records one curve
        /// (Panasonic) or one most often (the others).
        case make = "guessed from the camera"
        /// Nothing said: N-Log, which a Nikon Z writes untagged.
        case assumed = "assumed"
    }

    /// What a clip says it is. Apple tags its log footage, and a Sony camera
    /// writes the curve and gamut into its own XML. Otherwise the maker
    /// decides: a Lumix records V-Log; a Fujifilm F-Log2 if it has it, else
    /// F-Log; a Canon most often Canon Log 3; a Sony S-Log3 in S-Gamut3.Cine;
    /// an Insta360 I-Log. Anything else is taken for N-Log (BT.709 tags on a
    /// Zf).
    static func detect(_ tags: CameraTags) -> (format: LogFormat, basis: Basis) {
        switch tags.logTransfer {
        case appleLog2Tag: return (.appleLog2, .tagged)
        case appleLogTag: return (.appleLog, .tagged)
        default: break
        }
        if let gamma = tags.gamma?.lowercased(), gamma.contains("s-log3") {
            let cine = gamma.contains("cine") || (tags.primaries?.lowercased().contains("cine") ?? false)
            return (cine ? .slog3Cine : .slog3, .tagged)
        }
        let make = (tags.make ?? "").lowercased()
        if tags.panasonic || make.contains("panasonic") || make.contains("lumix") { return (.vlog, .make) }
        if make.contains("fujifilm") {
            return (fLog2Cameras.contains((tags.model ?? "").uppercased()) ? .flog2 : .flog, .make)
        }
        if make.contains("canon") { return (.clog3, .make) }
        if make.contains("sony") { return (.slog3Cine, .make) }
        if make.contains("insta360") { return (.ilog, .make) }
        return (.nlog, .assumed)
    }

    /// Fujifilm cameras that record F-Log2 (from the X-H2S on; Fujifilm's
    /// firmware and product pages). Every Fujifilm log camera records F-Log.
    static let fLog2Cameras: Set<String> = ["X-H2S", "X-H2", "X-T5", "X-S20", "X100VI", "X-T50", "X-M5", "X-E5",
                                            "GFX100 II", "GFX100S II", "GFX100RF", "X-T30 III"]
}

/// What a clip's own metadata says about the camera that recorded it, for
/// `LogFormat.detect`.
struct CameraTags: Equatable {
    /// Core Video's log tag, which Apple sets on footage recorded in Apple Log.
    var logTransfer: String?
    /// The file carries a Panasonic (Lumix) 'PANA' user-data block.
    var panasonic = false
    var make: String?
    var model: String?
    /// A Sony camera's `CaptureGammaEquation` and `CaptureColorPrimaries`,
    /// from the NonRealTimeMeta XML it writes into the file.
    var gamma: String?
    var primaries: String?
}

/// How a clip's Y'CbCr is read. Each setting follows the file unless it is
/// overridden, for footage whose tags are missing or wrong.
struct InputSettings: Codable, Equatable {
    enum Levels: String, Codable, CaseIterable, Identifiable {
        case auto = "From the file"
        case video = "Video range"
        case full = "Full range"
        var id: String { rawValue }
    }

    enum Matrix: String, Codable, CaseIterable, Identifiable {
        case auto = "From the file"
        case bt2020 = "BT.2020"
        case bt709 = "BT.709"
        case bt601 = "BT.601"
        var id: String { rawValue }
    }

    var levels: Levels = .auto
    var matrix: Matrix = .auto
    /// nil reads the format from the clip (`ClipInfo.detectedFormat`).
    var format: LogFormat? = nil
}

/// The codec Export writes. The raw values are kept as looks saved them
/// before there was a choice of compression: ProRes's is still its old name,
/// "ProRes 422 HQ", whichever flavour is chosen (`ExportSettings.proRes`).
enum ExportCodec: String, Codable, CaseIterable, Identifiable {
    case proRes422HQ = "ProRes 422 HQ"
    case hevc10 = "HEVC 10-bit"
    var id: String { rawValue }
    /// The family, as the toolbar names it: its compression is beside it.
    var title: String { self == .proRes422HQ ? "ProRes 422" : "HEVC 10-bit" }
}

/// ProRes 422's flavours, from the most compressed to the least. ProRes takes
/// no data rate: each flavour has Apple's target for a frame size and rate,
/// and a clip comes out near it.
enum ProResFlavour: String, Codable, CaseIterable, Identifiable {
    case proxy = "Proxy"
    case lt = "LT"
    case standard = "422"
    case hq = "HQ"
    var id: String { rawValue }

    /// The full name, as the file's codec is called: "ProRes 422 LT".
    var title: String { self == .standard ? "ProRes 422" : "ProRes 422 \(rawValue)" }

    /// Apple's target data rate at 1920 x 1080 and 29.97 frames a second, in
    /// Mbit/s (Apple ProRes white paper): 45, 102, 147 and 220. It grows with
    /// the pixels and the frames: 182, 409, 589 and 884 at 4K.
    private var megabitsAt1080p30: Double {
        switch self {
        case .proxy: return 45
        case .lt: return 102
        case .standard: return 147
        case .hq: return 220
        }
    }

    func megabits(width: Int, height: Int, frameRate: Double) -> Double {
        megabitsAt1080p30 * Double(width * height) / (1920 * 1080) * max(frameRate, 1) / 29.97
    }
}

/// HEVC's data rate: four levels from HEVC's own limits for the clip's size
/// and frame rate (`HEVCLevels`), or a rate of its own
/// (`ExportSettings.hevcCustomMegabits`).
///
/// Standard is the most the HEVC level the clip needs allows, High tier: 100
/// Mbit/s for 4K up to 30 fps, 160 up to 60 and 240 up to 120; for HD 30, 50
/// and 100. High and Maximum are the next two levels' limits, Smaller half of
/// Standard: 50, 100, 160 and 240 Mbit/s for 4K at 30 fps, 80, 160, 240 and
/// 480 at 60. Every export is labelled with the lowest level that holds it,
/// so a Standard file asks a player for no more than the clip's size and
/// frame rate do. Cameras' own HEVC sits on the same steps: a Sony FX5's 4K
/// 4:2:0 is 30, 50 or 100 Mbit/s at 24 fps, 45, 75 or 150 at 60 and 200 at
/// 120; a Lumix S1II's is 150 at 24-30, 200 at 60 and 300 at 120.
///
/// What each keeps of the film's grain was measured on 4K clips from a Nikon
/// Z (29.97 fps), a Lumix (DCI 4K 59.94) and an iPhone (30), Vision3 250D on
/// 2383 at 35 mm, through VideoToolbox's encoder: the share of the grain's
/// light that survives, against the uncompressed frames, was 0.4 at 0.125
/// bits a pixel and frame, 0.5 at 0.25, 0.65 at 0.5, 0.75 at 1 and 0.85 at 2,
/// alike at 30 and 60 fps. At 4K 30 fps the four levels keep about 50%, 60%,
/// 70% and 75%; at 60 fps a little less, as a camera's would; ProRes 422 HQ
/// 98-99%. Nothing in HEVC favours one rate over its neighbours: 104 Mbit/s
/// kept 1% more grain than 100, for 4% more data. Without grain the picture
/// levels off at 57-58 dB PSNR from 1 or 2 bits a pixel, and the encoder
/// spends less than it is asked for: a clean clip is often smaller than the
/// rate shown.
enum HEVCLevel: String, Codable, CaseIterable, Identifiable {
    case smaller = "Smaller"
    case standard = "Standard"
    case high = "High"
    case maximum = "Maximum"
    case custom = "Custom"
    var id: String { rawValue }

    /// The level's rate for a clip `width` x `height` at `frameRate`, in
    /// Mbit/s; nil under Custom.
    func megabits(width: Int, height: Int, frameRate: Double) -> Double? {
        let steps = HEVCLevels.steps(width: width, height: height, frameRate: frameRate)
        switch self {
        case .smaller: return steps[0] / 2
        case .standard: return steps[0]
        case .high: return steps[1]
        case .maximum: return steps[2]
        case .custom: return nil
        }
    }
}

struct ExportSettings: Codable, Equatable {
    var codec: ExportCodec = .proRes422HQ
    /// Which displays the file is encoded for. Both are tagged Rec.709;
    /// they differ in the curve the values are written with.
    var display: LookLUT.Output = .appleDisplay
    /// ProRes's flavour; nil, HQ, all there was before. Optional, as are the
    /// two below, so that settings saved before still open.
    var proRes: ProResFlavour? = nil
    /// HEVC's level; nil, Standard.
    var hevcLevel: HEVCLevel? = nil
    /// HEVC's data rate under Custom, in Mbit/s; nil, 100.
    var hevcCustomMegabits: Double? = nil

    /// The data rates HEVC can be set to, Mbit/s: up to the most HEVC's
    /// highest level, 6.2, allows, 800 at High tier.
    static let hevcMegabits: ClosedRange<Double> = 1...800

    var flavour: ProResFlavour { proRes ?? .hq }
    var level: HEVCLevel { hevcLevel ?? .standard }
    var customMegabits: Double {
        min(max(hevcCustomMegabits ?? 100, Self.hevcMegabits.lowerBound), Self.hevcMegabits.upperBound)
    }

    /// The data rate a clip `width` x `height` at `frameRate` is written at,
    /// in Mbit/s: HEVC's as set, ProRes's Apple's target for its flavour.
    func megabits(width: Int, height: Int, frameRate: Double) -> Double {
        switch codec {
        case .proRes422HQ:
            return flavour.megabits(width: width, height: height, frameRate: frameRate)
        case .hevc10:
            return megabits(level, width: width, height: height, frameRate: frameRate)
        }
    }

    /// HEVC's data rate at `level` for a clip of that size and frame rate.
    func megabits(_ level: HEVCLevel, width: Int, height: Int, frameRate: Double) -> Double {
        let rate = level.megabits(width: width, height: height, frameRate: frameRate) ?? customMegabits
        return min(max(rate, Self.hevcMegabits.lowerBound), Self.hevcMegabits.upperBound)
    }

    /// The compression alone, as the toolbar shows it: "HQ", "High", or a
    /// custom rate, "120 Mbit/s".
    var compressionTitle: String {
        switch codec {
        case .proRes422HQ: return flavour.rawValue
        case .hevc10: return level == .custom ? Self.rate(customMegabits) : level.rawValue
        }
    }

    /// The codec and its compression: "ProRes 422 LT", "HEVC 10-bit, High".
    var title: String {
        switch codec {
        case .proRes422HQ: return flavour.title
        case .hevc10: return "\(codec.title), \(compressionTitle)"
        }
    }

    /// A data rate as the app writes it: "207 Mbit/s", "8.5 Mbit/s".
    static func rate(_ megabits: Double) -> String {
        megabits < 10 ? String(format: "%.1f Mbit/s", megabits) : String(format: "%.0f Mbit/s", megabits)
    }
}

/// Everything that decides how a clip looks and is written out.
struct MotionLook: Codable, Equatable {
    /// The print: film, paper and the per-pixel controls. Spatial controls in
    /// it are ignored (`LookBaker.perPixel` turns them off); `autoExposure` is
    /// the clip meter's (`ClipModel.effectiveRecipe`), the other autos unused.
    var recipe: Recipe
    /// Glow's diffusion, glow and halation, applied to the print.
    var effects: GlowLook
    /// Grain on the finished frames; nil, none, as in a look saved before
    /// clips had any. Optional so that such a look still opens.
    var grain: MotionGrain? = nil
    var input = InputSettings()
    var export = ExportSettings()
    /// Which version of the app saved this look; nil before the clip meter.
    var version: Int? = MotionLook.currentVersion

    static let currentVersion = 2

    /// A look as saved by an earlier version, brought up to date. Before the
    /// clip meter, Auto exposure was forced off rather than chosen.
    func upgraded() -> MotionLook {
        guard version != Self.currentVersion else { return self }
        var next = self
        next.recipe.autoExposure = true
        next.version = Self.currentVersion
        return next
    }

    static var initial: MotionLook {
        var recipe = Recipe()
        recipe.filmStock = "kodak_portra_800"
        recipe.printStock = "kodak_portra_endura"
        // Highlight protection only (`ClipMeter`): a negative prints as shot,
        // a slide is pulled down as far as its latitude needs.
        recipe.autoExposure = true
        var effects = GlowLook()
        effects.bloom.enabled = false
        return MotionLook(recipe: recipe, effects: effects)
    }

    /// A black-and-white film, or any film printed on black-and-white paper.
    var printsInBlackAndWhite: Bool {
        let library = PackLibrary.shared
        guard let film = try? library.pack(recipe.filmStock) else { return false }
        if film.isMonochrome { return true }
        let onPaper = !film.isViewedDirectly(scanFilm: recipe.scanFilm)
        return onPaper && ((try? library.pack(recipe.printStock))?.isMonochrome ?? false)
    }

    /// Glow's effects as this film and paper print them. Glow's halation is a
    /// colour film's: the light scattered back off the base re-exposes the
    /// red-sensitive bottom layer, so the halo is red. In a black-and-white
    /// picture it is grey, as bright as the red one, so Amount keeps its meaning.
    var effectsAsPrinted: GlowLook {
        guard printsInBlackAndWhite else { return effects }
        var printed = effects
        let tint = printed.halation.tint
        let y = 0.2126 * tint.r + 0.7152 * tint.g + 0.0722 * tint.b
        printed.halation.tint = LinearRGB(r: y, g: y, b: y)
        return printed
    }

    /// What a clip opened for the first time starts from: this look — film,
    /// paper, film character, effects, how footage is read and written — with
    /// the dials set for one particular clip (exposure, tone, colour) back at
    /// their defaults. The same rule the Stills mode follows for photographs.
    func carryingLook() -> MotionLook {
        var next = self
        next.recipe = recipe.carryingLook()
        // The log format is the clip's own: a new clip is read from its tags,
        // so an override for one camera's footage does not misread another's.
        next.input.format = nil
        return next
    }
}
