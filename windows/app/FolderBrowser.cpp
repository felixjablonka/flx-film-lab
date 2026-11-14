#include "FolderBrowser.h"

#include "MotionDocument.h"
#include "Thumbnails.h"

#include <QCollator>
#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace {

// The RAW files the decoders read, and pictures already rendered: what the
// Open dialog offers.
const QStringList &pictureSuffixes()
{
    static const QStringList suffixes{
        "3fr", "ari", "arw", "bay", "cap", "cr2", "cr3", "crw", "dcr", "dcs", "dng", "drf", "eip", "erf", "fff",
        "iiq", "k25", "kdc", "mdc", "mef", "mos", "mrw", "nef", "nrw", "orf", "ori", "pef", "ptx", "raf", "raw",
        "rw2", "rwl", "rwz", "sr2", "srf", "srw", "x3f", "jpg", "jpeg", "png", "tif", "tiff"};
    return suffixes;
}

} // namespace

QString FolderBrowser::Item::name() const
{
    return QFileInfo(path).fileName();
}

FolderBrowser::FolderBrowser(Kind kind, const AppData &data, QObject *parent)
    : QObject(parent), m_kind(kind), m_thumbnails(std::make_unique<ThumbnailStore>(kind, data))
{
}

FolderBrowser::~FolderBrowser() = default;

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
    m_folder = folder;
    m_items = std::move(items);
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
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(items.begin(), items.end(),
              [&](const Item &a, const Item &b) { return collator.compare(a.name(), b.name()) < 0; });
    return items;
}
