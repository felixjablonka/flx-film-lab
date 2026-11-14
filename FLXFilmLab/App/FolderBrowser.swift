import Foundation
import os
import AVFoundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers
import CryptoKit

/// The folder in the sidebar: its pictures, which of them are selected, and
/// how each is flagged. The image on screen is the document's own
/// (`DocumentModel.fileURL`); the selection is what batch actions act on.
///
/// Its own object, which only the sidebar watches: a slider dragged changes
/// the document every tick, and the sidebar must not be laid out again for
/// that (1.4.0's lesson, README *Performance*).
@MainActor
final class FolderBrowser: ObservableObject {
    /// What a folder is read for: the Stills mode's pictures, or the Motion
    /// mode's clips.
    enum Kind {
        case pictures, clips
        var noun: String { self == .pictures ? "picture" : "clip" }
    }

    let kind: Kind

    init(kind: Kind = .pictures) {
        self.kind = kind
        thumbnails = ThumbnailStore(kind: kind)
    }

    struct Item: Identifiable, Hashable {
        let url: URL
        let size: Int64
        var id: URL { url }
        var name: String { url.lastPathComponent }
    }

    /// Which pictures the sidebar lists.
    enum Filter: String, CaseIterable, Identifiable {
        case all = "All"
        case picks = "Picks"
        case withoutRejects = "No Rejects"
        var id: String { rawValue }
    }

    @Published private(set) var folder: URL?
    @Published private(set) var items: [Item] = []
    @Published var selection: Set<URL> = []
    @Published private(set) var flags: [URL: ImageLibrary.Flag] = [:]
    /// The pictures with settings of their own; the others open with the look
    /// on screen.
    @Published private(set) var edited: Set<URL> = []
    @Published var filter: Filter = .all
    /// The image on screen, for the sidebar to mark.
    @Published var current: URL?
    /// Whether there are settings copied, for Paste Settings to Selected.
    @Published var hasCopiedSettings = false
    /// The batch export under way, if one is.
    @Published var batch: BatchProgress?
    let thumbnails: ThumbnailStore
    /// Where the flags and settings are kept, and the Save Settings Beside
    /// Images preference.
    weak var library: ImageLibrary? {
        didSet { savesSidecars = library?.savesSidecars ?? false }
    }
    /// A picture's own settings, if it has any (`DocumentModel.ownEdits`).
    var ownEdits: (URL) -> DocumentModel.ImageEdits? = { _ in nil }
    /// A clip's own grade, if it has one (`ClipModel.ownGrade`).
    var ownGrade: (URL) -> MotionLook? = { _ in nil }
    /// What Paste does to the selection: Paste Settings, or for clips Paste
    /// Look, if the mode has it.
    var pasteName = "Paste Settings"

    /// Save Settings Beside Images: each picture's settings and flag also in
    /// a file beside it (`ImageLibrary`).
    @Published var savesSidecars = false {
        didSet {
            if library?.savesSidecars != savesSidecars { library?.savesSidecars = savesSidecars }
        }
    }

    struct BatchProgress: Equatable {
        var done: Int
        var total: Int
        var current: String
        var folder: URL
    }

    /// A row has come into view: its picture, the print if it has settings
    /// of its own.
    func requestThumbnail(_ url: URL) {
        switch kind {
        case .pictures: thumbnails.request(url, edits: ownEdits(url))
        case .clips: thumbnails.requestClip(url, grade: ownGrade(url))
        }
    }

    /// The pictures the sidebar lists, in its order.
    var shown: [Item] {
        switch filter {
        case .all: return items
        case .picks: return items.filter { flags[$0.url] == .pick }
        case .withoutRejects: return items.filter { flags[$0.url] != .reject }
        }
    }

    /// The selection in the sidebar's order, rejects left out: what a batch
    /// action acts on.
    var selectedForBatch: [URL] {
        items.map(\.url).filter { selection.contains($0) && flags[$0] != .reject }
    }

    /// How many of the selection are rejects, which batch actions leave out.
    var selectedRejects: Int { selection.filter { flags[$0] == .reject }.count }

