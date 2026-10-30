// Keyframed values.
//
// Segment model (matches the behaviour measured in Stage 1, EXP-002):
//   * Each segment k0 -> k1 is evaluated as a cubic Bézier in (time, value):
//       P0 = (t0, v0)
//       P1 = (t0 + h0*dt, v0 + h0*dt*s0)      h0 = k0.outHandle, s0 = k0 outgoing slope
//       P2 = (t1 - h1*dt, v1 - h1*dt*s1)      h1 = k1.inHandle,  s1 = k1 incoming slope
//       P3 = (t1, v1)
//     The time coordinate is inverted numerically, so the value is a true function of time.
//   * Slopes come from the key's TangentMode:
//       Linear - slope of the adjacent segment (keeps straight lines straight)
//       Flat   - 0 (ease)
//       Auto   - Catmull-Rom: (v_next - v_prev)/(t_next - t_prev) at interior keys, flat at ends
//                (Filmora "Continuous Curve": overshoot is expected and intended)
//       Manual - inSlope / outSlope as given (value units per second)
//   * Interpolation::Linear segments are straight; Interpolation::Hold keeps v0 until t1.
//   * Before the first key the first value holds, after the last key the last value holds.
//
// T must be a vector-space type: T + T, T - T, T * double, and value-initialisable to zero.
#pragma once

#include "fa/core/time.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <vector>

namespace fa {

enum class Interpolation { Linear, Hold, Bezier };
enum class TangentMode { Linear, Flat, Auto, Manual };

template <typename T>
struct Keyframe {
    Time time;                      // in the owner's time base (clips: source time)
    T value{};
    Interpolation interp = Interpolation::Linear;   // shape of the segment that LEAVES this key
    TangentMode tangent = TangentMode::Linear;
    T inSlope{};                    // Manual only (value / second)
    T outSlope{};
    double inHandle = 0.5;          // fraction of the adjacent segment duration, [0, 1]
    double outHandle = 0.5;

    friend bool operator==(const Keyframe&, const Keyframe&) = default;
};

namespace detail {

// x(u) for a unit cubic with x0 = 0, x1 = a, x2 = b, x3 = 1.
inline double cubic(double u, double p1, double p2) {
    const double v = 1.0 - u;
    return 3.0 * v * v * u * p1 + 3.0 * v * u * u * p2 + u * u * u;
}
inline double cubicDeriv(double u, double p1, double p2) {
    const double v = 1.0 - u;
    return 3.0 * v * v * p1 + 6.0 * v * u * (p2 - p1) + 3.0 * u * u * (1.0 - p2);
}
// Solve cubic(u, p1, p2) == x for u in [0, 1]; monotonic when 0 <= p1, p2 <= 1.
inline double solveBezierParam(double x, double p1, double p2) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    double lo = 0.0, hi = 1.0, u = x;
    for (int i = 0; i < 64; ++i) {
        const double fx = cubic(u, p1, p2) - x;
        if (std::fabs(fx) < 1e-13) return u;
        if (fx > 0) hi = u; else lo = u;
        const double d = cubicDeriv(u, p1, p2);
        double next = (d > 1e-12) ? u - fx / d : 0.5 * (lo + hi);
        if (next <= lo || next >= hi) next = 0.5 * (lo + hi);   // Newton left the bracket: bisect
        u = next;
    }
    return u;
}

}  // namespace detail

template <typename T>
class Animatable {
public:
    using Key = Keyframe<T>;

    Animatable() = default;
    explicit Animatable(T constant) : constant_(constant) {}

    bool isAnimated() const { return !keys_.empty(); }
    const std::vector<Key>& keys() const { return keys_; }
    const T& constantValue() const { return constant_; }
    void setConstant(T v) { constant_ = v; }

