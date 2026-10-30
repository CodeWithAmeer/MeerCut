// M4 effects: registry/schema, Filmora-fitted colour models, stack semantics (order, strength, masks), blur,
// adjustment layers, serialization, and CPU/GPU parity of every effect with GPU passes.
#include "fa/model/keyframes.h"
#include "fa/model/serialize.h"
#include "fa/render/backend.h"
#include "fa/render/curves.h"
#include "fa/render/effects.h"
#include "fa/render/renderer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

using namespace fa;

namespace {

// Horizontal grey ramp 0..255 (one column per level) over `h` rows.
Image greyRamp(int h = 4) {
    Image img(256, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < 256; ++x) img.set(x, y, Rgba8{uint8_t(x), uint8_t(x), uint8_t(x), 255});
    return img;
}

// Seeded random colour patches with a few transparent / half-transparent pixels.
Image colourful(int w, int h, unsigned seed = 7) {
    Image img(w, h);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> d(0, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int px = x / 6, py = y / 6;
            std::mt19937 cell(static_cast<unsigned>(px * 7919 + py * 104729 + int(seed)));
            Rgba8 c{uint8_t(cell() & 255), uint8_t((cell() >> 8) & 255), uint8_t((cell() >> 16) & 255), 255};
            if (x > w - 12) c.a = static_cast<uint8_t>((w - x) * 20);
            img.set(x, y, c);
        }
    (void)d;
    return img;
}

EffectRef effect(const std::string& type, std::map<std::string, double> values = {}, const std::string& id = "fx1") {
    const EffectDef* def = findEffectDef(type);
    EXPECT_NE(def, nullptr) << type;
    EffectRef e = makeEffect(*def, id);
    for (const auto& [k, v] : values) e.params[k] = Animatable<double>(v);
    return e;
}

Image run(const Image& src, const std::vector<EffectRef>& stack, const Time& t = Time(0)) {
    std::vector<std::string> problems;
    const auto inst = evaluateEffects(stack, t, t.toDouble(), &problems);
    EXPECT_TRUE(problems.empty());
    Image out;
    applyEffectsCpu(src, inst, out);
    return out;
}

int maxDiff(const Image& a, const Image& b) {
    int m = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i) m = std::max(m, std::abs(int(a.pixels[i]) - int(b.pixels[i])));
    return m;
}

}  // namespace

// ---- registry -----------------------------------------------------------------------------------------

TEST(EffectRegistry, SchemasAreWellFormed) {
    std::set<std::string> ids;
    for (const auto& d : effectDefs()) {
        SCOPED_TRACE(d.id);
        EXPECT_TRUE(ids.insert(d.id).second);
        EXPECT_FALSE(d.label.empty());
        EXPECT_GE(d.version, 1);
        std::set<std::string> pids;
        for (const auto& p : d.params) {
            EXPECT_TRUE(pids.insert(p.id).second) << p.id;
            EXPECT_LE(p.min, p.def);
            EXPECT_LE(p.def, p.max);
            if (p.kind == ParamKind::Choice) EXPECT_EQ(p.choices.size(), size_t(p.max - p.min + 1));
        }
        EXPECT_TRUE(effectHasCpuImplementation(d.id));
        // defaults are complete and evaluate to the schema defaults
        const EffectRef e = makeEffect(d, "fx1");
        for (const auto& p : d.params) EXPECT_EQ(effectParam(e, d, p.id, Time(0)), p.def);
    }
}

TEST(EffectRegistry, UnknownTypesAreReportedNotSilent) {
    EffectRef e;
    e.id = "fx1";
    e.type = "vendor.unknown.effect";
    std::vector<std::string> problems;
    EXPECT_TRUE(evaluateEffects({e}, Time(0), 0, &problems).empty());
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_NE(problems[0].find("vendor.unknown.effect"), std::string::npos);
}

// ---- Basic colour ---------------------------------------------------------------------------------------

TEST(ColorBasic, NeutralSettingsAreIdentity) {
    const Image src = colourful(64, 48);
    std::vector<EffectRef> stack{effect("fa.color.basic")};
    EXPECT_TRUE(effectsAreIdentity(evaluateEffects(stack, Time(0), 0)));
    // forced through the pipeline anyway: float round trip is exact
    std::vector<EffectInstance> inst = evaluateEffects(stack, Time(0), 0);
    Image out;
    applyEffectsCpu(src, inst, out);
    EXPECT_EQ(out.pixels, src.pixels);
}

// Golden points read off Filmora's measured curves (EXP-006 curves_summary: input -> output, G channel; the
// measurements include the codec round trip, so +-3 levels).
TEST(ColorBasic, MatchesFilmoraMeasuredCurvesAtSamplePoints) {
    struct Case {
        const char* param;
        double value;
        std::vector<std::pair<int, int>> io;
    };
    const std::vector<Case> cases = {
        {"brightness", 50, {{33, 50}, {66, 98}, {97, 146}, {129, 193}, {161, 242}, {193, 255}}},
        {"brightness", -50, {{33, 0}, {66, 25}, {97, 57}, {129, 89}, {161, 121}, {224, 185}}},
        {"exposure", -50, {{33, 15}, {66, 29}, {97, 43}, {129, 57}, {161, 71}, {224, 99}}},
        {"exposure", 50, {{49, 109}, {97, 181}, {145, 234}, {193, 255}}},   // fit RMS 1.9; up to 4 low (25 -> 58 vs 62)
        {"contrast", 50, {{33, 24}, {66, 55}, {97, 91}, {129, 129}, {161, 168}, {193, 202}, {224, 234}}},
        {"contrast", -50, {{33, 44}, {66, 74}, {97, 102}, {129, 129}, {161, 155}, {193, 183}}},
        {"highlights", 50, {{49, 53}, {97, 109}, {145, 163}, {169, 188}, {193, 211}, {241, 247}}},
    };
    const Image ramp = greyRamp();
    for (const auto& c : cases) {
        SCOPED_TRACE(std::string(c.param) + " " + std::to_string(c.value));
        const Image out = run(ramp, {effect("fa.color.basic", {{c.param, c.value}})});
        for (const auto& [in, expect] : c.io) EXPECT_NEAR(out.at(in, 0).g, expect, 3) << "input " << in;
    }
    // temperature / tint: per-channel gains (mid grey 128)
    const Image warm = run(ramp, {effect("fa.color.basic", {{"temperature", 50}})});
    EXPECT_NEAR(warm.at(128, 0).r, 128 * 1.121, 1.5);
    EXPECT_NEAR(warm.at(128, 0).g, 128 * 0.992, 1.5);
    EXPECT_NEAR(warm.at(128, 0).b, 128 * 0.869, 1.5);
    const Image tint = run(ramp, {effect("fa.color.basic", {{"tint", 50}})});
    EXPECT_NEAR(tint.at(128, 0).r, 128 * 1.121, 1.5);
    EXPECT_NEAR(tint.at(128, 0).g, 128 * 0.892, 1.5);
    EXPECT_NEAR(tint.at(128, 0).b, 128 * 1.121, 1.5);
}

// Full comparison with the measured curve files when the Stage 1 evidence is present (RMS printed for the report).
TEST(ColorBasic, RmsAgainstFilmoraCurveFiles) {
    const std::filesystem::path dir = std::filesystem::path(FA_REPO_ROOT) / "experiments" / "EXP-006_color";
    if (!std::filesystem::exists(dir)) GTEST_SKIP() << "Stage 1 evidence not present: " << dir.string();
    struct File {
        const char* name;
        const char* param;
        double value;
        double maxRms;
    };
    const std::vector<File> files = {{"brightness_p50", "brightness", 50, 1.5},   {"brightness_m50", "brightness", -50, 1.5},
                                     {"exposure_m50", "exposure", -50, 1.5},      {"exposure_p50", "exposure", 50, 2.5},
                                     {"contrast_p50", "contrast", 50, 1.5},       {"contrast_m50", "contrast", -50, 1.5},
                                     {"highlight_p50", "highlights", 50, 1.5},    {"temperature_p50", "temperature", 50, 2.0},
                                     {"tint_p50", "tint", 50, 2.0}};
    for (const auto& f : files) {
        std::ifstream in(dir / (std::string(f.name) + ".curve.csv"));
        ASSERT_TRUE(in) << f.name;
        std::string line;
        std::getline(in, line);   // header: in,base_R,base_G,base_B,R,G,B
        double se = 0;
        int n = 0;
        while (std::getline(in, line)) {
            std::stringstream ss(line);
            std::string cell;
            std::vector<double> v;
            while (std::getline(ss, cell, ',')) v.push_back(std::stod(cell));
            if (v.size() < 7) continue;
            // our model applied to the baseline export value (what Filmora's effect saw) vs Filmora's output
            Image px(1, 1, Rgba8{uint8_t(std::lround(v[1])), uint8_t(std::lround(v[2])), uint8_t(std::lround(v[3])), 255});
            const Image out = run(px, {effect("fa.color.basic", {{f.param, f.value}})});
            const Rgba8 o = out.at(0, 0);
            for (int c = 0; c < 3; ++c) {
                const double e = (c == 0 ? o.r : c == 1 ? o.g : o.b) - v[4 + c];
                se += e * e;
                ++n;
            }
        }
        const double rms = std::sqrt(se / std::max(1, n));
        std::printf("  EXP-006 %-16s RMS %.2f levels (%d samples)\n", f.name, rms, n);
        EXPECT_LT(rms, f.maxRms) << f.name;
    }
}

