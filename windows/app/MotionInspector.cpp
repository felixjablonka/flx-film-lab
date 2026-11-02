#include "MotionInspector.h"

#include "MotionDocument.h"
#include "Theme.h"
#include "Widgets.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <cmath>

namespace {

QString filterFactor(float factor)
{
    return QStringLiteral("%1×, %2 stops").arg(factor, 0, 'f', 1).arg(std::log2(std::max(factor, 1.0f)), 0, 'f', 1);
}

QLabel *monoNote()
{
    auto *l = new QLabel;
    l->setFont(theme::mono(10));
    l->setWordWrap(true);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, theme::tertiary());
    l->setPalette(p);
    return l;
}

QPushButton *actionButton(const QString &text, const QString &help)
{
    auto *b = new QPushButton(text);
    b->setFont(theme::small());
    b->setToolTip(help);
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral("QPushButton { background: #3a3a3a; color: #e6e6e6; border: 1px solid #4a4a4a;"
                                    " border-radius: 5px; padding: 3px 12px; }"
                                    "QPushButton:hover { background: #474747; }"
                                    "QPushButton:disabled { color: #666; }"));
    return b;
}

} // namespace

MotionInspector::MotionInspector(MotionDocument &document, Vocabulary vocabulary, QWidget *parent)
    : QScrollArea(parent), m_doc(document), m_vocabulary(vocabulary)
{
    m_content = new QWidget;
    auto *layout = new QVBoxLayout(m_content);
    layout->setContentsMargins(12, 0, 12, 24);
    layout->setSpacing(0);
    setWidget(m_content);
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFixedWidth(306);
    setFrameShape(QFrame::NoFrame);
    m_content->setAutoFillBackground(true);
    QPalette p = m_content->palette();
    p.setColor(QPalette::Window, theme::inspector());
    m_content->setPalette(p);
    viewport()->setAutoFillBackground(true);
    viewport()->setPalette(p);

    buildStocks();
    buildExposure();
    buildTone();
    buildColour();
    buildEffects();
    buildInput();
    buildExport();
    layout->addStretch(1);

    connect(&m_doc, &MotionDocument::changed, this, &MotionInspector::refresh);
    refresh();
}

void MotionInspector::edit(const std::function<void(MotionLook &)> &change)
{
    MotionLook look = m_doc.look();
    change(look);
    m_doc.setLook(look);
}

void MotionInspector::refresh()
{
    for (const SpecBinding &b : m_specs) {
        const ControlSpec spec = b.spec();
        const double physical = b.physical();
        b.slider->setTitle(spec.name(m_vocabulary));
        b.slider->setRange(spec.low, spec.high);
        b.slider->setDefault(spec.fromPhysical(b.defaultPhysical()));
        b.slider->setValue(spec.fromPhysical(physical));
        b.slider->setHelp(spec.help(m_vocabulary, physical));
    }
    for (const auto &refresh : m_refreshers)
        refresh();
}

ParamSlider *MotionInspector::specSlider(const std::function<ControlSpec()> &spec, std::function<double()> physical,
                                         std::function<void(double)> setPhysical,
                                         std::function<double()> defaultPhysical)
{
    const ControlSpec s = spec();
    auto *slider = new ParamSlider(s.name(m_vocabulary), s.low, s.high, 0, s.decimals, s.unit);
    const size_t index = m_specs.size();
    m_specs.push_back({slider, spec, std::move(physical), std::move(setPhysical), std::move(defaultPhysical)});
    connect(slider, &ParamSlider::valueChanged, this, [this, index](double v) {
        const SpecBinding &b = m_specs[index];
        b.setPhysical(b.spec().toPhysical(v));
    });
    return slider;
}

// MARK: - Film & Paper

