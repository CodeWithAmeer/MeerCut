// M7 checkpoint B: Background Removal - the matte cache, the refinement of a matte, the real RVM analysis of a clip
// (temporal stability against frame-by-frame matting), the renderer's use of the mattes (preview = export, edits need
// no re-analysis, a missing matte is never exported silently) and the project format.
#include "fa/ai/matting.h"
#include "fa/ai/models.h"
#include "fa/media/media.h"
#include "fa/model/edit_ops.h"
#include "fa/model/serialize.h"
#include "fa/render/matte_cache.h"
#include "fa/render/renderer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <numbers>
#include <random>

using namespace fa;

namespace {

std::filesystem::path models() { return std::filesystem::path(FA_MODELS_DIR); }
std::filesystem::path tos() { return std::filesystem::path(FA_TESTMEDIA_DIR) / "ai" / "tears_of_steel_60s.mov"; }

std::filesystem::path tempDir(const char* name) {
    const auto d = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

// The analysis cache and the models for this test binary (restored by the destructor).
struct AiEnv {
    std::filesystem::path cache;
    explicit AiEnv(const char* name) : cache(tempDir(name)) {
        _wputenv_s(L"MEERCUT_ANALYSIS_CACHE", cache.wstring().c_str());
        ai::setModelsDirectoryOverride(models());
    }
    ~AiEnv() {
        _wputenv_s(L"MEERCUT_ANALYSIS_CACHE", L"");
        ai::setModelsDirectoryOverride({});
        std::error_code ec;
        std::filesystem::remove_all(cache, ec);
    }
};

bool rvmReady() {
    return ai::modelStatus(*ai::findModel("rvm-mobilenetv3"), false, models()) == ai::ModelStatus::Installed &&
           std::filesystem::exists(tos());
}

AlphaImage disc(int w, int h, double cx, double cy, double r) {
    AlphaImage a{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) a.a[static_cast<size_t>(y) * w + x] = std::hypot(x - cx, y - cy) <= r ? 255 : 0;
    return a;
}

double meanAlpha(const AlphaImage& a, int x0, int y0, int x1, int y1) {
    double s = 0;
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x, ++n) s += a.a[static_cast<size_t>(y) * a.width + x] / 255.0;
    return n ? s / n : 0;
}

int coverage(const AlphaImage& a) {
    int n = 0;
    for (uint8_t v : a.a) n += v >= 128;
    return n;
}

// The Tears of Steel bridge shot: frames 240 .. 299 of the excerpt (10 .. 12.5 s), two people standing and talking.
Project bridgeProject(int64_t firstFrame, int frames) {
    Project p;
    MediaSource s;
    s.id = "tos";
    s.path = tos().string();
    s.duration = Rational(60);
    s.frameRate = FrameRate(24, 1);
    s.width = 1280;
    s.height = 534;
    s.hasAudio = false;
    p.sources.push_back(s);
    Sequence q;
    q.id = "seq";
    q.width = 640;
    q.height = 268;
    q.frameRate = FrameRate(24, 1);
    Track t{"v1", TrackKind::Video, "Video 1"};
    Clip c;
    c.id = "c";
    c.sourceId = "tos";
    c.sourceIn = Rational(firstFrame, 24);
    c.duration = Rational(frames, 24);
    c.aiCutout.enabled = true;
    t.clips.push_back(c);
    q.tracks.push_back(t);
    p.sequences.push_back(q);
    return p;
}

}  // namespace

TEST(MatteCache, StoreRoundTripPreviewMarksAndEviction) {
    const auto root = tempDir("fa_matte_store");
    const MatteSetKey key{"media|1|2|3", "rvm-mobilenetv3", std::string(64, 'a'), 1};
    MatteStore store(key, root);
    const AlphaImage m = disc(64, 48, 30, 20, 12);
    EXPECT_FALSE(store.has(5));
    store.write(5, m);
    AlphaImage back;
    ASSERT_TRUE(store.read(5, back));
    EXPECT_EQ(back.width, 64);
    EXPECT_EQ(back.a, m.a);
    // a single-frame preview is drawn but not counted as analysed; analysing it replaces the mark
    store.writePreview(6, m);
    EXPECT_TRUE(store.has(6));
    EXPECT_FALSE(store.hasAnalysed(6));
    EXPECT_EQ(store.count(0, 10), 1);
    store.write(6, m);
    EXPECT_TRUE(store.hasAnalysed(6));
    EXPECT_EQ(store.count(0, 10), 2);
    // a different model version is a different set
    MatteSetKey other = key;
    other.modelSha = std::string(64, 'b');
    EXPECT_NE(other.hash(), key.hash());
    EXPECT_FALSE(MatteStore(other, root).has(5));
    // eviction: least recently used sets go first, never the one being used
    MatteStore second(other, root);
    for (int f = 0; f < 4; ++f) second.write(f, disc(256, 256, 128, 128, 60));
    store.touch();
    const uint64_t total = matteCacheBytes(root);
    trimMatteCache(total - 1, second.folder(), root);   // over the limit: the set not kept goes
    EXPECT_FALSE(std::filesystem::exists(store.folder()));
    EXPECT_TRUE(std::filesystem::exists(second.folder()));
    std::filesystem::remove_all(root);
}

