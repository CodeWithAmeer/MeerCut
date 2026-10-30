// M6 audio engine: volume automation, fades, crossfades, declick, balance, solo, equaliser, retiming and determinism,
// measured sample by sample on the Stage 1 tone media (A = 1 kHz, B = 1.5 kHz, both mono AAC at amplitude 0.125;
// H = 1 kHz PCM WAV at 0.0314) and on a stereo WAV written here (1 kHz left, 1.5 kHz right).
// Most checks compare a mix with a reference mix of the same clip without the setting under test: out = ref * gain.
#include "fa/media/media.h"
#include "fa/model/commands.h"
#include "fa/model/edit_ops.h"
#include "fa/model/keyframes.h"
#include "fa/model/serialize.h"
#include "fa/model/time_map.h"
#include "fa/render/audio_dsp.h"
#include "fa/render/audio_mix.h"
#include "fa/render/renderer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace fa;

namespace {

constexpr int kRate = 48000;

std::filesystem::path media(const char* name) { return std::filesystem::path(FA_TESTMEDIA_DIR) / name; }

MediaSource source(const std::string& id, const std::filesystem::path& file, bool video = true) {
    MediaSource s;
    s.id = id;
    s.path = file.string();
    s.duration = Rational(10);
    s.hasVideo = video;
    s.hasAudio = true;
    if (video) {
        s.frameRate = FrameRate::fps30();
        s.width = 1920;
        s.height = 1080;
    }
    return s;
}

// A sequence with a video track and two audio tracks, and the tone sources.
Project base() {
    Project p;
    p.sources = {source("A", media("A_red_1080p30_10s.mp4")), source("B", media("B_blue_1080p30_10s.mp4")),
                 source("H", media("H_tone1k_-12dB_10s.wav"), false)};
    Sequence s;
    s.id = "s";
    s.frameRate = FrameRate::fps30();
    s.sampleRate = kRate;
    Track v1{"v1", TrackKind::Video, "Video 1"};
    Track a1{"a1", TrackKind::Audio, "Audio 1"};
    Track a2{"a2", TrackKind::Audio, "Audio 2"};
    s.tracks = {v1, a1, a2};
    p.sequences.push_back(s);
    return p;
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

Keyframe<double> key(Time t, double v, Interpolation interp = Interpolation::Linear) {
    Keyframe<double> k;
    k.time = t;
    k.value = v;
    k.interp = interp;
    return k;
}

Track& track(Project& p, int audioIndex) { return p.sequences[0].tracks[static_cast<size_t>(audioIndex) + 1]; }

// Mix [from, from + n) in one call (chunk 0) or in pieces of `chunk` samples.
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
        const double ph = 2 * std::numbers::pi * f * i / kRate;
        c += v * std::cos(ph);
        s += v * std::sin(ph);
    }
    return 2 * std::hypot(c, s) / n;
}

// Largest |out - ref * gain(sample)| over [from, to) on both channels.
template <class Gain>
double worstGainError(const std::vector<float>& out, const std::vector<float>& ref, int64_t from, int64_t to, Gain gain) {
    double worst = 0;
    for (int64_t s = from; s < to; ++s) {
        const double g = gain(s);
        for (int ch = 0; ch < 2; ++ch) {
            const size_t i = static_cast<size_t>(s * 2 + ch);
            worst = std::max(worst, std::fabs(double(out[i]) - double(ref[i]) * g));
        }
    }
    return worst;
}

// 16-bit stereo PCM WAV: 1 kHz on the left, 1.5 kHz on the right, both at amplitude 0.25.
std::filesystem::path stereoWav() {
    const auto path = std::filesystem::temp_directory_path() / "fa_m6_stereo_lr.wav";
    const int n = kRate * 4;
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(36 + n * 4);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(kRate);
    u32(kRate * 4);
    u16(4);
    u16(16);
    f.write("data", 4);
    u32(n * 4);
    for (int i = 0; i < n; ++i) {
        u16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(0.25 * 32767 * std::sin(2 * std::numbers::pi * 1000 * i / kRate)))));
        u16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(0.25 * 32767 * std::sin(2 * std::numbers::pi * 1500 * i / kRate)))));
    }
    return path;
}

}  // namespace

// ---- volume automation ----------------------------------------------------------------------------------------

