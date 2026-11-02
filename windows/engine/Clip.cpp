#include "Clip.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>

std::string chromaName(Chroma c)
{
    switch (c) {
    case Chroma::C420: return "4:2:0";
    case Chroma::C422: return "4:2:2";
    case Chroma::C444: return "4:4:4";
    }
    return {};
}

namespace {

// Whether an environment variable is set: getenv, except on Windows, whose
// runtime counts getenv unsafe and has _dupenv_s.
bool environmentSet(const char *name)
{
#if defined(_WIN32)
    char *value = nullptr;
    size_t size = 0;
    const bool set = _dupenv_s(&value, &size, name) == 0 && value;
    std::free(value);
    return set;
#else
    return std::getenv(name) != nullptr;
#endif
}

[[noreturn]] void fail(const std::string &path, const std::string &why)
{
    const size_t slash = path.find_last_of("/\\");
    throw std::runtime_error((slash == std::string::npos ? path : path.substr(slash + 1)) + ": " + why);
}

std::string error(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return text;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string matrixName(AVColorSpace s)
{
    switch (s) {
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return "BT.2020";
    case AVCOL_SPC_BT709: return "BT.709";
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return "BT.601";
    default: return {};
    }
}

// The codec as an editor names it.
std::string codecName(const AVCodecParameters *p)
{
    switch (p->codec_id) {
    case AV_CODEC_ID_HEVC: return "HEVC";
    case AV_CODEC_ID_H264: return "H.264";
    case AV_CODEC_ID_PRORES:
        switch (p->profile) {
        case AV_PROFILE_PRORES_PROXY: return "ProRes 422 Proxy";
        case AV_PROFILE_PRORES_LT: return "ProRes 422 LT";
        case AV_PROFILE_PRORES_STANDARD: return "ProRes 422";
        case AV_PROFILE_PRORES_HQ: return "ProRes 422 HQ";
        case AV_PROFILE_PRORES_4444: return "ProRes 4444";
        case AV_PROFILE_PRORES_XQ: return "ProRes 4444 XQ";
        default: return "ProRes";
        }
    case AV_CODEC_ID_AV1: return "AV1";
    default: {
        const AVCodecDescriptor *d = avcodec_descriptor_get(p->codec_id);
        return d ? d->name : "video";
    }
    }
}

// The camera, from the file's metadata and its boxes: the maker and model
// FFmpeg reads, Apple's log tag (a 'logs' extension of the sample
// description), a Lumix camera's 'PANA' block (or Panasonic or Lumix named
// anywhere in its header), and a Sony camera's NonRealTimeMeta XML. Only box
// headers are read on the way past the media data, so a long clip costs no
// more than a short one (ClipInfo.cameraTags, sonyXML).
CameraTags cameraTags(const AVFormatContext *fmt, const std::string &path)
{
    CameraTags tags;
    auto tag = [&](const char *key) -> std::string {
        const AVDictionaryEntry *e = av_dict_get(fmt->metadata, key, nullptr, 0);
        return e ? e->value : "";
    };
    tags.make = tag("make");
    if (tags.make.empty())
        tags.make = tag("com.apple.quicktime.make");
    tags.model = tag("model");
    if (tags.model.empty())
        tags.model = tag("com.apple.quicktime.model");
    // an iPhone names its model with its maker in front, and no maker
    if (tags.make.empty() && tags.model.rfind("Apple ", 0) == 0)
        tags.make = "Apple";

    std::ifstream in(path, std::ios::binary);
    if (!in)
        return tags;
    in.seekg(0, std::ios::end);
    const uint64_t end = uint64_t(in.tellg());
    uint64_t offset = 0;
    auto attribute = [](const std::string &xml, const std::string &marker, const std::string &name) {
        const size_t at = xml.find(marker);
        if (at == std::string::npos)
            return std::string();
        const size_t open = xml.find(name + "=\"", at);
        if (open == std::string::npos)
            return std::string();
        const size_t from = open + name.size() + 2, close = xml.find('"', from);
        return close == std::string::npos ? std::string() : xml.substr(from, close - from);
    };
    while (offset + 8 <= end) {
        unsigned char header[16] = {};
        in.seekg(std::streamoff(offset));
        in.read(reinterpret_cast<char *>(header), 16);
        if (in.gcount() < 8)
            break;
        uint64_t size = (uint64_t(header[0]) << 24) | (uint64_t(header[1]) << 16) | (uint64_t(header[2]) << 8) | header[3];
        const std::string type(reinterpret_cast<const char *>(header) + 4, 4);
        if (size == 1 && in.gcount() >= 16) {
            size = 0;
            for (int i = 8; i < 16; ++i)
                size = size << 8 | header[i];
        }
        if (size == 0)
            size = end - offset;
        if (size < 8)
            break;
        if (type != "mdat" && type != "free" && type != "skip" && type != "wide" && size <= (64u << 20)) {
            std::string box(size_t(size), '\0');
            in.clear();
            in.seekg(std::streamoff(offset));
            in.read(box.data(), std::streamsize(size));
            if (box.find("com.apple.apple-wide-gamut.apple-log") != std::string::npos)
                tags.logTransfer = "com.apple.apple-wide-gamut.apple-log";
            else if (box.find("com.apple.rec2020.apple-log") != std::string::npos)
                tags.logTransfer = "com.apple.rec2020.apple-log";
            if (box.find("PANA") != std::string::npos)
                tags.panasonic = true;
            const std::string text = lower(box);
            if (text.find("panasonic") != std::string::npos || text.find("lumix") != std::string::npos)
                tags.panasonic = true;
            if (tags.gamma.empty() && box.find("CaptureGammaEquation") != std::string::npos) {
                tags.gamma = attribute(box, "\"CaptureGammaEquation\"", "value");
                tags.primaries = attribute(box, "\"CaptureColorPrimaries\"", "value");
                if (tags.make.empty())
                    tags.make = attribute(box, "<Device", "manufacturer");
                if (tags.model.empty())
                    tags.model = attribute(box, "<Device", "modelName");
            }
        }
        offset += size;
    }
    return tags;
}

int quarterTurnsOf(const AVStream *s)
{
    const AVPacketSideData *side = av_packet_side_data_get(s->codecpar->coded_side_data,
                                                           s->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (!side || side->size < 9 * 4)
        return 0;
    // FFmpeg's angle is counter-clockwise; the turns the track asks for, clockwise
    const double degrees = -av_display_rotation_get(reinterpret_cast<const int32_t *>(side->data));
    if (!std::isfinite(degrees))
        return 0;
    return ((int(std::lround(degrees / 90)) % 4) + 4) % 4;
}

} // namespace

std::string ClipInfo::summary() const
{
    std::string s = std::to_string(width) + "×" + std::to_string(height);
    char rate[32];
    if (std::round(frameRate) == frameRate)
        std::snprintf(rate, sizeof rate, " · %d fps", int(frameRate));
    else
        std::snprintf(rate, sizeof rate, " · %.3f fps", frameRate);
    s += rate;
    s += " · " + codecName + (bitDepth ? " " + std::to_string(bitDepth) + "-bit" : "");
    s += " · " + chromaName(chroma);
    if (fullRange)
        s += *fullRange ? " · full range" : " · video range";
    if (!matrixTag.empty())
        s += " · " + matrixTag;
    return s;
}

ClipInfo readClipInfo(const std::string &path)
{
    AVFormatContext *fmt = nullptr;
    if (int e = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr); e < 0)
        fail(path, error(e));
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext *)> hold(fmt, [](AVFormatContext *f) {
        avformat_close_input(&f);
    });
    if (int e = avformat_find_stream_info(fmt, nullptr); e < 0)
        fail(path, error(e));
    const int index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (index < 0)
        fail(path, "it has no video track");
    const AVStream *s = fmt->streams[index];
    const AVCodecParameters *p = s->codecpar;

