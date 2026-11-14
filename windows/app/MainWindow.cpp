#include "MainWindow.h"

#include "AppSettings.h"
#include "Canvas.h"
#include "DisplayColour.h"
#include "Icons.h"
#include "Document.h"
#include "Exporter.h"
#include "FolderSidebar.h"
#include "Inspector.h"
#include "MotionCanvas.h"
#include "MotionDocument.h"
#include "MotionInspector.h"
#include "SettingsDialog.h"
#include "Theme.h"
#include "TransportBar.h"
#include "Widgets.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QWindow>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFrame>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <map>

namespace {

// The RAW formats the decoders read, and pictures already rendered.
QString openFilter()
{
    static const char *raw[] = {"3fr", "ari", "arw", "bay", "cap", "cr2", "cr3", "crw", "dcr", "dcs", "dng", "drf",
                                "eip", "erf", "fff", "iiq", "k25", "kdc", "mdc", "mef", "mos", "mrw", "nef", "nrw",
                                "orf", "ori", "pef", "ptx", "raf", "raw", "rw2", "rwl", "rwz", "sr2", "srf", "srw",
                                "x3f"};
    QStringList patterns;
    for (const char *ext : raw)
        patterns << QStringLiteral("*.%1").arg(QLatin1StringView(ext));
    const QStringList rendered{"*.jpg", "*.jpeg", "*.png", "*.tif", "*.tiff"};
    return QStringLiteral("RAW files and pictures (%1);;RAW files (%2);;All files (*)")
        .arg((patterns + rendered).join(QLatin1Char(' ')), patterns.join(QLatin1Char(' ')));
}

QAction *menuAction(QMenu *menu, const QString &text, const QKeySequence &shortcut, QObject *target,
                   std::function<void()> slot)
{
    QAction *a = menu->addAction(text);
    a->setShortcut(shortcut);
    QObject::connect(a, &QAction::triggered, target, std::move(slot));
    return a;
}

// The toolbar's menus, the export format and the clip's codec: the choice
// in words on a capsule with a chevron, as the Mac's toolbar menus.
class ToolMenu : public QComboBox
{
public:
    ToolMenu()
    {
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
    }
    QSize sizeHint() const override
    {
        int widest = 0;
        for (int i = 0; i < count(); ++i)
            widest = std::max(widest, fontMetrics().horizontalAdvance(itemText(i)));
        return QSize(widest + 12 + 30 + 6, 34);   // a few pixels for the text's own rounding
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r(rect());
        p.setPen(Qt::NoPen);
        p.setBrush(underMouse() && isEnabled() ? QColor(0x42, 0x42, 0x42) : QColor(0x35, 0x35, 0x35));
        p.drawRoundedRect(r, 10, 10);
        const QColor ink = isEnabled() ? QColor(230, 230, 230) : QColor(110, 110, 110);
        p.setPen(ink);
        const QRectF text = r.adjusted(12, 0, -30, 0);
        p.drawText(text, Qt::AlignVCenter | Qt::AlignLeft,
                   fontMetrics().elidedText(currentText(), Qt::ElideRight, int(text.width())));
        icons::paint(p, QRectF(r.right() - 25, r.center().y() - 6.5, 13, 13), icons::Icon::Chevron, ink);
    }
};

QWidget *divider()
{
    auto *line = new QWidget;
    line->setFixedWidth(1);
    line->setAutoFillBackground(true);
    QPalette p = line->palette();
    p.setColor(QPalette::Window, QColor(0, 0, 0));
    line->setPalette(p);
    return line;
}

} // namespace

// The sidebar, and the line between it and the canvas, shown and hidden together.
QWidget *MainWindow::sidebarFor(FolderSidebar *sidebar, bool motion)
{
    auto *box = new QWidget;
    auto *layout = new QHBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(sidebar);
    layout->addWidget(divider());
    connect(sidebar, &FolderSidebar::openFolderRequested, this, &MainWindow::openFolderDialog);
    connect(sidebar, &FolderSidebar::exportSelectedRequested, this, &MainWindow::exportSelectedDialog);
    if (motion)
        return box;
    connect(sidebar, &FolderSidebar::openRequested, this, [this](const QString &path) { m_doc.open(path); });
    connect(sidebar, &FolderSidebar::flagRequested, this,
            [this](std::optional<ImageLibrary::Flag> f) { m_doc.flagSelection(f); });
    connect(sidebar, &FolderSidebar::flagPathRequested, this,
            [this](const QString &path, std::optional<ImageLibrary::Flag> f) { m_doc.flagPaths({path}, f); });
    connect(sidebar, &FolderSidebar::pasteToSelectedRequested, this, [this] { m_doc.pasteSettingsToSelected(); });
    connect(sidebar, &FolderSidebar::cancelBatchRequested, this, [this] { m_doc.cancelBatch(); });
    connect(sidebar, &FolderSidebar::stepRequested, this, [this](int offset) { m_doc.showNeighbour(offset); });
    return box;
}

QWidget *MainWindow::buildStills()
{
    auto *page = new QWidget;
    auto *layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_canvas = new Canvas(m_doc);
    m_inspector = new Inspector(m_doc);
    m_stillsSidebar = sidebarFor(new FolderSidebar(m_doc.browser()), false);
    layout->addWidget(m_stillsSidebar);
    layout->addWidget(m_canvas, 1);
    layout->addWidget(divider());
    layout->addWidget(m_inspector);
    connect(m_canvas, &Canvas::openRequested, this, &MainWindow::openDialog);
    return page;
}

