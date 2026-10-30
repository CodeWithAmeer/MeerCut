// CPU/GPU parity: the Direct3D 11 backend must reproduce the CPU reference compositor (the definition
// of the semantics) for every M2 feature. Tolerance: the GPU evaluates coordinates in float (CPU:
// double), so bilinear weights and anti-aliased edges may differ by a rounding step; colour math and
// the 8-bit rounding are identical. Skipped (with the reason) when no hardware GPU is available.
#include "fa/render/backend.h"
#include "fa/render/renderer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <random>

using namespace fa;

namespace {

constexpr int W = 320, H = 180;

// Textured, partly transparent source: gradients, a checker and an alpha ramp on the right edge.
Image texture(int w, int h, int seed) {
    Image img(w, h);
    std::mt19937 rng(static_cast<unsigned>(seed));
    std::uniform_int_distribution<int> noise(0, 40);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool check = ((x / 8) + (y / 8)) % 2 == 0;
            const uint8_t r = static_cast<uint8_t>(std::min(255, x * 255 / w + noise(rng)));
            const uint8_t g = static_cast<uint8_t>(check ? 200 : 40);
            const uint8_t b = static_cast<uint8_t>(std::min(255, y * 255 / h + seed * 30 % 100));
            const uint8_t a = static_cast<uint8_t>(x > w * 3 / 4 ? 255 - (x - w * 3 / 4) * 255 / (w / 4 + 1) : 255);
            img.set(x, y, Rgba8{r, g, b, a});
        }
    return img;
}

struct Diff {
    int maxAbs = 0;
    double mean = 0;
    double over1 = 0;   // fraction of channels differing by more than 1
};

Diff compare(const Image& a, const Image& b) {
    Diff d;
    EXPECT_EQ(a.width, b.width);
    EXPECT_EQ(a.height, b.height);
    long long sum = 0, over = 0, n = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        const int e = std::abs(int(a.pixels[i]) - int(b.pixels[i]));
        d.maxAbs = std::max(d.maxAbs, e);
        sum += e;
        over += e > 1;
        ++n;
    }
    d.mean = double(sum) / double(n);
    d.over1 = double(over) / double(n);
    return d;
}

// Parameter: true = high-performance adapter (discrete GPU), false = power-saving adapter (integrated).
class GpuParity : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        std::string why;
        gpu_ = makeGpuBackend(&why, GetParam());
        ASSERT_EQ(why.find("shader"), std::string::npos) << why;   // a shader that does not compile is a bug
        if (!gpu_) GTEST_SKIP() << "no GPU backend: " << why;
        std::printf("  [%s]\n", gpu_->name().c_str());
        cpu_ = makeCpuBackend();
        a_ = texture(W, H, 1);
        b_ = texture(W, H, 2);
        small_ = texture(97, 61, 3);
    }

    // Render the same layer stack on both backends and compare.
    Diff run(const std::vector<LayerDesc>& layers, const char* what, Rgba8 bg = {20, 30, 40, 255}) {
        for (RenderBackend* be : {cpu_.get(), gpu_.get()}) {
            be->beginFrame(W, H, bg);
            for (const auto& l : layers) be->drawLayer(l);
        }
        const Image c = cpu_->endFrame();
        const Image g = gpu_->endFrame();
        const Diff d = compare(c, g);
        std::printf("  %-34s max %d  mean %.4f  >1: %.5f%%\n", what, d.maxAbs, d.mean, d.over1 * 100);
        return d;
    }

    LayerDesc layer(const Image& img, const Clip& clip) const { return clipLayer(clip, Time(0), &img, W, H); }

    std::unique_ptr<RenderBackend> gpu_, cpu_;
    Image a_, b_, small_;
};

Clip transformed(double px, double py, double sx, double sy, double rot, double ax = 0, double ay = 0) {
    Clip c;
    c.transform.positionX.setConstant(px);
    c.transform.positionY.setConstant(py);
    c.transform.uniformScale = false;
    c.transform.scaleX.setConstant(sx);
    c.transform.scaleY.setConstant(sy);
    c.transform.rotation.setConstant(rot);
    c.transform.anchorX.setConstant(ax);
    c.transform.anchorY.setConstant(ay);
    return c;
}

