// M5 clip retiming: the time map (fa/model/time_map.h) against Filmora's measured EXP-004 exports, exact rational
// timing on non-integer and mixed frame rates, split / trim invariance, validation and persistence.
#include "fa/model/commands.h"
#include "fa/model/edit_ops.h"
#include "fa/model/serialize.h"
#include "fa/model/time_map.h"
#include "fa/render/timeline_eval.h"

#include <gtest/gtest.h>

#include <vector>

using namespace fa;

namespace {

// Filmora 16, Speed Ramping > Bullet on the 10 s / 30 fps test clip: the source frame of each of the 107 exported
// frames (Stage 1 EXP-004, experiments/EXP-004_speed/ramp_bullet.srcseq.json).
const std::vector<int> kFilmoraBullet = {
    0,   5,   10,  16,  21,  26,  31,  36,  42,  47,  52,  57,  62,  68,  73,  78,  83,  88,  94,  99,  104, 109,
    114, 120, 125, 129, 133, 135, 137, 138, 138, 139, 139, 140, 140, 141, 141, 142, 142, 143, 143, 144, 144, 145,
    145, 146, 146, 147, 147, 148, 148, 149, 149, 150, 150, 151, 151, 152, 152, 153, 153, 154, 154, 155, 155, 156,
    156, 157, 157, 158, 158, 159, 159, 160, 160, 161, 161, 162, 162, 163, 165, 168, 172, 176, 182, 187, 192, 197,
    202, 208, 213, 218, 223, 228, 234, 239, 244, 249, 254, 260, 265, 270, 275, 280, 286, 291, 296};

const FrameRate k30 = FrameRate::fps30();

Clip clip10s(ClipSpeed sp = {}) {
    Clip c;
    c.id = "c";
    c.sourceId = "A";
    c.sourceIn = Time(0);
    c.speed = std::move(sp);
    c.duration = isRetimed(c.speed) && !c.speed.freeze
                     ? frameDurationForSource(c.speed, Time(0), Time(10), k30, Time(10))
                     : Time(10);
    return c;
}

// Source frame index shown by every output frame of a clip (the decoder shows the frame visible at the time).
std::vector<int64_t> frames(const Clip& c, const FrameRate& out = k30, const FrameRate& src = k30) {
    std::vector<int64_t> f;
    const Time d = out.frameDuration();
    for (Time t(0); t < c.duration; t += d) f.push_back((sampleFrame(c, t, d, src).time * src.fps()).floor());
    return f;
}

std::vector<int64_t> range(int64_t from, int64_t to, int64_t step) {
    std::vector<int64_t> v;
    for (int64_t i = from; step > 0 ? i <= to : i >= to; i += step) v.push_back(i);
    return v;
}

}  // namespace

TEST(TimeMap, SpeedOneIsTheM0Rule) {
    const Clip c = clip10s();
    EXPECT_FALSE(isRetimed(c.speed));
    EXPECT_EQ(c.sourceOut(), Time(10));
    EXPECT_EQ(frames(c), range(0, 299, 1));
    Clip t = c;
    t.timelineStart = Time(3);
    t.sourceIn = Time(1);
    EXPECT_EQ(t.sourceTimeAt(Time(5)), Time(3));
}

// EXP-004 uniform speed: 2x -> 5.000 s, frames 0, 2, 4 ... 298 (no blending); 0.5x -> 20.000 s, 0, 0, 1, 1 ...
TEST(TimeMap, UniformSpeedMatchesFilmoraExp004) {
    ClipSpeed fast;
    fast.speed = Rational(2);
    const Clip c2 = clip10s(fast);
    EXPECT_EQ(c2.duration, Time(5));
    EXPECT_EQ(frames(c2), range(0, 298, 2));
    EXPECT_EQ(c2.sourceOut(), Time(10));

    ClipSpeed slow;
    slow.speed = Rational(1, 2);
    const Clip c05 = clip10s(slow);
    EXPECT_EQ(c05.duration, Time(20));
    const auto f = frames(c05);
    ASSERT_EQ(f.size(), 600u);
    for (size_t k = 0; k < f.size(); ++k) ASSERT_EQ(f[k], static_cast<int64_t>(k / 2)) << "output frame " << k;
}

