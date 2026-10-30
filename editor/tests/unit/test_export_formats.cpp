// M6 checkpoint D: export formats, encoder choice and the encoder paths (the container-level EXP-007 checks with
// ffprobe are in tests/e2e/m6_export_e2e.py).
#include "fa/media/export_format.h"
#include "fa/media/media.h"
#include "fa/model/edit_ops.h"
#include "fa/model/serialize.h"
#include "fa/render/renderer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <set>
#include <string>
#include <vector>

using namespace fa;

namespace {

std::filesystem::path media(const char* name) { return std::filesystem::path(FA_TESTMEDIA_DIR) / name; }
std::filesystem::path temp(const char* name) { return std::filesystem::temp_directory_path() / name; }

Image gradient(int w, int h, int seed) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.pixels.data() + (static_cast<size_t>(y) * w + x) * 4;
            p[0] = static_cast<uint8_t>((x * 255) / std::max(1, w - 1));
            p[1] = static_cast<uint8_t>((y * 255) / std::max(1, h - 1));
            p[2] = static_cast<uint8_t>((seed * 37) & 255);
            p[3] = 255;
        }
    return img;
}

std::vector<float> tone(int frames, double f) {
    std::vector<float> x(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) x[static_cast<size_t>(i) * 2] = x[static_cast<size_t>(i) * 2 + 1] =
        static_cast<float>(0.25 * std::sin(2 * std::numbers::pi * f * i / 48000));
    return x;
}

// Encode 10 frames (+ 1/3 s of audio when enabled) and probe the result.
MediaInfo encode(const std::filesystem::path& out, EncoderSettings s, bool withAudio, int srcW = 0, int srcH = 0) {
    s.audio = withAudio;
    VideoEncoder enc(out, s);
    const auto audio = tone(1600, 1000);
    for (int i = 0; i < 10; ++i) {
        if (!s.codec.empty()) enc.writeFrame(gradient(srcW ? srcW : s.width, srcH ? srcH : s.height, i));
        if (withAudio) enc.writeAudio(audio.data(), 1600);
    }
    enc.finish();
    return probeMedia(out);
}

Project twoClipProject() {
    Project p;
    p.sources = {MediaSource{"A", media("A_red_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920, 1080,
                             true, true}};
    Sequence s;
    s.id = "s";
    s.width = 320;
    s.height = 180;
    p.sequences.push_back(s);
    UndoStack st(p);
    st.execute(edit::addMedia(p, "s", "A", Rational(0)));
    st.execute(edit::trimClip(p, "s", p.sequences[0].tracks[0].clips[0].id, ClipEdge::Tail, Rational(1)));
    return p;
}

}  // namespace

TEST(ExportFormats, TableIsConsistent) {
    std::set<std::string> ids;
    for (const auto& f : exportFormats()) {
        EXPECT_TRUE(ids.insert(f.id).second) << f.id;
        EXPECT_EQ(f.extension.front(), '.');
        EXPECT_FALSE(f.muxer.empty());
        if (f.videoCodec.empty()) {
            EXPECT_FALSE(f.audioCodecs.empty()) << f.id;   // audio-only formats need audio
            EXPECT_TRUE(f.bitDepths.empty());
        } else {
            EXPECT_FALSE(f.bitDepths.empty()) << f.id;
            bool hasCpu = false;
            for (const auto& e : videoEncoderSpecs()) hasCpu |= e.codec == f.videoCodec && e.family == HardwareEncoding::Cpu;
            EXPECT_TRUE(hasCpu) << f.id << " has no CPU encoder";
        }
    }
    EXPECT_EQ(findExportFormat("gif")->audioCodecs.size(), 0u);   // GIF: no sound
    EXPECT_EQ(exportFormatForPath("C:/x/clip.MP4")->id, "mp4-h264");
    EXPECT_EQ(exportFormatForPath("clip.mov")->id, "mov-prores");
    EXPECT_EQ(exportFormatForPath("clip.webm")->id, "webm-vp9");
    EXPECT_EQ(exportFormatForPath("clip.flac")->id, "flac");
    EXPECT_EQ(exportFormatForPath("clip.avi"), nullptr);
    HardwareEncoding h;
    EXPECT_TRUE(hardwareFromId("nvenc", h));
    EXPECT_EQ(h, HardwareEncoding::Nvenc);
    EXPECT_FALSE(hardwareFromId("cuda", h));
    // quality levels get better (lower values) from level 0 to 4 for every constant-quality encoder
    for (const char* e : {"libx264", "libx265", "libsvtav1", "libvpx-vp9", "h264_nvenc", "hevc_qsv"})
        for (int l = 0; l < 4; ++l) EXPECT_GT(nativeQuality(e, l), nativeQuality(e, l + 1)) << e;
}

