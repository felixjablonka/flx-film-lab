import SwiftUI
import AppKit

/// On-canvas crop editing, over the whole frame as it is turned and levelled.
/// Drag a corner or an edge to size the crop, inside it to move it; with the
/// level tool on, draw a line along something that should be level or upright.
/// The crop stays inside the picture, and keeps its proportions unless they
/// are free. It is stored as fractions of the turned frame, so drawn on the
/// proxy it holds at full size.
struct CropOverlay: View {
    @ObservedObject var model: DocumentModel
    /// The image's size on the canvas; the overlay covers exactly it.
    let size: CGSize
    var onLightBackground: Bool = false

    private enum Part: Equatable {
        case corner(x: Int, y: Int)     // -1 for the left or top side, 1 for the right or bottom
        case edge(x: Int, y: Int)       // one of the two is 0
        case move
        case level
    }

    @State private var part: Part?
    /// The crop when the drag began, and as it is being dragged, in the
    /// turned frame's pixels.
    @State private var startRect: CGRect?
    @State private var liveRect: CGRect?
    @State private var levelLine: (CGPoint, CGPoint)?

    private var geometry: FrameGeometry { model.recipe.geometry }
    private var frame: CGSize { model.turnedFrame ?? size }
    /// Canvas points per frame pixel.
    private var scale: CGFloat { size.width / max(frame.width, 1) }

    /// The frame on screen is the one the crop is drawn on: after a turn,
    /// until the turned frame has rendered, the crop is not drawn at all.
    private var frameShowing: Bool {
        guard let map = model.displayMap else { return false }
        return abs(map.frame.width - frame.width) < 1 && abs(map.frame.height - frame.height) < 1
    }

    var body: some View {
        let rect = view(liveRect ?? geometry.cropRect(frame: frame))
        ZStack {
            if frameShowing {
                // Everything the crop leaves out, dimmed.
                Path { p in
                    p.addRect(CGRect(origin: .zero, size: size))
                    p.addRect(rect)
                }
                .fill(Color.black.opacity(0.55), style: FillStyle(eoFill: true))

                thirds(in: rect)
                    .stroke(Color.white.opacity(part == nil ? 0.22 : 0.5), lineWidth: 0.5)
                Rectangle()
                    .path(in: rect)
                    .stroke(Color.white.opacity(0.9), lineWidth: 1)
                    .shadow(color: .black.opacity(0.5), radius: 1)
                handles(in: rect)
                    .fill(Color.white)
                    .shadow(color: .black.opacity(0.6), radius: 1.5)

                if let line = levelLine {
                    Path { p in p.move(to: line.0); p.addLine(to: line.1) }
                        .stroke(Color.accentColor, style: StrokeStyle(lineWidth: 1.5, lineCap: .round))
                        .shadow(color: .black.opacity(0.6), radius: 1)
                }
            }
        }
        .frame(width: size.width, height: size.height)
        .contentShape(Rectangle())
        .gesture(drag)
        .onContinuousHover { phase in
            switch phase {
            case .active(let location): cursor(for: hit(location, rect: rect)).set()
            case .ended: NSCursor.arrow.set()
            }
        }
        .onDisappear { NSCursor.arrow.set() }
    }

    // MARK: Drawing

    private func view(_ r: CGRect) -> CGRect {
        CGRect(x: r.minX * scale, y: r.minY * scale, width: r.width * scale, height: r.height * scale)
    }

    private func thirds(in r: CGRect) -> Path {
        Path { p in
            for i in 1...2 {
                let x = r.minX + r.width * CGFloat(i) / 3, y = r.minY + r.height * CGFloat(i) / 3
                p.move(to: CGPoint(x: x, y: r.minY)); p.addLine(to: CGPoint(x: x, y: r.maxY))
                p.move(to: CGPoint(x: r.minX, y: y)); p.addLine(to: CGPoint(x: r.maxX, y: y))
            }
        }
    }

