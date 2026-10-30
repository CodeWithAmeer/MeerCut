#include "fa/model/keyframes.h"

#include "fa/model/time_map.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace fa::keys {

const char* styleLabel(Style s) {
    switch (s) {
        case Style::Linear: return "Linear";
        case Style::Hold: return "Hold";
        case Style::Ease: return "Ease In/Out";
        case Style::Smooth: return "Smooth (continuous curve)";
    }
    return "Linear";
}

bool hasKeyAt(const Animatable<double>& a, const Time& t) { return a.keyAt(t) != nullptr; }

namespace {
Keyframe<double> newKey(const Animatable<double>& a, const Time& t, double v) {
    Keyframe<double> k;
    k.time = t;
    k.value = v;
    // inherit the style of the previous key so a curve keeps its character
    const Keyframe<double>* prev = nullptr;
    for (const auto& x : a.keys())
        if (x.time < t) prev = &x;
    if (prev) {
        k.interp = prev->interp;
        k.tangent = prev->tangent == TangentMode::Manual ? TangentMode::Flat : prev->tangent;
    }
    return k;
}
}  // namespace

void setValueAt(Animatable<double>& a, const Time& t, double v) {
    if (!a.isAnimated()) {
        a.setConstant(v);
        return;
    }
    if (Keyframe<double>* k = a.keyAt(t)) {
        k->value = v;
        return;
    }
    a.setKey(newKey(a, t, v));
}

void toggleKey(Animatable<double>& a, const Time& t) {
    if (a.keyAt(t)) {
        const double v = a.value(t);
        a.removeKey(t);
        if (!a.isAnimated()) a.setConstant(v);
        return;
    }
    a.setKey(newKey(a, t, a.value(t)));
}

void setStyle(Animatable<double>& a, const Time& t, Style s) {
    Keyframe<double>* k = a.keyAt(t);
    if (!k) return;
    switch (s) {
        case Style::Linear:
            k->interp = Interpolation::Linear;
            k->tangent = TangentMode::Linear;
            break;
        case Style::Hold:
            k->interp = Interpolation::Hold;
            k->tangent = TangentMode::Linear;
            break;
        case Style::Ease:
            k->interp = Interpolation::Bezier;
            k->tangent = TangentMode::Flat;
            break;
        case Style::Smooth:
            k->interp = Interpolation::Bezier;
            k->tangent = TangentMode::Auto;
            break;
    }
}

std::optional<Style> styleAt(const Animatable<double>& a, const Time& t) {
    const Keyframe<double>* k = a.keyAt(t);
    if (!k) return std::nullopt;
    if (k->interp == Interpolation::Hold) return Style::Hold;
    if (k->interp == Interpolation::Linear) return Style::Linear;
    return k->tangent == TangentMode::Auto ? Style::Smooth : Style::Ease;
}

bool moveKey(Animatable<double>& a, const Time& from, const Time& to) {
    const Keyframe<double>* k = a.keyAt(from);
    if (!k || (from != to && a.keyAt(to))) return false;
    Keyframe<double> moved = *k;
    a.removeKey(from);
    moved.time = to;
    a.setKey(moved);
    return true;
}

std::vector<Property> properties(Clip& c) {
    std::vector<Property> out{
        {"opacity", "Opacity", &c.opacity},
        {"transform.positionX", "Position X", &c.transform.positionX},
        {"transform.positionY", "Position Y", &c.transform.positionY},
        {"transform.scaleX", "Scale width", &c.transform.scaleX},
        {"transform.scaleY", "Scale height", &c.transform.scaleY},
        {"transform.rotation", "Rotate", &c.transform.rotation},
        {"transform.anchorX", "Anchor X", &c.transform.anchorX},
        {"transform.anchorY", "Anchor Y", &c.transform.anchorY},
        {"audio.volume", "Volume", &c.volumeDb},          // M6 (dB)
        {"audio.balance", "Balance", &c.audio.balance},   // M6 (-1 .. 1)
    };
    for (auto& m : c.masks) {
        const std::string p = "mask." + m.id + ".";
        out.push_back({p + "centerX", "Mask X", &m.centerX});
        out.push_back({p + "centerY", "Mask Y", &m.centerY});
        out.push_back({p + "width", "Mask width", &m.width});
        out.push_back({p + "height", "Mask height", &m.height});
        out.push_back({p + "rotation", "Mask rotation", &m.rotation});
        out.push_back({p + "feather", "Mask feather", &m.feather});
        out.push_back({p + "opacity", "Mask opacity", &m.opacity});
    }
    for (auto& e : c.effects) {   // M4: effect strength, parameters and effect masks
        const std::string p = "fx." + e.id + ".";
        out.push_back({p + "mix", e.type + " strength", &e.mix});
        for (auto& [name, a] : e.params) out.push_back({p + name, e.type + " " + name, &a});
        for (auto& m : e.masks) {
            const std::string q = p + "mask." + m.id + ".";
            out.push_back({q + "centerX", "Effect mask X", &m.centerX});
            out.push_back({q + "centerY", "Effect mask Y", &m.centerY});
            out.push_back({q + "width", "Effect mask width", &m.width});
            out.push_back({q + "height", "Effect mask height", &m.height});
            out.push_back({q + "rotation", "Effect mask rotation", &m.rotation});
            out.push_back({q + "feather", "Effect mask feather", &m.feather});
            out.push_back({q + "opacity", "Effect mask opacity", &m.opacity});
        }
    }
    return out;
}

std::string newEffectId(const Clip& c) {
    int next = 1;
    for (const auto& e : c.effects)
        if (e.id.size() > 2 && e.id.compare(0, 2, "fx") == 0) next = std::max(next, std::atoi(e.id.c_str() + 2) + 1);
    return "fx" + std::to_string(next);
}

Animatable<double>* property(Clip& c, const std::string& id) {
    for (auto& p : properties(c))
        if (p.id == id) return p.value;
    return nullptr;
}

const Animatable<double>* property(const Clip& c, const std::string& id) { return property(const_cast<Clip&>(c), id); }

std::vector<Time> keyTimes(const Clip& c) {
    std::vector<Time> out;
    for (auto& p : properties(const_cast<Clip&>(c)))
        for (const auto& k : p.value->keys()) out.push_back(k.time);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

Time toTimeline(const Clip& c, const Time& sourceTime) {
    if (!isRetimed(c.speed)) return c.timelineStart + (sourceTime - c.sourceIn);
    return c.timelineStart + localAtSource(c, sourceTime);
}

Time toSource(const Clip& c, const Time& timelineTime) {
    if (!isRetimed(c.speed)) return c.sourceIn + (timelineTime - c.timelineStart);
    return sourceTimeAtLocal(c, timelineTime - c.timelineStart);
}

void moveKeysAt(Clip& c, const Time& from, const Time& to) {
    if (from == to) return;
    for (auto& p : properties(c))
        if (p.value->keyAt(from) && p.value->keyAt(to))
            throw std::runtime_error(p.label + " already has a keyframe at the target time");
    for (auto& p : properties(c)) moveKey(*p.value, from, to);
}

void removeKeysAt(Clip& c, const Time& t) {
    for (auto& p : properties(c))
        if (p.value->keyAt(t)) toggleKey(*p.value, t);
}

void setStyleAt(Clip& c, const Time& t, Style s) {
    for (auto& p : properties(c)) setStyle(*p.value, t, s);
}

}  // namespace fa::keys
