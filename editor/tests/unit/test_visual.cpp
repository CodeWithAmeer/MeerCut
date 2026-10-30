// M2 visual features on the CPU reference backend: transforms, keyframed motion, crop, masks, blend
// modes and transitions. Expected values come from independent geometry / formulas in this file.
#include "fa/model/keyframes.h"
#include "fa/model/serialize.h"
#include "fa/render/renderer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <thread>

using namespace fa;

namespace {

constexpr int W = 160, H = 90;   // canvas and source size (16:9)

// Source: black frame with a white 20x20 square centred and a red 6x6 marker in the top-left corner.
Image patternImage() {
    Image img(W, H, Rgba8{0, 0, 0, 255});
    for (int y = 35; y < 55; ++y)
        for (int x = 70; x < 90; ++x) img.set(x, y, Rgba8{255, 255, 255, 255});
    for (int y = 2; y < 8; ++y)
        for (int x = 2; x < 8; ++x) img.set(x, y, Rgba8{255, 0, 0, 255});
    return img;
}

struct Stats {
    double cx = 0, cy = 0, area = 0;
};
Stats whiteStats(const Image& img) {   // centroid/area of bright, neutral pixels (the white square)
    Stats s;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            const Rgba8 c = img.at(x, y);
            const double w = (c.r > 128 && c.g > 128 && c.b > 128) ? 1.0 : 0.0;
            s.cx += w * (x + 0.5);
            s.cy += w * (y + 0.5);
            s.area += w;
        }
    if (s.area > 0) {
        s.cx /= s.area;
        s.cy /= s.area;
    }
    return s;
}
Stats redStats(const Image& img) {
    Stats s;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            const Rgba8 c = img.at(x, y);
            const double w = (c.r > 128 && c.g < 80) ? 1.0 : 0.0;
            s.cx += w * (x + 0.5);
            s.cy += w * (y + 0.5);
            s.area += w;
        }
    if (s.area > 0) {
        s.cx /= s.area;
        s.cy /= s.area;
    }
    return s;
}

Image render(const Clip& clip, const Image& src, const Time& t = Time(0)) {
    auto be = makeCpuBackend();
    be->beginFrame(W, H, Rgba8{0, 0, 0, 255});
    be->drawLayer(clipLayer(clip, t, &src, W, H));
    return be->endFrame();
}

Clip baseClip() {
    Clip c;
    c.id = "c";
    c.sourceId = "s";
    c.duration = Rational(10);
    return c;
}

Keyframe<double> key(Rational t, double v, Interpolation i = Interpolation::Linear, TangentMode m = TangentMode::Linear) {
    Keyframe<double> k;
    k.time = t;
    k.value = v;
    k.interp = i;
    k.tangent = m;
    return k;
}

}  // namespace

TEST(Transform, IdentityIsPixelExact) {
    const Image src = patternImage();
    const Image out = render(baseClip(), src);
    EXPECT_EQ(out.pixels, src.pixels);
}

TEST(Transform, PositionMovesByCanvasFractions) {
    const Image src = patternImage();
    Clip c = baseClip();
    c.transform.positionX.setConstant(0.25);    // +40 px
    c.transform.positionY.setConstant(-0.1);    // -9 px
    const Stats s = whiteStats(render(c, src));
    EXPECT_NEAR(s.cx, 80 + 40, 0.01);
    EXPECT_NEAR(s.cy, 45 - 9, 0.01);
    EXPECT_NEAR(s.area, 400, 1);
}

TEST(Transform, ScaleAboutCentreAndAnchor) {
    const Image src = patternImage();
    Clip c = baseClip();
    c.transform.scaleX.setConstant(0.5);   // uniform: scaleY follows
    Stats s = whiteStats(render(c, src));
    EXPECT_NEAR(s.cx, 80, 0.3);
    EXPECT_NEAR(s.cy, 45, 0.3);
    EXPECT_NEAR(s.area, 100, 12);           // 10x10 (edge pixels anti-aliased)
    // pivot at the top-left corner of the layer: the corner stays, the square moves towards it
    c.transform.anchorX.setConstant(-0.5);
    c.transform.anchorY.setConstant(-0.5);
    s = whiteStats(render(c, src));
    EXPECT_NEAR(s.cx, 40, 0.3);             // 80 * 0.5
    EXPECT_NEAR(s.cy, 22.5, 0.3);
    // non-uniform
    Clip n = baseClip();
    n.transform.uniformScale = false;
    n.transform.scaleX.setConstant(2.0);
    n.transform.scaleY.setConstant(1.0);
    s = whiteStats(render(n, src));
    EXPECT_NEAR(s.area, 800, 4);
}

