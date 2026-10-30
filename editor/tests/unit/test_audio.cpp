// Audio decoding, mixing and AAC export, measured on the Stage 1 tone media:
// A_red_1080p30_10s.mp4 = mono AAC 48 kHz, 1 kHz sine at -18.06 dBFS (amplitude 0.125, verified in EXP-020);
// B_blue_1080p30_10s.mp4 = same with a 1.5 kHz sine; H_tone1k_-12dB_10s.wav = 1 kHz at amplitude 0.0314 (PCM).
#include "fa/media/media.h"
#include "fa/model/edit_ops.h"
#include "fa/render/audio_mix.h"
#include "fa/render/renderer.h"
#include "fa/model/time_map.h"

#include "../support/framecode.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <numbers>
#include <vector>

using namespace fa;

namespace {

std::filesystem::path media(const char* name) { return std::filesystem::path(FA_TESTMEDIA_DIR) / name; }

// Amplitude of frequency f in channel ch of interleaved stereo samples [from, from + n).
double toneAmplitude(const std::vector<float>& x, int ch, int64_t from, int n, double f, int rate = 48000) {
    double c = 0, s = 0;
    for (int i = 0; i < n; ++i) {
        const double v = x[static_cast<size_t>((from + i) * 2 + ch)];
        const double ph = 2 * std::numbers::pi * f * i / rate;
        c += v * std::cos(ph);
        s += v * std::sin(ph);
    }
    return 2 * std::hypot(c, s) / n;
}

}  // namespace

TEST(AudioDecoder, MonoAacIsCopiedToBothChannelsAtUnityGain) {
    AudioDecoder dec(media("A_red_1080p30_10s.mp4"));
    EXPECT_EQ(dec.info().sampleRate, 48000);
    EXPECT_EQ(dec.info().channels, 1);
    ASSERT_TRUE(dec.lengthSamples());
    EXPECT_NEAR(static_cast<double>(*dec.lengthSamples()), 480000.0, 1100.0);   // AAC frame granularity
    std::vector<float> buf(48000 * 2);
    dec.read(48000, 48000, buf.data());   // 1.0 .. 2.0 s
    EXPECT_NEAR(toneAmplitude(buf, 0, 0, 4800, 1000), 0.125, 0.003);
    EXPECT_NEAR(toneAmplitude(buf, 1, 0, 4800, 1000), 0.125, 0.003);
    EXPECT_LT(toneAmplitude(buf, 0, 0, 4800, 1500), 0.002);
}

TEST(AudioDecoder, PcmWavLevelAndSilenceOutsideTheStream) {
    AudioDecoder dec(media("H_tone1k_-12dB_10s.wav"));
    std::vector<float> buf(9600 * 2);
    dec.read(-4800, 9600, buf.data());   // half before the start (silence), half audio
    double pre = 0;
    for (int i = 0; i < 4800 * 2; ++i) pre = std::max(pre, std::fabs(double(buf[i])));
    EXPECT_EQ(pre, 0.0);
    EXPECT_NEAR(toneAmplitude(buf, 0, 4800, 4800, 1000), 0.0314, 0.0005);
    dec.read(480000 + 100, 4800, buf.data());   // after the end
    double post = 0;
    for (int i = 0; i < 4800 * 2; ++i) post = std::max(post, std::fabs(double(buf[i])));
    EXPECT_EQ(post, 0.0);
}

TEST(AudioDecoder, RandomAccessMatchesSequentialDecoding) {
    AudioDecoder seqDec(media("B_blue_1080p30_10s.mp4"));
    std::vector<float> all(480000 * 2);
    for (int64_t pos = 0; pos < 480000; pos += 4800) seqDec.read(pos, 4800, all.data() + pos * 2);
    EXPECT_EQ(seqDec.seekCount(), 0);
    AudioDecoder rnd(media("B_blue_1080p30_10s.mp4"));
    std::vector<float> chunk(1000 * 2);
    for (int64_t pos : {300000LL, 12345LL, 470000LL, 96000LL, 96000LL + 1000}) {
        rnd.read(pos, 1000, chunk.data());
        double maxDiff = 0;
        for (int i = 0; i < 2000; ++i) maxDiff = std::max(maxDiff, std::fabs(double(chunk[i] - all[pos * 2 + i])));
        EXPECT_LT(maxDiff, 1e-4) << "at sample " << pos;   // bit-exact decoding after a seek
    }
    EXPECT_GT(rnd.seekCount(), 0);
}