QWidget *MainWindow::buildMotion()
{
    auto *page = new QWidget;
    auto *layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *left = new QWidget;
    auto *column = new QVBoxLayout(left);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    m_motionCanvas = new MotionCanvas(m_motion);
    column->addWidget(m_motionCanvas, 1);
    column->addWidget(new TransportBar(m_motion));
    m_motionInspector = makeMotionInspector();
    m_motionSidebar = makeMotionSidebar();
    layout->addWidget(m_motionSidebar);
    layout->addWidget(left, 1);
    layout->addWidget(divider());
    layout->addWidget(m_motionInspector);
    connect(m_motionCanvas, &MotionCanvas::openRequested, this, &MainWindow::openDialog);
    return page;
}

MotionInspector *MainWindow::makeMotionInspector()
{
    auto *inspector = new MotionInspector(m_motion);
    connect(inspector, &MotionInspector::exportRequested, this, &MainWindow::exportDialog);
    connect(inspector, &MotionInspector::exportLUTRequested, this, &MainWindow::exportLUTDialog);
    return inspector;
}

QWidget *MainWindow::makeMotionSidebar()
{
    auto *sidebar = new FolderSidebar(m_motion.browser());
    connect(sidebar, &FolderSidebar::openRequested, this, [this](const QString &path) { m_motion.open(path); });
    connect(sidebar, &FolderSidebar::flagRequested, this,
            [this](std::optional<ImageLibrary::Flag> f) { m_motion.flagSelection(f); });
    connect(sidebar, &FolderSidebar::flagPathRequested, this,
            [this](const QString &path, std::optional<ImageLibrary::Flag> f) { m_motion.flagPaths({path}, f); });
    connect(sidebar, &FolderSidebar::pasteToSelectedRequested, this, [this] { m_motion.pasteLookToSelected(); });
    connect(sidebar, &FolderSidebar::cancelBatchRequested, this, [this] { m_motion.cancelExport(); });
    connect(sidebar, &FolderSidebar::stepRequested, this, [this](int offset) { m_motion.showNeighbour(offset); });
    return sidebarFor(sidebar, true);
}

// Text size changed (Settings): the inspectors and the sidebars made again at
// the new size, in their places, each section open or closed as it was left.
// What they show is the documents' and the browsers', so nothing else is lost.
void MainWindow::rebuildPanels()
{
    const auto sections = [](QWidget *panel) {
        std::map<QString, bool> open;
        for (const Section *s : panel->findChildren<Section *>())
            open[s->title()] = s->isExpanded();
        return open;
    };
    const auto reopen = [](QWidget *panel, const std::map<QString, bool> &open) {
        for (Section *s : panel->findChildren<Section *>())
            if (const auto it = open.find(s->title()); it != open.end() && it->second != s->isExpanded())
                s->setExpanded(it->second);
    };
    const auto swap = [](QWidget *old, QWidget *replacement) {
        delete old->parentWidget()->layout()->replaceWidget(old, replacement);
        delete old;
    };

    const auto stillsOpen = sections(m_inspector), motionOpen = sections(m_motionInspector);
    auto *inspector = new Inspector(m_doc);
    swap(m_inspector, inspector);
    m_inspector = inspector;
    reopen(m_inspector, stillsOpen);
    MotionInspector *motionInspector = makeMotionInspector();
    swap(m_motionInspector, motionInspector);
    m_motionInspector = motionInspector;
    reopen(m_motionInspector, motionOpen);

    QWidget *stillsSidebar = sidebarFor(new FolderSidebar(m_doc.browser()), false);
    swap(m_stillsSidebar, stillsSidebar);
    m_stillsSidebar = stillsSidebar;
    QWidget *motionSidebar = makeMotionSidebar();
    swap(m_motionSidebar, motionSidebar);
    m_motionSidebar = motionSidebar;
    setSidebarShown(appSettings().value("showSidebar", true).toBool());
}

SettingsDialog *MainWindow::showSettings()
{
    if (!m_settings) {
        m_settings = new SettingsDialog(m_doc, this);
        connect(m_settings, &SettingsDialog::textSizeChanged, this, &MainWindow::rebuildPanels);
        connect(m_settings, &SettingsDialog::vocabularyChanged, this,
                [this](Vocabulary v) { m_inspector->setVocabulary(v); });
        connect(m_settings, &SettingsDialog::filmsChanged, this, [this] {
            m_inspector->listFilms();
            m_motionInspector->listFilms();
        });
    }
    m_settings->show();
    m_settings->raise();
    m_settings->activateWindow();
    return m_settings;
}

