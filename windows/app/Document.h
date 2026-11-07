#pragma once

#include "Packs.h"
#include "Presets.h"
#include "EditHistory.h"
#include "Recipe.h"
#include "RenderEngine.h"

#include <QImage>
#include <QPointF>
#include <QSize>
#include <QObject>
#include <QThread>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>

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
// - Each image's edits are remembered for the session.
// - An image opened for the first time keeps the look (film, paper, film
//   character, output) while everything dialled for one frame starts fresh.
class Document : public QObject
{
    Q_OBJECT
public:
    explicit Document(const AppData &data, QObject *parent = nullptr);
    ~Document() override;

    const Recipe &recipe() const { return m_edits.recipe; }
    void setRecipe(const Recipe &recipe);
    WhiteBalance whiteBalance() const { return m_edits.whiteBalance; }
    void setWhiteBalance(WhiteBalance wb);
    double customTemperature() const { return m_edits.customTemperature; }
    double customTint() const { return m_edits.customTint; }
    void setCustomTemperature(double kelvin);
    void setCustomTint(double tint);

    const QImage &displayImage() const { return m_display; }
    // the print's histogram as it is on screen, and before its curves
    const PrintHistogram &histogram() const { return m_histogram; }
    QString status() const { return m_status; }
    QString errorMessage() const { return m_error; }
    void clearError();
    QString filePath() const { return m_file; }
    bool isRendering() const { return m_renderInFlight; }
    bool isDecoding() const { return m_isDecoding; }
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
    void setSelected(const std::string &id);
    void addGradient();
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
    };

    Look edits(const QString &path);
    DecodeKey decodeKey(const QString &path);
    void syncSource();
    void finishDecode(std::shared_ptr<DecodedPicture> picture, const QString &error, const DecodeKey &key,
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
    QString statusLine() const;
    // Runs `work` on the engine's thread.
    template<typename F>
    void onEngine(F &&work);

    AppData m_data;
    std::unique_ptr<PackLibrary> m_packs;      // this thread's
    std::unique_ptr<RenderEngine> m_engine;    // used on m_thread alone
    QThread m_thread;
    QObject *m_engineContext = nullptr;        // lives on m_thread

    Look m_edits;
    QImage m_display;
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
    std::string m_selected;
};
