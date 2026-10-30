// M7 checkpoint D: Auto Reframe - the canvas for each aspect, the crop path (the subject kept inside the margins,
// smooth, inside the picture, held then returned when the subject leaves, cut at shot changes), the transform it
// writes (the crop read back from the keyframes), face detection on real footage (YuNet, with a negative control and
// CPU against DirectML), the motion-track subject on the synthetic tracking clip with its known path, and the
// renderer showing exactly the planned crop.
#include "fa/ai/faces.h"
#include "fa/ai/models.h"
#include "fa/ai/reframe.h"
#include "fa/media/media.h"
#include "fa/render/backend.h"
#include "fa/render/renderer.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace fa;
using ai::Face;

namespace {

std::filesystem::path models() { return std::filesystem::path(FA_MODELS_DIR); }
std::filesystem::path media(const char* name) { return std::filesystem::path(FA_TESTMEDIA_DIR) / name; }
std::filesystem::path tos() { return media("ai") / "tears_of_steel_60s.mov"; }

struct ModelsEnv {
    ModelsEnv() { ai::setModelsDirectoryOverride(models()); }
    ~ModelsEnv() { ai::setModelsDirectoryOverride({}); }
};

bool yunetReady() {
    return ai::modelStatus(*ai::findModel("yunet-face"), false, models()) == ai::ModelStatus::Installed &&
           std::filesystem::exists(tos());
}

// A path of samples every 0.2 s for `seconds`, subject at x(t), y 0.5.
ai::ReframePath path16x9(double seconds, const std::function<std::optional<double>(double)>& x) {
    ai::ReframePath p;
    p.sourceWidth = 1920;
    p.sourceHeight = 1080;
    p.fps = 25;
    for (int i = 0; i * 0.2 <= seconds + 1e-9; ++i) {   // exact sample times (as frames give them), no accumulation
        const double t = i * 0.2;
        ai::ReframeSample s;
        s.t = t;
        if (const auto v = x(t)) {
            s.x = *v;
            s.y = 0.5;
            s.found = true;
            ++p.found;
        }
        p.samples.push_back(s);
    }
    return p;
}

double maxStep(const ai::ReframePath& p) {
    double m = 0;
    for (size_t i = 1; i < p.centre.size(); ++i) m = std::max(m, std::abs(p.centre[i].first - p.centre[i - 1].first));
    return m;
}

}  // namespace

TEST(AutoReframe, CanvasForEachAspect) {
    EXPECT_EQ(ai::reframeCanvas(1920, 1080, 9.0 / 16), std::make_pair(1080, 1920));
    EXPECT_EQ(ai::reframeCanvas(1920, 1080, 1.0), std::make_pair(1080, 1080));
    EXPECT_EQ(ai::reframeCanvas(1920, 1080, 4.0 / 5), std::make_pair(1080, 1350));
    EXPECT_EQ(ai::reframeCanvas(1920, 1080, 16.0 / 9), std::make_pair(1920, 1080));
    EXPECT_EQ(ai::reframeCanvas(1080, 1920, 16.0 / 9), std::make_pair(1920, 1080));
    EXPECT_EQ(ai::reframeCanvas(1920, 1080, 3.0 / 4), std::make_pair(1080, 1440));   // custom
    EXPECT_EQ(ai::reframeCanvas(1280, 534, 9.0 / 16), std::make_pair(534, 950));
}

// A subject crossing the picture: the crop follows it, keeps it inside the margins, never leaves the picture and
// moves smoothly; more smoothness moves the crop less often (larger dead zone) without losing the subject.
TEST(AutoReframe, FollowsAMovingSubjectInsideTheMarginsSmoothly) {
    ai::ReframeOptions o;
    o.aspect = 9.0 / 16;
    o.margin = 0.1;
    for (double smooth : {0.0, 0.5, 1.0}) {
        o.smoothness = smooth;
        auto p = path16x9(6.0, [](double t) { return std::optional<double>(0.15 + 0.7 * t / 6.0); });
        ai::planReframe(p, o);
        EXPECT_NEAR(p.cropWidth, (9.0 / 16) / (16.0 / 9), 1e-9);
        EXPECT_NEAR(p.cropHeight, 1.0, 1e-9);
        double worst = 0;
        for (size_t i = 0; i < p.samples.size(); ++i) {
            const double c = p.centre[i].first, half = p.cropWidth / 2;
            EXPECT_GE(c - half, -1e-9);
            EXPECT_LE(c + half, 1 + 1e-9);
            // where the picture allows it, the subject is at least the margin inside the crop
            const double room = half * (1 - 2 * o.margin);
            const double sx = p.samples[i].x;
            if (sx - room >= half && sx + room <= 1 - half) worst = std::max(worst, std::abs(sx - c) - room);
        }
        EXPECT_LE(worst, 1e-9) << "smoothness " << smooth;
        std::printf("smoothness %.1f: largest crop step per 0.2 s %.4f of the width\n", smooth, maxStep(p));
        EXPECT_LT(maxStep(p), 0.05);
    }
}