// ---- stack semantics -----------------------------------------------------------------------------------

TEST(EffectStack, OrderIsDeterministicAndMatters) {
    const Image src = colourful(80, 60);
    const EffectRef bright = effect("fa.color.basic", {{"brightness", 60}, {"contrast", 40}}, "fx1");
    const EffectRef blur = effect("fa.fx.blur.gaussian", {{"radius", 40}}, "fx2");
    const Image ab = run(src, {bright, blur}), ba = run(src, {blur, bright});
    EXPECT_EQ(ab.pixels, run(src, {bright, blur}).pixels);   // deterministic
    EXPECT_GT(maxDiff(ab, ba), 10);                           // order changes the result
    // disabled effects are skipped exactly
    EffectRef off = blur;
    off.enabled = false;
    EXPECT_EQ(run(src, {bright, off}).pixels, run(src, {bright}).pixels);
}

TEST(EffectStack, StrengthMixesLinearlyAndMasksLimitTheEffect) {
    const Image ramp = greyRamp(64);
    EffectRef full = effect("fa.color.basic", {{"brightness", -50}});
    EffectRef half = full;
    half.mix = Animatable<double>(0.5);
    const Image a = run(ramp, {full}), b = run(ramp, {half});
    for (int x : {64, 128, 200}) EXPECT_NEAR(b.at(x, 0).g, (x + a.at(x, 0).g) / 2.0, 1.0);
    // ellipse mask in the middle: unchanged outside, full effect at the centre
    EffectRef masked = full;
    Mask m;
    m.id = "m1";
    m.shape = MaskShape::Ellipse;
    m.width = Animatable<double>(0.5);
    m.height = Animatable<double>(0.5);
    masked.masks.push_back(m);
    const Image c = run(ramp, {masked});
    EXPECT_EQ(c.at(2, 32).g, ramp.at(2, 32).g);        // far left: outside
    EXPECT_EQ(c.at(128, 32).g, a.at(128, 32).g);       // centre: inside
    // animated strength (keyframes in source time)
    EffectRef anim = full;
    anim.mix.setKey({Time(0), 0.0});
    anim.mix.setKey({Time(2), 1.0});
    EXPECT_EQ(run(ramp, {anim}, Time(0)).pixels, ramp.pixels);
    EXPECT_NEAR(run(ramp, {anim}, Time(1)).at(128, 0).g, (128 + a.at(128, 0).g) / 2.0, 1.0);
}

TEST(EffectStack, KeyframedParameters) {
    EffectRef e = effect("fa.color.basic");
    e.params["brightness"].setKey({Time(0), 0.0});
    e.params["brightness"].setKey({Time(4), 100.0});
    const auto at1 = evaluateEffects({e}, Time(1), 1.0);
    ASSERT_EQ(at1.size(), 1u);
    EXPECT_NEAR(at1[0].value("brightness"), 25.0, 1e-6);
    // the generic keyframe property ids reach effect parameters
    Clip c;
    c.effects.push_back(e);
    ASSERT_NE(keys::property(c, "fx.fx1.brightness"), nullptr);
    ASSERT_NE(keys::property(c, "fx.fx1.mix"), nullptr);
    EXPECT_EQ(keys::keyTimes(c).size(), 2u);
    EXPECT_EQ(keys::newEffectId(c), "fx2");
}

// ---- Gaussian blur ---------------------------------------------------------------------------------------

TEST(GaussianBlur, ProfileMatchesTheGaussianAndKeepsTheMean) {
    // vertical black/white step: the blurred profile follows 0.5 * erfc(-x / (sigma sqrt 2))
    Image step(400, 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 400; ++x) step.set(x, y, x < 200 ? Rgba8{0, 0, 0, 255} : Rgba8{255, 255, 255, 255});
    for (double radius : {18.0, 150.0}) {   // 150: the downsampled path (sigma 50 -> factor 8 at 1080 lines)
        // picture height 8 -> px at 1080 scale is tiny; use the canvas-independent sigma via a 1080-line picture
        Image tall(400, 1080);
        for (int y = 0; y < 1080; ++y) std::memcpy(tall.row(y), step.row(0), 400 * 4);
        const Image out = run(tall, {effect("fa.fx.blur.gaussian", {{"radius", radius}})});
        const double sigma = radius / 3.0;
        double worst = 0;
        for (int x = 120; x < 280; ++x) {
            const double expect = 255.0 * 0.5 * std::erfc(-((x + 0.5) - 200.0) / (sigma * std::sqrt(2.0)));
            worst = std::max(worst, std::fabs(out.at(x, 540).g - expect));
        }
        std::printf("  blur radius %.0f: max deviation from the ideal Gaussian edge %.2f levels\n", radius, worst);
        EXPECT_LT(worst, radius > 100 ? 4.0 : 1.5);
        long long a = 0, b = 0;
        for (int x = 0; x < 400; ++x) {
            a += tall.at(x, 540).g;
            b += out.at(x, 540).g;
        }
        EXPECT_NEAR(double(b) / 400, double(a) / 400, 0.6);   // mean preserved (clamp-to-edge)
    }
}

TEST(GaussianBlur, TransparentPixelsDoNotDarkenEdges) {
    // white opaque square on fully transparent black: blurred colour stays white where alpha > 0 (premultiplied)
    Image img(200, 200, Rgba8{0, 0, 0, 0});
    for (int y = 60; y < 140; ++y)
        for (int x = 60; x < 140; ++x) img.set(x, y, Rgba8{255, 255, 255, 255});
    Image tall(200, 1080, Rgba8{0, 0, 0, 0});
    for (int y = 0; y < 200; ++y) std::memcpy(tall.row(y + 440), img.row(y), 200 * 4);
    const Image out = run(tall, {effect("fa.fx.blur.gaussian", {{"radius", 30}})});
    for (int x : {50, 55, 145, 150}) {
        const Rgba8 c = out.at(x, 540);
        EXPECT_GT(c.a, 10);
        EXPECT_GE(c.r, 253);
    }
}

// ---- project model -----------------------------------------------------------------------------------------

TEST(EffectModel, SerializationRoundTripV4) {
    Project p;
    Sequence s;
    s.id = "seq";
    Track v{"v1", TrackKind::Video, "Video 1"};
    Clip adj;
    adj.id = "adj";
    adj.adjustment = true;
    adj.duration = Rational(2);
    EffectRef e = effect("fa.color.basic", {{"exposure", 30}});
    e.mix.setKey({Time(0), 0.25});
    e.mix.setKey({Time(1), 1.0});
    e.strings["lut"] = "C:/luts/look.cube";
    e.curves["master"] = {{0, 0}, {0.5, 0.7}, {1, 1}};
    Mask m;
    m.id = "m1";
    e.masks.push_back(m);
    e.version = 2;
    adj.effects.push_back(e);
    adj.effects.push_back(effect("fa.fx.blur.gaussian", {{"radius", 12}}, "fx2"));
    v.clips.push_back(adj);
    s.tracks.push_back(v);
    p.sequences.push_back(s);
    ASSERT_TRUE(validate(p).empty()) << formatIssues(validate(p));
    const std::string json = projectToJsonString(p);
    EXPECT_NE(json.find("\"version\": " + std::to_string(Project::kFormatVersion)), std::string::npos);   // written as the current format (v4 effect fields, v5 since M5)
    const Project back = projectFromJsonString(json);
    EXPECT_EQ(back, p);
    // files before v4: effects without ids get stable ids on load
    const std::string old = R"({"format":"fa-project","version":1,"name":"","nextId":1,"sources":[{"id":"s","path":"a.mp4"}],
        "sequences":[{"id":"q","name":"","width":1920,"height":1080,"frameRate":"30","sampleRate":48000,"tracks":[
        {"id":"v","kind":"video","clips":[{"id":"c","source":"s","start":"0","in":"0","duration":"1",
        "effects":[{"type":"fa.color.basic","params":{"brightness":{"value":10,"keys":[]}}}]}]}]}]})";
    const Project o = projectFromJsonString(old);
    ASSERT_EQ(o.sequences[0].tracks[0].clips[0].effects.size(), 1u);
    EXPECT_EQ(o.sequences[0].tracks[0].clips[0].effects[0].id, "fx1");
}

TEST(EffectModel, ValidationRules) {
    Project p;
    Sequence s;
    s.id = "seq";
    Track a{"a1", TrackKind::Audio, "Audio 1"};
    Clip adj;
    adj.id = "adj";
    adj.adjustment = true;
    adj.duration = Rational(1);
    a.clips.push_back(adj);
    s.tracks.push_back(a);
    p.sequences.push_back(s);
    EXPECT_FALSE(validate(p).empty());   // adjustment layers belong on video tracks
    p.sequences[0].tracks[0].kind = TrackKind::Video;
    EXPECT_TRUE(validate(p).empty()) << formatIssues(validate(p));
    p.sequences[0].tracks[0].clips[0].effects = {effect("fa.color.basic", {}, "fx1"), effect("fa.color.basic", {}, "fx1")};
    EXPECT_FALSE(validate(p).empty());   // duplicate effect ids
}

// ---- adjustment layers through renderFrame -------------------------------------------------------------