TEST(MatteCache, RefinementThresholdEdgeFeatherAndFlickerSmoothing) {
    const AlphaImage m = disc(200, 200, 100, 100, 50);
    AiCutout s;
    s.smoothing = 0;
    s.spill = 0;
    // edge: grow / shrink by n px changes the radius by n (area pi r^2)
    for (double e : {-6.0, 6.0}) {
        s.edge = e;
        const double r = std::sqrt(coverage(refineMatte(m, nullptr, nullptr, s)) / std::numbers::pi);
        EXPECT_NEAR(r, 50 + e, 1.5) << "edge " << e;
    }
    s.edge = 0;
    // feather: a soft ramp across the edge, the area (half-coverage) kept
    s.feather = 4;
    const AlphaImage f = refineMatte(m, nullptr, nullptr, s);
    EXPECT_NEAR(std::sqrt(coverage(f) / std::numbers::pi), 50, 1.5);
    int soft = 0;
    for (uint8_t v : f.a) soft += v > 20 && v < 235;
    EXPECT_GT(soft, 2 * std::numbers::pi * 50 * 4);   // a band several px wide
    s.feather = 0;
    // threshold: faint haze (alpha 0.2) disappears, the subject stays
    AlphaImage haze = m;
    for (auto& v : haze.a) v = v ? v : 51;
    s.threshold = 0.3;
    const AlphaImage t = refineMatte(haze, nullptr, nullptr, s);
    EXPECT_LT(meanAlpha(t, 0, 0, 30, 30), 0.01);
    EXPECT_GT(meanAlpha(t, 90, 90, 110, 110), 0.99);
    s.threshold = 0;
    // flicker smoothing: an edge pixel that shimmers (0.55 / 0.45 / 0.55) is pulled towards its neighbours...
    s.smoothing = 1.0;
    AlphaImage a{1, 1, {140}}, p{1, 1, {115}}, n{1, 1, {115}};
    const AlphaImage sm = refineMatte(a, &p, &n, s);
    EXPECT_LT(std::abs(sm.a[0] - 115), std::abs(a.a[0] - 115));
    // ...but movement (a pixel that changes from background to subject) is not smeared: no ghost
    AlphaImage moving{1, 1, {255}}, before{1, 1, {0}}, after{1, 1, {255}};
    EXPECT_EQ(refineMatte(moving, &before, &after, s).a[0], 255);
    AlphaImage gone{1, 1, {0}};
    EXPECT_EQ(refineMatte(gone, &moving, &before, s).a[0], 0);
}

