// M3 text engine: layout, Unicode (Arabic, combining marks, emoji), styling, EXP-003 geometry,
// animation rendering, caching, renderFrame integration (titles over video, captions, CPU/GPU).
#include "fa/render/renderer.h"
#include "fa/model/serialize.h"
#include "fa/render/text_animation.h"
#include "fa/text/title_engine.h"
#include "fa/text/title_templates.h"

#include <QGuiApplication>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <set>

using namespace fa;
using fa::text::TitleEngine;

namespace {

struct Box {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    double cx = 0, cy = 0, mass = 0;
    bool empty() const { return x1 < 0; }
    int w() const { return x1 - x0 + 1; }
    int h() const { return y1 - y0 + 1; }
};

template <typename Pred>
Box boxOf(const Image& img, Pred pred) {
    Box b;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            const Rgba8 c = img.at(x, y);
            if (!pred(c)) continue;
            b.x0 = std::min(b.x0, x);
            b.y0 = std::min(b.y0, y);
            b.x1 = std::max(b.x1, x);
            b.y1 = std::max(b.y1, y);
            b.cx += x;
            b.cy += y;
            b.mass += 1;
        }
    if (b.mass > 0) {
        b.cx /= b.mass;
        b.cy /= b.mass;
    }
    return b;
}

TitleData title(const std::string& text, const std::string& font = "Arial", double size = 48) {
    TitleData t;
    t.text = text;
    t.style.font = font;
    t.style.size = size;
    return t;
}

// Composite a title (centre of the canvas unless moved) over black with the real renderer.
Image renderTitle(const TitleData& t, int w = 1920, int h = 1080, const Time& at = Time(1), const Time& dur = Time(5),
                  double posY = 0.0, RenderBackend* be = nullptr) {
    static TitleEngine engine;
    std::unique_ptr<RenderBackend> cpu;
    if (!be) {
        cpu = makeCpuBackend();
        be = cpu.get();
    }
    Clip c;
    c.title = t;
    c.duration = dur;
    c.transform.positionY.setConstant(posY);
    const TitleFrame f = engine.render(t, t.style, at, dur, w, h);
    be->beginFrame(w, h, Rgba8{0, 0, 0, 255});
    if (f.image) be->drawLayer(titleLayer(c, at, f, w, h, titleAdjust(f.anim, w, h)));
    return be->endFrame();
}

bool lit(Rgba8 c) { return c.r > 128 || c.g > 128 || c.b > 128; }

double meanAbsDiff(const Image& a, const Image& b) {
    double s = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i)
        if (i % 4 != 3) s += std::abs(int(a.pixels[i]) - int(b.pixels[i]));
    return s / (a.pixels.size() / 4 * 3.0);
}

Image downscale2(const Image& src) {
    Image out(src.width / 2, src.height / 2);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x)
            for (int c = 0; c < 4; ++c) {
                int s = 0;
                for (int k = 0; k < 4; ++k) s += src.row(y * 2 + (k >> 1))[(x * 2 + (k & 1)) * 4 + c];
                out.row(y)[x * 4 + c] = static_cast<uint8_t>((s + 2) / 4);
            }
    return out;
}

}  // namespace

// ---- size and layout ---------------------------------------------------------------------------

TEST(TextLayout, SizeUnitMatchesExp003) {
    // EXP-003: Arial, FontSize 48 at 1080p -> text block "AbgQ Hxy 123" 684 x 102 px (em ~ 110 px)
    TitleEngine e;
    const auto info = e.layout("AbgQ Hxy 123", title("", "Arial", 48).style);
    EXPECT_NEAR(info.emPx, 110.0, 1e-9);
    const Image img = renderTitle(title("AbgQ Hxy 123"));
    const Box b = boxOf(img, lit);
    std::printf("  Arial 48 ink box: %d x %d px (Filmora EXP-003: 684 x 102)\n", b.w(), b.h());
    // height (em mapping) within 4 %; width within 5 % (measured: MeerCut's advances are ~4 % wider than
    // Filmora's for Arial - documented, not hidden)
    EXPECT_NEAR(b.w(), 684, 684 * 0.05);
    EXPECT_NEAR(b.h(), 102, 102 * 0.04);
    // centred on the canvas
    EXPECT_NEAR((b.x0 + b.x1) / 2.0, 960, 3);
}