TEST(Transform, RotationIsClockwiseAboutTheCentre) {
    const Image src = patternImage();
    Clip c = baseClip();
    c.transform.rotation.setConstant(90);
    const Stats r = redStats(render(c, src));
    // marker at (5, 5) relative to centre (80, 45): (-75, -40) -> clockwise 90 deg -> (40, -75)... outside
    // the canvas vertically, so use 180 deg instead for a visible check
    (void)r;
    c.transform.rotation.setConstant(180);
    const Stats r180 = redStats(render(c, src));
    EXPECT_NEAR(r180.cx, 160 - 5, 0.5);
    EXPECT_NEAR(r180.cy, 90 - 5, 0.5);
    // 90 deg clockwise with scale 0.4 keeps the marker visible: (-75,-40)*0.4 = (-30,-16) -> (16, -30)
    c.transform.rotation.setConstant(90);
    c.transform.scaleX.setConstant(0.4);
    const Stats r90 = redStats(render(c, src));
    EXPECT_NEAR(r90.cx, 80 + 16, 0.8);
    EXPECT_NEAR(r90.cy, 45 - 30, 0.8);
}

TEST(Transform, FlipAndCrop) {
    const Image src = patternImage();
    Clip c = baseClip();
    c.transform.flipH = true;
    Stats r = redStats(render(c, src));
    EXPECT_NEAR(r.cx, 160 - 5, 0.5);
    EXPECT_NEAR(r.cy, 5, 0.5);
    c.transform.flipH = false;
    c.transform.flipV = true;
    r = redStats(render(c, src));
    EXPECT_NEAR(r.cy, 90 - 5, 0.5);
    // crop the left half: the square's left half disappears, the picture does not move
    Clip k = baseClip();
    k.transform.cropLeft = 0.5;
    const Image out = render(k, src);
    const Stats s = whiteStats(out);
    EXPECT_NEAR(s.area, 200, 4);
    EXPECT_NEAR(s.cx, 85, 0.6);
    EXPECT_EQ(redStats(out).area, 0);
}

TEST(Transform, FitsSmallerSourcesThenTransforms) {
    // 80x45 source on a 160x90 canvas: fitted to 100 % = canvas size, so positions are the same
    Image small(80, 45, Rgba8{0, 0, 0, 255});
    for (int y = 17; y < 27; ++y)
        for (int x = 35; x < 45; ++x) small.set(x, y, Rgba8{255, 255, 255, 255});
    Clip c = baseClip();
    c.transform.positionX.setConstant(0.25);
    const Stats s = whiteStats(render(c, small));
    EXPECT_NEAR(s.cx, 120, 0.5);
    EXPECT_NEAR(s.area, 400, 8);
}

TEST(Keyframes, AnimatedPositionMovesTheLayer) {
    const Image src = patternImage();
    Clip c = baseClip();
    c.sourceIn = Rational(2);
    c.transform.positionX.setKey(key(Rational(2), 0.0));
    c.transform.positionX.setKey(key(Rational(4), 0.25));
    // linear: half way at source 3 s
    EXPECT_NEAR(whiteStats(render(c, src, Rational(3))).cx, 100, 0.05);
    EXPECT_NEAR(whiteStats(render(c, src, Rational(4))).cx, 120, 0.05);
    // hold: no rotation before the second key, 180 at it
    Clip h = baseClip();
    h.transform.rotation.setKey(key(Rational(2), 0.0, Interpolation::Hold));
    h.transform.rotation.setKey(key(Rational(4), 180.0, Interpolation::Hold));
    EXPECT_NEAR(redStats(render(h, src, Rational(399, 100))).cy, 5, 0.6);
    EXPECT_NEAR(redStats(render(h, src, Rational(4))).cy, 85, 0.6);
    // ease (bezier, flat tangents): slower than linear at a quarter of the way
    keys::setStyle(c.transform.positionX, Rational(2), keys::Style::Ease);
    keys::setStyle(c.transform.positionX, Rational(4), keys::Style::Ease);
    const double eased = whiteStats(render(c, src, Rational(5, 2))).cx - 80;
    EXPECT_LT(eased, 10.0 - 2.0);   // linear would be 10 px
    EXPECT_GT(eased, 0.5);
}

