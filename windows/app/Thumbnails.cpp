#include "Thumbnails.h"

#include "DisplayColour.h"

#include "MotionEngine.h"
#include "SetupBuilder.h"

#include "libraw/libraw.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QPainter>
#include <QPointer>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QTransform>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>

namespace {

QString hashed(const QString &text)
{
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).left(16).toHex());
}

QString keyFor(const QString &path, const QJsonObject &settings)
{
    const QFileInfo info(path);
    const QString made = ImageLibrary::software() + QLatin1Char('|')
        + QString::number(info.lastModified().toSecsSinceEpoch()) + QLatin1Char('|') + QString::number(info.size())
        + QLatin1Char('|') + QString::fromUtf8(QJsonDocument(settings).toJson(QJsonDocument::Compact));
    return hashed(path) + QLatin1Char('/') + hashed(made);
}

const ProfilePack *tryPack(PackLibrary &packs, const std::string &stock)
{
    try {
        return &packs.pack(stock);
    } catch (const std::exception &) {
        return nullptr;
    }
}

// dcraw's flip as the turn that stands the picture up
QTransform upright(int flip)
{
    switch (flip) {
    case 3: return QTransform().rotate(180);
    case 5: return QTransform().rotate(-90);
    case 6: return QTransform().rotate(90);
    default: return {};
    }
}

} // namespace

// Makes the sidebar's prints and clip frames off the main thread, one at a
// time, in the order asked, on a renderer of its own (a GPU device of its
// own), so the picture on screen prints as fast as before. The renderer is
// let go once the sidebar has had its pictures a while.
class ThumbnailWorker
{
public:
    explicit ThumbnailWorker(AppData data) : m_data(std::move(data))
    {
        m_context = new QObject;
        m_context->moveToThread(&m_thread);
        QObject::connect(&m_thread, &QThread::finished, m_context, &QObject::deleteLater);
        m_thread.setObjectName(QStringLiteral("thumbnails"));
        m_thread.start(QThread::LowPriority);
    }

