// M5 retimed audio: the AudioMixer plays retimed clips through the same speed map as the video.
// A synthetic "beep track" (a 30 ms beep every second, each at its own frequency 600, 700 ... 1500 Hz) shows timing
// (onsets), order (reverse) and pitch at once; the 1 kHz tone of A_red is the EXP-004 pitch reference.
#include "fa/model/time_map.h"
#include "fa/render/audio_mix.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <vector>

using namespace fa;

namespace {

constexpr int kRate = 48000;

std::filesystem::path media(const char* name) { return std::filesystem::path(FA_TESTMEDIA_DIR) / name; }

double beepFrequency(int k) { return 600.0 + 100.0 * k; }

// 10 s, 48 kHz, 16-bit stereo WAV: beep k (k = 0..9) at 0.5 + k s, 30 ms, amplitude 0.5.
std::filesystem::path beepWav() {
    const auto path = std::filesystem::temp_directory_path() / "fa_beeps_10s.wav";
    if (std::filesystem::exists(path)) return path;
    const int n = 10 * kRate;
    std::vector<int16_t> pcm(static_cast<size_t>(n) * 2, 0);
    for (int k = 0; k < 10; ++k) {
        const int s0 = static_cast<int>((0.5 + k) * kRate), len = kRate * 30 / 1000;
        for (int i = 0; i < len; ++i) {
            const double v = 0.5 * std::sin(2 * std::numbers::pi * beepFrequency(k) * i / kRate);
            pcm[(s0 + i) * 2] = pcm[(s0 + i) * 2 + 1] = static_cast<int16_t>(v * 32767);
        }
    }
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(36 + static_cast<uint32_t>(pcm.size() * 2));
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(kRate);
    u32(kRate * 4);
    u16(4);
    u16(16);
    f.write("data", 4);
    u32(static_cast<uint32_t>(pcm.size() * 2));
    f.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));
    return path;
}

struct Rendered {
    std::vector<float> mono;   // channel 0
    int64_t first = 0;         // timeline sample of mono[0]
};

// One audio clip (source 0..10 s) with `speed`, mixed sequentially in 1024-sample blocks like playback / export.
Rendered mixClip(const std::filesystem::path& file, ClipSpeed speed, bool hasVideo = false) {
    Project p;
    MediaSource src{"W", file.string(), Rational(10), FrameRate::fps30(), 0, 0, hasVideo, true};
    p.sources.push_back(src);
    Sequence s;
    s.id = "s";
    Track a{"a1", TrackKind::Audio, "Audio 1"};
    Clip c;
    c.id = "c";
    c.sourceId = "W";
    c.speed = speed;
    c.duration = isRetimed(speed) ? frameDurationForSource(speed, Time(0), Time(10), FrameRate::fps30(), Time(10)) : Time(10);
    a.clips.push_back(c);
    s.tracks.push_back(a);
    p.sequences.push_back(s);
    AudioMixer mixer("", kRate, 2);
    Rendered r;
    const auto [cs, ce] = clipSampleRange(c, kRate);
    r.first = cs;
    std::vector<float> buf(1024 * 2);
    for (int64_t at = cs; at < ce; at += 1024) {
        const int n = static_cast<int>(std::min<int64_t>(1024, ce - at));
        mixer.mix(p, p.sequences[0], at, n, buf.data());
        for (int i = 0; i < n; ++i) r.mono.push_back(buf[i * 2]);
    }
    return r;
}

struct Beep {
    double onset;      // seconds (timeline)
    double freq;       // Hz from zero crossings
    double duration;   // seconds: the zero-crossing count resolves 1 / (2 duration) Hz
};

std::vector<Beep> findBeeps(const Rendered& r) {
    std::vector<Beep> out;
    const size_t n = r.mono.size();
    size_t i = 0;
    while (i < n) {
        if (std::fabs(r.mono[i]) < 0.1f) {
            ++i;
            continue;
        }
        size_t j = i, lastLoud = i;   // the run ends after 5 ms of quiet
        while (j < n && j - lastLoud < kRate / 200) {
            if (std::fabs(r.mono[j]) >= 0.1f) lastLoud = j;
            ++j;
        }
        int crossings = 0;
        for (size_t k = i + 1; k <= lastLoud; ++k) crossings += (r.mono[k - 1] < 0) != (r.mono[k] < 0);
        const double dur = static_cast<double>(lastLoud - i + 1) / kRate;
        if (dur > 0.004) out.push_back({static_cast<double>(r.first + static_cast<int64_t>(i)) / kRate, crossings / (2.0 * dur), dur});
        i = j;
    }
    return out;
}

