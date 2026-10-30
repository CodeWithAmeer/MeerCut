// M6 audio DSP building blocks: gain laws (EXP-020 where measured), crossfade curves, RBJ biquads, EQ presets.
#include "fa/render/audio_dsp.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

using namespace fa;

TEST(AudioDsp, GainConversionIsExact) {
    EXPECT_DOUBLE_EQ(dbToGain(0.0), 1.0);
    EXPECT_NEAR(dbToGain(-6.0206), 0.5, 1e-5);
    EXPECT_NEAR(dbToGain(6.0206), 2.0, 1e-4);
    EXPECT_NEAR(dbToGain(-20.0), 0.1, 1e-12);
    EXPECT_EQ(dbToGain(-120.0), 0.0);   // the silence floor
    EXPECT_NEAR(gainToDb(dbToGain(-37.5)), -37.5, 1e-9);
    EXPECT_TRUE(std::isinf(gainToDb(0.0)));
}

// EXP-020: the near channel stays at unity, the far channel follows cos(pi |b| / 2); measured -50 -> -3.02 dB,
// -80 -> -10.21 dB on the far (right) channel.
TEST(AudioDsp, BalanceFollowsTheMeasuredLaw) {
    double l = 0, r = 0;
    balanceGains(0.0, l, r);
    EXPECT_DOUBLE_EQ(l, 1.0);
    EXPECT_DOUBLE_EQ(r, 1.0);
    balanceGains(-0.5, l, r);
    EXPECT_DOUBLE_EQ(l, 1.0);
    EXPECT_NEAR(gainToDb(r), -3.01, 0.02);
    balanceGains(-0.8, l, r);
    EXPECT_NEAR(gainToDb(r), -10.2, 0.05);
    balanceGains(0.8, l, r);   // mirrored
    EXPECT_NEAR(gainToDb(l), -10.2, 0.05);
    EXPECT_DOUBLE_EQ(r, 1.0);
    balanceGains(-1.0, l, r);
    EXPECT_NEAR(r, 0.0, 1e-12);
    // negative control: a sqrt law would give 0.447 at -0.8, far from the measured 0.309
    EXPECT_GT(std::fabs(std::sqrt(1 - 0.8) - std::cos(std::numbers::pi * 0.8 / 2)), 0.1);
}

TEST(AudioDsp, CrossfadeCurvesHaveTheirDefinedMidpointsAndEnds) {
    struct Expect {
        CrossfadeShape shape;
        double midA, midB;   // dB at x = 0.5
    };
    for (const Expect& e : {Expect{CrossfadeShape::ConstantGain, -6.0206, -6.0206}, Expect{CrossfadeShape::EqualPower, -3.0103, -3.0103},
                            Expect{CrossfadeShape::Exponential, -30.29, -0.27}, Expect{CrossfadeShape::Logarithmic, -0.27, -30.29}}) {
        double a = 0, b = 0;
        crossfadeGains(e.shape, 0.0, a, b);
        EXPECT_NEAR(a, 1.0, 1e-12) << crossfadeShapeId(e.shape);
        EXPECT_NEAR(b, 0.0, 1e-12) << crossfadeShapeId(e.shape);
        crossfadeGains(e.shape, 1.0, a, b);
        EXPECT_NEAR(a, 0.0, 1e-12) << crossfadeShapeId(e.shape);
        EXPECT_NEAR(b, 1.0, 1e-12) << crossfadeShapeId(e.shape);
        crossfadeGains(e.shape, 0.5, a, b);
        EXPECT_NEAR(gainToDb(a), e.midA, 0.02) << crossfadeShapeId(e.shape);
        EXPECT_NEAR(gainToDb(b), e.midB, 0.02) << crossfadeShapeId(e.shape);
        // monotonic: the outgoing clip never gets louder, the incoming one never quieter
        double pa = 1, pb = 0;
        for (int i = 1; i <= 100; ++i) {
            crossfadeGains(e.shape, i / 100.0, a, b);
            EXPECT_LE(a, pa + 1e-12);
            EXPECT_GE(b, pb - 1e-12);
            pa = a;
            pb = b;
        }
        CrossfadeShape back;
        ASSERT_TRUE(crossfadeShapeFromId(crossfadeShapeId(e.shape), back));
        EXPECT_EQ(back, e.shape);
    }
    // equal power keeps the summed power of uncorrelated material constant, constant gain the summed amplitude
    for (double x : {0.1, 0.3, 0.77}) {
        double a = 0, b = 0;
        crossfadeGains(CrossfadeShape::EqualPower, x, a, b);
        EXPECT_NEAR(a * a + b * b, 1.0, 1e-12);
        crossfadeGains(CrossfadeShape::ConstantGain, x, a, b);
        EXPECT_NEAR(a + b, 1.0, 1e-12);
    }
    // negative control: the shapes really differ
    double a1 = 0, b1 = 0, a2 = 0, b2 = 0;
    crossfadeGains(CrossfadeShape::ConstantGain, 0.5, a1, b1);
    crossfadeGains(CrossfadeShape::EqualPower, 0.5, a2, b2);
    EXPECT_GT(std::fabs(a1 - a2), 0.2);
    CrossfadeShape none;
    EXPECT_FALSE(crossfadeShapeFromId("linear", none));
}

