// M5 stabilisation plan (pure math; the camera motion itself is measured by fa/vision/stabilize.h).
//
//   1. Camera path: the measured frame-to-frame motion accumulated from the first analysed frame.
//   2. Smooth path: the camera path through a Gaussian whose width follows the level (Weak / Normal / Strong); it
//      keeps intended camera moves (pans) and removes the shake. Edges renormalise the kernel over existing frames.
//   3. Correction of a frame = (smooth - real) x strength: it moves the picture from where the shaky camera put it
//      onto the smooth path.
//   4. Auto Zoom: the smallest zoom (about the picture centre) that keeps the corrected picture covering the whole
//      frame on every frame, up to the Max Zoom setting (at most kMaxStabilizeZoom). If the full correction needs
//      more, the corrected path is kept inside the band that zoom allows and made as smooth as the band permits
//      (complete correction wherever it fits; a rotation that still does not fit is weakened, eased over neighbouring
//      frames). Without Auto Zoom the zoom is 1 and the uncovered borders are transparent (they show what is below).
#pragma once

#include "fa/model/project.h"

#include <optional>
#include <string>

namespace fa {

inline constexpr double kMaxStabilizeZoom = 1.25;   // upper end of the Max Zoom setting

double stabilizeSigma(StabilizeLevel level);   // Gaussian sigma in frames
const char* stabilizeLevelId(StabilizeLevel level);   // "weak" / "normal" / "strong"
std::optional<StabilizeLevel> stabilizeLevelFromId(const std::string& id);

// Fills s.corrections, s.zoom and s.limitedShare from s.motion and the settings.
void planStabilization(Stabilization& s);

// The correction of the source frame shown at sourceTime: the analysed frame floor(sourceTime x frameRate); frames
// before / after the analysed range take the first / last one. nullptr when there are no corrections.
const FrameCorrection* stabilizationAt(const Stabilization& s, const Time& sourceTime);

}  // namespace fa