// EXP-004 reverse 1x: 10.000 s, frames 299, 298 ... 0. Reverse 2x shows the 2x frames backwards.
TEST(TimeMap, ReverseMatchesFilmoraExp004) {
    ClipSpeed rev;
    rev.reverse = true;
    const Clip r1 = clip10s(rev);
    EXPECT_EQ(r1.duration, Time(10));
    EXPECT_EQ(frames(r1), range(299, 0, -1));
    rev.speed = Rational(2);
    const Clip r2 = clip10s(rev);
    EXPECT_EQ(r2.duration, Time(5));
    EXPECT_EQ(frames(r2), range(298, 0, -2));
    EXPECT_LT(speedAtLocal(r2, Time(1)), 0.0);
}

// EXP-004 Bullet: keys at 0/40/46/54/60/100 % of the source with 5.2/5.2/0.5/0.5/5.2/5.2x; the closed-form map
// (smoothstep speed in output time, keys in source time, nearest frame) reproduces every exported frame.
TEST(TimeMap, BulletRampMatchesFilmoraExp004FrameForFrame) {
    const RampPreset* bullet = findRampPreset("bullet");
    ASSERT_TRUE(bullet);
    ClipSpeed sp;
    sp.ramp = rampKeys(bullet->keys, Time(0), Time(10));
    sp.rampPreset = "bullet";
    // continuous duration 3.5595 s (Filmora stores 3.5595 s); whole frames: 107, the length Filmora exported
    EXPECT_NEAR(durationForSource(sp, Time(0), Time(10)).toDouble(), 3.5595, 5e-5);
    const Clip c = clip10s(sp);
    EXPECT_EQ(c.duration, Time(107, 30));
    const auto f = frames(c);
    ASSERT_EQ(f.size(), kFilmoraBullet.size());
    int exact = 0;
    for (size_t k = 0; k < f.size(); ++k) exact += f[k] == kFilmoraBullet[k];
    std::printf("  Bullet: %d / %zu frames identical to Filmora's export\n", exact, f.size());
    EXPECT_EQ(exact, static_cast<int>(f.size()));
    EXPECT_TRUE(speedProblem(c).empty()) << speedProblem(c);
}

// Negative controls: the comparisons above detect a wrong mapping.
TEST(TimeMap, NegativeControlsDetectWrongMappings) {
    // speed eased in SOURCE time instead of output time gives a different length (Stage 1: 3.74-3.87 s)
    ClipSpeed sp;
    sp.ramp = rampKeys(findRampPreset("bullet")->keys, Time(0), Time(10));
    const Clip c = clip10s(sp);
    // floor instead of nearest frame: only about half of the frames match
    std::vector<int64_t> floored;
    const Time d = k30.frameDuration();
    for (Time t(0); t < c.duration; t += d) floored.push_back((sourceTimeAtLocal(c, t) * Rational(30)).floor());
    int exact = 0;
    for (size_t k = 0; k < floored.size() && k < kFilmoraBullet.size(); ++k) exact += floored[k] == kFilmoraBullet[k];
    EXPECT_LT(exact, 80);
    // a reverse map that samples the upper end of the interval duplicates frame 299 and misses frame 0
    ClipSpeed rev;
    rev.reverse = true;
    const Clip r = clip10s(rev);
    std::vector<int64_t> upper;
    for (Time t(0); t < r.duration; t += d) upper.push_back((sourceTimeAtLocal(r, t) * Rational(30)).floor());
    EXPECT_NE(upper, range(299, 0, -1));
}

TEST(TimeMap, FreezeHoldsOneFrame) {
    Clip c = clip10s();
    c.sourceIn = Time(4);
    c.speed.freeze = true;
    c.duration = Time(3);
    for (int64_t f : frames(c)) ASSERT_EQ(f, 120);
    EXPECT_EQ(sourceLength(c), Time(0));
    EXPECT_EQ(speedAtLocal(c, Time(1)), 0.0);
}

// 30000/1001 timeline, 30 fps source at 2x: exact rationals, no drift after 10 000 frames.
TEST(TimeMap, NonIntegerAndMixedFrameRatesStayExact) {
    const FrameRate ntsc = FrameRate::ntsc30();
    Clip c;
    c.sourceIn = Time(0);
    c.speed.speed = Rational(2);
    c.duration = Time(400);
    const Time d = ntsc.frameDuration();
    for (int64_t k : {int64_t(0), int64_t(1), int64_t(9999), int64_t(10000)}) {
        const Time s = sampleFrame(c, d * Rational(k), d, k30).time;
        EXPECT_EQ(s, Rational(2002 * k, 30000)) << "frame " << k;   // 2 k / (30000/1001), exactly
    }
    // 25 fps source on a 30 fps timeline at 1x: the frame visible at k/30 (5 output frames show 4 source frames)
    Clip m;
    m.duration = Time(1);
    const auto f = frames(m, k30, FrameRate::fps25());
    EXPECT_EQ(std::vector<int64_t>(f.begin(), f.begin() + 7), (std::vector<int64_t>{0, 0, 1, 2, 3, 4, 5}));
}