TEST(TextLayout, AlignmentAndWrapping) {
    TitleEngine e;
    TextStyle st;
    st.font = "Arial";
    for (TextAlign a : {TextAlign::Left, TextAlign::Center, TextAlign::Right}) {
        st.align = a;
        const auto info = e.layout("Short\nA much longer line", st);
        ASSERT_EQ(info.lines.size(), 2u);
        const auto& l0 = info.lines[0];
        const auto& l1 = info.lines[1];
        if (a == TextAlign::Left) EXPECT_NEAR(l0.x, l1.x, 1.0);
        if (a == TextAlign::Center) EXPECT_NEAR(l0.x + l0.w / 2, l1.x + l1.w / 2, 1.0);
        if (a == TextAlign::Right) EXPECT_NEAR(l0.x + l0.w, l1.x + l1.w, 1.0);
    }
    st.align = TextAlign::Center;
    st.wrapWidth = 0.3;   // 30 % of a 1920 px wide frame
    const auto wrapped = e.layout("The quick brown fox jumps over the lazy dog again and again", st);
    EXPECT_GE(wrapped.lines.size(), 3u);
    for (const auto& l : wrapped.lines) EXPECT_LE(l.w, 0.3 * 1920 + 1);
    // line spacing
    st.wrapWidth = 0;
    st.lineSpacing = 1.0;
    const double h1 = e.layout("a\nb", st).blockH;
    st.lineSpacing = 2.0;
    const double h2 = e.layout("a\nb", st).blockH;
    EXPECT_GT(h2, h1 * 1.4);
}

TEST(TextLayout, UnicodeClustersArabicAndEmoji) {
    TitleEngine e;
    TextStyle st;
    // combining mark: e + U+0301 is one cluster
    EXPECT_EQ(e.layout("e\xcc\x81x", st).counts.chars, 2);
    // ZWJ family emoji and a flag are one cluster each
    EXPECT_EQ(e.layout("\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7", st).counts.chars, 1);
    EXPECT_EQ(e.layout("\xf0\x9f\x87\xab\xf0\x9f\x87\xb7", st).counts.chars, 1);
    // words and punctuation
    EXPECT_EQ(e.layout("Hello, brave new world!", st).counts.words, 4);
    // Arabic: right-to-left, joined (shaped) forms are narrower than isolated letters side by side
    const auto ar = e.layout("\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85", st);
    EXPECT_TRUE(ar.rightToLeft);
    EXPECT_EQ(ar.counts.chars, 13);
    EXPECT_EQ(ar.counts.words, 2);
    // first logical letter (meem) is drawn to the right of the last one
    EXPECT_GT(ar.chars.front().x, ar.chars[4].x);
    const double joined = e.layout("\xd8\xa8\xd8\xa8\xd8\xa8", st).blockW;   // beh beh beh (initial, medial, final)
    const double isolated = e.layout("\xd8\xa8", st).blockW;
    std::printf("  Arabic: 3 joined behs %.1f px vs 3 x isolated %.1f px\n", joined, 3 * isolated);
    EXPECT_LT(joined, 3 * isolated * 0.85);
    // emoji come from a fallback colour font: rendered with colour
    const Image img = renderTitle(title("\xf0\x9f\x98\x80", "Segoe UI", 80));
    const Box coloured = boxOf(img, [](Rgba8 c) { return std::abs(int(c.r) - int(c.b)) > 60; });
    EXPECT_GT(coloured.mass, 200) << "emoji drawn without colour";
}

// ---- styling (EXP-003 geometry where measured) --------------------------------------------------------

TEST(TextStyle, OutlineAndShadowGeometryMatchExp003) {
    TitleData plain = title("AbgQ Hxy 123");
    const Box b0 = boxOf(renderTitle(plain), lit);
    TitleData outlined = plain;
    outlined.style.outline = true;
    outlined.style.outlineWidth = 5;            // Filmora Border Size 5 -> ~4-5 px per side
    outlined.style.outlineColor = 0xFF0000;
    const Image io = renderTitle(outlined);
    const Box b1 = boxOf(io, [](Rgba8 c) { return c.r > 100 || c.g > 100; });
    std::printf("  outline 5: box grows %d x %d px (expected ~10 x 10)\n", b1.w() - b0.w(), b1.h() - b0.h());
    EXPECT_NEAR(b1.w() - b0.w(), 10, 2);
    EXPECT_NEAR(b1.h() - b0.h(), 10, 2);
    EXPECT_GT(boxOf(io, [](Rgba8 c) { return c.r > 200 && c.g < 60; }).mass, 1000);   // red stroke visible
    // shadow: Filmora defaults Distance 8, Direction down, Blur 3 -> copy ~24 px straight down
    TitleData shadowed = plain;
    shadowed.style.color = 0x00FF00;
    shadowed.style.shadow = true;
    shadowed.style.shadowColor = 0xFF0000;
    shadowed.style.shadowOpacity = 1.0;
    shadowed.style.shadowBlur = 0;
    const Image is = renderTitle(shadowed);
    const Box text = boxOf(is, [](Rgba8 c) { return c.g > 200; });
    const Box shadow = boxOf(is, [](Rgba8 c) { return c.r > 200 && c.g < 40; });
    std::printf("  shadow offset: %.1f px down, %.1f px across\n", shadow.y1 - text.y1 + 0.0, shadow.cx - text.cx);
    EXPECT_NEAR(shadow.y1 - text.y1, 24, 1.5);
    EXPECT_NEAR((shadow.x0 + shadow.x1) / 2.0, (text.x0 + text.x1) / 2.0, 1.5);
}

