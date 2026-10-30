// M5 stabilisation: the plan (fa/model/stabilize_plan.h), the camera-motion measurement (fa/vision/stabilize.h) and the
// EXP-014 comparison end to end. testmedia/M_shaky_1080p30_5s.mp4 is a static scene shifted every frame by a seeded
// random-walk jitter (ground truth in M_shaky_1080p30_5s.jitter.json); Stage 1 measured the red marker's
// frame-to-frame motion (RMS) with tools/analyze_stab.py: input 13.27 px, Filmora 16 Normal 3.77 px, Strong 1.91 px.
#include "fa/media/media.h"
#include "fa/model/edit_ops.h"
#include "fa/model/stabilize_plan.h"
#include "fa/render/renderer.h"
#include "fa/vision/stabilize.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>
#include <vector>

using namespace fa;

namespace {

std::filesystem::path media(const char* name) { return std::filesystem::path(FA_TESTMEDIA_DIR) / name; }

// (dx, dy) per frame: the frame shows the scene so that the marker appears at centre - (dx, dy).
std::vector<std::pair<int, int>> jitter() {
    std::ifstream in(media("M_shaky_1080p30_5s.jitter.json"));
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    std::vector<std::pair<int, int>> out;
    const std::regex row(R"(\[(\d+),\s*(-?\d+),\s*(-?\d+)\])");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), row); it != std::sregex_iterator(); ++it)
        out.push_back({std::stoi((*it)[2]), std::stoi((*it)[3])});
    return out;
}

// Red marker centroid per frame (tools/analyze_stab.py: R > 170, G < 80, B < 80); NaN when it is not visible.
std::vector<std::pair<double, double>> markerPath(const std::filesystem::path& file, int frames) {
    VideoDecoder dec(file);
    std::vector<std::pair<double, double>> out;
    for (int f = 0; f < frames; ++f) {
        const Image& img = dec.frameAt(Rational(f, 30) + Rational(1, 120)).image;
        double sx = 0, sy = 0;
        int n = 0;
        // the marker stays within ~150 px of the centre (jitter up to 60 px, zoom up to 125 %)
        for (int y = std::max(0, img.height / 2 - 260); y < std::min(img.height, img.height / 2 + 260); ++y) {
            const uint8_t* p = img.row(y);
            for (int x = std::max(0, img.width / 2 - 260); x < std::min(img.width, img.width / 2 + 260); ++x)
                if (p[x * 4] > 170 && p[x * 4 + 1] < 80 && p[x * 4 + 2] < 80) {
                    sx += x;
                    sy += y;
                    ++n;
                }
        }
        out.push_back(n >= 100 ? std::pair<double, double>{sx / n, sy / n} : std::pair<double, double>{NAN, NAN});
    }
    return out;
}

double rmsStep(const std::vector<std::pair<double, double>>& path) {
    double sq = 0;
    int n = 0;
    for (size_t i = 1; i < path.size(); ++i) {
        if (std::isnan(path[i].first) || std::isnan(path[i - 1].first)) continue;
        sq += std::pow(path[i].first - path[i - 1].first, 2) + std::pow(path[i].second - path[i - 1].second, 2);
        ++n;
    }
    return n ? std::sqrt(sq / n) : NAN;
}

// Does the w x h picture, corrected by c and zoomed by z (as clipPlacement applies it), cover the whole frame?
bool covers(const FrameCorrection& c, double z, double w, double h) {
    for (const double qx : {-w / 2, w / 2})
        for (const double qy : {-h / 2, h / 2}) {
            const double ux = qx / z - c.dx * w, uy = qy / z - c.dy * h;
            const double vx = std::cos(c.da) * ux + std::sin(c.da) * uy, vy = -std::sin(c.da) * ux + std::cos(c.da) * uy;
            if (std::fabs(vx) > w / 2 + 1e-6 || std::fabs(vy) > h / 2 + 1e-6) return false;
        }
    return true;
}