// Bilinear weights and AA edges computed in float may round the other way at a few pixels.
void expectClose(const Diff& d) {
    EXPECT_LE(d.maxAbs, 2);
    EXPECT_LT(d.mean, 0.02);
    EXPECT_LT(d.over1, 0.0005);
}

}  // namespace

TEST_P(GpuParity, PlainLayersAreExact) {
    LayerDesc l1 = layer(a_, Clip{});
    LayerDesc l2 = layer(b_, Clip{});
    l2.opacity = 0.37;
    l2.sourceToCanvas.reset();   // identity placement: the integer fast path on both backends
    l1.sourceToCanvas.reset();
    const Diff d = run({l1, l2}, "plain + 37 % opacity");
    EXPECT_EQ(d.maxAbs, 0);
}

TEST_P(GpuParity, Transforms) {
    expectClose(run({layer(a_, Clip{}), layer(b_, transformed(0.1, -0.05, 0.6, 0.6, 0))}, "scale + position"));
    expectClose(run({layer(a_, Clip{}), layer(b_, transformed(0, 0, 0.7, 0.5, 33))}, "rotation + non-uniform scale"));
    expectClose(run({layer(a_, Clip{}), layer(b_, transformed(0.2, 0.1, 1.8, 1.8, -12, 0.3, -0.2))}, "upscale + anchor"));
    expectClose(run({layer(small_, transformed(0, 0, 1, 1, 0))}, "fit of a smaller source"));
    Clip f = transformed(-0.1, 0, 0.8, 0.8, 5);
    f.transform.flipH = true;
    f.transform.flipV = true;
    f.transform.cropLeft = 0.1;
    f.transform.cropTop = 0.2;
    f.transform.cropRight = 0.05;
    f.transform.cropBottom = 0.15;
    f.opacity.setConstant(0.8);
    expectClose(run({layer(a_, Clip{}), layer(b_, f)}, "flip + crop + opacity"));
}

// M5: a stabilised clip (correction shift + rotation + zoom, applied through the placement) on both backends.
TEST_P(GpuParity, StabilizedPictures) {
    Clip c;
    Stabilization st;
    st.width = W;
    st.height = H;
    st.motion = {CameraMotion{}};
    st.corrections = {FrameCorrection{0.03, -0.02, 0.04}};
    st.zoom = 1.12;
    c.stabilization = st;
    const LayerDesc l = layer(b_, c), plain = layer(b_, Clip{});
    ASSERT_TRUE(l.sourceToCanvas && plain.sourceToCanvas);
    const Vec2 moved = l.sourceToCanvas->apply(10, 10), still = plain.sourceToCanvas->apply(10, 10);
    EXPECT_GT(std::hypot(moved.x - still.x, moved.y - still.y), 5.0);   // the correction is part of the placement
    expectClose(run({layer(a_, Clip{}), l}, "stabilised (shift + rotation + zoom)"));
}

TEST_P(GpuParity, Masks) {
    for (MaskShape shape : {MaskShape::Rectangle, MaskShape::Ellipse, MaskShape::Polygon}) {
        Clip c = transformed(0.05, 0, 0.9, 0.9, 7);
        Mask m;
        m.shape = shape;
        m.centerX.setConstant(0.1);
        m.centerY.setConstant(-0.05);
        m.width.setConstant(0.8);
        m.height.setConstant(0.5);
        m.rotation.setConstant(20);
        m.cornerRadius = 0.25;
        if (shape == MaskShape::Polygon) m.points = {{0, -0.5}, {0.5, 0.1}, {0.2, 0.5}, {-0.3, 0.4}, {-0.5, -0.2}};
        c.masks.push_back(m);
        const char* names[] = {"mask rectangle (rounded)", "mask ellipse", "mask polygon"};
        expectClose(run({layer(a_, Clip{}), layer(b_, c)}, names[static_cast<int>(shape)]));
        c.masks[0].feather.setConstant(0.04);
        c.masks[0].inverted = true;
        c.masks[0].opacity.setConstant(0.7);
        expectClose(run({layer(a_, Clip{}), layer(b_, c)}, "  + feather, invert, opacity"));
    }
    Clip c;
    Mask m1, m2, m3;
    m1.shape = MaskShape::Ellipse;
    m2.shape = MaskShape::Rectangle;
    m2.mode = MaskMode::Subtract;
    m2.width.setConstant(0.2);
    m3.shape = MaskShape::Rectangle;
    m3.mode = MaskMode::Intersect;
    m3.centerX.setConstant(0.2);
    m3.width.setConstant(0.9);
    m3.feather.setConstant(0.02);
    c.masks = {m1, m2, m3};
    expectClose(run({layer(a_, Clip{}), layer(b_, c)}, "mask add/subtract/intersect"));
}