TEST(AudioMixM6, VolumeAutomationIsPerSampleAndInSourceTime) {
    Project p = base();
    Clip c = clip("c", "A", Time(1), Time(2), Time(6));   // timeline 1..7 s plays source 2..8 s
    Project ref = p;
    track(ref, 0).clips.push_back(c);
    c.volumeDb.setKey(key(Time(3), 0.0));     // source 3 s = timeline 2 s
    c.volumeDb.setKey(key(Time(5), -20.0));   // source 5 s = timeline 4 s
    track(p, 0).clips.push_back(c);
    const auto out = mixOf(p, 0, 8 * kRate), r = mixOf(ref, 0, 8 * kRate);
    auto dbAt = [](double src) { return src <= 3 ? 0.0 : src >= 5 ? -20.0 : -20.0 * (src - 3) / 2; };
    const int64_t from = kRate + 200, to = 7 * kRate - 200;   // inside the declick ramps
    EXPECT_LT(worstGainError(out, r, from, to, [&](int64_t s) { return std::pow(10.0, dbAt(s / double(kRate) + 1) / 20); }),
              2e-6);
    // negative control: the keys read as timeline times would be 1 s early
    EXPECT_GT(worstGainError(out, r, from, to, [&](int64_t s) { return std::pow(10.0, dbAt(s / double(kRate)) / 20); }),
              0.01);
    // the midpoint (timeline 3 s = source 4 s) is -10 dB
    EXPECT_NEAR(toneAmplitude(out, 0, 3 * kRate - 240, 480, 1000), 0.125 * std::pow(10.0, -0.5), 0.002);
}

TEST(AudioMixM6, HoldKeySwitchesAtTheExactSample) {
    Project p = base();
    Clip c = clip("c", "A", Time(0), Time(0), Time(6));
    Project ref = p;
    track(ref, 0).clips.push_back(c);
    c.volumeDb.setKey(key(Time(2), 0.0, Interpolation::Hold));
    c.volumeDb.setKey(key(Time(3), -40.0));
    track(p, 0).clips.push_back(c);
    const auto out = mixOf(p, 0, 4 * kRate), r = mixOf(ref, 0, 4 * kRate);
    EXPECT_LT(worstGainError(out, r, 3 * kRate - 2000, 3 * kRate, [](int64_t) { return 1.0; }), 1e-7);
    EXPECT_LT(worstGainError(out, r, 3 * kRate, 3 * kRate + 2000, [](int64_t) { return 0.01; }), 1e-7);
    double before = 0;   // the last sample before the switch is at full level (a non-zero sine sample)
    for (int64_t s = 3 * kRate - 4; s < 3 * kRate; ++s) before = std::max(before, double(std::fabs(r[s * 2])));
    EXPECT_GT(before, 0.01);
}

// Keys stay on their source instants through the speed map: 2x (resampled) and reverse.
TEST(AudioMixM6, AutomationOfRetimedClipsFollowsTheSpeedMap) {
    for (const bool reverse : {false, true}) {
        Project p = base();
        Clip c = clip("c", "A", Time(0), Time(0), reverse ? Time(10) : Time(5));
        if (reverse) c.speed.reverse = true;
        else c.speed.speed = Rational(2);
        c.speed.maintainPitch = false;
        Project ref = p;
        track(ref, 0).clips.push_back(c);
        c.volumeDb.setKey(key(Time(0), 0.0, Interpolation::Hold));
        c.volumeDb.setKey(key(Time(6), -40.0));
        track(p, 0).clips.push_back(c);
        const auto out = mixOf(p, 0, 5 * kRate), r = mixOf(ref, 0, 5 * kRate);
        // 2x: source 6 s plays at timeline 3 s (quiet after); reverse: source 6 s at timeline 4 s (quiet before)
        const int64_t edge = (reverse ? 4 : 3) * kRate;
        const double early = reverse ? 0.01 : 1.0, late = reverse ? 1.0 : 0.01;
        EXPECT_LT(worstGainError(out, r, edge - 2000, edge - 1, [&](int64_t) { return early; }), 1e-6) << reverse;
        EXPECT_LT(worstGainError(out, r, edge + 1, edge + 2000, [&](int64_t) { return late; }), 1e-6) << reverse;
        EXPECT_EQ(keys::toTimeline(track(p, 0).clips[0], Time(6)), Time(reverse ? 4 : 3)) << reverse;
        EXPECT_EQ(keys::toSource(track(p, 0).clips[0], Time(reverse ? 4 : 3)), Time(6)) << reverse;
    }
}

