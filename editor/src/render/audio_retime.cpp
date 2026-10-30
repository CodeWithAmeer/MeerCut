#include "fa/render/audio_retime.h"

#include "fa/model/time_map.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>

namespace fa {

namespace {

// Windowed sinc (Blackman), 16 zero crossings each side, tabulated with 512 steps per crossing.
constexpr int kZeroCrossings = 16;
constexpr int kTableSteps = 512;

const std::vector<float>& sincTable() {
    static const std::vector<float> table = [] {
        std::vector<float> t(kZeroCrossings * kTableSteps + 2);
        constexpr double pi = 3.14159265358979323846;
        for (size_t i = 0; i < t.size(); ++i) {
            const double x = static_cast<double>(i) / kTableSteps;
            const double sinc = x == 0 ? 1.0 : std::sin(pi * x) / (pi * x);
            const double w = x >= kZeroCrossings ? 0.0
                                                 : 0.42 + 0.5 * std::cos(pi * x / kZeroCrossings) +
                                                       0.08 * std::cos(2 * pi * x / kZeroCrossings);
            t[i] = static_cast<float>(sinc * w);
        }
        return t;
    }();
    return table;
}

inline float kernel(double x) {   // |x| in zero crossings
    x = std::fabs(x) * kTableSteps;
    const size_t i = static_cast<size_t>(x);
    if (i + 1 >= sincTable().size()) return 0.0f;
    const float f = static_cast<float>(x - static_cast<double>(i));
    return sincTable()[i] + (sincTable()[i + 1] - sincTable()[i]) * f;
}

}  // namespace

namespace {

constexpr int kFrame = 1536;          // 32 ms at 48 kHz
constexpr int kHop = kFrame / 2;      // output hop; periodic Hann windows at 50 % overlap sum to 1
constexpr int kSearch = 480;          // +-10 ms

const std::vector<float>& hann() {
    static const std::vector<float> w = [] {
        std::vector<float> v(kFrame);
        for (int i = 0; i < kFrame; ++i) v[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / kFrame));
        return v;
    }();
    return w;
}

}  // namespace

struct AudioRetimer::Stretch {
    std::string clipId;
    ClipSpeed speed;
    Time sourceIn, start, duration;
    // chosen source centre (in source samples) of recent synthesis frames: frame m is centred on output sample m * kHop
    std::map<int64_t, double> centre;
};

AudioRetimer::AudioRetimer(int sampleRate, int channels) : rate_(sampleRate), channels_(channels) {}
AudioRetimer::~AudioRetimer() = default;

void AudioRetimer::render(const Clip& clip, AudioDecoder& src, int64_t from, int count, float* out) {
    std::fill(out, out + static_cast<size_t>(count) * channels_, 0.0f);
    if (count <= 0 || clip.speed.freeze) return;   // freeze frames are silent
    const double v = std::fabs(SourceMapper(clip).speed((static_cast<double>(from) / rate_) - clip.timelineStart.toDouble()));
    const bool ramp = clip.speed.ramp.size() >= 2;
    if (clip.speed.maintainPitch && (ramp || std::fabs(v - 1.0) > 1e-9)) stretched(clip, src, from, count, out);
    else resampled(clip, src, from, count, out);
}

// Pitch follows the speed: every output sample reads the source at the mapped position (band-limited interpolation).
void AudioRetimer::resampled(const Clip& clip, AudioDecoder& src, int64_t from, int count, float* out) {
    const SourceMapper m(clip);
    const double start = clip.timelineStart.toDouble();
    std::vector<double> pos(static_cast<size_t>(count)), scale(static_cast<size_t>(count));
    double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest(), maxScale = 1;
    for (int i = 0; i < count; ++i) {
        const double t = static_cast<double>(from + i) / rate_ - start;
        pos[i] = m.source(t) * rate_;
        scale[i] = std::max(1.0, std::fabs(m.speed(t)));   // low-pass below the new Nyquist when sped up
        lo = std::min(lo, pos[i]);
        hi = std::max(hi, pos[i]);
        maxScale = std::max(maxScale, scale[i]);
    }
    const int64_t margin = static_cast<int64_t>(std::ceil(kZeroCrossings * maxScale)) + 2;
    const int64_t first = static_cast<int64_t>(std::floor(lo)) - margin;
    const int64_t last = static_cast<int64_t>(std::ceil(hi)) + margin;
    const int n = static_cast<int>(last - first + 1);
    buf_.resize(static_cast<size_t>(n) * channels_);
    src.read(first, n, buf_.data());
    for (int i = 0; i < count; ++i) {
        const double p = pos[i], s = scale[i];
        const int64_t k0 = static_cast<int64_t>(std::floor(p - kZeroCrossings * s)) + 1;
        const int64_t k1 = static_cast<int64_t>(std::floor(p + kZeroCrossings * s));
        double acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        double wsum = 0;
        for (int64_t k = k0; k <= k1; ++k) {
            const double w = kernel((p - static_cast<double>(k)) / s);
            if (w == 0.0) continue;
            wsum += w;
            const float* x = buf_.data() + (k - first) * channels_;
            for (int ch = 0; ch < channels_ && ch < 8; ++ch) acc[ch] += x[ch] * w;
        }
        if (wsum == 0.0) continue;
        for (int ch = 0; ch < channels_ && ch < 8; ++ch) out[i * channels_ + ch] = static_cast<float>(acc[ch] / wsum);
    }
}