TEST_P(GpuParity, MoreMasksThanTheShaderTakesUseTheExactCpuPath) {
    Clip c;
    for (int i = 0; i < 10; ++i) {
        Mask m;
        m.shape = MaskShape::Ellipse;
        m.centerX.setConstant(-0.7 + i * 0.15);
        m.width.setConstant(0.12);
        m.height.setConstant(0.3);
        c.masks.push_back(m);
    }
    const Diff d = run({layer(a_, Clip{}), layer(b_, transformed(0.1, 0, 0.7, 0.7, 10)), layer(b_, c)}, "10 masks (CPU fallback layer)");
    expectClose(d);
}

TEST_P(GpuParity, BlendModes) {
    for (int i = 0; i < kBlendModeCount; ++i) {
        Clip c = transformed(0, 0, 0.9, 0.9, 3);
        c.blend = static_cast<BlendMode>(i);
        c.opacity.setConstant(i % 2 ? 0.65 : 1.0);
        const std::string what = std::string("blend ") + blendModeLabel(c.blend);
        expectClose(run({layer(a_, Clip{}), layer(b_, c)}, what.c_str()));
    }
}

TEST_P(GpuParity, Transitions) {
    for (const auto& info : transitionTypes()) {
        for (double p : {0.2, 0.5, 0.8}) {
            TransitionRef t{info.id, Time(1)};
            t.direction = TransitionDirection::Up;
            t.softness = 0.15;
            const TransitionLayers tl = evaluateTransition(t, p);
            Clip ca = transformed(0, 0, 1, 1, 0), cb = transformed(0, 0, 0.9, 0.9, 0);
            std::vector<LayerDesc> ls;
            const LayerDesc la = clipLayer(ca, Time(0), &a_, W, H, tl.a), lb = clipLayer(cb, Time(0), &b_, W, H, tl.b);
            if (tl.bFirst) {
                if (tl.b.visible) ls.push_back(lb);
                if (tl.a.visible) ls.push_back(la);
            } else {
                if (tl.a.visible) ls.push_back(la);
                if (tl.b.visible) ls.push_back(lb);
            }
            const std::string what = std::string(info.label) + " p=" + std::to_string(p).substr(0, 3);
            expectClose(run(ls, what.c_str()));
        }
    }
}

TEST_P(GpuParity, ManyFramesReuseResources) {
    // canvas size changes and repeated frames keep working (resource reuse / re-creation)
    for (int i = 0; i < 20; ++i) {
        const int w = i % 2 ? 200 : 320, h = i % 2 ? 120 : 180;
        gpu_->beginFrame(w, h, Rgba8{0, 0, 0, 255});
        gpu_->drawLayer(clipLayer(transformed(0.01 * i, 0, 0.5, 0.5, i * 3.0), Time(0), &a_, w, h));
        const Image& g = gpu_->endFrame();
        cpu_->beginFrame(w, h, Rgba8{0, 0, 0, 255});
        cpu_->drawLayer(clipLayer(transformed(0.01 * i, 0, 0.5, 0.5, i * 3.0), Time(0), &a_, w, h));
        expectClose(compare(cpu_->endFrame(), g));
    }
}

