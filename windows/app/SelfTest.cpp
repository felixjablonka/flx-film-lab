#include "SelfTest.h"

#include "AppSettings.h"
#include "DisplayColour.h"
#include "Document.h"
#include "FolderSidebar.h"
#include "HEVCLevels.h"
#include "Inspector.h"
#include "MainWindow.h"
#include "MotionDocument.h"
#include "MotionInspector.h"
#include "RawUnpack.h"
#include "Resampler.h"
#include "SettingsDialog.h"
#include "Theme.h"
#include "Thumbnails.h"
#include "Vocabulary.h"
#include "Widgets.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QColorTransform>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHoverEvent>
#include <QImageReader>
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
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    // Decoding the pictures either side waits for a moving slider to pause,
    // but not for the picture's own first frame to be half a second old:
    // stepping on a second later found the next one not decoded yet (2.1.0).
    check(!doc.holdsDecodesAhead(), QStringLiteral("its first frame does not hold back decoding its neighbours"));

    std::printf("a look of its own, kept\n");
    Recipe changed = doc.recipe();
    changed.autoExposure = false;
    changed.exposureEV += 0.7;
    doc.setRecipe(changed);
    check(waitUntil([&] { return !doc.isRendering(); }, 30000) && doc.holdsDecodesAhead(),
          QStringLiteral("an edit's frame does, as a moving slider's do"));
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
    // the comment, as EXIF writes a user comment: its character code, then the text
    auto bytesOf = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const QByteArray commented = QByteArray("ASCII\0\0\0", 8) + "Edited with FLX Film Lab. Film profile: ";
    check(!made.isEmpty() && std::all_of(made.begin(), made.end(), [&](const QString &name) {
              return bytesOf(QDir(out).filePath(name)).contains(commented);
          }), QStringLiteral("each says in its comment it was edited here, and with which film"));

    std::printf("export at a long edge\n");
    // the size beside the format, chosen in the toolbar's menu and kept as
    // the Mac keeps it
    setExportSize(ExportSize{});
    window.chooseSize(1);
    const ExportSize chosen = exportSize();
    QComboBox *sizeMenu = nullptr;
    for (QComboBox *box : window.findChildren<QComboBox *>())
        if (box->toolTip().startsWith(QStringLiteral("How large Export writes")))
            sizeMenu = box;
    check(!chosen.fullSize && chosen.longEdge == ExportSize::kStandardLongEdge && chosen.limit() == 2048 && sizeMenu
              && sizeMenu->currentIndex() == 1 && sizeMenu->itemText(1) == QStringLiteral("2048 px Long Edge"),
          QStringLiteral("the toolbar's size: 2048 px on the long edge (%1)").arg(sizeMenu ? sizeMenu->currentText() : "no menu"));
    // the full-size prints of the batch above, their sizes to shrink
    auto sizeOf = [](const QString &path) { return QImageReader(path).size(); };
    auto shrunkTo = [](QSize full, int longEdge) {
        const auto [w, h] = resampler::exportSize(full.width(), full.height(), longEdge);
        return QSize(w, h);
    };
    // the picture on screen, the last of the batch: the checks after these
    // find the screen as they did before them
    const QString fullPrint = QDir(out).filePath(made.value(1));
    const QString single = QDir(scratch).filePath(QStringLiteral("single.jpg"));
    doc.exportTo(single, ExportFormat::Jpeg, 300);
    const bool exported = waitUntil([&] { return !doc.isExporting(); }, 300000);
    const QSize singleWant = shrunkTo(sizeOf(fullPrint), 300);
    check(exported && sizeOf(single) == singleWant && std::max(singleWant.width(), singleWant.height()) == 300,
          QStringLiteral("Export at a long edge of 300 px writes %1x%2 (the full print %3x%4)")
              .arg(sizeOf(single).width()).arg(sizeOf(single).height())
              .arg(sizeOf(fullPrint).width()).arg(sizeOf(fullPrint).height()));
    check(doc.status() == QStringLiteral("Exported single.jpg, %1 × %2").arg(singleWant.width()).arg(singleWant.height()),
          QStringLiteral("and the status line says how large: %1").arg(doc.status()));
    const std::string said = provenance(doc.recipe(), doc.packs(), "").description;
    check(bytesOf(single).contains(QByteArray("ASCII\0\0\0", 8) + QByteArray::fromStdString(said))
              && QString::fromStdString(said).startsWith(QStringLiteral("Edited with FLX Film Lab. Film profile: ")),
          QStringLiteral("its comment names the film and the paper: %1").arg(QString::fromStdString(said)));
    setExportComment({false, true, true, true});
    const QString uncredited = QDir(scratch).filePath(QStringLiteral("uncredited.jpg"));
    doc.exportTo(uncredited, ExportFormat::Jpeg, 300);
    const bool uncreditedDone = waitUntil([&] { return !doc.isExporting(); }, 300000);
    check(uncreditedDone && bytesOf(uncredited).contains(QByteArray("ASCII\0\0\0", 8) + "Film profile: ")
              && !bytesOf(uncredited).contains("Edited with FLX Film Lab"),
          QStringLiteral("the credit switched off, the comment starts at the film"));
    setExportComment(ExportComment::none());
    const QString silent = QDir(scratch).filePath(QStringLiteral("silent.jpg"));
    doc.exportTo(silent, ExportFormat::Jpeg, 300);
    const bool silentDone = waitUntil([&] { return !doc.isExporting(); }, 300000);
    setExportComment({});
    check(silentDone && !bytesOf(silent).contains("Edited with FLX Film Lab") && bytesOf(silent).contains("FLX Film Lab"),
          QStringLiteral("with nothing asked for in Settings, no comment: FLX Film Lab only as the software"));
    const QString outSmall = QDir(scratch).filePath(QStringLiteral("Prints small"));
    QDir().mkpath(outSmall);
    doc.browser().setSelection({items[1].path, items[2].path, items[3].path});
    doc.exportSelected(outSmall, ExportFormat::Jpeg, 160);
    check(waitUntil([&] { return !doc.browser().batch(); }, 300000), QStringLiteral("a batch at a long edge finishes"));
    QStringList sizes, wanted;
    for (const QString &name : made) {
        const QSize got = sizeOf(QDir(outSmall).filePath(name)), want = shrunkTo(sizeOf(QDir(out).filePath(name)), 160);
        sizes << QStringLiteral("%1x%2").arg(got.width()).arg(got.height());
        wanted << QStringLiteral("%1x%2").arg(want.width()).arg(want.height());
    }
    check(!made.isEmpty() && sizes == wanted && QDir(outSmall).entryList(QDir::Files) == made,
          QStringLiteral("each at the long edge, 160 px, in its own shape: %1").arg(sizes.join(", ")));
    window.chooseSize(0);
    check(exportSize().fullSize && exportSize().longEdge == 2048 && sizeMenu && sizeMenu->currentIndex() == 0,
          QStringLiteral("back to Full Size, the long edge kept to come back to"));

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

    std::printf("grain on clips\n");
    const QImage plain = motion.image().convertToFormat(QImage::Format_RGBA8888);
    MotionLook grained = motion.look();
    MotionGrain grain;
    grain.enabled = true;
    grain.gauge = FilmGauge::Sixteen;
    grain.amount = 2;
    grained.grain = grain;
    motion.setLook(grained);
    const auto shownChanged = [&](const QImage &from) {
        return waitUntil([&] {
            return !motion.isRendering() && motion.image().convertToFormat(QImage::Format_RGBA8888) != from;
        }, 60000);
    };
    check(shownChanged(plain), QStringLiteral("Grain redraws the frame"));
    // the frame's own grains: every pixel moved, the picture's light kept
    const QImage withGrain = motion.image().convertToFormat(QImage::Format_RGBA8888);
    double moved = 0, shift = 0;
    qint64 samples = 0;
    if (withGrain.size() == plain.size())
        for (int y = 0; y < plain.height(); y += 2) {
            const uchar *a = plain.constScanLine(y), *b = withGrain.constScanLine(y);
            for (int x = 0; x < plain.width(); x += 2)
                for (int c = 0; c < 3; ++c) {
                    const int d = int(b[x * 4 + c]) - int(a[x * 4 + c]);
                    moved += std::abs(d);
                    shift += d;
                    ++samples;
                }
        }
    const double n = double(std::max<qint64>(samples, 1));
    check(samples > 0 && moved / n > 1.0 && std::abs(shift / n) < 0.6,
          QStringLiteral("16 mm grain on the frame: %1 levels a pixel on average, the light kept (%2)")
              .arg(moved / n, 0, 'f', 2).arg(shift / n, 0, 'f', 2));
    check(waitUntil([&] { return motion.history().undoName() == std::optional<std::string>("Grain"); }, 5000),
          QStringLiteral("an undo step of its own: %1")
              .arg(QString::fromStdString(motion.history().undoName().value_or("nothing to undo"))));
    grained.grain->enabled = false;
    motion.setLook(grained);
    check(waitUntil([&] {
              return !motion.isRendering() && motion.image().convertToFormat(QImage::Format_RGBA8888) == plain;
          }, 60000),
          QStringLiteral("and off again, the frame as it was"));
    grained.grain->enabled = true;
    motion.setLook(grained);
    check(shownChanged(plain), QStringLiteral("on again, for the export"));

    std::printf("compression\n");
    MotionLook compressed = motion.look();
    compressed.exportSettings.codec = ExportCodec::HEVC10;
    compressed.exportSettings.hevcLevel = HEVCLevel::High;
    motion.setLook(compressed);
    QApplication::processEvents();
    QComboBox *compression = nullptr;
    for (QComboBox *box : window.findChildren<QComboBox *>())
        if (box->count() > 0 && box->itemText(0).startsWith(QStringLiteral("Smaller")))
            compression = box;
    const ClipInfo &shown = *motion.clip();
    QStringList levels;
    for (HEVCLevel l : {HEVCLevel::Smaller, HEVCLevel::Standard, HEVCLevel::High, HEVCLevel::Maximum})
        levels << QString::fromStdString(hevcLevelName(l)) + QStringLiteral(" · ")
                      + QString::fromStdString(ExportSettings::rate(
                          compressed.exportSettings.megabits(l, shown.width, shown.height, shown.frameRate)));
    QStringList menu;
    if (compression)
        for (int i = 0; i < compression->count(); ++i)
            menu << compression->itemText(i);
    check(compression && menu.mid(0, 4) == levels && compression->currentText() == levels[2]
              && menu.last() == QStringLiteral("Set Data Rate…"),
          QStringLiteral("the toolbar's HEVC levels, each with its rate for the clip: %1").arg(menu.join(", ")));

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
    if (made.size() == 1) {
        // as HEVC at High, labelled the lowest level and tier that hold it
        const std::string file = QDir(out).filePath(made[0]).toStdString();
        const ClipInfo source = readClipInfo(smallest.path.toStdString());
        const ClipInfo written = readClipInfo(file);
        const double asked = compressed.exportSettings.megabits(source.width, source.height, source.frameRate);
        const hevclevels::Label wanted = hevclevels::label(source.width, source.height, source.frameRate, asked);
        const auto label = hevclevels::labelOf(file);
        check(written.codecName == "HEVC" && label == wanted,
              QStringLiteral("as HEVC at High, %1 Mbit/s asked, %2 written, labelled %3")
                  .arg(asked, 0, 'f', 0).arg(written.dataRate / 1e6, 0, 'f', 1)
                  .arg(QString::fromStdString(label ? label->description() : "nothing")));
        QFile movie(QDir(out).filePath(made[0]));
        const QByteArray bytes = movie.open(QIODevice::ReadOnly) ? movie.readAll() : QByteArray();
        check(bytes.contains("com.apple.quicktime.description") && bytes.contains("Edited with FLX Film Lab. Film profile: "),
              QStringLiteral("its description says it was edited here, and with which film"));
    }
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

    std::printf("the comment in exported files\n");
    check(exportComment() == ExportComment{} && settings->commentExampleText()
              == QStringLiteral("For example: Edited with FLX Film Lab. Film profile: Kodak Tri-X 400; camera filter: Red, "
                                "Wratten 25; paper: Ilford Multigrade RC Deluxe"),
          QStringLiteral("the film, the filter and the paper, as the example says: %1").arg(settings->commentExampleText()));
    settings->chooseExportComment({true, true, false, true});
    check(exportComment() == ExportComment{true, true, false, true} && !appSettings().value("exportCommentPaper").toBool()
              && settings->commentExampleText().endsWith(QStringLiteral("camera filter: Red, Wratten 25")),
          QStringLiteral("the paper switched off, kept as the Mac keeps it: %1").arg(settings->commentExampleText()));
    settings->chooseExportComment({false, true, true, true});
    check(!appSettings().value("exportCommentEditedWith").toBool()
              && settings->commentExampleText()
                     == QStringLiteral("For example: Film profile: Kodak Tri-X 400; camera filter: Red, Wratten 25; paper: "
                                       "Ilford Multigrade RC Deluxe"),
          QStringLiteral("without the credit: %1").arg(settings->commentExampleText()));
    settings->chooseExportComment(ExportComment::none());
    check(settings->commentExampleText() == QStringLiteral("Nothing is written there."),
          QStringLiteral("all off: %1").arg(settings->commentExampleText()));
    settings->chooseExportComment({});

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

