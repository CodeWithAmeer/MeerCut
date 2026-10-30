#include "fa/model/motion_presets.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace fa {

namespace {

// One property's movement: values at fractions of the preset's window (0 = its start, 1 = its end), each multiplied by
// (or added to) the property's own resting value.
struct Track {
    enum class Prop { PosX, PosY, Scale, Rotation, Opacity } prop;
    std::vector<std::pair<double, double>> points;   // (fraction, value)
    bool relative = true;                            // scale / opacity: x resting value; position / rotation: + resting
};

struct Def {
    MotionPreset info;
    std::vector<Track> tracks;
};

using P = Track::Prop;

std::vector<Def> buildDefs() {
    std::vector<Def> d;
    auto in = [&](std::string id, std::string name, std::vector<Track> t) { d.push_back({{std::move(id), std::move(name), MotionSlot::In}, std::move(t)}); };
    auto out = [&](std::string id, std::string name, std::vector<Track> t) { d.push_back({{std::move(id), std::move(name), MotionSlot::Out}, std::move(t)}); };
    auto combo = [&](std::string id, std::string name, std::vector<Track> t) { d.push_back({{std::move(id), std::move(name), MotionSlot::Combo}, std::move(t)}); };
    // In: from the effect to rest
    in("fade_in", "Fade In", {{P::Opacity, {{0, 0}, {1, 1}}}});
    in("zoom_in", "Zoom In", {{P::Scale, {{0, 0.5}, {1, 1}}}, {P::Opacity, {{0, 0}, {0.4, 1}}}});
    in("zoom_out", "Zoom Out", {{P::Scale, {{0, 1.6}, {1, 1}}}, {P::Opacity, {{0, 0}, {0.4, 1}}}});
    in("boom", "Boom", {{P::Scale, {{0, 0.0}, {0.7, 1.12}, {1, 1}}}});
    in("slide_left", "Slide Left", {{P::PosX, {{0, 1.0}, {1, 0}}}});
    in("slide_right", "Slide Right", {{P::PosX, {{0, -1.0}, {1, 0}}}});
    in("slide_up", "Slide Up", {{P::PosY, {{0, 1.0}, {1, 0}}}});
    in("slide_down", "Slide Down", {{P::PosY, {{0, -1.0}, {1, 0}}}});
    in("rise", "Rise", {{P::PosY, {{0, 0.12}, {1, 0}}}, {P::Opacity, {{0, 0}, {1, 1}}}});
    in("spin_in", "Spin In", {{P::Rotation, {{0, -180}, {1, 0}}}, {P::Scale, {{0, 0.2}, {1, 1}}}, {P::Opacity, {{0, 0}, {0.5, 1}}}});
    in("swing_in", "Swing In", {{P::Rotation, {{0, -25}, {0.6, 8}, {1, 0}}}, {P::Opacity, {{0, 0}, {0.4, 1}}}});
    // Out: from rest to the effect
    out("fade_out", "Fade Out", {{P::Opacity, {{0, 1}, {1, 0}}}});
    out("zoom_in_out", "Zoom In", {{P::Scale, {{0, 1}, {1, 1.6}}}, {P::Opacity, {{0.6, 1}, {1, 0}}}});
    out("zoom_out_out", "Zoom Out", {{P::Scale, {{0, 1}, {1, 0.4}}}, {P::Opacity, {{0.6, 1}, {1, 0}}}});
    out("slide_left_out", "Slide Left", {{P::PosX, {{0, 0}, {1, -1.0}}}});
    out("slide_right_out", "Slide Right", {{P::PosX, {{0, 0}, {1, 1.0}}}});
    out("slide_up_out", "Slide Up", {{P::PosY, {{0, 0}, {1, -1.0}}}});
    out("slide_down_out", "Slide Down", {{P::PosY, {{0, 0}, {1, 1.0}}}});
    out("sink", "Sink", {{P::PosY, {{0, 0}, {1, 0.12}}}, {P::Opacity, {{0, 1}, {1, 0}}}});
    out("spin_out", "Spin Out", {{P::Rotation, {{0, 0}, {1, 180}}}, {P::Scale, {{0, 1}, {1, 0.2}}}, {P::Opacity, {{0.5, 1}, {1, 0}}}});
    // Combo: over the whole clip
    combo("ken_burns", "Ken Burns", {{P::Scale, {{0, 1.0}, {1, 1.18}}}, {P::PosX, {{0, -0.03}, {1, 0.03}}}});
    combo("push_in", "Slow Push In", {{P::Scale, {{0, 1.0}, {1, 1.25}}}});
    combo("pull_out", "Slow Pull Out", {{P::Scale, {{0, 1.25}, {1, 1.0}}}});
    combo("pan_left", "Pan Left", {{P::Scale, {{0, 1.2}, {1, 1.2}}}, {P::PosX, {{0, 0.08}, {1, -0.08}}}});
    combo("pan_right", "Pan Right", {{P::Scale, {{0, 1.2}, {1, 1.2}}}, {P::PosX, {{0, -0.08}, {1, 0.08}}}});
    {   // pulse: a gentle beat every second (keys generated for the clip's length in applyMotionPreset)
        d.push_back({{"pulse", "Pulse", MotionSlot::Combo}, {}});
        d.push_back({{"shake", "Shake", MotionSlot::Combo}, {}});
    }
    return d;
}

const std::vector<Def>& defs() {
    static const std::vector<Def> v = buildDefs();
    return v;
}

Animatable<double>& property(Clip& c, P p) {
    switch (p) {
        case P::PosX: return c.transform.positionX;
        case P::PosY: return c.transform.positionY;
        case P::Scale: return c.transform.scaleX;
        case P::Rotation: return c.transform.rotation;
        case P::Opacity: return c.opacity;
    }
    return c.opacity;
}

std::string& slotId(Clip& c, MotionSlot s) { return s == MotionSlot::In ? c.motionIn : s == MotionSlot::Out ? c.motionOut : c.motionCombo; }

// The window a slot covers, in source time (keys live in source time).
std::pair<Time, Time> window(const Clip& c, MotionSlot s) {
    const Time len = motionLength(c.duration);
    const Time a = s == MotionSlot::Out ? c.timelineStart + c.duration - len : c.timelineStart;
    const Time b = s == MotionSlot::In ? c.timelineStart + len : c.timelineStart + c.duration;
    Time sa = c.sourceTimeAt(a), sb = c.sourceTimeAt(b);
    if (sb < sa) std::swap(sa, sb);   // reversed clips
    return {sa, sb};
}

void removeKeysIn(Animatable<double>& a, const Time& from, const Time& to) {
    std::vector<Time> drop;
    for (const auto& k : a.keys())
        if (k.time >= from && k.time <= to) drop.push_back(k.time);
    for (const auto& t : drop) a.removeKey(t);
    if (a.keys().size() == 1) {   // a single key left: the property is constant at that value
        const double v = a.keys().front().value;
        a.clearKeys();
        a.setConstant(v);
    }
}

void putKey(Animatable<double>& a, const Time& t, double v) {
    Keyframe<double> k;
    k.time = t;
    k.value = v;
    k.interp = Interpolation::Bezier;   // eased, like Filmora's presets
    k.tangent = TangentMode::Flat;
    a.setKey(k);
}

}  // namespace

