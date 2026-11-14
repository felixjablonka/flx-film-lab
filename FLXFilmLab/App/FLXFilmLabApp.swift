import SwiftUI
import AppKit
import QuartzCore
import UniformTypeIdentifiers

@main
struct FLXFilmLabApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    // Both halves' documents live as long as the app, so switching modes
    // keeps the open image and the open clip. They are not state of the app's
    // own: as that, every change of theirs, a frame or a slider tick, made
    // the app rebuild its window, toolbar and menus.
    private var stills: DocumentModel { Documents.stills }
    private var motion: ClipModel { Documents.motion }
    @AppStorage("mode") private var modeRaw = AppMode.stills.rawValue

    init() { Self.takeOverSettings() }

    /// Settings saved before the app took its name live under its previous
    /// identifier. The first launch under this one takes them over — the mode,
    /// which autos are on, the window and where the Open panel was — along with
    /// the clip look the standalone Motion app kept, if there is no newer one.
    /// The old copies are left as they were.
    private static func takeOverSettings() {
        let defaults = UserDefaults.standard
        guard let current = Bundle.main.bundleIdentifier,
              defaults.persistentDomain(forName: current)?.isEmpty ?? true else { return }
        var settings = defaults.persistentDomain(forName: "studio.agx.AgXStudio") ?? [:]
        if settings[ClipModel.lookDefaultsKey] == nil,
           let look = defaults.persistentDomain(forName: "studio.agx.AgXMotion")?[ClipModel.lookDefaultsKey] {
            settings[ClipModel.lookDefaultsKey] = look
        }
        guard !settings.isEmpty else { return }
        defaults.setPersistentDomain(settings, forName: current)
    }

    /// The half on screen. The switch, ⌘1 and ⌘2 and files opened from Finder
    /// all set it here.
    private var mode: Binding<AppMode> {
        Binding(get: { AppMode(rawValue: modeRaw) ?? .stills }, set: { new in
            // Without Core Animation's implicit animations for the rest of this
            // turn of the run loop: the toolbar's items are drawn inside a glass
            // container layer of AppKit's, and a layer SwiftUI took out of it as
            // their contents changed made Core Animation fade the whole toolbar
            // for 0.25 s, in four runs out of ten.
            CATransaction.setDisableActions(true)
            modeRaw = new.rawValue
        })
    }

    var body: some Scene {
        Window("FLX Film Lab", id: "main") {
            RootView(stills: stills, motion: motion, mode: mode)
                .preferredColorScheme(.dark)
                .onAppear { appDelegate.attach(stills: stills, motion: motion, mode: mode) }
        }
        .windowToolbarStyle(.unified(showsTitle: true))
        .commands {
            CommandGroup(replacing: .newItem) {
                // Both act on the half on screen.
                Button("Open Folder…") {
                    if mode.wrappedValue == .motion { MotionPanels.openFolder(motion) } else { FolderPanels.openFolder(stills) }
                }
                .keyboardShortcut("o", modifiers: [.command, .shift])
                Button("Export Selected…") {
                    if mode.wrappedValue == .motion {
                        MotionPanels.exportSelected(motion)
                    } else {
                        let format = UserDefaults.standard.string(forKey: "exportFormat").flatMap(Exporter.Format.init) ?? .tiff16
                        FolderPanels.exportSelected(stills, format: format)
                    }
                }
                .keyboardShortcut("e", modifiers: [.command, .shift])
            }
            UndoCommands(stills: stills.history, motion: motion.history, mode: mode.wrappedValue)
            CommandGroup(after: .toolbar) {
                Button("Stills") { mode.wrappedValue = .stills }
                    .keyboardShortcut("1", modifiers: .command)
                Button("Motion") { mode.wrappedValue = .motion }
                    .keyboardShortcut("2", modifiers: .command)
                Divider()
                Button("Show or Hide Sidebar") {
                    let shown = UserDefaults.standard.object(forKey: "showSidebar") as? Bool ?? true
                    UserDefaults.standard.set(!shown, forKey: "showSidebar")
                }
                .keyboardShortcut("s", modifiers: [.command, .control])
                // the lines of the masks and gradients on the picture; they still act
                Button("Show or Hide Mask Lines") {
                    UserDefaults.standard.set(!UserDefaults.standard.bool(forKey: "hideMaskLines"), forKey: "hideMaskLines")
                }
                .keyboardShortcut("h", modifiers: [.command, .shift])
                .disabled(mode.wrappedValue == .motion)
                Button("Previous in Folder") {
                    if mode.wrappedValue == .motion { motion.showNeighbour(-1) } else { stills.showNeighbour(-1) }
                }
                .keyboardShortcut(.upArrow, modifiers: .command)
                Button("Next in Folder") {
                    if mode.wrappedValue == .motion { motion.showNeighbour(1) } else { stills.showNeighbour(1) }
                }
                .keyboardShortcut(.downArrow, modifiers: .command)
                Divider()
            }
            CommandGroup(after: .appInfo) {
                Link("agx-emulsion / spektrafilm", destination: URL(string: "https://github.com/andreavolpato/spektrafilm")!)
                Link("NegPy", destination: URL(string: "https://github.com/marcinz606/NegPy")!)
            }
        }
        // ⌘,: how the app looks and works, the same for every picture and clip
        Settings {
            SettingsView(stills: stills, library: Documents.library)
        }
    }
}

