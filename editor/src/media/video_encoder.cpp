#include "ffmpeg_common.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <ctime>
#include <system_error>
#include <thread>
#include <vector>

namespace fa {

namespace {
struct FormatOutputDeleter {
    void operator()(AVFormatContext* c) const {
        if (!c) return;
        if (c->pb && !(c->oformat->flags & AVFMT_NOFILE)) avio_closep(&c->pb);
        avformat_free_context(c);
    }
};
using FormatOutputPtr = std::unique_ptr<AVFormatContext, FormatOutputDeleter>;
struct BsfDeleter {
    void operator()(AVBSFContext* c) const { av_bsf_free(&c); }
};
using BsfPtr = std::unique_ptr<AVBSFContext, BsfDeleter>;

bool endsWith(const std::string& s, const char* tail) {
    const size_t n = std::char_traits<char>::length(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}
bool isNvenc(const std::string& e) { return endsWith(e, "_nvenc"); }
bool isQsv(const std::string& e) { return endsWith(e, "_qsv"); }
bool isAmf(const std::string& e) { return endsWith(e, "_amf"); }
bool isHardware(const std::string& e) { return isNvenc(e) || isQsv(e) || isAmf(e); }

// x264 speed names, 0 = ultrafast .. 8 = veryslow (medium = 5).
int speedIndex(const std::string& p) {
    static const char* names[] = {"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"};
    for (int i = 0; i < 9; ++i)
        if (p == names[i]) return i;
    return 5;
}

AVPixelFormat pixelFormatFor(const std::string& encoder, int bitDepth) {
    if (encoder == "prores_ks") return AV_PIX_FMT_YUV422P10LE;
    if (encoder == "gif") return AV_PIX_FMT_PAL8;
    if (isHardware(encoder)) return bitDepth == 10 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    return bitDepth == 10 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
}

std::string nowIso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_s(&utc, &t);
    char buf[40];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S.000000Z", &utc);
    return buf;
}

void setThreads(AVDictionary** opts, const std::map<std::string, std::string>& user) {
    // beyond 8 there is no speed gain at 1080p but each thread adds frame buffers (measured with x264: 787 MB -> 581 MB
    // peak, 55 -> 57 fps on 12 cores)
    if (user.count("threads")) return;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    av_dict_set(opts, "threads", std::to_string(std::min(8u, hw)).c_str(), 0);
}

// Speed, quality / bit rate options of each encoder.
void encoderOptions(const EncoderSettings& s, AVCodecContext* c, AVDictionary** opts) {
    const std::string& e = s.codec;
    const int sp = speedIndex(s.preset);
    const bool constantQuality = s.bitrate <= 0;
    auto set = [&](const char* k, const std::string& v) { av_dict_set(opts, k, v.c_str(), 0); };
    if (!constantQuality) c->bit_rate = s.bitrate;
    if (s.maxBitrate > 0) {
        c->rc_max_rate = s.maxBitrate;
        c->rc_buffer_size = static_cast<int>(std::min<int64_t>(INT_MAX, s.maxBitrate * 2));
    }
    if (e == "libx264" || e == "libx265") {
        set("preset", s.preset);
        if (constantQuality) set("crf", std::to_string(s.crf));
        if (e == "libx265") set("x265-params", "log-level=error");
        setThreads(opts, s.codecOptions);
    } else if (e == "libsvtav1") {
        static const int presets[] = {12, 11, 10, 9, 8, 7, 6, 5, 4};
        set("preset", std::to_string(presets[sp]));
        if (constantQuality) set("crf", std::to_string(s.crf));
    } else if (e == "libvpx-vp9") {
        static const int cpuUsed[] = {8, 8, 7, 6, 5, 4, 3, 2, 1};
        set("deadline", "good");
        set("cpu-used", std::to_string(cpuUsed[sp]));
        set("row-mt", "1");
        if (s.width >= 1280) set("tile-columns", "2");
        if (constantQuality) {
            set("crf", std::to_string(s.crf));
            c->bit_rate = 0;   // with crf: constant quality
        }
        setThreads(opts, s.codecOptions);
    } else if (isNvenc(e)) {
        static const char* presets[] = {"p1", "p2", "p3", "p3", "p4", "p5", "p6", "p7", "p7"};
        set("preset", presets[sp]);
        set("tune", "hq");
        set("rc", "vbr");
        if (constantQuality) {
            set("cq", std::to_string(s.crf));
            c->bit_rate = 0;
        }
        if (e == "h264_nvenc") set("profile", "high");
    } else if (isQsv(e)) {
        static const char* presets[] = {"veryfast", "veryfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"};
        set("preset", presets[sp]);
        if (constantQuality && e == "vp9_qsv") {
            // Intel's VP9 encoder ignores ICQ (every value gave the same file, M6 F) and FFmpeg clips its CQP to QP 51,
            // so the quality value becomes a VBR target: 0.14 bit per pixel at 24 (about x264's CRF 23 on detailed
            // pictures), doubling every 3.5 steps
            const double bitsPerPixel = 0.14 * std::pow(2.0, (24 - s.crf) / 3.5);
            c->bit_rate = static_cast<int64_t>(bitsPerPixel * s.width * s.height * s.frameRate.fps().toDouble());
        } else if (constantQuality) {
            c->global_quality = s.crf;   // ICQ
        }
        // With its default rate-control buffer, VP9 on Quick Sync never returns from a frame larger than the buffer
        // (noise / heavy grain at 1080p 2.7 Mbit/s hangs; FFmpeg's own CLI too, M6 F). A buffer of at least one raw
        // frame and 4 s of the bit rate avoids it.
        if (e == "vp9_qsv" && c->bit_rate > 0)
            c->rc_buffer_size = static_cast<int>(std::min<int64_t>(
                INT_MAX, std::max({int64_t(c->rc_buffer_size), c->bit_rate * 4, int64_t(s.width) * s.height * 12})));
    } else if (isAmf(e)) {
        set("quality", sp <= 3 ? "speed" : sp >= 6 ? "quality" : "balanced");
        if (constantQuality) {
            set("rc", "cqp");
            for (const char* k : {"qp_i", "qp_p", "qp_b"}) set(k, std::to_string(s.crf));
        }
    } else if (e == "prores_ks") {
        set("profile", std::to_string(std::clamp(s.proresProfile, 0, 3)));
        setThreads(opts, s.codecOptions);
    }
}

// Ordered (Bayer 8x8) dither thresholds, -0.5 .. 0.5.
const std::array<float, 64>& bayer8() {
    static const std::array<float, 64> m = [] {
        std::array<float, 64> b{};
        static const int base[8][8] = {{0, 32, 8, 40, 2, 34, 10, 42},   {48, 16, 56, 24, 50, 18, 58, 26},
                                       {12, 44, 4, 36, 14, 46, 6, 38},  {60, 28, 52, 20, 62, 30, 54, 22},
                                       {3, 35, 11, 43, 1, 33, 9, 41},   {51, 19, 59, 27, 49, 17, 57, 25},
                                       {15, 47, 7, 39, 13, 45, 5, 37},  {63, 31, 55, 23, 61, 29, 53, 21}};
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) b[static_cast<size_t>(y * 8 + x)] = (base[y][x] + 0.5f) / 64.0f - 0.5f;
        return b;
    }();
    return m;
}

}  // namespace

