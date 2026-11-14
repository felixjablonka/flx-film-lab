#include "ImageLibrary.h"

#include "AppSettings.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <tuple>

namespace {

QDateTime nowToTheSecond()
{
    // what is written is to the second, so what is compared in memory is too
    QDateTime now = QDateTime::currentDateTimeUtc();
    now.setTime(QTime(now.time().hour(), now.time().minute(), now.time().second()));
    return now;
}

QJsonObject editsToJson(const Look &look)
{
    return QJsonObject{{"recipe", recipeToJson(look.recipe)},
                       {"whiteBalance", whiteBalanceName(look.whiteBalance)},
                       {"customTemperature", look.customTemperature},
                       {"customTint", look.customTint}};
}

// As the Mac's ImageEdits decodes: the recipe needed, the rest with their
// defaults where an earlier build left them out.
std::optional<Look> editsFromJson(const QJsonValue &v)
{
    if (!v.isObject() || !v.toObject().value("recipe").isObject())
        return std::nullopt;
    const QJsonObject o = v.toObject();
    Look look;
    look.recipe = recipeFromJson(o.value("recipe").toObject());
    look.whiteBalance = o.value("whiteBalance").isString() ? whiteBalanceNamed(o.value("whiteBalance").toString())
                                                            : WhiteBalance::FilmReference;
    look.customTemperature = o.value("customTemperature").toDouble(5500);
    look.customTint = o.value("customTint").toDouble(0);
    return look;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QSaveFile out(path);
    return out.open(QIODevice::WriteOnly) && out.write(data) == data.size() && out.commit();
}

} // namespace

ImageLibrary::ImageLibrary(QString directory, QObject *parent)
    : QObject(parent), m_directory(std::move(directory))
{
    m_saveTimer.setSingleShot(true);
    connect(&m_saveTimer, &QTimer::timeout, this, [this] { save(); });
    m_writerContext = new QObject;
    m_writerContext->moveToThread(&m_writer);
    connect(&m_writer, &QThread::finished, m_writerContext, &QObject::deleteLater);
    m_writer.setObjectName(QStringLiteral("library"));
    m_writer.start(QThread::LowPriority);
}

ImageLibrary::~ImageLibrary()
{
    save(true);
    m_writer.quit();
    m_writer.wait();
}

QString ImageLibrary::standardDirectory()
{
    // %APPDATA%\FLX Film Lab\Library on Windows
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("Library"));
}

QString ImageLibrary::software()
{
    const QString version = QCoreApplication::applicationVersion();
    return version.isEmpty() ? QStringLiteral("FLX Film Lab") : QStringLiteral("FLX Film Lab ") + version;
}

bool ImageLibrary::savesSidecars() const
{
    return appSettings().value(sidecarsKey, false).toBool();
}

void ImageLibrary::setSavesSidecars(bool on)
{
    appSettings().setValue(sidecarsKey, on);
    if (!on)
        return;
    for (const auto &[key, folder] : m_folders)
        for (const auto &[name, record] : folder.records)
            writeSidecar(record, QDir(folder.path).filePath(name));
}

// MARK: - Records and their JSON, as the Mac writes them

QJsonObject ImageLibrary::recordToJson(const Record &record)
{
    // Swift's ISO 8601 strategy: to the second, in UTC, a Z at the end; it
    // reads no fractions of a second
    QJsonObject o{{"modified", record.modified.toUTC().toString(Qt::ISODate)}};
    if (record.flag)
        o.insert("flag", *record.flag == Flag::Pick ? QStringLiteral("pick") : QStringLiteral("reject"));
    if (record.edits)
        o.insert("edits", editsToJson(*record.edits));
    if (record.look)
        o.insert("look", motionLookToJson(*record.look));
    if (!record.app.isEmpty())
        o.insert("app", record.app);
    return o;
}

std::optional<ImageLibrary::Record> ImageLibrary::recordFromJson(const QJsonObject &o)
{
    const QDateTime modified = QDateTime::fromString(o.value("modified").toString(), Qt::ISODate);
    if (!modified.isValid())
        return std::nullopt;
    Record r;
    r.modified = modified.toUTC();
    const QString flag = o.value("flag").toString();
    if (flag == QStringLiteral("pick"))
        r.flag = Flag::Pick;
    else if (flag == QStringLiteral("reject"))
        r.flag = Flag::Reject;
    r.edits = editsFromJson(o.value("edits"));
    if (o.value("look").isObject())
        r.look = motionLookFromJson(o.value("look").toObject());
    r.app = o.value("app").toString();
    return r;
}

