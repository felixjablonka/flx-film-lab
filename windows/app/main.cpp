// FLX Film Lab for Windows: a RAW photo through film and paper.
//
//   FLXFilmLab [file]
//
// For testing, without anyone at the screen:
//   --screenshot PNG    once the file has rendered and nothing is pending, the window to PNG, then quit
//   --export PATH       once it has rendered, export it (the format from PATH's extension), then quit
//   --size WxH          the window's size
//   --recipe JSON       a recipe in the Mac app's form, put on the file once it has opened
//   --preview N         the preview's long side
//   --save-preview PNG  with --screenshot: the preview itself too, at its own size
//   --bench N           with --screenshot: first N renders, each with the exposure moved, timed
//   --cropping          with --screenshot: the crop being drawn
//   --select ID         with --screenshot: a mask or gradient selected, its handles showing
//   --loupe X,Y,W,H     the loupe open at X,Y (fractions of the print), W x H pixels of the print
//   --save-loupe PNG    with --loupe: the loupe's picture, once printed
//   --white-balance WB  once it has rendered, this white balance (decoding again): Daylight, As shot, ...
//   --library DIR       the library and the sidebar's cached prints there, not the app's own
//   --display-colour F  every monitor's colour mode and profile, and what the previews do with them, to F
//   --self-test RAWS    with --library: the folder workflow driven in the app, four of RAWS' files
//                       copied into DIR (SelfTest); --self-test-clips CLIPS for Motion's too
//
// The file may be a folder: it opens in the sidebar, in the half that has its
// kind of file, and --screenshot waits for the sidebar's first pictures too.
//
// With a movie, which opens in Motion:
//   --at SECONDS        the frame showing then
//   --play SECONDS      play that long first, and say how many frames were shown
//   --codec NAME        prores or hevc, for --export
//   --export-lut PATH   once it has rendered, the look as a .cube

#include "AppSettings.h"
#include "DisplayColour.h"
#include "Document.h"
#include "ImageLibrary.h"
#include "Icons.h"
#include "MainWindow.h"
#include "MotionDocument.h"
#include "SelfTest.h"
#include "Theme.h"
#include "Thumbnails.h"
#include "version.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

// The app's data, next to the program: the film packs, Apple's colour per
// camera, Lensfun's database and RawSpeed's camera list.
AppData appData()
{
    const QDir dir(QCoreApplication::applicationDirPath());
    return {dir.filePath(QStringLiteral("packs")), dir.filePath(QStringLiteral("apple-colour.json")),
            dir.filePath(QStringLiteral("lensfun")), dir.filePath(QStringLiteral("cameras.xml"))};
}

// One app at a time, as on the Mac: a file opened while it runs (Explorer's
// Open with) goes to the window already open.
QString instanceName()
{
    return QStringLiteral("FLXFilmLab-") + qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER"));
}