TEST(TimeMap, FrameBlendingWeights) {
    ClipSpeed sp;
    sp.speed = Rational(1, 2);
    sp.interpolation = FrameInterpolation::Blending;
    const Clip c = clip10s(sp);
    const Time d = k30.frameDuration();
    const FrameSample f0 = sampleFrame(c, Time(0), d, k30), f1 = sampleFrame(c, d, d, k30);
    EXPECT_EQ(f0.time, Time(0));
    EXPECT_FALSE(f0.next);   // on a source frame: nothing to blend
    EXPECT_EQ(f1.time, Time(0));
    ASSERT_TRUE(f1.next);
    EXPECT_EQ(*f1.next, Time(1, 30));
    EXPECT_DOUBLE_EQ(f1.weight, 0.5);
    ClipSpeed third;
    third.speed = Rational(1, 3);
    third.interpolation = FrameInterpolation::Blending;
    const FrameSample g = sampleFrame(clip10s(third), d * Rational(2), d, k30);
    EXPECT_NEAR(g.weight, 2.0 / 3.0, 1e-12);
}

// Splitting or trimming a retimed clip keeps every output frame where it was.
TEST(TimeMap, SplitAndTrimKeepTheMap) {
    for (int variant = 0; variant < 3; ++variant) {
        ClipSpeed sp;
        if (variant == 0) sp.speed = Rational(3, 2);
        if (variant == 1) {
            sp.speed = Rational(2);
            sp.reverse = true;
        }
        if (variant == 2) sp.ramp = rampKeys(findRampPreset("bullet")->keys, Time(0), Time(10));
        Project p;
        p.sources = {MediaSource{"A", "A.mp4", Rational(10), k30, 64, 36, true, false}};
        Sequence s;
        s.id = "s";
        Track v{"v1", TrackKind::Video, "Video 1"};
        Clip c = clip10s(sp);
        v.clips.push_back(c);
        s.tracks.push_back(v);
        p.sequences.push_back(s);
        ASSERT_TRUE(validate(p).empty()) << variant << ": " << validate(p)[0].message;
        std::vector<Time> before;
        for (int64_t k = 0; k < (c.duration * Rational(30)).floor(); ++k)
            before.push_back(evaluateVideoFrame(p.sequences[0], k, &p)[0].a.sourceTime);
        UndoStack st(p);
        st.execute(edit::splitAt(p, "s", Time(31, 30)));
        ASSERT_EQ(p.sequences[0].tracks[0].clips.size(), 2u);
        for (size_t k = 0; k < before.size(); ++k)
            ASSERT_EQ(evaluateVideoFrame(p.sequences[0], static_cast<int64_t>(k), &p)[0].a.sourceTime, before[k])
                << "variant " << variant << ", frame " << k << " after the split";
        st.undo();
        st.execute(edit::trimClip(p, "s", "c", ClipEdge::Head, Time(10, 30)));
        st.execute(edit::trimClip(p, "s", "c", ClipEdge::Tail, Time(60, 30)));
        for (int64_t k = 10; k < 60; ++k)
            ASSERT_EQ(evaluateVideoFrame(p.sequences[0], k, &p)[0].a.sourceTime, before[static_cast<size_t>(k)])
                << "variant " << variant << ", frame " << k << " after trimming";
        EXPECT_TRUE(validate(p).empty());
    }
}

