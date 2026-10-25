import SwiftUI

/// On-canvas mask editing: draw the outline, drag the control points.
/// Points are stored normalised, so a mask drawn on the proxy applies unchanged
/// to the full-resolution export. The exposure gradients are edited here too,
/// sharing the selection: identifiers are unique across both.
///
/// Points belong to the frame, not to the print: on a turned, levelled or
/// cropped print they are drawn and dragged through `map`, where the image on
/// screen comes from in the frame.
struct MaskOverlay: View {
    @Binding var masks: [LocalMask]
    @Binding var gradients: [ExposureGradient]
    @Binding var selected: UUID?
    let frame: CGRect
    var map: GeometryMap? = nil
    /// A white outline vanishes on a white surround, so it flips to dark.
    var onLightBackground: Bool = false

    /// Drag locations have to be read in the overlay's own space.
    ///
    /// `.position()` re-roots a view's coordinate space on itself, so a plain
    /// `DragGesture` on a positioned handle reports a location inside the 9-point
    /// circle rather than on the canvas — every drag normalised to roughly zero
    /// and threw the control point into the top-left corner. Naming the space
    /// and asking the gesture for that space is the fix.
    private static let space = "maskCanvas"

    /// Where a drag started, so a handle moves by the drag's translation instead
    /// of jumping its centre to the cursor.
    @State private var dragOrigin: [String: CGPoint] = [:]
    /// A gradient's handles at the start of a drag: the middle one moves both.
    @State private var gradientDragOrigin: [CGPoint]?

    var body: some View {
        ZStack {
            ForEach(masks) { mask in
                if mask.enabled {
                    outline(for: mask)
                        .stroke(style: StrokeStyle(lineWidth: mask.id == selected ? 1.5 : 1,
                                                   dash: mask.stops >= 0 ? [] : [4, 3]))
                        .foregroundStyle(mask.id == selected
                                         ? Color.accentColor
                                         : (onLightBackground ? Color.black.opacity(0.45)
                                                              : Color.white.opacity(0.45)))
                        .shadow(color: (onLightBackground ? Color.white : Color.black).opacity(0.6),
                                radius: 1)
                }
            }
            ForEach(gradients) { g in
                if g.enabled, g.points.count >= 2 {
                    let colour = g.id == selected
                        ? Color.accentColor
                        : (onLightBackground ? Color.black.opacity(0.45) : Color.white.opacity(0.45))
                    let width: CGFloat = g.id == selected ? 1.5 : 1
                    let lines = gradientLines(g)
                    // Solid where the compensation is full, dashed where it has
                    // faded out, so the direction reads at a glance. Clipped to
                    // the frame: there is nothing to act on past it.
                    lines.full.stroke(style: StrokeStyle(lineWidth: width))
                        .foregroundStyle(colour)
                        .shadow(color: (onLightBackground ? Color.white : Color.black).opacity(0.6), radius: 1)
                        .clipped()
                    lines.none.stroke(style: StrokeStyle(lineWidth: width, dash: [6, 4]))
                        .foregroundStyle(colour)
                        .shadow(color: (onLightBackground ? Color.white : Color.black).opacity(0.6), radius: 1)
                        .clipped()
                }
            }
            if let selected, let index = masks.firstIndex(where: { $0.id == selected }) {
                handles(maskIndex: index)
            } else if let selected, let index = gradients.firstIndex(where: { $0.id == selected }),
                      gradients[index].points.count >= 2 {
                gradientHandle(id: selected, pointIndex: 0)
                gradientHandle(id: selected, pointIndex: 1)
                gradientHandle(id: selected, pointIndex: nil)
            }
        }
        .coordinateSpace(name: Self.space)
        .allowsHitTesting(selected != nil)
    }

    /// A point of the frame, normalised, on the canvas.
    private func point(_ p: CGPoint) -> CGPoint {
        guard let map, !map.isIdentity else { return CGPoint(x: p.x * frame.width, y: p.y * frame.height) }
        let o = map.outputPoint(fromNormalised: p)
        return CGPoint(x: o.x / CGFloat(map.outputWidth) * frame.width,
                       y: o.y / CGFloat(map.outputHeight) * frame.height)
    }

