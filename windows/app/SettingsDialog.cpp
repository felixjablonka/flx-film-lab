#include "SettingsDialog.h"

#include "AppSettings.h"
#include "DisplayColour.h"
#include "Document.h"
#include "ImageLibrary.h"
#include "SetupBuilder.h"
#include "Widgets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <set>

namespace {

constexpr int kWidth = 480;
// the Films tab, as the Mac's (600 x 600): two columns of names that fit
constexpr int kFilmsWidth = 600, kFilmsHeight = 640;

QLabel *heading(const QString &text)
{
    auto *label = new QLabel(text);
    QFont f = theme::label();
    f.setPixelSize(13);
    f.setWeight(QFont::DemiBold);
    label->setFont(f);
    return label;
}

// The words under a control: what it does.
QLabel *explanation(const QString &text)
{
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    label->setFont(theme::small());
    QPalette p = label->palette();
    p.setColor(QPalette::WindowText, theme::secondary());
    label->setPalette(p);
    return label;
}

QComboBox *combo()
{
    auto *c = new QComboBox;
    c->setFont(theme::label());
    c->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    return c;
}

// A group's rows on a lighter ground with rounded corners, as macOS's grouped
// form draws them, a hairline between one row and the next.
class Group : public QFrame
{
public:
    Group()
    {
        setObjectName(QStringLiteral("settingsGroup"));
        setAttribute(Qt::WA_StyledBackground);
        setStyleSheet(QStringLiteral("#settingsGroup { background: #2a2a2a; border-radius: 8px; }"));
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(14, 10, 14, 12);
        m_layout->setSpacing(6);
    }

    // a control's row: its name, and the control at the right
    void addRow(const QString &title, QWidget *control = nullptr)
    {
        if (m_rows++ > 0) {
            m_layout->addSpacing(4);
            m_layout->addWidget(hairline());
            m_layout->addSpacing(4);
        }
        auto *row = new QHBoxLayout;
        row->setSpacing(12);
        auto *name = new QLabel(title);
        name->setFont(theme::label());
        row->addWidget(name);
        row->addStretch(1);
        if (control)
            row->addWidget(control);
        m_layout->addLayout(row);
    }
    // what goes under the row
    void addUnder(QWidget *widget) { m_layout->addWidget(widget); }

private:
    QVBoxLayout *m_layout;
    int m_rows = 0;
};

// A row of its own, hairline and all, to show or hide as a whole.
QWidget *rowOf(const QString &title, QWidget *control)
{
    auto *row = new QWidget;
    auto *v = new QVBoxLayout(row);
    v->setContentsMargins(0, 4, 0, 0);
    v->setSpacing(8);
    v->addWidget(hairline());
    auto *h = new QHBoxLayout;
    h->setSpacing(12);
    auto *name = new QLabel(title);
    name->setFont(theme::label());
    h->addWidget(name);
    h->addStretch(1);
    h->addWidget(control);
    v->addLayout(h);
    return row;
}

QRadioButton *radio(const QString &text)
{
    auto *b = new QRadioButton(text);
    b->setFont(theme::label());
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

// A film or paper picker for a row: the inspector's, grouped by kind, at the
// window's size of text.
StockPicker *stockPicker()
{
    auto *p = new StockPicker(QString(), {});
    p->combo()->setFont(theme::label());
    p->setFixedWidth(270);
    return p;
}

// A plain button with a border, as the Mac's push buttons in a form.
QPushButton *borderedButton(const QString &text)
{
    auto *b = new QPushButton(text);
    b->setFont(theme::label());
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral("QPushButton { background: #353535; color: #e0e0e0; border: 1px solid #454545;"
                                    " border-radius: 6px; padding: 3px 12px; }"
                                    "QPushButton:hover { background: #3e3e3e; }"
                                    "QPushButton:disabled { color: #666; border-color: #3a3a3a; }"));
    return b;
}

} // namespace

