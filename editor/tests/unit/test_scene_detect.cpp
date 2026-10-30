// M7 checkpoint D: scene detection against sequences with known boundaries, built from the test media:
// hard cuts, a white flash (not a cut), a fade through black (one boundary), shaky camera motion and a moving
// target (no boundaries), and a cut between two similar shots.
#include "fa/media/media.h"
#include "fa/model/edit_ops.h"
#include "fa/model/serialize.h"
#include "fa/vision/scene_detect.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

using namespace fa;
using namespace fa::vision;

namespace {

std::filesystem::path media(const char* name) { return std::filesystem::path(FA_TESTMEDIA_DIR) / name; }

struct Clip {
    const char* file;
    double from;   // seconds in the source
    int frames;
};

std::vector<Image> framesOf(const Clip& c) {
    VideoDecoder::Options o;
    o.maxWidth = 320;
    o.maxHeight = 180;
    VideoDecoder dec(media(c.file), o);
    const Rational fps = dec.info().frameRate.fps();
    std::vector<Image> out;
    const int64_t first = static_cast<int64_t>(std::lround(c.from * fps.toDouble()));
    for (int i = 0; i < c.frames; ++i) out.push_back(dec.frameAt(Time(first + i) / fps).image);
    return out;
}

Image scaled(const Image& img, double k) {
    Image out = img;
    for (size_t i = 0; i < out.pixels.size(); i += 4)
        for (int c = 0; c < 3; ++c) out.pixels[i + c] = static_cast<uint8_t>(std::lround(out.pixels[i + c] * k));
    return out;
}

struct Score {
    int hits = 0, missed = 0, falsePositives = 0, worstError = 0;
};

Score score(const std::vector<SceneBoundary>& found, const std::vector<int64_t>& truth, int tolerance) {
    Score s;
    std::set<size_t> used;
    for (int64_t t : truth) {
        int best = -1;
        int64_t bestErr = tolerance + 1;
        for (size_t i = 0; i < found.size(); ++i)
            if (!used.count(i) && std::llabs(found[i].frame - t) < bestErr) {
                best = static_cast<int>(i);
                bestErr = std::llabs(found[i].frame - t);
            }
        if (best >= 0) {
            used.insert(static_cast<size_t>(best));
            ++s.hits;
            s.worstError = std::max(s.worstError, static_cast<int>(bestErr));
        } else {
            ++s.missed;
        }
    }
    s.falsePositives = static_cast<int>(found.size() - used.size());
    return s;
}

}  // namespace