TEST(AudioMixM6, KeyframeMappingThroughRampsRoundTrips) {
    Clip c = clip("c", "A", Time(2), Time(1), Time(4));
    c.speed.ramp = rampKeys({{0.0, 1.0}, {0.5, 3.0}, {1.0, 0.5}}, Time(1), Time(6));
    c.duration = durationForSource(c.speed, c.sourceIn, Time(6));
    for (double s : {1.0, 2.5, 4.0, 6.9}) {
        const Time tl = keys::toTimeline(c, Rational::fromDouble(s, 1000));
        EXPECT_GE(tl, c.timelineStart);
        EXPECT_NEAR(keys::toSource(c, tl).toDouble(), s, 2e-6) << s;
    }
    // plain clips stay exact rationals
    const Clip plain = clip("p", "A", Time(2), Time(1), Time(4));
    EXPECT_EQ(keys::toTimeline(plain, Rational(7, 3)), Rational(7, 3) + Time(1));
}

// ---- fades -------------------------------------------------------------------------------------------------

TEST(AudioMixM6, FadesAreLinearInAmplitudeFromTheClipEdges) {
    Project p = base();
    Clip c = clip("c", "H", Rational(1, 2), Time(0), Time(4));   // timeline 0.5 .. 4.5 s
    Project ref = p;
    track(ref, 0).clips.push_back(c);
    c.audio.fadeIn = Time(1);
    c.audio.fadeOut = Time(2);
    track(p, 0).clips.push_back(c);
    const auto out = mixOf(p, 0, 5 * kRate), r = mixOf(ref, 0, 5 * kRate);
    const int64_t cs = kRate / 2, ce = cs + 4 * kRate;
    auto gain = [&](int64_t s) {
        const double local = (s - cs) / double(kRate);
        return local < 1 ? local : local > 2 ? (4 - local) / 2 : 1.0;
    };
    EXPECT_LT(worstGainError(out, r, cs + 96, ce - 96, gain), 2e-6);   // the reference has declick ramps at its edges
    EXPECT_EQ(out[cs * 2], 0.0f);                                       // the fade starts from silence
    // EXP-020: linear in amplitude, so the fade-in's midpoint is -6.02 dB (an equal-power fade would be -3.01 dB)
    const double mid = toneAmplitude(out, 0, cs + kRate / 2 - 240, 480, 1000) / toneAmplitude(r, 0, cs + kRate / 2 - 240, 480, 1000);
    EXPECT_NEAR(mid, 0.5, 0.01);
    EXPECT_GT(std::fabs(mid - std::sqrt(0.5)), 0.15);
}

TEST(AudioMixM6, FadesFollowTrimsAndSplits) {
    Project p = base();
    Clip c = clip("c", "H", Time(0), Time(0), Time(6));
    c.audio.fadeIn = Time(2);
    c.audio.fadeOut = Time(3);
    track(p, 0).clips.push_back(c);
    UndoStack st(p);
    st.execute(edit::trimClip(p, "s", "c", ClipEdge::Tail, Time(4)));   // 4 s: the fades no longer fit
    const Clip& t = track(p, 0).clips[0];
    EXPECT_EQ(t.audio.fadeIn, Time(2));
    EXPECT_EQ(t.audio.fadeOut, Time(2));
    for (const auto& issue : validate(p)) EXPECT_NE(issue.severity, ValidationIssue::Severity::Error) << issue.message;
    st.undo();
    EXPECT_EQ(track(p, 0).clips[0].audio.fadeOut, Time(3));
    st.execute(edit::splitAt(p, "s", Time(3), {"c"}));   // the inner edges get no fade
    ASSERT_EQ(track(p, 0).clips.size(), 2u);
    EXPECT_EQ(track(p, 0).clips[0].audio.fadeIn, Time(2));
    EXPECT_EQ(track(p, 0).clips[0].audio.fadeOut, Time(0));
    EXPECT_EQ(track(p, 0).clips[1].audio.fadeIn, Time(0));
    EXPECT_EQ(track(p, 0).clips[1].audio.fadeOut, Time(3));
    // fades that do not fit are refused
    EXPECT_THROW(st.execute(makeModifyClip("s", track(p, 0).clips[0].id, "Fade", [](Clip& x) { x.audio.fadeOut = Time(2); })),
                 EditError);
}

// ---- crossfades ----------------------------------------------------------------------------------------------