// Settings' Film & Paper tab (the Mac's 6a6e290): the default film and paper,
// what a picture with no settings of its own opens with, and how the paper
// follows a change of film, through the Stills and Motion documents as the
// pickers change film and paper, the paper last used with each kind shared
// between them.
void filmAndPaperChecks(MainWindow &window, Document &doc, MotionDocument &motion, const QString &pictures,
                        const QString &scratch)
{
    std::printf("film and paper in settings\n");
    SettingsDialog *settings = window.showSettings();
    if (!settings)
        return;
    const char *keys[] = {"newPictures", "defaultFilm", "defaultPaper", kPaperRuleKey, kPapersByKindKey};
    // from no setting at all, as a first launch has it
    for (const char *key : keys)
        appSettings().remove(key);
    settings->showPage(SettingsDialog::Page::FilmAndPaper);
    QCoreApplication::processEvents();
    settings->grab().save(QDir(scratch).filePath(QStringLiteral("settings-film-paper.png")));
    check(newPictures() == NewPictures::CarryLook && paperRule() == PaperRule::StaysWithKind
              && settings->shownDefaultFilm() == "kodak_portra_400"
              && settings->shownDefaultPaper() == "kodak_portra_endura" && settings->defaultPaperShown(),
          QStringLiteral("with nothing set: the look of the picture before, Portra 400 on its paper, the paper "
                         "staying with its kind"));
    settings->chooseDefaultFilm("kodak_tri_x_400");
    check(defaultFilmSetting() == "kodak_tri_x_400" && defaultPaperSetting() == "ilford_multigrade_rc_deluxe"
              && settings->shownDefaultPaper() == "ilford_multigrade_rc_deluxe",
          QStringLiteral("a default film brings its own paper: %1").arg(QString::fromStdString(defaultPaperSetting())));
    settings->chooseDefaultFilm("fujifilm_provia_100f");
    check(!settings->defaultPaperShown(), QStringLiteral("a default slide has no paper to choose"));
    settings->chooseDefaultFilm("kodak_tri_x_400");
    settings->chooseDefaultPaper("kodak_portra_endura");
    check(defaultPaperSetting() == "kodak_portra_endura" && settings->defaultPaperShown(),
          QStringLiteral("and the paper can then be another"));
    settings->chooseDefaultPaper("ilford_multigrade_rc_deluxe");

    std::printf("the paper as the film changes, in Stills and Motion\n");
    settings->choosePaperRule(PaperRule::FollowsFilm);
    doc.selectFilm("kodak_portra_400");
    doc.selectPaper("kodak_portra_endura");
    doc.selectFilm("kodak_gold_200");
    check(paperRule() == PaperRule::FollowsFilm && doc.recipe().printStock == "kodak_ektacolor_edge",
          QStringLiteral("following the film, Gold 200 brings its own paper: %1")
              .arg(QString::fromStdString(doc.recipe().printStock)));
    settings->choosePaperRule(PaperRule::StaysWithKind);
    doc.selectPaper("kodak_portra_endura");
    doc.selectFilm("kodak_portra_400");
    check(doc.recipe().printStock == "kodak_portra_endura",
          QStringLiteral("staying with the kind, Portra 400 keeps the paper Gold 200 was on"));
    motion.selectFilm("kodak_vision3_250d");
    motion.selectPaper("kodak_2393");
    motion.selectFilm("kodak_vision3_500t");
    check(motion.look().recipe.printStock == "kodak_2393",
          QStringLiteral("in Motion, another motion picture film keeps the print film chosen: %1")
              .arg(QString::fromStdString(motion.look().recipe.printStock)));
    doc.selectFilm("kodak_exr_200t");
    check(doc.recipe().printStock == "kodak_2393",
          QStringLiteral("and Stills brings back the print film Motion last used: %1")
              .arg(QString::fromStdString(doc.recipe().printStock)));

    std::printf("new pictures: the look before, or the default film and paper\n");
    // two pictures with no settings of their own: copies under new names
    const QString dir = QDir(scratch).filePath(QStringLiteral("New pictures"));
    QDir().mkpath(dir);
    QStringList fresh;
    for (const FolderBrowser::Item &item : FolderBrowser::listing(pictures, FolderBrowser::Kind::Pictures)) {
        if (fresh.size() == 2)
            break;
        if (QFileInfo(item.path).suffix().compare(QStringLiteral("nef"), Qt::CaseInsensitive) != 0)
            continue;
        const QString to = QDir(dir).filePath(QStringLiteral("New %1.NEF").arg(fresh.size() + 1));
        QFile::copy(item.path, to);
        fresh << FolderBrowser::canonical(to);
    }
    auto opened = [&](const QString &path) {
        doc.open(path);
        return waitUntil([&] {
            return doc.filePath() == path && doc.hasImage() && !doc.isDecoding() && !doc.isRendering()
                   && !doc.displayImage().isNull();
        }, 60000);
    };
    if (fresh.size() < 2 || doc.filePath().isEmpty()) {
        check(false, QStringLiteral("a picture on screen and two RAW files to open afresh from %1").arg(pictures));
    } else {
        const std::string before = doc.recipe().filmStock;
        settings->chooseNewPictures(NewPictures::CarryLook);
        check(opened(fresh[0]) && doc.recipe().filmStock == before,
              QStringLiteral("the look of the picture before: %1").arg(QString::fromStdString(doc.recipe().filmStock)));
        settings->chooseNewPictures(NewPictures::DefaultStocks);
        check(opened(fresh[1]) && doc.recipe().filmStock == "kodak_tri_x_400"
                  && doc.recipe().printStock == "ilford_multigrade_rc_deluxe",
              QStringLiteral("set to the defaults, the default film and paper: %1 on %2")
                  .arg(QString::fromStdString(doc.recipe().filmStock), QString::fromStdString(doc.recipe().printStock)));
    }

    for (const char *key : keys)
        appSettings().remove(key);
    settings->showPage(SettingsDialog::Page::General);
    settings->close();
}

} // namespace