TEST(ExportFormats, CpuEncodersOfEveryFormatAreBuiltAndWork) {
    for (const auto& f : exportFormats()) {
        if (f.videoCodec.empty()) {
            for (const auto& a : f.audioCodecs) EXPECT_TRUE(audioEncoderAvailable(a)) << a;
            continue;
        }
        // every format can be made with its CPU encoder at its first bit depth (8-bit; ProRes 10-bit)
        const EncoderChoice c = chooseVideoEncoder(f.videoCodec, HardwareEncoding::Cpu, f.bitDepths.front(), 640, 360,
                                                   FrameRate::fps30());
        EXPECT_FALSE(c.hardware);
        EXPECT_TRUE(encoderWorks(c.encoder, f.bitDepths.front())) << c.encoder;
        // the bit depths offered with CPU encoding are exactly those whose CPU session works (this build's x265 and
        // libvpx are 8-bit only, measured: 10-bit HEVC / VP9 need a hardware encoder)
        for (int d : availableBitDepths(f, HardwareEncoding::Cpu)) EXPECT_TRUE(encoderWorks(c.encoder, d)) << c.encoder << " " << d;
        const auto cpuDepths = availableBitDepths(f, HardwareEncoding::Cpu);
        for (int d : f.bitDepths)
            if (std::find(cpuDepths.begin(), cpuDepths.end(), d) == cpuDepths.end())
                EXPECT_THROW(chooseVideoEncoder(f.videoCodec, HardwareEncoding::Cpu, d, 640, 360, FrameRate::fps30()), MediaError)
                    << f.id << " " << d;
    }
}

TEST(ExportFormats, HardwareRequestsFallBackHonestly) {
    // AMF is not in this build: asking for it gives the CPU encoder and says why
    const EncoderChoice amf = chooseVideoEncoder("h264", HardwareEncoding::Amf, 8, 640, 360, FrameRate::fps30());
    EXPECT_EQ(amf.encoder, "libx264");
    EXPECT_FALSE(amf.hardware);
    EXPECT_NE(amf.note.find("h264_amf"), std::string::npos) << amf.note;
    EXPECT_NE(amf.note.find("falling back"), std::string::npos) << amf.note;
    // ProRes has no hardware encoder at all
    const EncoderChoice prores = chooseVideoEncoder("prores", HardwareEncoding::Auto, 10, 640, 360, FrameRate::fps30());
    EXPECT_EQ(prores.encoder, "prores_ks");
    EXPECT_FALSE(prores.hardware);
    // 10-bit H.264 is not offered; an unknown codec has no encoder
    EXPECT_THROW(chooseVideoEncoder("h264", HardwareEncoding::Cpu, 10, 640, 360, FrameRate::fps30()), MediaError);
    EXPECT_THROW(chooseVideoEncoder("theora", HardwareEncoding::Cpu, 8, 640, 360, FrameRate::fps30()), MediaError);
    // whatever Auto picks for AV1 here, it is only hardware if a real session opened (never assumed)
    const EncoderChoice av1 = chooseVideoEncoder("av1", HardwareEncoding::Auto, 8, 640, 360, FrameRate::fps30());
    if (av1.hardware) {
        const EncoderProbe* p = encoderProbe(av1.encoder);
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(p->works8);
    } else {
        EXPECT_EQ(av1.encoder, "libsvtav1");
    }
    std::cout << "[ info ] Auto AV1 here: " << av1.encoder << " (" << av1.note << ")\n";
}

