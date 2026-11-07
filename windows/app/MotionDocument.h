#pragma once

#include "AudioPlayback.h"
#include "EditHistory.h"
#include "MotionEngine.h"
#include "RenderEngine.h"
#include "SetupBuilder.h"

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <vector>

// A movie, by its name: what opens in the Motion mode (AppMode.opening).
bool isMovie(const QString &path);
// The movies the open dialog offers, as a filter.
QString movieFilter();

// The open clip, its grade, and everything that keeps the preview in step
// (ClipModel.swift).
//
// A control change re-bakes the print look (about 15 ms, on the engine's
// thread) and redraws the frame on screen; nothing is decoded again. Export
// bakes the look once more from exactly the settings it was started with and
// writes every frame through the same pipeline at full size, on a thread and
// a GPU device of its own, so the preview carries on meanwhile.
//
// Opening another clip follows the Stills mode's rule for photographs: the
// clip on screen stays until the new one has loaded, each clip's grade is
// remembered for the session, and a clip opened for the first time keeps the
// look (film, paper, film character, effects, how footage is read and
// written) while exposure, tone and colour start fresh.
class MotionDocument : public QObject
{
    Q_OBJECT
public:
    explicit MotionDocument(const AppData &data, QObject *parent = nullptr);
    ~MotionDocument() override;

    const MotionLook &look() const { return m_look; }
    void setLook(const MotionLook &look);
    const std::optional<ClipInfo> &clip() const { return m_clip; }
    QString clipPath() const { return m_clipPath; }
    bool isOpening() const { return m_isOpening; }
    bool isRendering() const { return m_renderInFlight; }
    QString status() const { return m_status; }
    QString errorMessage() const { return m_error; }
    void clearError();
    const std::optional<double> &exportProgress() const { return m_exportProgress; }
    bool isExporting() const { return m_exportProgress.has_value(); }
    // How the frame on screen is being read, as the inspector reports it.
    const std::optional<FrameReading> &reading() const { return m_reading; }
    double drawMilliseconds() const { return m_drawMilliseconds; }
    // What Auto exposure reads for this clip on this stock: none until the
    // clip has been metered.
    const std::optional<double> &meteredExposure() const { return m_metered; }
    bool isMetering() const { return m_isMetering; }
    bool showLog() const { return m_showLog; }
    void setShowLog(bool on);

    // The graded frame, the clip's way up, and how far the track turns it.
    const QImage &image() const { return m_image; }
    int quarterTurns() const { return m_clip ? m_clip->quarterTurns : 0; }
    // The size the canvas shows the frame at, in device pixels, the clip's
    // way up: the preview is graded at that size, never above the clip's own.
    void setViewSize(QSize graded);

    // Under the pointer, `sourcePixel` in the clip's own pixels: the code
    // values recorded there, as IRE, and the light they decode to as stops
    // from an 18% grey. None when the pointer leaves.
    void probe(std::optional<QPointF> sourcePixel);
    QString probeText() const { return m_probeText; }

    // The recipe as it prints: with Auto exposure on, the meter's reading in
    // place of the stored exposure.
    Recipe effectiveRecipe() const;
    // The log format the clip on screen is read as: the inspector's choice,
    // or what the clip says it is.
    LogFormat format() const;
    // How frames are read, with the format resolved.
    InputSettings readingSettings() const;

    PackLibrary &packs() { return *m_packs; }
    const ProfilePack *filmPack();
    const ProfilePack *paperPack();
    QString filmName();
    QString paperName();
    double defaultGrade();
    bool printsOnPaper();
    bool printsInSilver();

    void open(const QString &path);
    // The stock picker's choice (Recipe.loadingFilm).
    void selectFilm(const std::string &stock);
    void resetExposure();
    void resetTone();
    void resetColour();
    void resetEffects();
    // Undo and redo for the clip on screen (EditHistory): one step per
    // gesture, each clip with its own history for the session.
    EditHistory<MotionLook> &history() { return *m_history; }

