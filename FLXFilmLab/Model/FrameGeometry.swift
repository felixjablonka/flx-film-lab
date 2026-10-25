import CoreGraphics
import Foundation

/// The proportions a crop keeps.
enum CropAspect: String, Codable, CaseIterable, Identifiable {
    case original = "Original"
    case threeTwo = "3:2"
    case fourThree = "4:3"
    case fiveFour = "5:4"
    case panoramic = "65:24"
    case custom = "Custom"
    case free = "Free"

    var id: String { rawValue }

    /// Long side over short, where the preset fixes it.
    var longOverShort: Double? {
        switch self {
        case .threeTwo: return 3.0 / 2.0
        case .fourThree: return 4.0 / 3.0
        case .fiveFour: return 5.0 / 4.0
        case .panoramic: return 65.0 / 24.0
        case .original, .custom, .free: return nil
        }
    }
}

/// The print's shape: how the frame is turned, levelled and cropped.
///
/// It shapes the print as an enlarger's easel and a trimmer would. The film is
/// exposed and developed whole; the developed negative is then shaped, and the
/// grain and the print made on the print's own pixels, at the frame's pixel
/// size, so cropping enlarges the grain as cropping a print does
/// (`Renderer.render`, step 2d). Masks and gradients stay where they were
/// drawn on the frame, and the meters read only what the crop keeps.
struct FrameGeometry: Codable, Equatable {
    /// Quarter turns clockwise, 0 to 3, applied after the mirror.
    var quarterTurns: Int = 0
    /// Mirrored left to right, before the turns.
    var mirrored: Bool = false
    /// Levelling in degrees, positive clockwise on screen, within ±45.
    var straighten: Double = 0
    /// The crop as drawn, as fractions of the turned frame: its top-left
    /// corner, width and height. It is shrunk as far as the levelling needs
    /// (`cropRect`), so levelling back gives it back. Nil is the largest crop
    /// of `aspect` that fits, centred: the whole frame, unless the aspect or
    /// the levelling asks for less.
    var crop: CGRect?
    var aspect: CropAspect = .original
    /// The proportions stood upright; nil follows the turned frame's own.
    /// A custom aspect is stood up by its own sides instead.
    var portrait: Bool?
    /// A custom aspect's width and height, as the crop stands.
    var customWidth: Double = 1
    var customHeight: Double = 1

    static let maxStraighten = 45.0
    static let identity = FrameGeometry()

    /// The turned frame's size, for a source of `width` x `height`.
    func turnedSize(width: Double, height: Double) -> CGSize {
        quarterTurns % 2 == 0 ? CGSize(width: width, height: height) : CGSize(width: height, height: width)
    }

    /// Width over height of the crop, in pixels, or nil for a free crop.
    func ratio(frame: CGSize) -> Double? {
        let long: Double
        switch aspect {
        case .free:
            return nil
        case .original:
            long = Double(max(frame.width, frame.height) / max(min(frame.width, frame.height), 1))
        case .custom:
            return customWidth / max(customHeight, 1e-6)
        default:
            long = aspect.longOverShort ?? 1
        }
        let upright = portrait ?? (frame.height > frame.width)
        return upright ? 1 / long : long
    }

    /// Whether the crop is stood upright, as the swap button shows it.
    func isPortrait(frame: CGSize) -> Bool {
        if aspect == .custom { return customHeight > customWidth }
        if let portrait { return portrait }
        if let crop { return crop.height * frame.height > crop.width * frame.width }
        return frame.height > frame.width
    }

    // MARK: The crop

    /// The crop in the turned frame's pixels, inside the picture.
    func cropRect(frame: CGSize) -> CGRect {
        if let drawn = drawnCrop(frame: frame) { return fitted(drawn, frame: frame) }
        guard let r = ratio(frame: frame) else {
            if straighten == 0 { return CGRect(origin: .zero, size: frame) }
            return Self.largestCentred(ratio: Double(frame.width / max(frame.height, 1)), frame: frame,
                                       degrees: straighten)
        }
        return Self.largestCentred(ratio: r, frame: frame, degrees: straighten)
    }