    /// L-shaped corners and short bars at the middle of each edge, drawn just
    /// inside the crop so they never hide what it leaves out.
    private func handles(in r: CGRect) -> Path {
        let long: CGFloat = min(18, r.width / 3, r.height / 3), thick: CGFloat = 3
        return Path { p in
            for (x, y) in [(r.minX, r.minY), (r.maxX, r.minY), (r.minX, r.maxY), (r.maxX, r.maxY)] {
                let sx: CGFloat = x == r.minX ? 1 : -1, sy: CGFloat = y == r.minY ? 1 : -1
                p.addRect(CGRect(x: min(x, x + sx * long), y: min(y, y + sy * thick),
                                 width: long, height: thick))
                p.addRect(CGRect(x: min(x, x + sx * thick), y: min(y, y + sy * long),
                                 width: thick, height: long))
            }
            let bar = min(16, r.width / 4, r.height / 4)
            p.addRect(CGRect(x: r.midX - bar / 2, y: r.minY, width: bar, height: thick))
            p.addRect(CGRect(x: r.midX - bar / 2, y: r.maxY - thick, width: bar, height: thick))
            p.addRect(CGRect(x: r.minX, y: r.midY - bar / 2, width: thick, height: bar))
            p.addRect(CGRect(x: r.maxX - thick, y: r.midY - bar / 2, width: thick, height: bar))
        }
    }

    // MARK: Hit testing

    private func hit(_ v: CGPoint, rect r: CGRect) -> Part? {
        if model.isLevelling { return .level }
        let reach: CGFloat = 10
        let nearLeft = abs(v.x - r.minX) < reach, nearRight = abs(v.x - r.maxX) < reach
        let nearTop = abs(v.y - r.minY) < reach, nearBottom = abs(v.y - r.maxY) < reach
        let withinX = v.x > r.minX - reach && v.x < r.maxX + reach
        let withinY = v.y > r.minY - reach && v.y < r.maxY + reach
        let x = nearLeft ? -1 : (nearRight ? 1 : 0), y = nearTop ? -1 : (nearBottom ? 1 : 0)
        if x != 0 && y != 0 { return .corner(x: x, y: y) }
        if x != 0 && withinY { return .edge(x: x, y: 0) }
        if y != 0 && withinX { return .edge(x: 0, y: y) }
        if r.contains(v) { return .move }
        return nil
    }

    private func cursor(for part: Part?) -> NSCursor {
        switch part {
        case .edge(let x, _): return x != 0 ? .resizeLeftRight : .resizeUpDown
        case .corner: return .crosshair
        case .move: return .openHand
        case .level: return .crosshair
        case nil: return .arrow
        }
    }

    // MARK: Dragging

    private var drag: some Gesture {
        DragGesture(minimumDistance: 1, coordinateSpace: .local)
            .onChanged { value in
                if part == nil {
                    let start = geometry.cropRect(frame: frame)
                    part = hit(value.startLocation, rect: view(start))
                    startRect = start
                }
                switch part {
                case .level:
                    levelLine = (value.startLocation, value.location)
                case .some(let p):
                    guard let start = startRect else { return }
                    liveRect = dragged(p, start: start, by: CGSize(width: value.translation.width / scale,
                                                                   height: value.translation.height / scale))
                case nil:
                    break
                }
            }
            .onEnded { value in
                defer {
                    part = nil
                    startRect = nil
                    liveRect = nil
                    levelLine = nil
                }
                switch part {
                case .level:
                    let a = CGPoint(x: value.startLocation.x / size.width, y: value.startLocation.y / size.height)
                    let b = CGPoint(x: value.location.x / size.width, y: value.location.y / size.height)
                    model.level(from: a, to: b)
                    model.isLevelling = false
                case .some:
                    if let rect = liveRect { model.editGeometry { g, f in g.setCrop(rect, frame: f) } }
                case nil:
                    break
                }
            }
    }

