#include "MotionDocument.h"
#include "EditNames.h"

#include "AppSettings.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>

namespace {

const ProfilePack *tryPack(PackLibrary &packs, const std::string &stock)
{
    try {
        return &packs.pack(stock);
    } catch (const std::exception &) {
        return nullptr;
    }
}

const char *kLookKey = "motionLook";

// How an encoder is named in the status line, where it says something.
QString encoderNote(const std::string &encoder)
{
    if (encoder == "hevc_nvenc")
        return QStringLiteral(" (HEVC on NVIDIA's encoder)");
    if (encoder == "hevc_amf")
        return QStringLiteral(" (HEVC on AMD's encoder)");
    if (encoder == "hevc_qsv")
        return QStringLiteral(" (HEVC on Intel's encoder)");
    if (encoder == "hevc_videotoolbox")
        return QStringLiteral(" (HEVC on Apple's encoder)");
    if (encoder == "libx265")
        return QStringLiteral(" (HEVC with x265, on the processor)");
    return {};
}

const QStringList &movieExtensions()
{
    static const QStringList kinds{"mov", "mp4", "m4v", "mxf", "mkv", "avi", "mts", "m2ts", "mpg",
                                   "mpeg", "3gp", "webm", "insv"};
    return kinds;
}

} // namespace

bool isMovie(const QString &path)
{
    return movieExtensions().contains(QFileInfo(path).suffix().toLower());
}

QString movieFilter()
{
    QStringList patterns;
    for (const QString &ext : movieExtensions())
        patterns << QStringLiteral("*.") + ext;
    return QStringLiteral("Movies (%1);;All files (*)").arg(patterns.join(QLatin1Char(' ')));
}

MotionDocument::MotionDocument(const AppData &data, QObject *parent)
    : QObject(parent), m_data(data)
{
    m_packs = std::make_unique<PackLibrary>(data.packs);
    m_builder = std::make_unique<SetupBuilder>(*m_packs);
    m_engine = std::make_unique<MotionEngine>(data.packs);
    m_engineContext = new QObject;
    m_engineContext->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_engineContext, &QObject::deleteLater);
    m_thread.setObjectName(QStringLiteral("motion"));
    m_thread.start();

    // the look last used, brought up to date, with a new clip's dials
    const QByteArray stored = appSettings().value(kLookKey).toByteArray();
    const QJsonDocument json = QJsonDocument::fromJson(stored);
    const MotionLook look = json.isObject() ? motionLookFromJson(json.object()) : MotionLook::initial();
    const ProfilePack *film = tryPack(*m_packs, look.recipe.filmStock);
    m_look = look.carryingLook(film ? double(film->defaultGrade()) : double(kGradeReference));

    m_ticker.setTimerType(Qt::PreciseTimer);
    connect(&m_ticker, &QTimer::timeout, this, &MotionDocument::tick);
    m_statusClock.start();
    m_history = std::make_unique<EditHistory<MotionLook>>(
        this, [this] { return grade(); },
        [this](const MotionLook &g) {
            MotionLook next = g;
            next.exportSettings = m_look.exportSettings;
            setLook(next);
        },
        [this](const MotionLook &a, const MotionLook &b) {
            const ProfilePack *film = tryPack(*m_packs, b.recipe.filmStock);
            return motionChangeName(a, b, film && film->isMonochrome());
        },
        [this] { emit changed(); });
    m_history->joinSeconds = QApplication::doubleClickInterval() / 1000.0;
}

