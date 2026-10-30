// M6 cleanup / loudness / dynamics DSP (fa/render/audio_cleanup.h) on synthetic signals with known answers.
#include "fa/render/audio_cleanup.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace fa;

namespace {

constexpr int kRate = 48000;

std::vector<float> sine(int64_t n, double f, double amp, double phase = 0) {
    std::vector<float> x(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) x[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2 * std::numbers::pi * f * i / kRate + phase));
    return x;
}

double amplitudeAt(const std::vector<float>& x, int64_t from, int n, double f) {
    double c = 0, s = 0;
    for (int i = 0; i < n; ++i) {
        const double v = x[static_cast<size_t>(from + i)];
        c += v * std::cos(2 * std::numbers::pi * f * (from + i) / kRate);
        s += v * std::sin(2 * std::numbers::pi * f * (from + i) / kRate);
    }
    return 2 * std::hypot(c, s) / n;
}

double rms(const std::vector<float>& x, int64_t from, int64_t to) {
    double s = 0;
    for (int64_t i = from; i < to; ++i) s += double(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
    return std::sqrt(s / double(to - from));
}

std::vector<float> filtered(const std::vector<Biquad>& fs, const std::vector<float>& x) {
    std::vector<BiquadState> st(fs.size());
    std::vector<float> y(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        double v = x[i];
        for (size_t k = 0; k < fs.size(); ++k) v = st[k].process(fs[k], v);
        y[i] = static_cast<float>(v);
    }
    return y;
}

double responseDb(const std::vector<Biquad>& fs, double f) {
    double db = 0;
    for (const auto& b : fs) db += b.magnitudeDb(kRate, f);
    return db;
}

}  // namespace

// ---- loudness ------------------------------------------------------------------------------------------------------

TEST(Loudness, KWeightingMatchesTheStandardsCoefficientsAt48k) {
    const auto k = kWeighting(48000);
    // ITU-R BS.1770-4, Tables 1 and 2
    EXPECT_NEAR(k[0].b0, 1.53512485958697, 1e-9);
    EXPECT_NEAR(k[0].b1, -2.69169618940638, 1e-9);
    EXPECT_NEAR(k[0].b2, 1.19839281085285, 1e-9);
    EXPECT_NEAR(k[0].a1, -1.69065929318241, 1e-9);
    EXPECT_NEAR(k[0].a2, 0.73248077421585, 1e-9);
    EXPECT_NEAR(k[1].a1, -1.99004745483398, 1e-9);
    EXPECT_NEAR(k[1].a2, 0.99007225036621, 1e-9);
}

TEST(Loudness, SineReferenceLevels) {
    // EBU Tech 3341 test 1: a stereo 1 kHz sine at -23 dBFS reads -23.0 LUFS (+-0.1)
    for (double dbfs : {-23.0, -18.06, -40.0}) {
        const auto x = sine(kRate * 20, 1000, std::pow(10.0, dbfs / 20));
        std::vector<float> st(x.size() * 2);
        for (size_t i = 0; i < x.size(); ++i) st[2 * i] = st[2 * i + 1] = x[i];
        LoudnessMeter m(kRate, 2);
        m.add(st.data(), static_cast<int64_t>(x.size()));
        EXPECT_NEAR(m.integrated(), dbfs, 0.1) << dbfs;
    }
    // the relative gate: 20 s at -23 plus 20 s at -36 dBFS (more than 10 LU lower) reads -23 (+-0.1), the quiet part
    // being gated out; without the gate it would read about -25.8
    const auto loud = sine(kRate * 20, 1000, std::pow(10.0, -23.0 / 20)), quiet = sine(kRate * 20, 1000, std::pow(10.0, -36.0 / 20));
    std::vector<float> st;
    for (const auto* part : {&loud, &quiet})
        for (float v : *part) {
            st.push_back(v);
            st.push_back(v);
        }
    LoudnessMeter m(kRate, 2);
    m.add(st.data(), static_cast<int64_t>(st.size() / 2));
    EXPECT_NEAR(m.integrated(), -23.0, 0.1);
    LoudnessMeter silent(kRate, 2);
    std::vector<float> zeros(kRate * 4, 0.0f);
    silent.add(zeros.data(), kRate * 2);
    EXPECT_TRUE(std::isinf(silent.integrated()));
}

// ---- filters ---------------------------------------------------------------------------------------------------------

