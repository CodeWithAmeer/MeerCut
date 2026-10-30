// M6 checkpoint C in the mixer: auto ducking, cleanup DSP, loudness normalisation and the master limiter, measured
// sample by sample on the Stage 1 tone media and on WAV files written here (tone bursts, hum, noise).
#include "fa/media/media.h"
#include "fa/model/commands.h"
#include "fa/model/serialize.h"
#include "fa/render/audio_cleanup.h"
#include "fa/render/audio_mix.h"
#include "fa/render/renderer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numbers>
#include <random>
#include <string>
#include <vector>

using namespace fa;

namespace {

constexpr int kRate = 48000;

std::filesystem::path media(const char* name) { return std::filesystem::path(FA_TESTMEDIA_DIR) / name; }

// A 16-bit stereo WAV of `seconds` whose left / right samples come from f(i) (same on both channels).
std::filesystem::path writeWav(const std::string& name, double seconds, const std::function<double(int64_t)>& f) {
    const auto path = std::filesystem::temp_directory_path() / name;
    const int n = static_cast<int>(seconds * kRate);
    std::ofstream o(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); };
    o.write("RIFF", 4);
    u32(36 + n * 4);
    o.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(kRate);
    u32(kRate * 4);
    u16(4);
    u16(16);
    o.write("data", 4);
    u32(n * 4);
    for (int i = 0; i < n; ++i) {
        const auto v = static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::clamp(f(i), -1.0, 1.0) * 32767)));
        u16(v);
        u16(v);
    }
    return path;
}

double tone(int64_t i, double f, double a) { return a * std::sin(2 * std::numbers::pi * f * i / kRate); }

// 500 Hz "voice" bursts at 0.25 on [1.0, 2.0) and [3.0, 3.5) s, silence elsewhere (6 s).
std::filesystem::path burstsWav() {
    return writeWav("fa_m6_bursts.wav", 6, [](int64_t i) {
        const double t = double(i) / kRate;
        return (t >= 1.0 && t < 2.0) || (t >= 3.0 && t < 3.5) ? tone(i, 500, 0.25) : 0.0;
    });
}

MediaSource source(const std::string& id, const std::filesystem::path& file, bool video) {
    MediaSource s;
    s.id = id;
    s.path = file.string();
    s.duration = Rational(video ? 10 : 6);
    s.hasVideo = video;
    s.hasAudio = true;
    if (video) {
        s.frameRate = FrameRate::fps30();
        s.width = 1920;
        s.height = 1080;
    }
    return s;
}

Clip clip(const std::string& id, const std::string& src, Time start, Time in, Time dur) {
    Clip c;
    c.id = id;
    c.sourceId = src;
    c.timelineStart = start;
    c.sourceIn = in;
    c.duration = dur;
    return c;
}

// Music (B, 1.5 kHz at 0.125) on a1, a priority "voice" (bursts) on a2 with Auto Ducking on.
Project duckProject() {
    Project p;
    p.sources = {source("A", media("A_red_1080p30_10s.mp4"), true), source("B", media("B_blue_1080p30_10s.mp4"), true),
                 source("V", burstsWav(), false)};
    Sequence s;
    s.id = "s";
    s.sampleRate = kRate;
    s.tracks = {Track{"v1", TrackKind::Video, "Video 1"}, Track{"a1", TrackKind::Audio, "Music"},
                Track{"a2", TrackKind::Audio, "Voice"}};
    s.tracks[1].clips = {clip("m", "B", Time(0), Time(0), Time(6))};
    Clip v = clip("v", "V", Time(0), Time(0), Time(6));
    v.audio.ducking.enabled = true;
    v.audio.ducking.amountDb = -12;
    v.audio.ducking.thresholdDb = -40;
    v.audio.ducking.attackMs = 200;
    v.audio.ducking.holdMs = 300;
    v.audio.ducking.releaseMs = 500;
    s.tracks[2].clips = {v};
    p.sequences.push_back(s);
    return p;
}