namespace {
class SolidProvider final : public FrameProvider {
public:
    Result frame(const Clip&, const MediaSource&, const Time& t) override {
        img_.reset(320, 180, Rgba8{200, 100, 50, 255});
        Result r;
        r.image = &img_;
        r.requestedTime = r.frameTime = t;
        return r;
    }

private:
    Image img_;
};
}  // namespace

TEST(AdjustmentLayer, AdjustsTracksBelowOnlyWithinItsRangeAndMask) {
    Project p;
    MediaSource src;
    src.id = "src";
    src.path = "solid.mp4";
    src.duration = Rational(10);
    src.width = 320;
    src.height = 180;
    p.sources.push_back(src);
    Sequence s;
    s.id = "seq";
    s.width = 320;
    s.height = 180;
    Track v1{"v1", TrackKind::Video, "Video 1"}, v2{"v2", TrackKind::Video, "Video 2"}, v3{"v3", TrackKind::Video, "Video 3"};
    Clip media;
    media.id = "m";
    media.sourceId = "src";
    media.duration = Rational(10);
    v1.clips.push_back(media);
    Clip adj;
    adj.id = "adj";
    adj.adjustment = true;
    adj.timelineStart = Rational(1);
    adj.duration = Rational(2);
    adj.effects.push_back(effect("fa.color.basic", {{"saturation", -100}}));   // monochrome
    v2.clips.push_back(adj);
    Clip top = media;   // on Video 3, half size on the right: above the adjustment layer -> stays coloured
    top.id = "top";
    top.transform.scaleX = Animatable<double>(0.25);
    top.transform.positionX = Animatable<double>(0.3);
    v3.clips.push_back(top);
    s.tracks = {v1, v2, v3};
    p.sequences.push_back(s);
    ASSERT_TRUE(validate(p).empty()) << formatIssues(validate(p));
    SolidProvider prov;
    auto be = makeCpuBackend();
    auto frameAt = [&](int f) { return renderFrame(p.sequences[0], f, p, prov, *be); };
    const Image before = frameAt(15);   // 0.5 s: no adjustment
    EXPECT_EQ(before.at(40, 90).r, 200);
    const Image during = frameAt(45);   // 1.5 s: grey below, colour above
    const Rgba8 g = during.at(40, 90);
    EXPECT_EQ(g.r, g.g);
    EXPECT_EQ(g.g, g.b);
    const Rgba8 above = during.at(256, 90);
    EXPECT_GT(above.r, above.b + 100);
    EXPECT_EQ(frameAt(95).at(40, 90).r, 200);   // 3.17 s: after the adjustment layer
    // mask: only the left half of the frame (rectangle in canvas-height units), opacity 0.5
    Clip& a = p.sequences[0].tracks[1].clips[0];
    Mask m;
    m.id = "m1";
    m.shape = MaskShape::Rectangle;
    m.centerX = Animatable<double>(-0.45);
    m.width = Animatable<double>(0.9);
    m.height = Animatable<double>(2.0);
    a.masks.push_back(m);
    a.opacity = Animatable<double>(0.5);
    const Image masked = frameAt(45);
    const Rgba8 left = masked.at(20, 90), right = masked.at(200, 90);
    EXPECT_EQ(right.r, 200);
    const double grey = 0.2126 * 200 + 0.7152 * 100 + 0.0722 * 50;
    EXPECT_NEAR(left.r, (200 + grey) / 2, 1.5);
    EXPECT_NEAR(left.b, (50 + grey) / 2, 1.5);
}

// ---- CPU / GPU parity ----------------------------------------------------------------------------------------

namespace {
class EffectsGpuParity : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        std::string why;
        gpu_ = makeGpuBackend(&why, GetParam());
        // no adapter is a reason to skip; a shader that does not compile is a bug
        ASSERT_EQ(why.find("shader"), std::string::npos) << why;
        if (!gpu_) GTEST_SKIP() << "no GPU backend: " << why;
        cpu_ = makeCpuBackend();
    }
    // the picture with an effect stack composited at identity (so the comparison sees the stack's output)
    void compare(const Image& src, const std::vector<EffectRef>& stack, int maxAllowed, const char* what, bool adjustment = false,
                 double clipSeconds = 0.0) {
        auto inst = std::make_shared<std::vector<EffectInstance>>(evaluateEffects(stack, Time(0), clipSeconds));
        const int64_t passesBefore = gpu_->gpuEffectPasses(), fallbacksBefore = gpu_->cpuEffectFallbacks();
        LayerDesc l;
        if (adjustment) {
            l.adjustment = true;
        } else {
            l.image = &src;
        }
        l.effects = inst;
        LayerDesc base;
        base.image = &src;
        for (RenderBackend* be : {cpu_.get(), gpu_.get()}) {
            be->beginFrame(src.width, src.height, Rgba8{10, 20, 30, 255});
            if (adjustment) be->drawLayer(base);
            be->drawLayer(l);
        }
        const Image c = cpu_->endFrame();
        const Image& g = gpu_->endFrame();
        int m = 0;
        double sum = 0;
        for (size_t i = 0; i < c.pixels.size(); ++i) {
            const int d = std::abs(int(c.pixels[i]) - int(g.pixels[i]));
            m = std::max(m, d);
            sum += d;
        }
        std::printf("  %-34s max %d, mean %.4f, %lld GPU passes\n", what, m, sum / double(c.pixels.size()),
                    static_cast<long long>(gpu_->gpuEffectPasses() - passesBefore));
        EXPECT_LE(m, maxAllowed) << what;
        EXPECT_GT(gpu_->gpuEffectPasses(), passesBefore) << what << ": the GPU did not run effect passes";
        EXPECT_EQ(gpu_->cpuEffectFallbacks(), fallbacksBefore) << what << ": fell back to the CPU";
    }
    std::unique_ptr<RenderBackend> cpu_, gpu_;
};
}  // namespace

TEST_P(EffectsGpuParity, ColorBasic) {
    const Image src = colourful(256, 144);
    compare(src, {effect("fa.color.basic", {{"brightness", 40}, {"contrast", 30}, {"temperature", -20}})}, 1, "basic light/WB");
    compare(src, {effect("fa.color.basic", {{"exposure", 70}, {"highlights", -40}, {"shadows", 50}, {"saturation", 30},
                                            {"vibrance", 60}, {"gamma", -30}, {"whites", 20}, {"blacks", -20}, {"tint", 40}})},
            1, "basic all sliders");
    compare(src, {effect("fa.color.basic", {{"exposure", -80}, {"contrast", -60}, {"saturation", -100}})}, 1, "basic negative");
}

TEST_P(EffectsGpuParity, GaussianBlurAndStackWithMixAndMask) {
    const Image src = colourful(320, 180);
    compare(src, {effect("fa.fx.blur.gaussian", {{"radius", 60}})}, 1, "gaussian r60");
    const Image tall = colourful(480, 1080);   // 1080 lines: sigma 66 px -> the 8x downsampled path
    compare(tall, {effect("fa.fx.blur.gaussian", {{"radius", 200}})}, 1, "gaussian r200 (downsampled x8)");
    EffectRef masked = effect("fa.color.basic", {{"brightness", -60}}, "fx1");
    masked.mix = Animatable<double>(0.7);
    Mask m;
    m.id = "m1";
    m.feather = Animatable<double>(0.05);
    masked.masks.push_back(m);
    compare(src, {masked, effect("fa.fx.blur.gaussian", {{"radius", 30}}, "fx2")}, 1, "masked basic + blur");
}

TEST_P(EffectsGpuParity, AdjustmentLayer) {
    const Image src = colourful(320, 180);
    compare(src, {effect("fa.color.basic", {{"saturation", -80}, {"contrast", 40}}),
                  effect("fa.fx.blur.gaussian", {{"radius", 20}}, "fx2")},
            1, "adjustment basic + blur", true);
}

// Negative control: the comparison detects a one-unit parameter difference between the backends.
TEST_P(EffectsGpuParity, NegativeControlDetectsAParameterMismatch) {
    const Image src = colourful(256, 144);
    LayerDesc a, b;
    a.image = b.image = &src;
    a.effects = std::make_shared<std::vector<EffectInstance>>(evaluateEffects({effect("fa.color.basic", {{"brightness", 40}})}, Time(0), 0));
    b.effects = std::make_shared<std::vector<EffectInstance>>(evaluateEffects({effect("fa.color.basic", {{"brightness", 41}})}, Time(0), 0));
    cpu_->beginFrame(256, 144, Rgba8{});
    cpu_->drawLayer(a);
    gpu_->beginFrame(256, 144, Rgba8{});
    gpu_->drawLayer(b);
    const Image c = cpu_->endFrame();
    const Image& g = gpu_->endFrame();
    EXPECT_GT(maxDiff(c, g), 1);
}

// ---- HSL (EXP-015) -------------------------------------------------------------------------------------------