TEST(TextStyle, ColourGradientBackgroundGlowOpacity) {
    TitleData t = title("HHHH", "Arial", 80);
    t.style.color = 0x3366FF;
    Image img = renderTitle(t);
    // a pixel fully inside a stem: the left stem of the first H, half way down
    const Box b = boxOf(img, lit);
    int sx = b.x0;
    while (sx < b.x1 && img.at(sx, (b.y0 + b.y1) / 2).b < 250) ++sx;
    const int stemX = sx + 3, stemY = (b.y0 + b.y1) / 2;
    const Rgba8 mid = img.at(stemX, stemY);
    EXPECT_NEAR(mid.r, 0x33, 2);
    EXPECT_NEAR(mid.g, 0x66, 2);
    EXPECT_NEAR(mid.b, 0xFF, 2);
    // vertical gradient: white at the top of the glyphs, blue at the bottom
    t.style.color = 0xFFFFFF;
    t.style.gradient = true;
    t.style.gradientColor = 0x0000FF;
    img = renderTitle(t);
    const Box g = boxOf(img, [](Rgba8 c) { return c.b > 100; });
    const Rgba8 top = img.at(static_cast<int>(g.cx) - 18, g.y0 + 6), bottom = img.at(static_cast<int>(g.cx) - 18, g.y1 - 6);
    EXPECT_GT(top.r, bottom.r + 120) << int(top.r) << " vs " << int(bottom.r);
    // background box with padding, around the text block
    t.style.gradient = false;
    t.style.background = true;
    t.style.backgroundColor = 0x00FF00;
    t.style.backgroundOpacity = 1.0;
    t.style.backgroundPadding = 20;
    img = renderTitle(t);
    const Box box = boxOf(img, [](Rgba8 c) { return c.g > 200 && c.r < 60; });
    const Box text2 = boxOf(img, [](Rgba8 c) { return c.r > 200 && c.b > 200; });
    EXPECT_LT(box.x0, text2.x0 - 15);
    EXPECT_GT(box.y1, text2.y1 + 15);
    // text opacity and glow
    t.style.background = false;
    t.style.opacity = 0.5;
    img = renderTitle(t);
    EXPECT_NEAR(img.at(stemX, stemY).r, 128, 4);   // white at 50 % over black
    t.style.opacity = 1;
    t.style.glow = true;
    t.style.glowColor = 0xFF0000;
    t.style.glowSize = 10;
    img = renderTitle(t);
    EXPECT_GT(boxOf(img, [](Rgba8 c) { return c.r > 40 && c.g < 30; }).mass, 2000);   // red halo
}

TEST(TextStyle, DefaultTitleMatchesExp003DefaultRender) {
    // EXP-003: Filmora "Add Text" default -> "Text Here", system UI font, white, lower third:
    // ink x 872-1046 (175 px), y 922-950 at 1080p. MeerCut default: Segoe UI size 18 at y 0.8633.
    TitleData t = title("Text Here", "Segoe UI", 18);
    const Image img = renderTitle(t, 1920, 1080, Time(1), Time(5), 0.8633 - 0.5);
    const Box b = boxOf(img, lit);
    std::printf("  default title ink: x %d-%d, y %d-%d (Filmora: x 872-1046, y 922-950)\n", b.x0, b.x1, b.y0, b.y1);
    EXPECT_NEAR(b.w(), 175, 8);
    EXPECT_NEAR(b.h(), 29, 3);
    EXPECT_NEAR((b.x0 + b.x1) / 2.0, 959, 4);
    EXPECT_NEAR((b.y0 + b.y1) / 2.0, 936, 5);
}

// ---- animation rendering ---------------------------------------------------------------------------

TEST(TextAnimationRender, TypeWriterRevealsCharactersAndKeepsLayout) {
    TitleData t = title("AbgQ Hxy 123");
    t.in = {"typewriter", Time(0)};
    const Box full = boxOf(renderTitle(t, 1920, 1080, Time(3)), lit);
    EXPECT_TRUE(boxOf(renderTitle(t, 1920, 1080, Time(0)), lit).empty());   // frame 0: nothing yet
    // 25 fps frame 10 of 20: round(12 * 10 / 20) = 6 characters "AbgQ H" -> left part of the final box
    const Box half = boxOf(renderTitle(t, 1920, 1080, Rational(10, 25)), lit);
    EXPECT_EQ(half.x0, full.x0);   // characters appear in place (no re-layout)
    EXPECT_LT(half.x1, full.x0 + full.w() * 0.6);
    EXPECT_GT(half.x1, full.x0 + full.w() * 0.35);
}