MainWindow::MainWindow(Document &document, MotionDocument &motion, QWidget *parent)
    : QMainWindow(parent), m_doc(document), m_motion(motion)
{
    setAcceptDrops(true);
    setMinimumSize(1080, 700);
    m_pages = new QStackedWidget;
    m_pages->addWidget(buildStills());
    m_pages->addWidget(buildMotion());
    setCentralWidget(m_pages);

    // MARK: the menus
    QMenu *file = menuBar()->addMenu(QStringLiteral("&File"));
    menuAction(file, QStringLiteral("&Open…"), QKeySequence::Open, this, [this] { openDialog(); });
    // both act on the half on screen
    m_openFolder = menuAction(file, QStringLiteral("Open &Folder…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O),
                              this, [this] { openFolderDialog(); });
    m_exportMenu = menuAction(file, QStringLiteral("&Export…"), QKeySequence(Qt::CTRL | Qt::Key_E), this,
                             [this] { exportDialog(); });
    m_exportSelected = menuAction(file, QStringLiteral("Export &Selected…"),
                                  QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), this, [this] { exportSelectedDialog(); });
    m_exportLUT = menuAction(file, QStringLiteral("Export &LUT…"), {}, this, [this] { exportLUTDialog(); });
    file->addSeparator();
    // in File, not Edit: Edit is hidden in Motion, and Settings are both halves'
    menuAction(file, QStringLiteral("Se&ttings…"), QKeySequence(Qt::CTRL | Qt::Key_Comma), this,
               [this] { showSettings(); })
        ->setMenuRole(QAction::PreferencesRole);
    file->addSeparator();
    menuAction(file, QStringLiteral("E&xit"), QKeySequence::Quit, this, [this] { close(); });

    QMenu *edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    m_editMenu = edit->menuAction();
    // the half on screen's; a text field being typed in keeps its own
    m_undo = menuAction(edit, QStringLiteral("&Undo"), QKeySequence::Undo, this, [this] {
        if (m_mode == Mode::Motion)
            m_motion.history().undo();
        else
            m_doc.history().undo();
    });
    m_redo = menuAction(edit, QStringLiteral("&Redo"), QKeySequence::Redo, this, [this] {
        if (m_mode == Mode::Motion)
            m_motion.history().redo();
        else
            m_doc.history().redo();
    });
    m_redo->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Y), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});
    // the Edit menu is hidden in Motion; its keys work there too
    addAction(m_undo);
    addAction(m_redo);
    edit->addSeparator();
    m_copy = menuAction(edit, QStringLiteral("&Copy Settings"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this,
                       [this] { m_doc.copySettings(); });
    m_paste = menuAction(edit, QStringLiteral("&Paste Settings"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), this,
                        [this] { m_doc.pasteSettings(); });
    edit->addSeparator();
    m_crop = menuAction(edit, QStringLiteral("C&rop"), QKeySequence(Qt::CTRL | Qt::Key_R), this,
                        [this] { m_doc.setCropping(!m_doc.isCropping()); });
    m_crop->setCheckable(true);
    m_loupe = menuAction(edit, QStringLiteral("&Loupe"), QKeySequence(Qt::CTRL | Qt::Key_L), this,
                         [this] { m_doc.setLoupeOn(!m_doc.loupeOn()); });
    m_loupe->setCheckable(true);
    m_loupe->setToolTip(QStringLiteral("The print at 100% (Ctrl+L), as the export will have it: drag the loupe over "
                                       "the print. While it is open the picture is held at full size."));
    m_crop->setToolTip(QStringLiteral("Crop, turn and level the print (Ctrl+R): the whole frame shows with the crop on "
                                      "it. Return keeps the crop, Escape goes back."));
    m_hideMaskLines = menuAction(edit, QStringLiteral("&Hide Mask Lines"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H),
                                 this, [this] { m_doc.setMaskLinesHidden(!m_doc.maskLinesHidden()); });
    m_hideMaskLines->setCheckable(true);
    m_hideMaskLines->setToolTip(QStringLiteral("Hide the lines of the masks and gradients (Ctrl+Shift+H), to see the "
                                               "print without them: they still act. Choosing one to edit shows them "
                                               "again."));
    m_turnLeft = menuAction(edit, QStringLiteral("Turn &Left"), QKeySequence(Qt::CTRL | Qt::Key_BracketLeft), this,
                            [this] { m_doc.turn(false); });
    m_turnRight = menuAction(edit, QStringLiteral("Turn R&ight"), QKeySequence(Qt::CTRL | Qt::Key_BracketRight), this,
                             [this] { m_doc.turn(true); });
    // while cropping: Return keeps the crop, Escape goes back
    auto *keep = new QShortcut(QKeySequence(Qt::Key_Return), this, [this] { m_doc.endCropping(true); });
    auto *keepEnter = new QShortcut(QKeySequence(Qt::Key_Enter), this, [this] { m_doc.endCropping(true); });
    auto *cancel = new QShortcut(QKeySequence(Qt::Key_Escape), this, [this] { m_doc.endCropping(false); });
    for (QShortcut *s : {keep, keepEnter, cancel})
        s->setEnabled(false);
    connect(&m_doc, &Document::changed, this, [this, keep, keepEnter, cancel] {
        for (QShortcut *s : {keep, keepEnter, cancel})
            s->setEnabled(m_mode == Mode::Stills && m_doc.isCropping());
    });

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    m_stillsMode = menuAction(view, QStringLiteral("&Stills"), QKeySequence(Qt::CTRL | Qt::Key_1), this,
                              [this] { setMode(Mode::Stills); });
    m_motionMode = menuAction(view, QStringLiteral("&Motion"), QKeySequence(Qt::CTRL | Qt::Key_2), this,
                              [this] { setMode(Mode::Motion); });
    view->addSeparator();
    m_showSidebar = menuAction(view, QStringLiteral("Si&debar"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), this,
                               [this] { setSidebarShown(!appSettings().value("showSidebar", true).toBool()); });
    m_showSidebar->setCheckable(true);
    m_showSidebar->setToolTip(QStringLiteral("Show or hide the folder's pictures, or in Motion its clips (Ctrl+Shift+S)"));
    m_previous = menuAction(view, QStringLiteral("&Previous in Folder"), QKeySequence(Qt::CTRL | Qt::Key_Up), this, [this] {
        if (m_mode == Mode::Motion)
            m_motion.showNeighbour(-1);
        else
            m_doc.showNeighbour(-1);
    });
    m_next = menuAction(view, QStringLiteral("&Next in Folder"), QKeySequence(Qt::CTRL | Qt::Key_Down), this, [this] {
        if (m_mode == Mode::Motion)
            m_motion.showNeighbour(1);
        else
            m_doc.showNeighbour(1);
    });
    view->addSeparator();
    menuAction(view, QStringLiteral("Next &Surround"), QKeySequence(Qt::CTRL | Qt::Key_B), this,
               [this] { cycleSurround(); });
    // the same key for each half's untouched picture: only one of the two is
    // there at a time
    m_original = menuAction(view, QStringLiteral("&Original"), QKeySequence(Qt::Key_Backslash), this, [this] {
        if (m_doc.hasImage())
            m_doc.setShowOriginal(!m_doc.showOriginal());
    });
    m_original->setCheckable(true);
    m_original->setToolTip(QStringLiteral("The picture before film and paper (\\): the RAW as shot, rendered as macOS "
                                          "renders it, in the print's place. Again for the print."));
    m_logFootage = menuAction(view, QStringLiteral("&Log Footage"), QKeySequence(Qt::Key_Backslash), this,
                              [this] { m_motion.setShowLog(!m_motion.showLog()); });
    m_logFootage->setCheckable(true);
    m_logFootage->setToolTip(QStringLiteral("Show the untouched log footage, as a player shows it (\\)"));

    m_presets = menuBar()->addMenu(QStringLiteral("&Presets"));
    connect(m_presets, &QMenu::aboutToShow, this, [this] { m_doc.reloadPresets(); });

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    menuAction(help, QStringLiteral("agx-emulsion / spektrafilm"), {}, this,
              [] { QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/andreavolpato/spektrafilm"))); });
    menuAction(help, QStringLiteral("NegPy"), {}, this,
              [] { QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/marcinz606/NegPy"))); });
    help->addSeparator();
    menuAction(help, QStringLiteral("&About FLX Film Lab"), {}, this, [this] {
        // how the previews reach the monitor is in Settings
        QMessageBox::about(this, QStringLiteral("FLX Film Lab"),
                           QStringLiteral("<b>FLX Film Lab</b> %1<br>A RAW photo or a log clip through film and paper."
                                          "<br><br>GPU: Direct3D / Metal via Qt RHI<br>Movies: FFmpeg")
                               .arg(QApplication::applicationVersion()));
    });

    // Motion's keys with no menu item of their own: Space plays and pauses,
    // the arrow keys step a frame
    for (auto [key, work] : {std::pair<int, std::function<void()>>{Qt::Key_Space, [this] { m_motion.togglePlay(); }},
                             std::pair<int, std::function<void()>>{Qt::Key_Left, [this] { m_motion.step(-1); }},
                             std::pair<int, std::function<void()>>{Qt::Key_Right, [this] { m_motion.step(1); }}}) {
        auto *s = new QShortcut(QKeySequence(key), this, work);
        s->setEnabled(false);
        m_motionKeys.push_back(s);
    }

    // MARK: the toolbar: the Mac's, in icons (Icons), each group of
    // buttons on a capsule of its own; their names and keys in their tooltips
    QToolBar *bar = addToolBar(QStringLiteral("Toolbar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    bar->setStyleSheet(QStringLiteral(
        "QToolBar { background: #2a2a2a; border: none; border-bottom: 1px solid black; padding: 8px 6px;"
        " spacing: 0; }"
        "#toolGroup { background: #353535; border-radius: 10px; }"
        "#toolGroup QToolButton { background: transparent; border: none; border-radius: 8px; padding: 4px 7px; }"
        "#toolGroup QToolButton:hover { background: #424242; }"
        "#toolGroup QToolButton:checked { background: #575757; }"
        "#toolGroup QToolButton:pressed { background: #4c4c4c; }"
        "#toolGroup QToolButton[plain=\"true\"]:checked { background: transparent; }"
        "#toolGroup QToolButton[plain=\"true\"]:checked:hover { background: #424242; }"
        "#toolGroup QToolButton::menu-indicator { image: none; width: 0; }"));
    // One row across the bar: the left groups, the mode switch and the right
    // groups. Its two sides are kept the same width, so the switch sits in
    // the middle of the window, where the Mac's toolbar puts it, and stays
    // there when a half's own buttons come and go (Motion's right side is
    // narrower); only a window too narrow for both sides would move it.
    auto *row = new QWidget;
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *rowLayout = new QHBoxLayout(row);
    auto *leftSide = new QHBoxLayout;
    auto *rightSide = new QHBoxLayout;
    for (QHBoxLayout *l : {rowLayout, leftSide, rightSide}) {
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(0);
    }
    rowLayout->addLayout(leftSide, 1);
    bar->addWidget(row);
    // each group with room either side of it, on the side being filled
    QHBoxLayout *side = leftSide;
    auto place = [&side](QWidget *w) {
        auto *outer = new QWidget;
        auto *layout = new QHBoxLayout(outer);
        layout->setContentsMargins(4, 0, 4, 0);
        layout->addWidget(w);
        side->addWidget(outer);
        return outer;
    };
    auto group = [](std::initializer_list<QWidget *> members) {
        auto *g = new QFrame;
        g->setObjectName(QStringLiteral("toolGroup"));
        auto *layout = new QHBoxLayout(g);
        layout->setContentsMargins(3, 3, 3, 3);
        layout->setSpacing(2);
        for (QWidget *w : members)
            layout->addWidget(w);
        return g;
    };
    auto button = [](QAction *action, icons::Icon icon, QSize size = QSize(20, 20)) {
        action->setIcon(icons::icon(icon));
        action->setIconVisibleInMenu(false);
        auto *b = new QToolButton;
        b->setDefaultAction(action);
        b->setToolButtonStyle(Qt::ToolButtonIconOnly);
        b->setIconSize(size);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };
    // one of several, in icons: the surround and the mode switch
    auto choices = [&group](QButtonGroup *buttons, std::initializer_list<std::pair<icons::Icon, QString>> items,
                            QSize size) {
        QFrame *g = group({});
        int id = 0;
        for (const auto &[icon, tip] : items) {
            auto *b = new QToolButton;
            b->setIcon(icons::icon(icon));
            b->setIconSize(size);
            b->setCheckable(true);
            b->setToolTip(tip);
            b->setFocusPolicy(Qt::NoFocus);
            b->setCursor(Qt::PointingHandCursor);
            buttons->addButton(b, id++);
            g->layout()->addWidget(b);
        }
        return g;
    };

    auto *open = new QAction(QStringLiteral("Open"), this);
    connect(open, &QAction::triggered, this, [this] { openDialog(); });
    open->setToolTip(QStringLiteral("Open a RAW file, or in Motion a log clip (Ctrl+O); the sidebar shows the pictures "
                                    "or clips of its folder. Open Folder (Ctrl+Shift+O) opens a folder."));
    QToolButton *sidebarButton = button(m_showSidebar, icons::Icon::Sidebar);
    // shown or not, it is the same button, as the Mac's
    sidebarButton->setProperty("plain", true);
    m_showSidebar->setIcon(icons::icon(icons::Icon::Sidebar, false));
    place(group({sidebarButton, button(open, icons::Icon::Folder)}));

    // the surround: dark, middle grey or white, for both halves
    m_surround = new QButtonGroup(this);
    QFrame *surround = choices(m_surround,
                               {{icons::Icon::SurroundDark, QStringLiteral("Dark surround")},
                                {icons::Icon::SurroundGrey, QStringLiteral("Middle grey surround")},
                                {icons::Icon::SurroundWhite, QStringLiteral("White surround")}},
                               QSize(18, 18));
    surround->setToolTip(QStringLiteral(
        "Surround: dark, middle grey or white. Brightness and colour are judged against their surround: the same "
        "print reads heavy against white and washed against black. Ctrl+B cycles."));
    place(surround);
    connect(m_surround, &QButtonGroup::idClicked, this, &MainWindow::setSurround);

    leftSide->addStretch(1);
    // the halves, in the middle
    m_modes = new QButtonGroup(this);
    QFrame *modes = choices(m_modes,
                            {{icons::Icon::Stills, QStringLiteral("Stills: a RAW photo through film and paper (Ctrl+1)")},
                             {icons::Icon::Motion, QStringLiteral("Motion: a log clip, with playback, clip export and "
                                                                  "LUTs (Ctrl+2)")}},
                            QSize(22, 22));
    side = rowLayout;
    place(modes);
    connect(m_modes, &QButtonGroup::idClicked, this, [this](int id) { setMode(id == 1 ? Mode::Motion : Mode::Stills); });
    rightSide->addStretch(1);
    rowLayout->addLayout(rightSide, 1);
    side = rightSide;

    m_copy->setToolTip(QStringLiteral("Copy Settings (Ctrl+Shift+C): this image's film, paper, filter, every slider "
                                      "and auto, and white balance, to paste onto another."));
    // Motion's own, on the same keys: only one of each pair is ever enabled
    m_copyLook = new QAction(QStringLiteral("Copy Look"), this);
    connect(m_copyLook, &QAction::triggered, this, [this] { m_motion.copyLook(); });
    m_copyLook->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    m_copyLook->setToolTip(QStringLiteral("Copy Look (Ctrl+Shift+C): this clip's film, paper, every slider and the "
                                          "effects, to paste onto another."));
    m_pasteLook = new QAction(QStringLiteral("Paste Look"), this);
    connect(m_pasteLook, &QAction::triggered, this, [this] { m_motion.pasteLook(); });
    m_pasteLook->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
    addAction(m_copyLook);
    addAction(m_pasteLook);
    m_presetsButton = new QToolButton;
    m_presetsButton->setIcon(icons::icon(icons::Icon::Presets));
    m_presetsButton->setIconSize(QSize(27, 20));
    m_presetsButton->setMenu(m_presets);
    m_presetsButton->setPopupMode(QToolButton::InstantPopup);
    m_presetsButton->setFocusPolicy(Qt::NoFocus);
    m_presetsButton->setToolTip(QStringLiteral(
        "Presets: settings saved by name, to put on any image later, in this session or another. A preset holds what "
        "Copy Settings copies; the image it is put on keeps its own masks, gradients and crop."));
    // Hide Mask Lines is in the inspector, beside the gradients' and the
    // masks' buttons, and in the Edit menu
    QWidget *settingsGroup =
        place(group({button(m_copy, icons::Icon::Copy), button(m_paste, icons::Icon::Paste), m_presetsButton}));
    QWidget *viewGroup = place(group({button(m_crop, icons::Icon::Crop), button(m_original, icons::Icon::Original),
                                      button(m_loupe, icons::Icon::Loupe)}));
    QWidget *lookGroup = place(group({button(m_copyLook, icons::Icon::Copy), button(m_pasteLook, icons::Icon::Paste)}));
    QWidget *logGroup = place(group({button(m_logFootage, icons::Icon::Original)}));

    m_format = new ToolMenu;
    for (ExportFormat f : {ExportFormat::Tiff16, ExportFormat::Jpeg, ExportFormat::Png})
        m_format->addItem(formatName(f), int(f));
    m_format->setToolTip(QStringLiteral("The format Export writes."));
    m_format->setCurrentIndex(std::clamp(appSettings().value("exportFormat", 0).toInt(), 0, 2));
    connect(m_format, &QComboBox::activated, this, [](int i) { appSettings().setValue("exportFormat", i); });

    m_codec = new ToolMenu;
    for (ExportCodec c : {ExportCodec::ProRes422HQ, ExportCodec::HEVC10})
        m_codec->addItem(QString::fromStdString(exportCodecName(c)), int(c));
    m_codec->setToolTip(QStringLiteral("The clip's format: ProRes 422 HQ for editing and grading further, HEVC 10-bit "
                                       "for sharing."));
    connect(m_codec, &QComboBox::activated, this, [this](int i) {
        MotionLook look = m_motion.look();
        look.exportSettings.codec = ExportCodec(m_codec->itemData(i).toInt());
        m_motion.setLook(look);
    });

    m_stillsItems = {m_copy, m_paste, m_crop, m_original, m_loupe};
    m_motionItems = {m_copyLook, m_pasteLook, m_logFootage};
    m_stillsBar = {settingsGroup, viewGroup, place(m_format)};
    m_motionBar = {lookGroup, logGroup, place(m_codec)};

    m_export = new QAction(QStringLiteral("Export"), this);
    connect(m_export, &QAction::triggered, this, [this] { exportDialog(); });
    place(group({button(m_export, icons::Icon::Export)}));

    const int saved = appSettings().value("canvasBackground", 0).toInt();
    m_surround->button(std::clamp(saved, 0, 2))->setChecked(true);
    setSurround(std::clamp(saved, 0, 2));

    connect(&m_doc, &Document::changed, this, [this] {
        refresh();
        rebuildPresets();
    });
    connect(&m_motion, &MotionDocument::changed, this, &MainWindow::refresh);
    setMode(appSettings().value("mode").toString() == QStringLiteral("Motion") ? Mode::Motion : Mode::Stills);
    setSidebarShown(appSettings().value("showSidebar", true).toBool());
    rebuildPresets();

    if (!restoreGeometry(appSettings().value("windowGeometry").toByteArray()))
        resize(1440, 900);
}

void MainWindow::setMode(Mode mode)
{
    m_mode = mode;
    const bool motion = mode == Mode::Motion;
    m_pages->setCurrentIndex(motion ? 1 : 0);
    m_modes->button(motion ? 1 : 0)->setChecked(true);
    for (QAction *a : m_stillsItems)
        a->setVisible(!motion);
    for (QWidget *w : m_stillsBar)
        w->setVisible(!motion);
    m_editMenu->setVisible(!motion);
    for (QAction *a : m_motionItems)
        a->setVisible(motion);
    for (QWidget *w : m_motionBar)
        w->setVisible(motion);
    for (QShortcut *s : m_motionKeys)
        s->setEnabled(motion);
    m_presets->menuAction()->setVisible(!motion);
    for (QAction *a : {m_turnLeft, m_turnRight})
        a->setEnabled(!motion);
    m_exportLUT->setVisible(motion);
    m_stillsMode->setCheckable(true);
    m_motionMode->setCheckable(true);
    m_stillsMode->setChecked(!motion);
    m_motionMode->setChecked(motion);
    // a clip keeps playing behind the stills half otherwise
    if (!motion)
        m_motion.pause();
    if (motion && m_doc.isCropping())
        m_doc.endCropping(false);
    appSettings().setValue("mode", motion ? QStringLiteral("Motion") : QStringLiteral("Stills"));
    refresh();
}

void MainWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
    if (m_followingScreen)
        return;
    m_followingScreen = true;
    if (QWindow *window = windowHandle())
        connect(window, &QWindow::screenChanged, this, &MainWindow::followScreen);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive)
            followScreen();
    });
    connect(qApp, &QGuiApplication::screenAdded, this, &MainWindow::followScreen);
    connect(qApp, &QGuiApplication::screenRemoved, this, &MainWindow::followScreen);
    followScreen();
}

