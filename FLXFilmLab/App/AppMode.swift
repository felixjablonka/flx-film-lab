import Foundation
import UniformTypeIdentifiers

/// The app's two halves: stills (a RAW through film and paper) and motion (a
/// log clip through the same film, with playback, clip export and LUTs).
/// Each keeps its own document, so switching back finds the image or clip
/// as it was left.
enum AppMode: String, CaseIterable, Identifiable {
    case stills = "Stills"
    case motion = "Motion"

    var id: String { rawValue }

    var symbol: String {
        switch self {
        case .stills: return "camera"
        case .motion: return "video"
        }
    }

    /// Which half opens a file handed to the app (from Finder, the Dock or a
    /// drop): a movie is a clip, anything else an image.
    static func opening(_ url: URL) -> AppMode {
        let type = (try? url.resourceValues(forKeys: [.contentTypeKey]).contentType)
            ?? UTType(filenameExtension: url.pathExtension)
        return type?.conforms(to: .movie) == true ? .motion : .stills
    }
}