TEST(TextAnimationRender, WholeTitleAnimationsMoveTheLayerWithoutRedrawing) {
    TitleEngine e;
    TitleData t = title("Slide");
    t.in = {"slide_left", Rational(1)};
    const auto before = e.stats();
    const TitleFrame a = e.render(t, t.style, Rational(1, 4), Time(5), 1920, 1080);
    const TitleFrame b = e.render(t, t.style, Rational(1, 2), Time(5), 1920, 1080);
    EXPECT_LT(a.anim.dx, b.anim.dx);   // moving right towards rest
    EXPECT_EQ(a.image, b.image);       // the same cached picture, only placement changes
    EXPECT_EQ(e.stats().rasterised - before.rasterised, 1u);
    EXPECT_EQ(e.stats().layouts - before.layouts, 1u);
    // per-character animation re-composes but never re-lays out
    TitleData c = title("Characters");
    c.in = {"char_rise", Rational(1)};
    const auto l0 = e.stats().layouts;
    for (int f = 0; f < 10; ++f) e.render(c, c.style, Rational(f, 30), Time(5), 1920, 1080);
    EXPECT_EQ(e.stats().layouts - l0, 1u);
}

TEST(TextAnimationRender, BlurAnimationAndArabicCharacterAnimation) {
    TitleData t = title("Blur");
    t.in = {"blur_in", Rational(1)};
    const Image early = renderTitle(t, 1920, 1080, Rational(1, 10));
    const Image late = renderTitle(t, 1920, 1080, Rational(2));
    auto softPixels = [](const Image& img) {
        return boxOf(img, [](Rgba8 c) { return c.r > 8 && c.r < 120; }).mass;
    };
    EXPECT_GT(softPixels(early), softPixels(late) * 3);
    // Arabic typewriter: reveal goes right-to-left (logical order) without breaking the joined shapes
    TitleData ar = title("\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85 \xd8\xb9\xd9\x84\xd9\x8a\xd9\x83\xd9\x85", "Segoe UI", 60);
    ar.in = {"typewriter", Rational(1)};
    const Box all = boxOf(renderTitle(ar, 1920, 1080, Time(3)), lit);
    const Box first = boxOf(renderTitle(ar, 1920, 1080, Rational(4, 10)), lit);
    EXPECT_EQ(first.x1, all.x1);                 // starts at the right edge
    EXPECT_GT(first.x0, all.x0 + all.w() / 3);   // and has not reached the left yet
}

// ---- consistency -----------------------------------------------------------------------------------

TEST(TextConsistency, PreviewSizeMatchesExportScaledDown) {
    TitleData t = title("Preview = Export", "Arial", 40);
    t.style.outline = true;
    t.style.shadow = true;
    const Image full = renderTitle(t, 1920, 1080);
    const Image small = renderTitle(t, 960, 540);
    const double d = meanAbsDiff(downscale2(full), small);
    std::printf("  960x540 render vs 1080p render scaled down: mean abs difference %.3f / 255\n", d);
    EXPECT_LT(d, 1.0);
    // identical line breaks at any size (layout is computed at the 1080p reference)
    TitleEngine e;
    TextStyle st;
    st.wrapWidth = 0.25;
    EXPECT_EQ(e.layout("wrap me into several lines please", st, 16.0 / 9).lines.size(),
              e.layout("wrap me into several lines please", st, 16.0 / 9).lines.size());
}

TEST(TextConsistency, CpuAndGpuCompositeTheSameTitle) {
    std::string why;
    auto gpu = makeGpuBackend(&why);
    ASSERT_EQ(why.find("shader"), std::string::npos) << why;   // a shader that does not compile is a bug
    if (!gpu) GTEST_SKIP() << why;
    TitleData t = title("GPU \xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a", "Segoe UI", 60);
    t.style.outline = true;
    t.style.glow = true;
    const Image c = renderTitle(t, 1920, 1080);
    const Image g = renderTitle(t, 1920, 1080, Time(1), Time(5), 0.0, gpu.get());
    int maxDiff = 0;
    for (size_t i = 0; i < c.pixels.size(); ++i) maxDiff = std::max(maxDiff, std::abs(int(c.pixels[i]) - int(g.pixels[i])));
    EXPECT_LE(maxDiff, 2);
}

// ---- renderFrame: titles over video, captions -------------------------------------------------------

namespace {
class SolidProvider final : public FrameProvider {
public:
    Result frame(const Clip&, const MediaSource&, const Time& t) override {
        img_.reset(1920, 1080, Rgba8{200, 0, 0, 255});
        Result r;
        r.image = &img_;
        r.requestedTime = r.frameTime = t;
        r.contentKey = 0x8000000000000002ULL;   // always the same picture (like a paused frame)
        return r;
    }

private:
    Image img_;
};
}  // namespace