TEST(TimeMap, ValidationAndPersistence) {
    Clip c = clip10s();
    c.speed.speed = Rational(20);
    EXPECT_FALSE(speedProblem(c).empty());   // outside 0.1x .. 10x
    c.speed.speed = Rational(1, 20);
    EXPECT_FALSE(speedProblem(c).empty());
    c.speed = {};
    c.speed.freeze = true;
    c.speed.reverse = true;
    EXPECT_FALSE(speedProblem(c).empty());
    Clip title;
    title.title = TitleData{};
    title.speed.speed = Rational(2);
    EXPECT_FALSE(speedProblem(title).empty());

    // a clip that runs past its media when slowed down is reported
    Project p;
    p.sources = {MediaSource{"A", "A.mp4", Rational(10), k30, 64, 36, true, false}};
    Sequence s;
    s.id = "s";
    Track v{"v1", TrackKind::Video, "Video 1"};
    Clip slow = clip10s();
    slow.speed.speed = Rational(1, 2);
    slow.duration = Time(21);   // 21 s at 0.5x needs 10.5 s of source
    v.clips.push_back(slow);
    s.tracks.push_back(v);
    p.sequences.push_back(s);
    EXPECT_FALSE(validate(p).empty());
    p.sequences[0].tracks[0].clips[0].duration = Time(20);
    EXPECT_TRUE(validate(p).empty());

    // format v5 round trip of every speed field
    Clip& r = p.sequences[0].tracks[0].clips[0];
    r.speed.maintainPitch = false;
    r.speed.interpolation = FrameInterpolation::OpticalFlow;
    const Project back = projectFromJsonString(projectToJsonString(p));
    EXPECT_EQ(back, p);
    p.sequences[0].tracks[0].clips[0].speed = {};
    p.sequences[0].tracks[0].clips[0].duration = Time(5);
    ClipSpeed ramp;
    ramp.ramp = rampKeys(findRampPreset("bullet")->keys, Time(0), Time(10));
    ramp.rampPreset = "bullet";
    p.sequences[0].tracks[0].clips[0].speed = ramp;
    EXPECT_EQ(projectFromJsonString(projectToJsonString(p)), p);
    EXPECT_NE(projectToJsonString(p).find("\"rampPreset\""), std::string::npos);
}

// ---- edit operations (M5) ------------------------------------------------------------------------------------

namespace {
Project avProject() {   // A (10 s, video + audio) then B (10 s) on Video 1 / Audio 1, linked pairs
    Project p;
    p.sources = {MediaSource{"A", "A.mp4", Rational(10), k30, 64, 36, true, true},
                 MediaSource{"B", "B.mp4", Rational(10), k30, 64, 36, true, true}};
    Sequence s;
    s.id = "s";
    p.sequences.push_back(s);
    UndoStack st(p);
    st.execute(edit::addMedia(p, "s", "A", Time(0)));
    st.execute(edit::addMedia(p, "s", "B", Time(10)));
    return p;
}
const Clip& at(const Project& p, size_t track, size_t i) { return p.sequences[0].tracks[track].clips[i]; }
}  // namespace

TEST(SpeedEdits, UniformSpeedRetimesLinkedAudioAndRipples) {
    Project p = avProject();
    UndoStack st(p);
    const std::string a = at(p, 0, 0).id;
    ClipSpeed fast;
    fast.speed = Rational(2);
    st.execute(edit::setSpeed(p, "s", a, fast));
    EXPECT_EQ(at(p, 0, 0).duration, Time(5));
    EXPECT_EQ(at(p, 1, 0).duration, Time(5));           // the linked audio too
    EXPECT_EQ(at(p, 1, 0).speed, fast);
    EXPECT_EQ(at(p, 0, 1).timelineStart, Time(5));      // B rippled left on both tracks
    EXPECT_EQ(at(p, 1, 1).timelineStart, Time(5));
    EXPECT_TRUE(validate(p).empty());
    ClipSpeed slow;
    slow.speed = Rational(1, 2);
    slow.reverse = true;
    st.execute(edit::setSpeed(p, "s", a, slow));
    EXPECT_EQ(at(p, 0, 0).duration, Time(20));
    EXPECT_EQ(at(p, 0, 1).timelineStart, Time(20));
    EXPECT_TRUE(validate(p).empty());
    st.undo();
    st.undo();
    EXPECT_EQ(at(p, 0, 0).duration, Time(10));
    EXPECT_EQ(at(p, 0, 1).timelineStart, Time(10));
    EXPECT_EQ(at(p, 0, 0).speed, ClipSpeed{});
    // out of range and freeze through setSpeed are refused
    ClipSpeed bad;
    bad.speed = Rational(50);
    EXPECT_THROW(edit::setSpeed(p, "s", a, bad), EditError);
    ClipSpeed fz;
    fz.freeze = true;
    EXPECT_THROW(edit::setSpeed(p, "s", a, fz), EditError);
}