// A folder lists its RAW files, which the film is made from, and not the
// JPEGs and TIFFs beside them (the Mac's rawOnlyListing): a user's folder of
// NEFs with Photoshop TIFFs beside them stuck at "decoding…" (2.0.0). The
// listing goes by file type, so empty files stand in for the pictures.
void listingChecks(const QString &scratch)
{
    std::printf("a folder lists its RAW files only\n");
    const QString dir = QDir(scratch).filePath(QStringLiteral("mixed"));
    QDir().mkpath(dir);
    for (const char *name : {"Z9Z_0486.NEF", "Z9Z_0486-Edit.tif", "R0000320.DNG", "export.jpg", "DSC00012.ARW",
                             "scan.png", "IMG_0001.CR3", "DSCF1234.RAF", "notes.txt"}) {
        QFile file(QDir(dir).filePath(QString::fromLatin1(name)));
        if (!file.open(QIODevice::WriteOnly))
            check(false, QStringLiteral("could not write %1").arg(file.fileName()));
    }
    QStringList listed;
    for (const FolderBrowser::Item &item : FolderBrowser::listing(dir, FolderBrowser::Kind::Pictures))
        listed << item.name();
    check(listed == QStringList({"DSC00012.ARW", "DSCF1234.RAF", "IMG_0001.CR3", "R0000320.DNG", "Z9Z_0486.NEF"}),
          QStringLiteral("the NEF, DNG, ARW, CR3 and RAF, not the TIFF, JPEG or PNG: %1").arg(listed.join(", ")));
}