TEST(AudioDsp, BiquadsHaveTheirDesignResponse) {
    const double fs = 48000;
    const Biquad peak = Biquad::peaking(fs, 1000, 1.41, 6.0);
    EXPECT_NEAR(peak.magnitudeDb(fs, 1000), 6.0, 0.01);
    EXPECT_NEAR(peak.magnitudeDb(fs, 20), 0.0, 0.05);
    EXPECT_NEAR(peak.magnitudeDb(fs, 20000), 0.0, 0.1);
    EXPECT_TRUE(Biquad::peaking(fs, 1000, 1.41, 0.0).identity());   // 0 dB = no filter at all
    const Biquad hp = Biquad::highPass(fs, 100);
    EXPECT_NEAR(hp.magnitudeDb(fs, 100), -3.01, 0.02);
    EXPECT_LT(hp.magnitudeDb(fs, 25), -23.0);   // 12 dB per octave below
    EXPECT_NEAR(hp.magnitudeDb(fs, 2000), 0.0, 0.02);
    const Biquad lp = Biquad::lowPass(fs, 8000);
    EXPECT_NEAR(lp.magnitudeDb(fs, 8000), -3.01, 0.02);
    EXPECT_NEAR(lp.magnitudeDb(fs, 200), 0.0, 0.02);
    const Biquad n = Biquad::notch(fs, 50, 8);
    EXPECT_LT(n.magnitudeDb(fs, 50), -60.0);
    EXPECT_NEAR(n.magnitudeDb(fs, 200), 0.0, 0.1);
    // processing a 1 kHz sine through the peaking filter lifts it by 6 dB (steady state)
    BiquadState st;
    double peakOut = 0;
    for (int i = 0; i < 48000; ++i) {
        const double y = st.process(peak, std::sin(2 * std::numbers::pi * 1000 * i / fs));
        if (i > 24000) peakOut = std::max(peakOut, std::fabs(y));
    }
    EXPECT_NEAR(gainToDb(peakOut), 6.0, 0.05);
}

TEST(AudioDsp, EqPresetsAreWithinRange) {
    ASSERT_FALSE(eqPresets().empty());
    EXPECT_EQ(eqPresets().front().id, "flat");
    for (const auto& p : eqPresets()) {
        EXPECT_EQ(findEqPreset(p.id), &p);
        for (double g : p.gainsDb) EXPECT_LE(std::fabs(g), kEqMaxDb) << p.id;
    }
    EXPECT_EQ(findEqPreset("no-such-preset"), nullptr);
}