/// Undo and Redo in the Edit menu (⌘Z and ⇧⌘Z), for the half on screen: each
/// names what it would take back or do again. A text field being typed in
/// keeps its own, as everywhere on the Mac, while it has typing to undo, and
/// so does a sheet.
struct UndoCommands: Commands {
    @ObservedObject var stills: EditHistory<DocumentModel.ImageEdits>
    @ObservedObject var motion: EditHistory<MotionLook>
    let mode: AppMode

    var body: some Commands {
        CommandGroup(replacing: .undoRedo) {
            let undoName = mode == .stills ? stills.undoName : motion.undoName
            let redoName = mode == .stills ? stills.redoName : motion.redoName
            Button(undoName.map { "Undo \($0)" } ?? "Undo") {
                if Self.textTakes(\.canUndo) {
                    NSApp.sendAction(Selector(("undo:")), to: nil, from: nil)
                } else if mode == .stills { stills.undo() } else { motion.undo() }
            }
            .keyboardShortcut("z", modifiers: .command)
            .disabled(undoName == nil)
            Button(redoName.map { "Redo \($0)" } ?? "Redo") {
                if Self.textTakes(\.canRedo) {
                    NSApp.sendAction(Selector(("redo:")), to: nil, from: nil)
                } else if mode == .stills { stills.redo() } else { motion.redo() }
            }
            .keyboardShortcut("z", modifiers: [.command, .shift])
            .disabled(redoName == nil)
        }
    }

    /// The key window's own undo has it: a sheet's, or a text field's while
    /// it has typing to undo (or redo).
    @MainActor
    private static func textTakes(_ can: KeyPath<UndoManager, Bool>) -> Bool {
        guard let window = NSApp.keyWindow else { return false }
        if window.sheetParent != nil { return true }
        guard let text = window.firstResponder as? NSText else { return false }
        return text.undoManager?[keyPath: can] ?? false
    }
}

/// The two halves' documents, made once, on first use.
@MainActor
private enum Documents {
    /// One library for both halves: each writes a folder's whole file, so two
    /// would write over each other's pictures and clips.
    static let library = ImageLibrary()
    static let stills = DocumentModel(library: library)
    static let motion = ClipModel(library: library)
}

/// The window: one half at a time, with the switch between them in the
/// toolbar's centre.
///
/// Both halves are made once and kept, the one off screen transparent, out of
/// the pointer's reach, and its keys, drops and focus let go (`Half`, and what
/// reads `halfShown`). Made again at every switch, the other half's sidebar,
/// canvas and inspector, and a toolbar of its own, held the main thread
/// 0.2-0.26 s, and the toolbar then faded in for a quarter of a second; kept,
/// a switch takes about 0.07 s and one frame. The toolbar is the window's, the
/// same items in both halves (`WindowToolbar`).
struct RootView: View {
    // Each half watches its own document; the window around them need not.
    let stills: DocumentModel
    let motion: ClipModel
    @Binding var mode: AppMode

    var body: some View {
        ZStack {
            ContentView(model: stills).modifier(Half(shown: mode == .stills))
            MotionContentView(model: motion).modifier(Half(shown: mode == .motion))
        }
        .toolbar { WindowToolbar(stills: stills, motion: motion, mode: $mode) }
        .onChange(of: mode) {
            // A clip keeps playing behind the stills half otherwise.
            if mode != .motion { motion.playback.pause() }
            // whatever had the keys in the half now hidden lets them go
            NSApp.keyWindow?.makeFirstResponder(nil)
        }
    }
}

/// One half of the window, on screen or kept behind the other.
private struct Half: ViewModifier {
    let shown: Bool

    func body(content: Content) -> some View {
        content
            .environment(\.halfShown, shown)
            .opacity(shown ? 1 : 0)
            .allowsHitTesting(shown)
            .accessibilityHidden(!shown)
            .zIndex(shown ? 1 : 0)
    }
}

private struct HalfShownKey: EnvironmentKey {
    static let defaultValue = true
}

extension EnvironmentValues {
    /// Whether the half a view is in is the one on screen: its keys, drops,
    /// title and focus are off while it is not. Not `disabled`, which would
    /// make every control in the half update at each switch.
    var halfShown: Bool {
        get { self[HalfShownKey.self] }
        set { self[HalfShownKey.self] = newValue }
    }
}

