import Foundation
import simd

/// The film gauge a clip is taken to be shot on: the frame its grain is
/// measured against, as `FilmFormat` is for a photograph. The same emulsion
/// shows coarser grain on 16 mm than on 35 mm, and finer on 70 mm, because
/// the smaller frame is enlarged more to fill the same screen.
enum FilmGauge: String, Codable, CaseIterable, Identifiable {
    case sixteen = "16 mm"
    case thirtyFive = "35 mm"
    case seventy = "70 mm"
    var id: String { rawValue }

    /// The camera aperture, long side by short: Super 16, 12.52 x 7.41 mm;
    /// Super 35, the 4-perf full aperture, 24.89 x 18.66 mm; and 65 mm 5-perf,
    /// 52.48 x 23.01 mm, the negative a 70 mm print is made from. A frame is
    /// laid on it as large as it fits (`Renderer.pixelSizeUm`), so a 16:9 clip
    /// spans Super 16's and Super 35's width, and 65 mm's height.
    var frameMM: (long: Double, short: Double) {
        switch self {
        case .sixteen: return (12.52, 7.41)
        case .thirtyFive: return (24.89, 18.66)
        case .seventy: return (52.48, 23.01)
        }
    }
}

/// Grain on a clip: laid on each finished frame, after the look's LUT and
/// Glow's effects, every frame with grain of its own, as every frame of film
/// is a new piece of emulsion. Amount and Size are the Stills mode's.
struct MotionGrain: Codable, Equatable {
    var enabled = false
    var gauge: FilmGauge = .thirtyFive
    var amount: Double = 1
    var size: Double = 1

    var isVisible: Bool { enabled && amount > 0 }
}

/// One frame's grain: the look's table, the clip's settings, and what makes
/// each frame's grain its own, the clip's seed and the frame's number.
struct FrameGrain {
    var table: PrintGrainTable
    var settings: MotionGrain
    var clipSeed: UInt32
    var frame: Int

    /// The grains of frame `frame` of this clip, on this stock: the same in
    /// the preview, paused or playing, and in every export.
    var seed: UInt32 {
        table.stockSeed ^ (clipSeed &* 0x9E37_79B9) ^ (UInt32(truncatingIfNeeded: frame) &* 0x85EB_CA6B)
    }

    /// A clip's seed: its file's name, as a photograph's is (`Renderer.grainSeed`).
    static func clipSeed(for url: URL) -> UInt32 { Renderer.grainSeed(for: url) }

    /// The frame shown at `seconds` into a clip of `frameRate`.
    static func frameNumber(at seconds: Double, frameRate: Double) -> Int {
        guard seconds.isFinite, frameRate > 0 else { return 0 }
        return Int((seconds * frameRate).rounded())
    }
}

/// What a clip's frames take their grain from: the look's table and
/// settings, and the clip's own seed.
struct ClipGrain {
    var table: PrintGrainTable
    var settings: MotionGrain
    var clipSeed: UInt32

    init(table: PrintGrainTable, settings: MotionGrain, clip url: URL) {
        self.table = table
        self.settings = settings
        clipSeed = FrameGrain.clipSeed(for: url)
    }

    func frame(_ number: Int) -> FrameGrain {
        FrameGrain(table: table, settings: settings, clipSeed: clipSeed, frame: number)
    }
}