TEST(AudioMixM6, CrossfadeShapesMeasuredOnARealMix) {
    for (CrossfadeShape shape : {CrossfadeShape::ConstantGain, CrossfadeShape::EqualPower, CrossfadeShape::Exponential,
                                 CrossfadeShape::Logarithmic}) {
        Project p = base();
        track(p, 0).clips = {clip("a", "A", Time(0), Time(0), Time(3)), clip("b", "B", Time(3), Time(2), Time(3))};
        TransitionRef x;
        x.type = std::string(kAudioCrossfadePrefix) + crossfadeShapeId(shape);
        x.duration = Time(1);   // centred: 2.5 .. 3.5 s
        track(p, 0).clips[0].outTransition = x;
        const auto out = mixOf(p, 0, 6 * kRate);
        for (double pos : {0.25, 0.5, 0.75}) {
            double a = 0, b = 0;
            crossfadeGains(shape, pos, a, b);
            const int64_t at = static_cast<int64_t>((2.5 + pos) * kRate) - 240;
            EXPECT_NEAR(toneAmplitude(out, 0, at, 480, 1000), 0.125 * a, 0.004) << crossfadeShapeLabel(shape) << " " << pos;
            EXPECT_NEAR(toneAmplitude(out, 0, at, 480, 1500), 0.125 * b, 0.004) << crossfadeShapeLabel(shape) << " " << pos;
        }
        // no gap and no overlap outside the span
        EXPECT_NEAR(toneAmplitude(out, 0, static_cast<int64_t>(2.4 * kRate), 480, 1000), 0.125, 0.003);
        EXPECT_LT(toneAmplitude(out, 0, static_cast<int64_t>(2.4 * kRate), 480, 1500), 0.002);
        EXPECT_NEAR(toneAmplitude(out, 0, static_cast<int64_t>(3.55 * kRate), 480, 1500), 0.125, 0.003);
        EXPECT_LT(toneAmplitude(out, 0, static_cast<int64_t>(3.55 * kRate), 480, 1000), 0.002);
    }
    // Equal Power keeps the power of uncorrelated sources: -3.01 dB each at the midpoint (Constant Gain: -6.02 dB)
    double a = 0, b = 0;
    crossfadeGains(CrossfadeShape::EqualPower, 0.5, a, b);
    EXPECT_NEAR(20 * std::log10(a), -3.0103, 1e-3);
}

TEST(AudioMixM6, CrossfadeWithoutAHandleIsSilentWhereTheMediaEnds) {
    Project p = base();
    track(p, 0).clips = {clip("a", "A", Time(0), Time(7), Time(3)), clip("b", "B", Time(3), Time(0), Time(3))};
    TransitionRef x;
    x.type = std::string(kAudioCrossfadePrefix) + "equalPower";
    x.duration = Time(1);
    track(p, 0).clips[0].outTransition = x;
    const auto out = mixOf(p, 0, 6 * kRate);
    // b has no head handle (source 0): before the cut it contributes nothing; a (source 7..10 s) runs out at 3 s
    EXPECT_LT(toneAmplitude(out, 0, static_cast<int64_t>(2.75 * kRate) - 240, 480, 1500), 0.002);
    EXPECT_NEAR(toneAmplitude(out, 0, static_cast<int64_t>(2.75 * kRate) - 240, 480, 1000), 0.125 * std::cos(std::numbers::pi / 8), 0.004);
    EXPECT_LT(toneAmplitude(out, 0, static_cast<int64_t>(3.25 * kRate) - 240, 480, 1000), 0.002);
}

TEST(AudioMixM6, AudioAndPictureTransitionsStayOnTheirTracks) {
    Project p = base();
    UndoStack st(p);
    st.execute(edit::addMedia(p, "s", "A", Time(0)));
    st.execute(edit::addMedia(p, "s", "B", Time(10)));
    const Sequence& s = p.sequences[0];
    std::string videoClip, audioClip;
    for (const auto& t : s.tracks)
        if (!t.clips.empty()) (t.kind == TrackKind::Video ? videoClip : audioClip) = t.clips[0].id;
    ASSERT_FALSE(videoClip.empty());
    ASSERT_FALSE(audioClip.empty());
    TransitionRef dissolve{"fa.transition.dissolve", Time(1)}, xfade{std::string(kAudioCrossfadePrefix) + "equalPower", Time(1)};
    EXPECT_THROW(st.execute(edit::setTransition(p, "s", audioClip, dissolve)), EditError);
    EXPECT_THROW(st.execute(edit::setTransition(p, "s", videoClip, xfade)), EditError);
    st.execute(edit::setTransition(p, "s", audioClip, xfade));
    TransitionRef bad = xfade;
    bad.type = std::string(kAudioCrossfadePrefix) + "nonsense";
    EXPECT_FALSE(isAudioCrossfade("fa.audio.crossfade."));
    CrossfadeShape sh;
    EXPECT_FALSE(crossfadeShapeFromId("nonsense", sh));
}