std::vector<float> mixOf(const Project& p, int64_t from, int64_t n, int chunk = 0) {
    AudioMixer m("", kRate);
    std::vector<float> out(static_cast<size_t>(n) * 2);
    if (chunk <= 0) {
        m.mix(p, p.sequences[0], from, static_cast<int>(n), out.data());
    } else {
        for (int64_t pos = 0; pos < n; pos += chunk) {
            const int len = static_cast<int>(std::min<int64_t>(chunk, n - pos));
            m.mix(p, p.sequences[0], from + pos, len, out.data() + pos * 2);
        }
    }
    return out;
}

double toneAmplitude(const std::vector<float>& x, int ch, int64_t from, int n, double f) {
    double c = 0, s = 0;
    for (int i = 0; i < n; ++i) {
        const double v = x[static_cast<size_t>((from + i) * 2 + ch)];
        c += v * std::cos(2 * std::numbers::pi * f * i / kRate);
        s += v * std::sin(2 * std::numbers::pi * f * i / kRate);
    }
    return 2 * std::hypot(c, s) / n;
}

// The music track alone as the mix plays it: total minus the voice track alone (the voice is not ducked).
std::vector<float> musicPart(const Project& p, int64_t n) {
    Project voiceOnly = p;
    voiceOnly.sequences[0].tracks[1].clips.clear();
    const auto all = mixOf(p, 0, n), voice = mixOf(voiceOnly, 0, n);
    std::vector<float> out(all.size());
    for (size_t i = 0; i < all.size(); ++i) out[i] = all[i] - voice[i];
    return out;
}

}  // namespace

// ---- ducking -------------------------------------------------------------------------------------------------------

TEST(Ducking, MusicGoesDownAroundTheVoiceSampleBySample) {
    const Project p = duckProject();
    const int64_t n = 6 * kRate;
    const auto music = musicPart(p, n);
    Project plain = p;
    plain.sequences[0].tracks[2].clips[0].audio.ducking.enabled = false;
    const auto ref = musicPart(plain, n);
    const std::vector<std::pair<int64_t, int64_t>> runs{{kRate, 2 * kRate}, {3 * kRate, 3 * kRate + kRate / 2}};
    const int64_t A = kRate / 5, H = kRate * 3 / 10, R = kRate / 2;
    double worst = 0, worstLate = 0;
    for (int64_t s = 0; s < n; ++s) {
        const double g = dbToGain(-12.0 * duckDepth(runs, s, A, H, R));
        // negative control: an attack that starts at the voice instead of before it
        auto late = runs;
        for (auto& r : late) r.first += A;
        const double gl = dbToGain(-12.0 * duckDepth(late, s, A, H, R));
        for (int ch = 0; ch < 2; ++ch) {
            const size_t i = static_cast<size_t>(s * 2 + ch);
            worst = std::max(worst, std::fabs(double(music[i]) - double(ref[i]) * g));
            worstLate = std::max(worstLate, std::fabs(double(music[i]) - double(ref[i]) * gl));
        }
    }
    EXPECT_LT(worst, 2e-6);
    EXPECT_GT(worstLate, 0.02);
    // the levels themselves: full, half way down the attack (-6 dB), fully down (-12 dB), back up
    auto level = [&](double t) { return toneAmplitude(music, 0, static_cast<int64_t>(t * kRate) - 240, 480, 1500); };
    EXPECT_NEAR(level(0.5), 0.125, 0.003);
    EXPECT_NEAR(level(0.9), 0.125 * dbToGain(-6), 0.003);
    EXPECT_NEAR(level(1.5), 0.125 * dbToGain(-12), 0.002);
    EXPECT_NEAR(level(2.2), 0.125 * dbToGain(-12), 0.002);   // hold
    EXPECT_NEAR(level(2.55), 0.125 * dbToGain(-6), 0.003);   // release midpoint
    EXPECT_NEAR(level(5.0), 0.125, 0.003);
    // the voice itself is untouched
    Project voiceOnly = p;
    voiceOnly.sequences[0].tracks[1].clips.clear();
    EXPECT_NEAR(toneAmplitude(mixOf(voiceOnly, 0, n), 0, static_cast<int64_t>(1.5 * kRate), 4800, 500), 0.25, 0.003);
}

