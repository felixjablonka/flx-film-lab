#pragma once

#include "Motion.h"
#include "Presets.h"

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

// Each picture's own settings and flag, and each clip's look, kept between
// launches (ImageLibrary.swift).
//
// They live in the app's library in %APPDATA%\FLX Film Lab\Library, one file
// per folder, so nothing is written into the folders the pictures are in.
// With Save Settings Beside Images on, each record also goes into a small
// file beside its picture, DSC_0871.NEF.flx, which travels with the folder:
// to another drive, another PC, or the Mac app, which reads and writes the
// same files. Records carry the time they were made, and of a library record
// and a sidecar the newer counts. A sidecar is read wherever there is one,
// whatever the setting.
//
// A folder that has moved, or been renamed, is found again by its files'
// names and sizes: a library file whose files match at least half of the
// folder's is taken up for it, and left in place for the folder it was.
//
// Main thread only; the files are written on a thread of its own.
class ImageLibrary : public QObject
{
public:
    // Culling: a pick, or a reject, which batch actions leave out.
    enum class Flag { Pick, Reject };

    // What one picture or clip keeps: a picture its settings, a clip its
    // look. A record that has been cleared stays, empty, so that an older
    // sidecar cannot bring back what it cleared.
    struct Record {
        QDateTime modified;                 // UTC, to the second, as both apps write it
        std::optional<Flag> flag;
        std::optional<Look> edits;          // a picture's settings of its own
        std::optional<MotionLook> look;     // a clip's grade, without how files are written
        QString app;                        // what wrote it: "FLX Film Lab 1.6.0"
        bool operator==(const Record &) const = default;
    };

    // `directory` holds the library's files; the tests keep theirs apart.
    explicit ImageLibrary(QString directory = standardDirectory(), QObject *parent = nullptr);
    ~ImageLibrary() override;

    static QString standardDirectory();
    static QString sidecarPath(const QString &file) { return file + QStringLiteral(".flx"); }
    // The Save Settings Beside Images preference's key in the app's settings.
    static constexpr const char *sidecarsKey = "saveSidecars";

    // Save Settings Beside Images. Turned on, it writes the sidecars of every
    // record of the folders read so far.
    bool savesSidecars() const;
    void setSavesSidecars(bool on);

    // The records of `folder`'s files, `files` with their sizes, taking in
    // the sidecars beside them where they are newer.
    std::map<QString, Record> load(const QString &folder, const std::vector<std::pair<QString, qint64>> &files);
    // The record of one picture or clip, from its folder as read.
    std::optional<Record> record(const QString &file);

    // Change one record. It is written to the library once changes settle,
    // and to its sidecar at once when sidecars are on.
    void update(const QString &file, const std::function<void(Record &)> &change);
    // Write every folder changed since the last save; `wait` returns once
    // they are on disk (quitting, the tests).
    void save(bool wait = false);
    // Wait for every sidecar and library file sent off to be written.
    void finishWriting();
    // How long changes wait before they are written, so a slider dragged
    // writes once it has settled.
    int saveDelayMs = 1000;

    static QJsonObject recordToJson(const Record &record);
    static std::optional<Record> recordFromJson(const QJsonObject &o);
    static std::optional<Record> readSidecar(const QString &path);
    // The app's name and version, as a record says what wrote it.
    static QString software();

private:
    struct Folder {
        QString path;
        std::map<QString, Record> records;      // by file name
        std::map<QString, qint64> sizes;
    };

    static QString folderKey(const QString &folder);
    QString folderFile(const QString &key) const;
    std::optional<Folder> readFolderFile(const QString &key) const;
    std::optional<Folder> relocated(const QString &key, const std::vector<std::pair<QString, qint64>> &files);
    void scheduleSave();
    void writeSidecar(const Record &record, const QString &file);
    // `work` on the writer's thread, in order
    void write(std::function<void()> work, bool wait = false);

    QString m_directory;
    std::map<QString, Folder> m_folders;        // read so far, by path
    std::set<QString> m_unsaved;
    QTimer m_saveTimer;
    QThread m_writer;
    QObject *m_writerContext = nullptr;
};
