// Frame selection, cuts, layering and transitions without real media.
#include "fa/render/renderer.h"

#include <gtest/gtest.h>

#include <map>

using namespace fa;

namespace {

Clip makeClip(std::string id, std::string src, Time start, Time in, Time dur) {
    Clip c;
    c.id = std::move(id);
    c.sourceId = std::move(src);
    c.timelineStart = start;
    c.sourceIn = in;
    c.duration = dur;
    return c;
}

Project abProject(FrameRate rate) {
    Project p;
    p.sources = {MediaSource{"A", "A.mp4", Rational(10), FrameRate::fps30(), 64, 36, true, false},
                 MediaSource{"B", "B.mp4", Rational(10), FrameRate::fps30(), 64, 36, true, false}};
    Sequence s;
    s.id = "s";
    s.width = 64;
    s.height = 36;
    s.frameRate = rate;
    Track v;
    v.id = "v1";
    v.clips = {makeClip("a", "A", Rational(0), Rational(0), Rational(10)),
               makeClip("b", "B", Rational(10), Rational(0), Rational(10))};
    s.tracks.push_back(v);
    p.sequences.push_back(s);
    return p;
}

// Solid-colour frames: red = clip A, blue = clip B; green channel carries the source frame index.
class FakeProvider : public FrameProvider {
public:
    Result frame(const Clip& clip, const MediaSource& src, const Time& t) override {
        Result r;
        r.requestedTime = t;
        Time c = t;
        if (c < Time(0)) {
            c = Time(0);
            r.clamped = true;
        }
        const Time last = *src.duration - Rational(1, 30);
        if (c > last) {
            c = last;
            r.clamped = true;
        }
        r.frameIndex = timeToFrame(c, FrameRate::fps30());
        r.frameTime = frameToTime(r.frameIndex, FrameRate::fps30());
        Image& img = images_[clip.id];
        const uint8_t g = static_cast<uint8_t>(r.frameIndex % 256);
        img.reset(64, 36, src.id == "A" ? Rgba8{200, g, 0, 255} : Rgba8{0, g, 200, 255});
        r.image = &img;
        return r;
    }

private:
    std::map<std::string, Image> images_;
};

}  // namespace

TEST(TimelineEval, HardCutIsFrameAccurate) {
    const Project p = abProject(FrameRate::fps30());
    const Sequence& s = p.sequences[0];
    EXPECT_EQ(s.frameCount(), 600);
    for (int64_t n : {0, 1, 298, 299, 300, 301, 599}) {
        const auto layers = evaluateVideoFrame(s, n);
        ASSERT_EQ(layers.size(), 1u) << n;
        EXPECT_EQ(layers[0].a.clip->id, n < 300 ? "a" : "b") << n;
        EXPECT_EQ(layers[0].a.sourceTime, frameToTime(n < 300 ? n : n - 300, FrameRate::fps30())) << n;
        EXPECT_FALSE(layers[0].b.has_value());
    }
    EXPECT_TRUE(evaluateVideoFrame(s, 600).empty());   // past the end
}

TEST(TimelineEval, NtscSequenceSamplesThirtyFpsSourceByTime) {
    Project p = abProject(FrameRate::ntsc30());
    // cut at frame 150 of the 29.97 sequence
    auto& clips = p.sequences[0].tracks[0].clips;
    clips[0].duration = frameToTime(150, FrameRate::ntsc30());
    clips[1].timelineStart = clips[0].duration;
    clips[1].duration = frameToTime(150, FrameRate::ntsc30());
    ASSERT_FALSE(hasErrors(validate(p))) << formatIssues(validate(p));
    const Sequence& s = p.sequences[0];
    EXPECT_EQ(s.frameCount(), 300);
    for (int64_t n = 0; n < 300; ++n) {
        const auto l = evaluateVideoFrame(s, n);
        ASSERT_EQ(l.size(), 1u);
        const int64_t local = n < 150 ? n : n - 150;
        EXPECT_EQ(l[0].a.clip->id, n < 150 ? "a" : "b");
        // source frame = floor(local * 1001/30000 * 30) = floor(local * 1.001)
        EXPECT_EQ(timeToFrame(l[0].a.sourceTime, FrameRate::fps30()), (local * 1001) / 1000) << n;
    }
}