// ---- GIF palette (median cut over a 5-bit-per-channel histogram) ----------------------------------------------------

std::vector<uint32_t> buildGifPalette(const std::vector<const Image*>& frames, int colors) {
    colors = std::clamp(colors, 2, 256);
    std::vector<uint32_t> hist(32768, 0);
    std::vector<std::array<uint64_t, 3>> sums(32768, {0, 0, 0});   // the actual colours in each bin
    for (const Image* f : frames) {
        if (!f) continue;
        const uint8_t* p = f->pixels.data();
        const size_t n = static_cast<size_t>(f->width) * f->height;
        for (size_t i = 0; i < n; ++i, p += 4) {
            const size_t bin = (static_cast<size_t>(p[0] >> 3) << 10) | (static_cast<size_t>(p[1] >> 3) << 5) | (p[2] >> 3);
            ++hist[bin];
            sums[bin][0] += p[0];
            sums[bin][1] += p[1];
            sums[bin][2] += p[2];
        }
    }
    struct Box {
        std::vector<uint32_t> bins;   // histogram indices with counts > 0
        uint64_t count = 0;
    };
    std::vector<Box> boxes(1);
    for (uint32_t i = 0; i < hist.size(); ++i)
        if (hist[i]) {
            boxes[0].bins.push_back(i);
            boxes[0].count += hist[i];
        }
    if (boxes[0].bins.empty()) return std::vector<uint32_t>(static_cast<size_t>(colors), 0);
    auto channel = [](uint32_t bin, int ch) { return static_cast<int>((bin >> (10 - 5 * ch)) & 31); };
    while (static_cast<int>(boxes.size()) < colors) {
        // split the box with the largest (range x pixel count) along its widest channel at the median
        int best = -1;
        double bestScore = 0;
        int bestCh = 0;
        for (size_t b = 0; b < boxes.size(); ++b) {
            if (boxes[b].bins.size() < 2) continue;
            for (int ch = 0; ch < 3; ++ch) {
                int lo = 31, hi = 0;
                for (uint32_t bin : boxes[b].bins) {
                    lo = std::min(lo, channel(bin, ch));
                    hi = std::max(hi, channel(bin, ch));
                }
                const double score = static_cast<double>(hi - lo) * static_cast<double>(boxes[b].count);
                if (score > bestScore) {
                    bestScore = score;
                    best = static_cast<int>(b);
                    bestCh = ch;
                }
            }
        }
        if (best < 0) break;
        Box& box = boxes[static_cast<size_t>(best)];
        std::sort(box.bins.begin(), box.bins.end(), [&](uint32_t a, uint32_t b) {
            const int ca = channel(a, bestCh), cb = channel(b, bestCh);
            return ca != cb ? ca < cb : a < b;
        });
        uint64_t half = 0, acc = 0;
        for (uint32_t bin : box.bins) half += hist[bin];
        half /= 2;
        size_t cut = 1;
        for (size_t i = 0; i + 1 < box.bins.size(); ++i) {
            acc += hist[box.bins[i]];
            cut = i + 1;
            if (acc >= half) break;
        }
        Box other;
        other.bins.assign(box.bins.begin() + static_cast<std::ptrdiff_t>(cut), box.bins.end());
        box.bins.resize(cut);
        box.count = 0;
        for (uint32_t bin : box.bins) box.count += hist[bin];
        for (uint32_t bin : other.bins) other.count += hist[bin];
        boxes.push_back(std::move(other));
    }
    std::vector<uint32_t> pal;
    for (const Box& b : boxes) {
        double r = 0, g = 0, bl = 0, w = 0;   // the mean colour of the pixels in the box (exact for flat colours)
        for (uint32_t bin : b.bins) {
            r += static_cast<double>(sums[bin][0]);
            g += static_cast<double>(sums[bin][1]);
            bl += static_cast<double>(sums[bin][2]);
            w += hist[bin];
        }
        const auto q = [&](double v) { return static_cast<uint32_t>(std::clamp(std::lround(v / w), 0L, 255L)); };
        pal.push_back((q(r) << 16) | (q(g) << 8) | q(bl));
    }
    while (static_cast<int>(pal.size()) < colors) pal.push_back(pal.back());
    return pal;
}

