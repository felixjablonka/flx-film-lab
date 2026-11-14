import Foundation
import AppKit
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
/// while exposure, tone and colour start fresh. Each clip keeps its own undo
/// history for the session too (`history`).
@MainActor
final class ClipModel: ObservableObject {
    @Published var look: MotionLook {
        didSet {
            history.note { Self.grade(oldValue) }
            lookDidChange(from: oldValue)
            scheduleRecord()
        }
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
    /// The undo history of each clip that has been on screen, but the one
    /// that is: that one's is `history`'s.
    private var histories: [URL: EditHistory<MotionLook>.Stacks] = [:]
    private var isBaking = false
    private var bakeAgain = false
    private var exporter: ClipExporter?
    private var latestDrawMilliseconds = 0.0
    private var timingUpdateScheduled = false
    /// Each clip's metered highlight, by how it was read: decoding frames for
    /// it is the slow part, the exposure for a stock is arithmetic.
    private var highlights: [URL: (input: InputSettings, luminance: Float?)] = [:]
    private var meterTask: Task<Void, Never>?

    /// Each clip's look and flag, kept between launches: the library the
    /// Stills mode keeps its pictures' settings in, the same one.
    let library: ImageLibrary
    /// The folder in the sidebar: its clips, the selection and the flags.
    let browser = FolderBrowser(kind: .clips)
    /// The grade each clip opened with this session: one whose grade still
    /// equals it has nothing of its own to keep.
    private var openedWith: [URL: MotionLook] = [:]
    private var recordTask: Task<Void, Never>?
    private var batchCancel: BatchCancel?
    /// The export of the clip a batch is on.
    private var batchExporter: ClipExporter?
    /// A grade copied to paste onto other clips, with the clip it came from.
    @Published private(set) var copiedLook: (grade: MotionLook, source: String)? {
        didSet { browser.hasCopiedSettings = copiedLook != nil }
    }

    static let lookDefaultsKey = "look"

    init(library: ImageLibrary? = nil) {
        self.library = library ?? ImageLibrary()
        browser.library = self.library
        browser.pasteName = "Paste Look"
        let stored = UserDefaults.standard.data(forKey: Self.lookDefaultsKey)
            .flatMap { try? JSONDecoder().decode(MotionLook.self, from: $0) }
        look = (stored ?? .initial).upgraded().carryingLook()
        do {
            preview = try PreviewRenderer()
        } catch {
            fatalError("FLX Film Lab needs Metal: \(error.localizedDescription)")
        }
        playback.onFrame = { [weak self] buffer, info in self?.frameArrived(buffer, info) }
        browser.ownGrade = { [weak self] url in self?.ownGrade(url) }
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
        NotificationCenter.default.addObserver(forName: NSApplication.willTerminateNotification, object: nil,
                                               queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.recordCurrent()
                self?.library.save(wait: true)
                self?.library.finishWriting()
            }
        }
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
    /// What the print is, for an exported LUT: its title, and how it was made
    /// for its comment. The film on its paper; a slide alone, off the light
    /// table; an instant print alone, as the print it is.
    var printDescription: (title: String, made: String) {
        let film = try? PackLibrary.shared.pack(look.recipe.filmStock)
        if let film, film.isInstantPrint {
            let made = film.processingFahrenheit(look.recipe.processingTemperature)
                .map { "\(filmName), an instant print processed at \($0) F" } ?? "\(filmName), an instant print"
            return (filmName, made)
        }
        if film?.isViewedDirectly(scanFilm: look.recipe.scanFilm) ?? (look.recipe.scanFilm ?? false) {
            return (filmName, "\(filmName) on a light table")
        }
        return ("\(filmName) on \(paperName)", "\(filmName) printed on \(paperName)")
    }
    var isExporting: Bool { exportProgress != nil }

    // MARK: Undo

    /// Undo and redo for the clip on screen (`EditHistory`): one step per
    /// gesture, each clip with its own history for the session. How files are
    /// written is a preference, not part of a grade, so it is left out.
    lazy var history = EditHistory<MotionLook>(
        read: { [unowned self] in Self.grade(self.look) },
        write: { [unowned self] grade in
            var look = grade
            look.export = self.look.export
            self.look = look
        },
        describe: { a, b in
            var names = EditNames.recipe(a.recipe, b.recipe, vocabulary: .photographic)
            if a.effects != b.effects { names.append("Effects") }
            if a.input != b.input { names.append("Log Input") }
            return EditNames.title(names)
        })

    /// A look without how its files are written.
    private static func grade(_ look: MotionLook) -> MotionLook {
        var grade = look
        grade.export = ExportSettings()
        return grade
    }

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
    /// black and white also moves the print onto the new film's own paper, and
    /// a motion picture film onto its own print film (`Recipe.loadingFilm`),
    /// as in the Stills mode.
    func selectFilm(_ stock: String) {
        guard stock != look.recipe.filmStock else { return }
        look.recipe = look.recipe.loadingFilm(stock)
    }

    func resetExposure() {
        history.named("Reset Exposure") {
            look.recipe.exposureEV = 0
            look.recipe.autoExposure = MotionLook.initial.recipe.autoExposure
        }
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
        history.named("Reset Tone") {
            look.recipe.brightness = 0
            look.recipe.grade = look.recipe.defaultGrade
            look.recipe.shadows = 0
            look.recipe.highlights = 0
        }
    }

    func resetColour() {
        history.named("Reset Colour") {
            look.recipe.filterShiftY = 0
            look.recipe.filterShiftM = 0
        }
    }

    func resetEffects() { history.named("Reset Effects") { look.effects = MotionLook.initial.effects } }

    // MARK: Clips

    /// Safe during an export: that holds its own copy of the clip and the look.
    /// The sidebar lists the clip's folder.
    func open(_ given: URL) {
        let url = FolderBrowser.canonical(given)
        showFolder(of: url)
        browser.current = url
        isOpening = true
        Task {
            defer { isOpening = false }
            do {
                let info = try await ClipInfo.load(url)
                if clip?.url != url {
                    recordCurrent()
                    // What was done before the first clip came is not any clip's.
                    let steps = history.swap(for: histories.removeValue(forKey: url) ?? .init())
                    if let clip { histories[clip.url] = steps }
                }
                if let clip { edits[clip.url] = look }
                // Its look, chosen while the clip on screen is still the one
                // before, then the clip: assigning the look files it under the
                // clip on screen, which has to be the new one by then.
                var next = lookToOpen(url)
                clip = info
                // How files are written is a preference, not part of a grade.
                next.export = look.export
                if openedWith[url] == nil { openedWith[url] = Self.grade(next) }
                history.unrecorded { look = next }
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
                browser.current = clip?.url
            }
        }
    }

    /// The look a clip opens with: its own from this session, or kept from
    /// another, or the look on screen with exposure, tone and colour fresh.
    private func lookToOpen(_ url: URL) -> MotionLook {
        if url == clip?.url { return look }
        if let own = edits[url] { return own }
        if let kept = library.record(for: url)?.look { return kept.upgraded() }
        return look.carryingLook()
    }

    // MARK: The folder

    /// Open a folder: its clips in the sidebar, and the first on screen,
    /// unless the clip on screen is one of them.
    func openFolder(_ given: URL) {
        let folder = FolderBrowser.canonical(given)
        errorMessage = nil
        list(folder)
        guard let first = browser.shown.first?.url ?? browser.items.first?.url else {
            status = "No clips in \(folder.lastPathComponent)"
            return
        }
        if let current = clip?.url, browser.items.contains(where: { $0.url == current }) {
            browser.selection = [current]
            return
        }
        browser.selection = [first]
        open(first)
    }

    private func showFolder(of url: URL) {
        let folder = url.deletingLastPathComponent()
        if browser.folder?.standardizedFileURL != folder.standardizedFileURL { list(folder) }
        if !browser.selection.contains(url) { browser.selection = [url] }
    }

    private func list(_ folder: URL) {
        let items = FolderBrowser.listing(folder, kind: .clips)
        browser.show(folder, items: items, records: library.load(folder, files: items.map { ($0.url, $0.size) }))
    }

    /// The grade of the clip on screen goes into the library once it has
    /// settled, if it is its own: changed since it opened, or already kept.
    private func scheduleRecord() {
        recordTask?.cancel()
        recordTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(800))
            guard !Task.isCancelled else { return }
            self?.recordCurrent()
        }
    }

    private func recordCurrent() {
        recordTask?.cancel()
        recordTask = nil
        guard let url = clip?.url else { return }
        let grade = Self.grade(look)
        if let kept = library.record(for: url)?.look {
            guard kept.upgraded() != grade else { return }
        } else {
            guard let opened = openedWith[url], opened != grade else { return }
        }
        library.update(url) { $0.look = grade }
        browser.markEdited(url)
        browser.thumbnails.refreshClips([(url, grade)])
    }

    /// A clip's own grade, if it has one, for its graded thumbnail: none for
    /// one that would open with the look on screen.
    private func ownGrade(_ url: URL) -> MotionLook? {
        guard browser.edited.contains(url) else { return nil }
        if url == clip?.url { return Self.grade(look) }
        return (edits[url] ?? library.record(for: url)?.look?.upgraded()).map(Self.grade)
    }

    /// Flag the selection, or the clip on screen when nothing is selected.
    func flagSelection(_ flag: ImageLibrary.Flag?) {
        var targets = browser.items.map(\.url).filter { browser.selection.contains($0) }
        if targets.isEmpty, let current = clip?.url { targets = [current] }
        self.flag(flag, for: targets)
    }

    /// Flag those clips alone, the selection as it is: a row's button.
    func flag(_ flag: ImageLibrary.Flag?, for urls: [URL]) {
        for url in urls {
            library.update(url) { $0.flag = flag }
            browser.setFlag(flag, for: url)
        }
    }

    /// Step to the clip `offset` places along the sidebar.
    func showNeighbour(_ offset: Int) {
        guard let from = clip?.url ?? browser.shown.first?.url,
              let next = browser.neighbour(of: from, offset: offset), next != clip?.url else { return }
        browser.selection = [next]
        open(next)
    }

    // MARK: Copy and paste a look

    var canCopyLook: Bool { clip != nil }

    /// Copy the grade of the clip on screen, to paste onto others.
    func copyLook() {
        guard let clip else { return }
        copiedLook = (Self.grade(look), clip.url.lastPathComponent)
    }

    /// The copied grade on the clip on screen, which keeps how its own footage
    /// is read.
    func pasteLook() {
        guard let copied = copiedLook?.grade, clip != nil else { return }
        history.named("Paste Look") { look = pasting(copied, onto: look) }
    }

    /// The copied grade on every clip selected but the rejects, each keeping
    /// how its own footage is read, with the paste a step of its own history.
    func pasteLookToSelected() {
        guard let copied = copiedLook?.grade else { return }
        var changed: [(url: URL, grade: MotionLook)] = []
        for url in browser.selectedForBatch {
            if url == clip?.url {
                pasteLook()
                continue
            }
            let before = lookToOpen(url)
            let pasted = pasting(copied, onto: before)
            guard Self.grade(pasted) != Self.grade(before) else { continue }
            edits[url] = pasted
            histories[url] = EditHistory<MotionLook>.adding("Paste Look", before: Self.grade(before),
                                                            after: Self.grade(pasted), to: histories[url] ?? .init())
            if openedWith[url] == nil { openedWith[url] = Self.grade(before) }
            library.update(url) { $0.look = Self.grade(pasted) }
            browser.markEdited(url)
            changed.append((url, Self.grade(pasted)))
        }
        browser.thumbnails.refreshClips(changed)
    }

    private func pasting(_ grade: MotionLook, onto target: MotionLook) -> MotionLook {
        var next = grade
        next.input = target.input
        next.export = target.export
        return next
    }

    // MARK: Exporting the selection

    /// Write every clip selected but the rejects into `folder`, one after
    /// another, each graded with its own look, or, never opened, the look on
    /// screen as opening it would give it; each metered for itself, as on
    /// screen, in the format and for the displays the toolbar has chosen.
    func exportSelected(to folder: URL) {
        guard browser.batch == nil, !isExporting else { return }
        recordCurrent()
        let urls = browser.selectedForBatch
        guard !urls.isEmpty else { return }
        playback.pause()
        let onScreen = look
        var taken = Set<String>()
        let jobs: [(url: URL, look: MotionLook, target: URL)] = urls.map { url in
            var clipLook = lookToOpen(url)
            clipLook.export = onScreen.export
            let film = (try? PackLibrary.shared.pack(clipLook.recipe.filmStock))?.name ?? clipLook.recipe.filmStock
            let short = ["Kodak ", "Fujifilm ", "Ilford "].reduce(film) { $0.replacingOccurrences(of: $1, with: "") }
            let stem = url.deletingPathExtension().lastPathComponent + " " + short
            var name = stem + ".mov"
            var n = 2
            while taken.contains(name.lowercased())
                    || FileManager.default.fileExists(atPath: folder.appendingPathComponent(name).path) {
                name = "\(stem) \(n).mov"
                n += 1
            }
            taken.insert(name.lowercased())
            return (url, clipLook, folder.appendingPathComponent(name))
        }
        let cancel = BatchCancel()
        batchCancel = cancel
        browser.batch = .init(done: 0, total: jobs.count, current: jobs[0].url.lastPathComponent, folder: folder)
        exportProgress = 0
        Task {
            var failures: [String] = []
            for (i, job) in jobs.enumerated() {
                if cancel.isCancelled { break }
                browser.batch?.done = i
                browser.batch?.current = job.url.lastPathComponent
                status = "Exporting \(i + 1) of \(jobs.count): \(job.url.lastPathComponent)"
                exportProgress = 0
                do {
                    try await exportClip(job.url, look: job.look, to: job.target, cancel: cancel)
                } catch ClipExporter.ExportError.cancelled {
                    break
                } catch {
                    failures.append("\(job.url.lastPathComponent): \(error.localizedDescription)")
                }
            }
            let done = browser.batch?.done ?? 0
            browser.batch = nil
            batchCancel = nil
            exportProgress = nil
            if cancel.isCancelled {
                status = "Export stopped after \(done) of \(jobs.count)"
            } else if failures.isEmpty {
                status = "Exported \(jobs.count) clip\(jobs.count == 1 ? "" : "s") to \(folder.lastPathComponent)"
            } else {
                errorMessage = "Exported \(jobs.count - failures.count) of \(jobs.count); not exported: "
                    + failures.prefix(5).joined(separator: "; ") + (failures.count > 5 ? "; …" : "")
            }
        }
    }

    /// One clip of a batch, as Export writes the one on screen: read in its
    /// own format, metered for its stock, its look baked and every frame
    /// graded at full size.
    private func exportClip(_ url: URL, look given: MotionLook, to target: URL, cancel: BatchCancel) async throws {
        let info = try await ClipInfo.load(url)
        var look = given
        var settings = look.input
        settings.format = look.input.format ?? info.detectedFormat
        if look.recipe.autoExposure, let film = try? PackLibrary.shared.pack(look.recipe.filmStock) {
            let known = highlights[url].flatMap { $0.input == settings ? $0.luminance : nil }
            var luminance = known
            if luminance == nil {
                luminance = try await ClipMeter.highlight(of: info, settings: settings)
                highlights[url] = (settings, luminance)
            }
            if let luminance { look.recipe.exposureEV = Double(ClipMeter.exposure(highlight: luminance, film: film)) }
        }
        look.effects = look.effectsAsPrinted
        look.input = settings
        let lut = try await bakeEngine.bake(look.recipe, input: (settings.format ?? .nlog).lutInput)
        if cancel.isCancelled { throw ClipExporter.ExportError.cancelled }
        let exporter = ClipExporter()
        batchExporter = exporter
        defer { batchExporter = nil }
        try await exporter.export(
            source: url, to: target, look: look, lut: lut,
            options: .init(codec: look.export.codec, display: look.export.display, timeRange: nil),
            progress: { p in
                DispatchQueue.main.async {
                    MainActor.assumeIsolated {
                        if self.exportProgress != nil { self.exportProgress = p }
                    }
                }
            })
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

    /// Stop the export under way: a batch stops with the clip it is on.
    func cancelExport() {
        exporter?.cancel()
        batchCancel?.cancel()
        batchExporter?.cancel()
    }

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
        let (title, made) = printDescription
        var comments = [
            "Made with \(Exporter.Provenance.software) (Motion): \(made)",
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