TEST(Masks, EllipseRectanglePolygonInvertFeather) {
    Image white(W, H, Rgba8{255, 255, 255, 255});
    auto alphaAt = [&](const Clip& c, int x, int y) { return render(c, white).at(x, y).r / 255.0; };
    Clip c = baseClip();
    Mask m;
    m.id = "m1";
    m.shape = MaskShape::Ellipse;   // default 0.5 x 0.5 layer heights = circle of radius 22.5 px
    c.masks.push_back(m);
    EXPECT_NEAR(alphaAt(c, 80, 45), 1.0, 0.01);
    EXPECT_NEAR(alphaAt(c, 80 + 20, 45), 1.0, 0.01);
    EXPECT_NEAR(alphaAt(c, 80 + 25, 45), 0.0, 0.01);
    EXPECT_NEAR(alphaAt(c, 80, 45 + 25), 0.0, 0.01);
    // feather: Gaussian edge -> 0.5 on the boundary
    c.masks[0].feather.setConstant(0.05);
    EXPECT_NEAR(alphaAt(c, 80 + 22, 45), 0.5, 0.08);
    // invert: complement
    c.masks[0].feather.setConstant(0.0);
    c.masks[0].inverted = true;
    EXPECT_NEAR(alphaAt(c, 80, 45), 0.0, 0.01);
    EXPECT_NEAR(alphaAt(c, 5, 5), 1.0, 0.01);
    // rectangle 1.0 x 0.5 layer heights = 90 x 45 px, rotated 90 deg -> 45 wide, 90 tall
    Clip r = baseClip();
    Mask rm;
    rm.id = "r";
    rm.shape = MaskShape::Rectangle;
    rm.width.setConstant(1.0);
    rm.height.setConstant(0.5);
    r.masks.push_back(rm);
    EXPECT_NEAR(alphaAt(r, 80 + 40, 45), 1.0, 0.01);
    EXPECT_NEAR(alphaAt(r, 80, 45 + 20), 1.0, 0.01);
    EXPECT_NEAR(alphaAt(r, 80, 45 + 30), 0.0, 0.01);
    r.masks[0].rotation.setConstant(90);
    EXPECT_NEAR(alphaAt(r, 80 + 40, 45), 0.0, 0.01);
    EXPECT_NEAR(alphaAt(r, 80, 45 + 40), 1.0, 0.01);
    // polygon: triangle (0,-0.5) (0.5,0.5) (-0.5,0.5) scaled to 0.8 layer heights
    Clip p = baseClip();
    Mask pm;
    pm.id = "p";
    pm.shape = MaskShape::Polygon;
    pm.width.setConstant(0.8);
    pm.height.setConstant(0.8);
    pm.points = {{0, -0.5}, {0.5, 0.5}, {-0.5, 0.5}};
    p.masks.push_back(pm);
    EXPECT_NEAR(alphaAt(p, 80, 45 + 20), 1.0, 0.01);        // inside near the base
    EXPECT_NEAR(alphaAt(p, 80 + 30, 45 - 30), 0.0, 0.01);   // outside the slanted edge
    EXPECT_NEAR(alphaAt(p, 80, 45 - 30), 1.0, 0.01);        // near the apex (apex at y = 45 - 36)
}

TEST(Masks, CombineModesAndMaskOpacity) {
    Image white(W, H, Rgba8{255, 255, 255, 255});
    Clip c = baseClip();
    Mask big, hole;
    big.id = "a";
    big.shape = MaskShape::Rectangle;
    big.width.setConstant(1.6);
    big.height.setConstant(0.8);
    hole.id = "b";
    hole.shape = MaskShape::Ellipse;
    hole.width.setConstant(0.4);
    hole.height.setConstant(0.4);
    hole.mode = MaskMode::Subtract;
    c.masks = {big, hole};
    Image out = render(c, white);
    EXPECT_EQ(out.at(80, 45).r, 0);            // hole
    EXPECT_EQ(out.at(80 + 30, 45).r, 255);     // ring
    EXPECT_EQ(out.at(5, 5).r, 0);              // outside the rectangle
    c.masks[1].mode = MaskMode::Intersect;     // only the overlap remains
    out = render(c, white);
    EXPECT_EQ(out.at(80, 45).r, 255);
    EXPECT_EQ(out.at(80 + 30, 45).r, 0);
    c.masks = {big};
    c.masks[0].opacity.setConstant(0.5);
    out = render(c, white);
    EXPECT_NEAR(out.at(80, 45).r, 128, 1);
}

TEST(Masks, FollowTheLayerTransform) {
    Image white(W, H, Rgba8{255, 255, 255, 255});
    Clip c = baseClip();
    Mask m;
    m.id = "m";
    c.masks.push_back(m);
    c.transform.positionX.setConstant(0.25);
    const Image out = render(c, white);
    EXPECT_EQ(out.at(80 + 40, 45).r, 255);
    EXPECT_EQ(out.at(80, 45).r, 0);
}

