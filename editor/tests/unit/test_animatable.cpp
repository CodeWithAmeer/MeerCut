// Keyframe interpolation, checked against the Stage 1 measurements of Filmora (TEST_CASES.md EXP-002).
// Setup there: Position X keys at source 0 s and 4 s, value +0 px -> +480 px (stored 0.5 -> 0.75),
// 25 fps export, red-square centroid measured per frame.
#include "fa/anim/animatable.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace fa;

namespace {

Keyframe<double> key(Rational t, double v, Interpolation i, TangentMode m) {
    Keyframe<double> k;
    k.time = t;
    k.value = v;
    k.interp = i;
    k.tangent = m;
    return k;
}

// Unit cubic Bézier y(x) with control points P1=(x1,y1), P2=(x2,y2) — the form in which Stage 1 fits
// were reported. Used as an independent reference implementation.
double refBezier(double x, double x1, double y1, double x2, double y2) {
    double lo = 0, hi = 1;
    for (int i = 0; i < 200; ++i) {
        const double u = 0.5 * (lo + hi);
        const double v = 1 - u;
        const double bx = 3 * v * v * u * x1 + 3 * v * u * u * x2 + u * u * u;
        (bx < x ? lo : hi) = u;
    }
    const double u = 0.5 * (lo + hi), v = 1 - u;
    return 3 * v * v * u * y1 + 3 * v * u * u * y2 + u * u * u;
}

constexpr double kPx = 1920.0;   // normalised position -> pixels

}  // namespace

TEST(Animatable, ConstantWithoutKeys) {
    Animatable<double> a(0.25);
    EXPECT_FALSE(a.isAnimated());
    EXPECT_DOUBLE_EQ(a.value(123.0), 0.25);
}

TEST(Animatable, HoldsOutsideKeyRange) {
    Animatable<double> a;
    a.setKey(key(Rational(1), 2.0, Interpolation::Linear, TangentMode::Linear));
    a.setKey(key(Rational(3), 4.0, Interpolation::Linear, TangentMode::Linear));
    EXPECT_DOUBLE_EQ(a.value(0.0), 2.0);
    EXPECT_DOUBLE_EQ(a.value(10.0), 4.0);
}

TEST(Animatable, SetKeyReplacesAndSorts) {
    Animatable<double> a;
    a.setKey(key(Rational(3), 1.0, Interpolation::Linear, TangentMode::Linear));
    a.setKey(key(Rational(1), 0.0, Interpolation::Linear, TangentMode::Linear));
    a.setKey(key(Rational(3), 5.0, Interpolation::Linear, TangentMode::Linear));
    ASSERT_EQ(a.keys().size(), 2u);
    EXPECT_EQ(a.keys()[0].time, Rational(1));
    EXPECT_DOUBLE_EQ(a.keys()[1].value, 5.0);
    EXPECT_TRUE(a.removeKey(Rational(1)));
    EXPECT_FALSE(a.removeKey(Rational(1)));
}

// EXP-002 Linear: 4.8 px per frame at 25 fps (480 px over frames 0..100).
TEST(Animatable, Exp002LinearMovesUniformly) {
    Animatable<double> x;
    x.setKey(key(Rational(0), 0.5, Interpolation::Linear, TangentMode::Linear));
    x.setKey(key(Rational(4), 0.75, Interpolation::Linear, TangentMode::Linear));
    for (int f = 0; f <= 100; ++f) {
        const double px = (x.value(frameToTime(f, FrameRate::fps25())) - 0.5) * kPx;
        EXPECT_NEAR(px, 4.8 * f, 1e-9) << "frame " << f;
    }
}

// EXP-002 Freeze (Interpolation 6): hold 0.5 until 4.0 s, then jump.
TEST(Animatable, Exp002FreezeHoldsThenJumps) {
    Animatable<double> x;
    x.setKey(key(Rational(0), 0.5, Interpolation::Hold, TangentMode::Linear));
    x.setKey(key(Rational(4), 0.75, Interpolation::Hold, TangentMode::Linear));
    EXPECT_DOUBLE_EQ(x.value(3.96), 0.5);
    EXPECT_DOUBLE_EQ(x.value(4.0), 0.75);
}

