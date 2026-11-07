#include "ClipExport.h"

#include "SetupBuilder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

namespace {

std::string error(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return text;
}

[[noreturn]] void cannotRead(const std::string &why)
{
    throw std::runtime_error("Could not read the clip: " + why);
}

[[noreturn]] void cannotWrite(const std::string &why)
{
    throw std::runtime_error("Could not write the export: " + why);
}

std::filesystem::path fsPath(const std::string &utf8)
{
    return std::filesystem::path(reinterpret_cast<const char8_t *>(utf8.c_str()));
}

struct InputCloser {
    void operator()(AVFormatContext *f) const { avformat_close_input(&f); }
};
struct OutputCloser {
    void operator()(AVFormatContext *f) const
    {
        if (f->pb)
            avio_closep(&f->pb);
        avformat_free_context(f);
    }
};
struct CodecCloser {
    void operator()(AVCodecContext *c) const { avcodec_free_context(&c); }
};
struct FrameCloser {
    void operator()(AVFrame *f) const { av_frame_free(&f); }
};
struct PacketCloser {
    void operator()(AVPacket *p) const { av_packet_free(&p); }
};
using Encoder = std::unique_ptr<AVCodecContext, CodecCloser>;

// What every encoder is given: the frame, its timing, and Rec.709's tags at
// video levels, chroma sited left as the export kernels make it.
struct Picture {
    int width = 0, height = 0;
    AVRational timeBase{1, 600}, frameRate{30, 1}, aspect{1, 1};
    bool globalHeader = false;
};

Encoder open(const char *name, AVPixelFormat format, const Picture &p,
             const std::function<void(AVCodecContext *)> &configure)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    if (!codec)
        return nullptr;
    Encoder c(avcodec_alloc_context3(codec));
    if (!c)
        return nullptr;
    c->width = p.width;
    c->height = p.height;
    c->pix_fmt = format;
    c->time_base = p.timeBase;
    c->framerate = p.frameRate;
    c->sample_aspect_ratio = p.aspect;
    c->color_primaries = AVCOL_PRI_BT709;
    c->color_trc = AVCOL_TRC_BT709;
    c->colorspace = AVCOL_SPC_BT709;
    c->color_range = AVCOL_RANGE_MPEG;
    c->chroma_sample_location = AVCHROMA_LOC_LEFT;
    c->thread_count = 0;
    if (p.globalHeader)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    configure(c.get());
    if (avcodec_open2(c.get(), codec, nullptr) < 0)
        return nullptr;
    return c;
}

// ProRes 422 HQ for editing and grading further: Apple's own vendor code, so
// every application takes it for Apple's.
Encoder openProRes(const Picture &p)
{
    return open("prores_ks", AV_PIX_FMT_YUV422P10LE, p, [](AVCodecContext *c) {
        av_opt_set(c->priv_data, "profile", "hq", 0);
        av_opt_set(c->priv_data, "vendor", "apl0", 0);
    });
}

// HEVC Main 10 for sharing, on the GPU's encoder where there is one. A graded
// image carries more fine detail than flat log footage, so it is given a
// little more than the source had (ClipExporter.videoSettings).
Encoder openHEVC(const Picture &p, double sourceDataRate)
{
    const int64_t rate = int64_t(std::clamp(sourceDataRate * 1.25, 30e6, 300e6));
    const int gop = std::max(int(std::lround(av_q2d(p.frameRate))), 1);
    auto common = [rate, gop](AVCodecContext *c) {
        c->bit_rate = rate;
        c->rc_max_rate = rate * 2;
        c->rc_buffer_size = int(std::min<int64_t>(rate * 2, INT32_MAX));
        c->gop_size = gop;
        c->profile = AV_PROFILE_HEVC_MAIN_10;
    };
    struct Candidate {
        const char *name;
        AVPixelFormat format;
        std::function<void(AVCodecContext *)> options;
    };
    const Candidate candidates[] = {
        {"hevc_nvenc", AV_PIX_FMT_P010LE,
         [](AVCodecContext *c) {
             av_opt_set(c->priv_data, "preset", "p6", 0);
             av_opt_set(c->priv_data, "tune", "hq", 0);
             av_opt_set(c->priv_data, "rc", "vbr", 0);
             av_opt_set(c->priv_data, "profile", "main10", 0);
         }},
        {"hevc_amf", AV_PIX_FMT_P010LE,
         [](AVCodecContext *c) { av_opt_set(c->priv_data, "quality", "quality", 0); }},
        {"hevc_qsv", AV_PIX_FMT_P010LE,
         [](AVCodecContext *c) { av_opt_set(c->priv_data, "preset", "slower", 0); }},
        {"hevc_videotoolbox", AV_PIX_FMT_P010LE, [](AVCodecContext *) {}},
        {"libx265", AV_PIX_FMT_YUV420P10LE,
         [](AVCodecContext *c) { av_opt_set(c->priv_data, "preset", "fast", 0); }},
    };
    for (const Candidate &k : candidates)
        if (Encoder e = open(k.name, k.format, p, [&](AVCodecContext *c) {
                common(c);
                k.options(c);
            }))
            return e;
    return nullptr;
}

