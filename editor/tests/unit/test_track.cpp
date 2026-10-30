// M5 motion tracking (fa/vision/track.h) against the Stage 1 ground truth (EXP-013, testmedia/L_track_1080p30_5s.mp4:
// a 160 px checker patch moving x = 960 + 140 t, y = 540 + 120 sin(2 pi t / 5)). Filmora 16 measured on the same
// clip: RMS 7.7 px, max 13.6 px. Plus an occlusion: the tracker must say it lost the target, then find it again.
#include "fa/media/media.h"
#include "fa/model/serialize.h"
#include "fa/model/time_map.h"
#include "fa/vision/track.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <vector>

using namespace fa;

namespace {

std::filesystem::path media(const char* name) { return std::filesystem::path(FA_TESTMEDIA_DIR) / name; }

// The generator's path (tools/make_motion_tests.py): the patch is placed at integer-rounded corners.
std::pair<double, double> truth(int n) {
    const double t = n / 30.0;
    const double cx = 960 + 140 * t, cy = 540 + 120 * std::sin(2 * std::numbers::pi * t / 5);
    return {std::round(cx - 80) + 80, std::round(cy - 80) + 80};
}

void fill(Image& img, int x0, int y0, int w, int h, Rgba8 c) {
    for (int y = std::max(0, y0); y < std::min(img.height, y0 + h); ++y)
        for (int x = std::max(0, x0); x < std::min(img.width, x0 + w); ++x) {
            uint8_t* p = img.row(y) + x * 4;
            p[0] = c.r;
            p[1] = c.g;
            p[2] = c.b;
            p[3] = c.a;
        }
}

}  // namespace

TEST(MotionTracking, FollowsTheExp013TargetBetterThanFilmora) {
    VideoDecoder dec(media("L_track_1080p30_5s.mp4"));
    vision::RegionTracker tr;
    const auto [x0, y0] = truth(0);
    ASSERT_TRUE(tr.start(dec.frameAt(Rational(0)).image, vision::Box{x0, y0, 160, 160}));
    double sq = 0, worst = 0, minConf = 1;
    int n = 0;
    for (int f = 1; f < 150; ++f) {
        const vision::TrackPoint p = tr.next(dec.frameAt(Rational(f, 30)).image);
        const auto [tx, ty] = truth(f);
        const double e = std::hypot(p.cx - tx, p.cy - ty);
        sq += e * e;
        worst = std::max(worst, e);
        minConf = std::min(minConf, p.confidence);
        EXPECT_FALSE(p.lost) << "frame " << f;
        ++n;
    }
    const double rms = std::sqrt(sq / n);
    std::printf("  L_track: RMS %.2f px, max %.2f px over %d frames, lowest confidence %.2f (Filmora: 7.7 / 13.6 px)\n", rms,
                worst, n, minConf);
    EXPECT_LT(rms, 7.7);
    EXPECT_LT(worst, 13.6);
}

// A grey card covers the target for 15 frames: the tracker reports it (lost or low confidence) and picks the target
// up again afterwards; the reported positions after the occlusion are back on the path.
TEST(MotionTracking, ReportsAnOcclusionAndRecovers) {
    VideoDecoder dec(media("L_track_1080p30_5s.mp4"));
    vision::RegionTracker tr;
    const auto [x0, y0] = truth(0);
    ASSERT_TRUE(tr.start(dec.frameAt(Rational(0)).image, vision::Box{x0, y0, 160, 160}));
    int flagged = 0;
    double after = 0;
    for (int f = 1; f < 90; ++f) {
        Image img = dec.frameAt(Rational(f, 30)).image;
        const bool covered = f >= 40 && f < 55;
        if (covered) {
            const auto [tx, ty] = truth(f);
            fill(img, static_cast<int>(tx) - 110, static_cast<int>(ty) - 110, 220, 220, Rgba8{128, 128, 128, 255});
        }
        const vision::TrackPoint p = tr.next(img);
        if (covered && (p.lost || p.confidence < 0.5)) ++flagged;
        if (f >= 65) {
            const auto [tx, ty] = truth(f);
            after = std::max(after, std::hypot(p.cx - tx, p.cy - ty));
        }
    }
    std::printf("  occlusion: %d of 15 covered frames flagged; worst error 10+ frames later %.1f px\n", flagged, after);
    EXPECT_GE(flagged, 10);
    EXPECT_LT(after, 20.0);
}