namespace {
Rgba8 hsvPixel(float hDeg, float s, float v) {
    const float c = v * s, hp = hDeg / 60.0f, x = c * (1 - std::fabs(std::fmod(hp, 2.0f) - 1));
    float r = 0, g = 0, b = 0;
    if (hp < 1) r = c, g = x;
    else if (hp < 2) r = x, g = c;
    else if (hp < 3) g = c, b = x;
    else if (hp < 4) g = x, b = c;
    else if (hp < 5) r = x, b = c;
    else r = c, b = x;
    const float m = v - c;
    auto q = [](float f) { return uint8_t(std::lround(std::clamp(f, 0.0f, 1.0f) * 255)); };
    return {q(r + m), q(g + m), q(b + m), 255};
}
float hueOf(Rgba8 p) {
    const float r = p.r / 255.f, g = p.g / 255.f, b = p.b / 255.f;
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    if (d <= 0) return 0;
    const float h = mx == r ? 60 * ((g - b) / d) : mx == g ? 60 * ((b - r) / d + 2) : 60 * ((r - g) / d + 4);
    return h < 0 ? h + 360 : h;
}
float satOf(Rgba8 p) {
    const float mx = std::max({p.r, p.g, p.b}) / 255.f, mn = std::min({p.r, p.g, p.b}) / 255.f;
    return mx > 0 ? (mx - mn) / mx : 0;
}
float hueDelta(float from, float to) { return std::remainder(to - from, 360.0f); }

Rgba8 runPixel(Rgba8 px, const std::vector<EffectRef>& stack) { return run(Image(1, 1, px), stack).at(0, 0); }

// Rows of an EXP-0xx "*.curve.csv" (in, base_R, base_G, base_B, R, G, B), or empty when the evidence is absent.
std::vector<std::vector<double>> readCurveCsv(const std::filesystem::path& file) {
    std::vector<std::vector<double>> rows;
    std::ifstream in(file);
    std::string line;
    if (!std::getline(in, line)) return rows;
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string cell;
        std::vector<double> v;
        while (std::getline(ss, cell, ',')) v.push_back(std::stod(cell));
        if (v.size() == 7) rows.push_back(std::move(v));
    }
    return rows;
}
Rgba8 basePixel(const std::vector<double>& row) {
    auto q = [](double v) { return uint8_t(std::lround(std::clamp(v, 0.0, 255.0))); };
    return {q(row[1]), q(row[2]), q(row[3]), 255};
}
}  // namespace

TEST(ColorHsl, RmsAgainstFilmoraMeasuredPatches) {
    // EXP-015: Filmora's output for each of the 216 chart patches (tools/export_hsl_patches.py). Float fits of the
    // model: 1.27-1.46 levels; the 8-bit input and output add about 0.4.
    const std::filesystem::path dir = std::filesystem::path(FA_REPO_ROOT) / "experiments" / "EXP-015_hsl";
    const std::vector<std::pair<std::string, std::pair<std::string, double>>> cases = {
        {"hsl_red_hue_50", {"red.hue", 50}},           {"hsl_green_hue_50", {"green.hue", 50}},
        {"hsl_red_sat_-100", {"red.saturation", -100}}, {"hsl_blue_sat_-100", {"blue.saturation", -100}},
        {"hsl_red_lum_50", {"red.luminance", 50}}};
    for (const auto& [file, setting] : cases) {
        std::ifstream in(dir / (file + ".patches.csv"));
        if (!in) GTEST_SKIP() << "Stage 1 evidence not present";
        std::string line;
        std::getline(in, line);
        Image src(216, 1), expect(216, 1);
        int n = 0;
        while (std::getline(in, line) && n < 216) {
            std::stringstream ss(line);
            std::string cell;
            std::vector<double> v;
            while (std::getline(ss, cell, ',')) v.push_back(std::stod(cell));
            auto q = [](double x) { return uint8_t(std::lround(std::clamp(x, 0.0, 255.0))); };
            src.set(n, 0, Rgba8{q(v[0]), q(v[1]), q(v[2]), 255});
            expect.set(n, 0, Rgba8{q(v[3]), q(v[4]), q(v[5]), 255});
            ++n;
        }
        ASSERT_EQ(n, 216) << file;
        const Image out = run(src, {effect("fa.color.hsl", {{setting.first, setting.second}})});
        double se = 0, worst = 0;
        for (size_t i = 0; i < out.pixels.size(); ++i) {
            if (i % 4 == 3) continue;
            const double d = double(out.pixels[i]) - double(expect.pixels[i]);
            se += d * d;
            worst = std::max(worst, std::fabs(d));
        }
        const double rms = std::sqrt(se / (216.0 * 3));
        std::printf("  EXP-015 %-18s RMS %.2f levels, max %.0f\n", file.c_str(), rms, worst);
        EXPECT_LT(rms, 2.0) << file;
    }
}

TEST(ColorHsl, RangesHueSaturationLightness) {
    const auto hsl = [](std::map<std::string, double> v) { return std::vector<EffectRef>{effect("fa.color.hsl", std::move(v))}; };
    // hue +50 at a band centre: measured red +15.5 deg, green +20.5 deg
    EXPECT_NEAR(hueDelta(350, hueOf(runPixel(hsvPixel(350, 0.9f, 0.9f), hsl({{"red.hue", 50}})))), 15.5, 1.0);
    EXPECT_NEAR(hueDelta(120, hueOf(runPixel(hsvPixel(120, 0.9f, 0.9f), hsl({{"green.hue", 50}})))), 20.5, 1.0);
    // ranges blend: half way between the red (350) and orange (30) centres the red hue edit has half its effect
    EXPECT_NEAR(hueDelta(10, hueOf(runPixel(hsvPixel(10, 0.9f, 0.9f), hsl({{"red.hue", 50}})))), 7.75, 1.0);
    // outside a range nothing changes (measured: < 1 level); neutral grey under every range
    const Rgba8 blue = hsvPixel(231, 0.8f, 0.8f);
    EXPECT_EQ(runPixel(blue, hsl({{"red.hue", 100}, {"red.saturation", -100}, {"red.luminance", 100}})), blue);
    const Rgba8 grey{128, 128, 128, 255};
    EXPECT_EQ(runPixel(grey, hsl({{"red.hue", 100}, {"green.saturation", 100}, {"blue.luminance", 100}})), grey);
    // saturation -100 within 20 deg of the centre: the HSL lightness grey (max + min) / 2, as measured
    for (const Rgba8 in : {Rgba8{110, 109, 219, 255}, Rgba8{20, 22, 113, 255}, Rgba8{87, 110, 251, 255}}) {
        const Rgba8 o = runPixel(in, hsl({{"blue.saturation", -100}}));
        const int l = (std::max({in.r, in.g, in.b}) + std::min({in.r, in.g, in.b}) + 1) / 2;
        EXPECT_NEAR(o.r, l, 1);
        EXPECT_EQ(o.r, o.g);
        EXPECT_EQ(o.g, o.b);
    }
    // more saturation stays in gamut without a hue change
    const Rgba8 pale = hsvPixel(120, 0.4f, 0.7f), vivid = runPixel(pale, hsl({{"green.saturation", 100}}));
    EXPECT_GT(satOf(vivid), satOf(pale) + 0.2f);
    EXPECT_NEAR(hueOf(vivid), 120, 1.5);
    // luminance keeps the HSL saturation and raises the lightness most in the mid tones
    auto hslSat = [](Rgba8 p) {
        const double mx = std::max({p.r, p.g, p.b}) / 255.0, mn = std::min({p.r, p.g, p.b}) / 255.0, l = (mx + mn) / 2;
        return (mx - mn) / (1 - std::fabs(2 * l - 1));
    };
    const Rgba8 red{181, 13, 49, 255}, lighter = runPixel(red, hsl({{"red.luminance", 50}}));
    EXPECT_NEAR(hslSat(lighter), hslSat(red), 0.02);
    EXPECT_GT(lighter.r, red.r + 15);   // measured 181 -> 210
    const Rgba8 darker = runPixel(red, hsl({{"red.luminance", -50}}));
    EXPECT_LT(darker.r, red.r - 15);
    // every range has a centre where only that range acts
    const char* bands[8] = {"red", "orange", "yellow", "green", "aqua", "blue", "purple", "magenta"};
    const float centres[8] = {350, 30, 60, 120, 180, 231, 275, 310};
    for (int i = 0; i < 8; ++i) {
        const Rgba8 p = hsvPixel(centres[i], 0.9f, 0.9f);
        EXPECT_LT(satOf(runPixel(p, hsl({{std::string(bands[i]) + ".saturation", -100}}))), 0.02f) << bands[i];
        EXPECT_EQ(runPixel(p, hsl({{std::string(bands[(i + 4) % 8]) + ".saturation", -100}})), p) << bands[i];
    }
}

// ---- Curves (EXP-019) -------------------------------------------------------------------------------------------

