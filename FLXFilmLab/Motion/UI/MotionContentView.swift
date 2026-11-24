import SwiftUI
import AppKit
import UniformTypeIdentifiers

struct MotionContentView: View {
    @ObservedObject var model: ClipModel
    /// The folder's clips down the left (⌃⌘S), as the Stills mode's pictures.
    @AppStorage("showSidebar") private var showSidebar = true
    /// Settings' Text size: the side panels are made again when it changes.
    @AppStorage(PanelScale.key) private var textSize = PanelScale.standard.rawValue

    var body: some View {
        HStack(spacing: 0) {
            if showSidebar {
                FolderSidebar(browser: model.browser,
                              open: { model.open($0) },
                              openFolder: { MotionPanels.openFolder(model) },
                              flag: { model.flagSelection($0) },
                              flagOne: { model.flag($1, for: [$0]) },
                              pasteToSelected: { model.pasteLookToSelected() },
                              exportSelected: { MotionPanels.exportSelected(model) },
                              cancelBatch: { model.cancelExport() })
                    .equatable()
                    .id(textSize)
                Divider()
            }
            VStack(spacing: 0) {
                canvas
                TransportBar(playback: model.playback)
            }
            Divider()
            MotionInspector(model: model, onExport: chooseExport, onExportLUT: chooseLUTExport)
                .id(textSize)
        }
        .frame(minWidth: 1080, minHeight: 680)
        .background(Color(nsColor: .windowBackgroundColor))
        .background { HalfTitle(model.clip?.url.lastPathComponent ?? "FLX Film Lab") }
        .background { shortcuts }
        .sheet(isPresented: $model.showsDataRateSheet) {
            DataRateSheet(megabits: model.look.export.customMegabits,
                          recorded: model.clip.map { Double($0.dataRate) / 1_000_000 }) { chosen in
                if let chosen { model.setCustomRate(chosen) }
                model.showsDataRateSheet = false
            }
        }
    }

    // MARK: Canvas

    private var canvas: some View {
        ZStack {
            PreviewView(model: model)

            if model.clip == nil {
                VStack(spacing: 10) {
                    Image(systemName: "film")
                        .font(.system(size: 42, weight: .ultraLight))
                        .foregroundStyle(.tertiary)
                    Text("Drop a log clip here")
                        .font(.system(size: 14, weight: .medium)).foregroundStyle(.secondary)
                    Text("N-Log, V-Log, Apple Log or Apple Log 2, in any movie macOS can play")
                        .font(.system(size: 11)).foregroundStyle(.tertiary)
                    Button("Open…", action: chooseClip)
                        .buttonStyle(.borderedProminent).controlSize(.small).padding(.top, 6)
                }
                .colorScheme(model.surround == .white ? .light : .dark)
            }

            VStack {
                Spacer()
                if let error = model.errorMessage {
                    Text(error)
                        .font(.system(size: 11))
                        .padding(.horizontal, 12).padding(.vertical, 7)
                        .background(.red.opacity(0.85), in: Capsule())
                        .foregroundStyle(.white)
                        .onTapGesture { model.errorMessage = nil }
                        .padding(.bottom, 6)
                }
                if let progress = model.exportProgress {
                    exportBar(progress)
                } else {
                    statusBar
                }
            }
            .padding(.bottom, 10)
        }
    }

    private var statusBar: some View {
        HStack(spacing: 10) {
            Text(model.probeText ?? model.status)
            if model.drawMilliseconds > 0 && model.clip != nil && model.probeText == nil {
                Text(String(format: "%.0f ms", model.drawMilliseconds)).foregroundStyle(.tertiary)
            }
            if model.showLog {
                Text("log footage").foregroundStyle(.orange.opacity(0.9))
            }
            if model.isOpening {
                ProgressView().controlSize(.small).scaleEffect(0.55).frame(width: 12, height: 12)
            }
        }
        .font(.system(size: 10, design: .monospaced))
        .foregroundStyle(.secondary)
        .padding(.horizontal, 10).padding(.vertical, 5)
        .background(.ultraThinMaterial, in: Capsule())
        .colorScheme(model.surround == .white ? .light : .dark)
    }

