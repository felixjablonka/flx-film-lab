import SwiftUI

private func channelColor(_ c: Int) -> Color {
    [Color(red: 1, green: 0.27, blue: 0.27),
     Color(red: 0.3, green: 1, blue: 0.38),
     Color(red: 0.35, green: 0.52, blue: 1)][c]
}

/// One channel's bins as a filled outline across `rect`, scaled so `peak`
/// reaches the top.
private func histogramPath(_ bins: [UInt32], channel c: Int, peak: UInt32, in rect: CGRect) -> Path {
    var path = Path()
    path.move(to: CGPoint(x: rect.minX, y: rect.maxY))
    for i in 0..<256 {
        let v = min(CGFloat(bins[c * 256 + i]) / CGFloat(max(peak, 1)), 1)
        path.addLine(to: CGPoint(x: rect.minX + rect.width * CGFloat(i) / 255, y: rect.maxY - rect.height * v))
    }
    path.addLine(to: CGPoint(x: rect.maxX, y: rect.maxY))
    path.closeSubpath()
    return path
}

/// The tallest bin away from the two ends: those can hold whole areas
/// clipped to black or white and would flatten everything else.
private func histogramPeak(_ bins: [UInt32], channels: [Int]) -> UInt32 {
    var peak: UInt32 = 1
    for c in channels { for i in 1..<255 { peak = max(peak, bins[c * 256 + i]) } }
    return peak
}

/// The finished print's histogram: red, green and blue laid over one another
/// and added, so they show white where they agree. A mark at either end shows
/// a channel clipped there, in that channel's colour.
struct HistogramView: View {
    let bins: [UInt32]?
    var height: CGFloat = 56

    /// Pixels at 0 or 255, as a share of the print, per channel.
    private func clipped(_ bins: [UInt32], at bin: Int) -> [Double] {
        let total = Double(bins[0..<256].reduce(0) { $0 + UInt64($1) })
        return (0..<3).map { total > 0 ? Double(bins[$0 * 256 + bin]) / total : 0 }
    }

    var body: some View {
        Canvas { context, size in
            guard let bins, bins.count == 768 else { return }
            let rect = CGRect(origin: .zero, size: size).insetBy(dx: 0, dy: 3)
            let peak = histogramPeak(bins, channels: [0, 1, 2])
            context.blendMode = .plusLighter
            for c in 0..<3 {
                context.fill(histogramPath(bins, channel: c, peak: peak, in: rect),
                             with: .color(channelColor(c).opacity(0.72)))
            }
            context.blendMode = .normal
            // Over 0.05% of the print at an end is worth a mark.
            for (bin, x) in [(0, CGFloat(0)), (255, size.width)] {
                let share = clipped(bins, at: bin)
                guard share.contains(where: { $0 > 0.0005 }) else { continue }
                let lit = share.map { $0 > 0.0005 ? 1.0 : 0.2 }
                var mark = Path()
                let d: CGFloat = x == 0 ? 1 : -1
                mark.move(to: CGPoint(x: x, y: 0))
                mark.addLine(to: CGPoint(x: x + d * 9, y: 0))
                mark.addLine(to: CGPoint(x: x, y: 9))
                mark.closeSubpath()
                context.fill(mark, with: .color(Color(red: lit[0], green: lit[1], blue: lit[2])))
            }
        }
        .frame(height: height)
        .background(Color.black.opacity(0.28))
        .clipShape(RoundedRectangle(cornerRadius: 5))
        .help(helpText)
    }

    private var helpText: String {
        guard let bins, bins.count == 768 else { return "The print's histogram." }
        let low = clipped(bins, at: 0), high = clipped(bins, at: 255)
        func line(_ shares: [Double], _ end: String) -> String? {
            let named = zip(["red", "green", "blue"], shares).filter { $0.1 > 0.0005 }
            guard !named.isEmpty else { return nil }
            return "At \(end): " + named.map { String(format: "%@ %.1f%%", $0.0, $0.1 * 100) }
                .joined(separator: ", ") + "."
        }
        let marks = [line(low, "black"), line(high, "white")].compactMap { $0 }
        return (["The finished print's histogram, as its file will hold it: red, green and blue, white where they agree."]
                + marks).joined(separator: " ")
    }
}

/// Point curves on the finished print (`PrintCurves`): one for all three
/// channels and one for each. Click to add a point, drag it, drag it off the
/// graph to take it away. Behind the curve is the histogram of what it is
/// given, so the points can be put where the print's tones are.
struct CurvesEditor: View {
    @Binding var curves: PrintCurves?
    let histogram: PrintHistogram?
    var height: CGFloat = 176

