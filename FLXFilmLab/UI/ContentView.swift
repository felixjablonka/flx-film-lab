import SwiftUI
import UniformTypeIdentifiers

/// What surrounds the image on the canvas.
///
/// Brightness and colour are judged relative to their surround, not in
/// isolation — the same print reads dark against white and washed against
/// black. Being able to flip between the three standard surrounds is how you
/// tell a genuinely heavy print from one that only looks heavy.
///
/// This is a viewing preference. It is deliberately not part of `Recipe`, so it
/// never lands in a sidecar and can never change what is rendered.
enum CanvasBackground: String, CaseIterable, Identifiable {
    case dark = "Dark"
    case middleGrey = "Middle grey"
    case white = "White"

    var id: String { rawValue }

    var color: Color {
        switch self {
        // Not pure black: an absolute black surround makes everything in front
        // of it look milky.
        case .dark: return Color(white: 0.07)
        // L* 50 — the perceptual middle, and the neutral a print is normally
        // assessed against.
        case .middleGrey: return Color(white: 0.4663)
        case .white: return Color(white: 1.0)
        }
    }

    var isLight: Bool { self == .white }

    var symbol: String {
        switch self {
        case .dark: return "circle.fill"
        case .middleGrey: return "circle.lefthalf.filled"
        case .white: return "circle"
        }
    }

    var next: CanvasBackground {
        let all = CanvasBackground.allCases
        return all[(all.firstIndex(of: self)! + 1) % all.count]
    }
}

struct ContentView: View {
    @ObservedObject var model: DocumentModel
    @State private var selectedMask: UUID?
    /// The format Export and Export Selected write, kept between launches.
    @AppStorage("exportFormat") private var exportFormat: Exporter.Format = .tiff16
    /// The folder's pictures down the left (⌃⌘S).
    @AppStorage("showSidebar") private var showSidebar = true
    /// Settings' Text size: the side panels are made again when it changes.
    @AppStorage(PanelScale.key) private var textSize = PanelScale.standard.rawValue
    @State private var presetName = ""
    @AppStorage("canvasBackground") private var backgroundRaw = CanvasBackground.dark.rawValue
    /// The masks' and gradients' lines hidden, to judge the print without them.
    @AppStorage("hideMaskLines") private var hideMaskLines = false

    private var background: CanvasBackground {
        CanvasBackground(rawValue: backgroundRaw) ?? .dark
    }

