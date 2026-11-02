#include "Document.h"

#include "AppSettings.h"

#include <QFileInfo>
#include <QtMath>
#include <QTimer>

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

} // namespace

Document::Document(const AppData &data, QObject *parent)
    : QObject(parent), m_data(data), m_presetStore(PresetStore::standardDirectory())
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
    m_presets = m_presetStore.load();
}

Document::~Document()
{
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
    m_edits.whiteBalance = wb;
    if (!m_applyingEdits)
        syncSource();
    emit changed();
}

void Document::setCustomTemperature(double kelvin)
{
    m_edits.customTemperature = kelvin;
    if (!m_applyingEdits && m_edits.whiteBalance == WhiteBalance::Custom)
        syncSource();
    emit changed();
}

void Document::setCustomTint(double tint)
{
    m_edits.customTint = tint;
    if (!m_applyingEdits && m_edits.whiteBalance == WhiteBalance::Custom)
        syncSource();
    emit changed();
}

// MARK: - Opening

void Document::open(const QString &path)
{
    m_error.clear();
    if (path == m_file) {
        // back to the image on screen: a pending open is forgotten
        m_pending.clear();
    } else {
        m_pending = path;
        m_status = QStringLiteral("Opening %1…").arg(QFileInfo(path).fileName());
    }
    syncSource();
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
    // A short settle, so dragging the temperature decodes once at the end
    // rather than once a tick: a decode is far longer than a slider's frame.
    QTimer::singleShot(120, this, [this, key, generation] {
        if (generation != m_generation)
            return;
        onEngine([this, key, generation] {
            if (generation != m_generation)
                return;                         // asked for something newer meanwhile
            std::shared_ptr<DecodedPicture> picture;
            QString error;
            try {
                picture = std::make_shared<DecodedPicture>(m_engine->decode(key));
            } catch (const std::exception &e) {
                error = QString::fromUtf8(e.what());
            }
            QMetaObject::invokeMethod(this, [this, picture, error, key, generation] {
                finishDecode(picture, error, key, generation);
            }, Qt::QueuedConnection);
        });
    });
}

void Document::finishDecode(std::shared_ptr<DecodedPicture> picture, const QString &error, const DecodeKey &key,
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
            m_error = QStringLiteral("Could not open %1: %2").arg(QFileInfo(key.path).fileName(), error);
            m_status = statusLine();
        } else {
            m_error = error;
            m_status = QStringLiteral("Could not decode %1").arg(QFileInfo(key.path).fileName());
        }
        emit changed();
        return;
    }
    onEngine([this, picture, key] {
        RenderEngine::Source source;
        QString error;
        try {
            source = m_engine->setPicture(std::move(*picture), key.path);
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        QMetaObject::invokeMethod(this, [this, source, error, key] {
            if (!error.isEmpty()) {
                m_requested.reset();
                m_isDecoding = false;
                m_error = error;
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
        if (!m_file.isEmpty())
            m_editsByImage[m_file] = m_edits;
        const Look incoming = edits(key.path);
        m_applyingEdits = true;
        m_edits = incoming;
        m_isCropping = m_isLevelling = false;
        m_shapeBeforeCropping.reset();
        m_applyingEdits = false;
        m_file = key.path;
        if (m_pending == key.path)
            m_pending.clear();
        // the last image's readings must not show on this one's sliders
        m_metered = {};
        m_resolved.reset();
        m_displayMap.reset();
        // a mask selected on one image means nothing on the next
        m_selected.clear();
    }
    m_loaded = Loaded{key, source.id, source.width, source.height, source.camera};
    m_status = statusLine();
    // the incoming edits may want a decode of their own
    syncSource();
    scheduleRender();
    emit changed();
}

QString Document::statusLine() const
{
    if (!m_loaded)
        return QStringLiteral("Open a RAW file to begin");
    QString line = QStringLiteral("%1x%2").arg(m_loaded->width).arg(m_loaded->height);
    if (!m_loaded->camera.isEmpty())
        line += QStringLiteral("  ·  ") + m_loaded->camera;
    return line;
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
    emit changed();
}

void Document::pasteSettings()
{
    if (canPasteSettings())
        put(m_copied->first);
}

// Copied or saved settings onto the image on screen: film, paper, print or
// light table, camera filter, every slider and auto, and white balance.
// Masks, gradients and the crop belong to the frame they were made for.
void Document::put(const Look &look)
{
    Look pasted = look;
    pasted.recipe.gradients = m_edits.recipe.gradients;
    pasted.recipe.masks = m_edits.recipe.masks;
    pasted.recipe.gradients = m_edits.recipe.gradients;
    pasted.recipe.geometry = m_edits.recipe.geometry;
    pasted.recipe.shaped = m_edits.recipe.shaped;

    m_applyingEdits = true;
    m_edits = pasted;
    m_applyingEdits = false;
    // the white balance is decoded in, and a film-reference one follows the stock
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
    put(preset.look);
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
    if (m_isDecoding)
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

bool Document::isTransparency()
{
    const ProfilePack *film = filmPack();
    return m_edits.recipe.scanFilm.value_or(film && film->isPositive());
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
    // a frame for an image since replaced is dropped; the follow-up draws the new one
    if (output && m_loaded && sourceID == m_loaded->sourceID) {
        m_display = std::move(output->image);
        m_resolved = std::move(output->resolved);
        m_metered = output->metered;
        m_lastMilliseconds = output->milliseconds;
        m_displayMap = output->map;
        m_lastBreakdown = output->breakdown;
        emit imageChanged();
    }
    if (!error.isEmpty())
        m_error = error;
    if (m_renderAgain || (m_loaded && sourceID != m_loaded->sourceID)) {
        m_renderAgain = false;
        scheduleRender();
    }
    emit changed();
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

void Document::endCropping(bool keep)
{
    if (!keep && m_shapeBeforeCropping) {
        Recipe r = m_edits.recipe;
        r.geometry = *m_shapeBeforeCropping;
        r.shaped = !r.geometry.isIdentity();
        m_applyingEdits = true;
        setRecipe(r);
        m_applyingEdits = false;
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
    Recipe r = m_edits.recipe;
    r.geometry = {};
    r.shaped = false;
    setRecipe(r);
}

// MARK: - Graduated exposure, dodge & burn

void Document::setSelected(const std::string &id)
{
    if (id == m_selected)
        return;
    m_selected = id;
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

void Document::addGradient()
{
    Recipe r = m_edits.recipe;
    if (int(r.gradients.size()) >= kMaxExposureGradients)
        return;
    ExposureGradient g;
    g.id = newIdentifier();
    g.name = QStringLiteral("Gradient %1").arg(r.gradients.size() + 1).toStdString();
    const std::vector<PointD> points = framePoints({{g.ax, g.ay}, {g.bx, g.by}});
    g.ax = points[0].x;
    g.ay = points[0].y;
    g.bx = points[1].x;
    g.by = points[1].y;
    r.gradients.push_back(g);
    m_selected = g.id;
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
}

void Document::resetTone()
{
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
    setRecipe(r);
}

// Everything on the Colour page: filtration, regional casts, pre-flash.
void Document::resetColour()
{
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
}

// Couplers, halation, grain, glare, camera sharpness and development gamma,
// back to the stock's own measured defaults.
void Document::resetFilmCharacter()
{
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
}
