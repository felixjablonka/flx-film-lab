import SwiftUI
import AppKit

/// The folder's pictures down the left of the window, a small print and its
/// name beside it: click one to show it, ⌘- or ⇧-click to select several for
/// Paste Settings to Selected and Export Selected, ↑ and ↓ to step (with ⇧ to
/// extend the selection), ⌘A for all. P picks, X rejects and U unflags what is
/// selected; a reject is dimmed and left out of batch actions, never deleted.
///
/// Its own column rather than a List, so a selection is a thin accent line
/// round the print, not the system's heavy highlight.
///
/// It watches the browser and its thumbnails alone, never the document: a
/// slider dragged changes the document every tick, and must not lay the
/// sidebar out again. So it is equal to itself whatever its actions, which
/// always reach the same document, and ContentView keeps it `.equatable()`.
struct FolderSidebar: View, Equatable {
    nonisolated static func == (a: FolderSidebar, b: FolderSidebar) -> Bool {
        MainActor.assumeIsolated { a.browser === b.browser }
    }

    @ObservedObject var browser: FolderBrowser
    /// Where ⇧-click and ⇧↑↓ extend the selection from.
    @State private var anchor: URL?
    @FocusState private var focused: Bool
    let open: (URL) -> Void
    let openFolder: () -> Void
    let flag: (ImageLibrary.Flag?) -> Void
    /// A row's button on a picture not in the selection: that one alone.
    let flagOne: (URL, ImageLibrary.Flag?) -> Void
    let pasteToSelected: () -> Void
    let exportSelected: () -> Void
    let cancelBatch: () -> Void

    static var width: CGFloat { pt(236) }
    private var noun: String { browser.kind.noun }

    var body: some View {
        VStack(spacing: 0) {
            if let folder = browser.folder {
                FolderHeader(folder: folder, noun: noun, summary: summary, filter: browser.filter,
                             setFilter: { [browser] in browser.filter = $0 },
                             openFolder: openFolder)
                    .equatable()
                Divider()
                list
                footer
            } else {
                VStack(spacing: 12) {
                    Text("No folder open").foregroundStyle(.secondary)
                    Button("Open Folder…", action: openFolder)
                        .help("Show a folder's \(noun)s here (⇧⌘O): click one to work on it, select several to paste onto them or export them together.")
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .frame(width: Self.width)
    }

    private var summary: String {
        let shown = browser.shown.count, all = browser.items.count
        let picks = browser.flags.values.filter { $0 == .pick }.count
        let rejects = browser.flags.values.filter { $0 == .reject }.count
        var parts = [shown == all ? "\(all) \(noun)\(all == 1 ? "" : "s")" : "\(shown) of \(all)"]
        if picks > 0 { parts.append("\(picks) picked") }
        if rejects > 0 { parts.append("\(rejects) rejected") }
        return parts.joined(separator: " · ")
    }

    private var list: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(spacing: pt(2)) {
                    ForEach(browser.shown) { item in
                        SidebarTile(item: item, thumbnails: browser.thumbnails, flag: browser.flags[item.url],
                                    ownSettings: browser.edited.contains(item.url),
                                    selected: browser.selection.contains(item.url),
                                    current: browser.current == item.url,
                                    pasteName: browser.pasteName,
                                    setFlag: { f in
                                        // the selection, if the row is in it, else its picture
                                        // alone: the selection stays as it is
                                        if browser.selection.contains(item.url) { flag(f) } else { flagOne(item.url, f) }
                                    })
                            .id(item.url)
                            .contentShape(Rectangle())
                            .onTapGesture { click(item.url) }
                            .contextMenu { menu(for: item.url) }
                            .onAppear { browser.requestThumbnail(item.url) }
                    }
                }
                .padding(.horizontal, pt(8)).padding(.vertical, pt(6))
            }
            .focusable()
            .focusEffectDisabled()
            .focused($focused)
            // hidden with its half, it lets the keys go: ↑, ↓, P, X and U are the other half's
            .modifier(HalfLetsGo(focused: $focused))
            .onChange(of: browser.current) { _, url in
                if let url { proxy.scrollTo(url) }
            }
            .onAppear { if let url = browser.current { proxy.scrollTo(url) } }
            .onKeyPress(.upArrow) { step(-1) }
            .onKeyPress(.downArrow) { step(1) }
            .onKeyPress(characters: CharacterSet(charactersIn: "pxuPXUa")) { press in
                switch press.characters.lowercased() {
                case "a":
                    guard press.modifiers.contains(.command) else { return .ignored }
                    browser.selection = Set(browser.shown.map(\.url))
                case "p": flag(.pick)
                case "x": flag(.reject)
                default: flag(nil)
                }
                return .handled
            }
        }
    }