    var body: some View {
        HStack(spacing: 0) {
            if showSidebar {
                FolderSidebar(browser: model.browser,
                              open: { model.open(url: $0) },
                              openFolder: { FolderPanels.openFolder(model) },
                              flag: { model.flagSelection($0) },
                              flagOne: { model.flag($1, for: [$0]) },
                              pasteToSelected: { model.pasteSettingsToSelected() },
                              exportSelected: { FolderPanels.exportSelected(model, format: exportFormat) },
                              cancelBatch: { model.cancelBatch() })
                    .equatable()
                    .id(textSize)
                Divider()
            }
            canvas
            Divider()
            InspectorView(model: model, selectedMask: $selectedMask)
                .id(textSize)
        }
        .frame(minWidth: 1080, minHeight: 700)
        .background(Color(nsColor: .windowBackgroundColor))
        .background { HalfTitle(model.fileURL?.lastPathComponent ?? "FLX Film Lab") }
        // A mask selected on one image means nothing on the next.
        .onChange(of: model.fileURL) { selectedMask = nil }
        // Hiding the lines puts editing down; choosing a mask or gradient to
        // edit, or adding one, takes it up again, lines and all.
        .onChange(of: hideMaskLines) { if hideMaskLines { selectedMask = nil } }
        .onChange(of: selectedMask) { if selectedMask != nil { hideMaskLines = false } }
        // A preset file may have been copied in, or taken away, in Finder.
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            model.reloadPresets()
        }
        .sheet(isPresented: $model.showsPresetNaming) {
            PresetNameSheet(name: $presetName, taken: Set(model.presets.map(\.name))) { save in
                if save { model.savePreset(named: presetName) }
                model.showsPresetNaming = false
            }
        }
        // a new preset is first named after the film
        .onChange(of: model.showsPresetNaming) { if model.showsPresetNaming { presetName = model.filmPack?.name ?? "" } }
        .fileImporter(isPresented: $model.showsOpenPanel,
                      allowedContentTypes: [.rawImage, .image, UTType(filenameExtension: "dng") ?? .image],
                      allowsMultipleSelection: false) { result in
            if case .success(let urls) = result, let url = urls.first {
                _ = url.startAccessingSecurityScopedResource()
                model.open(url: url)
            }
        }
        .fileExporter(isPresented: $model.showsExportPanel,
                      document: PlaceholderDocument(),
                      contentType: exportFormat.utType,
                      defaultFilename: exportFilename) { result in
            if case .success(let url) = result {
                Task { await model.export(to: url, format: exportFormat) }
            }
        }
        .background {
            // Invisible shortcut host: ⌘B cycles the surround, ⌘[ and ⌘] turn
            // the picture a quarter, \\ shows the original and back; off while
            // Motion, with a \\ and a ⌘B of its own, is on screen.
            HalfKeys {
                Button("") { if model.hasImage { model.showOriginal.toggle() } }
                    .keyboardShortcut("\\", modifiers: [])
                Button("") { backgroundRaw = background.next.rawValue }
                    .keyboardShortcut("b", modifiers: .command)
                Button("") { model.turn(clockwise: false) }
                    .keyboardShortcut("[", modifiers: .command)
                Button("") { model.turn(clockwise: true) }
                    .keyboardShortcut("]", modifiers: .command)
            }
            .opacity(0)
        }
        .modifier(HalfDrop { providers in
            guard let provider = providers.first else { return false }
            _ = provider.loadObject(ofClass: URL.self) { url, _ in
                guard let url else { return }
                Task { @MainActor in
                    // A folder dropped opens in the sidebar, a picture on its own.
                    if (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true {
                        model.openFolder(url)
                    } else {
                        model.open(url: url)
                    }
                }
            }
            return true
        })
    }

    private var exportFilename: String {
        let stem = model.fileURL?.deletingPathExtension().lastPathComponent ?? "FLX Film Lab"
        let film = model.filmPack?.name.replacingOccurrences(of: " ", with: "") ?? "film"
        return "\(stem)_\(film)"
    }

    // MARK: Canvas

    /// The print and what is drawn on it, and the status line under it. They
    /// watch the preview themselves, so a finished frame redraws them alone.
    private var canvas: some View {
        PrintCanvas(model: model, preview: model.preview, selectedMask: $selectedMask,
                    background: background, linesHidden: hideMaskLines, open: { model.showsOpenPanel = true })
            .overlay(alignment: .bottom) {
                StatusLine(model: model, preview: model.preview, background: background)
            }
    }

}

// MARK: - The toolbar's Stills items

/// What the window's toolbar (`WindowToolbar`, FLXFilmLabApp.swift) shows for
/// Stills, item by item, in the places Motion's own (`MotionBar`) take in
/// Motion. Each watches only what it shows; the panels they ask for are
/// ContentView's.
enum StillsBar {
    static let surroundHelp = "Surround: dark, middle grey or white. Brightness and colour are judged against their surround — the same print reads heavy against white and washed against black. ⌘B cycles."

    struct OpenItem: View {
        let model: DocumentModel
        var body: some View {
            Button { model.showsOpenPanel = true } label: { Label("Open", systemImage: "folder") }
                .help("Open a RAW file; the sidebar shows the pictures of its folder. Open Folder (⇧⌘O) opens a folder.")
        }
    }

