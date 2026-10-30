#include "fa/model/serialize.h"

#include "fa/model/stabilize_plan.h"

#include "fa/core/app_data.h"

#include <cstdio>
#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace fa {

using nlohmann::json;

namespace {

constexpr const char* kFormatName = "fa-project";

// ---- error context ---------------------------------------------------------------------------
struct Ctx {
    std::string path;
    Ctx operator/(const std::string& k) const { return {path + "." + k}; }
    Ctx operator[](size_t i) const { return {path + "[" + std::to_string(i) + "]"}; }
    [[noreturn]] void fail(const std::string& msg) const { throw ModelError(path + ": " + msg); }
};

const json& need(const json& j, const char* key, const Ctx& c) {
    if (!j.is_object()) c.fail("expected an object");
    auto it = j.find(key);
    if (it == j.end()) (c / key).fail("missing required field");
    return *it;
}

template <typename T>
T getAs(const json& j, const Ctx& c) {
    try {
        return j.get<T>();
    } catch (const json::exception& e) {
        c.fail(std::string("wrong type: ") + e.what());
    }
}

template <typename T>
T req(const json& j, const char* key, const Ctx& c) { return getAs<T>(need(j, key, c), c / key); }

template <typename T>
T opt(const json& j, const char* key, const Ctx& c, T fallback) {
    auto it = j.find(key);
    return it == j.end() ? fallback : getAs<T>(*it, c / key);
}

Rational toRational(const json& j, const Ctx& c) {
    if (j.is_number_integer()) return Rational(j.get<int64_t>());
    if (!j.is_string()) c.fail("expected a rational string like \"1001/30000\"");
    try {
        return Rational::parse(j.get<std::string>());
    } catch (const std::exception& e) {
        c.fail(e.what());
    }
}
Rational reqRational(const json& j, const char* key, const Ctx& c) { return toRational(need(j, key, c), c / key); }

// ---- enums -----------------------------------------------------------------------------------
const char* toStr(TrackKind k) { return k == TrackKind::Video ? "video" : k == TrackKind::Audio ? "audio" : "subtitle"; }
TrackKind trackKindFrom(const std::string& s, const Ctx& c) {
    if (s == "video") return TrackKind::Video;
    if (s == "audio") return TrackKind::Audio;
    if (s == "subtitle") return TrackKind::Subtitle;
    c.fail("unknown track kind '" + s + "'");
}
const char* toStr(Interpolation i) {
    switch (i) {
        case Interpolation::Linear: return "linear";
        case Interpolation::Hold: return "hold";
        case Interpolation::Bezier: return "bezier";
    }
    return "linear";
}
Interpolation interpFrom(const std::string& s, const Ctx& c) {
    if (s == "linear") return Interpolation::Linear;
    if (s == "hold") return Interpolation::Hold;
    if (s == "bezier") return Interpolation::Bezier;
    c.fail("unknown interpolation '" + s + "'");
}
const char* toStr(TangentMode t) {
    switch (t) {
        case TangentMode::Linear: return "linear";
        case TangentMode::Flat: return "flat";
        case TangentMode::Auto: return "auto";
        case TangentMode::Manual: return "manual";
    }
    return "linear";
}
TangentMode tangentFrom(const std::string& s, const Ctx& c) {
    if (s == "linear") return TangentMode::Linear;
    if (s == "flat") return TangentMode::Flat;
    if (s == "auto") return TangentMode::Auto;
    if (s == "manual") return TangentMode::Manual;
    c.fail("unknown tangent mode '" + s + "'");
}
const char* toStr(TransitionAlignment a) {
    switch (a) {
        case TransitionAlignment::Centered: return "centered";
        case TransitionAlignment::StartsAtCut: return "startsAtCut";
        case TransitionAlignment::EndsAtCut: return "endsAtCut";
    }
    return "centered";
}
TransitionAlignment alignmentFrom(const std::string& s, const Ctx& c) {
    if (s == "centered") return TransitionAlignment::Centered;
    if (s == "startsAtCut") return TransitionAlignment::StartsAtCut;
    if (s == "endsAtCut") return TransitionAlignment::EndsAtCut;
    c.fail("unknown transition alignment '" + s + "'");
}

// ---- animatable<double> ----------------------------------------------------------------------
json animToJson(const Animatable<double>& a) {
    if (!a.isAnimated()) return a.constantValue();
    json keys = json::array();
    for (const auto& k : a.keys()) {
        json jk = {{"t", k.time.toString()}, {"v", k.value}, {"interp", toStr(k.interp)}, {"tangent", toStr(k.tangent)}};
        if (k.tangent == TangentMode::Manual) {
            jk["inSlope"] = k.inSlope;
            jk["outSlope"] = k.outSlope;
        }
        if (k.inHandle != 0.5) jk["inHandle"] = k.inHandle;
        if (k.outHandle != 0.5) jk["outHandle"] = k.outHandle;
        keys.push_back(std::move(jk));
    }
    return json{{"value", a.constantValue()}, {"keys", std::move(keys)}};
}

Animatable<double> animFromJson(const json& j, const Ctx& c) {
    if (j.is_number()) return Animatable<double>(j.get<double>());
    Animatable<double> a(opt<double>(j, "value", c, 0.0));
    const json& keys = need(j, "keys", c);
    if (!keys.is_array()) (c / "keys").fail("expected an array");
    for (size_t i = 0; i < keys.size(); ++i) {
        const json& jk = keys[i];
        const Ctx kc = (c / "keys")[i];
        Keyframe<double> k;
        k.time = reqRational(jk, "t", kc);
        k.value = req<double>(jk, "v", kc);
        k.interp = interpFrom(opt<std::string>(jk, "interp", kc, "linear"), kc / "interp");
        k.tangent = tangentFrom(opt<std::string>(jk, "tangent", kc, "linear"), kc / "tangent");
        k.inSlope = opt<double>(jk, "inSlope", kc, 0.0);
        k.outSlope = opt<double>(jk, "outSlope", kc, 0.0);
        k.inHandle = opt<double>(jk, "inHandle", kc, 0.5);
        k.outHandle = opt<double>(jk, "outHandle", kc, 0.5);
        if (k.inHandle < 0 || k.inHandle > 1 || k.outHandle < 0 || k.outHandle > 1)
            kc.fail("handle lengths must be within [0, 1]");
        for (const auto& existing : a.keys())
            if (existing.time == k.time) kc.fail("duplicate keyframe time " + k.time.toString());
        a.setKey(k);
    }
    return a;
}


// ---- M2 visual properties -------------------------------------------------------------------
const char* toStr(TransitionDirection d) {
    switch (d) {
        case TransitionDirection::Left: return "left";
        case TransitionDirection::Right: return "right";
        case TransitionDirection::Up: return "up";
        case TransitionDirection::Down: return "down";
    }
    return "left";
}
TransitionDirection directionFrom(const std::string& s, const Ctx& c) {
    if (s == "left") return TransitionDirection::Left;
    if (s == "right") return TransitionDirection::Right;
    if (s == "up") return TransitionDirection::Up;
    if (s == "down") return TransitionDirection::Down;
    c.fail("unknown transition direction '" + s + "'");
}
const char* toStr(MaskShape m) {
    switch (m) {
        case MaskShape::Rectangle: return "rectangle";
        case MaskShape::Ellipse: return "ellipse";
        case MaskShape::Polygon: return "polygon";
    }
    return "ellipse";
}
MaskShape maskShapeFrom(const std::string& s, const Ctx& c) {
    if (s == "rectangle") return MaskShape::Rectangle;
    if (s == "ellipse") return MaskShape::Ellipse;
    if (s == "polygon") return MaskShape::Polygon;
    c.fail("unknown mask shape '" + s + "'");
}
const char* toStr(MaskMode m) {
    switch (m) {
        case MaskMode::Add: return "add";
        case MaskMode::Subtract: return "subtract";
        case MaskMode::Intersect: return "intersect";
    }
    return "add";
}
MaskMode maskModeFrom(const std::string& s, const Ctx& c) {
    if (s == "add") return MaskMode::Add;
    if (s == "subtract") return MaskMode::Subtract;
    if (s == "intersect") return MaskMode::Intersect;
    c.fail("unknown mask mode '" + s + "'");
}

// Animatable written only when it differs from its default constant.
void putAnim(json& j, const char* key, const Animatable<double>& a, double def) {
    if (a.isAnimated() || a.constantValue() != def) j[key] = animToJson(a);
}
void getAnim(const json& j, const char* key, const Ctx& c, Animatable<double>& out) {
    if (j.contains(key)) out = animFromJson(j[key], c / key);
}

json transformToJson(const ClipTransform& t) {
    json j = json::object();
    putAnim(j, "positionX", t.positionX, 0.0);
    putAnim(j, "positionY", t.positionY, 0.0);
    putAnim(j, "scaleX", t.scaleX, 1.0);
    putAnim(j, "scaleY", t.scaleY, 1.0);
    putAnim(j, "rotation", t.rotation, 0.0);
    putAnim(j, "anchorX", t.anchorX, 0.0);
    putAnim(j, "anchorY", t.anchorY, 0.0);
    if (!t.uniformScale) j["uniformScale"] = false;
    if (t.flipH) j["flipH"] = true;
    if (t.flipV) j["flipV"] = true;
    if (t.cropLeft || t.cropTop || t.cropRight || t.cropBottom)
        j["crop"] = {t.cropLeft, t.cropTop, t.cropRight, t.cropBottom};
    return j;
}

ClipTransform transformFromJson(const json& j, const Ctx& c) {
    ClipTransform t;
    if (!j.is_object()) c.fail("expected an object");
    getAnim(j, "positionX", c, t.positionX);
    getAnim(j, "positionY", c, t.positionY);
    getAnim(j, "scaleX", c, t.scaleX);
    getAnim(j, "scaleY", c, t.scaleY);
    getAnim(j, "rotation", c, t.rotation);
    getAnim(j, "anchorX", c, t.anchorX);
    getAnim(j, "anchorY", c, t.anchorY);
    t.uniformScale = opt<bool>(j, "uniformScale", c, true);
    t.flipH = opt<bool>(j, "flipH", c, false);
    t.flipV = opt<bool>(j, "flipV", c, false);
    if (j.contains("crop")) {
        const json& cr = j["crop"];
        if (!cr.is_array() || cr.size() != 4) (c / "crop").fail("expected [left, top, right, bottom]");
        t.cropLeft = getAs<double>(cr[0], c / "crop");
        t.cropTop = getAs<double>(cr[1], c / "crop");
        t.cropRight = getAs<double>(cr[2], c / "crop");
        t.cropBottom = getAs<double>(cr[3], c / "crop");
    }
    return t;
}

json maskToJson(const Mask& m) {
    json j = {{"id", m.id}, {"shape", toStr(m.shape)}};
    if (!m.name.empty()) j["name"] = m.name;
    if (m.mode != MaskMode::Add) j["mode"] = toStr(m.mode);
    if (!m.enabled) j["enabled"] = false;
    if (m.inverted) j["inverted"] = true;
    putAnim(j, "centerX", m.centerX, 0.0);
    putAnim(j, "centerY", m.centerY, 0.0);
    putAnim(j, "width", m.width, 0.5);
    putAnim(j, "height", m.height, 0.5);
    putAnim(j, "rotation", m.rotation, 0.0);
    putAnim(j, "feather", m.feather, 0.0);
    putAnim(j, "opacity", m.opacity, 1.0);
    if (m.cornerRadius != 0.0) j["cornerRadius"] = m.cornerRadius;
    if (!m.points.empty()) {
        json pts = json::array();
        for (const auto& p : m.points) pts.push_back({p.x, p.y});
        j["points"] = std::move(pts);
    }
    return j;
}

Mask maskFromJson(const json& j, const Ctx& c) {
    Mask m;
    m.id = req<std::string>(j, "id", c);
    m.shape = maskShapeFrom(req<std::string>(j, "shape", c), c / "shape");
    m.name = opt<std::string>(j, "name", c, "");
    m.mode = maskModeFrom(opt<std::string>(j, "mode", c, "add"), c / "mode");
    m.enabled = opt<bool>(j, "enabled", c, true);
    m.inverted = opt<bool>(j, "inverted", c, false);
    getAnim(j, "centerX", c, m.centerX);
    getAnim(j, "centerY", c, m.centerY);
    getAnim(j, "width", c, m.width);
    getAnim(j, "height", c, m.height);
    getAnim(j, "rotation", c, m.rotation);
    getAnim(j, "feather", c, m.feather);
    getAnim(j, "opacity", c, m.opacity);
    m.cornerRadius = opt<double>(j, "cornerRadius", c, 0.0);
    if (j.contains("points")) {
        const json& pts = j["points"];
        if (!pts.is_array()) (c / "points").fail("expected an array of [x, y]");
        for (size_t i = 0; i < pts.size(); ++i) {
            if (!pts[i].is_array() || pts[i].size() != 2) ((c / "points")[i]).fail("expected [x, y]");
            m.points.push_back({getAs<double>(pts[i][0], (c / "points")[i]), getAs<double>(pts[i][1], (c / "points")[i])});
        }
    }
    return m;
}


// ---- text (M3) -------------------------------------------------------------------------------
std::string colorToStr(uint32_t c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%06X", static_cast<unsigned>(c & 0xFFFFFF));
    return buf;
}
uint32_t colorFrom(const json& j, const Ctx& c) {
    if (j.is_number_unsigned() || j.is_number_integer()) return static_cast<uint32_t>(j.get<int64_t>()) & 0xFFFFFF;
    const std::string s = getAs<std::string>(j, c);
    if (s.size() != 7 || s[0] != '#') c.fail("expected a colour \"#RRGGBB\"");
    try {
        return static_cast<uint32_t>(std::stoul(s.substr(1), nullptr, 16));
    } catch (const std::exception&) {
        c.fail("expected a colour \"#RRGGBB\"");
    }
}
const char* toStr(TextAlign a) { return a == TextAlign::Left ? "left" : a == TextAlign::Right ? "right" : "center"; }
TextAlign textAlignFrom(const std::string& s, const Ctx& c) {
    if (s == "left") return TextAlign::Left;
    if (s == "center") return TextAlign::Center;
    if (s == "right") return TextAlign::Right;
    c.fail("unknown text alignment '" + s + "'");
}
const char* toStr(TextAnchor a) { return a == TextAnchor::Bottom ? "bottom" : a == TextAnchor::Top ? "top" : "center"; }
TextAnchor textAnchorFrom(const std::string& s, const Ctx& c) {
    if (s == "center") return TextAnchor::Center;
    if (s == "bottom") return TextAnchor::Bottom;
    if (s == "top") return TextAnchor::Top;
    c.fail("unknown text anchor '" + s + "'");
}

// Only the fields that differ from `def` are written (small, readable files); reading starts from `def`.
json styleToJson(const TextStyle& s, const TextStyle& def) {
    json j = json::object();
    auto num = [&](const char* k, double v, double d) {
        if (v != d) j[k] = v;
    };
    auto flag = [&](const char* k, bool v, bool d) {
        if (v != d) j[k] = v;
    };
    auto col = [&](const char* k, uint32_t v, uint32_t d) {
        if (v != d) j[k] = colorToStr(v);
    };
    if (s.font != def.font) j["font"] = s.font;
    num("size", s.size, def.size);
    if (s.weight != def.weight) j["weight"] = s.weight;
    flag("italic", s.italic, def.italic);
    flag("underline", s.underline, def.underline);
    col("color", s.color, def.color);
    num("opacity", s.opacity, def.opacity);
    flag("gradient", s.gradient, def.gradient);
    col("gradientColor", s.gradientColor, def.gradientColor);
    num("gradientAngle", s.gradientAngle, def.gradientAngle);
    if (s.align != def.align) j["align"] = toStr(s.align);
    if (s.anchor != def.anchor) j["anchor"] = toStr(s.anchor);
    num("charSpacing", s.charSpacing, def.charSpacing);
    num("lineSpacing", s.lineSpacing, def.lineSpacing);
    num("wrapWidth", s.wrapWidth, def.wrapWidth);
    flag("outline", s.outline, def.outline);
    num("outlineWidth", s.outlineWidth, def.outlineWidth);
    col("outlineColor", s.outlineColor, def.outlineColor);
    num("outlineOpacity", s.outlineOpacity, def.outlineOpacity);
    flag("shadow", s.shadow, def.shadow);
    col("shadowColor", s.shadowColor, def.shadowColor);
    num("shadowOpacity", s.shadowOpacity, def.shadowOpacity);
    num("shadowDistance", s.shadowDistance, def.shadowDistance);
    num("shadowAngle", s.shadowAngle, def.shadowAngle);
    num("shadowBlur", s.shadowBlur, def.shadowBlur);
    flag("glow", s.glow, def.glow);
    col("glowColor", s.glowColor, def.glowColor);
    num("glowSize", s.glowSize, def.glowSize);
    num("glowOpacity", s.glowOpacity, def.glowOpacity);
    flag("background", s.background, def.background);
    col("backgroundColor", s.backgroundColor, def.backgroundColor);
    num("backgroundOpacity", s.backgroundOpacity, def.backgroundOpacity);
    num("backgroundPadding", s.backgroundPadding, def.backgroundPadding);
    num("backgroundRadius", s.backgroundRadius, def.backgroundRadius);
    return j;
}

TextStyle styleFromJson(const json& j, const Ctx& c, const TextStyle& def) {
    if (!j.is_object()) c.fail("expected an object");
    TextStyle s = def;
    s.font = opt<std::string>(j, "font", c, def.font);
    s.size = opt<double>(j, "size", c, def.size);
    s.weight = opt<int>(j, "weight", c, def.weight);
    s.italic = opt<bool>(j, "italic", c, def.italic);
    s.underline = opt<bool>(j, "underline", c, def.underline);
    auto col = [&](const char* k, uint32_t& out) {
        if (j.contains(k)) out = colorFrom(j[k], c / k);
    };
    col("color", s.color);
    s.opacity = opt<double>(j, "opacity", c, def.opacity);
    s.gradient = opt<bool>(j, "gradient", c, def.gradient);
    col("gradientColor", s.gradientColor);
    s.gradientAngle = opt<double>(j, "gradientAngle", c, def.gradientAngle);
    if (j.contains("align")) s.align = textAlignFrom(getAs<std::string>(j["align"], c / "align"), c / "align");
    if (j.contains("anchor")) s.anchor = textAnchorFrom(getAs<std::string>(j["anchor"], c / "anchor"), c / "anchor");
    s.charSpacing = opt<double>(j, "charSpacing", c, def.charSpacing);
    s.lineSpacing = opt<double>(j, "lineSpacing", c, def.lineSpacing);
    s.wrapWidth = opt<double>(j, "wrapWidth", c, def.wrapWidth);
    s.outline = opt<bool>(j, "outline", c, def.outline);
    s.outlineWidth = opt<double>(j, "outlineWidth", c, def.outlineWidth);
    col("outlineColor", s.outlineColor);
    s.outlineOpacity = opt<double>(j, "outlineOpacity", c, def.outlineOpacity);
    s.shadow = opt<bool>(j, "shadow", c, def.shadow);
    col("shadowColor", s.shadowColor);
    s.shadowOpacity = opt<double>(j, "shadowOpacity", c, def.shadowOpacity);
    s.shadowDistance = opt<double>(j, "shadowDistance", c, def.shadowDistance);
    s.shadowAngle = opt<double>(j, "shadowAngle", c, def.shadowAngle);
    s.shadowBlur = opt<double>(j, "shadowBlur", c, def.shadowBlur);
    s.glow = opt<bool>(j, "glow", c, def.glow);
    col("glowColor", s.glowColor);
    s.glowSize = opt<double>(j, "glowSize", c, def.glowSize);
    s.glowOpacity = opt<double>(j, "glowOpacity", c, def.glowOpacity);
    s.background = opt<bool>(j, "background", c, def.background);
    col("backgroundColor", s.backgroundColor);
    s.backgroundOpacity = opt<double>(j, "backgroundOpacity", c, def.backgroundOpacity);
    s.backgroundPadding = opt<double>(j, "backgroundPadding", c, def.backgroundPadding);
    s.backgroundRadius = opt<double>(j, "backgroundRadius", c, def.backgroundRadius);
    if (!(s.size > 0)) (c / "size").fail("font size must be positive");
    return s;
}

json animSlotToJson(const TextAnimation& a) {
    json j = {{"preset", a.preset}};
    if (!a.duration.isZero()) j["duration"] = a.duration.toString();
    return j;
}
TextAnimation animSlotFromJson(const json& j, const Ctx& c) {
    TextAnimation a;
    a.preset = req<std::string>(j, "preset", c);
    if (j.contains("duration")) a.duration = toRational(j["duration"], c / "duration");
    if (a.duration < Time(0)) (c / "duration").fail("negative duration");
    return a;
}

json titleToJson(const TitleData& t) {
    json j = {{"text", t.text}};
    json st = styleToJson(t.style, TextStyle{});
    if (!st.empty()) j["style"] = std::move(st);
    if (t.in.active()) j["in"] = animSlotToJson(t.in);
    if (t.loop.active()) j["loop"] = animSlotToJson(t.loop);
    if (t.out.active()) j["out"] = animSlotToJson(t.out);
    if (t.followTrackStyle) j["followTrackStyle"] = true;
    if (!t.templateId.empty()) j["template"] = t.templateId;
    return j;
}

TitleData titleFromJson(const json& j, const Ctx& c) {
    if (!j.is_object()) c.fail("expected an object");
    TitleData t;
    t.text = req<std::string>(j, "text", c);
    if (j.contains("style")) t.style = styleFromJson(j["style"], c / "style", TextStyle{});
    else t.style = TextStyle{};
    if (j.contains("in")) t.in = animSlotFromJson(j["in"], c / "in");
    if (j.contains("loop")) t.loop = animSlotFromJson(j["loop"], c / "loop");
    if (j.contains("out")) t.out = animSlotFromJson(j["out"], c / "out");
    t.followTrackStyle = opt<bool>(j, "followTrackStyle", c, false);
    t.templateId = opt<std::string>(j, "template", c, "");
    return t;
}

// ---- objects ---------------------------------------------------------------------------------
json toJson(const MediaSource& s) {
    json j = {{"id", s.id}, {"path", s.path}, {"hasVideo", s.hasVideo}, {"hasAudio", s.hasAudio}};
    if (s.duration) j["duration"] = s.duration->toString();
    if (s.frameRate) j["frameRate"] = s.frameRate->toString();
    if (s.width) j["width"] = s.width;
    if (s.height) j["height"] = s.height;
    if (!s.name.empty()) j["name"] = s.name;   // v8
    if (s.range) j["range"] = {{"start", s.range->start.toString()}, {"duration", s.range->duration.toString()}};
    return j;
}

MediaSource sourceFromJson(const json& j, const Ctx& c) {
    MediaSource s;
    s.id = req<std::string>(j, "id", c);
    s.path = req<std::string>(j, "path", c);
    s.hasVideo = opt<bool>(j, "hasVideo", c, true);
    s.hasAudio = opt<bool>(j, "hasAudio", c, false);
    if (j.contains("duration")) s.duration = toRational(j["duration"], c / "duration");
    if (j.contains("frameRate")) {
        try {
            s.frameRate = FrameRate(toRational(j["frameRate"], c / "frameRate"));
        } catch (const std::invalid_argument& e) {
            (c / "frameRate").fail(e.what());
        }
    }
    s.width = opt<int>(j, "width", c, 0);
    s.height = opt<int>(j, "height", c, 0);
    s.name = opt<std::string>(j, "name", c, std::string());
    if (j.contains("range")) {
        const Ctx rc = c / "range";
        s.range = TimeRange{reqRational(j["range"], "start", rc), reqRational(j["range"], "duration", rc)};
    }
    return s;
}

json toJson(const Clip& cl) {
    json j = {{"id", cl.id},
              {"start", cl.timelineStart.toString()},
              {"in", cl.sourceIn.toString()},
              {"duration", cl.duration.toString()}};
    if (!cl.isGenerated() || !cl.sourceId.empty()) j["source"] = cl.sourceId;
    if (cl.title) j["title"] = titleToJson(*cl.title);
    if (cl.sticker) {   // v10
        json st = {{"id", cl.sticker->id}};
        if (cl.sticker->color) st["color"] = cl.sticker->color;
        if (!cl.sticker->text.empty()) st["text"] = cl.sticker->text;
        j["sticker"] = std::move(st);
    }
    if (cl.adjustment) j["adjustment"] = true;
    if (!cl.name.empty()) j["name"] = cl.name;
    if (!cl.enabled) j["enabled"] = false;
    if (cl.colorMark != 0) j["colorMark"] = cl.colorMark;   // v10
    if (cl.muted) j["muted"] = true;                         // v10
    if (!cl.motionIn.empty() || !cl.motionOut.empty() || !cl.motionCombo.empty()) {   // v10
        json m = json::object();
        if (!cl.motionIn.empty()) m["in"] = cl.motionIn;
        if (!cl.motionOut.empty()) m["out"] = cl.motionOut;
        if (!cl.motionCombo.empty()) m["combo"] = cl.motionCombo;
        j["motion"] = std::move(m);
    }
    if (cl.opacity.isAnimated() || cl.opacity.constantValue() != 1.0) j["opacity"] = animToJson(cl.opacity);
    if (!cl.effects.empty()) {
        json fx = json::array();
        for (const auto& e : cl.effects) {
            json params = json::object();
            for (const auto& [name, a] : e.params) params[name] = animToJson(a);
            json je = {{"id", e.id}, {"type", e.type}, {"enabled", e.enabled}, {"params", std::move(params)}};
            if (e.version != 1) je["version"] = e.version;
            if (e.mix.isAnimated() || e.mix.constantValue() != 1.0) je["mix"] = animToJson(e.mix);
            if (!e.strings.empty()) je["strings"] = e.strings;
            if (!e.curves.empty()) {
                json cs = json::object();
                for (const auto& [name, pts] : e.curves) {
                    json arr = json::array();
                    for (const Vec2& v : pts) arr.push_back(json::array({v.x, v.y}));
                    cs[name] = std::move(arr);
                }
                je["curves"] = std::move(cs);
            }
            if (!e.masks.empty()) {
                json ms = json::array();
                for (const auto& m : e.masks) ms.push_back(maskToJson(m));
                je["masks"] = std::move(ms);
            }
            fx.push_back(std::move(je));
        }
        j["effects"] = std::move(fx);
    }
    if (cl.outTransition) {
        json t = {{"type", cl.outTransition->type},
                  {"duration", cl.outTransition->duration.toString()},
                  {"alignment", toStr(cl.outTransition->alignment)}};
        if (cl.outTransition->direction != TransitionDirection::Left) t["direction"] = toStr(cl.outTransition->direction);
        if (cl.outTransition->softness != 0.1) t["softness"] = cl.outTransition->softness;
        j["outTransition"] = std::move(t);
    }
    if (!cl.transform.isDefault()) j["transform"] = transformToJson(cl.transform);
    if (cl.blend != BlendMode::Normal) j["blend"] = blendModeId(cl.blend);
    if (!cl.masks.empty()) {
        json ms = json::array();
        for (const auto& m : cl.masks) ms.push_back(maskToJson(m));
        j["masks"] = std::move(ms);
    }
    if (!(cl.speed == ClipSpeed{})) {   // M5
        const ClipSpeed& sp = cl.speed;
        json js = json::object();
        if (sp.speed != Rational(1)) js["speed"] = sp.speed.toString();
        if (sp.reverse) js["reverse"] = true;
        if (sp.freeze) js["freeze"] = true;
        if (!sp.ramp.empty()) {
            json keys = json::array();
            for (const SpeedKey& k : sp.ramp) keys.push_back({{"source", k.source.toString()}, {"speed", k.speed}});
            js["ramp"] = std::move(keys);
        }
        if (!sp.rampPreset.empty()) js["rampPreset"] = sp.rampPreset;
        if (!sp.maintainPitch) js["maintainPitch"] = false;
        if (sp.interpolation != FrameInterpolation::Sampling)
            js["interpolation"] = sp.interpolation == FrameInterpolation::Blending             ? "blending"
                                  : sp.interpolation == FrameInterpolation::OpticalFlow        ? "opticalFlow"
                                  : sp.interpolation == FrameInterpolation::OpticalFlowQuality ? "opticalFlowQuality"
                                                                                               : "neural";   // v9
        j["speed"] = std::move(js);
    }
    if (cl.tracking) {   // M5: compact rows [frame, x, y, scale, confidence, lost]
        const MotionTrack& t = *cl.tracking;
        json rows = json::array();
        for (const TrackingSample& s : t.samples)
            rows.push_back(json::array({s.frame, s.x, s.y, s.scale, s.confidence, s.lost ? 1 : 0}));
        j["tracking"] = {{"box", json::array({t.boxX, t.boxY, t.boxW, t.boxH})}, {"firstFrame", t.firstFrame},
                         {"media", t.media}, {"samples", std::move(rows)}};
    }
    if (cl.stabilization) {   // M5: settings + compact motion rows [dx, dy, da, valid]; the plan is derived on load
        const Stabilization& st = *cl.stabilization;
        json rows = json::array();
        for (const CameraMotion& m : st.motion) rows.push_back(json::array({m.dx, m.dy, m.da, m.valid ? 1 : 0}));
        j["stabilization"] = {{"level", stabilizeLevelId(st.level)}, {"strength", st.strength}, {"autoZoom", st.autoZoom}, {"maxZoom", st.maxZoom},
                              {"firstFrame", st.firstFrame}, {"frameRate", st.frameRate.toString()},
                              {"width", st.width}, {"height", st.height}, {"media", st.media}, {"motion", std::move(rows)}};
    }
    if (!cl.linkId.empty()) j["link"] = cl.linkId;
    putAnim(j, "volumeDb", cl.volumeDb, 0.0);   // a number when constant (as before M6), keyframes otherwise
    if (!cl.audio.isDefault()) {                  // M6
        const ClipAudio& a = cl.audio;
        json ja = json::object();
        if (a.fadeIn != Time(0)) ja["fadeIn"] = a.fadeIn.toString();
        if (a.fadeOut != Time(0)) ja["fadeOut"] = a.fadeOut.toString();
        putAnim(ja, "balance", a.balance, 0.0);
        if (a.eqEnabled || !a.eqPreset.empty() || a.eqGainsDb != std::array<double, kAudioEqBands>{}) {
            json eq = {{"enabled", a.eqEnabled}, {"gainsDb", a.eqGainsDb}};
            if (!a.eqPreset.empty()) eq["preset"] = a.eqPreset;
            ja["eq"] = std::move(eq);
        }
        if (a.cleanup != AudioCleanup{}) {
            const AudioCleanup& k = a.cleanup;
            json jc = json::object();
            if (k.denoise != 0) jc["denoise"] = k.denoise;
            if (k.humHz != 0) jc["humHz"] = k.humHz;
            if (k.humHarmonics != 4) jc["humHarmonics"] = k.humHarmonics;
            if (k.highPassHz != 0) jc["highPassHz"] = k.highPassHz;
            if (k.lowPassHz != 0) jc["lowPassHz"] = k.lowPassHz;
            if (k.normalize) jc["normalize"] = true;
            if (k.targetLufs != -16.0) jc["targetLufs"] = k.targetLufs;
            if (k.measuredLufs) jc["measuredLufs"] = *k.measuredLufs;
            ja["cleanup"] = std::move(jc);
        }
        if (a.ducking != AudioDucking{}) {
            const AudioDucking& d = a.ducking;
            ja["ducking"] = {{"enabled", d.enabled},     {"targets", d.targets},   {"amountDb", d.amountDb},
                             {"thresholdDb", d.thresholdDb}, {"attackMs", d.attackMs}, {"holdMs", d.holdMs},
                             {"releaseMs", d.releaseMs}};
        }
        j["audio"] = std::move(ja);
    }
    if (!cl.aiCutout.isDefault()) {   // M7
        const AiCutout& a = cl.aiCutout;
        json jc = {{"enabled", a.enabled}, {"model", a.model}, {"quality", a.quality}};
        if (!a.modelSha.empty()) jc["modelSha256"] = a.modelSha;
        jc["threshold"] = a.threshold;
        jc["edge"] = a.edge;
        jc["feather"] = a.feather;
        jc["spill"] = a.spill;
        jc["smoothing"] = a.smoothing;
        if (a.background) {
            char hexc[8];
            std::snprintf(hexc, sizeof hexc, "#%02X%02X%02X", a.background->r, a.background->g, a.background->b);
            jc["background"] = hexc;
        }
        j["aiCutout"] = std::move(jc);
    }
    if (!cl.upscale.isDefault()) {   // M7 (v9)
        const ClipUpscale& u = cl.upscale;
        j["upscale"] = {{"mode", u.mode == UpscaleMode::Ai ? "ai" : u.mode == UpscaleMode::Lanczos ? "lanczos" : "off"},
                        {"scale", u.scale},
                        {"model", u.model},
                        {"deflicker", u.deflicker}};
    }
    return j;
}

Clip clipFromJson(const json& j, const Ctx& c) {
    Clip cl;
    cl.id = req<std::string>(j, "id", c);
    cl.adjustment = opt<bool>(j, "adjustment", c, false);
    if (j.contains("title") || cl.adjustment || j.contains("sticker")) {
        if (j.contains("title")) cl.title = titleFromJson(j["title"], c / "title");
        if (j.contains("sticker")) {
            const json& st = j["sticker"];
            const Ctx sc = c / "sticker";
            if (!st.is_object()) sc.fail("expected an object");
            StickerData d;
            d.id = req<std::string>(st, "id", sc);
            d.color = static_cast<uint32_t>(opt<double>(st, "color", sc, 0.0));
            d.text = opt<std::string>(st, "text", sc, "");
            cl.sticker = std::move(d);
        }
        cl.sourceId = opt<std::string>(j, "source", c, "");
    } else {
        cl.sourceId = req<std::string>(j, "source", c);
    }
    cl.name = opt<std::string>(j, "name", c, "");
    cl.timelineStart = reqRational(j, "start", c);
    cl.sourceIn = reqRational(j, "in", c);
    cl.duration = reqRational(j, "duration", c);
    cl.enabled = opt<bool>(j, "enabled", c, true);
    cl.colorMark = opt<int>(j, "colorMark", c, 0);
    cl.muted = opt<bool>(j, "muted", c, false);
    if (j.contains("motion")) {
        const json& m = j["motion"];
        const Ctx mc = c / "motion";
        if (!m.is_object()) mc.fail("expected an object");
        cl.motionIn = opt<std::string>(m, "in", mc, "");
        cl.motionOut = opt<std::string>(m, "out", mc, "");
        cl.motionCombo = opt<std::string>(m, "combo", mc, "");
    }
    if (j.contains("opacity")) cl.opacity = animFromJson(j["opacity"], c / "opacity");
    if (j.contains("effects")) {
        const json& fx = j["effects"];
        if (!fx.is_array()) (c / "effects").fail("expected an array");
        for (size_t i = 0; i < fx.size(); ++i) {
            const Ctx ec = (c / "effects")[i];
            EffectRef e;
            e.id = opt<std::string>(fx[i], "id", ec, "");
            if (e.id.empty()) e.id = "fx" + std::to_string(i + 1);   // files before v4 had no effect ids
            e.type = req<std::string>(fx[i], "type", ec);
            e.version = opt<int>(fx[i], "version", ec, 1);
            e.enabled = opt<bool>(fx[i], "enabled", ec, true);
            if (fx[i].contains("mix")) e.mix = animFromJson(fx[i]["mix"], ec / "mix");
            if (fx[i].contains("params")) {
                const json& ps = fx[i]["params"];
                if (!ps.is_object()) (ec / "params").fail("expected an object");
                for (auto it = ps.begin(); it != ps.end(); ++it)
                    e.params[it.key()] = animFromJson(it.value(), ec / "params" / it.key());
            }
            if (fx[i].contains("strings")) {
                const json& ss = fx[i]["strings"];
                if (!ss.is_object()) (ec / "strings").fail("expected an object");
                for (auto it = ss.begin(); it != ss.end(); ++it)
                    e.strings[it.key()] = getAs<std::string>(it.value(), ec / "strings" / it.key());
            }
            if (fx[i].contains("curves")) {
                const json& cs = fx[i]["curves"];
                if (!cs.is_object()) (ec / "curves").fail("expected an object");
                for (auto it = cs.begin(); it != cs.end(); ++it) {
                    const Ctx cc = ec / "curves" / it.key();
                    if (!it.value().is_array()) cc.fail("expected an array of [x, y]");
                    std::vector<Vec2> pts;
                    for (size_t k = 0; k < it.value().size(); ++k) {
                        const json& pt = it.value()[k];
                        if (!pt.is_array() || pt.size() != 2 || !pt[0].is_number() || !pt[1].is_number())
                            cc[k].fail("expected [x, y]");
                        pts.push_back({pt[0].get<double>(), pt[1].get<double>()});
                    }
                    e.curves[it.key()] = std::move(pts);
                }
            }
            if (fx[i].contains("masks")) {
                const json& ms = fx[i]["masks"];
                if (!ms.is_array()) (ec / "masks").fail("expected an array");
                for (size_t k = 0; k < ms.size(); ++k) e.masks.push_back(maskFromJson(ms[k], (ec / "masks")[k]));
            }
            cl.effects.push_back(std::move(e));
        }
    }
    if (j.contains("outTransition")) {
        const json& t = j["outTransition"];
        const Ctx tc = c / "outTransition";
        TransitionRef tr;
        tr.type = req<std::string>(t, "type", tc);
        tr.duration = reqRational(t, "duration", tc);
        tr.alignment = alignmentFrom(opt<std::string>(t, "alignment", tc, "centered"), tc / "alignment");
        tr.direction = directionFrom(opt<std::string>(t, "direction", tc, "left"), tc / "direction");
        tr.softness = opt<double>(t, "softness", tc, 0.1);
        cl.outTransition = tr;
    }
    if (j.contains("speed")) {   // M5
        const json& js = j["speed"];
        const Ctx sc = c / "speed";
        if (!js.is_object()) sc.fail("expected an object");
        ClipSpeed& sp = cl.speed;
        if (js.contains("speed")) sp.speed = toRational(js["speed"], sc / "speed");
        if (sp.speed <= Rational(0)) (sc / "speed").fail("speed must be positive");
        sp.reverse = opt<bool>(js, "reverse", sc, false);
        sp.freeze = opt<bool>(js, "freeze", sc, false);
        if (js.contains("ramp")) {
            const json& keys = js["ramp"];
            if (!keys.is_array()) (sc / "ramp").fail("expected an array");
            for (size_t k = 0; k < keys.size(); ++k) {
                const Ctx kc = (sc / "ramp")[k];
                SpeedKey key;
                key.source = reqRational(keys[k], "source", kc);
                key.speed = req<double>(keys[k], "speed", kc);
                if (!(key.speed > 0)) (kc / "speed").fail("speed must be positive");
                sp.ramp.push_back(key);
            }
        }
        sp.rampPreset = opt<std::string>(js, "rampPreset", sc, "");
        sp.maintainPitch = opt<bool>(js, "maintainPitch", sc, true);
        const std::string fi = opt<std::string>(js, "interpolation", sc, "sampling");
        if (fi == "sampling") sp.interpolation = FrameInterpolation::Sampling;
        else if (fi == "blending") sp.interpolation = FrameInterpolation::Blending;
        else if (fi == "opticalFlow") sp.interpolation = FrameInterpolation::OpticalFlow;
        else if (fi == "opticalFlowQuality") sp.interpolation = FrameInterpolation::OpticalFlowQuality;
        else if (fi == "neural") sp.interpolation = FrameInterpolation::Neural;
        else (sc / "interpolation").fail("expected sampling, blending, opticalFlow, opticalFlowQuality or neural");
    }
    if (j.contains("tracking")) {   // M5
        const json& jt = j["tracking"];
        const Ctx tc = c / "tracking";
        MotionTrack t;
        const json& box = need(jt, "box", tc);
        if (!box.is_array() || box.size() != 4) (tc / "box").fail("expected [x, y, w, h]");
        t.boxX = getAs<double>(box[0], (tc / "box")[0]);
        t.boxY = getAs<double>(box[1], (tc / "box")[1]);
        t.boxW = getAs<double>(box[2], (tc / "box")[2]);
        t.boxH = getAs<double>(box[3], (tc / "box")[3]);
        if (!(t.boxW > 0 && t.boxW <= 1 && t.boxH > 0 && t.boxH <= 1)) (tc / "box").fail("size must be in (0, 1]");
        t.firstFrame = opt<int64_t>(jt, "firstFrame", tc, 0);
        t.media = opt<std::string>(jt, "media", tc, "");
        const json& rows = need(jt, "samples", tc);
        if (!rows.is_array()) (tc / "samples").fail("expected an array");
        for (size_t i = 0; i < rows.size(); ++i) {
            const json& r = rows[i];
            const Ctx rc = (tc / "samples")[i];
            if (!r.is_array() || r.size() != 6) rc.fail("expected [frame, x, y, scale, confidence, lost]");
            TrackingSample s{getAs<int64_t>(r[0], rc[0]), getAs<double>(r[1], rc[1]), getAs<double>(r[2], rc[2]),
                          getAs<double>(r[3], rc[3]),  getAs<double>(r[4], rc[4]), getAs<int>(r[5], rc[5]) != 0};
            if (!t.samples.empty() && s.frame <= t.samples.back().frame) rc.fail("frames must increase");
            if (!(s.scale > 0) || !(s.confidence >= 0 && s.confidence <= 1)) rc.fail("scale must be > 0, confidence in [0, 1]");
            t.samples.push_back(s);
        }
        cl.tracking = std::move(t);
    }
    if (j.contains("stabilization")) {   // M5
        const json& js = j["stabilization"];
        const Ctx sc = c / "stabilization";
        Stabilization st;
        const auto level = stabilizeLevelFromId(opt<std::string>(js, "level", sc, "normal"));
        if (!level) (sc / "level").fail("expected weak, normal or strong");
        st.level = *level;
        st.strength = opt<double>(js, "strength", sc, 1.0);
        if (!(st.strength >= 0 && st.strength <= 1)) (sc / "strength").fail("must be in [0, 1]");
        st.autoZoom = opt<bool>(js, "autoZoom", sc, true);
        st.maxZoom = opt<double>(js, "maxZoom", sc, 1.25);   // files from before the setting: the old fixed limit
        if (!(st.maxZoom >= 1.0 && st.maxZoom <= 1.25)) (sc / "maxZoom").fail("must be in [1, 1.25]");
        st.firstFrame = opt<int64_t>(js, "firstFrame", sc, 0);
        st.frameRate = reqRational(js, "frameRate", sc);
        if (!(st.frameRate > Rational(0))) (sc / "frameRate").fail("must be positive");
        st.width = req<int>(js, "width", sc);
        st.height = req<int>(js, "height", sc);
        if (st.width <= 0 || st.height <= 0) (sc / "width").fail("picture size must be positive");
        st.media = opt<std::string>(js, "media", sc, "");
        const json& rows = need(js, "motion", sc);
        if (!rows.is_array()) (sc / "motion").fail("expected an array");
        for (size_t i = 0; i < rows.size(); ++i) {
            const json& r = rows[i];
            const Ctx rc = (sc / "motion")[i];
            if (!r.is_array() || r.size() != 4) rc.fail("expected [dx, dy, da, valid]");
            st.motion.push_back({getAs<double>(r[0], rc[0]), getAs<double>(r[1], rc[1]), getAs<double>(r[2], rc[2]),
                                 getAs<int>(r[3], rc[3]) != 0});
        }
        planStabilization(st);
        cl.stabilization = std::move(st);
    }
    cl.linkId = opt<std::string>(j, "link", c, "");
    if (j.contains("transform")) cl.transform = transformFromJson(j["transform"], c / "transform");
    if (j.contains("blend")) {
        const auto b = blendModeFromId(getAs<std::string>(j["blend"], c / "blend"));
        if (!b) (c / "blend").fail("unknown blend mode");
        cl.blend = *b;
    }
    if (j.contains("masks")) {
        const json& ms = j["masks"];
        if (!ms.is_array()) (c / "masks").fail("expected an array");
        for (size_t i = 0; i < ms.size(); ++i) cl.masks.push_back(maskFromJson(ms[i], (c / "masks")[i]));
    }
    if (j.contains("volumeDb")) cl.volumeDb = animFromJson(j["volumeDb"], c / "volumeDb");   // number (v1-v5) or keyframes
    if (j.contains("audio")) {   // M6
        const json& ja = j["audio"];
        const Ctx ac = c / "audio";
        if (!ja.is_object()) ac.fail("expected an object");
        ClipAudio& a = cl.audio;
        if (ja.contains("fadeIn")) a.fadeIn = reqRational(ja, "fadeIn", ac);
        if (ja.contains("fadeOut")) a.fadeOut = reqRational(ja, "fadeOut", ac);
        if (ja.contains("balance")) a.balance = animFromJson(ja["balance"], ac / "balance");
        if (ja.contains("eq")) {
            const json& eq = ja["eq"];
            const Ctx ec = ac / "eq";
            a.eqEnabled = opt<bool>(eq, "enabled", ec, false);
            a.eqPreset = opt<std::string>(eq, "preset", ec, "");
            if (eq.contains("gainsDb")) {
                const json& g = eq["gainsDb"];
                if (!g.is_array() || g.size() != kAudioEqBands)
                    (ec / "gainsDb").fail("expected " + std::to_string(kAudioEqBands) + " band gains");
                for (size_t i = 0; i < kAudioEqBands; ++i) a.eqGainsDb[i] = getAs<double>(g[i], (ec / "gainsDb")[i]);
            }
        }
        if (ja.contains("cleanup")) {
            const json& jc = ja["cleanup"];
            const Ctx cc = ac / "cleanup";
            AudioCleanup& k = a.cleanup;
            k.denoise = opt<double>(jc, "denoise", cc, 0.0);
            k.humHz = opt<double>(jc, "humHz", cc, 0.0);
            k.humHarmonics = opt<int>(jc, "humHarmonics", cc, 4);
            k.highPassHz = opt<double>(jc, "highPassHz", cc, 0.0);
            k.lowPassHz = opt<double>(jc, "lowPassHz", cc, 0.0);
            k.normalize = opt<bool>(jc, "normalize", cc, false);
            k.targetLufs = opt<double>(jc, "targetLufs", cc, -16.0);
            if (jc.contains("measuredLufs")) k.measuredLufs = getAs<double>(jc["measuredLufs"], cc / "measuredLufs");
        }
        if (ja.contains("ducking")) {
            const json& jd = ja["ducking"];
            const Ctx dc = ac / "ducking";
            AudioDucking& d = a.ducking;
            d.enabled = opt<bool>(jd, "enabled", dc, false);
            if (jd.contains("targets")) {
                const json& t = jd["targets"];
                if (!t.is_array()) (dc / "targets").fail("expected an array of track ids");
                for (size_t i = 0; i < t.size(); ++i) d.targets.push_back(getAs<std::string>(t[i], (dc / "targets")[i]));
            }
            d.amountDb = opt<double>(jd, "amountDb", dc, -12.0);
            d.thresholdDb = opt<double>(jd, "thresholdDb", dc, -40.0);
            d.attackMs = opt<double>(jd, "attackMs", dc, 250.0);
            d.holdMs = opt<double>(jd, "holdMs", dc, 300.0);
            d.releaseMs = opt<double>(jd, "releaseMs", dc, 500.0);
        }
    }
    if (j.contains("aiCutout")) {   // M7
        const json& jc = j["aiCutout"];
        const Ctx cc = c / "aiCutout";
        if (!jc.is_object()) cc.fail("expected an object");
        AiCutout& a = cl.aiCutout;
        a.enabled = opt<bool>(jc, "enabled", cc, false);
        a.model = opt<std::string>(jc, "model", cc, "rvm-mobilenetv3");
        a.quality = opt<int>(jc, "quality", cc, 1);
        a.modelSha = opt<std::string>(jc, "modelSha256", cc, "");
        a.threshold = opt<double>(jc, "threshold", cc, 0.0);
        a.edge = opt<double>(jc, "edge", cc, 0.0);
        a.feather = opt<double>(jc, "feather", cc, 0.0);
        a.spill = opt<double>(jc, "spill", cc, 0.5);
        a.smoothing = opt<double>(jc, "smoothing", cc, 0.5);
        if (jc.contains("background")) {
            const std::string h = getAs<std::string>(jc["background"], cc / "background");
            unsigned r = 0, g = 0, b = 0;
            if (h.size() != 7 || h[0] != '#' || std::sscanf(h.c_str() + 1, "%02x%02x%02x", &r, &g, &b) != 3)
                (cc / "background").fail("expected a colour #RRGGBB");
            a.background = Rgba8{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b), 255};
        }
    }
    if (j.contains("upscale")) {   // M7 (v9)
        const json& ju = j["upscale"];
        const Ctx uc = c / "upscale";
        if (!ju.is_object()) uc.fail("expected an object");
        const std::string mode = opt<std::string>(ju, "mode", uc, "off");
        if (mode == "ai") cl.upscale.mode = UpscaleMode::Ai;
        else if (mode == "lanczos") cl.upscale.mode = UpscaleMode::Lanczos;
        else if (mode == "off") cl.upscale.mode = UpscaleMode::Off;
        else (uc / "mode").fail("expected off, lanczos or ai");
        cl.upscale.scale = opt<int>(ju, "scale", uc, 2);
        cl.upscale.model = opt<std::string>(ju, "model", uc, "general");
        cl.upscale.deflicker = opt<double>(ju, "deflicker", uc, 0.5);
    }
    return cl;
}