TEST(TimelineEval, GapsAndLayers) {
    Project p = abProject(FrameRate::fps25());
    auto& v1 = p.sequences[0].tracks[0];
    v1.clips[1].timelineStart = Rational(12);   // 2 s gap
    Track v2;
    v2.id = "v2";
    v2.clips = {makeClip("top", "A", Rational(1), Rational(5), Rational(1))};
    p.sequences[0].tracks.push_back(v2);
    const Sequence& s = p.sequences[0];
    EXPECT_TRUE(evaluateVideoFrame(s, 25 * 11).empty());   // gap
    const auto l = evaluateVideoFrame(s, 25);              // t = 1 s: base + top
    ASSERT_EQ(l.size(), 2u);
    EXPECT_EQ(l[0].a.clip->id, "a");
    EXPECT_EQ(l[1].a.clip->id, "top");
    EXPECT_EQ(l[1].a.sourceTime, Rational(5));
    p.sequences[0].tracks[1].hidden = true;
    EXPECT_EQ(evaluateVideoFrame(p.sequences[0], 25).size(), 1u);
    p.sequences[0].tracks[0].clips[0].enabled = false;
    EXPECT_TRUE(evaluateVideoFrame(p.sequences[0], 25).empty());
}

// Filmora EXP-001a: 2 s dissolve centred on the 10 s cut, 30 fps, no handles:
// frames 270..329 blend linearly, mix 0.5 at 10.000 s; A's source runs past its media (held),
// B's source starts before 0 (held at frame 0).
TEST(TimelineEval, CenteredDissolveMatchesExp001a) {
    Project p = abProject(FrameRate::fps30());
    p.sequences[0].tracks[0].clips[0].outTransition =
        TransitionRef{"fa.transition.dissolve", Rational(2), TransitionAlignment::Centered};
    const Sequence& s = p.sequences[0];
    EXPECT_EQ(evaluateVideoFrame(s, 269)[0].b, std::nullopt);
    for (int64_t n = 270; n < 330; ++n) {
        const auto l = evaluateVideoFrame(s, n);
        ASSERT_EQ(l.size(), 1u);
        ASSERT_TRUE(l[0].b.has_value()) << n;
        EXPECT_NEAR(l[0].mix, (n - 270) / 60.0, 1e-12);
        EXPECT_EQ(l[0].a.sourceTime, frameToTime(n, FrameRate::fps30()));
        EXPECT_EQ(l[0].b->sourceTime, frameToTime(n - 300, FrameRate::fps30()));
    }
    EXPECT_NEAR(evaluateVideoFrame(s, 300)[0].mix, 0.5, 1e-12);
    EXPECT_EQ(evaluateVideoFrame(s, 330)[0].b, std::nullopt);

    FakeProvider fp;
    auto backend = makeCpuBackend();
    std::vector<FrameProvider::Result> frames;
    renderFrame(s, 285, p, fp, *backend, nullptr, &frames);
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0].frameIndex, 285);   // A plays real frames before the cut
    EXPECT_EQ(frames[1].frameIndex, 0);     // B frozen on its first frame (no head handle)
    EXPECT_TRUE(frames[1].clamped);
    frames.clear();
    renderFrame(s, 315, p, fp, *backend, nullptr, &frames);
    EXPECT_EQ(frames[0].frameIndex, 299);   // A frozen on its last frame
    EXPECT_TRUE(frames[0].clamped);
    EXPECT_EQ(frames[1].frameIndex, 15);
}

