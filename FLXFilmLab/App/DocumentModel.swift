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

    /// Longest edge of the interactive proxy. Big enough to judge tone and
    /// colour, small enough that the whole graph stays under a frame.
    var proxyMaxDimension = 2048 { didSet { syncSource() } }

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

    init() {
        let made = Result { try RenderEngine() }
        engine = try? made.get()
        if case .failure(let error) = made { errorMessage = error.localizedDescription }
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
        let fresh = recipe.carryingLook(autoExposure: prefersAuto(\.autoExposure),
                                        autoBrightness: prefersAuto(\.autoDensity),
                                        autoContrast: prefersAuto(\.autoGrade))
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
            let sourceID = await engine.setSource(decoded)
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

    // MARK: - Auto preferences

    /// Whether each auto is wanted on newly opened images: whatever its toggle
    /// was last set to by hand. Taking over from a meter by moving its slider
    /// is a decision about one frame and does not change this.
    private static let autoPreferenceKeys: [WritableKeyPath<Recipe, Bool>: String] = [
        \Recipe.autoExposure: "prefersAutoExposure",
        \Recipe.autoDensity: "prefersAutoBrightness",
        \Recipe.autoGrade: "prefersAutoContrast",
    ]

    func rememberAutoPreference(_ auto: WritableKeyPath<Recipe, Bool>, isOn: Bool) {
        guard let key = Self.autoPreferenceKeys[auto] else { return }
        UserDefaults.standard.set(isOn, forKey: key)
    }

    private func prefersAuto(_ auto: WritableKeyPath<Recipe, Bool>) -> Bool {
        guard let key = Self.autoPreferenceKeys[auto],
              let stored = UserDefaults.standard.object(forKey: key) as? Bool else {
            return Recipe()[keyPath: auto]
        }
        return stored
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
        Task { [weak self] in
            let result: Result<RenderEngine.Output?, Error>
            do { result = .success(try await engine.render(recipe, for: sourceID)) }
            catch { result = .failure(error) }
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
                let setup = try RenderSetupBuilder.build(recipe: resolved,
                                                         exposureEV: Float(resolved.exposureEV))
                let masks = resolved.masks.filter { $0.enabled }
                if !masks.isEmpty {
                    renderer.setDodgeBurnMap(
                        MaskRasterizer.rasterize(masks: masks, width: full.width, height: full.height))
                }
                _ = try renderer.render(setup: setup, recipe: resolved)
                guard let linear = renderer.readLinear() else { throw Exporter.ExportError.renderFailed }
                try Exporter.write(linear: linear, width: full.width, height: full.height,
                                   to: url, format: format, colorSpace: resolved.outputColorSpace,
                                   metadata: RawLoader.metadata(of: source))
            }.value
            status = "Exported \(url.lastPathComponent)"
        } catch {
            errorMessage = error.localizedDescription
            status = statusLine
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

    /// Couplers, halation, grain, glare and development gamma — back to the
    /// stock's own measured defaults.
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
        r.grade = defaults.grade
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
