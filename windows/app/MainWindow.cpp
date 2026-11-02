#include "MainWindow.h"

#include "AppSettings.h"
#include "Canvas.h"
#include "Document.h"
#include "Exporter.h"
#include "Inspector.h"
#include "MotionCanvas.h"
#include "MotionDocument.h"
#include "MotionInspector.h"
#include "Theme.h"
#include "TransportBar.h"
#include "Widgets.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

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

// Side by side buttons, one of them on: the surround and the mode switch.
QWidget *segmented(const QStringList &names, QButtonGroup *group)
{
    auto *box = new QWidget;
    auto *layout = new QHBoxLayout(box);
    layout->setContentsMargins(6, 0, 6, 0);
    layout->setSpacing(0);
    for (int i = 0; i < names.size(); ++i) {
        auto *b = new QPushButton(names[i]);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
        const char *radius = i == 0 ? "5px 0 0 5px" : i == names.size() - 1 ? "0 5px 5px 0" : "0";
        b->setStyleSheet(QStringLiteral("QPushButton { background: #333; color: #c8c8c8; border: 1px solid #444;"
                                        " border-radius: %1; padding: 3px 9px; }"
                                        "QPushButton:checked { background: #5a5a5a; color: white; }")
                             .arg(QLatin1StringView(radius)));
        group->addButton(b, i);
        layout->addWidget(b);
    }
    return box;
}

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

