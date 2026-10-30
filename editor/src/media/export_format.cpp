#include "fa/media/export_format.h"

#include "ffmpeg_common.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

namespace fa {

const char* hardwareLabel(HardwareEncoding h) {
    switch (h) {
        case HardwareEncoding::Auto: return "Auto (hardware when it works, else CPU)";
        case HardwareEncoding::Cpu: return "CPU (software)";
        case HardwareEncoding::Nvenc: return "NVIDIA NVENC";
        case HardwareEncoding::Qsv: return "Intel Quick Sync";
        case HardwareEncoding::Amf: return "AMD AMF";
    }
    return "?";
}

const char* hardwareId(HardwareEncoding h) {
    switch (h) {
        case HardwareEncoding::Auto: return "auto";
        case HardwareEncoding::Cpu: return "cpu";
        case HardwareEncoding::Nvenc: return "nvenc";
        case HardwareEncoding::Qsv: return "qsv";
        case HardwareEncoding::Amf: return "amf";
    }
    return "cpu";
}

bool hardwareFromId(const std::string& id, HardwareEncoding& out) {
    for (HardwareEncoding h : {HardwareEncoding::Auto, HardwareEncoding::Cpu, HardwareEncoding::Nvenc, HardwareEncoding::Qsv,
                               HardwareEncoding::Amf})
        if (id == hardwareId(h)) {
            out = h;
            return true;
        }
    return false;
}

const std::vector<ExportFormat>& exportFormats() {
    static const std::vector<ExportFormat> formats{
        {"mp4-h264", "MP4 - H.264", ".mp4", "mp4", "h264", {"aac"}, {8}},
        {"mp4-hevc", "MP4 - HEVC (H.265)", ".mp4", "mp4", "hevc", {"aac"}, {8, 10}},
        {"mov-hevc", "MOV - HEVC (H.265)", ".mov", "mov", "hevc", {"aac", "pcm_s16le", "pcm_s24le"}, {8, 10}},
        {"mp4-av1", "MP4 - AV1", ".mp4", "mp4", "av1", {"aac", "libopus"}, {8, 10}},
        {"mkv-av1", "MKV - AV1", ".mkv", "matroska", "av1", {"libopus", "aac", "flac"}, {8, 10}},
        {"webm-vp9", "WebM - VP9", ".webm", "webm", "vp9", {"libopus"}, {8, 10}},
        {"mov-prores", "MOV - Apple ProRes 422 (prores_ks)", ".mov", "mov", "prores", {"pcm_s16le", "pcm_s24le", "aac"}, {10}},
        {"gif", "GIF (animated, no sound)", ".gif", "gif", "gif", {}, {8}},
        {"wav", "WAV (audio only)", ".wav", "wav", "", {"pcm_s16le", "pcm_s24le"}, {}},
        {"flac", "FLAC (audio only)", ".flac", "flac", "", {"flac"}, {}},
    };
    return formats;
}

const ExportFormat* findExportFormat(const std::string& id) {
    for (const auto& f : exportFormats())
        if (f.id == id) return &f;
    return nullptr;
}

const ExportFormat* exportFormatForPath(const std::string& path) {
    std::string ext;
    if (const auto dot = path.find_last_of('.'); dot != std::string::npos) ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // the default format of each extension (the first in the list with it)
    for (const char* id : {"mp4-h264", "mov-prores", "mkv-av1", "webm-vp9", "gif", "wav", "flac"})
        if (const ExportFormat* f = findExportFormat(id); f && f->extension == ext) return f;
    return nullptr;
}

const std::vector<VideoEncoderSpec>& videoEncoderSpecs() {
    static const std::string nv = "NVIDIA NVENC through nv-codec-headers (MIT); the encoder runs in the NVIDIA driver";
    static const std::string qsv = "Intel Quick Sync through oneVPL (MIT); the encoder runs in the Intel driver";
    static const std::string amf = "AMD AMF (not in this build: the vcpkg amd-amf archive failed its integrity check)";
    static const std::vector<VideoEncoderSpec> specs{
        {"libx264", "h264", HardwareEncoding::Cpu, {8}, "x264 (GPL-2.0-or-later)"},
        {"h264_nvenc", "h264", HardwareEncoding::Nvenc, {8}, nv},
        {"h264_qsv", "h264", HardwareEncoding::Qsv, {8}, qsv},
        {"h264_amf", "h264", HardwareEncoding::Amf, {8}, amf},
        {"libx265", "hevc", HardwareEncoding::Cpu, {8, 10}, "x265 (GPL-2.0-or-later)"},
        {"hevc_nvenc", "hevc", HardwareEncoding::Nvenc, {8, 10}, nv},
        {"hevc_qsv", "hevc", HardwareEncoding::Qsv, {8, 10}, qsv},
        {"hevc_amf", "hevc", HardwareEncoding::Amf, {8, 10}, amf},
        {"libsvtav1", "av1", HardwareEncoding::Cpu, {8, 10}, "SVT-AV1 (BSD-3-Clause-Clear + AOMedia patent licence)"},
        {"av1_nvenc", "av1", HardwareEncoding::Nvenc, {8, 10}, nv},
        {"av1_qsv", "av1", HardwareEncoding::Qsv, {8, 10}, qsv},
        {"av1_amf", "av1", HardwareEncoding::Amf, {8, 10}, amf},
        {"libvpx-vp9", "vp9", HardwareEncoding::Cpu, {8, 10}, "libvpx (BSD-3-Clause + WebM patent grant)"},
        {"vp9_qsv", "vp9", HardwareEncoding::Qsv, {8, 10}, qsv},
        {"prores_ks", "prores", HardwareEncoding::Cpu, {10}, "FFmpeg prores_ks (LGPL-2.1-or-later)"},
        {"gif", "gif", HardwareEncoding::Cpu, {8}, "FFmpeg gif (LGPL-2.1-or-later) with MeerCut's palette"},
    };
    return specs;
}

namespace {

bool hardwareFamily(const std::string& e) {
    auto ends = [&](const char* t) {
        const size_t n = std::strlen(t);
        return e.size() >= n && e.compare(e.size() - n, n, t) == 0;
    };
    return ends("_nvenc") || ends("_qsv") || ends("_amf");
}

AVPixelFormat sessionPixelFormat(const std::string& encoder, int bitDepth) {
    if (encoder == "prores_ks") return AV_PIX_FMT_YUV422P10LE;
    if (encoder == "gif") return AV_PIX_FMT_PAL8;
    if (hardwareFamily(encoder)) return bitDepth == 10 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    return bitDepth == 10 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
}

// Open the encoder at w x h, encode a few grey frames and flush; "" on success, else why not.
std::string testSession(const std::string& name, int bitDepth, int w, int h, const FrameRate& rate) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec) return "not in this FFmpeg build";
    try {
        ff::CodecContextPtr c(avcodec_alloc_context3(codec));
        if (!c) return "out of memory";
        const AVPixelFormat pf = sessionPixelFormat(name, bitDepth);
        c->width = w;
        c->height = h;
        c->pix_fmt = pf;
        c->time_base = ff::toAV(rate.frameDuration());
        c->framerate = ff::toAV(rate.fps());
        c->color_primaries = AVCOL_PRI_BT709;
        c->color_trc = AVCOL_TRC_BT709;
        c->colorspace = AVCOL_SPC_BT709;
        c->color_range = AVCOL_RANGE_MPEG;
        if (hardwareFamily(name)) c->bit_rate = 2000000;
        AVDictionary* opts = nullptr;
        if (name == "libx264" || name == "libx265") av_dict_set(&opts, "preset", "ultrafast", 0);
        if (name == "libx265") av_dict_set(&opts, "x265-params", "log-level=error", 0);
        if (name == "libsvtav1") av_dict_set(&opts, "preset", "12", 0);
        if (name == "libvpx-vp9") {
            av_dict_set(&opts, "deadline", "realtime", 0);
            av_dict_set(&opts, "cpu-used", "8", 0);
        }
        const int r = avcodec_open2(c.get(), codec, &opts);
        av_dict_free(&opts);
        if (r < 0) return "cannot open: " + ff::errorString(r);
        ff::FramePtr f = ff::makeFrame();
        f->format = pf;
        f->width = w;
        f->height = h;
        if (av_frame_get_buffer(f.get(), 0) < 0) return "cannot allocate a test frame";
        if (pf == AV_PIX_FMT_PAL8) {
            std::memset(f->data[0], 0, static_cast<size_t>(f->linesize[0]) * h);
            std::memset(f->data[1], 0x80, 1024);   // the palette
        } else {
            const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(pf);
            for (int p = 0; p < 4 && f->data[p]; ++p) {
                const int ph = p == 0 ? h : AV_CEIL_RSHIFT(h, d->log2_chroma_h);
                std::memset(f->data[p], 0x02, static_cast<size_t>(f->linesize[p]) * ph);   // valid at 8 and 10 bits
            }
        }
        ff::PacketPtr pkt = ff::makePacket();
        int packets = 0;
        auto drain = [&]() -> std::string {
            for (;;) {
                const int rr = avcodec_receive_packet(c.get(), pkt.get());
                if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) return {};
                if (rr < 0) return "encoding failed: " + ff::errorString(rr);
                ++packets;
                av_packet_unref(pkt.get());
            }
        };
        for (int i = 0; i < 3; ++i) {
            f->pts = i;
            const int sr = avcodec_send_frame(c.get(), f.get());
            if (sr < 0) return "cannot encode: " + ff::errorString(sr);
            if (std::string why = drain(); !why.empty()) return why;
        }
        const int fr = avcodec_send_frame(c.get(), nullptr);
        if (fr < 0) return "cannot flush: " + ff::errorString(fr);
        if (std::string why = drain(); !why.empty()) return why;
        if (packets == 0) return "no output";
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}

}  // namespace

