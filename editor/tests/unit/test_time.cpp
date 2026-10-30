#include "fa/core/time.h"

#include <gtest/gtest.h>

using namespace fa;

TEST(Rational, NormalisesSignAndGcd) {
    EXPECT_EQ(Rational(2, 4), Rational(1, 2));
    EXPECT_EQ(Rational(3, -6), Rational(-1, 2));
    EXPECT_EQ(Rational(0, -5).den(), 1);
    EXPECT_THROW(Rational(1, 0), std::invalid_argument);
}

TEST(Rational, ArithmeticIsExact) {
    const Rational a(1001, 30000);
    Rational sum(0);
    for (int i = 0; i < 30000; ++i) sum += a;   // 30000 NTSC frames = exactly 1001 s
    EXPECT_EQ(sum, Rational(1001));
    EXPECT_EQ(Rational(1, 3) + Rational(1, 6), Rational(1, 2));
    EXPECT_EQ(Rational(2, 3) * Rational(9, 4), Rational(3, 2));
    EXPECT_EQ(Rational(1, 2) / Rational(1, 4), Rational(2));
    EXPECT_EQ(Rational(1, 2) - Rational(3, 4), Rational(-1, 4));
}

TEST(Rational, ComparisonAndRounding) {
    EXPECT_LT(Rational(1001, 30000), Rational(1, 29));
    EXPECT_GT(Rational(-1, 3), Rational(-1, 2));
    EXPECT_EQ(Rational(7, 2).floor(), 3);
    EXPECT_EQ(Rational(-7, 2).floor(), -4);
    EXPECT_EQ(Rational(7, 2).ceil(), 4);
    EXPECT_EQ(Rational(-7, 2).ceil(), -3);
    EXPECT_EQ(Rational(5, 2).round(), 3);
    EXPECT_EQ(Rational(-5, 2).round(), -3);
    EXPECT_EQ(Rational(7, 3).round(), 2);
}

TEST(Rational, ParseAndFormatRoundTrip) {
    for (const auto& r : {Rational(1001, 30000), Rational(-5, 7), Rational(42), Rational(0)})
        EXPECT_EQ(Rational::parse(r.toString()), r);
    EXPECT_EQ(Rational::parse("30000/1001"), Rational(30000, 1001));
    EXPECT_THROW(Rational::parse("1/x"), std::invalid_argument);
    EXPECT_THROW(Rational::parse("abc"), std::invalid_argument);
    EXPECT_THROW(Rational::parse("1/0"), std::invalid_argument);
}

TEST(Rational, OverflowThrowsInsteadOfWrapping) {
    const Rational big(INT64_MAX / 2, 1);
    EXPECT_THROW(big * Rational(4), RationalOverflow);
    EXPECT_THROW(big + big + big, RationalOverflow);
}

TEST(Rational, FromDouble) {
    EXPECT_EQ(Rational::fromDouble(29.97002997002997), Rational(30000, 1001));
    EXPECT_EQ(Rational::fromDouble(0.5), Rational(1, 2));
}

TEST(FrameRate, RejectsNonPositive) {
    EXPECT_THROW(FrameRate(0, 1), std::invalid_argument);
    EXPECT_THROW(FrameRate(-30, 1), std::invalid_argument);
}

class FrameConversion : public ::testing::TestWithParam<FrameRate> {};

TEST_P(FrameConversion, FrameTimeRoundTripIsExact) {
    const FrameRate r = GetParam();
    for (int64_t n : {0LL, 1LL, 2LL, 29LL, 30LL, 1000LL, 107892LL /* 1 h at 29.97 */, 2592000LL /* 24 h at 30 */}) {
        const Time t = frameToTime(n, r);
        EXPECT_EQ(timeToFrame(t, r), n);
        EXPECT_EQ(timeToFrame(t, r, Rounding::Nearest), n);
        EXPECT_EQ(timeToFrame(t, r, Rounding::Ceil), n);
        EXPECT_TRUE(isFrameAligned(t, r));
        // a moment just inside the frame still belongs to it; the next frame starts exactly at (n+1)/r
        const Time inside = t + r.frameDuration() * Rational(999, 1000);
        EXPECT_EQ(timeToFrame(inside, r), n);
        EXPECT_FALSE(isFrameAligned(inside, r));
        EXPECT_EQ(timeToFrame(inside, r, Rounding::Ceil), n + 1);
        EXPECT_EQ(snapToFrame(inside, r), frameToTime(n + 1, r));
    }
}

INSTANTIATE_TEST_SUITE_P(CommonRates, FrameConversion,
                         ::testing::Values(FrameRate::fps24(), FrameRate::fps25(), FrameRate::fps30(),
                                           FrameRate::ntsc30(), FrameRate::ntsc24(), FrameRate::ntsc60()));

TEST(FrameConversion, NoDriftOverLongDurations) {
    // Frame 1 798 201 at 30000/1001 starts at exactly 60 006.7 s.. computed without floating point.
    const FrameRate r = FrameRate::ntsc30();
    const Time t = frameToTime(1798201, r);
    EXPECT_EQ(t, Rational(1798201LL * 1001, 30000));
    // Accumulating frame durations gives the same exact value as direct conversion.
    Time acc(0);
    for (int i = 0; i < 100000; ++i) acc += r.frameDuration();
    EXPECT_EQ(acc, frameToTime(100000, r));
    EXPECT_EQ(timeToFrame(acc, r), 100000);
}

TEST(FrameConversion, CrossRateSampling) {
    // 30 fps source shown in a 29.97 sequence: sequence frame 1000 starts at 1000*1001/30000 s,
    // which lies inside source frame floor(1000 * 1001 / 1000) = 1001.
    const Time t = frameToTime(1000, FrameRate::ntsc30());
    EXPECT_EQ(timeToFrame(t, FrameRate::fps30()), 1001);
    // 24 fps sequence frame 1 at 1/24 s shows 25 fps source frame floor(25/24) = 1.
    EXPECT_EQ(timeToFrame(frameToTime(1, FrameRate::fps24()), FrameRate::fps25()), 1);
}

TEST(TimeRange, HalfOpen) {
    const TimeRange r{Rational(2), Rational(3)};
    EXPECT_TRUE(r.contains(Rational(2)));
    EXPECT_TRUE(r.contains(Rational(49, 10)));
    EXPECT_FALSE(r.contains(Rational(5)));
    EXPECT_TRUE(r.overlaps({Rational(4), Rational(2)}));
    EXPECT_FALSE(r.overlaps({Rational(5), Rational(2)}));
}

TEST(Timecode, Format) {
    EXPECT_EQ(formatTimecode(0, FrameRate::fps30()), "00:00:00:00");
    EXPECT_EQ(formatTimecode(30 * 3661 + 7, FrameRate::fps30()), "01:01:01:07");
    EXPECT_EQ(formatTimecode(25 * 60, FrameRate::fps25()), "00:01:00:00");
}
