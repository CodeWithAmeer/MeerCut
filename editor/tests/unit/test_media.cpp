// FFmpeg integration against the Stage 1 test media (testmedia/A_red..., B_blue...: 1920x1080,
// 30 fps, 10 s, H.264, frame index strip on every frame).
#include "fa/media/media.h"

#include "../support/framecode.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <random>

using namespace fa;

namespace {

std::filesystem::path media(const char* name) { return std::filesystem::path(FA_TESTMEDIA_DIR) / name; }

}  // namespace

TEST(Media, ProbeReportsExactStreamFacts) {
    const MediaInfo mi = probeMedia(media("A_red_1080p30_10s.mp4"));
    ASSERT_TRUE(mi.video);
    EXPECT_EQ(mi.video->codec, "h264");
    EXPECT_EQ(mi.video->width, 1920);
    EXPECT_EQ(mi.video->height, 1080);
    EXPECT_EQ(mi.video->frameRate, FrameRate::fps30());
    ASSERT_TRUE(mi.video->duration);
    EXPECT_EQ(*mi.video->duration, Rational(10));
    ASSERT_TRUE(mi.video->frameCount);
    EXPECT_EQ(*mi.video->frameCount, 300);
    ASSERT_TRUE(mi.audio);
    EXPECT_EQ(mi.audio->sampleRate, 48000);
}

TEST(Media, ProbeMissingFileFailsClearly) {
    try {
        probeMedia(media("does_not_exist.mp4"));
        FAIL();
    } catch (const MediaError& e) {
        EXPECT_NE(std::string(e.what()).find("not found"), std::string::npos) << e.what();
    }
}

TEST(Media, SequentialDecodeReturnsEveryFrameInOrder) {
    VideoDecoder dec(media("A_red_1080p30_10s.mp4"));
    for (int n = 0; n < 300; ++n) {
        const VideoFrame& f = dec.frameAt(frameToTime(n, FrameRate::fps30()));
        ASSERT_EQ(f.index, n);
        ASSERT_EQ(test::readStrip(f.image).index, n) << "frame " << n;
    }
    EXPECT_EQ(dec.seekCount(), 0);   // pure forward decoding
    EXPECT_EQ(dec.decodedFrameCount(), 300);
    // Untagged HD H.264 decoded with BT.709 (Filmora behaviour): #C00000 -> about (208, 18, 0).
    const Rgba8 c = dec.frameAt(Rational(5)).image.at(960, 540);
    EXPECT_NEAR(c.r, 208, 3);
    EXPECT_NEAR(c.g, 18, 3);
    EXPECT_NEAR(c.b, 0, 3);
}

TEST(Media, RandomAccessIsFrameExact) {
    VideoDecoder dec(media("B_blue_1080p30_10s.mp4"));
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> pick(0, 299);
    for (int i = 0; i < 40; ++i) {
        const int n = pick(rng);
        const VideoFrame& f = dec.frameAt(frameToTime(n, FrameRate::fps30()));
        EXPECT_EQ(f.index, n);
        EXPECT_EQ(test::readStrip(f.image).index, n) << "request " << n;
    }
    EXPECT_GT(dec.seekCount(), 0);
}

TEST(Media, TimesInsideAFrameMapToThatFrame) {
    VideoDecoder dec(media("A_red_1080p30_10s.mp4"));
    // 29.97 sequence sampling a 30 fps source: sequence frame k at k*1001/30000 s -> source floor(k*1.001)
    for (int k : {0, 1, 499, 500, 999, 1000 % 300}) {
        const Time t = frameToTime(k, FrameRate::ntsc30());
        const int expected = static_cast<int>((static_cast<int64_t>(k) * 1001) / 1000);
        if (expected >= 300) continue;
        EXPECT_EQ(test::readStrip(dec.frameAt(t).image).index, expected) << k;
    }
    // just before the next frame boundary (beyond the decoder's half-tick tolerance: 1/30720 s here)
    EXPECT_EQ(dec.frameAt(Rational(10, 30) - Rational(1, 10000)).index, 9);
    EXPECT_EQ(dec.frameAt(Rational(10, 30)).index, 10);
}

TEST(Media, ClampsBeforeStartAndAfterEnd) {
    VideoDecoder dec(media("A_red_1080p30_10s.mp4"));
    EXPECT_EQ(dec.frameAt(Rational(-1)).index, 0);
    EXPECT_EQ(dec.frameAt(Rational(299, 30)).index, 299);
    EXPECT_EQ(dec.frameAt(Rational(12)).index, 299);   // past the end: last frame
}

