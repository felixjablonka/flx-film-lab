#pragma once

#include "Packs.h"
#include "Presets.h"
#include "EditHistory.h"
#include "FolderBrowser.h"
#include "ImageLibrary.h"
#include "Recipe.h"
#include "RenderEngine.h"

#include <QImage>
#include <QPointF>
#include <QSize>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

// The open image, its edits, and the loop that keeps the canvas in step with
// the controls (DocumentModel.swift).
//
// Rendering runs on RenderEngine, on a thread of its own, so a slider drag
// never waits for the GPU: this side records the new value and shows each
// frame as it arrives. Edits render a preview sized for the screen; export
// decodes again at full size.
//
// Opening another image:
// - The image on screen keeps everything until the new one has decoded, so
//   a file that fails to open changes nothing.
// - Each image's edits are remembered for the session, and its own settings
//   and flag between sessions, in the library (ImageLibrary).
// - An image opened for the first time keeps the look (film, paper, film
//   character, output) while everything dialled for one frame starts fresh.
//
// The sidebar lists the folder of the image opened (FolderBrowser); the
// pictures either side of the one on screen are decoded ahead, so stepping
// to one shows it at once.
class Document : public QObject
{
    Q_OBJECT
public:
    Document(const AppData &data, ImageLibrary &library, QObject *parent = nullptr);
    ~Document() override;

    const Recipe &recipe() const { return m_edits.recipe; }
    void setRecipe(const Recipe &recipe);
    WhiteBalance whiteBalance() const { return m_edits.whiteBalance; }
    void setWhiteBalance(WhiteBalance wb);
    double customTemperature() const { return m_edits.customTemperature; }
    double customTint() const { return m_edits.customTint; }
    void setCustomTemperature(double kelvin);
    void setCustomTint(double tint);

    // the print in sRGB, and as the screen is given it (DisplayColour)
    const QImage &displayImage() const { return m_display; }
    const QImage &shownImage() const { return m_shown; }
    // the monitor's colour has changed: the print and the loupe made again for it
    void screenChanged();
    // the print's histogram as it is on screen, and before its curves
    const PrintHistogram &histogram() const { return m_histogram; }
    QString status() const { return m_status; }
    // the picture on screen, as the photograph's settings say it (the status
    // between other notes): camera, focal length, shutter speed, ISO
    QString statusLine() const;
    QString errorMessage() const { return m_error; }
    void clearError();
    QString filePath() const { return m_file; }
    bool isRendering() const { return m_renderInFlight; }
    bool isDecoding() const { return m_isDecoding; }
    // A decode or an open that has taken long enough to show, 0.3 s: what the
    // toolbar, the status line and the white balance's note wait for, so that
    // stepping through pictures decoded ahead, a few ms each, does not flash
    // them. Export and Copy Settings still refuse for as long as one is under
    // way (canExport, canCopySettings).
    bool isBusy() const { return m_isBusy; }
    // A render under way for 0.3 s or more, for the status line.
    bool isRenderingSlowly() const { return m_renderInFlight && m_renderSlow; }
    bool isExporting() const { return m_isExporting; }
    double lastRenderMilliseconds() const { return m_lastMilliseconds; }
    QString lastRenderBreakdown() const { return m_lastBreakdown; }
    const Metered &metered() const { return m_metered; }
    // The recipe as last rendered, the meters' choices in: what the curve
    // plot draws.
    const std::optional<Recipe> &resolvedRecipe() const { return m_resolved; }

    // The preview's long side, one of previewSizes; kept between launches.
    int previewSize() const { return m_previewSize; }
    // `keep`: kept between launches, as the inspector's choice is
    void setPreviewSize(int pixels, bool keep = true);
    static const std::vector<int> &previewSizes();

    bool hasImage() const { return m_loaded.has_value(); }
    // Export renders the image on screen with the values on screen, so it
    // waits until nothing is still being decoded.
    bool canExport() const { return m_loaded && m_pending.isEmpty() && !m_isDecoding && !m_isExporting; }
    bool canCopySettings() const { return !m_file.isEmpty() && m_pending.isEmpty(); }
    bool canPasteSettings() const { return m_copied.has_value() && canCopySettings(); }
    QString copiedSource() const { return m_copied ? m_copied->second : QString(); }