namespace {
float refBlend(BlendMode m, float b, float s) {   // independent formulas (W3C compositing spec / Photoshop)
    switch (m) {
        case BlendMode::Normal: return s;
        case BlendMode::Multiply: return b * s;
        case BlendMode::Screen: return 1 - (1 - b) * (1 - s);
        case BlendMode::Overlay: return b < 0.5f ? 2 * s * b : 1 - 2 * (1 - s) * (1 - b);
        case BlendMode::Darken: return b < s ? b : s;
        case BlendMode::Lighten: return b > s ? b : s;
        case BlendMode::ColorDodge: return s == 1 ? 1 : std::min(1.0f, b / (1 - s));
        case BlendMode::ColorBurn: return s == 0 ? 0 : 1 - std::min(1.0f, (1 - b) / s);
        case BlendMode::HardLight: return s < 0.5f ? 2 * s * b : 1 - 2 * (1 - s) * (1 - b);
        case BlendMode::SoftLight: return s <= 0.5f ? b - (1 - 2 * s) * b * (1 - b) : b + (2 * s - 1) * (std::sqrt(b) - b);
        case BlendMode::Difference: return std::fabs(s - b);
        case BlendMode::Exclusion: return s + b - 2 * s * b;
        case BlendMode::LinearDodge: return std::min(1.0f, s + b);
        case BlendMode::LinearBurn: return std::max(0.0f, s + b - 1);
    }
    return s;
}
}  // namespace

TEST(Blend, AllModesMatchTheReferenceFormulas) {
    const Rgba8 under{153, 77, 204, 255}, over{128, 204, 26, 255};
    Image a(W, H, under), b(W, H, over);
    for (int mode = 0; mode < kBlendModeCount; ++mode) {
        const auto bm = static_cast<BlendMode>(mode);
        for (double op : {1.0, 0.5}) {
            Clip top = baseClip();
            top.blend = bm;
            top.opacity.setConstant(op);
            auto be = makeCpuBackend();
            be->beginFrame(W, H, Rgba8{0, 0, 0, 255});
            be->drawLayer(clipLayer(baseClip(), Time(0), &a, W, H));
            be->drawLayer(clipLayer(top, Time(0), &b, W, H));
            const Rgba8 got = be->endFrame().at(10, 10);
            const uint8_t u[3] = {under.r, under.g, under.b}, o[3] = {over.r, over.g, over.b}, g[3] = {got.r, got.g, got.b};
            for (int ch = 0; ch < 3; ++ch) {
                const float bb = u[ch] / 255.0f, ss = o[ch] / 255.0f;
                const float want = (bb * (1 - float(op)) + refBlend(bm, bb, ss) * float(op)) * 255.0f;
                EXPECT_NEAR(g[ch], want, 1.0) << blendModeLabel(bm) << " opacity " << op << " channel " << ch;
            }
        }
    }
}

namespace {
class SolidProvider : public FrameProvider {
public:
    std::map<std::string, Image> images;
    Result frame(const Clip& clip, const MediaSource&, const Time& t) override {
        Result r;
        r.image = &images.at(clip.sourceId);
        r.requestedTime = t;
        return r;
    }
};

Project transitionProject(const std::string& type, TransitionDirection dir = TransitionDirection::Left, double soft = 0.0) {
    Project p;
    p.sources = {MediaSource{"A", "a", Rational(10), FrameRate::fps30(), W, H, true, false},
                 MediaSource{"B", "b", Rational(10), FrameRate::fps30(), W, H, true, false}};
    Sequence s;
    s.id = "s";
    s.width = W;
    s.height = H;
    s.frameRate = FrameRate::fps30();
    Track v;
    v.id = "v";
    Clip a = baseClip(), b = baseClip();
    a.id = "a";
    a.sourceId = "A";
    b.id = "b";
    b.sourceId = "B";
    b.timelineStart = Rational(10);
    TransitionRef t{type, Rational(2), TransitionAlignment::Centered};
    t.direction = dir;
    t.softness = soft;
    a.outTransition = t;
    v.clips = {a, b};
    s.tracks.push_back(v);
    p.sequences.push_back(s);
    return p;
}

Image renderAt(const Project& p, int64_t frame) {
    SolidProvider prov;
    prov.images["A"] = Image(W, H, Rgba8{200, 0, 0, 255});
    prov.images["B"] = Image(W, H, Rgba8{0, 0, 200, 255});
    auto be = makeCpuBackend();
    return renderFrame(p.sequences[0], frame, p, prov, *be);
}
}  // namespace

// span 9..11 s, frames 270..329; progress p at frame 270 + 60p
TEST(Transitions, FadeThroughBlack) {
    const Project p = transitionProject("fa.transition.dip_to_black");
    EXPECT_NEAR(renderAt(p, 285).at(80, 45).r, 100, 1);   // p = 0.25: A at half brightness
    const Rgba8 mid = renderAt(p, 300).at(80, 45);         // p = 0.5: black
    EXPECT_LE(mid.r + mid.b, 1);
    EXPECT_NEAR(renderAt(p, 315).at(80, 45).b, 100, 1);    // p = 0.75: B at half brightness
}