TEST(SceneDetect, CutsFlashFadeAndMotionWithKnownBoundaries) {
    std::vector<Image> seq;
    std::vector<int64_t> cuts;
    auto append = [&](const std::vector<Image>& v, bool cutBefore) {
        if (cutBefore && !seq.empty()) cuts.push_back(static_cast<int64_t>(seq.size()));
        seq.insert(seq.end(), v.begin(), v.end());
    };
    append(framesOf({"C_testsrc2_1080p30_10s.mp4", 0.0, 60}), false);   // 0: test pattern with moving parts
    append(framesOf({"L_track_1080p30_5s.mp4", 0.0, 75}), true);        // 60: a moving target on a textured field
    // a 2-frame white flash inside the L_track shot (camera flash): not a boundary
    {
        auto more = framesOf({"L_track_1080p30_5s.mp4", 2.5, 45});
        Image white = more[0];
        std::fill(white.pixels.begin(), white.pixels.end(), uint8_t(255));
        seq.push_back(white);
        seq.push_back(white);
        seq.insert(seq.end(), more.begin() + 2, more.end());
    }
    append(framesOf({"M_shaky_1080p30_5s.mp4", 0.0, 90}), true);        // 180: shaky hand-held camera
    // a fade through black into the green-screen shot: one boundary (the first frame after the black)
    {
        auto out = framesOf({"M_shaky_1080p30_5s.mp4", 3.0, 15});
        for (int i = 0; i < 15; ++i) seq.push_back(scaled(out[static_cast<size_t>(i)], 1.0 - (i + 1) / 15.0));
        auto in = framesOf({"D_greenscreen_disc_1080p30_10s.mp4", 1.0, 75});
        const int64_t black = static_cast<int64_t>(seq.size()) - 1;   // the last faded frame is black
        cuts.push_back(black + 1);
        for (int i = 0; i < 15; ++i) seq.push_back(scaled(in[static_cast<size_t>(i)], (i + 1) / 15.0));
        seq.insert(seq.end(), in.begin() + 15, in.end());
    }
    append(framesOf({"A_red_1080p30_10s.mp4", 0.0, 45}), true);   // a hard cut to a flat red picture
    append(framesOf({"B_blue_1080p30_10s.mp4", 0.0, 45}), true);   // red -> blue
    append(framesOf({"C_testsrc2_1080p30_10s.mp4", 5.0, 60}), true);
    SceneDetector det(SceneOptions{}, 30.0);
    for (const Image& f : seq) det.push(f);
    const auto found = det.finish();
    for (const auto& b : found)
        std::printf("boundary at %lld (%s, score %.1f)\n", static_cast<long long>(b.frame), b.kind == BoundaryKind::Fade ? "fade" : "cut",
                    b.score);
    const Score s = score(found, cuts, 2);
    std::printf("%zu frames, %zu true boundaries: %d found, %d missed, %d false, worst error %d frames\n", seq.size(),
                cuts.size(), s.hits, s.missed, s.falsePositives, s.worstError);
    EXPECT_EQ(s.missed, 0);
    EXPECT_EQ(s.falsePositives, 0);   // no boundary for the flash, the shaky camera or the moving target
    // hard cuts are frame-exact
    for (int64_t c : cuts) {
        if (std::find_if(found.begin(), found.end(), [&](const SceneBoundary& b) { return b.kind == BoundaryKind::Fade && std::llabs(b.frame - c) <= 2; }) != found.end())
            continue;
        EXPECT_NE(std::find_if(found.begin(), found.end(), [&](const SceneBoundary& b) { return b.frame == c; }), found.end())
            << "cut at " << c;
    }
}

TEST(SceneDetect, SensitivityAndMinimumSceneLength) {
    // two similar shots (the test pattern 3 s apart): a subtle cut found only at high sensitivity
    std::vector<Image> seq;
    for (const auto& f : framesOf({"C_testsrc2_1080p30_10s.mp4", 0.0, 45})) seq.push_back(f);
    for (const auto& f : framesOf({"C_testsrc2_1080p30_10s.mp4", 6.0, 45})) seq.push_back(f);
    int low = 0, high = 0;
    for (double sens : {0.0, 1.0}) {
        SceneOptions o;
        o.sensitivity = sens;
        SceneDetector det(o, 30.0);
        for (const Image& f : seq) det.push(f);
        const auto found = det.finish();
        (sens == 0.0 ? low : high) = static_cast<int>(found.size());
        for (const auto& b : found) EXPECT_EQ(b.frame, 45) << "sensitivity " << sens;
    }
    std::printf("subtle cut: %d boundary at sensitivity 0, %d at 1\n", low, high);
    EXPECT_GE(high, low);
    // negative control: one continuous shot gives no boundary at the highest sensitivity
    SceneOptions o;
    o.sensitivity = 1.0;
    SceneDetector det(o, 30.0);
    for (const auto& f : framesOf({"M_shaky_1080p30_5s.mp4", 0.0, 150})) det.push(f);
    EXPECT_TRUE(det.finish().empty());
    // minimum scene length: cuts 5 frames apart keep one boundary
    SceneOptions m;
    m.minSceneSeconds = 0.5;
    SceneDetector d2(m, 30.0);
    const auto red = framesOf({"A_red_1080p30_10s.mp4", 0.0, 30}), blue = framesOf({"B_blue_1080p30_10s.mp4", 0.0, 5}),
               test = framesOf({"C_testsrc2_1080p30_10s.mp4", 0.0, 30});
    for (const auto* v : {&red, &blue, &test})
        for (const auto& f : *v) d2.push(f);
    EXPECT_EQ(d2.finish().size(), 1u);
}