// EXP-002 "Ease In" applied to key 2 (Filmora Interpolation 13 on both keys, key 1 keeps its linear
// slope 0.0625/s, key 2 flat): measured Bézier P1=(0.50,0.49) P2=(0.50,1.00); model P1=(0.5,0.5).
TEST(Animatable, Exp002EaseIntoKey) {
    Animatable<double> x;
    auto k0 = key(Rational(0), 0.5, Interpolation::Bezier, TangentMode::Manual);
    k0.outSlope = 0.0625;   // stored _rightDerivativeY
    x.setKey(k0);
    x.setKey(key(Rational(4), 0.75, Interpolation::Bezier, TangentMode::Flat));
    double maxErrModel = 0, maxErrMeasured = 0;
    for (int f = 0; f <= 100; ++f) {
        const double u = f / 100.0;
        const double v = (x.value(4.0 * u) - 0.5) / 0.25;
        maxErrModel = std::max(maxErrModel, std::fabs(v - refBezier(u, 0.5, 0.5, 0.5, 1.0)));
        maxErrMeasured = std::max(maxErrMeasured, std::fabs(v - refBezier(u, 0.50, 0.49, 0.50, 1.00)) * 480.0);
    }
    EXPECT_LT(maxErrModel, 1e-9);
    // The published fit rounds P1.y to 0.49; that rounding alone allows up to 3*(4/9)*(1/3)*0.01*480 = 2.1 px.
    EXPECT_LT(maxErrMeasured, 2.2) << "too far from the curve fitted to Filmora's export";
    // starts at linear speed, arrives with zero speed
    const double v0 = (x.value(0.04) - x.value(0.0)) / 0.04;
    const double v1 = (x.value(4.0) - x.value(3.96)) / 0.04;
    EXPECT_NEAR(v0, 0.0625, 0.002);
    EXPECT_NEAR(v1, 0.0, 0.002);
}

// EXP-002 Continuous Curve on 2 keys (Interpolation 14, no slope): symmetric ease-in-out,
// measured P1=(0.50,0.00) P2=(0.50,1.00).
TEST(Animatable, Exp002ContinuousTwoKeysIsSymmetricEase) {
    Animatable<double> x;
    x.setKey(key(Rational(0), 0.5, Interpolation::Bezier, TangentMode::Auto));
    x.setKey(key(Rational(4), 0.75, Interpolation::Bezier, TangentMode::Auto));
    for (int f = 0; f <= 100; ++f) {
        const double u = f / 100.0;
        const double v = (x.value(4.0 * u) - 0.5) / 0.25;
        EXPECT_NEAR(v, refBezier(u, 0.5, 0.0, 0.5, 1.0), 1e-9);
    }
    EXPECT_NEAR(x.value(2.0), 0.625, 1e-12);   // symmetric: half way at half time
}

// "Ease In" applied to key 1 only while key 2 stays Linear: motion unchanged (straight line).
TEST(Animatable, Exp002EaseOnFirstKeyOnlyStaysLinear) {
    Animatable<double> x;
    auto k0 = key(Rational(0), 0.5, Interpolation::Bezier, TangentMode::Manual);
    k0.outSlope = 0.0625;
    x.setKey(k0);
    x.setKey(key(Rational(4), 0.75, Interpolation::Linear, TangentMode::Linear));
    for (int f = 0; f <= 100; ++f)
        EXPECT_NEAR((x.value(f * 0.04) - 0.5) * kPx, 4.8 * f, 1e-6);
}