SettingsDialog::SettingsDialog(Document &document, QWidget *parent)
    : QDialog(parent), m_doc(document)
{
    // its controls at the system's size, whatever the panels' text size
    const theme::ScaleScope standard(1.0);
    setWindowTitle(QStringLiteral("Settings"));
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, theme::window());
    setPalette(pal);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 14, 20, 20);
    layout->setSpacing(12);
    // the tabs, where the Mac's settings window has them, at the top
    m_tabs = new Segmented({QStringLiteral("General"), QStringLiteral("Film && Paper"), QStringLiteral("Films")});
    m_tabs->setFixedWidth(300);
    connect(m_tabs, &Segmented::chosen, this, [this](int i) { showPage(Page(i)); });
    layout->addWidget(m_tabs, 0, Qt::AlignHCenter);
    m_general = buildGeneral();
    m_filmAndPaper = buildFilmAndPaper();
    m_films = buildFilms();
    layout->addWidget(m_general);
    layout->addWidget(m_filmAndPaper);
    layout->addWidget(m_films, 1);
    showPage(Page::General);
}

QWidget *SettingsDialog::buildGeneral()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    layout->addWidget(heading(QStringLiteral("Interface")));
    auto *interface = new Group;
    m_textSize = new Segmented({theme::textSizeName(theme::TextSize::Standard),
                                theme::textSizeName(theme::TextSize::Larger),
                                theme::textSizeName(theme::TextSize::Largest)});
    m_textSize->setFixedWidth(240);
    connect(m_textSize, &Segmented::chosen, this, [this](int i) { chooseTextSize(theme::TextSize(i)); });
    interface->addRow(QStringLiteral("Text size"), m_textSize);
    interface->addUnder(explanation(QStringLiteral(
        "The inspector and the sidebar, larger as a whole: their text, controls and width. The toolbar keeps its "
        "size.")));
    m_names = combo();
    for (Vocabulary v : {Vocabulary::Photographic, Vocabulary::Darkroom})
        m_names->addItem(vocabularyName(v), int(v));
    connect(m_names, &QComboBox::activated, this,
            [this](int i) { chooseVocabulary(Vocabulary(m_names->itemData(i).toInt())); });
    interface->addRow(QStringLiteral("Control names"), m_names);
    interface->addUnder(explanation(QStringLiteral(
        "The Stills sliders. Photographic uses the names Capture One and Lightroom use, where positive always means "
        "brighter. Darkroom shows the underlying print controls. Either way the tooltip names the other.")));
    layout->addWidget(interface);

    layout->addSpacing(8);
    layout->addWidget(heading(QStringLiteral("Preview")));
    auto *preview = new Group;
    m_preview = combo();
    for (int size : Document::previewSizes())
        m_preview->addItem(QStringLiteral("%1 px").arg(size), size);
    connect(m_preview, &QComboBox::activated, this,
            [this](int i) { choosePreviewSize(m_preview->itemData(i).toInt()); });
    preview->addRow(QStringLiteral("Preview size"), m_preview);
    preview->addUnder(explanation(QStringLiteral(
        "The Stills preview's long side. Larger is sharper on screen and slower to redraw. A file smaller than this "
        "is shown at its own size, never enlarged, and export is always at full size.")));
    preview->addRow(QStringLiteral("Monitor"));
    m_monitor = new QLabel;
    m_monitor->setWordWrap(true);
    m_monitor->setTextFormat(Qt::PlainText);
    m_monitor->setFont(theme::label());
    preview->addUnder(m_monitor);
    preview->addUnder(explanation(QStringLiteral(
        "The previews follow the colour of the monitor the window is on, as macOS manages it for the Mac app. "
        "Exports carry their own profile and are never changed for a monitor.")));
    layout->addWidget(preview);

    layout->addSpacing(8);
    layout->addWidget(heading(QStringLiteral("Library")));
    auto *library = new Group;
    m_sidecars = new Switch;
    connect(m_sidecars, &QAbstractButton::clicked, this, [this](bool on) { chooseSidecars(on); });
    library->addRow(QStringLiteral("Save settings beside images"), m_sidecars);
    library->addUnder(explanation(QStringLiteral(
        "Each picture's settings, or clip's look, and its flag also go into a small file beside it, "
        "DSC_0871.NEF.flx, which travels with the folder to another drive, another PC or the Mac app. They are "
        "always kept in the app's library too; of the two, the newer counts.")));
    layout->addWidget(library);

    layout->addSpacing(8);
    layout->addWidget(heading(QStringLiteral("Comment in Exported Files")));
    auto *comment = new Group;
    m_commentEditedWith = new Switch;
    m_commentFilm = new Switch;
    m_commentPaper = new Switch;
    m_commentFilter = new Switch;
    for (Switch *s : {m_commentEditedWith, m_commentFilm, m_commentPaper, m_commentFilter})
        connect(s, &QAbstractButton::clicked, this, [this] {
            chooseExportComment({m_commentEditedWith->isChecked(), m_commentFilm->isChecked(),
                                 m_commentPaper->isChecked(), m_commentFilter->isChecked()});
        });
    comment->addRow(QStringLiteral("Edited with FLX Film Lab"), m_commentEditedWith);
    comment->addRow(QStringLiteral("Film profile"), m_commentFilm);
    comment->addRow(QStringLiteral("Paper or print film"), m_commentPaper);
    comment->addRow(QStringLiteral("Camera filter"), m_commentFilter);
    comment->addUnder(explanation(QStringLiteral(
        "What an exported picture's EXIF comment says, and its description where the camera left none, and an "
        "exported clip's description. A slide and an instant print, seen as they are, name no paper. With none on, "
        "nothing is written there; a camera's own description stays either way.")));
    m_commentExample = explanation(QString());
    comment->addUnder(m_commentExample);
    layout->addWidget(comment);
    return page;
}