TEST(Cleanup, HumNotchesRemoveMainsAndKeepTheRest) {
    CleanupSettings s;
    s.humHz = 50;
    s.humHarmonics = 4;
    const auto f = cleanupFilters(s, kRate);
    ASSERT_EQ(f.size(), 4u);
    for (int h = 1; h <= 4; ++h) EXPECT_LT(responseDb(f, 50.0 * h), -40.0) << h;
    EXPECT_NEAR(responseDb(f, 1000), 0.0, 0.05);
    EXPECT_NEAR(responseDb(f, 75), 0.0, 0.5);    // between the notches
    // on a signal: 50 Hz hum + 1 kHz tone
    auto x = sine(kRate * 3, 50, 0.1);
    const auto tone = sine(kRate * 3, 1000, 0.1);
    for (size_t i = 0; i < x.size(); ++i) x[i] += tone[i];
    const auto y = filtered(f, x);
    EXPECT_LT(amplitudeAt(y, kRate * 2, kRate / 2, 50), 0.1 * 0.01);   // > 40 dB down once settled
    EXPECT_NEAR(amplitudeAt(y, kRate * 2, kRate / 2, 1000), 0.1, 0.001);
    s.humHz = 60;
    EXPECT_LT(responseDb(cleanupFilters(s, kRate), 180), -40.0);
}

TEST(Cleanup, ButterworthHighAndLowPass) {
    CleanupSettings s;
    s.highPassHz = 100;
    auto f = cleanupFilters(s, kRate);
    EXPECT_NEAR(responseDb(f, 100), -3.01, 0.05);
    EXPECT_NEAR(responseDb(f, 50) - responseDb(f, 25), 24.0, 0.6);   // 24 dB per octave below the cutoff
    EXPECT_NEAR(responseDb(f, 2000), 0.0, 0.01);
    s = {};
    s.lowPassHz = 8000;
    f = cleanupFilters(s, kRate);
    EXPECT_NEAR(responseDb(f, 8000), -3.01, 0.05);
    EXPECT_NEAR(responseDb(f, 500), 0.0, 0.01);
    EXPECT_LT(responseDb(f, 16000), -20.0);
    EXPECT_TRUE(cleanupFilters(CleanupSettings{}, kRate).empty());
}

// ---- noise reduction -------------------------------------------------------------------------------------------------

// Speech-like test signal: a 1 kHz tone in "syllables" (300 ms on, 200 ms off) from 2 s to 4 s over steady noise.
TEST(Cleanup, DenoiseLowersSteadyNoiseAndKeepsIntermittentSound) {
    const int64_t n = kRate * 6;
    std::mt19937 rng(42);
    std::normal_distribution<double> g(0.0, 0.01);   // -40 dBFS rms white noise
    std::vector<float> x(static_cast<size_t>(n));
    const auto tone = sine(n, 1000, 0.125);
    auto on = [](int64_t i) {
        const double t = double(i) / kRate;
        return t >= 2.0 && t < 4.0 && std::fmod(t - 2.0, 0.5) < 0.3;
    };
    for (int64_t i = 0; i < n; ++i)
        x[static_cast<size_t>(i)] = static_cast<float>(g(rng)) + (on(i) ? tone[static_cast<size_t>(i)] : 0.0f);
    std::vector<float> y(x.size());
    denoiseChannel(x.data(), n, 0, 0, 0.8, y.data());
    // noise-only regions (away from the edges): at least 12 dB quieter
    const double before = rms(x, kRate, int64_t(1.8 * kRate)), after = rms(y, kRate, int64_t(1.8 * kRate));
    EXPECT_LT(20 * std::log10(after / before), -12.0) << before << " -> " << after;
    // the noise in the pauses between the syllables is reduced too
    EXPECT_LT(20 * std::log10(rms(y, int64_t(2.85 * kRate), int64_t(2.95 * kRate)) / rms(x, int64_t(2.85 * kRate), int64_t(2.95 * kRate))), -6.0);
    // the syllables keep their level within 0.5 dB (measured inside one, away from its edges)
    const double t0 = amplitudeAt(x, int64_t(2.55 * kRate), 9600, 1000), t1 = amplitudeAt(y, int64_t(2.55 * kRate), 9600, 1000);
    EXPECT_NEAR(20 * std::log10(t1 / t0), 0.0, 0.5);
    // strength 0 is a bit-exact bypass, and unity gains reconstruct exactly (negative control for the OLA)
    std::vector<float> z(x.size());
    denoiseChannel(x.data(), n, 0, 0, 0.0, z.data());
    EXPECT_EQ(z, x);
}

// Documented limitation: minimum statistics cannot tell a perfectly steady tone lasting longer than its 1.5 s window
// from steady noise (a whine), so it is reduced too. The noise reduction is meant for dialogue, not sustained notes.
TEST(Cleanup, DenoiseTreatsASteadyToneLongerThanTheWindowAsNoise) {
    const int64_t n = kRate * 6;
    const auto x = sine(n, 1000, 0.125);
    std::vector<float> y(x.size());
    denoiseChannel(x.data(), n, 0, 0, 0.8, y.data());
    EXPECT_LT(20 * std::log10(amplitudeAt(y, 3 * kRate, kRate, 1000) / 0.125), -6.0);
}