void MainWindow::followScreen()
{
    if (!displaycolour::setCurrent(displaycolour::read(windowHandle())))
        return;
    if (m_settings)
        m_settings->refresh();
    m_doc.screenChanged();
    m_motion.screenChanged();
    m_canvas->update();
    m_motionCanvas->update();
    for (QAbstractItemView *view : findChildren<QAbstractItemView *>())
        view->viewport()->update();
}

void MainWindow::setSidebarShown(bool shown)
{
    appSettings().setValue("showSidebar", shown);
    m_stillsSidebar->setVisible(shown);
    m_motionSidebar->setVisible(shown);
    m_showSidebar->setChecked(shown);
}

void MainWindow::openFile(const QString &path)
{
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (QFileInfo(absolute).isDir()) {
        // a folder opens in the half on screen, if it has that half's kind of
        // file; otherwise in the half that has
        const bool clips = !FolderBrowser::listing(absolute, FolderBrowser::Kind::Clips).empty();
        const bool pictures = !FolderBrowser::listing(absolute, FolderBrowser::Kind::Pictures).empty();
        const Mode half = m_mode == Mode::Motion ? (clips || !pictures ? Mode::Motion : Mode::Stills)
                                                 : (pictures || !clips ? Mode::Stills : Mode::Motion);
        setMode(half);
        if (half == Mode::Motion)
            m_motion.openFolder(absolute);
        else
            m_doc.openFolder(absolute);
        return;
    }
    if (isMovie(absolute)) {
        setMode(Mode::Motion);
        m_motion.open(absolute);
    } else {
        setMode(Mode::Stills);
        m_doc.open(absolute);
    }
}

