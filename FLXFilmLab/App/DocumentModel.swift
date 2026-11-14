import Foundation
import SwiftUI
import Combine
import CoreGraphics

/// The open image, its edits, and the loop that keeps the canvas in step with
/// the controls.
///
/// Rendering runs on `RenderEngine`, off the main actor, so a slider drag never
/// waits for the GPU: the main actor only records the new value and shows each
/// frame as it arrives. Interactive edits render a proxy sized for the canvas;
/// export re-decodes at native size and runs the same graph.
///
/// Opening another image:
/// - The image on screen keeps everything until the new one has actually
///   decoded, so a file that fails to open changes nothing.
/// - Each image's edits are remembered for the session: going back to one
///   restores it as it was.
/// - An image opened for the first time keeps the look — film, paper, film
///   character, output, which autos are on — while everything dialled for one
///   frame (exposure, white balance, tone, colour, masks) starts fresh.
/// - Each image keeps its own undo history for the session too (`history`).
@MainActor
final class DocumentModel: ObservableObject {

    /// Everything that belongs to one image. Codable for the library and the
    /// sidecars (`ImageLibrary`); every key but the recipe optional, so a file
    /// written by an earlier build still reads.
    struct ImageEdits: Equatable, Codable {
        var recipe: Recipe
        var whiteBalance: RawLoader.WhiteBalance
        var customTemperature: Double
        var customTint: Double

        init(recipe: Recipe, whiteBalance: RawLoader.WhiteBalance, customTemperature: Double, customTint: Double) {
            self.recipe = recipe
            self.whiteBalance = whiteBalance
            self.customTemperature = customTemperature
            self.customTint = customTint
        }

        private enum CodingKeys: String, CodingKey { case recipe, whiteBalance, customTemperature, customTint }

        init(from decoder: Decoder) throws {
            let c = try decoder.container(keyedBy: CodingKeys.self)
            recipe = try c.decode(Recipe.self, forKey: .recipe)
            whiteBalance = try c.decodeIfPresent(RawLoader.WhiteBalance.self, forKey: .whiteBalance) ?? .filmReference
            customTemperature = try c.decodeIfPresent(Double.self, forKey: .customTemperature) ?? 5500
            customTint = try c.decodeIfPresent(Double.self, forKey: .customTint) ?? 0
        }
    }

    @Published var recipe = Recipe() {
        didSet {
            edited { $0.recipe = oldValue }
            scheduleRecord()
            guard !applyingEdits else { return }
            // The film's reference illuminant sets the decode white balance, so
            // changing stock in that mode means re-decoding.
            if whiteBalance == .filmReference, oldValue.filmStock != recipe.filmStock {
                syncSource()
            }
            scheduleRender()
        }
    }
    /// What each frame brings (`PrintPreview`), kept apart from the edits: a
    /// finished frame redraws only the views that show it, not every control
    /// of the inspector, which once took the main thread's every moment
    /// while a slider was dragged.
    let preview = PrintPreview()
    var displayImage: CGImage? { preview.image }
    /// The print's histogram as it is on screen, and before its curves.
    var histogram: PrintHistogram? { preview.histogram }
    /// Where the image on screen comes from in the frame: the print as cut,
    /// or, while cropping, the whole frame turned and levelled.
    var displayMap: GeometryMap? { preview.map }
    var isRendering: Bool { preview.isRendering }
    @Published private(set) var status: String = "Open a RAW file to begin"
    @Published private(set) var errorMessage: String?
    /// The image on screen, the one being edited. It changes only once a newly
    /// opened file has decoded.
    @Published private(set) var fileURL: URL?
    /// A file that has been opened but not yet decoded.
    @Published private(set) var pendingURL: URL? {
        didSet { updateBusy() }
    }
    @Published var whiteBalance: RawLoader.WhiteBalance = .filmReference {
        didSet {
            edited { $0.whiteBalance = oldValue }
            scheduleRecord()
            if !applyingEdits, oldValue != whiteBalance { syncSource() }
        }
    }
    @Published var customTemperature: Double = 5500 {
        didSet {
            edited { $0.customTemperature = oldValue }
            scheduleRecord()
            if !applyingEdits, whiteBalance == .custom { syncSource() }
        }
    }
    @Published var customTint: Double = 0 {
        didSet {
            edited { $0.customTint = oldValue }
            scheduleRecord()
            if !applyingEdits, whiteBalance == .custom { syncSource() }
        }
    }
    /// Before and after (Original, \): the RAW as macOS renders it, as
    /// shot, in the print's place. Decoded the first time it is asked for, for
    /// the picture on screen alone (0.07-0.4 s at 2560 px on an M4 Pro, 17 MB),
    /// placed again when the crop or the turn moves; nothing at all until then.
    @Published var showOriginal = false {
        didSet {
            guard oldValue != showOriginal else { return }
            if showOriginal {
                updateOriginal()
            } else {
                originalDecode?.cancel()
                originalPlacing?.cancel()
                originalImage = nil
            }
        }
    }
    /// The original in the print's place, while it is shown.
    @Published private(set) var originalImage: CGImage?
    /// The panels the window's toolbar asks for, which ContentView puts up:
    /// Open's, Export's, and the sheet that names a new preset. The toolbar is
    /// the window's (RootView's), the same for both halves, not ContentView's.
    @Published var showsOpenPanel = false
    @Published var showsExportPanel = false
    @Published var showsPresetNaming = false
    /// The original of the picture on screen, whole, at the preview's size.
    private var originalFrame: (url: URL, longEdge: Int, image: CGImage)?
    private var originalDecode: Task<Void, Never>?
    private var originalPlacing: Task<Void, Never>?
    private var originalWatch: [AnyCancellable] = []
    var lastRenderMilliseconds: Double { preview.milliseconds }
    /// What the meters read, for the controls whose auto is on. The sliders
    /// show these while their auto is on.
    @Published private(set) var meteredExposureEV: Double?
    @Published private(set) var meteredBrightness: Double?
    @Published private(set) var meteredGrade: Double?
    /// The recipe as last rendered, with the meters' choices filled in: the
    /// print actually being made. No view shows it, so it is not published.
    private(set) var resolvedRecipe: Recipe?
    @Published private(set) var isDecoding = false {
        didSet { updateBusy() }
    }
    /// A decode or an open that has taken long enough to show, 0.3 s: what the
    /// toolbar, the status line and the white balance's note wait for, so that
    /// stepping through pictures decoded ahead, a few ms each, does not flash
    /// them. Export and Copy Settings still refuse for as long as one is under
    /// way (`canExport`, `canCopySettings`).
    @Published private(set) var isBusy = false
    private var busyTask: Task<Void, Never>?