const std::vector<MotionPreset>& motionPresets() {
    static const std::vector<MotionPreset> v = [] {
        std::vector<MotionPreset> out;
        for (const auto& d : defs()) out.push_back(d.info);
        return out;
    }();
    return v;
}

const MotionPreset* findMotionPreset(const std::string& id, MotionSlot slot) {
    for (const auto& p : motionPresets())
        if (p.id == id && p.slot == slot) return &p;
    return nullptr;
}

Time motionLength(const Time& clipLength) {
    const Time preferred = Rational(3, 5);
    const Time third = clipLength * Rational(1, 3);
    return third < preferred ? third : preferred;
}

void applyMotionPreset(Clip& c, MotionSlot slot, const std::string& id) {
    const Def* def = nullptr;
    for (const auto& d : defs())
        if (d.info.id == id && d.info.slot == slot) def = &d;
    if (!id.empty() && !def) throw ModelError("unknown motion preset '" + id + "'");
    const auto [from, to] = window(c, slot);
    // remove what the slot held: the keys in its window on the properties that preset wrote (not others: a Combo
    // window covers the In and Out ones)
    if (const std::string old = slotId(c, slot); !old.empty()) {
        std::vector<P> props;
        if (old == "pulse") props = {P::Scale};
        else if (old == "shake") props = {P::PosX};
        for (const auto& d : defs())
            if (d.info.id == old && d.info.slot == slot)
                for (const auto& t : d.tracks) props.push_back(t.prop);
        for (P p : props) {
            removeKeysIn(property(c, p), from, to);
            if (p == P::Scale) removeKeysIn(c.transform.scaleY, from, to);
        }
    }
    slotId(c, slot) = id;
    if (!def) return;
    auto resting = [&](P p) {
        const Animatable<double>& a = property(c, p);
        return a.isAnimated() ? a.value(slot == MotionSlot::Out ? from : to) : a.constantValue();
    };
    auto write = [&](P p, const std::vector<std::pair<double, double>>& points, bool relative) {
        const double rest = resting(p);
        auto put = [&](Animatable<double>& a, double rv) {
            const bool wasAnimated = a.isAnimated();
            const double constant = a.constantValue();
            for (const auto& [f, v] : points) {
                const Time t = from + (to - from) * Rational::fromDouble(f, 1000);
                putKey(a, t, relative ? rv * v : rv + v);
            }
            if (!wasAnimated) {   // keep the resting value outside the window
                const Time edge = slot == MotionSlot::In ? to : from;
                if (!a.keyAt(edge)) putKey(a, edge, constant);
            }
        };
        put(property(c, p), rest);
        if (p == P::Scale) put(c.transform.scaleY, c.transform.uniformScale ? rest : resting(P::Scale));
    };
    if (id == "pulse" || id == "shake") {   // keys for the clip's whole length
        const double len = (to - from).toDouble();
        if (len <= 0) return;
        std::vector<std::pair<double, double>> pts;
        if (id == "pulse") {
            for (double t = 0; t <= len + 1e-9; t += 0.5) pts.push_back({t / len, std::fmod(t, 1.0) < 0.25 ? 1.0 : 1.06});
            write(P::Scale, pts, true);
        } else {
            int i = 0;
            for (double t = 0; t <= len + 1e-9; t += 0.08, ++i) pts.push_back({t / len, (i % 2 ? 1 : -1) * 0.006 * (1 + (i % 3))});
            write(P::PosX, pts, false);
        }
        return;
    }
    for (const auto& tr : def->tracks)
        write(tr.prop, tr.points, tr.prop == P::Scale || tr.prop == P::Opacity);
}

}  // namespace fa
