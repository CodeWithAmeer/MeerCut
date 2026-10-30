#include "fa/core/time.h"

#include <cstdio>
#include <stdexcept>

namespace fa {

FrameRate::FrameRate(int64_t num, int64_t den) : FrameRate(Rational(num, den)) {}

FrameRate::FrameRate(const Rational& fps) : fps_(fps) {
    if (fps_ <= Rational(0)) throw std::invalid_argument("frame rate must be positive: " + fps.toString());
}

Time frameToTime(int64_t frame, const FrameRate& rate) { return Rational(frame) / rate.fps(); }

int64_t timeToFrame(const Time& t, const FrameRate& rate, Rounding mode) {
    const Rational f = t * rate.fps();
    switch (mode) {
        case Rounding::Floor: return f.floor();
        case Rounding::Ceil: return f.ceil();
        case Rounding::Nearest: return f.round();
    }
    return f.floor();
}

bool isFrameAligned(const Time& t, const FrameRate& rate) { return (t * rate.fps()).isInteger(); }

Time snapToFrame(const Time& t, const FrameRate& rate) {
    return frameToTime(timeToFrame(t, rate, Rounding::Nearest), rate);
}

std::string formatTimecode(int64_t frame, const FrameRate& rate) {
    const int64_t nominal = rate.fps().round();
    const bool neg = frame < 0;
    int64_t f = neg ? -frame : frame;
    const int64_t ff = f % nominal;
    f /= nominal;
    const int64_t ss = f % 60;
    f /= 60;
    const int64_t mm = f % 60;
    const int64_t hh = f / 60;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s%02lld:%02lld:%02lld:%02lld", neg ? "-" : "", static_cast<long long>(hh),
                  static_cast<long long>(mm), static_cast<long long>(ss), static_cast<long long>(ff));
    return buf;
}

}  // namespace fa
