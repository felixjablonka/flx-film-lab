import Foundation

/// Loads the baked agx-emulsion data out of the app bundle.
/// Packs are small and there are only ever two live at a time, so they are
/// decoded on demand and cached.
final class PackLibrary {
    static let shared = PackLibrary()

    let index: PackIndex
    let shared_: SharedData
    private let packsURL: URL
    private var cache: [String: ProfilePack] = [:]
    private var tcLutCache: [String: [Float]] = [:]
    /// The preview renders on its own actor while the inspector and exports
    /// read packs from other threads, so the caches are locked.
    private let lock = NSLock()

    enum LoadError: Error, LocalizedError {
        case resourcesMissing(String)
        var errorDescription: String? {
            switch self {
            case .resourcesMissing(let what):
                return "FLX Film Lab data pack is missing or unreadable: \(what)"
            }
        }
    }

    private init() {
        // Bundled at Contents/Resources/packs; the dev build also finds a
        // sibling source tree so the app can run without a full rebuild.
        let candidates: [URL] = [
            Bundle.main.resourceURL?.appendingPathComponent("packs"),
            URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
                .appendingPathComponent("FLXFilmLab/Resources/packs"),
        ].compactMap { $0 }

        guard let base = candidates.first(where: {
            FileManager.default.fileExists(atPath: $0.appendingPathComponent("index.json").path)
        }) else {
            fatalError("FLX Film Lab: no data packs found. Run tools/bake/bake_packs.py.")
        }
        packsURL = base

        let decoder = JSONDecoder()
        do {
            index = try decoder.decode(PackIndex.self,
                                       from: Data(contentsOf: base.appendingPathComponent("index.json")))
            shared_ = try decoder.decode(SharedData.self,
                                         from: Data(contentsOf: base.appendingPathComponent("shared.json")))
        } catch {
            fatalError("FLX Film Lab: could not read data packs at \(base.path): \(error)")
        }
    }

    func pack(_ stock: String) throws -> ProfilePack {
        lock.lock()
        defer { lock.unlock() }
        if let cached = cache[stock] { return cached }
        let url = packsURL.appendingPathComponent("\(stock).json")
        guard let data = try? Data(contentsOf: url) else {
            throw LoadError.resourcesMissing("\(stock).json")
        }
        let pack = try JSONDecoder().decode(ProfilePack.self, from: data)
        cache[stock] = pack
        return pack
    }

    /// The chromaticity LUT: agx's whole spectral upsampling plus the stock's
    /// 81-band sensitivity projection, collapsed to one (size, size, 3) grid.
    ///
    /// A black-and-white film shot through a camera filter has its own: the
    /// film's sensitivity times the filter's transmittance.
    func tcLut(for pack: ProfilePack, lensFilter: LensFilterRef? = nil) throws -> (values: [Float], size: Int) {
        guard let ref = pack.tcLut else { throw LoadError.resourcesMissing("tcLut for \(pack.stock)") }
        let file = lensFilter?.file ?? ref.file
        lock.lock()
        defer { lock.unlock() }
        if let cached = tcLutCache[file] { return (cached, ref.size) }
        let url = packsURL.appendingPathComponent(file)
        guard let data = try? Data(contentsOf: url) else {
            throw LoadError.resourcesMissing(file)
        }
        let count = ref.size * ref.size * ref.channels
        guard data.count == count * MemoryLayout<Float>.size else {
            throw LoadError.resourcesMissing("\(file) has \(data.count) bytes, expected \(count * 4)")
        }
        let values = data.withUnsafeBytes { Array($0.bindMemory(to: Float.self)) }
        tcLutCache[file] = values
        return (values, ref.size)
    }

    /// Papers and cine print films, in menu order.
    var printStocks: [PackIndexEntry] { index.printing }
    var filmStocks: [PackIndexEntry] { index.filming }

    func shaderSource() throws -> String {
        let candidates: [URL] = [
            Bundle.main.resourceURL?.appendingPathComponent("Shaders/flx.metal"),
            URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
                .appendingPathComponent("FLXFilmLab/Resources/Shaders/flx.metal"),
        ].compactMap { $0 }
        for url in candidates {
            if let source = try? String(contentsOf: url, encoding: .utf8) { return source }
        }
        throw LoadError.resourcesMissing("Shaders/flx.metal")
    }
}

extension Recipe {
    /// This recipe with another film loaded, the way both apps' stock pickers
    /// load one. Crossing between colour and black and white also moves the
    /// print onto the new film's own paper: a silver negative on RA-4 paper or
    /// a colour negative on Multigrade are real darkroom choices, but rarely
    /// the one meant by picking a film. A paper chosen for the kind of film
    /// already loaded stays.
    func loadingFilm(_ stock: String, library: PackLibrary = .shared) -> Recipe {
        var r = self
        r.filmStock = stock
        if let film = try? library.pack(stock), let previous = try? library.pack(filmStock),
           film.isMonochrome != previous.isMonochrome,
           let paper = try? library.pack(printStock), film.isMonochrome != paper.isMonochrome,
           let target = film.targetPrint, library.printStocks.contains(where: { $0.stock == target }) {
            r.printStock = target
        }
        return r
    }
}