TEST(Cleanup, DenoiseDoesNotDependOnTheBlockCut) {
    const int64_t n = kRate * 5;
    std::mt19937 rng(7);
    std::normal_distribution<double> g(0.0, 0.02);
    std::vector<float> x(static_cast<size_t>(n));
    const auto tone = sine(n, 440, 0.2);
    for (int64_t i = 0; i < n; ++i) x[static_cast<size_t>(i)] = static_cast<float>(g(rng)) + tone[static_cast<size_t>(i)];
    const int64_t anchor = 1234;   // the clip's first sample
    std::vector<float> whole(x.size());
    denoiseChannel(x.data(), n, 0, anchor, 0.6, whole.data());
    // a window [a, a + len) computed from just its margins gives the same final samples
    for (int64_t a : {int64_t(100000), int64_t(150001)}) {
        const int64_t len = 20000, s0 = a - kDenoiseMargin, s1 = a + len + kDenoiseMargin;
        std::vector<float> part(static_cast<size_t>(s1 - s0));
        denoiseChannel(x.data() + s0, s1 - s0, s0, anchor, 0.6, part.data());
        double worst = 0;
        for (int64_t i = a; i < a + len; ++i)
            worst = std::max(worst, double(std::fabs(part[static_cast<size_t>(i - s0)] - whole[static_cast<size_t>(i)])));
        EXPECT_LT(worst, 1e-6) << a;
    }
}

// ---- limiter -------------------------------------------------------------------------------------------------------

TEST(Limiter, KeepsPeaksUnderTheCeilingAndLeavesQuietAudioAlone) {
    const int W = kLimiterWindow;
    const int64_t n = kRate * 2;
    std::vector<float> x(static_cast<size_t>(n) * 2);
    for (int64_t i = 0; i < n; ++i) {
        const double a = i > kRate / 2 && i < kRate ? 1.6 : 0.3;   // a loud burst in the middle
        x[static_cast<size_t>(i) * 2] = static_cast<float>(a * std::sin(2 * std::numbers::pi * 200 * i / kRate));
        x[static_cast<size_t>(i) * 2 + 1] = static_cast<float>(-a * std::sin(2 * std::numbers::pi * 300 * i / kRate));
    }
    const double ceiling = std::pow(10.0, -1.0 / 20);
    const int64_t from = W, count = n - 2 * W;
    std::vector<float> y(static_cast<size_t>(count) * 2);
    limitPeaks(x.data(), n, 2, W, ceiling, from, count, y.data());
    float peak = 0;
    for (float v : y) peak = std::max(peak, std::fabs(v));
    EXPECT_LE(peak, ceiling);
    EXPECT_GT(peak, ceiling * 0.98);
    // well away from the burst the signal passes bit-exactly
    for (int64_t i = from; i < kRate / 2 - 2 * W; ++i) ASSERT_EQ(y[static_cast<size_t>(i - from) * 2], x[static_cast<size_t>(i) * 2]) << i;
    // locality: the same frames limited from a different window are bit-identical (exact fixed-point smoothing)
    std::vector<float> y2(static_cast<size_t>(10000) * 2);
    limitPeaks(x.data(), n, 2, W, ceiling, kRate / 2 + 777, 10000, y2.data());
    for (int64_t i = 0; i < 10000 * 2; ++i)
        ASSERT_EQ(y2[static_cast<size_t>(i)], y[static_cast<size_t>((kRate / 2 + 777 - from) * 2 + i)]) << i;
}

// ---- ducking envelope ----------------------------------------------------------------------------------------------

TEST(Ducking, EnvelopeAnticipatesHoldsAndReleases) {
    const std::vector<std::pair<int64_t, int64_t>> runs{{10000, 20000}, {50000, 51000}};
    const int64_t A = 4800, H = 9600, R = 14400;
    EXPECT_EQ(duckDepth(runs, 0, A, H, R), 0.0);
    EXPECT_EQ(duckDepth(runs, 10000 - A, A, H, R), 0.0);
    EXPECT_NEAR(duckDepth(runs, 10000 - A / 2, A, H, R), 0.5, 1e-12);   // smoothstep midpoint
    EXPECT_EQ(duckDepth(runs, 10000, A, H, R), 1.0);                     // fully down when the sound starts
    EXPECT_EQ(duckDepth(runs, 20000 + H - 1, A, H, R), 1.0);             // held
    EXPECT_NEAR(duckDepth(runs, 20000 + H + R / 2, A, H, R), 0.5, 1e-12);
    EXPECT_EQ(duckDepth(runs, 20000 + H + R, A, H, R), 0.0);
    // between two runs closer than release + attack: the deeper of the release and the next attack
    const double mid = duckDepth(runs, 50000 - A / 4, A, H, R);
    EXPECT_GT(mid, 0.8);
    // continuity: no sample-to-sample jump bigger than the steepest ramp allows
    double jump = 0;
    for (int64_t s = 0; s < 80000; ++s) jump = std::max(jump, std::fabs(duckDepth(runs, s + 1, A, H, R) - duckDepth(runs, s, A, H, R)));
    EXPECT_LT(jump, 1.5 / A + 1e-9);
    EXPECT_EQ(duckDepth({}, 1000, A, H, R), 0.0);
}
