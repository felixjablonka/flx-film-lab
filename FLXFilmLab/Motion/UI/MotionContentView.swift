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
        panel.message = "Choose where the \(count) clip\(count == 1 ? "" : "s") go, as \(exportCodecName(model.look.export.codec))"
        if panel.runModal() == .OK, let url = panel.url { model.exportSelected(to: url) }
    }

    private static func exportCodecName(_ codec: ExportCodec) -> String { codec.rawValue }
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

    struct CodecItem: View {
        @ObservedObject var model: ClipModel
        var body: some View {
            ToolbarChoice(selection: $model.look.export.codec)
                .help("The clip's format: ProRes 422 HQ for editing and grading further, HEVC 10-bit for sharing.")
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
