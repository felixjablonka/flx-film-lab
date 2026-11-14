import SwiftUI

/// The app's own settings (⌘,): how it looks and works, the same for every
/// picture and clip. What belongs to a picture, its output colour space among
/// it, stays in the inspector and travels with its settings and presets.
/// Two tabs: General, and Films, which the Film menu lists.
struct SettingsView: View {
    @ObservedObject var stills: DocumentModel
    let library: ImageLibrary

    var body: some View {
        TabView {
            GeneralSettings(stills: stills, library: library)
                .tabItem { Label("General", systemImage: "gearshape") }
            FilmSettings()
                .tabItem { Label("Films", systemImage: "film.stack") }
        }
        .preferredColorScheme(.dark)
    }
}

private func note(_ text: String) -> some View {
    Text(text)
        .font(.system(size: 11))
        .foregroundStyle(.secondary)
        .fixedSize(horizontal: false, vertical: true)
}

private struct GeneralSettings: View {
    @ObservedObject var stills: DocumentModel
    let library: ImageLibrary
    @AppStorage(PanelScale.key) private var textSize = PanelScale.standard.rawValue
    @AppStorage("vocabulary") private var vocabularyRaw = Vocabulary.photographic.rawValue
    @AppStorage(ImageLibrary.sidecarsKey) private var savesSidecars = false

    var body: some View {
        Form {
            SwiftUI.Section("Interface") {
                Picker("Text size", selection: $textSize) {
                    ForEach(PanelScale.allCases) { Text($0.rawValue).tag($0.rawValue) }
                }
                .pickerStyle(.segmented)
                note("The inspector and the sidebar, larger as a whole: their text, controls and width. The toolbar keeps the system's size.")
                Picker("Control names", selection: $vocabularyRaw) {
                    ForEach(Vocabulary.allCases) { Text($0.rawValue).tag($0.rawValue) }
                }
                note("The Stills sliders. Photographic uses the names Capture One and Lightroom use, where positive always means brighter. Darkroom shows the underlying print controls. Either way the tooltip names the other.")
            }
            SwiftUI.Section("Preview") {
                Picker("Preview size", selection: $stills.proxyMaxDimension) {
                    ForEach(DocumentModel.previewSizes, id: \.self) { Text(verbatim: "\($0) px").tag($0) }
                }
                note("The Stills preview's long side. Larger is sharper on screen and slower to redraw: on an M4 Pro a print control redraws 2560 px in about 29 ms and 4096 px in about 73. A file smaller than this is shown at its own size, never enlarged, and export is always at full size.")
            }
            SwiftUI.Section("Library") {
                Toggle("Save settings beside images", isOn: $savesSidecars)
                note("Each picture's settings, or clip's look, and its flag also go into a small file beside it, DSC_0871.NEF.flx, which travels with the folder to another drive, another Mac or the Windows app. They are always kept in the app's library too; of the two, the newer counts.")
            }
        }
        .formStyle(.grouped)
        .frame(width: 500)
        .fixedSize(horizontal: false, vertical: true)
        // turned on, the records already kept are written beside their files too
        .onChange(of: savesSidecars) { _, on in
            if on { library.savesSidecars = true }
        }
    }
}

/// Which films the Film menu lists, in Stills and Motion: each film ticked or
/// not, in the menu's own groups and order. A film left out still works: a
/// picture, clip or preset that uses it opens with it, and it stays in the
/// menu while in use (`FilmMenu.shown`).
struct FilmSettings: View {
    @AppStorage(FilmMenu.hiddenKey) private var hiddenStored = ""
    private let groups = PackIndexEntry.groups(PackLibrary.shared.filmStocks, films: true)

    private var hidden: Set<String> { FilmMenu.hidden(hiddenStored) }

    var body: some View {
        let all = groups.flatMap(\.entries)
        let shown = all.filter { !hidden.contains($0.stock) }.count
        Form {
            SwiftUI.Section {
                note("The films the Film menu lists, in Stills and Motion. A film left out keeps working: a picture, clip or preset that uses it opens with it, and the menu shows it while it is in use.")
                HStack {
                    Text("\(shown) of \(all.count) films shown").foregroundStyle(.secondary)
                    Spacer()
                    Button("Show All") { hiddenStored = "" }
                        .disabled(hidden.isEmpty)
                }
            }
            ForEach(groups, id: \.title) { group in
                SwiftUI.Section {
                    // two columns, in the menu's order down each
                    let half = (group.entries.count + 1) / 2
                    HStack(alignment: .top, spacing: 16) {
                        ForEach([Array(group.entries.prefix(half)), Array(group.entries.dropFirst(half))], id: \.first?.stock) {
                            column in
                            VStack(alignment: .leading, spacing: 7) {
                                ForEach(column) { entry in
                                    Toggle(entry.name, isOn: isShown(entry.stock))
                                        .toggleStyle(.checkbox)
                                }
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }
                    .padding(.vertical, 2)
                } header: {
                    HStack {
                        Text(group.title)
                        Spacer()
                        let every = group.entries.allSatisfy { !hidden.contains($0.stock) }
                        Button(every ? "Hide All" : "Show All") { setShown(group.entries, !every) }
                            .buttonStyle(.link)
                            .font(.system(size: 11))
                    }
                }
            }
        }
        .formStyle(.grouped)
        .frame(width: 600, height: 600)
    }

    private func isShown(_ stock: String) -> Binding<Bool> {
        Binding(get: { !hidden.contains(stock) },
                set: { show in
                    var set = hidden
                    if show { set.remove(stock) } else { set.insert(stock) }
                    hiddenStored = FilmMenu.stored(set)
                })
    }

    private func setShown(_ entries: [PackIndexEntry], _ show: Bool) {
        var set = hidden
        for entry in entries {
            if show { set.remove(entry.stock) } else { set.insert(entry.stock) }
        }
        hiddenStored = FilmMenu.stored(set)
    }
}