struct VideoEncoder::Impl {
    std::filesystem::path path;
    EncoderSettings s;
    FormatOutputPtr fmt;
    ff::CodecContextPtr enc;
    AVStream* stream = nullptr;
    AVPixelFormat pixfmt = AV_PIX_FMT_YUV420P;
    ff::FramePtr yuv = ff::makeFrame();
    ff::PacketPtr pkt = ff::makePacket();
    ff::SwsPtr sws;
    int swsW = 0, swsH = 0;
    BsfPtr colourTags;             // vp9_qsv: writes the BT.709 / limited-range colour config into the VP9 headers
    std::vector<uint8_t> gifLut;   // 32x32x32 nearest palette index
    Image scaled;                  // GIF: the frame at the output size
    int64_t written = 0;
    // audio
    ff::CodecContextPtr aenc;
    AVStream* astream = nullptr;
    ff::FramePtr aframe = ff::makeFrame();
    ff::PacketPtr apkt = ff::makePacket();
    std::vector<float> pending;   // interleaved samples not yet sent to the encoder
    int64_t audioIn = 0;          // samples received from the caller
    int64_t audioPts = 0;         // samples sent to the encoder
    bool headerWritten = false;
    bool finished = false;

    std::string where() const { return ff::pathUtf8(path); }
    bool hasVideo() const { return !s.codec.empty(); }

