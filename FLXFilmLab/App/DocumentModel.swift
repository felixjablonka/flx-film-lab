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
@MainActor
final class DocumentModel: ObservableObject {

    /// Everything that belongs to one image.
    struct ImageEdits {
        var recipe: Recipe
        var whiteBalance: RawLoader.WhiteBalance
        var customTemperature: Double
        var customTint: Double
    }

    @Published var recipe = Recipe() {
        didSet {
            guard !applyingEdits else { return }
            // The film's reference illuminant sets the decode white balance, so
            // changing stock in that mode means re-decoding.
            if whiteBalance == .filmReference, oldValue.filmStock != recipe.filmStock {
                syncSource()
            }
            scheduleRender()
        }
    }
    @Published private(set) var displayImage: CGImage?
    /// Where the image on screen comes from in the frame: the print as cut,
    /// or, while cropping, the whole frame turned and levelled.
    @Published private(set) var displayMap: GeometryMap?
    @Published private(set) var isRendering = false
    @Published private(set) var status: String = "Open a RAW file to begin"
    @Published private(set) var errorMessage: String?
    /// The image on screen, the one being edited. It changes only once a newly
    /// opened file has decoded.
    @Published private(set) var fileURL: URL?
    /// A file that has been opened but not yet decoded.
    @Published private(set) var pendingURL: URL?
    @Published var whiteBalance: RawLoader.WhiteBalance = .filmReference {
        didSet { if !applyingEdits, oldValue != whiteBalance { syncSource() } }
    }
    @Published var customTemperature: Double = 5500 {
        didSet { if !applyingEdits, whiteBalance == .custom { syncSource() } }
    }
    @Published var customTint: Double = 0 {
        didSet { if !applyingEdits, whiteBalance == .custom { syncSource() } }
    }
    @Published var showOriginal = false
    @Published private(set) var lastRenderMilliseconds: Double = 0
    /// What the meters read, for the controls whose auto is on. The sliders
    /// show these while their auto is on.
    @Published private(set) var meteredExposureEV: Double?
    @Published private(set) var meteredBrightness: Double?
    @Published private(set) var meteredGrade: Double?
    /// The recipe as last rendered, with the meters' choices filled in — what
    /// the curve plot draws, so it shows the print actually being made.
    @Published private(set) var resolvedRecipe: Recipe?
    @Published private(set) var isDecoding = false

    /// Longest edge of the interactive proxy, one of `previewSizes`. At 2560,
    /// big enough to judge tone and colour, small enough that a print control
    /// re-renders it in 23 ms and an exposure control in 40 ms on an M4 Pro;
    /// at 4096, 57 and 98 ms. The choice is kept between launches.
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
    struct DecodeKey: Equatable {
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
        var camera: String
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
    /// Set while one image's edits are swapped in for another's, so the swap
    /// does not trigger re-decodes and renders of its own half-way through.
    private var applyingEdits = false
    private var renderInFlight = false
    private var renderAgain = false

    init(presetStore: PresetStore = PresetStore(), defaults: UserDefaults = .standard) {
        self.defaults = defaults
        let saved = defaults.integer(forKey: Self.previewSizeKey)
        proxyMaxDimension = Self.previewSizes.contains(saved) ? saved : Self.defaultPreviewSize
        let made = Result { try RenderEngine() }
        engine = try? made.get()
        if case .failure(let error) = made { errorMessage = error.localizedDescription }
        self.presetStore = presetStore
        presets = presetStore.load()
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

    func open(url: URL) {
        errorMessage = nil
        if url == fileURL {
            // Back to the image already on screen: forget a pending open.
            pendingURL = nil
        } else {
            pendingURL = url
            status = "Opening \(url.lastPathComponent)…"
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
                errorMessage = "Could not open \(key.url.lastPathComponent): \(error.localizedDescription)"
                status = statusLine
            } else {
                errorMessage = error.localizedDescription
                status = "Could not decode \(key.url.lastPathComponent)"
            }
        case .success(let decoded):
            guard let engine else { return }
            let sourceID = await engine.setSource(decoded, grainSeed: Renderer.grainSeed(for: key.url))
            adopt(decoded, key: key, sourceID: sourceID)
        }
    }

    /// The engine now holds `decoded`; bring the model in line with it.
    private func adopt(_ decoded: RawLoader.Decoded, key: DecodeKey, sourceID: Int) {
        if requested == key {
            requested = nil
            isDecoding = false
        }
        if key.url != fileURL {
            // Switching images: put the outgoing one's edits away and bring
            // the incoming one's out, all at once.
            if let outgoing = fileURL { editsByImage[outgoing] = currentEdits }
            let incoming = edits(for: key.url)
            applyingEdits = true
            recipe = incoming.recipe
            whiteBalance = incoming.whiteBalance
            customTemperature = incoming.customTemperature
            customTint = incoming.customTint
            isCropping = false
            applyingEdits = false
            fileURL = key.url
            if pendingURL == key.url { pendingURL = nil }
            // Readings from the previous image must not show on this one's
            // sliders while its first frame renders.
            meteredExposureEV = nil
            meteredBrightness = nil
            meteredGrade = nil
            resolvedRecipe = nil
        }
        let camera = [decoded.cameraMake, decoded.cameraModel].compactMap { $0 }.joined(separator: " ")
        loaded = Loaded(key: key, sourceID: sourceID, width: decoded.width, height: decoded.height,
                        camera: camera)
        status = statusLine
        // The incoming edits may want a different decode (their own white
        // balance, or a stock with another reference illuminant).
        syncSource()
        scheduleRender()
    }

