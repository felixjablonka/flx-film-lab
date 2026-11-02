#include "ClipAudio.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

std::string error(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return text;
}

[[noreturn]] void fail(const std::string &path, const std::string &why)
{
    throw std::runtime_error("Cannot play the sound of " + path + ": " + why);
}

} // namespace

struct ClipAudio::Impl {
    std::string path;
    AVFormatContext *fmt = nullptr;
    AVCodecContext *dec = nullptr;
    SwrContext *swr = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    int stream = -1;
    AVRational timeBase{1, 1};
    double origin = 0;              // the video track's first time, which the picture counts from
    int rate = 48000, channels = 2;
    AVChannelLayout layout{};
    std::vector<float> pending;     // converted, not yet read
    size_t taken = 0;
    double skipUntil = -1;          // after a seek: the sound before this is decoded, not played
    bool drained = false, flushed = false;

    ~Impl()
    {
        swr_free(&swr);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        av_channel_layout_uninit(&layout);
    }

    void resampler()
    {
        swr_free(&swr);
        AVChannelLayout in{};
        if (dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&in, dec->ch_layout.nb_channels);
        else
            av_channel_layout_copy(&in, &dec->ch_layout);
        const int e = swr_alloc_set_opts2(&swr, &layout, AV_SAMPLE_FMT_FLT, rate, &in, dec->sample_fmt,
                                          dec->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&in);
        if (e < 0 || swr_init(swr) < 0)
            fail(path, "its sound cannot be converted for the speakers");
    }

    // The decoded frame, converted and appended; the part before a seek's
    // target dropped.
    void convert(const AVFrame *f)
    {
        const int room = swr_get_out_samples(swr, f ? f->nb_samples : 0);
        if (room <= 0)
            return;
        std::vector<float> out(size_t(room) * channels);
        uint8_t *to = reinterpret_cast<uint8_t *>(out.data());
        const int made = swr_convert(swr, &to, room, f ? const_cast<const uint8_t **>(f->extended_data) : nullptr,
                                     f ? f->nb_samples : 0);
        if (made <= 0)
            return;
        size_t first = 0;
        if (f && skipUntil >= 0) {
            const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
            const double t = ts == AV_NOPTS_VALUE ? skipUntil : double(ts) * av_q2d(timeBase) - origin;
            const double skip = (skipUntil - t) * rate;
            if (skip >= made)
                return;
            first = skip > 0 ? size_t(std::lround(skip)) : 0;
            skipUntil = -1;
        }
        pending.insert(pending.end(), out.begin() + std::ptrdiff_t(first * channels),
                       out.begin() + std::ptrdiff_t(size_t(made) * channels));
    }

    // One more frame's worth converted; false at the end.
    bool decodeMore()
    {
        for (;;) {
            const int r = avcodec_receive_frame(dec, frame);
            if (r == 0) {
                convert(frame);
                av_frame_unref(frame);
                return true;
            }
            if (r == AVERROR_EOF) {
                if (!flushed) {
                    flushed = true;
                    convert(nullptr);
                    return true;
                }
                return false;
            }
            if (r != AVERROR(EAGAIN))
                return false;   // a damaged packet ends the sound, not the picture
            if (drained)
                return false;
            if (av_read_frame(fmt, packet) < 0) {
                drained = true;
                avcodec_send_packet(dec, nullptr);
                continue;
            }
            if (packet->stream_index == stream)
                avcodec_send_packet(dec, packet);
            av_packet_unref(packet);
        }
    }
};

ClipAudio::ClipAudio(const std::string &path, int sampleRate, int channels)
    : m(std::make_unique<Impl>())
{
    m->path = path;
    m->rate = sampleRate;
    m->channels = std::max(channels, 1);
    av_channel_layout_default(&m->layout, m->channels);
    if (int e = avformat_open_input(&m->fmt, path.c_str(), nullptr, nullptr); e < 0)
        fail(path, error(e));
    if (int e = avformat_find_stream_info(m->fmt, nullptr); e < 0)
        fail(path, error(e));
    const AVCodec *codec = nullptr;
    m->stream = av_find_best_stream(m->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (m->stream < 0 || !codec)
        fail(path, "it has no sound this build can decode");
    for (unsigned i = 0; i < m->fmt->nb_streams; ++i)
        if (int(i) != m->stream)
            m->fmt->streams[i]->discard = AVDISCARD_ALL;
    const AVStream *s = m->fmt->streams[m->stream];
    m->timeBase = s->time_base;
    const int video = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video >= 0 && m->fmt->streams[video]->start_time != AV_NOPTS_VALUE)
        m->origin = double(m->fmt->streams[video]->start_time) * av_q2d(m->fmt->streams[video]->time_base);
    m->dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m->dec, s->codecpar);
    if (int e = avcodec_open2(m->dec, codec, nullptr); e < 0)
        fail(path, error(e));
    m->packet = av_packet_alloc();
    m->frame = av_frame_alloc();
    m->resampler();
}

ClipAudio::~ClipAudio() = default;

void ClipAudio::seek(double seconds)
{
    const double target = std::max(seconds, 0.0) + m->origin;
    av_seek_frame(m->fmt, m->stream, int64_t(std::floor(target / av_q2d(m->timeBase))), AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m->dec);
    m->resampler();
    m->pending.clear();
    m->taken = 0;
    m->skipUntil = std::max(seconds, 0.0);
    m->drained = m->flushed = false;
}

size_t ClipAudio::read(float *out, size_t frames)
{
    size_t done = 0;
    while (done < frames) {
        const size_t ready = (m->pending.size() - m->taken) / size_t(m->channels);
        if (ready == 0) {
            m->pending.clear();
            m->taken = 0;
            if (!m->decodeMore())
                break;
            continue;
        }
        const size_t n = std::min(ready, frames - done);
        std::memcpy(out + done * size_t(m->channels), m->pending.data() + m->taken, n * size_t(m->channels) * 4);
        m->taken += n * size_t(m->channels);
        done += n;
    }
    return done;
}
