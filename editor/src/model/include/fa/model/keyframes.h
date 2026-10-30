// Keyframe editing on clip properties (used by the inspector, the timeline markers and preview handles).
//
// Behaviour (Filmora-like, Stage 1 EXP-002): a property without keyframes has one constant value;
// once it has keyframes, setting its value at the playhead creates or updates the keyframe there
// ("auto-key"). Removing the last keyframe turns the property back into a constant (the value it had
// at that moment). Times are clip SOURCE times.
#pragma once

#include "fa/model/project.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fa::keys {

enum class Style { Linear, Hold, Ease, Smooth };   // Smooth = continuous curve (auto tangents)
const char* styleLabel(Style s);

bool hasKeyAt(const Animatable<double>& a, const Time& t);
// Set the value at t: auto-key when animated, else change the constant.
void setValueAt(Animatable<double>& a, const Time& t, double v);
// Add a keyframe with the current value at t, or remove the keyframe at t.
void toggleKey(Animatable<double>& a, const Time& t);
void setStyle(Animatable<double>& a, const Time& t, Style s);
std::optional<Style> styleAt(const Animatable<double>& a, const Time& t);
// Move the keyframe at `from` to `to` (fails when there is none at `from` or one already at `to`).
bool moveKey(Animatable<double>& a, const Time& from, const Time& to);

// All animatable properties of a clip, with stable ids ("opacity", "transform.positionX",
// "mask.<maskId>.width", "fx.<effectId>.<param>", "fx.<effectId>.mix", "fx.<effectId>.mask.<maskId>.width", ...). Pointers stay valid while the clip is not modified structurally.
struct Property {
    std::string id;
    std::string label;
    Animatable<double>* value;
};
std::vector<Property> properties(Clip& clip);
// A free effect instance id for the clip ("fx1", "fx2", ...).
std::string newEffectId(const Clip& clip);
Animatable<double>* property(Clip& clip, const std::string& id);
const Animatable<double>* property(const Clip& clip, const std::string& id);

// Sorted, de-duplicated keyframe times over all properties of the clip (source time).
std::vector<Time> keyTimes(const Clip& clip);
// Timeline time of a source-time keyframe on this clip, and the source time at a timeline time - through the clip's
// speed map (M5): exact for plain, uniform and reversed clips, numeric for ramps; a freeze frame shows its one source
// instant over the whole clip (its keys sit at the clip start). Before M6 these assumed speed 1, which misplaced
// keyframe markers of retimed clips.
Time toTimeline(const Clip& c, const Time& sourceTime);
Time toSource(const Clip& c, const Time& timelineTime);

// Apply to every property that has a keyframe at t.
void moveKeysAt(Clip& clip, const Time& from, const Time& to);   // throws EditError-like std::runtime_error on clash
void removeKeysAt(Clip& clip, const Time& t);
void setStyleAt(Clip& clip, const Time& t, Style s);

}  // namespace fa::keys