    void open() {
        ff::ensureInit();
        if (!hasVideo() && !s.audio) throw MediaError("an export needs video or audio");
        AVFormatContext* raw = nullptr;
        ff::check(avformat_alloc_output_context2(&raw, nullptr, s.muxer.empty() ? nullptr : s.muxer.c_str(), where().c_str()),
                  "cannot create output container for " + where());
        fmt.reset(raw);
        if (hasVideo()) openVideo();
        if (s.audio) openAudio();

        av_dict_set(&fmt->metadata, "creation_time", (s.creationTime.empty() ? nowIso8601() : s.creationTime).c_str(), 0);
        if (!s.title.empty()) av_dict_set(&fmt->metadata, "title", s.title.c_str(), 0);
        if (!(fmt->oformat->flags & AVFMT_NOFILE))
            ff::check(avio_open(&fmt->pb, where().c_str(), AVIO_FLAG_WRITE), "cannot create file " + where());
        AVDictionary* muxOpts = nullptr;
        const std::string muxer = fmt->oformat->name;
        if (muxer == "mp4" || muxer == "mov") {
            av_dict_set(&muxOpts, "movflags", "+faststart", 0);
            if (hasVideo()) {   // Filmora's exports: 1000 x fps track time scale (1/30000 at 30 fps, EXP-001)
                const Rational fps = s.frameRate.fps();
                const int64_t scale = fps.den() == 1 ? fps.num() * 1000 : fps.num();
                av_dict_set(&muxOpts, "video_track_timescale", std::to_string(scale).c_str(), 0);
            }
        }
        if (muxer == "mp4") av_dict_set(&muxOpts, "brand", "mp42", 0);   // EXP-001: major brand mp42
        if (muxer == "wav") av_dict_set(&muxOpts, "rf64", "auto", 0);   // > 4 GB stays readable
        const int hr = avformat_write_header(fmt.get(), &muxOpts);
        av_dict_free(&muxOpts);
        ff::check(hr, "cannot write header of " + where());
        headerWritten = true;
    }

    void openVideo() {
        const AVCodec* codec = avcodec_find_encoder_by_name(s.codec.c_str());
        if (!codec) throw MediaError("encoder '" + s.codec + "' is not available in this FFmpeg build");
        if (s.codec == "libsvtav1") ff::quietSvtAv1();
        stream = avformat_new_stream(fmt.get(), nullptr);
        if (!stream) throw MediaError("cannot add video stream to " + where());
        enc.reset(avcodec_alloc_context3(codec));
        if (!enc) throw MediaError("out of memory (encoder context)");
        if (s.width <= 0 || s.height <= 0 || (s.width % 2) || (s.height % 2))
            throw MediaError("output size must be positive and even, got " + std::to_string(s.width) + "x" +
                             std::to_string(s.height));
        if (s.bitDepth != 8 && s.bitDepth != 10) throw MediaError("bit depth must be 8 or 10");
        pixfmt = pixelFormatFor(s.codec, s.bitDepth);
        enc->width = s.width;
        enc->height = s.height;
        enc->pix_fmt = pixfmt;
        enc->framerate = ff::toAV(s.frameRate.fps());
        enc->time_base = ff::toAV(s.frameRate.frameDuration());
        enc->sample_aspect_ratio = AVRational{1, 1};
        if (s.codec != "gif") {   // SDR Rec.709, limited range, chroma sited left (EXP-001); never HDR tags
            enc->color_primaries = AVCOL_PRI_BT709;
            enc->color_trc = AVCOL_TRC_BT709;
            enc->colorspace = AVCOL_SPC_BT709;
            enc->color_range = AVCOL_RANGE_MPEG;
            enc->chroma_sample_location = AVCHROMA_LOC_LEFT;
        }
        enc->field_order = AV_FIELD_PROGRESSIVE;
        if (s.gopSize > 0) enc->gop_size = s.gopSize;
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        AVDictionary* opts = nullptr;
        encoderOptions(s, enc.get(), &opts);
        for (const auto& [k, v] : s.codecOptions) av_dict_set(&opts, k.c_str(), v.c_str(), 0);
        const int r = avcodec_open2(enc.get(), codec, &opts);
        if (r >= 0 && av_dict_count(opts) > 0) {
            std::string unused;
            const AVDictionaryEntry* e = nullptr;
            while ((e = av_dict_get(opts, "", e, AV_DICT_IGNORE_SUFFIX))) unused += std::string(" ") + e->key;
            av_dict_free(&opts);
            throw MediaError("unknown encoder option(s) for " + s.codec + ":" + unused);
        }
        av_dict_free(&opts);
        ff::check(r, "cannot open encoder " + s.codec);

        ff::check(avcodec_parameters_from_context(stream->codecpar, enc.get()), "encoder parameters");
        const std::string muxer = fmt->oformat->name;
        if (enc->codec_id == AV_CODEC_ID_HEVC && (muxer == "mp4" || muxer == "mov"))
            stream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');   // the HEVC sample entry Apple players accept
        stream->time_base = enc->time_base;
        stream->avg_frame_rate = enc->framerate;
        stream->r_frame_rate = enc->framerate;
        // Quick Sync's VP9 encoder leaves the colour space in its frame headers "unknown" (only the WebM container says
        // BT.709), so decoders that read the bitstream fall back to BT.601 and shift the colours; FFmpeg's own
        // vp9_metadata filter rewrites the headers (found by the M6 F quality check, TEST_RESULTS.md).
        if (s.codec == "vp9_qsv") openColourTags();

        yuv->format = pixfmt;
        yuv->width = s.width;
        yuv->height = s.height;
        ff::check(av_frame_get_buffer(yuv.get(), 0), "cannot allocate encoder frame");
        if (s.codec == "gif") prepareGif();
    }