    ClipInfo info;
    info.path = path;
    info.width = p->width;
    info.height = p->height;
    const AVRational rate = s->avg_frame_rate.num ? s->avg_frame_rate : s->r_frame_rate;
    info.frameRate = rate.den ? av_q2d(rate) : 0;
    info.duration = s->duration != AV_NOPTS_VALUE ? double(s->duration) * av_q2d(s->time_base)
                  : fmt->duration != AV_NOPTS_VALUE ? double(fmt->duration) / AV_TIME_BASE : 0;
    info.quarterTurns = quarterTurnsOf(s);
    info.codecName = codecName(p);
    if (const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(AVPixelFormat(p->format))) {
        info.bitDepth = d->comp[0].depth;
        info.chroma = d->log2_chroma_w == 0 ? Chroma::C444 : d->log2_chroma_h == 0 ? Chroma::C422 : Chroma::C420;
    }
    if (p->color_range == AVCOL_RANGE_JPEG)
        info.fullRange = true;
    else if (p->color_range == AVCOL_RANGE_MPEG)
        info.fullRange = false;
    info.matrixTag = matrixName(p->color_space);
    info.camera = cameraTags(fmt, path);
    info.hasAudio = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0;
    info.dataRate = double(p->bit_rate);
    return info;
}

// MARK: - ClipReader

