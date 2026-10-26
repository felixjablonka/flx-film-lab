import SwiftUI

/// The grade: film and paper, the print controls the Stills mode has, Glow's
/// effects, and how footage is read and written. The sliders are the Stills
/// mode's own, so each one names the darkroom quantity it drives in its tooltip.
struct MotionInspector: View {
    @ObservedObject var model: ClipModel
    var onExport: () -> Void
    var onExportLUT: () -> Void

    private let vocabulary = Vocabulary.photographic

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                stockSection
                exposureSection
                toneSection
                colourSection
                effectsSection
                inputSection
                exportSection
            }
            .padding(.horizontal, 12)
            .padding(.bottom, 24)
        }
        .frame(width: 296)
        // Not under the toolbar, as in Stills.
        .background(.ultraThinMaterial, ignoresSafeAreaEdges: [])
    }

    private var film: ProfilePack? { try? PackLibrary.shared.pack(model.look.recipe.filmStock) }
    private var paper: ProfilePack? { try? PackLibrary.shared.pack(model.look.recipe.printStock) }

    private var printsOnPaper: Binding<Bool> {
        Binding(get: { !(model.look.recipe.scanFilm ?? (film?.isPositive ?? false)) },
                set: { model.look.recipe.scanFilm = !$0 })
    }

    /// A black-and-white paper prints in silver: whatever the filtration, the
    /// picture comes out neutral, so white balance has nothing to act on.
    private var printsInSilver: Bool {
        printsOnPaper.wrappedValue && (paper?.isMonochrome ?? false)
    }

    /// The film picker goes through the model, which also moves the print to
    /// the new film's own paper when the choice crosses colour and black and
    /// white.
    private var filmStock: Binding<String> {
        Binding(get: { model.look.recipe.filmStock }, set: { model.selectFilm($0) })
    }

    // MARK: Film & paper

    private var stockSection: some View {
        Section("Film & Paper", subtitle: "The look is the film's and the paper's own spectral data, baked into the clip's colours.") {
            StockPicker(title: "Film", entries: PackLibrary.shared.filmStocks, selection: filmStock)
            ToggleRow(title: "Print on paper", isOn: printsOnPaper,
                      help: "Off reads the developed film straight off a light table, the way a slide is viewed. Slide stocks default to that.")
            if printsOnPaper.wrappedValue {
                StockPicker(title: "Paper", entries: PackLibrary.shared.printStocks, films: false,
                            selection: $model.look.recipe.printStock)
                if let suggested = film?.targetPrint, suggested != model.look.recipe.printStock,
                   PackLibrary.shared.printStocks.contains(where: { $0.stock == suggested }) {
                    Button {
                        model.look.recipe.printStock = suggested
                    } label: {
                        Label("Use this film's reference paper", systemImage: "wand.and.stars").font(.system(size: 10))
                    }
                    .buttonStyle(.link)
                }
            }
        }
    }

    // MARK: Exposure

    private var exposureSection: some View {
        Section(film?.isPositive == true ? "Exposure" : "Negative Exposure",
                subtitle: "How much light the film received. Expose for an 18% grey at \(model.format.curveName)'s \(model.format.grey) and 0 here prints it as mid-grey.") {
            ToggleRow(title: "Auto exposure", isOn: autoExposure,
                      help: "Keeps the clip's brightest tones inside the film's highlight latitude, metered once across the whole clip so exposure never moves within it. It only ever pulls exposure down: a negative has stops to spare and prints as shot, a slide keeps about one and a half stops above grey and is pulled down as far as it needs — what a slide shooter metering for the highlights would do. Move the slider to take over from there; that turns the auto off.")
            ParamSlider(title: "Exposure", value: exposure, range: -4...4, defaultValue: 0,
                        decimals: 2, unit: " EV",
                        help: "Moves the scene along the film's characteristic curve, as if the aperture had been different: overexposing a colour negative opens the shadows and softens colour, underexposing crushes them; a slide runs out of highlights first.")
            if model.look.recipe.autoExposure {
                Text(model.isMetering ? "Metering the clip…"
                     : model.meteredExposure.map { $0 == 0 ? "The clip fits this film's highlights as shot" : String(format: "Pulled %.2f EV to hold the clip's highlights", $0) }
                     ?? "Meters when a clip is open")
                    .font(.system(size: 9, design: .monospaced))
                    .foregroundStyle(.tertiary)
            }
            if let filters = film?.lensFilters, !filters.isEmpty {
                LensFilterPicker(filters: filters, selection: $model.look.recipe.lensFilter)
            }
            Button("Reset exposure") { model.resetExposure() }
                .buttonStyle(.link).font(.system(size: 10)).padding(.top, 2)
        }
    }

    /// With the auto on, the slider shows the meter's reading; moving it
    /// stores the new value and turns the auto off — the Stills mode's rule.
    private var exposure: Binding<Double> {
        Binding(
            get: {
                let r = model.look.recipe
                return r.autoExposure ? (model.meteredExposure ?? r.exposureEV) : r.exposureEV
            },
            set: { newValue in
                var r = model.look.recipe
                r.exposureEV = newValue
                r.autoExposure = false
                model.look.recipe = r
            })
    }

    /// Turning the auto off by hand leaves the slider where the meter had it.
    private var autoExposure: Binding<Bool> {
        Binding(
            get: { model.look.recipe.autoExposure },
            set: { on in
                var r = model.look.recipe
                if !on, let metered = model.meteredExposure { r.exposureEV = metered }
                r.autoExposure = on
                model.look.recipe = r
            })
    }

    // MARK: Tone

    private var toneSection: some View {
        Section("Tone", subtitle: "The print: paper grade and lightness moves, as in Stills.") {
            SpecSlider(spec: Controls.brightness, physical: $model.look.recipe.brightness, vocabulary: vocabulary)
            SpecSlider(spec: Controls.contrast(softest: film?.softestGrade ?? Constants.gradeMax),
                       physical: $model.look.recipe.grade, vocabulary: vocabulary,
                       defaultPhysical: model.look.recipe.defaultGrade)
            SpecSlider(spec: Controls.shadows, physical: $model.look.recipe.shadows, vocabulary: vocabulary)
            SpecSlider(spec: Controls.highlights, physical: $model.look.recipe.highlights, vocabulary: vocabulary)
            Button("Reset tone") { model.resetTone() }
                .buttonStyle(.link).font(.system(size: 10)).padding(.top, 2)
        }
    }

    // MARK: Colour

    private var colourSection: some View {
        Section("White Balance",
                subtitle: printsInSilver
                    ? "A black-and-white paper prints in silver, which has no colour to balance."
                    : "Corrected the way a lab would: with the enlarger's filters. A clip shot at a fixed 5500 K under tungsten goes orange, as daylight film does.") {
            if !printsInSilver {
                SpecSlider(spec: Controls.temperature, physical: $model.look.recipe.filterShiftY, vocabulary: vocabulary)
                SpecSlider(spec: Controls.tint, physical: $model.look.recipe.filterShiftM, vocabulary: vocabulary)
                Button("Reset colour") { model.resetColour() }
                    .buttonStyle(.link).font(.system(size: 10)).padding(.top, 2)
            }
        }
    }

    // MARK: Effects

    private var effectsSection: some View {
        Section("Effects", subtitle: "Bloom spreads the highlights softly; glow adds light around them; halation stands in for the film's own. Laid on the print, with sizes that follow the frame, so the preview matches the export.") {
            ToggleRow(title: "Bloom", isOn: $model.look.effects.bloom.enabled,
                      help: "Diffusion, like a mist filter on the lens: highlights bleed softly into darker surroundings and contrast falls around them, but nothing gets brighter than the highlight it came from. The Photoshop Pro Mist recipe: highlights selected, blurred and blended back with Lighten.")
            if model.look.effects.bloom.enabled {
                ParamSlider(title: "Amount", value: $model.look.effects.bloom.amount, range: 0...200, defaultValue: 100,
                            decimals: 0, unit: " %", help: "Layer opacity. Above 100 % it stacks like duplicated layers.")
                ParamSlider(title: "Size", value: $model.look.effects.bloom.size, range: 0...100, defaultValue: 45,
                            decimals: 0, help: "Blur radius, relative to the frame.")
                ParamSlider(title: "Threshold", value: $model.look.effects.bloom.threshold, range: 0...100, defaultValue: 80,
                            decimals: 0, help: "How bright a highlight must be to bloom. Lower includes more midtones.")
                DisclosureGroup("More") {
                    VStack(alignment: .leading, spacing: 5) {
                        ParamSlider(title: "Softness", value: $model.look.effects.bloom.softness, range: 0...100,
                                    defaultValue: 100, decimals: 0, unit: " %",
                                    help: "How gradually highlights above the threshold are selected.")
                        Picker("Blend", selection: $model.look.effects.bloom.blend) {
                            ForEach(BloomBlend.allCases, id: \.self) { Text($0.title).tag($0) }
                        }
                        .controlSize(.small).font(.system(size: 11))
                        ParamSlider(title: "Haze", value: $model.look.effects.bloom.haze, range: 0...100, defaultValue: 0,
                                    decimals: 0, help: "A wide veil over the whole frame, like a physical mist filter: it lifts the shadows and lowers contrast.")
                    }
                }
                .font(.system(size: 11))
            }

            ToggleRow(title: "Glow", isOn: $model.look.effects.glow.enabled,
                      help: "Added light, like the halo round a street lamp: the blurred highlights are added on top in linear light, so bright areas and what is near them get brighter. NegPy-style.")
            if model.look.effects.glow.enabled {
                ParamSlider(title: "Amount", value: $model.look.effects.glow.amount, range: 0...100, defaultValue: 30, decimals: 0)
                ParamSlider(title: "Size", value: $model.look.effects.glow.size, range: 0...100, defaultValue: 40, decimals: 0)
                ParamSlider(title: "Threshold", value: $model.look.effects.glow.threshold, range: 0...100, defaultValue: 50,
                            decimals: 0, help: "How bright an area must be to glow.")
            }

            ToggleRow(title: "Halation", isOn: $model.look.effects.halation.enabled,
                      help: "Light scattered back off the film base around bright highlights, NegPy-style: red on colour film, whose red-sensitive layer sits nearest the base; grey in black and white.")
            if model.look.effects.halation.enabled {
                ParamSlider(title: "Amount", value: $model.look.effects.halation.amount, range: 0...100, defaultValue: 40, decimals: 0)
                ParamSlider(title: "Size", value: $model.look.effects.halation.size, range: 0...100, defaultValue: 52, decimals: 0)
                ParamSlider(title: "Threshold", value: $model.look.effects.halation.threshold, range: 0...100, defaultValue: 83,
                            decimals: 0, help: "How bright a highlight must be to halate.")
                ToggleRow(title: "Halo only", isOn: $model.look.effects.halation.haloOnly,
                          help: "Keeps halation off the highlight itself, so it shows only as a ring around it.")
            }
            Button("Reset effects") { model.resetEffects() }
                .buttonStyle(.link).font(.system(size: 10)).padding(.top, 2)
        }
    }

    // MARK: Input

    private var inputSection: some View {
        Section("Log Input · \(model.format.shortName)", subtitle: "Read from the file's own tags. Override them only if a grey card does not land where it should.", expanded: false) {
            Picker("Format", selection: $model.look.input.format) {
                Text(model.clip.map { "From the file (\($0.detection.format.shortName), \($0.detection.basis.rawValue))" }
                     ?? "From the file").tag(LogFormat?.none)
                ForEach(LogFormat.allCases) { Text($0.rawValue).tag(LogFormat?.some($0)) }
            }
            .controlSize(.small).font(.system(size: 11))
            .help("Every curve but Apple Log is defined on the code values the camera records; Apple Log and Apple Log 2 (iPhone) on the 0–1 signal. Apple tags its log footage and a Sony camera names its curve in its own metadata. For a Lumix, Fujifilm, Canon or Insta360 file the camera's make decides: V-Log, F-Log2 where the camera has it (else F-Log), Canon Log 3, I-Log. Anything else is taken for N-Log. A choice here holds for this clip; the next one is read from its own tags.")
            Picker("Levels", selection: $model.look.input.levels) {
                ForEach(InputSettings.Levels.allCases) { Text($0.rawValue).tag($0) }
            }
            .controlSize(.small).font(.system(size: 11))
            Picker("Matrix", selection: $model.look.input.matrix) {
                ForEach(InputSettings.Matrix.allCases) { Text($0.rawValue).tag($0) }
            }
            .controlSize(.small).font(.system(size: 11))
            if let r = model.reading {
                Text("Reading \(r.format.shortName) · \(r.bitDepth)-bit \(r.chroma.rawValue) · \(r.fullRange ? "full" : "video") range · \(r.matrixName)")
                    .font(.system(size: 9, design: .monospaced)).foregroundStyle(.tertiary)
            }
        }
    }

    // MARK: Export

    private var exportSection: some View {
        Section("Export") {
            // The format is in the toolbar, beside Export, as in Stills.
            Picker("For", selection: $model.look.export.display) {
                Text("Apple displays").tag(LookLUT.Output.appleDisplay)
                Text("Rec.709 broadcast").tag(LookLUT.Output.bt1886)
            }
            .controlSize(.small).font(.system(size: 11))
            Text(model.look.export.display == .appleDisplay
                 ? "Looks as it does here in QuickTime, Photos and Safari on a Mac, iPhone or iPad, which decode Rec.709 video with a 1.961 power."
                 : "For a BT.1886 reference monitor or TV, which decode with a 2.4 power. On a Mac it will look lighter and flatter than here.")
                .font(.system(size: 10)).foregroundStyle(.tertiary)
                .fixedSize(horizontal: false, vertical: true)
            HStack {
                Button("Export…", action: onExport)
                    .disabled(model.clip == nil || model.isExporting)
                    .help("Every frame at full size, with the audio copied as it is (⌘E)")
                Button("Export LUT…", action: onExportLUT)
                    .help("The print as a 65-point .cube for DaVinci Resolve and other editors, Rec.709 (BT.1886) out, taking the clip's log the way Resolve reads it: N-Log and V-Log as data levels for a full-range file like a Zf's and video levels otherwise, Apple Log on its signal. Effects are not in it.")
            }
            .controlSize(.small)
            .padding(.top, 4)
        }
    }
}