void MainWindow::refresh()
{
    const bool motion = m_mode == Mode::Motion;
    const QString shown = motion ? m_motion.clipPath() : m_doc.filePath();
    setWindowTitle(shown.isEmpty() ? QStringLiteral("FLX Film Lab")
                                   : QFileInfo(shown).fileName() + QStringLiteral(" — FLX Film Lab"));
    const auto undoName = motion ? m_motion.history().undoName() : m_doc.history().undoName();
    const auto redoName = motion ? m_motion.history().redoName() : m_doc.history().redoName();
    m_undo->setText(undoName ? QStringLiteral("&Undo %1").arg(QString::fromStdString(*undoName)) : QStringLiteral("&Undo"));
    m_undo->setEnabled(undoName.has_value());
    m_redo->setText(redoName ? QStringLiteral("&Redo %1").arg(QString::fromStdString(*redoName)) : QStringLiteral("&Redo"));
    m_redo->setEnabled(redoName.has_value());
    // Greyed out only while a load has taken a while (isBusy), so stepping
    // through pictures does not flash them; the document still refuses
    // while one is under way.
    const bool ready = m_doc.hasImage() && !m_doc.isBusy();
    m_copy->setEnabled(!motion && ready);
    m_paste->setEnabled(!motion && ready && !m_doc.copiedSource().isEmpty());
    m_paste->setToolTip(!m_doc.copiedSource().isEmpty()
                            ? QStringLiteral("Paste the settings copied from %1 (Ctrl+Shift+V). This image keeps its own "
                                             "masks, gradients and crop.").arg(m_doc.copiedSource())
                            : QStringLiteral("Copy an image's settings first (Ctrl+Shift+C), then paste them onto "
                                             "another."));
    m_crop->setEnabled(!motion && m_doc.hasImage());
    m_crop->setChecked(m_doc.isCropping());
    m_loupe->setEnabled(!motion && m_doc.hasImage());
    m_loupe->setChecked(m_doc.loupeOn());
    m_original->setEnabled(!motion && m_doc.hasImage());
    m_original->setChecked(m_doc.showOriginal());
    m_hideMaskLines->setEnabled(!motion && m_doc.hasImage());
    m_hideMaskLines->setChecked(m_doc.maskLinesHidden());
    const bool canExport = motion ? m_motion.clip().has_value() && !m_motion.isExporting() : ready && !m_doc.isExporting();
    m_export->setEnabled(canExport);
    m_exportMenu->setEnabled(canExport);
    m_export->setToolTip(motion ? QStringLiteral("Write the graded clip (Ctrl+E)")
                                : QStringLiteral("Render at full resolution and write to disk. Waits until the image "
                                                 "on screen has finished loading."));
    m_logFootage->setChecked(m_motion.showLog());
    m_copyLook->setEnabled(motion && m_motion.canCopyLook());
    m_pasteLook->setEnabled(motion && m_motion.hasCopiedLook() && m_motion.clip().has_value());
    m_pasteLook->setToolTip(m_motion.hasCopiedLook()
                                ? QStringLiteral("Paste the look copied from %1 (Ctrl+Shift+V). This clip keeps how its "
                                                 "own footage is read.").arg(m_motion.copiedLookSource())
                                : QStringLiteral("Copy a clip's look first (Ctrl+Shift+C), then paste it onto another."));
    m_codec->setCurrentIndex(m_motion.look().exportSettings.codec == ExportCodec::HEVC10 ? 1 : 0);
}

