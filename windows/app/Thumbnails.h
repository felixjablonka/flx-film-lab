#pragma once

#include "FolderBrowser.h"

#include <QImage>
#include <QObject>
#include <QString>
#include <QThreadPool>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

class ThumbnailWorker;

// The sidebar's pictures, 256 px on the long side (ThumbnailStore,
// FolderBrowser.swift).
//
// A picture with no settings of its own shows the camera's preview, read out
// of the file. One with settings of its own shows its print: from the
// preview on screen when it was last there, from the disk cache, or made in
// the background on a renderer of its own, one at a time. A clip shows a
// frame a tenth of the way in (at most a second), as a player shows log
// footage; one with a look of its own, that frame graded with it, metered
// for its stock as Auto exposure meters the clip. Only the rows on screen
// ask, and at most `limit` pictures are kept in memory.
class ThumbnailStore : public QObject
{
    Q_OBJECT
public:
    static constexpr int size = 256;

    ThumbnailStore(FolderBrowser::Kind kind, const AppData &data, QObject *parent = nullptr);
    ~ThumbnailStore() override;

    FolderBrowser::Kind kind() const { return m_kind; }
    // The picture of `path`, null while there is none yet.
    QImage image(const QString &path) const;
    // The same as the screen is given it: the monitor's colour where it has a
    // profile of its own (DisplayColour), converted once for each.
    QImage shown(const QString &path) const;
    int count() const { return int(m_images.size()); }

    // Show `path`'s picture: its print, when it has settings of its own
    // (`edits`), otherwise the camera's preview.
    void request(const QString &path, const std::optional<Look> &edits);
    // Show a clip's frame: graded with its own look, if it has one, made in
    // the background and cached as a picture's print is; the frame as a
    // player shows it meanwhile, and for a clip with no look of its own.
    void requestClip(const QString &path, const std::optional<MotionLook> &grade);
    // The print on screen, shrunk, for the picture it is of: no render of
    // its own. Kept in the disk cache too.
    void setPrint(const QImage &image, const QString &path, const Look &edits);
    // The prints of pictures whose settings changed elsewhere than on screen
    // (Paste Settings to Selected), and clips' graded frames likewise.
    void refresh(const std::vector<std::pair<QString, Look>> &jobs);
    void refreshClips(const std::vector<std::pair<QString, MotionLook>> &jobs);
    void forgetAll();
    // Short of memory: keep only the most recently shown.
    void trim(int keep = 60);
    int limit = 400;

    // The key of a print: the file as it is on disk, the settings it is
    // printed with, and the app that prints it.
    static QString printKey(const QString &path, const Look &edits);
    static QString printKey(const QString &path, const MotionLook &grade);
    // `image` at `size` px on its long side.
    static QImage shrink(const QImage &image, int size = ThumbnailStore::size);
    // The camera's own preview of a file, from its embedded JPEG where it has
    // one, upright; a picture already rendered, read small.
    static QImage cameraPreview(const QString &path);
    static QString cacheDirectory();
    // Where the tests keep their prints instead.
    static inline QString cacheDirectoryOverride;
    static QImage loadCached(const QString &key);
    static void saveCached(const QImage &image, const QString &key);

signals:
    void imageChanged(const QString &path);
    // Pictures were let go: the rows that show them ask again.
    void trimmed();

private:
    void touch(const QString &path);
    void put(QImage image, const QString &path, const QString &key);
    void readPreview(const QString &path);
    void makePrint(const QString &path, const Look &edits, const QString &key);

    FolderBrowser::Kind m_kind;
    AppData m_data;
    std::map<QString, QImage> m_images;
    // converted for the screen, by the DisplayColour generation they were made for
    mutable std::map<QString, std::pair<int, QImage>> m_shown;
    // the print each picture shows, by its key; none for a camera preview
    std::map<QString, QString> m_shownPrint;
    std::set<QString> m_reading;
    std::map<QString, int> m_lastUsed;
    int m_clock = 0;
    std::set<QString> m_printing;
    QThreadPool m_previews;
    std::unique_ptr<ThumbnailWorker> m_worker;
};