TEST(Transitions, WipeRevealsFromTheLeadingEdge) {
    const Project p = transitionProject("fa.transition.wipe", TransitionDirection::Left, 0.0);
    const Image half = renderAt(p, 300);   // p = 0.5, travelling left: B on the right half
    EXPECT_EQ(half.at(150, 45).b, 200);
    EXPECT_EQ(half.at(10, 45).r, 200);
    const Project soft = transitionProject("fa.transition.wipe", TransitionDirection::Down, 0.2);
    const Image s = renderAt(soft, 300);  // travelling down: B from the top; edge at t = 0.6 with 0.2 softness
    EXPECT_EQ(s.at(80, 5).b, 200);
    EXPECT_EQ(s.at(80, 85).r, 200);
    EXPECT_NEAR(s.at(80, 45).b / 200.0, 0.5, 0.1);   // t = 0.5 -> (0.6 - 0.5) / 0.2
}

TEST(Transitions, SlideAndPushUseEasedOffsets) {
    const Project slide = transitionProject("fa.transition.slide", TransitionDirection::Left);
    Image img = renderAt(slide, 300);                // eased 0.5 -> B covers the right half, A untouched below
    EXPECT_EQ(img.at(10, 45).r, 200);
    EXPECT_EQ(img.at(150, 45).b, 200);
    img = renderAt(slide, 285);                      // p = 0.25 -> e = 8 * 0.25^4 = 0.03125: B almost off-screen
    const int edge = static_cast<int>(std::lround(W * (1 - 0.03125)));
    EXPECT_EQ(img.at(edge - 3, 45).r, 200);
    EXPECT_EQ(img.at(edge + 2, 45).b, 200);
    const Project push = transitionProject("fa.transition.push", TransitionDirection::Right);
    img = renderAt(push, 300);                       // A pushed right: A on the right half, B on the left
    EXPECT_EQ(img.at(150, 45).r, 200);
    EXPECT_EQ(img.at(10, 45).b, 200);
}

TEST(Transitions, ZoomEndsOnTheIncomingClip) {
    const Project z = transitionProject("fa.transition.zoom");
    EXPECT_EQ(renderAt(z, 270).at(80, 45).r, 200);   // p = 0: A
    const Rgba8 m = renderAt(z, 300).at(80, 45);      // half way: mix
    EXPECT_GT(m.r, 40);
    EXPECT_GT(m.b, 40);
    EXPECT_EQ(renderAt(z, 330).at(80, 45).b, 200);   // after the span: B
    // the catalogue: the six M2 transitions, then M11's thirteen
    std::vector<std::string> ids;
    for (const auto& t : transitionTypes()) ids.push_back(t.id);
    EXPECT_EQ(ids, (std::vector<std::string>{
                       "fa.transition.dissolve", "fa.transition.dip_to_black", "fa.transition.wipe", "fa.transition.slide",
                       "fa.transition.push", "fa.transition.zoom", "fa.transition.dip_to_white", "fa.transition.flash",
                       "fa.transition.circle_open", "fa.transition.circle_close", "fa.transition.clock",
                       "fa.transition.barn_door", "fa.transition.diamond", "fa.transition.blinds", "fa.transition.diagonal",
                       "fa.transition.spin", "fa.transition.zoom_in", "fa.transition.blur", "fa.transition.pixelate"}));
}

// ---- M11 transitions -------------------------------------------------------------------------------------------