    private func exportBar(_ progress: Double) -> some View {
        HStack(spacing: 10) {
            Text(model.status).font(.system(size: 10, design: .monospaced))
            ProgressView(value: progress).frame(width: 180)
            Text(String(format: "%.0f%%", progress * 100)).font(.system(size: 10, design: .monospaced))
            Button("Cancel") { model.cancelExport() }.controlSize(.small)
        }
        .foregroundStyle(.secondary)
        .padding(.horizontal, 12).padding(.vertical, 6)
        .background(.ultraThinMaterial, in: Capsule())
        .colorScheme(.dark)
    }

    // MARK: Keys

    /// Keyboard shortcuts with no menu item of their own.
    private var shortcuts: some View {
        // off while Stills, with a \ and a ⌘B of its own, is on screen
        HalfKeys {
            Button("") { model.playback.togglePlay() }.keyboardShortcut(.space, modifiers: [])
            Button("") { model.playback.step(-1) }.keyboardShortcut(.leftArrow, modifiers: [])
            Button("") { model.playback.step(1) }.keyboardShortcut(.rightArrow, modifiers: [])
            Button("") { model.showLog.toggle() }.keyboardShortcut("\\", modifiers: [])
            Button("") { model.surround = model.surround.next }.keyboardShortcut("b", modifiers: .command)
            Button("", action: chooseClip).keyboardShortcut("o", modifiers: .command)
            Button("", action: chooseExport).keyboardShortcut("e", modifiers: .command)
        }
        .opacity(0)
        .allowsHitTesting(false)
    }

    // MARK: Panels

    private func chooseClip() { MotionPanels.chooseClip(model) }

    private func chooseExport() { MotionPanels.chooseExport(model) }

    private func chooseLUTExport() {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [UTType(filenameExtension: "cube") ?? .data]
        panel.nameFieldStringValue = "\(model.printDescription.title).cube"
        if panel.runModal() == .OK, let url = panel.url { model.exportLUT(to: url) }
    }
}

/// The Motion mode's panels: Open and Export, from the toolbar, the keys and
/// the canvas, and the folder choosers of Open Folder and Export Selected.
@MainActor
enum MotionPanels {
    static func chooseClip(_ model: ClipModel) {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.movie]
        panel.allowsMultipleSelection = false
        panel.message = "Choose a log clip"
        if panel.runModal() == .OK, let url = panel.url { model.open(url) }
    }

    static func chooseExport(_ model: ClipModel) {
        guard model.clip != nil, !model.isExporting else { return }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.quickTimeMovie]
        panel.nameFieldStringValue = model.suggestedExportName
        panel.directoryURL = model.clip?.url.deletingLastPathComponent()
        if panel.runModal() == .OK, let url = panel.url { model.export(to: url) }
    }

    static func openFolder(_ model: ClipModel) {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.prompt = "Open Folder"
        panel.message = "Choose a folder of clips"
        if panel.runModal() == .OK, let url = panel.url { model.openFolder(url) }
    }

    static func exportSelected(_ model: ClipModel) {
        let count = model.browser.selectedForBatch.count
        guard count > 0, model.browser.batch == nil, !model.isExporting else { return }
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.prompt = "Export Here"
        panel.message = "Choose where the \(count) clip\(count == 1 ? "" : "s") go, as \(model.look.export.title)"
        if panel.runModal() == .OK, let url = panel.url { model.exportSelected(to: url) }
    }

}

// MARK: - The toolbar's Motion items

/// What the window's toolbar (`WindowToolbar`, FLXFilmLabApp.swift) shows for
/// Motion, item by item, in the places Stills' own (`StillsBar`) take in
/// Stills. Each watches only what it shows.
enum MotionBar {
    struct OpenItem: View {
        let model: ClipModel
        var body: some View {
            Button { MotionPanels.chooseClip(model) } label: { Label("Open", systemImage: "folder") }
                .help("Open a log clip (⌘O); the sidebar shows the clips of its folder. Open Folder (⇧⌘O) opens a folder.")
        }
    }