    PackLibrary &packs() { return *m_packs; }
    const ProfilePack *filmPack();
    const ProfilePack *paperPack();
    double defaultGrade();

    void open(const QString &path);
    // The film picker's choice: crossing between colour and black and white
    // also moves the print onto the new film's own paper (Recipe.loadingFilm).
    void selectFilm(const std::string &stock);

    void copySettings();
    void pasteSettings();

    const std::vector<Preset> &presets() const { return m_presets; }
    void reloadPresets();
    void savePreset(const QString &name);
    void applyPreset(const Preset &preset);
    void deletePreset(const QString &name);
    QString presetsFolder();

    // What the decode is balanced for, so the setting is never a guess.
    QString whiteBalanceSummary();
    double filmReferenceTemperature();
    // The stock read off a light table rather than printed.
    bool isTransparency();
    bool printsOnPaper();
    bool printsInSilver();

    void exportTo(const QString &path, ExportFormat format);

    // MARK: crop, turn and level
    // While on, the canvas shows the whole frame, turned and levelled, with
    // the crop drawn on it. The meters hold the reading they had until it is
    // done, so the print does not change under the crop while it is drawn.
    bool isCropping() const { return m_isCropping; }
    void setCropping(bool on);
    // the level tool: the next line drawn on the frame levels it
    bool isLevelling() const { return m_isLevelling; }
    void setLevelling(bool on);
    // keeping the crop, or going back to the one from before
    void endCropping(bool keep);
    // the frame as it stands turned, in the preview's pixels: what the crop's
    // proportions are worked out on
    std::optional<PointD> turnedFrame() const;
    void editGeometry(const std::function<void(FrameGeometry &, PointD)> &change);
    void turn(bool clockwise);
    void flip(bool horizontal);
    void setCropAspect(CropAspect aspect);
    void swapCropOrientation();
    void setCustomAspect(double width, double height);
    void setStraighten(double degrees);
    // levelled along a line from `a` to `b` on the frame as it shows while
    // cropping, normalised: the line comes out level, or upright if nearer it
    void level(PointD a, PointD b);
    void resetGeometry();
    // where the image on screen comes from in the frame
    const std::optional<GeometryMap> &displayMap() const { return m_displayMap; }

    // MARK: graduated exposure, dodge & burn
    // The mask or gradient whose handles show on the canvas: identifiers are
    // unique across both. A viewing state, never in the recipe.
    const std::string &selected() const { return m_selected; }
    // The masks' and gradients' lines hidden, to judge the print without them;
    // they go on acting. Hiding puts editing down; choosing a mask or gradient
    // to edit, or adding one, takes it up again, lines and all. A viewing
    // preference, kept between sessions.
    bool maskLinesHidden() const { return m_maskLinesHidden; }
    void setMaskLinesHidden(bool hidden);
    void setSelected(const std::string &id);
    void addGradient(bool oval = false);
    void addMask(MaskShape shape);
    void removeGradient(const std::string &id);
    void removeMask(const std::string &id);
    // points given as fractions of the print on screen, in the frame's own
    // normalised coordinates, as masks and gradients keep them: a new one
    // starts on what the crop keeps, the way up the print stands
    std::vector<PointD> framePoints(const std::vector<PointD> &points) const;

    void resetExposure();
    void resetTone();
    void resetColour();
    void resetFilmCharacter();

    // Undo and redo for the image on screen (EditHistory): one step per
    // gesture, each image with its own history for the session.
    EditHistory<Look> &history() { return *m_history; }

    // Original (\): the picture before film and paper, as Apple renders the
    // RAW by default (AppleRendering), as the Mac app's Original shows it: in
    // the print's place, turned, levelled and cropped as the print is. Decoded
    // the first time it is asked for, with the camera's white balance, for the
    // picture on screen alone, and placed again when the print's shape
    // changes; nothing at all until then.
    bool showOriginal() const { return m_showOriginal; }
    void setShowOriginal(bool on);
    // The original in the print's place as the screen is given it; null while
    // it is on its way (the print shows meanwhile).
    const QImage &originalShown() const { return m_originalShown; }
    const QImage &original() const { return m_original; }

