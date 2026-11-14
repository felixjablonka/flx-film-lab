#include "SelfTest.h"

#include "AppSettings.h"
#include "DisplayColour.h"
#include "Document.h"
#include "FolderSidebar.h"
#include "Inspector.h"
#include "MainWindow.h"
#include "MotionDocument.h"
#include "MotionInspector.h"
#include "SettingsDialog.h"
#include "Theme.h"
#include "Thumbnails.h"
#include "Vocabulary.h"
#include "Widgets.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColorTransform>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QListView>
#include <QAbstractScrollArea>
#include <QMenu>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>

namespace {

int g_failures = 0;

void check(bool ok, const QString &what)
{
    std::printf("   %s %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    std::fflush(stdout);
    if (!ok)
        ++g_failures;
}

// The event loop turned until `condition` holds, for at most `ms`.
bool waitUntil(const std::function<bool()> &condition, int ms)
{
    QElapsedTimer clock;
    clock.start();
    while (!condition() && clock.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return condition();
}

QStringList names(const std::vector<FolderBrowser::Item> &items)
{
    QStringList out;
    for (const auto &item : items)
        out << item.name();
    return out;
}

std::vector<std::pair<QString, qint64>> files(const std::vector<FolderBrowser::Item> &items)
{
    std::vector<std::pair<QString, qint64>> out;
    for (const auto &item : items)
        out.emplace_back(item.path, item.size);
    return out;
}

void pictureChecks(MainWindow &window, Document &doc, const QString &pictures, const QString &scratch)
{
    using Flag = ImageLibrary::Flag;
    // four RAW files copied into a roll of their own, with a note beside them
    const QString roll = QDir(scratch).filePath(QStringLiteral("Roll"));
    QDir().mkpath(roll);
    QStringList copied;
    for (const FolderBrowser::Item &item : FolderBrowser::listing(pictures, FolderBrowser::Kind::Pictures)) {
        if (copied.size() == 4)
            break;
        if (QFileInfo(item.path).suffix().compare(QStringLiteral("nef"), Qt::CaseInsensitive) != 0)
            continue;
        QFile::copy(item.path, QDir(roll).filePath(item.name()));
        copied << item.name();
    }
    QFile note(QDir(roll).filePath(QStringLiteral("notes.txt")));
    if (note.open(QIODevice::WriteOnly))
        note.write("notes");
    note.close();
    auto settled = [&] {
        return waitUntil([&] {
            return doc.hasImage() && !doc.isDecoding() && !doc.isRendering() && doc.browser().current() == doc.filePath()
                && !doc.displayImage().isNull();
        }, 60000);
    };

    std::printf("open a folder of pictures\n");
    window.openFile(roll);
    const auto items = doc.browser().items();
    check(window.mode() == MainWindow::Mode::Stills && names(items) == copied,
          QStringLiteral("its pictures by name, not its note, in the Stills half: %1").arg(names(items).join(", ")));
    if (items.size() < 4) {
        check(false, QStringLiteral("four RAW files to test with in %1").arg(pictures));
        return;
    }
    check(settled() && doc.filePath() == items[0].path && doc.browser().selection() == std::set<QString>{items[0].path},
          QStringLiteral("the first on screen and selected"));
    check(waitUntil([&] { return doc.prefetchedPictures().count(items[1].path) > 0; }, 30000),
          QStringLiteral("the next one decoded ahead"));
    bool flashed = false;
    doc.showNeighbour(1);
    waitUntil([&] {
        flashed = flashed || doc.isBusy();
        return doc.filePath() == items[1].path && !doc.isRendering() && !doc.displayImage().isNull();
    }, 30000);
    check(doc.filePath() == items[1].path && !flashed, QStringLiteral("stepping to it never shows a load"));
    check(settled(), QStringLiteral("and it prints"));

    std::printf("a look of its own, kept\n");
    Recipe changed = doc.recipe();
    changed.autoExposure = false;
    changed.exposureEV += 0.7;
    doc.setRecipe(changed);
    const QString one = items[1].path;
    check(waitUntil([&] {
              const auto r = doc.library().record(one);
              return r && r->edits && r->edits->recipe.exposureEV == changed.exposureEV;
          }, 5000),
          QStringLiteral("a picture changed keeps its settings in the library"));
    const auto lookedAt = doc.library().record(items[0].path);
    check(!lookedAt || !lookedAt->edits, QStringLiteral("one only looked at keeps none"));
    check(doc.browser().edited().count(one) && !doc.browser().edited().count(items[0].path),
          QStringLiteral("the sidebar marks the one with settings of its own"));

    std::printf("flags\n");
    doc.browser().setSelection({items[2].path});
    doc.flagSelection(Flag::Reject);
    doc.browser().setSelection({items[0].path});
    doc.flagSelection(Flag::Pick);
    check(doc.browser().flag(items[2].path) == Flag::Reject && doc.browser().flag(items[0].path) == Flag::Pick
              && doc.library().record(items[2].path)->flag == Flag::Reject,
          QStringLiteral("X rejects and P picks, in the library too"));
    doc.browser().setFilter(FolderBrowser::Filter::WithoutRejects);
    check(doc.browser().shown().size() == 3 && doc.browser().neighbour(one, 1) == items[3].path,
          QStringLiteral("No Rejects leaves the reject out, and stepping skips it"));
    doc.browser().setFilter(FolderBrowser::Filter::Picks);
    check(doc.browser().shown().size() == 1, QStringLiteral("Picks shows the pick alone"));
    doc.browser().setFilter(FolderBrowser::Filter::All);
    doc.browser().setSelection({items[1].path, items[2].path, items[3].path});
    check(doc.browser().selectedForBatch() == std::vector<QString>{items[1].path, items[3].path},
          QStringLiteral("a batch leaves the reject out"));

    std::printf("kept between launches\n");
    doc.library().save(true);
    {
        ImageLibrary again(QDir(scratch).filePath(QStringLiteral("Library")));
        auto records = again.load(roll, files(items));
        check(records[one].edits && records[one].edits->recipe.exposureEV == changed.exposureEV
                  && records[items[2].path].flag == Flag::Reject && records[items[0].path].flag == Flag::Pick,
              QStringLiteral("a new session finds the settings and the flags"));
    }

    std::printf("the sidebar's pictures\n");
    const bool pictured = waitUntil([&] { return doc.browser().thumbnails().count() >= 4; }, 30000);
    check(pictured, QStringLiteral("each row has its picture: %1").arg(doc.browser().thumbnails().count()));
    const Look kept{doc.recipe(), doc.whiteBalance(), doc.customTemperature(), doc.customTint()};
    doc.showNeighbour(1);
    settled();
    const QString print = QDir(ThumbnailStore::cacheDirectory()).filePath(ThumbnailStore::printKey(one, kept) + ".jpg");
    check(waitUntil([&] { return QFileInfo::exists(print); }, 5000),
          QStringLiteral("leaving it, its print from the screen is kept for the sidebar"));

    std::printf("copy and paste to the selection\n");
    doc.open(one);
    settled();
    doc.copySettings();
    doc.browser().setSelection({items[0].path, items[2].path, items[3].path});
    doc.pasteSettingsToSelected();
    auto exposureOf = [&](const QString &path) {
        const auto r = doc.library().record(path);
        return r && r->edits ? r->edits->recipe.exposureEV : -100.0;
    };
    check(exposureOf(items[0].path) == changed.exposureEV && exposureOf(items[3].path) == changed.exposureEV
              && exposureOf(items[2].path) == -100.0,
          QStringLiteral("Paste Settings to Selected: the others get it, the reject not"));
    doc.open(items[3].path);
    settled();
    check(doc.history().undoName() == std::optional<std::string>("Paste Settings") && doc.recipe().exposureEV == changed.exposureEV,
          QStringLiteral("the paste a step of the picture's own history: %1")
              .arg(QString::fromStdString(doc.history().undoName().value_or("nothing to undo"))));

    std::printf("export the selection\n");
    const QString out = QDir(scratch).filePath(QStringLiteral("Prints"));
    QDir().mkpath(out);
    doc.browser().setSelection({items[1].path, items[2].path, items[3].path});
    doc.exportSelected(out, ExportFormat::Jpeg);
    check(doc.browser().batch() && doc.browser().batch()->total == 2, QStringLiteral("two to print: the reject left out"));
    check(waitUntil([&] { return !doc.browser().batch(); }, 300000), QStringLiteral("the batch finishes"));
    const QStringList made = QDir(out).entryList(QDir::Files, QDir::Name);
    const auto film = QString::fromStdString(doc.filmPack() ? doc.filmPack()->name : "film").remove(QLatin1Char(' '));
    check(made == QStringList({QFileInfo(one).completeBaseName() + "_" + film + ".jpg",
                               QFileInfo(items[3].path).completeBaseName() + "_" + film + ".jpg"}),
          QStringLiteral("each named by the picture and its film: %1").arg(made.join(", ")));

    std::printf("settings beside the pictures\n");
    doc.browser().setSavesSidecars(true);
    doc.library().finishWriting();
    check(QFileInfo::exists(ImageLibrary::sidecarPath(one)) && QFileInfo::exists(ImageLibrary::sidecarPath(items[2].path)),
          QStringLiteral("turned on, each picture with a record has its .flx beside it"));
    doc.browser().setSavesSidecars(false);
}

void clipChecks(MainWindow &window, MotionDocument &motion, const QString &clips, const QString &scratch)
{
    using Flag = ImageLibrary::Flag;
    std::printf("open a folder of clips\n");
    // the two smallest of CLIPS copied into a folder of their own, as the
    // pictures are: what the checks write beside a clip never reaches the
    // originals
    const QString footage = QDir(scratch).filePath(QStringLiteral("Footage"));
    QDir().mkpath(footage);
    auto listed = FolderBrowser::listing(clips, FolderBrowser::Kind::Clips);
    std::sort(listed.begin(), listed.end(), [](const auto &a, const auto &b) { return a.size < b.size; });
    for (size_t i = 0; i < listed.size() && i < 2; ++i)
        QFile::copy(listed[i].path, QDir(footage).filePath(listed[i].name()));
    window.setMode(MainWindow::Mode::Motion);
    window.openFile(footage);
    const auto items = motion.browser().items();
    check(window.mode() == MainWindow::Mode::Motion && items.size() >= 2,
          QStringLiteral("its clips, in the Motion half: %1").arg(names(items).join(", ")));
    if (items.size() < 2)
        return;
    auto settled = [&] {
        return waitUntil([&] {
            return motion.clip() && !motion.isOpening() && !motion.isRendering() && !motion.image().isNull()
                && !motion.isMetering() && motion.browser().current() == motion.clipPath();
        }, 60000);
    };
    check(settled() && motion.clipPath() == items[0].path, QStringLiteral("the first on screen"));
    motion.showNeighbour(1);
    check(waitUntil([&] { return motion.clipPath() == items[1].path; }, 30000) && settled(),
          QStringLiteral("stepping to the next"));

    std::printf("a look of its own, kept\n");
    MotionLook look = motion.look();
    look.recipe.brightness += 0.15;
    motion.setLook(look);
    const QString one = items[1].path;
    check(waitUntil([&] {
              const auto r = motion.browser().library ? motion.browser().library->record(one) : std::nullopt;
              return r && r->look && r->look->recipe.brightness == look.recipe.brightness;
          }, 5000),
          QStringLiteral("a clip changed keeps its look in the library"));
    check(motion.browser().edited().count(one) > 0, QStringLiteral("the sidebar marks it"));
    MotionLook grade = motion.look();
    grade.exportSettings = {};
    const QString graded = QDir(ThumbnailStore::cacheDirectory()).filePath(ThumbnailStore::printKey(one, grade) + ".jpg");
    check(waitUntil([&] { return QFileInfo::exists(graded); }, 60000),
          QStringLiteral("its frame graded with its look, for the sidebar"));

    std::printf("flags, copy and paste\n");
    motion.browser().setSelection({items[0].path});
    motion.flagSelection(Flag::Pick);
    check(motion.browser().flag(items[0].path) == Flag::Pick, QStringLiteral("P picks a clip"));
    motion.copyLook();
    motion.pasteLookToSelected();
    const auto pasted = motion.browser().library->record(items[0].path);
    check(pasted && pasted->look && pasted->look->recipe.brightness == look.recipe.brightness,
          QStringLiteral("Paste Look to Selected gives the other clip the look"));

    std::printf("export the selection\n");
    // the smallest clip, so the test is quick
    const auto smallest = *std::min_element(items.begin(), items.end(),
                                            [](const auto &a, const auto &b) { return a.size < b.size; });
    const QString out = QDir(scratch).filePath(QStringLiteral("Clips"));
    QDir().mkpath(out);
    motion.browser().setSelection({smallest.path});
    motion.exportSelected(out);
    check(waitUntil([&] { return !motion.browser().batch() && !motion.isExporting(); }, 600000),
          QStringLiteral("the batch finishes"));
    const QStringList made = QDir(out).entryList(QDir::Files);
    check(made.size() == 1 && made[0].startsWith(QFileInfo(smallest.path).completeBaseName() + " ")
              && QFileInfo(QDir(out).filePath(made[0])).size() > 0,
          QStringLiteral("the clip written with its look: %1").arg(made.join(", ")));
}

// The previews colour-managed for a monitor with a profile of its own: a
// wide-gamut panel's, P3 with a 2.2 gamma, standing in for Windows' answer.
void displayChecks(Document &doc, MotionDocument &motion, const QString &scratch)
{
    std::printf("the previews on a wide-gamut monitor\n");
    const QString file = QDir(scratch).filePath(QStringLiteral("wide-gamut-monitor.icc"));
    QColorSpace monitor(QColorSpace::Primaries::DciP3D65, 2.2f);
    monitor.setDescription(QStringLiteral("A wide-gamut monitor"));
    QFile out(file);
    if (out.open(QIODevice::WriteOnly))
        out.write(monitor.iccProfile());
    out.close();
    qputenv("FLX_DISPLAY_PROFILE", file.toLocal8Bit());
    const displaycolour::State state = displaycolour::read(nullptr);
    check(state.space.isValid() && state.description == QStringLiteral("A wide-gamut monitor"),
          QStringLiteral("its profile read: %1").arg(state.description));
    auto frame = [&](const std::function<void()> &change) {
        const qint64 before = doc.shownImage().cacheKey();
        change();
        return waitUntil([&] { return doc.shownImage().cacheKey() != before && !doc.isRendering(); }, 30000);
    };
    // sRGB's print, then the same converted for the monitor
    Recipe r = doc.recipe();
    r.outputColorSpace = "sRGB";
    frame([&] { doc.setRecipe(r); });
    const QImage srgb = doc.displayImage();
    frame([&] {
        displaycolour::setCurrent(state);
        doc.screenChanged();
    });
    const QImage shown = doc.shownImage();
    QImage back = shown.convertToFormat(QImage::Format_RGB32);
    back.setColorSpace(monitor);
    back.convertToColorSpace(QColorSpace(QColorSpace::SRgb));
    int differ = 0, worst = 0;
    for (int y = 0; y < srgb.height(); y += 7)
        for (int x = 0; x < srgb.width(); x += 7) {
            const QRgb a = srgb.pixel(x, y), b = back.pixel(x, y), c = shown.pixel(x, y);
            worst = std::max({worst, std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
            differ += a != c;
        }
    check(differ > 0 && worst <= 3,
          QStringLiteral("the print converted for it, the same print brought back to sRGB: %1 of the pixels sampled "
                         "differ on the screen, %2 levels at most back in sRGB").arg(differ).arg(worst));
    const QRgb grey = displaycolour::toScreen(QColor::fromRgbF(0.4663f, 0.4663f, 0.4663f)).rgb();
    check(qRed(grey) != 119 && qRed(grey) == qGreen(grey) && qGreen(grey) == qBlue(grey),
          QStringLiteral("the middle grey surround too, still grey: %1").arg(qRed(grey)));
    const QString path = doc.filePath();
    const QImage thumb = doc.browser().thumbnails().image(path), thumbShown = doc.browser().thumbnails().shown(path);
    check(!thumb.isNull() && thumbShown.size() == thumb.size() && thumbShown != thumb,
          QStringLiteral("and the sidebar's picture"));
    // a Display P3 print reaches the monitor whole, nothing clipped to sRGB on the way
    r.outputColorSpace = "Display P3";
    frame([&] { doc.setRecipe(r); });
    QImage wide = doc.shownImage().convertToFormat(QImage::Format_RGB32);
    wide.setColorSpace(monitor);
    const QColorTransform toSrgb = monitor.transformationToColorSpace(QColorSpace(QColorSpace::SRgb));
    int beyond = 0;
    for (int y = 0; y < wide.height(); y += 3)
        for (int x = 0; x < wide.width(); x += 3) {
            const QColor c = toSrgb.map(QColor(wide.pixel(x, y)).toRgb().convertTo(QColor::ExtendedRgb));
            beyond += c.redF() > 1.01f || c.greenF() > 1.01f || c.blueF() > 1.01f || c.redF() < -0.01f
                || c.greenF() < -0.01f || c.blueF() < -0.01f;
        }
    check(beyond > 0, QStringLiteral("a Display P3 print shows colours past sRGB on it: %1 of the pixels sampled").arg(beyond));
    // Motion's frame follows the same monitor, as the window tells both halves
    if (motion.clip()) {
        const qint64 before = motion.shownImage().cacheKey();
        motion.screenChanged();
        waitUntil([&] { return motion.shownImage().cacheKey() != before && !motion.isRendering(); }, 30000);
        check(!motion.shownImage().isNull() && motion.shownImage() != motion.image(),
              QStringLiteral("Motion's frame, converted for it"));
    }
    // back to a monitor that is sRGB: the print as it was
    qunsetenv("FLX_DISPLAY_PROFILE");
    r.outputColorSpace = "sRGB";
    frame([&] {
        displaycolour::setCurrent({});
        doc.screenChanged();
        doc.setRecipe(r);
    });
    check(doc.shownImage().convertToFormat(QImage::Format_RGB32) == doc.displayImage().convertToFormat(QImage::Format_RGB32),
          QStringLiteral("on an sRGB monitor the screen is given the sRGB print"));
}

// Settings (File > Settings…): Text size makes the panels again at its size,
// keeping what they show; Control names, Preview size and Save Settings
// Beside Images reach what they change; the monitor's colour is said there.
// Each setting is put back as it was.
// Original (\): the picture before film and paper, in the print's place.
void originalChecks(MainWindow &window, Document &doc, const QString &scratch)
{
    std::printf("original\n");
    QAction *action = nullptr;
    for (QAction *a : window.findChildren<QAction *>())
        if (a->text() == QStringLiteral("&Original"))
            action = a;
    check(action && action->shortcut() == QKeySequence(Qt::Key_Backslash) && action->isVisible() && action->isEnabled(),
          QStringLiteral("View > Original, \\, in Stills"));
    if (!action)
        return;
    window.setMode(MainWindow::Mode::Motion);
    check(!action->isVisible(), QStringLiteral("not in Motion, where \\ is Log Footage's"));
    window.setMode(MainWindow::Mode::Stills);

    auto placed = [&] {
        return waitUntil([&] {
            return !doc.isRendering() && !doc.original().isNull() && doc.original().size() == doc.displayImage().size();
        }, 60000);
    };
    action->trigger();
    check(doc.showOriginal() && action->isChecked(), QStringLiteral("it turns Original on"));
    // each condition before its message: the order a call's arguments are
    // worked out in is the compiler's
    const bool arrived = placed();
    check(arrived, QStringLiteral("the original arrives, the print's size: %1 x %2")
                       .arg(doc.original().width()).arg(doc.original().height()));
    // not the print, and a picture: its pixels vary, and differ from the print's
    const QImage original = doc.original().convertToFormat(QImage::Format_RGB32);
    const QImage print = doc.displayImage().convertToFormat(QImage::Format_RGB32);
    double differ = 0, spread = 0, mean = 0;
    int n = 0;
    for (int y = 0; y < original.height(); y += 7)
        for (int x = 0; x < original.width(); x += 7) {
            const QRgb o = original.pixel(x, y), p = print.pixel(x, y);
            differ += std::abs(qGray(o) - qGray(p)) + std::abs(qRed(o) - qRed(p)) + std::abs(qBlue(o) - qBlue(p));
            mean += qGray(o);
            spread += double(qGray(o)) * qGray(o);
            ++n;
        }
    mean /= std::max(n, 1);
    const double deviation = std::sqrt(std::max(0.0, spread / std::max(n, 1) - mean * mean));
    check(differ / std::max(n, 1) > 3 && deviation > 10,
          QStringLiteral("it is not the print, and holds a picture (mean %1, deviation %2)").arg(mean, 0, 'f', 0)
              .arg(deviation, 0, 'f', 0));
    original.save(QDir(scratch).filePath(QStringLiteral("original.png")));
    window.grab().save(QDir(scratch).filePath(QStringLiteral("original-window.png")));

    // turned with the print
    const QSize before = doc.displayImage().size();
    doc.turn(true);
    const bool turned = waitUntil([&] { return doc.displayImage().size() == before.transposed(); }, 60000) && placed();
    check(turned, QStringLiteral("turned a quarter, it turns with the print: %1 x %2")
                      .arg(doc.original().width()).arg(doc.original().height()));
    doc.turn(false);
    const bool back = waitUntil([&] { return doc.displayImage().size() == before; }, 60000) && placed();
    check(back, QStringLiteral("and back"));

    action->trigger();
    check(!doc.showOriginal() && doc.original().isNull() && !action->isChecked(),
          QStringLiteral("again, the print, and the original let go"));
}

// The sidebar's pick and reject buttons, on the row under the pointer: a
// click flags its picture (or the selection it is in), again unflags it.
void flagButtonChecks(MainWindow &window, Document &doc, const QString &scratch)
{
    using Flag = ImageLibrary::Flag;
    std::printf("pick and reject buttons\n");
    FolderSidebar *sidebar = nullptr;
    for (FolderSidebar *s : window.findChildren<FolderSidebar *>())
        if (s->isVisibleTo(&window))
            sidebar = s;
    const auto shown = doc.browser().shown();
    int row = -1;
    for (size_t i = 0; i < shown.size(); ++i)
        if (!doc.browser().flag(shown[i].path)) {
            row = int(i);
            break;
        }
    if (!sidebar || row < 0) {
        check(false, QStringLiteral("a sidebar with a picture to flag"));
        return;
    }
    QWidget *viewport = sidebar->listViewport();
    QPoint last;
    // the pointer to a row's button, as Windows moves it there, then a click
    auto hover = [&](QPoint at) {
        const QPointF global = viewport->mapToGlobal(QPointF(at));
        QHoverEvent move(QEvent::HoverMove, QPointF(at), global, QPointF(last));
        QApplication::sendEvent(viewport, &move);
        QMouseEvent tracked(QEvent::MouseMove, QPointF(at), global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &tracked);
        last = at;
        QCoreApplication::processEvents();
    };
    auto click = [&](Flag kind) {
        const QPoint at = sidebar->flagButtonCentre(row, kind);
        hover(at);
        const QPointF global = viewport->mapToGlobal(QPointF(at));
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(at), global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at), global, Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(viewport, &release);
        QCoreApplication::processEvents();
    };
    const QString path = shown[size_t(row)].path;
    // not the selection's: that picture alone, the selection and the list's
    // place as they were
    if (doc.browser().selection().count(path))
        doc.browser().setSelection({doc.filePath()});
    const std::set<QString> selection = doc.browser().selection();
    auto *list = qobject_cast<QAbstractScrollArea *>(viewport->parentWidget());
    const int scrolled = list ? list->verticalScrollBar()->value() : 0;
    click(Flag::Pick);
    const bool picked = doc.browser().flag(path) == Flag::Pick;
    check(picked, QStringLiteral("the flag button picks the row's picture"));
    const bool kept = doc.browser().selection() == selection
        && (!list || list->verticalScrollBar()->value() == scrolled);
    check(kept, QStringLiteral("and leaves the selection and the list's place as they were"));
    // a quick second click, as Windows makes it a double click: still picked
    const QPoint at = sidebar->flagButtonCentre(row, Flag::Pick);
    const QPointF global = viewport->mapToGlobal(QPointF(at));
    QMouseEvent twice(QEvent::MouseButtonDblClick, QPointF(at), global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(viewport, &twice);
    QMouseEvent up(QEvent::MouseButtonRelease, QPointF(at), global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &up);
    const bool stillPicked = doc.browser().flag(path) == Flag::Pick;
    check(stillPicked, QStringLiteral("a double click on it is one click, not two"));
    click(Flag::Pick);
    const bool unpicked = !doc.browser().flag(path);
    check(unpicked, QStringLiteral("again, unflags it"));
    click(Flag::Reject);
    const bool rejected = doc.browser().flag(path) == Flag::Reject;
    check(rejected, QStringLiteral("the cross rejects it"));
    // the row as the pointer finds it, to look at
    hover(sidebar->flagButtonCentre(row, Flag::Pick));
    sidebar->grab().save(QDir(scratch).filePath(QStringLiteral("sidebar-buttons.png")));
    click(Flag::Reject);
    const bool unrejected = !doc.browser().flag(path);
    check(unrejected, QStringLiteral("again, unflags it"));
}

// The toolbar: the mode switch keeps its place when the half changes, the
// middle of the window, as the Mac's does, though Motion has fewer buttons.
void toolbarChecks(MainWindow &window)
{
    std::printf("the toolbar\n");
    QToolButton *stills = nullptr, *motion = nullptr;
    for (QToolButton *b : window.findChildren<QToolButton *>()) {
        if (b->toolTip().startsWith(QStringLiteral("Stills: ")))
            stills = b;
        else if (b->toolTip().startsWith(QStringLiteral("Motion: ")))
            motion = b;
    }
    if (!stills || !motion) {
        check(false, QStringLiteral("the mode switch in the toolbar"));
        return;
    }
    const MainWindow::Mode was = window.mode();
    auto middle = [&](MainWindow::Mode half) {
        window.setMode(half);
        waitUntil([] { return false; }, 200); // the layout settles
        return (stills->mapTo(&window, QPoint(0, 0)).x() + motion->mapTo(&window, QPoint(motion->width(), 0)).x())
               / 2.0;
    };
    const double inStills = middle(MainWindow::Mode::Stills);
    const double inMotion = middle(MainWindow::Mode::Motion);
    window.setMode(was);
    const double centre = window.width() / 2.0;
    const bool kept = std::abs(inStills - inMotion) < 0.5 && std::abs(inStills - centre) <= 1;
    check(kept, QStringLiteral("the mode switch stays in the middle of the window in both halves (at %1 and %2 px, "
                               "the middle at %3)")
                    .arg(inStills).arg(inMotion).arg(centre));
}

// The inspector's menus: the wheel scrolls the panel and changes none; a menu
// opens as the Mac's, the choice ticked; the status says the photograph.
void menuChecks(MainWindow &window, Document &doc, const QString &scratch)
{
    std::printf("menus and the status\n");
    MenuButton *menu = nullptr;
    for (Choice *c : window.findChildren<Choice *>())
        if (c->isVisibleTo(&window) && c->combo()->count() > 3 && !qobject_cast<StockPicker *>(c)) {
            menu = c->combo();
            break;
        }
    if (!menu) {
        check(false, QStringLiteral("a menu in the inspector"));
        return;
    }
    const int before = menu->currentIndex();
    const QPointF centre(menu->width() / 2.0, menu->height() / 2.0);
    QWheelEvent wheel(centre, menu->mapToGlobal(centre), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(menu, &wheel);
    const bool unchanged = menu->currentIndex() == before;
    check(unchanged, QStringLiteral("the wheel over a menu changes nothing"));
    // the menu, open, to look at
    int ticked = -1, choices = 0;
    QTimer::singleShot(400, &window, [&] {
        if (auto *open = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
            for (QAction *a : open->actions())
                if (a->data().isValid()) {
                    ++choices;
                    if (a->isChecked())
                        ticked = a->data().toInt();
                }
            open->grab().save(QDir(scratch).filePath(QStringLiteral("menu-open.png")));
            open->close();
        }
    });
    menu->showPopup();
    const bool opened = choices == menu->count() && ticked == before;
    check(opened, QStringLiteral("a menu opens with every choice, the one chosen ticked (%1 of %2)")
                      .arg(choices).arg(menu->count()));
    // the film's menu, taller than the screen: one column that scrolls, as
    // the Mac's, never columns side by side
    StockPicker *films = nullptr;
    for (StockPicker *p : window.findChildren<StockPicker *>())
        if (p->isVisibleTo(&window)) {
            films = p;
            break;
        }
    std::set<int> columns;
    int shown = 0;
    if (films) {
        QTimer::singleShot(400, &window, [&] {
            if (auto *open = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                for (QAction *a : open->actions())
                    if (a->data().isValid()) {
                        columns.insert(open->actionGeometry(a).left());
                        ++shown;
                    }
                open->grab().save(QDir(scratch).filePath(QStringLiteral("film-menu.png")));
                open->close();
            }
        });
        films->combo()->showPopup();
    }
    const bool oneColumn = shown > 0 && columns.size() == 1;
    check(oneColumn, QStringLiteral("the film menu is one column that scrolls (%1 films, %2 column(s))")
                         .arg(shown).arg(columns.size()));
    const QString status = doc.statusLine();
    const bool photographic = status.contains(QStringLiteral("ISO ")) && status.contains(QStringLiteral(" mm"))
        && status.contains(QStringLiteral(" s")) && !status.contains(QStringLiteral(" ms"));
    check(photographic, QStringLiteral("the status says the camera, focal length, shutter speed and ISO: %1")
                            .arg(status));
}

void settingsChecks(MainWindow &window, Document &doc, const QString &scratch)
{
    std::printf("settings\n");
    const theme::TextSize sizeBefore = theme::textSize();
    const QVariant namesBefore = appSettings().value("vocabulary");
    const int previewBefore = doc.previewSize();
    const bool sidecarsBefore = doc.library().savesSidecars();

    SettingsDialog *settings = window.showSettings();
    check(settings && settings->isVisible(), QStringLiteral("File > Settings… opens them"));
    if (!settings)
        return;
    settings->grab().save(QDir(scratch).filePath(QStringLiteral("settings.png")));

    // the panels as they are: one of each, at the text size's widths and type
    auto panels = [&](theme::TextSize size) {
        const double scale = size == theme::TextSize::Largest ? 1.3 : size == theme::TextSize::Larger ? 1.15 : 1.0;
        const int inspector = int(std::lround(306 * scale)), sidebar = int(std::lround(236 * scale));
        const int type = int(std::lround(12 * scale));
        const auto stills = window.findChildren<Inspector *>();
        const auto motion = window.findChildren<MotionInspector *>();
        const auto sidebars = window.findChildren<FolderSidebar *>();
        bool widths = stills.size() == 1 && motion.size() == 1 && sidebars.size() == 2;
        if (widths) {
            widths = stills[0]->maximumWidth() == inspector && motion[0]->maximumWidth() == inspector;
            for (FolderSidebar *s : sidebars)
                widths = widths && s->maximumWidth() == sidebar;
        }
        int labelType = 0;
        if (stills.size() == 1)
            for (QLabel *l : stills[0]->findChildren<QLabel *>())
                if (l->text() == controls::brightness.name(stills[0]->vocabulary()))
                    labelType = l->font().pixelSize();
        check(widths && labelType == type,
              QStringLiteral("%1: one of each panel, the inspectors %2 px wide, the sidebars %3, a slider's name in "
                             "%4 px type")
                  .arg(theme::textSizeName(size))
                  .arg(inspector)
                  .arg(sidebar)
                  .arg(labelType));
        // every film's and paper's name whole on its menu's button, in both
        // halves, each measured on screen
        std::pair<int, QString> worst{-1000, QString()};
        QStringList pickers;
        const MainWindow::Mode was = window.mode();
        for (MainWindow::Mode half : {MainWindow::Mode::Stills, MainWindow::Mode::Motion}) {
            window.setMode(half);
            QCoreApplication::processEvents();
            QWidget *panel = half == MainWindow::Mode::Stills ? static_cast<QWidget *>(stills.value(0))
                                                              : static_cast<QWidget *>(motion.value(0));
            if (panel)
                for (StockPicker *picker : panel->findChildren<StockPicker *>()) {
                    const auto over = picker->overflow();
                    pickers << QString::number(picker->combo()->textRoom());
                    if (over.first > worst.first)
                        worst = over;
                }
        }
        window.setMode(was);
        QCoreApplication::processEvents();
        const QString fit =
            QStringLiteral("%1: every film and paper name fits its menu (room %2 px; the longest, %3, %4 px %5)")
                .arg(theme::textSizeName(size), pickers.join(QStringLiteral("/")), worst.second)
                .arg(std::abs(worst.first))
                .arg(worst.first <= 0 ? QStringLiteral("to spare") : QStringLiteral("over"));
#if defined(Q_OS_WIN)
        check(!pickers.isEmpty() && worst.first <= 0, fit);
#else
        // the Mac build's type is wider than Segoe UI: a name too long ends in "…" there
        std::printf("   info %s\n", qPrintable(fit));
#endif
        return stills.size() == 1 ? stills[0] : nullptr;
    };

    std::printf("text size\n");
    settings->chooseTextSize(theme::TextSize::Standard);
    Inspector *inspector = panels(theme::TextSize::Standard);
    // a section opened that opens closed, to see it kept
    Section *output = nullptr;
    if (inspector)
        for (Section *section : inspector->findChildren<Section *>())
            if (section->title() == QStringLiteral("Output"))
                output = section;
    if (output)
        output->setExpanded(true);
    const std::set<QString> selection = doc.browser().selection();
    for (theme::TextSize size : {theme::TextSize::Larger, theme::TextSize::Largest}) {
        settings->chooseTextSize(size);
        inspector = panels(size);
        QCoreApplication::processEvents();
        window.grab().save(QDir(scratch).filePath(QStringLiteral("stills-%1.png").arg(theme::textSizeName(size))));
    }
    bool kept = false;
    if (inspector)
        for (Section *section : inspector->findChildren<Section *>())
            if (section->title() == QStringLiteral("Output"))
                kept = section->isExpanded();
    check(kept, QStringLiteral("a section left open stays open"));
    // the folder's sidebar made again shows the same pictures, the same selected
    QListView *list = nullptr;
    for (FolderSidebar *sidebar : window.findChildren<FolderSidebar *>())
        if (sidebar->isVisibleTo(&window) && window.mode() == MainWindow::Mode::Stills)
            if (auto *l = sidebar->findChild<QListView *>(); l && l->isVisibleTo(&window))
                list = l;
    check(list && list->model()->rowCount() == int(doc.browser().shown().size())
              && list->selectionModel()->selectedIndexes().size() == int(selection.size())
              && doc.browser().selection() == selection,
          QStringLiteral("the sidebar made again lists the folder's %1 pictures, %2 of them selected")
              .arg(list ? list->model()->rowCount() : -1)
              .arg(list ? list->selectionModel()->selectedIndexes().size() : -1));
    window.setMode(MainWindow::Mode::Motion);
    QCoreApplication::processEvents();
    window.grab().save(QDir(scratch).filePath(QStringLiteral("motion-Largest.png")));
    window.setMode(MainWindow::Mode::Stills);
    settings->chooseTextSize(theme::TextSize::Standard);
    inspector = panels(theme::TextSize::Standard);
    check(appSettings().value("textSize").toString() == QStringLiteral("Standard"),
          QStringLiteral("kept for the next launch"));

    std::printf("control names\n");
    auto named = [&](const QString &name) {
        if (!inspector)
            return false;
        for (QLabel *l : inspector->findChildren<QLabel *>())
            if (l->text() == name)
                return true;
        return false;
    };
    settings->chooseVocabulary(Vocabulary::Darkroom);
    check(named(controls::brightness.darkroomName) && !named(controls::brightness.photographicName)
              && appSettings().value("vocabulary").toString() == vocabularyName(Vocabulary::Darkroom),
          QStringLiteral("Darkroom renames the Stills sliders: %1").arg(controls::brightness.darkroomName));
    bool motionPhotographic = false;
    for (MotionInspector *m : window.findChildren<MotionInspector *>())
        for (QLabel *l : m->findChildren<QLabel *>())
            motionPhotographic = motionPhotographic || l->text() == controls::brightness.photographicName;
    check(motionPhotographic, QStringLiteral("Motion's keep their photographic names, as on the Mac"));
    settings->chooseVocabulary(Vocabulary::Photographic);
    check(named(controls::brightness.photographicName), QStringLiteral("and Photographic names them back"));

    std::printf("preview size\n");
    settings->choosePreviewSize(2048);
    check(doc.previewSize() == 2048 && appSettings().value("previewSize").toInt() == 2048,
          QStringLiteral("2048 px chosen in Settings is the preview's, and kept"));
    settings->choosePreviewSize(previewBefore);

    std::printf("save settings beside images\n");
    const QString picture = doc.filePath();
    QFile::remove(ImageLibrary::sidecarPath(picture));
    settings->chooseSidecars(true);
    doc.library().finishWriting();
    check(doc.library().savesSidecars() && QFileInfo::exists(ImageLibrary::sidecarPath(picture)),
          QStringLiteral("turned on in Settings, the picture on screen gets its .flx"));
    settings->chooseSidecars(sidecarsBefore);

    std::printf("the monitor\n");
    const QString file = QDir(scratch).filePath(QStringLiteral("wide-gamut-monitor.icc"));
    qputenv("FLX_DISPLAY_PROFILE", file.toLocal8Bit());
    displaycolour::setCurrent(displaycolour::read(nullptr));
    settings->refresh();
    check(settings->monitorText().contains(QStringLiteral("A wide-gamut monitor")),
          QStringLiteral("says what the previews do: %1").arg(settings->monitorText()));
    qunsetenv("FLX_DISPLAY_PROFILE");
    displaycolour::setCurrent({});
    settings->refresh();
    check(settings->monitorText().contains(QStringLiteral("sRGB")), QStringLiteral("and on an sRGB monitor: %1")
                                                                         .arg(settings->monitorText()));

    settings->chooseTextSize(sizeBefore);
    if (namesBefore.isValid())
        appSettings().setValue("vocabulary", namesBefore);
    else
        appSettings().remove("vocabulary");
    settings->close();
}

// Settings' Films tab (FilmSettings): every film ticked or not, in the Film
// menu's groups; a film left out leaves the Stills and Motion menus, but not
// while a picture uses it; a group hidden and every film shown again. The
// setting is put back as it was.
void filmSettingsChecks(MainWindow &window, Document &doc, MotionDocument &motion, const QString &scratch)
{
    std::printf("films in settings\n");
    const std::string before = hiddenFilms();
    const std::vector<PackIndexEntry> &all = doc.packs().filmEntries();
    SettingsDialog *settings = window.showSettings();
    if (!settings)
        return;
    setHiddenFilms({});
    settings->showPage(SettingsDialog::Page::Films);
    QCoreApplication::processEvents();
    settings->grab().save(QDir(scratch).filePath(QStringLiteral("settings-films.png")));
    const auto boxes = settings->findChildren<QCheckBox *>();
    int ticked = 0;
    for (QCheckBox *b : boxes)
        ticked += b->isChecked() ? 1 : 0;
    check(int(boxes.size()) == int(all.size()) && ticked == int(all.size())
              && settings->filmsShownText() == QStringLiteral("%1 of %1 films shown").arg(all.size()),
          QStringLiteral("the Films tab ticks every film: %1 boxes, %2").arg(boxes.size()).arg(settings->filmsShownText()));

    // the film menus: the Stills inspector's, and Motion's
    auto filmPicker = [](QWidget *inspector) -> StockPicker * {
        for (StockPicker *p : inspector->findChildren<StockPicker *>())
            for (const std::string &stock : p->listed())
                if (stock == "kodak_portra_400" || stock == "kodak_tri_x_400")
                    return p;
        return nullptr;
    };
    const auto stillsInspectors = window.findChildren<Inspector *>();
    const auto motionInspectors = window.findChildren<MotionInspector *>();
    StockPicker *stills = stillsInspectors.isEmpty() ? nullptr : filmPicker(stillsInspectors.first());
    StockPicker *clips = motionInspectors.isEmpty() ? nullptr : filmPicker(motionInspectors.first());
    if (!stills || !clips) {
        check(false, QStringLiteral("the Stills and Motion film menus are there"));
        return;
    }
    auto lists = [](StockPicker *p, const std::string &stock) {
        const std::vector<std::string> s = p->listed();
        return std::find(s.begin(), s.end(), stock) != s.end();
    };
    const std::string film = doc.recipe().filmStock;
    std::vector<std::string> hide;
    for (const std::string stock : {"kodak_vision3_500t", "agfa_apx_25", "fujifilm_neopan_1600"})
        if (stock != film && stock != motion.look().recipe.filmStock)
            hide.push_back(stock);
    for (const std::string &stock : hide)
        settings->chooseFilmShown(stock, false);
    QCoreApplication::processEvents();
    bool gone = true;
    for (const std::string &stock : hide)
        gone = gone && !lists(stills, stock) && !lists(clips, stock);
    check(gone && stills->listed().size() == all.size() - hide.size() && clips->listed().size() == all.size() - hide.size(),
          QStringLiteral("%1 films left out: Stills lists %2, Motion %3 of %4; %5")
              .arg(hide.size()).arg(stills->listed().size()).arg(clips->listed().size()).arg(all.size())
              .arg(settings->filmsShownText()));
    check(appSettings().value(filmmenu::kHiddenKey).toString().toStdString() == filmmenu::stored(std::set<std::string>(hide.begin(), hide.end())),
          QStringLiteral("kept as the Mac keeps them: %1").arg(appSettings().value(filmmenu::kHiddenKey).toString()));
    bool unticked = true;
    for (QCheckBox *b : boxes)
        if (b->text() == QStringLiteral("Agfa APX 25"))
            unticked = !b->isChecked();
    check(unticked, QStringLiteral("APX 25's box is unticked"));

    doc.selectFilm("agfa_apx_25");
    QCoreApplication::processEvents();
    const bool inUse = lists(stills, "agfa_apx_25") && stills->current().toString() == QStringLiteral("agfa_apx_25");
    doc.selectFilm(film);
    QCoreApplication::processEvents();
    check(inUse && !lists(stills, "agfa_apx_25"), QStringLiteral("a hidden film in use stays in the menu, and leaves it after"));

    settings->chooseGroupShown("Motion picture", false);
    QCoreApplication::processEvents();
    int motionFilms = 0;
    for (const StockGroup &g : stockGroups(all, true))
        if (g.title == "Motion picture")
            motionFilms = int(g.entries.size());
    bool noMotion = true;
    for (const StockGroup &g : stockGroups(all, true))
        if (g.title == "Motion picture")
            for (const PackIndexEntry &e : g.entries)
                noMotion = noMotion && (!lists(clips, e.stock) || e.stock == motion.look().recipe.filmStock);
    check(noMotion && motionFilms > 5, QStringLiteral("Hide All leaves a group's %1 films out").arg(motionFilms));
    settings->chooseGroupShown({}, true);
    QCoreApplication::processEvents();
    check(stills->listed().size() == all.size() && clips->listed().size() == all.size()
              && !appSettings().contains(filmmenu::kHiddenKey),
          QStringLiteral("Show All lists every film again and leaves no setting"));

    setHiddenFilms(before);
    settings->showPage(SettingsDialog::Page::General);
    settings->close();
    window.findChildren<Inspector *>().first()->listFilms();
    window.findChildren<MotionInspector *>().first()->listFilms();
}

} // namespace

int runSelfTest(MainWindow &window, Document &document, MotionDocument &motion, const QString &pictures,
                const QString &clips, const QString &scratch)
{
    g_failures = 0;
    pictureChecks(window, document, pictures, scratch);
    if (!clips.isEmpty())
        clipChecks(window, motion, clips, scratch);
    window.setMode(MainWindow::Mode::Stills);
    displayChecks(document, motion, scratch);
    originalChecks(window, document, scratch);
    flagButtonChecks(window, document, scratch);
    toolbarChecks(window);
    menuChecks(window, document, scratch);
    settingsChecks(window, document, scratch);
    filmSettingsChecks(window, document, motion, scratch);
    std::printf(g_failures == 0 ? "\nall app checks pass\n" : "\n%d app check(s) failed\n", g_failures);
    return g_failures;
}