TEST(Media, EncodeDecodeRoundTripNtsc) {
    const auto out = std::filesystem::temp_directory_path() / "fa_encoder_roundtrip.mp4";
    EncoderSettings s;
    s.width = 640;
    s.height = 360;
    s.frameRate = FrameRate::ntsc30();
    s.preset = "veryfast";
    s.crf = 12;
    const int frames = 47;
    {
        VideoEncoder enc(out, s);
        Image img;
        for (int n = 0; n < frames; ++n) {
            img.reset(640, 360, Rgba8{static_cast<uint8_t>(n * 5), 80, 160, 255});
            test::drawStrip(img, n);
            enc.writeFrame(img);
        }
        enc.finish();
        EXPECT_EQ(enc.framesWritten(), frames);
    }
    const MediaInfo mi = probeMedia(out);
    ASSERT_TRUE(mi.video);
    EXPECT_EQ(mi.video->codec, "h264");
    EXPECT_EQ(mi.video->frameRate, FrameRate::ntsc30());
    ASSERT_TRUE(mi.video->frameCount);
    EXPECT_EQ(*mi.video->frameCount, frames);
    EXPECT_EQ(mi.video->colorSpace, "bt709");
    EXPECT_EQ(mi.video->colorRange, "tv");
    ASSERT_TRUE(mi.video->duration);
    EXPECT_EQ(*mi.video->duration, frameToTime(frames, FrameRate::ntsc30()));

    {
        VideoDecoder dec(out);
        for (int n = 0; n < frames; ++n) {
            const VideoFrame& f = dec.frameAt(frameToTime(n, FrameRate::ntsc30()));
            EXPECT_EQ(f.index, n);
            EXPECT_EQ(test::readStrip(f.image).index, n);
            EXPECT_NEAR(f.image.at(320, 300).r, n * 5, 3);
        }
    }   // close the file before deleting it
    std::filesystem::remove(out);
}

// Frames of another size are scaled to the output size since M6 (export resolution presets); an empty frame is still
// refused, and an encoder destroyed without finish() removes its partial file.
TEST(Media, EncoderScalesOtherSizesRefusesEmptyFramesAndCleansUp) {
    const auto out = std::filesystem::temp_directory_path() / "fa_encoder_abort.mp4";
    {
        EncoderSettings s;
        s.width = 320;
        s.height = 240;
        VideoEncoder enc(out, s);
        EXPECT_NO_THROW(enc.writeFrame(Image(100, 100)));
        EXPECT_THROW(enc.writeFrame(Image()), MediaError);
    }   // destroyed without finish(): partial file removed
    EXPECT_FALSE(std::filesystem::exists(out));
    EncoderSettings odd;
    odd.width = 321;
    odd.height = 240;
    EXPECT_THROW(VideoEncoder(out, odd), MediaError);
}

// M2: the slice-threaded colour conversion must give exactly the single-threaded result.
TEST(Media, ThreadedColourConversionIsBitExact) {
    VideoDecoder::Options one;
    one.convertThreads = 1;
    VideoDecoder::Options many;
    many.convertThreads = 8;
    VideoDecoder a(media("C_testsrc2_1080p30_10s.mp4"), one), b(media("C_testsrc2_1080p30_10s.mp4"), many);
    for (int n : {0, 1, 2, 45, 150, 299}) {
        const Time t = frameToTime(n, FrameRate::fps30());
        const Image ia = a.frameAt(t).image;
        const Image& ib = b.frameAt(t).image;
        ASSERT_EQ(ia.pixels, ib.pixels) << "frame " << n;
    }
    // preview-size (scaled) conversion too
    one.maxWidth = many.maxWidth = 960;
    one.maxHeight = many.maxHeight = 540;
    VideoDecoder c(media("C_testsrc2_1080p30_10s.mp4"), one), d(media("C_testsrc2_1080p30_10s.mp4"), many);
    for (int n : {0, 77, 230}) {
        const Time t = frameToTime(n, FrameRate::fps30());
        const Image ic = c.frameAt(t).image;
        ASSERT_EQ(ic.pixels, d.frameAt(t).image.pixels) << "scaled frame " << n;
    }
}

// M2: D3D11VA hardware decoding delivers the same frames (index exact, colours within rounding of the
// NV12 vs planar conversion path); skipped when the machine has no hardware decoder.
TEST(Media, HardwareDecodingMatchesSoftware) {
    VideoDecoder::Options hw;
    hw.hardware = true;
    VideoDecoder h(media("A_red_1080p30_10s.mp4"), hw), s(media("A_red_1080p30_10s.mp4"));
    h.frameAt(Time(0));
    if (!h.hardwareActive()) GTEST_SKIP() << "no hardware decoding: " << h.decoderDescription();
    std::printf("  %s\n", h.decoderDescription().c_str());
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> pick(0, 299);
    for (int i = 0; i < 30; ++i) {
        const int n = i < 10 ? i : pick(rng);   // sequential start, then random access
        const Time t = frameToTime(n, FrameRate::fps30());
        const VideoFrame& fh = h.frameAt(t);
        const Image ih = fh.image;
        ASSERT_EQ(fh.index, n);
        ASSERT_EQ(test::readStrip(ih).index, n) << "hardware frame " << n;
        const Image& is = s.frameAt(t).image;
        ASSERT_EQ(ih.width, is.width);
        int maxDiff = 0;
        for (size_t k = 0; k < ih.pixels.size(); ++k) maxDiff = std::max(maxDiff, std::abs(int(ih.pixels[k]) - int(is.pixels[k])));
        EXPECT_LE(maxDiff, 3) << "frame " << n;
    }
    EXPECT_TRUE(h.hardwareActive());
}