TEST(MatteCache, SpillCleanupRecoversTheSubjectColourOnSoftEdges) {
    // a subject (warm grey) over green, with a 12 px soft edge composited exactly: I = a F + (1 - a) B
    const int w = 160, h = 120;
    const Rgba8 subject{180, 150, 140, 255}, green{20, 200, 40, 255};
    Image pic(w, h);
    AlphaImage m{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double a = std::clamp((x - 74.0) / 12.0, 0.0, 1.0);
            const size_t i = static_cast<size_t>(y) * w + x;
            m.a[i] = static_cast<uint8_t>(std::lround(a * 255));
            uint8_t* px = &pic.pixels[i * 4];
            px[0] = static_cast<uint8_t>(std::lround(a * subject.r + (1 - a) * green.r));
            px[1] = static_cast<uint8_t>(std::lround(a * subject.g + (1 - a) * green.g));
            px[2] = static_cast<uint8_t>(std::lround(a * subject.b + (1 - a) * green.b));
            px[3] = 255;
        }
    auto edgeError = [&](double spill) {
        Image out = pic;
        AiCutout s;
        s.smoothing = 0;
        s.spill = spill;
        applyCutout(out, m, nullptr, nullptr, s);
        double err = 0;
        int n = 0;
        for (int y = 20; y < 100; ++y)
            for (int x = 78; x < 84; ++x, ++n) {   // the soft edge (alpha 0.3 .. 0.8)
                const uint8_t* px = &out.pixels[(static_cast<size_t>(y) * w + x) * 4];
                err += std::abs(px[1] - subject.g);   // green channel: where the spill shows
            }
        return err / n;
    };
    const double without = edgeError(0.0), with = edgeError(1.0);
    std::printf("soft-edge green error: %.1f levels without spill cleanup, %.1f with\n", without, with);
    EXPECT_GT(without, 20.0);   // negative control: the green background bleeds into the edge
    EXPECT_LT(with, 6.0);
    // alpha goes into the picture; the colour fill replaces the background
    Image filled = pic;
    AiCutout s;
    s.background = Rgba8{0, 0, 255, 255};
    s.smoothing = 0;
    applyCutout(filled, m, nullptr, nullptr, s);
    EXPECT_EQ(filled.pixels[(60 * w + 10) * 4 + 2], 255);   // background area: blue
    EXPECT_EQ(filled.pixels[(60 * w + 150) * 4 + 0], subject.r);
}

// Real analysis of a moving shot with RVM: frames analysed in order, the people matted on every frame, and the
// recurrent model steadier than the same model run frame by frame (the negative control).
TEST(AiCutout, RvmAnalysisOfRealFootageIsTemporallyStable) {
    if (!rvmReady()) GTEST_SKIP() << "RVM or testmedia/ai missing";
    AiEnv env("fa_cutout_cache_a");
    AiCutout cut;
    cut.enabled = true;
    const ai::MattingJob job{tos(), 240, 287, cut, ai::Accelerator::Auto};
    const ai::MattingReport r = ai::analyseMattes(job);
    std::printf("RVM: %lld frames in %.2f s (%.1f ms/frame) on %s\n", static_cast<long long>(r.framesAnalysed), r.seconds,
                1000 * r.seconds / std::max<int64_t>(1, r.framesAnalysed), r.provider.c_str());
    EXPECT_EQ(r.framesAnalysed, 48);
    const MatteStore store(ai::cutoutKey(tos(), cut));
    EXPECT_EQ(store.count(240, 287), 48);
    // a second run reuses everything
    EXPECT_EQ(ai::analyseMattes(job).framesCached, 48);
    // flicker: mean frame-to-frame change of the matte in the background (top 30 %) and on the people
    VideoDecoder dec(tos());
    ai::MattingSession single(cut, ai::Accelerator::Auto);   // the same model without its memory: reset every frame
    double recurrentFlicker = 0, frameFlicker = 0, people = 0;
    AlphaImage prevR, prevF;
    for (int64_t f = 240; f <= 287; ++f) {
        AlphaImage a;
        ASSERT_TRUE(store.read(f, a));
        single.reset();
        const AlphaImage one = single.run(dec.frameAt(Time(f) / Rational(24)).image);
        people += (meanAlpha(a, 390, 200, 450, 330) + meanAlpha(a, 770, 200, 860, 330)) / 2;
        if (!prevR.empty()) {
            double dr = 0, df = 0;
            for (size_t i = 0; i < a.a.size(); ++i) {
                dr += std::abs(a.a[i] - prevR.a[i]);
                df += std::abs(one.a[i] - prevF.a[i]);
            }
            recurrentFlicker += dr / a.a.size() / 255.0;
            frameFlicker += df / a.a.size() / 255.0;
        }
        prevR = a;
        prevF = one;
    }
    people /= 48;
    recurrentFlicker /= 47;
    frameFlicker /= 47;
    std::printf("people alpha %.3f; mean frame-to-frame matte change: recurrent %.5f, frame by frame %.5f\n", people,
                recurrentFlicker, frameFlicker);
    EXPECT_GT(people, 0.85);
    EXPECT_LT(recurrentFlicker, frameFlicker);
}