TEST(AudioMixer, CutSwitchesSourcesAtTheExactSample) {
    Project p;
    p.sources = {MediaSource{"A", media("A_red_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920, 1080,
                             true, true},
                 MediaSource{"B", media("B_blue_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920,
                             1080, true, true}};
    Sequence s;
    s.id = "s";
    s.frameRate = FrameRate::fps30();
    p.sequences.push_back(s);
    UndoStack st(p);
    edit::AddedClips a, b;
    st.execute(edit::addMedia(p, "s", "A", Rational(0), {}, &a));
    st.execute(edit::trimClip(p, "s", a.videoClipId, ClipEdge::Tail, Rational(2)));
    st.execute(edit::addMedia(p, "s", "B", Rational(2), {}, &b));
    const Sequence& seq = p.sequences[0];
    AudioMixer mixer("", 48000);
    std::vector<float> out(48000 * 2 * 4);
    mixer.mix(p, seq, 0, 48000 * 4, out.data());
    // before the cut: only 1 kHz; after: only 1.5 kHz (B starts at its own source 0)
    EXPECT_NEAR(toneAmplitude(out, 0, 96000 - 4800, 4800, 1000), 0.125, 0.003);
    EXPECT_LT(toneAmplitude(out, 0, 96000 - 4800, 4800, 1500), 0.002);
    EXPECT_NEAR(toneAmplitude(out, 1, 96000, 4800, 1500), 0.125, 0.003);
    EXPECT_LT(toneAmplitude(out, 1, 96000, 4800, 1000), 0.002);
    // sample-exact boundary: A contributes up to sample 95999, B from sample 96000 on. Since M6 both sides of a
    // hard cut get a 2 ms declick ramp (audio_mix.h), so the boundary is checked on each clip alone: A's samples
    // stop exactly at 96000 and B's start exactly there.
    Project onlyA = p, onlyB = p;
    onlyA.sequences[0].tracks[1].clips[1].enabled = false;
    onlyB.sequences[0].tracks[1].clips[0].enabled = false;
    std::vector<float> a1(4000 * 2), b1(4000 * 2);
    mixer.mix(onlyA, onlyA.sequences[0], 94000, 4000, a1.data());
    mixer.mix(onlyB, onlyB.sequences[0], 94000, 4000, b1.data());
    double aBefore = 0, aAfter = 0, bBefore = 0, bAfter = 0;
    for (int i = 0; i < 4000; ++i) {
        const double av = std::fabs(a1[static_cast<size_t>(i) * 2]), bv = std::fabs(b1[static_cast<size_t>(i) * 2]);
        if (94000 + i < 96000) aBefore = std::max(aBefore, av), bBefore = std::max(bBefore, bv);
        else aAfter = std::max(aAfter, av), bAfter = std::max(bAfter, bv);
    }
    EXPECT_GT(aBefore, 0.1);
    EXPECT_EQ(aAfter, 0.0);
    EXPECT_EQ(bBefore, 0.0);
    EXPECT_GT(bAfter, 0.1);
    EXPECT_NE(b1[(96001 - 94000) * 2], 0.0f);   // B's second sample (its sine starts at phase 0)
}

TEST(AudioMixer, GainMuteDisableAndRanges) {
    Project p;
    p.sources = {MediaSource{"A", media("A_red_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920, 1080,
                             true, true}};
    Sequence s;
    s.id = "s";
    s.frameRate = FrameRate::ntsc30();
    p.sequences.push_back(s);
    UndoStack st(p);
    edit::AddedClips a;
    st.execute(edit::addMedia(p, "s", "A", Rational(0), {}, &a));
    Sequence& seq = p.sequences[0];
    Track& at = seq.tracks[1];
    AudioMixer mixer("", 48000);
    std::vector<float> out(4800 * 2);
    at.clips[0].volumeDb.setConstant(-6.0206);   // half amplitude
    mixer.mix(p, seq, 48000, 4800, out.data());
    EXPECT_NEAR(toneAmplitude(out, 0, 0, 4800, 1000), 0.0625, 0.002);
    at.volumeDb = -6.0206;            // track gain adds
    mixer.mix(p, seq, 48000, 4800, out.data());
    EXPECT_NEAR(toneAmplitude(out, 0, 0, 4800, 1000), 0.03125, 0.001);
    at.muted = true;
    mixer.mix(p, seq, 48000, 4800, out.data());
    EXPECT_EQ(toneAmplitude(out, 0, 0, 4800, 1000), 0.0);
    at.muted = false;
    at.clips[0].enabled = false;
    mixer.mix(p, seq, 48000, 4800, out.data());
    EXPECT_EQ(toneAmplitude(out, 0, 0, 4800, 1000), 0.0);
    // sample range of a clip at an NTSC frame boundary: 1001/30000 s * 48000 = 1601.6 -> [1602, ...)
    Clip c;
    c.timelineStart = frameToTime(1, FrameRate::ntsc30());
    c.duration = frameToTime(1, FrameRate::ntsc30());
    EXPECT_EQ(clipSampleRange(c, 48000), (std::pair<int64_t, int64_t>{1602, 3204}));
}

TEST(AudioMixer, ToleratesMissingMediaWhenAsked) {
    Project p;
    p.sources = {MediaSource{"X", "C:/definitely/missing.mp4", Rational(10), FrameRate::fps30(), 1920, 1080, true, true}};
    Sequence s;
    s.id = "s";
    p.sequences.push_back(s);
    UndoStack st(p);
    st.execute(edit::addMedia(p, "s", "X", Rational(0)));
    AudioMixer mixer("", 48000);
    std::vector<float> out(100 * 2, 1.0f);
    EXPECT_THROW(mixer.mix(p, p.sequences[0], 0, 100, out.data()), MediaError);
    mixer.mix(p, p.sequences[0], 0, 100, out.data(), /*tolerateErrors=*/true);
    EXPECT_EQ(out[0], 0.0f);
    ASSERT_EQ(mixer.errors().size(), 1u);
}

TEST(Export, VideoAndAacAudioAreSynchronised) {
    Project p;
    p.sources = {MediaSource{"A", media("A_red_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920, 1080,
                             true, true},
                 MediaSource{"B", media("B_blue_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920,
                             1080, true, true}};
    Sequence s;
    s.id = "s";
    s.width = 640;
    s.height = 360;
    s.frameRate = FrameRate::fps30();
    p.sequences.push_back(s);
    UndoStack st(p);
    edit::AddedClips a, b;
    st.execute(edit::addMedia(p, "s", "A", Rational(0), {}, &a));
    st.execute(edit::trimClip(p, "s", a.videoClipId, ClipEdge::Tail, Rational(1)));
    st.execute(edit::addMedia(p, "s", "B", Rational(1), {}, &b));
    st.execute(edit::trimClip(p, "s", b.videoClipId, ClipEdge::Tail, Rational(2)));
    const auto out = std::filesystem::temp_directory_path() / "fa_export_av.mp4";
    RenderOptions opt;
    opt.encoder.preset = "veryfast";
    const RenderReport r = renderProject(p, "", out, opt);
    EXPECT_EQ(r.framesWritten, 60);
    EXPECT_EQ(r.audioSamples, 96000);
    const MediaInfo mi = probeMedia(out);
    ASSERT_TRUE(mi.audio);
    EXPECT_EQ(mi.audio->codec, "aac");
    EXPECT_EQ(mi.audio->sampleRate, 48000);
    EXPECT_EQ(mi.audio->channels, 2);
    {
        AudioDecoder dec(out);
        std::vector<float> pcm(96000 * 2);
        dec.read(0, 96000, pcm.data());
        // tone switch at exactly 1.000 s: compare 1 kHz vs 1.5 kHz energy in 2 ms windows around the cut
        auto share = [&](int64_t from) {
            const double x = toneAmplitude(pcm, 0, from, 96, 1000), y = toneAmplitude(pcm, 0, from, 96, 1500);
            return y / (x + y + 1e-9);
        };
        EXPECT_LT(share(48000 - 480), 0.1);    // 10 ms before the cut: A
        EXPECT_GT(share(48000 + 384), 0.9);    // 8 ms after the cut: B
        EXPECT_NEAR(toneAmplitude(pcm, 0, 24000, 4800, 1000), 0.125, 0.01);   // AAC keeps the level
    }
    std::filesystem::remove(out);
}

// An adjustment layer with an active effect exports (it used to throw "invalid vector subscript": the per-frame
// bookkeeping had no entry for adjustment clips, and the frame-map code ran even when no map was requested).
TEST(Export, AdjustmentLayersExportWithAndWithoutAFrameMap) {
    Project p;
    p.sources = {MediaSource{"A", media("A_red_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920, 1080,
                             true, true}};
    Sequence s;
    s.id = "s";
    p.sequences.push_back(s);
    UndoStack st(p);
    st.execute(edit::addMedia(p, "s", "A", Rational(0)));
    Track v2{"v2", TrackKind::Video, "Video 2"};
    Clip adj;
    adj.id = "adj";
    adj.adjustment = true;
    adj.duration = Rational(10);
    EffectRef bw;
    bw.id = "bw";
    bw.type = "fa.filter.bw";
    adj.effects.push_back(bw);
    v2.clips.push_back(adj);
    p.sequences[0].tracks.push_back(v2);
    const auto out = std::filesystem::temp_directory_path() / "fa_export_adjustment.mp4";
    const auto csv = std::filesystem::temp_directory_path() / "fa_export_adjustment.csv";
    for (const bool withMap : {false, true}) {
        RenderOptions opt;
        opt.encoder.preset = "ultrafast";
        opt.includeAudio = false;
        opt.lastFrame = 14;
        if (withMap) opt.frameMapCsv = csv;
        const RenderReport r = renderProject(p, "", out, opt);
        EXPECT_EQ(r.framesWritten, 15) << "map " << withMap;
        const Rgba8 px = VideoDecoder(out).frameAt(Rational(1, 6)).image.at(960, 540);
        EXPECT_LT(std::abs(int(px.r) - int(px.g)), 4) << "the red picture is grey";
        EXPECT_LT(std::abs(int(px.g) - int(px.b)), 4);
        EXPECT_LT(px.r, 120);
    }
    int adjustmentRows = 0;
    {
        std::ifstream map(csv);
        std::string line;
        while (std::getline(map, line)) adjustmentRows += line.find(",adj,") != std::string::npos;
    }
    EXPECT_EQ(adjustmentRows, 15);
    std::filesystem::remove(out);
    std::filesystem::remove(csv);
}

TEST(Export, CancelRemovesOutput) {
    Project p;
    p.sources = {MediaSource{"A", media("A_red_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920, 1080,
                             true, true}};
    Sequence s;
    s.id = "s";
    p.sequences.push_back(s);
    UndoStack st(p);
    st.execute(edit::addMedia(p, "s", "A", Rational(0)));
    const auto out = std::filesystem::temp_directory_path() / "fa_export_cancel.mp4";
    std::atomic<bool> cancel{false};
    RenderOptions opt;
    opt.encoder.preset = "ultrafast";
    opt.cancel = &cancel;
    opt.progress = [&](int64_t done, int64_t) {
        if (done == 10) cancel = true;
    };
    EXPECT_THROW(renderProject(p, "", out, opt), RenderCancelled);
    EXPECT_FALSE(std::filesystem::exists(out));
    auto partial = out;
    partial += ".partial.mp4";
    EXPECT_FALSE(std::filesystem::exists(partial));
}

// M5: retimed clips exported end to end (the real edit operations, renderProject, decoded MP4): A_red at 2x with
// Maintain Pitch, then reversed, then with a freeze frame. Video frames from the frame-index strip, audio from
// the 1 kHz tone (kept at 2x, silent during the hold), and the lengths agree.
TEST(Export, RetimedClipsExportInSync) {
    Project p;
    p.sources = {MediaSource{"A", media("A_red_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920, 1080,
                             true, true}};
    Sequence s;
    s.id = "s";
    p.sequences.push_back(s);
    UndoStack st(p);
    st.execute(edit::addMedia(p, "s", "A", Rational(0)));
    const std::string v = p.sequences[0].tracks[0].clips[0].id;
    const auto out = std::filesystem::temp_directory_path() / "fa_export_speed.mp4";
    auto exportNow = [&] {
        RenderOptions opt;
        opt.encoder.preset = "ultrafast";
        return renderProject(p, "", out, opt);
    };
    auto stripAt = [&](int64_t frame) {
        VideoDecoder dec(out);
        return test::readStrip(dec.frameAt(Rational(frame, 30) + Rational(1, 120)).image).index;
    };
    std::vector<float> pcm;
    auto readAudio = [&](int64_t n) {
        AudioDecoder dec(out);
        pcm.assign(static_cast<size_t>(n) * 2, 0.0f);
        dec.read(0, static_cast<int>(n), pcm.data());
    };

    ClipSpeed fast;
    fast.speed = Rational(2);
    st.execute(edit::setSpeed(p, "s", v, fast));
    RenderReport r = exportNow();
    EXPECT_EQ(r.framesWritten, 150);
    EXPECT_EQ(r.audioSamples, 5 * 48000);
    for (int64_t k : {int64_t(0), int64_t(31), int64_t(149)}) EXPECT_EQ(stripAt(k), 2 * k) << "2x, frame " << k;
    readAudio(5 * 48000);
    EXPECT_NEAR(toneAmplitude(pcm, 0, 48000, 4800, 1000), 0.125, 0.02);   // pitch kept, level kept
    EXPECT_LT(toneAmplitude(pcm, 0, 48000, 4800, 2000), 0.01);

    ClipSpeed rev;
    rev.reverse = true;
    st.execute(edit::setSpeed(p, "s", v, rev));
    r = exportNow();
    EXPECT_EQ(r.framesWritten, 300);
    for (int64_t k : {int64_t(0), int64_t(100), int64_t(299)}) EXPECT_EQ(stripAt(k), 299 - k) << "reverse, frame " << k;

    st.execute(edit::setSpeed(p, "s", v, ClipSpeed{}));
    st.execute(edit::freezeFrame(p, "s", v, Time(2), Time(1)));
    r = exportNow();
    EXPECT_EQ(r.framesWritten, 330);
    EXPECT_EQ(stripAt(59), 59);
    for (int64_t k : {int64_t(60), int64_t(75), int64_t(89), int64_t(90)}) EXPECT_EQ(stripAt(k), 60) << "freeze, frame " << k;
    EXPECT_EQ(stripAt(91), 61);
    readAudio(11 * 48000);
    EXPECT_LT(toneAmplitude(pcm, 0, 2 * 48000 + 4800, 4800, 1000), 0.005);   // the hold is silent
    EXPECT_NEAR(toneAmplitude(pcm, 0, 4 * 48000, 4800, 1000), 0.125, 0.02);  // the tone resumes after it
    std::filesystem::remove(out);
}