// The pipeline's planes, 10-bit codes in the low bits, into the encoder's
// frame: planar as they are, or P010's high bits with Cb and Cr interleaved.
void fill(AVFrame *f, const std::vector<uint16_t> planes[3], int width, int height, int chromaWidth,
          int chromaHeight)
{
    if (f->format == AV_PIX_FMT_P010LE) {
        for (int y = 0; y < height; ++y) {
            auto *row = reinterpret_cast<uint16_t *>(f->data[0] + size_t(y) * size_t(f->linesize[0]));
            const uint16_t *from = &planes[0][size_t(y) * width];
            for (int x = 0; x < width; ++x)
                row[x] = uint16_t(from[x] << 6);
        }
        for (int y = 0; y < chromaHeight; ++y) {
            auto *row = reinterpret_cast<uint16_t *>(f->data[1] + size_t(y) * size_t(f->linesize[1]));
            const uint16_t *cb = &planes[1][size_t(y) * chromaWidth], *cr = &planes[2][size_t(y) * chromaWidth];
            for (int x = 0; x < chromaWidth; ++x) {
                row[2 * x] = uint16_t(cb[x] << 6);
                row[2 * x + 1] = uint16_t(cr[x] << 6);
            }
        }
        return;
    }
    for (int c = 0; c < 3; ++c) {
        const int w = c == 0 ? width : chromaWidth, h = c == 0 ? height : chromaHeight;
        for (int y = 0; y < h; ++y)
            std::memcpy(f->data[c] + size_t(y) * size_t(f->linesize[c]), &planes[c][size_t(y) * w], size_t(w) * 2);
    }
}

// Container keys the muxer writes itself, or that would describe the source
// file rather than this one.
bool ownKey(const char *key)
{
    for (const char *k : {"major_brand", "minor_version", "compatible_brands", "encoder", "handler_name", "vendor_id"})
        if (std::strcmp(key, k) == 0)
            return true;
    return false;
}

void copyMetadata(AVDictionary **to, const AVDictionary *from)
{
    const AVDictionaryEntry *e = nullptr;
    while ((e = av_dict_iterate(from, e)))
        if (!ownKey(e->key))
            av_dict_set(to, e->key, e->value, 0);
}

} // namespace