    void openColourTags() {
        const AVBitStreamFilter* f = av_bsf_get_by_name("vp9_metadata");
        if (!f) throw MediaError("the vp9_metadata bitstream filter is not in this FFmpeg build");
        AVBSFContext* raw = nullptr;
        ff::check(av_bsf_alloc(f, &raw), "cannot create the vp9_metadata filter");
        colourTags.reset(raw);
        ff::check(avcodec_parameters_copy(colourTags->par_in, stream->codecpar), "vp9_metadata parameters");
        colourTags->time_base_in = enc->time_base;
        ff::check(av_opt_set(colourTags->priv_data, "color_space", "bt709", 0), "vp9_metadata colour space");
        ff::check(av_opt_set(colourTags->priv_data, "color_range", "tv", 0), "vp9_metadata colour range");
        ff::check(av_bsf_init(colourTags.get()), "cannot start the vp9_metadata filter");
    }

    void prepareGif() {
        if (s.palette.size() != 256) throw MediaError("a GIF export needs a 256-colour palette");
        gifLut.resize(32768);
        for (int i = 0; i < 32768; ++i) {
            const int r = ((i >> 10) & 31) * 8 + 4, g = ((i >> 5) & 31) * 8 + 4, b = (i & 31) * 8 + 4;
            int best = 0, bestD = INT_MAX;
            for (int k = 0; k < 256; ++k) {
                const uint32_t c = s.palette[static_cast<size_t>(k)];
                const int dr = r - static_cast<int>((c >> 16) & 255), dg = g - static_cast<int>((c >> 8) & 255), db = b - static_cast<int>(c & 255);
                const int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;   // weighted RGB distance
                if (d < bestD) {
                    bestD = d;
                    best = k;
                }
            }
            gifLut[static_cast<size_t>(i)] = static_cast<uint8_t>(best);
        }
    }