// Timeline time at which the clip plays source time s (inverse of the map, by bisection on the frame grid).
double predictedOnset(const ClipSpeed& speed, double s) {
    Clip c;
    c.speed = speed;
    c.duration = frameDurationForSource(speed, Time(0), Time(10), FrameRate::fps30(), Time(10));
    const SourceMapper m(c);
    double lo = 0, hi = c.duration.toDouble();
    const bool reverse = speed.reverse;
    for (int it = 0; it < 80; ++it) {
        const double mid = (lo + hi) / 2;
        const bool before = reverse ? m.source(mid) > s : m.source(mid) < s;
        (before ? lo : hi) = mid;
    }
    return lo;
}

double toneLevelDb(const std::vector<float>& x, size_t from, size_t to) {
    double e = 0;
    for (size_t i = from; i < to; ++i) e += x[i] * x[i];
    return 10 * std::log10(e / static_cast<double>(to - from) + 1e-20);
}

double dominantHz(const std::vector<float>& x, size_t from, size_t n) {   // zero crossings of a clean tone
    int crossings = 0;
    for (size_t k = from + 1; k < from + n; ++k) crossings += (x[k - 1] < 0) != (x[k] < 0);
    return crossings / (2.0 * n / kRate);
}

}  // namespace

// EXP-004: 2x with Maintain Pitch OFF -> 2 kHz (plain resampling, level unchanged); ON -> 1 kHz kept (Filmora: -1.8 dB).
TEST(RetimedAudio, PitchAt2xMatchesExp004) {
    const auto a = media("A_red_1080p30_10s.mp4");
    const Rendered plain = mixClip(a, {}, true);
    ClipSpeed off;
    off.speed = Rational(2);
    off.maintainPitch = false;
    ClipSpeed on = off;
    on.maintainPitch = true;
    const Rendered r2off = mixClip(a, off, true), r2on = mixClip(a, on, true);
    EXPECT_EQ(r2off.mono.size(), static_cast<size_t>(5 * kRate));   // 5.000 s, like the video
    const size_t w0 = kRate, w = kRate;   // 1 s window inside the clip
    const double base = toneLevelDb(plain.mono, w0, w0 + w), lOff = toneLevelDb(r2off.mono, w0, w0 + w),
                 lOn = toneLevelDb(r2on.mono, w0, w0 + w);
    const double fOff = dominantHz(r2off.mono, w0, w), fOn = dominantHz(r2on.mono, w0, w);
    std::printf("  2x: pitch off %.1f Hz %+.2f dB; pitch kept %.1f Hz %+.2f dB (Filmora: 2000 Hz +0.0 dB / 1000 Hz -1.8 dB)\n",
                fOff, lOff - base, fOn, lOn - base);
    EXPECT_NEAR(fOff, 2000, 10);
    EXPECT_NEAR(fOn, 1000, 10);
    EXPECT_NEAR(lOff - base, 0.0, 0.5);
    EXPECT_NEAR(lOn - base, 0.0, 2.5);   // recorded, not copied: Filmora loses 1.8 dB here
}