struct ClipReader::Impl {
    std::string path;
    AVFormatContext *fmt = nullptr;
    AVCodecContext *dec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    AVFrame *transferred = nullptr;     // a frame the GPU decoded, in memory
    SwsContext *convert = nullptr;
    AVBufferRef *device = nullptr;      // the video decoder's, where there is one
    AVPixelFormat hardware = AV_PIX_FMT_NONE;
    int stream = -1;
    AVRational timeBase{1, 1};
    double start = 0;               // the stream's first time, which frame times count from
    bool drained = false;

    ~Impl()
    {
        sws_freeContext(convert);
        av_frame_free(&transferred);
        av_frame_free(&frame);
        av_buffer_unref(&device);
        av_packet_free(&packet);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
    }

    // the next decoded frame, into `frame`; false at the end
    bool decodeNext()
    {
        for (;;) {
            const int r = avcodec_receive_frame(dec, frame);
            if (r == 0)
                return true;
            if (r == AVERROR_EOF)
                return false;
            if (r != AVERROR(EAGAIN))
                fail(path, error(r));
            if (drained)
                return false;
            const int read = av_read_frame(fmt, packet);
            if (read < 0) {
                drained = true;
                avcodec_send_packet(dec, nullptr);
                continue;
            }
            if (packet->stream_index == stream)
                avcodec_send_packet(dec, packet);
            av_packet_unref(packet);
        }
    }

    double timeOf(const AVFrame *f) const
    {
        const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        return ts == AV_NOPTS_VALUE ? 0 : double(ts) * av_q2d(timeBase) - start;
    }

    // the frame's layout and tags, and room for its planes
    static VideoFrame shaped(const AVFrame *f, const AVPixFmtDescriptor *d)
    {
        VideoFrame v;
        v.width = f->width;
        v.height = f->height;
        v.bitDepth = d->comp[0].depth;
        v.chroma = d->log2_chroma_w == 0 ? Chroma::C444 : d->log2_chroma_h == 0 ? Chroma::C422 : Chroma::C420;
        v.chromaWidth = AV_CEIL_RSHIFT(f->width, d->log2_chroma_w);
        v.chromaHeight = AV_CEIL_RSHIFT(f->height, d->log2_chroma_h);
        v.fullRange = f->color_range == AVCOL_RANGE_JPEG;
        v.matrixTag = matrixName(f->colorspace);
        v.chromaLocation = int(f->chroma_location);
        v.planes[0].resize(size_t(v.width) * v.height);
        v.planes[1].resize(size_t(v.chromaWidth) * v.chromaHeight);
        v.planes[2].resize(size_t(v.chromaWidth) * v.chromaHeight);
        return v;
    }

    VideoFrame deinterleaved(const AVFrame *f, const AVPixFmtDescriptor *d)
    {
        VideoFrame v = shaped(f, d);
        v.time = timeOf(f);
        v.pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        if (v.pts == AV_NOPTS_VALUE)
            v.pts = 0;
        const bool wide = d->comp[0].depth > 8;
        const int shift = d->comp[0].shift;
        for (int y = 0; y < v.height; ++y) {
            const uint8_t *row = f->data[0] + size_t(y) * size_t(f->linesize[0]);
            uint16_t *to = &v.planes[0][size_t(y) * v.width];
            if (wide) {
                const auto *words = reinterpret_cast<const uint16_t *>(row);
                for (int x = 0; x < v.width; ++x)
                    to[x] = uint16_t(words[x] >> shift);
            } else {
                for (int x = 0; x < v.width; ++x)
                    to[x] = row[x];
            }
        }
        const int cb = d->comp[1].offset / (wide ? 2 : 1), cr = d->comp[2].offset / (wide ? 2 : 1);
        for (int y = 0; y < v.chromaHeight; ++y) {
            const uint8_t *row = f->data[1] + size_t(y) * size_t(f->linesize[1]);
            uint16_t *toCb = &v.planes[1][size_t(y) * v.chromaWidth];
            uint16_t *toCr = &v.planes[2][size_t(y) * v.chromaWidth];
            if (wide) {
                const auto *words = reinterpret_cast<const uint16_t *>(row);
                for (int x = 0; x < v.chromaWidth; ++x) {
                    toCb[x] = uint16_t(words[2 * x + cb] >> shift);
                    toCr[x] = uint16_t(words[2 * x + cr] >> shift);
                }
            } else {
                for (int x = 0; x < v.chromaWidth; ++x) {
                    toCb[x] = row[2 * x + cb];
                    toCr[x] = row[2 * x + cr];
                }
            }
        }
        return v;
    }