void MotionInspector::buildStocks()
{
    auto *section = new Section(QStringLiteral("Film & Paper"),
                                QStringLiteral("The look is the film's and the paper's own spectral data, baked into "
                                               "the clip's colours."));
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();

    auto *film = new StockPicker(QStringLiteral("Film"), stockGroups(m_doc.packs().filmEntries(), true));
    connect(film, &Choice::chosen, this, [this](const QVariant &v) { m_doc.selectFilm(v.toString().toStdString()); });
    body->addWidget(film);

    auto *onPaper = new ToggleRow(QStringLiteral("Print on paper"));
    onPaper->setHelp(QStringLiteral("Off reads the developed film straight off a light table, the way a slide is "
                                    "viewed. Slide stocks default to that."));
    connect(onPaper, &ToggleRow::toggled, this, [this](bool on) { edit([&](MotionLook &l) { l.recipe.scanFilm = !on; }); });
    body->addWidget(onPaper);

    auto *paper = new StockPicker(QStringLiteral("Paper"), stockGroups(m_doc.packs().paperEntries(), false));
    connect(paper, &Choice::chosen, this, [this](const QVariant &v) {
        edit([&](MotionLook &l) { l.recipe.printStock = v.toString().toStdString(); });
    });
    body->addWidget(paper);

    auto *reference = new LinkButton(QStringLiteral("Use this film's reference paper"));
    connect(reference, &QPushButton::clicked, this, [this] {
        if (const ProfilePack *f = m_doc.filmPack())
            edit([&](MotionLook &l) { l.recipe.printStock = f->targetPrint; });
    });
    body->addWidget(reference);

    m_refreshers.push_back([=, this] {
        const Recipe &r = m_doc.look().recipe;
        film->setCurrent(QString::fromStdString(r.filmStock));
        const bool printing = m_doc.printsOnPaper();
        onPaper->setChecked(printing);
        paper->setVisible(printing);
        paper->setCurrent(QString::fromStdString(r.printStock));
        const ProfilePack *f = m_doc.filmPack();
        bool suggest = false;
        if (printing && f && !f->targetPrint.empty() && f->targetPrint != r.printStock)
            for (const PackIndexEntry &e : m_doc.packs().paperEntries())
                suggest = suggest || e.stock == f->targetPrint;
        reference->setVisible(suggest);
    });
}

// MARK: - Exposure