TEST(ExportFormats, EncoderPathsWriteWhatTheyClaim) {
    EncoderSettings base;
    base.width = 320;
    base.height = 180;
    base.preset = "ultrafast";
    struct Case {
        const char* file;
        const char* codec;
        const char* muxer;
        int depth;
        const char* audio;
        const char* wantCodec;
        const char* wantPix;
    };
    const Case cases[] = {
        {"fa_d_hevc.mp4", "libx265", "mp4", 8, "aac", "hevc", "yuv420p"},
        {"fa_d_av1_10.mp4", "libsvtav1", "mp4", 10, "aac", "av1", "yuv420p10le"},
        {"fa_d_av1.mkv", "libsvtav1", "matroska", 8, "libopus", "av1", "yuv420p"},
        {"fa_d_vp9.webm", "libvpx-vp9", "webm", 8, "libopus", "vp9", "yuv420p"},
        {"fa_d_prores.mov", "prores_ks", "mov", 10, "pcm_s24le", "prores", "yuv422p10le"},
    };
    for (const Case& c : cases) {
        EncoderSettings s = base;
        s.codec = c.codec;
        s.muxer = c.muxer;
        s.bitDepth = c.depth;
        s.audioCodec = c.audio;
        s.crf = nativeQuality(c.codec, 2);
        const MediaInfo mi = encode(temp(c.file), s, true);
        ASSERT_TRUE(mi.video) << c.file;
        EXPECT_EQ(mi.video->codec, c.wantCodec) << c.file;
        EXPECT_EQ(mi.video->pixelFormat, c.wantPix) << c.file;
        EXPECT_EQ(mi.video->width, 320);
        ASSERT_TRUE(mi.audio) << c.file;
        EXPECT_EQ(mi.audio->sampleRate, 48000);
        std::filesystem::remove(temp(c.file));
    }
    // frames larger than the output are scaled down
    EncoderSettings scaled = base;
    const MediaInfo small = encode(temp("fa_d_scaled.mp4"), scaled, false, 640, 360);
    ASSERT_TRUE(small.video);
    EXPECT_EQ(small.video->width, 320);
    EXPECT_EQ(small.video->height, 180);
    std::filesystem::remove(temp("fa_d_scaled.mp4"));
    // audio only: WAV 24-bit and FLAC
    for (const char* a : {"pcm_s24le", "flac"}) {
        EncoderSettings s = base;
        s.codec.clear();
        s.muxer = std::string(a) == "flac" ? "flac" : "wav";
        s.audioCodec = a;
        s.audioBits = 24;
        const auto out = temp(std::string(a) == "flac" ? "fa_d_audio.flac" : "fa_d_audio.wav");
        const MediaInfo mi = encode(out, s, true);
        EXPECT_FALSE(mi.video);
        ASSERT_TRUE(mi.audio);
        EXPECT_EQ(mi.audio->codec, std::string(a) == "flac" ? "flac" : "pcm_s24le");
        std::vector<float> pcm(1600 * 2);
        {
            AudioDecoder dec(out);
            dec.read(1600, 1600, pcm.data());   // the second chunk written (each chunk restarts the tone)
        }
        const auto ref = tone(1600, 1000);
        double worst = 0;
        for (size_t i = 0; i < pcm.size(); ++i) worst = std::max(worst, double(std::fabs(pcm[i] - ref[i])));
        EXPECT_LT(worst, 2e-6) << a;   // 24-bit lossless: within half an LSB
        std::filesystem::remove(out);
    }
    // VP9 on Quick Sync ignores ICQ (every quality value gave the same file, M6 F): its quality value is a VBR target,
    // so the quality presets must change the result. Noise frames (no encoder can make them small for free).
    if (encoderWorks("vp9_qsv", 8)) {
        std::vector<uintmax_t> sizes;
        for (int level : {0, 4}) {
            EncoderSettings s = base;
            s.codec = "vp9_qsv";
            s.muxer = "webm";
            s.crf = nativeQuality("vp9_qsv", level);
            const auto out = temp("fa_f_vp9_qsv.webm");
            {
                VideoEncoder enc(out, s);
                uint32_t seed = 1;
                for (int i = 0; i < 30; ++i) {
                    Image img(s.width, s.height);
                    for (auto& b : img.pixels) b = static_cast<uint8_t>((seed = seed * 1664525u + 1013904223u) >> 24);
                    enc.writeFrame(img);
                }
                enc.finish();
            }
            sizes.push_back(std::filesystem::file_size(out));
            std::filesystem::remove(out);
        }
        EXPECT_GT(sizes[1], sizes[0] * 3) << "Small file " << sizes[0] << " bytes, High " << sizes[1] << " bytes";
    }
    // a video-less encoder without audio is refused
    EncoderSettings none = base;
    none.codec.clear();
    EXPECT_THROW(VideoEncoder(temp("fa_d_none.wav"), none), MediaError);
    // Opus needs 48 kHz (or 24 / 16 / 12 / 8 kHz)
    EncoderSettings opus = base;
    opus.codec.clear();
    opus.muxer = "matroska";
    opus.audio = true;
    opus.audioCodec = "libopus";
    opus.audioSampleRate = 44100;
    EXPECT_THROW(VideoEncoder(temp("fa_d_opus.mka"), opus), MediaError);
}

