// M5 optical-flow frame interpolation (fa/vision/flow.h): moving objects land half way, occlusions fall back to the
// blend, real footage is predicted better than by blending, failures are reported.
#include "fa/media/media.h"
#include "fa/vision/flow.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>

using namespace fa;

namespace {

void put(Image& img, int x, int y, Rgba8 c) {
    uint8_t* p = img.row(y) + x * 4;
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = c.a;
}

// Smooth background + a 120 px noise-textured square at (x, 300).
Image scene(int x, uint32_t seed = 7) {
    Image img(960, 540);
    for (int y = 0; y < 540; ++y)
        for (int i = 0; i < 960; ++i)
            put(img, i, y, Rgba8{static_cast<uint8_t>(40 + i / 8), static_cast<uint8_t>(60 + y / 6), 90, 255});
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> d(0, 255);
    std::vector<uint8_t> tex(120 * 120);
    for (auto& t : tex) t = static_cast<uint8_t>(d(rng));
    for (int y = 0; y < 120; ++y)
        for (int i = 0; i < 120; ++i) {
            // blocky texture (6 px cells) so the flow has structure at the working size
            const uint8_t v = tex[(y / 6) * 120 + i / 6];
            put(img, x + i, 300 + y, Rgba8{v, static_cast<uint8_t>(255 - v), static_cast<uint8_t>(v / 2), 255});
        }
    return img;
}

double mad(const Image& a, const Image& b, int x0, int y0, int x1, int y1) {
    double s = 0;
    long n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x, ++n) {
            const Rgba8 p = a.at(x, y), q = b.at(x, y);
            s += std::abs(p.r - q.r) + std::abs(p.g - q.g) + std::abs(p.b - q.b);
        }
    return s / (3.0 * n);
}

Image blend(const Image& a, const Image& b, double w) {
    Image o(a.width, a.height);
    for (size_t i = 0; i < a.pixels.size(); ++i) o.pixels[i] = static_cast<uint8_t>(std::lround(a.pixels[i] * (1 - w) + b.pixels[i] * w));
    return o;
}

}  // namespace

TEST(OpticalFlow, MovingObjectLandsHalfWay) {
    const Image a = scene(300), b = scene(324), truth = scene(312);
    for (auto q : {vision::FlowQuality::Fast, vision::FlowQuality::Quality}) {
        Image mid;
        vision::FlowStats st;
        ASSERT_TRUE(vision::interpolateFrames(a, b, 0.5, mid, q, &st));
        const double flowErr = mad(mid, truth, 280, 290, 460, 430), blendErr = mad(blend(a, b, 0.5), truth, 280, 290, 460, 430);
        vision::FlowStats warm;   // timing of a second call (the first one sets OpenCV up)
        Image again;
        vision::interpolateFrames(a, b, 0.5, again, q, &warm);
        std::printf("  %s: object error %.2f (blend %.2f), mean motion %.1f px, fallback %.1f %%, %.1f ms (flow %.1f ms)\n",
                    q == vision::FlowQuality::Fast ? "fast" : "quality", flowErr, blendErr, st.meanMotion,
                    100 * st.fallbackShare, warm.milliseconds, warm.flowMilliseconds);
        EXPECT_LT(flowErr, blendErr / 3);   // the square sits at +12 px instead of a double image
        EXPECT_GT(st.fallbackShare, 0.0);   // the uncovered strip at the trailing edge is detected
        EXPECT_LT(st.fallbackShare, 0.2);
    }
    // w = 0 and 1 give the two input frames back (up to resampling)
    Image e0, e1;
    ASSERT_TRUE(vision::interpolateFrames(a, b, 0.0, e0));
    ASSERT_TRUE(vision::interpolateFrames(a, b, 1.0, e1));
    EXPECT_LT(mad(e0, a, 0, 0, 960, 540), 1.0);
    EXPECT_LT(mad(e1, b, 0, 0, 960, 540), 1.0);
}

// Real footage (Stage 1 tracking clip): frames n and n+2 predict frame n+1 better than their blend.
TEST(OpticalFlow, PredictsTheMissingFrameOfRealFootage) {
    VideoDecoder::Options o;
    o.maxWidth = 960;
    o.maxHeight = 540;
    VideoDecoder dec(std::filesystem::path(FA_TESTMEDIA_DIR) / "L_track_1080p30_5s.mp4", o);
    for (int n : {20, 70}) {
        const Image a = dec.frameAt(Rational(n, 30)).image;
        const Image truth = dec.frameAt(Rational(n + 1, 30)).image;
        const Image b = dec.frameAt(Rational(n + 2, 30)).image;
        Image mid;
        ASSERT_TRUE(vision::interpolateFrames(a, b, 0.5, mid, vision::FlowQuality::Quality));
        const double flowErr = mad(mid, truth, 0, 0, a.width, a.height), blendErr = mad(blend(a, b, 0.5), truth, 0, 0, a.width, a.height);
        std::printf("  L_track frame %d: interpolated error %.3f, blend %.3f\n", n + 1, flowErr, blendErr);
        EXPECT_LT(flowErr, blendErr);
    }
}

TEST(OpticalFlow, RefusesMismatchedFrames) {
    Image a(64, 36), b(32, 18), out;
    EXPECT_FALSE(vision::interpolateFrames(a, b, 0.5, out));
    EXPECT_TRUE(out.empty());
    // negative control: the error measure tells a wrong position from the right one
    EXPECT_GT(mad(scene(300), scene(312), 280, 290, 460, 430), 10.0);
}

// One flow serves every in-between time of a source pair (the renderer reuses it in slow motion): reusing it gives
// the same pictures as estimating it for each time, and a flow for frames of another size is refused.
TEST(OpticalFlow, OneFlowServesEveryInBetweenTime) {
    VideoDecoder::Options o;
    o.maxWidth = 960;
    o.maxHeight = 540;
    VideoDecoder dec(std::filesystem::path(FA_TESTMEDIA_DIR) / "L_track_1080p30_5s.mp4", o);
    const Image a = dec.frameAt(Rational(40, 30)).image;
    const Image b = dec.frameAt(Rational(41, 30)).image;
    const auto flow = vision::estimateFlow(a, b, vision::FlowQuality::Fast);
    ASSERT_TRUE(flow);
    for (double w : {0.25, 0.5, 0.75}) {
        Image reused, fresh;
        ASSERT_TRUE(vision::warpWithFlow(a, b, *flow, w, reused));
        ASSERT_TRUE(vision::interpolateFrames(a, b, w, fresh, vision::FlowQuality::Fast));
        const double d = mad(reused, fresh, 0, 0, a.width, a.height);
        std::printf("  w %.2f: reused flow vs fresh %.4f\n", w, d);
        EXPECT_LT(d, 0.05);
    }
    const Image small(320, 180);
    Image out;
    EXPECT_FALSE(vision::warpWithFlow(small, small, *flow, 0.5, out));
    EXPECT_TRUE(out.empty());
}
