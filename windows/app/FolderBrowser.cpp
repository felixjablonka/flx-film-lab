#include "FolderBrowser.h"

#include "AppSettings.h"
#include "MotionDocument.h"
#include "RawUnpack.h"
#include "Thumbnails.h"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <atomic>
#include <limits>
#include <thread>

namespace {

// The RAW files the decoders read, which the film is made from: what a folder
// lists (FolderBrowser.listing). A JPEG or TIFF beside them, often an export
// or a Photoshop edit of one, is left out; the Open dialog still offers it on
// its own.
const QStringList &pictureSuffixes()
{
    static const QStringList suffixes{
        "3fr", "ari", "arw", "bay", "cap", "cr2", "cr3", "crw", "dcr", "dcs", "dng", "drf", "eip", "erf", "fff",
        "iiq", "k25", "kdc", "mdc", "mef", "mos", "mrw", "nef", "nrw", "orf", "ori", "pef", "ptx", "raf", "raw",
        "rw2", "rwl", "rwz", "sr2", "srf", "srw", "x3f"};
    return suffixes;
}

// Every sidebar alive, for the order they share (main thread only).
std::vector<FolderBrowser *> &browsers()
{
    static std::vector<FolderBrowser *> all;
    return all;
}

QCollator byName()
{
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    return collator;
}

} // namespace

QString FolderBrowser::Item::name() const
{
    return QFileInfo(path).fileName();
}

FolderBrowser::FolderBrowser(Kind kind, const AppData &data, QObject *parent)
    : QObject(parent), m_kind(kind), m_order(foldersByDateShot() ? Order::DateShot : Order::Name),
      m_thumbnails(std::make_unique<ThumbnailStore>(kind, data))
{
    browsers().push_back(this);
}

FolderBrowser::~FolderBrowser()
{
    auto &all = browsers();
    all.erase(std::remove(all.begin(), all.end(), this), all.end());
}

void FolderBrowser::setOrder(Order order)
{
    setFoldersByDateShot(order == Order::DateShot);
    for (FolderBrowser *browser : browsers())
        browser->applyOrder(order);
}

void FolderBrowser::applyOrder(Order order)
{
    if (order == m_order)
        return;
    m_order = order;
    m_items = ordered(std::move(m_items));
    emit listChanged();
    emit orderChanged();
}

std::vector<FolderBrowser::Item> FolderBrowser::ordered(std::vector<Item> items)
{
    QCollator collator = byName();
    if (m_order == Order::Name) {
        std::stable_sort(items.begin(), items.end(),
                         [&](const Item &a, const Item &b) { return collator.compare(a.name(), b.name()) < 0; });
        return items;
    }
    std::vector<QString> unread;
    for (const Item &item : items)
        if (!m_shotDates.count(item.path))
            unread.push_back(item.path);
    if (!unread.empty())
        for (const auto &[path, when] : shotDates(unread, m_kind))
            m_shotDates[path] = when;
    const auto when = [this](const Item &item) {
        const auto it = m_shotDates.find(item.path);
        return it == m_shotDates.end() ? std::numeric_limits<qint64>::max() : it->second;
    };
    std::stable_sort(items.begin(), items.end(), [&](const Item &a, const Item &b) {
        const qint64 first = when(a), second = when(b);
        if (first != second)
            return first < second;
        return collator.compare(a.name(), b.name()) < 0;
    });
    return items;
}

