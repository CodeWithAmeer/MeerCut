#include "fa/render/text_animation.h"

#include "fa/anim/animatable.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fa {

namespace {

using S = TextAnimStage;
using U = TextAnimUnit;

const std::vector<TextAnimPreset> kPresets = {
    // ---- In ----
    {"fade", "Fade In", S::In, "Fade & Blur", 500, U::Whole, false, "Fades in."},
    {"typewriter", "Type Writer", S::In, "Typing", 800, U::Char, true,
     "Characters appear one at a time."},
    {"slide_left", "Slide In Left", S::In, "Motion", 600, U::Whole, false, "Slides in from the left while fading in."},
    {"slide_right", "Slide In Right", S::In, "Motion", 600, U::Whole, false, "Slides in from the right while fading in."},
    {"slide_up", "Slide Up", S::In, "Motion", 600, U::Whole, false, "Rises into place while fading in."},
    {"slide_down", "Slide Down", S::In, "Motion", 600, U::Whole, false, "Drops into place while fading in."},
    {"zoom_in", "Zoom In", S::In, "Zoom", 600, U::Whole, false, "Grows from small while fading in."},
    {"pop", "Pop", S::In, "Zoom", 500, U::Whole, false, "Pops in with a slight overshoot."},
    {"roll_in", "Roll In", S::In, "Motion", 700, U::Whole, false, "Rotates and grows into place."},
    {"blur_in", "Blur In", S::In, "Fade & Blur", 800, U::Whole, false, "Comes into focus while fading in."},
    {"drop_in", "Drop In", S::In, "Motion", 900, U::Whole, false, "Falls from above and bounces to rest."},
    {"char_fade", "Character Fade", S::In, "Typing", 1000, U::Char, false, "Characters fade in one after another."},
    {"char_rise", "Character Rise", S::In, "Typing", 1000, U::Char, false, "Characters rise into place one after another."},
    {"char_pop", "Character Pop", S::In, "Typing", 1000, U::Char, false, "Characters pop in one after another."},
    {"word_reveal", "Word by Word", S::In, "Typing", 1000, U::Word, false, "Words appear one at a time."},
    {"word_fade", "Word Fade", S::In, "Typing", 1200, U::Word, false, "Words fade in one after another."},
    {"line_slide", "Line by Line", S::In, "Motion", 1000, U::Line, false, "Lines slide in from the left one after another."},
    // ---- Loop ----
    {"bounce", "Bouncing", S::Loop, "Motion", 1600, U::Whole, true,
     "Hops up and drops down, period 1.6 s."},
    {"pulse", "Pulse", S::Loop, "Motion", 1000, U::Whole, false, "Gently grows and shrinks."},
    {"swing", "Swing", S::Loop, "Motion", 1200, U::Whole, false, "Swings left and right."},
    {"shake", "Trembling", S::Loop, "Motion", 500, U::Whole, false, "Small continuous shake."},
    {"blink", "Blinking", S::Loop, "Flicker", 1000, U::Whole, false, "Blinks on and off."},
    {"float", "Float", S::Loop, "Motion", 2000, U::Whole, false, "Floats slowly up and down."},
    {"wave", "Wave", S::Loop, "Motion", 1200, U::Char, false, "A wave runs through the characters."},
    {"roll_up", "Credits Roll", S::Loop, "Credits", 0, U::Whole, false,
     "Scrolls the text from below the frame to above it over the visible time."},
    // ---- Out ----
    {"fade", "Fade Out", S::Out, "Fade & Blur", 500, U::Whole, false, "Fades out."},
    {"typewriter", "Type Writer", S::Out, "Typing", 800, U::Char, false, "Characters disappear from the end, one at a time."},
    {"slide_left", "Slide Out Left", S::Out, "Motion", 600, U::Whole, false, "Slides out to the left while fading out."},
    {"slide_right", "Slide Out Right", S::Out, "Motion", 600, U::Whole, false, "Slides out to the right while fading out."},
    {"slide_up", "Slide Out Up", S::Out, "Motion", 600, U::Whole, false, "Rises out while fading out."},
    {"slide_down", "Slide Out Down", S::Out, "Motion", 600, U::Whole, false, "Drops out while fading out."},
    {"zoom_out", "Zoom Out", S::Out, "Zoom", 600, U::Whole, false, "Shrinks while fading out."},
    {"blur_out", "Blur Out", S::Out, "Fade & Blur", 800, U::Whole, false, "Goes out of focus while fading out."},
    {"char_fade", "Character Fade", S::Out, "Typing", 1000, U::Char, false, "Characters fade out one after another."},
    {"word_hide", "Word by Word", S::Out, "Typing", 1000, U::Word, false, "Words disappear one at a time."},
    {"shake_fade", "Fade Out Shake", S::Out, "Motion", 840, U::Whole, true,
     "Shakes, then fades out."},
};

constexpr double kSlidePx = 240.0;

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
double easeOutCubic(double p) { return 1.0 - std::pow(1.0 - p, 3.0); }
double easeInCubic(double p) { return p * p * p; }
double easeOutBack(double p) {
    const double c1 = 1.70158, c3 = c1 + 1.0;
    return 1.0 + c3 * std::pow(p - 1.0, 3.0) + c1 * std::pow(p - 1.0, 2.0);
}
double easeOutBounce(double p) {
    const double n = 7.5625, d = 2.75;
    if (p < 1.0 / d) return n * p * p;
    if (p < 2.0 / d) return n * (p -= 1.5 / d) * p + 0.75;
    if (p < 2.5 / d) return n * (p -= 2.25 / d) * p + 0.9375;
    return n * (p -= 2.625 / d) * p + 0.984375;
}

// Progress of unit i of n when units start one after another, each animating over `span` of the window.
double staggered(double p, int i, int n, double span = 0.4) {
    if (n <= 1) return clamp01(p);
    const double start = (1.0 - span) * i / (n - 1);
    return clamp01((p - start) / span);
}

// Number of units shown at fraction `num/den` of a reveal over n units with hard steps:
// round(n * num / den) (EXP-003 Type Writer), exact in rational arithmetic.
int revealCount(int n, const Time& elapsed, const Time& total) {
    if (total <= Time(0)) return n;
    const Rational r = Rational(n) * elapsed / total;
    return static_cast<int>(std::clamp<int64_t>(r.round(), 0, n));
}

// Bouncing (EXP-003): keys over one period (Bezier); flat tangents at the measured top and bottom so the
// extremes are exactly there, and the same slope where the curve passes the rest position at the period
// boundary, so consecutive periods join smoothly.
const Animatable<double>& bounceCurve() {
    static const Animatable<double> curve = [] {
        Animatable<double> a;
        const double crossing = (-59.0 - 120.0) / 0.65;   // bottom (0.70) to the next top (1.35), per period
        auto key = [&](Rational t, double v, TangentMode m, double slope = 0.0) {
            Keyframe<double> k;
            k.time = t;
            k.value = v;
            k.interp = Interpolation::Bezier;
            k.tangent = m;
            k.inSlope = k.outSlope = slope;
            a.setKey(k);
        };
        key(Rational(0), 0.0, TangentMode::Manual, crossing);
        key(Rational(35, 100), -59.0, TangentMode::Flat);   // top 14/40 of the period after the start
        key(Rational(70, 100), 120.0, TangentMode::Flat);   // bottom 28/40
        key(Rational(1), 0.0, TangentMode::Manual, crossing);
        return a;
    }();
    return curve;
}

// Smooth deterministic jitter (frame-rate independent): sum of incommensurate sines.
double jitter(double t, double seed) {
    return (std::sin(2 * std::numbers::pi * (7.31 * t + seed)) + 0.6 * std::sin(2 * std::numbers::pi * (13.17 * t + 2.1 * seed))) / 1.6;
}

void applyWholeIn(const std::string& id, double p, TitleAnimState& s) {
    if (id == "fade") s.opacity = p;
    else if (id.rfind("slide_", 0) == 0) {
        const double e = 1.0 - easeOutCubic(p);
        if (id == "slide_left") s.dx = -kSlidePx * e;
        if (id == "slide_right") s.dx = kSlidePx * e;
        if (id == "slide_up") s.dy = kSlidePx * e;
        if (id == "slide_down") s.dy = -kSlidePx * e;
        s.opacity = clamp01(p * 1.6);
    } else if (id == "zoom_in") {
        s.scale = 0.3 + 0.7 * easeOutCubic(p);
        s.opacity = clamp01(p * 1.5);
    } else if (id == "pop") {
        s.scale = std::max(0.0, easeOutBack(p));
        s.opacity = clamp01(p * 3.0);
    } else if (id == "roll_in") {
        const double e = easeOutCubic(p);
        s.rotation = -90.0 * (1.0 - e);
        s.scale = 0.5 + 0.5 * e;
        s.opacity = clamp01(p * 2.0);
    } else if (id == "blur_in") {
        s.blur = 24.0 * (1.0 - easeOutCubic(p));
        s.opacity = clamp01(p * 1.4);
    } else if (id == "drop_in") {
        s.dy = -360.0 * (1.0 - easeOutBounce(p));
        s.opacity = clamp01(p * 4.0);
    }
}

void applyWholeOut(const std::string& id, double p, TitleAnimState& s) {   // p: 0 = at rest .. 1 = gone
    if (id == "fade") s.opacity = 1.0 - p;
    else if (id.rfind("slide_", 0) == 0) {
        const double e = easeInCubic(p);
        if (id == "slide_left") s.dx = -kSlidePx * e;
        if (id == "slide_right") s.dx = kSlidePx * e;
        if (id == "slide_up") s.dy = -kSlidePx * e;
        if (id == "slide_down") s.dy = kSlidePx * e;
        s.opacity = clamp01((1.0 - p) * 1.6);
    } else if (id == "zoom_out") {
        s.scale = 1.0 - 0.7 * easeInCubic(p);
        s.opacity = clamp01((1.0 - p) * 1.5);
    } else if (id == "blur_out") {
        s.blur = 24.0 * easeInCubic(p);
        s.opacity = clamp01((1.0 - p) * 1.4);
    } else if (id == "shake_fade") {   // EXP-003: 15 frames of shake, then 6 frames of linear fade (21 total)
        const double shakeEnd = 15.0 / 21.0;
        if (p < shakeEnd) {
            const double t = p * 0.84;
            s.dx = 3.0 * jitter(t, 0.13);
            s.dy = 1.4 * jitter(t, 0.71);
        } else {
            s.opacity = clamp01(1.0 - (p - shakeEnd) / (1.0 - shakeEnd));
        }
    }
}

void applyLoop(const std::string& id, double phase, double tSec, double windowProgress, const TextUnitCounts& counts,
               TitleAnimState& s) {
    if (id == "bounce") s.dy = bounceCurve().value(phase);
    else if (id == "pulse") s.scale = 1.0 + 0.04 * (1.0 - std::cos(2 * std::numbers::pi * phase));
    else if (id == "swing") s.rotation = 8.0 * std::sin(2 * std::numbers::pi * phase);
    else if (id == "shake") {
        s.dx = 4.0 * jitter(tSec, 0.37);
        s.dy = 3.0 * jitter(tSec, 0.83);
    } else if (id == "blink") s.opacity = phase < 0.5 ? 1.0 : 0.25;
    else if (id == "float") s.dy = 12.0 * std::sin(2 * std::numbers::pi * phase);
    else if (id == "roll_up") {   // text centre from below the frame to above it
        const double travel = 540.0 + counts.blockHeight / 2.0;
        s.dy = travel - 2.0 * travel * windowProgress;
    } else if (id == "wave") {
        s.unit = U::Char;
        s.units.resize(static_cast<size_t>(std::max(counts.chars, 0)));
        for (size_t i = 0; i < s.units.size(); ++i)
            s.units[i].dy = -12.0 * std::sin(2 * std::numbers::pi * (phase - 0.08 * static_cast<double>(i)));
    }
}

// Per-unit In/Out. `p` is the slot progress; `out` reverses the effect (1 = gone).
void applyUnits(const std::string& id, bool out, double p, const Time& elapsed, const Time& total, TextAnimUnit unit,
                const TextUnitCounts& counts, TitleAnimState& s) {
    const int n = std::max(counts.count(unit), 0);
    s.unit = unit;
    s.units.assign(static_cast<size_t>(n), UnitAnimState{});
    if (id == "typewriter" || id == "word_reveal" || id == "word_hide") {
        const int shown = out ? n - revealCount(n, elapsed, total) : revealCount(n, elapsed, total);
        for (int i = 0; i < n; ++i) s.units[static_cast<size_t>(i)].opacity = i < shown ? 1.0 : 0.0;
        return;
    }
    for (int i = 0; i < n; ++i) {
        UnitAnimState& u = s.units[static_cast<size_t>(i)];
        const double q = staggered(p, i, n);
        const double v = out ? 1.0 - q : q;   // 1 = at rest
        if (id == "char_fade" || id == "word_fade") {
            u.opacity = v;
        } else if (id == "char_rise") {
            u.opacity = v;
            u.dy = 40.0 * (1.0 - easeOutCubic(v));
        } else if (id == "char_pop") {
            u.scale = std::max(0.0, easeOutBack(v));
            u.opacity = clamp01(v * 3.0);
        } else if (id == "line_slide") {
            u.dx = -kSlidePx * (1.0 - easeOutCubic(v));
            u.opacity = clamp01(v * 1.6);
        }
    }
}

}  // namespace

