#include "fa/render/audio_cleanup.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <numbers>

namespace fa {

namespace {

// In-place iterative radix-2 FFT (size a power of two), with tables built once per size.
class Fft {
public:
    explicit Fft(int n) : n_(n), rev_(static_cast<size_t>(n)), tw_(static_cast<size_t>(n / 2)) {
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
            rev_[static_cast<size_t>(i)] = r;
        }
        for (int i = 0; i < n / 2; ++i)
            tw_[static_cast<size_t>(i)] = std::polar(1.0, -2.0 * std::numbers::pi * i / n);
    }
    void run(std::vector<std::complex<double>>& a, bool inverse) const {
        for (int i = 0; i < n_; ++i)
            if (i < rev_[static_cast<size_t>(i)]) std::swap(a[static_cast<size_t>(i)], a[static_cast<size_t>(rev_[static_cast<size_t>(i)])]);
        for (int len = 2; len <= n_; len <<= 1) {
            const int step = n_ / len;
            for (int i = 0; i < n_; i += len)
                for (int j = 0; j < len / 2; ++j) {
                    std::complex<double> w = tw_[static_cast<size_t>(j * step)];
                    if (inverse) w = std::conj(w);
                    const std::complex<double> u = a[static_cast<size_t>(i + j)];
                    const std::complex<double> v = a[static_cast<size_t>(i + j + len / 2)] * w;
                    a[static_cast<size_t>(i + j)] = u + v;
                    a[static_cast<size_t>(i + j + len / 2)] = u - v;
                }
        }
        if (inverse)
            for (auto& x : a) x /= n_;
    }

private:
    int n_;
    std::vector<int> rev_;
    std::vector<std::complex<double>> tw_;
};

const Fft& denoiseFft() {
    static const Fft f(kDenoiseFft);
    return f;
}

int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

double smooth01(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

// Sliding minimum over a window of +-r entries (clamped at the ends), per column of a frames x bins matrix.
void slidingMin(const std::vector<double>& in, int frames, int bins, int r, std::vector<double>& out) {
    out.assign(in.size(), 0.0);
    std::deque<int> q;
    for (int b = 0; b < bins; ++b) {
        q.clear();
        int next = 0;   // next frame to push
        for (int k = 0; k < frames; ++k) {
            const int hi = std::min(frames - 1, k + r), lo = std::max(0, k - r);
            for (; next <= hi; ++next) {
                const double v = in[static_cast<size_t>(next) * bins + b];
                while (!q.empty() && in[static_cast<size_t>(q.back()) * bins + b] >= v) q.pop_back();
                q.push_back(next);
            }
            while (q.front() < lo) q.pop_front();
            out[static_cast<size_t>(k) * bins + b] = in[static_cast<size_t>(q.front()) * bins + b];
        }
    }
}

// Box average over +-rt frames and +-rf bins (clamped at the edges).
void boxAverage(const std::vector<double>& in, int frames, int bins, int rt, int rf, std::vector<double>& out) {
    std::vector<double> tmp(in.size());
    for (int k = 0; k < frames; ++k)
        for (int b = 0; b < bins; ++b) {
            double s = 0;
            int n = 0;
            for (int d = -rf; d <= rf; ++d) {
                const int bb = std::clamp(b + d, 0, bins - 1);
                s += in[static_cast<size_t>(k) * bins + bb];
                ++n;
            }
            tmp[static_cast<size_t>(k) * bins + b] = s / n;
        }
    out.assign(in.size(), 0.0);
    for (int k = 0; k < frames; ++k)
        for (int b = 0; b < bins; ++b) {
            double s = 0;
            int n = 0;
            for (int d = -rt; d <= rt; ++d) {
                const int kk = std::clamp(k + d, 0, frames - 1);
                s += tmp[static_cast<size_t>(kk) * bins + b];
                ++n;
            }
            out[static_cast<size_t>(k) * bins + b] = s / n;
        }
}

}  // namespace

void denoiseChannel(const float* in, int64_t n, int64_t inStart, int64_t anchor, double strength, float* out) {
    if (strength <= 0 || n <= 0) {
        std::copy(in, in + n, out);
        return;
    }
    strength = std::min(1.0, strength);
    constexpr int N = kDenoiseFft, H = kDenoiseHop, bins = N / 2 + 1;
    // frames j on the anchored grid that overlap [inStart, inStart + n)
    const int64_t j0 = floorDiv(inStart - anchor - (N - 1), H) + 1, j1 = floorDiv(inStart + n - 1 - anchor, H);
    const int frames = static_cast<int>(j1 - j0 + 1);
    if (frames <= 0) {
        std::copy(in, in + n, out);
        return;
    }
    std::vector<double> win(N);
    for (int i = 0; i < N; ++i) win[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / N);   // periodic
    std::vector<std::complex<double>> spec(static_cast<size_t>(frames) * bins);
    std::vector<double> power(static_cast<size_t>(frames) * bins);
    std::vector<std::complex<double>> buf(N);
    const Fft& fft = denoiseFft();
    for (int k = 0; k < frames; ++k) {
        const int64_t s0 = anchor + (j0 + k) * H;
        for (int i = 0; i < N; ++i) {
            const int64_t idx = s0 + i - inStart;
            const double x = idx >= 0 && idx < n ? in[idx] : 0.0;
            buf[static_cast<size_t>(i)] = x * win[static_cast<size_t>(i)];
        }
        fft.run(buf, false);
        for (int b = 0; b < bins; ++b) {
            spec[static_cast<size_t>(k) * bins + b] = buf[static_cast<size_t>(b)];
            power[static_cast<size_t>(k) * bins + b] = std::norm(buf[static_cast<size_t>(b)]);
        }
    }
    std::vector<double> p5, p3, noise;
    boxAverage(power, frames, bins, 2, 2, p5);
    boxAverage(power, frames, bins, 1, 1, p3);
    slidingMin(p5, frames, bins, kDenoiseHistoryFrames, noise);
    const double alpha = 1.0 + 1.5 * strength, floorGain = dbToGain(-6.0 - 18.0 * strength);
    std::vector<double> acc(static_cast<size_t>(n), 0.0), g(bins), gs(bins);
    for (int k = 0; k < frames; ++k) {
        for (int b = 0; b < bins; ++b) {
            const size_t i = static_cast<size_t>(k) * bins + b;
            const double nz = kDenoiseBias * noise[i];
            g[static_cast<size_t>(b)] = p3[i] > 0 ? std::max(floorGain, 1.0 - alpha * nz / p3[i]) : floorGain;
        }
        for (int b = 0; b < bins; ++b) {
            const double l = g[static_cast<size_t>(std::max(0, b - 1))], r = g[static_cast<size_t>(std::min(bins - 1, b + 1))];
            gs[static_cast<size_t>(b)] = (l + g[static_cast<size_t>(b)] + r) / 3.0;
        }
        for (int b = 0; b < bins; ++b) buf[static_cast<size_t>(b)] = spec[static_cast<size_t>(k) * bins + b] * gs[static_cast<size_t>(b)];
        for (int b = 1; b < N / 2; ++b) buf[static_cast<size_t>(N - b)] = std::conj(buf[static_cast<size_t>(b)]);
        fft.run(buf, true);
        const int64_t s0 = anchor + (j0 + k) * H;
        for (int i = 0; i < N; ++i) {
            const int64_t idx = s0 + i - inStart;
            if (idx >= 0 && idx < n) acc[static_cast<size_t>(idx)] += buf[static_cast<size_t>(i)].real();
        }
    }
    for (int64_t i = 0; i < n; ++i) out[i] = static_cast<float>(acc[static_cast<size_t>(i)] * 0.5);   // Hann at 75 %: sum 2
}

std::vector<Biquad> cleanupFilters(const CleanupSettings& s, double fs) {
    std::vector<Biquad> out;
    if (s.humHz > 0)
        for (int h = 1; h <= std::clamp(s.humHarmonics, 1, 8); ++h)
            if (s.humHz * h < fs * 0.45) out.push_back(Biquad::notch(fs, s.humHz * h, 10.0));
    // 4th-order Butterworth = two biquads with these Q values
    constexpr double q1 = 0.54119610014619698, q2 = 1.3065629648763766;
    if (s.highPassHz > 0) {
        out.push_back(Biquad::highPass(fs, s.highPassHz, q1));
        out.push_back(Biquad::highPass(fs, s.highPassHz, q2));
    }
    if (s.lowPassHz > 0 && s.lowPassHz < fs * 0.49) {
        out.push_back(Biquad::lowPass(fs, s.lowPassHz, q1));
        out.push_back(Biquad::lowPass(fs, s.lowPassHz, q2));
    }
    return out;
}

// ---- loudness ----------------------------------------------------------------------------------------------------

std::array<Biquad, 2> kWeighting(double fs) {
    // BS.1770-4 analogue prototypes (the 48 kHz coefficients of the standard follow from these exactly).
    std::array<Biquad, 2> k;
    {
        const double f0 = 1681.974450955533, gDb = 3.999843853973347, q = 0.7071752369554196;
        const double K = std::tan(std::numbers::pi * f0 / fs);
        const double vh = std::pow(10.0, gDb / 20.0), vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + K / q + K * K;
        k[0].b0 = (vh + vb * K / q + K * K) / a0;
        k[0].b1 = 2.0 * (K * K - vh) / a0;
        k[0].b2 = (vh - vb * K / q + K * K) / a0;
        k[0].a1 = 2.0 * (K * K - 1.0) / a0;
        k[0].a2 = (1.0 - K / q + K * K) / a0;
    }
    {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double K = std::tan(std::numbers::pi * f0 / fs);
        const double a0 = 1.0 + K / q + K * K;
        k[1].b0 = 1.0;
        k[1].b1 = -2.0;
        k[1].b2 = 1.0;
        k[1].a1 = 2.0 * (K * K - 1.0) / a0;
        k[1].a2 = (1.0 - K / q + K * K) / a0;
    }
    return k;
}

LoudnessMeter::LoudnessMeter(int sampleRate, int channels)
    : rate_(sampleRate), channels_(channels), k_(kWeighting(sampleRate)), state_(static_cast<size_t>(channels)),
      subLen_(sampleRate / 10), subAcc_(static_cast<size_t>(channels), 0.0) {}

void LoudnessMeter::add(const float* x, int64_t frames) {
    for (int64_t i = 0; i < frames; ++i) {
        for (int c = 0; c < channels_; ++c) {
            auto& st = state_[static_cast<size_t>(c)];
            double v = x[i * channels_ + c];
            v = st[0].process(k_[0], v);
            v = st[1].process(k_[1], v);
            subAcc_[static_cast<size_t>(c)] += v * v;
        }
        if (++inSub_ == subLen_) {
            double sum = 0;
            for (double& a : subAcc_) {
                sum += a / static_cast<double>(subLen_);
                a = 0;
            }
            subBlocks_.push_back(sum);
            inSub_ = 0;
        }
    }
}

namespace {
double lufs(double meanSquareSum) { return -0.691 + 10.0 * std::log10(meanSquareSum); }
}

double LoudnessMeter::integrated() const {
    std::vector<double> blocks;   // 400 ms blocks every 100 ms
    for (size_t i = 0; i + 4 <= subBlocks_.size(); ++i)
        blocks.push_back((subBlocks_[i] + subBlocks_[i + 1] + subBlocks_[i + 2] + subBlocks_[i + 3]) / 4.0);
    double sum = 0;
    int n = 0;
    for (double z : blocks)
        if (z > 0 && lufs(z) > -70.0) {
            sum += z;
            ++n;
        }
    if (n == 0) return -HUGE_VAL;
    const double relative = lufs(sum / n) - 10.0;
    sum = 0;
    n = 0;
    for (double z : blocks)
        if (z > 0 && lufs(z) > -70.0 && lufs(z) > relative) {
            sum += z;
            ++n;
        }
    return n ? lufs(sum / n) : -HUGE_VAL;
}

double LoudnessMeter::momentaryMax() const {
    double best = -HUGE_VAL;
    for (size_t i = 0; i + 4 <= subBlocks_.size(); ++i) {
        const double z = (subBlocks_[i] + subBlocks_[i + 1] + subBlocks_[i + 2] + subBlocks_[i + 3]) / 4.0;
        if (z > 0) best = std::max(best, lufs(z));
    }
    return best;
}

// ---- limiter ---------------------------------------------------------------------------------------------------------

void limitPeaks(const float* in, int64_t frames, int channels, int window, double ceiling, int64_t from, int64_t count,
                float* out) {
    const int64_t W = window;
    // need(m) for m in [from - W, from + count + W)
    const int64_t a = from - W, b = from + count + W;
    std::vector<double> need(static_cast<size_t>(b - a), 1.0);
    for (int64_t m = a; m < b; ++m) {
        if (m < 0 || m >= frames) continue;
        double pk = 0;
        for (int c = 0; c < channels; ++c) pk = std::max(pk, static_cast<double>(std::fabs(in[m * channels + c])));
        if (pk > ceiling) need[static_cast<size_t>(m - a)] = ceiling / pk;
    }
    // g1(n) = min over need[n .. n + W] for n in [from - W, from + count)
    const int64_t g1From = from - W, g1To = from + count;
    std::vector<double> g1(static_cast<size_t>(g1To - g1From));
    std::deque<int64_t> q;
    int64_t next = g1From;
    for (int64_t nn = g1From; nn < g1To; ++nn) {
        for (; next <= nn + W; ++next) {
            const double v = need[static_cast<size_t>(next - a)];
            while (!q.empty() && need[static_cast<size_t>(q.back() - a)] >= v) q.pop_back();
            q.push_back(next);
        }
        while (q.front() < nn) q.pop_front();
        g1[static_cast<size_t>(nn - g1From)] = need[static_cast<size_t>(q.front() - a)];
    }
    // g(n) = mean of g1 over [n - W, n], summed in fixed point (exact): the result at a sample depends only on the input
    // within +-2W of it, never on where the computation started. Rounding down keeps g <= the true mean <= need.
    constexpr double kOne = 4294967296.0;   // 2^32
    auto fixed = [&](int64_t k) { return static_cast<int64_t>(std::floor(g1[static_cast<size_t>(k - g1From)] * kOne)); };
    int64_t sum = 0;
    for (int64_t k = from - W; k <= from; ++k) sum += fixed(k);
    const int64_t full = (W + 1) * static_cast<int64_t>(kOne);
    for (int64_t nn = from; nn < from + count; ++nn) {
        if (nn > from) sum += fixed(nn) - fixed(nn - W - 1);
        const bool untouched = sum >= full;   // nothing to limit nearby: the samples pass bit-exactly
        const double g = static_cast<double>(sum) / static_cast<double>(full);
        for (int c = 0; c < channels; ++c)
            out[(nn - from) * channels + c] = untouched ? in[nn * channels + c] : static_cast<float>(in[nn * channels + c] * g);
    }
}

// ---- ducking -------------------------------------------------------------------------------------------------------

double duckDepth(const std::vector<std::pair<int64_t, int64_t>>& runs, int64_t s, int64_t attack, int64_t hold,
                 int64_t release) {
    // the last run starting at or before s, and the first run starting after s
    auto it = std::upper_bound(runs.begin(), runs.end(), s, [](int64_t v, const std::pair<int64_t, int64_t>& r) { return v < r.first; });
    double d = 0;
    if (it != runs.begin()) {
        const auto& r = *(it - 1);
        const int64_t endHold = r.second + hold;
        if (s < endHold) return 1.0;
        if (release > 0 && s < endHold + release) d = smooth01(1.0 - static_cast<double>(s - endHold) / release);
    }
    if (it != runs.end() && attack > 0) {
        const int64_t until = it->first - s;
        if (until <= attack) d = std::max(d, smooth01(1.0 - static_cast<double>(until) / attack));
    }
    return d;
}

}  // namespace fa