// ---- edges: no clicks, no gaps --------------------------------------------------------------------------------

TEST(AudioMixM6, SplitClipsPlaySeamlesslyWithTheirAutomation) {
    Project p = base();
    Clip c = clip("c", "A", Time(0), Time(1), Time(6));
    c.volumeDb.setKey(key(Time(2), -3.0));
    c.volumeDb.setKey(key(Time(5), -15.0));   // a ramp across the split point
    c.audio.balance.setKey(key(Time(2), -0.5));
    c.audio.balance.setKey(key(Time(6), 0.5));
    track(p, 0).clips.push_back(c);
    const auto whole = mixOf(p, 0, 7 * kRate);
    UndoStack st(p);
    st.execute(edit::splitAt(p, "s", Rational(37, 10), {"c"}));
    ASSERT_EQ(track(p, 0).clips.size(), 2u);
    const auto split = mixOf(p, 0, 7 * kRate);
    double worst = 0;
    for (size_t i = 0; i < whole.size(); ++i) worst = std::max(worst, double(std::fabs(whole[i] - split[i])));
    EXPECT_LT(worst, 1e-6);   // no declick, no gap, the same gains: the split is inaudible
}

TEST(AudioMixM6, HardCutsGetAShortDeclickRamp) {
    Project p = base();
    // B starts 8 samples into its sine (phase 90 degrees): without a ramp the cut would jump by 0.125 at once
    track(p, 0).clips = {clip("a", "A", Time(0), Time(0), Time(2)), clip("b", "B", Time(2), Rational(1, 6000), Time(2))};
    const auto out = mixOf(p, 0, 4 * kRate);
    double jump = 0;
    for (int64_t s = 2 * kRate - 200; s < 2 * kRate + 200; ++s)
        jump = std::max(jump, double(std::fabs(out[(s + 1) * 2] - out[s * 2])));
    const double sineStep = 2 * std::numbers::pi * 1500 * 0.125 / kRate;   // largest step of B's own waveform
    EXPECT_LT(jump, sineStep * 1.1);
    // negative control: B's first sample really is at the crest
    AudioDecoder dec(media("B_blue_1080p30_10s.mp4"), kRate, 2);
    std::vector<float> raw(2 * 2);
    dec.read(8, 2, raw.data());
    EXPECT_GT(std::fabs(raw[0]), 0.1f);
    // the ramp is only 2 ms: 3 ms after the cut B is at full level
    EXPECT_NEAR(toneAmplitude(out, 0, 2 * kRate + 144, 480, 1500), 0.125, 0.003);
}

// ---- balance and stereo ---------------------------------------------------------------------------------------

TEST(AudioMixM6, BalanceKeepsChannelsAndFollowsTheMeasuredLaw) {
    Project p = base();
    p.sources.push_back(source("LR", stereoWav(), false));
    Clip c = clip("c", "LR", Time(0), Time(0), Time(4));
    c.audio.balance.setConstant(0.5);   // right: the left channel drops by cos(pi/4) = -3.01 dB
    track(p, 0).clips.push_back(c);
    const auto out = mixOf(p, 0, 4 * kRate);
    const int64_t at = kRate;
    EXPECT_NEAR(toneAmplitude(out, 0, at, 4800, 1000), 0.25 * std::cos(std::numbers::pi / 4), 0.002);
    EXPECT_NEAR(toneAmplitude(out, 1, at, 4800, 1500), 0.25, 0.002);
    EXPECT_LT(toneAmplitude(out, 0, at, 4800, 1500), 0.001);   // stereo integrity: nothing crosses over
    EXPECT_LT(toneAmplitude(out, 1, at, 4800, 1000), 0.001);
    // animated: -1 (left only) at source 1 s to +1 at source 3 s
    track(p, 0).clips[0].audio.balance.setKey(key(Time(1), -1.0));
    track(p, 0).clips[0].audio.balance.setKey(key(Time(3), 1.0));
    const auto anim = mixOf(p, 0, 4 * kRate);
    EXPECT_LT(toneAmplitude(anim, 1, kRate / 2 - 2400, 4800, 1500), 0.001);       // hard left
    EXPECT_NEAR(toneAmplitude(anim, 0, 2 * kRate - 240, 480, 1000), 0.25, 0.003);  // centre: both at unity
    EXPECT_NEAR(toneAmplitude(anim, 1, 2 * kRate - 240, 480, 1500), 0.25, 0.003);
    // a mono source plays on both channels at unity (EXP-020; not Filmora's +3 dB mono-AAC quirk)
    Project m = base();
    track(m, 0).clips.push_back(clip("m", "A", Time(0), Time(0), Time(2)));
    const auto mono = mixOf(m, 0, 2 * kRate);
    EXPECT_NEAR(toneAmplitude(mono, 0, kRate, 4800, 1000), 0.125, 0.002);
    EXPECT_NEAR(toneAmplitude(mono, 1, kRate, 4800, 1000), 0.125, 0.002);
}