TEST(Ducking, TargetsMuteSoloAndAutomationRules) {
    const int64_t at = static_cast<int64_t>(1.5 * kRate);
    auto musicLevel = [&](const Project& p) {
        const auto m = musicPart(p, 2 * kRate);
        return toneAmplitude(m, 0, at - 240, 480, 1500);
    };
    const double down = 0.125 * dbToGain(-12);
    Project p = duckProject();
    EXPECT_NEAR(musicLevel(p), down, 0.002);
    // targets: only listed tracks are lowered
    p.sequences[0].tracks[2].clips[0].audio.ducking.targets = {"a3"};
    Track a3{"a3", TrackKind::Audio, "Other"};
    p.sequences[0].tracks.push_back(a3);
    EXPECT_NEAR(musicLevel(p), 0.125, 0.002);
    p.sequences[0].tracks[2].clips[0].audio.ducking.targets = {"a1"};
    EXPECT_NEAR(musicLevel(p), down, 0.002);
    // a muted priority track does not duck
    p.sequences[0].tracks[2].muted = true;
    {
        const auto all = mixOf(p, 0, 2 * kRate);
        EXPECT_NEAR(toneAmplitude(all, 0, at - 240, 480, 1500), 0.125, 0.002);
    }
    p.sequences[0].tracks[2].muted = false;
    // a priority clip never ducks its own track
    Project same = duckProject();
    same.sequences[0].tracks[1].clips[0].audio.ducking = same.sequences[0].tracks[2].clips[0].audio.ducking;
    same.sequences[0].tracks[2].clips[0].audio.ducking.enabled = false;
    {
        const auto all = mixOf(same, 0, 2 * kRate);
        EXPECT_NEAR(toneAmplitude(all, 0, at - 240, 480, 1500), 0.125, 0.002);
    }
    // the music's own automation is kept: -6 dB on the music and -12 dB of ducking = -18 dB
    p.sequences[0].tracks[1].clips[0].volumeDb.setConstant(-6.0);
    EXPECT_NEAR(musicLevel(p), 0.125 * dbToGain(-18), 0.002);
    // the voice's level decides: at -30 dB the bursts (RMS -15 dBFS -> -45 dBFS) fall under the -40 dBFS threshold
    p.sequences[0].tracks[2].clips[0].volumeDb.setConstant(-30.0);
    EXPECT_NEAR(musicLevel(p), 0.125 * dbToGain(-6), 0.002);
}

TEST(Ducking, ChunkingSeekingAndRetimedPriorityClips) {
    Project p = duckProject();
    const int64_t n = 6 * kRate;
    const auto once = mixOf(p, 0, n);
    for (int chunk : {1024, 4096, 999}) {
        const auto pieces = mixOf(p, 0, n, chunk);
        size_t diff = 0;
        for (size_t i = 0; i < once.size(); ++i) diff += once[i] != pieces[i];
        EXPECT_EQ(diff, 0u) << chunk;
    }
    for (int64_t at : {int64_t(40000), int64_t(150000)}) {
        const auto tail = mixOf(p, at, 30000);
        double worst = 0;
        for (size_t i = 0; i < tail.size(); ++i) worst = std::max(worst, double(std::fabs(tail[i] - once[static_cast<size_t>(at * 2) + i])));
        EXPECT_LT(worst, 1e-4) << at;
    }
    // the voice at 2x: its bursts play at [0.5, 1.0) and [1.5, 1.75) s, and the music is down there
    Clip& v = p.sequences[0].tracks[2].clips[0];
    v.speed.speed = Rational(2);
    v.speed.maintainPitch = false;
    v.duration = Time(3);
    const auto music = musicPart(p, 3 * kRate);
    EXPECT_NEAR(toneAmplitude(music, 0, static_cast<int64_t>(0.75 * kRate) - 240, 480, 1500), 0.125 * dbToGain(-12), 0.002);
    EXPECT_NEAR(toneAmplitude(music, 0, static_cast<int64_t>(0.2 * kRate) - 240, 480, 1500), 0.125, 0.003);
}