void MotionInspector::buildExposure()
{
    auto *section = new Section(QStringLiteral("Negative Exposure"));
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();

    auto *autoExposure = new ToggleRow(QStringLiteral("Auto exposure"));
    autoExposure->setHelp(QStringLiteral(
        "Keeps the clip's brightest tones inside the film's highlight latitude, metered once across the whole clip "
        "so exposure never moves within it. It only ever pulls exposure down: a negative has stops to spare and "
        "prints as shot, a slide keeps about one and a half stops above grey and is pulled down as far as it needs "
        "— what a slide shooter metering for the highlights would do. Move the slider to take over from there; that "
        "turns the auto off."));
    // turning the auto off by hand leaves the slider where the meter had it
    connect(autoExposure, &ToggleRow::toggled, this, [this](bool on) {
        edit([&](MotionLook &l) {
            if (!on && m_doc.meteredExposure())
                l.recipe.exposureEV = *m_doc.meteredExposure();
            l.recipe.autoExposure = on;
        });
    });
    body->addWidget(autoExposure);

    auto *exposure = new ParamSlider(QStringLiteral("Exposure"), -4, 4, 0, 2, QStringLiteral(" EV"));
    exposure->setHelp(QStringLiteral(
        "Moves the scene along the film's characteristic curve, as if the aperture had been different: "
        "overexposing a colour negative opens the shadows and softens colour, underexposing crushes them; a slide "
        "runs out of highlights first."));
    // moving it stores the new value and turns the auto off, as in Stills
    connect(exposure, &ParamSlider::valueChanged, this, [this](double v) {
        edit([&](MotionLook &l) {
            l.recipe.exposureEV = v;
            l.recipe.autoExposure = false;
        });
    });
    body->addWidget(exposure);

    auto *metered = monoNote();
    body->addWidget(metered);

    auto *filter = new Choice(QStringLiteral("Filter"));
    connect(filter, &Choice::chosen, this, [this](const QVariant &v) {
        edit([&](MotionLook &l) {
            if (v.toString().isEmpty())
                l.recipe.lensFilter.reset();
            else
                l.recipe.lensFilter = v.toString().toStdString();
        });
    });
    body->addWidget(filter);

    auto *reset = new LinkButton(QStringLiteral("Reset exposure"));
    connect(reset, &QPushButton::clicked, this, [this] { m_doc.resetExposure(); });
    body->addWidget(reset);

    QString filtersOf;
    m_refreshers.push_back([=, this]() mutable {
        const Recipe &r = m_doc.look().recipe;
        const ProfilePack *film = m_doc.filmPack();
        const LogFormat f = m_doc.format();
        section->setTitle(film && film->isPositive() ? QStringLiteral("Exposure") : QStringLiteral("Negative Exposure"));
        section->setSubtitle(QStringLiteral("How much light the film received. Expose for an 18% grey at %1's %2 and 0 "
                                            "here prints it as mid-grey.")
                                 .arg(QString::fromStdString(logFormatCurveName(f)),
                                      QString::fromStdString(logFormatGrey(f))));
        autoExposure->setChecked(r.autoExposure);
        exposure->setValue(r.autoExposure && m_doc.meteredExposure() ? *m_doc.meteredExposure() : r.exposureEV);
        metered->setVisible(r.autoExposure);
        if (m_doc.isMetering())
            metered->setText(QStringLiteral("Metering the clip…"));
        else if (const auto &ev = m_doc.meteredExposure())
            metered->setText(*ev == 0 ? QStringLiteral("The clip fits this film's highlights as shot")
                                      : QStringLiteral("Pulled %1 EV to hold the clip's highlights").arg(*ev, 0, 'f', 2));
        else
            metered->setText(QStringLiteral("Meters when a clip is open"));

        const bool filters = film && !film->lensFilters.empty();
        filter->setVisible(filters);
        if (filters && filtersOf != QString::fromStdString(film->stock)) {
            filtersOf = QString::fromStdString(film->stock);
            filter->clear();
            filter->addItem(QStringLiteral("None"), QString());
            bool wratten = false;
            for (const LensFilterRef &lf : film->lensFilters) {
                filter->addItem(QString::fromStdString(lf.designation()) + QStringLiteral(" · ") + filterFactor(lf.factor),
                                QString::fromStdString(lf.id));
                wratten = wratten || !lf.wratten.empty();
            }
            filter->setHelp(wratten
                ? QStringLiteral("A coloured filter on the lens, measured from Kodak's Wratten data. It passes its own "
                                 "colour and holds back the rest: yellow, orange and red darken a blue sky "
                                 "progressively; red lightens skin and darkens foliage; green does the opposite; blue "
                                 "lightens skies and haze and deepens reds. Exposure is opened up by the filter "
                                 "factor shown, as a photographer would, so a grey card prints the same.")
                : QStringLiteral("A warming or conversion filter on the lens, measured from its maker's transmission "
                                 "curves (Kodak's Wratten 81C, 85 and 85B, Tiffen's 812): the film sees the scene "
                                 "through it, wavelength by wavelength, with its own sensitivities. The 81C holds "
                                 "back blue and some green, a plain amber warmth; the 812 holds back violet and a "
                                 "little blue and lets blue and green through alike, so its warmth leans pink: both "
                                 "for daylight film's cool shade, overcast light and flash. The 85 and 85B hold back "
                                 "blue and much of the green and pass the red: they convert daylight for tungsten "
                                 "film, rated 5500 K to 3400 K and to 3200 K. Measured, both go further than rated, "
                                 "to about 3300 K and 2800 K, and a tungsten film's own layers see the same: balanced "
                                 "for 3200 K, it takes daylight through the 85 a little cool and through the 85B a "
                                 "little warm. Kodak names the 85 for its tungsten stocks in daylight. On daylight "
                                 "film they warm strongly. Exposure is opened up by the filter factor shown, as a "
                                 "photographer would, so the picture keeps its brightness: for the 85 and 85B the "
                                 "factor in daylight, the light they are made for."));
        }
        if (filters)
            filter->setCurrent(r.lensFilter ? QString::fromStdString(*r.lensFilter) : QString());
    });
}

// MARK: - Tone

