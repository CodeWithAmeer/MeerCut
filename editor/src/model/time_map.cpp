#include "fa/model/time_map.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace fa {

namespace {

// Source / output seconds in double -> Time (microsecond grid: deterministic, no accumulation).
Time fromSeconds(double s) { return Time(static_cast<int64_t>(std::llround(s * 1e6)), 1000000); }

// Speed ramp: output time u (seconds from the first key) <-> source time s (absolute seconds).
struct Ramp {
    struct Seg {
        double u0, T, p0, v0, v1;
        double p1() const { return p0 + T * (v0 + v1) / 2.0; }
    };
    std::vector<Seg> segs;
    double U = 0, pFirst = 0, pLast = 0, vFirst = 1, vLast = 1;

    explicit Ramp(const std::vector<SpeedKey>& keys) {
        if (keys.size() < 2) throw std::invalid_argument("a speed ramp needs at least two keys");
        pFirst = keys.front().source.toDouble();
        vFirst = keys.front().speed;
        double u = 0;
        for (size_t i = 0; i + 1 < keys.size(); ++i) {
            const double p0 = keys[i].source.toDouble(), p1 = keys[i + 1].source.toDouble();
            const double v0 = keys[i].speed, v1 = keys[i + 1].speed;
            const double T = 2.0 * (p1 - p0) / (v0 + v1);
            segs.push_back({u, T, p0, v0, v1});
            u += T;
        }
        U = u;
        pLast = keys.back().source.toDouble();
        vLast = keys.back().speed;
    }
    // Source advance inside a segment after a fraction a of its output duration: integral of the smoothstep speed.
    static double advance(const Seg& g, double a) { return g.v0 * g.T * a + (g.v1 - g.v0) * g.T * (a * a * a - a * a * a * a / 2.0); }
    static double smooth(double a) { return a * a * (3.0 - 2.0 * a); }

    double source(double u) const {   // G(u)
        if (u <= 0) return pFirst + vFirst * u;
        if (u >= U) return pLast + vLast * (u - U);
        for (const Seg& g : segs)
            if (u <= g.u0 + g.T) return g.p0 + advance(g, g.T > 0 ? (u - g.u0) / g.T : 0.0);
        return pLast;
    }
    double speed(double u) const {
        if (u <= 0) return vFirst;
        if (u >= U) return vLast;
        for (const Seg& g : segs)
            if (u <= g.u0 + g.T) return g.v0 + (g.v1 - g.v0) * smooth(g.T > 0 ? (u - g.u0) / g.T : 0.0);
        return vLast;
    }
    double output(double s) const {   // G^-1(s): monotonic, so Newton with a bisection guard
        if (s <= pFirst) return (s - pFirst) / vFirst;
        if (s >= pLast) return U + (s - pLast) / vLast;
        for (const Seg& g : segs) {
            if (s > g.p1()) continue;
            const double target = s - g.p0;
            double lo = 0, hi = 1, a = std::clamp(target / std::max(1e-12, g.p1() - g.p0), 0.0, 1.0);
            for (int it = 0; it < 60; ++it) {
                const double f = advance(g, a) - target;
                if (std::fabs(f) < 1e-13) break;
                (f > 0 ? hi : lo) = a;
                const double d = g.T * (g.v0 + (g.v1 - g.v0) * smooth(a));
                double next = a - f / d;
                if (!(next > lo && next < hi)) next = (lo + hi) / 2;
                a = next;
            }
            return g.u0 + a * g.T;
        }
        return U;
    }
};

bool hasRamp(const ClipSpeed& sp) { return sp.ramp.size() >= 2; }

}  // namespace

Time Clip::sourceOut() const { return sourceIn + sourceLength(*this); }
Time Clip::sourceTimeAt(const Time& t) const { return sourceTimeAtLocal(*this, t - timelineStart); }

