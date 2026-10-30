// Playback library: background preview rendering and the audio playback clock (headless).
#include "fa/model/edit_ops.h"
#include "fa/model/time_map.h"
#include "fa/playback/audio_player.h"
#include "fa/playback/preview_renderer.h"
#include "fa/render/renderer.h"

#include "../support/framecode.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

using namespace fa;

namespace {

std::filesystem::path media(const char* name) { return std::filesystem::path(FA_TESTMEDIA_DIR) / name; }

ProjectSnapshot abSnapshot(uint64_t version = 1) {
    auto p = std::make_shared<Project>();
    p->sources = {MediaSource{"A", media("A_red_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920, 1080,
                              true, true},
                  MediaSource{"B", media("B_blue_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920,
                              1080, true, true}};
    Sequence s;
    s.id = "s";
    s.frameRate = FrameRate::fps30();
    p->sequences.push_back(s);
    UndoStack st(*p);
    st.execute(edit::addMedia(*p, "s", "A", Rational(0)));
    st.execute(edit::addMedia(*p, "s", "B", Rational(10)));
    ProjectSnapshot snap;
    snap.project = p;
    snap.sequenceId = "s";
    snap.version = version;
    return snap;
}

struct Waiter {
    std::mutex m;
    std::condition_variable cv;
    std::vector<PreviewFrame> frames;
    PreviewFrame waitFor(int64_t index) {
        std::unique_lock lk(m);
        PreviewFrame out;
        cv.wait_for(lk, std::chrono::seconds(10), [&] {
            for (const auto& f : frames)
                if (f.index == index) {
                    out = f;
                    return true;
                }
            return false;
        });
        return out;
    }
};

}  // namespace

TEST(PreviewRenderer, DeliversFrameAccurateFramesAtPreviewSize) {
    PreviewRenderer r(960, 540);
    Waiter w;
    r.setCallback([&](const PreviewFrame& f) {
        std::lock_guard lk(w.m);
        w.frames.push_back(f);
        w.cv.notify_all();
    });
    r.setSnapshot(abSnapshot());
    for (int64_t f : {299, 300, 0, 450}) {
        r.request(f);
        const PreviewFrame pf = w.waitFor(f);
        ASSERT_TRUE(pf.image) << "frame " << f;
        EXPECT_EQ(pf.image->width, 960);
        EXPECT_EQ(pf.image->height, 540);
        EXPECT_EQ(test::readStrip(*pf.image).index, f < 300 ? f : f - 300) << "frame " << f;
        EXPECT_TRUE(pf.offline.empty());
    }
}

TEST(PreviewRenderer, PrefetchFillsTheCacheAndEditsInvalidateIt) {
    PreviewRenderer r(480, 270);
    r.setSnapshot(abSnapshot(1));
    r.setPrefetch(100, 10);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!r.cached(109) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(r.cached(109));
    EXPECT_EQ(test::readStrip(*r.cached(105)->image).index, 105);
    r.setSnapshot(abSnapshot(2));   // an edit
    EXPECT_FALSE(r.cached(105));
}

// M5: retimed clips through the shared renderFrame (preview and export): the frame-index strip of A_red shows
// exactly the source frames the time map chooses (2x: 0, 2, 4 ...; Bullet ramp: Filmora's 107 frames).
TEST(PreviewRenderer, RetimedClipsShowTheMappedSourceFrames) {
    for (int variant = 0; variant < 3; ++variant) {
        const auto t0 = std::chrono::steady_clock::now();
        ProjectSnapshot snap = abSnapshot();
        auto p = std::make_shared<Project>(*snap.project);
        Track& v = p->sequences[0].tracks[0];
        v.clips.erase(v.clips.begin() + 1);   // A only
        Clip& c = v.clips[0];
        if (variant == 0) c.speed.speed = Rational(2);
        else if (variant == 1) c.speed.ramp = rampKeys(findRampPreset("bullet")->keys, Time(0), Time(10));
        else c.speed.reverse = true;   // backwards through the decoder's chunk cache
        c.duration = frameDurationForSource(c.speed, Time(0), Time(10), FrameRate::fps30(), Time(10));
        snap.project = p;
        PreviewRenderer r(480, 270);
        r.setBackend(BackendChoice::Cpu);
        r.setSnapshot(snap);
        const int64_t n = (c.duration * Rational(30)).floor();
        std::vector<int64_t> got;
        for (int64_t k = 0; k < n; k += variant == 0 ? 7 : 1) {
            const PreviewFrame f = r.renderNow(k);
            ASSERT_TRUE(f.image);
            got.push_back(test::readStrip(*f.image).index);
            const int64_t want = variant == 0 ? 2 * k
                                 : variant == 2 ? 299 - k
                                                : static_cast<int64_t>((sampleFrame(c, Time(k, 30), Time(1, 30), FrameRate::fps30()).time * Rational(30)).floor());
            ASSERT_EQ(got.back(), want) << "variant " << variant << ", output frame " << k;
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  variant %d: %zu frames in %.2f s\n", variant, got.size(), secs);
        if (variant == 2) EXPECT_EQ(got.size(), 300u);
        if (variant == 1) {
            EXPECT_EQ(n, 107);
            EXPECT_EQ(got.front(), 0);
            EXPECT_EQ(got[30], 138);   // the slow-motion middle (Filmora: 138, 138, 139, 139 ...)
            EXPECT_EQ(got.back(), 296);
        }
    }
}

TEST(PreviewRenderer, MatchesTheExportRenderAtFullSize) {
    const ProjectSnapshot snap = abSnapshot();
    PreviewRenderer r(1920, 1080);
    r.setSnapshot(snap);
    DecoderPool pool("");
    auto backend = makeCpuBackend();
    for (int64_t f : {12, 310}) {
        const PreviewFrame pf = r.renderNow(f);
        const Image& ex = renderFrame(*snap.sequence(), f, *snap.project, pool, *backend);
        ASSERT_EQ(pf.image->width, ex.width);
        EXPECT_EQ(pf.image->pixels, ex.pixels) << "preview and export differ at frame " << f;
    }
}

TEST(PreviewRenderer, OfflineMediaBecomesAPlaceholder) {
    ProjectSnapshot snap = abSnapshot();
    auto p = std::make_shared<Project>(*snap.project);
    p->sources[0].path = "C:/no/such/file.mp4";
    snap.project = p;
    PreviewRenderer r(320, 180);
    r.setSnapshot(snap);
    const PreviewFrame f = r.renderNow(10);
    ASSERT_TRUE(f.image);
    ASSERT_EQ(f.offline.size(), 1u);
    EXPECT_GT(f.image->at(160, 90).r, f.image->at(160, 90).b);   // dark red placeholder
    const PreviewFrame g = r.renderNow(310);                     // B is still fine
    EXPECT_EQ(test::readStrip(*g.image).index, 10);
}

// Before / after: the left half is the frame without effects, the right half the frame with them - both exactly
// what the export renders; switching does not keep frames of the other mode in the cache.
TEST(PreviewRenderer, CompareShowsTheFrameWithoutEffectsOnTheLeft) {
    ProjectSnapshot snap = abSnapshot();
    auto p = std::make_shared<Project>(*snap.project);
    EffectRef bw;
    bw.id = "bw";
    bw.type = "fa.filter.bw";
    p->sequences[0].tracks[0].clips[0].effects.push_back(bw);
    snap.project = p;
    PreviewRenderer r(1920, 1080);
    r.setBackend(BackendChoice::Cpu);
    r.setSnapshot(snap);
    DecoderPool pool("");
    auto backend = makeCpuBackend();
    const Image with = renderFrame(*snap.sequence(), 12, *p, pool, *backend);
    Project plain = *p;
    plain.sequences[0].tracks[0].clips[0].effects.clear();
    const Image without = renderFrame(plain.sequences[0], 12, plain, pool, *backend);
    ASSERT_NE(with.pixels, without.pixels);   // the filter changes the red picture

    const PreviewFrame normal = r.renderNow(12);
    EXPECT_FALSE(normal.compare);
    EXPECT_EQ(normal.image->pixels, with.pixels);
    r.setCompare(true);
    EXPECT_FALSE(r.cached(12));   // the frame of the other mode is gone
    const PreviewFrame split = r.renderNow(12);
    ASSERT_TRUE(split.image);
    EXPECT_TRUE(split.compare);
    const size_t half = 960 * 4;
    for (int y = 0; y < 1080; y += 7) {
        ASSERT_EQ(std::memcmp(split.image->row(y), without.row(y), half), 0) << "left half, row " << y;
        ASSERT_EQ(std::memcmp(split.image->row(y) + half, with.row(y) + half, half), 0) << "right half, row " << y;
    }
    r.setCompare(false);
    EXPECT_EQ(r.renderNow(12).image->pixels, with.pixels);
}

// Content keys (GPU texture reuse) are a promise: the same file, decoder and frame give the same key; another frame,
// or the same source id relinked to another file, never does.
TEST(DecoderPool, ContentKeysIdentifyFileAndFrame) {
    DecoderPool pool("");
    MediaSource src{"A", media("A_red_1080p30_10s.mp4").string(), Rational(10), FrameRate::fps30(), 1920, 1080, true, true};
    Clip c;
    c.id = "c";
    c.sourceId = "A";
    c.duration = Rational(10);
    const uint64_t k5 = pool.frame(c, src, Rational(5, 30)).contentKey;
    EXPECT_NE(k5, 0u);
    EXPECT_EQ(pool.frame(c, src, Rational(6, 30)).frameIndex, 6);
    const uint64_t k6 = pool.frame(c, src, Rational(6, 30)).contentKey;
    EXPECT_NE(k5, k6);
    EXPECT_EQ(pool.frame(c, src, Rational(5, 30)).contentKey, k5);   // back to frame 5
    src.path = media("B_blue_1080p30_10s.mp4").string();             // relinked: same id, other file
    const FrameProvider::Result b = pool.frame(c, src, Rational(5, 30));
    EXPECT_EQ(b.frameIndex, 5);
    EXPECT_NE(b.contentKey, k5);
}

TEST(AudioPlayer, ClockAdvancesInRealTimeOnTheNullDevice) {
    AudioPlayer player(AudioPlayer::Backend::Null);
    ASSERT_TRUE(player.deviceAvailable()) << player.lastError();
    const ProjectSnapshot snap = abSnapshot();
    player.start(snap, 48000);   // 1 s into the timeline
    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const int64_t pos = player.position();
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    player.stop();
    EXPECT_NEAR(static_cast<double>(pos - 48000), elapsed * 48000, 48000 * 0.08) << "clock drifts from real time";
    EXPECT_EQ(player.underrunSamples(), 0) << "the mixer could not keep up";
    EXPECT_FALSE(player.playing());
}