void MainWindow::setSurround(int id)
{
    m_canvas->setSurround(Surround(id));
    m_motionCanvas->setSurround(Surround(id));
    appSettings().setValue("canvasBackground", id);
}

void MainWindow::cycleSurround()
{
    const int next = (int(m_canvas->surround()) + 1) % 3;
    m_surround->button(next)->setChecked(true);
    setSurround(next);
}

// Saved settings: choose one to put it on the image on screen.
void MainWindow::rebuildPresets()
{
    m_presets->clear();
    const bool can = m_doc.hasImage() && !m_doc.isBusy();
    for (const Preset &preset : m_doc.presets()) {
        QAction *a = m_presets->addAction(preset.name, this, [this, preset] { m_doc.applyPreset(preset); });
        a->setEnabled(can);
    }
    if (m_doc.presets().empty())
        m_presets->addAction(QStringLiteral("No presets yet"))->setEnabled(false);
    m_presets->addSeparator();
    m_presets->addAction(QStringLiteral("Save Settings as Preset…"), this, [this] { savePresetDialog(); })
        ->setEnabled(can);
    if (!m_doc.presets().empty()) {
        QMenu *remove = m_presets->addMenu(QStringLiteral("Delete Preset"));
        for (const Preset &preset : m_doc.presets())
            remove->addAction(preset.name, this, [this, name = preset.name] { m_doc.deletePreset(name); });
    }
    m_presets->addAction(QStringLiteral("Show Presets Folder"), this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_doc.presetsFolder()));
    });
}

