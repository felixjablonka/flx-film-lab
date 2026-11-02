#include "Inspector.h"

#include "AppSettings.h"
#include "CurvePlot.h"
#include "Document.h"
#include "Theme.h"
#include "Widgets.h"

#include <QCheckBox>
#include <QDoubleValidator>
#include <QMouseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace {

// A gradient's or a mask's row: its switch, its name, a delete button, and
// under them, while it is selected, its controls. A click on the row selects it.
class ItemRow : public QWidget
{
public:
    ItemRow(bool selected, std::function<void()> select)
        : m_select(std::move(select))
    {
        setAttribute(Qt::WA_StyledBackground);
        setObjectName(QStringLiteral("itemRow"));
        setStyleSheet(selected ? QStringLiteral("#itemRow { background: rgba(%1, %2, %3, 26); border-radius: 4px; }")
                                     .arg(theme::accent().red()).arg(theme::accent().green()).arg(theme::accent().blue())
                               : QStringLiteral("#itemRow { background: transparent; }"));
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(6, 6, 6, 6);
        m_layout->setSpacing(4);
    }
    QVBoxLayout *layout() const { return m_layout; }

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton)
            m_select();
    }

private:
    std::function<void()> m_select;
    QVBoxLayout *m_layout;
};

QToolButton *deleteButton(const QString &help)
{
    auto *b = new QToolButton;
    b->setText(QStringLiteral("✕"));
    b->setToolTip(help);
    b->setAutoRaise(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral("QToolButton { color: %1; border: none; padding: 0 2px; }"
                                    "QToolButton:hover { color: %2; }")
                         .arg(theme::tertiary().name(), theme::primary().name()));
    return b;
}

QPushButton *addButton(const QString &text)
{
    auto *b = new QPushButton(text);
    b->setFont(theme::small());
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral("QPushButton { background: #333; color: #d0d0d0; border: 1px solid #444;"
                                    " border-radius: 4px; padding: 2px 8px; }"
                                    "QPushButton:hover { background: #3b3b3b; }"
                                    "QPushButton:disabled { color: #666; }"));
    return b;
}

QColor tint(int r, int g, int b)
{
    return QColor(r, g, b);
}

QString filterFactor(float factor)
{
    return QStringLiteral("%1×, %2 stops").arg(factor, 0, 'f', 1).arg(std::log2(std::max(factor, 1.0f)), 0, 'f', 1);
}

} // namespace

Inspector::Inspector(Document &document, QWidget *parent)
    : QScrollArea(parent), m_doc(document)
{
    m_vocabulary = appSettings().value("vocabulary").toString() == QStringLiteral("Darkroom") ? Vocabulary::Darkroom
                                                                                             : Vocabulary::Photographic;
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

    buildCrop();
    buildStocks();
    buildExposure();
    buildGradients();
    buildPrint();
    buildShadowsHighlights();
    buildColour();
    buildMasks();
    buildEffects();
    buildFilmCharacter();
    buildOutput();
    layout->addStretch(1);

    connect(&m_doc, &Document::changed, this, &Inspector::refresh);
    refresh();
}

void Inspector::edit(const std::function<void(Recipe &)> &change)
{
    Recipe r = m_doc.recipe();
    change(r);
    m_doc.setRecipe(r);
}

void Inspector::refresh()
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

ParamSlider *Inspector::specSlider(const std::function<ControlSpec()> &spec, std::function<double()> physical,
                                   std::function<void(double)> setPhysical, std::function<double()> defaultPhysical)
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

ParamSlider *Inspector::slider(const QString &title, double low, double high, double defaultValue, int decimals,
                               const QString &unit, double Recipe::*value, const QString &help)
{
    auto *s = new ParamSlider(title, low, high, defaultValue, decimals, unit);
    s->setHelp(help);
    connect(s, &ParamSlider::valueChanged, this, [this, value](double v) { edit([&](Recipe &r) { r.*value = v; }); });
    m_refreshers.push_back([this, s, value] { s->setValue(m_doc.recipe().*value); });
    return s;
}

ToggleRow *Inspector::toggle(const QString &title, bool Recipe::*value, const QString &help)
{
    auto *t = new ToggleRow(title);
    t->setHelp(help);
    connect(t, &ToggleRow::toggled, this, [this, value](bool on) { edit([&](Recipe &r) { r.*value = on; }); });
    m_refreshers.push_back([this, t, value] { t->setChecked(m_doc.recipe().*value); });
    return t;
}

// MARK: - Crop & Straighten