    /// A point on the canvas in the frame, normalised.
    private func normalized(_ p: CGPoint) -> CGPoint {
        var n = CGPoint(x: p.x / max(frame.width, 1), y: p.y / max(frame.height, 1))
        if let map, !map.isIdentity {
            n = map.normalisedSource(fromOutput: CGPoint(x: n.x * CGFloat(map.outputWidth),
                                                         y: n.y * CGFloat(map.outputHeight)))
        }
        return CGPoint(x: min(max(n.x, -0.5), 1.5), y: min(max(n.y, -0.5), 1.5))
    }

    /// Where `start` goes when dragged by `translation` on the canvas.
    private func dragged(_ start: CGPoint, by translation: CGSize) -> CGPoint {
        let p = point(start)
        return normalized(CGPoint(x: p.x + translation.width, y: p.y + translation.height))
    }

    private func outline(for mask: LocalMask) -> Path {
        var path = Path()
        let pts = mask.points.map(point)
        guard !pts.isEmpty else { return path }
        switch mask.shape {
        case .polygon:
            guard pts.count >= 3 else { return path }
            path.move(to: pts[0])
            for p in pts.dropFirst() { path.addLine(to: p) }
            path.closeSubpath()
        case .oval:
            guard pts.count >= 3 else { return path }
            let c = pts[0]
            let u = CGPoint(x: pts[1].x - c.x, y: pts[1].y - c.y)
            let v = CGPoint(x: pts[2].x - c.x, y: pts[2].y - c.y)
            for i in 0...64 {
                let t = 2 * CGFloat.pi * CGFloat(i) / 64
                let p = CGPoint(x: c.x + u.x * cos(t) + v.x * sin(t),
                                y: c.y + u.y * cos(t) + v.y * sin(t))
                if i == 0 { path.move(to: p) } else { path.addLine(to: p) }
            }
        case .gradient:
            guard pts.count >= 2 else { return path }
            // The two card edges, perpendicular to the handle direction
            let a = pts[0], b = pts[1]
            let d = CGPoint(x: b.x - a.x, y: b.y - a.y)
            let length = max(sqrt(d.x * d.x + d.y * d.y), 1)
            let n = CGPoint(x: -d.y / length, y: d.x / length)
            let reach = max(frame.width, frame.height)
            for anchor in [a, b] {
                path.move(to: CGPoint(x: anchor.x - n.x * reach, y: anchor.y - n.y * reach))
                path.addLine(to: CGPoint(x: anchor.x + n.x * reach, y: anchor.y + n.y * reach))
            }
            path.move(to: a); path.addLine(to: b)
        }
        return path
    }

    @ViewBuilder
    private func handles(maskIndex: Int) -> some View {
        // Re-check the index on every access: a mask can be deleted while its
        // handles are on screen.
        if masks.indices.contains(maskIndex) {
            let mask = masks[maskIndex]
            ForEach(mask.points.indices, id: \.self) { i in
                handle(maskIndex: maskIndex, pointIndex: i, isCentre: mask.shape == .oval && i == 0)
            }
        }
    }