// The subject leaves: the crop holds where it was for holdSeconds, then returns to the focal point; it comes back:
// the crop finds it again.
TEST(AutoReframe, SubjectExitHoldsThenReturnsToTheFocalPoint) {
    ai::ReframeOptions o;
    o.aspect = 9.0 / 16;
    o.smoothness = 0.2;
    o.holdSeconds = 1.0;
    o.focusX = 0.5;
    auto p = path16x9(8.0, [](double t) -> std::optional<double> {
        if (t < 2.0 || t >= 6.0) return 0.8;
        return std::nullopt;   // gone from 2 s to 6 s
    });
    ai::planReframe(p, o);
    auto at = [&](double t) {
        for (size_t i = 0; i < p.samples.size(); ++i)
            if (std::abs(p.samples[i].t - t) < 0.1) return p.centre[i].first;
        return -1.0;
    };
    const double max = 1 - p.cropWidth / 2;   // 0.8 is beyond what the crop can reach: it sits at the right edge
    EXPECT_NEAR(at(1.0), std::min(0.8, max), 0.02);
    EXPECT_NEAR(at(2.0), std::min(0.8, max), 0.03);   // held where the subject left (the hold lasts until 2.8 s)
    EXPECT_NEAR(at(5.0), 0.5, 0.03);                  // back at the focal point
    EXPECT_NEAR(at(7.8), std::min(0.8, max), 0.03);   // found again
}

// A shot change: the crop jumps at the cut (keys hold until it) instead of panning across it.
TEST(AutoReframe, ShotChangeIsACutNotAPan) {
    ai::ReframeOptions o;
    o.aspect = 9.0 / 16;
    o.smoothness = 0.8;
    auto p = path16x9(4.0, [](double t) { return std::optional<double>(t < 2.0 ? 0.25 : 0.75); });
    p.cuts = {2.0};
    ai::planReframe(p, o);
    const ClipTransform tr = ai::reframeTransform(p, Time(10), 1080, 1920, ClipTransform{});
    const auto before = ai::cropWindowAt(tr, 10 + 2.0 - 1.0 / 25, 1920, 1080, 1080, 1920);
    const auto after = ai::cropWindowAt(tr, 10 + 2.0, 1920, 1080, 1080, 1920);
    std::printf("crop centre one frame before the cut %.3f, on the cut %.3f\n", before.x, after.x);
    EXPECT_NEAR(before.x, 0.25 + 0.0, 0.03);
    EXPECT_NEAR(after.x, 0.75, 0.03);
}

// The transform written for the path shows the planned crop: read back from the thinned keyframes at every sample.
TEST(AutoReframe, TransformKeyframesReproduceThePlannedCrop) {
    ai::ReframeOptions o;
    o.aspect = 1.0;
    o.smoothness = 0.5;
    auto p = path16x9(5.0, [](double t) { return std::optional<double>(0.5 + 0.3 * std::sin(t)); });
    ai::planReframe(p, o);
    const auto [cw, ch] = ai::reframeCanvas(1920, 1080, o.aspect);
    ClipTransform base;
    base.rotation = Animatable<double>(0.0);
    const ClipTransform tr = ai::reframeTransform(p, Time(3), cw, ch, base);
    EXPECT_TRUE(tr.positionX.isAnimated());
    EXPECT_LT(tr.positionX.keys().size(), p.samples.size());   // thinned
    EXPECT_FALSE(tr.positionY.isAnimated());                     // full height: no vertical movement
    double worst = 0;
    for (size_t i = 0; i < p.samples.size(); ++i) {
        const auto w = ai::cropWindowAt(tr, 3 + p.samples[i].t, 1920, 1080, cw, ch);
        worst = std::max(worst, std::abs(w.x - p.centre[i].first));
        EXPECT_NEAR(w.w, p.cropWidth, 1e-9);
        EXPECT_NEAR(w.h, 1.0, 1e-9);
    }
    std::printf("keys %zu for %zu samples, worst crop error %.5f of the width\n", tr.positionX.keys().size(), p.samples.size(), worst);
    EXPECT_LT(worst, 0.004);   // the thinning tolerance (0.002 of the canvas width) in source terms
}