void MotionInspector::buildTone()
{
    auto *section = new Section(QStringLiteral("Tone"), QStringLiteral("The print: paper grade and lightness moves, as "
                                                                       "in Stills."));
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();
    body->addWidget(specSlider([] { return controls::brightness; }, [this] { return m_doc.look().recipe.brightness; },
                               [this](double v) { edit([&](MotionLook &l) { l.recipe.brightness = v; }); }));
    body->addWidget(specSlider(
        [this] {
            const ProfilePack *film = m_doc.filmPack();
            return controls::contrast(film ? film->softestGrade() : kGradeMax);
        },
        [this] { return m_doc.look().recipe.grade; },
        [this](double v) { edit([&](MotionLook &l) { l.recipe.grade = v; }); }, [this] { return m_doc.defaultGrade(); }));
    body->addWidget(specSlider([] { return controls::shadows; }, [this] { return m_doc.look().recipe.shadows; },
                               [this](double v) { edit([&](MotionLook &l) { l.recipe.shadows = v; }); }));
    body->addWidget(specSlider([] { return controls::highlights; }, [this] { return m_doc.look().recipe.highlights; },
                               [this](double v) { edit([&](MotionLook &l) { l.recipe.highlights = v; }); }));
    auto *reset = new LinkButton(QStringLiteral("Reset tone"));
    connect(reset, &QPushButton::clicked, this, [this] { m_doc.resetTone(); });
    body->addWidget(reset);
}

// MARK: - White Balance

void MotionInspector::buildColour()
{
    auto *section = new Section(QStringLiteral("White Balance"));
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();
    auto *temperature = specSlider([] { return controls::temperature; },
                                   [this] { return m_doc.look().recipe.filterShiftY; },
                                   [this](double v) { edit([&](MotionLook &l) { l.recipe.filterShiftY = v; }); });
    auto *tintSlider = specSlider([] { return controls::tint; }, [this] { return m_doc.look().recipe.filterShiftM; },
                                  [this](double v) { edit([&](MotionLook &l) { l.recipe.filterShiftM = v; }); });
    body->addWidget(temperature);
    body->addWidget(tintSlider);
    auto *reset = new LinkButton(QStringLiteral("Reset colour"));
    connect(reset, &QPushButton::clicked, this, [this] { m_doc.resetColour(); });
    body->addWidget(reset);
    m_refreshers.push_back([=, this] {
        const bool silver = m_doc.printsInSilver();
        section->setSubtitle(silver ? QStringLiteral("A black-and-white paper prints in silver, which has no colour to "
                                                     "balance.")
                                    : QStringLiteral("Corrected the way a lab would: with the enlarger's filters. A "
                                                     "clip shot at a fixed 5500 K under tungsten goes orange, as "
                                                     "daylight film does."));
        temperature->setVisible(!silver);
        tintSlider->setVisible(!silver);
        reset->setVisible(!silver);
    });
}

// MARK: - Effects