namespace {

std::filesystem::path sintel() { return std::filesystem::path(FA_TESTMEDIA_DIR) / "ai" / "sintel_trailer_720p.mp4"; }

// Frames [first, first + count) of the Sintel trailer at 320 px.
std::vector<Image> sintelFrames(int64_t first, int count) {
    VideoDecoder::Options o;
    o.maxWidth = 320;
    o.maxHeight = 180;
    VideoDecoder dec(sintel(), o);
    std::vector<Image> out;
    for (int i = 0; i < count; ++i) out.push_back(dec.frameAt(Time(first + i, 24)).image);
    return out;
}

Image mix(const Image& a, const Image& b, double t) {
    Image out = a;
    for (size_t i = 0; i < out.pixels.size(); ++i)
        out.pixels[i] = static_cast<uint8_t>(std::lround(a.pixels[i] * (1 - t) + b.pixels[i] * t));
    return out;
}

}  // namespace

// Real footage edited with exactly known boundaries: single shots of the Sintel trailer (each checked to contain no
// cut, see TEST_RESULTS.md) joined by hard cuts, a dissolve and a fade through black, with a white flash inside a shot.
// The fast whip-pan shot (frames 712-742) and the flickering fire shot are the hardest negatives in the trailer.
TEST(SceneDetect, RealFootageEditedWithKnownBoundaries) {
    if (!std::filesystem::exists(sintel())) GTEST_SKIP() << "testmedia/ai not fetched";
    std::vector<Image> seq;
    std::vector<int64_t> cuts;
    auto add = [&](int64_t first, int count, bool cutBefore) {
        if (cutBefore) cuts.push_back(static_cast<int64_t>(seq.size()));
        for (auto& f : sintelFrames(first, count)) seq.push_back(std::move(f));
    };
    add(712, 30, false);   // 0: whip pan, dragon and Sintel
    add(440, 60, true);    // 30: dark corridor, slow camera
    add(540, 28, true);    // 90: Sintel in front of fire (flickering light)
    {   // a 2-frame white flash inside the next shot (the city): not a boundary
        auto city = sintelFrames(576, 40);
        cuts.push_back(static_cast<int64_t>(seq.size()));
        for (int i = 0; i < 40; ++i) {
            if (i == 20 || i == 21) {
                Image w = city[static_cast<size_t>(i)];
                std::fill(w.pixels.begin(), w.pixels.end(), uint8_t(255));
                seq.push_back(w);
            } else {
                seq.push_back(city[static_cast<size_t>(i)]);
            }
        }
    }
    add(624, 26, true);    // 158: dragon in the sky
    // a 12-frame dissolve into the desert: a gradual boundary (counted when found anywhere inside it)
    const int64_t dissolveStart = static_cast<int64_t>(seq.size());
    {
        auto a = sintelFrames(800, 12), b = sintelFrames(880, 60);
        for (int i = 0; i < 12; ++i) seq.push_back(mix(a[static_cast<size_t>(i)], b[static_cast<size_t>(i)], (i + 1) / 13.0));
        seq.insert(seq.end(), b.begin() + 12, b.end());
    }
    // a fade through black into the snow shot: one boundary, the first frame after the black
    int64_t fadeBoundary = 0;
    {
        auto out = sintelFrames(940, 12), in = sintelFrames(140, 50);
        for (int i = 0; i < 12; ++i) seq.push_back(mix(out[static_cast<size_t>(i)], Image(out[0].width, out[0].height), (i + 1) / 12.0));
        fadeBoundary = static_cast<int64_t>(seq.size());
        for (int i = 0; i < 12; ++i) seq.push_back(mix(Image(in[0].width, in[0].height), in[static_cast<size_t>(i)], (i + 1) / 12.0));
        seq.insert(seq.end(), in.begin() + 12, in.end());
    }
    add(334, 30, true);    // Sintel close-up (dark)
    SceneDetector det(SceneOptions{}, 24.0);
    for (const Image& f : seq) det.push(f);
    const auto found = det.finish();
    for (const auto& b : found)
        std::printf("boundary at %lld (%s, score %.1f)\n", static_cast<long long>(b.frame), b.kind == BoundaryKind::Fade ? "fade" : "cut",
                    b.score);
    // hard cuts: frame-exact
    const Score hard = score(found, cuts, 0);
    // the fade: the first frame after the black, +-2
    const bool fadeFound = std::any_of(found.begin(), found.end(), [&](const SceneBoundary& b) { return std::llabs(b.frame - fadeBoundary) <= 2; });
    const bool dissolveFound = std::any_of(found.begin(), found.end(), [&](const SceneBoundary& b) {
        return b.frame >= dissolveStart && b.frame <= dissolveStart + 12;
    });
    int falsePositives = 0;
    for (const auto& b : found) {
        const bool known = std::find(cuts.begin(), cuts.end(), b.frame) != cuts.end() || std::llabs(b.frame - fadeBoundary) <= 2 ||
                           (b.frame >= dissolveStart && b.frame <= dissolveStart + 12);
        falsePositives += !known;
    }
    std::printf("%zu frames: %d of %zu hard cuts frame-exact, fade %s, dissolve %s, %d false\n", seq.size(), hard.hits,
                cuts.size(), fadeFound ? "found" : "missed", dissolveFound ? "found" : "missed", falsePositives);
    EXPECT_EQ(hard.missed, 0);
    EXPECT_TRUE(fadeFound);
    EXPECT_EQ(falsePositives, 0);   // whip pan, fire flicker, flash
    RecordProperty("dissolve", dissolveFound ? "found" : "missed");
}