// The new preset's name, and whether it replaces one, as it is typed.
void MainWindow::savePresetDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Save Settings as Preset"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *heading = new QLabel(QStringLiteral("Save Settings as Preset"));
    QFont h = theme::label();
    h.setPixelSize(14);
    h.setWeight(QFont::DemiBold);
    heading->setFont(h);
    layout->addWidget(heading);
    layout->addWidget(note(QStringLiteral(
        "Film, paper, print or light table, camera filter, every slider and auto, and white balance, to put on any "
        "image later. Masks, gradients and the crop stay with their image.")));
    auto *name = new QLineEdit;
    const ProfilePack *film = m_doc.filmPack();
    name->setText(film ? QString::fromStdString(film->name) : QString());
    name->selectAll();
    layout->addWidget(name);
    auto *warning = new QLabel(QStringLiteral(" "));
    warning->setFont(theme::small());
    warning->setStyleSheet(QStringLiteral("color: #ff9f0a;"));
    layout->addWidget(warning);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    QPushButton *save = buttons->button(QDialogButtonBox::Save);
    auto update = [&] {
        const QString trimmed = name->text().trimmed();
        bool replaces = false;
        for (const Preset &p : m_doc.presets())
            replaces = replaces || p.name == trimmed;
        warning->setText(replaces ? QStringLiteral("Replaces the preset of that name.") : QStringLiteral(" "));
        save->setText(replaces ? QStringLiteral("Replace") : QStringLiteral("Save"));
        save->setEnabled(!trimmed.isEmpty());
    };
    connect(name, &QLineEdit::textChanged, &dialog, update);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    update();
    dialog.setFixedWidth(380);
    if (dialog.exec() == QDialog::Accepted)
        m_doc.savePreset(name->text());
}