// Export with Background Removal: the mattes are used (people kept, background = the fill colour), the preview frame
// is the export frame, a missing matte is refused, and edits (trim, move, split) need no new analysis.
TEST(AiCutout, ExportUsesTheMattesAndEditsNeedNoReanalysis) {
    if (!rvmReady()) GTEST_SKIP() << "RVM or testmedia/ai missing";
    AiEnv env("fa_cutout_cache_b");
    Project p = bridgeProject(240, 24);
    Clip& c = p.sequences[0].tracks[0].clips[0];
    c.aiCutout.background = Rgba8{0, 0, 255, 255};
    // not analysed: the renderer refuses (no silent export without the cutout)
    const auto out = std::filesystem::temp_directory_path() / "fa_cutout_export.mp4";
    std::filesystem::remove(out);   // a refused export must not leave a file behind
    {
        Project q = p;
        q.sequences[0].tracks[0].clips[0].aiCutout.modelSha = ai::findModel("rvm-mobilenetv3")->files.front().sha256;
        RenderOptions o;
        o.includeAudio = false;
        EXPECT_THROW(renderProject(q, "", out, o), RenderError);
        EXPECT_FALSE(std::filesystem::exists(out));
    }
    // prepare (what the export window and fa-render do) analyses the 24 frames and records the model
    const ai::CutoutPreparation prep = ai::prepareCutouts(p, "", "");
    EXPECT_EQ(prep.clips, 1);
    EXPECT_EQ(prep.framesAnalysed, 24);
    EXPECT_EQ(p.sequences[0].tracks[0].clips[0].aiCutout.modelSha.size(), 64u);
    RenderOptions o;
    o.includeAudio = false;
    o.encoder.crf = 0;
    const RenderReport rep = renderProject(p, "", out, o);
    EXPECT_EQ(rep.framesWritten, 24);
    // the exported frame: blue background, the people kept, and the preview path draws the same frame
    Image exported;
    {
        VideoDecoder dec(out);   // closed again before the file is removed
        exported = dec.frameAt(Time(12, 24)).image;
    }
    DecoderPool pool("");
    const auto backend = makeBackend(BackendChoice::Cpu);
    const Image preview = renderFrame(p.sequences[0], 12, p, pool, *backend);
    auto at = [](const Image& im, double fx, double fy, int ch) {
        const int x = static_cast<int>(fx * im.width), y = static_cast<int>(fy * im.height);
        return static_cast<int>(im.pixels[(static_cast<size_t>(y) * im.width + x) * 4 + ch]);
    };
    EXPECT_GT(at(exported, 0.10, 0.15, 2), 200);   // sky -> blue fill
    EXPECT_LT(at(exported, 0.10, 0.15, 0), 40);
    EXPECT_LT(at(exported, 0.33, 0.50, 2), 200);   // the person on the left keeps their colours
    double diff = 0;
    for (size_t i = 0; i < preview.pixels.size(); i += 4)
        for (int k = 0; k < 3; ++k) diff += std::abs(int(preview.pixels[i + k]) - int(exported.pixels[i + k]));
    diff /= preview.pixels.size() / 4 * 3;
    std::printf("preview vs export frame: mean difference %.2f levels\n", diff);
    EXPECT_LT(diff, 3.0);   // the encoder's YUV 4:2:0 conversion only
    // edits: trim the head, move, split - every frame drawn still has its matte (no new analysis)
    Project e = p;
    UndoStack st(e);
    st.execute(edit::trimClip(e, "seq", "c", ClipEdge::Head, Rational(6, 24)));
    st.execute(edit::moveClip(e, "seq", "c", Rational(2), "v1"));
    st.execute(edit::splitAt(e, "seq", Rational(2) + Rational(8, 24), {"c"}));
    DecoderPool edited("");
    for (int64_t f = 48; f < 48 + 18; ++f) renderFrame(e.sequences[0], f, e, edited, *backend);
    EXPECT_TRUE(edited.clipsMissingMattes().empty());
    std::filesystem::remove(out);
}