struct SourceMapper::Impl {
    double in = 0, dur = 0, v = 1;
    bool freeze = false, reverse = false;
    std::optional<Ramp> ramp;
    double uIn = 0;
};
SourceMapper::SourceMapper(const Clip& c) : impl_(std::make_unique<Impl>()) {
    Impl& m = *impl_;
    m.in = c.sourceIn.toDouble();
    m.dur = c.duration.toDouble();
    m.v = c.speed.speed.toDouble();
    m.freeze = c.speed.freeze;
    m.reverse = c.speed.reverse;
    if (!m.freeze && hasRamp(c.speed)) {
        m.ramp.emplace(c.speed.ramp);
        m.uIn = m.ramp->output(m.in);
    }
}
SourceMapper::~SourceMapper() = default;
SourceMapper::SourceMapper(SourceMapper&&) noexcept = default;
double SourceMapper::source(double t) const {
    const Impl& m = *impl_;
    if (m.freeze) return m.in;
    if (m.ramp) return m.ramp->source(m.uIn + t);
    return m.reverse ? m.in + (m.dur - t) * m.v : m.in + t * m.v;
}
double SourceMapper::speed(double t) const {
    const Impl& m = *impl_;
    if (m.freeze) return 0.0;
    if (m.ramp) return m.ramp->speed(m.uIn + t);
    return m.reverse ? -m.v : m.v;
}

bool isRetimed(const ClipSpeed& sp) { return sp.freeze || sp.reverse || hasRamp(sp) || sp.speed != Rational(1); }

Time sourceTimeAtLocal(const Clip& c, const Time& local) {
    const ClipSpeed& sp = c.speed;
    if (sp.freeze) return c.sourceIn;
    if (hasRamp(sp)) {
        const Ramp r(sp.ramp);
        return fromSeconds(r.source(r.output(c.sourceIn.toDouble()) + local.toDouble()));
    }
    if (sp.reverse) return c.sourceIn + (c.duration - local) * sp.speed;
    return c.sourceIn + local * sp.speed;
}

Time localAtSource(const Clip& c, const Time& source) {
    const ClipSpeed& sp = c.speed;
    if (sp.freeze) return Time(0);
    if (hasRamp(sp)) {
        const Ramp r(sp.ramp);
        return fromSeconds(r.output(source.toDouble()) - r.output(c.sourceIn.toDouble()));
    }
    if (sp.reverse) return c.duration - (source - c.sourceIn) / sp.speed;
    return (source - c.sourceIn) / sp.speed;
}

Time sourceLength(const Clip& c) {
    const ClipSpeed& sp = c.speed;
    if (sp.freeze) return Time(0);
    if (hasRamp(sp)) return sourceTimeAtLocal(c, c.duration) - c.sourceIn;
    return c.duration * sp.speed;
}

Time durationForSource(const ClipSpeed& sp, const Time& sourceIn, const Time& length) {
    if (sp.freeze) throw std::invalid_argument("a freeze frame has no source length");
    if (hasRamp(sp)) {
        const Ramp r(sp.ramp);
        const double in = sourceIn.toDouble();
        return fromSeconds(r.output(in + length.toDouble()) - r.output(in));
    }
    return length / sp.speed;
}

double speedAtLocal(const Clip& c, const Time& local) {
    const ClipSpeed& sp = c.speed;
    if (sp.freeze) return 0.0;
    if (hasRamp(sp)) {
        const Ramp r(sp.ramp);
        return r.speed(r.output(c.sourceIn.toDouble()) + local.toDouble());
    }
    return sp.reverse ? -sp.speed.toDouble() : sp.speed.toDouble();
}