// The camera motion of the shaky clip as the editor's analysis measures it (frames decoded 960 px wide).
const std::vector<CameraMotion>& shakyMotion() {
    static const std::vector<CameraMotion> motion = [] {
        VideoDecoder::Options o;
        o.maxWidth = 960;
        o.maxHeight = 540;
        VideoDecoder dec(media("M_shaky_1080p30_5s.mp4"), o);
        vision::MotionEstimator est;
        std::vector<CameraMotion> m;
        for (int f = 0; f < 150; ++f) {
            const Image& img = dec.frameAt(Rational(f, 30)).image;
            const vision::CameraStep s = est.next(img);
            m.push_back({s.dx / img.width, s.dy / img.height, s.da, s.valid});
        }
        return m;
    }();
    return motion;
}

}  // namespace

// A pan of 3 px per frame plus shake: the plan keeps the pan, removes the shake, covers the frame on every frame with
// the zoom it reports, and does nothing at strength 0.
TEST(Stabilization, PlanKeepsThePanRemovesTheShakeAndCoversTheFrame) {
    const int n = 150;
    std::mt19937 rng(7);
    std::normal_distribution<double> shake(0.0, 6.0);
    std::vector<double> x(n), y(n);
    for (int i = 0; i < n; ++i) {
        x[i] = 3.0 * i + shake(rng);
        y[i] = shake(rng);
    }
    Stabilization st;
    st.width = 1920;
    st.height = 1080;
    for (int i = 0; i < n; ++i)
        st.motion.push_back({i ? (x[i] - x[i - 1]) / 1920 : 0.0, i ? (y[i] - y[i - 1]) / 1080 : 0.0, 0.0, i > 0});
    planStabilization(st);
    ASSERT_EQ(st.corrections.size(), size_t(n));
    double inSq = 0, outSq = 0;
    for (int i = 1; i < n; ++i) {   // frame-to-frame motion minus the pan
        const double ox = x[i] + st.corrections[i].dx * 1920 - x[i - 1] - st.corrections[i - 1].dx * 1920;
        const double oy = y[i] + st.corrections[i].dy * 1080 - y[i - 1] - st.corrections[i - 1].dy * 1080;
        inSq += std::pow(x[i] - x[i - 1] - 3.0, 2) + std::pow(y[i] - y[i - 1], 2);
        outSq += std::pow(ox - 3.0, 2) + std::pow(oy, 2);
    }
    const double in = std::sqrt(inSq / (n - 1)), out = std::sqrt(outSq / (n - 1));
    std::printf("  pan + shake: residual shake %.2f px -> %.2f px per frame, zoom %.3f\n", in, out, st.zoom);
    EXPECT_LT(out, 0.1 * in);
    EXPECT_GT(st.zoom, 1.0);
    EXPECT_LE(st.zoom, kMaxStabilizeZoom);
    for (int i = 0; i < n; ++i) EXPECT_TRUE(covers(st.corrections[i], st.zoom, 1920, 1080)) << "frame " << i;

    Stabilization off = st;   // negative controls
    off.strength = 0;
    planStabilization(off);
    for (const auto& c : off.corrections) EXPECT_EQ(c, FrameCorrection{});
    EXPECT_EQ(off.zoom, 1.0);
    Stabilization noZoom = st;
    noZoom.autoZoom = false;
    planStabilization(noZoom);
    EXPECT_EQ(noZoom.zoom, 1.0);
    EXPECT_EQ(noZoom.corrections, st.corrections);   // the zoom limit was not reached: same corrections
    EXPECT_EQ(st.limitedShare, 0.0);
}