    // MARK: playback (PlaybackController)
    bool isPlaying() const { return m_playing; }
    double time() const { return m_time; }
    double duration() const { return m_clip ? m_clip->duration : 0; }
    double frameRate() const { return m_clip ? m_clip->frameRate : 0; }
    void togglePlay();
    void play();
    void pause();
    void seek(double seconds);
    void step(int frames);
    // The sound, silent for tests; and how much of it has been handed to the
    // speakers since playback last started.
    void setMuted(bool muted) { m_audio.setMuted(muted); }
    double soundHanded() const { return m_audio.handedSeconds(); }

    QString suggestedExportName();
    void exportTo(const QString &path);
    void cancelExport();
    // The print as a .cube for DaVinci Resolve and other editors, Rec.709
    // BT.1886 out, taking the clip's log the way Resolve reads it. Per-pixel
    // only: no effects.
    void exportLUT(const QString &path);
    QString suggestedLUTName();

signals:
    // Anything the controls or the toolbar show.
    void changed();
    // A new frame for the canvas.
    void frameChanged();
    // The status line, the probe, the export's progress.
    void statusChanged();
    // The playhead, as it plays or is moved.
    void timeChanged();

private:
    void lookDidChange(const MotionLook &old);
    void store();
    void adopt(const ClipInfo &info, const QString &path, int id);
    void scheduleRender();
    void finishRender(std::optional<MotionEngine::Output> output, const QString &error);
    void meter();
    void finishMeter(int generation, const QString &path, const InputSettings &settings, std::optional<double> highlight);
    void updateMeteredExposure();
    void tick();
    double lastFrameTime() const;
    void runOnEngine(std::function<void()> work);

    AppData m_data;
    std::unique_ptr<PackLibrary> m_packs;      // this thread's
    std::unique_ptr<SetupBuilder> m_builder;
    std::unique_ptr<MotionEngine> m_engine;    // used on m_thread alone
    QThread m_thread;
    QObject *m_engineContext = nullptr;

    MotionLook m_look;
    std::optional<ClipInfo> m_clip;
    QString m_clipPath;
    int m_clipID = 0, m_clipCounter = 0;
    bool m_isOpening = false;
    QString m_status = QStringLiteral("Open a log clip to begin");
    QString m_error;
    std::optional<FrameReading> m_reading;
    double m_drawMilliseconds = 0;
    QElapsedTimer m_statusClock;
    std::optional<double> m_metered;
    bool m_isMetering = false;
    bool m_showLog = false;
    std::map<QString, MotionLook> m_edits;
    std::unique_ptr<EditHistory<MotionLook>> m_history;
    // every other clip's undo history; the one on screen's is m_history's
    std::map<QString, EditHistory<MotionLook>::Stacks> m_histories;
    // a look without how its files are written, which is a preference
    MotionLook grade() const
    {
        MotionLook g = m_look;
        g.exportSettings = {};
        return g;
    }
    // each clip's metered highlight, by how it was read: decoding frames for
    // it is the slow part, the exposure for a stock is arithmetic
    std::map<QString, std::pair<InputSettings, std::optional<double>>> m_highlights;
    int m_meterGeneration = 0;
    std::vector<QPointer<QThread>> m_workers;  // meters and the export, waited for on the way out

    QImage m_image;
    std::shared_ptr<const VideoFrame> m_frame;
    QSize m_viewSize;
    bool m_renderInFlight = false, m_renderAgain = false;
    std::optional<QPointF> m_probePoint;
    QString m_probeText;

    bool m_playing = false;
    double m_time = 0, m_playFrom = 0;
    AudioPlayback m_audio;
    QElapsedTimer m_playClock;
    QTimer m_ticker;

    std::optional<double> m_exportProgress;
    std::atomic<bool> m_cancelExport{false};
};