    private var statusLine: String {
        guard let loaded else { return "Open a RAW file to begin" }
        return "\(loaded.width)x\(loaded.height)" + (loaded.camera.isEmpty ? "" : "  ·  \(loaded.camera)")
    }

    // MARK: - Stocks

    /// Change film the way the stock picker does: crossing between colour and
    /// black and white also moves the print onto the new film's own paper
    /// (`Recipe.loadingFilm`).
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

    @Published private(set) var copiedSettings: CopiedSettings?

    var canCopySettings: Bool { fileURL != nil && pendingURL == nil }
    var canPasteSettings: Bool { copiedSettings != nil && canCopySettings }

    func copySettings() {
        guard canCopySettings, let name = fileURL?.lastPathComponent else { return }
        copiedSettings = CopiedSettings(edits: currentEdits, source: name)
    }

    /// Paste the copied settings onto the image on screen.
    func pasteSettings() {
        guard canPasteSettings, let copied = copiedSettings?.edits else { return }
        put(copied)
    }

    /// Put copied or saved settings on the image on screen: film, paper,
    /// print or light table, camera filter, every slider and auto, and white
    /// balance. Masks, gradients and the crop belong to the frame they were
    /// made for, so this image keeps its own.
    private func put(_ edits: ImageEdits) {
        var pasted = edits.recipe
        pasted.masks = recipe.masks
        pasted.gradients = recipe.gradients
        pasted.geometry = recipe.geometry
        applyingEdits = true
        recipe = pasted
        whiteBalance = edits.whiteBalance
        customTemperature = edits.customTemperature
        customTint = edits.customTint
        applyingEdits = false
        // White balance is decoded in, and a film-reference balance follows
        // the stock: decode again if either moved.
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
        put(ImageEdits(recipe: preset.recipe, whiteBalance: preset.whiteBalance,
                       customTemperature: preset.customTemperature, customTint: preset.customTint))
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
        if isDecoding { return "decoding…" }
        switch whiteBalance {
        case .filmReference:
            let name = filmPack?.referenceIlluminant ?? "D55"
            return String(format: "%@ · %.0f K", name, filmReferenceTemperature)
        case .asShot: return "camera as-shot"
        case .daylight: return "6504 K"
        case .tungsten: return "2850 K"
        case .custom: return String(format: "%.0f K · tint %+.0f", customTemperature, customTint)
        }
    }

    /// The illuminant this stock is balanced for, as a colour temperature.
    var filmReferenceTemperature: Double {
        RawLoader.referenceTemperature(filmPack?.referenceIlluminant ?? "D55")
    }

    /// True when the loaded stock is being read as a transparency rather than
    /// printed, which changes which controls are meaningful.
    var isTransparency: Bool {
        recipe.scanFilm ?? (filmPack?.isPositive ?? false)
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
        isRendering = true
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
        isRendering = false
        switch result {
        case .success(let output):
            // A frame for an image that has since been replaced is dropped;
            // the follow-up render below draws the new one.
            if let output, sourceID == loaded?.sourceID {
                displayImage = output.image
                displayMap = output.map
                resolvedRecipe = output.resolved
                meteredExposureEV = output.metered.exposureEV
                meteredBrightness = output.metered.brightness
                meteredGrade = output.metered.grade
                lastRenderMilliseconds = output.milliseconds
            }
        case .failure(let error):
            errorMessage = error.localizedDescription
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
                let full = try RawLoader.load(url: source, options: options)
                let renderer = try Renderer()
                renderer.setImage(full.pixels, width: full.width, height: full.height)
                renderer.grainSeed = Renderer.grainSeed(for: source)
                let setup = try RenderSetupBuilder.build(recipe: resolved,
                                                         exposureEV: Float(resolved.exposureEV))
                let masks = resolved.masks.filter { $0.enabled }
                if !masks.isEmpty {
                    renderer.setDodgeBurnMap(
                        MaskRasterizer.rasterize(masks: masks, width: full.width, height: full.height))
                }
                _ = try renderer.render(setup: setup, recipe: resolved)
                guard let linear = renderer.readLinear() else { throw Exporter.ExportError.renderFailed }
                try Exporter.write(linear: linear, width: Int(renderer.outputSize.width),
                                   height: Int(renderer.outputSize.height),
                                   to: url, format: format, colorSpace: resolved.outputColorSpace,
                                   metadata: RawLoader.metadata(of: source))
            }.value
            status = "Exported \(url.lastPathComponent)"
        } catch {
            errorMessage = error.localizedDescription
            status = statusLine
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
            if !isCropping { isLevelling = false }
            if !applyingEdits { scheduleRender() }
        }
    }
    /// The shape the print had when cropping began: what the meters read
    /// meanwhile, and what Cancel goes back to.
    private var shapeBeforeCropping: FrameGeometry?

    /// The level tool: the next line drawn on the frame levels it.
    @Published var isLevelling = false {
        didSet { if isLevelling { isCropping = true } }
    }

    /// Finish cropping, keeping the crop, or going back to the one before.
    func endCropping(keep: Bool) {
        if !keep, let before = shapeBeforeCropping { recipe.geometry = before }
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
    func resetGeometry() { recipe.geometry = .identity }

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
    func resetColour() {
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
    func resetFilmCharacter() {
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
    func resetExposure() {
        var r = recipe
        let defaults = Recipe()
        r.exposureEV = defaults.exposureEV
        r.autoExposure = defaults.autoExposure
        recipe = r
        whiteBalance = .filmReference
        customTemperature = 5500
        customTint = 0
    }

    func resetTone() {
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
        recipe = r
    }
}