std::optional<ImageLibrary::Record> ImageLibrary::readSidecar(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? recordFromJson(doc.object()) : std::nullopt;
}

// MARK: - Reading

QString ImageLibrary::folderKey(const QString &folder)
{
    const QFileInfo info(folder);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

// The library file of a folder: a name from its path.
QString ImageLibrary::folderFile(const QString &key) const
{
    const QByteArray digest = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).left(16).toHex();
    return QDir(m_directory).filePath(QString::fromLatin1(digest) + QStringLiteral(".json"));
}

namespace {

std::optional<std::tuple<QString, std::map<QString, ImageLibrary::Record>, std::map<QString, qint64>>>
parseFolder(const QByteArray &data)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
        return std::nullopt;
    const QJsonObject o = doc.object();
    std::map<QString, ImageLibrary::Record> records;
    const QJsonObject r = o.value("records").toObject();
    for (auto it = r.begin(); it != r.end(); ++it)
        if (auto record = ImageLibrary::recordFromJson(it.value().toObject()))
            records[it.key()] = std::move(*record);
    std::map<QString, qint64> sizes;
    const QJsonObject s = o.value("sizes").toObject();
    for (auto it = s.begin(); it != s.end(); ++it)
        sizes[it.key()] = qint64(it.value().toDouble());
    return std::tuple(o.value("path").toString(), std::move(records), std::move(sizes));
}

} // namespace

std::optional<ImageLibrary::Folder> ImageLibrary::readFolderFile(const QString &key) const
{
    QFile file(folderFile(key));
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    auto parsed = parseFolder(file.readAll());
    if (!parsed || std::get<0>(*parsed) != key)
        return std::nullopt;
    return Folder{key, std::move(std::get<1>(*parsed)), std::move(std::get<2>(*parsed))};
}

// A folder the library has no file for may be one it knew elsewhere: the
// library file whose files' names and sizes match the most of this folder's,
// at least half of them, lends it the records of the files that match.
std::optional<ImageLibrary::Folder> ImageLibrary::relocated(const QString &key,
                                                            const std::vector<std::pair<QString, qint64>> &files)
{
    if (files.empty())
        return std::nullopt;
    std::map<QString, qint64> sizes;
    for (const auto &[path, size] : files)
        sizes[QFileInfo(path).fileName()] = size;
    std::optional<std::pair<Folder, std::vector<QString>>> best;
    const QDir dir(m_directory);
    for (const QString &name : dir.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        auto parsed = parseFolder(file.readAll());
        if (!parsed || std::get<0>(*parsed) == key)
            continue;
        const auto &theirs = std::get<2>(*parsed);
        std::vector<QString> matches;
        for (const auto &[fileName, size] : sizes)
            if (const auto it = theirs.find(fileName); it != theirs.end() && it->second == size)
                matches.push_back(fileName);
        if (matches.size() * 2 >= files.size() && matches.size() > (best ? best->second.size() : 0))
            best.emplace(Folder{std::get<0>(*parsed), std::move(std::get<1>(*parsed)), theirs}, std::move(matches));
    }
    if (!best)
        return std::nullopt;
    Folder found{key, {}, {}};
    for (const QString &name : best->second) {
        if (const auto it = best->first.records.find(name); it != best->first.records.end())
            found.records[name] = it->second;
        found.sizes[name] = best->first.sizes[name];
    }
    m_unsaved.insert(key);
    return found;
}