FrameSample sampleFrame(const Clip& c, const Time& local, const Time& frameDuration,
                        const std::optional<FrameRate>& sourceRate) {
    const ClipSpeed& sp = c.speed;
    FrameSample out;
    if (sp.freeze) {
        out.time = c.sourceIn;
        return out;
    }
    const bool blend = sp.interpolation != FrameInterpolation::Sampling && sourceRate;
    const Rational fps = sourceRate ? sourceRate->fps() : Rational(0);
    if (hasRamp(sp)) {
        const Ramp r(sp.ramp);
        const double s = r.source(r.output(c.sourceIn.toDouble()) + local.toDouble());
        if (!sourceRate) {
            out.time = fromSeconds(s);
            return out;
        }
        const double p = s * fps.toDouble();   // position in source frames
        if (blend) {
            const double f0 = std::floor(p + 1e-9);
            out.time = Time(static_cast<int64_t>(f0)) / fps;
            const double w = p - f0;
            if (w > 1e-6) {
                out.next = Time(static_cast<int64_t>(f0) + 1) / fps;
                out.weight = w;
            }
            return out;
        }
        out.time = Time(static_cast<int64_t>(std::floor(p + 0.5))) / fps;   // nearest frame (EXP-004 Bullet)
        return out;
    }
    // uniform / reverse: the lower end of the source interval [s(local), s(local + d)) (reverse: (s(local + d), s(local)])
    Time s = sp.reverse ? sourceTimeAtLocal(c, local + frameDuration) : sourceTimeAtLocal(c, local);
    if (sp.reverse && s < c.sourceIn && local < c.duration) s = c.sourceIn;   // last frame of a partially covered interval
    out.time = s;
    if (blend) {
        const Rational p = s * fps;   // exact position in source frames
        const int64_t f0 = p.floor();
        const Rational w = p - Rational(f0);
        out.time = Time(f0) / fps;
        if (!w.isZero()) {
            out.next = Time(f0 + 1) / fps;
            out.weight = w.toDouble();
        }
    }
    return out;
}

Time sourceInAfterHeadTrim(const Clip& c, const Time& delta) {
    const ClipSpeed& sp = c.speed;
    if (sp.freeze || sp.reverse) return c.sourceIn;   // reverse: the head plays the end of the range
    if (hasRamp(sp)) return sourceTimeAtLocal(c, delta);
    return c.sourceIn + delta * sp.speed;
}

Time sourceInAfterDurationChange(const Clip& c, const Time& newDuration) {
    const ClipSpeed& sp = c.speed;
    if (sp.reverse && !sp.freeze && !hasRamp(sp)) return c.sourceIn + (c.duration - newDuration) * sp.speed;
    return c.sourceIn;
}

Time frameDurationForSource(const ClipSpeed& sp, const Time& sourceIn, const Time& length, const FrameRate& rate,
                            const std::optional<Time>& mediaDuration) {
    const Time d = rate.frameDuration();
    const Time ideal = durationForSource(sp, sourceIn, length);
    int64_t frames = std::max<int64_t>(1, (ideal / d).ceil());
    if (mediaDuration) {   // the last frame's sample must start inside the media
        Clip probe;
        probe.sourceIn = sourceIn;
        probe.speed = sp;
        while (frames > 1) {
            probe.duration = d * Rational(frames);
            if (lastSampledSource(probe, rate) < *mediaDuration) break;
            --frames;
        }
    }
    return d * Rational(frames);
}

Time lastSampledSource(const Clip& c, const FrameRate& rate) {
    const Time d = rate.frameDuration();
    const int64_t frames = (c.duration / d).ceil();
    const Time last = d * Rational(std::max<int64_t>(0, frames - 1));
    if (c.speed.reverse && !c.speed.freeze) return sourceTimeAtLocal(c, d);   // frame 0 samples the end of the range
    return sourceTimeAtLocal(c, last);
}

bool exceedsMedia(const Clip& c, const FrameRate& rate, const Time& mediaDuration, const Time& slack) {
    if (!isRetimed(c.speed)) return c.sourceOut() > mediaDuration + slack;
    return !(lastSampledSource(c, rate) < mediaDuration + slack);
}

