// M9 stickers: every catalogue sticker draws a picture, animated ones move, and the renderer places a sticker with the
// clip's transform exactly like a media frame (CPU backend).
#include "fa/model/serialize.h"
#include "fa/render/backend.h"
#include "fa/render/renderer.h"
#include "fa/text/stickers.h"
#include "fa/text/title_engine.h"

#include <QImage>

#include <gtest/gtest.h>

#include <map>
#include <set>

using namespace fa;

namespace {

int drawnPixels(const QImage& img) {
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        const uchar* row = img.constScanLine(y);
        for (int x = 0; x < img.width(); ++x) n += row[x * 4 + 3] > 32;
    }
    return n;
}

struct NoMedia final : FrameProvider {
    Result frame(const Clip&, const MediaSource&, const Time&) override { throw RenderError("no media in this test"); }
};

Project stickerProject(const StickerData& s, double scale, double x, double y) {
    Project p;
    Sequence q;
    q.id = "s";
    q.frameRate = FrameRate::fps30();
    q.width = 640;
    q.height = 360;
    Track v{"v", TrackKind::Video, "Video 1"};
    Clip c;
    c.id = "c";
    c.sticker = s;
    c.duration = Time(5);
    c.transform.scaleX.setConstant(scale);
    c.transform.scaleY.setConstant(scale);
    c.transform.positionX.setConstant(x);
    c.transform.positionY.setConstant(y);
    v.clips = {c};
    q.tracks = {v};
    p.sequences = {q};
    return p;
}

}  // namespace

TEST(Stickers, EveryStickerDrawsAndAnimatedOnesMove) {
    const auto& cat = text::stickerCatalog();
    ASSERT_GE(cat.size(), 50u);
    const auto& cats = text::stickerCategories();
    std::set<std::string> ids;
    int animated = 0, moved = 0;
    for (const auto& s : cat) {
        EXPECT_TRUE(ids.insert(s.id).second) << "duplicate id " << s.id;
        EXPECT_NE(std::find(cats.begin(), cats.end(), s.category), cats.end()) << s.id << " has an unlisted category";
        const int w = s.fullFrame ? 384 : 128, h = s.fullFrame ? 216 : 128;
        const QImage a = text::stickerImage(StickerData{s.id, 0, {}}, 1.5, 5.0, w, h);
        ASSERT_EQ(a.format(), QImage::Format_RGBA8888) << s.id;
        ASSERT_EQ(a.size(), QSize(w, h)) << s.id;
        EXPECT_GT(drawnPixels(a), s.fullFrame ? 50 : w * h / 100) << s.id << " draws (almost) nothing";
        const QImage early = text::stickerImage(StickerData{s.id, 0, {}}, 0.15, 5.0, w, h);
        if (s.animated) {
            ++animated;
            moved += early != a;
        } else {
            EXPECT_EQ(early, a) << s.id << " is not animated but changes";
        }
    }
    EXPECT_EQ(moved, animated) << "every animated sticker changes between 0.15 s and 1.5 s";
    EXPECT_EQ(drawnPixels(text::stickerImage(StickerData{"no.such.sticker", 0, {}}, 1.0, 5.0, 64, 64)), 0);
}

TEST(Stickers, ColourAndWordsChangeThePicture) {
    const QImage plain = text::stickerImage(StickerData{"badge.live", 0, {}}, 1.0, 5.0, 200, 200);
    const QImage blue = text::stickerImage(StickerData{"badge.live", 0x2040FF, {}}, 1.0, 5.0, 200, 200);
    const QImage words = text::stickerImage(StickerData{"badge.live", 0, "ON AIR"}, 1.0, 5.0, 200, 200);
    EXPECT_NE(plain, blue);
    EXPECT_NE(plain, words);
}

// M11: 30 more stickers in eight new groups; every sticker that takes a colour or words shows them
TEST(Stickers, M11StickersAndEveryColourAndWordsTakeEffect) {
    const auto& cat = text::stickerCatalog();
    EXPECT_EQ(cat.size(), 93u);   // M9's 63 and M11's 30
    std::map<std::string, int> perCategory;
    for (const auto& s : cat) ++perCategory[s.category];
    for (const char* c : {"Weather", "Travel", "Music", "Gaming", "Holiday", "Sale", "Tech", "Callout"})
        EXPECT_GE(perCategory[c], 2) << c;
    for (const auto& s : cat) {
        if (s.fullFrame) continue;
        const QImage plain = text::stickerImage(StickerData{s.id, 0, {}}, 1.0, 5.0, 160, 160);
        if (s.tintable)
            EXPECT_NE(plain, text::stickerImage(StickerData{s.id, 0x2040FF, {}}, 1.0, 5.0, 160, 160)) << s.id << " ignores its colour";
        if (!s.defaultText.empty())
            EXPECT_NE(plain, text::stickerImage(StickerData{s.id, 0, "Hi!"}, 1.0, 5.0, 160, 160)) << s.id << " ignores its words";
    }
}