    /// A click shows the picture and selects it alone; with ⌘ it joins the
    /// selection or leaves it, with ⇧ the selection runs to it from the last
    /// picture clicked.
    private func click(_ url: URL) {
        focused = true
        let keys = NSEvent.modifierFlags
        if keys.contains(.command) {
            if browser.selection.contains(url), browser.selection.count > 1 {
                browser.selection.remove(url)
                if url == browser.current, let next = browser.shown.first(where: { browser.selection.contains($0.url) }) {
                    open(next.url)
                }
                return
            }
            browser.selection.insert(url)
        } else if keys.contains(.shift), let from = anchor ?? browser.current {
            browser.selection = Set(run(from: from, to: url))
            open(url)
            return
        } else {
            browser.selection = [url]
        }
        anchor = url
        open(url)
    }

    /// ↑ or ↓: the picture before or after the one on screen, alone, or with
    /// ⇧ added to the selection from the last picture clicked.
    private func step(_ offset: Int) -> KeyPress.Result {
        guard let from = browser.current ?? browser.shown.first?.url,
              let next = browser.neighbour(of: from, offset: offset) else { return .handled }
        if NSEvent.modifierFlags.contains(.shift) {
            browser.selection = Set(run(from: anchor ?? from, to: next))
        } else {
            browser.selection = [next]
            anchor = next
        }
        open(next)
        return .handled
    }

    /// The pictures listed from `a` to `b`, both included.
    private func run(from a: URL, to b: URL) -> [URL] {
        let urls = browser.shown.map(\.url)
        guard let i = urls.firstIndex(of: a), let j = urls.firstIndex(of: b) else { return [b] }
        return Array(urls[min(i, j)...max(i, j)])
    }

    /// A picture's menu acts on the selection if it is in it, otherwise on
    /// that picture alone. It shows the keys that do the same in the sidebar:
    /// P, X and U, ⌘A, and ⇧⌘E from anywhere.
    @ViewBuilder
    private func menu(for url: URL) -> some View {
        Button("Pick") { act(on: url) { flag(.pick) } }
            .keyboardShortcut("p", modifiers: [])
        Button("Reject") { act(on: url) { flag(.reject) } }
            .keyboardShortcut("x", modifiers: [])
        Button("Unflag") { act(on: url) { flag(nil) } }
            .keyboardShortcut("u", modifiers: [])
        Divider()
        Button("\(browser.pasteName) to Selected") { act(on: url, pasteToSelected) }
            .disabled(!browser.hasCopiedSettings)
        Button("Export Selected…") { act(on: url, exportSelected) }
            .keyboardShortcut("e", modifiers: [.command, .shift])
            .disabled(browser.batch != nil)
        Divider()
        Button("Select All") { browser.selection = Set(browser.shown.map(\.url)) }
            .keyboardShortcut("a", modifiers: .command)
        Button("Show in Finder") {
            let urls = browser.selection.contains(url) ? Array(browser.selection) : [url]
            NSWorkspace.shared.activateFileViewerSelecting(urls)
        }
    }

    private func act(on url: URL, _ action: () -> Void) {
        if !browser.selection.contains(url) { browser.selection = [url] }
        action()
    }

