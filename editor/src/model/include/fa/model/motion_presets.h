// M9 motion presets (Filmora's Animation tab: In / Out / Combo): ready-made movements written as ordinary keyframes
// on a clip's transform and opacity, so they render everywhere (CPU / GPU, preview / export) and stay editable.
// The clip remembers which preset each slot holds (Clip::motionIn / motionOut / motionCombo); applying another or
// "none" first removes the keys that preset wrote in its time window.
#pragma once

#include "fa/model/project.h"

#include <string>
#include <vector>

namespace fa {

enum class MotionSlot { In, Out, Combo };

struct MotionPreset {
    std::string id;     // "fade", "zoom_in", "slide_left", "ken_burns", ...
    std::string name;
    MotionSlot slot = MotionSlot::In;
};

const std::vector<MotionPreset>& motionPresets();
const MotionPreset* findMotionPreset(const std::string& id, MotionSlot slot);

// The length an In / Out preset takes on a clip of `clipLength` (0.6 s, at most a third of the clip).
Time motionLength(const Time& clipLength);

// Apply (or with id "" remove) a preset in its slot. Throws ModelError for an unknown id.
void applyMotionPreset(Clip& clip, MotionSlot slot, const std::string& id);

}  // namespace fa