    /// The presets, Crop, Original and Loupe, one item, where Motion has Log
    /// footage. (Copy and Paste, before them, are the window's: `CopyButton`.)
    struct ViewItem: View {
        @ObservedObject var model: DocumentModel
        var body: some View {
            HStack(spacing: 0) {
                presetsMenu
                Toggle(isOn: $model.isCropping) { Label("Crop", systemImage: "crop") }
                    .keyboardShortcut("r", modifiers: .command)
                    .disabled(!model.hasImage)
                    .help("Crop, turn and level the print (⌘R): the whole frame shows with the crop on it. Return keeps the crop, Escape goes back.")
                Toggle(isOn: $model.showOriginal) { Label("Original", systemImage: "square.split.2x1") }
                    .disabled(!model.hasImage)
                    .help("The picture before film and paper (\\): the RAW as macOS renders it, as shot, in the print's place. Again for the print.")
                Toggle(isOn: $model.loupeOn) { Label("Loupe", systemImage: "magnifyingglass") }
                    .keyboardShortcut("l", modifiers: .command)
                    .disabled(!model.hasImage)
                    .help("The print at 100% (⌘L), as the export will have it: drag the loupe over the print. While it is open the picture is held at full size.")
            }
        }

        /// Saved settings: choose one to put it on the image on screen.
        private var presetsMenu: some View {
            Menu {
                ForEach(model.presets) { preset in
                    Button(preset.name) { model.applyPreset(preset) }
                        .disabled(!model.hasImage || model.isBusy)
                }
                if model.presets.isEmpty {
                    Text("No presets yet")
                }
                Divider()
                Button("Save Settings as Preset…") { model.showsPresetNaming = true }
                    .disabled(!model.hasImage || model.isBusy)
                if !model.presets.isEmpty {
                    Menu("Delete Preset") {
                        ForEach(model.presets) { preset in
                            Button(preset.name) { model.deletePreset(preset) }
                        }
                    }
                }
                Button("Show Presets in Finder") {
                    if let folder = model.presetsFolder() { NSWorkspace.shared.open(folder) }
                }
            } label: {
                Label("Presets", systemImage: "film.stack")
            }
            .help("Presets: settings saved by name, to put on any image later, in this session or another. A preset holds what Copy Settings copies; the image it is put on keeps its own masks, gradients and crop.")
        }
    }

    struct FormatItem: View {
        @AppStorage("exportFormat") private var exportFormat: Exporter.Format = .tiff16
        var body: some View {
            ToolbarChoice(selection: $exportFormat)
                .help("The format Export writes.")
        }
    }

    struct ExportItem: View {
        @ObservedObject var model: DocumentModel
        var body: some View {
            Button { model.showsExportPanel = true } label: { Label("Export", systemImage: "square.and.arrow.up") }
                .disabled(!model.hasImage || model.isBusy)
                .help("Render at full resolution and write to disk. Waits until the image on screen has finished loading.")
        }
    }
}

/// The print on its surround, with the crop or the masks drawn on it and the
/// loupe over it.
private struct PrintCanvas: View {
    @ObservedObject var model: DocumentModel
    @ObservedObject var preview: PrintPreview
    @Binding var selectedMask: UUID?
    let background: CanvasBackground
    let linesHidden: Bool
    let open: () -> Void