    // The loupe (Ctrl+L): the print at 100%, one pixel of the export to one
    // of the screen, under a square dragged over the print. While it is open
    // the picture is held at full size, decoded as the export decodes it,
    // and only the part under the loupe is printed from it.
    bool loupeOn() const { return m_loupeOn; }
    void setLoupeOn(bool on);
    // the loupe's centre, in fractions of the print as it shows
    QPointF loupeCentre() const { return m_loupeCentre; }
    void moveLoupe(QPointF centre);
    // the loupe's size in pixels of the screen, one to a pixel of the print
    void setLoupeSize(QSize pixels);
    const QImage &loupeImage() const { return m_loupeImage; }
    bool loupeLoading() const { return m_loupeLoading; }
    // reading the picture or printing the loupe's part: for the tests
    bool loupeBusy() const { return m_loupeLoading || m_loupeInFlight || m_loupeAgain; }

    // MARK: the folder, flags and batches
    FolderBrowser &browser() { return m_browser; }
    ImageLibrary &library() { return m_library; }
    // A folder's pictures in the sidebar, and the first of them on screen,
    // unless the picture on screen is one of them.
    void openFolder(const QString &folder);
    // Flag the selection, or the picture on screen when nothing is selected:
    // P, X and U in the sidebar.
    void flagSelection(std::optional<ImageLibrary::Flag> flag);
    // those pictures alone, the selection as it is (a row's button)
    void flagPaths(const std::vector<QString> &paths, std::optional<ImageLibrary::Flag> flag);
    // Step to the picture `offset` places along the sidebar.
    void showNeighbour(int offset);
    // The copied settings onto every picture selected but the rejects, as
    // Paste Settings puts them on the one on screen: each keeps its own masks,
    // gradients and crop, and has the paste as a step of its own history.
    void pasteSettingsToSelected();
    // Every picture selected but the rejects printed at full size into
    // `folder`, one after another, each with its own settings, or, never
    // opened, the look on screen with its autos as opening it would give it.
    void exportSelected(const QString &folder, ExportFormat format);
    void cancelBatch();
    bool isBatchExporting() const { return m_browser.batch().has_value(); }
    // The edits on screen into the library, and the library onto the disk.
    void prepareToQuit();
    // For the tests: the pictures decoded ahead, and the undo histories kept.
    std::set<QString> prefetchedPictures() const;
    int historiesHeld() const { return int(m_historyByImage.size()); }
    // Only the last 30 pictures visited keep their undo history, so a session
    // through hundreds of pictures does not hold hundreds of histories.
    static constexpr int historiesKept = 30;

signals:
    // Anything the controls, the toolbar or the status show.
    void changed();
    // A new frame for the canvas.
    void imageChanged();
    // A new picture under the loupe, or the loupe opened, moved or put away.
    void loupeChanged();

private:
    struct Loaded {
        DecodeKey key;
        int sourceID = 0;
        int width = 0, height = 0;
        QString camera;
        float focalLength = 0, shutter = 0, iso = 0;    // mm, seconds, ISO; 0 where the file has none
    };

    Look edits(const QString &path);
    DecodeKey decodeKey(const QString &path);
    void syncSource();
    void finishDecode(std::shared_ptr<const DecodedPicture> picture, const QString &error, const DecodeKey &key,
                      int generation);
    void adopt(const RenderEngine::Source &source, const DecodeKey &key);
    void put(const Look &look);
    // a whole set of edits at once, decoded and rendered again as need be
    void restore(const Look &look);
    // the edits are about to change: the history's step opens on them as they are
    void edited() { m_history->note([this] { return m_edits; }); }
    void scheduleRender();
    // the picture held at full size for the loupe as the image on screen
    // decodes, read again when it decodes otherwise
    void syncLoupe();
    void requestLoupe();
    void closeLoupe();
    void finishRender(std::optional<RenderEngine::Output> output, const QString &error, int sourceID);
    // MARK: the folder
    void showFolder(const QString &path);
    void list(const QString &folder);
    // A picture's own settings, if it has any: none for one that would open
    // with the look on screen.
    std::optional<Look> ownEdits(const QString &path);
    // The edits on screen go into the library once they have settled, if they
    // are its own: changed since it opened, or already kept.
    void scheduleRecord();
    void recordCurrent();
    // The print on screen, shrunk, as the sidebar's picture of a picture with
    // settings of its own.
    void keepPrint(const QString &path);
    // Decode the pictures either side of the one on screen at the preview's
    // size, on an engine of their own, once it has had its first frame; keep
    // no other decodes.
    void prefetchNeighbours();
    void prefetchNext(std::vector<DecodeKey> keys, int generation);
    void updateBusy();
    void finishBatch(const QStringList &failures, int total, bool cancelled, const QString &folder);
    // Runs `work` on the engine's thread.
    template<typename F>
    void onEngine(F &&work);