// M4: a keyed picture (LayerDesc::imageKey) is copied to the GPU once and reused while the key comes back.
TEST_P(GpuParity, KeyedPicturesAreUploadedOnce) {
    Image pic(W, H, Rgba8{200, 40, 40, 255});
    LayerDesc l;
    l.image = &pic;
    l.imageKey = 42;
    auto frame = [&] {
        gpu_->beginFrame(W, H, Rgba8{0, 0, 0, 255});
        gpu_->drawLayer(l);
        return gpu_->endFrame().at(W / 2, H / 2);
    };
    const int64_t up0 = gpu_->textureUploads(), re0 = gpu_->textureReuses();
    EXPECT_EQ(frame(), (Rgba8{200, 40, 40, 255}));
    EXPECT_EQ(gpu_->textureUploads(), up0 + 1);
    EXPECT_EQ(frame(), (Rgba8{200, 40, 40, 255}));
    EXPECT_EQ(gpu_->textureUploads(), up0 + 1);   // the second frame reuses the texture ...
    EXPECT_EQ(gpu_->textureReuses(), re0 + 1);
    pic.reset(W, H, Rgba8{40, 40, 200, 255});
    EXPECT_EQ(frame(), (Rgba8{200, 40, 40, 255}));   // ... so the key is a promise: same key = same pixels
    l.imageKey = 43;
    EXPECT_EQ(frame(), (Rgba8{40, 40, 200, 255}));   // a new key uploads again
    EXPECT_EQ(gpu_->textureUploads(), up0 + 2);
    l.imageKey = 0;                                   // no key: an upload every frame
    frame();
    frame();
    EXPECT_EQ(gpu_->textureUploads(), up0 + 4);
}

INSTANTIATE_TEST_SUITE_P(Adapters, GpuParity, ::testing::Values(true, false),
                         [](const ::testing::TestParamInfo<bool>& i) { return i.param ? "HighPerformance" : "PowerSaving"; });

TEST(GpuSelection, AutoFallsBackAndReportsWhatItUses) {
    std::string used;
    auto cpu = makeBackend(BackendChoice::Cpu, &used);
    EXPECT_EQ(used, "cpu");
    auto any = makeBackend(BackendChoice::Auto, &used);
    ASSERT_TRUE(any);
    EXPECT_FALSE(used.empty());
    std::printf("  auto backend: %s\n", used.c_str());
    for (const auto& a : listGpuAdapters())
        std::printf("  adapter: %s, %zu MB%s%s%s\n", a.name.c_str(), a.dedicatedVideoMemory >> 20, a.software ? " (software)" : "",
                    a.highPerformanceChoice ? " [high performance]" : "", a.powerSavingChoice ? " [power saving]" : "");
}

// Auto = per-frame hybrid: plain frames stay on the CPU (no GPU work at all), frames with compositing
// work run on the GPU (GPU timestamp queries prove it executed there) and match the CPU reference.
TEST(GpuSelection, AutoRoutesCompositingWorkToTheGpu) {
    std::string why;
    const bool haveGpu = makeGpuBackend(&why) != nullptr;
    ASSERT_EQ(why.find("shader"), std::string::npos) << why;
    if (!haveGpu) GTEST_SKIP() << "no GPU backend: " << why;
    std::string used;
    auto be = makeBackend(BackendChoice::Auto, &used);
    ASSERT_NE(used.find("auto"), std::string::npos) << used;
    be->setTimingEnabled(true);
    const Image src = texture(1280, 720, 4);
    LayerDesc plain;
    plain.image = &src;   // identity placement at the canvas size
    be->beginFrame(1280, 720, Rgba8{0, 0, 0, 255});
    be->drawLayer(plain);
    const Image a = be->endFrame();
    EXPECT_LT(be->lastGpuMilliseconds(), 0.0) << "a plain frame must not use the GPU";
    Clip c;
    c.transform.rotation.setConstant(10);
    c.blend = BlendMode::Screen;
    const LayerDesc fx = clipLayer(c, Time(0), &src, 1280, 720);
    be->beginFrame(1280, 720, Rgba8{0, 0, 0, 255});
    be->drawLayer(plain);
    be->drawLayer(fx);
    const Image g = be->endFrame();
    EXPECT_GE(be->lastGpuMilliseconds(), 0.0) << "a frame with compositing work must run on the GPU";
    auto cpu = makeCpuBackend();
    cpu->beginFrame(1280, 720, Rgba8{0, 0, 0, 255});
    cpu->drawLayer(plain);
    cpu->drawLayer(fx);
    expectClose(compare(cpu->endFrame(), g));
}
