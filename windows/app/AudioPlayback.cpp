#include "AudioPlayback.h"

#include "ClipAudio.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <vector>

// What the sink pulls from: the clip's sound decoded as it is asked for, in
// the speakers' format. The sink may pull from a thread of its own, so the
// decoder is behind a lock.
class AudioPlayback::Source : public QIODevice
{
public:
    Source(std::unique_ptr<ClipAudio> audio, const QAudioFormat &format)
        : m_audio(std::move(audio)), m_format(format)
    {
    }

    void startAt(double seconds)
    {
        std::lock_guard<std::mutex> hold(m_lock);
        m_audio->seek(seconds);
        m_handed = 0;
    }

    double handed() const
    {
        std::lock_guard<std::mutex> hold(m_lock);
        return double(m_handed) / std::max(m_format.sampleRate(), 1);
    }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 1 << 20; }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        std::lock_guard<std::mutex> hold(m_lock);
        const int channels = m_format.channelCount();
        const bool floats = m_format.sampleFormat() == QAudioFormat::Float;
        const qint64 frameBytes = qint64(channels) * (floats ? 4 : 2);
        const size_t frames = size_t(maxSize / frameBytes);
        if (frames == 0)
            return 0;
        size_t got;
        if (floats) {
            got = m_audio->read(reinterpret_cast<float *>(data), frames);
        } else {
            m_scratch.resize(frames * size_t(channels));
            got = m_audio->read(m_scratch.data(), frames);
            auto *to = reinterpret_cast<int16_t *>(data);
            for (size_t i = 0; i < got * size_t(channels); ++i)
                to[i] = int16_t(std::lround(std::clamp(m_scratch[i], -1.0f, 1.0f) * 32767.0f));
        }
        m_handed += got;
        // past the end, silence, so the sink idles rather than stops on us
        const qint64 written = qint64(got) * frameBytes;
        std::memset(data + written, 0, size_t(qint64(frames) * frameBytes - written));
        return qint64(frames) * frameBytes;
    }

    qint64 writeData(const char *, qint64) override { return -1; }

private:
    mutable std::mutex m_lock;
    std::unique_ptr<ClipAudio> m_audio;
    QAudioFormat m_format;
    std::vector<float> m_scratch;
    qint64 m_handed = 0;
};

AudioPlayback::AudioPlayback(QObject *parent)
    : QObject(parent)
{
}

AudioPlayback::~AudioPlayback()
{
    stop();
}

void AudioPlayback::setClip(const QString &path, bool hasSound)
{
    stop();
    m_sink.reset();
    m_source.reset();
    m_path = path;
    m_hasSound = hasSound;
}

void AudioPlayback::start(double seconds)
{
    if (!m_hasSound || m_path.isEmpty())
        return;
    if (!m_source) {
        // the speakers' own rate and channels, in floats where they take them
        const QAudioDevice device = QMediaDevices::defaultAudioOutput();
        if (device.isNull())
            return;
        QAudioFormat format = device.preferredFormat();
        format.setChannelCount(std::clamp(format.channelCount(), 1, 2));
        format.setSampleFormat(QAudioFormat::Float);
        if (!device.isFormatSupported(format))
            format.setSampleFormat(QAudioFormat::Int16);
        try {
            auto audio = std::make_unique<ClipAudio>(m_path.toStdString(), format.sampleRate(), format.channelCount());
            m_source = std::make_unique<Source>(std::move(audio), format);
        } catch (const std::exception &e) {
            // the picture plays on without its sound
            std::fprintf(stderr, "%s\n", e.what());
            m_hasSound = false;
            return;
        }
        m_format = format;
        m_sink = std::make_unique<QAudioSink>(device, format);
        // a tenth of a second ahead: enough that a busy moment does not break
        // the sound, little enough that a pause stops it at once
        m_sink->setBufferSize(qsizetype(format.bytesForDuration(100'000)));
        m_sink->setVolume(m_muted ? 0.0 : 1.0);
        m_source->open(QIODevice::ReadOnly);
    }
    m_sink->stop();
    m_source->startAt(seconds);
    m_sink->start(m_source.get());
}

void AudioPlayback::stop()
{
    if (m_sink)
        m_sink->stop();
}

void AudioPlayback::setMuted(bool muted)
{
    m_muted = muted;
    if (m_sink)
        m_sink->setVolume(muted ? 0.0 : 1.0);
}

double AudioPlayback::playedSeconds() const
{
    return m_sink && m_sink->state() != QAudio::StoppedState ? double(m_sink->processedUSecs()) / 1e6 : 0;
}

double AudioPlayback::handedSeconds() const
{
    return m_source ? m_source->handed() : 0;
}
