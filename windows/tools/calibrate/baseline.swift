import CoreImage
import Foundation

// Apple's baseline exposure for each RAW file, as Core Image's RAW decoder
// gives it (CIRAWFilter.baselineExposure), in EV: what iso_exposure.py reads
// at each ISO a camera's files may carry.
//
//   flxbaseline FILE...    one line per file: FILE, a tab, EV ("-" where it does not open)
//
// On the decoder version the film decode uses (RawLoader.keepDecoderVersion):
// Core Image's RAW decoder 8 where its default is newer and the file offers 8.
func decoderNumber(_ version: CIRAWDecoderVersion) -> Int? { Int(version.rawValue.prefix(while: \.isNumber)) }
for path in CommandLine.arguments.dropFirst() {
    if let filter = CIRAWFilter(imageURL: URL(fileURLWithPath: path)) {
        if let current = decoderNumber(filter.decoderVersion), current > 8,
           let kept = filter.supportedDecoderVersions.first(where: { decoderNumber($0) == 8 }) {
            filter.decoderVersion = kept
        }
        print("\(path)\t\(filter.baselineExposure)")
    } else {
        print("\(path)\t-")
    }
}
