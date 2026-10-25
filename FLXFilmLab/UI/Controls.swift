import SwiftUI

/// A labelled slider with a numeric readout, a tick at its default, and
/// double-click-to-reset. Every tone control in the inspector is one of these,
/// so "back to neutral" is always one gesture away.
struct ParamSlider: View {
    let title: String
    @Binding var value: Double
    var range: ClosedRange<Double>
    var defaultValue: Double = 0
    var decimals: Int = 2
    var unit: String = ""
    var help: String = ""

    @State private var isHovering = false

    private var isModified: Bool { abs(value - defaultValue) > 1e-9 }

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 4) {
                Text(title)
                    .font(.system(size: 11))
                    .foregroundStyle(isModified ? Color.primary : Color.secondary)
                if isModified {
                    Circle().frame(width: 3, height: 3).foregroundStyle(Color.accentColor)
                }
                Spacer(minLength: 4)
                // + 0 turns the -0 of a flipped scale (0 / -0.4) into 0.
                Text(String(format: "%@%.\(decimals)f%@", value > 0 && defaultValue == 0 ? "+" : "", value + 0, unit))
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .contentTransition(.numericText())
            }
            Slider(value: $value, in: range)
                .controlSize(.mini)
                .resetOnDoubleClick { withAnimation(.easeOut(duration: 0.15)) { value = defaultValue } }
        }
        .help(help.isEmpty ? "Double-click the slider to reset" : help + "\n\nDouble-click the slider to reset.")
        .padding(.vertical, 1)
    }
}

/// Collapsible inspector section.
struct Section<Content: View>: View {
    let title: String
    var subtitle: String? = nil
    @State private var expanded: Bool
    @ViewBuilder let content: () -> Content

    init(_ title: String, subtitle: String? = nil, expanded: Bool = true,
         @ViewBuilder content: @escaping () -> Content) {
        self.title = title
        self.subtitle = subtitle
        self._expanded = State(initialValue: expanded)
        self.content = content
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Button {
                withAnimation(.easeOut(duration: 0.16)) { expanded.toggle() }
            } label: {
                HStack(spacing: 5) {
                    Image(systemName: "chevron.right")
                        .font(.system(size: 8, weight: .bold))
                        .rotationEffect(.degrees(expanded ? 90 : 0))
                        .foregroundStyle(.tertiary)
                    Text(title.uppercased())
                        .font(.system(size: 10, weight: .semibold))
                        .tracking(0.6)
                        .foregroundStyle(.secondary)
                    Spacer()
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .padding(.top, 10)
            .padding(.bottom, expanded ? 6 : 2)

            if expanded {
                if let subtitle {
                    Text(subtitle)
                        .font(.system(size: 10))
                        .foregroundStyle(.tertiary)
                        .padding(.bottom, 6)
                        .fixedSize(horizontal: false, vertical: true)
                }
                VStack(alignment: .leading, spacing: 5) { content() }
            }
            Divider().opacity(0.4).padding(.top, 8)
        }
    }
}

/// Global / R / G / B selector for the per-layer trims.
struct ChannelTrimRow: View {
    let title: String
    @Binding var trim: RGBTrim
    var range: ClosedRange<Double>
    var help: String = ""

    private let names = ["R", "G", "B"]
    private let tints: [Color] = [.red, .green, .blue]

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title)
                .font(.system(size: 11))
                .foregroundStyle(trim.isZero ? Color.secondary : Color.primary)
            HStack(spacing: 6) {
                ForEach(0..<3, id: \.self) { i in
                    VStack(spacing: 1) {
                        Slider(value: Binding(get: { trim[i] }, set: { trim[i] = $0 }), in: range)
                            .controlSize(.mini)
                            .tint(tints[i])
                            .resetOnDoubleClick { trim[i] = 0 }
                        Text(names[i])
                            .font(.system(size: 8, design: .monospaced))
                            .foregroundStyle(.tertiary)
                    }
                }
            }
        }
        .help(help)
        .padding(.vertical, 1)
    }
}