TEST(ExportFormats, GifPaletteKeepsFewColoursExactly) {
    Image img(64, 32);
    const uint32_t colours[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF};
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x) {
            const uint32_t c = colours[(x / 16) % 4];
            uint8_t* p = img.pixels.data() + (static_cast<size_t>(y) * 64 + x) * 4;
            p[0] = static_cast<uint8_t>(c >> 16);
            p[1] = static_cast<uint8_t>(c >> 8);
            p[2] = static_cast<uint8_t>(c);
            p[3] = 255;
        }
    const auto pal = buildGifPalette({&img});
    ASSERT_EQ(pal.size(), 256u);
    for (uint32_t c : colours) EXPECT_NE(std::find(pal.begin(), pal.end(), c), pal.end()) << std::hex << c;   // exact
    EXPECT_EQ(buildGifPalette({&img}), pal);   // deterministic
    // a GIF written with it decodes to those colours
    EncoderSettings s;
    s.width = 64;
    s.height = 32;
    s.codec = "gif";
    s.muxer = "gif";
    s.palette = pal;
    s.dither = false;
    {
        VideoEncoder enc(temp("fa_d_palette.gif"), s);
        enc.writeFrame(img);
        enc.finish();
    }
    Rgba8 px;
    {
        VideoDecoder dec(temp("fa_d_palette.gif"));   // closed again before the file is removed
        px = dec.frameAt(Time(0)).image.at(40, 16);  // the blue band, exactly
    }
    EXPECT_EQ(px.r, 0);
    EXPECT_EQ(px.g, 0);
    EXPECT_EQ(px.b, 255);
    std::filesystem::remove(temp("fa_d_palette.gif"));
}