// Where each wipe shape starts (0) and ends (1) on a 16:9 canvas.
TEST(M11Transitions, WipeShapesRevealInTheirOrder) {
    const double a = 16.0 / 9.0;
    WipeParams w;
    w.shape = WipeShape::Circle;
    EXPECT_NEAR(wipeOrder(w, 0.5, 0.5, a), 0.0, 1e-12);
    EXPECT_NEAR(wipeOrder(w, 0, 0, a), 1.0, 1e-12);
    EXPECT_NEAR(wipeOrder(w, 1, 1, a), 1.0, 1e-12);
    EXPECT_NEAR(wipeOrder(w, 0.5, 0.0, a), wipeOrder(w, 0.5 + 0.5 / a, 0.5, a), 1e-12);   // round on the canvas
    // ... also between the axes (a diamond passes the checks above): a point on the 45-degree line is revealed together
    // with a point on an axis as far from the centre (0.3 of the height)
    const double r = 0.3, d = r * std::sqrt(0.5);
    const double axis = wipeOrder(w, 0.5, 0.5 - r, a), diagonal = wipeOrder(w, 0.5 + d / a, 0.5 - d, a);
    EXPECT_NEAR(diagonal, axis, 1e-12);
    w.shape = WipeShape::Diamond;
    EXPECT_NEAR(wipeOrder(w, 0.5, 0.5, a), 0.0, 1e-12);
    EXPECT_NEAR(wipeOrder(w, 1, 0, a), 1.0, 1e-12);
    EXPECT_NEAR(wipeOrder(w, 0.5, 0.0, a), wipeOrder(w, 0.5 + 0.5 / a, 0.5, a), 1e-12);
    // a diamond reaches the 45-degree point later (by the square root of 2), and a point with the same |dx| + |dy| at once
    EXPECT_NEAR(wipeOrder(w, 0.5 + d / a, 0.5 - d, a), std::sqrt(2.0) * wipeOrder(w, 0.5, 0.5 - r, a), 1e-12);
    EXPECT_NEAR(wipeOrder(w, 0.5 + 0.1 / a, 0.5 - 0.2, a), wipeOrder(w, 0.5, 0.5 - 0.3, a), 1e-12);
    w.shape = WipeShape::Split;   // vertical doors for a horizontal direction
    w.dirX = 1;
    w.dirY = 0;
    EXPECT_NEAR(wipeOrder(w, 0.5, 0.1, a), 0.0, 1e-12);
    EXPECT_NEAR(wipeOrder(w, 0.0, 0.7, a), 1.0, 1e-12);
    EXPECT_NEAR(wipeOrder(w, 0.75, 0.0, a), 0.5, 1e-12);
    w.dirX = 0;
    w.dirY = 1;
    EXPECT_NEAR(wipeOrder(w, 0.1, 0.5, a), 0.0, 1e-12);
    EXPECT_NEAR(wipeOrder(w, 0.1, 1.0, a), 1.0, 1e-12);
    w.shape = WipeShape::Clock;   // clockwise from 12 o'clock
    EXPECT_NEAR(wipeOrder(w, 0.5 + 1e-9, 0.0, a), 0.0, 1e-6);
    EXPECT_NEAR(wipeOrder(w, 1.0, 0.5, a), 0.25, 1e-12);
    EXPECT_NEAR(wipeOrder(w, 0.5, 1.0, a), 0.5, 1e-12);
    EXPECT_NEAR(wipeOrder(w, 0.0, 0.5, a), 0.75, 1e-12);
    EXPECT_NEAR(wipeOrder(w, 0.5 - 1e-9, 0.0, a), 1.0, 1e-6);
    w.shape = WipeShape::Blinds;   // ten stripes opening together
    w.stripes = 10;
    w.dirX = 1;
    w.dirY = 0;
    EXPECT_NEAR(wipeOrder(w, 0.0, 0.3, a), 0.0, 1e-12);
    EXPECT_NEAR(wipeOrder(w, 0.05, 0.3, a), 0.5, 1e-9);
    EXPECT_NEAR(wipeOrder(w, 0.15, 0.9, a), 0.5, 1e-9);
    w.dirX = -1;   // each stripe opens from its other side
    EXPECT_NEAR(wipeOrder(w, 0.02, 0.3, a), 0.8, 1e-9);
}

namespace {
bool isA(Rgba8 c) { return c.r > 150 && c.g < 50 && c.b < 50; }
bool isB(Rgba8 c) { return c.b > 150 && c.r < 50 && c.g < 50; }
Image renderWith(const Project& p, int64_t frame, const Image& a, const Image& b) {
    SolidProvider prov;
    prov.images["A"] = a;
    prov.images["B"] = b;
    auto be = makeCpuBackend();
    return renderFrame(p.sequences[0], frame, p, prov, *be);
}
}  // namespace

// Rendered through the timeline (span 9..11 s, frames 270..329, p = 0.5 at frame 300): A red, B blue.
TEST(M11Transitions, ShapesRevealTheNextClipWhereTheyShould) {
    for (const auto& info : transitionTypes()) {   // every transition starts on A and ends on B
        SCOPED_TRACE(info.id);
        const Project p = transitionProject(info.id, TransitionDirection::Right);
        EXPECT_TRUE(isA(renderAt(p, 269).at(80, 45)));
        EXPECT_TRUE(isB(renderAt(p, 330).at(80, 45)));
    }
    const Image open = renderAt(transitionProject("fa.transition.circle_open"), 300);
    EXPECT_TRUE(isB(open.at(80, 45)));   // the centre first
    EXPECT_TRUE(isA(open.at(2, 2)));     // the corners last
    const Image close = renderAt(transitionProject("fa.transition.circle_close"), 300);
    EXPECT_TRUE(isA(close.at(80, 45)));
    EXPECT_TRUE(isB(close.at(2, 2)));
    const Image clock = renderAt(transitionProject("fa.transition.clock"), 285);   // a quarter turn: 12 to 3 o'clock
    EXPECT_TRUE(isB(clock.at(120, 20)));
    EXPECT_TRUE(isA(clock.at(40, 20)));
    EXPECT_TRUE(isA(clock.at(120, 70)));
    const Image doors = renderAt(transitionProject("fa.transition.barn_door", TransitionDirection::Right), 300);
    EXPECT_TRUE(isB(doors.at(80, 10)));
    EXPECT_TRUE(isA(doors.at(2, 45)));
    EXPECT_TRUE(isA(doors.at(157, 45)));
    const Image flat = renderAt(transitionProject("fa.transition.barn_door", TransitionDirection::Down), 300);
    EXPECT_TRUE(isB(flat.at(5, 45)));
    EXPECT_TRUE(isA(flat.at(80, 2)));
    const Image diamond = renderAt(transitionProject("fa.transition.diamond"), 300);
    EXPECT_TRUE(isB(diamond.at(80, 45)));
    EXPECT_TRUE(isB(diamond.at(80, 2)));
    EXPECT_TRUE(isA(diamond.at(2, 2)));
    const Image blinds = renderAt(transitionProject("fa.transition.blinds", TransitionDirection::Right), 300);
    for (int stripe : {0, 5, 9}) {   // 16 px stripes, each half open
        EXPECT_TRUE(isB(blinds.at(stripe * 16 + 3, 40))) << stripe;
        EXPECT_TRUE(isA(blinds.at(stripe * 16 + 12, 40))) << stripe;
    }
    const Image diagonal = renderAt(transitionProject("fa.transition.diagonal", TransitionDirection::Right), 300);
    EXPECT_TRUE(isB(diagonal.at(2, 2)));
    EXPECT_TRUE(isA(diagonal.at(157, 87)));
    const Image zoomIn = renderAt(transitionProject("fa.transition.zoom_in"), 300);
    EXPECT_TRUE(isB(zoomIn.at(80, 45)));
    EXPECT_TRUE(isA(zoomIn.at(2, 2)));
    const Image spin = renderAt(transitionProject("fa.transition.spin"), 300);   // half way: both turned, shrunk
    EXPECT_GT(spin.at(80, 45).r, 60);
    EXPECT_GT(spin.at(80, 45).b, 60);
    EXPECT_EQ(spin.at(2, 2).r + spin.at(2, 2).g + spin.at(2, 2).b, 0);   // the background shows in the corners
}

