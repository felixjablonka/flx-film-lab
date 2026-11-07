import SwiftUI

/// The print at 100% (⌘L) under a square dragged over it: one pixel of the
/// export to one pixel of the screen. It stands over the part of the print it
/// shows, as a loupe on a light table does.
struct LoupeView: View {
    @ObservedObject var model: DocumentModel
    @ObservedObject var preview: PrintPreview
    /// Where the fitted print lies on the canvas.
    let print: CGRect
    @Environment(\.displayScale) private var displayScale
    /// Where in the loupe the drag took hold of it, so it moves with the
    /// pointer instead of jumping to centre on it.
    @State private var grab: CGSize?
    private let side: CGFloat = 300

    /// One pixel of the print to one of the screen: asks again when the
    /// screen's scale is not what the last request assumed.
    private func size() {
        let px = Int(side * displayScale)
        model.setLoupeSize(width: px, height: px)
    }

    var body: some View {
        let c = model.loupeCentre
        let at = CGPoint(x: print.minX + c.x * print.width, y: print.minY + c.y * print.height)
        ZStack {
            if let image = preview.loupeImage {
                Image(decorative: image, scale: displayScale)
                    .interpolation(.none)
            } else {
                Color.black.opacity(0.6)
            }
            if model.loupeLoading {
                VStack(spacing: 6) {
                    ProgressView().controlSize(.small)
                    Text("Reading the picture at full size…")
                        .font(.system(size: 11)).foregroundStyle(.white)
                }
            }
        }
        .frame(width: side, height: side)
        .background(Color.black.opacity(0.6))
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(.white.opacity(0.85), lineWidth: 1.5))
        .overlay(alignment: .bottomTrailing) {
            Text("100%")
                .font(.system(size: 10).monospacedDigit())
                .padding(.horizontal, 5).padding(.vertical, 2)
                .background(.black.opacity(0.5), in: Capsule())
                .foregroundStyle(.white)
                .padding(6)
        }
        .shadow(color: .black.opacity(0.35), radius: 8, y: 3)
        .contentShape(Rectangle())
        .position(at)
        .gesture(
            DragGesture(minimumDistance: 0, coordinateSpace: .named("canvas"))
                .onChanged { g in
                    let offset = grab ?? CGSize(width: g.startLocation.x - at.x, height: g.startLocation.y - at.y)
                    grab = offset
                    let p = CGPoint(x: g.location.x - offset.width, y: g.location.y - offset.height)
                    model.moveLoupe(to: CGPoint(x: (p.x - print.minX) / max(print.width, 1),
                                                y: (p.y - print.minY) / max(print.height, 1)))
                }
                .onEnded { _ in grab = nil }
        )
        .onAppear { size() }
        .onChange(of: displayScale) { size() }
        .help("The print at 100%, as the export will have it: grain, halation, sharpness and all. Drag it over the print; ⌘L puts it away.")
    }
}