// The whole Sintel trailer against the hand-checked reference list (TEST_RESULTS.md): 11 hard cuts (3 of them cuts to
// black) and 8 gradual transitions (fades through black or white, dissolves, title fades). Every candidate frame difference was inspected visually; the
// shot from 712 to 742 (a whip pan) and the fire shot (536-570) contain no cuts.
TEST(SceneDetect, SintelTrailerAgainstTheHandCheckedReference) {
    if (!std::filesystem::exists(sintel())) GTEST_SKIP() << "testmedia/ai not fetched";
    const SceneResult r = detectScenes(sintel(), SceneOptions{});
    EXPECT_EQ(r.frames, 1253);
    EXPECT_NEAR(r.fps, 24.0, 1e-6);
    EXPECT_EQ(r.thumbnails.size(), r.boundaries.size() + 1);
    const std::vector<int64_t> hardCuts = {334, 396, 404, 408, 420, 571, 621, 658, 672, 1163, 1220};
    // gradual: [first, last] frames of each transition from the per-frame luma (a detection anywhere inside counts)
    const std::vector<std::pair<int64_t, int64_t>> gradual = {{116, 140}, {196, 228}, {252, 298}, {498, 538},
                                                              {752, 784}, {842, 876}, {964, 1022}, {1098, 1160}};
    // parts of a hard cut: the fade-out before the cut from black at 396, the fade-ins after the cuts to black
    const std::vector<std::pair<int64_t, int64_t>> withCut = {{360, 395}, {420, 446}, {672, 694}};
    for (const auto& b : r.boundaries)
        std::printf("  boundary at %lld (%s, score %.1f)\n", static_cast<long long>(b.frame), b.kind == BoundaryKind::Fade ? "fade" : "cut", b.score);
    const Score hard = score(r.boundaries, hardCuts, 1);
    int exact = 0;
    for (int64_t c : hardCuts) exact += std::any_of(r.boundaries.begin(), r.boundaries.end(), [&](const SceneBoundary& b) { return b.frame == c; });
    int gradualFound = 0, falsePositives = 0;
    for (const auto& [a, b] : gradual)
        gradualFound += std::any_of(r.boundaries.begin(), r.boundaries.end(), [&](const SceneBoundary& x) {
            return x.frame >= a && x.frame <= b && std::find(hardCuts.begin(), hardCuts.end(), x.frame) == hardCuts.end();
        });
    for (const auto& x : r.boundaries) {
        const bool nearCut = std::any_of(hardCuts.begin(), hardCuts.end(), [&](int64_t c) { return std::llabs(x.frame - c) <= 1; });
        auto inside = [&](const auto& g) { return x.frame >= g.first && x.frame <= g.second; };
        const bool inGradual = std::any_of(gradual.begin(), gradual.end(), inside) || std::any_of(withCut.begin(), withCut.end(), inside);
        if (!nearCut && !inGradual) {
            ++falsePositives;
            std::printf("false boundary at %lld\n", static_cast<long long>(x.frame));
        }
    }
    std::printf("Sintel trailer, %lld frames in %.1f s: hard cuts %d / %zu found (%d frame-exact), gradual %d / %zu, "
                "false %d\n",
                static_cast<long long>(r.frames), r.seconds, hard.hits, hardCuts.size(), exact, gradualFound, gradual.size(),
                falsePositives);
    RecordProperty("hard_found", hard.hits);
    RecordProperty("gradual_found", gradualFound);
    RecordProperty("false_positives", falsePositives);
    EXPECT_GE(hard.hits, 10);
    EXPECT_GE(exact, 9);
    EXPECT_LE(falsePositives, 1);
}