    private func updateBusy() {
        guard pendingURL != nil || isDecoding else {
            busyTask?.cancel()
            busyTask = nil
            if isBusy { isBusy = false }
            if status.hasPrefix("Opening ") { status = statusLine }
            return
        }
        guard busyTask == nil, !isBusy else { return }
        busyTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(300))
            guard let self, !Task.isCancelled else { return }
            self.busyTask = nil
            guard self.pendingURL != nil || self.isDecoding else { return }
            self.isBusy = true
            if let pending = self.pendingURL { self.status = "Opening \(pending.lastPathComponent)…" }
        }
    }

    /// Longest edge of the interactive proxy, one of `previewSizes`. At 2560,
    /// big enough to judge tone and colour, small enough that a print control
    /// re-renders it in about 29 ms and an exposure control in 50 ms on an M4
    /// Pro; at 4096, 73 and 122 ms (README, Performance). The choice is kept
    /// between launches.
    var proxyMaxDimension: Int {
        didSet {
            defaults.set(proxyMaxDimension, forKey: Self.previewSizeKey)
            syncSource()
        }
    }
    static let previewSizes = [1024, 2048, 2560, 3072, 4096]
    static let defaultPreviewSize = 2560
    private static let previewSizeKey = "previewSize"
    private let defaults: UserDefaults

    /// One decode: the file and every option that changes its pixels.
    struct DecodeKey: Hashable {
        var url: URL
        var whiteBalance: RawLoader.WhiteBalance
        var referenceIlluminant: String
        var temperature: Double
        var tint: Double
        var maxDimension: Int?

        var options: RawLoader.Options {
            RawLoader.Options(whiteBalance: whiteBalance, filmReference: referenceIlluminant,
                              temperature: temperature, tint: tint, maxDimension: maxDimension)
        }
    }

    /// What the engine holds right now.
    private struct Loaded {
        var key: DecodeKey
        var sourceID: Int
        var width: Int
        var height: Int
        /// The photograph's settings in one line: the status (`PhotoLine`).
        var photo: String
    }

    private let engine: RenderEngine?
    private var loaded: Loaded?
    /// The decode in flight, if any.
    private var requested: DecodeKey?
    /// Monotonic token for decode requests.
    ///
    /// Every white-balance change re-decodes the RAW, and a slider drag fires
    /// one per tick. Without this, whichever decode *finished* last won rather
    /// than whichever was *asked for* last — so the temperature you let go of
    /// was often not the one on screen.
    private var sourceGeneration = 0
    private var decodeTask: Task<Void, Never>?
    private var editsByImage: [URL: ImageEdits] = [:]
    /// The undo history of each image that has been on screen, but the one
    /// that is: that one's is `history`'s.
    private var historyByImage: [URL: EditHistory<ImageEdits>.Stacks] = [:]
    /// Set while one image's edits are swapped in for another's, so the swap
    /// does not trigger re-decodes and renders of its own half-way through.
    private var applyingEdits = false
    private var renderInFlight = false
    private var renderAgain = false

    /// Each picture's own settings and flag, kept between launches.
    let library: ImageLibrary
    /// The folder in the sidebar, its selection and flags.
    let browser = FolderBrowser()
    /// What each picture opened with this session: one whose edits still
    /// equal these has nothing of its own to keep in the library.
    private var openedWith: [URL: ImageEdits] = [:]
    /// The picture on screen and the two either side of it in the sidebar,
    /// decoded at the preview's size, so stepping to a neighbour shows it at
    /// once. The one on screen shares its pixels with the engine.
    private var decoded: [DecodeKey: RawLoader.Decoded] = [:]
    private var prefetchTask: Task<Void, Never>?
    /// The pictures visited, the most recent last: only the last
    /// `historiesKept` keep their undo history, so a session through hundreds
    /// of pictures does not hold hundreds of histories.
    private var visited: [URL] = []
    static let historiesKept = 30
    private var recordTask: Task<Void, Never>?
    private var printTask: Task<Void, Never>?
    private var memoryPressure: DispatchSourceMemoryPressure?
    private var batchCancel: BatchCancel?

    init(presetStore: PresetStore = PresetStore(), defaults: UserDefaults = .standard,
         library: ImageLibrary? = nil) {
        self.defaults = defaults
        let saved = defaults.integer(forKey: Self.previewSizeKey)
        proxyMaxDimension = Self.previewSizes.contains(saved) ? saved : Self.defaultPreviewSize
        let made = Result { try RenderEngine() }
        engine = try? made.get()
        if case .failure(let error) = made { errorMessage = error.localizedDescription }
        self.presetStore = presetStore
        presets = presetStore.load()
        self.library = library ?? ImageLibrary(defaults: defaults)
        browser.library = self.library
        browser.ownEdits = { [weak self] url in self?.ownEdits(url) }
        // The original follows the picture on screen and the print's shape.
        // @Published sends before it sets: the new map is the one passed.
        originalWatch = [
            preview.$map
                .removeDuplicates { a, b in
                    a?.sourceSize == b?.sourceSize && a?.crop == b?.crop && a?.sourceToOutput == b?.sourceToOutput
                }
                .sink { [weak self] map in
                    MainActor.assumeIsolated {
                        guard let self, self.showOriginal, let frame = self.originalFrame,
                              frame.url == self.fileURL else { return }
                        if frame.longEdge != self.proxyMaxDimension {
                            self.updateOriginal()
                        } else {
                            self.placeOriginal(frame.image, url: frame.url, map: map)
                        }
                    }
                },
            $fileURL
                .removeDuplicates()
                .sink { [weak self] url in
                    MainActor.assumeIsolated {
                        guard let self, url != self.originalFrame?.url else { return }
                        // never the last picture's original over this one's print
                        self.originalImage = nil
                        self.originalFrame = nil
                        self.updateOriginal(url: url)
                    }
                },
        ]
        // Short of memory, the neighbours' decodes and the sidebar's pictures
        // go first.
        let pressure = DispatchSource.makeMemoryPressureSource(eventMask: [.warning, .critical], queue: .main)
        pressure.setEventHandler { [weak self] in
            MainActor.assumeIsolated { self?.memoryRanShort() }
        }
        pressure.activate()
        memoryPressure = pressure
        NotificationCenter.default.addObserver(forName: NSApplication.willTerminateNotification, object: nil,
                                               queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.recordCurrent()
                self?.library.save(wait: true)
                self?.library.finishWriting()
            }
        }
    }

    var hasImage: Bool { loaded != nil }

    /// Export renders the image on screen with the values on screen, so it
    /// waits until nothing is still being decoded.
    var canExport: Bool { loaded != nil && pendingURL == nil && !isDecoding }

    var filmEntries: [PackIndexEntry] { PackLibrary.shared.filmStocks }
    var paperEntries: [PackIndexEntry] { PackLibrary.shared.printStocks }

    var filmPack: ProfilePack? { try? PackLibrary.shared.pack(recipe.filmStock) }
    var paperPack: ProfilePack? { try? PackLibrary.shared.pack(recipe.printStock) }

    // MARK: - Opening

    func open(url given: URL) {
        let url = FolderBrowser.canonical(given)
        errorMessage = nil
        showFolder(of: url)
        if url == fileURL {
            // Back to the image already on screen: forget a pending open.
            pendingURL = nil
        } else {
            pendingURL = url
            // The sidebar marks it at once; the status line says so only if
            // it takes a while (`isBusy`).
            browser.current = url
        }
        syncSource()
    }

    private var currentEdits: ImageEdits {
        ImageEdits(recipe: recipe, whiteBalance: whiteBalance,
                   customTemperature: customTemperature, customTint: customTint)
    }

    /// The edits an image opens with: its own if it has been open before this
    /// session, otherwise the current look with a fresh start for the rest.
    private func edits(for url: URL) -> ImageEdits {
        if url == fileURL { return currentEdits }
        if let remembered = editsByImage[url] { return remembered }
        if let saved = library.record(for: url)?.edits { return saved }
        let fresh = recipe.carryingLook()
        return ImageEdits(recipe: fresh, whiteBalance: .filmReference,
                          customTemperature: 5500, customTint: 0)
    }

    private func decodeKey(for url: URL) -> DecodeKey {
        let edits = edits(for: url)
        let film = try? PackLibrary.shared.pack(edits.recipe.filmStock)
        return DecodeKey(url: url, whiteBalance: edits.whiteBalance,
                         referenceIlluminant: film?.referenceIlluminant ?? "D55",
                         temperature: edits.customTemperature, tint: edits.customTint,
                         maxDimension: proxyMaxDimension)
    }

    /// Make the engine hold what the current state asks for: the pending file
    /// if there is one, otherwise the image on screen, decoded with that
    /// image's white balance. Decodes only if that is neither loaded nor on its
    /// way already.
    private func syncSource() {
        guard let target = pendingURL ?? fileURL else { return }
        let key = decodeKey(for: target)
        if key == requested { return }
        if key == loaded?.key {
            // Already there; anything still in flight is stale.
            if requested != nil {
                decodeTask?.cancel()
                sourceGeneration += 1
                requested = nil
                isDecoding = false
            }
            return
        }
        requested = key
        sourceGeneration += 1
        let generation = sourceGeneration
        isDecoding = true
        decodeTask?.cancel()
        if let ready = decoded[key] {
            // Decoded ahead of time, as a neighbour: no settle, no decode.
            decodeTask = Task { [weak self] in
                await self?.finishDecode(.success(ready), key: key, generation: generation)
            }
            return
        }
        decodeTask = Task { [weak self] in
            // A short settle so dragging the temperature slider decodes once at
            // the end instead of once per tick — a RAW decode is ~0.5 s, far
            // longer than a slider frame.
            try? await Task.sleep(for: .milliseconds(120))
            if Task.isCancelled { return }
            let result = await Task.detached(priority: .userInitiated) {
                Result { try RawLoader.load(url: key.url, options: key.options) }
            }.value
            guard let self, !Task.isCancelled else { return }
            await self.finishDecode(result, key: key, generation: generation)
        }
    }

    private func finishDecode(_ result: Result<RawLoader.Decoded, Error>,
                              key: DecodeKey, generation: Int) async {
        // Ignore a decode that a newer request has already superseded.
        guard generation == sourceGeneration else { return }
        switch result {
        case .failure(let error):
            requested = nil
            isDecoding = false
            if key.url == pendingURL {
                // A new file that will not open changes nothing: the image on
                // screen stays, with its edits.
                pendingURL = nil
                browser.current = fileURL
                errorMessage = "Could not open \(key.url.lastPathComponent): \(error.localizedDescription)"
                status = statusLine
            } else {
                errorMessage = error.localizedDescription
                status = "Could not decode \(key.url.lastPathComponent)"
            }
        case .success(let picture):
            guard let engine else { return }
            let sourceID = await engine.setSource(picture, grainSeed: Renderer.grainSeed(for: key.url))
            decoded[key] = picture
            adopt(picture, key: key, sourceID: sourceID)
        }
    }

    /// The engine now holds `decoded`; bring the model in line with it.
    private func adopt(_ decoded: RawLoader.Decoded, key: DecodeKey, sourceID: Int) {
        if requested == key {
            requested = nil
            isDecoding = false
        }
        if key.url != fileURL {
            // Switching images: put the outgoing one's edits and undo history
            // away and bring the incoming one's out, all at once.
            if let outgoing = fileURL {
                recordCurrent()
                keepPrint(of: outgoing)
            }
            let steps = history.swap(for: historyByImage.removeValue(forKey: key.url) ?? .init())
            if let outgoing = fileURL {
                editsByImage[outgoing] = currentEdits
                historyByImage[outgoing] = steps
                visited.removeAll { $0 == outgoing }
                visited.append(outgoing)
                if visited.count > Self.historiesKept {
                    for old in visited.prefix(visited.count - Self.historiesKept) { historyByImage[old] = nil }
                    visited.removeFirst(visited.count - Self.historiesKept)
                }
            }
            let incoming = edits(for: key.url)
            if openedWith[key.url] == nil { openedWith[key.url] = incoming }
            history.unrecorded {
                applyingEdits = true
                recipe = incoming.recipe
                whiteBalance = incoming.whiteBalance
                customTemperature = incoming.customTemperature
                customTint = incoming.customTint
                isCropping = false
                applyingEdits = false
            }
            fileURL = key.url
            browser.current = key.url
            if pendingURL == key.url { pendingURL = nil }
            // The previous image's readings stay on the sliders until this
            // one's first frame brings its own, a few ms later: blanked, the
            // auto sliders jumped twice at every picture stepped to.
            resolvedRecipe = nil
        }
        let photo = PhotoLine.text(camera: PhotoLine.camera(make: decoded.cameraMake, model: decoded.cameraModel),
                                   focalLength: decoded.focalLength, shutter: decoded.exposureTime, iso: decoded.iso)
        loaded = Loaded(key: key, sourceID: sourceID, width: decoded.width, height: decoded.height,
                        photo: photo)
        status = statusLine
        syncLoupe()
        // The incoming edits may want a different decode (their own white
        // balance, or a stock with another reference illuminant).
        syncSource()
        scheduleRender()
        prefetchNeighbours()
    }

    /// The photograph as its camera took it: the camera, the focal length,
    /// the shutter speed and the ISO, as far as the file says (`PhotoLine`).
    private var statusLine: String {
        guard let loaded else { return "Open a RAW file to begin" }
        return loaded.photo
    }

    // MARK: - Stocks

    /// Change film the way the stock picker does: crossing between colour and
    /// black and white also moves the print onto the new film's own paper, and
    /// a motion picture film onto its own print film (`Recipe.loadingFilm`).
    func selectFilm(_ stock: String) {
        guard stock != recipe.filmStock else { return }
        recipe = recipe.loadingFilm(stock)
    }

    // MARK: - Copy and paste settings

    /// Settings copied from one image to paste onto others, with the name of
    /// the image they came from.
    struct CopiedSettings {
        var edits: ImageEdits
        var source: String
    }

    @Published private(set) var copiedSettings: CopiedSettings? {
        didSet { browser.hasCopiedSettings = copiedSettings != nil }
    }

    var canCopySettings: Bool { fileURL != nil && pendingURL == nil }
    var canPasteSettings: Bool { copiedSettings != nil && canCopySettings }

    func copySettings() {
        guard canCopySettings, let name = fileURL?.lastPathComponent else { return }
        copiedSettings = CopiedSettings(edits: currentEdits, source: name)
    }

    /// Paste the copied settings onto the image on screen.
    func pasteSettings() {
        guard canPasteSettings, let copied = copiedSettings?.edits else { return }
        history.named("Paste Settings") { put(copied) }
    }

    /// Put copied or saved settings on the image on screen: film, paper,
    /// print or light table, camera filter, every slider and auto, and white
    /// balance. Masks, gradients and the crop belong to the frame they were
    /// made for, so this image keeps its own.
    private func put(_ edits: ImageEdits) {
        var pasted = edits
        pasted.recipe.masks = recipe.masks
        pasted.recipe.gradients = recipe.gradients
        pasted.recipe.geometry = recipe.geometry
        restore(pasted)
    }

    /// Put a whole set of edits on the image on screen at once. White balance
    /// is decoded in, and a film-reference balance follows the stock: decode
    /// again if either moved.
    private func restore(_ edits: ImageEdits) {
        applyingEdits = true
        recipe = edits.recipe
        whiteBalance = edits.whiteBalance
        customTemperature = edits.customTemperature
        customTint = edits.customTint
        applyingEdits = false
        syncSource()
        scheduleRender()
    }

    // MARK: - Presets

    /// The saved presets, by name.
    @Published private(set) var presets: [Preset] = []
    private let presetStore: PresetStore

    /// Read the presets again, for a file added or taken away in Finder.
    func reloadPresets() { presets = presetStore.load() }

    /// Save what Copy Settings would copy as a preset called `name`,
    /// replacing one of that name.
    func savePreset(named name: String) {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard canCopySettings, !trimmed.isEmpty else { return }
        let preset = Preset(name: trimmed, recipe: recipe, whiteBalance: whiteBalance,
                            customTemperature: customTemperature, customTint: customTint)
        do {
            try presetStore.save(preset)
        } catch {
            errorMessage = "Could not save the preset \(trimmed): \(error.localizedDescription)"
        }
        reloadPresets()
    }

    /// Put a preset on the image on screen, as Paste Settings does.
    func applyPreset(_ preset: Preset) {
        guard canCopySettings else { return }
        let films = Set(filmEntries.map(\.stock)), papers = Set(paperEntries.map(\.stock))
        guard films.contains(preset.recipe.filmStock), papers.contains(preset.recipe.printStock) else {
            errorMessage = "The preset \(preset.name) uses a film or paper this version does not have."
            return
        }
        history.named("Apply Preset") {
            put(ImageEdits(recipe: preset.recipe, whiteBalance: preset.whiteBalance,
                           customTemperature: preset.customTemperature, customTint: preset.customTint))
        }
    }

    func deletePreset(_ preset: Preset) {
        do {
            try presetStore.delete(preset)
        } catch {
            errorMessage = "Could not delete the preset \(preset.name): \(error.localizedDescription)"
        }
        reloadPresets()
    }

    /// The presets' folder, made if need be.
    func presetsFolder() -> URL? {
        do { return try presetStore.ensuredDirectory() } catch {
            errorMessage = "Could not make the presets folder: \(error.localizedDescription)"
            return nil
        }
    }

    // MARK: - White balance

    /// What the decode is actually balanced for, so the setting is never a
    /// guess about whether it took effect.
    var whiteBalanceSummary: String {
        if isBusy && isDecoding { return "decoding…" }
        switch whiteBalance {
        case .filmReference:
            let name = filmPack?.referenceIlluminant ?? "D55"
            return String(format: "%@ · %.0f K", name.hasPrefix("BB") ? "Tungsten" : name, filmReferenceTemperature)
        case .asShot: return "camera as-shot"
        case .daylightD55: return "D55 · 5503 K"
        case .daylight: return "D65 · 6504 K"
        case .tungsten3200: return "3200 K"
        case .tungsten: return "2850 K"
        case .custom: return String(format: "%.0f K · tint %+.0f", customTemperature, customTint)
        }
    }

    /// The illuminant this stock is balanced for, as a colour temperature.
    var filmReferenceTemperature: Double {
        RawLoader.referenceTemperature(filmPack?.referenceIlluminant ?? "D55")
    }

    /// True when the loaded stock is being read as a transparency rather than
    /// printed, which changes which controls are meaningful. An instant print
    /// is the print, seen as it is.
    var isTransparency: Bool {
        guard let film = filmPack else { return recipe.scanFilm ?? false }
        return film.isViewedDirectly(scanFilm: recipe.scanFilm)
    }

    // MARK: - Rendering

    /// Ask for a frame. While one is being made, further requests collapse into
    /// a single follow-up with whatever the recipe is by then, so a fast drag
    /// renders its latest position rather than queueing every tick.
    func scheduleRender() {
        guard let engine, let loaded else { return }
        if renderInFlight {
            renderAgain = true
            return
        }
        renderInFlight = true
        preview.isRendering = true
        let recipe = self.recipe
        let sourceID = loaded.sourceID
        let wholeFrame = isCropping
        let metering = shapeBeforeCropping
        Task { [weak self] in
            let result: Result<RenderEngine.Output?, Error>
            do {
                result = .success(try await engine.render(recipe, for: sourceID, wholeFrame: wholeFrame,
                                                          metering: metering))
            } catch { result = .failure(error) }
            self?.finishRender(result, sourceID: sourceID)
        }
    }

    private func finishRender(_ result: Result<RenderEngine.Output?, Error>, sourceID: Int) {
        renderInFlight = false
        preview.isRendering = false
        switch result {
        case .success(let output):
            // A frame for an image that has since been replaced is dropped;
            // the follow-up render below draws the new one.
            if let output, sourceID == loaded?.sourceID {
                preview.show(output)
                resolvedRecipe = output.resolved
                // Published only when they move: the inspector redraws for them.
                if meteredExposureEV != output.metered.exposureEV { meteredExposureEV = output.metered.exposureEV }
                if meteredBrightness != output.metered.brightness { meteredBrightness = output.metered.brightness }
                if meteredGrade != output.metered.grade { meteredGrade = output.metered.grade }
                requestLoupe()
                schedulePrintKeep()
            }
        case .failure(let error):
            errorMessage = error.localizedDescription
            meteredExposureEV = nil
            meteredBrightness = nil
            meteredGrade = nil
        }
        if renderAgain || sourceID != loaded?.sourceID {
            renderAgain = false
            scheduleRender()
        }
    }

    // MARK: - Export

    func export(to url: URL, format: Exporter.Format) async {
        guard let engine, let loaded, canExport else {
            errorMessage = "Wait for the image to finish loading, then export."
            return
        }
        status = "Exporting at full resolution…"
        var options = loaded.key.options
        options.maxDimension = nil
        let source = loaded.key.url

        do {
            // Exactly the values on screen, metered on the same decode.
            let resolved = try await engine.resolve(recipe).recipe
            try await Task.detached(priority: .userInitiated) {
                try Self.printFullSize(source, options: options, resolved: resolved, to: url, format: format)
            }.value
            status = "Exported \(url.lastPathComponent)"
        } catch {
            errorMessage = error.localizedDescription
            status = statusLine
        }
    }

    // MARK: - The folder, flags and batches

    /// Open a folder: its pictures in the sidebar, and the first of them on
    /// screen, unless the picture on screen is one of them.
    func openFolder(_ given: URL) {
        let folder = FolderBrowser.canonical(given)
        errorMessage = nil
        list(folder)
        guard let first = browser.shown.first?.url ?? browser.items.first?.url else {
            status = "No pictures in \(folder.lastPathComponent)"
            return
        }
        if let current = fileURL, browser.items.contains(where: { $0.url == current }) {
            browser.selection = [current]
            return
        }
        browser.selection = [first]
        open(url: first)
    }

    /// The sidebar lists the folder of the picture opened, with it selected.
    private func showFolder(of url: URL) {
        let folder = url.deletingLastPathComponent()
        if browser.folder?.standardizedFileURL != folder.standardizedFileURL { list(folder) }
        if !browser.selection.contains(url) { browser.selection = [url] }
    }

    private func list(_ folder: URL) {
        let items = FolderBrowser.listing(folder)
        let records = library.load(folder, files: items.map { ($0.url, $0.size) })
        browser.show(folder, items: items, records: records)
        prefetchNeighbours()
    }

    /// A picture's own settings, if it has any: none for one that would open
    /// with the look on screen.
    private func ownEdits(_ url: URL) -> ImageEdits? {
        guard browser.edited.contains(url) else { return nil }
        if url == fileURL { return currentEdits }
        return editsByImage[url] ?? library.record(for: url)?.edits
    }

    /// The edits of the picture on screen go into the library once they have
    /// settled, if they are its own: changed since it opened, or already kept.
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
        guard let url = fileURL else { return }
        let edits = currentEdits
        if let kept = library.record(for: url)?.edits {
            guard kept != edits else { return }
        } else {
            guard let opened = openedWith[url], opened != edits else { return }
        }
        library.update(url) { $0.edits = edits }
        browser.markEdited(url)
    }

    /// The print on screen, shrunk, as the sidebar's picture of a picture with
    /// settings of its own, once the frames have stopped coming.
    private func schedulePrintKeep() {
        printTask?.cancel()
        printTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(1500))
            guard !Task.isCancelled, let self, let url = self.fileURL else { return }
            self.keepPrint(of: url)
        }
    }

    private func keepPrint(of url: URL) {
        guard url == fileURL, browser.edited.contains(url), !isCropping, !showOriginal, !preview.isRendering,
              let image = preview.image else { return }
        browser.thumbnails.setPrint(image, for: url, edits: currentEdits)
    }

    /// Decode the pictures either side of the one on screen, at the preview's
    /// size, after it has had its first frame; keep no other decodes.
    private func prefetchNeighbours() {
        prefetchTask?.cancel()
        guard let current = fileURL else { return }
        let neighbours = [browser.neighbour(of: current, offset: 1), browser.neighbour(of: current, offset: -1)]
            .compactMap { $0 }.filter { $0 != current }
        let wanted = neighbours.map { decodeKey(for: $0) }
        let keep = Set(wanted + [loaded?.key, requested].compactMap { $0 })
        decoded = decoded.filter { keep.contains($0.key) }
        let missing = wanted.filter { decoded[$0] == nil }
        guard !missing.isEmpty else { return }
        prefetchTask = Task { [weak self] in
            for key in missing {
                try? await Task.sleep(for: .milliseconds(150))
                if Task.isCancelled { return }
                let picture = await Task.detached(priority: .utility) {
                    try? RawLoader.load(url: key.url, options: key.options)
                }.value
                guard let self, !Task.isCancelled else { return }
                if let picture { self.decoded[key] = picture }
            }
        }
    }

    /// The pictures decoded ahead of time, and how many undo histories are
    /// kept, for the tests.
    var prefetchedPictures: Set<URL> { Set(decoded.keys.map(\.url)) }
    var historiesHeld: Int { historyByImage.count }

    private func memoryRanShort() {
        prefetchTask?.cancel()
        decoded = decoded.filter { $0.key == loaded?.key }
        browser.thumbnails.trim()
        if !showOriginal { originalFrame = nil }
    }

    /// The original of the picture on screen, decoded once for it, then put
    /// in the print's place as the print stands when the decode is done.
    private func updateOriginal(url givenURL: URL? = nil) {
        guard showOriginal, let url = givenURL ?? fileURL else { return }
        let longEdge = proxyMaxDimension
        if let frame = originalFrame, frame.url == url, frame.longEdge == longEdge {
            placeOriginal(frame.image, url: url)
            return
        }
        originalDecode?.cancel()
        originalDecode = Task { [weak self] in
            let frame = await Task.detached(priority: .userInitiated) {
                RawLoader.original(url: url, longEdge: longEdge)
            }.value
            guard let self, !Task.isCancelled, self.showOriginal, self.fileURL == url, let frame else { return }
            self.originalFrame = (url, longEdge, frame)
            self.placeOriginal(frame, url: url)
        }
    }

    /// The decoded original turned, levelled and cropped as the print is.
    private func placeOriginal(_ frame: CGImage, url: URL, map given: GeometryMap? = nil) {
        guard let map = given ?? preview.map else { return }
        originalPlacing?.cancel()
        originalPlacing = Task { [weak self] in
            let placed = await Task.detached(priority: .userInitiated) { RawLoader.placed(frame, on: map) }.value
            guard let self, !Task.isCancelled, self.showOriginal, self.fileURL == url else { return }
            self.originalImage = placed
        }
    }

    /// Flag the selection, or the picture on screen when nothing is selected:
    /// P, X and U in the sidebar.
    func flagSelection(_ flag: ImageLibrary.Flag?) {
        var targets = browser.items.map(\.url).filter { browser.selection.contains($0) }
        if targets.isEmpty, let current = fileURL { targets = [current] }
        self.flag(flag, for: targets)
    }

    /// Flag those pictures alone, the selection as it is: a row's button.
    func flag(_ flag: ImageLibrary.Flag?, for urls: [URL]) {
        for url in urls {
            library.update(url) { $0.flag = flag }
            browser.setFlag(flag, for: url)
        }
    }

    /// Step to the picture `offset` places along the sidebar.
    func showNeighbour(_ offset: Int) {
        guard let from = fileURL ?? browser.shown.first?.url,
              let next = browser.neighbour(of: from, offset: offset), next != fileURL else { return }
        browser.selection = [next]
        open(url: next)
    }

    /// Paste the copied settings onto every picture selected but the rejects,
    /// as Paste Settings puts them on the one on screen: each keeps its own
    /// masks, gradients and crop, and has the paste as a step of its own
    /// history, to undo when it is on screen.
    func pasteSettingsToSelected() {
        guard let copied = copiedSettings?.edits else { return }
        var changed: [(url: URL, edits: ImageEdits)] = []
        for url in browser.selectedForBatch {
            if url == fileURL {
                pasteSettings()
                continue
            }
            let before = edits(for: url)
            var pasted = copied
            pasted.recipe.masks = before.recipe.masks
            pasted.recipe.gradients = before.recipe.gradients
            pasted.recipe.geometry = before.recipe.geometry
            guard pasted != before else { continue }
            editsByImage[url] = pasted
            historyByImage[url] = EditHistory<ImageEdits>.adding("Paste Settings", before: before, after: pasted,
                                                                 to: historyByImage[url] ?? .init())
            if openedWith[url] == nil { openedWith[url] = before }
            library.update(url) { $0.edits = pasted }
            browser.markEdited(url)
            changed.append((url, pasted))
        }
        browser.thumbnails.refresh(changed)
        prefetchNeighbours()
    }

    /// Print every picture selected but the rejects at full size into
    /// `folder`, one after another, each with its own settings, or, never
    /// opened, the look on screen with its autos as opening it would give it.
    /// Each is metered on its own preview-sized decode, so it prints as it
    /// would on screen. The names are the picture's and the film's, as
    /// Export's; a name already taken gets a number.
    func exportSelected(to folder: URL, format: Exporter.Format) {
        guard browser.batch == nil else { return }
        recordCurrent()
        let urls = browser.selectedForBatch
        guard !urls.isEmpty else { return }
        var taken = Set<String>()
        let jobs: [(url: URL, edits: ImageEdits, target: URL)] = urls.map { url in
            let edits = edits(for: url)
            let film = (try? PackLibrary.shared.pack(edits.recipe.filmStock))?.name
                .replacingOccurrences(of: " ", with: "") ?? "film"
            let stem = url.deletingPathExtension().lastPathComponent + "_" + film
            var name = stem + "." + format.fileExtension
            var n = 2
            while taken.contains(name.lowercased())
                    || FileManager.default.fileExists(atPath: folder.appendingPathComponent(name).path) {
                name = "\(stem)-\(n).\(format.fileExtension)"
                n += 1
            }
            taken.insert(name.lowercased())
            return (url, edits, folder.appendingPathComponent(name))
        }
        let cancel = BatchCancel()
        batchCancel = cancel
        let proxy = proxyMaxDimension
        browser.batch = .init(done: 0, total: jobs.count, current: jobs[0].url.lastPathComponent, folder: folder)
        Task { [weak self] in
            var failures: [String] = []
            for (i, job) in jobs.enumerated() {
                if cancel.isCancelled { break }
                self?.browser.batch?.done = i
                self?.browser.batch?.current = job.url.lastPathComponent
                let result = await Task.detached(priority: .utility) {
                    Result { try DocumentModel.printSelected(job.url, edits: job.edits, previewSize: proxy,
                                                             to: job.target, format: format) }
                }.value
                if case .failure(let error) = result {
                    failures.append("\(job.url.lastPathComponent): \(error.localizedDescription)")
                }
            }
            self?.finishBatch(failures: failures, total: jobs.count, cancelled: cancel.isCancelled, folder: folder)
        }
    }

    func cancelBatch() { batchCancel?.cancel() }

    private func finishBatch(failures: [String], total: Int, cancelled: Bool, folder: URL) {
        let done = browser.batch?.done ?? 0
        browser.batch = nil
        batchCancel = nil
        if cancelled {
            status = "Export stopped after \(done) of \(total)"
        } else if failures.isEmpty {
            status = "Exported \(total) print\(total == 1 ? "" : "s") to \(folder.lastPathComponent)"
        } else {
            errorMessage = "Exported \(total - failures.count) of \(total); not exported: "
                + failures.prefix(5).joined(separator: "; ") + (failures.count > 5 ? "; …" : "")
        }
    }

    /// One picture of a batch, as Export prints the one on screen: metered on
    /// its decode at the preview's size, then printed at full size.
    nonisolated static func printSelected(_ url: URL, edits: ImageEdits, previewSize: Int, to target: URL,
                                          format: Exporter.Format) throws {
        let film = try? PackLibrary.shared.pack(edits.recipe.filmStock)
        var options = RawLoader.Options(whiteBalance: edits.whiteBalance,
                                        filmReference: film?.referenceIlluminant ?? "D55",
                                        temperature: edits.customTemperature, tint: edits.customTint,
                                        maxDimension: previewSize)
        let resolved = try autoreleasepool {
            let proxy = try RawLoader.load(url: url, options: options)
            return try RenderEngine.resolved(edits.recipe, pixels: proxy.pixels, width: proxy.width,
                                             height: proxy.height)
        }
        options.maxDimension = nil
        try printFullSize(url, options: options, resolved: resolved, to: target, format: format)
    }

    /// Decode `source` at full size and print it with `resolved`, every meter
    /// already read, into `url`: Export's render, the batch's too.
    nonisolated static func printFullSize(_ source: URL, options: RawLoader.Options, resolved: Recipe, to url: URL,
                                          format: Exporter.Format) throws {
        try autoreleasepool {
            let full = try RawLoader.load(url: source, options: options)
            let renderer = try Renderer()
            renderer.setImage(full.pixels, width: full.width, height: full.height)
            renderer.grainSeed = Renderer.grainSeed(for: source)
            let setup = try RenderSetupBuilder.build(recipe: resolved, exposureEV: Float(resolved.exposureEV))
            let masks = resolved.masks.filter { $0.enabled }
            if !masks.isEmpty {
                renderer.setDodgeBurnMap(MaskRasterizer.rasterize(masks: masks, width: full.width, height: full.height))
            }
            _ = try renderer.render(setup: setup, recipe: resolved)
            guard let linear = renderer.readLinear() else { throw Exporter.ExportError.renderFailed }
            try Exporter.write(linear: linear, width: Int(renderer.outputSize.width),
                               height: Int(renderer.outputSize.height),
                               to: url, format: format, colorSpace: resolved.outputColorSpace,
                               metadata: RawLoader.metadata(of: source),
                               provenance: Exporter.Provenance(recipe: resolved))
        }
    }

    // MARK: - The loupe

    /// The loupe (⌘L): the print at 100%, one pixel of the export to one of
    /// the screen, under a square dragged over the print. While it is open
    /// the picture is held at full size, decoded as the export decodes it,
    /// and only the part under the loupe is printed from it, as the export
    /// prints those pixels.
    @Published var loupeOn = false {
        didSet {
            guard oldValue != loupeOn else { return }
            if loupeOn { syncLoupe() } else { closeLoupe() }
        }
    }
    /// The loupe's centre, in fractions of the print as it shows.
    @Published private(set) var loupeCentre = CGPoint(x: 0.5, y: 0.5)
    /// The print at 100% under it.
    var loupeImage: CGImage? { preview.loupeImage }
    /// The picture is being read at full size for it.
    @Published private(set) var loupeLoading = false
    /// The loupe's size in pixels of the screen, one to a pixel of the print.
    private var loupeSize = (width: 0, height: 0)
    private var loupeInFlight = false
    private var loupeAgain = false
    private var fullFrameTask: Task<Void, Never>?
    /// What the picture held at full size, or on its way, was decoded with.
    private var fullFrameKey: String?

    /// What the picture on screen decodes at full size with: the file and its
    /// white balance, as the proxy's (`DecodeKey`) without its size.
    private var wantedFullFrameKey: String? {
        guard let key = loaded?.key else { return nil }
        return "\(key.url.path)|\(key.whiteBalance)|\(key.referenceIlluminant)|\(key.temperature)|\(key.tint)"
    }

    /// Reading the picture or printing the loupe's part: for the tests.
    var loupeBusy: Bool { loupeLoading || loupeInFlight || loupeAgain }

    func moveLoupe(to centre: CGPoint) {
        loupeCentre = CGPoint(x: min(max(centre.x, 0), 1), y: min(max(centre.y, 0), 1))
        requestLoupe()
    }

    func setLoupeSize(width: Int, height: Int) {
        guard loupeSize != (width, height) else { return }
        loupeSize = (width, height)
        requestLoupe()
    }

    /// Hold the picture on screen at full size for the loupe, reading it
    /// again when what is held was decoded for another picture or white
    /// balance.
    private func syncLoupe() {
        guard loupeOn, let engine, let loaded, let key = wantedFullFrameKey else { return }
        if fullFrameKey == key {
            requestLoupe()
            return
        }
        fullFrameTask?.cancel()
        fullFrameKey = key
        loupeLoading = true
        preview.loupeImage = nil
        var options = loaded.key.options
        options.maxDimension = nil
        let url = loaded.key.url, sourceID = loaded.sourceID
        fullFrameTask = Task { [weak self] in
            let result = await Task.detached(priority: .userInitiated) {
                Result { try RawLoader.load(url: url, options: options) }
            }.value
            guard let self, !Task.isCancelled, self.fullFrameKey == key else { return }
            switch result {
            case .success(let decoded):
                let held = (try? await engine.holdFullFrame(decoded, key: key, for: sourceID)) ?? false
                guard !Task.isCancelled, self.fullFrameKey == key else { return }
                self.loupeLoading = false
                if held {
                    // The preview prints again, keeping its print before
                    // Bloom & Glow for the loupe's; the loupe follows it.
                    self.scheduleRender()
                } else if self.loaded?.sourceID != sourceID {
                    // Another picture came in meanwhile: read that one.
                    self.fullFrameKey = nil
                    self.syncLoupe()
                } else {
                    self.fullFrameKey = nil
                    self.loupeOn = false
                    self.errorMessage = "There is not enough memory to hold the picture at full size for the loupe."
                }
            case .failure(let error):
                self.loupeLoading = false
                self.fullFrameKey = nil
                self.loupeOn = false
                self.errorMessage = "Could not read the picture at full size: \(error.localizedDescription)"
            }
        }
    }

    private func closeLoupe() {
        fullFrameTask?.cancel()
        fullFrameTask = nil
        fullFrameKey = nil
        preview.loupeImage = nil
        loupeLoading = false
        loupeAgain = false
        if let engine { Task { await engine.releaseFullFrame() } }
    }

    /// Ask for the loupe's picture. While one is being made, further requests
    /// collapse into one follow-up with whatever is wanted by then, so a drag
    /// shows where the loupe is, not every place it has been.
    func requestLoupe() {
        guard loupeOn, !loupeLoading, !isCropping, let engine, let loaded, let key = fullFrameKey,
              loupeSize.width > 0 else { return }
        if loupeInFlight {
            loupeAgain = true
            return
        }
        loupeInFlight = true
        let recipe = self.recipe, centre = loupeCentre, size = loupeSize, sourceID = loaded.sourceID
        Task { [weak self] in
            let image = try? await engine.renderLoupe(recipe, centre: centre, width: size.width,
                                                      height: size.height, key: key, for: sourceID)
            guard let self else { return }
            self.loupeInFlight = false
            if let image, self.loupeOn { self.preview.loupeImage = image }
            if self.loupeAgain {
                self.loupeAgain = false
                self.requestLoupe()
            }
        }
    }

    // MARK: - Crop, turn and level

    /// While on, the canvas shows the whole frame, turned and levelled, with
    /// the crop drawn on it. The meters hold the reading they had until it is
    /// done, so the print does not change under the crop while it is drawn.
    @Published var isCropping = false {
        didSet {
            guard oldValue != isCropping else { return }
            shapeBeforeCropping = isCropping ? recipe.geometry : nil
            historyBeforeCropping = isCropping ? history.mark() : nil
            if !isCropping { isLevelling = false }
            if !applyingEdits { scheduleRender() }
            if !isCropping { requestLoupe() }
        }
    }
    /// The shape the print had when cropping began: what the meters read
    /// meanwhile, and what Cancel goes back to.
    private var shapeBeforeCropping: FrameGeometry?
    /// Where the undo history stood when cropping began.
    private var historyBeforeCropping: EditHistory<ImageEdits>.Mark?

    /// The level tool: the next line drawn on the frame levels it.
    @Published var isLevelling = false {
        didSet { if isLevelling { isCropping = true } }
    }

    /// Finish cropping, keeping the crop, or going back to the one before.
    ///
    /// A cancelled crop never happened: the steps it took come off the undo
    /// history, so the next ⌘Z undoes what came before it. When something
    /// other than the shape was changed meanwhile, going back is a step of
    /// its own instead.
    func endCropping(keep: Bool) {
        if !keep, let before = shapeBeforeCropping {
            let onlyShape: (ImageEdits, ImageEdits) -> Bool = { a, b in
                var a = a
                a.recipe.frameGeometry = b.recipe.frameGeometry
                return a == b
            }
            if let mark = historyBeforeCropping, history.rewind(to: mark, ifEach: onlyShape) {
                history.unrecorded { recipe.geometry = before }
            } else {
                history.named("Cancel Crop") { recipe.geometry = before }
            }
        }
        isCropping = false
    }

    /// The frame as it stands turned, in the proxy's pixels: what the crop's
    /// proportions are worked out on. Its crop is normalised, so it holds at
    /// any size.
    var turnedFrame: CGSize? {
        guard let loaded else { return nil }
        return recipe.geometry.turnedSize(width: Double(loaded.width), height: Double(loaded.height))
    }

    /// Change the print's shape, worked out on the frame as it stands.
    func editGeometry(_ change: (inout FrameGeometry, CGSize) -> Void) {
        guard let frame = turnedFrame else { return }
        var g = recipe.geometry
        change(&g, frame)
        recipe.geometry = g
    }

    func turn(clockwise: Bool) { editGeometry { g, _ in g.turn(clockwise: clockwise) } }
    func flip(horizontal: Bool) { editGeometry { g, _ in g.flip(horizontal: horizontal) } }
    func setCropAspect(_ aspect: CropAspect) { editGeometry { g, frame in g.setAspect(aspect, frame: frame) } }
    func swapCropOrientation() { editGeometry { g, frame in g.swapOrientation(frame: frame) } }

    func setCustomAspect(width: Double, height: Double) {
        guard width > 0, height > 0 else { return }
        editGeometry { g, frame in
            g.customWidth = width
            g.customHeight = height
            g.setAspect(.custom, frame: frame)
        }
    }

    func setStraighten(_ degrees: Double) { editGeometry { g, _ in g.setStraighten(degrees) } }

    /// Level the picture along a line drawn on it, from `a` to `b` in the
    /// frame as it shows while cropping (normalised): the line comes out
    /// level, or upright if it is nearer upright.
    func level(from a: CGPoint, to b: CGPoint) {
        editGeometry { g, frame in
            let dx = Double(b.x - a.x) * Double(frame.width), dy = Double(b.y - a.y) * Double(frame.height)
            guard dx * dx + dy * dy > 4 else { return }
            let angle = atan2(dy, dx) * 180 / .pi
            g.setStraighten(g.straighten - (angle - 90 * (angle / 90).rounded()))
        }
    }

    /// Back to the frame as shot.
    func resetGeometry() { history.named("Reset Crop") { recipe.geometry = .identity } }

    /// Points given as fractions of the print on screen, in the frame's own
    /// normalised coordinates, as masks and gradients store them: so a new one
    /// starts on what the crop keeps, the way up the print stands.
    func framePoints(_ points: [CGPoint]) -> [CGPoint] {
        guard let map = displayMap, !map.isIdentity else { return points }
        return points.map {
            map.normalisedSource(fromOutput: CGPoint(x: $0.x * CGFloat(map.outputWidth),
                                                     y: $0.y * CGFloat(map.outputHeight)))
        }
    }

    // MARK: - Undo

    /// Undo and redo for the image on screen (`EditHistory`): one step per
    /// gesture, each image with its own history for the session.
    lazy var history = EditHistory<ImageEdits>(
        read: { [unowned self] in self.currentEdits },
        write: { [unowned self] in self.restore($0) },
        describe: { [unowned self] a, b in
            var names = EditNames.recipe(a.recipe, b.recipe, vocabulary: self.vocabulary)
            if a.whiteBalance != b.whiteBalance || a.customTemperature != b.customTemperature
                || a.customTint != b.customTint {
                names.append("White Balance")
            }
            return EditNames.title(names)
        })

    /// The edits have just changed; `undo` turns the edits as they are now
    /// into what they were.
    private func edited(_ undo: (inout ImageEdits) -> Void) {
        history.note {
            var before = currentEdits
            undo(&before)
            return before
        }
    }

    /// The names the inspector's controls go by (Output > Control names).
    private var vocabulary: Vocabulary {
        defaults.string(forKey: "vocabulary").flatMap(Vocabulary.init(rawValue:)) ?? .photographic
    }

    // MARK: - Recipe sidecars

    func saveRecipe(to url: URL) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(recipe).write(to: url)
    }

    func loadRecipe(from url: URL) throws {
        recipe = try JSONDecoder().decode(Recipe.self, from: Data(contentsOf: url))
    }

    // MARK: - Resets

    /// Everything on the Colour page: filtration, regional casts, pre-flash.
    func resetColour() { history.named("Reset Colour") { resettingColour() } }

    private func resettingColour() {
        var r = recipe
        let defaults = Recipe()
        r.filterShiftY = defaults.filterShiftY
        r.filterShiftM = defaults.filterShiftM
        r.shadowCMY = .zero
        r.highlightCMY = .zero
        r.preflashExposure = defaults.preflashExposure
        r.preflashY = defaults.preflashY
        r.preflashM = defaults.preflashM
        recipe = r
    }

    /// Couplers, halation, grain, glare, camera sharpness and development
    /// gamma — back to the stock's own measured defaults.
    func resetFilmCharacter() { history.named("Reset Film Character") { resettingFilmCharacter() } }

    private func resettingFilmCharacter() {
        var r = recipe
        let defaults = Recipe()
        r.couplersEnabled = defaults.couplersEnabled
        r.couplersAmount = defaults.couplersAmount
        r.couplersDiffusion = defaults.couplersDiffusion
        r.halationEnabled = defaults.halationEnabled
        r.halationAmount = defaults.halationAmount
        r.halationScale = defaults.halationScale
        r.scatterAmount = defaults.scatterAmount
        r.scatterScale = defaults.scatterScale
        r.cameraSharpness = defaults.cameraSharpness
        r.grainEnabled = defaults.grainEnabled
        r.grainAmount = defaults.grainAmount
        r.grainScale = defaults.grainScale
        r.glareEnabled = defaults.glareEnabled
        r.glareAmount = defaults.glareAmount
        r.densityCurveGamma = defaults.densityCurveGamma
        recipe = r
    }

    /// The Negative Exposure section: exposure, its meter, and white balance.
    func resetExposure() { history.named("Reset Exposure") { resettingExposure() } }

    private func resettingExposure() {
        var r = recipe
        let defaults = Recipe()
        r.exposureEV = defaults.exposureEV
        r.autoExposure = defaults.autoExposure
        recipe = r
        whiteBalance = .filmReference
        customTemperature = 5500
        customTint = 0
    }

    func resetTone() { history.named("Reset Tone") { resettingTone() } }

    private func resettingTone() {
        var r = recipe
        let defaults = Recipe()
        r.autoDensity = defaults.autoDensity
        r.autoGrade = defaults.autoGrade
        r.brightness = defaults.brightness
        r.density = defaults.density
        r.grade = r.defaultGrade
        r.shadowGrade = defaults.shadowGrade
        r.highlightGrade = defaults.highlightGrade
        r.shadows = defaults.shadows
        r.highlights = defaults.highlights
        r.toe = defaults.toe
        r.shoulder = defaults.shoulder
        r.toeWidth = defaults.toeWidth
        r.shoulderWidth = defaults.shoulderWidth
        r.snap = defaults.snap
        r.gradeTrim = .zero
        r.toeTrim = .zero
        r.shoulderTrim = .zero
        r.snapTrim = .zero
        r.shadowCMY = .zero
        r.highlightCMY = .zero
        r.curves = nil
        recipe = r
    }
}

/// What each frame of the image on screen brings: the print, where it comes
/// from in the frame, its histogram, whether one is being made and how long
/// the last took, and the loupe's part. The canvas, the status line, the
/// histogram and the loupe watch it; the inspector's controls do not.
@MainActor
final class PrintPreview: ObservableObject {
    @Published fileprivate(set) var image: CGImage?
    @Published fileprivate(set) var map: GeometryMap?
    @Published fileprivate(set) var histogram: PrintHistogram?
    @Published fileprivate(set) var isRendering = false
    @Published fileprivate(set) var milliseconds: Double = 0
    @Published fileprivate(set) var loupeImage: CGImage?

    fileprivate func show(_ output: RenderEngine.Output) {
        image = output.image
        map = output.map
        histogram = output.histogram
        milliseconds = output.milliseconds
    }
}

/// Stops a batch between two pictures: set on the main thread, read by the
/// batch as it goes.
final class BatchCancel: @unchecked Sendable {
    private let lock = NSLock()
    private var cancelled = false
    var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return cancelled }
    func cancel() { lock.lock(); cancelled = true; lock.unlock() }
}