// What a picture with no settings of its own opens with, in Stills, and how
// the paper follows a change of film, in Stills and Motion (NewPictures,
// PaperRule): the Mac's Film & Paper tab.
QWidget *SettingsDialog::buildFilmAndPaper()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    layout->addWidget(heading(QStringLiteral("New Pictures")));
    auto *pictures = new Group;
    pictures->addRow(QStringLiteral("Open with"));
    m_carryLook = radio(QStringLiteral("The look of the picture before"));
    m_defaultStocks = radio(QStringLiteral("The default film and paper"));
    connect(m_carryLook, &QRadioButton::clicked, this, [this] { chooseNewPictures(NewPictures::CarryLook); });
    connect(m_defaultStocks, &QRadioButton::clicked, this, [this] { chooseNewPictures(NewPictures::DefaultStocks); });
    pictures->addUnder(m_carryLook);
    pictures->addUnder(m_defaultStocks);
    pictures->addUnder(explanation(QStringLiteral(
        "For a picture with no settings of its own. The look of the picture before is its film, paper and film "
        "character, with the exposure, tone and colour fresh; the first picture after launch has none before it and "
        "opens with the default film and paper. A picture with settings of its own always opens with them.")));
    m_defaultFilm = stockPicker();
    connect(m_defaultFilm, &Choice::chosen, this,
            [this](const QVariant &v) { chooseDefaultFilm(v.toString().toStdString()); });
    pictures->addRow(QStringLiteral("Default film"), m_defaultFilm);
    m_defaultPaper = stockPicker();
    m_defaultPaper->setGroups(stockGroups(m_doc.packs().paperEntries(), false));
    connect(m_defaultPaper, &Choice::chosen, this,
            [this](const QVariant &v) { chooseDefaultPaper(v.toString().toStdString()); });
    m_defaultPaperRow = rowOf(QStringLiteral("Default paper"), m_defaultPaper);
    pictures->addUnder(m_defaultPaperRow);
    m_noPaper = explanation(QStringLiteral(
        "A slide is seen on the light table, and an instant print is the print: neither is printed on paper."));
    pictures->addUnder(m_noPaper);
    layout->addWidget(pictures);

    layout->addSpacing(8);
    layout->addWidget(heading(QStringLiteral("Changing the Film")));
    auto *changing = new Group;
    changing->addRow(QStringLiteral("The paper"));
    m_followsFilm = radio(QStringLiteral("Follows the film"));
    m_staysWithKind = radio(QStringLiteral("Stays with the kind of film"));
    connect(m_followsFilm, &QRadioButton::clicked, this, [this] { choosePaperRule(PaperRule::FollowsFilm); });
    connect(m_staysWithKind, &QRadioButton::clicked, this, [this] { choosePaperRule(PaperRule::StaysWithKind); });
    changing->addUnder(m_followsFilm);
    changing->addUnder(m_staysWithKind);
    changing->addUnder(explanation(QStringLiteral(
        "Follows the film: each film brings the paper it is printed on by reference, and a motion picture film its own "
        "print film. Stays with the kind of film: the paper stays while you change from one colour negative to "
        "another, as do the print film of motion picture negatives and the paper of black-and-white ones; a film of "
        "another kind brings back the paper last used with that kind, the first time its own. In Stills and "
        "Motion.")));
    layout->addWidget(changing);
    return page;
}

