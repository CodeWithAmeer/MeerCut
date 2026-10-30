// Title animations (M3): three independent slots per title - In, Loop, Out (Filmora inAnimation /
// loopAnimation / outAnimation, EXP-003).
//
// Timing model (EXP-003 parts 2-3): In plays in [0, dIn], Out in [D - dOut, D] and Loop repeats in
// between (period P from its start). When dIn + dOut exceeds the clip length D, both are scaled down
// proportionally. Only one slot is active at any time. All timing is evaluated from exact clip-local
// rational time, so a frame shows the same state at any frame rate.
//
// A preset changes either the whole title (opacity, offset, scale, rotation, blur — applied when the
// title layer is composited, no re-rendering) or text units (characters = grapheme clusters, words,
// lines — the text engine re-composes the cached unit sprites). Offsets are px at 1080p.
//
// Presets measured in Filmora (EXP-003) and reproduced here:
//   * Type Writer (In): units appear one at a time with hard steps; visible = round(N * t / dIn),
//     N = characters including spaces; default dIn = 0.8 s (20 frames at 25 fps).
//   * Bouncing (Loop): period 1.6 s; rises ~59 px above rest, drops ~120 px below (asymmetric).
//   * Fade Out Shake (Out): small shake (x +-3 px, y +-1.4 px) for the first 15/21, then a linear fade
//     to 0 over the last 6/21 of 0.84 s.
// All other presets are original MeerCut designs (no Filmora comparison claimed).
#pragma once

#include "fa/model/title.h"

#include <string>
#include <string_view>
#include <vector>

namespace fa {

enum class TextAnimStage { In, Loop, Out };
enum class TextAnimUnit { Whole, Char, Word, Line };

struct TextAnimPreset {
    const char* id;
    const char* label;
    TextAnimStage stage;
    const char* category;
    int defaultMs;              // In/Out: duration; Loop: period (0 = spans the whole loop window)
    TextAnimUnit unit;
    bool measured;              // behaviour compared with Filmora (EXP-003)
    const char* description;
};

const std::vector<TextAnimPreset>& textAnimPresets();
const TextAnimPreset* findTextAnimPreset(TextAnimStage stage, std::string_view id);

struct UnitAnimState {
    double opacity = 1.0;
    double dx = 0.0, dy = 0.0;  // px at 1080p
    double scale = 1.0;
    double rotation = 0.0;      // degrees clockwise
    friend bool operator==(const UnitAnimState&, const UnitAnimState&) = default;
};

struct TitleAnimState {
    // whole title
    double opacity = 1.0;
    double dx = 0.0, dy = 0.0;  // px at 1080p
    double scale = 1.0;
    double rotation = 0.0;
    double blur = 0.0;          // Gaussian sigma, px at 1080p
    // text units (empty = every unit at rest)
    TextAnimUnit unit = TextAnimUnit::Whole;
    std::vector<UnitAnimState> units;
    bool atRest() const {
        return opacity == 1.0 && dx == 0.0 && dy == 0.0 && scale == 1.0 && rotation == 0.0 && blur == 0.0 && units.empty();
    }
};

struct TextUnitCounts {
    int chars = 0;              // grapheme clusters, spaces included
    int words = 0;              // words (each with its trailing spaces)
    int lines = 0;
    double blockHeight = 0.0;   // px at 1080p (Credits Roll travel)
    int count(TextAnimUnit u) const {
        return u == TextAnimUnit::Char ? chars : u == TextAnimUnit::Word ? words : u == TextAnimUnit::Line ? lines : 1;
    }
};

struct TitleAnimWindows {
    Time inEnd;      // In: [0, inEnd]
    Time outStart;   // Out: [outStart, D]
};
TitleAnimWindows titleAnimWindows(const TitleData& title, const Time& clipDuration);
Time effectiveDuration(const TextAnimation& slot, TextAnimStage stage);

TitleAnimState evaluateTitleAnimation(const TitleData& title, const Time& local, const Time& clipDuration,
                                      const TextUnitCounts& counts);

}  // namespace fa