    var body: some View {
        ZStack {
            background.color
                .animation(.easeInOut(duration: 0.18), value: background)

            if let print = preview.image {
                // the original in the print's place while it is shown, alone
                let original = model.showOriginal ? model.originalImage : nil
                let image = original ?? print
                GeometryReader { geo in
                    let fitted = fit(imageSize: CGSize(width: image.width, height: image.height),
                                     into: geo.size)
                    ZStack {
                        Image(decorative: image, scale: 1, orientation: .up)
                            .resizable()
                            .interpolation(.high)
                            .frame(width: fitted.width, height: fitted.height)
                            // Lighter contact shadow on a light surround, so the
                            // frame reads as an edge rather than a vignette.
                            .shadow(color: .black.opacity(background.isLight ? 0.22 : 0.5),
                                    radius: background.isLight ? 10 : 18, y: 5)

                        if original != nil {
                            EmptyView()
                        } else if model.isCropping {
                            CropOverlay(model: model, preview: preview, size: fitted,
                                        onLightBackground: background.isLight)
                        } else {
                            MaskOverlay(masks: $model.recipe.masks, gradients: $model.recipe.gradients,
                                        selected: $selectedMask,
                                        frame: CGRect(origin: .zero, size: fitted),
                                        map: preview.map,
                                        onLightBackground: background.isLight,
                                        linesHidden: linesHidden)
                                .frame(width: fitted.width, height: fitted.height)
                        }
                    }
                    .frame(width: geo.size.width, height: geo.size.height)
                    .overlay {
                        if model.loupeOn, !model.isCropping, original == nil {
                            LoupeView(model: model, preview: preview,
                                      print: CGRect(x: (geo.size.width - fitted.width) / 2,
                                                    y: (geo.size.height - fitted.height) / 2,
                                                    width: fitted.width, height: fitted.height))
                        }
                    }
                    .coordinateSpace(name: "canvas")
                }
                .padding(28)
                .overlay(alignment: .top) {
                    if original != nil {
                        Text("Original")
                            .font(.system(size: 11, weight: .semibold))
                            .padding(.horizontal, 10).padding(.vertical, 4)
                            .background(.ultraThinMaterial, in: Capsule())
                            .help("The RAW as macOS renders it, as shot, before film and paper. \\ or the toolbar's Original for the print.")
                            .padding(.top, 8)
                    } else if model.isCropping {
                        cropBar
                    }
                }
            } else {
                VStack(spacing: 10) {
                    Image(systemName: "camera.aperture")
                        .font(.system(size: 42, weight: .ultraLight))
                        .foregroundStyle(.tertiary)
                    Text("Drop a RAW file here")
                        .font(.system(size: 14, weight: .medium)).foregroundStyle(.secondary)
                    Text("Nikon NEF, Leica DNG, and the other formats macOS reads")
                        .font(.system(size: 11)).foregroundStyle(.tertiary)
                    Button("Open…", action: open)
                        .buttonStyle(.borderedProminent).controlSize(.small).padding(.top, 6)
                }
                .colorScheme(background.isLight ? .light : .dark)
            }

            if let error = model.errorMessage {
                VStack {
                    Spacer()
                    Text(error)
                        .font(.system(size: 11))
                        .padding(.horizontal, 12).padding(.vertical, 7)
                        .background(.red.opacity(0.85), in: Capsule())
                        .foregroundStyle(.white)
                        .padding(.bottom, 14)
                }
            }
        }
    }

    /// While cropping: what the drags do, and the way out.
    private var cropBar: some View {
        HStack(spacing: 10) {
            Text(model.isLevelling
                 ? "Draw a line along something that should be level or upright"
                 : "Drag a corner or an edge · drag inside to move")
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
            Toggle(isOn: $model.isLevelling) { Label("Level", systemImage: "level") }
                .toggleStyle(.button).controlSize(.small)
                .help("Draw a line on the picture along a horizon, or along an upright, and the picture is levelled to it.")
            Button("Cancel") { model.endCropping(keep: false) }
                .keyboardShortcut(.cancelAction).controlSize(.small)
            Button("Done") { model.endCropping(keep: true) }
                .keyboardShortcut(.defaultAction).controlSize(.small)
        }
        .padding(.horizontal, 12).padding(.vertical, 6)
        .background(.ultraThinMaterial, in: Capsule())
        .colorScheme(background.isLight ? .light : .dark)
        .padding(.top, 8)
    }

    private func fit(imageSize: CGSize, into container: CGSize) -> CGSize {
        guard imageSize.width > 0, imageSize.height > 0 else { return .zero }
        let scale = min(container.width / imageSize.width, container.height / imageSize.height)
        return CGSize(width: imageSize.width * scale, height: imageSize.height * scale)
    }
}

/// What the image on screen is, and whether a frame or a decode is on its way.
private struct StatusLine: View {
    @ObservedObject var model: DocumentModel
    @ObservedObject var preview: PrintPreview
    let background: CanvasBackground