TEST(M11Transitions, DipToWhiteAndFlash) {
    const Project dip = transitionProject("fa.transition.dip_to_white");
    const Rgba8 quarter = renderAt(dip, 285).at(80, 45);   // A half way to white
    EXPECT_NEAR(quarter.r, 228, 1);
    EXPECT_NEAR(quarter.g, 128, 1);
    EXPECT_NEAR(quarter.b, 128, 1);
    const Rgba8 mid = renderAt(dip, 300).at(80, 45);
    EXPECT_EQ(mid.r + mid.g + mid.b, 3 * 255);
    const Rgba8 late = renderAt(dip, 315).at(80, 45);   // B half way from white
    EXPECT_NEAR(late.r, 128, 1);
    EXPECT_NEAR(late.b, 228, 1);
    const Project flash = transitionProject("fa.transition.flash");
    EXPECT_TRUE(isA(renderAt(flash, 276).at(80, 45)));   // p = 0.1: no flash yet
    const Rgba8 rising = renderAt(flash, 291).at(80, 45);   // p = 0.35: A half white
    EXPECT_NEAR(rising.r, 228, 1);
    EXPECT_NEAR(rising.g, 128, 1);
    const Rgba8 peak = renderAt(flash, 300).at(80, 45);
    EXPECT_EQ(peak.r + peak.g + peak.b, 3 * 255);
    const Rgba8 falling = renderAt(flash, 309).at(80, 45);   // p = 0.65: B half white
    EXPECT_NEAR(falling.b, 228, 1);
    EXPECT_NEAR(falling.g, 128, 1);
}

TEST(M11Transitions, BlurAndPixelateWorkOnThePicture) {
    Image edge(W, H, Rgba8{0, 0, 0, 255});   // white left half, black right half
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W / 2; ++x) edge.set(x, y, Rgba8{255, 255, 255, 255});
    const Project blur = transitionProject("fa.transition.blur");
    EXPECT_EQ(renderWith(blur, 270, edge, edge).at(79, 45).r, 255);   // sharp before
    const Image mid = renderWith(blur, 300, edge, edge);               // 60 px at 1080 = 5 px here
    EXPECT_LT(mid.at(79, 45).r, 230);
    EXPECT_GT(mid.at(80, 45).r, 25);
    EXPECT_EQ(renderWith(blur, 330, edge, edge).at(80, 45).r, 0);     // sharp after
    Image checker(W, H, Rgba8{0, 0, 0, 255});   // 2 px squares
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if ((x / 2 + y / 2) % 2) checker.set(x, y, Rgba8{255, 255, 255, 255});
    const Project pix = transitionProject("fa.transition.pixelate");
    const uint8_t before = renderWith(pix, 269, checker, checker).at(10, 10).r;
    EXPECT_TRUE(before == 0 || before == 255);
    const Image blocks = renderWith(pix, 300, checker, checker);   // 81 px at 1080 = 6.75 px blocks: averaged
    for (int x : {10, 40, 100}) EXPECT_NEAR(blocks.at(x, 40).r, 128, 40) << x;
}