    @ViewBuilder
    private var footer: some View {
        if let batch = browser.batch {
            Divider()
            VStack(alignment: .leading, spacing: 4) {
                ProgressView(value: Double(batch.done), total: Double(max(batch.total, 1)))
                HStack(spacing: 6) {
                    Text("Exporting \(batch.done + 1) of \(batch.total): \(batch.current)")
                        .font(.panel(10)).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                    Spacer(minLength: 0)
                    Button("Stop", action: cancelBatch).controlSize(.panel(.small))
                        .help("Stop after the \(noun) being written now.")
                }
            }
            .padding(10)
        } else if browser.selection.count > 1 {
            let count = browser.selectedForBatch.count, rejects = browser.selectedRejects
            Divider()
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 6) {
                    Button(browser.pasteName, action: pasteToSelected)
                        .disabled(!browser.hasCopiedSettings || count == 0)
                        .help(browser.kind == .pictures
                              ? "Put the copied settings on each picture selected, as Paste Settings puts them on the one on screen: each keeps its own masks, gradients and crop."
                              : "Put the copied look on each clip selected, as Paste Look puts it on the one on screen: each keeps how its own footage is read.")
                    Button("Export \(count)…", action: exportSelected)
                        .disabled(count == 0)
                        .help(browser.kind == .pictures
                              ? "Print each picture selected at full size into a folder, one after another, each with its own settings; a picture never opened gets the look on screen, metered for itself."
                              : "Write each clip selected into a folder, one after another, each with its own look; a clip never opened gets the look on screen, metered for itself. In the format chosen in the toolbar.")
                }
                .controlSize(.panel(.small))
                if rejects > 0 {
                    Text("\(rejects) rejected, left out").font(.panel(10)).foregroundStyle(.secondary)
                }
            }
            .padding(10)
        }
    }
}

/// The sidebar's head: the folder, its menu, the filter and the count. A view
/// of its own, equal to itself while they are the same, so selecting or
/// opening a picture leaves it alone. Updated while a click or a key was being
/// handled, the segmented filter was laid out at its ideal width for a moment,
/// 15 pt wider than the sidebar, and the head jumped sideways and back.
struct FolderHeader: View, Equatable {
    let folder: URL
    let noun: String
    let summary: String
    let filter: FolderBrowser.Filter
    let setFilter: (FolderBrowser.Filter) -> Void
    let openFolder: () -> Void

    nonisolated static func == (a: FolderHeader, b: FolderHeader) -> Bool {
        MainActor.assumeIsolated {
            a.folder == b.folder && a.noun == b.noun && a.summary == b.summary && a.filter == b.filter
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: pt(6)) {
            HStack(spacing: pt(6)) {
                Image(systemName: "folder").font(.panel(12)).foregroundStyle(.secondary)
                Text(folder.lastPathComponent).font(.panel(12, weight: .semibold)).lineLimit(1)
                    .truncationMode(.middle)
                Spacer(minLength: 0)
                Menu {
                    Button("Open Folder…", action: openFolder)
                    Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([folder]) }
                } label: {
                    Image(systemName: "ellipsis.circle").font(.panel(13))
                }
                .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
                .help("Open another folder, or show this one in Finder.")
            }
            // Its own view too: a flag changes the count, not the filter.
            FilterPicker(filter: filter, noun: noun, setFilter: setFilter).equatable()
            Text(summary).font(.panel(10)).foregroundStyle(.secondary)
        }
        .padding(.horizontal, pt(10)).padding(.vertical, pt(8))
    }
}

/// All, Picks or No Rejects, updated only when the filter changes.
struct FilterPicker: View, Equatable {
    let filter: FolderBrowser.Filter
    let noun: String
    let setFilter: (FolderBrowser.Filter) -> Void

    nonisolated static func == (a: FilterPicker, b: FilterPicker) -> Bool {
        MainActor.assumeIsolated { a.filter == b.filter && a.noun == b.noun }
    }