// ---- cleanup in the mix ----------------------------------------------------------------------------------------------

TEST(CleanupMix, HumRemovalAndNoiseReductionWithoutBlockSeams) {
    std::mt19937 rng(3);
    std::normal_distribution<double> g(0.0, 0.01);
    std::vector<double> noise(static_cast<size_t>(kRate) * 8);
    for (auto& v : noise) v = g(rng);
    // a 1 kHz "voice" in syllables (300 ms on, 200 ms off) over continuous 50 Hz hum and white noise
    const auto file = writeWav("fa_m6_hum_noise.wav", 8, [&](int64_t i) {
        const bool on = std::fmod(double(i) / kRate, 0.5) < 0.3;
        return (on ? tone(i, 1000, 0.2) : 0.0) + tone(i, 50, 0.1) + noise[static_cast<size_t>(i)];
    });
    Project p;
    p.sources = {source("H", file, false)};
    p.sources[0].duration = Rational(8);
    Sequence s;
    s.id = "s";
    s.sampleRate = kRate;
    s.tracks = {Track{"a1", TrackKind::Audio, "A"}};
    s.tracks[0].clips = {clip("c", "H", Time(0), Time(0), Time(8))};
    p.sequences.push_back(s);
    const auto dry = mixOf(p, 0, 8 * kRate);
    Clip& c = p.sequences[0].tracks[0].clips[0];
    c.audio.cleanup.humHz = 50;
    c.audio.cleanup.denoise = 0.8;
    const auto wet = mixOf(p, 0, 8 * kRate);
    const int64_t at = 4 * kRate;
    EXPECT_NEAR(toneAmplitude(dry, 0, at, kRate, 50), 0.1, 0.003);
    EXPECT_LT(toneAmplitude(wet, 0, at, kRate, 50), 0.002);                      // hum gone (> 30 dB down)
    const int64_t syllable = at + kRate / 20;   // 4.05 .. 4.25 s: inside a syllable
    EXPECT_NEAR(toneAmplitude(wet, 0, syllable, 9600, 1000) / toneAmplitude(dry, 0, syllable, 9600, 1000), 1.0, 0.06);
    // the noise floor drops (energy in 3..6 kHz)
    auto band = [&](const std::vector<float>& x) {
        double e = 0;
        for (double f = 3000; f < 6000; f += 250) e += std::pow(toneAmplitude(x, 0, at, 9600, f), 2);
        return e;
    };
    EXPECT_LT(10 * std::log10(band(wet) / band(dry)), -10.0);
    // no seams at the 16384-sample DSP blocks: the jump across each boundary is no bigger than the signal's own jumps
    // around it (a seam from mismatched filter states would stand out; 1.5x allows for chance)
    double worstRatio = 0;
    for (int64_t b = kDspBlock; b + 64 < 7 * kRate; b += kDspBlock) {
        if (b < kRate) continue;
        const double across = std::fabs(wet[static_cast<size_t>(b * 2)] - wet[static_cast<size_t>((b - 1) * 2)]);
        double typical = 0;
        for (int64_t k = b - 64; k < b + 64; ++k)
            if (k != b - 1) typical = std::max(typical, double(std::fabs(wet[static_cast<size_t>((k + 1) * 2)] - wet[static_cast<size_t>(k * 2)])));
        worstRatio = std::max(worstRatio, across / typical);
    }
    EXPECT_LT(worstRatio, 1.5);
    // chunk independence with the noise reduction on
    const auto pieces = mixOf(p, 0, 8 * kRate, 1024);
    size_t diff = 0;
    for (size_t i = 0; i < wet.size(); ++i) diff += wet[i] != pieces[i];
    EXPECT_EQ(diff, 0u);
}

// ---- loudness -----------------------------------------------------------------------------------------------------