json toJson(const Track& t) {
    json clips = json::array();
    for (const auto& c : t.clips) clips.push_back(toJson(c));
    json j = {{"id", t.id}, {"kind", toStr(t.kind)}, {"clips", std::move(clips)}};
    if (!t.name.empty()) j["name"] = t.name;
    if (t.hidden) j["hidden"] = true;
    if (t.muted) j["muted"] = true;
    if (t.solo) j["solo"] = true;
    if (t.locked) j["locked"] = true;
    if (t.volumeDb != 0.0) j["volumeDb"] = t.volumeDb;
    if (t.kind == TrackKind::Subtitle) {
        json st = styleToJson(t.subtitleStyle, Track::defaultSubtitleStyle());
        if (!st.empty()) j["subtitleStyle"] = std::move(st);
        if (t.subtitleY != 0.92) j["subtitleY"] = t.subtitleY;
    }
    return j;
}

Track trackFromJson(const json& j, const Ctx& c) {
    Track t;
    t.id = req<std::string>(j, "id", c);
    t.kind = trackKindFrom(req<std::string>(j, "kind", c), c / "kind");
    t.name = opt<std::string>(j, "name", c, "");
    t.hidden = opt<bool>(j, "hidden", c, false);
    t.muted = opt<bool>(j, "muted", c, false);
    t.solo = opt<bool>(j, "solo", c, false);
    t.locked = opt<bool>(j, "locked", c, false);
    t.volumeDb = opt<double>(j, "volumeDb", c, 0.0);
    if (j.contains("subtitleStyle"))
        t.subtitleStyle = styleFromJson(j["subtitleStyle"], c / "subtitleStyle", Track::defaultSubtitleStyle());
    t.subtitleY = opt<double>(j, "subtitleY", c, 0.92);
    const json& clips = need(j, "clips", c);
    if (!clips.is_array()) (c / "clips").fail("expected an array");
    for (size_t i = 0; i < clips.size(); ++i) t.clips.push_back(clipFromJson(clips[i], (c / "clips")[i]));
    return t;
}

