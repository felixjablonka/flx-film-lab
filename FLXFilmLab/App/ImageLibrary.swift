import Foundation
import CryptoKit

/// Each image's own settings and flag, kept between launches.
///
/// They live in the app's library in Application Support, one file per folder,
/// so nothing is written into the folders the pictures are in. With Save
/// Settings Beside Images on, each image's record is also written to a small
/// file beside it, `DSC_0871.NEF.flx`, which travels with the folder: to
/// another drive, another Mac, or the Windows app. Records carry the time they
/// were made, and of a library record and a sidecar the newer counts. A
/// sidecar is read whenever there is one, whatever the setting.
///
/// A folder that has moved, or been renamed, is found again by its files'
/// names and sizes: a library file whose files match at least half of the
/// folder's is taken up for it, and left in place for the folder it was.
@MainActor
final class ImageLibrary {
    /// Culling: a pick, or a reject, which batch actions leave out.
    enum Flag: String, Codable, CaseIterable {
        case pick, reject
    }

    /// What one picture or clip keeps: a picture its settings, a clip its
    /// look. A record that has been cleared stays, empty, so that an older
    /// sidecar cannot bring back what it cleared.
    struct Record: Codable, Equatable {
        var modified: Date
        var flag: Flag?
        var edits: DocumentModel.ImageEdits?
        /// A clip's grade in the Motion mode, without how files are written.
        var look: MotionLook?
        /// What wrote it: "FLX Film Lab 1.6.0".
        var app: String?
    }

    /// One folder's records, by file name, and its files' sizes, by which the
    /// folder is recognised after a move.
    struct Folder: Codable {
        var path: String
        var records: [String: Record] = [:]
        var sizes: [String: Int64] = [:]
    }

    nonisolated static let sidecarExtension = "flx"
    /// The Save Settings Beside Images preference, in `defaults`.
    nonisolated static let sidecarsKey = "saveSidecars"