void Inspector::buildCrop()
{
    auto *section = new Section(QStringLiteral("Crop & Straighten"),
                                QStringLiteral("The print's shape. The film is exposed and developed whole; the meters "
                                               "read only what the crop keeps."));
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();
    auto button = [](const QString &text, const QString &help, bool checkable = false) {
        auto *b = new QToolButton;
        b->setText(text);
        b->setToolTip(help);
        b->setCheckable(checkable);
        b->setCursor(Qt::PointingHandCursor);
        b->setFont(theme::label());
        b->setStyleSheet(QStringLiteral("QToolButton { background: #333; color: #d8d8d8; border: 1px solid #444;"
                                        " border-radius: 5px; padding: 2px 8px; }"
                                        "QToolButton:checked { background: %1; color: white; border-color: %1; }"
                                        "QToolButton:hover:!checked { background: #3b3b3b; }"
                                        "QToolButton:disabled { color: #666; }").arg(theme::accent().name()));
        return b;
    };

    auto *first = new QWidget;
    auto *firstRow = new QHBoxLayout(first);
    firstRow->setContentsMargins(0, 1, 0, 1);
    firstRow->setSpacing(6);
    auto *crop = button(QStringLiteral("Crop"),
                        QStringLiteral("Show the whole frame with the crop on it, to drag its corners and edges "
                                       "(Ctrl+R)."), true);
    connect(crop, &QToolButton::clicked, this, [this](bool on) { m_doc.setCropping(on); });
    auto *aspect = new Choice({});
    for (CropAspect a : cropAspects())
        aspect->addItem(QString::fromStdString(cropAspectName(a)), int(a));
    aspect->setHelp(QStringLiteral("The crop's proportions. Original keeps the frame's own; Free has none."));
    connect(aspect, &Choice::chosen, this, [this](const QVariant &v) { m_doc.setCropAspect(CropAspect(v.toInt())); });
    auto *swap = button(QStringLiteral("Swap"), QStringLiteral("Stand the crop up, or lay it down."));
    connect(swap, &QToolButton::clicked, this, [this] { m_doc.swapCropOrientation(); });
    firstRow->addWidget(crop);
    firstRow->addWidget(aspect, 1);
    firstRow->addWidget(swap);
    body->addWidget(first);

    // a custom aspect's sides, as the crop stands
    auto *custom = new QWidget;
    auto *customRow = new QHBoxLayout(custom);
    customRow->setContentsMargins(0, 1, 0, 1);
    customRow->setSpacing(4);
    auto side = [](void) {
        auto *field = new QLineEdit;
        field->setFixedWidth(56);
        field->setFont(theme::label());
        auto *validator = new QDoubleValidator(0.01, 100000, 3, field);
        validator->setNotation(QDoubleValidator::StandardNotation);
        field->setValidator(validator);
        return field;
    };
    auto *customWidth = side(), *customHeight = side();
    auto *colon = new QLabel(QStringLiteral(":"));
    customRow->addWidget(customWidth);
    customRow->addWidget(colon);
    customRow->addWidget(customHeight);
    customRow->addStretch(1);
    auto setCustom = [this, customWidth, customHeight] {
        const double w = QLocale().toDouble(customWidth->text()), h = QLocale().toDouble(customHeight->text());
        if (w > 0 && h > 0)
            m_doc.setCustomAspect(w, h);
    };
    connect(customWidth, &QLineEdit::editingFinished, this, setCustom);
    connect(customHeight, &QLineEdit::editingFinished, this, setCustom);
    body->addWidget(custom);

    auto *second = new QWidget;
    auto *secondRow = new QHBoxLayout(second);
    secondRow->setContentsMargins(0, 1, 0, 1);
    secondRow->setSpacing(6);
    auto *left = button(QStringLiteral("↺"), QStringLiteral("Turn a quarter to the left (Ctrl+[)."));
    auto *right = button(QStringLiteral("↻"), QStringLiteral("Turn a quarter to the right (Ctrl+])."));
    auto *mirror = button(QStringLiteral("↔"), QStringLiteral("Mirror left to right."));
    auto *mirrorUp = button(QStringLiteral("↕"), QStringLiteral("Mirror top to bottom."));
    auto *level = button(QStringLiteral("Level"),
                         QStringLiteral("Draw a line on the picture along a horizon, or along an upright, and the "
                                        "picture is levelled to it."), true);
    connect(left, &QToolButton::clicked, this, [this] { m_doc.turn(false); });
    connect(right, &QToolButton::clicked, this, [this] { m_doc.turn(true); });
    connect(mirror, &QToolButton::clicked, this, [this] { m_doc.flip(true); });
    connect(mirrorUp, &QToolButton::clicked, this, [this] { m_doc.flip(false); });
    connect(level, &QToolButton::clicked, this, [this](bool on) { m_doc.setLevelling(on); });
    for (QToolButton *b : {left, right, mirror, mirrorUp})
        secondRow->addWidget(b);
    secondRow->addStretch(1);
    secondRow->addWidget(level);
    body->addWidget(second);

    auto *straighten = new ParamSlider(QStringLiteral("Straighten"), -FrameGeometry::maxStraighten,
                                       FrameGeometry::maxStraighten, 0, 1, QStringLiteral("°"));
    straighten->setHelp(QStringLiteral(
        "Levels the picture, turning it clockwise for positive values. The crop shrinks as far as it must to stay "
        "inside the picture, and grows back when you level the other way."));
    connect(straighten, &ParamSlider::valueChanged, this, [this](double v) { m_doc.setStraighten(v); });
    body->addWidget(straighten);

    auto *reset = new LinkButton(QStringLiteral("Reset crop"));
    connect(reset, &QPushButton::clicked, this, [this] { m_doc.resetGeometry(); });
    body->addWidget(reset);

    m_refreshers.push_back([=, this] {
        const FrameGeometry &g = m_doc.recipe().geometry;
        const bool image = m_doc.hasImage();
        crop->setChecked(m_doc.isCropping());
        level->setChecked(m_doc.isLevelling());
        aspect->setCurrent(int(g.aspect));
        swap->setEnabled(image && g.aspect != CropAspect::Free);
        custom->setVisible(g.aspect == CropAspect::Custom);
        if (!customWidth->hasFocus())
            customWidth->setText(QLocale().toString(g.customWidth, 'g', 6));
        if (!customHeight->hasFocus())
            customHeight->setText(QLocale().toString(g.customHeight, 'g', 6));
        for (QWidget *w : {static_cast<QWidget *>(crop), static_cast<QWidget *>(left), static_cast<QWidget *>(right),
                           static_cast<QWidget *>(mirror), static_cast<QWidget *>(mirrorUp),
                           static_cast<QWidget *>(level)})
            w->setEnabled(image);
        straighten->setValue(g.straighten);
        reset->setEnabled(!g.isIdentity());
    });
}

// MARK: - Film & Paper