    @State private var channel: PrintCurves.Channel = .rgb
    /// The point being dragged, by its place in its channel's points.
    @State private var dragging: Int?
    /// The dragged point is off the graph: it goes when let go.
    @State private var removing = false

    private let inset: CGFloat = 6
    private var value: PrintCurves { curves ?? PrintCurves() }
    private var points: [CurvePoint] { PrintCurves.sanitized(value[channel]) }

    private func colour(_ channel: PrintCurves.Channel) -> Color {
        switch channel {
        case .rgb: return .white
        case .red: return channelColor(0)
        case .green: return channelColor(1)
        case .blue: return channelColor(2)
        }
    }

    var body: some View {
        VStack(spacing: 5) {
            HStack(spacing: 6) {
                Picker("", selection: $channel) {
                    Text("RGB").tag(PrintCurves.Channel.rgb)
                    Text("Red").tag(PrintCurves.Channel.red)
                    Text("Green").tag(PrintCurves.Channel.green)
                    Text("Blue").tag(PrintCurves.Channel.blue)
                }
                .pickerStyle(.segmented).labelsHidden().controlSize(.small)
                Button { straighten(channel) } label: { Image(systemName: "arrow.counterclockwise") }
                    .buttonStyle(.borderless).controlSize(.small)
                    .disabled(PrintCurves.isStraight(value[channel]))
                    .help("Straighten this curve again.")
            }
            GeometryReader { geo in
                Canvas { context, size in draw(&context, size: size) }
                    .contentShape(Rectangle())
                    .gesture(drag(in: geo.size))
                    .overlay(alignment: .topLeading) { readout }
            }
            .frame(height: height)
            .background(Color.black.opacity(0.28))
            .clipShape(RoundedRectangle(cornerRadius: 5))
            .contextMenu {
                Button("Straighten \(channelName) Curve") { straighten(channel) }
                    .disabled(PrintCurves.isStraight(value[channel]))
                Button("Straighten All Curves") { curves = nil }
                    .disabled(value.isIdentity)
            }
            .help("Curves on the finished print, as its file will hold it, after everything else. RGB moves all three channels; Red, Green and Blue each move one, after RGB. Click to add a point, drag it, drag it off the graph to take it away. Behind the curve is the histogram of the print it is given. The meters read the print before the curves.")
        }
    }

    private var channelName: String {
        switch channel {
        case .rgb: return "RGB"
        case .red: return "Red"
        case .green: return "Green"
        case .blue: return "Blue"
        }
    }

    /// In and out on the 0...255 scale of an 8-bit file, while a point is dragged.
    @ViewBuilder private var readout: some View {
        if let i = dragging, i < points.count {
            Text(removing ? "Let go to remove"
                 : String(format: "%.0f → %.0f", points[i].x * 255, points[i].y * 255))
                .font(.system(size: 9, design: .monospaced))
                .foregroundStyle(removing ? Color.orange : Color.secondary)
                .padding(5)
        }
    }

    // MARK: Geometry

    private func plotRect(_ size: CGSize) -> CGRect {
        CGRect(origin: .zero, size: size).insetBy(dx: inset, dy: inset)
    }

    private func location(of p: CurvePoint, in size: CGSize) -> CGPoint {
        let r = plotRect(size)
        return CGPoint(x: r.minX + r.width * p.x, y: r.maxY - r.height * p.y)
    }

    private func curvePoint(at location: CGPoint, in size: CGSize) -> CurvePoint {
        let r = plotRect(size)
        return CurvePoint(x: min(max(Double((location.x - r.minX) / max(r.width, 1)), 0), 1),
                          y: min(max(Double((r.maxY - location.y) / max(r.height, 1)), 0), 1))
    }

    // MARK: Drawing