    /// The crop moved or resized by a drag of `d` frame pixels, as far as it
    /// can go inside the picture.
    private func dragged(_ part: Part, start s: CGRect, by d: CGSize) -> CGRect {
        let ratio = geometry.ratio(frame: frame)
        let minimum = max(24 / scale, 8)
        switch part {
        case .move:
            // One axis at a time, so a crop pushed against an edge slides along it.
            let across = furthest(from: s, to: s.offsetBy(dx: d.width, dy: 0))
            return furthest(from: across, to: across.offsetBy(dx: 0, dy: d.height))

        case .corner(let x, let y):
            // The opposite corner stays put.
            let anchor = CGPoint(x: x < 0 ? s.maxX : s.minX, y: y < 0 ? s.maxY : s.minY)
            var w = max((x < 0 ? s.maxX - (s.minX + d.width) : s.maxX + d.width - s.minX), minimum)
            var h = max((y < 0 ? s.maxY - (s.minY + d.height) : s.maxY + d.height - s.minY), minimum)
            if let r = ratio {
                // Whichever side the drag moved further sets the size.
                if w / r > h { h = w / r } else { w = h * r }
                if h < minimum { h = minimum; w = h * r }
                if w < minimum { w = minimum; h = w / r }
                return furthest(from: s, to: anchored(anchor, x: x, y: y, width: w, height: h))
            }
            // A free corner one side at a time, so it slides along an edge it meets.
            let wide = furthest(from: s, to: anchored(anchor, x: x, y: y, width: w, height: s.height))
            return furthest(from: wide, to: anchored(anchor, x: x, y: y, width: wide.width, height: h))

        case .edge(let x, let y):
            let sideways = x != 0   // the left or the right edge, which sets the width
            let wanted = sideways
                ? max(x < 0 ? s.maxX - (s.minX + d.width) : s.width + d.width, minimum)
                : max(y < 0 ? s.maxY - (s.minY + d.height) : s.height + d.height, minimum)
            guard let r = ratio else {
                let target = sideways
                    ? CGRect(x: x < 0 ? s.maxX - wanted : s.minX, y: s.minY, width: wanted, height: s.height)
                    : CGRect(x: s.minX, y: y < 0 ? s.maxY - wanted : s.minY, width: s.width, height: wanted)
                return furthest(from: s, to: target)
            }
            // With fixed proportions the other sides follow about the middle of
            // the crop, moved as little as keeps it inside the picture.
            func fit(_ side: CGFloat) -> CGRect? {
                let size = sideways ? CGSize(width: side, height: side / r) : CGSize(width: side * r, height: side)
                return sideways
                    ? place(size, x: x < 0 ? s.maxX - size.width : s.minX, near: s.midY - size.height / 2)
                    : place(size, y: y < 0 ? s.maxY - size.height : s.minY, near: s.midX - size.width / 2)
            }
            if let whole = fit(wanted) { return whole }
            // As large as fits between the crop as it was and the size asked for.
            var lo = sideways ? s.width : s.height, hi = wanted, best = s
            for _ in 0..<24 {
                let mid = (lo + hi) / 2
                if let rect = fit(mid) { best = rect; lo = mid } else { hi = mid }
            }
            return best

        case .level:
            return s
        }
    }

    /// A rect of `width` x `height` with one corner at `anchor`, reaching
    /// left of it for `x` -1 and up from it for `y` -1.
    private func anchored(_ anchor: CGPoint, x: Int, y: Int, width: CGFloat, height: CGFloat) -> CGRect {
        CGRect(x: x < 0 ? anchor.x - width : anchor.x, y: y < 0 ? anchor.y - height : anchor.y,
               width: width, height: height)
    }

    /// A crop of `size` at `x` (or `y`), placed along the other axis as near
    /// `near` as it can be inside the picture, if it fits anywhere. The
    /// picture is convex, so the places that fit are one stretch: find one,
    /// then the end of the stretch nearest `near`.
    private func place(_ size: CGSize, x: CGFloat? = nil, y: CGFloat? = nil, near: CGFloat) -> CGRect? {
        let g = geometry, f = frame
        func rect(_ v: CGFloat) -> CGRect {
            CGRect(x: x ?? v, y: y ?? v, width: size.width, height: size.height)
        }
        let span = x != nil ? f.height - size.height : f.width - size.width
        guard span >= 0 else { return nil }
        let start = min(max(near, 0), span)
        if g.contains(rect(start), frame: f) { return rect(start) }
        let fits = (0...32).map { span * CGFloat($0) / 32 }.filter { g.contains(rect($0), frame: f) }
        guard var inside = fits.min(by: { abs($0 - start) < abs($1 - start) }) else { return nil }
        var outside = start
        for _ in 0..<24 {
            let mid = (inside + outside) / 2
            if g.contains(rect(mid), frame: f) { inside = mid } else { outside = mid }
        }
        return rect(inside)
    }

    /// As far from `from`, which lies inside the picture, towards `to` as
    /// stays inside it.
    private func furthest(from a: CGRect, to b: CGRect) -> CGRect {
        let g = geometry, f = frame
        if g.contains(b, frame: f) { return b }
        func between(_ t: CGFloat) -> CGRect {
            CGRect(x: a.minX + (b.minX - a.minX) * t, y: a.minY + (b.minY - a.minY) * t,
                   width: a.width + (b.width - a.width) * t, height: a.height + (b.height - a.height) * t)
        }
        var lo: CGFloat = 0, hi: CGFloat = 1
        for _ in 0..<24 {
            let mid = (lo + hi) / 2
            if g.contains(between(mid), frame: f) { lo = mid } else { hi = mid }
        }
        return between(lo)
    }
}