void MotionInspector::buildEffects()
{
    auto *section = new Section(
        QStringLiteral("Effects"),
        QStringLiteral("Bloom spreads the highlights softly; glow adds light around them; halation stands in for the "
                       "film's own. Laid on the print, with sizes that follow the frame, so the preview matches the "
                       "export."));
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();
    auto editEffects = [this](const std::function<void(GlowLook &)> &change) {
        edit([&](MotionLook &l) { change(l.effects); });
    };
    // a slider on one of the effects' values
    auto slider = [&](const QString &title, double low, double high, double def, const QString &unit,
                      std::function<double &(GlowLook &)> value, const QString &help = {}) {
        auto *s = new ParamSlider(title, low, high, def, 0, unit);
        s->setHelp(help);
        connect(s, &ParamSlider::valueChanged, this, [editEffects, value](double v) {
            editEffects([&](GlowLook &g) { value(g) = v; });
        });
        m_refreshers.push_back([this, s, value] {
            GlowLook g = m_doc.look().effects;
            s->setValue(value(g));
        });
        return s;
    };
    auto group = [](QVBoxLayout *&layout) {
        auto *w = new QWidget;
        layout = new QVBoxLayout(w);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(5);
        return w;
    };

    auto *bloom = new ToggleRow(QStringLiteral("Bloom"));
    bloom->setHelp(QStringLiteral(
        "Diffusion, like a mist filter on the lens: highlights bleed softly into darker surroundings and contrast "
        "falls around them, but nothing gets brighter than the highlight it came from. The Photoshop Pro Mist "
        "recipe: highlights selected, blurred and blended back with Lighten."));
    connect(bloom, &ToggleRow::toggled, this, [editEffects](bool on) { editEffects([&](GlowLook &g) { g.bloom.enabled = on; }); });
    body->addWidget(bloom);
    QVBoxLayout *bloomLayout = nullptr;
    QWidget *bloomControls = group(bloomLayout);
    bloomLayout->addWidget(slider(QStringLiteral("Amount"), 0, 200, 100, QStringLiteral(" %"),
                                  [](GlowLook &g) -> double & { return g.bloom.amount; },
                                  QStringLiteral("Layer opacity. Above 100 % it stacks like duplicated layers.")));
    bloomLayout->addWidget(slider(QStringLiteral("Size"), 0, 100, 45, {},
                                  [](GlowLook &g) -> double & { return g.bloom.size; },
                                  QStringLiteral("Blur radius, relative to the frame.")));
    bloomLayout->addWidget(slider(QStringLiteral("Threshold"), 0, 100, 80, {},
                                  [](GlowLook &g) -> double & { return g.bloom.threshold; },
                                  QStringLiteral("How bright a highlight must be to bloom. Lower includes more "
                                                 "midtones.")));
    auto *more = new Disclosure(QStringLiteral("More"));
    more->body()->addWidget(slider(QStringLiteral("Softness"), 0, 100, 100, QStringLiteral(" %"),
                                   [](GlowLook &g) -> double & { return g.bloom.softness; },
                                   QStringLiteral("How gradually highlights above the threshold are selected.")));
    auto *blend = new Choice(QStringLiteral("Blend"));
    blend->addItem(QStringLiteral("Lighten"), QStringLiteral("lighten"));
    blend->addItem(QStringLiteral("Screen"), QStringLiteral("screen"));
    blend->addItem(QStringLiteral("Add"), QStringLiteral("add"));
    connect(blend, &Choice::chosen, this, [editEffects](const QVariant &v) {
        editEffects([&](GlowLook &g) { g.bloom.blend = v.toString().toStdString(); });
    });
    more->body()->addWidget(blend);
    more->body()->addWidget(slider(QStringLiteral("Haze"), 0, 100, 0, {},
                                   [](GlowLook &g) -> double & { return g.bloom.haze; },
                                   QStringLiteral("A wide veil over the whole frame, like a physical mist filter: it "
                                                  "lifts the shadows and lowers contrast.")));
    bloomLayout->addWidget(more);
    body->addWidget(bloomControls);

    auto *glow = new ToggleRow(QStringLiteral("Glow"));
    glow->setHelp(QStringLiteral(
        "Added light, like the halo round a street lamp: the blurred highlights are added on top in linear light, so "
        "bright areas and what is near them get brighter. NegPy-style."));
    connect(glow, &ToggleRow::toggled, this, [editEffects](bool on) { editEffects([&](GlowLook &g) { g.glow.enabled = on; }); });
    body->addWidget(glow);
    QVBoxLayout *glowLayout = nullptr;
    QWidget *glowControls = group(glowLayout);
    glowLayout->addWidget(slider(QStringLiteral("Amount"), 0, 100, 30, {}, [](GlowLook &g) -> double & { return g.glow.amount; }));
    glowLayout->addWidget(slider(QStringLiteral("Size"), 0, 100, 40, {}, [](GlowLook &g) -> double & { return g.glow.size; }));
    glowLayout->addWidget(slider(QStringLiteral("Threshold"), 0, 100, 50, {},
                                 [](GlowLook &g) -> double & { return g.glow.threshold; },
                                 QStringLiteral("How bright an area must be to glow.")));
    body->addWidget(glowControls);

    auto *halation = new ToggleRow(QStringLiteral("Halation"));
    halation->setHelp(QStringLiteral(
        "Light scattered back off the film base around bright highlights, NegPy-style: red on colour film, whose "
        "red-sensitive layer sits nearest the base; grey in black and white."));
    connect(halation, &ToggleRow::toggled, this,
            [editEffects](bool on) { editEffects([&](GlowLook &g) { g.halation.enabled = on; }); });
    body->addWidget(halation);
    QVBoxLayout *halationLayout = nullptr;
    QWidget *halationControls = group(halationLayout);
    halationLayout->addWidget(slider(QStringLiteral("Amount"), 0, 100, 40, {},
                                     [](GlowLook &g) -> double & { return g.halation.amount; }));
    halationLayout->addWidget(slider(QStringLiteral("Size"), 0, 100, 52, {},
                                     [](GlowLook &g) -> double & { return g.halation.size; }));
    halationLayout->addWidget(slider(QStringLiteral("Threshold"), 0, 100, 83, {},
                                     [](GlowLook &g) -> double & { return g.halation.threshold; },
                                     QStringLiteral("How bright a highlight must be to halate.")));
    auto *haloOnly = new ToggleRow(QStringLiteral("Halo only"));
    haloOnly->setHelp(QStringLiteral("Keeps halation off the highlight itself, so it shows only as a ring around it."));
    connect(haloOnly, &ToggleRow::toggled, this,
            [editEffects](bool on) { editEffects([&](GlowLook &g) { g.halation.haloOnly = on; }); });
    halationLayout->addWidget(haloOnly);
    body->addWidget(halationControls);

    auto *reset = new LinkButton(QStringLiteral("Reset effects"));
    connect(reset, &QPushButton::clicked, this, [this] { m_doc.resetEffects(); });
    body->addWidget(reset);

    m_refreshers.push_back([=, this] {
        const GlowLook &g = m_doc.look().effects;
        bloom->setChecked(g.bloom.enabled);
        bloomControls->setVisible(g.bloom.enabled);
        blend->setCurrent(QString::fromStdString(g.bloom.blend));
        glow->setChecked(g.glow.enabled);
        glowControls->setVisible(g.glow.enabled);
        halation->setChecked(g.halation.enabled);
        halationControls->setVisible(g.halation.enabled);
        haloOnly->setChecked(g.halation.haloOnly);
    });
}