// Shake larger than the zoom limit allows: the zoom stays at 125 %, every frame is still covered, and the weakening
// is eased (no correction jumps between neighbouring frames beyond the shake itself).
// Shake larger than Max Zoom allows (the longest band is +-192 px at 125 %): the zoom stays at the limit, every frame is
// still covered, and the corrected path is far steadier than the camera's - and steadier than scaling the correction
// down until it fits.
TEST(Stabilization, MaxZoomKeepsTheCorrectedPathInsideTheFrame) {
    const int n = 120;
    Stabilization st;
    st.width = 1920;
    st.height = 1080;
    st.level = StabilizeLevel::Strong;
    st.maxZoom = kMaxStabilizeZoom;
    std::vector<double> x(n);
    for (int i = 0; i < n; ++i) x[i] = (i % 2 ? 1 : -1) * (i >= 40 && i < 80 ? 300.0 : 20.0);   // violent middle part
    for (int i = 0; i < n; ++i) st.motion.push_back({i ? (x[i] - x[i - 1]) / 1920 : 0.0, 0.0, 0.0, i > 0});
    planStabilization(st);
    auto rmsStepOf = [&](const Stabilization& plan) {   // the corrected content position, zoomed about the centre
        double sq = 0;
        for (int i = 1; i < n; ++i) {
            const double a = (x[i] + plan.corrections[i].dx * 1920) * plan.zoom, b = (x[i - 1] + plan.corrections[i - 1].dx * 1920) * plan.zoom;
            sq += (a - b) * (a - b);
        }
        return std::sqrt(sq / (n - 1));
    };
    Stabilization none = st;
    none.strength = 0;
    planStabilization(none);
    std::printf("  violent shake: zoom %.3f, limited on %.0f %% of the frames, RMS step %.1f px (camera %.1f px)\n", st.zoom,
                st.limitedShare * 100, rmsStepOf(st), rmsStepOf(none));
    EXPECT_NEAR(st.zoom, kMaxStabilizeZoom, 1e-6);
    EXPECT_GT(st.limitedShare, 0.2);
    for (int i = 0; i < n; ++i) EXPECT_TRUE(covers(st.corrections[i], st.zoom, 1920, 1080)) << "frame " << i;
    EXPECT_LT(rmsStepOf(st), 0.5 * rmsStepOf(none));
}