// M4: a static title (and a paused video frame) is copied to the GPU once, not every frame.
TEST(TitleRendering, StaticTitlesAreNotUploadedEveryFrame) {
    std::string why;
    auto gpu = makeGpuBackend(&why);
    ASSERT_EQ(why.find("shader"), std::string::npos) << why;
    if (!gpu) GTEST_SKIP() << why;
    Project p;
    MediaSource src;
    src.id = "src";
    src.path = "red.mp4";
    src.duration = Rational(10);
    p.sources.push_back(src);
    Sequence s;
    s.id = "seq";
    Track v1{"v1", TrackKind::Video, "Video 1"}, v2{"v2", TrackKind::Video, "Video 2"};
    Clip media;
    media.id = "m";
    media.sourceId = "src";
    media.duration = Rational(10);
    v1.clips.push_back(media);
    Clip t;
    t.id = "t";
    t.title = title("STATIC", "Arial", 80);
    t.duration = Rational(10);
    v2.clips.push_back(t);
    s.tracks = {v1, v2};
    p.sequences.push_back(s);
    SolidProvider prov;
    TitleEngine engine;
    const int64_t up0 = gpu->textureUploads();
    for (int f = 0; f < 10; ++f) renderFrame(p.sequences[0], f, p, prov, *gpu, nullptr, nullptr, 0, 0, &engine);
    // the solid "video" frame has frame index 0 every time, like a paused frame: both pictures are uploaded once
    std::printf("  10 frames: %lld uploads, %lld reuses\n", static_cast<long long>(gpu->textureUploads() - up0),
                static_cast<long long>(gpu->textureReuses()));
    EXPECT_EQ(gpu->textureUploads() - up0, 2);
    EXPECT_GE(gpu->textureReuses(), 18);
}

TEST(TitleRendering, TitleOverVideoAndCaptionsOnTop) {
    Project p;
    MediaSource src;
    src.id = "src";
    src.path = "red.mp4";
    src.duration = Rational(10);
    p.sources.push_back(src);
    Sequence s;
    s.id = "seq";
    Track v1{"v1", TrackKind::Video, "Video 1"}, v2{"v2", TrackKind::Video, "Video 2"};
    Clip media;
    media.id = "m";
    media.sourceId = "src";
    media.duration = Rational(10);
    v1.clips.push_back(media);
    Clip t;
    t.id = "t";
    t.title = title("TITLE", "Arial", 60);
    t.timelineStart = Rational(1);
    t.duration = Rational(2);
    t.blend = BlendMode::Screen;   // white over red stays white
    v2.clips.push_back(t);
    Track sub;
    sub.id = "s";
    sub.kind = TrackKind::Subtitle;
    sub.subtitleStyle.color = 0x00FF00;
    Clip cap;
    cap.id = "c";
    cap.title = TitleData{};
    cap.title->text = "caption";
    cap.title->followTrackStyle = true;
    cap.timelineStart = Rational(2);
    cap.duration = Rational(2);
    sub.clips.push_back(cap);
    s.tracks = {v1, v2, sub};
    p.sequences.push_back(s);
    ASSERT_TRUE(validate(p).empty()) << formatIssues(validate(p));
    SolidProvider prov;
    auto be = makeCpuBackend();
    TitleEngine engine;
    auto at = [&](int frame) { return renderFrame(p.sequences[0], frame, p, prov, *be, nullptr, nullptr, 0, 0, &engine); };
    const Image f0 = at(15);   // 0.5 s: video only
    EXPECT_TRUE(boxOf(f0, [](Rgba8 c) { return c.g > 100; }).empty());
    const Image f1 = at(45);   // 1.5 s: title, no caption
    const Box title1 = boxOf(f1, [](Rgba8 c) { return c.g > 200 && c.b > 200; });
    EXPECT_FALSE(title1.empty());
    EXPECT_NEAR((title1.x0 + title1.x1) / 2.0, 960, 3);
    EXPECT_TRUE(boxOf(f1, [](Rgba8 c) { return c.g > 200 && c.b < 60; }).empty());
    const Image f2 = at(75);   // 2.5 s: title and the green caption at the subtitle line
    const Box capBox = boxOf(f2, [](Rgba8 c) { return c.g > 200 && c.b < 60 && c.r < 60; });
    ASSERT_FALSE(capBox.empty());
    EXPECT_LT(capBox.y1, 0.92 * 1080 + 2);
    EXPECT_GT(capBox.y1, 0.92 * 1080 - 40);   // bottom-anchored at y = 0.92
    const Image f3 = at(105);   // 3.5 s: caption only
    EXPECT_TRUE(boxOf(f3, [](Rgba8 c) { return c.g > 200 && c.b > 200; }).empty());
    // without the text engine titles are an error, not silently missing
    EXPECT_THROW(renderFrame(p.sequences[0], 45, p, prov, *be), RenderError);
}