void Inspector::buildStocks()
{
    auto *section = new Section(QStringLiteral("Film & Paper"),
                                QStringLiteral("Spectral profiles derived from manufacturer datasheets."));
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();

    auto *film = new StockPicker(QStringLiteral("Film"), stockGroups(m_doc.packs().filmEntries(), true));
    connect(film, &Choice::chosen, this, [this](const QVariant &v) { m_doc.selectFilm(v.toString().toStdString()); });
    body->addWidget(film);

    auto *format = new Segmented({QStringLiteral("35 mm"), QStringLiteral("120")});
    format->setHelp(QStringLiteral(
        "Sets the frame size, and with it the pixel pitch that scales grain, halation and coupler diffusion: 36 x 24 "
        "mm on 35 mm, 84 x 56 mm for a 6x9 on 120. The picture is laid on the frame as large as it fits, so a 4:3 "
        "one spans the short side. The same emulsion on 120 shows visibly finer grain than on 35 mm, because the "
        "frame is enlarged less."));
    connect(format, &Segmented::chosen, this,
            [this](int i) { edit([&](Recipe &r) { r.filmFormat = i == 0 ? "35 mm" : "120"; }); });
    body->addWidget(labelled(QStringLiteral("Film format"), format));

    auto *sharpness = new Segmented({QStringLiteral("Film"), QStringLiteral("Camera")});
    sharpness->setHelp(QStringLiteral(
        "Film: detail as the film renders it. Scattering inside the emulsion softens the finest detail and "
        "development's edge effects lift coarser edges, so on 35 mm a high-resolution file's finest detail softens "
        "as the film's would. Camera: the camera file's own detail, with the film's colour, tone, halation and "
        "grain."));
    connect(sharpness, &Segmented::chosen, this, [this](int i) {
        edit([&](Recipe &r) { r.cameraSharpness = i == 1 ? std::optional<bool>(true) : std::nullopt; });
    });
    body->addWidget(labelled(QStringLiteral("Sharpness"), sharpness));

    auto *onPaper = new ToggleRow(QStringLiteral("Print on paper"));
    onPaper->setHelp(QStringLiteral(
        "Off reads the developed film straight off a light table, the way a slide is viewed. Slide stocks default "
        "to that: printing a positive onto RA-4 paper inverts it, which is what a lab would really produce and "
        "almost never what you want."));
    connect(onPaper, &ToggleRow::toggled, this, [this](bool on) { edit([&](Recipe &r) { r.scanFilm = !on; }); });
    body->addWidget(onPaper);

    auto *paper = new StockPicker(QStringLiteral("Paper"), stockGroups(m_doc.packs().paperEntries(), false));
    connect(paper, &Choice::chosen, this,
            [this](const QVariant &v) { edit([&](Recipe &r) { r.printStock = v.toString().toStdString(); }); });
    body->addWidget(paper);

    auto *reference = new LinkButton(QStringLiteral("Use this film's reference paper"));
    connect(reference, &QPushButton::clicked, this, [this] {
        if (const ProfilePack *f = m_doc.filmPack())
            edit([&](Recipe &r) { r.printStock = f->targetPrint; });
    });
    body->addWidget(reference);

    m_refreshers.push_back([=, this] {
        const Recipe &r = m_doc.recipe();
        film->setCurrent(QString::fromStdString(r.filmStock));
        format->setCurrent(r.filmFormat == "35 mm" ? 0 : 1);
        sharpness->setCurrent(r.keepsCameraSharpness() ? 1 : 0);
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

void Inspector::buildExposure()
{
    auto *section = new Section(QStringLiteral("Negative Exposure"), QStringLiteral("How much light the film received."));
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();

    auto *autoExposure = new ToggleRow(QStringLiteral("Auto exposure"));
    autoExposure->setHelp(QStringLiteral(
        "Meters the frame and sets the Exposure slider to it: the median is pulled most of the way to an 18% grey, "
        "but never so far that the brightest tones run past the stock's highlight headroom. Move the slider to take "
        "over from there; that turns the auto off."));
    // turning an auto off by hand keeps the slider where the meter had it
    connect(autoExposure, &ToggleRow::toggled, this, [this](bool on) {
        edit([&](Recipe &r) {
            if (!on && r.autoExposure && m_doc.metered().exposureEV)
                r.exposureEV = *m_doc.metered().exposureEV;
            r.autoExposure = on;
        });
    });
    body->addWidget(autoExposure);

    auto *exposure = new ParamSlider(QStringLiteral("Exposure"), -5, 5, 0, 2, QStringLiteral(" EV"));
    exposure->setHelp(QStringLiteral("Moves the scene along the film's characteristic curve. Overexposing a colour "
                                     "negative lifts shadow detail and desaturates; underexposing crushes it."));
    // moving a metered slider stores the value and turns the auto off
    connect(exposure, &ParamSlider::valueChanged, this, [this](double v) {
        edit([&](Recipe &r) {
            r.exposureEV = v;
            r.autoExposure = false;
        });
    });
    body->addWidget(exposure);

    auto *filter = new Choice(QStringLiteral("Filter"));
    connect(filter, &Choice::chosen, this, [this](const QVariant &v) {
        edit([&](Recipe &r) {
            if (v.toString().isEmpty())
                r.lensFilter.reset();
            else
                r.lensFilter = v.toString().toStdString();
        });
    });
    body->addWidget(filter);

    auto *wb = new Choice(QStringLiteral("White balance"));
    for (WhiteBalance w : kWhiteBalances)
        wb->addItem(whiteBalanceLabel(w), int(w));
    wb->setHelp(QStringLiteral("The RAW is decoded for this illuminant, so the film sees the light the way it would "
                               "have on set. Film reference uses whatever the loaded stock is balanced for."));
    connect(wb, &Choice::chosen, this, [this](const QVariant &v) { m_doc.setWhiteBalance(WhiteBalance(v.toInt())); });
    body->addWidget(wb);

    auto *summary = new QLabel;
    summary->setFont(theme::mono(10));
    body->addWidget(summary);

    auto *temperature = new ParamSlider(QStringLiteral("Temperature"), 2000, 12000, 5500, 0, QStringLiteral(" K"));
    connect(temperature, &ParamSlider::valueChanged, this, [this](double v) { m_doc.setCustomTemperature(v); });
    body->addWidget(temperature);
    auto *tintSlider = new ParamSlider(QStringLiteral("Tint"), -150, 150, 0, 0);
    connect(tintSlider, &ParamSlider::valueChanged, this, [this](double v) { m_doc.setCustomTint(v); });
    body->addWidget(tintSlider);

    auto *reset = new LinkButton(QStringLiteral("Reset exposure"));
    connect(reset, &QPushButton::clicked, this, [this] { m_doc.resetExposure(); });
    body->addWidget(reset);

    QString filtersOf;
    m_refreshers.push_back([=, this]() mutable {
        const Recipe &r = m_doc.recipe();
        const ProfilePack *film = m_doc.filmPack();
        section->setTitle(film && film->isPositive() ? QStringLiteral("Exposure") : QStringLiteral("Negative Exposure"));
        autoExposure->setChecked(r.autoExposure);
        exposure->setValue(r.autoExposure && m_doc.metered().exposureEV ? *m_doc.metered().exposureEV : r.exposureEV);

        const bool filters = film && !film->lensFilters.empty();
        filter->setVisible(filters);
        if (filters && filtersOf != QString::fromStdString(film->stock)) {
            filtersOf = QString::fromStdString(film->stock);
            filter->clear();
            filter->addItem(QStringLiteral("None"), QString());
            bool wratten = false;
            for (const LensFilterRef &f : film->lensFilters) {
                filter->addItem(QString::fromStdString(f.designation()) + QStringLiteral(" · ") + filterFactor(f.factor),
                                QString::fromStdString(f.id));
                wratten = wratten || !f.wratten.empty();
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

        wb->setCurrent(int(m_doc.whiteBalance()));
        summary->setText(m_doc.whiteBalanceSummary());
        QPalette p = summary->palette();
        p.setColor(QPalette::WindowText, m_doc.isDecoding() ? QColor(255, 159, 10) : theme::tertiary());
        summary->setPalette(p);
        const bool custom = m_doc.whiteBalance() == WhiteBalance::Custom;
        temperature->setVisible(custom);
        tintSlider->setVisible(custom);
        temperature->setValue(m_doc.customTemperature());
        tintSlider->setValue(m_doc.customTint());
    });
}

// MARK: - Graduated Exposure

// Capture, not darkroom: the gradients change what the film receives, which is
// why they sit with the exposure and not with Dodge & Burn.
void Inspector::buildGradients()
{
    auto *section = new Section(QStringLiteral("Graduated Exposure"),
                                QStringLiteral("Exposure compensation that fades across the frame, given to the film "
                                               "when the picture is taken."));
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();
    auto *empty = note(QStringLiteral(
        "Add a gradient, then drag its handles on the image: the full compensation is at the solid line, none past "
        "the dashed one, and the middle handle moves both. Gradients stack, each adding its own exposure where it "
        "covers."));
    body->addWidget(empty);
    auto *rows = new QWidget;
    auto *rowsLayout = new QVBoxLayout(rows);
    rowsLayout->setContentsMargins(0, 0, 0, 0);
    rowsLayout->setSpacing(2);
    body->addWidget(rows);
    auto *add = addButton(QStringLiteral("Add gradient"));
    connect(add, &QPushButton::clicked, this, [this] { m_doc.addGradient(); });
    body->addWidget(add, 0, Qt::AlignLeft);

    // the rows, built again when the gradients or the selection change
    auto built = std::make_shared<QString>();
    auto rowRefreshers = std::make_shared<std::vector<std::function<void()>>>();
    m_refreshers.push_back([=, this] {
        const Recipe &r = m_doc.recipe();
        empty->setVisible(r.gradients.empty());
        add->setVisible(int(r.gradients.size()) < kMaxExposureGradients);
        QString signature = QString::fromStdString(m_doc.selected());
        for (const ExposureGradient &g : r.gradients)
            signature += QLatin1Char('|') + QString::fromStdString(g.id);
        if (signature != *built) {
            *built = signature;
            rowRefreshers->clear();
            while (QLayoutItem *item = rowsLayout->takeAt(0)) {
                delete item->widget();
                delete item;
            }
            for (const ExposureGradient &g : r.gradients) {
                const std::string id = g.id;
                const bool selected = id == m_doc.selected();
                auto *row = new ItemRow(selected, [this, id] { m_doc.setSelected(id); });
                auto *head = new QHBoxLayout;
                head->setSpacing(5);
                auto *enabled = new QCheckBox;
                auto *name = new QLabel(QString::fromStdString(g.name));
                QFont f = theme::label();
                f.setWeight(selected ? QFont::DemiBold : QFont::Normal);
                name->setFont(f);
                auto *ev = new QLabel;
                ev->setFont(theme::mono(10));
                QPalette p = ev->palette();
                p.setColor(QPalette::WindowText, theme::tertiary());
                ev->setPalette(p);
                auto *remove = deleteButton(QStringLiteral("Delete this gradient."));
                head->addWidget(enabled);
                head->addWidget(name);
                head->addStretch(1);
                head->addWidget(ev);
                head->addWidget(remove);
                row->layout()->addLayout(head);
                auto edit = [this, id](const std::function<void(ExposureGradient &)> &change) {
                    Recipe r = m_doc.recipe();
                    for (ExposureGradient &x : r.gradients)
                        if (x.id == id)
                            change(x);
                    m_doc.setRecipe(r);
                };
                connect(enabled, &QCheckBox::toggled, this, [edit](bool on) { edit([&](ExposureGradient &x) { x.enabled = on; }); });
                connect(remove, &QToolButton::clicked, this, [this, id] { m_doc.removeGradient(id); });
                ParamSlider *exposure = nullptr;
                if (selected) {
                    exposure = new ParamSlider(QStringLiteral("Exposure"), -4, 4, 0, 2, QStringLiteral(" EV"));
                    exposure->setHelp(QStringLiteral(
                        "Exposure on the covered side, like the Exposure slider: positive gives the film more light, "
                        "negative less. Where gradients overlap, their exposures add."));
                    connect(exposure, &ParamSlider::valueChanged, this,
                            [edit](double v) { edit([&](ExposureGradient &x) { x.exposureEV = v; }); });
                    row->layout()->addWidget(exposure);
                }
                rowsLayout->addWidget(row);
                // shown now: one added while its section is on screen was not
                row->show();
                rowRefreshers->push_back([this, id, row, enabled, ev, exposure] {
                    for (const ExposureGradient &x : m_doc.recipe().gradients) {
                        if (x.id != id)
                            continue;
                        const QSignalBlocker block(enabled);
                        enabled->setChecked(x.enabled);
                        const QString text = QStringLiteral("%1%2 EV")
                                                 .arg(x.exposureEV >= 0 ? "+" : "")
                                                 .arg(x.exposureEV + 0.0, 0, 'f', 2);
                        if (text != ev->text()) {
                            ev->setText(text);
                            row->layout()->invalidate();
                            row->layout()->activate();
                        }
                        if (exposure)
                            exposure->setValue(x.exposureEV);
                    }
                });
            }
        }
        for (const auto &refresh : *rowRefreshers)
            refresh();
    });
}

// MARK: - The Print

void Inspector::buildPrint()
{
    auto *section = new Section(QStringLiteral("The Print"),
                                QStringLiteral("How bright the print is, and how much contrast it carries."));
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();

    // the resolved recipe carries what the meters chose, so the curve shows
    // the print actually being made
    auto *plot = new CurvePlot;
    body->addWidget(plot);
    body->addSpacing(4);

    auto *autoBrightness = new ToggleRow(QStringLiteral("Auto brightness"));
    autoBrightness->setHelp(QStringLiteral(
        "Meters the textured parts of the frame and sets Brightness to move the midtones partway toward a mid-grey "
        "print, so a deliberately low-key or high-key shot keeps its mood. Move the slider to take over from there; "
        "that turns the auto off."));
    connect(autoBrightness, &ToggleRow::toggled, this, [this](bool on) {
        edit([&](Recipe &r) {
            if (!on && r.autoDensity && m_doc.metered().brightness)
                r.brightness = *m_doc.metered().brightness;
            r.autoDensity = on;
        });
    });
    body->addWidget(autoBrightness);
    body->addWidget(specSlider(
        [] { return controls::brightness; },
        [this] {
            const Recipe &r = m_doc.recipe();
            return r.autoDensity && m_doc.metered().brightness ? *m_doc.metered().brightness : r.brightness;
        },
        [this](double v) {
            edit([&](Recipe &r) {
                r.brightness = v;
                r.autoDensity = false;
            });
        }));
    body->addWidget(specSlider([] { return controls::printExposure; }, [this] { return m_doc.recipe().density; },
                               [this](double v) { edit([&](Recipe &r) { r.density = v; }); }));

    auto *autoContrast = new ToggleRow(QStringLiteral("Auto contrast"));
    autoContrast->setHelp(QStringLiteral(
        "Fits the frame's textured range onto the paper and sets Contrast to it: softens until the darkest and "
        "lightest textured tones still hold detail, and hardens a flat scene only partway. Move the slider to take "
        "over from there; that turns the auto off."));
    connect(autoContrast, &ToggleRow::toggled, this, [this](bool on) {
        edit([&](Recipe &r) {
            if (!on && r.autoGrade && m_doc.metered().grade)
                r.grade = *m_doc.metered().grade;
            r.autoGrade = on;
        });
    });
    body->addWidget(autoContrast);
    body->addWidget(specSlider(
        [this] {
            const ProfilePack *film = m_doc.filmPack();
            return controls::contrast(film ? film->softestGrade() : kGradeMax);
        },
        [this] {
            const Recipe &r = m_doc.recipe();
            return r.autoGrade && m_doc.metered().grade ? *m_doc.metered().grade : r.grade;
        },
        [this](double v) {
            edit([&](Recipe &r) {
                r.grade = v;
                r.autoGrade = false;
            });
        },
        [this] { return m_doc.defaultGrade(); }));

    auto *split = note({});
    QFont f = theme::small();
    f.setWeight(QFont::Medium);
    split->setFont(f);
    body->addWidget(split);
    body->addWidget(specSlider([] { return controls::shadowContrast; }, [this] { return m_doc.recipe().shadowGrade; },
                               [this](double v) { edit([&](Recipe &r) { r.shadowGrade = v; }); }));
    body->addWidget(specSlider([] { return controls::highlightContrast; },
                               [this] { return m_doc.recipe().highlightGrade; },
                               [this](double v) { edit([&](Recipe &r) { r.highlightGrade = v; }); }));

    m_refreshers.push_back([=, this] {
        const Recipe &r = m_doc.recipe();
        autoBrightness->setChecked(r.autoDensity);
        autoContrast->setChecked(r.autoGrade);
        split->setText(m_vocabulary == Vocabulary::Photographic ? QStringLiteral("Split contrast")
                                                                : QStringLiteral("Split grade"));
        plot->show(m_doc.printsOnPaper() ? m_doc.paperPack() : nullptr, m_doc.resolvedRecipe().value_or(r),
                   m_doc.packs().shared());
    });
}

// MARK: - Shadows & Highlights

void Inspector::buildShadowsHighlights()
{
    auto *section = new Section(
        QStringLiteral("Shadows & Highlights"),
        QStringLiteral("Lighten or darken one half of the print without touching mid-grey, then set how each end "
                       "rolls off."));
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();
    auto recipeValue = [this](double Recipe::*v) { return [this, v] { return m_doc.recipe().*v; }; };
    auto setRecipe = [this](double Recipe::*v) { return [this, v](double x) { edit([&](Recipe &r) { r.*v = x; }); }; };

    body->addWidget(specSlider([] { return controls::shadows; }, recipeValue(&Recipe::shadows), setRecipe(&Recipe::shadows)));
    body->addWidget(specSlider([] { return controls::highlights; }, recipeValue(&Recipe::highlights),
                               setRecipe(&Recipe::highlights)));
    body->addWidget(hairline());
    body->addWidget(specSlider([] { return controls::blacks; }, recipeValue(&Recipe::toe), setRecipe(&Recipe::toe)));
    body->addWidget(specSlider([] { return controls::blacksRange; }, recipeValue(&Recipe::toeWidth),
                               setRecipe(&Recipe::toeWidth), [] { return 1.0; }));
    body->addWidget(specSlider([] { return controls::whites; }, recipeValue(&Recipe::shoulder), setRecipe(&Recipe::shoulder)));
    body->addWidget(specSlider([] { return controls::whitesRange; }, recipeValue(&Recipe::shoulderWidth),
                               setRecipe(&Recipe::shoulderWidth), [] { return 1.0; }));
    body->addWidget(specSlider([] { return controls::midtoneContrast; }, recipeValue(&Recipe::snap),
                               setRecipe(&Recipe::snap)));

    auto *crossover = new Disclosure(QStringLiteral("Colour crossover"));
    crossover->body()->addWidget(note(QStringLiteral(
        "Each dye layer has its own curve. A filter can only shift a whole layer in parallel, so it cannot fix "
        "crossover: shadows casting one colour and highlights the complement. These can.")));
    const QStringList rgb{QStringLiteral("R"), QStringLiteral("G"), QStringLiteral("B")};
    const QList<QColor> rgbTints{tint(255, 69, 58), tint(48, 209, 88), tint(10, 132, 255)};
    struct Trim {
        RGBTrim Recipe::*value;
        double range;
        QString photographic, darkroom;
    };
    const Trim trims[] = {{&Recipe::gradeTrim, 30, QStringLiteral("Contrast"), QStringLiteral("Grade trim")},
                          {&Recipe::toeTrim, 1, QStringLiteral("Blacks"), QStringLiteral("Toe trim")},
                          {&Recipe::shoulderTrim, 1, QStringLiteral("Whites"), QStringLiteral("Shoulder trim")},
                          {&Recipe::snapTrim, 0.5, QStringLiteral("Midtones"), QStringLiteral("Snap trim")}};
    for (const Trim &t : trims) {
        auto *row = new TrimRow(t.photographic, rgb, rgbTints, -t.range, t.range);
        connect(row, &TrimRow::valueChanged, this, [this, value = t.value](int i, double v) {
            edit([&](Recipe &r) {
                RGBTrim &trim = r.*value;
                (i == 0 ? trim.red : i == 1 ? trim.green : trim.blue) = v;
            });
        });
        crossover->body()->addWidget(row);
        m_refreshers.push_back([=, this] {
            const RGBTrim &trim = m_doc.recipe().*(t.value);
            row->setTitle(m_vocabulary == Vocabulary::Photographic ? t.photographic : t.darkroom);
            row->setValues(trim.red, trim.green, trim.blue);
        });
    }
    body->addWidget(crossover);

    auto *reset = new LinkButton(QStringLiteral("Reset tone"));
    connect(reset, &QPushButton::clicked, this, [this] { m_doc.resetTone(); });
    body->addWidget(reset);
    m_refreshers.push_back([=, this] { crossover->setVisible(!m_doc.printsInSilver()); });
}

// MARK: - Colour

void Inspector::buildColour()
{
    auto *section = new Section(QStringLiteral("Colour"));
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();

    auto *temperature = specSlider([] { return controls::temperature; }, [this] { return m_doc.recipe().filterShiftY; },
                                   [this](double v) { edit([&](Recipe &r) { r.filterShiftY = v; }); });
    auto *tintSlider = specSlider([] { return controls::tint; }, [this] { return m_doc.recipe().filterShiftM; },
                                  [this](double v) { edit([&](Recipe &r) { r.filterShiftM = v; }); });
    body->addWidget(temperature);
    body->addWidget(tintSlider);

    const QStringList cmy{QStringLiteral("C"), QStringLiteral("M"), QStringLiteral("Y")};
    const QList<QColor> cmyTints{tint(50, 173, 230), tint(255, 51, 179), tint(255, 214, 10)};
    std::vector<TrimRow *> regional;
    for (const auto &[title, value, help] :
         {std::tuple{QStringLiteral("Shadow colour"), &Recipe::shadowCMY,
                     QStringLiteral("Colour correction weighted to the shadows only, in Kodak CC units.")},
          std::tuple{QStringLiteral("Highlight colour"), &Recipe::highlightCMY,
                     QStringLiteral("Colour correction weighted to the highlights only, in Kodak CC units.")}}) {
        auto *row = new TrimRow(title, cmy, cmyTints, -20, 20);
        row->setHelp(help);
        connect(row, &TrimRow::valueChanged, this, [this, value](int i, double v) {
            edit([&](Recipe &r) {
                CMYOffset &o = r.*value;
                (i == 0 ? o.cyan : i == 1 ? o.magenta : o.yellow) = v;
            });
        });
        body->addWidget(row);
        regional.push_back(row);
        m_refreshers.push_back([this, row, value] {
            const CMYOffset &o = m_doc.recipe().*value;
            row->setValues(o.cyan, o.magenta, o.yellow);
        });
    }
    body->addWidget(specSlider([] { return controls::highlightProtection; },
                               [this] { return m_doc.recipe().preflashExposure; },
                               [this](double v) { edit([&](Recipe &r) { r.preflashExposure = v; }); }));

    auto *reset = new LinkButton(QStringLiteral("Reset colour"));
    connect(reset, &QPushButton::clicked, this, [this] { m_doc.resetColour(); });
    body->addWidget(reset);

    m_refreshers.push_back([=, this] {
        const bool silver = m_doc.printsInSilver();
        section->setSubtitle(silver ? QStringLiteral("A black-and-white paper prints in silver, which has no colour to "
                                                     "balance. Pre-flash still holds the highlights.")
                                    : QStringLiteral("Overall balance moves the enlarger's real filters. The regional "
                                                     "pair works in density, which a filter cannot do."));
        temperature->setVisible(!silver);
        tintSlider->setVisible(!silver);
        for (TrimRow *row : regional)
            row->setVisible(!silver);
    });
}

// MARK: - Dodge & Burn

void Inspector::buildMasks()
{
    auto *section = new Section(QStringLiteral("Dodge & Burn"),
                                QStringLiteral("Masks change print exposure, so they roll into paper black the way a "
                                               "real burn does."));
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();
    auto *empty = note(QStringLiteral("Add a mask, then drag on the image to place its points."));
    body->addWidget(empty);
    auto *rows = new QWidget;
    auto *rowsLayout = new QVBoxLayout(rows);
    rowsLayout->setContentsMargins(0, 0, 0, 0);
    rowsLayout->setSpacing(2);
    body->addWidget(rows);
    auto *adds = new QWidget;
    auto *addsRow = new QHBoxLayout(adds);
    addsRow->setContentsMargins(0, 4, 0, 0);
    addsRow->setSpacing(6);
    for (const auto &[label, shape] : {std::pair{QStringLiteral("Polygon"), MaskShape::Polygon},
                                       std::pair{QStringLiteral("Oval"), MaskShape::Oval},
                                       std::pair{QStringLiteral("Gradient"), MaskShape::Gradient}}) {
        auto *b = addButton(label);
        connect(b, &QPushButton::clicked, this, [this, shape] { m_doc.addMask(shape); });
        addsRow->addWidget(b);
    }
    addsRow->addStretch(1);
    body->addWidget(adds);

    auto built = std::make_shared<QString>();
    auto rowRefreshers = std::make_shared<std::vector<std::function<void()>>>();
    m_refreshers.push_back([=, this] {
        const Recipe &r = m_doc.recipe();
        empty->setVisible(r.masks.empty());
        QString signature = QString::fromStdString(m_doc.selected()) + QLatin1Char('#') + vocabularyName(m_vocabulary);
        for (const LocalMask &m : r.masks)
            signature += QLatin1Char('|') + QString::fromStdString(m.id);
        if (signature != *built) {
            *built = signature;
            rowRefreshers->clear();
            while (QLayoutItem *item = rowsLayout->takeAt(0)) {
                delete item->widget();
                delete item;
            }
            for (const LocalMask &m : r.masks) {
                const std::string id = m.id;
                const bool selected = id == m_doc.selected();
                auto *row = new ItemRow(selected, [this, id] { m_doc.setSelected(id); });
                auto *head = new QHBoxLayout;
                head->setSpacing(5);
                auto *enabled = new QCheckBox;
                auto *name = new QLabel(QString::fromStdString(m.name));
                QFont f = theme::label();
                f.setWeight(selected ? QFont::DemiBold : QFont::Normal);
                name->setFont(f);
                auto *remove = deleteButton(QStringLiteral("Delete this mask."));
                head->addWidget(enabled);
                head->addWidget(name);
                head->addStretch(1);
                head->addWidget(remove);
                row->layout()->addLayout(head);
                auto edit = [this, id](const std::function<void(LocalMask &)> &change) {
                    Recipe r = m_doc.recipe();
                    for (LocalMask &x : r.masks)
                        if (x.id == id)
                            change(x);
                    m_doc.setRecipe(r);
                };
                connect(enabled, &QCheckBox::toggled, this, [edit](bool on) { edit([&](LocalMask &x) { x.enabled = on; }); });
                connect(remove, &QToolButton::clicked, this, [this, id] { m_doc.removeMask(id); });
                ParamSlider *stops = nullptr, *feather = nullptr, *grade = nullptr;
                QCheckBox *invert = nullptr;
                if (selected) {
                    const ControlSpec &exposureSpec = controls::maskExposure, &gradeSpec = controls::maskContrast;
                    stops = new ParamSlider(exposureSpec.name(m_vocabulary), exposureSpec.low, exposureSpec.high, 0,
                                            exposureSpec.decimals, exposureSpec.unit);
                    connect(stops, &ParamSlider::valueChanged, this, [edit](double v) {
                        edit([&](LocalMask &x) { x.stops = controls::maskExposure.toPhysical(v); });
                    });
                    feather = new ParamSlider(QStringLiteral("Feather"), 0, 0.25, 0.04, 3);
                    connect(feather, &ParamSlider::valueChanged, this,
                            [edit](double v) { edit([&](LocalMask &x) { x.feather = v; }); });
                    grade = new ParamSlider(gradeSpec.name(m_vocabulary), gradeSpec.low, gradeSpec.high, 0,
                                            gradeSpec.decimals, gradeSpec.unit);
                    connect(grade, &ParamSlider::valueChanged, this, [edit](double v) {
                        edit([&](LocalMask &x) { x.grade = controls::maskContrast.toPhysical(v); });
                    });
                    invert = new QCheckBox(QStringLiteral("Invert"));
                    invert->setFont(theme::small());
                    connect(invert, &QCheckBox::toggled, this, [edit](bool on) { edit([&](LocalMask &x) { x.inverted = on; }); });
                    for (QWidget *w : {static_cast<QWidget *>(stops), static_cast<QWidget *>(feather),
                                       static_cast<QWidget *>(grade), static_cast<QWidget *>(invert)})
                        row->layout()->addWidget(w);
                }
                rowsLayout->addWidget(row);
                // shown now: one added while its section is on screen was not
                row->show();
                rowRefreshers->push_back([this, id, enabled, stops, feather, grade, invert] {
                    for (const LocalMask &x : m_doc.recipe().masks) {
                        if (x.id != id)
                            continue;
                        const QSignalBlocker block(enabled);
                        enabled->setChecked(x.enabled);
                        if (stops) {
                            stops->setValue(controls::maskExposure.fromPhysical(x.stops));
                            stops->setHelp(controls::maskExposure.help(m_vocabulary, x.stops));
                            feather->setValue(x.feather);
                            grade->setValue(controls::maskContrast.fromPhysical(x.grade));
                            grade->setHelp(controls::maskContrast.help(m_vocabulary, x.grade));
                            const QSignalBlocker blockInvert(invert);
                            invert->setChecked(x.inverted);
                        }
                    }
                });
            }
        }
        for (const auto &refresh : *rowRefreshers)
            refresh();
    });
}

// MARK: - Bloom & Glow

void Inspector::buildEffects()
{
    auto *section = new Section(
        QStringLiteral("Bloom & Glow"),
        QStringLiteral("Bloom spreads the highlights softly; glow adds light around them. Laid on the finished print, "
                       "with sizes that follow the frame, so the preview matches the export."),
        false);
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();
    // the effects, all off until first touched
    auto look = [this] { return m_doc.recipe().effects.value_or(GlowLook::off()); };
    auto editLook = [this, look](const std::function<void(GlowLook &)> &change) {
        Recipe r = m_doc.recipe();
        GlowLook g = look();
        change(g);
        r.effects = g;
        m_doc.setRecipe(r);
    };
    auto lookSlider = [&](const QString &title, double low, double high, double def, const QString &unit,
                          double GlowLook::Bloom::*bloomValue, double GlowLook::Glow::*glowValue, const QString &help) {
        auto *s = new ParamSlider(title, low, high, def, 0, unit);
        s->setHelp(help);
        connect(s, &ParamSlider::valueChanged, this, [editLook, bloomValue, glowValue](double v) {
            editLook([&](GlowLook &g) {
                if (bloomValue)
                    g.bloom.*bloomValue = v;
                else
                    g.glow.*glowValue = v;
            });
        });
        m_refreshers.push_back([s, look, bloomValue, glowValue] {
            const GlowLook g = look();
            s->setValue(bloomValue ? g.bloom.*bloomValue : g.glow.*glowValue);
        });
        return s;
    };

    auto *bloom = new ToggleRow(QStringLiteral("Bloom"));
    bloom->setHelp(QStringLiteral(
        "Diffusion, like a mist filter on the lens: highlights bleed softly into darker surroundings and contrast "
        "falls around them, but nothing gets brighter than the highlight it came from. The Photoshop Pro Mist "
        "recipe: highlights selected, blurred and blended back with Lighten."));
    connect(bloom, &ToggleRow::toggled, this, [editLook](bool on) { editLook([&](GlowLook &g) { g.bloom.enabled = on; }); });
    body->addWidget(bloom);
    auto *bloomControls = new QWidget;
    auto *bloomLayout = new QVBoxLayout(bloomControls);
    bloomLayout->setContentsMargins(0, 0, 0, 0);
    bloomLayout->setSpacing(5);
    bloomLayout->addWidget(lookSlider(QStringLiteral("Amount"), 0, 200, 100, QStringLiteral(" %"), &GlowLook::Bloom::amount,
                                      nullptr, QStringLiteral("Layer opacity. Above 100 % it stacks like duplicated layers.")));
    bloomLayout->addWidget(lookSlider(QStringLiteral("Size"), 0, 100, 45, {}, &GlowLook::Bloom::size, nullptr,
                                      QStringLiteral("Blur radius, relative to the frame.")));
    bloomLayout->addWidget(lookSlider(QStringLiteral("Threshold"), 0, 100, 80, {}, &GlowLook::Bloom::threshold, nullptr,
                                      QStringLiteral("How bright a highlight must be to bloom. Lower includes more "
                                                     "midtones.")));
    auto *more = new Disclosure(QStringLiteral("More"));
    more->body()->addWidget(lookSlider(QStringLiteral("Softness"), 0, 100, 100, QStringLiteral(" %"),
                                       &GlowLook::Bloom::softness, nullptr,
                                       QStringLiteral("How gradually highlights above the threshold are selected.")));
    auto *blend = new Choice(QStringLiteral("Blend"));
    blend->addItem(QStringLiteral("Lighten"), QStringLiteral("lighten"));
    blend->addItem(QStringLiteral("Screen"), QStringLiteral("screen"));
    blend->addItem(QStringLiteral("Add"), QStringLiteral("add"));
    connect(blend, &Choice::chosen, this,
            [editLook](const QVariant &v) { editLook([&](GlowLook &g) { g.bloom.blend = v.toString().toStdString(); }); });
    more->body()->addWidget(blend);
    more->body()->addWidget(lookSlider(QStringLiteral("Haze"), 0, 100, 0, {}, &GlowLook::Bloom::haze, nullptr,
                                       QStringLiteral("A wide veil over the whole frame, like a physical mist filter: it "
                                                      "lifts the shadows and lowers contrast.")));
    bloomLayout->addWidget(more);
    body->addWidget(bloomControls);

    auto *glow = new ToggleRow(QStringLiteral("Glow"));
    glow->setHelp(QStringLiteral(
        "Added light, like the halo round a street lamp: the blurred highlights are added on top in linear light, so "
        "bright areas and what is near them get brighter. NegPy-style."));
    connect(glow, &ToggleRow::toggled, this, [editLook](bool on) { editLook([&](GlowLook &g) { g.glow.enabled = on; }); });
    body->addWidget(glow);
    auto *glowControls = new QWidget;
    auto *glowLayout = new QVBoxLayout(glowControls);
    glowLayout->setContentsMargins(0, 0, 0, 0);
    glowLayout->setSpacing(5);
    glowLayout->addWidget(lookSlider(QStringLiteral("Amount"), 0, 100, 30, {}, nullptr, &GlowLook::Glow::amount, {}));
    glowLayout->addWidget(lookSlider(QStringLiteral("Size"), 0, 100, 40, {}, nullptr, &GlowLook::Glow::size, {}));
    glowLayout->addWidget(lookSlider(QStringLiteral("Threshold"), 0, 100, 50, {}, nullptr, &GlowLook::Glow::threshold,
                                     QStringLiteral("How bright an area must be to glow.")));
    body->addWidget(glowControls);

    auto *reset = new LinkButton(QStringLiteral("Reset bloom & glow"));
    connect(reset, &QPushButton::clicked, this, [this] {
        Recipe r = m_doc.recipe();
        r.effects.reset();
        m_doc.setRecipe(r);
    });
    body->addWidget(reset);

    m_refreshers.push_back([=] {
        const GlowLook g = look();
        bloom->setChecked(g.bloom.enabled);
        bloomControls->setVisible(g.bloom.enabled);
        blend->setCurrent(QString::fromStdString(g.bloom.blend));
        glow->setChecked(g.glow.enabled);
        glowControls->setVisible(g.glow.enabled);
    });
}

// MARK: - Film Character

void Inspector::buildFilmCharacter()
{
    auto *section = new Section(QStringLiteral("Film Character"), {}, false);
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();

    // A black-and-white film has no couplers, but development holds itself
    // back the same way; the engine runs it as one layer's own inhibition.
    auto *edges = new QWidget;
    auto *edgesLayout = new QVBoxLayout(edges);
    edgesLayout->setContentsMargins(0, 0, 0, 0);
    edgesLayout->setSpacing(5);
    edgesLayout->addWidget(toggle(
        QStringLiteral("Edge effects"), &Recipe::couplersEnabled,
        QStringLiteral("Development's by-products spread out of dense areas and hold back development beside them, so "
                       "every edge comes out crisper: the adjacency effect behind silver film's acutance. Matched to "
                       "Kodak's measured sharpness of Tri-X in D-76.")));
    edgesLayout->addWidget(slider(QStringLiteral("Edge effect"), 0, 2, 1, 2, {}, &Recipe::couplersAmount,
                                  QStringLiteral("How strongly development holds back next to dense areas. 1 is D-76; "
                                                 "above that leans toward a dilute acutance developer such as "
                                                 "Rodinal; 0 is none.")));
    auto *edgeWidth = slider(QStringLiteral("Edge width"), 0, 3, 1, 2, {}, &Recipe::couplersDiffusion,
                             QStringLiteral("How far the by-products spread before they act. Wider reads as local "
                                            "contrast more than as edge sharpness."));
    edgesLayout->addWidget(edgeWidth);
    edgesLayout->addWidget(hairline());
    body->addWidget(edges);

    auto *couplers = new QWidget;
    auto *couplersLayout = new QVBoxLayout(couplers);
    couplersLayout->setContentsMargins(0, 0, 0, 0);
    couplersLayout->setSpacing(5);
    couplersLayout->addWidget(toggle(
        QStringLiteral("DIR couplers"), &Recipe::couplersEnabled,
        QStringLiteral("Inhibitors released as density forms, cutting density in neighbouring layers. This is where a "
                       "film's saturation and edge sharpness come from: a datasheet curve alone does not look like "
                       "film.")));
    couplersLayout->addWidget(slider(QStringLiteral("Coupler amount"), 0, 2, 1, 2, {}, &Recipe::couplersAmount));
    // a stock whose published MTF shows no edge effect (Provia 400X) has an
    // inhibitor with no reach: nothing to widen
    auto *diffusion = slider(QStringLiteral("Coupler diffusion"), 0, 3, 1, 2, {}, &Recipe::couplersDiffusion,
                             QStringLiteral("How far the inhibitor spreads before it acts. Wider diffusion reads as "
                                            "local contrast and apparent sharpness."));
    couplersLayout->addWidget(diffusion);
    couplersLayout->addWidget(hairline());
    body->addWidget(couplers);

    body->addWidget(toggle(QStringLiteral("Halation"), &Recipe::halationEnabled,
                           QStringLiteral("Light passing through the emulsion, reflecting off the base and coming "
                                          "back: the red bloom around bright edges.")));
    body->addWidget(slider(QStringLiteral("Halation"), 0, 4, 1, 2, {}, &Recipe::halationAmount));
    body->addWidget(slider(QStringLiteral("Halation size"), 0.2, 4, 1, 2, {}, &Recipe::halationScale));
    auto *scatter = slider(QStringLiteral("Scatter"), 0, 4, 1, 2, {}, &Recipe::scatterAmount,
                           QStringLiteral("Scattering inside the emulsion itself, a much tighter effect than halation."));
    body->addWidget(scatter);
    body->addWidget(hairline());

    body->addWidget(toggle(
        QStringLiteral("Grain"), &Recipe::grainEnabled,
        QStringLiteral("The stock's own grain, as strong as its datasheet measures it, in each emulsion layer: "
                       "coarsest where a negative is thin. Its dye clouds or silver clumps have a real size on the "
                       "film, so the preview shows the grain the export will.")));
    body->addWidget(slider(QStringLiteral("Grain"), 0, 3, 1, 2, {}, &Recipe::grainAmount));
    body->addWidget(slider(QStringLiteral("Grain size"), 0.25, 4, 1, 2, {}, &Recipe::grainScale,
                           QStringLiteral("Larger clouds and coarser grain, as a faster emulsion has.")));
    body->addWidget(hairline());

    body->addWidget(toggle(QStringLiteral("Viewing glare"), &Recipe::glareEnabled,
                           QStringLiteral("Light scattering off the print surface, which is why a real print's blacks "
                                          "are never as deep as the numbers suggest.")));
    body->addWidget(slider(QStringLiteral("Glare"), 0, 0.4, 0.03, 3, QStringLiteral("%"), &Recipe::glareAmount));
    body->addWidget(slider(QStringLiteral("Curve gamma"), 0.5, 2, 1, 2, {}, &Recipe::densityCurveGamma,
                           QStringLiteral("Development contrast of the negative itself: push and pull processing.")));

    auto *reset = new LinkButton(QStringLiteral("Reset film character"));
    connect(reset, &QPushButton::clicked, this, [this] { m_doc.resetFilmCharacter(); });
    body->addWidget(reset);

    m_refreshers.push_back([=, this] {
        const ProfilePack *film = m_doc.filmPack();
        const bool monochrome = film && film->isMonochrome();
        const bool hasCouplers = film && film->dirCouplers.has_value() && !monochrome;
        edges->setVisible(monochrome);
        couplers->setVisible(hasCouplers);
        diffusion->setVisible(hasCouplers && film->dirCouplers->diffusionSizeUm > 0);
        const bool camera = m_doc.recipe().keepsCameraSharpness();
        edgeWidth->setEnabled(!camera);
        diffusion->setEnabled(!camera);
        scatter->setEnabled(!camera);
    });
}

// MARK: - Output

void Inspector::buildOutput()
{
    auto *section = new Section(QStringLiteral("Output"), {}, false);
    m_content->layout()->addWidget(section);
    QVBoxLayout *body = section->body();

    auto *space = new Choice(QStringLiteral("Colour space"));
    for (const std::string &name : m_doc.packs().outputColorSpaces())
        space->addItem(QString::fromStdString(name), QString::fromStdString(name));
    connect(space, &Choice::chosen, this,
            [this](const QVariant &v) { edit([&](Recipe &r) { r.outputColorSpace = v.toString().toStdString(); }); });
    body->addWidget(space);

    body->addWidget(toggle(QStringLiteral("Gamut compression"), &Recipe::gamutCompress,
                           QStringLiteral("Pulls colours the film reached that the output space cannot show back "
                                          "inside it, moving chroma rather than clipping channels.")));

    auto *names = new Choice(QStringLiteral("Control names"));
    names->addItem(vocabularyName(Vocabulary::Photographic), int(Vocabulary::Photographic));
    names->addItem(vocabularyName(Vocabulary::Darkroom), int(Vocabulary::Darkroom));
    names->setHelp(QStringLiteral(
        "Photographic uses the names Capture One and Lightroom use, where positive always means brighter. Darkroom "
        "shows the underlying print controls. Either way the tooltip names the other."));
    connect(names, &Choice::chosen, this, [this](const QVariant &v) {
        m_vocabulary = Vocabulary(v.toInt());
        appSettings().setValue("vocabulary", vocabularyName(m_vocabulary));
        refresh();
    });
    body->addWidget(names);

    auto *preview = new Choice(QStringLiteral("Preview size"));
    for (int size : Document::previewSizes())
        preview->addItem(QStringLiteral("%1 px").arg(size), size);
    preview->setHelp(QStringLiteral(
        "The preview's long side. Larger is sharper on screen and slower to redraw. A file smaller than this is "
        "shown at its own size, never enlarged, and export is always at full size. Kept between launches."));
    connect(preview, &Choice::chosen, this, [this](const QVariant &v) { m_doc.setPreviewSize(v.toInt()); });
    body->addWidget(preview);

    m_refreshers.push_back([=, this] {
        space->setCurrent(QString::fromStdString(m_doc.recipe().outputColorSpace));
        names->setCurrent(int(m_vocabulary));
        preview->setCurrent(m_doc.previewSize());
    });
}
