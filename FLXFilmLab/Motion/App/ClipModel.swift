import Foundation
import Combine
import CoreVideo

/// The open clip, its grade, and everything that keeps the preview in step.
///
/// A control change re-bakes the print look (about 10 ms, off the main
/// thread) and redraws the frame on screen; nothing else is re-decoded. Export
/// bakes the look once more from exactly the settings it was started with and
/// writes every frame through the same pipeline at full size.
///
/// Opening another clip follows the Stills mode's rule for photographs: the clip on
/// screen stays until the new one has loaded, each clip's grade is remembered
/// for the session, and a clip opened for the first time keeps the look
/// (film, paper, film character, effects, how footage is read and written)
/// while exposure, tone and colour start fresh.
@MainActor
final class ClipModel: ObservableObject {
    @Published var look: MotionLook {
        didSet { lookDidChange(from: oldValue) }
    }
    @Published private(set) var clip: ClipInfo?
    @Published private(set) var isOpening = false
    @Published private(set) var status = "Open a log clip to begin"
    @Published var errorMessage: String?
    @Published private(set) var exportProgress: Double?
    /// How the frame on screen is being read, as the inspector reports it.
    @Published private(set) var reading: FramePipeline.Reading?
    @Published private(set) var drawMilliseconds: Double = 0
    /// The recipe whose bake the preview is showing; behind `effectiveRecipe`
    /// for the few milliseconds a bake takes.
    @Published private(set) var bakedRecipe: Recipe?
    /// The log format that bake reads.
    @Published private(set) var bakedFormat: LogFormat?
    /// What Auto exposure reads for this clip on this stock (`ClipMeter`):
    /// nil until the clip has been metered.
    @Published private(set) var meteredExposure: Double?
    @Published private(set) var isMetering = false
    @Published var showLog = false {
        didSet {
            let mode: FramePipeline.Mode = showLog ? .log : .graded
            preview.update { $0.mode = mode }
        }
    }
    @Published var surround = Surround.dark {
        didSet {
            let s = surround
            preview.update { $0.surround = s }
        }
    }

    let playback = PlaybackController()
    let preview: PreviewRenderer
    private let bakeEngine = LookBakeEngine()
    private var edits: [URL: MotionLook] = [:]
    private var isBaking = false
    private var bakeAgain = false
    private var exporter: ClipExporter?
    private var latestDrawMilliseconds = 0.0
    private var timingUpdateScheduled = false
    /// Each clip's metered highlight, by how it was read: decoding frames for
    /// it is the slow part, the exposure for a stock is arithmetic.
    private var highlights: [URL: (input: InputSettings, luminance: Float?)] = [:]
    private var meterTask: Task<Void, Never>?

    static let lookDefaultsKey = "look"