    VideoFrame copy()
    {
        AVFrame *f = frame;
        // a frame the GPU decoded comes into memory only when it is shown,
        // not for those decoded on past
        if (f->format == hardware) {
            av_frame_unref(transferred);
            if (int e = av_hwframe_transfer_data(transferred, f, 0); e < 0)
                fail(path, error(e));
            av_frame_copy_props(transferred, f);
            f = transferred;
        }
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
        const bool planarYuv = d && (d->flags & AV_PIX_FMT_FLAG_PLANAR) && !(d->flags & AV_PIX_FMT_FLAG_RGB)
                            && d->nb_components >= 3 && d->comp[0].depth <= 12 && d->comp[0].shift == 0;
        // Cb and Cr interleaved in one plane (NV12, P010, P210, as GPUs
        // decode), each sample in a 16-bit word's high bits past 8
        const bool semiPlanar = d && !(d->flags & AV_PIX_FMT_FLAG_RGB) && d->nb_components == 3
                             && d->comp[0].plane == 0 && d->comp[1].plane == 1 && d->comp[2].plane == 1
                             && d->comp[0].depth <= 12;
        if (semiPlanar)
            return deinterleaved(f, d);
        AVFrame *source = f;
        AVFrame *converted = nullptr;
        if (!planarYuv) {
            // anything else as planar 4:4:4 10-bit, its range and matrix kept
            converted = av_frame_alloc();
            converted->format = AV_PIX_FMT_YUV444P10LE;
            converted->width = f->width;
            converted->height = f->height;
            av_frame_get_buffer(converted, 0);
            convert = sws_getCachedContext(convert, f->width, f->height, AVPixelFormat(f->format), f->width, f->height,
                                           AV_PIX_FMT_YUV444P10LE, SWS_POINT, nullptr, nullptr, nullptr);
            if (!convert)
                fail(path, "its frames are in a layout that cannot be read");
            sws_scale(convert, f->data, f->linesize, 0, f->height, converted->data, converted->linesize);
            converted->color_range = f->color_range;
            converted->colorspace = f->colorspace;
            converted->chroma_location = f->chroma_location;
            source = converted;
            d = av_pix_fmt_desc_get(AV_PIX_FMT_YUV444P10LE);
        }
        VideoFrame v;
        v.width = source->width;
        v.height = source->height;
        v.bitDepth = d->comp[0].depth;
        v.chroma = d->log2_chroma_w == 0 ? Chroma::C444 : d->log2_chroma_h == 0 ? Chroma::C422 : Chroma::C420;
        v.chromaWidth = AV_CEIL_RSHIFT(source->width, d->log2_chroma_w);
        v.chromaHeight = AV_CEIL_RSHIFT(source->height, d->log2_chroma_h);
        v.fullRange = f->color_range == AVCOL_RANGE_JPEG;
        v.matrixTag = matrixName(f->colorspace);
        v.chromaLocation = int(f->chroma_location);
        v.time = timeOf(f);
        v.pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        if (v.pts == AV_NOPTS_VALUE)
            v.pts = 0;
        const bool wide = d->comp[0].depth > 8;
        for (int c = 0; c < 3; ++c) {
            const int w = c == 0 ? v.width : v.chromaWidth, h = c == 0 ? v.height : v.chromaHeight;
            std::vector<uint16_t> &plane = v.planes[c];
            plane.resize(size_t(w) * h);
            for (int y = 0; y < h; ++y) {
                const uint8_t *row = source->data[c] + size_t(y) * size_t(source->linesize[c]);
                if (wide)
                    std::memcpy(&plane[size_t(y) * w], row, size_t(w) * 2);
                else
                    for (int x = 0; x < w; ++x)
                        plane[size_t(y) * w + x] = row[x];
            }
        }
        if (converted)
            av_frame_free(&converted);
        return v;
    }
};

namespace {

// The decoder's pick from the formats it offers: the GPU's, where one was set
// up and the clip's profile has it, else the first it decodes into memory.
AVPixelFormat pickFormat(AVCodecContext *c, const AVPixelFormat *offered)
{
    const AVPixelFormat wanted = *static_cast<const AVPixelFormat *>(c->opaque);
    for (const AVPixelFormat *p = offered; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == wanted)
            return *p;
    for (const AVPixelFormat *p = offered; *p != AV_PIX_FMT_NONE; ++p)
        if (const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p); d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL))
            return *p;
    return AV_PIX_FMT_NONE;
}

} // namespace

