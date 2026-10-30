// Time model: every timestamp/duration is an exact Rational number of seconds.
//
// Frame n of a stream with rate r starts at t = n / r and covers [n/r, (n+1)/r).
// Conversions are exact; a timestamp maps to the frame whose interval contains it
// (floor), which is also the frame a viewer sees at that instant.
#pragma once

#include "fa/core/rational.h"

#include <cstdint>
#include <string>

namespace fa {

using Time = Rational;   // seconds

class FrameRate {
public:
    FrameRate() = default;
    FrameRate(int64_t num, int64_t den = 1);   // throws unless num > 0 && den > 0
    explicit FrameRate(const Rational& fps);

    const Rational& fps() const { return fps_; }
    Rational frameDuration() const { return fps_.inverse(); }
    double toDouble() const { return fps_.toDouble(); }
    std::string toString() const { return fps_.toString(); }

    friend bool operator==(const FrameRate& a, const FrameRate& b) = default;

    // Common rates.
    static FrameRate fps24() { return {24, 1}; }
    static FrameRate fps25() { return {25, 1}; }
    static FrameRate fps30() { return {30, 1}; }
    static FrameRate ntsc30() { return {30000, 1001}; }
    static FrameRate ntsc24() { return {24000, 1001}; }
    static FrameRate ntsc60() { return {60000, 1001}; }

private:
    Rational fps_{30, 1};
};

enum class Rounding { Floor, Ceil, Nearest };

// Exact start time of frame n.
Time frameToTime(int64_t frame, const FrameRate& rate);
// Frame index containing time t (Floor), or the nearest frame boundary (Nearest), or the
// first frame starting at or after t (Ceil).
int64_t timeToFrame(const Time& t, const FrameRate& rate, Rounding mode = Rounding::Floor);
// True when t lies exactly on a frame boundary of `rate`.
bool isFrameAligned(const Time& t, const FrameRate& rate);
// Snap t to the nearest frame boundary of `rate`.
Time snapToFrame(const Time& t, const FrameRate& rate);

// Half-open interval [start, start + duration).
struct TimeRange {
    Time start;
    Time duration;

    Time end() const { return start + duration; }
    bool contains(const Time& t) const { return start <= t && t < end(); }
    bool overlaps(const TimeRange& o) const { return start < o.end() && o.start < end(); }
    bool isEmpty() const { return duration <= Time(0); }
    friend bool operator==(const TimeRange& a, const TimeRange& b) = default;
};

// SMPTE-like "HH:MM:SS:FF" for diagnostics (non-drop-frame, uses the rounded nominal rate).
std::string formatTimecode(int64_t frame, const FrameRate& rate);

}  // namespace fa