// A sticker clip goes through the media path: scale and position like a video frame (square stickers: the canvas
// height at scale 1), and a full-frame background covers the canvas.
TEST(Stickers, PlacedWithTheClipTransformLikeMedia) {
    text::TitleEngine engine;
    auto be = makeCpuBackend();
    NoMedia media;
    Project p = stickerProject(StickerData{"shape.circle", 0xFF0000, {}}, 0.25, 0.25, 0.0);
    ASSERT_FALSE(hasErrors(validate(p)));
    const Image& img = renderFrame(p.sequences[0], 30, p, media, *be, nullptr, nullptr, 640, 360, &engine);
    int x0 = 1 << 30, x1 = -1, y0 = 1 << 30, y1 = -1;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            const Rgba8 c = img.at(x, y);
            if (c.r > 128 && c.g < 80 && c.b < 80) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
        }
    ASSERT_GE(x1, 0);
    EXPECT_NEAR((x0 + x1) / 2.0, 480.0, 3.0);           // 0.25 of the width right of the centre
    EXPECT_NEAR((y0 + y1) / 2.0, 180.0, 3.0);
    EXPECT_NEAR(x1 - x0 + 1, 0.25 * 360 * 0.96, 4.0);   // the circle spans 96 % of the sticker, which is the canvas height

    Project bg = stickerProject(StickerData{"bg.solid", 0x2040FF, {}}, 1.0, 0, 0);
    const Image& full = renderFrame(bg.sequences[0], 10, bg, media, *be, nullptr, nullptr, 640, 360, &engine);
    for (const auto& [x, y] : {std::pair{0, 0}, {639, 0}, {0, 359}, {639, 359}, {320, 180}}) {
        const Rgba8 c = full.at(x, y);
        EXPECT_NEAR(c.b, 255, 2);
        EXPECT_NEAR(c.r, 0x20, 2);
    }
    // without a sticker engine the renderer says so instead of drawing nothing
    EXPECT_THROW(renderFrame(p.sequences[0], 30, p, media, *be, nullptr, nullptr, 640, 360, nullptr), RenderError);
}


// M10: an enlarged sticker is drawn at the size it is shown, not stretched: at Scale 200 % its edges are as crisp as
// at 100 % (the stretched picture's were twice as wide), at the same size and place.
TEST(Stickers, EnlargedStickersStaySharp) {
    text::TitleEngine engine;
    auto be = makeCpuBackend();
    NoMedia media;
    auto softEdge = [](const Image& img, int64_t* inkOut) {
        int64_t soft = 0, ink = 0;
        for (int y = 0; y < img.height; ++y)
            for (int x = 0; x < img.width; ++x) {
                const Rgba8 c = img.at(x, y);
                const int l = std::max({int(c.r), int(c.g), int(c.b)});
                if (l <= 24) continue;
                ++ink;
                if (l < 231) ++soft;
            }
        if (inkOut) *inkOut = ink;
        return ink ? double(soft) / double(ink) : 0.0;
    };
    const StickerData star{"shape.star", 0, {}};
    Project p = stickerProject(star, 2.0, 0, 0);
    ASSERT_FALSE(hasErrors(validate(p)));
    const Image enlarged = renderFrame(p.sequences[0], 30, p, media, *be, nullptr, nullptr, 640, 360, &engine);
    // the old way: the canvas-height picture, placed with Scale 200 %
    const Clip& c = p.sequences[0].tracks[0].clips[0];
    const TitleFrame f = engine.renderSticker(star, Time(1), c.duration, 640, 360);
    ASSERT_TRUE(f.image);
    EXPECT_EQ(f.image->height, 360);
    be->beginFrame(640, 360, Rgba8{0, 0, 0, 255});
    be->drawLayer(clipLayer(c, Time(1), f.image, 640, 360));
    const Image stretched = be->endFrame();
    int64_t inkNew = 0, inkOld = 0;
    const double sNew = softEdge(enlarged, &inkNew), sOld = softEdge(stretched, &inkOld);
    std::printf("  star at 200 %%: soft edge share %.3f (stretched picture %.3f), ink %lld / %lld px\n", sNew, sOld,
                static_cast<long long>(inkNew), static_cast<long long>(inkOld));
    EXPECT_LT(sNew, sOld * 0.7);
    EXPECT_NEAR(double(inkNew), double(inkOld), inkOld * 0.02);   // the same star, the same size
    // drawn larger: the engine's picture for 200 % is twice the canvas height
    EXPECT_EQ(engine.renderSticker(star, Time(1), c.duration, 640, 360, 2.0).image->height, 720);
}