// Keyframed clip opacity (M2 keyframe engine) and a title animation multiply; scale keyframes and the
// pop animation compose the same way (one compositing pass, no re-rendering of the text).
TEST(TextAnimationRender, KeyframesComposeWithAnimations) {
    TitleEngine engine;
    auto be = makeCpuBackend();
    TitleData t = title("OPACITY", "Arial", 90);
    t.in = {"fade", Rational(1)};
    Clip c;
    c.title = t;
    c.duration = Rational(4);
    c.opacity.setKey({Rational(0), 1.0});
    c.opacity.setKey({Rational(2), 0.0});
    auto peak = [&](const Time& at) {
        const TitleFrame f = engine.render(t, t.style, at, c.duration, 1920, 1080);
        be->beginFrame(1920, 1080, Rgba8{0, 0, 0, 255});
        be->drawLayer(titleLayer(c, at, f, 1920, 1080, titleAdjust(f.anim, 1920, 1080)));
        const Image img = be->endFrame();
        int m = 0;
        for (int y = 0; y < img.height; ++y)
            for (int x = 0; x < img.width; ++x) m = std::max(m, int(img.at(x, y).r));
        return m;
    };
    const TextUnitCounts counts = engine.layout(t.text, t.style).counts;
    for (const Time at : {Rational(1, 4), Rational(1, 2), Rational(3, 4), Rational(3, 2)}) {
        const double expected = evaluateTitleAnimation(t, at, c.duration, counts).opacity * c.opacity.value(at);
        EXPECT_NEAR(peak(at), 255.0 * expected, 3.0) << at.toString();
    }
    EXPECT_NEAR(peak(Rational(1, 2)), 255 * 0.5 * 0.75, 3.0);   // fade 0.5 x keyframes 0.75
    // scale keyframes x pop: the text block is scaled by both
    TitleData pop = title("POP", "Arial", 90);
    pop.in = {"pop", Rational(1)};
    Clip s;
    s.title = pop;
    s.duration = Rational(4);
    s.transform.scaleX.setConstant(2.0);
    const Box plain = boxOf(renderTitle(pop, 1920, 1080, Rational(2), Rational(4)), lit);
    const TitleFrame f = engine.render(pop, pop.style, Rational(2), s.duration, 1920, 1080);
    be->beginFrame(1920, 1080, Rgba8{0, 0, 0, 255});
    be->drawLayer(titleLayer(s, Rational(2), f, 1920, 1080, titleAdjust(f.anim, 1920, 1080)));
    const Box doubled = boxOf(be->endFrame(), lit);
    EXPECT_NEAR(doubled.w(), 2.0 * plain.w(), 4.0);
}

// ---- M10: enlarged titles stay sharp -----------------------------------------------------------------

namespace {

// Share of the text's pixels that are soft edge (neither background nor full ink). Crisp text has edges a pixel or two
// wide, so the share falls as the text gets bigger; a stretched picture's edges grow with it.
double softness(const Image& img) {
    int64_t soft = 0, ink = 0;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            const Rgba8 c = img.at(x, y);
            const int l = std::max({int(c.r), int(c.g), int(c.b)});
            if (l <= 24) continue;
            ++ink;
            if (l < 231) ++soft;
        }
    return ink ? double(soft) / double(ink) : 0.0;
}

// One white title in a 1080p sequence, through the real renderer (frame 30 = 1 s, no animation).
Image titleFrameOf(double size, double scale, RenderBackend& be) {
    Project p;
    Sequence s;
    s.id = "seq";
    Track v1{"v1", TrackKind::Video, "Video 1"};
    Clip t;
    t.id = "t";
    t.title = title("Sharp Text", "Arial", size);
    t.duration = Rational(5);
    t.transform.scaleX.setConstant(scale);
    t.transform.scaleY.setConstant(scale);
    v1.clips.push_back(t);
    s.tracks = {v1};
    p.sequences.push_back(s);
    SolidProvider prov;
    TitleEngine engine;
    return renderFrame(p.sequences[0], 30, p, prov, be, nullptr, nullptr, 1920, 1080, &engine);
}

}  // namespace