TEST(ExportFormats, RendererFormatsRatesAndSafeOutput) {
    Project p = twoClipProject();
    RenderOptions opt;
    opt.encoder.preset = "ultrafast";
    // an existing finished file is never destroyed by a failed or cancelled export
    const auto out = temp("fa_d_keep.mp4");
    {
        std::ofstream f(out, std::ios::binary);
        f << "previous export";
    }
    std::atomic<bool> cancel{false};
    opt.cancel = &cancel;
    opt.progress = [&](int64_t done, int64_t) {
        if (done == 5) cancel = true;
    };
    EXPECT_THROW(renderProject(p, "", out, opt), RenderCancelled);
    {
        std::ifstream f(out, std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(f)), {});
        EXPECT_EQ(s, "previous export");
    }
    opt.cancel = nullptr;
    opt.progress = nullptr;
    // a finished export replaces it
    RenderReport r = renderProject(p, "", out, opt);
    EXPECT_EQ(r.videoEncoder, "libx264");
    EXPECT_EQ(r.format, "mp4-h264");
    EXPECT_EQ(r.framesWritten, 30);
    EXPECT_GT(std::filesystem::file_size(out), 1000u);
    // another frame rate and size: 1 s at 23.976 = 24 frames, 160x90
    opt.outputRate = FrameRate(Rational(24000, 1001));
    opt.outputWidth = 160;
    opt.outputHeight = 90;
    r = renderProject(p, "", out, opt);
    EXPECT_EQ(r.framesWritten, 24);
    MediaInfo mi = probeMedia(out);
    ASSERT_TRUE(mi.video);
    EXPECT_EQ(mi.video->width, 160);
    EXPECT_EQ(mi.video->frameRate, FrameRate(Rational(24000, 1001)));
    EXPECT_EQ(r.audioSamples, 48048);   // 24 frames of 1001/24000 s
    opt.outputRate.reset();
    opt.outputWidth = opt.outputHeight = 0;
    // audio-only WAV: no video rendered
    const auto wav = temp("fa_d_mix.wav");
    r = renderProject(p, "", wav, opt);
    EXPECT_EQ(r.framesWritten, 0);
    EXPECT_EQ(r.audioSamples, 48000);
    mi = probeMedia(wav);
    EXPECT_FALSE(mi.video);
    ASSERT_TRUE(mi.audio);
    EXPECT_EQ(mi.audio->codec, "pcm_s16le");
    std::filesystem::remove(wav);
    // refused combinations
    opt.includeAudio = false;
    EXPECT_THROW(renderProject(p, "", wav, opt), RenderError);   // an audio format without audio
    opt.includeAudio = true;
    opt.format = "webm-vp9";
    opt.audioCodec = "aac";   // AAC cannot go into WebM
    EXPECT_THROW(renderProject(p, "", temp("fa_d_bad.webm"), opt), RenderError);
    opt.audioCodec.clear();
    opt.format = "mp4-h264";
    opt.encoder.bitDepth = 10;   // 10-bit H.264 is not offered
    EXPECT_THROW(renderProject(p, "", out, opt), RenderError);
    opt.encoder.bitDepth = 8;
    opt.format = "no-such-format";
    EXPECT_THROW(renderProject(p, "", out, opt), RenderError);
    std::filesystem::remove(out);
}

