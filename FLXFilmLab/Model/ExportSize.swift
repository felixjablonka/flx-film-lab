import Foundation

/// How large Export and Export Selected write a print, chosen beside the
/// format in the toolbar and kept between launches: at its full size, or with
/// its long edge a number of pixels, the short edge in proportion. A print
/// smaller than that is written at its full size: never enlarged
/// (`Resampler`). The long edge is kept while the full size is chosen, to
/// come back to.
struct ExportSize: Equatable {
    var isFullSize = true
    var longEdge = ExportSize.standardLongEdge

    static let key = "exportSize"
    static let longEdgeKey = "exportLongEdge"
    /// The stored values of `isFullSize`.
    static let fullSize = "full"
    static let limited = "long"
    /// The long edge until another is set: a common size for showing a
    /// picture on the web.
    static let standardLongEdge = 2048
    /// The long edges that can be set: from a small picture to the largest a
    /// JPEG holds.
    static let longEdges = 64...65_500

    /// The long edge to shrink to, nil for the full size.
    var limit: Int? { isFullSize ? nil : longEdge }

    static func current(_ defaults: UserDefaults) -> ExportSize {
        ExportSize(stored: defaults.string(forKey: key), longEdge: defaults.integer(forKey: longEdgeKey))
    }

    func save(_ defaults: UserDefaults) {
        defaults.set(isFullSize ? Self.fullSize : Self.limited, forKey: Self.key)
        defaults.set(longEdge, forKey: Self.longEdgeKey)
    }

    /// "Full Size" or "2048 px", as the toolbar shows it.
    var title: String { isFullSize ? "Full Size" : "\(longEdge) px" }

    /// How large a file is written, after its format in a panel's message:
    /// "JPEG at full size", "JPEG, 2048 px on the long edge".
    var phrase: String { isFullSize ? " at full size" : ", \(longEdge) px on the long edge" }
}

extension ExportSize {
    /// From the values stored under `key` and `longEdgeKey`, as `@AppStorage`
    /// holds them: anything but "long" is the full size, and a long edge out
    /// of `longEdges` (none stored) the standard one.
    init(stored mode: String?, longEdge: Int) {
        self.init(isFullSize: mode != Self.limited,
                  longEdge: Self.longEdges.contains(longEdge) ? longEdge : Self.standardLongEdge)
    }
}