TEST(Loudness, ClipMeasurementAndNormalisation) {
    Project p = duckProject();
    p.sequences[0].tracks[2].clips.clear();
    Clip& m = p.sequences[0].tracks[1].clips[0];
    m.sourceId = "A";   // 1 kHz at -18.06 dBFS on both channels
    m.duration = Time(5);
    const double lufs = measureClipLoudness(p, p.sequences[0], m, "");
    EXPECT_NEAR(lufs, -18.06, 0.2);
    // the clip's own volume does not change the measurement (normalisation comes on top of it)
    m.volumeDb.setConstant(-10);
    EXPECT_NEAR(measureClipLoudness(p, p.sequences[0], m, ""), lufs, 1e-9);
    m.volumeDb.setConstant(0);
    m.audio.cleanup.normalize = true;
    m.audio.cleanup.targetLufs = -23;
    const auto unmeasured = mixOf(p, 0, 2 * kRate);
    EXPECT_NEAR(toneAmplitude(unmeasured, 0, kRate, 4800, 1000), 0.125, 0.002);   // not measured: no gain
    m.audio.cleanup.measuredLufs = lufs;
    const auto normalised = mixOf(p, 0, 2 * kRate);
    EXPECT_NEAR(20 * std::log10(toneAmplitude(normalised, 0, kRate, 4800, 1000) / toneAmplitude(unmeasured, 0, kRate, 4800, 1000)),
                -23 - lufs, 0.01);
    LoudnessMeter after(kRate, 2);
    const auto longer = mixOf(p, 0, 5 * kRate);
    after.add(longer.data(), 5 * kRate);
    EXPECT_NEAR(after.integrated(), -23.0, 0.2);
    std::atomic<bool> cancel{true};
    EXPECT_TRUE(std::isnan(measureClipLoudness(p, p.sequences[0], m, "", &cancel)));
}

// ---- master limiter -------------------------------------------------------------------------------------------------

TEST(MasterLimiter, CeilingHoldsAndStreamingIsExact) {
    Project p = duckProject();
    p.sequences[0].tracks[2].clips[0].audio.ducking.enabled = false;
    p.sequences[0].tracks[2].clips[0].sourceId = "A";   // 1 kHz + 1.5 kHz, both pushed to about 0.9
    p.sequences[0].tracks[1].clips[0].volumeDb.setConstant(17.0);
    p.sequences[0].tracks[2].clips[0].volumeDb.setConstant(17.0);
    const int64_t n = 4 * kRate;
    float rawPeak = 0;
    for (float v : mixOf(p, 0, n)) rawPeak = std::max(rawPeak, std::fabs(v));
    EXPECT_GT(rawPeak, 1.2f);   // without the limiter the sum exceeds full scale
    p.sequences[0].limiter = true;
    p.sequences[0].limiterCeilingDb = -1.0;
    const auto once = mixOf(p, 0, n);
    float peak = 0;
    for (float v : once) peak = std::max(peak, std::fabs(v));
    EXPECT_LE(peak, float(dbToGain(-1.0)));
    EXPECT_GT(peak, float(dbToGain(-1.5)));
    for (int chunk : {1024, 4096, 333}) {   // streamed in pieces: the same samples
        const auto pieces = mixOf(p, 0, n, chunk);
        size_t diff = 0;
        for (size_t i = 0; i < once.size(); ++i) diff += once[i] != pieces[i];
        EXPECT_EQ(diff, 0u) << chunk;
    }
    const auto fresh = mixOf(p, 100000, 20000);   // a fresh start (seek) agrees within the decoder's seek accuracy
    double worst = 0;
    for (size_t i = 0; i < fresh.size(); ++i) worst = std::max(worst, double(std::fabs(fresh[i] - once[200000 + i])));
    EXPECT_LT(worst, 2e-4);
    // quiet material passes untouched
    p.sequences[0].tracks[1].clips[0].volumeDb.setConstant(0.0);
    p.sequences[0].tracks[2].clips[0].volumeDb.setConstant(0.0);
    Project off = p;
    off.sequences[0].limiter = false;
    EXPECT_EQ(mixOf(p, 0, kRate), mixOf(off, 0, kRate));
}

// ---- project format -------------------------------------------------------------------------------------------------