TEST(VisualModel, SerializationRoundTripAndVersion) {
    Project p = transitionProject("fa.transition.wipe", TransitionDirection::Up, 0.25);
    Clip& c = p.sequences[0].tracks[0].clips[0];
    c.transform.positionX.setKey(key(Rational(1), 0.1));
    c.transform.positionX.setKey(key(Rational(2), -0.2, Interpolation::Bezier, TangentMode::Auto));
    c.transform.rotation.setConstant(12.5);
    c.transform.uniformScale = false;
    c.transform.scaleY.setConstant(1.5);
    c.transform.flipV = true;
    c.transform.cropRight = 0.1;
    c.blend = BlendMode::Overlay;
    Mask m;
    m.id = "m1";
    m.shape = MaskShape::Polygon;
    m.points = {{0, -0.5}, {0.5, 0.5}, {-0.5, 0.5}};
    m.mode = MaskMode::Subtract;
    m.inverted = true;
    m.feather.setKey(key(Rational(0), 0.0));
    m.feather.setKey(key(Rational(3), 0.1));
    c.masks.push_back(m);
    const std::string json = projectToJsonString(p);
    const std::string current = "\"version\": " + std::to_string(Project::kFormatVersion);
    EXPECT_NE(json.find(current), std::string::npos);
    EXPECT_EQ(projectFromJsonString(json), p);
    // a version 1 file (M0/M1) still loads, with default visual properties
    std::string v1 = json;
    v1.replace(v1.find(current), current.size(), "\"version\": 1");
    EXPECT_NO_THROW(projectFromJsonString(v1));
    EXPECT_TRUE(Clip{}.transform.isDefault());
}

TEST(VisualModel, KeyframeHelpers) {
    Clip c = baseClip();
    Animatable<double>& x = c.transform.positionX;
    keys::setValueAt(x, Rational(1), 0.3);   // not animated: constant
    EXPECT_FALSE(x.isAnimated());
    EXPECT_DOUBLE_EQ(x.constantValue(), 0.3);
    keys::toggleKey(x, Rational(1));          // first key holds the current value
    keys::setValueAt(x, Rational(3), 0.5);    // auto-key
    EXPECT_EQ(x.keys().size(), 2u);
    EXPECT_NEAR(x.value(2.0), 0.4, 1e-12);
    keys::setStyleAt(c, Rational(1), keys::Style::Hold);
    EXPECT_NEAR(x.value(2.0), 0.3, 1e-12);
    EXPECT_EQ(keys::styleAt(x, Rational(1)), keys::Style::Hold);
    keys::toggleKey(c.opacity, Rational(3));
    EXPECT_EQ(keys::keyTimes(c), (std::vector<Time>{Rational(1), Rational(3)}));
    keys::moveKeysAt(c, Rational(3), Rational(4));
    EXPECT_EQ(keys::keyTimes(c), (std::vector<Time>{Rational(1), Rational(4)}));
    EXPECT_THROW(keys::moveKeysAt(c, Rational(1), Rational(4)), std::runtime_error);
    keys::removeKeysAt(c, Rational(4));
    EXPECT_EQ(keys::keyTimes(c), (std::vector<Time>{Rational(1)}));
    keys::toggleKey(x, Rational(1));          // last key removed -> constant again
    EXPECT_FALSE(x.isAnimated());
    EXPECT_DOUBLE_EQ(x.constantValue(), 0.3);
    EXPECT_NE(keys::property(c, "transform.rotation"), nullptr);
}

// Export, preview and the browser thumbnails composite at the same time, each with its own CPU backend,
// sharing the row-parallel pool (M3 found a deadlock when two threads used the pool at once).
TEST(CpuBackend, ConcurrentFramesFromSeveralThreads) {
    auto frame = [](int seed, int iterations) {
        auto be = makeCpuBackend();
        Image src(640, 360, Rgba8{uint8_t(40 * seed), 100, 200, 255});
        Clip c;
        c.transform.rotation.setConstant(10.0 * seed);
        c.transform.scaleX.setConstant(0.8);
        Image out;
        for (int i = 0; i < iterations; ++i) {
            be->beginFrame(640, 360, Rgba8{0, 0, 0, 255});
            be->drawLayer(clipLayer(c, Time(0), &src, 640, 360));
            out = be->endFrame();
        }
        return out;
    };
    std::vector<Image> reference, results(4);
    for (int k = 0; k < 4; ++k) reference.push_back(frame(k, 1));
    std::vector<std::thread> threads;
    for (int k = 0; k < 4; ++k) threads.emplace_back([&, k] { results[static_cast<size_t>(k)] = frame(k, 150); });
    for (auto& t : threads) t.join();
    for (int k = 0; k < 4; ++k) EXPECT_EQ(results[static_cast<size_t>(k)].pixels, reference[static_cast<size_t>(k)].pixels) << k;
}