// MARK: - Log Input

void MotionInspector::buildInput()
{
    auto *section = new Section(QStringLiteral("Log Input"),
                                QStringLiteral("Read from the file's own tags. Override them only if a grey card does "
                                               "not land where it should."),
                                false);
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();

    auto *format = new Choice(QStringLiteral("Format"));
    format->setHelp(QStringLiteral(
        "Every curve but Apple Log is defined on the code values the camera records; Apple Log and Apple Log 2 "
        "(iPhone) on the 0–1 signal. Apple tags its log footage and a Sony camera names its curve in its own "
        "metadata. For a Lumix, Fujifilm, Canon or Insta360 file the camera's make decides: V-Log, F-Log2 where the "
        "camera has it (else F-Log), Canon Log 3, I-Log. Anything else is taken for N-Log. A choice here holds for "
        "this clip; the next one is read from its own tags."));
    connect(format, &Choice::chosen, this, [this](const QVariant &v) {
        edit([&](MotionLook &l) {
            if (v.toInt() < 0)
                l.input.format.reset();
            else
                l.input.format = LogFormat(v.toInt());
        });
    });
    body->addWidget(format);

    auto *levels = new Choice(QStringLiteral("Levels"));
    for (auto l : {InputSettings::Levels::Auto, InputSettings::Levels::Video, InputSettings::Levels::Full})
        levels->addItem(QString::fromStdString(levelsName(l)), int(l));
    connect(levels, &Choice::chosen, this,
            [this](const QVariant &v) { edit([&](MotionLook &l) { l.input.levels = InputSettings::Levels(v.toInt()); }); });
    body->addWidget(levels);

    auto *matrix = new Choice(QStringLiteral("Matrix"));
    for (auto m : {InputSettings::Matrix::Auto, InputSettings::Matrix::BT2020, InputSettings::Matrix::BT709,
                   InputSettings::Matrix::BT601})
        matrix->addItem(QString::fromStdString(matrixName(m)), int(m));
    connect(matrix, &Choice::chosen, this,
            [this](const QVariant &v) { edit([&](MotionLook &l) { l.input.matrix = InputSettings::Matrix(v.toInt()); }); });
    body->addWidget(matrix);

    auto *reading = monoNote();
    body->addWidget(reading);

    QString builtFor = QStringLiteral("none");
    m_refreshers.push_back([=, this]() mutable {
        const MotionLook &look = m_doc.look();
        section->setTitle(QStringLiteral("Log Input · ") + QString::fromStdString(logFormatShortName(m_doc.format())));
        // the file's own reading first, with what it was read from; made
        // again only for another clip
        const QString first = m_doc.clip() ? QStringLiteral("From the file (%1, %2)")
                                                 .arg(QString::fromStdString(logFormatShortName(m_doc.clip()->detection().first)),
                                                      QString::fromStdString(detectionBasisName(m_doc.clip()->detection().second)))
                                           : QStringLiteral("From the file");
        if (first != builtFor) {
            builtFor = first;
            format->clear();
            format->addItem(first, -1);
            for (LogFormat f : logFormats())
                format->addItem(QString::fromStdString(logFormatName(f)), int(f));
        }
        format->setCurrent(look.input.format ? int(*look.input.format) : -1);
        levels->setCurrent(int(look.input.levels));
        matrix->setCurrent(int(look.input.matrix));
        if (const auto &r = m_doc.reading()) {
            reading->setVisible(true);
            reading->setText(QStringLiteral("Reading %1 · %2-bit %3 · %4 range · %5")
                                 .arg(QString::fromStdString(logFormatShortName(r->format)))
                                 .arg(r->bitDepth)
                                 .arg(QString::fromStdString(chromaName(r->chroma)),
                                      r->fullRange ? QStringLiteral("full") : QStringLiteral("video"),
                                      QString::fromStdString(r->matrixName)));
        } else {
            reading->setVisible(false);
        }
    });
}

