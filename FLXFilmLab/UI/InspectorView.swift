import SwiftUI

struct InspectorView: View {
    @ObservedObject var model: DocumentModel
    @Binding var selectedMask: UUID?
    @AppStorage("vocabulary") private var vocabularyRaw = Vocabulary.photographic.rawValue

    private var vocabulary: Vocabulary { Vocabulary(rawValue: vocabularyRaw) ?? .photographic }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                cropSection
                stockSection
                exposureSection
                gradientSection
                printSection
                shadowHighlightSection
                colourSection
                localSection
                effectsSection
                filmCharacterSection
                outputSection
            }
            .padding(.horizontal, 12)
            .padding(.bottom, 24)
        }
        .frame(width: 296)
        .background(.ultraThinMaterial)
    }

    /// `Recipe.scanFilm` is a tri-state (nil follows the stock); the toggle
    /// shows the resolved value and writes an explicit override.
    private var printsOnPaper: Binding<Bool> {
        Binding(
            get: { !(model.recipe.scanFilm ?? (model.filmPack?.isPositive ?? false)) },
            set: { model.recipe.scanFilm = !$0 })
    }

    /// A black-and-white paper prints in silver: whatever the filtration and
    /// whatever each layer is told, the picture comes out neutral, so the
    /// colour controls have nothing to act on.
    private var printsInSilver: Bool {
        printsOnPaper.wrappedValue && (model.paperPack?.isMonochrome ?? false)
    }

    /// The film picker goes through the model, which also moves the print to
    /// the new film's own paper when the choice crosses colour and black and
    /// white.
    private var filmStock: Binding<String> {
        Binding(get: { model.recipe.filmStock }, set: { model.selectFilm($0) })
    }

    // MARK: Crop & straighten

    private var geometry: FrameGeometry { model.recipe.geometry }

    private var cropAspect: Binding<CropAspect> {
        Binding(get: { model.recipe.geometry.aspect }, set: { model.setCropAspect($0) })
    }

    private var straighten: Binding<Double> {
        Binding(get: { model.recipe.geometry.straighten }, set: { model.setStraighten($0) })
    }

    private func customSide(_ side: WritableKeyPath<FrameGeometry, Double>) -> Binding<Double> {
        Binding(get: { model.recipe.geometry[keyPath: side] },
                set: { value in
                    var g = model.recipe.geometry
                    g[keyPath: side] = value
                    model.setCustomAspect(width: g.customWidth, height: g.customHeight)
                })
    }

    private var cropSection: some View {
        Section("Crop & Straighten",
                subtitle: "The print's shape. The film is exposed and developed whole; the meters read only what the crop keeps.") {
            HStack(spacing: 6) {
                Toggle(isOn: $model.isCropping) { Label("Crop", systemImage: "crop").font(.system(size: 11)) }
                    .toggleStyle(.button).controlSize(.small)
                    .disabled(!model.hasImage)
                    .help("Show the whole frame with the crop on it, to drag its corners and edges (⌘R).")
                Picker("", selection: cropAspect) {
                    ForEach(CropAspect.allCases) { Text($0.rawValue).tag($0) }
                }
                .labelsHidden().controlSize(.small).frame(maxWidth: 104)
                .help("The crop's proportions. Original keeps the frame's own; Free has none.")
                Button { model.swapCropOrientation() } label: { Image(systemName: "rectangle.portrait.rotate") }
                    .buttonStyle(.borderless).controlSize(.small)
                    .disabled(geometry.aspect == .free || !model.hasImage)
                    .help("Stand the crop up, or lay it down.")
            }

            if geometry.aspect == .custom {
                HStack(spacing: 4) {
                    TextField("Width", value: customSide(\.customWidth), format: .number)
                        .frame(width: 52)
                    Text(":").foregroundStyle(.secondary)
                    TextField("Height", value: customSide(\.customHeight), format: .number)
                        .frame(width: 52)
                    Spacer()
                }
                .textFieldStyle(.roundedBorder).controlSize(.small).font(.system(size: 11))
            }

            HStack(spacing: 12) {
                Button { model.turn(clockwise: false) } label: { Image(systemName: "rotate.left") }
                    .help("Turn a quarter to the left (⌘[).")
                Button { model.turn(clockwise: true) } label: { Image(systemName: "rotate.right") }
                    .help("Turn a quarter to the right (⌘]).")
                Button { model.flip(horizontal: true) } label: {
                    Image(systemName: "arrow.left.and.right.righttriangle.left.righttriangle.right")
                }
                .help("Mirror left to right.")
                Button { model.flip(horizontal: false) } label: {
                    Image(systemName: "arrow.up.and.down.righttriangle.up.righttriangle.down")
                }
                .help("Mirror top to bottom.")
                Spacer()
                Toggle(isOn: $model.isLevelling) { Label("Level", systemImage: "level").font(.system(size: 11)) }
                    .toggleStyle(.button).controlSize(.small)
                    .help("Draw a line on the picture along a horizon, or along an upright, and the picture is levelled to it.")
            }
            .buttonStyle(.borderless)
            .disabled(!model.hasImage)

            ParamSlider(title: "Straighten", value: straighten, range: -FrameGeometry.maxStraighten...FrameGeometry.maxStraighten,
                        defaultValue: 0, decimals: 1, unit: "°",
                        help: "Levels the picture, turning it clockwise for positive values. The crop shrinks as far as it must to stay inside the picture, and grows back when you level the other way.")

            Button("Reset crop") { model.resetGeometry() }
                .buttonStyle(.link).font(.system(size: 10)).padding(.top, 2)
                .disabled(model.recipe.frameGeometry == nil)
        }
    }

    // MARK: Stocks

    private var stockSection: some View {
        Section("Film & Paper", subtitle: "Spectral profiles derived from manufacturer datasheets.") {
            StockPicker(title: "Film", entries: model.filmEntries, selection: filmStock)

            ToggleRow(title: "Print on paper", isOn: printsOnPaper,
                      help: "Off reads the developed film straight off a light table, the way a slide is viewed. Slide stocks default to that: printing a positive onto RA-4 paper inverts it, which is what a lab would really produce and almost never what you want.")

            if printsOnPaper.wrappedValue {
                StockPicker(title: "Paper", entries: model.paperEntries, films: false,
                            selection: $model.recipe.printStock)
            }

            if printsOnPaper.wrappedValue, let film = model.filmPack, let suggested = film.targetPrint,
               suggested != model.recipe.printStock,
               model.paperEntries.contains(where: { $0.stock == suggested }) {
                Button {
                    model.recipe.printStock = suggested
                } label: {
                    Label("Use this film's reference paper", systemImage: "wand.and.stars")
                        .font(.system(size: 10))
                }
                .buttonStyle(.link)
            }
        }
    }

    // MARK: Exposure

    private var exposureSection: some View {
        Section(model.filmPack?.isPositive == true ? "Exposure" : "Negative Exposure",
                subtitle: "How much light the film received.") {
            ToggleRow(title: "Auto exposure",
                      isOn: autoToggle(\.autoExposure, value: \.exposureEV, reading: { model.meteredExposureEV }),
                      help: "Meters the frame and sets the Exposure slider to it: the median is pulled most of the way to an 18% grey, but never so far that the brightest tones run past the stock's highlight headroom. Move the slider to take over from there; that turns the auto off.")
            ParamSlider(title: "Exposure",
                        value: metered(\.exposureEV, auto: \.autoExposure, reading: { model.meteredExposureEV }),
                        range: -5...5, defaultValue: 0,
                        decimals: 2, unit: " EV",
                        help: "Moves the scene along the film's characteristic curve. Overexposing a colour negative lifts shadow detail and desaturates; underexposing crushes it.")

            if let filters = model.filmPack?.lensFilters, !filters.isEmpty {
                LensFilterPicker(filters: filters, selection: $model.recipe.lensFilter)
            }

            Picker("White balance", selection: $model.whiteBalance) {
                ForEach(RawLoader.WhiteBalance.allCases) { Text($0.rawValue).tag($0) }
            }
            .controlSize(.small).font(.system(size: 11))
            .help("The RAW is decoded for this illuminant, so the film sees the light the way it would have on set. Film reference uses whatever the loaded stock is balanced for.")

            Text(model.whiteBalanceSummary)
                .font(.system(size: 9, design: .monospaced))
                .foregroundStyle(model.isDecoding ? AnyShapeStyle(Color.orange) : AnyShapeStyle(.tertiary))

            if model.whiteBalance == .custom {
                ParamSlider(title: "Temperature", value: $model.customTemperature,
                            range: 2000...12000, defaultValue: 5500, decimals: 0, unit: " K")
                ParamSlider(title: "Tint", value: $model.customTint,
                            range: -150...150, defaultValue: 0, decimals: 0)
            }

            Button("Reset exposure") { model.resetExposure() }
                .buttonStyle(.link).font(.system(size: 10)).padding(.top, 2)
        }
    }

    // MARK: Graduated exposure

    /// A binding that resolves by identifier, like the masks' below.
    private func gradientBinding(_ id: UUID) -> Binding<ExposureGradient> {
        Binding(
            get: { model.recipe.gradients.first(where: { $0.id == id }) ?? ExposureGradient() },
            set: { updated in
                guard let index = model.recipe.gradients.firstIndex(where: { $0.id == id }) else { return }
                model.recipe.gradients[index] = updated
            })
    }

    /// Capture, not darkroom: the gradients change what the film receives,
    /// which is why they sit with the exposure and not with Dodge & Burn,
    /// which works on the print.
    private var gradientSection: some View {
        Section("Graduated Exposure",
                subtitle: "Exposure compensation that fades across the frame, given to the film when the picture is taken.") {
            if model.recipe.gradients.isEmpty {
                Text("Add a gradient, then drag its handles on the image: the full compensation is at the solid line, none past the dashed one, and the middle handle moves both. Gradients stack, each adding its own exposure where it covers.")
                    .font(.system(size: 10)).foregroundStyle(.tertiary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            // By identifier, for the same reason as the masks: a row can
            // delete its own gradient.
            ForEach(model.recipe.gradients.map(\.id), id: \.self) { id in
                if let gradient = model.recipe.gradients.first(where: { $0.id == id }) {
                    GradientRow(gradient: gradientBinding(id),
                                isSelected: selectedMask == id,
                                select: { selectedMask = id },
                                delete: {
                                    if selectedMask == id { selectedMask = nil }
                                    model.recipe.gradients.removeAll { $0.id == id }
                                })
                    .id(gradient.id)
                }
            }

            if model.recipe.gradients.count < Constants.maxExposureGradients {
                Button {
                    var gradient = ExposureGradient()
                    gradient.name = "Gradient \(model.recipe.gradients.count + 1)"
                    gradient.points = model.framePoints(gradient.points)
                    model.recipe.gradients.append(gradient)
                    selectedMask = gradient.id
                } label: {
                    Label("Add gradient", systemImage: "square.tophalf.filled")
                        .font(.system(size: 10))
                }
                .buttonStyle(.bordered).controlSize(.mini)
                .padding(.top, 4)
            }
        }
    }

    // MARK: Metered controls

    /// A slider whose auto is on shows the meter's reading. Moving it stores
    /// the new value and turns the auto off, so the meter's position is simply
    /// where manual adjustment starts.
    private func metered(_ value: WritableKeyPath<Recipe, Double>,
                         auto: WritableKeyPath<Recipe, Bool>,
                         reading: @escaping () -> Double?) -> Binding<Double> {
        Binding(
            get: {
                let recipe = model.recipe
                return recipe[keyPath: auto] ? (reading() ?? recipe[keyPath: value]) : recipe[keyPath: value]
            },
            set: { newValue in
                var recipe = model.recipe
                recipe[keyPath: value] = newValue
                recipe[keyPath: auto] = false
                model.recipe = recipe
            })
    }

    /// Turning an auto off by hand keeps the slider where the meter had it,
    /// rather than jumping back to whatever was stored before.
    private func autoToggle(_ auto: WritableKeyPath<Recipe, Bool>,
                            value: WritableKeyPath<Recipe, Double>,
                            reading: @escaping () -> Double?) -> Binding<Bool> {
        Binding(
            get: { model.recipe[keyPath: auto] },
            set: { isOn in
                var recipe = model.recipe
                if !isOn, recipe[keyPath: auto], let current = reading() {
                    recipe[keyPath: value] = current
                }
                recipe[keyPath: auto] = isOn
                model.recipe = recipe
            })
    }

    // MARK: Print

    private var printSection: some View {
        Section("The Print", subtitle: "How bright the print is, and how much contrast it carries.") {
            // The resolved recipe carries what the meters chose, so the curve
            // shows the contrast and brightness actually being printed.
            CurvePlotView(paper: printsOnPaper.wrappedValue ? model.paperPack : nil,
                          recipe: model.resolvedRecipe ?? model.recipe)
                .padding(.bottom, 4)

            ToggleRow(title: "Auto brightness",
                      isOn: autoToggle(\.autoDensity, value: \.brightness, reading: { model.meteredBrightness }),
                      help: "Meters the textured parts of the frame and sets Brightness to move the midtones partway toward a mid-grey print, so a deliberately low-key or high-key shot keeps its mood. Move the slider to take over from there; that turns the auto off.")
            SpecSlider(spec: Controls.brightness,
                       physical: metered(\.brightness, auto: \.autoDensity, reading: { model.meteredBrightness }),
                       vocabulary: vocabulary)
            SpecSlider(spec: Controls.printExposure, physical: $model.recipe.density,
                       vocabulary: vocabulary)

            ToggleRow(title: "Auto contrast",
                      isOn: autoToggle(\.autoGrade, value: \.grade, reading: { model.meteredGrade }),
                      help: "Fits the frame's textured range onto the paper and sets Contrast to it: softens until the darkest and lightest textured tones still hold detail, and hardens a flat scene only partway. Move the slider to take over from there; that turns the auto off.")
            SpecSlider(spec: Controls.contrast(softest: model.filmPack?.softestGrade ?? Constants.gradeMax),
                       physical: metered(\.grade, auto: \.autoGrade, reading: { model.meteredGrade }),
                       vocabulary: vocabulary, defaultPhysical: model.recipe.defaultGrade)

            Text(vocabulary == .photographic ? "Split contrast" : "Split grade")
                .font(.system(size: 10, weight: .medium)).foregroundStyle(.tertiary).padding(.top, 3)
            SpecSlider(spec: Controls.shadowContrast, physical: $model.recipe.shadowGrade,
                       vocabulary: vocabulary)
            SpecSlider(spec: Controls.highlightContrast, physical: $model.recipe.highlightGrade,
                       vocabulary: vocabulary)
        }
    }

    // MARK: Shadows & highlights — the headline controls

    private var shadowHighlightSection: some View {
        Section("Shadows & Highlights",
                subtitle: "Lighten or darken one half of the print without touching mid-grey, then set how each end rolls off.") {
            SpecSlider(spec: Controls.shadows, physical: $model.recipe.shadows,
                       vocabulary: vocabulary)
            SpecSlider(spec: Controls.highlights, physical: $model.recipe.highlights,
                       vocabulary: vocabulary)

            Divider().opacity(0.3).padding(.vertical, 3)

            SpecSlider(spec: Controls.blacks, physical: $model.recipe.toe, vocabulary: vocabulary)
            SpecSlider(spec: Controls.blacksRange, physical: $model.recipe.toeWidth,
                       vocabulary: vocabulary, defaultPhysical: 1)
            SpecSlider(spec: Controls.whites, physical: $model.recipe.shoulder, vocabulary: vocabulary)
            SpecSlider(spec: Controls.whitesRange, physical: $model.recipe.shoulderWidth,
                       vocabulary: vocabulary, defaultPhysical: 1)
            SpecSlider(spec: Controls.midtoneContrast, physical: $model.recipe.snap,
                       vocabulary: vocabulary)

            if !printsInSilver {
            DisclosureGroup {
                Text("Each dye layer has its own curve. A filter can only shift a whole layer in parallel, so it cannot fix crossover — shadows casting one colour and highlights the complement. These can.")
                    .font(.system(size: 9)).foregroundStyle(.tertiary)
                    .padding(.vertical, 4).fixedSize(horizontal: false, vertical: true)
                ChannelTrimRow(title: vocabulary == .photographic ? "Contrast" : "Grade trim",
                               trim: $model.recipe.gradeTrim, range: -30...30)
                ChannelTrimRow(title: vocabulary == .photographic ? "Blacks" : "Toe trim",
                               trim: $model.recipe.toeTrim, range: -1...1)
                ChannelTrimRow(title: vocabulary == .photographic ? "Whites" : "Shoulder trim",
                               trim: $model.recipe.shoulderTrim, range: -1...1)
                ChannelTrimRow(title: vocabulary == .photographic ? "Midtones" : "Snap trim",
                               trim: $model.recipe.snapTrim, range: -0.5...0.5)
            } label: {
                Text("Colour crossover").font(.system(size: 10, weight: .medium)).foregroundStyle(.tertiary)
            }
            .padding(.top, 3)
            }

            Button("Reset tone") { model.resetTone() }
                .buttonStyle(.link).font(.system(size: 10)).padding(.top, 2)
        }
    }

    // MARK: Colour

    private var colourSection: some View {
        Section("Colour",
                subtitle: printsInSilver
                    ? "A black-and-white paper prints in silver, which has no colour to balance. Pre-flash still holds the highlights."
                    : "Overall balance moves the enlarger's real filters. The regional pair works in density, which a filter cannot do.") {
            if !printsInSilver {
                SpecSlider(spec: Controls.temperature, physical: $model.recipe.filterShiftY,
                           vocabulary: vocabulary)
                SpecSlider(spec: Controls.tint, physical: $model.recipe.filterShiftM,
                           vocabulary: vocabulary)
                CMYRow(title: "Shadow colour", offset: $model.recipe.shadowCMY,
                       help: "Colour correction weighted to the shadows only, in Kodak CC units.")
                CMYRow(title: "Highlight colour", offset: $model.recipe.highlightCMY,
                       help: "Colour correction weighted to the highlights only, in Kodak CC units.")
            }
            SpecSlider(spec: Controls.highlightProtection, physical: $model.recipe.preflashExposure,
                       vocabulary: vocabulary)

            Button("Reset colour") { model.resetColour() }
                .buttonStyle(.link).font(.system(size: 10)).padding(.top, 2)
        }
    }

    /// A binding that resolves by identifier, so it stays valid however the
    /// array is reordered and degrades to a no-op once the mask is gone.
    private func maskBinding(_ id: UUID) -> Binding<LocalMask> {
        Binding(
            get: { model.recipe.masks.first(where: { $0.id == id }) ?? LocalMask() },
            set: { updated in
                guard let index = model.recipe.masks.firstIndex(where: { $0.id == id }) else { return }
                model.recipe.masks[index] = updated
            })
    }

    // MARK: Dodge & burn

    private var localSection: some View {
        Section("Dodge & Burn",
                subtitle: "Masks change print exposure, so they roll into paper black the way a real burn does.") {
            if model.recipe.masks.isEmpty {
                Text("Add a mask, then drag on the image to place its points.")
                    .font(.system(size: 10)).foregroundStyle(.tertiary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            // Iterate over identifiers, not over `$model.recipe.masks`.
            //
            // `ForEach` over a collection binding hands each row a binding that
            // indexes the array by position. Deleting from inside such a row
            // leaves the row holding a binding to an element that no longer
            // exists, and SwiftUI crashes on the next read. Looking the element
            // up by id on every access makes removal safe from anywhere.
            ForEach(model.recipe.masks.map(\.id), id: \.self) { id in
                if let mask = model.recipe.masks.first(where: { $0.id == id }) {
                    MaskRow(mask: maskBinding(id), vocabulary: vocabulary,
                            isSelected: selectedMask == id,
                            select: { selectedMask = id },
                            delete: {
                                if selectedMask == id { selectedMask = nil }
                                model.recipe.masks.removeAll { $0.id == id }
                            })
                    .id(mask.id)
                }
            }

            HStack(spacing: 6) {
                ForEach(MaskShape.allCases, id: \.self) { shape in
                    Button {
                        var mask = LocalMask()
                        mask.shape = shape
                        mask.name = "\(shape.rawValue.capitalized) \(model.recipe.masks.count + 1)"
                        mask.points = model.framePoints(defaultPoints(for: shape))
                        model.recipe.masks.append(mask)
                        selectedMask = mask.id
                    } label: {
                        Label(shape.rawValue.capitalized, systemImage: icon(for: shape))
                            .font(.system(size: 10))
                    }
                    .buttonStyle(.bordered).controlSize(.mini)
                }
            }
            .padding(.top, 4)
        }
    }

    private func icon(for shape: MaskShape) -> String {
        switch shape {
        case .polygon: return "pentagon"
        case .oval: return "circle"
        case .gradient: return "square.righthalf.filled"
        }
    }

    private func defaultPoints(for shape: MaskShape) -> [CGPoint] {
        switch shape {
        case .polygon:
            return [CGPoint(x: 0.35, y: 0.35), CGPoint(x: 0.65, y: 0.35),
                    CGPoint(x: 0.65, y: 0.65), CGPoint(x: 0.35, y: 0.65)]
        case .oval:
            return [CGPoint(x: 0.5, y: 0.5), CGPoint(x: 0.72, y: 0.5), CGPoint(x: 0.5, y: 0.72)]
        case .gradient:
            return [CGPoint(x: 0.5, y: 0.1), CGPoint(x: 0.5, y: 0.5)]
        }
    }

    // MARK: Film character

    private var filmCharacterSection: some View {
        Section("Film Character", expanded: false) {
            // A black-and-white film has no couplers, but development holds
            // itself back the same way; the engine runs it as one layer's
            // own inhibition.
            if model.filmPack?.isMonochrome == true {
                ToggleRow(title: "Edge effects", isOn: $model.recipe.couplersEnabled,
                          help: "Development's by-products spread out of dense areas and hold back development beside them, so every edge comes out crisper — the adjacency effect behind silver film's acutance. Matched to Kodak's measured sharpness of Tri-X in D-76.")
                ParamSlider(title: "Edge effect", value: $model.recipe.couplersAmount,
                            range: 0...2, defaultValue: 1, decimals: 2,
                            help: "How strongly development holds back next to dense areas. 1 is D-76; above that leans toward a dilute acutance developer such as Rodinal; 0 is none.")
                ParamSlider(title: "Edge width", value: $model.recipe.couplersDiffusion,
                            range: 0...3, defaultValue: 1, decimals: 2,
                            help: "How far the by-products spread before they act. Wider reads as local contrast more than as edge sharpness.")

                Divider().opacity(0.3).padding(.vertical, 3)
            } else if let couplers = model.filmPack?.defaults.dirCouplers {
                ToggleRow(title: "DIR couplers", isOn: $model.recipe.couplersEnabled,
                          help: "Inhibitors released as density forms, cutting density in neighbouring layers. This is where a film's saturation and edge sharpness come from — a datasheet curve alone does not look like film.")
                ParamSlider(title: "Coupler amount", value: $model.recipe.couplersAmount,
                            range: 0...2, defaultValue: 1, decimals: 2)
                // A stock whose published MTF shows no edge effect (Provia
                // 400X) has an inhibitor with no reach: nothing to widen.
                if couplers.diffusionSizeUm > 0 {
                    ParamSlider(title: "Coupler diffusion", value: $model.recipe.couplersDiffusion,
                                range: 0...3, defaultValue: 1, decimals: 2,
                                help: "How far the inhibitor spreads before it acts. Wider diffusion reads as local contrast and apparent sharpness.")
                }

                Divider().opacity(0.3).padding(.vertical, 3)
            }

            ToggleRow(title: "Halation", isOn: $model.recipe.halationEnabled,
                      help: "Light passing through the emulsion, reflecting off the base and coming back — the red bloom around bright edges.")
            ParamSlider(title: "Halation", value: $model.recipe.halationAmount,
                        range: 0...4, defaultValue: 1, decimals: 2)
            ParamSlider(title: "Halation size", value: $model.recipe.halationScale,
                        range: 0.2...4, defaultValue: 1, decimals: 2)
            ParamSlider(title: "Scatter", value: $model.recipe.scatterAmount,
                        range: 0...4, defaultValue: 1, decimals: 2,
                        help: "Scattering inside the emulsion itself, a much tighter effect than halation.")

            Divider().opacity(0.3).padding(.vertical, 3)

            ToggleRow(title: "Grain", isOn: $model.recipe.grainEnabled,
                      help: "Stochastic silver-halide development, modelled per layer. Visible at 100% and on export.")
            ParamSlider(title: "Grain", value: $model.recipe.grainAmount,
                        range: 0...3, defaultValue: 1, decimals: 2)
            ParamSlider(title: "Grain size", value: $model.recipe.grainScale,
                        range: 0.25...4, defaultValue: 1, decimals: 2)
            Picker("Film format", selection: $model.recipe.filmFormat) {
                ForEach(FilmFormat.allCases) { Text($0.rawValue).tag($0) }
            }
            .pickerStyle(.segmented).controlSize(.small).font(.system(size: 11))
            .help("Sets the frame size, and with it the pixel pitch that scales grain, halation and coupler diffusion. The same emulsion on 120 shows visibly finer grain than on 35 mm, because the frame is enlarged less.")

            Divider().opacity(0.3).padding(.vertical, 3)

            ToggleRow(title: "Viewing glare", isOn: $model.recipe.glareEnabled,
                      help: "Light scattering off the print surface, which is why a real print's blacks are never as deep as the numbers suggest.")
            ParamSlider(title: "Glare", value: $model.recipe.glareAmount,
                        range: 0...0.4, defaultValue: 0.03, decimals: 3, unit: "%")
            ParamSlider(title: "Curve gamma", value: $model.recipe.densityCurveGamma,
                        range: 0.5...2, defaultValue: 1, decimals: 2,
                        help: "Development contrast of the negative itself — push and pull processing.")

            Button("Reset film character") { model.resetFilmCharacter() }
                .buttonStyle(.link).font(.system(size: 10)).padding(.top, 2)
        }
    }

    // MARK: Bloom & Glow

    /// The effects, all off until first touched.
    private var effects: Binding<GlowLook> {
        Binding(get: { model.recipe.effects ?? .off }, set: { model.recipe.effects = $0 })
    }

    private var effectsSection: some View {
        Section("Bloom & Glow",
                subtitle: "Bloom spreads the highlights softly; glow adds light around them. Laid on the finished print, with sizes that follow the frame, so the preview matches the export.",
                expanded: false) {
            ToggleRow(title: "Bloom", isOn: effects.bloom.enabled,
                      help: "Diffusion, like a mist filter on the lens: highlights bleed softly into darker surroundings and contrast falls around them, but nothing gets brighter than the highlight it came from. The Photoshop Pro Mist recipe: highlights selected, blurred and blended back with Lighten.")
            if effects.wrappedValue.bloom.enabled {
                ParamSlider(title: "Amount", value: effects.bloom.amount, range: 0...200, defaultValue: 100,
                            decimals: 0, unit: " %", help: "Layer opacity. Above 100 % it stacks like duplicated layers.")
                ParamSlider(title: "Size", value: effects.bloom.size, range: 0...100, defaultValue: 45,
                            decimals: 0, help: "Blur radius, relative to the frame.")
                ParamSlider(title: "Threshold", value: effects.bloom.threshold, range: 0...100, defaultValue: 80,
                            decimals: 0, help: "How bright a highlight must be to bloom. Lower includes more midtones.")
                DisclosureGroup("More") {
                    VStack(alignment: .leading, spacing: 5) {
                        ParamSlider(title: "Softness", value: effects.bloom.softness, range: 0...100,
                                    defaultValue: 100, decimals: 0, unit: " %",
                                    help: "How gradually highlights above the threshold are selected.")
                        Picker("Blend", selection: effects.bloom.blend) {
                            ForEach(BloomBlend.allCases, id: \.self) { Text($0.title).tag($0) }
                        }
                        .controlSize(.small).font(.system(size: 11))
                        ParamSlider(title: "Haze", value: effects.bloom.haze, range: 0...100, defaultValue: 0,
                                    decimals: 0, help: "A wide veil over the whole frame, like a physical mist filter: it lifts the shadows and lowers contrast.")
                    }
                }
                .font(.system(size: 11))
            }

            ToggleRow(title: "Glow", isOn: effects.glow.enabled,
                      help: "Added light, like the halo round a street lamp: the blurred highlights are added on top in linear light, so bright areas and what is near them get brighter. NegPy-style.")
            if effects.wrappedValue.glow.enabled {
                ParamSlider(title: "Amount", value: effects.glow.amount, range: 0...100, defaultValue: 30, decimals: 0)
                ParamSlider(title: "Size", value: effects.glow.size, range: 0...100, defaultValue: 40, decimals: 0)
                ParamSlider(title: "Threshold", value: effects.glow.threshold, range: 0...100, defaultValue: 50,
                            decimals: 0, help: "How bright an area must be to glow.")
            }

            Button("Reset bloom & glow") { model.recipe.effects = nil }
                .buttonStyle(.link).font(.system(size: 10)).padding(.top, 2)
        }
    }

    // MARK: Output

    private var outputSection: some View {
        Section("Output", expanded: false) {
            Picker("Colour space", selection: $model.recipe.outputColorSpace) {
                ForEach(PackLibrary.shared.shared_.outputColorSpaces, id: \.self) { Text($0).tag($0) }
            }
            .controlSize(.small).font(.system(size: 11))
            ToggleRow(title: "Gamut compression", isOn: $model.recipe.gamutCompress,
                      help: "Pulls colours the film reached that the output space cannot show back inside it, moving chroma rather than clipping channels.")
            Picker("Control names", selection: $vocabularyRaw) {
                ForEach(Vocabulary.allCases) { Text($0.rawValue).tag($0.rawValue) }
            }
            .controlSize(.small).font(.system(size: 11))
            .help("Photographic uses the names Capture One and Lightroom use, where positive always means brighter. Darkroom shows the underlying print controls. Either way the tooltip names the other.")

            Picker("Preview size", selection: $model.proxyMaxDimension) {
                ForEach(DocumentModel.previewSizes, id: \.self) { Text("\($0) px").tag($0) }
            }
            .controlSize(.small).font(.system(size: 11))
            .help("The preview's long side. Larger is sharper on screen and slower to redraw: on an M4 Pro a print control redraws 2560 px in about 23 ms and 4096 px in about 57. A file smaller than this is shown at its own size, never enlarged, and export is always at full size. Kept between launches.")
        }
    }
}

private struct GradientRow: View {
    @Binding var gradient: ExposureGradient
    let isSelected: Bool
    let select: () -> Void
    let delete: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 5) {
                Toggle("", isOn: $gradient.enabled).labelsHidden().toggleStyle(.checkbox).controlSize(.mini)
                Text(gradient.name).font(.system(size: 11, weight: isSelected ? .semibold : .regular))
                Spacer()
                Text(String(format: "%+.2f EV", gradient.exposureEV + 0))
                    .font(.system(size: 9, design: .monospaced)).foregroundStyle(.tertiary)
                Button { delete() } label: { Image(systemName: "trash").font(.system(size: 9)) }
                    .buttonStyle(.plain).foregroundStyle(.tertiary)
            }
            .contentShape(Rectangle())
            .onTapGesture { select() }

            if isSelected {
                ParamSlider(title: "Exposure", value: $gradient.exposureEV, range: -4...4, defaultValue: 0,
                            decimals: 2, unit: " EV",
                            help: "Exposure on the covered side, like the Exposure slider: positive gives the film more light, negative less. Where gradients overlap, their exposures add.")
            }
        }
        .padding(6)
        .background(isSelected ? Color.accentColor.opacity(0.1) : Color.clear)
        .clipShape(RoundedRectangle(cornerRadius: 4))
    }
}