namespace {

// The 256x144 test session of an encoder at a bit depth, run once per process: "" = it works here.
std::string cachedSession(const std::string& encoder, int bitDepth) {
    static std::mutex m;
    static std::map<std::pair<std::string, int>, std::string> cache;
    std::lock_guard lk(m);
    const auto key = std::make_pair(encoder, bitDepth);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    ff::ensureInit();
    ff::quietSvtAv1();
    const int level = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET);   // failing hardware sessions are expected on most machines
    const std::string why = testSession(encoder, bitDepth, 256, 144, FrameRate::fps30());
    av_log_set_level(level);
    return cache.emplace(key, why).first->second;
}

bool offers(const VideoEncoderSpec& s, int bitDepth) {
    return std::find(s.bitDepths.begin(), s.bitDepths.end(), bitDepth) != s.bitDepths.end();
}

}  // namespace

const std::vector<EncoderProbe>& probeEncoders() {
    static std::once_flag once;
    static std::vector<EncoderProbe> probes;
    std::call_once(once, [] {
        ff::ensureInit();
        for (const auto& spec : videoEncoderSpecs()) {
            EncoderProbe p;
            p.encoder = spec.encoder;
            p.compiled = avcodec_find_encoder_by_name(spec.encoder.c_str()) != nullptr;
            if (!p.compiled) {
                p.detail = "not in this FFmpeg build";
            } else {
                std::string why8, why10;
                if (offers(spec, 8)) p.works8 = (why8 = cachedSession(spec.encoder, 8)).empty();
                if (offers(spec, 10)) p.works10 = (why10 = cachedSession(spec.encoder, 10)).empty();
                if (!why8.empty()) p.detail = "8-bit: " + why8;
                if (!why10.empty()) p.detail += (p.detail.empty() ? "" : "; ") + std::string("10-bit: ") + why10;
            }
            probes.push_back(std::move(p));
        }
        for (const char* a : {"aac", "libopus", "pcm_s16le", "pcm_s24le", "flac"}) {
            EncoderProbe p;
            p.encoder = a;
            p.compiled = avcodec_find_encoder_by_name(a) != nullptr;
            p.works8 = p.compiled;
            if (!p.compiled) p.detail = "not in this FFmpeg build";
            probes.push_back(std::move(p));
        }
    });
    return probes;
}