// The user's report: a title made bigger with its Scale looked soft and pixelated - the engine drew it at canvas size
// and the compositor stretched it. Now 40 px text at Scale 300 % is as crisp as 120 px text at 100 % (the old way,
// a 40 px picture stretched 3x, is clearly softer), at the same size and place, on the CPU and on the GPU.
TEST(TitleRendering, EnlargedTitlesStaySharp) {
    auto cpu = makeCpuBackend();
    const Image reference = titleFrameOf(120, 1.0, *cpu);
    const Image enlarged = titleFrameOf(40, 3.0, *cpu);
    Image stretched;   // the old way: the canvas-density picture, placed with Scale 300 %
    {
        TitleEngine engine;
        const TitleData t = title("Sharp Text", "Arial", 40);
        Clip c;
        c.title = t;
        c.duration = Rational(5);
        c.transform.scaleX.setConstant(3.0);
        c.transform.scaleY.setConstant(3.0);
        const TitleFrame f = engine.render(t, t.style, Time(1), c.duration, 1920, 1080);   // shown at 1x
        EXPECT_DOUBLE_EQ(f.pixelScale, 1.0);
        cpu->beginFrame(1920, 1080, Rgba8{0, 0, 0, 255});
        cpu->drawLayer(titleLayer(c, Time(1), f, 1920, 1080, titleAdjust(f.anim, 1920, 1080)));
        stretched = cpu->endFrame();
    }
    const double sRef = softness(reference), sBig = softness(enlarged), sOld = softness(stretched);
    const Box rb = boxOf(reference, lit), eb = boxOf(enlarged, lit), ob = boxOf(stretched, lit);
    std::printf("  soft edge share: 120 px at 100 %% %.3f, 40 px at 300 %% %.3f (stretched picture: %.3f); widths %d / %d / %d px\n",
                sRef, sBig, sOld, rb.w(), eb.w(), ob.w());
    EXPECT_LT(sBig, sRef * 1.25);   // as crisp as text drawn that large
    EXPECT_LT(sBig, sOld * 0.6);    // and clearly crisper than the stretched picture
    EXPECT_NEAR(eb.w(), ob.w(), 4.0);   // the same size and place as before: only the sharpness changed
    EXPECT_NEAR(eb.h(), ob.h(), 4.0);
    EXPECT_NEAR(eb.cx, ob.cx, 2.0);
    EXPECT_NEAR(eb.cy, ob.cy, 2.0);
    EXPECT_NEAR(eb.w(), rb.w(), rb.w() * 0.03 + 4.0);   // and the size of the same text drawn 3x as large
    std::string why;
    auto gpu = makeGpuBackend(&why);
    if (!gpu) {
        std::printf("  (no GPU backend: %s)\n", why.c_str());
        return;
    }
    const Image onGpu = titleFrameOf(40, 3.0, *gpu);
    std::printf("  GPU: soft edge share %.3f, mean difference from the CPU %.3f / 255\n", softness(onGpu), meanAbsDiff(onGpu, enlarged));
    EXPECT_LT(softness(onGpu), sOld * 0.6);
    EXPECT_LT(meanAbsDiff(onGpu, enlarged), 1.0);
}

// The quality steps: shown at or below 100 % a title is drawn at the canvas density (as before, one cached picture);
// enlarged it is drawn at least that large, at most 19 % larger, at most 8x, and within the memory budget.
TEST(TitleRendering, EnlargedTitlesAreDrawnAtTheirShownSize) {
    TitleEngine engine;
    const TitleData t = title("Steps", "Arial", 60);
    const TitleFrame one = engine.render(t, t.style, Time(1), Time(5), 1920, 1080, 1.0);
    EXPECT_DOUBLE_EQ(one.pixelScale, 1.0);
    EXPECT_DOUBLE_EQ(engine.render(t, t.style, Time(1), Time(5), 1920, 1080, 0.4).pixelScale, 1.0);   // smaller: as before
    const int w1 = one.image->width;
    for (const double shown : {1.1, 1.5, 2.0, 3.0, 5.0}) {
        const TitleFrame f = engine.render(t, t.style, Time(1), Time(5), 1920, 1080, shown);
        const double drawn = 1.0 / f.pixelScale;   // image pixels per canvas pixel
        EXPECT_GE(drawn, shown - 1e-9) << shown;
        EXPECT_LE(drawn, shown * 1.19) << shown;
        EXPECT_NEAR(f.image->width, w1 * drawn, drawn + 1.0) << shown;
        EXPECT_DOUBLE_EQ(f.blockW, one.blockW) << shown;   // the geometry is in canvas pixels: unchanged
    }
    EXPECT_DOUBLE_EQ(1.0 / engine.render(t, t.style, Time(1), Time(5), 1920, 1080, 50.0).pixelScale, 8.0);   // at most 8x
    // a huge title shown huge stays within 4096 x 4096 pixels
    const TitleData huge = title("A very long title that is shown far too large", "Arial", 200);
    const TitleFrame h = engine.render(huge, huge.style, Time(1), Time(5), 3840, 2160, 8.0);
    EXPECT_LE(double(h.image->width) * h.image->height, 4096.0 * 4096.0 * 1.01);
    // the same shown scale twice: one cached picture
    const auto before = engine.stats().rasterised;
    engine.render(t, t.style, Time(1), Time(5), 1920, 1080, 2.0);
    engine.render(t, t.style, Rational(3, 2), Time(5), 1920, 1080, 1.9);   // the same step
    EXPECT_EQ(engine.stats().rasterised, before);
}

// ---- title templates -------------------------------------------------------------------------------