    /// The largest crop of width-over-height `ratio`, centred, that lies
    /// inside a frame levelled by `degrees`, and inside the frame.
    static func largestCentred(ratio r: Double, frame: CGSize, degrees: Double) -> CGRect {
        let theta = abs(degrees) * .pi / 180
        let a = Double(frame.width) / 2, b = Double(frame.height) / 2
        let halfHeight = min(a / (r * cos(theta) + sin(theta)), b / (r * sin(theta) + cos(theta)),
                             a / r, b)
        let halfWidth = r * halfHeight
        return CGRect(x: a - halfWidth, y: b - halfHeight, width: 2 * halfWidth, height: 2 * halfHeight)
    }

    /// The crop as drawn, in the turned frame's pixels.
    func drawnCrop(frame: CGSize) -> CGRect? {
        crop.map { CGRect(x: $0.minX * frame.width, y: $0.minY * frame.height,
                          width: $0.width * frame.width, height: $0.height * frame.height) }
    }

    /// 1/4096 of the frame's long edge, about half a pixel of the preview the
    /// crop is drawn on, so that a crop fits the same at every size.
    static func tolerance(frame: CGSize) -> Double {
        Double(max(frame.width, frame.height)) / 4096
    }

    /// Whether `rect`, in the turned frame's pixels, lies inside the frame and
    /// inside the picture once it is levelled by `straighten`.
    func contains(_ rect: CGRect, frame: CGSize) -> Bool {
        let tolerance = Self.tolerance(frame: frame)
        guard rect.width > 0, rect.height > 0,
              Double(rect.minX) >= -tolerance, Double(rect.minY) >= -tolerance,
              Double(rect.maxX) <= Double(frame.width) + tolerance,
              Double(rect.maxY) <= Double(frame.height) + tolerance else { return false }
        let theta = straighten * .pi / 180
        let c = cos(theta), s = sin(theta)
        let cx = Double(frame.width) / 2, cy = Double(frame.height) / 2
        let a = cx + tolerance, b = cy + tolerance
        for corner in [CGPoint(x: rect.minX, y: rect.minY), CGPoint(x: rect.maxX, y: rect.minY),
                       CGPoint(x: rect.minX, y: rect.maxY), CGPoint(x: rect.maxX, y: rect.maxY)] {
            let x = Double(corner.x) - cx, y = Double(corner.y) - cy
            // Back into the unlevelled frame: the rotation by -straighten.
            let u = c * x + s * y, v = -s * x + c * y
            if abs(u) > a || abs(v) > b { return false }
        }
        return true
    }

    /// `rect` shrunk about its centre until it lies inside the picture, or the
    /// largest centred crop of its proportions if its centre does not.
    func fitted(_ rect: CGRect, frame: CGSize) -> CGRect {
        if contains(rect, frame: frame) { return rect }
        let centre = CGPoint(x: rect.midX, y: rect.midY)
        var lo = 0.0, hi = 1.0
        for _ in 0..<40 {
            let mid = (lo + hi) / 2
            let candidate = rect.scaled(by: mid, about: centre)
            if contains(candidate, frame: frame) { lo = mid } else { hi = mid }
        }
        let shrunk = rect.scaled(by: lo, about: centre)
        if lo > 0.05 { return shrunk }
        return Self.largestCentred(ratio: Double(rect.width / max(rect.height, 1e-6)), frame: frame,
                                   degrees: straighten)
    }

    /// Store a crop given in the turned frame's pixels. The whole frame,
    /// unlevelled, is no crop at all, whatever the aspect.
    mutating func setCrop(_ rect: CGRect, frame: CGSize) {
        let whole = CGRect(origin: .zero, size: frame), t = Self.tolerance(frame: frame)
        if straighten == 0, abs(rect.minX - whole.minX) < t, abs(rect.minY - whole.minY) < t,
           abs(rect.maxX - whole.maxX) < t, abs(rect.maxY - whole.maxY) < t {
            crop = nil
            return
        }
        // On a grid of 2^-40 of the frame, where 1 - x is exact: turned and
        // mirrored any number of times, the crop comes back as it was.
        func snap(_ v: CGFloat) -> CGFloat { (v * 0x1p40).rounded() / 0x1p40 }
        let x0 = snap(rect.minX / frame.width), y0 = snap(rect.minY / frame.height)
        let x1 = snap(rect.maxX / frame.width), y1 = snap(rect.maxY / frame.height)
        crop = CGRect(x: x0, y: y0, width: x1 - x0, height: y1 - y0)
    }