json toJson(const Sequence& s) {
    json tracks = json::array();
    for (const auto& t : s.tracks) tracks.push_back(toJson(t));
    json j = {{"id", s.id},
              {"name", s.name},
              {"width", s.width},
              {"height", s.height},
              {"frameRate", s.frameRate.toString()},
              {"sampleRate", s.sampleRate},
              {"tracks", std::move(tracks)}};
    if (s.limiter || s.limiterCeilingDb != -1.0) j["limiter"] = {{"enabled", s.limiter}, {"ceilingDb", s.limiterCeilingDb}};
    if (s.markIn || s.markOut) {   // M6 In / Out
        json m = json::object();
        if (s.markIn) m["in"] = s.markIn->toString();
        if (s.markOut) m["out"] = s.markOut->toString();
        j["marks"] = std::move(m);
    }
    return j;
}

Sequence sequenceFromJson(const json& j, const Ctx& c) {
    Sequence s;
    s.id = req<std::string>(j, "id", c);
    s.name = opt<std::string>(j, "name", c, "");
    s.width = req<int>(j, "width", c);
    s.height = req<int>(j, "height", c);
    try {
        s.frameRate = FrameRate(reqRational(j, "frameRate", c));
    } catch (const std::invalid_argument& e) {
        (c / "frameRate").fail(e.what());
    }
    s.sampleRate = opt<int>(j, "sampleRate", c, 48000);
    if (j.contains("limiter")) {   // M6 master limiter
        const json& jl = j["limiter"];
        s.limiter = opt<bool>(jl, "enabled", c / "limiter", false);
        s.limiterCeilingDb = opt<double>(jl, "ceilingDb", c / "limiter", -1.0);
    }
    if (j.contains("marks")) {
        const json& jm = j["marks"];
        if (jm.contains("in")) s.markIn = reqRational(jm, "in", c / "marks");
        if (jm.contains("out")) s.markOut = reqRational(jm, "out", c / "marks");
    }
    const json& tracks = need(j, "tracks", c);
    if (!tracks.is_array()) (c / "tracks").fail("expected an array");
    for (size_t i = 0; i < tracks.size(); ++i) s.tracks.push_back(trackFromJson(tracks[i], (c / "tracks")[i]));
    return s;
}

}  // namespace