    // Insert or replace the key at k.time; keeps keys sorted by time.
    void setKey(const Key& k) {
        auto it = std::lower_bound(keys_.begin(), keys_.end(), k.time,
                                   [](const Key& a, const Time& t) { return a.time < t; });
        if (it != keys_.end() && it->time == k.time) *it = k;
        else keys_.insert(it, k);
    }
    bool removeKey(const Time& t) {
        auto it = std::find_if(keys_.begin(), keys_.end(), [&](const Key& a) { return a.time == t; });
        if (it == keys_.end()) return false;
        keys_.erase(it);
        return true;
    }
    void clearKeys() { keys_.clear(); }
    Key* keyAt(const Time& t) {
        for (auto& k : keys_)
            if (k.time == t) return &k;
        return nullptr;
    }
    const Key* keyAt(const Time& t) const { return const_cast<Animatable*>(this)->keyAt(t); }

    // Value at time t (seconds, same base as key times).
    T value(double t) const {
        if (keys_.empty()) return constant_;
        if (t <= keys_.front().time.toDouble()) return keys_.front().value;
        if (t >= keys_.back().time.toDouble()) return keys_.back().value;
        const auto it = std::upper_bound(keys_.begin(), keys_.end(), t,
                                         [](double x, const Key& a) { return x < a.time.toDouble(); });
        const size_t i1 = static_cast<size_t>(it - keys_.begin());
        return evalSegment(i1 - 1, t);
    }
    T value(const Time& t) const { return value(t.toDouble()); }

    // Effective outgoing / incoming slope at key i (value per second).
    T outSlopeAt(size_t i) const { return slopeAt(i, /*outgoing=*/true); }
    T inSlopeAt(size_t i) const { return slopeAt(i, /*outgoing=*/false); }

    friend bool operator==(const Animatable&, const Animatable&) = default;

private:
    T segmentSlope(size_t i0) const {   // straight-line slope of segment i0 -> i0+1
        const Key& a = keys_[i0];
        const Key& b = keys_[i0 + 1];
        const double dt = (b.time - a.time).toDouble();
        return (b.value - a.value) * (1.0 / dt);
    }

    T slopeAt(size_t i, bool outgoing) const {
        const Key& k = keys_[i];
        const size_t n = keys_.size();
        switch (k.tangent) {
            case TangentMode::Flat: return T{};
            case TangentMode::Manual: return outgoing ? k.outSlope : k.inSlope;
            case TangentMode::Linear:
                if (outgoing) return i + 1 < n ? segmentSlope(i) : T{};
                return i > 0 ? segmentSlope(i - 1) : T{};
            case TangentMode::Auto: {
                if (i == 0 || i + 1 >= n) return T{};   // end keys are flat
                const Key& p = keys_[i - 1];
                const Key& q = keys_[i + 1];
                return (q.value - p.value) * (1.0 / (q.time - p.time).toDouble());
            }
        }
        return T{};
    }

    T evalSegment(size_t i0, double t) const {
        const Key& a = keys_[i0];
        const Key& b = keys_[i0 + 1];
        const double t0 = a.time.toDouble(), t1 = b.time.toDouble();
        const double dt = t1 - t0;
        const double x = (t - t0) / dt;
        switch (a.interp) {
            case Interpolation::Hold: return a.value;
            case Interpolation::Linear: return a.value + (b.value - a.value) * x;
            case Interpolation::Bezier: break;
        }
        const double h0 = std::clamp(a.outHandle, 0.0, 1.0);
        const double h1 = std::clamp(b.inHandle, 0.0, 1.0);
        const double u = detail::solveBezierParam(x, h0, 1.0 - h1);
        const double w = 1.0 - u;
        const T p0 = a.value;
        const T p1 = a.value + slopeAt(i0, true) * (h0 * dt);
        const T p2 = b.value - slopeAt(i0 + 1, false) * (h1 * dt);
        const T p3 = b.value;
        return p0 * (w * w * w) + p1 * (3.0 * w * w * u) + p2 * (3.0 * w * u * u) + p3 * (u * u * u);
    }

    T constant_{};
    std::vector<Key> keys_;
};

// Small 2-D vector used for positions/scales.
struct Vec2 {
    double x = 0.0;
    double y = 0.0;
    friend Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
    friend Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
    friend Vec2 operator*(Vec2 a, double s) { return {a.x * s, a.y * s}; }
    friend bool operator==(const Vec2&, const Vec2&) = default;
};

}  // namespace fa