QWidget *MainWindow::buildStills()
{
    auto *page = new QWidget;
    auto *layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_canvas = new Canvas(m_doc);
    m_inspector = new Inspector(m_doc);
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
    m_motionInspector = new MotionInspector(m_motion, m_inspector->vocabulary());
    layout->addWidget(left, 1);
    layout->addWidget(divider());
    layout->addWidget(m_motionInspector);
    connect(m_motionCanvas, &MotionCanvas::openRequested, this, &MainWindow::openDialog);
    connect(m_motionInspector, &MotionInspector::exportRequested, this, &MainWindow::exportDialog);
    connect(m_motionInspector, &MotionInspector::exportLUTRequested, this, &MainWindow::exportLUTDialog);
    return page;
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
    m_exportMenu = menuAction(file, QStringLiteral("&Export…"), QKeySequence(Qt::CTRL | Qt::Key_E), this,
                             [this] { exportDialog(); });
    m_exportLUT = menuAction(file, QStringLiteral("Export &LUT…"), {}, this, [this] { exportLUTDialog(); });
    file->addSeparator();
    menuAction(file, QStringLiteral("E&xit"), QKeySequence::Quit, this, [this] { close(); });

    QMenu *edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    m_editMenu = edit->menuAction();
    m_copy = menuAction(edit, QStringLiteral("&Copy Settings"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this,
                       [this] { m_doc.copySettings(); });
    m_paste = menuAction(edit, QStringLiteral("&Paste Settings"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), this,
                        [this] { m_doc.pasteSettings(); });
    edit->addSeparator();
    m_crop = menuAction(edit, QStringLiteral("C&rop"), QKeySequence(Qt::CTRL | Qt::Key_R), this,
                        [this] { m_doc.setCropping(!m_doc.isCropping()); });
    m_crop->setCheckable(true);
    m_crop->setToolTip(QStringLiteral("Crop, turn and level the print (Ctrl+R): the whole frame shows with the crop on "
                                      "it. Return keeps the crop, Escape goes back."));
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
    menuAction(view, QStringLiteral("Next &Surround"), QKeySequence(Qt::CTRL | Qt::Key_B), this,
               [this] { cycleSurround(); });
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

    // MARK: the toolbar
    QToolBar *bar = addToolBar(QStringLiteral("Toolbar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    bar->setStyleSheet(QStringLiteral("QToolBar { background: #2a2a2a; border: none; border-bottom: 1px solid black;"
                                      " padding: 4px 8px; spacing: 6px; }"
                                      "QToolButton { padding: 4px 10px; border-radius: 5px; }"
                                      "QToolButton:hover { background: #3a3a3a; }"
                                      "QToolButton:checked { background: #4a4a4a; }"
                                      "QToolButton:disabled { color: #666; }"));
    bar->addAction(QStringLiteral("Open"), this, [this] { openDialog(); })
        ->setToolTip(QStringLiteral("Open a RAW file, or in Motion a log clip (Ctrl+O)"));

    // the surround: dark, middle grey or white, for both halves
    m_surround = new QButtonGroup(this);
    QWidget *surround = segmented({QStringLiteral("Dark"), QStringLiteral("Grey"), QStringLiteral("White")}, m_surround);
    surround->setToolTip(QStringLiteral(
        "Surround: dark, middle grey or white. Brightness and colour are judged against their surround: the same "
        "print reads heavy against white and washed against black. Ctrl+B cycles."));
    bar->addWidget(surround);
    connect(m_surround, &QButtonGroup::idClicked, this, &MainWindow::setSurround);

    auto spacer = [] {
        auto *s = new QWidget;
        s->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        return s;
    };
    bar->addWidget(spacer());
    // the halves, in the middle
    m_modes = new QButtonGroup(this);
    QWidget *modes = segmented({QStringLiteral("Stills"), QStringLiteral("Motion")}, m_modes);
    modes->setToolTip(QStringLiteral("Stills: a RAW photo through film and paper. Motion: a log clip, with playback, "
                                     "clip export and LUTs. Ctrl+1 and Ctrl+2 switch."));
    bar->addWidget(modes);
    connect(m_modes, &QButtonGroup::idClicked, this, [this](int id) { setMode(id == 1 ? Mode::Motion : Mode::Stills); });
    bar->addWidget(spacer());

    bar->addAction(m_copy);
    m_copy->setToolTip(QStringLiteral("Copy this image's settings to paste onto another (Ctrl+Shift+C): film, paper, "
                                      "filter, every slider and auto, and white balance."));
    bar->addAction(m_paste);
    m_presetsButton = new QToolButton;
    m_presetsButton->setText(QStringLiteral("Presets"));
    m_presetsButton->setMenu(m_presets);
    m_presetsButton->setPopupMode(QToolButton::InstantPopup);
    m_presetsButton->setToolTip(QStringLiteral(
        "Presets: settings saved by name, to put on any image later, in this session or another. A preset holds what "
        "Copy Settings copies; the image it is put on keeps its own masks, gradients and crop."));
    m_stillsItems = {m_copy, m_paste, bar->addWidget(m_presetsButton), m_crop};
    bar->addAction(m_crop);

    bar->addAction(m_logFootage);
    m_motionItems.push_back(m_logFootage);

    m_format = new QComboBox;
    for (ExportFormat f : {ExportFormat::Tiff16, ExportFormat::Jpeg, ExportFormat::Png})
        m_format->addItem(formatName(f), int(f));
    m_format->setToolTip(QStringLiteral("The format Export writes."));
    m_format->setCurrentIndex(std::clamp(appSettings().value("exportFormat", 0).toInt(), 0, 2));
    connect(m_format, &QComboBox::activated, this, [](int i) { appSettings().setValue("exportFormat", i); });
    m_stillsItems.push_back(bar->addWidget(m_format));

    m_codec = new QComboBox;
    for (ExportCodec c : {ExportCodec::ProRes422HQ, ExportCodec::HEVC10})
        m_codec->addItem(QString::fromStdString(exportCodecName(c)), int(c));
    m_codec->setToolTip(QStringLiteral("The clip's format: ProRes 422 HQ for editing and grading further, HEVC 10-bit "
                                       "for sharing."));
    connect(m_codec, &QComboBox::activated, this, [this](int i) {
        MotionLook look = m_motion.look();
        look.exportSettings.codec = ExportCodec(m_codec->itemData(i).toInt());
        m_motion.setLook(look);
    });
    m_motionItems.push_back(bar->addWidget(m_codec));

    m_export = bar->addAction(QStringLiteral("Export"), this, [this] { exportDialog(); });

    const int saved = appSettings().value("canvasBackground", 0).toInt();
    m_surround->button(std::clamp(saved, 0, 2))->setChecked(true);
    setSurround(std::clamp(saved, 0, 2));

    connect(&m_doc, &Document::changed, this, [this] {
        refresh();
        rebuildPresets();
    });
    connect(&m_motion, &MotionDocument::changed, this, &MainWindow::refresh);
    setMode(appSettings().value("mode").toString() == QStringLiteral("Motion") ? Mode::Motion : Mode::Stills);
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
    m_editMenu->setVisible(!motion);
    for (QAction *a : m_motionItems)
        a->setVisible(motion);
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

void MainWindow::openFile(const QString &path)
{
    const QString absolute = QFileInfo(path).absoluteFilePath();
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
    m_copy->setEnabled(!motion && m_doc.canCopySettings());
    m_paste->setEnabled(!motion && m_doc.canPasteSettings());
    m_paste->setToolTip(m_doc.canPasteSettings()
                            ? QStringLiteral("Paste the settings copied from %1 (Ctrl+Shift+V). This image keeps its own "
                                             "masks, gradients and crop.").arg(m_doc.copiedSource())
                            : QStringLiteral("Copy an image's settings first (Ctrl+Shift+C), then paste them onto "
                                             "another."));
    m_crop->setEnabled(!motion && m_doc.hasImage());
    m_crop->setChecked(m_doc.isCropping());
    const bool canExport = motion ? m_motion.clip().has_value() && !m_motion.isExporting() : m_doc.canExport();
    m_export->setEnabled(canExport);
    m_exportMenu->setEnabled(canExport);
    m_export->setToolTip(motion ? QStringLiteral("Write the graded clip (Ctrl+E)")
                                : QStringLiteral("Render at full resolution and write to disk. Waits until the image "
                                                 "on screen has finished loading."));
    m_logFootage->setChecked(m_motion.showLog());
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
    const bool can = m_doc.canCopySettings();
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
    if (m_motion.isExporting()
        && QMessageBox::question(this, QStringLiteral("FLX Film Lab"),
                                 QStringLiteral("A clip is still being exported. Quit and cancel it?"))
               != QMessageBox::Yes) {
        e->ignore();
        return;
    }
    appSettings().setValue("windowGeometry", saveGeometry());
    QMainWindow::closeEvent(e);
}