    // MARK: Edits

    /// A drawn crop given the current proportions: as large as before and
    /// where it was, kept inside the picture. Without one, the largest crop of
    /// the proportions follows by itself.
    mutating func refit(frame: CGSize) {
        guard crop != nil, let r = ratio(frame: frame) else { return }
        let current = cropRect(frame: frame)
        let area = Double(current.width * current.height)
        let width = sqrt(area * r), height = sqrt(area / r)
        var rect = CGRect(x: Double(current.midX) - width / 2, y: Double(current.midY) - height / 2,
                          width: width, height: height)
        let largest = Self.largestCentred(ratio: r, frame: frame, degrees: straighten)
        if rect.width > largest.width { rect = rect.scaled(by: largest.width / rect.width, about: rect.centre) }
        setCrop(fitted(rect, frame: frame), frame: frame)
    }

    /// Choose the proportions, refitting the crop to them.
    mutating func setAspect(_ newAspect: CropAspect, frame: CGSize) {
        aspect = newAspect
        refit(frame: frame)
    }

    /// Stand the crop up, or lay it down.
    mutating func swapOrientation(frame: CGSize) {
        if aspect == .custom {
            swap(&customWidth, &customHeight)
        } else {
            portrait = !isPortrait(frame: frame)
        }
        if aspect == .free, crop != nil {
            // A free crop keeps its own shape, turned the other way about its centre.
            let rect = cropRect(frame: frame)
            let turned = CGRect(x: rect.midX - rect.height / 2, y: rect.midY - rect.width / 2,
                                width: rect.height, height: rect.width)
            setCrop(fitted(turned, frame: frame), frame: frame)
        } else {
            refit(frame: frame)
        }
    }

    /// Level by `degrees`; the crop shrinks as far as it must to stay inside
    /// the picture.
    mutating func setStraighten(_ degrees: Double) {
        straighten = min(max(degrees, -Self.maxStraighten), Self.maxStraighten)
    }

    /// A quarter turn, the crop and its proportions turning with the picture.
    mutating func turn(clockwise: Bool) {
        quarterTurns = (quarterTurns + (clockwise ? 1 : 3)) % 4
        if let c = crop {
            crop = clockwise ? CGRect(x: 1 - c.maxY, y: c.minX, width: c.height, height: c.width)
                             : CGRect(x: c.minY, y: 1 - c.maxX, width: c.height, height: c.width)
        }
        if let p = portrait { portrait = !p }
        swap(&customWidth, &customHeight)
    }

    /// Mirror the picture as it stands, left to right or top to bottom.
    mutating func flip(horizontal: Bool) {
        // With the turns applied after the mirror, a mirror of the turned
        // frame is the source's mirror and the turns reversed; top to bottom
        // is that and a half turn.
        quarterTurns = horizontal ? (4 - quarterTurns) % 4 : (6 - quarterTurns) % 4
        mirrored.toggle()
        straighten = -straighten
        if let c = crop {
            crop = horizontal ? CGRect(x: 1 - c.maxX, y: c.minY, width: c.width, height: c.height)
                              : CGRect(x: c.minX, y: 1 - c.maxY, width: c.width, height: c.height)
        }
    }

    /// Back to the frame as shot: no turns, mirror, levelling or crop.
    mutating func reset() { self = FrameGeometry() }

    // MARK: Metering

    /// The same crop of the same picture with the turns and the mirror
    /// undone, so that turning or flipping a print never changes how the
    /// meters print it.
    var upright: FrameGeometry {
        var g = self
        while g.quarterTurns % 4 != 0 { g.turn(clockwise: false) }
        if g.mirrored { g.flip(horizontal: true) }
        return g
    }