std::string exportClip(QRhi *rhi, const std::string &source, const std::string &destination, const LookLUT &lut,
                       const GlowLook &effects, const InputSettings &input, LogFormat format,
                       const ClipExportOptions &options, const std::function<bool(double)> &progress)
{
    const ClipInfo info = readClipInfo(source);
    ClipReader reader(source);

    // the source again, for its sound, its tags and where its time starts
    AVFormatContext *rawIn = nullptr;
    if (int e = avformat_open_input(&rawIn, source.c_str(), nullptr, nullptr); e < 0)
        cannotRead(error(e));
    std::unique_ptr<AVFormatContext, InputCloser> in(rawIn);
    if (int e = avformat_find_stream_info(in.get(), nullptr); e < 0)
        cannotRead(error(e));
    const int videoIndex = av_find_best_stream(in.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0)
        cannotRead("no video track");
    const AVStream *videoIn = in->streams[videoIndex];
    // every track keeps its place against the others: the file's earliest
    // time becomes zero for all of them
    const int64_t offset = in->start_time != AV_NOPTS_VALUE ? in->start_time : 0;

    // written beside the destination, then put in its place
    const std::filesystem::path finalPath = fsPath(destination);
    std::filesystem::path partial = finalPath;
    partial += ".partial";
    const std::string partialName = destination + ".partial";
    AVFormatContext *rawOut = nullptr;
    if (int e = avformat_alloc_output_context2(&rawOut, nullptr, "mov", partialName.c_str()); e < 0 || !rawOut)
        cannotWrite(error(e));
    std::unique_ptr<AVFormatContext, OutputCloser> out(rawOut);
    bool opened = false;
    struct Cleanup {
        const std::filesystem::path &path;
        bool &opened;
        bool done = false;
        ~Cleanup()
        {
            if (opened && !done) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        }
    } cleanup{partial, opened};

    Picture picture;
    picture.width = info.width;
    picture.height = info.height;
    picture.timeBase = videoIn->time_base;
    const AVRational rate = videoIn->avg_frame_rate.num ? videoIn->avg_frame_rate : videoIn->r_frame_rate;
    if (rate.num > 0 && rate.den > 0)
        picture.frameRate = rate;
    if (videoIn->codecpar->sample_aspect_ratio.num > 0)
        picture.aspect = videoIn->codecpar->sample_aspect_ratio;
    picture.globalHeader = (out->oformat->flags & AVFMT_GLOBALHEADER) != 0;
    Encoder encoder = options.codec == ExportCodec::ProRes422HQ ? openProRes(picture)
                                                                 : openHEVC(picture, info.dataRate);
    if (!encoder)
        cannotWrite(options.codec == ExportCodec::ProRes422HQ ? "this build has no ProRes encoder"
                                                               : "no HEVC 10-bit encoder would open on this computer");
    const std::string encoderName = encoder->codec->name;

    AVStream *videoOut = avformat_new_stream(out.get(), nullptr);
    if (!videoOut)
        cannotWrite("cannot add the video track");
    avcodec_parameters_from_context(videoOut->codecpar, encoder.get());
    videoOut->time_base = encoder->time_base;
    videoOut->avg_frame_rate = picture.frameRate;
    if (options.codec == ExportCodec::HEVC10)
        videoOut->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');  // what Apple's players read
    // the source's rotation, as it asks on playback
    for (int i = 0; i < videoIn->codecpar->nb_coded_side_data; ++i) {
        const AVPacketSideData &sd = videoIn->codecpar->coded_side_data[i];
        if (sd.type != AV_PKT_DATA_DISPLAYMATRIX)
            continue;
        if (AVPacketSideData *copy = av_packet_side_data_new(&videoOut->codecpar->coded_side_data,
                                                             &videoOut->codecpar->nb_coded_side_data, sd.type, sd.size,
                                                             0))
            std::memcpy(copy->data, sd.data, sd.size);
    }
    copyMetadata(&videoOut->metadata, videoIn->metadata);

    // the sound, copied as it is, every track the container can carry
    std::vector<int> audioMap(in->nb_streams, -1);
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVStream *s = in->streams[i];
        if (s->codecpar->codec_type != AVMEDIA_TYPE_AUDIO
            || avformat_query_codec(out->oformat, s->codecpar->codec_id, FF_COMPLIANCE_NORMAL) != 1)
            continue;
        AVStream *o = avformat_new_stream(out.get(), nullptr);
        if (!o || avcodec_parameters_copy(o->codecpar, s->codecpar) < 0)
            cannotWrite("cannot add a sound track");
        o->codecpar->codec_tag = 0;
        // a track that does not say how its channels lie (a Zf's) is taken
        // for the usual layout of that many, so the file can say it
        if (o->codecpar->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC && o->codecpar->ch_layout.nb_channels > 0) {
            const int channels = o->codecpar->ch_layout.nb_channels;
            av_channel_layout_uninit(&o->codecpar->ch_layout);
            av_channel_layout_default(&o->codecpar->ch_layout, channels);
        }
        o->time_base = s->time_base;
        copyMetadata(&o->metadata, s->metadata);
        audioMap[i] = o->index;
    }
    for (unsigned i = 0; i < in->nb_streams; ++i)
        if (audioMap[i] < 0)
            in->streams[i]->discard = AVDISCARD_ALL;
    copyMetadata(&out->metadata, in->metadata);
    if (!options.software.empty())
        av_dict_set(&out->metadata, "com.apple.quicktime.software", options.software.c_str(), 0);
    if (!options.description.empty())
        av_dict_set(&out->metadata, "com.apple.quicktime.description", options.description.c_str(), 0);

    if (int e = avio_open(&out->pb, partialName.c_str(), AVIO_FLAG_WRITE); e < 0)
        cannotWrite(error(e));
    opened = true;
    AVDictionary *muxing = nullptr;
    // the camera's own keys (make, model, place, Apple's) as they came
    av_dict_set(&muxing, "movflags", "use_metadata_tags", 0);
    const int header = avformat_write_header(out.get(), &muxing);
    av_dict_free(&muxing);
    if (header < 0)
        cannotWrite(error(header));

    const double span = options.seconds > 0 ? std::min(options.seconds, info.duration) : info.duration;
    std::unique_ptr<AVPacket, PacketCloser> packet(av_packet_alloc()), sound(av_packet_alloc());
    bool soundDone = std::none_of(audioMap.begin(), audioMap.end(), [](int i) { return i >= 0; });
    double soundTime = -1e9;
    // sound up to `until` seconds, so the muxer interleaves it with the picture
    auto writeSound = [&](double until) {
        while (!soundDone && soundTime < until) {
            if (av_read_frame(in.get(), sound.get()) < 0) {
                soundDone = true;
                break;
            }
            const int target = audioMap[size_t(sound->stream_index)];
            if (target < 0) {
                av_packet_unref(sound.get());
                continue;
            }
            const AVStream *s = in->streams[sound->stream_index];
            const int64_t shift = av_rescale_q(offset, AV_TIME_BASE_Q, s->time_base);
            if (sound->pts != AV_NOPTS_VALUE)
                sound->pts -= shift;
            if (sound->dts != AV_NOPTS_VALUE) {
                sound->dts -= shift;
                soundTime = double(sound->dts) * av_q2d(s->time_base);
            }
            if (options.seconds > 0 && soundTime >= span) {
                av_packet_unref(sound.get());
                soundDone = true;
                break;
            }
            av_packet_rescale_ts(sound.get(), s->time_base, out->streams[target]->time_base);
            sound->stream_index = target;
            sound->pos = -1;
            if (int e = av_interleaved_write_frame(out.get(), sound.get()); e < 0)
                cannotWrite("the sound was refused: " + error(e));
        }
    };
    auto drain = [&](const AVFrame *frame) {
        if (int e = avcodec_send_frame(encoder.get(), frame); e < 0 && e != AVERROR_EOF)
            cannotWrite("a frame was refused: " + error(e));
        for (;;) {
            const int r = avcodec_receive_packet(encoder.get(), packet.get());
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                return;
            if (r < 0)
                cannotWrite(error(r));
            av_packet_rescale_ts(packet.get(), encoder->time_base, videoOut->time_base);
            packet->stream_index = videoOut->index;
            if (packet->dts != AV_NOPTS_VALUE)
                writeSound(double(packet->dts) * av_q2d(videoOut->time_base) + 0.5);
            if (int e = av_interleaved_write_frame(out.get(), packet.get()); e < 0)
                cannotWrite(error(e));
        }
    };

    FramePipeline pipeline(rhi);
    pipeline.setLUT(lut);
    const EffectsParams fx = effectsFor(effects, std::max(info.width, info.height));
    const Chroma chroma = options.codec == ExportCodec::ProRes422HQ ? Chroma::C422 : Chroma::C420;
    std::unique_ptr<AVFrame, FrameCloser> frame(av_frame_alloc());
    frame->format = encoder->pix_fmt;
    frame->width = info.width;
    frame->height = info.height;
    if (int e = av_frame_get_buffer(frame.get(), 0); e < 0)
        cannotWrite(error(e));
    const int64_t shift = av_rescale_q(offset, AV_TIME_BASE_Q, videoIn->time_base);
    std::vector<uint16_t> planes[3];
    while (auto v = reader.next()) {
        if (options.seconds > 0 && v->time >= span)
            break;
        if (progress && !progress(span > 0 ? std::clamp(v->time / span, 0.0, 1.0) : 0))
            throw ExportCancelled();
        pipeline.renderExport(*v, readingOf(*v, input, format), fx, options.display, chroma, planes);
        if (int e = av_frame_make_writable(frame.get()); e < 0)
            cannotWrite(error(e));
        const int chromaWidth = chroma == Chroma::C422 || chroma == Chroma::C420 ? (v->width + 1) / 2 : v->width;
        const int chromaHeight = chroma == Chroma::C420 ? (v->height + 1) / 2 : v->height;
        fill(frame.get(), planes, v->width, v->height, chromaWidth, chromaHeight);
        frame->pts = v->pts - shift;
        frame->color_primaries = AVCOL_PRI_BT709;
        frame->color_trc = AVCOL_TRC_BT709;
        frame->colorspace = AVCOL_SPC_BT709;
        frame->color_range = AVCOL_RANGE_MPEG;
        frame->chroma_location = AVCHROMA_LOC_LEFT;
        drain(frame.get());
    }
    drain(nullptr);
    writeSound(1e18);
    if (int e = av_write_trailer(out.get()); e < 0)
        cannotWrite(error(e));
    avio_closep(&out->pb);

    // the file the save dialog was told to replace, if there is one
    std::error_code failed;
    std::filesystem::rename(partial, finalPath, failed);
    if (failed && std::filesystem::exists(finalPath)) {
        failed.clear();
        std::filesystem::remove(finalPath, failed);
        if (!failed)
            std::filesystem::rename(partial, finalPath, failed);
    }
    if (failed)
        cannotWrite("cannot put the finished file in place: " + failed.message());
    cleanup.done = true;
    if (progress)
        progress(1);
    return encoderName;
}