// Which films the Film menu lists, in Stills and Motion: each film ticked or
// not, in the menu's own groups and order. A film left out still works: a
// picture, clip or preset that uses it opens with it, and it stays in the
// menu while in use (filmmenu::shown).
QWidget *SettingsDialog::buildFilms()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *top = new Group;
    top->addUnder(explanation(QStringLiteral(
        "The films the Film menu lists, in Stills and Motion. A film left out keeps working: a picture, clip or "
        "preset that uses it opens with it, and the menu shows it while it is in use.")));
    auto *count = new QWidget;
    auto *countRow = new QHBoxLayout(count);
    countRow->setContentsMargins(0, 4, 0, 0);
    m_filmsShown = new QLabel;
    m_filmsShown->setFont(theme::label());
    QPalette secondary = m_filmsShown->palette();
    secondary.setColor(QPalette::WindowText, theme::secondary());
    m_filmsShown->setPalette(secondary);
    countRow->addWidget(m_filmsShown);
    countRow->addStretch(1);
    m_showAll = borderedButton(QStringLiteral("Show All"));
    connect(m_showAll, &QPushButton::clicked, this, [this] { chooseGroupShown({}, true); });
    countRow->addWidget(m_showAll);
    top->addUnder(count);
    layout->addWidget(top);

    // the groups in a list that scrolls, on the window's own ground
    auto *area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setFrameShape(QFrame::NoFrame);
    auto *list = new QWidget;
    auto *listLayout = new QVBoxLayout(list);
    listLayout->setContentsMargins(0, 0, 12, 0);
    listLayout->setSpacing(8);
    QPalette ground = list->palette();
    ground.setColor(QPalette::Window, theme::window());
    list->setAutoFillBackground(true);
    list->setPalette(ground);
    area->viewport()->setAutoFillBackground(true);
    area->viewport()->setPalette(ground);

    m_groups = stockGroups(m_doc.packs().filmEntries(), true);
    for (const StockGroup &group : m_groups) {
        auto *head = new QHBoxLayout;
        head->setContentsMargins(0, 6, 0, 0);
        head->addWidget(heading(QString::fromStdString(group.title)));
        head->addStretch(1);
        auto *toggle = new LinkButton(QStringLiteral("Hide All"));
        toggle->setFont(theme::small());
        const std::string title = group.title;
        connect(toggle, &QPushButton::clicked, this, [this, title, toggle] {
            chooseGroupShown(title, toggle->text() == QStringLiteral("Show All"));
        });
        head->addWidget(toggle);
        m_groupButtons.push_back(toggle);
        listLayout->addLayout(head);

        // two columns, in the menu's order down each
        auto *box = new Group;
        auto *grid = new QWidget;
        auto *gridLayout = new QGridLayout(grid);
        gridLayout->setContentsMargins(0, 2, 0, 2);
        gridLayout->setHorizontalSpacing(16);
        gridLayout->setVerticalSpacing(7);
        const int n = int(group.entries.size()), half = (n + 1) / 2;
        for (int i = 0; i < n; ++i) {
            const PackIndexEntry &entry = group.entries[size_t(i)];
            auto *check = new QCheckBox(QString::fromStdString(entry.name));
            check->setFont(theme::label());
            check->setCursor(Qt::PointingHandCursor);
            const std::string stock = entry.stock;
            connect(check, &QCheckBox::clicked, this, [this, stock](bool on) { chooseFilmShown(stock, on); });
            gridLayout->addWidget(check, i % half, i / half);
            m_filmBoxes.emplace_back(stock, check);
        }
        gridLayout->setColumnStretch(0, 1);
        gridLayout->setColumnStretch(1, 1);
        box->addUnder(grid);
        listLayout->addWidget(box);
    }
    listLayout->addStretch(1);
    area->setWidget(list);
    layout->addWidget(area, 1);
    return page;
}

