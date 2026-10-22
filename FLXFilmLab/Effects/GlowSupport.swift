import Foundation

/// The one piece of Glow's `SourceImage.swift` the vendored kernels need.
public enum GlowError: LocalizedError {
    case kernel(String)

    public var errorDescription: String? {
        switch self {
        case .kernel(let name): return "The image filter \(name) failed to load."
        }
    }
}
