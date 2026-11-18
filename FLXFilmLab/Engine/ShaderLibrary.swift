import Foundation
import Metal

/// The app's shaders as Metal libraries: compiled ahead into `<name>.metallib`
/// (build.sh, where the Metal compiler is installed), else compiled here from
/// their source, about half a second the first time, which macOS then caches.
///
/// The library is looked for in the app's bundle, then in the build folder of
/// the repository the tools and the tests run in, where one older than its
/// source is passed over. `FLX_COMPILE_SHADERS=1` compiles the source
/// whatever there is, to compare the two.
enum ShaderLibrary {
    /// The libraries this process loaded compiled ahead, by name: for the checks.
    nonisolated(unsafe) private(set) static var loadedAhead: Set<String> = []

    /// `name`'s library: compiled ahead if there is one, else `source()`
    /// compiled with `options`. `sourcePath` is the source's path in the
    /// repository, which a library in its build folder must be newer than.
    static func make(_ name: String, device: MTLDevice, sourcePath: String, options: MTLCompileOptions?,
                     source: () throws -> String) throws -> MTLLibrary {
        if ProcessInfo.processInfo.environment["FLX_COMPILE_SHADERS"] != "1" {
            let fm = FileManager.default
            let repository = URL(fileURLWithPath: fm.currentDirectoryPath)
            let inRepository = repository.appendingPathComponent(sourcePath)
            let candidates = [Bundle.main.resourceURL?.appendingPathComponent("Shaders/\(name).metallib"),
                              repository.appendingPathComponent("build/shaders/\(name).metallib")]
            for case let url? in candidates where fm.fileExists(atPath: url.path) {
                if fm.fileExists(atPath: inRepository.path), modified(url) < modified(inRepository),
                   url.standardizedFileURL.path.hasPrefix(repository.standardizedFileURL.path + "/") {
                    continue    // older than the source it was compiled from
                }
                if let library = try? device.makeLibrary(URL: url) {
                    loadedAhead.insert(name)
                    return library
                }
            }
        }
        return try device.makeLibrary(source: try source(), options: options)
    }

    private static func modified(_ url: URL) -> Date {
        (try? url.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
    }
}
