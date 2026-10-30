#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX   // windows.h (via the D3D11VA headers) must not define min/max
#endif
#if defined(_WIN32)
#include <d3d11.h>   // before the C headers: d3d11.h declares C++ operators
#include <dxgi.h>
#endif

#include "ffmpeg_common.h"
#include "fa/media/hdr.h"

extern "C" {
#include <libavutil/hwcontext.h>
#if defined(_WIN32)
#include <libavutil/hwcontext_d3d11va.h>
#endif
}

#include <algorithm>
#include <future>
#include <thread>
#include <utility>

namespace fa {

namespace {

// How far ahead of the current position we keep decoding instead of seeking.
const Time kMaxForwardDecode(2);

struct BufferDeleter {
    void operator()(AVBufferRef* b) const { av_buffer_unref(&b); }
};
using BufferPtr = std::unique_ptr<AVBufferRef, BufferDeleter>;

struct Slot {
    ff::FramePtr raw = ff::makeFrame();
    Time time;
    uint64_t serial = 0;
    bool valid = false;
};

}  // namespace

struct VideoDecoder::Impl {
    std::filesystem::path path;
    ff::FormatInputPtr fmt;
    ff::CodecContextPtr dec;
    AVStream* stream = nullptr;
    VideoStreamInfo info;
    ff::PacketPtr pkt = ff::makePacket();
    ff::FramePtr scratch = ff::makeFrame();

    Slot cur;    // last decoded frame with time <= requested time
    Slot next;   // decoded look-ahead frame (time > requested time)
    bool flushSent = false;
    bool eof = false;
    bool decodedSinceSeek = false;
    uint64_t serialCounter = 0;
    Time lastFrameTime;
    bool haveLastFrameTime = false;
    Time tolerance;   // absorbs timestamp rounding in coarse time bases

    VideoFrame out;
    uint64_t outSerial = 0;
    ff::SwsPtr sws;
    int swsW = 0, swsH = 0, swsFmt = -1, swsCs = -1, swsRange = -1, swsHdr = -1;
    // HDR (PQ / HLG) sources: 16-bit R'G'B' from swscale, tone mapped to SDR Rec.709 (hdr.h)
    std::unique_ptr<HdrToSdr> hdr;
    HdrTransfer hdrKind = HdrTransfer::None;
    bool hdr2020 = false;
    std::vector<uint16_t> hdrRgb;
    int outW = 0, outH = 0;
    Options options;

    // hardware decoding (D3D11VA)
    BufferPtr hwDevice;
    AVPixelFormat hwFormat = AV_PIX_FMT_NONE;
    std::string hwNote;              // why hardware decoding is not used / the adapter in use
    int64_t hwFrames = 0;            // frames that came out of the GPU decoder
    ff::FramePtr transfer = ff::makeFrame();
    ff::FramePtr dstFrame = ff::makeFrame();   // wraps out.image for sws_scale_frame

    static AVPixelFormat pickFormat(AVCodecContext* ctx, const AVPixelFormat* fmts) {
        const auto* self = static_cast<const Impl*>(ctx->opaque);
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == self->hwFormat) return *p;
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {   // first software format
            const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
            if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
        }
        return fmts[0];
    }

    void setupHardware(const AVCodec* codec) {
#if defined(_WIN32)
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
                hwFormat = cfg->pix_fmt;
                break;
            }
        }
        if (hwFormat == AV_PIX_FMT_NONE) {
            hwNote = std::string("software (no D3D11VA decoder for ") + codec->name + ")";
            return;
        }
        AVBufferRef* dev = nullptr;
        const int r = av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
        if (r < 0) {
            hwNote = "software (D3D11VA device: " + ff::errorString(r) + ")";
            hwFormat = AV_PIX_FMT_NONE;
            return;
        }
        hwDevice.reset(dev);
        hwNote = "D3D11VA";
        const auto* hw = reinterpret_cast<const AVHWDeviceContext*>(hwDevice->data);
        const auto* d3d = static_cast<const AVD3D11VADeviceContext*>(hw->hwctx);
        IDXGIDevice* dxgi = nullptr;
        if (d3d->device && SUCCEEDED(d3d->device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgi->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc{};
                if (SUCCEEDED(adapter->GetDesc(&desc))) {
                    char name[256] = {};
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, nullptr, nullptr);
                    hwNote += std::string(" on ") + name;
                }
                adapter->Release();
            }
            dxgi->Release();
        }