    nonisolated static var standardDirectory: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("FLX Film Lab", isDirectory: true)
            .appendingPathComponent("Library", isDirectory: true)
    }

    let directory: URL
    private let defaults: UserDefaults
    /// The folders read so far, by path.
    private var folders: [String: Folder] = [:]
    private var unsaved: Set<String> = []
    private var saveTask: Task<Void, Never>?
    /// Files are written here, in order, off the main thread.
    private let writer = DispatchQueue(label: "lab.flx.FLXFilmLab.library", qos: .utility)
    /// How long changes wait before they are written, so a slider dragged
    /// writes once it has settled.
    var saveDelay: Duration = .seconds(1)

    init(directory: URL = ImageLibrary.standardDirectory, defaults: UserDefaults = .standard) {
        self.directory = directory
        self.defaults = defaults
    }

    /// Save Settings Beside Images. Turned on, it writes the sidecars of every
    /// record of the folders read so far.
    var savesSidecars: Bool {
        get { defaults.bool(forKey: Self.sidecarsKey) }
        set {
            defaults.set(newValue, forKey: Self.sidecarsKey)
            guard newValue else { return }
            for folder in folders.values {
                for (name, record) in folder.records {
                    writeSidecar(record, for: URL(fileURLWithPath: folder.path).appendingPathComponent(name))
                }
            }
        }
    }

    nonisolated static func sidecar(for url: URL) -> URL { url.appendingPathExtension(sidecarExtension) }

    // MARK: Reading

    /// The records of `folder`'s files, `files` with their sizes, taking in
    /// the sidecars beside them where they are newer.
    func load(_ folder: URL, files: [(url: URL, size: Int64)]) -> [URL: Record] {
        let path = Self.path(folder)
        var entry = folders[path] ?? readFolderFile(path) ?? relocated(path, files: files) ?? Folder(path: path)
        for file in files {
            let name = file.url.lastPathComponent
            if entry.sizes[name] != file.size {
                entry.sizes[name] = file.size
                unsaved.insert(path)
            }
            guard let sidecar = Self.readSidecar(Self.sidecar(for: file.url)) else { continue }
            if sidecar.modified > (entry.records[name]?.modified ?? .distantPast) {
                entry.records[name] = sidecar
                unsaved.insert(path)
            }
        }
        folders[path] = entry
        if unsaved.contains(path) { scheduleSave() }
        var out: [URL: Record] = [:]
        for file in files {
            if let record = entry.records[file.url.lastPathComponent] { out[file.url] = record }
        }
        return out
    }

    /// The record of one image, from its folder as read.
    func record(for url: URL) -> Record? {
        let path = Self.path(url.deletingLastPathComponent())
        if folders[path] == nil, let read = readFolderFile(path) { folders[path] = read }
        return folders[path]?.records[url.lastPathComponent]
    }

    // MARK: Writing

    /// Change one image's record. It is written to the library once changes
    /// settle, and to its sidecar at once when sidecars are on.
    func update(_ url: URL, _ change: (inout Record) -> Void) {
        let path = Self.path(url.deletingLastPathComponent())
        var entry = folders[path] ?? readFolderFile(path) ?? Folder(path: path)
        let name = url.lastPathComponent
        var record = entry.records[name] ?? Record(modified: Date())
        let before = record
        change(&record)
        guard record.flag != before.flag || record.edits != before.edits || record.look != before.look
                || entry.records[name] == nil else { return }
        record.modified = Date()
        record.app = Exporter.Provenance.software
        entry.records[name] = record
        if entry.sizes[name] == nil {
            entry.sizes[name] = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize).map(Int64.init) ?? 0
        }
        folders[path] = entry
        unsaved.insert(path)
        if savesSidecars { writeSidecar(record, for: url) }
        scheduleSave()
    }

    private func scheduleSave() {
        saveTask?.cancel()
        let delay = saveDelay
        saveTask = Task { [weak self] in
            try? await Task.sleep(for: delay)
            guard !Task.isCancelled else { return }
            self?.save()
        }
    }

    /// Write every folder changed since the last save; `wait` returns once
    /// they are on disk (quitting, the tests).
    func save(wait: Bool = false) {
        saveTask?.cancel()
        saveTask = nil
        let directory = self.directory
        // A loop, not compactMap with a map inside it: Swift 6.3's compiler
        // (Xcode 26.6, GitHub's) never finished optimising those closures.
        var files: [(URL, Folder)] = []
        for path in unsaved {
            if let folder = folders[path] { files.append((Self.folderFile(path, in: directory), folder)) }
        }
        unsaved.removeAll()
        let write = {
            try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let encoder = JSONEncoder()
            encoder.dateEncodingStrategy = .iso8601
            encoder.outputFormatting = [.sortedKeys]
            for (url, folder) in files {
                guard let data = try? encoder.encode(folder) else { continue }
                try? data.write(to: url, options: .atomic)
            }
        }
        if wait { writer.sync(execute: write) } else { writer.async(execute: write) }
    }

    /// Wait for every sidecar and library file sent off to be written.
    func finishWriting() {
        writer.sync {}
    }

    private func writeSidecar(_ record: Record, for url: URL) {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        guard let data = try? encoder.encode(record) else { return }
        let file = Self.sidecar(for: url)
        // A folder that cannot be written, a card or a share read-only, keeps
        // the record in the library alone.
        writer.async { try? data.write(to: file, options: .atomic) }
    }

    // MARK: Files

    nonisolated private static func path(_ folder: URL) -> String {
        folder.standardizedFileURL.resolvingSymlinksInPath().path
    }

    /// The library file of the folder at `path`: a name from its path.
    nonisolated private static func folderFile(_ path: String, in directory: URL) -> URL {
        let digest = SHA256.hash(data: Data(path.utf8)).prefix(16).map { String(format: "%02x", $0) }.joined()
        return directory.appendingPathComponent(digest).appendingPathExtension("json")
    }

    nonisolated private static func decoder() -> JSONDecoder {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return decoder
    }

    private func readFolderFile(_ path: String) -> Folder? {
        guard let data = try? Data(contentsOf: Self.folderFile(path, in: directory)),
              let folder = try? Self.decoder().decode(Folder.self, from: data), folder.path == path else { return nil }
        return folder
    }

    nonisolated static func readSidecar(_ url: URL) -> Record? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        return try? decoder().decode(Record.self, from: data)
    }

    /// A folder the library has no file for may be one it knew elsewhere: the
    /// library file whose files' names and sizes match the most of this
    /// folder's, at least half of them, lends it the records of the files
    /// that match.
    private func relocated(_ path: String, files: [(url: URL, size: Int64)]) -> Folder? {
        guard !files.isEmpty,
              let urls = try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)
        else { return nil }
        var sizes: [String: Int64] = [:]
        for file in files { sizes[file.url.lastPathComponent] = file.size }
        var best: (folder: Folder, matches: [String])?
        for url in urls where url.pathExtension == "json" {
            guard let data = try? Data(contentsOf: url),
                  let folder = try? Self.decoder().decode(Folder.self, from: data), folder.path != path else { continue }
            let matches = sizes.compactMap { name, size in folder.sizes[name] == size ? name : nil }
            if matches.count * 2 >= files.count, matches.count > (best?.matches.count ?? 0) {
                best = (folder, matches)
            }
        }
        guard let best else { return nil }
        var found = Folder(path: path)
        for name in best.matches {
            found.records[name] = best.folder.records[name]
            found.sizes[name] = best.folder.sizes[name]
        }
        unsaved.insert(path)
        return found
    }
}