    AppData m_data;
    std::unique_ptr<PackLibrary> m_packs;      // this thread's
    std::unique_ptr<RenderEngine> m_engine;    // used on m_thread alone
    QThread m_thread;
    QObject *m_engineContext = nullptr;        // lives on m_thread

    Look m_edits;
    QImage m_display, m_shown;
    PrintHistogram m_histogram;
    QString m_status = QStringLiteral("Open a RAW file to begin");
    QString m_error;
    QString m_file;                            // the image on screen
    QString m_pending;                         // opened, not yet decoded
    std::optional<Loaded> m_loaded;
    std::optional<DecodeKey> m_requested;      // the decode in flight
    // Every white balance change decodes again, and a drag asks once a tick:
    // the decode asked for last wins, not the one that finished last.
    std::atomic<int> m_generation{0};
    bool m_isDecoding = false;
    bool m_isExporting = false;
    bool m_renderInFlight = false;
    bool m_renderAgain = false;
    bool m_applyingEdits = false;
    double m_lastMilliseconds = 0;
    QString m_lastBreakdown;
    Metered m_metered;
    std::optional<Recipe> m_resolved;
    std::map<QString, Look> m_editsByImage;
    std::unique_ptr<EditHistory<Look>> m_history;
    // every other image's undo history; the one on screen's is m_history's
    std::map<QString, EditHistory<Look>::Stacks> m_historyByImage;
    std::optional<EditHistory<Look>::Mark> m_historyBeforeCropping;
    bool m_loupeOn = false, m_loupeLoading = false, m_loupeInFlight = false, m_loupeAgain = false;
    QPointF m_loupeCentre{0.5, 0.5};
    QSize m_loupeSize;
    QImage m_loupeImage;
    std::optional<DecodeKey> m_fullFrameKey;    // what the picture held at full size, or on its way, decodes with
    int m_loupeGeneration = 0;                  // a read made stale by a newer one, or by putting the loupe away
    std::optional<std::pair<Look, QString>> m_copied;
    PresetStore m_presetStore;
    std::vector<Preset> m_presets;
    int m_previewSize = 2560;
    bool m_isCropping = false, m_isLevelling = false;
    std::optional<FrameGeometry> m_shapeBeforeCropping;    // what the meters read meanwhile, and Cancel's
    std::optional<GeometryMap> m_displayMap;
    // the original (showOriginal): the picture's, whole, at the preview's
    // size, sRGB; and placed on the print's shape
    void requestOriginal();
    void placeOriginal();
    bool m_showOriginal = false;
    struct OriginalFrame {
        QString path;
        int previewSize = 0;
        QImage image;
    };
    std::optional<OriginalFrame> m_originalFrame;
    std::optional<GeometryMap> m_originalMap;   // what m_original was placed on
    QImage m_original, m_originalShown;
    int m_originalGeneration = 0;
    QString m_originalDecoding;                 // the path whose original is on its way
    std::string m_selected;
    bool m_maskLinesHidden = false;

    ImageLibrary &m_library;
    FolderBrowser m_browser;
    // What each picture opened with this session: one whose edits still equal
    // these has nothing of its own to keep in the library.
    std::map<QString, Look> m_openedWith;
    // The picture on screen and the two either side of it in the sidebar,
    // decoded at the preview's size.
    std::vector<std::pair<DecodeKey, std::shared_ptr<const DecodedPicture>>> m_decoded;
    std::atomic<int> m_prefetchGeneration{0};
    QThread m_prefetchThread;
    QObject *m_prefetchContext = nullptr;
    std::unique_ptr<RenderEngine> m_prefetchEngine;     // used on m_prefetchThread alone
    // the pictures visited, the most recent last (historiesKept)
    std::vector<QString> m_visited;
    QTimer m_recordTimer, m_printTimer, m_busyTimer, m_renderSlowTimer;
    bool m_isBusy = false, m_renderSlow = false;
    std::shared_ptr<std::atomic<bool>> m_batchCancel;
    QPointer<QThread> m_batchThread;
};