TEST(SpeedEdits, SlowingDownNeverRunsPastTheMedia) {
    Project p = avProject();
    UndoStack st(p);
    const std::string a = at(p, 0, 0).id;
    const std::vector<std::pair<Rational, Time>> steps = {
        {Rational(7, 10), Time(429, 30)},   // 14.3 s: the last frame samples 9.9867 s, inside the media
        {Rational(1), Time(10)},            // back to 1x: the full 10 s again, not 10 s + rounding
        {Rational(3, 10), Time(1000, 30)},
        {Rational(1), Time(10)}};
    for (const auto& [v, want] : steps) {
        ClipSpeed sp;
        if (v != Rational(1)) sp.speed = v;
        st.execute(edit::setSpeed(p, "s", a, sp));
        EXPECT_EQ(at(p, 0, 0).duration, want) << v.toString();
        ASSERT_TRUE(validate(p).empty()) << v.toString() << ": " << validate(p)[0].message;
    }
    // the speed map cannot be changed behind the speed commands' back
    EXPECT_THROW(st.execute(makeModifyClip("s", a, "x", [](Clip& c) { c.speed.speed = Rational(2); })), EditError);
}

TEST(SpeedEdits, FreezeFrameInsertsASilentHold) {
    Project p = avProject();
    UndoStack st(p);
    const std::string a = at(p, 0, 0).id;
    std::string hold;
    st.execute(edit::freezeFrame(p, "s", a, Time(4), Time(2), &hold));
    // Video 1: A[0,4) hold[4,6) A'[6,12) B[12,22); Audio 1: A[0,4) A'[6,12) B[12,22)
    ASSERT_EQ(p.sequences[0].tracks[0].clips.size(), 4u);
    const Clip& h = at(p, 0, 1);
    EXPECT_EQ(h.id, hold);
    EXPECT_TRUE(h.speed.freeze);
    EXPECT_EQ(h.sourceIn, Time(4));      // the frame on screen at the playhead
    EXPECT_EQ(h.timelineStart, Time(4));
    EXPECT_TRUE(h.linkId.empty());       // no audio: silent
    EXPECT_EQ(at(p, 0, 2).timelineStart, Time(6));
    EXPECT_EQ(at(p, 0, 2).sourceIn, Time(4));   // resumes with the next frame
    EXPECT_EQ(at(p, 1, 1).timelineStart, Time(6));
    EXPECT_EQ(at(p, 0, 3).timelineStart, Time(12));
    EXPECT_TRUE(validate(p).empty());
    for (int64_t k : {int64_t(119), int64_t(120), int64_t(179), int64_t(180)}) {   // frames 3.967, 4, 5.967, 6 s
        const auto l = evaluateVideoFrame(p.sequences[0], k, &p);
        ASSERT_EQ(l.size(), 1u);
        const int64_t src = (l[0].a.sourceTime * Rational(30)).floor();
        EXPECT_EQ(src, k == 119 ? 119 : 120) << "output frame " << k;
    }
    st.undo();
    EXPECT_EQ(p.sequences[0].tracks[0].clips.size(), 2u);
    // boundaries: first frame of the clip (no split) and its last frame
    st.execute(edit::freezeFrame(p, "s", a, Time(0), Time(1)));
    EXPECT_TRUE(at(p, 0, 0).speed.freeze);
    EXPECT_EQ(at(p, 0, 0).sourceIn, Time(0));
    EXPECT_EQ(at(p, 0, 1).timelineStart, Time(1));
    st.undo();
    st.execute(edit::freezeFrame(p, "s", a, Time(299, 30), Time(1)));
    EXPECT_EQ(at(p, 0, 1).sourceIn, Time(299, 30));
    EXPECT_TRUE(validate(p).empty());
    st.undo();
    // a trimmed, retimed clip freezes the frame it really shows
    ClipSpeed fast;
    fast.speed = Rational(2);
    st.execute(edit::setSpeed(p, "s", a, fast));
    st.execute(edit::trimClip(p, "s", a, ClipEdge::Head, Time(1)));
    st.execute(edit::freezeFrame(p, "s", a, Time(2), Time(1)));
    EXPECT_EQ(at(p, 0, 1).sourceIn, Time(4));   // 2x: timeline 2 s shows source 4 s
    EXPECT_EQ(at(p, 0, 2).sourceIn, Time(4));   // the right half resumes there at 2x
    EXPECT_EQ(at(p, 0, 2).speed.speed, Rational(2));
    EXPECT_TRUE(validate(p).empty());
    EXPECT_THROW(edit::freezeFrame(p, "s", a, Time(20), Time(1)), EditError);   // not on the clip
}