// EXP-001c Prefix (EndsAtCut) and Postfix (StartsAtCut) spans.
TEST(TimelineEval, PrefixAndPostfixSpans) {
    Project p = abProject(FrameRate::fps30());
    auto& a = p.sequences[0].tracks[0].clips[0];
    a.outTransition = TransitionRef{"fa.transition.dissolve", Rational(2), TransitionAlignment::EndsAtCut};
    auto span = transitionSpan(p.sequences[0].tracks[0], 0);
    ASSERT_TRUE(span);
    EXPECT_EQ(span->range, (TimeRange{Rational(8), Rational(2)}));
    a.outTransition->alignment = TransitionAlignment::StartsAtCut;
    a.outTransition->duration = Rational(1);
    span = transitionSpan(p.sequences[0].tracks[0], 0);
    EXPECT_EQ(span->range, (TimeRange{Rational(10), Rational(1)}));
    const auto l = evaluateVideoFrame(p.sequences[0], 315);
    ASSERT_TRUE(l[0].b);
    EXPECT_NEAR(l[0].mix, 0.5, 1e-12);
    // invalid: transition longer than the incoming clip
    a.outTransition->duration = Rational(11);
    EXPECT_THROW(transitionSpan(p.sequences[0].tracks[0], 0), ModelError);
    // invalid: no adjacent clip
    a.outTransition->duration = Rational(1);
    p.sequences[0].tracks[0].clips[1].timelineStart = Rational(11);
    EXPECT_THROW(transitionSpan(p.sequences[0].tracks[0], 0), ModelError);
}

TEST(CpuBackend, DissolveIsLinearMixInEncodedRgb) {
    Project p = abProject(FrameRate::fps30());
    p.sequences[0].tracks[0].clips[0].outTransition =
        TransitionRef{"fa.transition.dissolve", Rational(2), TransitionAlignment::Centered};
    FakeProvider fp;
    auto backend = makeCpuBackend();
    for (int64_t n : {270, 285, 300, 315, 329}) {
        const Image& img = renderFrame(p.sequences[0], n, p, fp, *backend);
        const double mix = (n - 270) / 60.0;
        const Rgba8 c = img.at(32, 18);
        EXPECT_NEAR(c.r, 200 * (1 - mix), 1.0) << n;
        EXPECT_NEAR(c.b, 200 * mix, 1.0) << n;
        EXPECT_EQ(c.a, 255);
    }
}

TEST(CpuBackend, OpacityKeyframesUseSourceTime) {
    Project p = abProject(FrameRate::fps30());
    Clip& a = p.sequences[0].tracks[0].clips[0];
    a.sourceIn = Rational(2);   // timeline 0 shows source 2 s
    Keyframe<double> k0, k1;
    k0.time = Rational(2);
    k0.value = 0.0;
    k1.time = Rational(3);
    k1.value = 1.0;
    a.opacity.setKey(k0);
    a.opacity.setKey(k1);
    FakeProvider fp;
    auto backend = makeCpuBackend();
    const Image& img = renderFrame(p.sequences[0], 15, p, fp, *backend);   // t = 0.5 s -> source 2.5 s
    EXPECT_NEAR(img.at(1, 1).r, 100, 1);
}

TEST(CpuBackend, FitLetterboxesSmallerSources) {
    auto backend = makeCpuBackend();
    Image src(32, 32, Rgba8{255, 255, 255, 255});   // square onto 64x36
    backend->beginFrame(64, 36, Rgba8{0, 0, 0, 255});
    LayerDesc l;
    l.image = &src;
    backend->drawLayer(l);
    const Image& out = backend->endFrame();
    EXPECT_EQ(out.at(32, 18).r, 255);   // centre covered
    EXPECT_EQ(out.at(2, 18).r, 0);      // pillarbox left
    EXPECT_EQ(out.at(61, 18).r, 0);     // pillarbox right
    EXPECT_EQ(out.at(15, 18).r, 255);   // 36x36 placed at x 14..50
    EXPECT_GT(out.at(14, 18).r, 200);   // edge pixel: anti-aliased (bilinear with transparent border)
    EXPECT_LT(out.at(13, 18).r, 60);
}
