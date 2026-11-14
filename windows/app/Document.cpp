#include "Document.h"
#include "EditNames.h"
#include "Exif.h"
#include "AppSettings.h"
#include "Exporter.h"
#include "Thumbnails.h"
#include "AppleRendering.h"
#include "DisplayColour.h"

#include <QApplication>
#include <QColorSpace>
#include <QPainter>
#include <QTransform>
#include <QDir>
#include <QFileInfo>
#include <QtMath>
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
#include <exception>

namespace {

// The recipe a newly opened image starts from: this one's look (film, paper,
// how it is printed, the film character and the output) with everything
// dialled for one frame back at its default, and the autos as a new image
// has them (Recipe.carryingLook).
Recipe carryingLook(const Recipe &from, double defaultGrade)
{
    Recipe r;
    r.filmStock = from.filmStock;
    r.grade = defaultGrade;
    r.printStock = from.printStock;
    r.lensFilter = from.lensFilter;
    r.scanFilm = from.scanFilm;
    r.normalizeToBase = from.normalizeToBase;
    r.densityCurveGamma = from.densityCurveGamma;
    r.couplersEnabled = from.couplersEnabled;
    r.couplersAmount = from.couplersAmount;
    r.couplersDiffusion = from.couplersDiffusion;
    r.halationEnabled = from.halationEnabled;
    r.halationAmount = from.halationAmount;
    r.halationScale = from.halationScale;
    r.scatterAmount = from.scatterAmount;
    r.scatterScale = from.scatterScale;
    r.cameraSharpness = from.cameraSharpness;
    r.grainEnabled = from.grainEnabled;
    r.grainAmount = from.grainAmount;
    r.grainScale = from.grainScale;
    r.glareEnabled = from.glareEnabled;
    r.glareAmount = from.glareAmount;
    r.effects = from.effects;
    r.filmFormat = from.filmFormat;
    r.processingTemperature = from.processingTemperature;
    r.outputColorSpace = from.outputColorSpace;
    r.gamutCompress = from.gamutCompress;
    return r;
}

const ProfilePack *tryPack(PackLibrary &packs, const std::string &stock)
{
    try {
        return &packs.pack(stock);
    } catch (const std::exception &) {
        return nullptr;
    }
}

// The illuminants the stocks name, as colour temperatures
// (RawLoader.referenceTemperature).
double referenceTemperature(const std::string &illuminant)
{
    if (illuminant == "BB3200")
        return 3200;        // a 3200 K lamp, the tungsten stocks' reference
    if (illuminant == "T")
        return 2856;        // agx's household bulb, read as CIE A
    if (illuminant == "TH-KG3" || illuminant == "TH-KG3-L")
        return 3400;
    if (illuminant == "D50")
        return 5003;
    if (illuminant == "D65")
        return 6504;
    if (illuminant == "K75P")
        return 5400;
    return 5503;
}

// Short of memory, decoding ahead waits and the sidebar keeps fewer pictures
// (the Mac's memory pressure): under a gigabyte and a half free, or nine
// tenths of it in use.
bool lowOnMemory()
{
#if defined(Q_OS_WIN)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        return status.ullAvailPhys < 1536ull << 20 || status.dwMemoryLoad >= 90;
#endif
    return false;
}

} // namespace

Document::Document(const AppData &data, ImageLibrary &library, QObject *parent)
    : QObject(parent), m_data(data), m_presetStore(PresetStore::standardDirectory()), m_library(library),
      m_browser(FolderBrowser::Kind::Pictures, data)
{
    m_packs = std::make_unique<PackLibrary>(data.packs);
    m_engine = std::make_unique<RenderEngine>(data);
    m_engineContext = new QObject;
    m_engineContext->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_engineContext, &QObject::deleteLater);
    m_thread.setObjectName(QStringLiteral("render"));
    m_thread.start();

    const int saved = appSettings().value("previewSize").toInt();
    const auto &sizes = previewSizes();
    if (std::find(sizes.begin(), sizes.end(), saved) != sizes.end())
        m_previewSize = saved;
    m_maskLinesHidden = appSettings().value("hideMaskLines", false).toBool();
    m_presets = m_presetStore.load();
    m_history = std::make_unique<EditHistory<Look>>(
        this, [this] { return m_edits; }, [this](const Look &look) { restore(look); },
        [this](const Look &a, const Look &b) {
            const ProfilePack *film = tryPack(*m_packs, b.recipe.filmStock);
            const Vocabulary v = appSettings().value("vocabulary").toString() == QStringLiteral("Darkroom")
                ? Vocabulary::Darkroom : Vocabulary::Photographic;
            return lookChangeName(a, b, v, film && film->isMonochrome());
        },
        [this] { emit changed(); });
    m_history->joinSeconds = QApplication::doubleClickInterval() / 1000.0;

    m_browser.library = &m_library;
    m_browser.ownEdits = [this](const QString &path) { return ownEdits(path); };
    m_recordTimer.setSingleShot(true);
    m_recordTimer.setInterval(800);
    connect(&m_recordTimer, &QTimer::timeout, this, [this] { recordCurrent(); });
    // the print kept for the sidebar once the frames have stopped coming
    m_printTimer.setSingleShot(true);
    m_printTimer.setInterval(1500);
    connect(&m_printTimer, &QTimer::timeout, this, [this] { keepPrint(m_file); });
    m_busyTimer.setSingleShot(true);
    m_busyTimer.setInterval(300);
    connect(&m_busyTimer, &QTimer::timeout, this, [this] {
        if (m_pending.isEmpty() && !m_isDecoding)
            return;
        m_isBusy = true;
        if (!m_pending.isEmpty())
            m_status = QStringLiteral("Opening %1…").arg(QFileInfo(m_pending).fileName());
        emit changed();
    });
    m_renderSlowTimer.setSingleShot(true);
    m_renderSlowTimer.setInterval(300);
    connect(&m_renderSlowTimer, &QTimer::timeout, this, [this] {
        if (!m_renderInFlight)
            return;
        m_renderSlow = true;
        emit changed();
    });
    m_prefetchContext = new QObject;
    m_prefetchContext->moveToThread(&m_prefetchThread);
    connect(&m_prefetchThread, &QThread::finished, m_prefetchContext, &QObject::deleteLater);
    m_prefetchThread.setObjectName(QStringLiteral("prefetch"));
    m_prefetchThread.start(QThread::LowPriority);
}