// What reads `halfShown`, each on its own, so a switch updates these and not
// the whole half around them.

/// The window's title, while its half is on screen.
struct HalfTitle: View {
    let text: String
    @Environment(\.halfShown) private var shown

    init(_ text: String) { self.text = text }

    var body: some View {
        if shown { Color.clear.navigationTitle(text) }
    }
}

/// A half's own keys (hidden buttons with shortcuts), off while it is hidden:
/// both halves have a \ and a ⌘B.
struct HalfKeys<Content: View>: View {
    @Environment(\.halfShown) private var shown
    @ViewBuilder let content: Content

    var body: some View {
        content.disabled(!shown)
    }
}

/// Files dropped on a half, while it is on screen.
struct HalfDrop: ViewModifier {
    let perform: ([NSItemProvider]) -> Bool
    @Environment(\.halfShown) private var shown

    func body(content: Content) -> some View {
        content.onDrop(of: shown ? [.fileURL] : [], isTargeted: nil, perform: perform)
    }
}

/// A sidebar lets the keys go when its half is hidden: ↑, ↓, P, X and U are
/// the other half's then.
struct HalfLetsGo: ViewModifier {
    var focused: FocusState<Bool>.Binding
    @Environment(\.halfShown) private var shown

    func body(content: Content) -> some View {
        content.onChange(of: shown) { if !shown { focused.wrappedValue = false } }
    }
}

/// The window's toolbar: the same items in Stills and Motion, each showing its
/// half's own (`StillsBar`, `MotionBar`), so a switch changes what some of them
/// show and nothing else. With a toolbar of each half's, SwiftUI took every
/// item out of the toolbar at every switch and put new ones in.
struct WindowToolbar: ToolbarContent {
    let stills: DocumentModel
    let motion: ClipModel
    @Binding var mode: AppMode

