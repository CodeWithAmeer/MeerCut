#include "ffmpeg_common.h"

#include <atomic>
#include <mutex>
#include <sstream>

namespace fa {

namespace {
std::once_flag gInitOnce;
std::atomic<bool> gUserLogLevel{false};
}  // namespace

namespace ff {

void ensureInit() {
    std::call_once(gInitOnce, [] {
        if (!gUserLogLevel) av_log_set_level(AV_LOG_ERROR);
    });
}

std::string pathUtf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());   // FFmpeg on Windows expects UTF-8 file names
}

FormatInputPtr openInput(const std::filesystem::path& file) {
    ensureInit();
    if (!std::filesystem::exists(file)) throw MediaError("media file not found: " + pathUtf8(file));
    AVFormatContext* raw = nullptr;
    const int err = avformat_open_input(&raw, pathUtf8(file).c_str(), nullptr, nullptr);
    if (err < 0) throw MediaError("cannot open " + pathUtf8(file) + ": " + errorString(err));
    FormatInputPtr ctx(raw);
    check(avformat_find_stream_info(ctx.get(), nullptr), "cannot read stream info of " + pathUtf8(file));
    return ctx;
}

int swsColorspaceFor(AVColorSpace cs, int height) {
    switch (cs) {
        case AVCOL_SPC_BT709: return SWS_CS_ITU709;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
        case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
        case AVCOL_SPC_FCC: return SWS_CS_FCC;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: return SWS_CS_BT2020;
        default: return height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
}

}  // namespace ff

std::string ffmpegVersionInfo() {
    std::ostringstream os;
    auto v = [](unsigned x) {
        return std::to_string(AV_VERSION_MAJOR(x)) + "." + std::to_string(AV_VERSION_MINOR(x)) + "." +
               std::to_string(AV_VERSION_MICRO(x));
    };
    os << "FFmpeg " << av_version_info() << " (libavformat " << v(avformat_version()) << ", libavcodec "
       << v(avcodec_version()) << ", libavutil " << v(avutil_version()) << ", libswscale " << v(swscale_version())
       << ")";
    return os.str();
}

void setFfmpegLogLevel(int level) {
    gUserLogLevel = true;
    av_log_set_level(level);
}

MediaInfo probeMedia(const std::filesystem::path& file) {
    ff::FormatInputPtr ctx = ff::openInput(file);
    MediaInfo info;
    info.path = file;
    info.container = ctx->iformat ? ctx->iformat->name : "";
    if (ctx->duration != AV_NOPTS_VALUE && ctx->duration > 0)
        info.duration = Rational(ctx->duration, AV_TIME_BASE);

    const int vi = av_find_best_stream(ctx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vi >= 0) {
        const AVStream* st = ctx->streams[vi];
        const AVCodecParameters* par = st->codecpar;
        VideoStreamInfo v;
        v.index = vi;
        v.codec = avcodec_get_name(par->codec_id);
        v.width = par->width;
        v.height = par->height;
        const char* pf = av_get_pix_fmt_name(static_cast<AVPixelFormat>(par->format));
        v.pixelFormat = pf ? pf : "unknown";
        AVRational fr = st->avg_frame_rate;
        if (fr.num <= 0 || fr.den <= 0) fr = st->r_frame_rate;
        if (fr.num <= 0 || fr.den <= 0) throw MediaError("cannot determine frame rate of " + ff::pathUtf8(file));
        v.frameRate = FrameRate(fr.num, fr.den);
        v.timeBase = ff::toRational(st->time_base);
        v.startTime = st->start_time != AV_NOPTS_VALUE ? ff::tsToTime(st->start_time, st->time_base) : Time(0);
        if (st->duration != AV_NOPTS_VALUE && st->duration > 0) v.duration = ff::tsToTime(st->duration, st->time_base);
        if (st->nb_frames > 0) v.frameCount = st->nb_frames;
        const char* cs = av_color_space_name(par->color_space);
        v.colorSpace = cs ? cs : "unknown";
        const char* cr = av_color_range_name(par->color_range);
        v.colorRange = cr ? cr : "unknown";
        const char* trc = av_color_transfer_name(par->color_trc);
        v.transfer = trc ? trc : "unknown";
        const char* pri = av_color_primaries_name(par->color_primaries);
        v.primaries = pri ? pri : "unknown";
        info.video = v;
    }
    const int ai = av_find_best_stream(ctx.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (ai >= 0) {
        const AVStream* st = ctx->streams[ai];
        AudioStreamInfo a;
        a.index = ai;
        a.codec = avcodec_get_name(st->codecpar->codec_id);
        a.sampleRate = st->codecpar->sample_rate;
        a.channels = st->codecpar->ch_layout.nb_channels;
        a.startTime = st->start_time != AV_NOPTS_VALUE ? ff::tsToTime(st->start_time, st->time_base) : Time(0);
        if (st->duration != AV_NOPTS_VALUE && st->duration > 0) a.duration = ff::tsToTime(st->duration, st->time_base);
        info.audio = a;
    }
    return info;
}

}  // namespace fa
