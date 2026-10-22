import SwiftUI

/// The paper's characteristic curve as the render will actually use it.
///
/// The samples come from `ToneCurve.density` — the same function that fills the
/// LUT the GPU reads — so the plot cannot drift out of step with the image.
/// The dashed line is the curve before any tone control, which makes it obvious
/// what each slider is doing to the shadows and highlights.
struct CurvePlotView: View {
    let paper: ProfilePack?
    let recipe: Recipe
    var height: CGFloat = 132

    private var samples: [(logExposure: Float, density: SIMD3<Float>)] {
        guard let paper else { return [] }
        return ToneCurve.plotSamples(paper: paper, recipe: recipe)
    }

    private var neutralSamples: [(logExposure: Float, density: SIMD3<Float>)] {
        guard let paper else { return [] }
        var base = Recipe()
        base.printStock = recipe.printStock
        return ToneCurve.plotSamples(paper: paper, recipe: base)
    }

    var body: some View {
        GeometryReader { geo in
            let s = samples
            let n = neutralSamples
            let dMax = max(paper?.densityMax.max() ?? 2.5, 0.5)
            ZStack {
                grid(in: geo.size, dMax: dMax)

                if !n.isEmpty && !recipe.toneIsNeutral {
                    path(for: n, channel: -1, size: geo.size, dMax: dMax)
                        .stroke(style: StrokeStyle(lineWidth: 1, dash: [3, 3]))
                        .foregroundStyle(.tertiary)
                }
                ForEach(0..<3, id: \.self) { c in
                    path(for: s, channel: c, size: geo.size, dMax: dMax)
                        .stroke(lineWidth: 1.3)
                        .foregroundStyle(channelColor(c))
                        .blendMode(.screen)
                }
            }
            .background(Color.black.opacity(0.28))
            .clipShape(RoundedRectangle(cornerRadius: 5))
        }
        .frame(height: height)
        .overlay(alignment: .topLeading) {
            Text("D")
                .font(.system(size: 8, design: .monospaced))
                .foregroundStyle(.tertiary).padding(4)
        }
        .overlay(alignment: .bottomTrailing) {
            Text("log E")
                .font(.system(size: 8, design: .monospaced))
                .foregroundStyle(.tertiary).padding(4)
        }
    }

    private func channelColor(_ c: Int) -> Color {
        [Color(red: 1, green: 0.35, blue: 0.35),
         Color(red: 0.35, green: 1, blue: 0.45),
         Color(red: 0.45, green: 0.6, blue: 1)][c]
    }

    /// `channel == -1` averages the three, for the neutral reference line.
    private func path(for samples: [(logExposure: Float, density: SIMD3<Float>)],
                      channel: Int, size: CGSize, dMax: Float) -> Path {
        var p = Path()
        guard samples.count > 1 else { return p }
        let x0 = samples.first!.logExposure
        let x1 = samples.last!.logExposure
        for (i, sample) in samples.enumerated() {
            let tx = CGFloat((sample.logExposure - x0) / max(x1 - x0, 1e-6))
            let d = channel < 0
                ? (sample.density.x + sample.density.y + sample.density.z) / 3
                : sample.density[channel]
            let ty = CGFloat(min(max(d / dMax, 0), 1.08))
            // Density increases downward: more density is a darker print.
            let point = CGPoint(x: tx * size.width, y: ty * size.height)
            if i == 0 { p.move(to: point) } else { p.addLine(to: point) }
        }
        return p
    }

    private func grid(in size: CGSize, dMax: Float) -> some View {
        Canvas { context, _ in
            var path = Path()
            for i in 1..<4 {
                let x = size.width * CGFloat(i) / 4
                path.move(to: CGPoint(x: x, y: 0)); path.addLine(to: CGPoint(x: x, y: size.height))
            }
            for i in 1..<4 {
                let y = size.height * CGFloat(i) / 4
                path.move(to: CGPoint(x: 0, y: y)); path.addLine(to: CGPoint(x: size.width, y: y))
            }
            context.stroke(path, with: .color(.white.opacity(0.06)), lineWidth: 0.5)
        }
    }
}
