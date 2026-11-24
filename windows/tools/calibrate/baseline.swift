import CoreImage
import Foundation

// Apple's baseline exposure for each RAW file, as Core Image's RAW decoder
// gives it (CIRAWFilter.baselineExposure), in EV: what iso_exposure.py reads
// at each ISO a camera's files may carry.
//
//   flxbaseline FILE...    one line per file: FILE, a tab, EV ("-" where it does not open)
//
// On the decoder version the film decode uses (RawLoader.keepDecoderVersion):
// Core Image's RAW decoder 8, or the newest version before it the file offers,
// where its default is newer.
func decoderNumber(_ version: CIRAWDecoderVersion) -> Int? { Int(version.rawValue.prefix(while: \.isNumber)) }
// 8, or the newest version before it the file offers (a D700's are 6, 7 and 9)
func keptVersion(_ filter: CIRAWFilter) -> CIRAWDecoderVersion? {
    var kept: (version: CIRAWDecoderVersion, number: Int)?
    for version in filter.supportedDecoderVersions {
        guard let n = decoderNumber(version), n <= 8, n > (kept?.number ?? Int.min) else { continue }
        kept = (version, n)
    }
    return kept?.version
}
for path in CommandLine.arguments.dropFirst() {
    if let filter = CIRAWFilter(imageURL: URL(fileURLWithPath: path)) {
        if let current = decoderNumber(filter.decoderVersion), current > 8, let kept = keptVersion(filter) {
            filter.decoderVersion = kept
        }
        print("\(path)\t\(filter.baselineExposure)")
    } else {
        print("\(path)\t-")
    }
}