struct CMYRow: View {
    let title: String
    @Binding var offset: CMYOffset
    var range: ClosedRange<Double> = -20...20
    var help: String = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title)
                .font(.system(size: 11))
                .foregroundStyle(offset.isZero ? Color.secondary : Color.primary)
            HStack(spacing: 6) {
                trim("C", .cyan, Binding(get: { offset.cyan }, set: { offset.cyan = $0 }))
                trim("M", Color(red: 1, green: 0.2, blue: 0.7), Binding(get: { offset.magenta }, set: { offset.magenta = $0 }))
                trim("Y", .yellow, Binding(get: { offset.yellow }, set: { offset.yellow = $0 }))
            }
        }
        .help(help)
        .padding(.vertical, 1)
    }

    private func trim(_ label: String, _ tint: Color, _ binding: Binding<Double>) -> some View {
        VStack(spacing: 1) {
            Slider(value: binding, in: range).controlSize(.mini).tint(tint)
                .resetOnDoubleClick { binding.wrappedValue = 0 }
            Text(label).font(.system(size: 8, design: .monospaced)).foregroundStyle(.tertiary)
        }
    }
}

extension View {
    /// Runs `reset` on a double-click. A macOS slider handles its own clicks, so
    /// a plain tap gesture on it never sees the second one; recognised
    /// alongside the slider's own, it does. The two clicks still move the
    /// slider first, and the reset then puts it back.
    func resetOnDoubleClick(_ reset: @escaping () -> Void) -> some View {
        simultaneousGesture(TapGesture(count: 2).onEnded(reset))
    }
}

struct ToggleRow: View {
    let title: String
    @Binding var isOn: Bool
    var help: String = ""

    var body: some View {
        Toggle(isOn: $isOn) {
            Text(title).font(.system(size: 11))
        }
        .toggleStyle(.switch)
        .controlSize(.mini)
        .help(help)
    }
}

/// A slider driven by a `ControlSpec`: presented in the chosen vocabulary,
/// stored as the physical parameter.
struct SpecSlider: View {
    let spec: ControlSpec
    @Binding var physical: Double
    let vocabulary: Vocabulary
    var defaultPhysical: Double = 0

    var body: some View {
        ParamSlider(title: spec.name(vocabulary),
                    value: spec.binding($physical),
                    range: spec.range,
                    defaultValue: spec.fromPhysical(defaultPhysical),
                    decimals: spec.decimals,
                    unit: spec.unit,
                    help: spec.help(vocabulary, physical: physical))
    }
}

/// A film or paper picker with its stocks grouped by kind
/// (`PackIndexEntry.groups`): colour negative, motion picture, colour slide and
/// black and white for films; colour, motion picture print film and black and
/// white for papers.
struct StockPicker: View {
    let title: String
    let entries: [PackIndexEntry]
    var films = true
    @Binding var selection: String

    var body: some View {
        LabeledContent {
            Picker("", selection: $selection) {
                ForEach(PackIndexEntry.groups(entries, films: films), id: \.title) { group in
                    SwiftUI.Section(group.title) {
                        ForEach(group.entries) { entry in
                            Text(entry.name).tag(entry.stock)
                        }
                    }
                }
            }
            .labelsHidden().controlSize(.small)
        } label: {
            Text(title).font(.system(size: 11)).foregroundStyle(.secondary)
        }
    }
}

/// The camera filters a stock carries (`ProfilePack.lensFilters`): Kodak's
/// Wratten filters on a black-and-white film, Tiffen's warming filters on a
/// slide. Each shows how far it holds back a mid-grey.
struct LensFilterPicker: View {
    let filters: [LensFilterRef]
    @Binding var selection: String?

    var body: some View {
        Picker("Filter", selection: $selection) {
            Text("None").tag(String?.none)
            ForEach(filters) { filter in
                Text("\(filter.designation) · \(Self.factor(filter.factor))")
                    .tag(String?.some(filter.id))
            }
        }
        .controlSize(.small).font(.system(size: 11))
        .help(filters.contains { $0.wratten != nil }
              ? "A coloured filter on the lens, measured from Kodak's Wratten data. It passes its own colour and holds back the rest: yellow, orange and red darken a blue sky progressively; red lightens skin and darkens foliage; green does the opposite; blue lightens skies and haze and deepens reds. Exposure is opened up by the filter factor shown, as a photographer would, so a grey card prints the same."
              : "A warming filter on the lens, measured from Tiffen's own transmission curves, for slide film's cool shade, overcast light and flash. The 81C holds back blue and some green, a plain amber warmth; the 812 holds back violet and a little blue and lets blue and green through alike, so its warmth leans pink. Exposure is opened up by the filter factor shown, as a photographer would, so the picture keeps its brightness.")
    }

    /// "8.2×, 3.0 stops": how much the filter holds back from a mid-grey.
    static func factor(_ factor: Float) -> String {
        String(format: "%.1f×, %.1f stops", factor, log2(max(factor, 1)))
    }
}