TEST(ColorCurves, FilmorasControlPointRuleAndMeasuredRender) {
    // EXP-019: the dragged Y curve has knots (0,0) (0.5,0.75128) (1,1) and Filmora stored the Bezier handles
    // (0.1667,0.2923) (0.3333,0.5846) | (0.6667,0.9179) (0.8333,0.9590): our tangent rule must reproduce them, i.e.
    // the value at the middle of each segment is the Bezier midpoint 0.125 y0 + 0.375 (h1 + h2) + 0.125 y1.
    const std::vector<Vec2> knots = {{0, 0}, {0.5, 0.75128}, {1, 1}};
    EXPECT_NEAR(evaluateCurve(knots, 0.25, false), 0.375 * (0.2923 + 0.5846) + 0.125 * 0.75128, 1e-3);
    EXPECT_NEAR(evaluateCurve(knots, 0.75, false), 0.125 * 0.75128 + 0.375 * (0.9179 + 0.9590) + 0.125, 1e-3);
    EXPECT_NEAR(evaluateCurve(knots, 0.5, false), 0.75128, 1e-9);
    EffectRef e = effect("fa.color.curves");
    e.curves["master"] = knots;
    // measured render on the grey ramp: 33->58, 66->110, 129->193, 193->236
    const Image out = run(greyRamp(), {e});
    for (const auto& [in, expect] : std::vector<std::pair<int, int>>{{33, 58}, {66, 110}, {129, 193}, {193, 236}})
        EXPECT_NEAR(out.at(in, 0).g, expect, 2) << in;
    // RMS against the whole measured render (when the Stage 1 evidence is present)
    const auto rows = readCurveCsv(std::filesystem::path(FA_REPO_ROOT) / "experiments" / "EXP-019_curves" / "curve_y_mid075.curve.csv");
    if (!rows.empty()) {
        double seModel = 0, sePipe = 0;
        for (const auto& row : rows) {
            const double model = 255.0 * evaluateCurve(knots, row[1] / 255.0, false) - row[4];
            seModel += model * model;
            const Rgba8 px = runPixel(basePixel(row), {e});
            for (int c = 0; c < 3; ++c) {
                const double d = (c == 0 ? px.r : c == 1 ? px.g : px.b) - row[4 + c];
                sePipe += d * d / 3.0;
            }
        }
        const double rmsModel = std::sqrt(seModel / double(rows.size())), rmsPipe = std::sqrt(sePipe / double(rows.size()));
        std::printf("  EXP-019 Y curve: model RMS %.2f, 8-bit pipeline RMS %.2f levels (%zu samples)\n", rmsModel, rmsPipe, rows.size());
        EXPECT_LT(rmsModel, 0.8);
        EXPECT_LT(rmsPipe, 1.2);
    }
    // a channel curve acts on its channel only; the default (identity) curves are skipped as identity
    EffectRef red = effect("fa.color.curves");
    red.curves["red"] = {{0, 0}, {1, 0.5}};
    const Rgba8 r = runPixel(Rgba8{200, 200, 200, 255}, {red});
    EXPECT_NEAR(r.r, 100, 1);
    EXPECT_EQ(r.g, 200);
    EXPECT_EQ(r.b, 200);
    EXPECT_TRUE(effectsAreIdentity(evaluateEffects({effect("fa.color.curves")}, Time(0), 0)));
    EXPECT_FALSE(effectsAreIdentity(evaluateEffects({red}, Time(0), 0)));
}

TEST(ColorCurves, HueCurvesRotateAndDesaturateSelectedHues) {
    EffectRef e = effect("fa.color.curves");
    e.curves["hueHue"] = {{0.0, 0.5}, {120.0 / 360, 0.5 + 30.0 / 360}, {240.0 / 360, 0.5}};   // green +30 deg
    e.curves["hueSat"] = {{0.0, 0.5}, {240.0 / 360, 0.0}, {0.9, 0.5}};                    // blue -> grey
    EXPECT_NEAR(hueDelta(120, hueOf(runPixel(hsvPixel(120, 0.9f, 0.9f), {e}))), 30, 1.5);
    EXPECT_LT(satOf(runPixel(hsvPixel(240, 0.9f, 0.9f), {e})), 0.03f);
    const Rgba8 red = hsvPixel(0, 0.9f, 0.9f);
    EXPECT_EQ(runPixel(red, {e}), red);   // both curves neutral at 0 deg
    const Rgba8 grey{90, 90, 90, 255};
    EXPECT_EQ(runPixel(grey, {e}), grey);
    // the hue axis wraps: a point near 360 deg shapes the curve just above 0 deg
    EffectRef wrap = effect("fa.color.curves");
    wrap.curves["hueHue"] = {{0.25, 0.5}, {0.5, 0.5}, {0.98, 0.5 + 20.0 / 360}};
    EXPECT_GT(hueDelta(5, hueOf(runPixel(hsvPixel(5, 0.9f, 0.9f), {wrap}))), 5.0);
}

// ---- Color wheels (EXP-016) ---------------------------------------------------------------------------------------

TEST(ColorWheels, RmsAgainstFilmoraMeasuredWheels) {
    const std::filesystem::path dir = std::filesystem::path(FA_REPO_ROOT) / "experiments" / "EXP-016_wheels";
    for (const auto& [file, param] : std::vector<std::pair<std::string, std::string>>{
             {"wheel_sha_r_0.5", "shadows.r"}, {"wheel_mid_r_0.5", "midtones.r"}, {"wheel_hig_r_0.5", "highlights.r"}}) {
        const auto rows = readCurveCsv(dir / (file + ".curve.csv"));
        if (rows.empty()) GTEST_SKIP() << "Stage 1 evidence not present";
        double se = 0;
        for (const auto& row : rows) {
            const Rgba8 o = runPixel(basePixel(row), {effect("fa.color.wheels", {{param, 0.5}})});
            for (int c = 0; c < 3; ++c) {
                const double d = (c == 0 ? o.r : c == 1 ? o.g : o.b) - row[4 + c];
                se += d * d;
            }
        }
        const double rms = std::sqrt(se / (3.0 * double(rows.size())));
        std::printf("  EXP-016 %-16s RMS %.2f levels\n", file.c_str(), rms);
        EXPECT_LT(rms, 1.2) << file;
    }
}

TEST(ColorWheels, TonalRangesAndNeutrality) {
    // neutral wheels are identity; a shadows edit leaves white alone, a highlights edit leaves black alone
    EXPECT_TRUE(effectsAreIdentity(evaluateEffects({effect("fa.color.wheels")}, Time(0), 0)));
    const Rgba8 white{255, 255, 255, 255}, black{0, 0, 0, 255};
    EXPECT_EQ(runPixel(white, {effect("fa.color.wheels", {{"shadows.b", 1.0}, {"shadows.lum", 1.0}})}), white);
    EXPECT_EQ(runPixel(black, {effect("fa.color.wheels", {{"highlights.r", -1.0}, {"highlights.lum", 1.0}})}), black);
    // midtones luminance brightens mid grey, not the ends
    const Rgba8 mid = runPixel(Rgba8{128, 128, 128, 255}, {effect("fa.color.wheels", {{"midtones.lum", 0.5}})});
    EXPECT_GT(mid.g, 128 + 30);
    EXPECT_EQ(mid.r, mid.g);
    EXPECT_EQ(mid.b, mid.g);
}

// ---- vignette, sharpen ------------------------------------------------------------------------------------------

TEST(Vignette, DarkensCornersKeepsCentre) {
    const Image flat(320, 180, Rgba8{200, 200, 200, 255});
    const Image out = run(flat, {effect("fa.fx.vignette", {{"amount", -80}})});
    EXPECT_EQ(out.at(160, 90).g, 200);
    EXPECT_LT(out.at(1, 1).g, 60);
    EXPECT_LT(out.at(318, 178).g, 60);
    EXPECT_GT(out.at(160, 1).g, out.at(1, 1).g + 10);   // mid edge less than the corner
    const Image light = run(flat, {effect("fa.fx.vignette", {{"amount", 80}})});
    EXPECT_GT(light.at(1, 1).g, 240);
    EXPECT_EQ(light.at(160, 90).g, 200);
    EXPECT_TRUE(effectsAreIdentity(evaluateEffects({effect("fa.fx.vignette", {{"amount", 0}})}, Time(0), 0)));
}

TEST(Sharpen, OvershootsEdgesLeavesFlatAreas) {
    Image tall(200, 1080);
    for (int y = 0; y < 1080; ++y)
        for (int x = 0; x < 200; ++x) tall.set(x, y, x < 100 ? Rgba8{80, 80, 80, 255} : Rgba8{170, 170, 170, 255});
    // amount 150 %, sigma 3 px: 1.5 px from the edge the blur is 80 + 90 Phi(-0.5) = 107.8 -> 80 - 1.5 * 27.8 = 38
    const Image out = run(tall, {effect("fa.fx.sharpen", {{"amount", 150}, {"radius", 3}})});
    EXPECT_EQ(out.at(20, 540).g, 80);
    EXPECT_EQ(out.at(180, 540).g, 170);
    EXPECT_NEAR(out.at(98, 540).g, 38, 3);
    EXPECT_NEAR(out.at(101, 540).g, 212, 3);
    EXPECT_TRUE(effectsAreIdentity(evaluateEffects({effect("fa.fx.sharpen", {{"amount", 0}})}, Time(0), 0)));
}