void SettingsDialog::showPage(Page page)
{
    m_page = page;
    m_tabs->setCurrent(int(page));
    // the other page leaves the layout, so the window takes this one's size
    m_general->setVisible(page == Page::General);
    m_filmAndPaper->setVisible(page == Page::FilmAndPaper);
    m_films->setVisible(page == Page::Films);
    refresh();
}

void SettingsDialog::chooseTextSize(theme::TextSize size)
{
    if (size != theme::textSize()) {
        theme::setTextSize(size);
        emit textSizeChanged();
    }
    refresh();
}

void SettingsDialog::chooseVocabulary(Vocabulary vocabulary)
{
    appSettings().setValue("vocabulary", vocabularyName(vocabulary));
    emit vocabularyChanged(vocabulary);
    refresh();
}

void SettingsDialog::choosePreviewSize(int pixels)
{
    m_doc.setPreviewSize(pixels);
    refresh();
}

void SettingsDialog::chooseSidecars(bool on)
{
    // turned on, the records already kept are written beside their files too
    m_doc.library().setSavesSidecars(on);
    refresh();
}

void SettingsDialog::chooseExportComment(const ExportComment &comment)
{
    setExportComment(comment);
    refresh();
}

QString SettingsDialog::commentExampleText()
{
    Recipe recipe;
    recipe.filmStock = "kodak_tri_x_400";
    recipe.printStock = "ilford_multigrade_rc_deluxe";
    recipe.lensFilter = "red";
    const std::string said = provenance(recipe, m_doc.packs(), "", exportComment()).description;
    return said.empty() ? QStringLiteral("Nothing is written there.")
                        : QStringLiteral("For example: %1").arg(QString::fromStdString(said));
}

void SettingsDialog::chooseFilmShown(const std::string &stock, bool shown)
{
    std::set<std::string> hidden = filmmenu::hidden(hiddenFilms());
    if (shown)
        hidden.erase(stock);
    else
        hidden.insert(stock);
    setHiddenFilms(filmmenu::stored(hidden));
    emit filmsChanged();
    refresh();
}

void SettingsDialog::chooseGroupShown(const std::string &group, bool shown)
{
    std::set<std::string> hidden = filmmenu::hidden(hiddenFilms());
    for (const StockGroup &g : m_groups)
        if (group.empty() || g.title == group)
            for (const PackIndexEntry &e : g.entries) {
                if (shown)
                    hidden.erase(e.stock);
                else
                    hidden.insert(e.stock);
            }
    // Show All with nothing else hidden leaves no setting, as the Mac's
    setHiddenFilms(group.empty() && shown ? std::string() : filmmenu::stored(hidden));
    emit filmsChanged();
    refresh();
}

void SettingsDialog::chooseNewPictures(NewPictures choice)
{
    setNewPictures(choice);
    refresh();
}

void SettingsDialog::chooseDefaultFilm(const std::string &stock)
{
    setDefaultFilm(stock);
    // a new default film brings its own paper, which the paper picker can then change
    if (const std::optional<std::string> own = ownPaper(m_doc.packs(), stock))
        setDefaultPaper(*own);
    refresh();
}

void SettingsDialog::chooseDefaultPaper(const std::string &stock)
{
    setDefaultPaper(stock);
    refresh();
}

void SettingsDialog::choosePaperRule(PaperRule rule)
{
    setPaperRule(rule);
    refresh();
}

std::string SettingsDialog::shownDefaultFilm() const
{
    return m_defaultFilm->current().toString().toStdString();
}

std::string SettingsDialog::shownDefaultPaper() const
{
    return m_defaultPaper->current().toString().toStdString();
}

