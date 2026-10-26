import SwiftUI
import AppKit

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
            ResettingSlider(value: $value, range: range) {
                withAnimation(.easeOut(duration: 0.15)) { value = defaultValue }
            }
            .controlSize(.mini)
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
                        ResettingSlider(value: Binding(get: { trim[i] }, set: { trim[i] = $0 }), range: range) {
                            trim[i] = 0
                        }
                        .controlSize(.mini)
                        .tint(tints[i])
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
            ResettingSlider(value: binding, range: range) { binding.wrappedValue = 0 }
                .controlSize(.mini).tint(tint)
            Text(label).font(.system(size: 8, design: .monospaced)).foregroundStyle(.tertiary)
        }
    }
}

/// A slider that goes back to its neutral value on a double-click.
///
/// A slider tracks the mouse itself, and whether a SwiftUI gesture on it sees
/// the clicks depends on the SDK the app is built with: a tap gesture
/// alongside the slider's own worked built with the macOS 26 SDK, and not in
/// the 1.1.0 release, built with the macOS 15 SDK. Nor can an event monitor
/// find the slider: SwiftUI places the AppKit views inside a scroll view
/// where AppKit's own coordinates do not say where they are on screen. The
/// slider's own editing calls work with any of them: when editing starts on
/// the second click of a double-click, or ends on its mouse-up (AppKit counts
/// the clicks), the slider has taken both clicks as moves, and `reset` puts
/// it back once it ends. Either is enough, in case one is reported after a
/// later event.
struct ResettingSlider: View {
    @Binding var value: Double
    let range: ClosedRange<Double>
    let reset: () -> Void
    @State private var startedOnDoubleClick = false

    var body: some View {
        Slider(value: $value, in: range, onEditingChanged: { editing in
            let event = NSApp.currentEvent
            let isClick = event?.type == .leftMouseDown || event?.type == .leftMouseUp
            let doubleClick = isClick && (event?.clickCount ?? 0) >= 2
            if editing {
                startedOnDoubleClick = doubleClick
            } else if startedOnDoubleClick || doubleClick {
                startedOnDoubleClick = false
                reset()
            }
        })
    }
}

/// A toolbar control that shows its choice by name and lists the others when
/// clicked: a button that pops up a menu, the choice ticked.
///
/// A Menu or a Picker in the toolbar shows no text at all in an app built with
/// the macOS 26 SDK and run on macOS 27, as 1.1.1 was: a capsule with only its
/// chevron. A button's own label shows in either. The label is one text, the
/// chevron inside it: with an image beside the text, macOS 26 merged the
/// button with its neighbours into one capsule.
struct ToolbarChoice<Value: CaseIterable & Hashable & RawRepresentable>: View
    where Value.RawValue == String, Value.AllCases: RandomAccessCollection {
    @Binding var selection: Value
    var width: CGFloat = 130

    var body: some View {
        Button(action: popUp) {
            (Text(selection.rawValue + " ")
                + Text(Image(systemName: "chevron.down")).font(.system(size: 9, weight: .semibold)))
                .frame(width: width)
        }
    }

    /// The choices, the current one ticked and placed under the pointer, as a
    /// pop-up button places it.
    private func popUp() {
        let menu = NSMenu()
        let target = MenuTarget { selection = $0 }
        var current: NSMenuItem?
        for value in Value.allCases {
            let item = NSMenuItem(title: value.rawValue, action: #selector(MenuTarget<Value>.choose(_:)),
                                  keyEquivalent: "")
            item.target = target
            item.representedObject = value
            if value == selection {
                item.state = .on
                current = item
            }
            menu.addItem(item)
        }
        // Returns once the menu has closed, so `target` lives as long as it.
        if let event = NSApp.currentEvent, event.type == .leftMouseUp, let view = event.window?.contentView {
            menu.popUp(positioning: current, at: view.convert(event.locationInWindow, from: nil), in: view)
        } else {
            menu.popUp(positioning: current, at: NSEvent.mouseLocation, in: nil)
        }
    }
}

private final class MenuTarget<Value>: NSObject {
    let chosen: (Value) -> Void
    init(_ chosen: @escaping (Value) -> Void) { self.chosen = chosen }
    @objc func choose(_ item: NSMenuItem) {
        if let value = item.representedObject as? Value { chosen(value) }
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