// YuNet on real footage: the two people on the bridge, nothing on a test pattern, the same faces on the CPU and on
// DirectML.
TEST(AiFaces, FindsThePeopleInRealFootageAndNothingInAPattern) {
    ModelsEnv env;
    if (!yunetReady()) GTEST_SKIP() << "YuNet or testmedia/ai missing";
    VideoDecoder dec(tos());
    const Image bridge = dec.frameAt(Time(10)).image;
    ai::FaceDetector cpu(ai::Accelerator::Cpu);
    const auto faces = cpu.detect(bridge);
    for (const auto& f : faces) std::printf("face %.3f %.3f %.3f x %.3f score %.2f\n", f.x, f.y, f.w, f.h, f.score);
    ASSERT_EQ(faces.size(), 2u);
    // the reference: OpenCV's own YuNet implementation (cv2.FaceDetectorYN 5.0, same model file, run on the same frame
    // at full size during development): boxes (x, y, w, h) 0.316 0.130 0.044 0.133 and 0.607 0.174 0.038 0.134
    const Face ref[2] = {{0.316, 0.130, 0.044, 0.133, 0.95}, {0.607, 0.174, 0.038, 0.134, 0.91}};
    for (const Face& r : ref) {
        const auto it = std::min_element(faces.begin(), faces.end(), [&](const Face& a, const Face& b) {
            return std::abs(a.x - r.x) < std::abs(b.x - r.x);
        });
        std::printf("reference %.3f %.3f %.3f %.3f -> found %.3f %.3f %.3f %.3f\n", r.x, r.y, r.w, r.h, it->x, it->y, it->w, it->h);
        EXPECT_NEAR(it->x + it->w / 2, r.x + r.w / 2, 0.01);
        EXPECT_NEAR(it->y + it->h / 2, r.y + r.h / 2, 0.02);
        EXPECT_NEAR(it->h, r.h, 0.03);
    }
    VideoDecoder pattern(media("C_testsrc2_1080p30_10s.mp4"));
    EXPECT_TRUE(cpu.detect(pattern.frameAt(Time(3)).image).empty());
    ai::FaceDetector gpu(ai::Accelerator::Gpu);
    const auto g = gpu.detect(bridge);
    ASSERT_EQ(g.size(), faces.size()) << gpu.provider();
    for (size_t i = 0; i < g.size(); ++i) {
        EXPECT_NEAR(g[i].x, faces[i].x, 0.005);
        EXPECT_NEAR(g[i].y, faces[i].y, 0.005);
    }
}

// Faces on real footage, 9:16: Tears of Steel 30-37 s - a face at x 0.57-0.67 (31-33 s), a shot change, a face at
// x 0.32-0.36 (34-36 s) (positions from OpenCV's YuNet, 1 s apart): every detected face stays inside the crop, the
// shot change is found and the crop jumps there instead of panning.
TEST(AiReframe, FacesOfRealFootageStayInTheVerticalCrop) {
    ModelsEnv env;
    if (!yunetReady()) GTEST_SKIP() << "YuNet or testmedia/ai missing";
    ai::ReframeOptions o;
    o.aspect = 9.0 / 16;
    o.margin = 0.08;
    const ai::ReframePath p = ai::analyseReframe(tos(), Time(30), Time(37), o);
    std::printf("Tears of Steel 30-37 s: %zu samples, subject in %d, %zu shot changes, %.1f s (%s)\n", p.samples.size(),
                p.found, p.cuts.size(), p.seconds, p.provider.c_str());
    ASSERT_GT(p.found, static_cast<int>(p.samples.size()) / 3);
    EXPECT_GE(p.cuts.size(), 1u);
    int inside = 0, found = 0;
    for (size_t i = 0; i < p.samples.size(); ++i) {
        if (!p.samples[i].found) continue;
        ++found;
        const double half = p.cropWidth / 2;
        inside += std::abs(p.samples[i].x - p.centre[i].first) <= half;
    }
    std::printf("subject inside the crop in %d of %d samples\n", inside, found);
    EXPECT_EQ(inside, found);
    // the crop follows the subject across the cut: right part of the picture at 32 s, left part at 35 s
    auto centreAt = [&](double t) {
        size_t best = 0;
        for (size_t i = 0; i < p.samples.size(); ++i)
            if (std::abs(p.samples[i].t - t) < std::abs(p.samples[best].t - t)) best = i;
        return p.centre[best].first;
    };
    std::printf("crop centre at 32 s %.3f, at 35 s %.3f\n", centreAt(2.0), centreAt(5.0));
    EXPECT_GT(centreAt(2.0), 0.5);
    EXPECT_LT(centreAt(5.0), 0.45);
}