// M6 E: In / Out marks in the model, and exporting only the selected clips (with their linked partners).
TEST(ExportRanges, MarksAndSelectedClips) {
    Project p;
    p.sources = {MediaSource{"A", media("A_red_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920, 1080,
                             true, true},
                 MediaSource{"B", media("B_blue_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920, 1080,
                             true, true}};
    Sequence s;
    s.id = "s";
    s.width = 320;
    s.height = 180;
    p.sequences.push_back(s);
    UndoStack st(p);
    edit::AddedClips a, b;
    st.execute(edit::addMedia(p, "s", "A", Rational(0), {}, &a));
    st.execute(edit::trimClip(p, "s", a.videoClipId, ClipEdge::Tail, Rational(1)));
    st.execute(edit::addMedia(p, "s", "B", Rational(1), {}, &b));
    st.execute(edit::trimClip(p, "s", b.videoClipId, ClipEdge::Tail, Rational(2)));
    st.execute(edit::setTransition(p, "s", a.videoClipId, TransitionRef{"fa.transition.dissolve", Rational(1, 3)}));
    // marks: round trip, validation
    p.sequences[0].markIn = Rational(1, 2);
    p.sequences[0].markOut = Rational(3, 2);
    EXPECT_EQ(projectFromJsonString(projectToJsonString(p)), p);
    auto errors = [](const Project& q) {
        int n = 0;
        for (const auto& i : validate(q)) n += i.severity == ValidationIssue::Severity::Error;
        return n;
    };
    EXPECT_EQ(errors(p), 0);
    Project bad = p;
    bad.sequences[0].markOut = Rational(1, 2);   // not after In
    EXPECT_GT(errors(bad), 0);
    bad.sequences[0].markOut = Rational(1, 45);   // not on a frame
    EXPECT_GT(errors(bad), 0);
    // the In to Out range through the renderer: frames 15 .. 44
    RenderOptions opt;
    opt.encoder.preset = "ultrafast";
    opt.firstFrame = 15;
    opt.lastFrame = 44;
    const auto out = temp("fa_e_range.mp4");
    RenderReport r = renderProject(p, "", out, opt);
    EXPECT_EQ(r.framesWritten, 30);
    EXPECT_EQ(r.audioSamples, 48000);
    // only B (its audio comes with it), the dissolve into it is dropped with A: 1 s, blue from the first frame
    opt.firstFrame = 0;
    opt.lastFrame = -1;
    opt.onlyClips = {b.videoClipId};
    r = renderProject(p, "", out, opt);
    EXPECT_EQ(r.framesWritten, 30);
    EXPECT_EQ(r.audioSamples, 48000);
    {
        VideoDecoder dec(out);
        const Rgba8 px = dec.frameAt(Time(0)).image.at(160, 90);
        EXPECT_GT(px.b, 150);
        EXPECT_LT(px.r, 60);
    }
    {
        AudioDecoder dec(out);
        std::vector<float> pcm(4800 * 2);
        dec.read(24000, 4800, pcm.data());
        double c = 0, s1 = 0, c2 = 0, s2 = 0;   // B's 1.5 kHz, none of A's 1 kHz
        for (int i = 0; i < 4800; ++i) {
            const double v = pcm[static_cast<size_t>(i) * 2];
            c += v * std::cos(2 * std::numbers::pi * 1500 * i / 48000);
            s1 += v * std::sin(2 * std::numbers::pi * 1500 * i / 48000);
            c2 += v * std::cos(2 * std::numbers::pi * 1000 * i / 48000);
            s2 += v * std::sin(2 * std::numbers::pi * 1000 * i / 48000);
        }
        EXPECT_NEAR(2 * std::hypot(c, s1) / 4800, 0.125, 0.01);
        EXPECT_LT(2 * std::hypot(c2, s2) / 4800, 0.005);
    }
    opt.onlyClips = {"no-such-clip"};
    EXPECT_THROW(renderProject(p, "", out, opt), RenderError);
    std::filesystem::remove(out);
}

// M6 E: audio sample rate / channels of the export, the overlays rule of "selected clips", and outputs that cannot be
// written (a missing drive, a read-only file): an error, never a reported success, and an existing file stays intact.
TEST(ExportRanges, AudioFormatOverlaysAndUnwritableOutputs) {
    Project p;
    p.sources = {MediaSource{"A", media("A_red_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920, 1080,
                             true, true},
                 MediaSource{"B", media("B_blue_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920, 1080,
                             true, true}};
    Sequence s;
    s.id = "s";
    s.width = 320;
    s.height = 180;
    p.sequences.push_back(s);
    UndoStack st(p);
    edit::AddedClips b;
    st.execute(edit::addMedia(p, "s", "B", Rational(0), {}, &b));
    st.execute(edit::trimClip(p, "s", b.videoClipId, ClipEdge::Tail, Rational(1)));
    // an overlay above B: A's picture on a second video track (no audio), covering the frame
    Track v2{"v2", TrackKind::Video, "Video 2"};
    Clip over;
    over.id = "overlay";
    over.sourceId = "A";
    over.duration = Rational(1);
    v2.clips.push_back(over);
    p.sequences[0].tracks.push_back(v2);
    RenderOptions opt;
    opt.encoder.preset = "ultrafast";
    const auto out = temp("fa_e_overlay.mp4");
    auto centreAt = [&](double t) {
        VideoDecoder dec(out);
        return dec.frameAt(Rational::fromDouble(t, 1000)).image.at(160, 90);
    };
    // the whole timeline: the overlay (red) is on top
    renderProject(p, "", out, opt);
    EXPECT_GT(centreAt(0.5).r, 150);
    // "selected clips" alone = B and its audio: the overlay is left out (blue)
    opt.onlyClips = {b.videoClipId};
    renderProject(p, "", out, opt);
    EXPECT_GT(centreAt(0.5).b, 150);
    EXPECT_LT(centreAt(0.5).r, 60);
    // the selection's span with everything in it (what the window's option sends): the overlay is back
    opt.onlyClips.clear();
    opt.firstFrame = 0;
    opt.lastFrame = 29;
    renderProject(p, "", out, opt);
    EXPECT_GT(centreAt(0.5).r, 150);

    // 44.1 kHz mono: the mix is made at that rate; a mono source keeps its level
    opt.audioSampleRate = 44100;
    opt.audioChannels = 1;
    const RenderReport r = renderProject(p, "", out, opt);
    EXPECT_EQ(r.audioSamples, 44100);
    const MediaInfo mi = probeMedia(out);
    ASSERT_TRUE(mi.audio);
    EXPECT_EQ(mi.audio->sampleRate, 44100);
    EXPECT_EQ(mi.audio->channels, 1);
    {
        AudioDecoder dec(out, 44100, 1);
        std::vector<float> pcm(4410);
        dec.read(22050, 4410, pcm.data());
        double c = 0, sn = 0;
        for (int i = 0; i < 4410; ++i) {
            c += pcm[static_cast<size_t>(i)] * std::cos(2 * std::numbers::pi * 1500 * i / 44100);
            sn += pcm[static_cast<size_t>(i)] * std::sin(2 * std::numbers::pi * 1500 * i / 44100);
        }
        EXPECT_NEAR(2 * std::hypot(c, sn) / 4410, 0.125, 0.01);
    }
    opt.audioSampleRate = 0;
    opt.audioChannels = 2;
    // a stereo source folded to mono: (L + R) / 2 keeps a centred tone's level (swresample's default would add 3 dB)
    {
        const auto wav = temp("fa_e_centred.wav");
        {
            EncoderSettings ws;
            ws.codec.clear();
            ws.muxer = "wav";
            ws.audio = true;
            ws.audioCodec = "pcm_s16le";
            VideoEncoder w(wav, ws);
            std::vector<float> x(48000 * 2);
            for (int i = 0; i < 48000; ++i) x[static_cast<size_t>(i) * 2] = x[static_cast<size_t>(i) * 2 + 1] =
                static_cast<float>(0.2 * std::sin(2 * std::numbers::pi * 1000 * i / 48000));
            w.writeAudio(x.data(), 48000);
            w.finish();
        }
        AudioDecoder mono(wav, 48000, 1);
        std::vector<float> pcm(4800);
        mono.read(9600, 4800, pcm.data());
        float peak = 0;
        for (float v : pcm) peak = std::max(peak, std::fabs(v));
        EXPECT_NEAR(peak, 0.2f, 0.002f);
    }

    // an output on a drive that does not exist: an error, no file
    std::string drive;
    for (char d = 'Z'; d >= 'H' && drive.empty(); --d)
        if (!std::filesystem::exists(std::string(1, d) + ":\\")) drive = std::string(1, d);
    ASSERT_FALSE(drive.empty());
    EXPECT_ANY_THROW(renderProject(p, "", drive + ":\\meercut\\out.mp4", opt));
    // a read-only existing file cannot be replaced: an error, and the file is unchanged, no partial file left
    {
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        f << "previous export";
    }
    std::filesystem::permissions(out, std::filesystem::perms::owner_read, std::filesystem::perm_options::replace);
    EXPECT_THROW(renderProject(p, "", out, opt), RenderError);
    {
        std::ifstream f(out, std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(f)), {});
        EXPECT_EQ(got, "previous export");
    }
    auto partial = out;
    partial += ".partial.mp4";
    EXPECT_FALSE(std::filesystem::exists(partial));
    std::filesystem::permissions(out, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    std::filesystem::remove(out);
}
