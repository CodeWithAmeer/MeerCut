// Internal FFmpeg helpers: RAII owners, error formatting, rational conversion, colour setup.
#pragma once

#include "fa/core/time.h"
#include "fa/media/media.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <cstdlib>
#include <memory>
#include <string>

namespace fa::ff {

inline std::string errorString(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof buf);
    return buf;
}

// Throw MediaError("<what>: <ffmpeg message>") when err < 0.
inline int check(int err, const std::string& what) {
    if (err < 0) throw MediaError(what + ": " + errorString(err));
    return err;
}

struct FormatInputDeleter {
    void operator()(AVFormatContext* c) const { avformat_close_input(&c); }
};
struct CodecContextDeleter {
    void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
struct FrameDeleter {
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
struct SwsDeleter {
    void operator()(SwsContext* s) const { sws_freeContext(s); }
};

using FormatInputPtr = std::unique_ptr<AVFormatContext, FormatInputDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;
struct SwrDeleter {
    void operator()(SwrContext* s) const { swr_free(&s); }
};
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;

inline FramePtr makeFrame() {
    FramePtr f(av_frame_alloc());
    if (!f) throw MediaError("out of memory (av_frame_alloc)");
    return f;
}
inline PacketPtr makePacket() {
    PacketPtr p(av_packet_alloc());
    if (!p) throw MediaError("out of memory (av_packet_alloc)");
    return p;
}

// SVT-AV1 prints a banner to stderr for every encoder it opens unless SVT_LOG says otherwise: errors only (once per
// process, and only when the user has not set SVT_LOG).
inline void quietSvtAv1() {
    static const bool done = [] {
        size_t n = 0;
        getenv_s(&n, nullptr, 0, "SVT_LOG");
        if (n == 0) _putenv_s("SVT_LOG", "1");
        return true;
    }();
    (void)done;
}

inline Rational toRational(AVRational r) { return Rational(r.num, r.den); }
inline AVRational toAV(const Rational& r) {
    if (r.num() > INT32_MAX || r.num() < INT32_MIN || r.den() > INT32_MAX)
        throw MediaError("rational " + r.toString() + " does not fit an AVRational");
    return AVRational{static_cast<int>(r.num()), static_cast<int>(r.den())};
}
// ts (in stream time base) -> seconds, exactly.
inline Time tsToTime(int64_t ts, AVRational tb) { return Rational(ts) * toRational(tb); }

// One-time library setup (log level: errors only unless setFfmpegLogLevel() was called).
void ensureInit();

FormatInputPtr openInput(const std::filesystem::path& file);
std::string pathUtf8(const std::filesystem::path& p);

// YUV <-> RGB matrix choice (BT.709 for HD/unspecified HD, BT.601 for SD; Stage 1 EXP-001a).
int swsColorspaceFor(AVColorSpace cs, int height);

}  // namespace fa::ff