std::map<QString, ImageLibrary::Record> ImageLibrary::load(const QString &folder,
                                                           const std::vector<std::pair<QString, qint64>> &files)
{
    const QString key = folderKey(folder);
    Folder entry;
    if (const auto it = m_folders.find(key); it != m_folders.end())
        entry = it->second;
    else if (auto read = readFolderFile(key))
        entry = std::move(*read);
    else if (auto moved = relocated(key, files))
        entry = std::move(*moved);
    else
        entry = Folder{key, {}, {}};
    for (const auto &[path, size] : files) {
        const QString name = QFileInfo(path).fileName();
        if (const auto it = entry.sizes.find(name); it == entry.sizes.end() || it->second != size) {
            entry.sizes[name] = size;
            m_unsaved.insert(key);
        }
        const auto sidecar = readSidecar(sidecarPath(path));
        if (!sidecar)
            continue;
        const auto kept = entry.records.find(name);
        if (kept == entry.records.end() || sidecar->modified > kept->second.modified) {
            entry.records[name] = *sidecar;
            m_unsaved.insert(key);
        }
    }
    std::map<QString, Record> out;
    for (const auto &[path, size] : files)
        if (const auto it = entry.records.find(QFileInfo(path).fileName()); it != entry.records.end())
            out[path] = it->second;
    m_folders[key] = std::move(entry);
    if (m_unsaved.count(key))
        scheduleSave();
    return out;
}

std::optional<ImageLibrary::Record> ImageLibrary::record(const QString &file)
{
    const QFileInfo info(file);
    const QString key = folderKey(info.absolutePath());
    auto it = m_folders.find(key);
    if (it == m_folders.end()) {
        auto read = readFolderFile(key);
        if (!read)
            return std::nullopt;
        it = m_folders.emplace(key, std::move(*read)).first;
    }
    const auto r = it->second.records.find(info.fileName());
    return r == it->second.records.end() ? std::nullopt : std::optional(r->second);
}

// MARK: - Writing

void ImageLibrary::update(const QString &file, const std::function<void(Record &)> &change)
{
    const QFileInfo info(file);
    const QString key = folderKey(info.absolutePath());
    auto it = m_folders.find(key);
    if (it == m_folders.end()) {
        auto read = readFolderFile(key);
        it = m_folders.emplace(key, read ? std::move(*read) : Folder{key, {}, {}}).first;
    }
    Folder &entry = it->second;
    const QString name = info.fileName();
    const auto kept = entry.records.find(name);
    const bool isNew = kept == entry.records.end();
    Record record = isNew ? Record{nowToTheSecond(), {}, {}, {}, {}} : kept->second;
    const Record before = record;
    change(record);
    if (!isNew && record.flag == before.flag && record.edits == before.edits && record.look == before.look)
        return;
    record.modified = nowToTheSecond();
    record.app = software();
    entry.records[name] = record;
    if (!entry.sizes.count(name))
        entry.sizes[name] = info.size();
    m_unsaved.insert(key);
    if (savesSidecars())
        writeSidecar(record, file);
    scheduleSave();
}

void ImageLibrary::scheduleSave()
{
    m_saveTimer.start(saveDelayMs);
}

void ImageLibrary::save(bool wait)
{
    m_saveTimer.stop();
    std::vector<std::pair<QString, QByteArray>> files;
    for (const QString &key : m_unsaved) {
        const auto it = m_folders.find(key);
        if (it == m_folders.end())
            continue;
        QJsonObject records, sizes;
        for (const auto &[name, record] : it->second.records)
            records.insert(name, recordToJson(record));
        for (const auto &[name, size] : it->second.sizes)
            sizes.insert(name, double(size));
        const QJsonObject o{{"path", key}, {"records", records}, {"sizes", sizes}};
        files.emplace_back(folderFile(key), QJsonDocument(o).toJson(QJsonDocument::Compact));
    }
    m_unsaved.clear();
    if (files.empty() && !wait)
        return;
    const QString directory = m_directory;
    write([directory, files = std::move(files)] {
        QDir().mkpath(directory);
        for (const auto &[path, data] : files)
            writeFile(path, data);
    }, wait);
}

void ImageLibrary::finishWriting()
{
    write([] {}, true);
}

void ImageLibrary::writeSidecar(const Record &record, const QString &file)
{
    // a folder that cannot be written, a card or a share read-only, keeps
    // the record in the library alone
    write([path = sidecarPath(file), data = QJsonDocument(recordToJson(record)).toJson(QJsonDocument::Indented)] {
        writeFile(path, data);
    });
}

void ImageLibrary::write(std::function<void()> work, bool wait)
{
    QMetaObject::invokeMethod(m_writerContext, std::move(work),
                              wait ? Qt::BlockingQueuedConnection : Qt::QueuedConnection);
}