TEST(AiCutout, ProjectFormatRoundTripAndOlderVersions) {
    Project p = bridgeProject(0, 24);
    AiCutout& a = p.sequences[0].tracks[0].clips[0].aiCutout;
    a.model = "birefnet-general-tiny";
    a.quality = 2;
    a.modelSha = std::string(64, 'c');
    a.threshold = 0.2;
    a.edge = -2;
    a.feather = 1.5;
    a.spill = 0.8;
    a.smoothing = 0.3;
    a.background = Rgba8{10, 20, 30, 255};
    const std::string json = projectToJsonString(p);
    const Project back = projectFromJsonString(json);
    EXPECT_EQ(back.sequences[0].tracks[0].clips[0].aiCutout, a);
    // written with the current format version (7 introduced the field)
    const std::string current = "\"version\": " + std::to_string(Project::kFormatVersion);
    EXPECT_GE(Project::kFormatVersion, 7);
    EXPECT_NE(json.find(current), std::string::npos);
    // a version-6 file without the field loads with Background Removal off
    std::string v6 = projectToJsonString(bridgeProject(0, 24));
    v6.replace(v6.find(current), current.size(), "\"version\": 6");
    const auto pos = v6.find("\"aiCutout\"");
    ASSERT_NE(pos, std::string::npos);
    Project plain = bridgeProject(0, 24);
    plain.sequences[0].tracks[0].clips[0].aiCutout = AiCutout{};
    std::string old = projectToJsonString(plain);
    old.replace(old.find(current), current.size(), "\"version\": 6");
    EXPECT_EQ(old.find("aiCutout"), std::string::npos);
    EXPECT_TRUE(projectFromJsonString(old).sequences[0].tracks[0].clips[0].aiCutout.isDefault());
    // invalid settings are rejected by validation
    Project bad = bridgeProject(0, 24);
    bad.sequences[0].tracks[0].clips[0].aiCutout.threshold = 2.0;
    EXPECT_TRUE(hasErrors(validate(bad)));
}

// Cancel keeps every frame already written; the next analysis computes only the rest. Frames analysed in two separate
// runs (each run starting with RVM's 30-frame warm-up) match one continuous analysis: the recurrent state is rebuilt
// at every gap, never carried over from unrelated frames.
TEST(AiCutout, CancelResumeAndRecurrentStateAcrossRuns) {
    if (!rvmReady()) GTEST_SKIP() << "RVM or testmedia/ai missing";
    AiEnv env("fa_cutout_cache_c");
    AiCutout cut;
    cut.enabled = true;
    // cancel after 10 of 48 frames
    std::atomic<bool> cancel{false};
    const ai::MattingJob job{tos(), 240, 287, cut, ai::Accelerator::Auto};
    const ai::MattingReport first = ai::analyseMattes(job, [&](int64_t done, int64_t) {
        if (done >= 10) cancel = true;
    }, &cancel);
    EXPECT_GE(first.framesAnalysed, 10);
    EXPECT_LT(first.framesAnalysed, 48);
    const MatteStore store(ai::cutoutKey(tos(), cut));
    EXPECT_EQ(store.count(240, 287), first.framesAnalysed);
    const ai::MattingReport rest = ai::analyseMattes(job);
    EXPECT_EQ(rest.framesCached, first.framesAnalysed);
    EXPECT_EQ(rest.framesAnalysed, 48 - first.framesAnalysed);
    EXPECT_EQ(store.count(240, 287), 48);
    // the same 48 frames analysed in one run (another cache) match: mean difference per frame
    AiEnv other("fa_cutout_cache_d");
    ASSERT_EQ(ai::analyseMattes(job).framesAnalysed, 48);
    const MatteStore single(ai::cutoutKey(tos(), cut), other.cache);
    double worst = 0;
    for (int64_t f = 240; f <= 287; ++f) {
        AlphaImage a, b;
        ASSERT_TRUE(MatteStore(ai::cutoutKey(tos(), cut), env.cache).read(f, a));
        ASSERT_TRUE(single.read(f, b));
        double d = 0;
        for (size_t i = 0; i < a.a.size(); ++i) d += std::abs(a.a[i] - b.a[i]);
        worst = std::max(worst, d / a.a.size());
    }
    std::printf("two runs (cancelled after %lld frames) vs one run: worst mean matte difference %.3f levels\n",
                static_cast<long long>(first.framesAnalysed), worst);
    EXPECT_LT(worst, 1.0);
}