const std::vector<TextAnimPreset>& textAnimPresets() { return kPresets; }

const TextAnimPreset* findTextAnimPreset(TextAnimStage stage, std::string_view id) {
    for (const auto& p : kPresets)
        if (p.stage == stage && id == p.id) return &p;
    return nullptr;
}

Time effectiveDuration(const TextAnimation& slot, TextAnimStage stage) {
    if (!slot.active()) return Time(0);
    if (slot.duration > Time(0)) return slot.duration;
    const TextAnimPreset* p = findTextAnimPreset(stage, slot.preset);
    return p ? Rational(p->defaultMs, 1000) : Time(0);
}

TitleAnimWindows titleAnimWindows(const TitleData& t, const Time& D) {
    Time din = effectiveDuration(t.in, S::In), dout = effectiveDuration(t.out, S::Out);
    if (din + dout > D && din + dout > Time(0)) {   // scale both to fit the clip
        const Rational k = D / (din + dout);
        din = din * k;
        dout = dout * k;
    }
    return {din, D - dout};
}

TitleAnimState evaluateTitleAnimation(const TitleData& t, const Time& local, const Time& D, const TextUnitCounts& counts) {
    TitleAnimState s;
    if (D <= Time(0)) return s;
    const TitleAnimWindows w = titleAnimWindows(t, D);
    // In
    if (t.in.active() && local < w.inEnd) {
        const TextAnimPreset* p = findTextAnimPreset(S::In, t.in.preset);
        if (!p) return s;
        const double prog = clamp01((local / w.inEnd).toDouble());
        if (p->unit == U::Whole) applyWholeIn(p->id, prog, s);
        else applyUnits(p->id, false, prog, local, w.inEnd, p->unit, counts, s);
        return s;
    }
    // Out
    if (t.out.active() && local >= w.outStart) {
        const TextAnimPreset* p = findTextAnimPreset(S::Out, t.out.preset);
        if (!p) return s;
        const Time total = D - w.outStart, elapsed = local - w.outStart;
        // progress measured to the clip's last frame end: the final frame shows the fully-out state
        const double prog = total > Time(0) ? clamp01((elapsed / total).toDouble()) : 1.0;
        if (p->unit == U::Whole) applyWholeOut(p->id, prog, s);
        else applyUnits(p->id, true, prog, elapsed, total, p->unit, counts, s);
        return s;
    }
    // Loop
    if (t.loop.active()) {
        const TextAnimPreset* p = findTextAnimPreset(S::Loop, t.loop.preset);
        if (!p) return s;
        const Time start = t.in.active() ? w.inEnd : Time(0);
        const Time end = t.out.active() ? w.outStart : D;
        const Time since = local - start;
        const Time period = effectiveDuration(t.loop, S::Loop);
        double phase = 0.0;
        if (period > Time(0)) {
            const Rational cycles = since / period;
            phase = (cycles - Rational(cycles.floor())).toDouble();
        }
        const double windowProgress = end > start ? clamp01((since / (end - start)).toDouble()) : 0.0;
        applyLoop(p->id, phase, since.toDouble(), windowProgress, counts, s);
    }
    return s;
}

}  // namespace fa