// Scenes in the library (project format v8): an item places only its range of the file, under its own name; saving
// and loading keeps it; a v7 project (no ranges) loads unchanged; a negative range is refused by validation.
TEST(SceneDetect, SceneItemsPlaceOnlyTheirRange) {
    Project p;
    MediaSource whole;
    whole.id = "f";
    whole.path = "film.mp4";
    whole.duration = Rational(60);
    whole.frameRate = FrameRate(24);
    whole.hasAudio = true;
    p.sources.push_back(whole);
    MediaSource scene = whole;
    scene.id = "f_s3";
    scene.name = "film - Scene 3";
    scene.range = TimeRange{Rational(334, 24), Rational(62, 24)};
    p.sources.push_back(scene);
    Sequence q;
    q.id = "seq";
    q.frameRate = FrameRate(24);
    p.sequences.push_back(q);
    edit::AddedClips added;
    auto cmd = edit::addMedia(p, "seq", "f_s3", Time(0), {}, &added);
    cmd->apply(p);
    const fa::Clip* v = edit::findClip(p.sequences[0], added.videoClipId);
    const fa::Clip* a = edit::findClip(p.sequences[0], added.audioClipId);
    ASSERT_TRUE(v && a);
    EXPECT_EQ(v->sourceIn, Rational(334, 24));
    EXPECT_EQ(v->duration, Rational(62, 24));
    EXPECT_EQ(v->name, "film - Scene 3");
    EXPECT_EQ(a->sourceIn, v->sourceIn);   // the sound of the scene, in sync
    EXPECT_EQ(a->duration, v->duration);
    const Project back = projectFromJsonString(projectToJsonString(p));
    EXPECT_EQ(back.sources[1].name, "film - Scene 3");
    ASSERT_TRUE(back.sources[1].range.has_value());
    EXPECT_EQ(back.sources[1].range->start, Rational(334, 24));
    EXPECT_FALSE(back.sources[0].range.has_value());
    // an older project (format 7, no ranges) loads with whole-file items
    const std::string v7 = R"({"format":"fa-project","version":7,"sources":[{"id":"f","path":"film.mp4","hasVideo":true}],"sequences":[]})";
    const Project old = projectFromJsonString(v7);
    EXPECT_FALSE(old.sources[0].range.has_value());
    EXPECT_TRUE(old.sources[0].name.empty());
    Project bad = p;
    bad.sources[1].range->duration = Rational(0);
    EXPECT_TRUE(hasErrors(validate(bad)));
}