    private func draw(_ context: inout GraphicsContext, size: CGSize) {
        let r = plotRect(size)

        // What the curve is given.
        if let histogram, histogram.after.count == 768 {
            let bins = histogram.input(of: channel, curves: curves)
            let shown: [Int] = channel == .rgb ? [0, 1, 2]
                : [channel == .red ? 0 : channel == .green ? 1 : 2]
            let peak = histogramPeak(bins, channels: shown)
            var layer = context
            layer.blendMode = .plusLighter
            for c in shown {
                layer.fill(histogramPath(bins, channel: c, peak: peak, in: r),
                           with: .color(channelColor(c).opacity(channel == .rgb ? 0.16 : 0.22)))
            }
        }

        var grid = Path()
        for i in 1..<4 {
            let x = r.minX + r.width * CGFloat(i) / 4, y = r.minY + r.height * CGFloat(i) / 4
            grid.move(to: CGPoint(x: x, y: r.minY)); grid.addLine(to: CGPoint(x: x, y: r.maxY))
            grid.move(to: CGPoint(x: r.minX, y: y)); grid.addLine(to: CGPoint(x: r.maxX, y: y))
        }
        context.stroke(grid, with: .color(.white.opacity(0.07)), lineWidth: 0.5)
        var diagonal = Path()
        diagonal.move(to: CGPoint(x: r.minX, y: r.maxY)); diagonal.addLine(to: CGPoint(x: r.maxX, y: r.minY))
        context.stroke(diagonal, with: .color(.white.opacity(0.18)), style: StrokeStyle(lineWidth: 0.75, dash: [3, 3]))

        // The other curves, faint: the channels' under RGB, RGB's under a channel.
        let others: [PrintCurves.Channel] = channel == .rgb ? [.red, .green, .blue] : [.rgb]
        for other in others where !PrintCurves.isStraight(value[other]) {
            context.stroke(curvePath(value[other], in: size), with: .color(colour(other).opacity(0.35)),
                           lineWidth: 1)
        }
        context.stroke(curvePath(points, in: size), with: .color(colour(channel).opacity(0.95)), lineWidth: 1.5)

        for (i, p) in points.enumerated() {
            let c = location(of: p, in: size)
            let dot = Path(ellipseIn: CGRect(x: c.x - 4, y: c.y - 4, width: 8, height: 8))
            let held = i == dragging
            context.fill(dot, with: .color(held ? (removing ? .orange : colour(channel)) : .black.opacity(0.7)))
            context.stroke(dot, with: .color(removing && held ? .orange : colour(channel)), lineWidth: 1.2)
        }
    }

    private func curvePath(_ points: [CurvePoint], in size: CGSize) -> Path {
        let spline = PrintCurves.Spline(points)
        var path = Path()
        let steps = 160
        for i in 0...steps {
            let x = Double(i) / Double(steps)
            let at = location(of: CurvePoint(x: x, y: spline(x)), in: size)
            if i == 0 { path.move(to: at) } else { path.addLine(to: at) }
        }
        return path
    }

    // MARK: Editing

    /// Points closer than this in x would make the curve all but vertical.
    private let gap = 0.01

    private func drag(in size: CGSize) -> some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { g in
                if dragging == nil { dragging = grabOrAdd(at: g.startLocation, in: size) }
                guard let i = dragging else { return }
                move(i, to: g.location, in: size)
            }
            .onEnded { _ in
                if removing, let i = dragging {
                    var p = points
                    p.remove(at: i)
                    set(p)
                }
                dragging = nil
                removing = false
            }
    }

    /// The point within reach of `at`, or a new one there: on the curve if
    /// the click was close to it, so the curve holds until it is dragged.
    private func grabOrAdd(at: CGPoint, in size: CGSize) -> Int? {
        let p = points
        let near = p.indices.min { hypot(location(of: p[$0], in: size).x - at.x, location(of: p[$0], in: size).y - at.y)
            < hypot(location(of: p[$1], in: size).x - at.x, location(of: p[$1], in: size).y - at.y) }
        if let near, hypot(location(of: p[near], in: size).x - at.x, location(of: p[near], in: size).y - at.y) <= 9 {
            return near
        }
        var new = curvePoint(at: at, in: size)
        guard let after = p.firstIndex(where: { $0.x > new.x }), after > 0,
              new.x - p[after - 1].x >= gap, p[after].x - new.x >= gap else { return nil }
        let onCurve = PrintCurves.Spline(p)(new.x)
        if abs(location(of: CurvePoint(x: new.x, y: onCurve), in: size).y - at.y) <= 9 { new.y = onCurve }
        var edited = p
        edited.insert(new, at: after)
        set(edited)
        return after
    }

    private func move(_ i: Int, to at: CGPoint, in size: CGSize) {
        var p = points
        guard i < p.count else { return }
        let target = curvePoint(at: at, in: size)
        let low = i == 0 ? 0 : p[i - 1].x + gap
        let high = i == p.count - 1 ? 1 : p[i + 1].x - gap
        p[i] = CurvePoint(x: min(max(target.x, low), high), y: target.y)
        // An inner point dragged well off the graph is taken away on release.
        let r = plotRect(size)
        removing = i > 0 && i < p.count - 1 && !r.insetBy(dx: -18, dy: -18).contains(at)
        set(p)
    }

    private func set(_ edited: [CurvePoint]) {
        var c = value
        c[channel] = edited
        curves = c
    }

    private func straighten(_ channel: PrintCurves.Channel) {
        var c = value
        c[channel] = PrintCurves.straight
        curves = c.isIdentity ? nil : c
    }
}