std::string projectToJsonString(const Project& p, int indent) {
    json sources = json::array();
    for (const auto& s : p.sources) sources.push_back(toJson(s));
    json seqs = json::array();
    for (const auto& s : p.sequences) seqs.push_back(toJson(s));
    const json j = {{"format", kFormatName},
                    {"version", Project::kFormatVersion},
                    {"name", p.name},
                    {"nextId", p.nextId},
                    {"sources", std::move(sources)},
                    {"sequences", std::move(seqs)}};
    return j.dump(indent) + "\n";
}

Project projectFromJsonString(const std::string& text) {
    json j;
    try {
        j = json::parse(text);
    } catch (const json::parse_error& e) {
        throw ModelError(std::string("project file is not valid JSON: ") + e.what());
    }
    const Ctx c{"$"};
    if (req<std::string>(j, "format", c) != kFormatName) (c / "format").fail("not an fa-project file");
    const int version = req<int>(j, "version", c);
    if (version < 1 || version > Project::kFormatVersion)
        (c / "version").fail("unsupported project version " + std::to_string(version) + " (this build reads 1.." +
                             std::to_string(Project::kFormatVersion) + ")");
    // Migration hook: when kFormatVersion grows, upgrade older JSON here before parsing.
    Project p;
    p.name = opt<std::string>(j, "name", c, "");
    p.nextId = opt<uint64_t>(j, "nextId", c, 1);
    const json& sources = need(j, "sources", c);
    if (!sources.is_array()) (c / "sources").fail("expected an array");
    for (size_t i = 0; i < sources.size(); ++i) p.sources.push_back(sourceFromJson(sources[i], (c / "sources")[i]));
    const json& seqs = need(j, "sequences", c);
    if (!seqs.is_array()) (c / "sequences").fail("expected an array");
    for (size_t i = 0; i < seqs.size(); ++i) p.sequences.push_back(sequenceFromJson(seqs[i], (c / "sequences")[i]));
    return p;
}