// ---- tracks ----------------------------------------------------------------------------------------------------

TEST(AudioMixM6, TrackGainMuteAndSolo) {
    Project p = base();
    track(p, 0).clips.push_back(clip("a", "A", Time(0), Time(0), Time(2)));
    track(p, 1).clips.push_back(clip("b", "B", Time(0), Time(0), Time(2)));
    auto levels = [&] {
        const auto out = mixOf(p, kRate / 2, 4800);
        return std::pair{toneAmplitude(out, 0, 0, 4800, 1000), toneAmplitude(out, 0, 0, 4800, 1500)};
    };
    auto [a, b] = levels();
    EXPECT_NEAR(a, 0.125, 0.002);
    EXPECT_NEAR(b, 0.125, 0.002);
    track(p, 0).volumeDb = -6.0206;
    std::tie(a, b) = levels();
    EXPECT_NEAR(a, 0.0625, 0.002);
    track(p, 1).solo = true;   // only the soloed track plays
    std::tie(a, b) = levels();
    EXPECT_LT(a, 1e-6);
    EXPECT_NEAR(b, 0.125, 0.002);
    track(p, 0).solo = true;   // both soloed
    std::tie(a, b) = levels();
    EXPECT_NEAR(a, 0.0625, 0.002);
    EXPECT_NEAR(b, 0.125, 0.002);
    track(p, 1).muted = true;   // mute wins over solo
    std::tie(a, b) = levels();
    EXPECT_NEAR(a, 0.0625, 0.002);
    EXPECT_LT(b, 1e-6);
}

// ---- equaliser -------------------------------------------------------------------------------------------------

TEST(AudioMixM6, EqualiserMatchesItsDesignAndHasNoBlockSeams) {
    Project p = base();
    Clip c = clip("c", "H", Time(0), Time(0), Time(5));
    c.audio.eqEnabled = true;
    c.audio.eqGainsDb[5] = 12.0;   // the 1 kHz band
    track(p, 0).clips.push_back(c);
    const auto out = mixOf(p, 0, 5 * kRate);
    const double expected = 0.0314 * std::pow(10.0, Biquad::peaking(kRate, 1000, 1.41, 12).magnitudeDb(kRate, 1000) / 20);
    EXPECT_NEAR(toneAmplitude(out, 0, 2 * kRate, 4800, 1000), expected, 0.002);
    EXPECT_NEAR(expected / 0.0314, std::pow(10.0, 12.0 / 20), 0.05);
    // continuity across the anchored DSP blocks: no step bigger than the boosted sine's own
    double jump = 0;   // (after the filter has settled on the tone that starts with the file)
    for (int64_t s = kRate / 10; s + 1 < 5 * kRate - 200; ++s) jump = std::max(jump, double(std::fabs(out[(s + 1) * 2] - out[s * 2])));
    EXPECT_LT(jump, 2 * std::numbers::pi * 1000 * expected / kRate * 1.05);
    // a band far from the tone leaves it alone; disabling the EQ restores the original
    track(p, 0).clips[0].audio.eqGainsDb = {};
    track(p, 0).clips[0].audio.eqGainsDb[0] = -12.0;
    EXPECT_NEAR(toneAmplitude(mixOf(p, 0, 3 * kRate), 0, 2 * kRate, 4800, 1000), 0.0314, 0.0006);
    for (const auto& pre : eqPresets()) {
        for (double g : pre.gainsDb) EXPECT_LE(std::fabs(g), kEqMaxDb) << pre.id;
    }
}

// ---- determinism: playback chunks, seeks and export give the same samples --------------------------------------