// EXP-002 three keys + Continuous Curve: X = 0 / +480 / +240 px at 0 / 2 / 4 s. Filmora stored the
// interior slope 0.03125 = Catmull-Rom, flat ends; export matched the model to 0.43 px RMS and
// overshot to a peak of +488 px at 2.2 s.
TEST(Animatable, Exp002ThreeKeyContinuousCurveOvershoots) {
    Animatable<double> x;
    x.setKey(key(Rational(0), 0.5, Interpolation::Bezier, TangentMode::Auto));
    x.setKey(key(Rational(2), 0.75, Interpolation::Bezier, TangentMode::Auto));
    x.setKey(key(Rational(4), 0.625, Interpolation::Bezier, TangentMode::Auto));
    EXPECT_NEAR(x.inSlopeAt(1), 0.03125, 1e-12);
    EXPECT_NEAR(x.outSlopeAt(1), 0.03125, 1e-12);
    EXPECT_DOUBLE_EQ(x.outSlopeAt(0), 0.0);
    EXPECT_DOUBLE_EQ(x.inSlopeAt(2), 0.0);

    double peak = -1e9, peakT = 0;
    for (int f = 0; f <= 100; ++f) {   // 25 fps frames
        const double t = f / 25.0;
        const double px = (x.value(t) - 0.5) * kPx;
        if (px > peak) {
            peak = px;
            peakT = t;
        }
    }
    EXPECT_NEAR(peak, 488.0, 1.5);
    EXPECT_NEAR(peakT, 2.2, 0.08);
    EXPECT_NEAR((x.value(2.0) - 0.5) * kPx, 480.0, 1e-9);   // passes through the key exactly
}

TEST(Animatable, LinearSegmentsStayStraightWithBezierNeighbours) {
    // Tangent "Linear" uses each adjacent segment's slope, so a Bezier segment between linear
    // tangents is a straight line.
    Animatable<double> x;
    x.setKey(key(Rational(0), 0.0, Interpolation::Bezier, TangentMode::Linear));
    x.setKey(key(Rational(1), 10.0, Interpolation::Bezier, TangentMode::Linear));
    for (int i = 0; i <= 10; ++i) EXPECT_NEAR(x.value(i / 10.0), i * 1.0, 1e-9);
}

TEST(Animatable, HandleLengthsChangeShape) {
    Animatable<double> a, b;
    auto k0 = key(Rational(0), 0.0, Interpolation::Bezier, TangentMode::Flat);
    auto k1 = key(Rational(1), 1.0, Interpolation::Bezier, TangentMode::Flat);
    a.setKey(k0);
    a.setKey(k1);
    k0.outHandle = 0.1;
    k1.inHandle = 0.1;
    b.setKey(k0);
    b.setKey(k1);
    // short handles ease less: closer to linear at u = 0.25
    EXPECT_LT(a.value(0.25), b.value(0.25));
    EXPECT_NEAR(a.value(0.5), 0.5, 1e-12);
    EXPECT_NEAR(b.value(0.5), 0.5, 1e-12);
}

TEST(Animatable, Vec2Componentwise) {
    Animatable<Vec2> p;
    Keyframe<Vec2> a, b;
    a.time = Rational(0);
    a.value = {0, 10};
    b.time = Rational(2);
    b.value = {4, 20};
    p.setKey(a);
    p.setKey(b);
    const Vec2 m = p.value(1.0);
    EXPECT_DOUBLE_EQ(m.x, 2.0);
    EXPECT_DOUBLE_EQ(m.y, 15.0);
}

TEST(Animatable, MonotonicTimeInversionNearEndpoints) {
    Animatable<double> x;
    x.setKey(key(Rational(0), 0.0, Interpolation::Bezier, TangentMode::Flat));
    x.setKey(key(Rational(1), 1.0, Interpolation::Bezier, TangentMode::Flat));
    double prev = -1;
    for (int i = 0; i <= 1000; ++i) {
        const double v = x.value(i / 1000.0);
        EXPECT_GE(v, prev - 1e-12);
        prev = v;
    }
}