// Pitch kept: WSOLA. Synthesis frame m covers output samples [m*kHop - kFrame/2, m*kHop + kFrame/2) and reads the
// source around c = map(m*kHop) + delta (playing direction), delta in [-kSearch, kSearch] chosen so that the frame
// continues the previous one (whose natural continuation is its centre + dir*kHop). Each frame's choice depends on
// the previous frame only, so sequential rendering (playback, export) is deterministic; a jump restarts the chain.
void AudioRetimer::stretched(const Clip& clip, AudioDecoder& src, int64_t from, int count, float* out) {
    const SourceMapper m(clip);
    const double start = clip.timelineStart.toDouble();
    auto nominal = [&](int64_t outSample) { return m.source(static_cast<double>(outSample) / rate_ - start) * rate_; };
    auto direction = [&](int64_t outSample) { return m.speed(static_cast<double>(outSample) / rate_ - start) < 0 ? -1 : 1; };
    if (!st_) st_ = std::make_unique<Stretch>();
    Stretch& S = *st_;
    if (!(S.clipId == clip.id && S.speed == clip.speed && S.sourceIn == clip.sourceIn && S.start == clip.timelineStart &&
          S.duration == clip.duration)) {
        S = Stretch{};
        S.clipId = clip.id;
        S.speed = clip.speed;
        S.sourceIn = clip.sourceIn;
        S.start = clip.timelineStart;
        S.duration = clip.duration;
    }
    const int64_t m0 = static_cast<int64_t>(std::floor(static_cast<double>(from - kFrame / 2) / kHop)) + 1;
    const int64_t m1 = static_cast<int64_t>(std::floor(static_cast<double>(from + count - 1 + kFrame / 2) / kHop));
    // the chain continues if the frame before m0 is known; otherwise it restarts at m0 (seek)
    if (!S.centre.count(m0 - 1) && !S.centre.count(m0)) S.centre.clear();
    // source range needed: every nominal centre +- (frame/2 + search)
    double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
    for (int64_t mm = m0; mm <= m1; ++mm) {
        const double c = nominal(mm * kHop);
        lo = std::min(lo, c);
        hi = std::max(hi, c);
        if (auto it = S.centre.find(mm); it != S.centre.end()) {
            lo = std::min(lo, it->second);
            hi = std::max(hi, it->second);
        }
    }
    const int64_t margin = kFrame / 2 + 2 * kSearch + kHop + 4;
    const int64_t first = static_cast<int64_t>(std::floor(lo)) - margin;
    const int n = static_cast<int>(static_cast<int64_t>(std::ceil(hi)) + margin - first + 1);
    src_.resize(static_cast<size_t>(n) * channels_);
    src.read(first, n, src_.data());
    mono_.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        float a = 0;
        for (int ch = 0; ch < channels_; ++ch) a += src_[static_cast<size_t>(i) * channels_ + ch];
        mono_[i] = a / channels_;
    }
    auto at = [&](double pos) -> int64_t { return static_cast<int64_t>(std::llround(pos)) - first; };   // index into src_
    auto mono = [&](int64_t i) { return i >= 0 && i < n ? mono_[static_cast<size_t>(i)] : 0.0f; };
    const std::vector<float>& w = hann();
    for (int64_t mm = m0; mm <= m1; ++mm) {
        const int dir = direction(mm * kHop);
        double c;
        if (auto it = S.centre.find(mm); it != S.centre.end()) {
            c = it->second;   // computed by the previous call (the frame straddles both)
        } else {
            const double nom = nominal(mm * kHop);
            c = nom;
            if (auto prev = S.centre.find(mm - 1); prev != S.centre.end()) {
                // compare the first half of a candidate frame with the natural continuation of the previous frame
                const double nat = prev->second + dir * kHop;
                const int64_t natI = at(nat);
                double natE = 0;
                for (int k = -kFrame / 2; k < 0; k += 2) natE += static_cast<double>(mono(natI + dir * k)) * mono(natI + dir * k);
                if (natE > 1e-9) {
                    auto score = [&](int d) {
                        const int64_t ci = at(nom) + d;
                        double xy = 0, yy = 0;
                        for (int k = -kFrame / 2; k < 0; k += 2) {
                            const double a = mono(natI + dir * k), b = mono(ci + dir * k);
                            xy += a * b;
                            yy += b * b;
                        }
                        return yy > 1e-12 ? xy / std::sqrt(yy * natE) : -1.0;
                    };
                    int best = 0;
                    double bestScore = score(0);
                    for (int d = -kSearch; d <= kSearch; d += 8) {   // coarse
                        const double sc = score(d);
                        if (sc > bestScore) {
                            bestScore = sc;
                            best = d;
                        }
                    }
                    const int coarse = best;
                    for (int d = coarse - 7; d <= coarse + 7; ++d) {   // fine
                        if (d < -kSearch || d > kSearch || d == coarse) continue;
                        const double sc = score(d);
                        if (sc > bestScore) {
                            bestScore = sc;
                            best = d;
                        }
                    }
                    c = std::round(nom) + best;
                }
            }
            S.centre[mm] = c;
        }
        // overlap-add the frame
        const int64_t ci = at(c);
        const int64_t o0 = mm * kHop - kFrame / 2;
        for (int k = 0; k < kFrame; ++k) {
            const int64_t o = o0 + k;
            if (o < from || o >= from + count) continue;
            const int64_t si = ci + dir * (k - kFrame / 2);
            if (si < 0 || si >= n) continue;
            float* dst = out + (o - from) * channels_;
            const float* x = src_.data() + si * channels_;
            for (int ch = 0; ch < channels_; ++ch) dst[ch] += x[ch] * w[k];
        }
    }
    while (!S.centre.empty() && S.centre.begin()->first < m1 - 4) S.centre.erase(S.centre.begin());   // keep it small
}

}  // namespace fa