// The motion-track subject (M5) on the synthetic tracking clip, whose patch path is known exactly: the patch stays in
// the 9:16 crop on every frame, and the rendered frame is the planned crop of the source.
TEST(AutoReframe, MotionTrackSubjectAndTheRenderedCrop) {
    const auto file = media("L_track_1080p30_5s.mp4");
    std::ifstream in(media("L_track_1080p30_5s.path.json"));
    if (!in) GTEST_SKIP() << "testmedia missing";
    const auto truth = nlohmann::json::parse(in)["centre_path"];
    MotionTrack track;
    for (const auto& s : truth) {
        TrackingSample ts;
        ts.frame = s[0].get<int64_t>();
        ts.x = s[1].get<double>() / 1920;
        ts.y = s[2].get<double>() / 1080;
        ts.confidence = 1;
        track.samples.push_back(ts);
    }
    ai::ReframeOptions o;
    o.aspect = 9.0 / 16;
    o.subject = ai::ReframeSubject::MotionTrack;
    o.margin = 0.1;
    const ai::ReframePath p = ai::analyseReframe(file, Time(0), Time(5), o, &track);
    ASSERT_EQ(p.found, static_cast<int>(p.samples.size()));
    EXPECT_TRUE(p.cuts.empty());
    const ClipTransform tr = ai::reframeTransform(p, Time(0), 1080, 1920, ClipTransform{});
    int worstFrame = -1;
    double worst = 1;
    for (const auto& s : truth) {
        const int64_t f = s[0].get<int64_t>();
        const auto w = ai::cropWindowAt(tr, f / 30.0, 1920, 1080, 1080, 1920);
        const double px = s[1].get<double>() / 1920;
        // distance of the patch centre from the crop's nearer edge, in crop widths (> 0: inside)
        const double margin = (w.w / 2 - std::abs(px - w.x)) / w.w;
        if (margin < worst) {
            worst = margin;
            worstFrame = static_cast<int>(f);
        }
    }
    std::printf("patch centre: closest to a crop edge %.3f of the crop width (frame %d)\n", worst, worstFrame);
    EXPECT_GT(worst, 0.05);
    // the renderer shows the crop: frame 75 of a 1080 x 1920 sequence = the source columns of the crop window
    Project proj;
    MediaSource src;
    src.id = "s";
    const std::u8string u8 = file.u8string();
    src.path = std::string(u8.begin(), u8.end());
    src.hasVideo = true;
    proj.sources.push_back(src);
    Sequence seq;
    seq.id = "seq";
    seq.width = 1080;
    seq.height = 1920;
    seq.frameRate = FrameRate(30);
    Track t;
    t.id = "v1";
    Clip c;
    c.id = "c";
    c.sourceId = "s";
    c.duration = Time(5);
    c.transform = tr;
    t.clips.push_back(c);
    seq.tracks.push_back(t);
    proj.sequences.push_back(seq);
    DecoderPool pool("");
    const auto backend = makeBackend(BackendChoice::Cpu);
    const Image out = renderFrame(proj.sequences[0], 75, proj, pool, *backend);
    VideoDecoder dec(file);
    const Image& source = dec.frameAt(Time(75, 30)).image;
    const auto w = ai::cropWindowAt(tr, 75 / 30.0, 1920, 1080, 1080, 1920);
    double err = 0;
    int n = 0;
    for (int y = 100; y < 1900; y += 37)
        for (int x = 20; x < 1060; x += 29) {
            const int sx = static_cast<int>((w.x - w.w / 2 + (x + 0.5) / 1080 * w.w) * 1920);
            const int sy = static_cast<int>((y + 0.5) / 1920 * 1080);
            for (int ch = 0; ch < 3; ++ch)
                err += std::abs(out.pixels[(static_cast<size_t>(y) * 1080 + x) * 4 + ch] -
                                source.pixels[(static_cast<size_t>(sy) * 1920 + sx) * 4 + ch]);
            n += 3;
        }
    std::printf("rendered frame vs the crop of the source: mean difference %.2f levels\n", err / n);
    EXPECT_LT(err / n, 6.0);   // resampling only (the 1080-pixel-wide source is scaled 1.78x)
}