TEST(AudioCleanupModel, RoundTripAndValidation) {
    Project p = duckProject();
    Clip& v = p.sequences[0].tracks[2].clips[0];
    v.audio.ducking.targets = {"a1"};
    v.audio.cleanup.denoise = 0.4;
    v.audio.cleanup.humHz = 60;
    v.audio.cleanup.humHarmonics = 3;
    v.audio.cleanup.highPassHz = 80;
    v.audio.cleanup.lowPassHz = 12000;
    v.audio.cleanup.normalize = true;
    v.audio.cleanup.targetLufs = -14;
    v.audio.cleanup.measuredLufs = -20.5;
    p.sequences[0].limiter = true;
    p.sequences[0].limiterCeilingDb = -2;
    EXPECT_EQ(projectFromJsonString(projectToJsonString(p)), p);
    auto errors = [](const Project& q) {
        int n = 0;
        for (const auto& i : validate(q)) n += i.severity == ValidationIssue::Severity::Error;
        return n;
    };
    EXPECT_EQ(errors(p), 0);
    for (const auto& change : std::vector<std::function<void(Project&)>>{
             [](Project& q) { q.sequences[0].tracks[2].clips[0].audio.cleanup.denoise = 1.5; },
             [](Project& q) { q.sequences[0].tracks[2].clips[0].audio.cleanup.humHz = 55; },
             [](Project& q) { q.sequences[0].tracks[2].clips[0].audio.cleanup.highPassHz = 5; },
             [](Project& q) { q.sequences[0].tracks[2].clips[0].audio.ducking.amountDb = 6; },
             [](Project& q) { q.sequences[0].tracks[2].clips[0].audio.ducking.targets = {"v1"}; },
             [](Project& q) { q.sequences[0].limiterCeilingDb = 1; }}) {
        Project q = p;
        change(q);
        EXPECT_GT(errors(q), 0);
    }
    // a version 5 project has none of it, and M6 B projects without the C fields load with the defaults
    Project old = duckProject();
    old.sequences[0].tracks[2].clips[0].audio = {};
    const Project loaded = projectFromJsonString(projectToJsonString(old));
    EXPECT_EQ(loaded.sequences[0].tracks[2].clips[0].audio, ClipAudio{});
    EXPECT_FALSE(loaded.sequences[0].limiter);
}

// ---- export ---------------------------------------------------------------------------------------------------------

// The exported AAC carries the ducking (the music down around the voice) and the limiter's ceiling (the export's own
// level check reads the limited mix).
TEST(Ducking, ExportCarriesDuckingAndTheLimiter) {
    Project p = duckProject();
    p.sequences[0].width = 320;
    p.sequences[0].height = 180;
    p.sequences[0].limiter = true;
    p.sequences[0].limiterCeilingDb = -6.0;
    p.sequences[0].tracks[1].clips[0].volumeDb.setConstant(12.0);   // the music alone would peak at 0.5 (-6 dBFS)
    const auto out = std::filesystem::temp_directory_path() / "fa_m6_export_ducking.mp4";
    RenderOptions opt;
    opt.encoder.preset = "ultrafast";
    const RenderReport r = renderProject(p, "", out, opt);
    EXPECT_LE(r.audioPeak, float(dbToGain(-6.0)));
    EXPECT_EQ(r.audioClippedSamples, 0);
    std::vector<float> pcm(static_cast<size_t>(6 * kRate) * 2);
    {
        AudioDecoder dec(out);   // closed again before the file is removed
        dec.read(0, 6 * kRate, pcm.data());
    }
    const auto mix = mixOf(p, 0, 6 * kRate);
    for (double t : {0.5, 1.5, 2.55, 5.0}) {
        const int64_t at = static_cast<int64_t>(t * kRate) - 1200;
        EXPECT_NEAR(toneAmplitude(pcm, 0, at, 2400, 1500), toneAmplitude(mix, 0, at, 2400, 1500), 0.006) << t;
    }
    EXPECT_LT(toneAmplitude(pcm, 0, static_cast<int64_t>(1.5 * kRate), 2400, 1500),
              0.5 * toneAmplitude(pcm, 0, static_cast<int64_t>(0.5 * kRate), 2400, 1500));   // really down
    std::filesystem::remove(out);
}