bool SettingsDialog::defaultPaperShown() const
{
    return !m_defaultPaperRow->isHidden();
}

QString SettingsDialog::monitorText() const
{
    const displaycolour::State screen = displaycolour::current();
    if (screen.managedByWindows)
        return QStringLiteral("Windows manages colour on this monitor (%1): the previews go to it in sRGB, and it "
                              "maps them to the display.")
            .arg(screen.mode);
    if (screen.space.isValid())
        return QStringLiteral("The previews are converted to the monitor's profile, %1.").arg(screen.description);
    return QStringLiteral("The previews are in sRGB, as the monitor is.");
}

QString SettingsDialog::filmsShownText() const
{
    const std::set<std::string> hidden = filmmenu::hidden(hiddenFilms());
    int all = 0, shown = 0;
    for (const StockGroup &g : m_groups)
        for (const PackIndexEntry &e : g.entries) {
            ++all;
            shown += hidden.count(e.stock) ? 0 : 1;
        }
    return QStringLiteral("%1 of %2 films shown").arg(shown).arg(all);
}

void SettingsDialog::refresh()
{
    // shown as things are: none of these signals a change
    m_textSize->setCurrent(int(theme::textSize()));
    const bool darkroom = appSettings().value("vocabulary").toString() == vocabularyName(Vocabulary::Darkroom);
    m_names->setCurrentIndex(m_names->findData(int(darkroom ? Vocabulary::Darkroom : Vocabulary::Photographic)));
    m_preview->setCurrentIndex(m_preview->findData(m_doc.previewSize()));
    m_sidecars->setChecked(m_doc.library().savesSidecars());
    const ExportComment comment = exportComment();
    m_commentEditedWith->setChecked(comment.editedWith);
    m_commentFilm->setChecked(comment.film);
    m_commentPaper->setChecked(comment.paper);
    m_commentFilter->setChecked(comment.filter);
    m_commentExample->setText(commentExampleText());
    m_monitor->setText(monitorText());

    // the defaults as a picture would start with them
    const Recipe start = startingRecipe(m_doc.packs(), defaultFilmSetting(), defaultPaperSetting());
    const bool carries = newPictures() == NewPictures::CarryLook;
    m_carryLook->setChecked(carries);
    m_defaultStocks->setChecked(!carries);
    m_defaultFilm->setGroups(stockGroups(filmmenu::shown(m_doc.packs().filmEntries(), hiddenFilms(), start.filmStock), true));
    m_defaultFilm->setCurrent(QString::fromStdString(start.filmStock));
    m_defaultPaper->setCurrent(QString::fromStdString(start.printStock));
    bool onPaper = true;
    try {
        onPaper = !m_doc.packs().pack(start.filmStock).isViewedDirectly(std::nullopt);
    } catch (const std::exception &) {
    }
    m_defaultPaperRow->setVisible(onPaper);
    m_noPaper->setVisible(!onPaper);
    const bool follows = paperRule() == PaperRule::FollowsFilm;
    m_followsFilm->setChecked(follows);
    m_staysWithKind->setChecked(!follows);

    const std::set<std::string> hidden = filmmenu::hidden(hiddenFilms());
    for (const auto &[stock, check] : m_filmBoxes)
        check->setChecked(!hidden.count(stock));
    for (size_t i = 0; i < m_groups.size() && i < m_groupButtons.size(); ++i) {
        bool every = true;
        for (const PackIndexEntry &e : m_groups[i].entries)
            every = every && !hidden.count(e.stock);
        m_groupButtons[i]->setText(every ? QStringLiteral("Hide All") : QStringLiteral("Show All"));
    }
    m_filmsShown->setText(filmsShownText());
    m_showAll->setEnabled(!hidden.empty());

    // General and Film & Paper as tall as their words need at their width;
    // Films the Mac's size, its list scrolling
    if (m_page == Page::Films)
        setFixedSize(kFilmsWidth, kFilmsHeight);
    else
        setFixedSize(kWidth, layout()->totalHeightForWidth(kWidth));
}

void SettingsDialog::showEvent(QShowEvent *e)
{
    refresh();
    QDialog::showEvent(e);
}