const EncoderProbe* encoderProbe(const std::string& encoder) {
    for (const auto& p : probeEncoders())
        if (p.encoder == encoder) return &p;
    return nullptr;
}

bool audioEncoderAvailable(const std::string& encoder) {
    ff::ensureInit();
    return avcodec_find_encoder_by_name(encoder.c_str()) != nullptr;
}

bool encoderWorks(const std::string& encoder, int bitDepth) {
    ff::ensureInit();
    return avcodec_find_encoder_by_name(encoder.c_str()) && cachedSession(encoder, bitDepth).empty();
}

std::vector<int> availableBitDepths(const ExportFormat& f, HardwareEncoding preference) {
    std::vector<int> out;
    for (int d : f.bitDepths) {
        bool ok = false;
        for (const auto& s : videoEncoderSpecs()) {
            if (s.codec != f.videoCodec || !offers(s, d)) continue;
            const bool allowed = s.family == HardwareEncoding::Cpu ||
                                 preference == HardwareEncoding::Auto || preference == s.family;
            if (allowed && encoderWorks(s.encoder, d)) ok = true;
        }
        if (ok) out.push_back(d);
    }
    return out;
}

EncoderChoice chooseVideoEncoder(const std::string& codec, HardwareEncoding preference, int bitDepth, int width,
                                 int height, const FrameRate& rate) {
    ff::ensureInit();
    const VideoEncoderSpec* cpu = nullptr;
    std::vector<const VideoEncoderSpec*> hw;
    for (const auto& s : videoEncoderSpecs()) {
        if (s.codec != codec) continue;
        if (s.family == HardwareEncoding::Cpu) cpu = &s;
        else if (preference == HardwareEncoding::Auto || preference == s.family) hw.push_back(&s);
    }
    if (!cpu) throw MediaError("no " + codec + " encoder is known");
    EncoderChoice out;
    const bool askedForHardware = preference != HardwareEncoding::Cpu;
    if (askedForHardware && hw.empty() && preference != HardwareEncoding::Auto)
        out.note = std::string(hardwareLabel(preference)) + " has no " + codec + " encoder";
    for (const VideoEncoderSpec* s : hw) {
        std::string why;
        if (!offers(*s, bitDepth)) {
            why = std::to_string(bitDepth) + "-bit is not offered";
        } else if (!avcodec_find_encoder_by_name(s->encoder.c_str())) {
            why = "not in this FFmpeg build";
        } else if (const std::string probe = cachedSession(s->encoder, bitDepth); !probe.empty()) {
            why = probe;
        } else {   // and at the export's own size and rate
            const int level = av_log_get_level();
            av_log_set_level(AV_LOG_QUIET);
            why = testSession(s->encoder, bitDepth, width, height, rate);
            av_log_set_level(level);
        }
        if (why.empty()) {
            out.encoder = s->encoder;
            out.hardware = true;
            out.note += (out.note.empty() ? "" : "; ") + std::string("using ") + s->encoder;
            return out;
        }
        out.note += (out.note.empty() ? "" : "; ") + s->encoder + ": " + why;
    }
    if (!avcodec_find_encoder_by_name(cpu->encoder.c_str()))
        throw MediaError(cpu->encoder + " is not in this FFmpeg build" + (out.note.empty() ? "" : " (" + out.note + ")"));
    if (!offers(*cpu, bitDepth))
        throw MediaError(codec + " " + std::to_string(bitDepth) + "-bit is not offered" +
                         (out.note.empty() ? "" : " (" + out.note + ")"));
    if (const std::string why = cachedSession(cpu->encoder, bitDepth); !why.empty())
        throw MediaError(cpu->encoder + " cannot make " + std::to_string(bitDepth) + "-bit video in this build (" + why +
                         ")" + (out.note.empty() ? std::string() : "; " + out.note));
    out.encoder = cpu->encoder;
    out.hardware = false;
    if (askedForHardware && !hw.empty())
        out.note += (out.note.empty() ? "" : "; ") + std::string("falling back to ") + cpu->encoder;
    return out;
}

int nativeQuality(const std::string& encoder, int level) {
    level = std::clamp(level, 0, 4);
    auto pick = [&](std::initializer_list<int> v) { return *(v.begin() + level); };
    if (encoder == "libx264") return pick({28, 25, 23, 20, 18});
    if (encoder == "libx265") return pick({30, 28, 25, 22, 20});
    if (encoder == "libsvtav1") return pick({45, 40, 35, 30, 25});
    if (encoder == "libvpx-vp9") return pick({42, 37, 33, 28, 24});
    if (hardwareFamily(encoder)) return pick({30, 27, 24, 21, 19});
    return 0;
}

}  // namespace fa