void MainWindow::openDialog()
{
    if (m_mode == Mode::Motion) {
        const QString folder =
            appSettings()
                .value("openClipFolder", QStandardPaths::writableLocation(QStandardPaths::MoviesLocation))
                .toString();
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Choose a log clip"), folder,
                                                          movieFilter());
        if (path.isEmpty())
            return;
        appSettings().setValue("openClipFolder", QFileInfo(path).absolutePath());
        m_motion.open(path);
        return;
    }
    const QString folder = appSettings()
                               .value("openFolder", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation))
                               .toString();
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open"), folder, openFilter());
    if (path.isEmpty())
        return;
    appSettings().setValue("openFolder", QFileInfo(path).absolutePath());
    m_doc.open(path);
}

void MainWindow::openFolderDialog()
{
    const bool motion = m_mode == Mode::Motion;
    const char *key = motion ? "openClipFolder" : "openFolder";
    const QString start = appSettings()
                              .value(key, QStandardPaths::writableLocation(motion ? QStandardPaths::MoviesLocation
                                                                                  : QStandardPaths::PicturesLocation))
                              .toString();
    const QString folder = QFileDialog::getExistingDirectory(
        this, motion ? QStringLiteral("Open a folder of clips") : QStringLiteral("Open a folder of pictures"), start);
    if (folder.isEmpty())
        return;
    appSettings().setValue(key, folder);
    if (motion)
        m_motion.openFolder(folder);
    else
        m_doc.openFolder(folder);
}

void MainWindow::exportSelectedDialog()
{
    const bool motion = m_mode == Mode::Motion;
    const char *key = motion ? "exportClipsFolder" : "exportFolder";
    const QString folder = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Export the selection to"), appSettings().value(key).toString());
    if (folder.isEmpty())
        return;
    appSettings().setValue(key, folder);
    if (motion)
        m_motion.exportSelected(folder);
    else
        m_doc.exportSelected(folder, ExportFormat(m_format->currentData().toInt()));
}

QString MainWindow::exportFilename() const
{
    const QString stem = m_doc.filePath().isEmpty() ? QStringLiteral("FLX Film Lab")
                                                    : QFileInfo(m_doc.filePath()).completeBaseName();
    const ProfilePack *film = const_cast<Document &>(m_doc).filmPack();
    QString name = film ? QString::fromStdString(film->name) : QStringLiteral("film");
    name.remove(QLatin1Char(' '));
    return stem + QLatin1Char('_') + name;
}

void MainWindow::exportDialog()
{
    if (m_mode == Mode::Motion) {
        if (!m_motion.clip() || m_motion.isExporting())
            return;
        const QString folder = QFileInfo(m_motion.clipPath()).absolutePath();
        QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export"),
                                                    QDir(folder).filePath(m_motion.suggestedExportName()),
                                                    QStringLiteral("QuickTime movie (*.mov)"));
        if (path.isEmpty())
            return;
        if (!path.endsWith(QStringLiteral(".mov"), Qt::CaseInsensitive))
            path += QStringLiteral(".mov");
        // the clip itself is never written over
        if (QFileInfo(path).canonicalFilePath() == QFileInfo(m_motion.clipPath()).canonicalFilePath()) {
            QMessageBox::warning(this, QStringLiteral("Export"),
                                 QStringLiteral("The export would replace the clip it is made from. Choose another "
                                                "name."));
            return;
        }
        m_motion.exportTo(path);
        return;
    }
    if (!m_doc.canExport())
        return;
    const ExportFormat format = ExportFormat(m_format->currentData().toInt());
    const QString folder = appSettings().value("exportFolder", QFileInfo(m_doc.filePath()).absolutePath()).toString();
    const QString suggested = QDir(folder).filePath(exportFilename() + QLatin1Char('.') + formatExtension(format));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export"), suggested,
        QStringLiteral("%1 (*.%2)").arg(formatName(format), formatExtension(format)));
    if (path.isEmpty())
        return;
    appSettings().setValue("exportFolder", QFileInfo(path).absolutePath());
    m_doc.exportTo(path, format);
}

void MainWindow::exportLUTDialog()
{
    const QString folder =
        appSettings()
            .value("lutFolder", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
            .toString();
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export LUT"),
                                                QDir(folder).filePath(m_motion.suggestedLUTName()),
                                                QStringLiteral("Cube LUT (*.cube)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QStringLiteral(".cube"), Qt::CaseInsensitive))
        path += QStringLiteral(".cube");
    appSettings().setValue("lutFolder", QFileInfo(path).absolutePath());
    m_motion.exportLUT(path);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls() && e->mimeData()->urls().first().isLocalFile())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    const QList<QUrl> urls = e->mimeData()->urls();
    if (!urls.isEmpty() && urls.first().isLocalFile())
        openFile(urls.first().toLocalFile());
}

void MainWindow::changeEvent(QEvent *e)
{
    // a preset file may have been copied in, or taken away, meanwhile
    if (e->type() == QEvent::ActivationChange && isActiveWindow())
        m_doc.reloadPresets();
    QMainWindow::changeEvent(e);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    // an export under way would be lost: say so first
    if (m_doc.isBatchExporting()
        && QMessageBox::question(this, QStringLiteral("FLX Film Lab"),
                                 QStringLiteral("Pictures are still being exported. Quit and stop after the one being "
                                                "written now?"))
               != QMessageBox::Yes) {
        e->ignore();
        return;
    }
    if (m_motion.isExporting()
        && QMessageBox::question(this, QStringLiteral("FLX Film Lab"),
                                 QStringLiteral("A clip is still being exported. Quit and cancel it?"))
               != QMessageBox::Yes) {
        e->ignore();
        return;
    }
    appSettings().setValue("windowGeometry", saveGeometry());
    m_doc.prepareToQuit();
    m_motion.prepareToQuit();
    QMainWindow::closeEvent(e);
}