std::map<QString, qint64> FolderBrowser::shotDates(const std::vector<QString> &paths, Kind kind)
{
    std::vector<qint64> found(paths.size(), 0);
    std::atomic<size_t> next{0};
    const auto work = [&] {
        for (size_t i = next++; i < paths.size(); i = next++) {
            const QString &path = paths[i];
            qint64 when = 0;
            if (kind == Kind::Pictures)
                when = captureTimeWithLibRaw(path.toStdString());
            else if (const std::optional<qint64> made = movieDate(path))
                when = *made;
            if (when <= 0) {
                const QFileInfo info(path);
                const QDateTime born = info.birthTime();
                when = (born.isValid() ? born : info.lastModified()).toSecsSinceEpoch();
            }
            found[i] = when;
        }
    };
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::thread> pool;
    for (int t = 1; t < threads && size_t(t) < paths.size(); ++t)
        pool.emplace_back(work);
    work();
    for (std::thread &th : pool)
        th.join();
    std::map<QString, qint64> dates;
    for (size_t i = 0; i < paths.size(); ++i)
        dates[paths[i]] = found[i];
    return dates;
}

std::optional<qint64> FolderBrowser::movieDate(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const qint64 size = file.size();
    const auto be = [](const QByteArray &d, int at, int count) {
        quint64 v = 0;
        for (int i = 0; i < count; ++i)
            v = v << 8 | quint8(d[at + i]);
        return v;
    };
    struct Box {
        qint64 length, header;
        QByteArray type, data;
    };
    // the box at `offset`, its first `count` bytes read
    const auto box = [&](qint64 offset, int count) -> std::optional<Box> {
        if (!file.seek(offset))
            return std::nullopt;
        const QByteArray d = file.read(count);
        if (d.size() < 8)
            return std::nullopt;
        qint64 length = qint64(be(d, 0, 4)), header = 8;
        if (length == 1) {
            if (d.size() < 16)
                return std::nullopt;
            length = qint64(be(d, 8, 8));
            header = 16;
        } else if (length == 0) {
            length = size - offset;
        }
        if (length < header)
            return std::nullopt;
        return Box{length, header, d.mid(4, 4), d};
    };
    for (qint64 at = 0; at + 8 <= size;) {
        const std::optional<Box> top = box(at, 16);
        if (!top)
            return std::nullopt;
        if (top->type == "moov") {
            for (qint64 child = at + top->header; child + 8 <= at + top->length;) {
                const std::optional<Box> inner = box(child, 28);
                if (!inner)
                    return std::nullopt;
                if (inner->type == "mvhd" && inner->data.size() >= 28) {
                    const int body = int(inner->header);
                    const quint64 created = quint8(inner->data[body]) == 1 ? be(inner->data, body + 4, 8)
                                                                           : be(inner->data, body + 4, 4);
                    // seconds since 1904
                    return created > 0 ? std::optional(qint64(created) - 2'082'844'800) : std::nullopt;
                }
                child += inner->length;
            }
            return std::nullopt;
        }
        at += top->length;
    }
    return std::nullopt;
}

bool FolderBrowser::isPicture(const QString &path)
{
    return pictureSuffixes().contains(QFileInfo(path).suffix().toLower());
}

void FolderBrowser::setSelection(std::set<QString> selection)
{
    if (selection == m_selection)
        return;
    m_selection = std::move(selection);
    emit selectionChanged();
    emit stateChanged();
}

std::optional<FolderBrowser::Flag> FolderBrowser::flag(const QString &path) const
{
    const auto it = m_flags.find(path);
    return it == m_flags.end() ? std::nullopt : std::optional(it->second);
}

void FolderBrowser::setFilter(Filter filter)
{
    if (filter == m_filter)
        return;
    m_filter = filter;
    emit listChanged();
}

void FolderBrowser::setCurrent(const QString &path)
{
    if (path == m_current)
        return;
    m_current = path;
    emit selectionChanged();
}

void FolderBrowser::setHasCopiedSettings(bool copied)
{
    if (copied == m_hasCopied)
        return;
    m_hasCopied = copied;
    emit stateChanged();
}

void FolderBrowser::setBatch(std::optional<Batch> batch)
{
    if (batch == m_batch)
        return;
    m_batch = std::move(batch);
    emit stateChanged();
}

bool FolderBrowser::savesSidecars() const
{
    return library && library->savesSidecars();
}

void FolderBrowser::setSavesSidecars(bool on)
{
    if (!library || on == library->savesSidecars())
        return;
    library->setSavesSidecars(on);
    emit stateChanged();
}

void FolderBrowser::requestThumbnail(const QString &path)
{
    if (m_kind == Kind::Pictures)
        m_thumbnails->request(path, ownEdits(path));
    else
        m_thumbnails->requestClip(path, ownGrade(path));
}

std::vector<FolderBrowser::Item> FolderBrowser::shown() const
{
    if (m_filter == Filter::All)
        return m_items;
    std::vector<Item> out;
    for (const Item &item : m_items) {
        const auto f = flag(item.path);
        if (m_filter == Filter::Picks ? f == Flag::Pick : f != Flag::Reject)
            out.push_back(item);
    }
    return out;
}

std::vector<QString> FolderBrowser::selectedForBatch() const
{
    std::vector<QString> out;
    for (const Item &item : m_items)
        if (m_selection.count(item.path) && flag(item.path) != Flag::Reject)
            out.push_back(item.path);
    return out;
}

int FolderBrowser::selectedRejects() const
{
    return int(std::count_if(m_selection.begin(), m_selection.end(),
                             [this](const QString &p) { return flag(p) == Flag::Reject; }));
}

std::optional<QString> FolderBrowser::neighbour(const QString &path, int offset) const
{
    const std::vector<Item> list = shown();
    const auto it = std::find_if(list.begin(), list.end(), [&](const Item &i) { return i.path == path; });
    if (it == list.end())
        return list.empty() ? std::nullopt : std::optional(list.front().path);
    const long j = long(it - list.begin()) + offset;
    return j >= 0 && j < long(list.size()) ? std::optional(list[size_t(j)].path) : std::nullopt;
}

void FolderBrowser::show(const QString &folder, std::vector<Item> items,
                         const std::map<QString, ImageLibrary::Record> &records)
{
    if (folder != m_folder)
        m_shotDates.clear();
    m_folder = folder;
    m_items = ordered(std::move(items));
    m_flags.clear();
    m_edited.clear();
    for (const auto &[path, record] : records) {
        if (record.flag)
            m_flags[path] = *record.flag;
        // a picture's settings of its own, a clip's look
        if (m_kind == Kind::Pictures ? record.edits.has_value() : record.look.has_value())
            m_edited.insert(path);
    }
    std::set<QString> kept;
    for (const QString &path : m_selection)
        if (std::any_of(m_items.begin(), m_items.end(), [&](const Item &i) { return i.path == path; }))
            kept.insert(path);
    m_selection = std::move(kept);
    m_thumbnails->forgetAll();
    emit listChanged();
    emit selectionChanged();
    emit stateChanged();
}

void FolderBrowser::setFlag(std::optional<Flag> flag, const QString &path)
{
    if (this->flag(path) == flag)
        return;
    if (flag)
        m_flags[path] = *flag;
    else
        m_flags.erase(path);
    // a filter may now leave it out, or take it in
    if (m_filter == Filter::All)
        emit rowsChanged();
    else
        emit listChanged();
    emit stateChanged();
}

void FolderBrowser::markEdited(const QString &path)
{
    if (m_edited.insert(path).second)
        emit rowsChanged();
}

QString FolderBrowser::canonical(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

std::vector<FolderBrowser::Item> FolderBrowser::listing(const QString &folder, Kind kind)
{
    const QDir dir(canonical(folder));
    std::vector<Item> items;
    // the folder spelled once, its files' names as the listing spells them
    const QString base = dir.absolutePath();
    for (const QFileInfo &info : dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::NoSort)) {
        const QString path = base + QLatin1Char('/') + info.fileName();
        if (kind == Kind::Pictures ? isPicture(path) : isMovie(path))
            items.push_back(Item{path, info.size()});
    }
    QCollator collator = byName();
    std::sort(items.begin(), items.end(),
              [&](const Item &a, const Item &b) { return collator.compare(a.name(), b.name()) < 0; });
    return items;
}
