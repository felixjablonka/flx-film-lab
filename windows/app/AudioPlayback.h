#pragma once

#include <QAudioFormat>
#include <QObject>
#include <QString>

#include <memory>

class QAudioSink;

// The clip's sound with the picture (AVPlayer's part in the Mac's
// PlaybackController): started where playback starts, stopped when it
// pauses, started again where a seek lands. The picture keeps its own clock;
// the sound runs on the speakers' from the same moment, so the two stay
// together to the speakers' latency, a few hundredths of a second.
class AudioPlayback : public QObject
{
    Q_OBJECT
public:
    explicit AudioPlayback(QObject *parent = nullptr);
    ~AudioPlayback() override;

    // The clip whose sound plays; one without sound plays none.
    void setClip(const QString &path, bool hasSound);
    // Play from `seconds` into the clip, counted as the picture's frames are.
    void start(double seconds);
    void stop();
    // Silent, for tests that play without anyone listening.
    void setMuted(bool muted);
    // The sound handed to the speakers since the last start, in seconds.
    double handedSeconds() const;
    // What the speakers report having played since the last start, in
    // seconds; none before they run.
    double playedSeconds() const;

private:
    class Source;

    QString m_path;
    bool m_hasSound = false, m_muted = false;
    QAudioFormat m_format;
    std::unique_ptr<Source> m_source;
    std::unique_ptr<QAudioSink> m_sink;
};
