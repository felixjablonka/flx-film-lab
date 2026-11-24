#pragma once

#include "Packs.h"
#include "Provenance.h"

#include <QSettings>
#include <QString>

#include <string>

// The app's settings, kept between launches: on Windows under the
// registry's HKEY_CURRENT_USER\Software\FLX Film Lab. Main thread only.
QSettings &appSettings();

// A test run's settings in a file of their own instead (--library), so it
// never reads or changes the person's. Before the first appSettings().
void setAppSettingsFile(const QString &path);

// The films Settings hides from the Film menu (filmmenu::stored), read once
// and kept, as the inspectors ask on every change.
std::string hiddenFilms();
void setHiddenFilms(const std::string &stored);

// What a picture with no settings of its own opens with, in Stills (Settings,
// Film & Paper; the Mac's NewPictures): the look of the picture on screen, or
// the default film and paper. The first picture after launch, with none on
// screen, takes the defaults either way. Kept as the Mac keeps them.
enum class NewPictures { CarryLook, DefaultStocks };
NewPictures newPictures();
void setNewPictures(NewPictures choice);
// The default film and paper as set, empty where none is (startingRecipe
// falls back to Portra 400 and the film's own paper).
std::string defaultFilmSetting();
std::string defaultPaperSetting();
void setDefaultFilm(const std::string &stock);
void setDefaultPaper(const std::string &stock);

// How the paper follows a change of film (PaperRule), and the paper last used
// with each kind of film (PaperMemory), in Stills and Motion.
PaperRule paperRule();
void setPaperRule(PaperRule rule);
PaperMemory paperMemory();
void setPaperMemory(const PaperMemory &memory);

// How large Export and Export Selected write a print (the Mac's ExportSize),
// chosen beside the format in the toolbar: at its full size, or with its long
// edge a number of pixels, the short edge in proportion, never enlarged. The
// long edge is kept while the full size is chosen, to come back to. Kept
// under the Mac's names.
struct ExportSize {
    bool fullSize = true;
    int longEdge = 2048;

    // the long edge until another is set, and the ones that can be
    static constexpr int kStandardLongEdge = 2048, kFewest = 64, kMost = 65500;
    // the long edge to shrink to, 0 for the full size
    int limit() const { return fullSize ? 0 : longEdge; }
    // "Full Size" or "2048 px", as the toolbar shows it
    QString title() const;
    // after a format in a dialog's words: " at full size", ", 2048 px on the
    // long edge"
    QString phrase() const;
    bool operator==(const ExportSize &) const = default;
};
ExportSize exportSize();
void setExportSize(const ExportSize &size);

// What an exported picture's or clip's comment names (Settings, General; the
// Mac's ExportComment), under the Mac's names: all three unless set.
ExportComment exportComment();
void setExportComment(const ExportComment &comment);

// How the sidebars order a folder: by name, or by the date shot
// (FolderBrowser::Order), one setting for Stills and Motion, under the Mac's
// name and values.
bool foldersByDateShot();
void setFoldersByDateShot(bool byDate);