    /// What the meters read of `pixels` (RGBA, `width` x `height`): the part
    /// of the frame the print keeps, at the frame's own scale, or the frame
    /// itself when it keeps all of it.
    func metered(_ pixels: [Float], width: Int, height: Int) -> (pixels: [Float], width: Int, height: Int) {
        let map = GeometryMap(upright, sourceWidth: width, sourceHeight: height)
        if map.isIdentity { return (pixels, width, height) }
        return map.resample(pixels, maxLongEdge: max(width, height))
    }
}

/// A geometry worked out for one size of source: where each pixel of the
/// print comes from in the frame, and back.
struct GeometryMap {
    let sourceSize: CGSize
    /// The frame after its turns and mirror.
    let frame: CGSize
    /// The crop in the turned frame's pixels, whole pixels.
    let crop: CGRect
    /// Source pixel to print pixel.
    let sourceToOutput: CGAffineTransform
    let outputToSource: CGAffineTransform
    let isIdentity: Bool

    var outputWidth: Int { Int(crop.width) }
    var outputHeight: Int { Int(crop.height) }

    /// `wholeFrame` shows the whole turned, levelled frame, its corners empty
    /// where levelling leaves them, for editing the crop.
    init(_ g: FrameGeometry, sourceWidth: Int, sourceHeight: Int, wholeFrame: Bool = false) {
        let w = Double(sourceWidth), h = Double(sourceHeight)
        sourceSize = CGSize(width: w, height: h)
        var orient = CGAffineTransform.identity
        var fw = w, fh = h
        if g.mirrored { orient = orient.concatenating(CGAffineTransform(a: -1, b: 0, c: 0, d: 1, tx: fw, ty: 0)) }
        for _ in 0..<((g.quarterTurns % 4 + 4) % 4) {
            // A clockwise quarter turn: (x, y) in a w x h frame to (h - y, x).
            orient = orient.concatenating(CGAffineTransform(a: 0, b: 1, c: -1, d: 0, tx: fh, ty: 0))
            swap(&fw, &fh)
        }
        frame = CGSize(width: fw, height: fh)
        let theta = g.straighten * .pi / 180
        let c = cos(theta), s = sin(theta)
        let cx = fw / 2, cy = fh / 2
        // Clockwise on screen, y down, about the frame's centre.
        let level = CGAffineTransform(a: c, b: s, c: -s, d: c,
                                      tx: cx - c * cx + s * cy, ty: cy - s * cx - c * cy)
        let rect = wholeFrame ? CGRect(x: 0, y: 0, width: fw, height: fh) : g.cropRect(frame: frame)
        // To whole pixels: inwards when levelled, so none reach past the
        // picture into its empty corners.
        let inwards = !wholeFrame && g.straighten != 0
        var x0 = max(inwards ? (rect.minX - 1e-6).rounded(.up) : rect.minX.rounded(), 0)
        var y0 = max(inwards ? (rect.minY - 1e-6).rounded(.up) : rect.minY.rounded(), 0)
        var x1 = min(inwards ? (rect.maxX + 1e-6).rounded(.down) : rect.maxX.rounded(), fw)
        var y1 = min(inwards ? (rect.maxY + 1e-6).rounded(.down) : rect.maxY.rounded(), fh)
        let whole = x0 == 0 && y0 == 0 && x1 == fw && y1 == fh
        if !wholeFrame, !whole, let r = g.ratio(frame: frame) {
            // The crop is drawn on the proxy, whose sides are rounded, so at
            // this size its proportions are worked out again in whole pixels,
            // the side that is too long giving way.
            let w = x1 - x0, h = y1 - y0
            if w > (h * r).rounded() {
                let narrower = max((h * r).rounded(), 1)
                x0 += ((w - narrower) / 2).rounded(.down)
                x1 = x0 + narrower
            } else if h > (w / r).rounded() {
                let shorter = max((w / r).rounded(), 1)
                y0 += ((h - shorter) / 2).rounded(.down)
                y1 = y0 + shorter
            }
        }
        crop = CGRect(x: x0, y: y0, width: max(x1 - x0, 1), height: max(y1 - y0, 1))
        sourceToOutput = orient.concatenating(level).concatenating(CGAffineTransform(translationX: -x0, y: -y0))
        outputToSource = sourceToOutput.inverted()
        isIdentity = g.quarterTurns % 4 == 0 && !g.mirrored && g.straighten == 0
            && crop == CGRect(x: 0, y: 0, width: w, height: h)
    }

