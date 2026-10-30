// M5 Render Preview cache: the lossless frame codec, content keys, the disk cache (limits, safe writes, corrupt
// files) and the range render feeding the player.
#include "fa/media/media.h"
#include "fa/model/commands.h"
#include "fa/model/edit_ops.h"
#include "fa/playback/frame_codec.h"
#include "fa/playback/preview_renderer.h"
#include "fa/playback/render_cache.h"
#include "fa/playback/render_preview.h"
#include "fa/render/renderer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

using namespace fa;

namespace {

std::filesystem::path media(const char* name) { return std::filesystem::path(FA_TESTMEDIA_DIR) / name; }

std::filesystem::path freshDir(const char* name) {
    const auto d = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

Project twoClips() {   // A (red, 0-10 s) and B (blue, 10-20 s) on Video 1
    Project p;
    p.sources = {MediaSource{"A", media("A_red_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920, 1080, true, true},
                 MediaSource{"B", media("B_blue_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920, 1080, true, true}};
    Sequence s;
    s.id = "s";
    p.sequences.push_back(s);
    UndoStack st(p);
    st.execute(edit::addMedia(p, "s", "A", Rational(0)));
    st.execute(edit::addMedia(p, "s", "B", Rational(10)));
    return p;
}

FrameKeyer keyer(const Project& p, int w = 480, int h = 270) {
    return FrameKeyer(p, *p.findSequence("s"), "", FrameKeyer::Config{w, h, "cpu", false});
}

}  // namespace

TEST(RenderCache, CodecIsLosslessAndRejectsDamage) {
    Image noise(321, 179);
    std::mt19937 rng(3);
    for (auto& b : noise.pixels) b = static_cast<uint8_t>(rng());
    Image flat(640, 360, Rgba8{12, 34, 56, 255});
    VideoDecoder dec(media("C_testsrc2_1080p30_10s.mp4"));
    const Image real = dec.frameAt(Rational(2)).image;
    for (const Image* img : std::initializer_list<const Image*>{&noise, &flat, &real}) {
        const auto bytes = encodeFrame(*img);
        const auto back = decodeFrame(bytes.data(), bytes.size());
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(back->width, img->width);
        EXPECT_EQ(back->height, img->height);
        EXPECT_EQ(back->pixels, img->pixels);   // bit-exact
        std::printf("  %dx%d: %.1f %% of raw\n", img->width, img->height, 100.0 * bytes.size() / img->pixels.size());
        auto cut = bytes;   // truncated
        cut.resize(cut.size() / 2);
        EXPECT_FALSE(decodeFrame(cut.data(), cut.size()).has_value());
    }
    auto bytes = encodeFrame(real);
    bytes[0] = 'x';   // not a QOI image
    EXPECT_FALSE(decodeFrame(bytes.data(), bytes.size()).has_value());
    EXPECT_FALSE(decodeFrame(nullptr, 0).has_value());
    // timing of a preview-sized frame
    VideoDecoder::Options o;
    o.maxWidth = 960;
    o.maxHeight = 540;
    VideoDecoder small(media("C_testsrc2_1080p30_10s.mp4"), o);
    const Image prev = small.frameAt(Rational(3)).image;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> enc;
    for (int i = 0; i < 10; ++i) enc = encodeFrame(prev);
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i) EXPECT_TRUE(decodeFrame(enc.data(), enc.size()).has_value());
    const auto t2 = std::chrono::steady_clock::now();
    std::printf("  %dx%d frame: encode %.2f ms, decode %.2f ms, %.0f KB\n", prev.width, prev.height,
                std::chrono::duration<double, std::milli>(t1 - t0).count() / 10,
                std::chrono::duration<double, std::milli>(t2 - t1).count() / 10, enc.size() / 1024.0);
}

// An edit changes the keys of exactly the frames it can affect; settings outside the picture do not.
TEST(RenderCache, KeysFollowTheContent) {
    Project p = twoClips();
    const FrameKeyer k0 = keyer(p);
    EXPECT_NE(k0.key(10), k0.key(11));   // every frame has its own key
    EXPECT_EQ(k0.key(10), keyer(p).key(10));   // deterministic
    Clip& b = p.sequences[0].tracks[0].clips[1];
    b.opacity.setConstant(0.5);   // B changes: frames from 300 on (and B's reach) change
    const FrameKeyer k1 = keyer(p);
    for (int64_t f : {0, 100, 290}) EXPECT_EQ(k0.key(f), k1.key(f)) << f;
    for (int64_t f : {300, 450, 599}) EXPECT_NE(k0.key(f), k1.key(f)) << f;
    // a transition from A into B widens both reaches around the cut
    p.sequences[0].tracks[0].clips[0].outTransition = TransitionRef{"fa.transition.dissolve", Rational(1)};
    const FrameKeyer k2 = keyer(p);
    EXPECT_NE(k1.key(290), k2.key(290));
    EXPECT_NE(k1.key(310), k2.key(310));
    EXPECT_EQ(k1.key(100), k2.key(100));
    // locking or muting a track does not change pixels: same keys
    p.sequences[0].tracks[0].locked = true;
    EXPECT_EQ(k2.key(100), keyer(p).key(100));
    // hiding it does
    p.sequences[0].tracks[0].hidden = true;
    EXPECT_NE(k2.key(100), keyer(p).key(100));
    p.sequences[0].tracks[0].hidden = false;
    p.sequences[0].tracks[0].locked = false;
    // preview size and backend are part of the key
    EXPECT_NE(keyer(p).key(100), keyer(p, 960, 540).key(100));
    EXPECT_NE(keyer(p).key(100), FrameKeyer(p, *p.findSequence("s"), "", FrameKeyer::Config{480, 270, "gpu", false}).key(100));
}

TEST(RenderCache, ChangedMediaFilesChangeTheKeys) {
    const auto dir = freshDir("fa_rc_media");
    const auto copy = dir / "A.mp4";
    std::filesystem::copy_file(media("A_red_1080p30_10s.mp4"), copy);
    Project p = twoClips();
    p.sources[0].path = copy.string();
    const FrameKey before = keyer(p).key(10), otherClip = keyer(p).key(400);
    std::ofstream(copy, std::ios::binary | std::ios::app) << "x";   // the file is replaced / re-encoded
    EXPECT_NE(keyer(p).key(10), before);
    EXPECT_EQ(keyer(p).key(400), otherClip);   // B's frames do not use that file
}

TEST(RenderCache, StoresLoadsEvictsAndSurvivesDamage) {
    const auto dir = freshDir("fa_rc_store");
    Image img(200, 100, Rgba8{1, 2, 3, 255});
    for (int x = 0; x < 200; ++x) img.set(x, x / 2, Rgba8{200, static_cast<uint8_t>(x), 9, 255});
    const size_t one = encodeFrame(img).size();
    {
        RenderCache c(dir, one * 3 + one / 2);   // room for three frames
        for (uint64_t i = 1; i <= 3; ++i) ASSERT_TRUE(c.store({i, i}, img));
        EXPECT_EQ(c.frameCount(), 3u);
        ASSERT_TRUE(c.load({1, 1}).has_value());   // 1 is now the most recently used
        EXPECT_EQ(c.load({1, 1})->pixels, img.pixels);
        ASSERT_TRUE(c.store({4, 4}, img));         // over the limit: the least recently used (2) goes
        EXPECT_EQ(c.frameCount(), 3u);
        EXPECT_FALSE(c.contains({2, 2}));
        EXPECT_TRUE(c.contains({1, 1}) && c.contains({3, 3}) && c.contains({4, 4}));
        EXPECT_LE(c.sizeBytes(), c.limit());
    }
    // a leftover of an interrupted write is cleaned up; the index is rebuilt from the files
    std::ofstream(dir / ("00000000000000050000000000000005.qoi.tmp123"), std::ios::binary) << "partial";
    {
        RenderCache c(dir, 1ull << 30);
        EXPECT_EQ(c.frameCount(), 3u);
        EXPECT_FALSE(std::filesystem::exists(dir / ("00000000000000050000000000000005.qoi.tmp123")));
        // a damaged file is a miss and is removed
        const auto file = dir / (FrameKey{3, 3}.hex() + ".qoi");
        std::filesystem::resize_file(file, std::filesystem::file_size(file) / 2);
        EXPECT_FALSE(c.load({3, 3}).has_value());
        EXPECT_FALSE(c.contains({3, 3}));
        EXPECT_FALSE(std::filesystem::exists(file));
        const uint64_t changes = c.changeCount();
        c.clear();
        EXPECT_EQ(c.frameCount(), 0u);
        EXPECT_EQ(c.sizeBytes(), 0u);
        EXPECT_GT(c.changeCount(), changes);
    }
}

// The range render fills the cache; the player then shows those frames (bit-identical to rendering them) and
// renders again only where an edit changed the content. Export never reads the cache.
TEST(RenderCache, RenderPreviewFeedsThePlayerExactly) {
    const auto dir = freshDir("fa_rc_range");
    auto cache = std::make_shared<RenderCache>(dir);
    Project p = twoClips();
    p.sequences[0].tracks[0].clips[0].outTransition = TransitionRef{"fa.transition.dissolve", Rational(1)};
    ProjectSnapshot snap{std::make_shared<const Project>(p), "s", "", 1};

    PreviewRenderer live(480, 270, 8);
    live.setBackend(BackendChoice::Cpu);
    live.setSnapshot(snap);
    const PreviewFrame first = live.renderNow(5);   // creates the backend: its name is part of the keys
    std::vector<std::shared_ptr<const Image>> direct;
    for (int64_t f : {int64_t(5), int64_t(295), int64_t(305), int64_t(400)}) direct.push_back(live.renderNow(f).image);

    RenderPreviewRequest req;
    req.snapshot = snap;
    req.from = 250;
    req.to = 320;
    req.maxWidth = 480;
    req.maxHeight = 270;
    req.backend = BackendChoice::Cpu;
    req.expectBackend = live.backendName();
    std::atomic<bool> cancel{false};
    int64_t lastDone = 0;
    const RenderPreviewResult r = renderPreviewRange(req, *cache, cancel, [&](int64_t done, int64_t) { lastDone = done; });
    std::printf("  rendered %lld frames in %.2f s\n", static_cast<long long>(r.rendered), r.seconds);
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_EQ(r.rendered, 70);
    EXPECT_EQ(lastDone, 70);
    EXPECT_EQ(cache->frameCount(), 70u);
    // again: nothing to do
    const RenderPreviewResult again = renderPreviewRange(req, *cache, cancel);
    EXPECT_EQ(again.rendered, 0);
    EXPECT_EQ(again.alreadyCached, 70);

    // the player uses the cache for the rendered range only, with identical pixels
    PreviewRenderer player(480, 270, 8);
    player.setBackend(BackendChoice::Cpu);
    player.setSnapshot(snap);
    player.setRenderCache(cache);
    player.renderNow(0);   // backend created
    const uint64_t hits0 = player.renderCacheHits();
    const PreviewFrame c295 = player.renderNow(295), c305 = player.renderNow(305);
    EXPECT_TRUE(c295.fromRenderCache && c305.fromRenderCache);
    EXPECT_EQ(player.renderCacheHits(), hits0 + 2);
    EXPECT_EQ(c295.image->pixels, direct[1]->pixels);
    EXPECT_EQ(c305.image->pixels, direct[2]->pixels);
    const PreviewFrame c400 = player.renderNow(400);   // outside the rendered range: rendered
    EXPECT_FALSE(c400.fromRenderCache);
    EXPECT_EQ(c400.image->pixels, direct[3]->pixels);

    // an edit to B: B's frames (and those of the transition into it, B's reach starts 31 frames before the cut) are
    // rendered again; A's frames outside the transition still come from the cache
    p.sequences[0].tracks[0].clips[1].opacity.setConstant(0.4);
    player.setSnapshot(ProjectSnapshot{std::make_shared<const Project>(p), "s", "", 2});
    EXPECT_TRUE(player.renderNow(260).fromRenderCache);
    EXPECT_FALSE(player.renderNow(282).fromRenderCache);
    EXPECT_FALSE(player.renderNow(305).fromRenderCache);
    // compare mode never uses it
    player.setCompare(true);
    EXPECT_FALSE(player.renderNow(260).fromRenderCache);
    player.setCompare(false);

    // cancellation stops early
    std::atomic<bool> stop{true};
    req.from = 0;
    req.to = 200;
    const RenderPreviewResult cancelled = renderPreviewRange(req, *cache, stop);
    EXPECT_TRUE(cancelled.cancelled);
    EXPECT_EQ(cancelled.rendered, 0);

    // a different backend than the player's is refused (its pixels may differ by a level)
    req.expectBackend = "gpu: something else";
    req.to = 20;
    const RenderPreviewResult wrong = renderPreviewRange(req, *cache, cancel);
    EXPECT_FALSE(wrong.error.empty());
    EXPECT_EQ(wrong.rendered, 0);
    (void)first;
}
