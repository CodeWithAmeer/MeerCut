// M4 HDR handling: PQ / HLG transfer functions against their standards, the documented HDR -> SDR tone mapping
// (reference white, knee, peak, hue kept, gamut clipping) and the fast row conversion used by the decoder.
#include "fa/media/hdr.h"
#include "fa/media/media.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace fa;

TEST(HdrTransfer, PqAndHlgMatchTheStandards) {
    // SMPTE ST 2084: 0 -> 0, 1 -> 10000 nits; signal 0.5081 ~ 100 nits, 0.5807 ~ 203 nits (BT.2408 reference white),
    // 0.7518 ~ 1000 nits
    EXPECT_NEAR(HdrToSdr::pqToNits(0.0f), 0.0f, 1e-6f);
    EXPECT_NEAR(HdrToSdr::pqToNits(1.0f), 10000.0f, 0.5f);
    EXPECT_NEAR(HdrToSdr::pqToNits(0.5081f), 100.0f, 1.0f);
    EXPECT_NEAR(HdrToSdr::pqToNits(0.5807f), 203.0f, 1.5f);
    EXPECT_NEAR(HdrToSdr::pqToNits(0.7518f), 1000.0f, 5.0f);
    // ARIB STD-B67 inverse OETF: 0.5 -> 1/12, 1 -> 1 (continuous at 0.5)
    EXPECT_NEAR(HdrToSdr::hlgToScene(0.5f), 1.0f / 12.0f, 1e-6f);
    EXPECT_NEAR(HdrToSdr::hlgToScene(0.5001f), 1.0f / 12.0f, 1e-4f);
    EXPECT_NEAR(HdrToSdr::hlgToScene(1.0f), 1.0f, 1e-4f);
    EXPECT_EQ(hdrTransferFromName("smpte2084"), HdrTransfer::PQ);
    EXPECT_EQ(hdrTransferFromName("arib-std-b67"), HdrTransfer::HLG);
    EXPECT_EQ(hdrTransferFromName("bt709"), HdrTransfer::None);
}

TEST(HdrToneMap, KneeAndPeak) {
    const HdrToSdr pq(HdrTransfer::PQ, true, 1000.0f);
    for (float x : {0.0f, 0.1f, 0.5f, 0.75f}) EXPECT_FLOAT_EQ(pq.toneMap(x), x);   // unchanged below the knee
    EXPECT_NEAR(pq.toneMap(1000.0f / 203.0f), 1.0f, 1e-5f);                        // content peak -> 100 %
    EXPECT_EQ(pq.toneMap(50.0f), 1.0f);                                             // beyond the peak: clipped
    float prev = 0;
    for (float x = 0; x < 6; x += 0.01f) {   // monotonic and continuous
        const float y = pq.toneMap(x);
        EXPECT_GE(y + 1e-6f, prev);
        EXPECT_LT(y - prev, 0.012f);
        prev = y;
    }
}

TEST(HdrToSdrConversion, ReferenceWhiteHueAndGamut) {
    const HdrToSdr pq(HdrTransfer::PQ, true);
    // a grey at reference white (203 nits) lands at 87.5 % light (signal ~241): bright, with headroom for highlights
    const auto w = pq.convert(0.5807f, 0.5807f, 0.5807f);
    EXPECT_NEAR(w[0] * 255, 241, 2);
    EXPECT_NEAR(w[0], w[1], 1e-3f);
    EXPECT_NEAR(w[1], w[2], 1e-3f);
    // dark greys keep their relation (below the knee only the transfer functions act)
    const auto d1 = pq.convert(0.3f, 0.3f, 0.3f), d2 = pq.convert(0.4f, 0.4f, 0.4f);
    EXPECT_LT(d1[0], d2[0]);
    // a bright saturated colour is scaled as a whole (hue kept), not clipped channel by channel
    const auto c = pq.convert(0.75f, 0.55f, 0.45f);
    EXPECT_GT(c[0], c[1]);
    EXPECT_GT(c[1], c[2]);
    EXPECT_LE(c[0], 1.0f);
    // BT.2020 green is outside BT.709: clipped to the gamut, never NaN
    const auto g = pq.convert(0.0f, 0.6f, 0.0f);
    for (float v : g) EXPECT_TRUE(v >= 0.0f && v <= 1.0f);
    EXPECT_GT(g[1], 0.5f);
    // HLG: 75 % signal (HLG reference white per BT.2408) is a bright, neutral grey
    const HdrToSdr hlg(HdrTransfer::HLG, true);
    const auto h = hlg.convert(0.75f, 0.75f, 0.75f);
    EXPECT_GT(h[0] * 255, 200);
    EXPECT_NEAR(h[0], h[2], 1e-3f);
}