std::string speedProblem(const Clip& c) {
    const ClipSpeed& sp = c.speed;
    if (!isRetimed(sp) && sp.interpolation == FrameInterpolation::Sampling) return {};
    if (c.isGenerated()) return "titles and adjustment layers cannot be retimed";
    if (sp.freeze && (sp.reverse || hasRamp(sp))) return "a freeze frame cannot also be reversed or ramped";
    if (sp.reverse && hasRamp(sp)) return "speed ramps play forward (reverse works with a uniform speed)";
    if (sp.ramp.size() == 1) return "a speed ramp needs at least two keys";
    const double v = sp.speed.toDouble();
    if (!hasRamp(sp) && (v < kMinSpeed - 1e-12 || v > kMaxSpeed + 1e-12))
        return "speed " + std::to_string(v) + "x is outside the supported range 0.1x .. 10x";
    for (size_t i = 0; i < sp.ramp.size(); ++i) {
        const SpeedKey& k = sp.ramp[i];
        if (!(k.speed >= kMinSpeed - 1e-12 && k.speed <= kMaxSpeed + 1e-12))
            return "ramp speed " + std::to_string(k.speed) + "x is outside the supported range 0.1x .. 10x";
        if (i > 0 && !(sp.ramp[i - 1].source < k.source)) return "speed ramp keys must be in increasing source time";
    }
    // (outside its keys a ramp continues at the edge speed; running past the media is checked by exceedsMedia)
    return {};
}

std::string mediaIdentity(const std::filesystem::path& file) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) return {};
    const auto mtime = std::filesystem::last_write_time(file, ec).time_since_epoch().count();
    return file.generic_string() + "|" + std::to_string(size) + "|" + std::to_string(mtime);
}

const TrackingSample* trackSampleAt(const MotionTrack& track, int64_t frame) {
    const auto& s = track.samples;
    auto it = std::lower_bound(s.begin(), s.end(), frame, [](const TrackingSample& a, int64_t f) { return a.frame < f; });
    const TrackingSample* best = nullptr;
    for (auto c : {it, it == s.begin() ? s.end() : std::prev(it)}) {
        if (c == s.end()) continue;
        if (std::llabs(c->frame - frame) <= 1 && (!best || std::llabs(c->frame - frame) < std::llabs(best->frame - frame))) best = &*c;
    }
    return best;
}

const std::vector<RampPreset>& rampPresets() {
    // MeerCut's shapes (original), described by what they are for; "bullet" uses the measured Filmora key points.
    static const std::vector<RampPreset> presets = {
        {"bullet", "Bullet", "Fast, a slow-motion moment in the middle, fast again (key points of a measured reference preset)",
         {{0.0, 5.2}, {0.40, 5.2}, {0.46, 0.5}, {0.54, 0.5}, {0.60, 5.2}, {1.0, 5.2}}},
        {"slowmo-hit", "Slow-Mo Hit", "Normal speed, a long slow-motion stretch at two thirds, back to normal",
         {{0.0, 1.0}, {0.55, 1.0}, {0.62, 0.3}, {0.78, 0.3}, {0.85, 1.0}, {1.0, 1.0}}},
        {"speed-up", "Speed Up", "Starts at normal speed and accelerates to 4x",
         {{0.0, 1.0}, {0.3, 1.0}, {1.0, 4.0}}},
        {"slow-down", "Slow Down", "Starts at 4x and settles into normal speed",
         {{0.0, 4.0}, {0.7, 1.0}, {1.0, 1.0}}},
        {"pulse", "Pulse", "Alternates fast and slow sections",
         {{0.0, 2.5}, {0.2, 2.5}, {0.3, 0.5}, {0.45, 0.5}, {0.55, 2.5}, {0.75, 2.5}, {0.85, 0.5}, {1.0, 0.5}}},
        {"time-lapse", "Time-Lapse Ends", "Very fast start and end, real time in the middle",
         {{0.0, 8.0}, {0.25, 8.0}, {0.35, 1.0}, {0.65, 1.0}, {0.75, 8.0}, {1.0, 8.0}}},
    };
    return presets;
}

const RampPreset* findRampPreset(const std::string& id) {
    for (const auto& p : rampPresets())
        if (p.id == id) return &p;
    return nullptr;
}

std::vector<SpeedKey> rampKeys(const std::vector<std::pair<double, double>>& fractions, const Time& sourceIn,
                               const Time& length) {
    std::vector<SpeedKey> keys;
    for (const auto& [f, v] : fractions) {
        // key positions on the microsecond grid; the first and last are the exact range ends
        Time at = f <= 0.0 ? sourceIn : f >= 1.0 ? sourceIn + length : sourceIn + fromSeconds(f * length.toDouble());
        keys.push_back({at, v});
    }
    return keys;
}

}  // namespace fa