    private func handle(maskIndex: Int, pointIndex: Int, isCentre: Bool) -> some View {
        let position = masks.indices.contains(maskIndex)
            && masks[maskIndex].points.indices.contains(pointIndex)
            ? point(masks[maskIndex].points[pointIndex]) : .zero

        return Circle()
            .fill(isCentre ? Color.white : Color.accentColor)
            .overlay(Circle().stroke(isCentre ? Color.accentColor : .white, lineWidth: 1.4))
            .frame(width: isCentre ? 11 : 9, height: isCentre ? 11 : 9)
            // A 9-point target is hard to hit; the padding is invisible but grabbable.
            .contentShape(Circle().inset(by: -9))
            .position(position)
            .gesture(
                DragGesture(minimumDistance: 0, coordinateSpace: .named(Self.space))
                    .onChanged { value in
                        guard masks.indices.contains(maskIndex),
                              masks[maskIndex].points.indices.contains(pointIndex) else { return }

                        let key = "\(masks[maskIndex].id)-\(pointIndex)"
                        let start = dragOrigin[key] ?? masks[maskIndex].points[pointIndex]
                        if dragOrigin[key] == nil { dragOrigin[key] = start }

                        let moved = dragged(start, by: value.translation)

                        if isCentre {
                            // An oval's axes are stored relative to its centre, so
                            // moving the centre alone would reshape it rather than
                            // move it. Carry the axis handles along.
                            let shift = CGPoint(x: moved.x - masks[maskIndex].points[0].x,
                                                y: moved.y - masks[maskIndex].points[0].y)
                            for j in masks[maskIndex].points.indices {
                                masks[maskIndex].points[j].x += shift.x
                                masks[maskIndex].points[j].y += shift.y
                            }
                        } else {
                            masks[maskIndex].points[pointIndex] = moved
                        }
                    }
                    .onEnded { _ in
                        guard masks.indices.contains(maskIndex) else { return }
                        dragOrigin["\(masks[maskIndex].id)-\(pointIndex)"] = nil
                    }
            )
    }

    // MARK: - Exposure gradient

    /// The full-compensation edge with the handle line, and the edge where it
    /// has faded out, each drawn right across the frame.
    private func gradientLines(_ g: ExposureGradient) -> (full: Path, none: Path) {
        let a = point(g.points[0]), b = point(g.points[1])
        let d = CGPoint(x: b.x - a.x, y: b.y - a.y)
        let length = max(sqrt(d.x * d.x + d.y * d.y), 1)
        let n = CGPoint(x: -d.y / length, y: d.x / length)
        let reach = max(frame.width, frame.height) * 2
        var full = Path(), none = Path()
        full.move(to: CGPoint(x: a.x - n.x * reach, y: a.y - n.y * reach))
        full.addLine(to: CGPoint(x: a.x + n.x * reach, y: a.y + n.y * reach))
        full.move(to: a); full.addLine(to: b)
        none.move(to: CGPoint(x: b.x - n.x * reach, y: b.y - n.y * reach))
        none.addLine(to: CGPoint(x: b.x + n.x * reach, y: b.y + n.y * reach))
        return (full, none)
    }

    /// One of a gradient's two handles, or (`pointIndex` nil) the one between
    /// them, which slides the whole transition without changing its angle or
    /// width. Found by identifier on every access, so deleting the gradient
    /// while it is being dragged is harmless.
    private func gradientHandle(id: UUID, pointIndex: Int?) -> some View {
        let points = gradients.first(where: { $0.id == id })?.points ?? []
        let position: CGPoint
        if points.count >= 2 {
            position = pointIndex.map { point(points[$0]) }
                ?? point(CGPoint(x: (points[0].x + points[1].x) / 2, y: (points[0].y + points[1].y) / 2))
        } else {
            position = .zero
        }
        let isMiddle = pointIndex == nil

        return Circle()
            .fill(isMiddle ? Color.white : Color.accentColor)
            .overlay(Circle().stroke(isMiddle ? Color.accentColor : .white, lineWidth: 1.4))
            .frame(width: isMiddle ? 11 : 9, height: isMiddle ? 11 : 9)
            .contentShape(Circle().inset(by: -9))
            .position(position)
            .gesture(
                DragGesture(minimumDistance: 0, coordinateSpace: .named(Self.space))
                    .onChanged { value in
                        guard let index = gradients.firstIndex(where: { $0.id == id }),
                              gradients[index].points.count >= 2 else { return }
                        let start = gradientDragOrigin ?? gradients[index].points
                        if gradientDragOrigin == nil { gradientDragOrigin = start }
                        if let i = pointIndex {
                            gradients[index].points[i] = dragged(start[i], by: value.translation)
                        } else {
                            gradients[index].points = start.map { dragged($0, by: value.translation) }
                        }
                    }
                    .onEnded { _ in gradientDragOrigin = nil }
            )
    }
}