    void openAudio() {
        const AVCodec* codec = avcodec_find_encoder_by_name(s.audioCodec.c_str());
        if (!codec) throw MediaError("audio encoder '" + s.audioCodec + "' is not available in this FFmpeg build");
        if (s.audioSampleRate <= 0 || s.audioChannels <= 0 || s.audioChannels > 8)
            throw MediaError("invalid audio output format");
        if (s.audioCodec == "libopus" && s.audioSampleRate != 48000 && s.audioSampleRate != 24000 &&
            s.audioSampleRate != 16000 && s.audioSampleRate != 12000 && s.audioSampleRate != 8000)
            throw MediaError("Opus needs a 48 kHz (or 24 / 16 / 12 / 8 kHz) sequence, not " + std::to_string(s.audioSampleRate) + " Hz");
        astream = avformat_new_stream(fmt.get(), nullptr);
        if (!astream) throw MediaError("cannot add audio stream to " + where());
        aenc.reset(avcodec_alloc_context3(codec));
        if (!aenc) throw MediaError("out of memory (audio encoder context)");
        const std::string& a = s.audioCodec;
        if (a == "pcm_s16le" || (a == "flac" && s.audioBits != 24)) {
            aenc->sample_fmt = AV_SAMPLE_FMT_S16;
        } else if (a == "pcm_s24le" || a == "flac") {
            aenc->sample_fmt = AV_SAMPLE_FMT_S32;
            aenc->bits_per_raw_sample = 24;
        } else if (a == "libopus") {
            aenc->sample_fmt = AV_SAMPLE_FMT_FLT;
        } else {
            aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;   // aac
        }
        aenc->sample_rate = s.audioSampleRate;
        av_channel_layout_default(&aenc->ch_layout, s.audioChannels);
        if (a == "aac" || a == "libopus") aenc->bit_rate = s.audioBitrate;
        aenc->time_base = AVRational{1, s.audioSampleRate};
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER) aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ff::check(avcodec_open2(aenc.get(), codec, nullptr), "cannot open audio encoder " + a);
        ff::check(avcodec_parameters_from_context(astream->codecpar, aenc.get()), "audio encoder parameters");
        astream->time_base = aenc->time_base;
    }

    void drainCodec(AVCodecContext* ctx, AVStream* st, AVPacket* p, bool video) {
        for (;;) {
            const int r = avcodec_receive_packet(ctx, p);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return;
            ff::check(r, std::string(video ? "video" : "audio") + " encoding failed for " + where());
            if (video && p->duration == 0) p->duration = 1;   // one frame in encoder time base
            if (video && colourTags) {
                ff::check(av_bsf_send_packet(colourTags.get(), p), "vp9_metadata failed for " + where());
                drainColourTags(ctx, st, p);
                continue;
            }
            writePacket(ctx, st, p);
        }
    }

    void drainColourTags(AVCodecContext* ctx, AVStream* st, AVPacket* p) {
        for (;;) {
            const int r = av_bsf_receive_packet(colourTags.get(), p);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return;
            ff::check(r, "vp9_metadata failed for " + where());
            writePacket(ctx, st, p);
        }
    }

    void writePacket(AVCodecContext* ctx, AVStream* st, AVPacket* p) {
        av_packet_rescale_ts(p, ctx->time_base, st->time_base);
        p->stream_index = st->index;
        ff::check(av_interleaved_write_frame(fmt.get(), p), "cannot write packet to " + where());
    }

    // Send `n` pending samples (n <= frame size) as one frame in the encoder's sample format.
    void sendAudioFrame(int n) {
        av_frame_unref(aframe.get());
        aframe->format = aenc->sample_fmt;
        aframe->sample_rate = s.audioSampleRate;
        ff::check(av_channel_layout_copy(&aframe->ch_layout, &aenc->ch_layout), "audio layout");
        aframe->nb_samples = n;
        ff::check(av_frame_get_buffer(aframe.get(), 0), "cannot allocate audio frame");
        const int ch = s.audioChannels;
        const float* src = pending.data();
        switch (aenc->sample_fmt) {
            case AV_SAMPLE_FMT_FLTP:
                for (int c = 0; c < ch; ++c) {
                    float* dst = reinterpret_cast<float*>(aframe->extended_data[c]);
                    for (int i = 0; i < n; ++i) dst[i] = src[static_cast<size_t>(i) * ch + c];
                }
                break;
            case AV_SAMPLE_FMT_FLT:
                std::copy(src, src + static_cast<size_t>(n) * ch, reinterpret_cast<float*>(aframe->data[0]));
                break;
            case AV_SAMPLE_FMT_S16: {
                auto* dst = reinterpret_cast<int16_t*>(aframe->data[0]);
                for (size_t i = 0; i < static_cast<size_t>(n) * ch; ++i)
                    dst[i] = static_cast<int16_t>(std::clamp(std::lrint(src[i] * 32767.0), -32768L, 32767L));
                break;
            }
            case AV_SAMPLE_FMT_S32: {   // 24-bit in the top bits
                auto* dst = reinterpret_cast<int32_t*>(aframe->data[0]);
                for (size_t i = 0; i < static_cast<size_t>(n) * ch; ++i)
                    dst[i] = static_cast<int32_t>(std::clamp(std::lrint(src[i] * 8388607.0), -8388608L, 8388607L)) * 256;
                break;
            }
            default:
                throw MediaError("unsupported audio sample format");
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(n) * ch);
        aframe->pts = audioPts;
        audioPts += n;
        ff::check(avcodec_send_frame(aenc.get(), aframe.get()), "cannot send audio to encoder");
        drainCodec(aenc.get(), astream, apkt.get(), false);
    }

    void writeAudio(const float* data, int64_t frames) {
        if (finished) throw MediaError("encoder already finished");
        if (!aenc) throw MediaError("audio was not enabled for " + where());
        if (frames <= 0) return;
        pending.insert(pending.end(), data, data + frames * s.audioChannels);
        audioIn += frames;
        const int fs = aenc->frame_size > 0 ? aenc->frame_size : 1024;
        while (static_cast<int64_t>(pending.size()) >= static_cast<int64_t>(fs) * s.audioChannels) sendAudioFrame(fs);
    }

    // RGBA (any size) -> the encoder's picture: Lanczos when scaling, BT.709 full-range RGB -> limited-range YUV,
    // chroma sited left (horizontally co-sited, vertically centred for 4:2:0).
    SwsContext* converter(int w, int h, AVPixelFormat dst) {
        if (sws && swsW == w && swsH == h) return sws.get();
        SwsContext* c = sws_alloc_context();
        if (!c) throw MediaError("cannot create the picture converter");
        sws.reset(c);
        const bool scaling = w != s.width || h != s.height;
        const int flags = (scaling ? SWS_LANCZOS : SWS_BICUBIC) | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INP;
        av_opt_set_int(c, "srcw", w, 0);
        av_opt_set_int(c, "srch", h, 0);
        av_opt_set_int(c, "src_format", AV_PIX_FMT_RGBA, 0);
        av_opt_set_int(c, "dstw", s.width, 0);
        av_opt_set_int(c, "dsth", s.height, 0);
        av_opt_set_int(c, "dst_format", dst, 0);
        av_opt_set_int(c, "sws_flags", flags, 0);
        if (dst != AV_PIX_FMT_RGBA) {
            av_opt_set_int(c, "dst_h_chr_pos", 0, 0);
            av_opt_set_int(c, "dst_v_chr_pos", 128, 0);
        }
        ff::check(sws_init_context(c, nullptr, nullptr), "cannot set up the picture converter");
        if (dst != AV_PIX_FMT_RGBA) {
            const int* bt709 = sws_getCoefficients(SWS_CS_ITU709);
            sws_setColorspaceDetails(c, bt709, 1, bt709, 0, 0, 1 << 16, 1 << 16);   // full RGB -> limited YUV
        }
        swsW = w;
        swsH = h;
        return c;
    }

    void writeGif(const Image& img) {
        const Image* src = &img;
        if (img.width != s.width || img.height != s.height) {   // scale in RGBA first
            scaled.reset(s.width, s.height);
            SwsContext* c = converter(img.width, img.height, AV_PIX_FMT_RGBA);
            const uint8_t* in[4] = {img.pixels.data(), nullptr, nullptr, nullptr};
            const int inStride[4] = {img.stride(), 0, 0, 0};
            uint8_t* out[4] = {scaled.pixels.data(), nullptr, nullptr, nullptr};
            const int outStride[4] = {scaled.stride(), 0, 0, 0};
            sws_scale(c, in, inStride, 0, img.height, out, outStride);
            src = &scaled;
        }
        ff::check(av_frame_make_writable(yuv.get()), "encoder frame not writable");
        auto* pal = reinterpret_cast<uint32_t*>(yuv->data[1]);
        for (int k = 0; k < 256; ++k) pal[k] = 0xFF000000u | s.palette[static_cast<size_t>(k)];
        const auto& bayer = bayer8();
        for (int y = 0; y < s.height; ++y) {
            const uint8_t* p = src->pixels.data() + static_cast<size_t>(y) * src->stride();
            uint8_t* o = yuv->data[0] + static_cast<size_t>(y) * yuv->linesize[0];
            for (int x = 0; x < s.width; ++x, p += 4) {
                const float d = s.dither ? bayer[static_cast<size_t>((y & 7) * 8 + (x & 7))] * 16.0f : 0.0f;
                auto q = [&](uint8_t v) { return static_cast<int>(std::clamp(v + d, 0.0f, 255.0f)) >> 3; };
                o[x] = gifLut[static_cast<size_t>((q(p[0]) << 10) | (q(p[1]) << 5) | q(p[2]))];
            }
        }
    }

    void write(const Image& img) {
        if (finished) throw MediaError("encoder already finished");
        if (!enc) throw MediaError(where() + " has no video stream");
        if (img.width <= 0 || img.height <= 0 || img.pixels.size() != static_cast<size_t>(img.width) * img.height * 4)
            throw MediaError("empty or malformed frame for " + where());
        if (s.codec == "gif") {
            writeGif(img);
        } else {
            SwsContext* c = converter(img.width, img.height, pixfmt);
            ff::check(av_frame_make_writable(yuv.get()), "encoder frame not writable");
            const uint8_t* src[4] = {img.pixels.data(), nullptr, nullptr, nullptr};
            const int srcStride[4] = {img.stride(), 0, 0, 0};
            sws_scale(c, src, srcStride, 0, img.height, yuv->data, yuv->linesize);
            yuv->color_primaries = AVCOL_PRI_BT709;
            yuv->color_trc = AVCOL_TRC_BT709;
            yuv->colorspace = AVCOL_SPC_BT709;
            yuv->color_range = AVCOL_RANGE_MPEG;
            yuv->chroma_location = AVCHROMA_LOC_LEFT;
        }
        yuv->pts = written;
        yuv->duration = 1;
        ff::check(avcodec_send_frame(enc.get(), yuv.get()), "cannot send frame to encoder");
        ++written;
        drainCodec(enc.get(), stream, pkt.get(), true);
    }

    void finish() {
        if (finished) return;
        if (enc) {
            ff::check(avcodec_send_frame(enc.get(), nullptr), "cannot flush encoder");
            drainCodec(enc.get(), stream, pkt.get(), true);
            if (colourTags) {
                ff::check(av_bsf_send_packet(colourTags.get(), nullptr), "cannot flush vp9_metadata");
                drainColourTags(enc.get(), stream, pkt.get());
            }
        }
        if (aenc) {
            if (!pending.empty()) sendAudioFrame(static_cast<int>(pending.size() / s.audioChannels));   // short last frame
            ff::check(avcodec_send_frame(aenc.get(), nullptr), "cannot flush audio encoder");
            drainCodec(aenc.get(), astream, apkt.get(), false);
        }
        ff::check(av_write_trailer(fmt.get()), "cannot finalize " + where());
        finished = true;
        fmt.reset();   // closes the file
    }
};

VideoEncoder::VideoEncoder(const std::filesystem::path& file, const EncoderSettings& settings)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = file;
    impl_->s = settings;
    try {
        impl_->open();
    } catch (...) {
        impl_->fmt.reset();
        std::error_code ec;
        std::filesystem::remove(file, ec);
        throw;
    }
}

VideoEncoder::~VideoEncoder() {
    if (impl_ && !impl_->finished) {
        impl_->fmt.reset();
        std::error_code ec;
        std::filesystem::remove(impl_->path, ec);   // never leave a truncated, unplayable file behind
    }
}

void VideoEncoder::writeFrame(const Image& rgba) { impl_->write(rgba); }
void VideoEncoder::finish() { impl_->finish(); }
int64_t VideoEncoder::framesWritten() const { return impl_->written; }
void VideoEncoder::writeAudio(const float* interleaved, int64_t frames) { impl_->writeAudio(interleaved, frames); }
int64_t VideoEncoder::audioSamplesWritten() const { return impl_->audioIn; }

}  // namespace fa