    /// The picture `offset` places from `url` among those listed.
    func neighbour(of url: URL, offset: Int) -> URL? {
        let list = shown
        guard let i = list.firstIndex(where: { $0.url == url }) else { return list.first?.url }
        let j = i + offset
        return list.indices.contains(j) ? list[j].url : nil
    }

    func show(_ folder: URL, items: [Item], records: [URL: ImageLibrary.Record]) {
        self.folder = folder
        self.items = items
        flags = records.compactMapValues(\.flag)
        // A picture's settings of its own, a clip's look.
        edited = Set(records.filter { kind == .pictures ? $0.value.edits != nil : $0.value.look != nil }.keys)
        selection = selection.filter { url in items.contains { $0.url == url } }
        thumbnails.forgetAll()
    }

    func setFlag(_ flag: ImageLibrary.Flag?, for url: URL) { flags[url] = flag }

    func markEdited(_ url: URL) {
        if !edited.contains(url) { edited.insert(url) }
    }

    /// One spelling for each file: absolute, with no base URL and no `.` or
    /// `..`, so the same picture reached from the sidebar, an Open panel,
    /// Finder or a drop compares equal. A listing's URLs are relative to the
    /// folder's, which `==` tells apart from the same file's absolute URL.
    nonisolated static func canonical(_ url: URL) -> URL {
        URL(fileURLWithPath: url.absoluteURL.standardizedFileURL.path)
    }

    /// A folder's pictures, by name as Finder sorts them: anything an image
    /// type, RAW files included, but not a movie, a vector drawing or a hidden
    /// file. Sidecars (`.flx`) are not images. For `.clips`, its movies.
    nonisolated static func listing(_ folder: URL, kind: Kind = .pictures) -> [Item] {
        let keys: Set<URLResourceKey> = [.fileSizeKey, .contentTypeKey, .isRegularFileKey]
        let urls = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: Array(keys),
                                                                 options: [.skipsHiddenFiles])) ?? []
        return urls.compactMap { url -> Item? in
            guard let values = try? url.resourceValues(forKeys: keys), values.isRegularFile == true,
                  let type = values.contentType ?? UTType(filenameExtension: url.pathExtension) else { return nil }
            switch kind {
            case .pictures:
                guard type.conforms(to: .image), !type.conforms(to: .svg), !type.conforms(to: .movie) else { return nil }
            case .clips:
                guard type.conforms(to: .movie) else { return nil }
            }
            return Item(url: canonical(url), size: Int64(values.fileSize ?? 0))
        }
        .sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }
}

/// The sidebar's pictures, 256 px on the long side.
///
/// A picture with no settings of its own shows the camera's preview, read out
/// of the file (about 25 ms for a NEF). One with settings of its own shows its
/// print: from the preview on screen when it was last there, from the disk
/// cache, or made in the background, one at a time (about 0.45 s, nearly all
/// of it the RAW's decode). Only the rows on screen ask, a few files at a
/// time, and at most `limit` pictures are kept in memory.
@MainActor
final class ThumbnailStore: ObservableObject {
    nonisolated static let size = 256
    /// Pictures or clips: a clip's thumbnail is a frame of it, as QuickTime
    /// shows it, log footage and all.
    let kind: FolderBrowser.Kind

    init(kind: FolderBrowser.Kind = .pictures) {
        self.kind = kind
    }
    @Published private(set) var images: [URL: CGImage] = [:]
    /// The print each picture shows, by its key; none for a camera preview.
    private var shownPrint: [URL: String] = [:]
    private var reading: Set<URL> = []
    private var lastUsed: [URL: Int] = [:]
    private var clock = 0
    var limit = 400
    private let previews: OperationQueue = {
        let q = OperationQueue()
        q.maxConcurrentOperationCount = 3
        q.qualityOfService = .utility
        return q
    }()
    private let printer = PrintThumbnailMaker()
    private let clipMaker = ClipPrintMaker()
    private var printing: Set<String> = []

    /// Where the tests keep their prints instead.
    nonisolated(unsafe) static var cacheDirectoryOverride: URL?