#else
        (void)codec;
        hwNote = "software (hardware decoding is implemented for Windows / D3D11VA)";
#endif
    }

    // Output size for a w x h source under the maxWidth/maxHeight limit.
    void outputSize(int w, int h, int& ow, int& oh) const {
        ow = w;
        oh = h;
        if (options.maxWidth > 0 && options.maxHeight > 0 && (w > options.maxWidth || h > options.maxHeight)) {
            const double s = std::min(double(options.maxWidth) / w, double(options.maxHeight) / h);
            ow = std::max(2, static_cast<int>(w * s) & ~1);
            oh = std::max(2, static_cast<int>(h * s) & ~1);
        }
    }

    int64_t seeks = 0;
    int64_t decoded = 0;

    std::string where() const { return ff::pathUtf8(path); }

    void open(const Options& opt) {
        options = opt;
        fmt = ff::openInput(path);
        const AVCodec* codec = nullptr;
        const int idx = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (idx < 0) throw MediaError("no video stream in " + where());
        if (!codec) throw MediaError("no decoder for the video stream of " + where());
        stream = fmt->streams[idx];
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
            if (static_cast<int>(i) != idx) fmt->streams[i]->discard = AVDISCARD_ALL;

        hwDevice.reset();
        hwFormat = AV_PIX_FMT_NONE;
        hwNote = "software";
        if (opt.hardware) setupHardware(codec);
        dec.reset(avcodec_alloc_context3(codec));
        if (!dec) throw MediaError("out of memory (decoder context)");
        ff::check(avcodec_parameters_to_context(dec.get(), stream->codecpar), "decoder parameters for " + where());
        dec->pkt_timebase = stream->time_base;
        if (hwDevice) {
            dec->hw_device_ctx = av_buffer_ref(hwDevice.get());
            dec->opaque = this;
            dec->get_format = &Impl::pickFormat;
            dec->extra_hw_frames = 4;   // cur/next slots keep surfaces referenced
            dec->thread_count = 1;
        } else {
            dec->thread_count = opt.threads;
        }
        const int r = avcodec_open2(dec.get(), codec, nullptr);
        if (r < 0 && hwDevice) {   // the hardware path refused: plain software decoder
            hwNote = "software (D3D11VA open: " + ff::errorString(r) + ")";
            Options sw = opt;
            sw.hardware = false;
            open(sw);
            options = opt;
            return;
        }
        ff::check(r, "cannot open decoder for " + where());

        // Reuse the probe logic for consistent stream information.
        info.index = idx;
        info.codec = avcodec_get_name(stream->codecpar->codec_id);
        info.width = stream->codecpar->width;
        info.height = stream->codecpar->height;
        const char* pf = av_get_pix_fmt_name(static_cast<AVPixelFormat>(stream->codecpar->format));
        info.pixelFormat = pf ? pf : "unknown";
        AVRational fr = stream->avg_frame_rate;
        if (fr.num <= 0 || fr.den <= 0) fr = stream->r_frame_rate;
        if (fr.num <= 0 || fr.den <= 0) throw MediaError("cannot determine frame rate of " + where());
        info.frameRate = FrameRate(fr.num, fr.den);
        info.timeBase = ff::toRational(stream->time_base);
        info.startTime =
            stream->start_time != AV_NOPTS_VALUE ? ff::tsToTime(stream->start_time, stream->time_base) : Time(0);
        if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0)
            info.duration = ff::tsToTime(stream->duration, stream->time_base);
        if (stream->nb_frames > 0) info.frameCount = stream->nb_frames;
        const char* cs = av_color_space_name(stream->codecpar->color_space);
        info.colorSpace = cs ? cs : "unknown";
        const char* cr = av_color_range_name(stream->codecpar->color_range);
        info.colorRange = cr ? cr : "unknown";
        const char* trc = av_color_transfer_name(stream->codecpar->color_trc);
        info.transfer = trc ? trc : "unknown";
        const char* pri = av_color_primaries_name(stream->codecpar->color_primaries);
        info.primaries = pri ? pri : "unknown";

        // Half a time-base tick, but never more than a quarter frame.
        tolerance = min(info.timeBase * Rational(1, 2), info.frameRate.frameDuration() * Rational(1, 4));
    }

    Time frameTime(const AVFrame* f) {
        int64_t ts = f->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) ts = f->pts;
        Time t;
        if (ts != AV_NOPTS_VALUE) {
            t = ff::tsToTime(ts, stream->time_base) - info.startTime;
        } else if (haveLastFrameTime) {
            t = lastFrameTime + info.frameRate.frameDuration();   // synthesize CFR timestamps
        } else {
            t = Time(0);
        }
        lastFrameTime = t;
        haveLastFrameTime = true;
        return t;
    }

    // Decode the next frame into dst; false at end of stream.
    bool decodeOne(Slot& dst) {
        for (;;) {
            const int r = avcodec_receive_frame(dec.get(), scratch.get());
            if (r == 0) {
                av_frame_unref(dst.raw.get());
                av_frame_move_ref(dst.raw.get(), scratch.get());
                dst.time = frameTime(dst.raw.get());
                dst.serial = ++serialCounter;
                dst.valid = true;
                if (dst.raw->format == hwFormat && hwFormat != AV_PIX_FMT_NONE) ++hwFrames;
                ++decoded;
                decodedSinceSeek = true;
                return true;
            }
            if (r == AVERROR_EOF) {
                eof = true;
                return false;
            }
            if (r != AVERROR(EAGAIN)) ff::check(r, "decode error in " + where());
            if (flushSent) {   // drained but decoder still says EAGAIN: treat as end
                eof = true;
                return false;
            }
            const int rr = av_read_frame(fmt.get(), pkt.get());
            if (rr == AVERROR_EOF) {
                ff::check(avcodec_send_packet(dec.get(), nullptr), "flushing decoder of " + where());
                flushSent = true;
                continue;
            }
            ff::check(rr, "read error in " + where());
            if (pkt->stream_index == stream->index) {
                const int sr = avcodec_send_packet(dec.get(), pkt.get());
                av_packet_unref(pkt.get());
                if (sr < 0 && sr != AVERROR(EAGAIN) && sr != AVERROR_INVALIDDATA)
                    ff::check(sr, "cannot send packet to decoder of " + where());
            } else {
                av_packet_unref(pkt.get());
            }
        }
    }

    void seekTo(const Time& t) {
        const Time containerTime = max(Time(0), t) + info.startTime;
        const int64_t ts = (containerTime / info.timeBase).floor();
        int r = av_seek_frame(fmt.get(), stream->index, ts, AVSEEK_FLAG_BACKWARD);
        if (r < 0) r = av_seek_frame(fmt.get(), stream->index, ts, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
        ff::check(r, "seek to " + t.toString() + " s failed in " + where());
        avcodec_flush_buffers(dec.get());
        cur.valid = next.valid = false;
        eof = flushSent = false;
        decodedSinceSeek = false;
        haveLastFrameTime = false;
        ++seeks;
    }

    // Position cur/next around t (cur.time <= t + tol < next.time, or cur = last frame at EOF).
    // Returns false if t precedes the first decodable frame (cur then holds that first frame).
    bool advanceTo(const Time& t) {
        const Time tq = t + tolerance;
        for (;;) {
            if (!next.valid && !eof) decodeOne(next);
            if (!next.valid) return cur.valid;               // EOF: cur is the last frame
            if (next.time <= tq) {
                std::swap(cur, next);
                next.valid = false;
                continue;
            }
            if (!cur.valid) {   // first decoded frame after the seek point is already past t
                std::swap(cur, next);
                next.valid = false;
                return false;
            }
            return true;
        }
    }

    const VideoFrame& frameAt(const Time& t) {
        const Time tq = t + tolerance;
        const bool known = cur.valid && cur.time <= tq && (next.valid ? tq < next.time : eof);
        if (!known) {
            const bool forward = cur.valid && cur.time <= tq && (tq - cur.time) <= kMaxForwardDecode;
            const bool atStart = !cur.valid && !decodedSinceSeek && seeks == 0 && t <= kMaxForwardDecode;
            if (!forward && !atStart) seekTo(t);
            bool ok = advanceTo(t);
            // Seek landed after t (sparse/inexact index), or so close to the end that nothing decodable followed
            // (containers without an index, e.g. MPEG-TS, land on a non-key frame of the last GOP): back off
            // progressively.
            Time backoff(1);
            while (!ok && (!cur.valid || cur.time > tq) && t > Time(0)) {
                const Time target = max(Time(0), t - backoff);
                seekTo(target);
                ok = advanceTo(t);
                if (target == Time(0)) break;
                backoff = backoff * Rational(2);
            }
            if (!cur.valid) throw MediaError("no decodable video frame at " + t.toString() + " s in " + where());
        }
        return convert(cur);
    }

    const VideoFrame& convert(const Slot& s) {
        if (outSerial == s.serial) return out;
        const AVFrame* f = s.raw.get();
        if (hwFormat != AV_PIX_FMT_NONE && f->format == hwFormat) {   // GPU surface -> system memory (NV12)
            av_frame_unref(transfer.get());
            ff::check(av_hwframe_transfer_data(transfer.get(), f, 0), "GPU frame transfer in " + where());
            ff::check(av_frame_copy_props(transfer.get(), f), "frame properties in " + where());
            f = transfer.get();
        }
        const int cs = ff::swsColorspaceFor(f->colorspace, f->height);
        const AVPixelFormat pf = static_cast<AVPixelFormat>(f->format);
        const bool fullRange = f->color_range == AVCOL_RANGE_JPEG || pf == AV_PIX_FMT_YUVJ420P ||
                               pf == AV_PIX_FMT_YUVJ422P || pf == AV_PIX_FMT_YUVJ444P;
        const int range = fullRange ? 1 : 0;
        int ow = 0, oh = 0;
        outputSize(f->width, f->height, ow, oh);
        const HdrTransfer ht = f->color_trc == AVCOL_TRC_SMPTE2084   ? HdrTransfer::PQ
                               : f->color_trc == AVCOL_TRC_ARIB_STD_B67 ? HdrTransfer::HLG
                                                                        : HdrTransfer::None;
        const AVPixelFormat dstFormat = ht != HdrTransfer::None ? AV_PIX_FMT_RGB48 : AV_PIX_FMT_RGBA;
        if (!sws || swsW != f->width || swsH != f->height || swsFmt != f->format || swsCs != cs || swsRange != range ||
            outW != ow || outH != oh || swsHdr != static_cast<int>(ht)) {
            const unsigned flags = (ow == f->width && oh == f->height) ? (SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT)
                                                                        : (SWS_AREA | SWS_ACCURATE_RND);
            // Explicitly initialised (stateful) context so the conversion runs slice-threaded through
            // sws_scale_frame(); the output is identical to the single-threaded conversion.
            sws.reset(sws_alloc_context());
            if (!sws) throw MediaError("cannot create colour converter for " + where());
            SwsContext* c = sws.get();
            c->src_w = f->width;
            c->src_h = f->height;
            c->src_format = pf;
            c->dst_w = ow;
            c->dst_h = oh;
            c->dst_format = dstFormat;
            c->flags = flags;
            c->threads = options.convertThreads;
            ff::check(sws_init_context(c, nullptr, nullptr), "colour converter for " + where());
            outW = ow;
            outH = oh;
            // YUV (source matrix, source range) -> full-range RGB; neutral brightness/contrast/saturation.
            const int* coeff = sws_getCoefficients(cs);
            sws_setColorspaceDetails(c, coeff, range, coeff, 1, 0, 1 << 16, 1 << 16);
            swsW = f->width;
            swsH = f->height;
            swsFmt = f->format;
            swsCs = cs;
            swsRange = range;
            swsHdr = static_cast<int>(ht);
        }
        if (out.image.width != outW || out.image.height != outH) out.image.reset(outW, outH);   // fully overwritten
        if (ht != HdrTransfer::None) {
            convertHdr(f, ht);
        } else {
            convertSdr(f);
        }
        out.time = s.time;
        out.index = (s.time * info.frameRate.fps()).round();
        outSerial = s.serial;
        return out;
    }

    void convertHdr(const AVFrame* f, HdrTransfer ht) {
        const bool p2020 = f->color_primaries == AVCOL_PRI_BT2020;
        if (!hdr || hdrKind != ht || hdr2020 != p2020) {
            hdr = std::make_unique<HdrToSdr>(ht, p2020);
            hdrKind = ht;
            hdr2020 = p2020;
        }
        hdrRgb.resize(static_cast<size_t>(outW) * outH * 3);
        dstFrame->format = AV_PIX_FMT_RGB48;
        dstFrame->width = outW;
        dstFrame->height = outH;
        dstFrame->buf[0] = av_buffer_create(reinterpret_cast<uint8_t*>(hdrRgb.data()), hdrRgb.size() * 2,
                                            [](void*, uint8_t*) {}, nullptr, 0);
        if (!dstFrame->buf[0]) throw MediaError("out of memory (frame buffer)");
        dstFrame->data[0] = reinterpret_cast<uint8_t*>(hdrRgb.data());
        dstFrame->linesize[0] = outW * 6;
        const int sr = sws_scale_frame(sws.get(), dstFrame.get(), f);
        av_frame_unref(dstFrame.get());
        ff::check(sr, "colour conversion in " + where());
        // tone mapping, rows split over a few threads (the tables are read-only)
        const int parts = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
        auto rows = [this](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                hdr->convertRow(hdrRgb.data() + static_cast<size_t>(y) * outW * 3, out.image.row(y), outW);
        };
        std::vector<std::future<void>> jobs;
        const int step = (outH + parts - 1) / parts;
        for (int y = step; y < outH; y += step) jobs.push_back(std::async(std::launch::async, rows, y, std::min(outH, y + step)));
        rows(0, std::min(outH, step));
        for (auto& j : jobs) j.get();
    }

    void convertSdr(const AVFrame* f) {
        dstFrame->format = AV_PIX_FMT_RGBA;
        dstFrame->width = outW;
        dstFrame->height = outH;
        // sws_scale_frame() allocates its own output unless the frame carries a buffer reference: wrap
        // out.image (non-owning) so the conversion writes straight into it.
        dstFrame->buf[0] = av_buffer_create(out.image.pixels.data(), out.image.pixels.size(), [](void*, uint8_t*) {}, nullptr, 0);
        if (!dstFrame->buf[0]) throw MediaError("out of memory (frame buffer)");
        dstFrame->data[0] = out.image.pixels.data();
        dstFrame->linesize[0] = out.image.stride();
        const int sr = sws_scale_frame(sws.get(), dstFrame.get(), f);
        av_frame_unref(dstFrame.get());
        ff::check(sr, "colour conversion in " + where());
    }
};