TEST_P(EffectsGpuParity, ColorToolsAndDetail) {
    const Image src = colourful(320, 180);
    compare(src, {effect("fa.color.hsl", {{"red.hue", 60}, {"green.saturation", -70}, {"blue.luminance", 40}, {"orange.hue", -30},
                                          {"aqua.saturation", 50}, {"magenta.luminance", -60}})},
            1, "hsl");
    EffectRef curves = effect("fa.color.curves");
    curves.curves["master"] = {{0, 0.05}, {0.3, 0.2}, {0.7, 0.85}, {1, 0.95}};
    curves.curves["red"] = {{0, 0}, {0.5, 0.6}, {1, 1}};
    curves.curves["hueHue"] = {{0.1, 0.5}, {0.4, 0.6}, {0.8, 0.45}};
    curves.curves["hueSat"] = {{0.2, 0.5}, {0.6, 0.1}};
    compare(src, {curves}, 1, "curves");
    compare(src, {effect("fa.color.wheels", {{"shadows.b", 0.4}, {"midtones.r", -0.3}, {"highlights.g", 0.5}, {"midtones.lum", 0.2}})},
            1, "wheels");
    compare(src, {effect("fa.fx.vignette", {{"amount", -70}, {"roundness", 60}})}, 1, "vignette");
    compare(src, {effect("fa.fx.vignette", {{"amount", 50}, {"size", 30}, {"feather", 80}})}, 1, "vignette light");
    const Image tall = colourful(320, 540);
    compare(tall, {effect("fa.fx.sharpen", {{"amount", 200}, {"radius", 4}})}, 1, "sharpen");
}

TEST_P(EffectsGpuParity, Luts) {
    const Image src = colourful(320, 180);
    auto lutFx = [](const std::string& ref, double strength, double domain = 0) {
        EffectRef e = effect("fa.color.lut", {{"strength", strength}, {"domain", domain}});
        e.strings["lut"] = ref;
        return e;
    };
    const std::filesystem::path luts = std::filesystem::path(FA_TESTMEDIA_DIR) / "luts";
    compare(src, {lutFx((luts / "lut_random_3.cube").string(), 100)}, 1, "lut random 3^3 (tetrahedral)");
    compare(src, {lutFx("builtin:teal-orange", 60)}, 1, "lut built-in look, strength 60");
    compare(src, {lutFx((luts / "lut_domain_half_2.cube").string(), 100, 0)}, 1, "lut domain standard");
    compare(src, {lutFx((luts / "lut_domain_half_2.cube").string(), 100, 1)}, 1, "lut domain Filmora");
    // 1-D shaper + 3-D table
    const auto file = std::filesystem::temp_directory_path() / "meercut_parity_shaper.cube";
    {
        std::ofstream o(file);
        o << "LUT_1D_SIZE 256\nLUT_3D_SIZE 3\n";
        for (int i = 0; i < 256; ++i) {
            const double x = std::pow(i / 255.0, 0.7);
            o << x << " " << x << " " << x << "\n";
        }
        for (int b = 0; b < 3; ++b)
            for (int g = 0; g < 3; ++g)
                for (int r = 0; r < 3; ++r) o << (r * 0.4 + g * 0.1) << " " << (g * 0.5) << " " << (b * 0.3 + r * 0.15) << "\n";
    }
    compare(src, {lutFx(file.string(), 100)}, 1, "lut 1-D shaper + 3-D");
    std::filesystem::remove(file);
}

TEST_P(EffectsGpuParity, EffectsLibrary) {
    const Image src = colourful(320, 180);
    const Image tall = colourful(240, 540);   // half of 1080 lines: sizes scale with the picture height
    compare(tall, {effect("fa.fx.blur.directional", {{"angle", 30}, {"length", 60}})}, 1, "directional blur");
    compare(tall, {effect("fa.fx.glow", {{"threshold", 40}, {"radius", 50}, {"intensity", 150}})}, 1, "glow");
    compare(tall, {effect("fa.fx.pixelate", {{"size", 30}})}, 1, "pixelate");
    EffectRef grain = effect("fa.fx.grain", {{"amount", 80}, {"size", 3}, {"color", 1}});
    compare(src, {grain}, 1, "film grain (colour)");
    compare(src, {effect("fa.fx.grain", {{"amount", 50}})}, 1, "film grain (mono)");
    compare(tall, {effect("fa.fx.rgbsplit", {{"offset", 25}, {"angle", 45}})}, 1, "rgb split");
    compare(src, {effect("fa.fx.edge")}, 1, "edges");
    compare(src, {effect("fa.fx.edge", {{"mode", 1}, {"strength", 200}})}, 1, "pencil sketch");
    compare(src, {effect("fa.fx.edge", {{"mode", 2}})}, 1, "coloured edges");
    compare(src, {effect("fa.fx.chromatic", {{"amount", 100}})}, 1, "chromatic aberration");
    compare(tall, {effect("fa.fx.wave", {{"amplitude", 30}, {"wavelength", 200}})}, 1, "wave");
    compare(tall, {effect("fa.fx.wave", {{"amplitude", 20}, {"direction", 1}})}, 1, "wave vertical");
    compare(tall, {effect("fa.fx.bulge", {{"amount", 80}, {"radius", 40}, {"centerX", 20}})}, 1, "bulge");
    compare(tall, {effect("fa.fx.bulge", {{"amount", -70}})}, 1, "pinch");
    compare(tall, {effect("fa.fx.chromakey", {{"color", double(0x40A060)}, {"tolerance", 50}, {"spill", 60}})}, 1,
            "chroma key (feather, thickness -2)");
    compare(tall, {effect("fa.fx.chromakey", {{"color", double(0x40A060)}, {"thickness", 6}, {"feather", 0}, {"alphaView", 1}})},
            1, "chroma key matte view, dilated");
    compare(src, {effect("fa.filter.bw", {{"red", 50}, {"green", 30}, {"blue", 20}})}, 1, "black & white");
    compare(src, {effect("fa.filter.sepia")}, 1, "sepia");
    EffectRef look = effect("fa.filter.look", {{"strength", 70}});
    look.strings["lut"] = "builtin:vintage";
    compare(src, {look}, 1, "look (built-in LUT)");
}

// M11: every new effect has GPU passes that draw what the CPU reference draws (animated ones at a later moment).
TEST_P(EffectsGpuParity, M11EffectsLibrary) {
    const Image src = colourful(320, 180);
    const Image tall = colourful(240, 540);   // half of 1080 lines: sizes scale with the picture height
    for (int mode = 0; mode < 5; ++mode)
        compare(src, {effect("fa.fx.mirror", {{"mode", double(mode)}})}, 1, ("mirror mode " + std::to_string(mode)).c_str());
    compare(src, {effect("fa.fx.kaleidoscope", {{"segments", 7}, {"angle", 20}, {"zoom", 130}})}, 1, "kaleidoscope");
    compare(tall, {effect("fa.fx.swirl", {{"angle", 300}, {"radius", 60}, {"centerX", 20}})}, 1, "swirl");
    compare(src, {effect("fa.fx.fisheye", {{"amount", 80}})}, 1, "fisheye");
    compare(src, {effect("fa.fx.fisheye", {{"amount", -70}})}, 1, "fisheye (pinch)");
    compare(tall, {effect("fa.fx.blur.zoom", {{"amount", 40}, {"centerY", -30}})}, 1, "zoom blur");
    compare(tall, {effect("fa.fx.tiltshift", {{"position", 10}, {"width", 20}, {"blur", 80}})}, 1, "tilt-shift");
    compare(tall, {effect("fa.fx.shake", {{"amount", 40}, {"rotation", 3}})}, 1, "camera shake", false, 1.37);
    compare(src, {effect("fa.fx.glitch", {{"amount", 90}, {"bands", 9}})}, 1, "glitch", false, 2.6);
    compare(tall, {effect("fa.fx.vhs", {{"amount", 80}, {"scanlines", 70}, {"noise", 60}, {"tracking", 100}})}, 1, "vhs",
            false, 4.2);
    compare(src, {effect("fa.fx.oldfilm", {{"scratches", 100}, {"dust", 100}, {"flicker", 100}})}, 1, "old film", false, 3.3);
    compare(src, {effect("fa.fx.lightleak", {{"amount", 90}, {"speed", 1}})}, 1, "light leak", false, 2.0);
    for (int style = 0; style < 3; ++style)
        compare(src, {effect("fa.fx.halftone", {{"size", 30}, {"angle", 30}, {"style", double(style)}})}, 1,
                ("halftone style " + std::to_string(style)).c_str());
    compare(src, {effect("fa.fx.posterize", {{"levels", 4}})}, 1, "posterize");
    compare(src, {effect("fa.fx.emboss")}, 1, "emboss (grey)");
    compare(src, {effect("fa.fx.emboss", {{"style", 1}, {"strength", 200}, {"angle", -45}})}, 1, "emboss (colour)");
    compare(src, {effect("fa.fx.invert", {{"amount", 60}})}, 1, "invert");
    compare(src, {effect("fa.fx.letterbox")}, 1, "letterbox 2.39:1");
    compare(src, {effect("fa.fx.letterbox", {{"ratio", 6}, {"color", double(0x204080)}, {"opacity", 70}})}, 1,
            "letterbox 9:16, coloured");
    compare(src, {effect("fa.fx.spotlight", {{"centerX", -20}, {"size", 40}})}, 1, "spotlight");
    compare(src, {effect("fa.filter.duotone")}, 1, "duotone");
}