// Sorting a folder (FolderBrowser::Order, tests/app's folderOrder): by file
// name, or by the date shot, as LibRaw reads it, ties by name; one order for
// both halves and every menu that shows it, kept. In a folder whose name has
// a letter outside ASCII, which Windows' narrow file calls misread. The
// clips' dates are the creation times AVFoundation reads on the Mac.
void orderChecks(MainWindow &window, Document &doc, MotionDocument &motion, const QString &pictures,
                 const QString &clips, const QString &scratch)
{
    using Kind = FolderBrowser::Kind;
    using Order = FolderBrowser::Order;
    std::printf("sort a folder by the date shot\n");
    std::vector<QString> originals;
    for (const FolderBrowser::Item &item : FolderBrowser::listing(pictures, Kind::Pictures))
        if (originals.size() < 4 && QFileInfo(item.path).suffix().compare(QStringLiteral("nef"), Qt::CaseInsensitive) == 0)
            originals.push_back(item.path);
    if (originals.size() < 4) {
        check(false, QStringLiteral("four NEFs to sort in %1").arg(pictures));
        return;
    }
    const std::map<QString, qint64> dates = FolderBrowser::shotDates(originals, Kind::Pictures);
    // the test pictures' DateTimeOriginal, as macOS's ImageIO reads it: the
    // camera's clock, here local time as LibRaw reads it
    const std::map<QString, QDateTime> known{
        {QStringLiteral("DSC_0036.NEF"), QDateTime(QDate(2025, 11, 25), QTime(11, 45, 6))},
        {QStringLiteral("DSC_0086.NEF"), QDateTime(QDate(2026, 4, 23), QTime(22, 11, 3))},
        {QStringLiteral("DSC_0280.NEF"), QDateTime(QDate(2026, 5, 3), QTime(3, 32, 53))},
        {QStringLiteral("DSC_0336.NEF"), QDateTime(QDate(2026, 5, 3), QTime(5, 26, 4))}};
    QStringList read, wrong;
    for (const QString &path : originals) {
        const QString name = QFileInfo(path).fileName();
        const QDateTime when = QDateTime::fromSecsSinceEpoch(dates.at(path));
        read << QStringLiteral("%1 %2").arg(name, when.toString(Qt::ISODate));
        if (known.count(name) && known.at(name) != when)
            wrong << name;
    }
    check(wrong.isEmpty(), QStringLiteral("each picture's date and time as the camera wrote them: %1").arg(read.join(", ")));

    // named against the order they were shot in, the last shot first
    std::vector<QString> byDate = originals;
    std::stable_sort(byDate.begin(), byDate.end(), [&](const QString &a, const QString &b) { return dates.at(a) < dates.at(b); });
    const QString folder = QDir(scratch).filePath(QStringLiteral("Ausfl\u00FCge \u00E9t\u00E9"));
    QDir().mkpath(folder);
    const QStringList letters{"A", "B", "C", "D"};
    std::map<QString, QString> copyOf;              // the copy's name: the picture it is
    for (int i = 0; i < 4; ++i) {
        // copied in the order of their names, so their files' own dates go by name
        const QString name = letters[i] + QStringLiteral(".NEF");
        QFile::copy(byDate[size_t(3 - i)], QDir(folder).filePath(name));
        copyOf[name] = byDate[size_t(3 - i)];
    }
    std::vector<QString> copies;
    for (const QString &name : letters)
        copies.push_back(FolderBrowser::canonical(QDir(folder).filePath(name + QStringLiteral(".NEF"))));
    const std::map<QString, qint64> copied = FolderBrowser::shotDates(copies, Kind::Pictures);
    bool same = true;
    for (const QString &path : copies)
        same = same && copied.at(path) == dates.at(copyOf.at(QFileInfo(path).fileName()));
    check(same, QStringLiteral("read the same in a folder named %1").arg(QFileInfo(folder).fileName()));
    RawMosaic there, here;
    readMetadataWithLibRaw(copies[0].toStdString(), there);
    readMetadataWithLibRaw(copyOf.at(QStringLiteral("A.NEF")).toStdString(), here);
    check(here.exposure.iso > 0 && there.exposure.iso == here.exposure.iso && there.orientation == here.orientation
              && there.lensModel == here.lensModel,
          QStringLiteral("and the rest of what the camera wrote, its ISO %1 there, %2 here")
              .arg(there.exposure.iso).arg(here.exposure.iso));

    const bool stored = foldersByDateShot();
    doc.browser().setOrder(Order::Name);
    window.openFile(folder);
    check(names(doc.browser().items()) == QStringList({"A.NEF", "B.NEF", "C.NEF", "D.NEF"}),
          QStringLiteral("by file name: %1").arg(names(doc.browser().items()).join(", ")));
    // every menu that shows the order: both sidebars' and the View menu's
    auto ticked = [&](const QString &text) {
        int on = 0, all = 0;
        for (QAction *action : window.findChildren<QAction *>())
            if (action->text() == text) {
                ++all;
                on += action->isChecked();
            }
        return all >= 3 && on == all;
    };
    check(ticked(QStringLiteral("&File Name")), QStringLiteral("File Name ticked in the sidebars' menus and the View menu"));
    doc.browser().setOrder(Order::DateShot);
    check(names(doc.browser().items()) == QStringList({"D.NEF", "C.NEF", "B.NEF", "A.NEF"}),
          QStringLiteral("by the date shot, the first shot first: %1").arg(names(doc.browser().items()).join(", ")));
    check(motion.browser().order() == Order::DateShot && foldersByDateShot() && ticked(QStringLiteral("&Date Shot")),
          QStringLiteral("for the clips too, kept, and ticked in every menu"));
    const QString d = copies[3], c = copies[2];
    check(doc.browser().neighbour(d, 1) == std::optional<QString>(c),
          QStringLiteral("Next in Folder goes by the date"));
    doc.browser().setOrder(Order::Name);
    check(names(doc.browser().items()) == QStringList({"A.NEF", "B.NEF", "C.NEF", "D.NEF"}) && !foldersByDateShot()
              && motion.browser().order() == Order::Name,
          QStringLiteral("and back by file name"));
    doc.browser().setOrder(stored ? Order::DateShot : Order::Name);
    waitUntil([&] { return !doc.isDecoding() && !doc.isRendering(); }, 60000);

    std::printf("a clip's date\n");
    const std::map<QString, QDateTime> clipDates{
        {QStringLiteral("A001_10221746_C282.mov"), QDateTime(QDate(2026, 10, 22), QTime(15, 46, 20), QTimeZone::UTC)},
        {QStringLiteral("A001_10221747_C283.mov"), QDateTime(QDate(2026, 10, 22), QTime(15, 47, 4), QTimeZone::UTC)}};
    if (!clips.isEmpty()) {
        QStringList found;
        int matched = 0;
        for (const FolderBrowser::Item &item : FolderBrowser::listing(clips, Kind::Clips)) {
            const std::optional<qint64> made = FolderBrowser::movieDate(item.path);
            found << QStringLiteral("%1 %2").arg(item.name(), made ? QDateTime::fromSecsSinceEpoch(*made, QTimeZone::UTC)
                                                                          .toString(Qt::ISODate)
                                                                    : QStringLiteral("none"));
            if (clipDates.count(item.name()) && made && *made == clipDates.at(item.name()).toSecsSinceEpoch())
                ++matched;
        }
        check(matched == 2, QStringLiteral("from its movie header, as AVFoundation reads it: %1").arg(found.join(", ")));
        // the smallest clip in a folder named as the pictures' was: its date,
        // and its log tag, which its own boxes carry
        const std::vector<FolderBrowser::Item> all = FolderBrowser::listing(clips, Kind::Clips);
        if (!all.empty()) {
            const FolderBrowser::Item smallest = *std::min_element(
                all.begin(), all.end(), [](const auto &a, const auto &b) { return a.size < b.size; });
            const QString reels = QDir(scratch).filePath(QStringLiteral("Clips \u00E9t\u00E9"));
            QDir().mkpath(reels);
            const QString there = QDir(reels).filePath(smallest.name());
            QFile::copy(smallest.path, there);
            const ClipInfo here = readClipInfo(smallest.path.toStdString()), away = readClipInfo(there.toStdString());
            check(!here.camera.logTransfer.empty() && away.camera.logTransfer == here.camera.logTransfer
                      && away.camera.make == here.camera.make && FolderBrowser::movieDate(there) == FolderBrowser::movieDate(smallest.path),
                  QStringLiteral("and in a folder named %1, its date and its log tag, %2")
                      .arg(QFileInfo(reels).fileName(), QString::fromStdString(away.camera.logTransfer)));
        }
    }
    const QString note = QDir(folder).filePath(QStringLiteral("notes.mov"));
    QFile file(note);
    if (file.open(QIODevice::WriteOnly))
        file.write("not a movie");
    file.close();
    const std::map<QString, qint64> noted = FolderBrowser::shotDates({note}, Kind::Clips);
    check(!FolderBrowser::movieDate(note) && std::abs(noted.at(note) - QDateTime::currentSecsSinceEpoch()) < 600,
          QStringLiteral("and a file with none goes by when it was made"));
}

int runSelfTest(MainWindow &window, Document &document, MotionDocument &motion, const QString &pictures,
                const QString &clips, const QString &scratch)
{
    g_failures = 0;
    listingChecks(scratch);
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
    filmAndPaperChecks(window, document, motion, pictures, scratch);
    orderChecks(window, document, motion, pictures, clips, scratch);
    std::printf(g_failures == 0 ? "\nall app checks pass\n" : "\n%d app check(s) failed\n", g_failures);
    return g_failures;
}