    init() {
        let stored = UserDefaults.standard.data(forKey: Self.lookDefaultsKey)
            .flatMap { try? JSONDecoder().decode(MotionLook.self, from: $0) }
        look = (stored ?? .initial).upgraded().carryingLook()
        do {
            preview = try PreviewRenderer()
        } catch {
            fatalError("FLX Film Lab needs Metal: \(error.localizedDescription)")
        }
        playback.onFrame = { [weak self] buffer, info in self?.frameArrived(buffer, info) }
        preview.onDraw = { [weak self] ms, error in
            guard let self else { return }
            if let error { self.errorMessage = error.localizedDescription }
            // Every published change redraws the inspector, so the timing is
            // published a few times a second, always ending on the latest.
            self.latestDrawMilliseconds = ms
            guard !self.timingUpdateScheduled else { return }
            self.timingUpdateScheduled = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    self.timingUpdateScheduled = false
                    self.drawMilliseconds = self.latestDrawMilliseconds
                }
            }
        }
        pushLookToPreview()
        scheduleBake()
    }

    /// The recipe as it prints: with Auto exposure on, the meter's reading
    /// in place of the stored exposure.
    var effectiveRecipe: Recipe {
        var r = look.recipe
        if r.autoExposure, let ev = meteredExposure { r.exposureEV = ev }
        return r
    }

    /// The log format the clip on screen is read as: the inspector's choice,
    /// or what the clip says it is.
    var format: LogFormat { look.input.format ?? clip?.detectedFormat ?? .nlog }

    /// How frames are read, with the format resolved.
    var readingSettings: InputSettings {
        var settings = look.input
        settings.format = format
        return settings
    }

    var filmName: String { (try? PackLibrary.shared.pack(look.recipe.filmStock))?.name ?? look.recipe.filmStock }
    var paperName: String { (try? PackLibrary.shared.pack(look.recipe.printStock))?.name ?? look.recipe.printStock }
    var isExporting: Bool { exportProgress != nil }

    // MARK: Look

    private func lookDidChange(from old: MotionLook) {
        if look.input != old.input {
            meter()
        } else if look.recipe.filmStock != old.recipe.filmStock {
            updateMeteredExposure()
        }
        if look.recipe != old.recipe || look.input.format != old.input.format { scheduleBake() }
        if look.effectsAsPrinted != old.effectsAsPrinted || look.input != old.input { pushLookToPreview() }
        // Re-read the frame on screen at once, so a paused clip's reading and
        // probe follow an override without waiting for the next frame.
        if look.input != old.input {
            reading = frame.flatMap { try? FramePipeline.reading(of: $0, settings: readingSettings) }
        }
        if let clip { edits[clip.url] = look }
        if let data = try? JSONEncoder().encode(look) {
            UserDefaults.standard.set(data, forKey: Self.lookDefaultsKey)
        }
    }

    private func pushLookToPreview() {
        let effects = look.effectsAsPrinted, input = readingSettings
        preview.update {
            $0.effects = effects
            $0.input = input
        }
    }

    /// One bake at a time; moves made while one runs fold into a single next one.
    private func scheduleBake() {
        guard !isBaking else {
            bakeAgain = true
            return
        }
        isBaking = true
        let recipe = effectiveRecipe, bakeFormat = self.format
        Task {
            do {
                let lut = try await bakeEngine.bake(recipe, input: bakeFormat.lutInput)
                preview.setLUT(lut)
                bakedRecipe = recipe
                bakedFormat = bakeFormat
            } catch {
                errorMessage = "Could not bake the look: \(error.localizedDescription)"
            }
            isBaking = false
            if bakeAgain {
                bakeAgain = false
                scheduleBake()
            }
        }
    }

    /// Change film the way the stock picker does: crossing between colour and
    /// black and white also moves the print onto the new film's own paper
    /// (`Recipe.loadingFilm`), as in the Stills mode.
    func selectFilm(_ stock: String) {
        guard stock != look.recipe.filmStock else { return }
        look.recipe = look.recipe.loadingFilm(stock)
    }

    func resetExposure() {
        look.recipe.exposureEV = 0
        look.recipe.autoExposure = MotionLook.initial.recipe.autoExposure
    }

    // MARK: Metering

    /// Meter the clip on screen, unless it already has been as it is read now.
    private func meter() {
        guard let clip else { return }
        if let known = highlights[clip.url], known.input == readingSettings {
            updateMeteredExposure()
            return
        }
        meterTask?.cancel()
        isMetering = true
        let input = readingSettings
        meterTask = Task {
            let luminance = try? await ClipMeter.highlight(of: clip, settings: input)
            guard !Task.isCancelled else { return }
            highlights[clip.url] = (input, luminance)
            isMetering = false
            if self.clip?.url == clip.url { updateMeteredExposure() }
        }
    }

    /// The stock's reading of the clip's metered highlight.
    private func updateMeteredExposure() {
        guard let clip, let known = highlights[clip.url], known.input == readingSettings,
              let luminance = known.luminance,
              let film = try? PackLibrary.shared.pack(look.recipe.filmStock) else {
            if meteredExposure != nil { meteredExposure = nil; scheduleBake() }
            return
        }
        let ev = Double(ClipMeter.exposure(highlight: luminance, film: film))
        guard ev != meteredExposure else { return }
        meteredExposure = ev
        scheduleBake()
    }

    func resetTone() {
        look.recipe.brightness = 0
        look.recipe.grade = look.recipe.defaultGrade
        look.recipe.shadows = 0
        look.recipe.highlights = 0
    }

    func resetColour() {
        look.recipe.filterShiftY = 0
        look.recipe.filterShiftM = 0
    }

    func resetEffects() { look.effects = MotionLook.initial.effects }

    // MARK: Clips

    /// Safe during an export: that holds its own copy of the clip and the look.
    func open(_ url: URL) {
        isOpening = true
        Task {
            defer { isOpening = false }
            do {
                let info = try await ClipInfo.load(url)
                if let clip { edits[clip.url] = look }
                // The clip first: assigning the look files it under the clip
                // on screen, which has to be the new one by then.
                clip = info
                var next = edits[url] ?? look.carryingLook()
                // How files are written is a preference, not part of a grade.
                next.export = look.export
                look = next
                reading = nil
                meteredExposure = nil
                meter()
                // Another clip can be in another format with the same look.
                pushLookToPreview()
                if bakedFormat != format { scheduleBake() }
                playback.load(info)
                errorMessage = nil
                status = info.summary
                if (info.bitDepth ?? 10) < 10 {
                    errorMessage = "This clip is 8-bit; log footage is recorded in 10-bit. Is it a log clip?"
                }
            } catch {
                errorMessage = error.localizedDescription
            }
        }
    }

    private func frameArrived(_ buffer: CVPixelBuffer, _ info: ClipInfo) {
        // A frame still on its way from the previous clip is not this one's.
        guard info.url == clip?.url else { return }
        preview.show(buffer, quarterTurns: info.quarterTurns)
        frame = buffer
        let now = try? FramePipeline.reading(of: buffer, settings: readingSettings)
        if now != reading { reading = now }
        if let probePoint { probe(at: probePoint) }
    }

    /// Under the pointer: the code values recorded there, as IRE, and the
    /// light they decode to as stops from an 18% grey. A grey card exposed the
    /// way the camera maker recommends reads 0 stops: N-Log code 372 (35%),
    /// V-Log 433 (42%), Apple Log signal 0.488.
    @Published private(set) var probeText: String?
    private var frame: CVPixelBuffer?
    private var probePoint: CGPoint?

    /// `point` is in the source frame's own pixels; nil when the pointer leaves.
    func probe(at point: CGPoint?) {
        probePoint = point
        guard let point, let frame, let reading,
              let codes = FramePipeline.codes(at: Int(point.x), Int(point.y), of: frame, reading: reading) else {
            if probeText != nil { probeText = nil }
            return
        }
        let mean = (codes.x + codes.y + codes.z) / 3
        let light = reading.format.scene(codes: codes, fullRange: reading.fullRange)
        let luminance = 0.2627 * light.x + 0.6780 * light.y + 0.0593 * light.z
        let stops = luminance > 0 ? String(format: "%+.1f stops", log2(luminance / 0.18)) : "below black"
        probeText = String(format: "%@ %.0f %.0f %.0f · %.0f%% IRE · %@", reading.format.shortName as NSString,
                           codes.x, codes.y, codes.z, (mean - 64) / 876 * 100, stops)
    }

    var suggestedExportName: String {
        let stem = clip?.url.deletingPathExtension().lastPathComponent ?? "FLX Film Lab"
        let film = ["Kodak ", "Fujifilm ", "Ilford "].reduce(filmName) { $0.replacingOccurrences(of: $1, with: "") }
        return "\(stem) \(film).mov"
    }

    func export(to url: URL) {
        guard let clip, !isExporting else { return }
        playback.pause()
        exportProgress = 0
        var look = self.look
        look.recipe = effectiveRecipe
        look.effects = look.effectsAsPrinted
        look.input = readingSettings
        let exporter = ClipExporter()
        self.exporter = exporter
        status = "Exporting \(url.lastPathComponent)…"
        Task {
            do {
                let lut = try await bakeEngine.bake(look.recipe, input: (look.input.format ?? .nlog).lutInput)
                try await exporter.export(
                    source: clip.url, to: url, look: look, lut: lut,
                    options: .init(codec: look.export.codec, display: look.export.display, timeRange: nil),
                    progress: { p in
                        DispatchQueue.main.async {
                            MainActor.assumeIsolated {
                                if self.exportProgress != nil { self.exportProgress = p }
                            }
                        }
                    })
                status = "Exported \(url.lastPathComponent)"
            } catch ClipExporter.ExportError.cancelled {
                status = "Export cancelled"
            } catch {
                errorMessage = error.localizedDescription
                status = clip.summary
            }
            exportProgress = nil
            self.exporter = nil
        }
    }

    func cancelExport() { exporter?.cancel() }

    /// The print as a .cube for DaVinci Resolve and other editors, Rec.709
    /// BT.1886 out. The clip's log goes in the way Resolve reads it
    /// (`LogFormat.exportInput`): N-Log or V-Log code values / 1023 for a
    /// full-range file, which is what a Zf writes, video levels for a
    /// video-range one, and Apple Log on its signal. Per-pixel only — no effects.
    func exportLUT(to url: URL) {
        let recipe = effectiveRecipe
        let fullRange: Bool
        switch look.input.levels {
        case .full: fullRange = true
        case .video: fullRange = false
        case .auto: fullRange = clip?.fullRange ?? true
        }
        let lutInput = format.exportInput(fullRange: fullRange)
        let title = "\(filmName) on \(paperName)"
        var comments = [
            "Made with FLX Film Lab (Motion): \(filmName) printed on \(paperName)",
            String(format: "Exposure %+.2f EV, brightness %+.3f, grade R%.0f, shadows %+.2f, highlights %+.2f",
                   recipe.exposureEV, recipe.brightness, recipe.grade, recipe.shadows, recipe.highlights),
            String(format: "Filtration on top of neutral: %+.0fY %+.0fM", recipe.filterShiftY, recipe.filterShiftM),
            "Per-pixel only: diffusion, glow and halation are not in a LUT",
        ]
        if let filter = (try? PackLibrary.shared.pack(recipe.filmStock))?.lensFilter(recipe.lensFilter) {
            comments.insert(String(format: "Camera filter: %@, %.1fx (exposure opened %.1f stops)",
                                   filter.designation, filter.factor, log2(max(filter.factor, 1))), at: 1)
        }
        Task {
            do {
                let text = try await bakeEngine.cube(recipe, size: 65, input: lutInput, output: .bt1886,
                                                     title: title, comments: comments)
                try text.write(to: url, atomically: true, encoding: .utf8)
                status = "Wrote \(url.lastPathComponent)"
            } catch {
                errorMessage = "Could not write the LUT: \(error.localizedDescription)"
            }
        }
    }
}