ClipReader::ClipReader(const std::string &path)
    : m(std::make_unique<Impl>())
{
    m->path = path;
    if (int e = avformat_open_input(&m->fmt, path.c_str(), nullptr, nullptr); e < 0)
        fail(path, error(e));
    if (int e = avformat_find_stream_info(m->fmt, nullptr); e < 0)
        fail(path, error(e));
    const AVCodec *codec = nullptr;
    m->stream = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (m->stream < 0 || !codec)
        fail(path, "it has no video track this build can decode");
    AVStream *s = m->fmt->streams[m->stream];
    m->timeBase = s->time_base;
    m->start = s->start_time != AV_NOPTS_VALUE ? double(s->start_time) * av_q2d(s->time_base) : 0;
    m->dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m->dec, s->codecpar);
    // The GPU's video decoder where there is one, as the Mac's player decodes:
    // 4K at 60 frames a second is more than the processor keeps up with.
    // HEVC's decoders all give the same samples, bit for bit, so the grade
    // does not change. FLX_SOFTWARE_DECODE keeps to the processor.
    std::vector<AVHWDeviceType> kinds;
#if defined(__APPLE__)
    kinds = {AV_HWDEVICE_TYPE_VIDEOTOOLBOX};
#elif defined(_WIN32)
    kinds = {AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_DXVA2};
#endif
    if (environmentSet("FLX_SOFTWARE_DECODE"))
        kinds.clear();
    for (AVHWDeviceType kind : kinds) {
        AVPixelFormat format = AV_PIX_FMT_NONE;
        for (int i = 0; const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i); ++i)
            if (config->device_type == kind && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                format = config->pix_fmt;
        if (format == AV_PIX_FMT_NONE || av_hwdevice_ctx_create(&m->device, kind, nullptr, nullptr, 0) < 0)
            continue;
        m->hardware = format;
        m->dec->hw_device_ctx = av_buffer_ref(m->device);
        m->dec->opaque = &m->hardware;
        m->dec->get_format = pickFormat;
        m_decoder = av_hwdevice_get_type_name(kind);
        break;
    }
    // in software, every core: 4K HEVC is heavy to decode
    m->dec->thread_count = m->device ? 1 : 0;
    m->dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (int e = avcodec_open2(m->dec, codec, nullptr); e < 0)
        fail(path, error(e));
    m->packet = av_packet_alloc();
    m->frame = av_frame_alloc();
    m->transferred = av_frame_alloc();
    const AVRational rate = s->avg_frame_rate.num ? s->avg_frame_rate : s->r_frame_rate;
    if (rate.num > 0 && rate.den > 0)
        m_frameDuration = av_q2d(av_inv_q(rate));
}

ClipReader::~ClipReader() = default;

std::optional<VideoFrame> ClipReader::next()
{
    if (!m->decodeNext())
        return std::nullopt;
    VideoFrame v = m->copy();
    m_last = v.time;
    return v;
}

std::optional<VideoFrame> ClipReader::frameAt(double seconds)
{
    seconds = std::max(seconds, 0.0);
    const double half = m_frameDuration / 2;
    AVStream *s = m->fmt->streams[m->stream];
    const auto stamp = [&](double t) { return int64_t(std::floor((t + m->start) / av_q2d(m->timeBase))); };
    const int64_t target = stamp(seconds);
    // Ahead of the last frame, decoding on is quicker unless a keyframe lies
    // between it and the target: then the seek skips what is before that.
    bool decodeOn = m_last >= 0 && seconds > m_last + half;
    if (decodeOn) {
        if (const AVIndexEntry *key = avformat_index_get_entry_from_timestamp(s, target, AVSEEK_FLAG_BACKWARD))
            decodeOn = key->timestamp <= stamp(m_last);
        else
            decodeOn = seconds < m_last + 4 * m_frameDuration;
    }
    if (!decodeOn) {
        if (av_seek_frame(m->fmt, m->stream, target, AVSEEK_FLAG_BACKWARD) < 0)
            av_seek_frame(m->fmt, m->stream, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(m->dec);
        m->drained = false;
    }
    // the frame showing at `seconds`: decoding on past those that end before
    // it, copying only it (frames come out in display order)
    while (m->decodeNext()) {
        const double t = m->timeOf(m->frame);
        if (t + half <= seconds)
            continue;
        VideoFrame v = m->copy();
        m_last = v.time;
        return v;
    }
    return std::nullopt;
}

std::pair<int, int> ClipReader::timeBase() const
{
    return {m->timeBase.num, m->timeBase.den};
}