MotionDocument::~MotionDocument()
{
    m_ticker.stop();
    m_cancelExport = true;
    for (const QPointer<QThread> &worker : m_workers)
        if (worker)
            worker->wait();
    QMetaObject::invokeMethod(m_engineContext, [this] { m_engine.reset(); }, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

void MotionDocument::runOnEngine(std::function<void()> work)
{
    QMetaObject::invokeMethod(m_engineContext, std::move(work), Qt::QueuedConnection);
}

// MARK: - The look

void MotionDocument::setLook(const MotionLook &look)
{
    m_history->note([this] { return grade(); });
    const MotionLook old = m_look;
    m_look = look;
    lookDidChange(old);
}

void MotionDocument::lookDidChange(const MotionLook &old)
{
    if (!(m_look.input == old.input))
        meter();
    else if (m_look.recipe.filmStock != old.recipe.filmStock)
        updateMeteredExposure();
    // re-read the frame on screen at once, so a paused clip's reading and
    // probe follow an override without waiting for the next frame
    if (!(m_look.input == old.input) && m_frame) {
        m_reading = readingOf(*m_frame, readingSettings(), format());
        if (m_probePoint)
            probe(m_probePoint);
    }
    store();
    scheduleRender();
    emit changed();
}

void MotionDocument::store()
{
    if (m_clip)
        m_edits[m_clipPath] = m_look;
    appSettings().setValue(kLookKey, QJsonDocument(motionLookToJson(m_look)).toJson(QJsonDocument::Compact));
}

Recipe MotionDocument::effectiveRecipe() const
{
    Recipe r = m_look.recipe;
    if (r.autoExposure && m_metered)
        r.exposureEV = *m_metered;
    return r;
}

LogFormat MotionDocument::format() const
{
    if (m_look.input.format)
        return *m_look.input.format;
    return m_clip ? m_clip->detection().first : LogFormat::NLog;
}

InputSettings MotionDocument::readingSettings() const
{
    InputSettings settings = m_look.input;
    settings.format = format();
    return settings;
}

const ProfilePack *MotionDocument::filmPack()
{
    return tryPack(*m_packs, m_look.recipe.filmStock);
}

const ProfilePack *MotionDocument::paperPack()
{
    return tryPack(*m_packs, m_look.recipe.printStock);
}

QString MotionDocument::filmName()
{
    const ProfilePack *p = filmPack();
    return QString::fromStdString(p ? p->name : m_look.recipe.filmStock);
}

QString MotionDocument::paperName()
{
    const ProfilePack *p = paperPack();
    return QString::fromStdString(p ? p->name : m_look.recipe.printStock);
}

double MotionDocument::defaultGrade()
{
    const ProfilePack *film = filmPack();
    return film ? double(film->defaultGrade()) : double(kGradeReference);
}

bool MotionDocument::printsOnPaper()
{
    const ProfilePack *film = filmPack();
    return film ? !film->isViewedDirectly(m_look.recipe.scanFilm) : !m_look.recipe.scanFilm.value_or(false);
}

// A black-and-white paper prints in silver: whatever the filtration, the
// picture comes out neutral, so white balance has nothing to act on.
bool MotionDocument::printsInSilver()
{
    const ProfilePack *paper = paperPack();
    return printsOnPaper() && paper && paper->isMonochrome();
}

void MotionDocument::selectFilm(const std::string &stock)
{
    if (stock == m_look.recipe.filmStock)
        return;
    MotionLook next = m_look;
    next.recipe = loadingFilm(m_look.recipe, stock, *m_packs);
    setLook(next);
}

void MotionDocument::resetExposure()
{
    m_history->named("Reset Exposure", [&] {
        MotionLook next = m_look;
        next.recipe.exposureEV = 0;
        next.recipe.autoExposure = MotionLook::initial().recipe.autoExposure;
        setLook(next);
    });
}

void MotionDocument::resetTone()
{
    m_history->named("Reset Tone", [&] {
        MotionLook next = m_look;
        next.recipe.brightness = 0;
        next.recipe.grade = defaultGrade();
        next.recipe.shadows = 0;
        next.recipe.highlights = 0;
        setLook(next);
    });
}

void MotionDocument::resetColour()
{
    m_history->named("Reset Colour", [&] {
        MotionLook next = m_look;
        next.recipe.filterShiftY = 0;
        next.recipe.filterShiftM = 0;
        setLook(next);
    });
}

void MotionDocument::resetEffects()
{
    m_history->named("Reset Effects", [&] {
        MotionLook next = m_look;
        next.effects = MotionLook::initial().effects;
        setLook(next);
    });
}

void MotionDocument::setShowLog(bool on)
{
    if (m_showLog == on)
        return;
    m_showLog = on;
    scheduleRender();
    emit changed();
}

void MotionDocument::clearError()
{
    m_error.clear();
    emit statusChanged();
}

// MARK: - Clips

// Safe during an export: that holds its own copy of the clip and the look.
void MotionDocument::open(const QString &path)
{
    m_isOpening = true;
    emit changed();
    const int id = ++m_clipCounter;
    runOnEngine([this, path, id] {
        QString error;
        std::optional<ClipInfo> info;
        try {
            info = m_engine->open(path, id);
        } catch (const std::exception &e) {
            error = QString::fromStdString(e.what());
        }
        QMetaObject::invokeMethod(this, [this, info, error, path, id] {
            if (info) {
                adopt(*info, path, id);
            } else {
                m_isOpening = false;
                m_error = error;
                emit changed();
                emit statusChanged();
            }
        });
    });
}

void MotionDocument::adopt(const ClipInfo &info, const QString &path, int id)
{
    pause();
    if (path != m_clipPath) {
        // what was done before the first clip came is not any clip's
        EditHistory<MotionLook>::Stacks steps;
        if (const auto it = m_histories.find(path); it != m_histories.end()) {
            steps = std::move(it->second);
            m_histories.erase(it);
        }
        steps = m_history->swap(std::move(steps));
        if (m_clip)
            m_histories[m_clipPath] = std::move(steps);
    }
    if (m_clip)
        m_edits[m_clipPath] = m_look;
    // the clip first: storing the look files it under the clip on screen,
    // which has to be the new one by then
    m_clip = info;
    m_clipPath = path;
    m_clipID = id;
    const auto known = m_edits.find(path);
    MotionLook next = known != m_edits.end() ? known->second : m_look.carryingLook(defaultGrade());
    // how files are written is a preference, not part of a grade
    next.exportSettings = m_look.exportSettings;
    m_look = next;
    m_reading.reset();
    m_metered.reset();
    m_frame.reset();
    m_image = QImage();
    m_time = 0;
    m_probePoint.reset();
    m_probeText.clear();
    m_audio.setClip(path, info.hasAudio);
    store();
    meter();
    m_error.clear();
    m_status = QString::fromStdString(info.summary());
    if (info.bitDepth > 0 && info.bitDepth < 10)
        m_error = QStringLiteral("This clip is 8-bit; log footage is recorded in 10-bit. Is it a log clip?");
    m_isOpening = false;
    // the canvas sizes the new clip's frame when it hears of it, and that renders
    m_viewSize = QSize();
    emit changed();
    emit frameChanged();
    emit statusChanged();
}

// MARK: - Rendering

void MotionDocument::setViewSize(QSize graded)
{
    if (graded == m_viewSize)
        return;
    m_viewSize = graded;
    scheduleRender();
}

// One frame at a time; whatever changes while it is made folds into a single
// next one with the newest state, so a drag or playback never queues up
// stale frames.
void MotionDocument::scheduleRender()
{
    if (!m_clip || m_viewSize.isEmpty())
        return;
    if (m_renderInFlight) {
        m_renderAgain = true;
        return;
    }
    m_renderInFlight = true;
    MotionEngine::Request request;
    request.clip = m_clipID;
    request.time = m_time;
    request.recipe = effectiveRecipe();
    request.format = format();
    request.input = readingSettings();
    request.effects = effectsAsPrinted(m_look, *m_packs);
    request.mode = m_showLog ? FramePipeline::Mode::Log : FramePipeline::Mode::Graded;
    request.size = m_viewSize;
    runOnEngine([this, request] {
        QString error;
        std::optional<MotionEngine::Output> output;
        try {
            output = m_engine->render(request);
        } catch (const std::exception &e) {
            error = QString::fromStdString(e.what());
        }
        QMetaObject::invokeMethod(this, [this, output = std::move(output), error]() mutable {
            finishRender(std::move(output), error);
        });
    });
}

void MotionDocument::finishRender(std::optional<MotionEngine::Output> output, const QString &error)
{
    m_renderInFlight = false;
    bool statusMoved = false;
    if (!error.isEmpty()) {
        m_error = error;
        statusMoved = true;
    }
    if (output && output->clip == m_clipID) {
        m_frame = output->frame;
        if (!m_reading || !(*m_reading == output->reading)) {
            m_reading = output->reading;
            emit changed();
        }
        if (!output->image.isNull()) {
            m_image = std::move(output->image);
            m_drawMilliseconds = output->milliseconds;
            emit frameChanged();
        }
        if (m_probePoint)
            probe(m_probePoint);
        // the timing a few times a second, not with every frame
        if (m_statusClock.elapsed() > 300) {
            m_statusClock.restart();
            statusMoved = true;
        }
    }
    if (statusMoved)
        emit statusChanged();
    if (m_renderAgain) {
        m_renderAgain = false;
        scheduleRender();
    }
}

// MARK: - Metering

// Meter the clip on screen, unless it already has been as it is read now.
void MotionDocument::meter()
{
    if (!m_clip)
        return;
    const InputSettings settings = readingSettings();
    if (const auto known = m_highlights.find(m_clipPath); known != m_highlights.end() && known->second.first == settings) {
        updateMeteredExposure();
        return;
    }
    const int generation = ++m_meterGeneration;
    m_isMetering = true;
    const QString path = m_clipPath;
    const LogFormat f = format();
    QPointer<MotionDocument> self(this);
    QThread *worker = QThread::create([self, generation, path, settings, f] {
        std::optional<double> highlight;
        try {
            highlight = clipHighlight(path.toStdString(), settings, f);
        } catch (const std::exception &) {
        }
        QMetaObject::invokeMethod(qApp, [self, generation, path, settings, highlight] {
            if (self)
                self->finishMeter(generation, path, settings, highlight);
        });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    m_workers.erase(std::remove_if(m_workers.begin(), m_workers.end(), [](const QPointer<QThread> &t) { return !t; }),
                    m_workers.end());
    m_workers.emplace_back(worker);
    worker->start(QThread::LowPriority);
}

void MotionDocument::finishMeter(int generation, const QString &path, const InputSettings &settings,
                                 std::optional<double> highlight)
{
    m_highlights[path] = {settings, highlight};
    if (generation != m_meterGeneration)
        return;
    m_isMetering = false;
    if (path == m_clipPath)
        updateMeteredExposure();
    emit changed();
}

// The stock's reading of the clip's metered highlight.
void MotionDocument::updateMeteredExposure()
{
    const auto known = m_highlights.find(m_clipPath);
    const ProfilePack *film = filmPack();
    if (!m_clip || known == m_highlights.end() || !(known->second.first == readingSettings()) || !known->second.second
        || !film) {
        if (m_metered) {
            m_metered.reset();
            scheduleRender();
        }
        return;
    }
    const double ev = clipExposure(*known->second.second, m_builder->highlightHeadroomStops(*film));
    if (m_metered && *m_metered == ev)
        return;
    m_metered = ev;
    scheduleRender();
    emit changed();
}

// MARK: - The probe

// A grey card exposed the way the camera maker recommends reads 0 stops:
// N-Log code 372 (35%), V-Log 433 (42%), Apple Log signal 0.488.
void MotionDocument::probe(std::optional<QPointF> sourcePixel)
{
    m_probePoint = sourcePixel;
    std::optional<Vec3d> codes;
    if (sourcePixel && m_frame && m_reading)
        codes = codesAt(*m_frame, *m_reading, int(sourcePixel->x()), int(sourcePixel->y()));
    if (!codes) {
        if (!m_probeText.isEmpty()) {
            m_probeText.clear();
            emit statusChanged();
        }
        return;
    }
    const Vec3d &c = *codes;
    const double mean = (c[0] + c[1] + c[2]) / 3;
    const Vec3d light = logScene(m_reading->format, c, m_reading->fullRange);
    const double luminance = 0.2627 * light[0] + 0.6780 * light[1] + 0.0593 * light[2];
    const QString stops = luminance > 0 ? QString::asprintf("%+.1f stops", std::log2(luminance / 0.18))
                                        : QStringLiteral("below black");
    const QString text = QString::asprintf("%s %.0f %.0f %.0f · %.0f%% IRE · ",
                                           logFormatShortName(m_reading->format).c_str(), c[0], c[1], c[2],
                                           (mean - 64) / 876 * 100)
                       + stops;
    if (text != m_probeText) {
        m_probeText = text;
        emit statusChanged();
    }
}

// MARK: - Playback

double MotionDocument::lastFrameTime() const
{
    if (!m_clip)
        return 0;
    const double frame = m_clip->frameRate > 0 ? 1 / m_clip->frameRate : 1.0 / 30;
    return std::max(m_clip->duration - frame, 0.0);
}

void MotionDocument::togglePlay()
{
    if (m_playing)
        pause();
    else
        play();
}

void MotionDocument::play()
{
    if (!m_clip || m_playing)
        return;
    if (m_time >= lastFrameTime())
        m_time = 0;
    m_playing = true;
    m_playFrom = m_time;
    // the sound first: opening it and the speakers takes a moment the
    // picture's clock must not count
    m_audio.start(m_time);
    m_playClock.start();
    const double rate = m_clip->frameRate > 0 ? m_clip->frameRate : 30;
    // twice a frame, so each is asked for close to when it is due
    m_ticker.start(std::max(int(500 / rate), 4));
    scheduleRender();
    emit changed();
    emit statusChanged();
}

void MotionDocument::pause()
{
    if (!m_playing)
        return;
    m_playing = false;
    m_ticker.stop();
    m_audio.stop();
    emit changed();
    emit statusChanged();
}

void MotionDocument::tick()
{
    if (qEnvironmentVariableIsSet("FLX_TIMING"))
        std::fprintf(stderr, "clock %.3f s, sound played %.3f s, handed %.3f s\n",
                     double(m_playClock.nsecsElapsed()) / 1e9, m_audio.playedSeconds(), m_audio.handedSeconds());
    double t = m_playFrom + double(m_playClock.nsecsElapsed()) / 1e9;
    const double end = lastFrameTime();
    if (t >= end) {
        t = end;
        pause();
    }
    if (t == m_time)
        return;
    m_time = t;
    scheduleRender();
    emit timeChanged();
}

void MotionDocument::seek(double seconds)
{
    if (!m_clip)
        return;
    m_time = std::clamp(seconds, 0.0, lastFrameTime());
    if (m_playing) {
        m_playFrom = m_time;
        m_audio.start(m_time);
        m_playClock.start();
    }
    scheduleRender();
    emit timeChanged();
}

void MotionDocument::step(int frames)
{
    if (!m_clip)
        return;
    pause();
    const double frame = m_clip->frameRate > 0 ? 1 / m_clip->frameRate : 1.0 / 30;
    const double from = m_frame ? m_frame->time : m_time;
    seek(from + frames * frame);
}

// MARK: - Export

QString MotionDocument::suggestedExportName()
{
    const QString stem = m_clip ? QFileInfo(m_clipPath).completeBaseName() : QStringLiteral("FLX Film Lab");
    QString film = filmName();
    for (const QString &maker : {QStringLiteral("Kodak "), QStringLiteral("Fujifilm "), QStringLiteral("Ilford ")})
        film.remove(maker);
    return stem + QLatin1Char(' ') + film + QStringLiteral(".mov");
}

void MotionDocument::exportTo(const QString &path)
{
    if (!m_clip || isExporting())
        return;
    pause();
    m_exportProgress = 0;
    m_cancelExport = false;
    MotionLook look = m_look;
    look.recipe = effectiveRecipe();
    look.effects = effectsAsPrinted(m_look, *m_packs);
    look.input = readingSettings();
    const LogFormat f = format();
    const QString source = m_clipPath, packs = m_data.packs, name = QFileInfo(path).fileName();
    m_status = QStringLiteral("Exporting %1…").arg(name);
    QPointer<MotionDocument> self(this);
    QThread *worker = QThread::create([this, self, look, f, source, packs, path, name] {
        QString error;
        std::string encoder;
        bool cancelled = false;
        int shown = -1;
        try {
            encoder = runClipExport(packs, source, path, look, f, [this, self, &shown](double p) {
                const int permille = int(p * 1000);
                if (permille != shown) {
                    shown = permille;
                    QMetaObject::invokeMethod(qApp, [self, p] {
                        if (self && self->m_exportProgress) {
                            self->m_exportProgress = p;
                            emit self->statusChanged();
                        }
                    });
                }
                return !m_cancelExport.load();
            });
        } catch (const ExportCancelled &) {
            cancelled = true;
        } catch (const std::exception &e) {
            error = QString::fromStdString(e.what());
        }
        QMetaObject::invokeMethod(qApp, [self, error, encoder, cancelled, name] {
            if (!self)
                return;
            self->m_exportProgress.reset();
            if (cancelled) {
                self->m_status = QStringLiteral("Export cancelled");
            } else if (!error.isEmpty()) {
                self->m_error = error;
                self->m_status = self->m_clip ? QString::fromStdString(self->m_clip->summary()) : QString();
            } else {
                self->m_status = QStringLiteral("Exported %1").arg(name) + encoderNote(encoder);
            }
            emit self->changed();
            emit self->statusChanged();
        });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    m_workers.emplace_back(worker);
    worker->start();
    emit changed();
    emit statusChanged();
}

void MotionDocument::cancelExport()
{
    m_cancelExport = true;
}

// What the print is, for an exported LUT (ClipModel.printDescription): its
// title, and how it was made for its comment. The film on its paper; a slide
// alone, off the light table; an instant print alone, as the print it is.
std::pair<std::string, std::string> MotionDocument::printDescription()
{
    const std::string film = filmName().toStdString(), paper = paperName().toStdString();
    const ProfilePack *f = filmPack();
    if (f && f->isInstantPrint()) {
        const auto fahrenheit = f->processingFahrenheit(m_look.recipe.processingTemperature);
        return {film, fahrenheit ? film + ", an instant print processed at " + std::to_string(*fahrenheit) + " F"
                                 : film + ", an instant print"};
    }
    if (f ? f->isViewedDirectly(m_look.recipe.scanFilm) : m_look.recipe.scanFilm.value_or(false))
        return {film, film + " on a light table"};
    return {film + " on " + paper, film + " printed on " + paper};
}

QString MotionDocument::suggestedLUTName()
{
    return QString::fromStdString(printDescription().first) + QStringLiteral(".cube");
}

// The clip's log goes in the way Resolve reads it (exportInputOf): N-Log or
// V-Log code values / 1023 for a full-range file, which is what a Zf writes,
// video levels for a video-range one, and Apple Log on its signal.
void MotionDocument::exportLUT(const QString &path)
{
    const Recipe recipe = effectiveRecipe();
    bool fullRange = true;
    switch (m_look.input.levels) {
    case InputSettings::Levels::Full:
        fullRange = true;
        break;
    case InputSettings::Levels::Video:
        fullRange = false;
        break;
    case InputSettings::Levels::Auto:
        fullRange = m_clip && m_clip->fullRange ? *m_clip->fullRange : true;
        break;
    }
    const LutInput input = exportInputOf(format(), fullRange);
    const std::pair<std::string, std::string> description = printDescription();
    const std::string title = description.first;
    std::vector<std::string> comments{
        "Made with FLX Film Lab (Motion): " + description.second,
        QString::asprintf("Exposure %+.2f EV, brightness %+.3f, grade R%.0f, shadows %+.2f, highlights %+.2f",
                          recipe.exposureEV, recipe.brightness, recipe.grade, recipe.shadows, recipe.highlights)
            .toStdString(),
        QString::asprintf("Filtration on top of neutral: %+.0fY %+.0fM", recipe.filterShiftY, recipe.filterShiftM)
            .toStdString(),
        "Per-pixel only: diffusion, glow and halation are not in a LUT",
    };
    if (const ProfilePack *f = filmPack())
        if (const LensFilterRef *filter = f->lensFilter(recipe.lensFilter))
            comments.insert(comments.begin() + 1,
                            QString::asprintf("Camera filter: %s, %.1fx (exposure opened %.1f stops)",
                                              filter->designation().c_str(), double(filter->factor),
                                              std::log2(std::max(double(filter->factor), 1.0)))
                                .toStdString());
    const QString name = QFileInfo(path).fileName();
    runOnEngine([this, recipe, input, title, comments, path, name] {
        QString error;
        try {
            const std::string text = m_engine->cube(recipe, input, title, comments);
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(text.data(), qint64(text.size()))
                                                                               != qint64(text.size()))
                throw std::runtime_error(file.errorString().toStdString());
        } catch (const std::exception &e) {
            error = QString::fromStdString(e.what());
        }
        QMetaObject::invokeMethod(this, [this, error, name] {
            if (error.isEmpty())
                m_status = QStringLiteral("Wrote %1").arg(name);
            else
                m_error = QStringLiteral("Could not write the LUT: %1").arg(error);
            emit statusChanged();
        });
    });
}