    /// A source point in normalised frame coordinates (as masks store them) to
    /// the print's pixels, and back.
    func outputPoint(fromNormalised p: CGPoint) -> CGPoint {
        CGPoint(x: p.x * sourceSize.width, y: p.y * sourceSize.height).applying(sourceToOutput)
    }

    func normalisedSource(fromOutput p: CGPoint) -> CGPoint {
        let s = p.applying(outputToSource)
        return CGPoint(x: s.x / max(sourceSize.width, 1), y: s.y / max(sourceSize.height, 1))
    }

    /// The print cut out of `pixels` (RGBA, `sourceSize`), bilinear, at most
    /// `maxLongEdge` on its long side: what the meters read. A crop that is
    /// neither turned nor levelled is copied pixel for pixel.
    func resample(_ pixels: [Float], maxLongEdge: Int) -> (pixels: [Float], width: Int, height: Int) {
        let sw = Int(sourceSize.width), sh = Int(sourceSize.height)
        let scale = min(1, Double(maxLongEdge) / Double(max(outputWidth, outputHeight, 1)))
        let ow = max(Int((Double(outputWidth) * scale).rounded()), 1)
        let oh = max(Int((Double(outputHeight) * scale).rounded()), 1)
        var out = [Float](repeating: 0, count: ow * oh * 4)
        guard pixels.count >= sw * sh * 4 else { return (out, ow, oh) }
        let t = outputToSource
        let left = Int(t.tx), top = Int(t.ty)
        if scale == 1, t.a == 1, t.b == 0, t.c == 0, t.d == 1, Double(left) == t.tx, Double(top) == t.ty,
           left >= 0, top >= 0, left + ow <= sw, top + oh <= sh {
            pixels.withUnsafeBufferPointer { src in
                out.withUnsafeMutableBufferPointer { dst in
                    for y in 0..<oh {
                        dst.baseAddress!.advanced(by: y * ow * 4)
                            .update(from: src.baseAddress!.advanced(by: ((top + y) * sw + left) * 4), count: ow * 4)
                    }
                }
            }
            return (out, ow, oh)
        }
        pixels.withUnsafeBufferPointer { src in
            out.withUnsafeMutableBufferPointer { buffer in
                let dst = buffer
                DispatchQueue.concurrentPerform(iterations: oh) { y in
                    for x in 0..<ow {
                        let ox = (Double(x) + 0.5) / scale, oy = (Double(y) + 0.5) / scale
                        let sx = t.a * ox + t.c * oy + t.tx - 0.5, sy = t.b * ox + t.d * oy + t.ty - 0.5
                        let fx = sx.rounded(.down), fy = sy.rounded(.down)
                        let ax = Float(sx - fx), ay = Float(sy - fy)
                        let x0 = min(max(Int(fx), 0), sw - 1), y0 = min(max(Int(fy), 0), sh - 1)
                        let x1 = min(max(Int(fx) + 1, 0), sw - 1), y1 = min(max(Int(fy) + 1, 0), sh - 1)
                        let o = (y * ow + x) * 4
                        for ch in 0..<4 {
                            let p00 = src[(y0 * sw + x0) * 4 + ch], p10 = src[(y0 * sw + x1) * 4 + ch]
                            let p01 = src[(y1 * sw + x0) * 4 + ch], p11 = src[(y1 * sw + x1) * 4 + ch]
                            let top = p00 + (p10 - p00) * ax, bottom = p01 + (p11 - p01) * ax
                            dst[o + ch] = top + (bottom - top) * ay
                        }
                    }
                }
            }
        }
        return (out, ow, oh)
    }
}

extension CGRect {
    var centre: CGPoint { CGPoint(x: midX, y: midY) }

    func scaled(by factor: Double, about c: CGPoint) -> CGRect {
        CGRect(x: c.x - (c.x - minX) * factor, y: c.y - (c.y - minY) * factor,
               width: width * factor, height: height * factor)
    }
}