VideoDecoder::VideoDecoder(const std::filesystem::path& file) : VideoDecoder(file, Options{}) {}

VideoDecoder::VideoDecoder(const std::filesystem::path& file, Options options) : impl_(std::make_unique<Impl>()) {
    impl_->path = file;
    impl_->open(options);
}

VideoDecoder::~VideoDecoder() = default;

const VideoStreamInfo& VideoDecoder::info() const { return impl_->info; }
const std::filesystem::path& VideoDecoder::path() const { return impl_->path; }
const VideoFrame& VideoDecoder::frameAt(const Time& t) {
    if (!impl_->hwDevice) return impl_->frameAt(t);
    try {
        return impl_->frameAt(t);
    } catch (const MediaError& e) {   // driver/decoder failure on the GPU path: continue in software
        const std::string why = e.what();
        Options sw = impl_->options;
        sw.hardware = false;
        impl_->cur.valid = impl_->next.valid = false;
        impl_->outSerial = 0;
        impl_->open(sw);
        impl_->eof = impl_->flushSent = false;
        impl_->decodedSinceSeek = true;   // forces a seek to t
        impl_->hwNote = "software (hardware decoding failed: " + why + ")";
        return impl_->frameAt(t);
    }
}
bool VideoDecoder::hardwareActive() const { return impl_->hwFrames > 0 && impl_->hwDevice != nullptr; }
std::string VideoDecoder::decoderDescription() const { return impl_->hwNote; }
int64_t VideoDecoder::seekCount() const { return impl_->seeks; }
int64_t VideoDecoder::decodedFrameCount() const { return impl_->decoded; }

}  // namespace fa