    /// Log footage, where Stills has the presets, Crop, Original and Loupe; in
    /// a stack, as they are.
    struct ViewItem: View {
        @ObservedObject var model: ClipModel
        var body: some View {
            HStack(spacing: 0) {
                Toggle(isOn: $model.showLog) { Label("Log footage", systemImage: "square.split.2x1") }
                    .help("Show the untouched log footage, as QuickTime shows it (\\)")
            }
        }
    }

    /// The clip's codec, and how hard it is compressed: each choice with the
    /// data rate it comes to for the clip on screen.
    struct CodecItem: View {
        @ObservedObject var model: ClipModel
        var body: some View {
            HStack(spacing: 6) {
                CodecChoice(export: $model.look.export)
                    .help("The clip's format: ProRes 422 for editing and grading further, HEVC 10-bit for sharing.")
                CompressionChoice(export: $model.look.export,
                                  clip: model.clip.map { (Int($0.size.width), Int($0.size.height), $0.frameRate) },
                                  setRate: { model.showsDataRateSheet = true })
                    .help("How hard the clip is compressed, and the data rate that comes to for the clip on screen. ProRes: Proxy, LT, 422 or HQ, each near Apple's target rate; HQ keeps all of the film's grain. HEVC: four levels from HEVC's own limits for the clip's size and frame rate. Standard is the most its HEVC level allows (100 Mbit/s for 4K up to 30 fps, 160 up to 60, 30 for HD at 30), High and Maximum the next two levels', Smaller half of Standard; at 4K 30 fps they keep about 50%, 60%, 70% and 75% of the grain. Or a rate of your own. Each file is labelled with the lowest HEVC level that holds it, as cameras label theirs. HEVC aims for the rate shown; a clip without grain often comes out smaller, and the status line says what it came to.")
            }
        }
    }

    struct ExportItem: View {
        @ObservedObject var model: ClipModel
        var body: some View {
            Button { MotionPanels.chooseExport(model) } label: { Label("Export", systemImage: "square.and.arrow.up") }
                .disabled(model.clip == nil || model.isExporting)
                .help("Write the graded clip (⌘E)")
        }
    }
}

/// HEVC's data rate of its own, typed in Mbit/s.
struct DataRateSheet: View {
    let recorded: Double?
    let done: (Double?) -> Void
    @State private var text: String

    init(megabits: Double, recorded: Double?, done: @escaping (Double?) -> Void) {
        self.recorded = recorded
        self.done = done
        _text = State(initialValue: megabits.rounded() == megabits ? String(Int(megabits)) : String(megabits))
    }

    /// The number typed, if it is a rate HEVC can be set to.
    private var chosen: Double? {
        Double(text.trimmingCharacters(in: .whitespaces).replacingOccurrences(of: ",", with: "."))
            .flatMap { ExportSettings.hevcMegabits.contains($0) ? $0 : nil }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Data Rate").font(.headline)
            HStack(spacing: 6) {
                TextField("", text: $text)
                    .frame(width: 90)
                    .multilineTextAlignment(.trailing)
                    .onSubmit { if let chosen { done(chosen) } }
                Text("Mbit/s")
            }
            Text(verbatim: "HEVC 10-bit at this average data rate, whatever the clip's size and frame rate. "
                 + (recorded.map { "The clip on screen was recorded at \(ExportSettings.rate($0)). " } ?? "")
                 + "Grain, new in every frame, needs a high rate to come through. From \(Int(ExportSettings.hevcMegabits.lowerBound)) to \(Int(ExportSettings.hevcMegabits.upperBound)) Mbit/s.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            HStack {
                Spacer()
                Button("Cancel") { done(nil) }
                    .keyboardShortcut(.cancelAction)
                Button("Set") { done(chosen) }
                    .keyboardShortcut(.defaultAction)
                    .disabled(chosen == nil)
            }
        }
        .padding(20)
        .frame(width: 360)
    }
}
