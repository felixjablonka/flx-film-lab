#pragma once

#include "ImageLibrary.h"
#include "RenderEngine.h"

#include <QObject>
#include <QString>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class ThumbnailStore;

// The folder in the sidebar: its pictures, which of them are selected, and
// how each is flagged (FolderBrowser.swift). The image on screen is the
// document's own; the selection is what batch actions act on.
//
// Its own object, which only the sidebar watches: a slider dragged changes
// the document every tick, and the sidebar must not be laid out again for
// that.
class FolderBrowser : public QObject
{
    Q_OBJECT
public:
    // What a folder is read for: the Stills mode's pictures, or the Motion
    // mode's clips.
    enum class Kind { Pictures, Clips };
    // Which pictures the sidebar lists.
    enum class Filter { All, Picks, WithoutRejects };
    using Flag = ImageLibrary::Flag;

    struct Item {
        QString path;           // canonical (FolderBrowser::canonical)
        qint64 size = 0;
        QString name() const;
        bool operator==(const Item &) const = default;
    };
    // The batch export under way.
    struct Batch {
        int done = 0, total = 0;
        QString current, folder;
        bool operator==(const Batch &) const = default;
    };

    FolderBrowser(Kind kind, const AppData &data, QObject *parent = nullptr);
    ~FolderBrowser() override;

    Kind kind() const { return m_kind; }
    QString noun() const { return m_kind == Kind::Pictures ? QStringLiteral("picture") : QStringLiteral("clip"); }

    const QString &folder() const { return m_folder; }
    const std::vector<Item> &items() const { return m_items; }
    const std::set<QString> &selection() const { return m_selection; }
    void setSelection(std::set<QString> selection);
    const std::map<QString, Flag> &flags() const { return m_flags; }
    std::optional<Flag> flag(const QString &path) const;
    // The pictures with settings of their own (a clip, a look of its own);
    // the others open with the look on screen.
    const std::set<QString> &edited() const { return m_edited; }
    Filter filter() const { return m_filter; }
    void setFilter(Filter filter);
    // The image on screen, for the sidebar to mark.
    const QString &current() const { return m_current; }
    void setCurrent(const QString &path);
    // Whether there are settings copied, for Paste Settings to Selected.
    bool hasCopiedSettings() const { return m_hasCopied; }
    void setHasCopiedSettings(bool copied);
    const std::optional<Batch> &batch() const { return m_batch; }
    void setBatch(std::optional<Batch> batch);
    // Save Settings Beside Images (ImageLibrary).
    bool savesSidecars() const;
    void setSavesSidecars(bool on);
    // What Paste does to the selection: Paste Settings, or for clips Paste Look.
    QString pasteName = QStringLiteral("Paste Settings");

    ThumbnailStore &thumbnails() { return *m_thumbnails; }
    // Where the flags and settings are kept.
    ImageLibrary *library = nullptr;
    // A picture's own settings, a clip's own grade, if it has one.
    std::function<std::optional<Look>(const QString &)> ownEdits = [](const QString &) { return std::nullopt; };
    std::function<std::optional<MotionLook>(const QString &)> ownGrade = [](const QString &) { return std::nullopt; };
    // A row has come into view: its picture, the print if it has settings of its own.
    void requestThumbnail(const QString &path);

    // The pictures the sidebar lists, in its order.
    std::vector<Item> shown() const;
    // The selection in the sidebar's order, rejects left out: what a batch
    // action acts on.
    std::vector<QString> selectedForBatch() const;
    // How many of the selection are rejects, which batch actions leave out.
    int selectedRejects() const;
    // The picture `offset` places from `path` among those listed.
    std::optional<QString> neighbour(const QString &path, int offset) const;

    void show(const QString &folder, std::vector<Item> items, const std::map<QString, ImageLibrary::Record> &records);
    void setFlag(std::optional<Flag> flag, const QString &path);
    void markEdited(const QString &path);

    // One spelling for each file: absolute, cleaned, with its folders and
    // its name as the disk spells them, so the same picture reached from the
    // sidebar, an Open dialog, Explorer or a drop compares equal.
    static QString canonical(const QString &path);
    // A folder's pictures (or its movies, for clips), by name as Explorer
    // sorts them: the RAW files the decoders read and pictures already
    // rendered, none hidden; sidecars (.flx) are not pictures.
    static std::vector<Item> listing(const QString &folder, Kind kind);
    static bool isPicture(const QString &path);

signals:
    // The folder, its pictures or the filter: the rows themselves.
    void listChanged();
    // The flags or the settings of their own: how rows look.
    void rowsChanged();
    // The selection, or the picture on screen.
    void selectionChanged();
    // The batch, Paste's state, the sidecar preference: the footer and the menu.
    void stateChanged();

private:
    Kind m_kind;
    QString m_folder;
    std::vector<Item> m_items;
    std::set<QString> m_selection;
    std::map<QString, Flag> m_flags;
    std::set<QString> m_edited;
    Filter m_filter = Filter::All;
    QString m_current;
    bool m_hasCopied = false;
    std::optional<Batch> m_batch;
    std::unique_ptr<ThumbnailStore> m_thumbnails;
};