TEST(HdrToSdrConversion, RowConversionMatchesThePixelFunction) {
    for (HdrTransfer t : {HdrTransfer::PQ, HdrTransfer::HLG}) {
        const HdrToSdr conv(t, true);
        std::vector<uint16_t> row;
        for (int i = 0; i < 64; ++i)
            for (int c = 0; c < 3; ++c) row.push_back(static_cast<uint16_t>((i * 997 + c * 15013) % 65536));
        std::vector<uint8_t> out(64 * 4);
        conv.convertRow(row.data(), out.data(), 64);
        for (int i = 0; i < 64; ++i) {
            const auto ref = conv.convert(row[i * 3] / 65535.0f, row[i * 3 + 1] / 65535.0f, row[i * 3 + 2] / 65535.0f);
            for (int c = 0; c < 3; ++c) EXPECT_NEAR(out[i * 4 + c], ref[c] * 255, 1.5) << i << " " << c;
            EXPECT_EQ(out[i * 4 + 3], 255);
        }
    }
}

TEST(HdrDecode, PqAndHlgClipsAreToneMappedToSdr) {
    // tools/make_testmedia.ps1: 10-bit BT.2020 clips with exact code values: left half reference white (PQ 203 nits,
    // HLG 75 %), right half R'G'B' 0.6 / 0.3 / 0.3. The decoder (swscale + tables) must match the model.
    for (const char* name : {"P_hdr_pq_1080p30_2s.mp4", "P_hdr_hlg_1080p30_2s.mp4"}) {
        const bool pq = std::string(name).find("_pq_") != std::string::npos;
        const HdrToSdr model(pq ? HdrTransfer::PQ : HdrTransfer::HLG, true);
        const auto red = model.convert(0.6f, 0.3f, 0.3f);
        const auto file = std::filesystem::path(FA_TESTMEDIA_DIR) / name;
        ASSERT_TRUE(std::filesystem::exists(file)) << name << " missing: run tools/make_testmedia.ps1";
        const MediaInfo mi = probeMedia(file);
        ASSERT_TRUE(mi.video.has_value());
        EXPECT_TRUE(mi.video->isHdr()) << mi.video->transfer;
        EXPECT_EQ(mi.video->primaries, "bt2020");
        VideoDecoder dec(file);
        EXPECT_TRUE(dec.info().isHdr());
        const VideoFrame& f = dec.frameAt(Time(1));
        const Rgba8 w = f.image.at(480, 540), r = f.image.at(1440, 540);
        std::printf("  %s: reference white -> %d %d %d, red -> %d %d %d\n", name, w.r, w.g, w.b, r.r, r.g, r.b);
        EXPECT_NEAR(w.g, 241, 2) << name;   // reference white at 87.5 % light, not clipped, not washed out
        EXPECT_LE(std::abs(int(w.r) - int(w.g)), 1) << name;
        EXPECT_LE(std::abs(int(w.b) - int(w.g)), 1) << name;
        EXPECT_NEAR(r.r, red[0] * 255, 3) << name;   // a red that stays red, as the model says
        EXPECT_NEAR(r.g, red[1] * 255, 3) << name;
        EXPECT_NEAR(r.b, red[2] * 255, 3) << name;
        EXPECT_GT(r.r, r.g + 60) << name;
    }
    // SDR sources are not touched by any of this
    const auto sdr = std::filesystem::path(FA_TESTMEDIA_DIR) / "A_red_1080p30_10s.mp4";
    EXPECT_FALSE(probeMedia(sdr).video->isHdr());
}