void saveProject(const Project& p, const std::filesystem::path& file) {
    const std::string text = projectToJsonString(p);
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
        if (!os) throw ModelError("cannot write " + tmp.string());
        os << text;
        if (!os.flush()) throw ModelError("write failed: " + tmp.string());
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) throw ModelError("cannot replace " + file.string() + ": " + ec.message());
}

Project loadProject(const std::filesystem::path& file) {
    std::ifstream is(file, std::ios::binary);
    if (!is) throw ModelError("cannot open project file " + file.string());
    std::ostringstream ss;
    ss << is.rdbuf();
    Project p;
    try {
        p = projectFromJsonString(ss.str());
    } catch (const ModelError& e) {
        throw ModelError(file.string() + ": " + e.what());
    }
    remapRenamedMediaPaths(p, localAppData());
    return p;
}

int remapRenamedMediaPaths(Project& project, const std::filesystem::path& localRoot) {
    int n = 0;
    for (auto& s : project.sources) {
        const std::filesystem::path old(std::u8string(s.path.begin(), s.path.end()));
        if (!old.is_absolute()) continue;
        const std::filesystem::path now = renamedLegacyPath(old, localRoot);
        if (now == old) continue;
        const std::u8string u = now.generic_u8string();
        s.path.assign(u.begin(), u.end());
        ++n;
    }
    return n;
}

std::filesystem::path resolveMediaPath(const MediaSource& src, const std::filesystem::path& projectFile) {
    const std::filesystem::path p(std::u8string(src.path.begin(), src.path.end()));   // paths are UTF-8
    if (p.is_absolute()) return p.lexically_normal();
    const auto base = projectFile.has_parent_path() ? projectFile.parent_path() : std::filesystem::current_path();
    return (base / p).lexically_normal();
}

std::string clipToJsonString(const Clip& clip, int indent) { return toJson(clip).dump(indent); }

Clip clipFromJsonString(const std::string& text) {
    json j;
    try {
        j = json::parse(text);
    } catch (const json::parse_error& e) {
        throw ModelError(std::string("clip is not valid JSON: ") + e.what());
    }
    return clipFromJson(j, Ctx{"$"});
}

}  // namespace fa