TEST(MotionTracking, RefusesATexturelessRegion) {
    Image flat(640, 360, Rgba8{90, 90, 90, 255});
    vision::RegionTracker tr;
    EXPECT_FALSE(tr.start(flat, vision::Box{320, 180, 100, 100}));
}

// Negative control for the measurement above: a box placed 12 px to the right of the target is followed faithfully,
// so the error against the ground truth stays at the placement offset - the comparison does see a wrong position.
TEST(MotionTracking, NegativeControlMeasuresAMisplacedBox) {
    VideoDecoder dec(media("L_track_1080p30_5s.mp4"));
    vision::RegionTracker tr;
    const auto [x0, y0] = truth(0);
    ASSERT_TRUE(tr.start(dec.frameAt(Rational(0)).image, vision::Box{x0 + 12, y0, 160, 160}));
    double sq = 0;
    int n = 0;
    for (int f = 1; f < 150; f += 1) {
        const vision::TrackPoint p = tr.next(dec.frameAt(Rational(f, 30)).image);
        const auto [tx, ty] = truth(f);
        sq += std::pow(p.cx - tx, 2) + std::pow(p.cy - ty, 2);
        ++n;
    }
    const double rms = std::sqrt(sq / n);
    std::printf("  box 12 px off: RMS %.2f px against the ground truth\n", rms);
    EXPECT_GT(rms, 10.0);
    EXPECT_LT(rms, 14.0);
}

// The stored result: exact round trip in the clip format, errors that name the field, nearest-frame lookup, and a
// media identity that changes when the file does (stale tracking data).
TEST(MotionTracking, DataRoundTripsValidatesAndGoesStale) {
    Clip c;
    c.id = "c1";
    c.sourceId = "s1";
    c.duration = Rational(5);
    MotionTrack t;
    t.boxX = 0.68;
    t.boxY = 0.5;
    t.boxW = 160.0 / 1920;
    t.boxH = 160.0 / 1080;
    t.firstFrame = 75;
    t.media = "C:/m/L.mp4|123|456";
    for (int64_t f = 0; f < 150; ++f) t.samples.push_back({f, 0.5 + f / 1000.0, 0.5, 1.0 + f / 10000.0, 0.97, f == 40});
    c.tracking = t;
    const Clip back = clipFromJsonString(clipToJsonString(c));
    ASSERT_TRUE(back.tracking.has_value());
    EXPECT_EQ(*back.tracking, t);

    auto expectError = [&](const std::string& from, const std::string& to, const std::string& fragment) {
        std::string text = clipToJsonString(c, -1);
        const auto at = text.find(from);
        ASSERT_NE(at, std::string::npos) << from;
        text.replace(at, from.size(), to);
        try {
            clipFromJsonString(text);
            ADD_FAILURE() << "no error for " << to;
        } catch (const ModelError& e) {
            EXPECT_NE(std::string(e.what()).find(fragment), std::string::npos) << e.what();
        }
    };
    expectError("[0,0.5,0.5,1.0,0.97,0]", "[0,0.5,0.5,1.0]", "tracking.samples[0]");
    expectError("[1,0.501,0.5", "[0,0.501,0.5", "frames must increase");
    expectError("[0,0.5,0.5,1.0,0.97,0]", "[0,0.5,0.5,1.0,1.5,0]", "confidence");
    expectError("\"firstFrame\":75", "\"firstFrame\":\"x\"", "tracking.firstFrame");

    EXPECT_EQ(trackSampleAt(t, 10)->frame, 10);
    EXPECT_EQ(trackSampleAt(t, 150)->frame, 149);   // one frame past the end: the nearest sample
    EXPECT_EQ(trackSampleAt(t, 151), nullptr);
    EXPECT_EQ(trackSampleAt(t, -2), nullptr);

    const auto file = std::filesystem::temp_directory_path() / "fa_track_identity.bin";
    std::ofstream(file, std::ios::binary) << "abc";
    const std::string id0 = mediaIdentity(file);
    EXPECT_FALSE(id0.empty());
    EXPECT_EQ(mediaIdentity(file), id0);
    std::ofstream(file, std::ios::binary | std::ios::app) << "more";
    EXPECT_NE(mediaIdentity(file), id0);
    std::filesystem::remove(file);
    EXPECT_TRUE(mediaIdentity(file).empty());
}