// Documented fallback: a stack beyond the shader limits (here 9 masks on an effect, 9 on an adjustment layer; the
// shaders take 8) runs on the CPU reference inside the GPU backend - the same pixels, counted as a CPU fallback.
TEST_P(EffectsGpuParity, StacksBeyondTheShaderLimitsUseTheCpuReference) {
    const Image src = colourful(256, 144);
    auto nineMasks = [](std::vector<Mask>& masks) {
        for (int i = 0; i < 9; ++i) {
            Mask m;
            m.id = "m" + std::to_string(i);
            m.shape = MaskShape::Rectangle;
            m.centerX = Animatable<double>(-0.8 + 0.2 * i);
            m.width = Animatable<double>(0.12);
            m.height = Animatable<double>(0.9);
            masks.push_back(m);
        }
    };
    EffectRef blur = effect("fa.fx.blur.gaussian", {{"radius", 40}});
    nineMasks(blur.masks);
    Clip adj;
    adj.adjustment = true;
    adj.effects = {effect("fa.color.basic", {{"saturation", -100}})};
    nineMasks(adj.masks);
    const LayerDesc adjLayer = adjustmentLayer(adj, Time(0));
    LayerDesc base, l;
    base.image = l.image = &src;
    l.effects = std::make_shared<std::vector<EffectInstance>>(evaluateEffects({blur}, Time(0), 0.0));
    for (const bool adjustment : {false, true}) {
        const int64_t passes0 = gpu_->gpuEffectPasses(), fallbacks0 = gpu_->cpuEffectFallbacks();
        for (RenderBackend* be : {cpu_.get(), gpu_.get()}) {
            be->beginFrame(src.width, src.height, Rgba8{10, 20, 30, 255});
            be->drawLayer(adjustment ? base : l);
            if (adjustment) be->drawLayer(adjLayer);
        }
        const Image c = cpu_->endFrame();
        const Image& g = gpu_->endFrame();
        int m = 0;
        for (size_t i = 0; i < c.pixels.size(); ++i) m = std::max(m, std::abs(int(c.pixels[i]) - int(g.pixels[i])));
        const char* what = adjustment ? "adjustment layer with 9 masks" : "blur with 9 masks";
        std::printf("  %-34s max %d, %lld CPU fallbacks\n", what, m, static_cast<long long>(gpu_->cpuEffectFallbacks() - fallbacks0));
        EXPECT_EQ(m, 0) << what;   // the CPU reference itself, composited exactly
        EXPECT_EQ(gpu_->cpuEffectFallbacks(), fallbacks0 + 1) << what;
        EXPECT_EQ(gpu_->gpuEffectPasses(), passes0) << what;
        // and the masks matter: the masked blur differs from the unmasked one
        if (!adjustment) {
            EffectRef plain = effect("fa.fx.blur.gaussian", {{"radius", 40}});
            LayerDesc u;
            u.image = &src;
            u.effects = std::make_shared<std::vector<EffectInstance>>(evaluateEffects({plain}, Time(0), 0.0));
            cpu_->beginFrame(src.width, src.height, Rgba8{10, 20, 30, 255});
            cpu_->drawLayer(u);
            EXPECT_NE(cpu_->endFrame().pixels, c.pixels);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Adapters, EffectsGpuParity, ::testing::Values(true, false),
                         [](const auto& info) { return info.param ? "HighPerformance" : "PowerSaving"; });

// ---- M11 effects: what each one does, on the CPU reference -----------------------------------------------------

namespace {
Image solid(int w, int h, Rgba8 c) { return Image(w, h, c); }
int lumaAt(const Image& img, int x, int y) {
    const Rgba8 c = img.at(x, y);
    return static_cast<int>(std::lround(0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b));
}
int channelDiff(Rgba8 a, Rgba8 b) {
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b), std::abs(a.a - b.a)});
}
}  // namespace

TEST(M11Effects, MirrorReflectsOneHalfOntoTheOther) {
    const Image src = colourful(64, 36);
    const Image lr = run(src, {effect("fa.fx.mirror")});   // left onto right
    for (int y : {0, 17, 35})
        for (int x = 0; x < 32; ++x) {
            EXPECT_EQ(channelDiff(lr.at(x, y), src.at(x, y)), 0) << x << "," << y;
            EXPECT_EQ(channelDiff(lr.at(63 - x, y), src.at(x, y)), 0) << x << "," << y;
        }
    const Image quad = run(src, {effect("fa.fx.mirror", {{"mode", 4}})});   // the top-left quarter in all four
    for (int y = 0; y < 18; y += 5)
        for (int x = 0; x < 32; x += 7) {
            EXPECT_EQ(channelDiff(quad.at(63 - x, 35 - y), src.at(x, y)), 0);
            EXPECT_EQ(channelDiff(quad.at(x, 35 - y), src.at(x, y)), 0);
        }
}

TEST(M11Effects, KaleidoscopeIsSymmetricAboutItsWedges) {
    const Image src = colourful(200, 200);
    const Image k = run(src, {effect("fa.fx.kaleidoscope", {{"segments", 6}})});
    EXPECT_GT(maxDiff(k, src), 100);
    int worst = 0;   // mirrored wedges: symmetric about the horizontal line through the centre
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 200; ++x) worst = std::max(worst, channelDiff(k.at(x, y), k.at(x, 199 - y)));
    EXPECT_LE(worst, 2);
}

TEST(M11Effects, SwirlTurnsOnlyInsideItsRadius) {
    const Image src = colourful(160, 90);
    const Image sw = run(src, {effect("fa.fx.swirl", {{"angle", 270}, {"radius", 30}})});   // 27 px radius
    for (auto [x, y] : {std::pair{0, 0}, {159, 89}, {5, 45}, {150, 10}}) EXPECT_EQ(channelDiff(sw.at(x, y), src.at(x, y)), 0);
    int moved = 0, inside = 0;
    for (int y = 30; y < 60; ++y)
        for (int x = 65; x < 95; ++x)
            if (std::hypot(x + 0.5 - 80, y + 0.5 - 45) < 18) {
                ++inside;
                moved += channelDiff(sw.at(x, y), src.at(x, y)) > 10 ? 1 : 0;
            }
    EXPECT_GT(moved, inside / 2);
}

namespace {
int whiteRun(const Image& img, int y) {   // width of the white part of a row
    int n = 0;
    for (int x = 0; x < img.width; ++x) n += img.at(x, y).r > 127 ? 1 : 0;
    return n;
}
}  // namespace

TEST(M11Effects, FisheyeMagnifiesTheMiddleAndKeepsTheCorners) {
    Image src = solid(160, 90, Rgba8{0, 0, 0, 255});   // a 20 px white square in the middle
    for (int y = 35; y < 55; ++y)
        for (int x = 70; x < 90; ++x) src.set(x, y, Rgba8{255, 255, 255, 255});
    for (int y = 0; y < 3; ++y)   // a red 3 x 3 corner (a pixel centre is never exactly on the corner)
        for (int x = 0; x < 3; ++x) src.set(x, y, Rgba8{255, 0, 0, 255});
    const Image bulge = run(src, {effect("fa.fx.fisheye", {{"amount", 100}})});
    const Image pinch = run(src, {effect("fa.fx.fisheye", {{"amount", -100}})});
    EXPECT_GE(whiteRun(bulge, 45), 34);   // magnified by about 1 / 0.5 at the centre
    EXPECT_LE(whiteRun(pinch, 45), 16);
    EXPECT_EQ(bulge.at(0, 0).r, 255);   // the corner stays (within a pixel)
    EXPECT_EQ(pinch.at(0, 0).r, 255);
}

TEST(M11Effects, ZoomBlurStreaksTowardsTheCentre) {
    Image src = solid(160, 90, Rgba8{0, 0, 0, 255});   // a white line well right of the centre
    for (int y = 0; y < 90; ++y) src.set(130, y, Rgba8{255, 255, 255, 255});
    const Image zb = run(src, {effect("fa.fx.blur.zoom", {{"amount", 60}})});
    EXPECT_EQ(zb.at(80, 45).r, 0);    // the centre does not move
    EXPECT_GT(zb.at(140, 45).r, 3);   // the line streaks outwards from the centre
    EXPECT_LT(zb.at(130, 45).r, 128);
    EXPECT_EQ(zb.at(120, 45).r, 0);   // never towards the centre
}

TEST(M11Effects, TiltShiftKeepsItsBandSharp) {
    const Image src = colourful(160, 90);
    const Image ts = run(src, {effect("fa.fx.tiltshift", {{"blur", 200}})});   // the middle 30 % sharp
    for (int y = 34; y < 56; ++y)
        for (int x = 0; x < 160; x += 3) EXPECT_EQ(channelDiff(ts.at(x, y), src.at(x, y)), 0) << y;
    int blurred = 0;
    for (int x = 0; x < 140; ++x) blurred += channelDiff(ts.at(x, 2), src.at(x, 2)) > 20 ? 1 : 0;
    EXPECT_GT(blurred, 70);
}

TEST(M11Effects, CameraShakeMovesThePictureOverTime) {
    const Image src = colourful(240, 135);
    const EffectRef shake = effect("fa.fx.shake", {{"amount", 60}, {"rotation", 2}});
    const Image a = run(src, {shake}, Time(0)), b = run(src, {shake}, Time(Rational(1, 2)));
    EXPECT_GT(maxDiff(a, b), 100);
    EXPECT_EQ(maxDiff(run(src, {shake}, Time(Rational(1, 2))), b), 0);   // the same moment, the same picture
    const EffectRef still = effect("fa.fx.shake", {{"amount", 0}, {"rotation", 0}, {"zoom", 0}});
    EXPECT_EQ(maxDiff(run(src, {still}, Time(1)), src), 0);
}