// The cache limit is applied by the analysis itself (production code): with a tiny limit, analysing a second set
// removes the least recently used one, never the set just written.
TEST(AiCutout, AnalysisEnforcesTheCacheLimit) {
    if (!rvmReady()) GTEST_SKIP() << "RVM or testmedia/ai missing";
    _wputenv_s(L"MEERCUT_ANALYSIS_CACHE_MB", L"");
    EXPECT_EQ(analysisCacheLimitBytes(), 8ull << 30);   // the default
    AiEnv env("fa_cutout_cache_e");
    AiCutout fast, standard;
    fast.enabled = standard.enabled = true;
    fast.quality = 0;
    standard.quality = 1;
    ASSERT_EQ(ai::analyseMattes({tos(), 240, 245, fast, ai::Accelerator::Auto}).framesAnalysed, 6);
    const MatteStore a(ai::cutoutKey(tos(), fast));
    ASSERT_TRUE(std::filesystem::exists(a.folder()));
    // a limit of 1.5 times the first set: the second set (about as large) pushes the total over it
    const uint64_t one = matteCacheBytes(env.cache);
    ASSERT_GT(one, 0u);
    const std::wstring mb = std::to_wstring(1.5 * one / 1048576.0);
    _wputenv_s(L"MEERCUT_ANALYSIS_CACHE_MB", mb.c_str());
    EXPECT_NEAR(static_cast<double>(analysisCacheLimitBytes()), 1.5 * one, 2.0);
    ASSERT_EQ(ai::analyseMattes({tos(), 240, 245, standard, ai::Accelerator::Auto}).framesAnalysed, 6);
    const MatteStore b(ai::cutoutKey(tos(), standard));
    EXPECT_FALSE(std::filesystem::exists(a.folder()));   // evicted
    EXPECT_TRUE(std::filesystem::exists(b.folder()));    // the set just written is kept
    EXPECT_LE(matteCacheBytes(env.cache), analysisCacheLimitBytes());
    _wputenv_s(L"MEERCUT_ANALYSIS_CACHE_MB", L"");
}

// Negative controls: a missing model and a corrupt model file fail with a clear error and write nothing; on a GPU
// with too little memory for BiRefNet the analysis stops with GpuMemoryError instead of moving to the CPU by itself.
TEST(AiCutout, MissingCorruptAndOutOfMemoryModelsFailCleanly) {
    if (!std::filesystem::exists(tos())) GTEST_SKIP() << "testmedia/ai missing";
    const auto cache = tempDir("fa_cutout_cache_f");
    _wputenv_s(L"MEERCUT_ANALYSIS_CACHE", cache.wstring().c_str());
    AiCutout cut;
    cut.enabled = true;
    const auto empty = tempDir("fa_cutout_nomodels");
    ai::setModelsDirectoryOverride(empty);
    try {
        ai::analyseMattes({tos(), 240, 241, cut, ai::Accelerator::Cpu});
        ADD_FAILURE() << "analysed without a model";
    } catch (const ai::ModelError& e) {
        EXPECT_NE(std::string(e.what()).find("Model Manager"), std::string::npos) << e.what();
    }
    // a file of the right size with wrong content (not checked by hash at load time): the runtime refuses it
    const ai::ModelInfo& rvm = *ai::findModel("rvm-mobilenetv3");
    std::filesystem::create_directories(empty / rvm.id);
    {
        std::ofstream bad(empty / rvm.id / std::filesystem::path(rvm.files[0].path).filename(), std::ios::binary);
        std::string junk(static_cast<size_t>(rvm.files[0].bytes), 'x');
        bad.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }
    EXPECT_THROW(ai::analyseMattes({tos(), 240, 241, cut, ai::Accelerator::Cpu}), std::exception);
    EXPECT_FALSE(std::filesystem::exists(cache / "mattes") && !std::filesystem::is_empty(cache / "mattes"));
    // BiRefNet on a GPU of 4 GB or less: GpuMemoryError, nothing written, no silent CPU run
    ai::setModelsDirectoryOverride(models());
    AiCutout general;
    general.enabled = true;
    general.model = "birefnet-general-tiny";
    const auto& caps = ai::aiCapabilities();
    const int g = ai::preferredDirectMlGpu();
    const bool small = g >= 0 && caps.directMl && caps.gpus[static_cast<size_t>(g)].memoryMb <= 4500;
    EXPECT_EQ(ai::cutoutFitsGpu(general), !small && g >= 0);
    EXPECT_TRUE(ai::cutoutFitsGpu(cut));
    if (small && ai::modelStatus(*ai::findModel(general.model), false, models()) == ai::ModelStatus::Installed) {
        const auto t0 = std::chrono::steady_clock::now();
        EXPECT_THROW(ai::analyseMattes({tos(), 240, 240, general, ai::Accelerator::Gpu}), ai::GpuMemoryError);
        std::printf("BiRefNet on %s: out of memory reported after %.1f s\n", caps.gpus[static_cast<size_t>(g)].name.c_str(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        EXPECT_EQ(MatteStore(ai::cutoutKey(tos(), general), cache).count(240, 240), 0);
    }
    ai::setModelsDirectoryOverride({});
    _wputenv_s(L"MEERCUT_ANALYSIS_CACHE", L"");
    std::filesystem::remove_all(cache);
    std::filesystem::remove_all(empty);
}