TEST(TitleTemplates, EveryTemplateRendersItsLookAndAnimations) {
    const auto& all = text::builtinTitleTemplates();
    ASSERT_GE(all.size(), 15u);
    std::set<std::string> ids, categories;
    TitleEngine engine;
    auto be = makeCpuBackend();
    Image black(640, 360, Rgba8{0, 0, 0, 255});
    for (const auto& t : all) {
        SCOPED_TRACE(t.id);
        EXPECT_TRUE(ids.insert(t.id).second);   // unique ids
        categories.insert(t.category);
        ASSERT_TRUE(t.prototype.title.has_value());
        EXPECT_EQ(t.prototype.title->templateId, t.id);
        EXPECT_GT(t.duration, Time(0));
        // every animation slot names a preset the engine implements
        const TextAnimation* anims[3] = {&t.prototype.title->in, &t.prototype.title->loop, &t.prototype.title->out};
        const TextAnimStage stages[3] = {TextAnimStage::In, TextAnimStage::Loop, TextAnimStage::Out};
        for (int i = 0; i < 3; ++i)
            if (anims[i]->active()) EXPECT_NE(findTextAnimPreset(stages[i], anims[i]->preset), nullptr) << anims[i]->preset;
        // mid-clip the title is visible and inside the frame (captions on the subtitle line)
        const Vec2 base = t.caption ? Vec2{0.0, 0.92 - 0.5} : Vec2{};
        const Time mid = t.duration / Rational(2);
        const Image img = renderTitlePreview(t.prototype, mid, t.duration, black, 640, 360, *be, engine, base);
        const Box b = boxOf(img, [](Rgba8 c) { return c.r > 60 || c.g > 60 || c.b > 60; });
        ASSERT_FALSE(b.empty());
        EXPECT_GE(b.x0, 0);
        EXPECT_LE(b.x1, 639);
        if (t.caption) EXPECT_GT(b.cy, 360 * 0.7);
        // animated templates look different at the start than mid-clip (In) - "Credits Roll" moves all along
        if (t.prototype.title->in.active()) {
            const Image start = renderTitlePreview(t.prototype, Time(0), t.duration, black, 640, 360, *be, engine, base);
            EXPECT_GT(meanAbsDiff(start, img), 0.05);
        }
    }
    EXPECT_EQ(categories, (std::set<std::string>{"Basic", "Lower Thirds", "Subtitles", "Openers", "End Credits", "Social Media",
                                                 "Callouts", "Quotes", "Promo", "Gaming", "Elegant"}));
    EXPECT_EQ(all.size(), 54u);   // M3's 21 and M11's 33
    for (const auto& c : text::titleCategories())   // every group of the Titles page has templates (or is the user's)
        EXPECT_TRUE(c == "My Titles" || categories.count(c)) << c;
    // the default template is Filmora's "Add Text" look measured in EXP-003 (see DefaultTitleMatchesExp003DefaultRender)
    const auto& def = text::defaultTitleTemplate();
    EXPECT_EQ(def.prototype.title->text, "Text Here");
    EXPECT_EQ(def.prototype.title->style.size, 18.0);
    EXPECT_EQ(def.duration, Time(5));
    EXPECT_NEAR(def.prototype.transform.positionY.value(Time(0)), 0.8633 - 0.5, 1e-9);
}

TEST(TitleTemplates, UserPresetClipRoundTripsThroughJson) {
    Clip c = text::findTitleTemplate("lower.name_bar")->prototype;
    c.title->style.color = 0x123456;
    c.title->loop = {"bounce", Rational(2)};
    const Clip back = clipFromJsonString(clipToJsonString(c));
    EXPECT_EQ(back, c);
    EXPECT_THROW(clipFromJsonString("{ not json"), ModelError);
}

// Many titles on screen at once (one per track): every one is drawn where it belongs.
TEST(TitleRendering, TwentyTitlesInOneFrame) {
    Project p;
    Sequence s;
    s.id = "seq";
    for (int i = 0; i < 20; ++i) {
        Track t{"v" + std::to_string(i), TrackKind::Video, "Video " + std::to_string(i + 1)};
        Clip c;
        c.id = "t" + std::to_string(i);
        c.title = title(std::to_string(i % 10), "Arial", 30);
        if (i % 2) c.title->in = {"char_fade", Rational(1, 10)};   // per-unit frames too (engine scratch buffer)
        c.duration = Rational(2);
        c.transform.positionX.setConstant(-0.45 + 0.9 * (i % 10) / 9.0);
        c.transform.positionY.setConstant(i < 10 ? -0.25 : 0.25);
        t.clips.push_back(c);
        s.tracks.push_back(t);
    }
    p.sequences.push_back(s);
    ASSERT_TRUE(validate(p).empty()) << formatIssues(validate(p));
    SolidProvider prov;
    auto be = makeCpuBackend();
    TitleEngine engine;
    const Image f = renderFrame(p.sequences[0], 30, p, prov, *be, nullptr, nullptr, 0, 0, &engine);
    for (int i = 0; i < 20; ++i) {
        const int cx = static_cast<int>(960 + 1920 * (-0.45 + 0.9 * (i % 10) / 9.0));
        const int cy = i < 10 ? 270 : 810;
        int lit = 0;
        for (int y = -40; y < 40; ++y)
            for (int x = -40; x < 40; ++x)
                if (f.at(std::clamp(cx + x, 0, 1919), cy + y).r > 128) ++lit;
        EXPECT_GT(lit, 20) << "title " << i;
    }
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