TEST(AudioMixM6, ChunkingAndSeekingDoNotChangeTheMix) {
    Project p = base();
    Clip a = clip("a", "A", Time(0), Time(0), Time(3));
    a.volumeDb.setKey(key(Time(1), 0.0));
    a.volumeDb.setKey(key(Time(2), -12.0, Interpolation::Bezier));
    a.audio.fadeIn = Rational(1, 3);
    a.audio.eqEnabled = true;
    a.audio.eqGainsDb[4] = 6.0;
    Clip b = clip("b", "B", Time(3), Time(2), Time(3));
    b.audio.balance.setKey(key(Time(2), -1.0));
    b.audio.balance.setKey(key(Time(5), 1.0));
    b.audio.eqEnabled = true;
    b.audio.eqGainsDb[2] = -6.0;
    TransitionRef x{std::string(kAudioCrossfadePrefix) + "exponential", Time(1)};
    a.outTransition = x;
    track(p, 0).clips = {a, b};
    Clip fast = clip("f", "H", Time(1), Time(0), Time(2));   // 2x, pitch follows (resampled)
    fast.speed.speed = Rational(2);
    fast.speed.maintainPitch = false;
    fast.volumeDb.setKey(key(Time(1), -6.0));
    fast.volumeDb.setKey(key(Time(3), 0.0));
    track(p, 1).clips = {fast};
    const int64_t n = 6 * kRate;
    const auto once = mixOf(p, 0, n);
    for (int chunk : {1024, 4096, 777}) {
        const auto pieces = mixOf(p, 0, n, chunk);
        size_t diff = 0;
        for (size_t i = 0; i < once.size(); ++i) diff += once[i] != pieces[i];
        EXPECT_EQ(diff, 0u) << "chunk " << chunk;   // bit-identical
    }
    // a fresh mixer that starts in the middle (a seek) produces the same samples there, within the decoder's own
    // seek accuracy (AudioDecoder.RandomAccessMatchesSequentialDecoding: 1e-4)
    for (int64_t at : {int64_t(70000), int64_t(150001), int64_t(3 * kRate - 5)}) {
        const auto tail = mixOf(p, at, 20000);
        double worst = 0;
        for (size_t i = 0; i < tail.size(); ++i) worst = std::max(worst, double(std::fabs(tail[i] - once[static_cast<size_t>(at * 2) + i])));
        EXPECT_LT(worst, 1e-4) << "seek to " << at;
    }
}

// Linked A/V clips move together; the audio keys and fades move with them.
TEST(AudioMixM6, MovingALinkedClipMovesItsAutomation) {
    Project p = base();
    UndoStack st(p);
    edit::AddedClips added;
    st.execute(edit::addMedia(p, "s", "A", Time(0), {}, &added));
    ASSERT_FALSE(added.audioClipId.empty());
    st.execute(makeModifyClip("s", added.audioClipId, "Audio", [](Clip& c) {
        c.volumeDb.setKey(key(Time(1), 0.0));
        c.volumeDb.setKey(key(Time(4), -18.0));
        c.audio.fadeOut = Time(2);
    }));
    const auto before = mixOf(p, 0, 10 * kRate);
    st.execute(edit::moveClip(p, "s", added.videoClipId, Time(1)));   // the video clip: its audio partner follows
    const auto after = mixOf(p, kRate, 10 * kRate);
    double worst = 0;
    for (size_t i = 0; i < before.size(); ++i) worst = std::max(worst, double(std::fabs(before[i] - after[i])));
    EXPECT_LT(worst, 1e-6);
}

// ---- project format ----------------------------------------------------------------------------------------------