    var body: some View {
        Picker("Show", selection: Binding(get: { filter }, set: setFilter)) {
            ForEach(FolderBrowser.Filter.allCases) { Text($0.rawValue).tag($0) }
        }
        .pickerStyle(.segmented).labelsHidden().controlSize(.panel(.small))
        .help("All the \(noun)s, only the picks, or all but the rejects.")
    }
}

/// One picture in the sidebar: a small print, and its name beside it in small
/// type, its flag at the end of the row. Selected, a thin accent line goes
/// round the print, a little stronger on the one on screen, whose name is in
/// full colour. With the pointer over it, the row shows a pick and a reject
/// button, which flag its picture, or the selection it is in, as P and X do,
/// and unflag it again. It reads its own thumbnail, so a new one redraws only
/// the rows on screen.
private struct SidebarTile: View {
    let item: FolderBrowser.Item
    @ObservedObject var thumbnails: ThumbnailStore
    let flag: ImageLibrary.Flag?
    let ownSettings: Bool
    let selected: Bool
    let current: Bool
    let pasteName: String
    let setFlag: (ImageLibrary.Flag?) -> Void
    @State private var hovering = false

    var body: some View {
        HStack(spacing: pt(8)) {
            picture
                .frame(width: pt(84), height: pt(56))
                .opacity(flag == .reject ? 0.4 : 1)
            HStack(spacing: pt(4)) {
                Text(item.name)
                    .font(.panel(10))
                    .foregroundStyle(current ? .primary : selected ? .secondary : .tertiary)
                    .lineLimit(1).truncationMode(.middle)
                if ownSettings {
                    Circle().fill(.tertiary).frame(width: pt(4), height: pt(4))
                        .help(thumbnails.kind == .clips ? "Has a look of its own."
                              : "Has settings of its own; its picture here is its print.")
                }
            }
            .opacity(flag == .reject ? 0.4 : 1)
            Spacer(minLength: 0)
            if hovering {
                HStack(spacing: pt(1)) {
                    flagButton(.pick)
                    flagButton(.reject)
                }
            } else if let flag {
                Image(systemName: flag == .pick ? "flag.fill" : "xmark")
                    .font(.panel(8, weight: .bold))
                    .foregroundStyle(flag == .pick ? Color.accentColor : Color.red.opacity(0.85))
                    .help(flag == .pick ? "Picked" : "Rejected: left out of \(pasteName) to Selected and Export Selected. U unflags it.")
            }
        }
        .padding(.vertical, pt(3)).padding(.horizontal, pt(4))
        .onHover { hovering = $0 }
    }

    /// Pick or reject this picture, or the selection it is in; again, unflag it.
    private func flagButton(_ kind: ImageLibrary.Flag) -> some View {
        let on = flag == kind
        return Button { setFlag(on ? nil : kind) } label: {
            Image(systemName: kind == .pick ? (on ? "flag.fill" : "flag") : "xmark")
                .font(.panel(9, weight: .bold))
                .foregroundStyle(on ? (kind == .pick ? Color.accentColor : Color.red.opacity(0.85)) : Color.secondary)
                .frame(width: pt(18), height: pt(18))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(kind == .pick
              ? (on ? "Picked: click to unflag it (U)" : "Pick (P)")
              : (on ? "Rejected: click to unflag it (U)"
                    : "Reject (X): left out of \(pasteName) to Selected and Export Selected, never deleted"))
    }

    @ViewBuilder
    private var picture: some View {
        let shape = RoundedRectangle(cornerRadius: 2, style: .continuous)
        if let image = thumbnails.images[item.url] {
            Image(decorative: image, scale: 1)
                .resizable().aspectRatio(contentMode: .fit)
                .clipShape(shape)
                .padding(2)
                .overlay(RoundedRectangle(cornerRadius: 4, style: .continuous)
                    .strokeBorder(Color.accentColor.opacity(current ? 1 : 0.6), lineWidth: current ? 1.5 : 1)
                    .opacity(selected ? 1 : 0))
        } else {
            shape.fill(.quaternary).padding(2)
        }
    }
}