TEST(M11Effects, GlitchSlipsBandsAndChangesSeveralTimesASecond) {
    const Image src = colourful(200, 120);
    const EffectRef g = effect("fa.fx.glitch", {{"amount", 100}, {"speed", 8}});
    const Image a = run(src, {g}, Time(0));
    EXPECT_EQ(maxDiff(run(src, {g}, Time(Rational(1, 20))), a), 0);   // within one eighth of a second: the same
    EXPECT_GT(maxDiff(run(src, {g}, Time(Rational(1, 2))), a), 100);   // four slots later: another pattern
    int slipped = 0;   // whole rows moved
    for (int y = 0; y < 120; ++y) {
        int changed = 0;
        for (int x = 0; x < 200; ++x) changed += channelDiff(a.at(x, y), src.at(x, y)) > 30 ? 1 : 0;
        slipped += changed > 100 ? 1 : 0;
    }
    EXPECT_GT(slipped, 10);
}

TEST(M11Effects, VhsBleedsRedAndBlueApart) {
    Image src = solid(960, 540, Rgba8{0, 0, 0, 255});   // a 4 px white line; at 540 lines the bleed is 2.5 px
    for (int y = 0; y < 540; ++y)
        for (int x = 480; x < 484; ++x) src.set(x, y, Rgba8{255, 255, 255, 255});
    const Image v = run(src, {effect("fa.fx.vhs", {{"amount", 100}, {"scanlines", 0}, {"noise", 0}, {"tracking", 0}})});
    // red shows the picture to its right: the line's red edge is on the left (the washed-out colour mixes a fifth of
    // the luma back into the other channels)
    EXPECT_GT(v.at(478, 100).r, 100);
    EXPECT_LT(v.at(478, 100).b, 20);
    EXPECT_GT(v.at(485, 100).b, 100);
    EXPECT_LT(v.at(485, 100).r, 20);
    const Image lines = run(solid(960, 540, Rgba8{200, 200, 200, 255}), {effect("fa.fx.vhs", {{"amount", 0}, {"scanlines", 100},
                                                                                             {"noise", 0}, {"tracking", 0}})});
    int lo = 255, hi = 0;
    for (int y = 0; y < 12; ++y) {
        lo = std::min(lo, int(lines.at(10, y).g));
        hi = std::max(hi, int(lines.at(10, y).g));
    }
    EXPECT_GT(hi - lo, 40);   // dark and light lines alternating
}

TEST(M11Effects, OldFilmIsBrownAndFlickers) {
    const Image grey = solid(160, 90, Rgba8{128, 128, 128, 255});
    const Image brown = run(grey, {effect("fa.fx.oldfilm", {{"scratches", 0}, {"dust", 0}, {"flicker", 0}, {"vignette", 0}})});
    EXPECT_GT(brown.at(80, 45).r, brown.at(80, 45).g + 10);
    EXPECT_GT(brown.at(80, 45).g, brown.at(80, 45).b + 10);
    const EffectRef flick = effect("fa.fx.oldfilm", {{"scratches", 0}, {"dust", 0}, {"flicker", 100}, {"vignette", 0}});
    std::set<int> levels;   // film frames 24 a second: the brightness changes from frame to frame
    for (int f = 0; f < 6; ++f) levels.insert(run(grey, {flick}, Time(Rational(f, 24))).at(80, 45).g);
    EXPECT_GE(levels.size(), 3u);
    const Image corners = run(grey, {effect("fa.fx.oldfilm", {{"amount", 0}, {"scratches", 0}, {"dust", 0}, {"flicker", 0}})});
    EXPECT_LT(corners.at(0, 0).g, corners.at(80, 45).g - 20);   // the vignette darkens the corners
}

TEST(M11Effects, LightLeakOnlyBrightens) {
    const Image src = colourful(160, 90);
    const Image ll = run(src, {effect("fa.fx.lightleak", {{"amount", 100}})});
    int brighter = 0;
    for (int y = 0; y < 90; ++y)
        for (int x = 0; x < 148; ++x) {   // (the right edge is partly transparent in the source)
            const Rgba8 a = src.at(x, y), b = ll.at(x, y);
            EXPECT_GE(b.r + 1, a.r);
            EXPECT_GE(b.g + 1, a.g);
            EXPECT_GE(b.b + 1, a.b);
            brighter += b.r > a.r + 30 ? 1 : 0;
        }
    EXPECT_GT(brighter, 500);
}

TEST(M11Effects, HalftoneDotsFollowTheTone) {
    for (int level : {64, 128, 192}) {
        const Image src = solid(240, 135, Rgba8{uint8_t(level), uint8_t(level), uint8_t(level), 255});
        const Image ht = run(src, {effect("fa.fx.halftone", {{"size", 80}})});   // 10 px dots at this size
        double sum = 0;
        int inked = 0;
        for (int y = 0; y < 135; ++y)
            for (int x = 0; x < 240; ++x) {
                sum += ht.at(x, y).r;
                inked += ht.at(x, y).r < 30 || ht.at(x, y).r > 225 ? 1 : 0;
            }
        EXPECT_NEAR(sum / (240.0 * 135.0), level, 40) << level;   // the dots average to about the tone
        EXPECT_GT(inked, 240 * 135 * 6 / 10) << level;              // mostly ink or paper
    }
    const Image white = run(solid(64, 36, Rgba8{255, 255, 255, 255}), {effect("fa.fx.halftone")});
    EXPECT_EQ(maxDiff(white, solid(64, 36, Rgba8{255, 255, 255, 255})), 0);   // no dots on white
}

TEST(M11Effects, PosterizeKeepsOnlyItsLevels) {
    const Image p = run(greyRamp(), {effect("fa.fx.posterize", {{"levels", 3}})});
    std::set<int> values;
    for (int x = 0; x < 256; ++x) values.insert(p.at(x, 0).r);
    EXPECT_EQ(values, (std::set<int>{0, 128, 255}));
}

TEST(M11Effects, EmbossIsMidGreyWhereFlat) {
    const Image flat = run(solid(64, 36, Rgba8{30, 160, 90, 255}), {effect("fa.fx.emboss")});
    EXPECT_EQ(flat.at(32, 18).r, 128);
    EXPECT_EQ(flat.at(32, 18).b, 128);
    Image edge = solid(64, 36, Rgba8{0, 0, 0, 255});
    for (int y = 0; y < 36; ++y)
        for (int x = 32; x < 64; ++x) edge.set(x, y, Rgba8{255, 255, 255, 255});
    const Image e = run(edge, {effect("fa.fx.emboss", {{"angle", 180}})});   // lit from the left
    EXPECT_GT(std::abs(int(e.at(31, 18).r) - 128) + std::abs(int(e.at(32, 18).r) - 128), 100);
}

TEST(M11Effects, InvertIsTheNegative) {
    const Image src = colourful(64, 36);
    const Image inv = run(src, {effect("fa.fx.invert")});
    for (int y = 0; y < 36; y += 5)
        for (int x = 0; x < 50; x += 3) {
            EXPECT_EQ(inv.at(x, y).r, 255 - src.at(x, y).r);
            EXPECT_EQ(inv.at(x, y).b, 255 - src.at(x, y).b);
        }
}

TEST(M11Effects, LetterboxBarsCoverOutsideTheRatio) {
    const Image src = solid(160, 90, Rgba8{200, 200, 200, 255});
    const Image cine = run(src, {effect("fa.fx.letterbox")});   // 2.39:1: bars of (90 - 160 / 2.39) / 2 = 11.53 px
    for (int y : {0, 10, 79, 89}) EXPECT_EQ(cine.at(80, y).r, 0) << y;
    for (int y : {12, 45, 77}) EXPECT_EQ(cine.at(80, y).r, 200) << y;
    EXPECT_NEAR(cine.at(80, 11).r, 200 * (1 - 0.53), 2);   // the partly covered row
    const Image vertical = run(src, {effect("fa.fx.letterbox", {{"ratio", 6}})});   // 9:16: side bars of 54.69 px
    for (int x : {0, 53, 106, 159}) EXPECT_EQ(vertical.at(x, 45).r, 0) << x;
    for (int x : {56, 80, 104}) EXPECT_EQ(vertical.at(x, 45).r, 200) << x;
}

TEST(M11Effects, SpotlightDarkensOutsideItsLight) {
    const Image s = run(solid(160, 90, Rgba8{255, 255, 255, 255}), {effect("fa.fx.spotlight")});
    EXPECT_EQ(s.at(80, 45).r, 255);
    EXPECT_NEAR(s.at(0, 0).r, 255 * 0.25, 1);
}

TEST(M11Effects, DuotoneMapsBlackAndWhiteToItsTwoColours) {
    const EffectRef d = effect("fa.filter.duotone", {{"shadows", double(0x102030)}, {"highlights", double(0xF0E0D0)}});
    const Rgba8 dark = run(solid(8, 8, Rgba8{0, 0, 0, 255}), {d}).at(4, 4);
    const Rgba8 light = run(solid(8, 8, Rgba8{255, 255, 255, 255}), {d}).at(4, 4);
    EXPECT_EQ((std::array<int, 3>{dark.r, dark.g, dark.b}), (std::array<int, 3>{0x10, 0x20, 0x30}));
    EXPECT_EQ((std::array<int, 3>{light.r, light.g, light.b}), (std::array<int, 3>{0xF0, 0xE0, 0xD0}));
    EXPECT_EQ(findEffectDef("fa.filter.duotone")->section, EffectSection::Filters);
}