// MARK: - Export

void MotionInspector::buildExport()
{
    auto *section = new Section(QStringLiteral("Export"));
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();

    // the format is in the toolbar, beside Export, as in Stills
    auto *display = new Choice(QStringLiteral("For"));
    display->addItem(QStringLiteral("Windows displays"), int(LutOutput::SRGB));
    display->addItem(QStringLiteral("Apple displays"), int(LutOutput::AppleDisplay));
    display->addItem(QStringLiteral("Rec.709 broadcast"), int(LutOutput::BT1886));
    connect(display, &Choice::chosen, this, [this](const QVariant &v) {
        edit([&](MotionLook &l) { l.exportSettings.display = LutOutput(v.toInt()); });
    });
    body->addWidget(display);
    auto *explanation = note({});
    body->addWidget(explanation);

    auto *buttons = new QWidget;
    auto *row = new QHBoxLayout(buttons);
    row->setContentsMargins(0, 4, 0, 0);
    row->setSpacing(8);
    auto *exportButton = actionButton(QStringLiteral("Export…"),
                                      QStringLiteral("Every frame at full size, with the sound copied as it is "
                                                     "(Ctrl+E)"));
    auto *lutButton = actionButton(
        QStringLiteral("Export LUT…"),
        QStringLiteral("The print as a 65-point .cube for DaVinci Resolve and other editors, Rec.709 (BT.1886) out, "
                       "taking the clip's log the way Resolve reads it: N-Log and V-Log as data levels for a "
                       "full-range file like a Zf's and video levels otherwise, Apple Log on its signal. Effects are "
                       "not in it."));
    connect(exportButton, &QPushButton::clicked, this, &MotionInspector::exportRequested);
    connect(lutButton, &QPushButton::clicked, this, &MotionInspector::exportLUTRequested);
    row->addWidget(exportButton);
    row->addWidget(lutButton);
    row->addStretch(1);
    body->addWidget(buttons);

    m_refreshers.push_back([=, this] {
        const LutOutput d = m_doc.look().exportSettings.display;
        display->setCurrent(int(d));
        switch (d) {
        case LutOutput::SRGB:
            explanation->setText(QStringLiteral(
                "Looks as it does here in Windows' Media Player, in browsers and in most players and editors on a PC, "
                "which show Rec.709 video on the screen as it is: written with the sRGB curve the screen decodes. On "
                "a Mac, iPhone or iPad it will look a little lighter."));
            break;
        case LutOutput::AppleDisplay:
            explanation->setText(QStringLiteral(
                "Looks as it does here in QuickTime, Photos and Safari on a Mac, iPhone or iPad, which decode Rec.709 "
                "video with a 1.961 power. On a PC it will look a little darker."));
            break;
        default:
            explanation->setText(QStringLiteral(
                "For a BT.1886 reference monitor or TV, which decode with a 2.4 power. On a computer it will look "
                "lighter and flatter than here."));
            break;
        }
        exportButton->setEnabled(m_doc.clip().has_value() && !m_doc.isExporting());
    });
}