TEST(AudioMixM6, AudioSettingsRoundTripAndVersion5ProjectsLoad) {
    Project p = base();
    Clip a = clip("a", "A", Time(0), Time(0), Time(3));
    a.volumeDb.setKey(key(Time(1), -2.5));
    a.volumeDb.setKey(key(Rational(7, 3), -9.0, Interpolation::Hold));
    a.audio.fadeIn = Rational(1, 2);
    a.audio.fadeOut = Rational(2, 3);
    a.audio.balance.setConstant(-0.25);
    a.audio.eqEnabled = true;
    a.audio.eqPreset = "voice-clarity";
    a.audio.eqGainsDb = findEqPreset("voice-clarity")->gainsDb;
    a.outTransition = TransitionRef{std::string(kAudioCrossfadePrefix) + "logarithmic", Time(1)};
    track(p, 0).clips = {a, clip("b", "B", Time(3), Time(1), Time(3))};
    track(p, 1).solo = true;
    const Project q = projectFromJsonString(projectToJsonString(p));
    EXPECT_EQ(q, p);

    // A version 5 file: a plain number for the clip volume, no audio object, no solo.
    Project old = base();
    Clip c = clip("c", "A", Time(0), Time(0), Time(2));
    c.volumeDb.setConstant(-6.5);
    track(old, 0).clips = {c};
    std::string json = projectToJsonString(old);
    EXPECT_NE(json.find("\"volumeDb\": -6.5"), std::string::npos);   // the constant keeps the v5 form
    const std::string current = "\"version\": " + std::to_string(Project::kFormatVersion);
    const size_t v = json.find(current);
    ASSERT_NE(v, std::string::npos);
    json.replace(v, current.size(), "\"version\": 5");   // the whole number (two digits since format 10)
    const Project loaded = projectFromJsonString(json);
    const Clip& lc = loaded.sequences[0].tracks[1].clips[0];
    EXPECT_FALSE(lc.volumeDb.isAnimated());
    EXPECT_EQ(lc.volumeDb.constantValue(), -6.5);
    EXPECT_TRUE(lc.audio.isDefault());
    EXPECT_FALSE(loaded.sequences[0].tracks[1].solo);
    // and it sounds as before: a plain dB multiply
    EXPECT_NEAR(toneAmplitude(mixOf(loaded, 0, kRate), 0, kRate / 2, 4800, 1000), 0.125 * std::pow(10.0, -6.5 / 20), 0.002);
}

TEST(AudioMixM6, InvalidAudioSettingsAreRejected) {
    auto errors = [](const Project& p) {
        int n = 0;
        for (const auto& i : validate(p)) n += i.severity == ValidationIssue::Severity::Error;
        return n;
    };
    Project p = base();
    track(p, 0).clips = {clip("a", "A", Time(0), Time(0), Time(2))};
    EXPECT_EQ(errors(p), 0);
    Clip& c = track(p, 0).clips[0];
    c.audio.fadeIn = Rational(3, 2);
    c.audio.fadeOut = Time(1);   // 2.5 s of fades in a 2 s clip
    EXPECT_GT(errors(p), 0);
    c.audio = {};
    c.audio.balance.setConstant(1.5);
    EXPECT_GT(errors(p), 0);
    c.audio = {};
    c.audio.eqGainsDb[3] = 13;
    EXPECT_GT(errors(p), 0);
    c.audio = {};
    c.volumeDb.setKey(key(Time(1), 30.0));
    EXPECT_GT(errors(p), 0);
}

// ---- export ---------------------------------------------------------------------------------------------------

// The exported AAC carries the automation, fades and crossfade (levels within the codec's accuracy), and clipping is
// counted.
TEST(AudioMixM6, ExportCarriesTheMixAndCountsClipping) {
    Project p = base();
    p.sequences[0].width = 320;
    p.sequences[0].height = 180;
    Clip a = clip("a", "A", Time(0), Time(0), Time(2));
    a.audio.fadeIn = Rational(1, 2);
    a.volumeDb.setKey(key(Time(1), 0.0));
    a.volumeDb.setKey(key(Rational(3, 2), -12.0));
    track(p, 0).clips = {a};
    const auto out = std::filesystem::temp_directory_path() / "fa_m6_export_audio.mp4";
    RenderOptions opt;
    opt.encoder.preset = "ultrafast";
    RenderReport r = renderProject(p, "", out, opt);
    EXPECT_EQ(r.audioClippedSamples, 0);
    EXPECT_NEAR(r.audioPeak, 0.125, 0.01);
    {
        AudioDecoder dec(out);
        std::vector<float> pcm(2 * kRate * 2);
        dec.read(0, 2 * kRate, pcm.data());
        const auto ref = mixOf(p, 0, 2 * kRate);
        for (double t : {0.25, 0.75, 1.25, 1.75}) {
            const int64_t at = static_cast<int64_t>(t * kRate) - 1200;
            EXPECT_NEAR(toneAmplitude(pcm, 0, at, 2400, 1000), toneAmplitude(ref, 0, at, 2400, 1000), 0.004) << t;
        }
    }
    track(p, 0).clips[0].volumeDb = Animatable<double>(14.0);   // +14 dB clip and +6 dB track: 0.125 -> 1.25
    track(p, 0).volumeDb = 6.0;
    r = renderProject(p, "", out, opt);
    EXPECT_GT(r.audioClippedSamples, 0);
    EXPECT_GT(r.audioPeak, 1.0f);
    EXPECT_TRUE(std::any_of(r.warnings.begin(), r.warnings.end(),
                            [](const std::string& w) { return w.find("audio clipped") != std::string::npos; }));
    std::filesystem::remove(out);
}