private struct MaskRow: View {
    @Binding var mask: LocalMask
    let vocabulary: Vocabulary
    let isSelected: Bool
    let select: () -> Void
    let delete: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 5) {
                Toggle("", isOn: $mask.enabled).labelsHidden().toggleStyle(.checkbox).controlSize(.mini)
                Text(mask.name).font(.system(size: 11, weight: isSelected ? .semibold : .regular))
                Spacer()
                Button { delete() } label: { Image(systemName: "trash").font(.system(size: 9)) }
                    .buttonStyle(.plain).foregroundStyle(.tertiary)
            }
            .contentShape(Rectangle())
            .onTapGesture { select() }

            if isSelected {
                SpecSlider(spec: Controls.maskExposure, physical: $mask.stops, vocabulary: vocabulary)
                ParamSlider(title: "Feather", value: $mask.feather, range: 0...0.25, defaultValue: 0.04,
                            decimals: 3)
                SpecSlider(spec: Controls.maskContrast, physical: $mask.grade, vocabulary: vocabulary)
                Toggle("Invert", isOn: $mask.inverted)
                    .font(.system(size: 10)).toggleStyle(.checkbox).controlSize(.mini)
            }
        }
        .padding(6)
        .background(isSelected ? Color.accentColor.opacity(0.1) : Color.clear)
        .clipShape(RoundedRectangle(cornerRadius: 4))
    }
}