    nonisolated static var cacheDirectory: URL {
        if let override = cacheDirectoryOverride { return override }
        return FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("lab.flx.FLXFilmLab", isDirectory: true)
            .appendingPathComponent("Thumbnails", isDirectory: true)
    }

    /// The key of a picture's print: the file as it is on disk, the settings
    /// it is printed with, and the app that prints it.
    nonisolated static func printKey(_ url: URL, edits: DocumentModel.ImageEdits) -> String {
        printKey(url, settings: json(edits))
    }

    /// A clip's graded frame, by its grade.
    nonisolated static func printKey(_ url: URL, grade: MotionLook) -> String {
        printKey(url, settings: json(grade))
    }

    nonisolated private static func json<T: Encodable>(_ value: T) -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = .sortedKeys
        return (try? encoder.encode(value)).map { String(decoding: $0, as: UTF8.self) } ?? ""
    }

    nonisolated private static func printKey(_ url: URL, settings: String) -> String {
        let values = try? url.resourceValues(forKeys: [.contentModificationDateKey, .fileSizeKey])
        let made = "\(Exporter.Provenance.software)|\(values?.contentModificationDate?.timeIntervalSince1970 ?? 0)|"
            + "\(values?.fileSize ?? 0)|\(settings)"
        return hash(url.path) + "/" + hash(made)
    }

    nonisolated private static func hash(_ text: String) -> String {
        SHA256.hash(data: Data(text.utf8)).prefix(16).map { String(format: "%02x", $0) }.joined()
    }

    /// Show `url`'s picture: its print, when it has settings of its own
    /// (`edits`), otherwise the camera's preview.
    func request(_ url: URL, edits: DocumentModel.ImageEdits?) {
        clock += 1
        lastUsed[url] = clock
        guard let edits else {
            if shownPrint[url] != nil { shownPrint[url] = nil; images[url] = nil }
            readPreview(url)
            return
        }
        let key = Self.printKey(url, edits: edits)
        if shownPrint[url] == key { return }
        if let cached = Self.loadCached(key) {
            put(cached, for: url, print: key)
            return
        }
        if images[url] == nil { readPreview(url) }
        makePrint(url, edits: edits, key: key)
    }

    /// The print on screen, shrunk, for the picture it is of: no render of
    /// its own. Kept in the disk cache too.
    func setPrint(_ image: CGImage, for url: URL, edits: DocumentModel.ImageEdits) {
        guard let small = Self.shrink(image) else { return }
        let key = Self.printKey(url, edits: edits)
        put(small, for: url, print: key)
        DispatchQueue.global(qos: .utility).async { Self.saveCached(small, key: key) }
    }

    /// Make the prints of `jobs` in the background, after their settings were
    /// changed elsewhere than on screen (Paste Settings to Selected).
    func refresh(_ jobs: [(url: URL, edits: DocumentModel.ImageEdits)]) {
        for job in jobs where lastUsed[job.url] != nil { request(job.url, edits: job.edits) }
    }

    /// Show a clip's frame: graded with its own look, if it has one, made in
    /// the background and cached as a picture's print is; the frame as
    /// QuickTime shows it meanwhile, and for a clip with no look of its own.
    func requestClip(_ url: URL, grade: MotionLook?) {
        clock += 1
        lastUsed[url] = clock
        guard let grade else {
            if shownPrint[url] != nil { shownPrint[url] = nil; images[url] = nil }
            readPreview(url)
            return
        }
        let key = Self.printKey(url, grade: grade)
        if shownPrint[url] == key { return }
        if let cached = Self.loadCached(key) {
            put(cached, for: url, print: key)
            return
        }
        if images[url] == nil { readPreview(url) }
        guard !printing.contains(key) else { return }
        printing.insert(key)
        let maker = clipMaker, round = clipMaker.round
        Task { [weak self] in
            let image = await maker.make(url, grade: grade, round: round)
            guard let self else { return }
            self.printing.remove(key)
            guard let image, let small = Self.shrink(image) else { return }
            DispatchQueue.global(qos: .utility).async { Self.saveCached(small, key: key) }
            self.put(small, for: url, print: key)
        }
    }

    /// The graded frames of clips whose look changed elsewhere than on screen
    /// (Paste Look to Selected), or on screen (the clip's look, once settled).
    func refreshClips(_ jobs: [(url: URL, grade: MotionLook)]) {
        for job in jobs where lastUsed[job.url] != nil { requestClip(job.url, grade: job.grade) }
    }

    func forgetAll() {
        previews.cancelAllOperations()
        printer.cancelAll()
        clipMaker.cancelAll()
        images = [:]
        shownPrint = [:]
        reading = []
        lastUsed = [:]
        printing = []
    }

    /// Short of memory: keep only the most recently shown.
    func trim(keeping count: Int = 60) {
        let keep = Set(lastUsed.sorted { $0.value > $1.value }.prefix(count).map(\.key))
        for url in images.keys where !keep.contains(url) {
            images[url] = nil
            shownPrint[url] = nil
            lastUsed[url] = nil
        }
    }

    private func put(_ image: CGImage, for url: URL, print key: String?) {
        images[url] = image
        shownPrint[url] = key
        if images.count > limit { trim(keeping: limit * 3 / 4) }
    }

    private func readPreview(_ url: URL) {
        guard images[url] == nil, !reading.contains(url) else { return }
        reading.insert(url)
        let kind = self.kind
        previews.addOperation { [weak self] in
            let thumb = kind == .clips ? Self.clipFrame(url) : Self.cameraPreview(url)
            Task { @MainActor in
                guard let self else { return }
                self.reading.remove(url)
                // A print that came in meanwhile stays.
                if let thumb, self.shownPrint[url] == nil { self.put(thumb, for: url, print: nil) }
            }
        }
    }

    private func makePrint(_ url: URL, edits: DocumentModel.ImageEdits, key: String) {
        guard !printing.contains(key) else { return }
        printing.insert(key)
        printer.make(url, edits: edits) { [weak self] image in
            Task { @MainActor in
                guard let self else { return }
                self.printing.remove(key)
                guard let image, let small = Self.shrink(image) else { return }
                DispatchQueue.global(qos: .utility).async { Self.saveCached(small, key: key) }
                self.put(small, for: url, print: key)
            }
        }
    }

    /// The camera's own preview of a file, from its embedded JPEG where it has
    /// one, upright.
    nonisolated static func cameraPreview(_ url: URL) -> CGImage? {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, [kCGImageSourceShouldCache: false] as CFDictionary)
        else { return nil }
        let options: [CFString: Any] = [kCGImageSourceCreateThumbnailFromImageIfAbsent: true,
                                        kCGImageSourceThumbnailMaxPixelSize: size,
                                        kCGImageSourceCreateThumbnailWithTransform: true,
                                        kCGImageSourceShouldCacheImmediately: true]
        return CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary)
    }

    /// A frame of a clip a tenth of the way in (at most a second), upright, as
    /// AVFoundation shows it: log footage looks flat, as in QuickTime.
    nonisolated static func clipFrame(_ url: URL) -> CGImage? {
        let asset = AVURLAsset(url: url)
        let generator = AVAssetImageGenerator(asset: asset)
        generator.appliesPreferredTrackTransform = true
        generator.maximumSize = CGSize(width: size, height: size)
        let done = DispatchSemaphore(value: 0)
        nonisolated(unsafe) var frame: CGImage?
        Task.detached {
            let duration = (try? await asset.load(.duration)).map(CMTimeGetSeconds) ?? 0
            let at = CMTime(seconds: min(1, max(0, duration / 10)), preferredTimescale: 600)
            frame = try? await generator.image(at: at).image
            done.signal()
        }
        done.wait()
        return frame
    }

    /// `image` at `size` px on its long side, in its own colour space.
    nonisolated static func shrink(_ image: CGImage, to size: Int = ThumbnailStore.size) -> CGImage? {
        let scale = min(1, Double(size) / Double(max(image.width, image.height)))
        let w = max(1, Int((Double(image.width) * scale).rounded())), h = max(1, Int((Double(image.height) * scale).rounded()))
        let space = image.colorSpace ?? CGColorSpace(name: CGColorSpace.sRGB)!
        guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue
                                      | CGBitmapInfo.byteOrder32Little.rawValue) else { return nil }
        ctx.interpolationQuality = .high
        ctx.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
        return ctx.makeImage()
    }

    // MARK: The disk cache: one print a picture, the newest

    nonisolated private static func cacheFile(_ key: String) -> URL {
        cacheDirectory.appendingPathComponent(key + ".jpg")
    }

    nonisolated static func loadCached(_ key: String) -> CGImage? {
        guard let source = CGImageSourceCreateWithURL(cacheFile(key) as CFURL, nil) else { return nil }
        return CGImageSourceCreateImageAtIndex(source, 0, nil)
    }

    nonisolated static func saveCached(_ image: CGImage, key: String) {
        let file = cacheFile(key)
        let folder = file.deletingLastPathComponent()
        let fm = FileManager.default
        // The print before goes: nothing asks for it unless its settings come back.
        if let old = try? fm.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil) {
            for f in old where f.lastPathComponent != file.lastPathComponent { try? fm.removeItem(at: f) }
        }
        try? fm.createDirectory(at: folder, withIntermediateDirectories: true)
        guard let dest = CGImageDestinationCreateWithURL(file as CFURL, UTType.jpeg.identifier as CFString, 1, nil)
        else { return }
        CGImageDestinationAddImage(dest, image, [kCGImageDestinationLossyCompressionQuality: 0.85] as CFDictionary)
        CGImageDestinationFinalize(dest)
    }
}

