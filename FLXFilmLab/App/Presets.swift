import Foundation

/// Settings saved by name, to put on any image later, in this session or
/// another: what Copy Settings copies — film, paper, print or light table,
/// camera filter, every slider and auto, and white balance. Masks, gradients
/// and the crop belong to the frame they were made for and are never in one.
struct Preset: Codable, Equatable, Identifiable {
    var name: String
    var recipe: Recipe
    var whiteBalance: RawLoader.WhiteBalance
    var customTemperature: Double
    var customTint: Double

    var id: String { name }

    init(name: String, recipe: Recipe, whiteBalance: RawLoader.WhiteBalance,
         customTemperature: Double, customTint: Double) {
        self.name = name
        var look = recipe
        look.masks = []
        look.gradients = []
        look.frameGeometry = nil
        self.recipe = look
        self.whiteBalance = whiteBalance
        self.customTemperature = customTemperature
        self.customTint = customTint
    }
}

/// The presets on disk, one JSON file each, in Application Support. A preset
/// is shared by copying its file; one copied into the folder is read the next
/// time the app comes forward.
struct PresetStore {
    let directory: URL
    /// How a deleted preset's file goes: into the Trash, so a preset deleted
    /// by mistake can be got back.
    var discard: (URL) throws -> Void = { try FileManager.default.trashItem(at: $0, resultingItemURL: nil) }

    static var standardDirectory: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("FLX Film Lab", isDirectory: true)
            .appendingPathComponent("Presets", isDirectory: true)
    }

    init(directory: URL = PresetStore.standardDirectory) {
        self.directory = directory
    }

    /// Every preset that reads, by name; a file that does not is left alone.
    /// Two files of the same name give one preset.
    func load() -> [Preset] {
        var seen = Set<String>()
        return files().compactMap { $0.preset }
            .sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
            .filter { seen.insert($0.name).inserted }
    }

    /// Write `preset`, over the file of a preset of the same name if there is one.
    func save(_ preset: Preset) throws {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let url = files().first { $0.preset?.name == preset.name }?.url ?? fileURL(for: preset.name)
        try encoder.encode(preset).write(to: url, options: .atomic)
    }

    func delete(_ preset: Preset) throws {
        for file in files() where file.preset?.name == preset.name { try discard(file.url) }
    }

    /// The folder, made if it is not there yet, to show in Finder.
    func ensuredDirectory() throws -> URL {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }

    private func files() -> [(url: URL, preset: Preset?)] {
        let urls = (try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil,
                                                                 options: .skipsHiddenFiles)) ?? []
        return urls.filter { $0.pathExtension.lowercased() == "json" }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
            .map { url in (url, (try? Data(contentsOf: url)).flatMap { try? JSONDecoder().decode(Preset.self, from: $0) }) }
    }

    /// The file a new preset is written to, named after it as far as a file
    /// name allows.
    private func fileURL(for name: String) -> URL {
        var file = name.replacingOccurrences(of: "/", with: "-").replacingOccurrences(of: ":", with: "-")
        if file.hasPrefix(".") { file = "_" + file.dropFirst() }
        return directory.appendingPathComponent(file).appendingPathExtension("json")
    }
}