Document::~Document()
{
    if (m_batchCancel)
        *m_batchCancel = true;
    if (m_batchThread)
        m_batchThread->wait();
    ++m_prefetchGeneration;
    QMetaObject::invokeMethod(m_prefetchContext, [this] { m_prefetchEngine.reset(); }, Qt::BlockingQueuedConnection);
    m_prefetchThread.quit();
    m_prefetchThread.wait();
    // the engine's GPU work is done on its thread, and its device goes there too
    QMetaObject::invokeMethod(m_engineContext, [this] { m_engine.reset(); }, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

template<typename F>
void Document::onEngine(F &&work)
{
    QMetaObject::invokeMethod(m_engineContext, std::forward<F>(work), Qt::QueuedConnection);
}

const std::vector<int> &Document::previewSizes()
{
    // At 2560, big enough to judge tone and colour, small enough to redraw
    // between a slider's ticks.
    static const std::vector<int> sizes{1024, 2048, 2560, 3072, 4096};
    return sizes;
}

void Document::setPreviewSize(int pixels, bool keep)
{
    if (pixels == m_previewSize)
        return;
    m_previewSize = pixels;
    if (keep)
        appSettings().setValue("previewSize", pixels);
    syncSource();
    emit changed();
}

const ProfilePack *Document::filmPack()
{
    return tryPack(*m_packs, m_edits.recipe.filmStock);
}

const ProfilePack *Document::paperPack()
{
    return tryPack(*m_packs, m_edits.recipe.printStock);
}

double Document::defaultGrade()
{
    const ProfilePack *film = filmPack();
    return film ? double(film->defaultGrade()) : double(kGradeReference);
}

void Document::clearError()
{
    m_error.clear();
    emit changed();
}

// MARK: - The edits

void Document::setRecipe(const Recipe &recipe)
{
    edited();
    scheduleRecord();
    const std::string oldFilm = m_edits.recipe.filmStock;
    m_edits.recipe = recipe;
    if (!m_applyingEdits) {
        // the film's reference light sets the decode's white balance, so
        // changing stock in that mode decodes again
        if (m_edits.whiteBalance == WhiteBalance::FilmReference && oldFilm != recipe.filmStock)
            syncSource();
        scheduleRender();
    }
    emit changed();
}

void Document::setWhiteBalance(WhiteBalance wb)
{
    if (wb == m_edits.whiteBalance)
        return;
    edited();
    scheduleRecord();
    m_edits.whiteBalance = wb;
    if (!m_applyingEdits)
        syncSource();
    emit changed();
}

void Document::setCustomTemperature(double kelvin)
{
    edited();
    scheduleRecord();
    m_edits.customTemperature = kelvin;
    if (!m_applyingEdits && m_edits.whiteBalance == WhiteBalance::Custom)
        syncSource();
    emit changed();
}

void Document::setCustomTint(double tint)
{
    edited();
    scheduleRecord();
    m_edits.customTint = tint;
    if (!m_applyingEdits && m_edits.whiteBalance == WhiteBalance::Custom)
        syncSource();
    emit changed();
}

// MARK: - Opening

void Document::open(const QString &given)
{
    const QString path = FolderBrowser::canonical(given);
    m_error.clear();
    showFolder(path);
    if (path == m_file) {
        // back to the image on screen: a pending open is forgotten
        m_pending.clear();
    } else {
        m_pending = path;
        // the sidebar marks it at once; the status line says so only if it
        // takes a while (isBusy)
        m_browser.setCurrent(path);
    }
    syncSource();
    updateBusy();
    emit changed();
}

// The edits an image opens with: its own if it was open before this session,
// otherwise the current look with a fresh start for the rest.
Look Document::edits(const QString &path)
{
    if (path == m_file)
        return m_edits;
    if (const auto it = m_editsByImage.find(path); it != m_editsByImage.end())
        return it->second;
    if (const auto kept = m_library.record(path); kept && kept->edits)
        return *kept->edits;
    Look fresh;
    fresh.recipe = carryingLook(m_edits.recipe, defaultGrade());
    return fresh;
}

DecodeKey Document::decodeKey(const QString &path)
{
    const Look e = edits(path);
    const ProfilePack *film = tryPack(*m_packs, e.recipe.filmStock);
    DecodeKey key;
    key.path = path;
    key.whiteBalance = decodeName(e.whiteBalance);
    key.reference = film && !film->referenceIlluminant.empty() ? film->referenceIlluminant : "D55";
    key.temperature = e.customTemperature;
    key.tint = e.customTint;
    key.maxDimension = m_previewSize;
    return key;
}

// Make the engine hold what the state asks for: the pending file if there is
// one, otherwise the image on screen, decoded with its white balance. Decodes
// only if that is neither loaded nor on its way already.
void Document::syncSource()
{
    const QString target = m_pending.isEmpty() ? m_file : m_pending;
    if (target.isEmpty())
        return;
    const DecodeKey key = decodeKey(target);
    if (m_requested && *m_requested == key)
        return;
    if (m_loaded && m_loaded->key == key) {
        // already there; anything still in flight is stale
        if (m_requested) {
            ++m_generation;
            m_requested.reset();
            m_isDecoding = false;
        }
        return;
    }
    m_requested = key;
    const int generation = ++m_generation;
    m_isDecoding = true;
    updateBusy();
    for (const auto &[ready, picture] : m_decoded)
        if (ready == key) {
            // decoded ahead of time, as a neighbour: no settle, no decode
            QTimer::singleShot(0, this, [this, picture = picture, key, generation] {
                finishDecode(picture, {}, key, generation);
            });
            return;
        }
    // A short settle, so dragging the temperature decodes once at the end
    // rather than once a tick: a decode is far longer than a slider's frame.
    QTimer::singleShot(120, this, [this, key, generation] {
        if (generation != m_generation)
            return;
        onEngine([this, key, generation] {
            if (generation != m_generation)
                return;                         // asked for something newer meanwhile
            std::shared_ptr<const DecodedPicture> picture;
            QString error;
            try {
                picture = std::make_shared<const DecodedPicture>(m_engine->decode(key));
            } catch (const std::exception &e) {
                error = QString::fromUtf8(e.what());
            }
            QMetaObject::invokeMethod(this, [this, picture, error, key, generation] {
                finishDecode(picture, error, key, generation);
            }, Qt::QueuedConnection);
        });
    });
}

void Document::finishDecode(std::shared_ptr<const DecodedPicture> picture, const QString &error, const DecodeKey &key,
                            int generation)
{
    // a decode a newer request has superseded is dropped
    if (generation != m_generation)
        return;
    if (!picture) {
        m_requested.reset();
        m_isDecoding = false;
        if (key.path == m_pending) {
            // a new file that won't open changes nothing
            m_pending.clear();
            m_browser.setCurrent(m_file);
            m_error = QStringLiteral("Could not open %1: %2").arg(QFileInfo(key.path).fileName(), error);
            m_status = statusLine();
        } else {
            m_error = error;
            m_status = QStringLiteral("Could not decode %1").arg(QFileInfo(key.path).fileName());
        }
        updateBusy();
        emit changed();
        return;
    }
    // kept, to step back to without decoding again
    std::erase_if(m_decoded, [&](const auto &d) { return d.first == key; });
    m_decoded.emplace_back(key, picture);
    onEngine([this, picture, key] {
        RenderEngine::Source source;
        QString error;
        try {
            DecodedPicture copy = *picture;
            source = m_engine->setPicture(std::move(copy), key.path);
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        QMetaObject::invokeMethod(this, [this, source, error, key] {
            if (!error.isEmpty()) {
                m_requested.reset();
                m_isDecoding = false;
                m_error = error;
                updateBusy();
                emit changed();
                return;
            }
            adopt(source, key);
        }, Qt::QueuedConnection);
    });
}

// The engine now holds the decode of `key`; the rest follows.
void Document::adopt(const RenderEngine::Source &source, const DecodeKey &key)
{
    if (m_requested && *m_requested == key) {
        m_requested.reset();
        m_isDecoding = false;
    }
    if (key.path != m_file) {
        // switching images: the outgoing one's edits put away, the incoming
        // one's brought out, all at once
        if (!m_file.isEmpty()) {
            recordCurrent();
            keepPrint(m_file);
        }
        EditHistory<Look>::Stacks steps;
        if (const auto it = m_historyByImage.find(key.path); it != m_historyByImage.end()) {
            steps = std::move(it->second);
            m_historyByImage.erase(it);
        }
        steps = m_history->swap(std::move(steps));
        if (!m_file.isEmpty()) {
            m_editsByImage[m_file] = m_edits;
            m_historyByImage[m_file] = std::move(steps);
            std::erase(m_visited, m_file);
            m_visited.push_back(m_file);
            if (int(m_visited.size()) > historiesKept) {
                const auto old = m_visited.end() - historiesKept;
                for (auto it = m_visited.begin(); it != old; ++it)
                    m_historyByImage.erase(*it);
                m_visited.erase(m_visited.begin(), old);
            }
        }
        const Look incoming = edits(key.path);
        if (!m_openedWith.count(key.path))
            m_openedWith[key.path] = incoming;
        m_applyingEdits = true;
        m_edits = incoming;
        m_isCropping = m_isLevelling = false;
        m_shapeBeforeCropping.reset();
        m_applyingEdits = false;
        m_file = key.path;
        m_browser.setCurrent(key.path);
        if (m_pending == key.path)
            m_pending.clear();
        // The previous image's readings stay on the sliders until this one's
        // first frame brings its own, a few ms later: blanked, the auto
        // sliders jumped twice at every picture stepped to.
        m_resolved.reset();
        m_displayMap.reset();
        // a mask selected on one image means nothing on the next
        m_selected.clear();
    }
    m_loaded = Loaded{key, source.id, source.width, source.height, source.camera,
                      source.focalLength, source.shutter, source.iso};
    m_status = statusLine();
    syncLoupe();
    // the incoming edits may want a decode of their own
    syncSource();
    scheduleRender();
    updateBusy();
    prefetchNeighbours();
    emit changed();
}

// The photograph as its camera took it: the camera, the focal length, the
// shutter speed and the ISO, as far as the file says (photoLine).
QString Document::statusLine() const
{
    if (!m_loaded)
        return QStringLiteral("Open a RAW file to begin");
    return photoLine(m_loaded->camera, m_loaded->focalLength, m_loaded->shutter, m_loaded->iso);
}

// MARK: - Stocks

void Document::selectFilm(const std::string &stock)
{
    if (stock != m_edits.recipe.filmStock)
        setRecipe(loadingFilm(m_edits.recipe, stock, *m_packs));
}

// MARK: - Copy and paste, presets

void Document::copySettings()
{
    if (!canCopySettings())
        return;
    m_copied.emplace(m_edits, QFileInfo(m_file).fileName());
    m_browser.setHasCopiedSettings(true);
    emit changed();
}

void Document::pasteSettings()
{
    if (canPasteSettings())
        m_history->named("Paste Settings", [this] { put(m_copied->first); });
}

// Copied or saved settings onto the image on screen: film, paper, print or
// light table, camera filter, every slider and auto, and white balance.
// Masks, gradients and the crop belong to the frame they were made for.
void Document::put(const Look &look)
{
    edited();
    Look pasted = look;
    pasted.recipe.gradients = m_edits.recipe.gradients;
    pasted.recipe.masks = m_edits.recipe.masks;
    pasted.recipe.gradients = m_edits.recipe.gradients;
    pasted.recipe.geometry = m_edits.recipe.geometry;
    pasted.recipe.shaped = m_edits.recipe.shaped;

    m_applyingEdits = true;
    m_edits = pasted;
    m_applyingEdits = false;
    scheduleRecord();
    // the white balance is decoded in, and a film-reference one follows the stock
    syncSource();
    scheduleRender();
    emit changed();
}

void Document::restore(const Look &look)
{
    m_applyingEdits = true;
    m_edits = look;
    m_applyingEdits = false;
    scheduleRecord();
    syncSource();
    scheduleRender();
    emit changed();
}

void Document::reloadPresets()
{
    m_presets = m_presetStore.load();
    emit changed();
}

void Document::savePreset(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (!canCopySettings() || trimmed.isEmpty())
        return;
    try {
        m_presetStore.save({trimmed, m_edits});
    } catch (const std::exception &e) {
        m_error = QStringLiteral("Could not save the preset %1: %2").arg(trimmed, QString::fromUtf8(e.what()));
    }
    reloadPresets();
}

void Document::applyPreset(const Preset &preset)
{
    if (!canCopySettings())
        return;
    const auto &films = m_packs->filmEntries();
    const auto &papers = m_packs->paperEntries();
    const bool hasFilm = std::any_of(films.begin(), films.end(),
                                     [&](const PackIndexEntry &e) { return e.stock == preset.look.recipe.filmStock; });
    const bool hasPaper = std::any_of(papers.begin(), papers.end(), [&](const PackIndexEntry &e) {
        return e.stock == preset.look.recipe.printStock;
    });
    if (!hasFilm || !hasPaper) {
        m_error = QStringLiteral("The preset %1 uses a film or paper this version does not have.").arg(preset.name);
        emit changed();
        return;
    }
    m_history->named("Apply Preset", [&] { put(preset.look); });
}

void Document::deletePreset(const QString &name)
{
    try {
        m_presetStore.remove(name);
    } catch (const std::exception &e) {
        m_error = QStringLiteral("Could not delete the preset %1: %2").arg(name, QString::fromUtf8(e.what()));
    }
    reloadPresets();
}

QString Document::presetsFolder()
{
    return m_presetStore.ensuredDirectory();
}

// MARK: - White balance

QString Document::whiteBalanceSummary()
{
    if (m_isBusy && m_isDecoding)
        return QStringLiteral("decoding…");
    switch (m_edits.whiteBalance) {
    case WhiteBalance::FilmReference: {
        const ProfilePack *film = filmPack();
        const std::string name = film && !film->referenceIlluminant.empty() ? film->referenceIlluminant : "D55";
        // a blackbody lamp, "BB3200", by what it is
        const QString shown = name.rfind("BB", 0) == 0 ? QStringLiteral("Tungsten") : QString::fromStdString(name);
        return QStringLiteral("%1 · %2 K").arg(shown).arg(filmReferenceTemperature(), 0, 'f', 0);
    }
    case WhiteBalance::AsShot: return QStringLiteral("camera as-shot");
    case WhiteBalance::DaylightD55: return QStringLiteral("D55 · 5503 K");
    case WhiteBalance::Daylight: return QStringLiteral("D65 · 6504 K");
    case WhiteBalance::Tungsten3200: return QStringLiteral("3200 K");
    case WhiteBalance::Tungsten: return QStringLiteral("2850 K");
    case WhiteBalance::Custom:
        return QStringLiteral("%1 K · tint %2%3")
            .arg(m_edits.customTemperature, 0, 'f', 0)
            .arg(m_edits.customTint >= 0 ? "+" : "")
            .arg(m_edits.customTint, 0, 'f', 0);
    }
    return {};
}

double Document::filmReferenceTemperature()
{
    const ProfilePack *film = filmPack();
    return referenceTemperature(film && !film->referenceIlluminant.empty() ? film->referenceIlluminant : "D55");
}

// Read as a transparency rather than printed; an instant print is the print,
// seen as it is.
bool Document::isTransparency()
{
    const ProfilePack *film = filmPack();
    return film ? film->isViewedDirectly(m_edits.recipe.scanFilm) : m_edits.recipe.scanFilm.value_or(false);
}

bool Document::printsOnPaper()
{
    return !isTransparency();
}

// A black-and-white paper prints in silver: whatever the filtration, the
// picture comes out neutral, so the colour controls have nothing to act on.
bool Document::printsInSilver()
{
    const ProfilePack *paper = paperPack();
    return printsOnPaper() && paper && paper->isMonochrome();
}

// MARK: - Rendering

// Ask for a frame. While one is being made, further requests collapse into a
// single follow-up with whatever the recipe is by then, so a fast drag
// renders its latest position rather than queueing every tick.
void Document::scheduleRender()
{
    if (!m_loaded)
        return;
    if (m_renderInFlight) {
        m_renderAgain = true;
        return;
    }
    m_renderInFlight = true;
    if (!m_renderSlowTimer.isActive())
        m_renderSlowTimer.start();
    const Recipe recipe = m_edits.recipe;
    const int sourceID = m_loaded->sourceID;
    const bool wholeFrame = m_isCropping;
    const std::optional<FrameGeometry> metering = m_shapeBeforeCropping;
    onEngine([this, recipe, sourceID, wholeFrame, metering] {
        std::optional<RenderEngine::Output> output;
        QString error;
        try {
            output = m_engine->render(recipe, sourceID, wholeFrame, metering);
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        QMetaObject::invokeMethod(this, [this, output = std::move(output), error, sourceID]() mutable {
            finishRender(std::move(output), error, sourceID);
        }, Qt::QueuedConnection);
    });
}

void Document::finishRender(std::optional<RenderEngine::Output> output, const QString &error, int sourceID)
{
    m_renderInFlight = false;
    if (!m_renderAgain && !(m_loaded && sourceID != m_loaded->sourceID)) {
        m_renderSlowTimer.stop();
        m_renderSlow = false;
    }
    // a frame for an image since replaced is dropped; the follow-up draws the new one
    if (output && m_loaded && sourceID == m_loaded->sourceID) {
        m_display = std::move(output->image);
        m_shown = std::move(output->shown);
        m_histogram = std::move(output->histogram);
        m_resolved = std::move(output->resolved);
        m_metered = output->metered;
        m_lastMilliseconds = output->milliseconds;
        m_displayMap = output->map;
        m_lastBreakdown = output->breakdown;
        if (m_showOriginal) {
            // never another picture's original over this one's print
            if (m_originalFrame && m_originalFrame->path != m_loaded->key.path) {
                m_originalFrame.reset();
                m_original = {};
                m_originalShown = {};
                m_originalMap.reset();
            }
            requestOriginal();
        }
        emit imageChanged();
        requestLoupe();
        m_printTimer.start();
    }
    if (!error.isEmpty()) {
        m_error = error;
        m_metered = {};
    }
    if (m_renderAgain || (m_loaded && sourceID != m_loaded->sourceID)) {
        m_renderAgain = false;
        scheduleRender();
    }
    emit changed();
}

void Document::screenChanged()
{
    scheduleRender();
    requestLoupe();
    if (!m_original.isNull())
        m_originalShown = displaycolour::toScreen(m_original);
}

// MARK: - Original

void Document::setShowOriginal(bool on)
{
    if (on == m_showOriginal)
        return;
    m_showOriginal = on;
    if (on) {
        requestOriginal();
    } else {
        ++m_originalGeneration;
        m_originalDecoding.clear();
        m_original = {};
        m_originalShown = {};
        m_originalMap.reset();
    }
    emit imageChanged();
    emit changed();
}

// The picture on screen's original: decoded once, with the camera's white
// balance at the preview's size, then placed on the print as it stands.
void Document::requestOriginal()
{
    if (!m_showOriginal || !m_loaded)
        return;
    const QString path = m_loaded->key.path;
    if (m_originalFrame && m_originalFrame->path == path && m_originalFrame->previewSize == m_previewSize) {
        placeOriginal();
        return;
    }
    if (m_originalDecoding == path)
        return;
    m_originalDecoding = path;
    DecodeKey key = m_loaded->key;
    key.whiteBalance = "as-shot";
    key.maxDimension = m_previewSize;
    const int generation = ++m_originalGeneration;
    const int size = m_previewSize;
    onEngine([this, key, generation, size] {
        QImage image;
        try {
            const DecodedPicture picture = m_engine->decode(key);
            const Dump &pixels = picture.pixels;
            image = QImage(pixels.width, pixels.height, QImage::Format_RGBA8888);
            const bool raw = picture.mosaic.width > 0;
            applerendering::toSRGB8(pixels.floats.data(), size_t(pixels.width) * pixels.height,
                                    picture.baselineExposure, picture.decodedExposure, picture.dng, image.bits(), raw);
            image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        } catch (const std::exception &) {
            image = {};
        }
        QMetaObject::invokeMethod(this, [this, image = std::move(image), key, generation, size]() mutable {
            if (generation != m_originalGeneration)
                return;
            m_originalDecoding.clear();
            if (image.isNull() || !m_loaded || m_loaded->key.path != key.path)
                return;
            m_originalFrame = OriginalFrame{key.path, size, std::move(image)};
            placeOriginal();
        }, Qt::QueuedConnection);
    });
}

namespace {
bool sameShape(const GeometryMap &a, const GeometryMap &b)
{
    const Affine &s = a.sourceToOutput, &t = b.sourceToOutput;
    return a.sourceWidth == b.sourceWidth && a.sourceHeight == b.sourceHeight && a.crop.x == b.crop.x
        && a.crop.y == b.crop.y && a.crop.w == b.crop.w && a.crop.h == b.crop.h && s.a == t.a && s.b == t.b
        && s.c == t.c && s.d == t.d && s.tx == t.tx && s.ty == t.ty;
}
} // namespace

// The original turned, levelled and cropped as the print is: its pixels run
// through the print's own map, bilinear, so switching between the two moves
// nothing. The frame's empty corners, where levelling leaves them, are empty.
void Document::placeOriginal()
{
    if (!m_showOriginal || !m_originalFrame || !m_displayMap || !m_loaded
        || m_originalFrame->path != m_loaded->key.path)
        return;
    const GeometryMap &map = *m_displayMap;
    if (m_originalMap && sameShape(*m_originalMap, map) && !m_original.isNull())
        return;
    const QImage &frame = m_originalFrame->image;
    QImage placed(map.outputWidth(), map.outputHeight(), QImage::Format_RGBA8888_Premultiplied);
    placed.fill(Qt::transparent);
    {
        QPainter p(&placed);
        // smooth corners where levelling turns the frame
        p.setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing);
        const Affine &m = map.sourceToOutput;
        // the decode the print was made from, should the original's differ by a pixel
        const QTransform fit = QTransform::fromScale(map.sourceWidth / std::max(frame.width(), 1),
                                                     map.sourceHeight / std::max(frame.height(), 1));
        p.setTransform(fit * QTransform(m.a, m.b, m.c, m.d, m.tx, m.ty));
        p.drawImage(QPointF(0, 0), frame);
    }
    // no empty corners where the print has none: the canvas draws the same
    // contact shadow behind either
    if (!m_display.hasAlphaChannel())
        placed = placed.convertToFormat(QImage::Format_RGBX8888);
    placed.setColorSpace(QColorSpace(QColorSpace::SRgb));
    m_original = std::move(placed);
    m_originalShown = displaycolour::toScreen(m_original);
    m_originalMap = map;
    emit imageChanged();
}

// MARK: - Export

void Document::exportTo(const QString &path, ExportFormat format)
{
    if (!canExport()) {
        m_error = QStringLiteral("Wait for the image to finish loading, then export.");
        emit changed();
        return;
    }
    m_isExporting = true;
    m_status = QStringLiteral("Exporting at full resolution…");
    const Recipe recipe = m_edits.recipe;
    const DecodeKey key = m_loaded->key;
    onEngine([this, recipe, key, path, format] {
        QString error;
        try {
            m_engine->exportTo(recipe, key, path, format);
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        QMetaObject::invokeMethod(this, [this, error, path] {
            m_isExporting = false;
            if (error.isEmpty()) {
                m_status = QStringLiteral("Exported %1").arg(QFileInfo(path).fileName());
            } else {
                m_error = error;
                m_status = statusLine();
            }
            emit changed();
        }, Qt::QueuedConnection);
    });
    emit changed();
}

// MARK: - Crop, turn and level

void Document::setCropping(bool on)
{
    if (on == m_isCropping || (on && !m_loaded))
        return;
    m_isCropping = on;
    m_shapeBeforeCropping = on ? std::optional<FrameGeometry>(m_edits.recipe.geometry) : std::nullopt;
    if (!on)
        QTimer::singleShot(0, this, [this] { requestLoupe(); });
    m_historyBeforeCropping = on ? std::optional(m_history->mark()) : std::nullopt;
    if (!on)
        m_isLevelling = false;
    if (!m_applyingEdits)
        scheduleRender();
    emit changed();
}

void Document::setLevelling(bool on)
{
    m_isLevelling = on && m_loaded;
    if (m_isLevelling)
        setCropping(true);
    emit changed();
}

// A cancelled crop never happened: the steps it took come off the undo
// history, so the next Ctrl+Z undoes what came before it. When something
// other than the shape was changed meanwhile, going back is a step of its own.
void Document::endCropping(bool keep)
{
    if (!keep && m_shapeBeforeCropping) {
        Recipe r = m_edits.recipe;
        r.geometry = *m_shapeBeforeCropping;
        r.shaped = !r.geometry.isIdentity();
        const auto onlyShape = [](const Look &a, const Look &b) {
            Look c = a;
            c.recipe.geometry = b.recipe.geometry;
            c.recipe.shaped = b.recipe.shaped;
            return c == b;
        };
        const auto goBack = [&] {
            m_applyingEdits = true;
            setRecipe(r);
            m_applyingEdits = false;
        };
        if (m_historyBeforeCropping && m_history->rewind(*m_historyBeforeCropping, onlyShape))
            m_history->unrecorded(goBack);
        else
            m_history->named("Cancel Crop", goBack);
    }
    setCropping(false);
}

std::optional<PointD> Document::turnedFrame() const
{
    if (!m_loaded)
        return std::nullopt;
    return m_edits.recipe.geometry.turnedSize(m_loaded->width, m_loaded->height);
}

void Document::editGeometry(const std::function<void(FrameGeometry &, PointD)> &change)
{
    const auto frame = turnedFrame();
    if (!frame)
        return;
    Recipe r = m_edits.recipe;
    change(r.geometry, *frame);
    r.shaped = !r.geometry.isIdentity();
    setRecipe(r);
}

void Document::turn(bool clockwise)
{
    editGeometry([&](FrameGeometry &g, PointD) { g.turn(clockwise); });
}

void Document::flip(bool horizontal)
{
    editGeometry([&](FrameGeometry &g, PointD) { g.flip(horizontal); });
}

void Document::setCropAspect(CropAspect aspect)
{
    editGeometry([&](FrameGeometry &g, PointD frame) { g.setAspect(aspect, frame); });
}

void Document::swapCropOrientation()
{
    editGeometry([](FrameGeometry &g, PointD frame) { g.swapOrientation(frame); });
}

void Document::setCustomAspect(double width, double height)
{
    if (!(width > 0 && height > 0))
        return;
    editGeometry([&](FrameGeometry &g, PointD frame) {
        g.customWidth = width;
        g.customHeight = height;
        g.setAspect(CropAspect::Custom, frame);
    });
}

void Document::setStraighten(double degrees)
{
    editGeometry([&](FrameGeometry &g, PointD) { g.setStraighten(degrees); });
}

void Document::level(PointD a, PointD b)
{
    editGeometry([&](FrameGeometry &g, PointD frame) {
        const double dx = (b.x - a.x) * frame.x, dy = (b.y - a.y) * frame.y;
        if (dx * dx + dy * dy <= 4)
            return;
        const double angle = std::atan2(dy, dx) * 180 / M_PI;
        g.setStraighten(g.straighten - (angle - 90 * std::round(angle / 90)));
    });
}

void Document::resetGeometry()
{
    m_history->named("Reset Crop", [&] {
        Recipe r = m_edits.recipe;
        r.geometry = {};
        r.shaped = false;
        setRecipe(r);
    });
}

// MARK: - Graduated exposure, dodge & burn

void Document::setSelected(const std::string &id)
{
    if (id == m_selected)
        return;
    m_selected = id;
    if (!id.empty() && m_maskLinesHidden) {
        m_maskLinesHidden = false;
        appSettings().setValue("hideMaskLines", false);
    }
    emit changed();
}

void Document::setMaskLinesHidden(bool hidden)
{
    if (hidden == m_maskLinesHidden)
        return;
    m_maskLinesHidden = hidden;
    appSettings().setValue("hideMaskLines", hidden);
    if (hidden)
        m_selected.clear();
    emit changed();
}

std::vector<PointD> Document::framePoints(const std::vector<PointD> &points) const
{
    if (!m_displayMap || m_displayMap->isIdentity)
        return points;
    std::vector<PointD> out;
    for (const PointD &p : points)
        out.push_back(m_displayMap->normalisedSource(
            {p.x * m_displayMap->outputWidth(), p.y * m_displayMap->outputHeight()}));
    return out;
}

// A new gradient, or oval, laid on the part of the frame in view.
void Document::addGradient(bool oval)
{
    Recipe r = m_edits.recipe;
    if (int(r.gradients.size()) >= kMaxExposureGradients)
        return;
    ExposureGradient g;
    g.id = newIdentifier();
    g.name = QStringLiteral("%1 %2").arg(oval ? "Oval" : "Gradient").arg(r.gradients.size() + 1).toStdString();
    std::vector<PointD> points = {{g.ax, g.ay}, {g.bx, g.by}};
    if (oval) {
        g.shape = GradientShape::Oval;
        g.points = 3;
        points = {{0.5, 0.5}, {0.75, 0.5}, {0.5, 0.75}};
    }
    points = framePoints(points);
    g.ax = points[0].x;
    g.ay = points[0].y;
    g.bx = points[1].x;
    g.by = points[1].y;
    if (oval) {
        g.cx = points[2].x;
        g.cy = points[2].y;
    }
    r.gradients.push_back(g);
    m_selected = g.id;
    if (m_maskLinesHidden) {
        m_maskLinesHidden = false;
        appSettings().setValue("hideMaskLines", false);
    }
    setRecipe(r);
}

void Document::addMask(MaskShape shape)
{
    Recipe r = m_edits.recipe;
    LocalMask m;
    m.id = newIdentifier();
    m.shape = shape;
    const char *names[] = {"Polygon", "Oval", "Gradient"};
    m.name = QStringLiteral("%1 %2").arg(QLatin1StringView(names[int(shape)])).arg(r.masks.size() + 1).toStdString();
    switch (shape) {
    case MaskShape::Polygon: m.points = {{0.35, 0.35}, {0.65, 0.35}, {0.65, 0.65}, {0.35, 0.65}}; break;
    case MaskShape::Oval: m.points = {{0.5, 0.5}, {0.72, 0.5}, {0.5, 0.72}}; break;
    case MaskShape::Gradient: m.points = {{0.5, 0.1}, {0.5, 0.5}}; break;
    }
    m.points = framePoints(m.points);
    r.masks.push_back(m);
    m_selected = m.id;
    if (m_maskLinesHidden) {
        m_maskLinesHidden = false;
        appSettings().setValue("hideMaskLines", false);
    }
    setRecipe(r);
}

void Document::removeGradient(const std::string &id)
{
    Recipe r = m_edits.recipe;
    std::erase_if(r.gradients, [&](const ExposureGradient &g) { return g.id == id; });
    if (m_selected == id)
        m_selected.clear();
    setRecipe(r);
}

void Document::removeMask(const std::string &id)
{
    Recipe r = m_edits.recipe;
    std::erase_if(r.masks, [&](const LocalMask &m) { return m.id == id; });
    if (m_selected == id)
        m_selected.clear();
    setRecipe(r);
}

// MARK: - Resets

// The Negative Exposure section: exposure, its meter, and white balance.
void Document::resetExposure()
{
    m_history->named("Reset Exposure", [&] {
        edited();   // the white balance changes before the recipe does
        Recipe r = m_edits.recipe;
        const Recipe defaults;
        r.exposureEV = defaults.exposureEV;
        r.autoExposure = defaults.autoExposure;
        m_applyingEdits = true;
        m_edits.whiteBalance = WhiteBalance::FilmReference;
        m_edits.customTemperature = 5500;
        m_edits.customTint = 0;
        m_applyingEdits = false;
        syncSource();
        setRecipe(r);
    });
}

void Document::resetTone()
{
    m_history->named("Reset Tone", [&] {
        Recipe r = m_edits.recipe;
        const Recipe d;
        r.autoDensity = d.autoDensity;
        r.autoGrade = d.autoGrade;
        r.brightness = d.brightness;
        r.density = d.density;
        r.grade = defaultGrade();
        r.shadowGrade = d.shadowGrade;
        r.highlightGrade = d.highlightGrade;
        r.shadows = d.shadows;
        r.highlights = d.highlights;
        r.toe = d.toe;
        r.shoulder = d.shoulder;
        r.toeWidth = d.toeWidth;
        r.shoulderWidth = d.shoulderWidth;
        r.snap = d.snap;
        r.gradeTrim = {};
        r.toeTrim = {};
        r.shoulderTrim = {};
        r.snapTrim = {};
        r.shadowCMY = {};
        r.highlightCMY = {};
        r.curves.reset();
        setRecipe(r);
    });
}

// Everything on the Colour page: filtration, regional casts, pre-flash.
void Document::resetColour()
{
    m_history->named("Reset Colour", [&] {
        Recipe r = m_edits.recipe;
        const Recipe d;
        r.filterShiftY = d.filterShiftY;
        r.filterShiftM = d.filterShiftM;
        r.shadowCMY = {};
        r.highlightCMY = {};
        r.preflashExposure = d.preflashExposure;
        r.preflashY = d.preflashY;
        r.preflashM = d.preflashM;
        setRecipe(r);
    });
}

// Couplers, halation, grain, glare, camera sharpness and development gamma,
// back to the stock's own measured defaults.
void Document::resetFilmCharacter()
{
    m_history->named("Reset Film Character", [&] {
        Recipe r = m_edits.recipe;
        const Recipe d;
        r.couplersEnabled = d.couplersEnabled;
        r.couplersAmount = d.couplersAmount;
        r.couplersDiffusion = d.couplersDiffusion;
        r.halationEnabled = d.halationEnabled;
        r.halationAmount = d.halationAmount;
        r.halationScale = d.halationScale;
        r.scatterAmount = d.scatterAmount;
        r.scatterScale = d.scatterScale;
        r.cameraSharpness = d.cameraSharpness;
        r.grainEnabled = d.grainEnabled;
        r.grainAmount = d.grainAmount;
        r.grainScale = d.grainScale;
        r.glareEnabled = d.glareEnabled;
        r.glareAmount = d.glareAmount;
        r.densityCurveGamma = d.densityCurveGamma;
        setRecipe(r);
    });
}

// MARK: - The loupe

void Document::setLoupeOn(bool on)
{
    if (on == m_loupeOn || (on && !m_loaded))
        return;
    m_loupeOn = on;
    if (on)
        syncLoupe();
    else
        closeLoupe();
    emit loupeChanged();
    emit changed();
}

void Document::moveLoupe(QPointF centre)
{
    m_loupeCentre = QPointF(std::clamp(centre.x(), 0.0, 1.0), std::clamp(centre.y(), 0.0, 1.0));
    emit loupeChanged();
    requestLoupe();
}

void Document::setLoupeSize(QSize pixels)
{
    if (pixels == m_loupeSize)
        return;
    m_loupeSize = pixels;
    requestLoupe();
}

void Document::syncLoupe()
{
    if (!m_loupeOn || !m_loaded)
        return;
    DecodeKey wanted = m_loaded->key;
    wanted.maxDimension = 0;
    if (m_fullFrameKey && *m_fullFrameKey == wanted) {
        requestLoupe();
        return;
    }
    m_fullFrameKey = wanted;
    m_loupeLoading = true;
    m_loupeImage = {};
    const int generation = ++m_loupeGeneration;
    emit loupeChanged();
    onEngine([this, wanted, generation] {
        QString error;
        try {
            DecodedPicture full = m_engine->decode(wanted);
            m_engine->holdFullFrame(std::move(full), wanted.path);
        } catch (const std::bad_alloc &) {
            error = QStringLiteral("There is not enough memory to hold the picture at full size for the loupe.");
        } catch (const std::exception &e) {
            error = QStringLiteral("Could not read the picture at full size: %1").arg(QString::fromUtf8(e.what()));
        }
        QMetaObject::invokeMethod(this, [this, generation, error] {
            if (generation != m_loupeGeneration)
                return;
            m_loupeLoading = false;
            if (!error.isEmpty()) {
                m_error = error;
                m_loupeOn = false;
                closeLoupe();
            } else {
                requestLoupe();
            }
            emit loupeChanged();
            emit changed();
        }, Qt::QueuedConnection);
    });
}

void Document::closeLoupe()
{
    ++m_loupeGeneration;
    m_fullFrameKey.reset();
    m_loupeImage = {};
    m_loupeLoading = false;
    m_loupeAgain = false;
    onEngine([this] { m_engine->releaseFullFrame(); });
}

// One at a time: while one is printed, further requests collapse into one
// follow-up with whatever is wanted by then, so a drag shows where the loupe
// is, not every place it has been.
void Document::requestLoupe()
{
    if (!m_loupeOn || m_loupeLoading || m_isCropping || !m_fullFrameKey || m_loupeSize.isEmpty() || !m_loaded)
        return;
    if (m_loupeInFlight) {
        m_loupeAgain = true;
        return;
    }
    m_loupeInFlight = true;
    const Recipe recipe = m_edits.recipe;
    const QPointF centre = m_loupeCentre;
    const QSize size = m_loupeSize;
    const int generation = m_loupeGeneration;
    onEngine([this, recipe, centre, size, generation] {
        QImage image;
        QString error;
        try {
            image = m_engine->renderLoupe(recipe, centre, size.width(), size.height());
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        QMetaObject::invokeMethod(this, [this, image = std::move(image), error, generation]() mutable {
            m_loupeInFlight = false;
            if (generation == m_loupeGeneration && m_loupeOn) {
                if (!image.isNull())
                    m_loupeImage = std::move(image);
                if (!error.isEmpty())
                    m_error = error;
                emit loupeChanged();
            }
            if (m_loupeAgain) {
                m_loupeAgain = false;
                requestLoupe();
            }
        }, Qt::QueuedConnection);
    });
}


// MARK: - The folder, flags and batches

void Document::openFolder(const QString &given)
{
    const QString folder = FolderBrowser::canonical(given);
    m_error.clear();
    list(folder);
    const std::vector<FolderBrowser::Item> shown = m_browser.shown();
    const QString first = !shown.empty()                  ? shown.front().path
                          : !m_browser.items().empty()    ? m_browser.items().front().path
                                                          : QString();
    if (first.isEmpty()) {
        m_status = QStringLiteral("No pictures in %1").arg(QFileInfo(folder).fileName());
        emit changed();
        return;
    }
    const auto &items = m_browser.items();
    if (!m_file.isEmpty()
        && std::any_of(items.begin(), items.end(), [&](const FolderBrowser::Item &i) { return i.path == m_file; })) {
        m_browser.setSelection({m_file});
        emit changed();
        return;
    }
    m_browser.setSelection({first});
    open(first);
}

// The sidebar lists the folder of the picture opened, with it selected.
void Document::showFolder(const QString &path)
{
    const QString folder = FolderBrowser::canonical(QFileInfo(path).absolutePath());
    if (folder != m_browser.folder())
        list(folder);
    if (!m_browser.selection().count(path))
        m_browser.setSelection({path});
}

void Document::list(const QString &folder)
{
    std::vector<FolderBrowser::Item> items = FolderBrowser::listing(folder, FolderBrowser::Kind::Pictures);
    std::vector<std::pair<QString, qint64>> files;
    files.reserve(items.size());
    for (const FolderBrowser::Item &item : items)
        files.emplace_back(item.path, item.size);
    const auto records = m_library.load(folder, files);
    m_browser.show(folder, std::move(items), records);
    prefetchNeighbours();
}

std::optional<Look> Document::ownEdits(const QString &path)
{
    if (!m_browser.edited().count(path))
        return std::nullopt;
    if (path == m_file)
        return m_edits;
    if (const auto it = m_editsByImage.find(path); it != m_editsByImage.end())
        return it->second;
    if (const auto kept = m_library.record(path))
        return kept->edits;
    return std::nullopt;
}

void Document::scheduleRecord()
{
    m_recordTimer.start();
}

void Document::recordCurrent()
{
    m_recordTimer.stop();
    if (m_file.isEmpty())
        return;
    const Look now = m_edits;
    if (const auto kept = m_library.record(m_file); kept && kept->edits) {
        if (*kept->edits == now)
            return;
    } else {
        const auto opened = m_openedWith.find(m_file);
        if (opened == m_openedWith.end() || opened->second == now)
            return;
    }
    m_library.update(m_file, [&](ImageLibrary::Record &r) { r.edits = now; });
    m_browser.markEdited(m_file);
}

void Document::keepPrint(const QString &path)
{
    if (path.isEmpty() || path != m_file || !m_browser.edited().count(path) || m_isCropping || m_renderInFlight
        || m_display.isNull())
        return;
    m_browser.thumbnails().setPrint(m_display, path, m_edits);
}

void Document::prefetchNeighbours()
{
    const int generation = ++m_prefetchGeneration;
    if (m_file.isEmpty())
        return;
    std::vector<DecodeKey> wanted;
    for (int offset : {1, -1})
        if (const auto next = m_browser.neighbour(m_file, offset); next && *next != m_file)
            wanted.push_back(decodeKey(*next));
    std::vector<DecodeKey> keep = wanted;
    if (m_loaded)
        keep.push_back(m_loaded->key);
    if (m_requested)
        keep.push_back(*m_requested);
    if (lowOnMemory()) {
        // short of memory: the picture on screen alone, and fewer pictures in the sidebar
        keep = m_loaded ? std::vector<DecodeKey>{m_loaded->key} : std::vector<DecodeKey>{};
        wanted.clear();
        m_browser.thumbnails().trim();
        // and the original kept for showing again, unless it shows
        if (!m_showOriginal)
            m_originalFrame.reset();
    }
    std::erase_if(m_decoded, [&](const auto &d) { return std::find(keep.begin(), keep.end(), d.first) == keep.end(); });
    std::vector<DecodeKey> missing;
    for (const DecodeKey &key : wanted)
        if (std::none_of(m_decoded.begin(), m_decoded.end(), [&](const auto &d) { return d.first == key; }))
            missing.push_back(key);
    if (missing.empty())
        return;
    QTimer::singleShot(150, this, [this, missing, generation] { prefetchNext(missing, generation); });
}

// One at a time, on the prefetch engine, once the picture on screen has had
// its first frame; anything asked for meanwhile drops the rest.
void Document::prefetchNext(std::vector<DecodeKey> keys, int generation)
{
    if (generation != m_prefetchGeneration || keys.empty())
        return;
    if (m_isDecoding || m_renderInFlight || !m_pending.isEmpty()) {
        QTimer::singleShot(150, this, [this, keys, generation] { prefetchNext(keys, generation); });
        return;
    }
    const DecodeKey key = keys.front();
    keys.erase(keys.begin());
    const AppData data = m_data;
    QMetaObject::invokeMethod(m_prefetchContext, [this, key, keys, generation, data] {
        if (generation != m_prefetchGeneration)
            return;
        if (!m_prefetchEngine)
            m_prefetchEngine = std::make_unique<RenderEngine>(data);
        std::shared_ptr<const DecodedPicture> picture;
        try {
            picture = std::make_shared<const DecodedPicture>(m_prefetchEngine->decode(key));
        } catch (const std::exception &) {
        }
        // the engine let go once nothing has been decoded ahead for a while
        QTimer::singleShot(30000, m_prefetchContext, [this, generation] {
            if (generation == m_prefetchGeneration)
                m_prefetchEngine.reset();
        });
        QMetaObject::invokeMethod(this, [this, key, keys, generation, picture] {
            if (generation != m_prefetchGeneration)
                return;
            if (picture)
                m_decoded.emplace_back(key, picture);
            QTimer::singleShot(150, this, [this, keys, generation] { prefetchNext(keys, generation); });
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

std::set<QString> Document::prefetchedPictures() const
{
    std::set<QString> out;
    for (const auto &[key, picture] : m_decoded)
        out.insert(key.path);
    return out;
}

void Document::updateBusy()
{
    if (m_pending.isEmpty() && !m_isDecoding) {
        m_busyTimer.stop();
        m_isBusy = false;
        if (m_status.startsWith(QStringLiteral("Opening ")))
            m_status = statusLine();
        return;
    }
    if (!m_isBusy && !m_busyTimer.isActive())
        m_busyTimer.start();
}

void Document::flagSelection(std::optional<ImageLibrary::Flag> flag)
{
    std::vector<QString> targets;
    for (const FolderBrowser::Item &item : m_browser.items())
        if (m_browser.selection().count(item.path))
            targets.push_back(item.path);
    if (targets.empty() && !m_file.isEmpty())
        targets = {m_file};
    flagPaths(targets, flag);
}

void Document::flagPaths(const std::vector<QString> &paths, std::optional<ImageLibrary::Flag> flag)
{
    for (const QString &path : paths) {
        m_library.update(path, [&](ImageLibrary::Record &r) { r.flag = flag; });
        m_browser.setFlag(flag, path);
    }
}

void Document::showNeighbour(int offset)
{
    const std::vector<FolderBrowser::Item> shown = m_browser.shown();
    const QString from = !m_file.isEmpty() ? m_file : !shown.empty() ? shown.front().path : QString();
    if (from.isEmpty())
        return;
    const auto next = m_browser.neighbour(from, offset);
    if (!next || *next == m_file)
        return;
    m_browser.setSelection({*next});
    open(*next);
}

void Document::pasteSettingsToSelected()
{
    if (!m_copied)
        return;
    const Look copied = m_copied->first;
    std::vector<std::pair<QString, Look>> pastedOn;
    for (const QString &path : m_browser.selectedForBatch()) {
        if (path == m_file) {
            pasteSettings();
            continue;
        }
        const Look before = edits(path);
        Look pasted = copied;
        pasted.recipe.masks = before.recipe.masks;
        pasted.recipe.gradients = before.recipe.gradients;
        pasted.recipe.geometry = before.recipe.geometry;
        pasted.recipe.shaped = before.recipe.shaped;
        if (pasted == before)
            continue;
        m_editsByImage[path] = pasted;
        m_historyByImage[path] = EditHistory<Look>::adding("Paste Settings", before, pasted, m_historyByImage[path]);
        if (!m_openedWith.count(path))
            m_openedWith[path] = before;
        m_library.update(path, [&](ImageLibrary::Record &r) { r.edits = pasted; });
        m_browser.markEdited(path);
        pastedOn.emplace_back(path, pasted);
    }
    m_browser.thumbnails().refresh(pastedOn);
    prefetchNeighbours();
    emit changed();
}

void Document::exportSelected(const QString &folder, ExportFormat format)
{
    if (m_browser.batch())
        return;
    recordCurrent();
    const std::vector<QString> paths = m_browser.selectedForBatch();
    if (paths.empty())
        return;
    // Each is metered on its own decode at the preview's size, so it prints
    // as it would on screen. The names are the picture's and the film's, as
    // Export's; a name already taken gets a number.
    struct Job {
        QString path, target;
        Look edits;
        DecodeKey proxy;
    };
    std::vector<Job> jobs;
    std::set<QString> taken;
    const QString extension = formatExtension(format);
    for (const QString &path : paths) {
        const Look e = edits(path);
        const ProfilePack *film = tryPack(*m_packs, e.recipe.filmStock);
        QString filmName = film ? QString::fromStdString(film->name) : QStringLiteral("film");
        filmName.remove(QLatin1Char(' '));
        const QString stem = QFileInfo(path).completeBaseName() + QLatin1Char('_') + filmName;
        QString name = stem + QLatin1Char('.') + extension;
        for (int n = 2; taken.count(name.toLower()) || QFileInfo::exists(QDir(folder).filePath(name)); ++n)
            name = QStringLiteral("%1-%2.%3").arg(stem).arg(n).arg(extension);
        taken.insert(name.toLower());
        jobs.push_back({path, QDir(folder).filePath(name), e, decodeKey(path)});
    }
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_batchCancel = cancel;
    m_browser.setBatch(FolderBrowser::Batch{0, int(jobs.size()), QFileInfo(jobs.front().path).fileName(), folder});
    QPointer<Document> self(this);
    const AppData data = m_data;
    QThread *worker = QThread::create([self, jobs, cancel, format, folder, data] {
        // an engine of its own, so the picture on screen prints as before
        RenderEngine engine(data);
        QStringList failures;
        int done = 0;
        for (const Job &job : jobs) {
            if (*cancel)
                break;
            QMetaObject::invokeMethod(qApp, [self, done, name = QFileInfo(job.path).fileName()] {
                if (self && self->m_browser.batch()) {
                    FolderBrowser::Batch b = *self->m_browser.batch();
                    b.done = done;
                    b.current = name;
                    self->m_browser.setBatch(b);
                }
            });
            try {
                engine.setPicture(engine.decode(job.proxy), job.path);
                engine.exportTo(job.edits.recipe, job.proxy, job.target, format);
            } catch (const std::exception &e) {
                failures << QStringLiteral("%1: %2").arg(QFileInfo(job.path).fileName(), QString::fromUtf8(e.what()));
            }
            ++done;
        }
        const bool cancelled = *cancel;
        QMetaObject::invokeMethod(qApp, [self, failures, done, total = int(jobs.size()), cancelled, folder] {
            if (!self)
                return;
            if (self->m_browser.batch()) {
                FolderBrowser::Batch b = *self->m_browser.batch();
                b.done = done;
                self->m_browser.setBatch(b);
            }
            self->finishBatch(failures, total, cancelled, folder);
        });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    m_batchThread = worker;
    worker->start(QThread::LowPriority);
    emit changed();
}

void Document::cancelBatch()
{
    if (m_batchCancel)
        *m_batchCancel = true;
}

void Document::finishBatch(const QStringList &failures, int total, bool cancelled, const QString &folder)
{
    const int done = m_browser.batch() ? m_browser.batch()->done : 0;
    m_browser.setBatch(std::nullopt);
    m_batchCancel.reset();
    if (cancelled) {
        m_status = QStringLiteral("Export stopped after %1 of %2").arg(done).arg(total);
    } else if (failures.isEmpty()) {
        m_status = QStringLiteral("Exported %1 print%2 to %3").arg(total).arg(total == 1 ? "" : "s").arg(QFileInfo(folder).fileName());
    } else {
        m_error = QStringLiteral("Exported %1 of %2; not exported: %3%4")
                      .arg(total - failures.size())
                      .arg(total)
                      .arg(failures.mid(0, 5).join(QStringLiteral("; ")))
                      .arg(failures.size() > 5 ? QStringLiteral("; …") : QString());
    }
    emit changed();
}

void Document::prepareToQuit()
{
    recordCurrent();
    if (m_batchCancel)
        *m_batchCancel = true;
    m_library.save(true);
}