    ~ThumbnailWorker()
    {
        cancelAll();
        // the GPU's work is done on its thread, and its devices go there too
        QMetaObject::invokeMethod(m_context, [this] { release(); }, Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }

    int round() const { return m_generation.load(); }
    void cancelAll() { ++m_generation; }

    // `make` on the worker's thread, after the jobs before it; `done` with its
    // picture on the main thread, a null one when it failed or was dropped
    void post(int round, std::function<QImage(ThumbnailWorker &)> make, QObject *receiver,
              std::function<void(QImage)> done)
    {
        QPointer<QObject> guard(receiver);
        QMetaObject::invokeMethod(m_context, [this, round, make = std::move(make), guard, done = std::move(done)] {
            QImage image;
            if (round == m_generation.load()) {
                try {
                    image = make(*this);
                } catch (const std::exception &) {
                    image = {};
                }
            }
            m_lastJob.start();
            QTimer::singleShot(10000, m_context, [this] {
                if (m_lastJob.isValid() && m_lastJob.elapsed() >= 9900)
                    release();
            });
            QMetaObject::invokeMethod(qApp, [guard, done, image = std::move(image)] {
                if (guard)
                    done(image);
            });
        });
    }

    // The print of a picture with settings of its own: decoded small, metered
    // on that decode as it would be on screen, printed with its settings.
    QImage print(const QString &path, const Look &edits)
    {
        if (!m_engine)
            m_engine = std::make_unique<RenderEngine>(m_data);
        DecodeKey key;
        key.path = path;
        key.whiteBalance = decodeName(edits.whiteBalance);
        const ProfilePack *film = tryPack(packs(), edits.recipe.filmStock);
        key.reference = film && !film->referenceIlluminant.empty() ? film->referenceIlluminant : "D55";
        key.temperature = edits.customTemperature;
        key.tint = edits.customTint;
        key.maxDimension = 512;
        const RenderEngine::Source source = m_engine->setPicture(m_engine->decode(key), path);
        const auto output = m_engine->render(edits.recipe, source.id);
        return output ? output->image : QImage();
    }

    // A frame of a clip a tenth of the way in (at most a second), turned as
    // it plays: as a player shows log footage, or graded with `grade`, read
    // as the preview reads it and metered as Auto exposure meters the clip.
    QImage clipFrame(const QString &path, const std::optional<MotionLook> &grade)
    {
        if (!m_motion)
            m_motion = std::make_unique<MotionEngine>(m_data.packs);
        const int id = ++m_clipID;
        const ClipInfo info = m_motion->open(path, id);
        MotionEngine::Request r;
        r.clip = id;
        r.time = std::min(1.0, std::max(0.0, info.duration / 10));
        r.format = grade && grade->input.format ? *grade->input.format : info.detection().first;
        r.input = grade ? grade->input : InputSettings{};
        r.input.format = r.format;
        if (grade) {
            r.recipe = grade->recipe;
            if (r.recipe.autoExposure) {
                if (const ProfilePack *film = tryPack(packs(), r.recipe.filmStock)) {
                    if (const auto highlight = clipHighlight(path.toStdString(), r.input, r.format)) {
                        if (!m_builder)
                            m_builder = std::make_unique<SetupBuilder>(packs());
                        r.recipe.exposureEV = clipExposure(*highlight, m_builder->highlightHeadroomStops(*film));
                    }
                }
            }
            r.effects = effectsAsPrinted(*grade, packs());
            r.mode = FramePipeline::Mode::Graded;
        } else {
            r.mode = FramePipeline::Mode::Log;
        }
        const double scale = std::min(1.0, double(ThumbnailStore::size) / std::max(info.width, info.height));
        r.size = QSize(std::max(1, int(std::lround(info.width * scale))), std::max(1, int(std::lround(info.height * scale))));
        const auto output = m_motion->render(r);
        if (!output || output->image.isNull())
            return {};
        return info.quarterTurns % 4 ? output->image.transformed(QTransform().rotate(90.0 * info.quarterTurns))
                                     : output->image;
    }

private:
    PackLibrary &packs()
    {
        if (!m_packs)
            m_packs = std::make_unique<PackLibrary>(m_data.packs);
        return *m_packs;
    }

    void release()
    {
        m_builder.reset();
        m_engine.reset();
        m_motion.reset();
        m_lastJob.invalidate();
    }

    AppData m_data;
    QThread m_thread;
    QObject *m_context = nullptr;
    std::atomic<int> m_generation{0};
    QElapsedTimer m_lastJob;
    std::unique_ptr<PackLibrary> m_packs;
    std::unique_ptr<SetupBuilder> m_builder;
    std::unique_ptr<RenderEngine> m_engine;
    std::unique_ptr<MotionEngine> m_motion;
    int m_clipID = 0;
};

ThumbnailStore::ThumbnailStore(FolderBrowser::Kind kind, const AppData &data, QObject *parent)
    : QObject(parent), m_kind(kind), m_data(data)
{
    m_previews.setMaxThreadCount(3);
    m_previews.setThreadPriority(QThread::LowPriority);
}

ThumbnailStore::~ThumbnailStore()
{
    m_previews.clear();
    m_previews.waitForDone();
}

QImage ThumbnailStore::image(const QString &path) const
{
    const auto it = m_images.find(path);
    return it == m_images.end() ? QImage() : it->second;
}

QImage ThumbnailStore::shown(const QString &path) const
{
    const QImage original = image(path);
    if (original.isNull() || !displaycolour::active())
        return original;
    const int generation = displaycolour::generation();
    auto &converted = m_shown[path];
    if (converted.first != generation || converted.second.isNull() || converted.second.size() != original.size()) {
        QImage source = original;
        source.setColorSpace(QColorSpace(QColorSpace::SRgb));
        converted = {generation, displaycolour::toScreen(source)};
    }
    return converted.second;
}

QString ThumbnailStore::printKey(const QString &path, const Look &edits)
{
    return keyFor(path, QJsonObject{{"recipe", recipeToJson(edits.recipe)},
                                    {"whiteBalance", whiteBalanceName(edits.whiteBalance)},
                                    {"customTemperature", edits.customTemperature},
                                    {"customTint", edits.customTint}});
}

QString ThumbnailStore::printKey(const QString &path, const MotionLook &grade)
{
    return keyFor(path, motionLookToJson(grade));
}

void ThumbnailStore::touch(const QString &path)
{
    m_lastUsed[path] = ++m_clock;
}

void ThumbnailStore::request(const QString &path, const std::optional<Look> &edits)
{
    touch(path);
    if (!edits) {
        if (m_shownPrint.erase(path)) {
            m_images.erase(path);
            m_shown.erase(path);
            emit imageChanged(path);
        }
        readPreview(path);
        return;
    }
    const QString key = printKey(path, *edits);
    if (const auto it = m_shownPrint.find(path); it != m_shownPrint.end() && it->second == key)
        return;
    if (QImage cached = loadCached(key); !cached.isNull()) {
        put(std::move(cached), path, key);
        return;
    }
    if (!m_images.count(path))
        readPreview(path);
    makePrint(path, *edits, key);
}

void ThumbnailStore::requestClip(const QString &path, const std::optional<MotionLook> &grade)
{
    touch(path);
    if (!grade) {
        if (m_shownPrint.erase(path)) {
            m_images.erase(path);
            m_shown.erase(path);
            emit imageChanged(path);
        }
        readPreview(path);
        return;
    }
    const QString key = printKey(path, *grade);
    if (const auto it = m_shownPrint.find(path); it != m_shownPrint.end() && it->second == key)
        return;
    if (QImage cached = loadCached(key); !cached.isNull()) {
        put(std::move(cached), path, key);
        return;
    }
    if (!m_images.count(path))
        readPreview(path);
    if (!m_printing.insert(key).second)
        return;
    if (!m_worker)
        m_worker = std::make_unique<ThumbnailWorker>(m_data);
    const MotionLook look = *grade;
    m_worker->post(m_worker->round(), [path, look](ThumbnailWorker &w) { return w.clipFrame(path, look); }, this,
                   [this, path, key](QImage image) {
                       m_printing.erase(key);
                       if (image.isNull())
                           return;
                       QImage shrunk = shrink(image);
                       QThreadPool::globalInstance()->start([shrunk, key] { saveCached(shrunk, key); });
                       put(std::move(shrunk), path, key);
                   });
}

void ThumbnailStore::setPrint(const QImage &image, const QString &path, const Look &edits)
{
    QImage shrunk = shrink(image);
    if (shrunk.isNull())
        return;
    const QString key = printKey(path, edits);
    QThreadPool::globalInstance()->start([shrunk, key] { saveCached(shrunk, key); });
    put(std::move(shrunk), path, key);
}

void ThumbnailStore::refresh(const std::vector<std::pair<QString, Look>> &jobs)
{
    for (const auto &[path, edits] : jobs)
        if (m_lastUsed.count(path))
            request(path, edits);
}

void ThumbnailStore::refreshClips(const std::vector<std::pair<QString, MotionLook>> &jobs)
{
    for (const auto &[path, grade] : jobs)
        if (m_lastUsed.count(path))
            requestClip(path, grade);
}

void ThumbnailStore::forgetAll()
{
    m_previews.clear();
    if (m_worker)
        m_worker->cancelAll();
    m_images.clear();
    m_shown.clear();
    m_shownPrint.clear();
    m_reading.clear();
    m_lastUsed.clear();
    m_printing.clear();
    emit trimmed();
}

void ThumbnailStore::trim(int keep)
{
    std::vector<std::pair<int, QString>> used;
    for (const auto &[path, at] : m_lastUsed)
        used.emplace_back(at, path);
    std::sort(used.begin(), used.end(), std::greater<>());
    std::set<QString> kept;
    for (int i = 0; i < std::min(keep, int(used.size())); ++i)
        kept.insert(used[size_t(i)].second);
    for (auto it = m_images.begin(); it != m_images.end();) {
        if (!kept.count(it->first)) {
            m_shown.erase(it->first);
            m_shownPrint.erase(it->first);
            m_lastUsed.erase(it->first);
            it = m_images.erase(it);
        } else {
            ++it;
        }
    }
    emit trimmed();
}

void ThumbnailStore::put(QImage image, const QString &path, const QString &key)
{
    m_shown.erase(path);
    m_images[path] = std::move(image);
    if (key.isEmpty())
        m_shownPrint.erase(path);
    else
        m_shownPrint[path] = key;
    if (int(m_images.size()) > limit)
        trim(limit * 3 / 4);
    emit imageChanged(path);
}

void ThumbnailStore::readPreview(const QString &path)
{
    if (m_images.count(path) || !m_reading.insert(path).second)
        return;
    if (m_kind == FolderBrowser::Kind::Clips) {
        // a frame of the clip, as a player shows it, on the worker's decoder
        if (!m_worker)
            m_worker = std::make_unique<ThumbnailWorker>(m_data);
        m_worker->post(m_worker->round(), [path](ThumbnailWorker &w) { return w.clipFrame(path, std::nullopt); },
                       this, [this, path](QImage image) {
                           m_reading.erase(path);
                           // a graded frame that came in meanwhile stays
                           if (!image.isNull() && !m_shownPrint.count(path))
                               put(shrink(image), path, {});
                       });
        return;
    }
    QPointer<ThumbnailStore> self(this);
    m_previews.start([self, path] {
        QImage image = cameraPreview(path);
        QMetaObject::invokeMethod(qApp, [self, path, image = std::move(image)]() mutable {
            if (!self)
                return;
            self->m_reading.erase(path);
            // a print that came in meanwhile stays
            if (!image.isNull() && !self->m_shownPrint.count(path))
                self->put(std::move(image), path, {});
        });
    });
}

void ThumbnailStore::makePrint(const QString &path, const Look &edits, const QString &key)
{
    if (!m_printing.insert(key).second)
        return;
    if (!m_worker)
        m_worker = std::make_unique<ThumbnailWorker>(m_data);
    m_worker->post(m_worker->round(), [path, edits](ThumbnailWorker &w) { return w.print(path, edits); }, this,
                   [this, path, key](QImage image) {
                       m_printing.erase(key);
                       if (image.isNull())
                           return;
                       QImage shrunk = shrink(image);
                       QThreadPool::globalInstance()->start([shrunk, key] { saveCached(shrunk, key); });
                       put(std::move(shrunk), path, key);
                   });
}

QImage ThumbnailStore::shrink(const QImage &image, int size)
{
    if (image.isNull())
        return {};
    const double scale = std::min(1.0, double(size) / std::max(image.width(), image.height()));
    if (scale >= 1)
        return image.convertToFormat(QImage::Format_RGB32);
    return image
        .scaled(std::max(1, int(std::lround(image.width() * scale))), std::max(1, int(std::lround(image.height() * scale))),
                Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_RGB32);
}

QImage ThumbnailStore::cameraPreview(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool rendered = suffix == "jpg" || suffix == "jpeg" || suffix == "png" || suffix == "tif" || suffix == "tiff";
    if (!rendered) {
        // The smallest of the previews the camera embedded that still fills
        // the thumbnail: a NEF's 640 px JPEG rather than its full-size one.
        auto raw = std::make_unique<LibRaw>();
        if (raw->open_file(QFileInfo(path).absoluteFilePath().toStdString().c_str()) == LIBRAW_SUCCESS) {
            const libraw_thumbnail_list_t &list = raw->imgdata.thumbs_list;
            int best = -1, bestSide = 0;
            for (int i = 0; i < list.thumbcount; ++i) {
                const auto &t = list.thumblist[i];
                if (t.tformat != LIBRAW_INTERNAL_THUMBNAIL_JPEG)
                    continue;
                const int side = std::max(t.twidth, t.theight);
                // the smallest that fills the thumbnail; while none does, the largest
                const bool better = best < 0 || (side >= size ? bestSide < size || side < bestSide : side > bestSide);
                if (better) {
                    best = i;
                    bestSide = side;
                }
            }
            const bool unpacked = best >= 0 ? raw->unpack_thumb_ex(best) == LIBRAW_SUCCESS
                                            : raw->unpack_thumb() == LIBRAW_SUCCESS;
            if (unpacked) {
                int error = 0;
                libraw_processed_image_t *thumb = raw->dcraw_make_mem_thumb(&error);
                QImage image;
                if (thumb && thumb->type == LIBRAW_IMAGE_JPEG) {
                    QByteArray bytes(reinterpret_cast<const char *>(thumb->data), int(thumb->data_size));
                    QBuffer buffer(&bytes);
                    QImageReader reader(&buffer, "jpg");
                    const QSize full = reader.size();
                    if (full.isValid() && std::max(full.width(), full.height()) > 2 * size) {
                        // JPEG's own scaling, far quicker than reading it whole
                        const double scale = double(2 * size) / std::max(full.width(), full.height());
                        reader.setScaledSize(QSize(int(full.width() * scale), int(full.height() * scale)));
                    }
                    image = reader.read();
                } else if (thumb && thumb->type == LIBRAW_IMAGE_BITMAP && thumb->colors == 3 && thumb->bits == 8) {
                    image = QImage(thumb->data, thumb->width, thumb->height, thumb->width * 3, QImage::Format_RGB888).copy();
                }
                if (thumb)
                    LibRaw::dcraw_clear_mem(thumb);
                // the preview is the sensor's way up; the file says which way stands it up
                if (!image.isNull())
                    return shrink(image.transformed(upright(raw->imgdata.sizes.flip)));
            }
        }
    }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize full = reader.size();
    if (full.isValid() && std::max(full.width(), full.height()) > 2 * size) {
        const double scale = double(2 * size) / std::max(full.width(), full.height());
        reader.setScaledSize(QSize(int(full.width() * scale), int(full.height() * scale)));
    }
    return shrink(reader.read());
}

// MARK: - The disk cache: one print a picture, the newest

QString ThumbnailStore::cacheDirectory()
{
    if (!cacheDirectoryOverride.isEmpty())
        return cacheDirectoryOverride;
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath(QStringLiteral("Thumbnails"));
}

QImage ThumbnailStore::loadCached(const QString &key)
{
    const QString file = QDir(cacheDirectory()).filePath(key + QStringLiteral(".jpg"));
    if (!QFileInfo::exists(file))
        return {};
    return QImage(file).convertToFormat(QImage::Format_RGB32);
}

void ThumbnailStore::saveCached(const QImage &image, const QString &key)
{
    const QFileInfo file(QDir(cacheDirectory()).filePath(key + QStringLiteral(".jpg")));
    QDir folder = file.absoluteDir();
    // the print before goes: nothing asks for it unless its settings come back
    if (folder.exists())
        for (const QString &old : folder.entryList(QDir::Files))
            if (old != file.fileName())
                folder.remove(old);
    folder.mkpath(QStringLiteral("."));
    QImageWriter writer(file.absoluteFilePath(), "jpg");
    writer.setQuality(85);
    writer.write(image);
}