/// Makes the sidebar's prints off the main thread, one after another at
/// utility priority, on a renderer of its own, so the picture on screen
/// prints as fast as before. Each is metered on its own small decode, as the
/// picture would be on screen.
final class PrintThumbnailMaker: @unchecked Sendable {
    private let queue = DispatchQueue(label: "lab.flx.FLXFilmLab.thumbnail-prints", qos: .utility)
    private var renderer: Renderer?
    private let lock = NSLock()
    private var generation = 0

    func make(_ url: URL, edits: DocumentModel.ImageEdits, done: @escaping @Sendable (CGImage?) -> Void) {
        lock.lock(); let mine = generation; lock.unlock()
        queue.async {
            self.lock.lock(); let current = mine == self.generation; self.lock.unlock()
            guard current else { return done(nil) }
            done(autoreleasepool { self.render(url, edits: edits) })
        }
    }

    func cancelAll() {
        lock.lock(); generation += 1; lock.unlock()
    }

    private func render(_ url: URL, edits: DocumentModel.ImageEdits) -> CGImage? {
        do {
            if renderer == nil { renderer = try Renderer() }
            guard let renderer else { return nil }
            let film = try? PackLibrary.shared.pack(edits.recipe.filmStock)
            let options = RawLoader.Options(whiteBalance: edits.whiteBalance,
                                            filmReference: film?.referenceIlluminant ?? "D55",
                                            temperature: edits.customTemperature, tint: edits.customTint,
                                            maxDimension: 512)
            let decoded = try RawLoader.load(url: url, options: options)
            let resolved = try RenderEngine.resolved(edits.recipe, pixels: decoded.pixels,
                                                     width: decoded.width, height: decoded.height)
            // A new image clears the last one's dodge and burn.
            renderer.setImage(decoded.pixels, width: decoded.width, height: decoded.height)
            renderer.grainSeed = Renderer.grainSeed(for: url)
            let setup = try RenderSetupBuilder.build(recipe: resolved, exposureEV: Float(resolved.exposureEV))
            let masks = resolved.masks.filter(\.enabled)
            if !masks.isEmpty {
                renderer.setDodgeBurnMap(MaskRasterizer.rasterize(masks: masks, width: decoded.width, height: decoded.height))
            }
            _ = try renderer.render(setup: setup, recipe: resolved)
            return renderer.makeDisplayImage(colorSpaceName: resolved.outputColorSpace)
        } catch {
            return nil
        }
    }
}