// EXP-014 at Filmora's crop: Filmora zooms about 2-3 % and leaves 3.77 px (Normal). With Max Zoom 103 % MeerCut keeps the
// corrected path inside the frame; measured against the generator's jitter (plan level, no export).
TEST(Stabilization, MaxZoomTradesCropForSteadinessBetterThanStrength) {
    const auto jit = jitter();
    ASSERT_EQ(jit.size(), size_t(150));
    Stabilization st;
    st.width = 1920;
    st.height = 1080;
    st.motion = shakyMotion();
    auto residual = [&](const Stabilization& plan) {   // the marker: content at -jitter, corrected, zoomed
        double sq = 0;
        for (size_t i = 1; i < 150; ++i) {
            const double ax = (-jit[i].first + plan.corrections[i].dx * 1920) * plan.zoom;
            const double ay = (-jit[i].second + plan.corrections[i].dy * 1080) * plan.zoom;
            const double bx = (-jit[i - 1].first + plan.corrections[i - 1].dx * 1920) * plan.zoom;
            const double by = (-jit[i - 1].second + plan.corrections[i - 1].dy * 1080) * plan.zoom;
            sq += (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
        }
        return std::sqrt(sq / 149);
    };
    st.maxZoom = kMaxStabilizeZoom;
    planStabilization(st);
    const double full = residual(st), fullZoom = st.zoom;
    st.maxZoom = 1.03;
    planStabilization(st);
    const double banded = residual(st), bandedZoom = st.zoom;
    for (const auto& c : st.corrections) EXPECT_TRUE(covers(c, st.zoom, 1920, 1080));
    Stabilization weak = st;   // the other way to crop less: scale the whole correction down
    weak.maxZoom = kMaxStabilizeZoom;
    weak.strength = 0.5;
    planStabilization(weak);
    const double halfStrength = residual(weak);
    std::printf("  Normal: full %.2f px at zoom %.3f; Max Zoom 103 %%: %.2f px at %.3f (limited on %.0f %% of the frames); "
                "strength 50 %%: %.2f px at %.3f; Filmora: 3.77 px at about 1.02-1.03\n",
                full, fullZoom, banded, bandedZoom, st.limitedShare * 100, halfStrength, weak.zoom);
    EXPECT_LE(bandedZoom, 1.03 + 1e-6);
    EXPECT_LT(banded, 3.77);
    EXPECT_LT(banded, 0.5 * halfStrength);
}

// The measured camera motion of the shaky clip against the generator's jitter.
TEST(Stabilization, MeasuresTheCameraMotionOfTheShakyClip) {
    const auto jit = jitter();
    ASSERT_EQ(jit.size(), size_t(150));
    const auto& m = shakyMotion();
    double sq = 0;
    int valid = 0;
    for (size_t i = 1; i < m.size(); ++i) {
        if (!m[i].valid) continue;
        ++valid;
        // the picture content moves by -(jitter change)
        const double ex = m[i].dx * 1920 + (jit[i].first - jit[i - 1].first);
        const double ey = m[i].dy * 1080 + (jit[i].second - jit[i - 1].second);
        sq += ex * ex + ey * ey;
    }
    const double rms = std::sqrt(sq / std::max(1, valid));
    std::printf("  camera motion vs ground truth: RMS %.2f px over %d measured steps\n", rms, valid);
    EXPECT_GE(valid, 145);
    EXPECT_LT(rms, 1.0);
}

// EXP-014 end to end: exports stabilised at Normal and Strong, the marker's frame-to-frame motion measured the way
// Stage 1 measured Filmora. Negative control: strength 0 leaves the input shake.
TEST(Stabilization, Exp014ExportsAreSteadierThanFilmora) {
    Project p;
    p.sources = {MediaSource{"M", media("M_shaky_1080p30_5s.mp4").string(), Rational(5), FrameRate::fps30(), 1920, 1080, true,
                             false}};
    Sequence s;
    s.id = "s";
    p.sequences.push_back(s);
    UndoStack undo(p);
    undo.execute(edit::addMedia(p, "s", "M", Rational(0)));
    Clip& clip = p.sequences[0].tracks[0].clips[0];
    const double input = rmsStep(markerPath(media("M_shaky_1080p30_5s.mp4"), 150));
    auto exportWith = [&](StabilizeLevel level, double strength, const char* name, double* zoom, double maxZoom = 1.10) {
        Stabilization st;
        st.level = level;
        st.strength = strength;
        st.maxZoom = maxZoom;
        st.frameRate = Rational(30);
        st.width = 1920;
        st.height = 1080;
        st.motion = shakyMotion();
        planStabilization(st);
        *zoom = st.zoom;
        clip.stabilization = st;
        const auto out = std::filesystem::temp_directory_path() / name;
        RenderOptions opt;
        opt.encoder.preset = "ultrafast";
        const RenderReport r = renderProject(p, "", out, opt);
        EXPECT_EQ(r.framesWritten, 150);
        return rmsStep(markerPath(out, 150));
    };
    double zNormal = 0, zStrong = 0, zOff = 0;
    const double normal = exportWith(StabilizeLevel::Normal, 1.0, "fa_stab_normal.mp4", &zNormal);
    const double strong = exportWith(StabilizeLevel::Strong, 1.0, "fa_stab_strong.mp4", &zStrong);
    const double off = exportWith(StabilizeLevel::Normal, 0.0, "fa_stab_off.mp4", &zOff);
    double zHalf = 0;   // the crop trade-off: Filmora zooms about 2-3 % (EXP-014), MeerCut's full correction more
    const double half = exportWith(StabilizeLevel::Normal, 1.0, "fa_stab_103.mp4", &zHalf, 1.03);
    std::printf("  EXP-014 marker RMS step: input %.2f px, Normal %.2f px (zoom %.3f), Strong %.2f px (zoom %.3f), "
                "Normal with Max Zoom 103 %% %.2f px (zoom %.3f), strength 0 %.2f px; Filmora 16: input 13.27, Normal 3.77, "
                "Strong 1.91\n",
                input, normal, zNormal, strong, zStrong, half, zHalf, off);
    EXPECT_LE(zHalf, 1.03 + 1e-6);
    EXPECT_LT(half, 3.77);   // steadier than Filmora's Normal at about the same crop
    EXPECT_NEAR(input, 13.27, 0.3);   // the measurement agrees with Stage 1's on the same file
    EXPECT_LT(normal, 3.77);
    EXPECT_LT(strong, 1.91);
    EXPECT_GT(off, 12.5);   // nothing was stabilised: the shake is still there
    EXPECT_EQ(zOff, 1.0);
}