    var body: some ToolbarContent {
        ToolbarItem(placement: .navigation) { SidebarItem(mode: mode) }
        ToolbarItem(placement: .navigation) {
            Either(mode) { StillsBar.OpenItem(model: stills) } motion: { MotionBar.OpenItem(model: motion) }
        }
        ToolbarItem(placement: .navigation) { SurroundItem(mode: mode, motion: motion).equatable() }
        ToolbarItem(placement: .principal) {
            Picker("Mode", selection: $mode) {
                ForEach(AppMode.allCases) { m in
                    Label(m.rawValue, systemImage: m.symbol).tag(m)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .help("Stills: a RAW photo through film and paper. Motion: a log clip, with playback, clip export and LUTs. ⌘1 and ⌘2 switch.")
        }
        ToolbarItem {
            // each its own circle; the same two buttons in both halves
            ControlGroup {
                CopyButton(mode: mode, stills: stills, motion: motion)
                PasteButton(mode: mode, stills: stills, motion: motion)
            }
        }
        ToolbarItem {
            Either(mode) { StillsBar.ViewItem(model: stills) } motion: { MotionBar.ViewItem(model: motion) }
        }
        ToolbarItem {
            Either(mode) { StillsBar.FormatItem() } motion: { MotionBar.CodecItem(model: motion) }
        }
        ToolbarItem {
            Either(mode) { StillsBar.ExportItem(model: stills) } motion: { MotionBar.ExportItem(model: motion) }
        }
    }
}

/// A toolbar item's content for each half, inside one stack that stays: SwiftUI
/// names a toolbar item after the view at the top of its content, and an item
/// whose top view changed was taken out of the toolbar and a new one put in.
private struct Either<Stills: View, Motion: View>: View {
    let mode: AppMode
    let stills: () -> Stills
    let motion: () -> Motion

    init(_ mode: AppMode, @ViewBuilder stills: @escaping () -> Stills, @ViewBuilder motion: @escaping () -> Motion) {
        self.mode = mode
        self.stills = stills
        self.motion = motion
    }

    var body: some View {
        HStack(spacing: 0) {
            if mode == .stills { stills() } else { motion() }
        }
    }
}

/// Copy Settings or Copy Look (⇧⌘C): one button for both halves, what it
/// does and says following the mode. A control group whose buttons changed
/// with the mode was taken out of the toolbar and put back at each switch.
private struct CopyButton: View {
    let mode: AppMode
    @ObservedObject var stills: DocumentModel
    @ObservedObject var motion: ClipModel

    var body: some View {
        let isStills = mode == .stills
        Button { if isStills { stills.copySettings() } else { motion.copyLook() } } label: {
            Label(isStills ? "Copy Settings" : "Copy Look", systemImage: "doc.on.doc")
        }
        .keyboardShortcut("c", modifiers: [.command, .shift])
        .disabled(isStills ? !stills.hasImage || stills.isBusy : motion.clip == nil)
        .help(isStills
              ? "Copy this image's settings to paste onto another (⇧⌘C): film, paper, filter, every slider and auto, and white balance."
              : "Copy this clip's look to paste onto another (⇧⌘C): film, paper, every control and the effects.")
    }
}

/// Paste Settings or Paste Look (⇧⌘V), as `CopyButton`.
private struct PasteButton: View {
    let mode: AppMode
    @ObservedObject var stills: DocumentModel
    @ObservedObject var motion: ClipModel

    var body: some View {
        let isStills = mode == .stills
        Button { if isStills { stills.pasteSettings() } else { motion.pasteLook() } } label: {
            Label(isStills ? "Paste Settings" : "Paste Look", systemImage: "doc.on.clipboard")
        }
        .keyboardShortcut("v", modifiers: [.command, .shift])
        .disabled(isStills ? !stills.hasImage || stills.isBusy || stills.copiedSettings == nil
                           : motion.clip == nil || motion.copiedLook == nil)
        .help(isStills
              ? stills.copiedSettings.map {
                  "Paste the settings copied from \($0.source) (⇧⌘V). This image keeps its own masks, gradients and crop."
              } ?? "Copy an image's settings first (⇧⌘C), then paste them onto another."
              : motion.copiedLook.map {
                  "Paste the look copied from \($0.source) (⇧⌘V). This clip keeps how its own footage is read."
              } ?? "Copy a clip's look first (⇧⌘C), then paste it onto another; several at once from the sidebar.")
    }
}

/// Show or hide the sidebar: one setting for both halves.
private struct SidebarItem: View {
    let mode: AppMode
    @AppStorage("showSidebar") private var showSidebar = true

    var body: some View {
        Button { showSidebar.toggle() } label: { Label("Sidebar", systemImage: "sidebar.left") }
            .help("Show or hide the folder's \(mode == .stills ? "pictures" : "clips") (⌃⌘S)")
    }
}

/// The surround: one picker for both halves, whose surrounds are the same
/// three greys, each half keeping its own. A switch changes at most which one
/// is chosen, so the picker is never made again (it would be laid out at its
/// ideal width for a moment, as `SurroundPicker` says).
private struct SurroundItem: View, Equatable {
    let mode: AppMode
    @ObservedObject var motion: ClipModel
    @AppStorage("canvasBackground") private var stillsSurround = CanvasBackground.dark.rawValue

    nonisolated static func == (a: SurroundItem, b: SurroundItem) -> Bool {
        MainActor.assumeIsolated { a.mode == b.mode }
    }

    var body: some View {
        let chosen = mode == .stills ? stillsSurround : motion.surround.rawValue
        SurroundPicker(choices: CanvasBackground.allCases, selection: CanvasBackground(rawValue: chosen) ?? .dark,
                       symbol: \.symbol, help: StillsBar.surroundHelp,
                       choose: { [mode, motion] choice in
                           if mode == .stills {
                               stillsSurround = choice.rawValue
                           } else {
                               motion.surround = Surround(rawValue: choice.rawValue) ?? .dark
                           }
                       })
            .equatable()
    }
}

/// Files opened from Finder ("Open With", or dropped on the Dock icon): a
/// movie opens in motion, anything else in stills, and the window switches
/// to that half.
final class AppDelegate: NSObject, NSApplicationDelegate {
    @MainActor private var stills: DocumentModel?
    @MainActor private var motion: ClipModel?
    @MainActor private var mode: Binding<AppMode>?
    @MainActor private var pending: [URL] = []

    @MainActor
    func attach(stills: DocumentModel, motion: ClipModel, mode: Binding<AppMode>) {
        self.stills = stills
        self.motion = motion
        self.mode = mode
        pending.forEach(open)
        pending = []
    }

    @MainActor
    func application(_ application: NSApplication, open urls: [URL]) {
        guard let url = urls.first else { return }
        if stills == nil { pending = [url] } else { open(url) }
    }

    @MainActor
    private func open(_ url: URL) {
        // A folder opens in the half on screen, if it has that half's kind of
        // file; otherwise in the half that has.
        if (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true {
            let clips = !FolderBrowser.listing(url, kind: .clips).isEmpty
            let pictures = !FolderBrowser.listing(url, kind: .pictures).isEmpty
            let half: AppMode = mode?.wrappedValue == .motion ? (clips || !pictures ? .motion : .stills)
                                                               : (pictures || !clips ? .stills : .motion)
            mode?.wrappedValue = half
            if half == .motion { motion?.openFolder(url) } else { stills?.openFolder(url) }
            return
        }
        let target = AppMode.opening(url)
        mode?.wrappedValue = target
        switch target {
        case .stills: stills?.open(url: url)
        case .motion: motion?.open(url)
        }
    }
}