// Every beep starts where the speed map puts its source time (pitch off: exact resampling; pitch kept: time stretch).
TEST(RetimedAudio, BeepsStayOnTheSpeedMap) {
    const auto wav = beepWav();
    struct Case {
        const char* name;
        ClipSpeed sp;
        double tolerance;   // seconds
    };
    std::vector<Case> cases;
    ClipSpeed sp;
    sp.speed = Rational(2);
    sp.maintainPitch = false;
    cases.push_back({"2x pitch off", sp, 0.002});
    sp.maintainPitch = true;
    cases.push_back({"2x pitch kept", sp, 0.020});
    sp = {};
    sp.speed = Rational(1, 2);
    sp.maintainPitch = false;
    cases.push_back({"0.5x pitch off", sp, 0.002});
    sp.maintainPitch = true;
    cases.push_back({"0.5x pitch kept", sp, 0.020});
    sp = {};
    sp.reverse = true;
    cases.push_back({"reverse 1x", sp, 0.002});
    sp = {};
    sp.ramp = rampKeys(findRampPreset("bullet")->keys, Time(0), Time(10));
    sp.maintainPitch = false;
    cases.push_back({"Bullet ramp pitch off", sp, 0.003});
    sp.maintainPitch = true;
    cases.push_back({"Bullet ramp pitch kept", sp, 0.020});
    sp = {};
    sp.reverse = true;
    sp.speed = Rational(2);
    cases.push_back({"reverse 2x pitch kept", sp, 0.020});
    sp = {};
    sp.speed = Rational(4);
    cases.push_back({"4x pitch kept", sp, 0.020});
    for (const auto& cs : cases) {
        // A slowed-down 30 ms beep can come out of WSOLA as two or three bursts inside its stretched span (the stretch
        // continues the waveform into the silence after the beep, then re-enters it): bursts within that span count
        // as one beep, timed by the first. Beeps are 1 s of source apart, far more than any span.
        const double span = 0.030 / std::max(0.1, cs.sp.speed.toDouble()) + 0.080;
        std::vector<Beep> beeps;
        for (const Beep& b : findBeeps(mixClip(wav, cs.sp)))
            if (beeps.empty() || b.onset - beeps.back().onset > span) beeps.push_back(b);
        Clip probe;
        probe.speed = cs.sp;
        probe.duration = frameDurationForSource(cs.sp, Time(0), Time(10), FrameRate::fps30(), Time(10));
        const SourceMapper map(probe);
        // match every heard beep to the beep the map predicts nearest to it
        double worst = 0;
        std::vector<int> heard(10, 0);
        for (const Beep& b : beeps) {
            int bk = -1;
            double err = 1e9;
            for (int k = 0; k < 10; ++k) {
                const double s = cs.sp.reverse ? 0.5 + k + 0.030 : 0.5 + k;   // reverse: a beep starts with its end
                const double e = b.onset - predictedOnset(cs.sp, s);
                if (std::fabs(e) < std::fabs(err)) {
                    err = e;
                    bk = k;
                }
            }
            ++heard[bk];
            worst = std::max(worst, std::fabs(err));
            // the speed over the whole beep (ramps change it within 30 ms); the count resolves 1.5 / (2 duration) Hz
            const double s0 = cs.sp.reverse ? 0.5 + bk + 0.030 : 0.5 + bk, s1 = cs.sp.reverse ? 0.5 + bk : 0.5 + bk + 0.030;
            const double v = 0.030 / std::max(1e-6, std::fabs(predictedOnset(cs.sp, s1) - predictedOnset(cs.sp, s0)));
            const double resolution = 1.5 / (2.0 * b.duration);
            if (!cs.sp.maintainPitch)
                EXPECT_NEAR(b.freq, beepFrequency(bk) * v, beepFrequency(bk) * v * 0.06 + resolution) << cs.name << " beep " << bk;
            else EXPECT_NEAR(b.freq, beepFrequency(bk), beepFrequency(bk) * 0.06 + resolution) << cs.name << " beep " << bk;
        }
        // every beep is heard exactly once - except where Maintain Pitch runs above 2x: WSOLA then hears only 2/speed
        // of the source and a 30 ms beep can fall between its frames
        int missing = 0;
        for (int k = 0; k < 10; ++k) {
            const double t = predictedOnset(cs.sp, cs.sp.reverse ? 0.5 + k + 0.030 : 0.5 + k);
            const bool mayDrop = cs.sp.maintainPitch && std::fabs(map.speed(t)) > 2.0 + 1e-9;
            EXPECT_LE(heard[k], 1) << cs.name << " beep " << k;
            if (!mayDrop) EXPECT_EQ(heard[k], 1) << cs.name << " beep " << k << " (local speed " << map.speed(t) << ")";
            missing += heard[k] == 0;
        }
        std::printf("  %-22s worst onset error %.1f ms, %d of 10 beeps heard\n", cs.name, worst * 1000, 10 - missing);
        EXPECT_GT(beeps.size(), 0u) << cs.name;
        EXPECT_LT(worst, cs.tolerance) << cs.name;
    }
}

// Negative control: the onset check notices a 50 ms shift.
TEST(RetimedAudio, NegativeControlDetectsAnOffset) {
    ClipSpeed sp;
    sp.speed = Rational(2);
    sp.maintainPitch = false;
    const auto beeps = findBeeps(mixClip(beepWav(), sp));
    ASSERT_EQ(beeps.size(), 10u);
    EXPECT_GT(std::fabs(beeps[3].onset - (predictedOnset(sp, 3.5) + 0.050)), 0.040);
}

TEST(RetimedAudio, FreezeFramesAreSilent) {
    ClipSpeed sp;
    sp.freeze = true;
    Project p;
    p.sources = {MediaSource{"W", beepWav().string(), Rational(10), FrameRate::fps30(), 0, 0, false, true}};
    Sequence s;
    s.id = "s";
    Track a{"a1", TrackKind::Audio, "Audio 1"};
    Clip c;
    c.id = "c";
    c.sourceId = "W";
    c.sourceIn = Rational(1, 2);   // a beep starts here
    c.speed = sp;
    c.duration = Time(2);
    a.clips.push_back(c);
    s.tracks.push_back(a);
    p.sequences.push_back(s);
    AudioMixer mixer("", kRate, 2);
    std::vector<float> buf(kRate * 2);
    mixer.mix(p, p.sequences[0], 0, kRate, buf.data());
    float peak = 0;
    for (float v : buf) peak = std::max(peak, std::fabs(v));
    EXPECT_EQ(peak, 0.0f);
}