/// Grades the sidebar's clip frames off the main thread, one at a time, in
/// the order asked: the frame the thumbnail shows, read as the preview reads
/// it, metered for the stock as Auto exposure meters the clip, its look baked
/// and the frame graded with it, effects and all.
actor ClipPrintMaker {
    private var pipeline: FramePipeline?
    private var baker: LookBakeEngine?
    /// The job asked for last; each waits for the one before it.
    private var last: Task<Void, Never>?
    private var waiting = 0
    private var release: Task<Void, Never>?
    nonisolated private let generation = OSAllocatedUnfairLock(initialState: 0)

    /// The jobs asked for now; `cancelAll` drops them, and those not started
    /// give nothing.
    nonisolated var round: Int { generation.withLock { $0 } }
    nonisolated func cancelAll() { generation.withLock { $0 += 1 } }
    nonisolated private func isCurrent(_ round: Int) -> Bool { generation.withLock { $0 == round } }

    func make(_ url: URL, grade: MotionLook, round: Int) async -> CGImage? {
        release?.cancel()
        waiting += 1
        let before = last
        let job = Task { () -> CGImage? in
            await before?.value
            guard self.isCurrent(round) else { return nil }
            return await self.render(url, grade: grade, round: round)
        }
        last = Task { _ = await job.value }
        let image = await job.value
        waiting -= 1
        if waiting == 0 { releaseWhenIdle() }
        return image
    }

    /// The pipeline and the baker hold some 60 MB: let them go once the
    /// sidebar has had its frames a while.
    private func releaseWhenIdle() {
        release = Task {
            try? await Task.sleep(for: .seconds(10))
            guard !Task.isCancelled else { return }
            pipeline = nil
            baker = nil
        }
    }

    private func render(_ url: URL, grade: MotionLook, round: Int) async -> CGImage? {
        do {
            let info = try await ClipInfo.load(url)
            var settings = grade.input
            settings.format = grade.input.format ?? info.detectedFormat
            var look = grade
            if look.recipe.autoExposure, let film = try? PackLibrary.shared.pack(look.recipe.filmStock),
               let luminance = try await ClipMeter.highlight(of: info, settings: settings) {
                look.recipe.exposureEV = Double(ClipMeter.exposure(highlight: luminance, film: film))
            }
            guard isCurrent(round), let frame = try await Self.frame(of: info) else { return nil }
            let baker = self.baker ?? LookBakeEngine()
            self.baker = baker
            let lut = try await baker.bake(look.recipe, input: (settings.format ?? .nlog).lutInput)
            if pipeline == nil { pipeline = try FramePipeline() }
            guard let pipeline, isCurrent(round) else { return nil }
            pipeline.setLUT(lut)
            let w = CVPixelBufferGetWidth(frame), h = CVPixelBufferGetHeight(frame)
            let scale = min(1, Double(ThumbnailStore.size) / Double(max(w, h)))
            return try pipeline.thumbnail(frame, width: max(1, Int((Double(w) * scale).rounded())),
                                          height: max(1, Int((Double(h) * scale).rounded())),
                                          effects: look.effectsAsPrinted, settings: settings,
                                          quarterTurns: info.quarterTurns)
        } catch {
            return nil
        }
    }

    /// The frame the thumbnail shows, a tenth of the way in (at most a
    /// second), decoded as the preview decodes it.
    private static func frame(of info: ClipInfo) async throws -> CVPixelBuffer? {
        let asset = AVURLAsset(url: info.url)
        guard let track = try await asset.loadTracks(withMediaType: .video).first else { return nil }
        let reader = try AVAssetReader(asset: asset)
        let at = min(1, max(0, info.durationSeconds / 10))
        reader.timeRange = CMTimeRange(start: CMTime(seconds: at, preferredTimescale: 600),
                                       duration: CMTime(seconds: 1, preferredTimescale: 600))
        let output = AVAssetReaderTrackOutput(track: track, outputSettings: info.decodeAttributes)
        output.alwaysCopiesSampleData = false
        reader.add(output)
        defer { reader.cancelReading() }
        guard reader.startReading(), let sample = output.copyNextSampleBuffer() else { return nil }
        return CMSampleBufferGetImageBuffer(sample)
    }
}