// Hands `file` to the app already running, if there is one.
bool handOver(const QString &file)
{
    QLocalSocket socket;
    socket.connectToServer(instanceName());
    if (!socket.waitForConnected(500))
        return false;
#if defined(Q_OS_WIN)
    // the running app may bring its window to the front
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    socket.write(file.toUtf8() + '\n');
    socket.waitForBytesWritten(2000);
    socket.disconnectFromServer();
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
#if defined(Q_OS_MACOS)
    // the Mac build is for developing the port: its settings and presets stay
    // apart from the Mac app's own
    QApplication::setApplicationName(QStringLiteral("FLX Film Lab Port"));
#else
    QApplication::setApplicationName(QStringLiteral("FLX Film Lab"));
#endif
    QApplication::setApplicationDisplayName(QStringLiteral("FLX Film Lab"));
    QApplication::setApplicationVersion(QStringLiteral(FLX_VERSION_TEXT));
    theme::apply(app);

    QCommandLineParser parser;
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("A RAW file to open."));
    const QCommandLineOption screenshot(QStringLiteral("screenshot"), QStringLiteral("Window to PNG, then quit."),
                                        QStringLiteral("png"));
    const QCommandLineOption exportTo(QStringLiteral("export"), QStringLiteral("Export, then quit."),
                                      QStringLiteral("path"));
    const QCommandLineOption size(QStringLiteral("size"), QStringLiteral("Window size."), QStringLiteral("WxH"));
    const QCommandLineOption recipe(QStringLiteral("recipe"), QStringLiteral("A recipe to put on the file."),
                                    QStringLiteral("json"));
    const QCommandLineOption preview(QStringLiteral("preview"), QStringLiteral("The preview's long side."),
                                     QStringLiteral("pixels"));
    const QCommandLineOption savePreview(QStringLiteral("save-preview"), QStringLiteral("The preview to PNG."),
                                         QStringLiteral("png"));
    const QCommandLineOption bench(QStringLiteral("bench"), QStringLiteral("Time N renders."), QStringLiteral("n"));
    const QCommandLineOption cropping(QStringLiteral("cropping"), QStringLiteral("Show the crop being drawn."));
    const QCommandLineOption select(QStringLiteral("select"), QStringLiteral("Select a mask or gradient."),
                                    QStringLiteral("id"));
    const QCommandLineOption whiteBalance(QStringLiteral("white-balance"), QStringLiteral("A white balance to set."),
                                          QStringLiteral("name"));
    const QCommandLineOption at(QStringLiteral("at"), QStringLiteral("Motion: the frame at a time."),
                                QStringLiteral("seconds"));
    const QCommandLineOption play(QStringLiteral("play"), QStringLiteral("Motion: play first."),
                                  QStringLiteral("seconds"));
    const QCommandLineOption codec(QStringLiteral("codec"), QStringLiteral("Motion: prores or hevc."),
                                   QStringLiteral("name"));
    const QCommandLineOption exportLUT(QStringLiteral("export-lut"), QStringLiteral("Motion: the look as a .cube."),
                                       QStringLiteral("path"));
    const QCommandLineOption loupe(QStringLiteral("loupe"), QStringLiteral("Open the loupe at X,Y, W x H."),
                                   QStringLiteral("x,y,w,h"));
    const QCommandLineOption saveLoupe(QStringLiteral("save-loupe"), QStringLiteral("The loupe's picture to PNG."),
                                       QStringLiteral("png"));
    const QCommandLineOption saveOriginal(QStringLiteral("save-original"),
                                          QStringLiteral("The original, in the print's place, to PNG."),
                                          QStringLiteral("png"));
    const QCommandLineOption libraryDir(QStringLiteral("library"),
                                        QStringLiteral("The library, the sidebar's cached prints and the settings in DIR."),
                                        QStringLiteral("dir"));
    const QCommandLineOption selfTest(QStringLiteral("self-test"), QStringLiteral("The folder workflow, on RAWS."),
                                      QStringLiteral("raws"));
    const QCommandLineOption selfTestClips(QStringLiteral("self-test-clips"),
                                           QStringLiteral("With --self-test: Motion's too, on CLIPS."), QStringLiteral("clips"));
    const QCommandLineOption displayColour(QStringLiteral("display-colour"),
                                           QStringLiteral("The monitors' colour, to FILE."), QStringLiteral("file"));
    const QCommandLineOption iconSheet(QStringLiteral("icon-sheet"),
                                       QStringLiteral("Every toolbar icon, normal and on, to PNG, at 2x."),
                                       QStringLiteral("png"));
    const QCommandLineOption textSize(QStringLiteral("text-size"),
                                      QStringLiteral("The panels' text size for this run: Standard, Larger or Largest."),
                                      QStringLiteral("size"));
    parser.addOptions({screenshot, exportTo, size, recipe, preview, savePreview, bench, cropping, select, whiteBalance,
                       at, play, codec, exportLUT, loupe, saveLoupe, saveOriginal, libraryDir, selfTest, selfTestClips,
                       displayColour, textSize, iconSheet});
    parser.addHelpOption();
    parser.process(app);

    if (parser.isSet(iconSheet))
        return icons::sheet(40, 2).save(parser.value(iconSheet)) ? 0 : 2;

    if (parser.isSet(displayColour)) {
        QFile out(parser.value(displayColour));
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
            return 2;
        for (const displaycolour::State &s : displaycolour::readAll())
            out.write(QStringLiteral("%1: %2; %3; profile %4 (%5); previews %6\n")
                          .arg(s.monitor, s.mode,
                               s.managedByWindows ? QStringLiteral("Windows manages colour") : QStringLiteral("Windows leaves colour to the app"),
                               s.profile.isEmpty() ? QStringLiteral("none") : s.profile, s.description,
                               s.space.isValid() ? QStringLiteral("converted to it") : QStringLiteral("left in sRGB"))
                          .toUtf8());
        return 0;
    }

    // the test switches run an app of their own; anything else is handed on
    const bool testing = parser.isSet(screenshot) || parser.isSet(exportTo) || parser.isSet(exportLUT) || parser.isSet(saveLoupe)
        || parser.isSet(saveOriginal) || parser.isSet(selfTest);
    const QStringList files = parser.positionalArguments();
    if (!testing && handOver(files.isEmpty() ? QString() : QFileInfo(files.first()).absoluteFilePath()))
        return 0;

    // One library for both halves: each writes a folder's whole file, so two
    // would write over each other's pictures and clips.
    QString libraryAt = ImageLibrary::standardDirectory();
    if (parser.isSet(libraryDir)) {
        // the settings too: a test run never reads or changes the person's
        setAppSettingsFile(QDir(parser.value(libraryDir)).filePath(QStringLiteral("settings.ini")));
        libraryAt = QDir(parser.value(libraryDir)).filePath(QStringLiteral("Library"));
        ThumbnailStore::cacheDirectoryOverride = QDir(parser.value(libraryDir)).filePath(QStringLiteral("Thumbnails"));
    }
    ImageLibrary library(libraryAt);
    Document document(appData(), library);
    if (parser.isSet(preview))
        document.setPreviewSize(parser.value(preview).toInt(), false);
    MotionDocument motion(appData(), library);
    if (parser.isSet(textSize))
        for (theme::TextSize s : {theme::TextSize::Standard, theme::TextSize::Larger, theme::TextSize::Largest})
            if (parser.value(textSize).compare(theme::textSizeName(s), Qt::CaseInsensitive) == 0)
                theme::setTextSize(s, false);
    MainWindow window(document, motion);
    if (parser.isSet(size)) {
        const QStringList wh = parser.value(size).split(QLatin1Char('x'));
        if (wh.size() == 2)
            window.resize(wh[0].toInt(), wh[1].toInt());
    }
    window.show();

    QLocalServer server;
    if (!testing) {
        QLocalServer::removeServer(instanceName());     // one left by an app that crashed
        server.listen(instanceName());
        QObject::connect(&server, &QLocalServer::newConnection, &app, [&server, &window] {
            while (QLocalSocket *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QLocalSocket::readyRead, &window, [socket, &window] {
                    const QString path = QString::fromUtf8(socket->readAll()).trimmed();
                    if (!path.isEmpty())
                        window.openFile(path);
                    if (window.isMinimized())
                        window.showNormal();
                    window.raise();
                    window.activateWindow();
                });
            }
        });
    }

    if (parser.isSet(selfTest)) {
        if (!parser.isSet(libraryDir)) {
            std::fprintf(stderr, "--self-test needs --library DIR, where it works\n");
            return 2;
        }
        QTimer::singleShot(0, &app, [&] {
            const int failed = runSelfTest(window, document, motion, parser.value(selfTest), parser.value(selfTestClips),
                                           parser.value(libraryDir));
            QCoreApplication::exit(failed == 0 ? 0 : 1);
        });
        return app.exec();
    }

    if (!files.isEmpty())
        window.openFile(files.first());
    const bool folder = !files.isEmpty() && QFileInfo(files.first()).isDir();
    // the sidebar's first pictures, as far as they fill it, before the
    // screenshot: a folder's, or those of the folder of the file opened
    const bool listed = !files.isEmpty();
    QElapsedTimer launched;
    launched.start();
    auto sidebarReady = [&](FolderBrowser &browser) {
        const int wanted = std::min<int>(int(browser.items().size()), 8);
        return !listed || browser.thumbnails().count() >= wanted || launched.elapsed() > 30000;
    };

    // the test switches with a movie: wait until its frame is graded
    if ((parser.isSet(screenshot) || parser.isSet(exportTo) || parser.isSet(exportLUT) || parser.isSet(saveLoupe)) && !files.isEmpty()
        && (isMovie(files.first()) || (folder && window.mode() == MainWindow::Mode::Motion))) {
        auto *poll = new QTimer(&app);
        auto stage = std::make_shared<int>(0);      // 0 opening, 1 seek, 2 play, 3 export, 4 done
        auto quiet = std::make_shared<int>(0);
        auto frames = std::make_shared<int>(0);
        auto clock = std::make_shared<QElapsedTimer>();
        QObject::connect(&motion, &MotionDocument::frameChanged, &app, [frames] { ++*frames; });
        // tests play without anyone listening
        motion.setMuted(true);
        QObject::connect(poll, &QTimer::timeout, &app, [&, poll, stage, quiet, frames, clock] {
            if (!motion.errorMessage().isEmpty() && !motion.clip()) {
                std::fprintf(stderr, "error: %s\n", qPrintable(motion.errorMessage()));
                QCoreApplication::exit(1);
                return;
            }
            const bool busy = motion.isOpening() || motion.isRendering() || motion.image().isNull()
                || motion.isMetering() || motion.isExporting() || motion.isPlaying() || !sidebarReady(motion.browser());
            *quiet = busy ? 0 : *quiet + 1;
            if (*stage == 2 && motion.isPlaying() && clock->elapsed() >= parser.value(play).toDouble() * 1000)
                motion.pause();
            if (*quiet < 3)
                return;
            if (*stage == 0) {
                *stage = 1;
                if (parser.isSet(at)) {
                    motion.seek(parser.value(at).toDouble());
                    *quiet = 0;
                    return;
                }
            }
            if (*stage == 1) {
                *stage = 2;
                if (parser.isSet(play)) {
                    *frames = 0;
                    clock->start();
                    motion.play();
                    *quiet = 0;
                    return;
                }
            }
            if (*stage == 2) {
                *stage = 3;
                if (parser.isSet(play)) {
                    const double seconds = double(clock->elapsed()) / 1000;
                    std::printf("played %.1f s: %d frames shown, %.1f fps (the clip is %.3f fps); %.2f s of sound "
                                "handed to the speakers\n",
                                seconds, *frames, *frames / std::max(seconds, 1e-3), motion.frameRate(),
                                motion.soundHanded());
                }
                if (parser.isSet(exportLUT))
                    motion.exportLUT(QFileInfo(parser.value(exportLUT)).absoluteFilePath());
                if (parser.isSet(exportTo)) {
                    MotionLook look = motion.look();
                    look.exportSettings.codec =
                        parser.value(codec) == QStringLiteral("hevc") ? ExportCodec::HEVC10 : ExportCodec::ProRes422HQ;
                    motion.setLook(look);
                    clock->start();
                    motion.exportTo(QFileInfo(parser.value(exportTo)).absoluteFilePath());
                    *quiet = 0;
                    return;
                }
            }
            if (*stage == 3) {
                *stage = 4;
                if (parser.isSet(exportTo))
                    std::printf("export took %.1f s\n", double(clock->elapsed()) / 1000);
                if (parser.isSet(screenshot))
                    window.grab().save(parser.value(screenshot));
                if (parser.isSet(savePreview))
                    motion.image().save(parser.value(savePreview));
                std::printf("%s  %.0f ms\n", qPrintable(motion.status()), motion.drawMilliseconds());
                if (!motion.errorMessage().isEmpty())
                    std::fprintf(stderr, "error: %s\n", qPrintable(motion.errorMessage()));
                poll->stop();
                QCoreApplication::exit(motion.errorMessage().isEmpty() ? 0 : 1);
            }
        });
        poll->start(100);
        return app.exec();
    }

    // the test switches: wait until the file has rendered and nothing is pending
    if (testing) {
        if (files.isEmpty()) {
            std::fprintf(stderr, "--screenshot and --export need a file\n");
            return 2;
        }
        auto *poll = new QTimer(&app);
        auto recipeApplied = std::make_shared<bool>(!parser.isSet(recipe));
        auto exportStarted = std::make_shared<bool>(false);
        auto quiet = std::make_shared<int>(0);
        auto benched = std::make_shared<std::vector<double>>();
        const int benchRuns = parser.value(bench).toInt();
        QObject::connect(poll, &QTimer::timeout, &app, [&, poll, recipeApplied, exportStarted, quiet, benched,
                                                        benchRuns] {
            if (!document.errorMessage().isEmpty()) {
                std::fprintf(stderr, "error: %s\n", qPrintable(document.errorMessage()));
                if (!document.hasImage()) {
                    // the window as the person sees the error
                    if (parser.isSet(screenshot))
                        window.grab().save(parser.value(screenshot));
                    poll->stop();
                    QCoreApplication::exit(1);
                }
            }
            const bool busy = document.isDecoding() || document.isRendering() || document.displayImage().isNull()
                || document.isExporting() || !sidebarReady(document.browser());
            *quiet = busy ? 0 : *quiet + 1;
            if (*quiet < 3)
                return;
            if (!*recipeApplied) {
                QFile f(parser.value(recipe));
                if (!f.open(QIODevice::ReadOnly)) {
                    std::fprintf(stderr, "cannot read %s\n", qPrintable(parser.value(recipe)));
                    QCoreApplication::exit(2);
                    return;
                }
                document.setRecipe(recipeFromJson(QJsonDocument::fromJson(f.readAll()).object()));
                *recipeApplied = true;
                *quiet = 0;
                return;
            }
            if (parser.isSet(select) && document.selected() != parser.value(select).toStdString())
                document.setSelected(parser.value(select).toStdString());
            if (parser.isSet(whiteBalance) && document.whiteBalance() != whiteBalanceNamed(parser.value(whiteBalance))) {
                document.setWhiteBalance(whiteBalanceNamed(parser.value(whiteBalance)));
                *quiet = 0;
                return;
            }
            if (parser.isSet(cropping) && !document.isCropping()) {
                document.setCropping(true);
                *quiet = 0;
                return;
            }
            if (int(benched->size()) < benchRuns) {
                // the slider's tick: the exposure moved, the auto off
                if (!benched->empty() || document.recipe().autoExposure) {
                    benched->push_back(document.lastRenderMilliseconds());
                    std::fprintf(stderr, "%s\n", qPrintable(document.lastRenderBreakdown()));
                }
                // odd ticks move the exposure (the film stage again), even ones
                // the brightness (the print alone)
                Recipe r = document.recipe();
                r.autoExposure = false;
                if (benched->size() % 2)
                    r.exposureEV += benched->size() % 4 == 1 ? 0.05 : -0.05;
                else
                    r.brightness += benched->size() % 4 == 0 ? 0.01 : -0.01;
                document.setRecipe(r);
                *quiet = 0;
                return;
            }
            if (parser.isSet(loupe) && !document.loupeOn()) {
                const QStringList v = parser.value(loupe).split(QLatin1Char(','));
                if (v.size() == 4) {
                    document.moveLoupe(QPointF(v[0].toDouble(), v[1].toDouble()));
                    document.setLoupeSize(QSize(v[2].toInt(), v[3].toInt()));
                    document.setLoupeOn(true);
                }
                *quiet = 0;
                return;
            }
            if (document.loupeOn() && (document.loupeBusy() || document.loupeImage().isNull())) {
                *quiet = 0;
                return;
            }
            if (parser.isSet(saveOriginal) && !document.showOriginal()) {
                document.setShowOriginal(true);
                *quiet = 0;
                return;
            }
            if (document.showOriginal() && document.original().isNull()) {
                *quiet = 0;
                return;
            }
            if (parser.isSet(exportTo) && !*exportStarted) {
                const QString path = QFileInfo(parser.value(exportTo)).absoluteFilePath();
                const QString ext = QFileInfo(path).suffix().toLower();
                const ExportFormat format = ext == "jpg" || ext == "jpeg" ? ExportFormat::Jpeg
                                            : ext == "png"                 ? ExportFormat::Png
                                                                           : ExportFormat::Tiff16;
                document.exportTo(path, format);
                *exportStarted = true;
                *quiet = 0;
                return;
            }
            if (parser.isSet(screenshot))
                window.grab().save(parser.value(screenshot));
            if (parser.isSet(savePreview))
                document.displayImage().save(parser.value(savePreview));
            if (parser.isSet(saveLoupe))
                document.loupeImage().save(parser.value(saveLoupe));
            if (parser.isSet(saveOriginal))
                document.original().save(parser.value(saveOriginal));
            if (!benched->empty()) {
                std::vector<double> t(benched->begin() + 1, benched->end());
                std::sort(t.begin(), t.end());
                if (!t.empty())
                    std::printf("renders after the first: median %.1f ms, fastest %.1f, slowest %.1f (%zu)\n",
                                t[t.size() / 2], t.front(), t.back(), t.size());
            }
            std::printf("%s  %.0f ms\n", qPrintable(document.status()), document.lastRenderMilliseconds());
            poll->stop();
            QCoreApplication::exit(document.errorMessage().isEmpty() ? 0 : 1);
        });
        poll->start(100);
    }
    return app.exec();
}