    var body: some View {
        HStack(spacing: 10) {
            Text(model.status)
            if model.isBusy && model.isDecoding {
                Text("decoding…").foregroundStyle(.orange.opacity(0.9))
            }
            // Its place kept whether it turns or not, so the line does not
            // change width with every frame.
            ProgressView().controlSize(.small).scaleEffect(0.55).frame(width: 12, height: 12)
                .opacity(preview.isRendering || (model.isBusy && model.isDecoding) ? 1 : 0)
        }
        .font(.system(size: 10, design: .monospaced))
        .foregroundStyle(.secondary)
        .padding(.horizontal, 10).padding(.vertical, 5)
        .background(.ultraThinMaterial, in: Capsule())
        .colorScheme(background.isLight ? .light : .dark)
        .padding(.bottom, 10)
    }
}

/// The name for a new preset, and whether it replaces one, as it is typed.
private struct PresetNameSheet: View {
    @Binding var name: String
    /// The presets' names.
    let taken: Set<String>
    let done: (_ save: Bool) -> Void

    private var trimmed: String { name.trimmingCharacters(in: .whitespacesAndNewlines) }
    private var isEmpty: Bool { trimmed.isEmpty }
    private var replaces: Bool { taken.contains(trimmed) }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Save Settings as Preset").font(.headline)
            Text("Film, paper, print or light table, camera filter, every slider and auto, and white balance, to put on any image later. Masks, gradients and the crop stay with their image.")
                .font(.system(size: 11)).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            TextField("Name", text: $name)
                .textFieldStyle(.roundedBorder)
                .onSubmit { if !isEmpty { done(true) } }
            Text(replaces ? "Replaces the preset of that name." : " ")
                .font(.system(size: 10)).foregroundStyle(.orange)
            HStack {
                Spacer()
                Button("Cancel") { done(false) }
                    .keyboardShortcut(.cancelAction)
                Button(replaces ? "Replace" : "Save") { done(true) }
                    .keyboardShortcut(.defaultAction)
                    .disabled(isEmpty)
            }
        }
        .padding(20)
        .frame(width: 360)
    }
}

/// `fileExporter` needs a document type; the real write happens in the
/// completion handler at full resolution, so this carries no data.
private struct PlaceholderDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.tiff, .jpeg, .png] }
    init() {}
    init(configuration: ReadConfiguration) throws {}
    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        FileWrapper(regularFileWithContents: Data())
    }
}

/// The folder choosers of Open Folder and Export Selected, for the toolbar,
/// the sidebar and the menus alike.
@MainActor
enum FolderPanels {
    static func openFolder(_ model: DocumentModel) {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.prompt = "Open Folder"
        panel.message = "Choose a folder of pictures"
        if panel.runModal() == .OK, let url = panel.url { model.openFolder(url) }
    }

    static func exportSelected(_ model: DocumentModel, format: Exporter.Format) {
        let count = model.browser.selectedForBatch.count
        guard count > 0, model.browser.batch == nil else { return }
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.prompt = "Export Here"
        panel.message = "Choose where the \(count) print\(count == 1 ? "" : "s") go, as \(format.rawValue)"
        if panel.runModal() == .OK, let url = panel.url { model.exportSelected(to: url, format: format) }
    }
}

/// The surround in the toolbar: three symbols in a segmented control. A view
/// of its own, equal to itself while the choice is, so the toolbar's being
/// built again at every change to the document leaves it alone. Updated while
/// a click or a key was being handled, it was laid out at its ideal width for
/// a moment and its symbols shifted sideways and back.
struct SurroundPicker<Choice: Hashable & Identifiable>: View, Equatable {
    let choices: [Choice]
    let selection: Choice
    let symbol: (Choice) -> String
    let help: String
    let choose: (Choice) -> Void

    nonisolated static func == (a: SurroundPicker, b: SurroundPicker) -> Bool {
        MainActor.assumeIsolated { a.choices == b.choices && a.selection == b.selection && a.help == b.help }
    }

    var body: some View {
        Picker("", selection: Binding(get: { selection }, set: choose)) {
            ForEach(choices) { Image(systemName: symbol($0)).tag($0) }
        }
        .pickerStyle(.segmented).labelsHidden().frame(width: 108)
        .help(help)
    }
}
