// Video transitions: the outgoing clip A and the incoming clip B are both rendered as layers; a
// transition modifies their parameters as a function of progress p (0..1 over the span, linear in
// time, Stage 1 EXP-001) and decides the drawing order.
//
//   dissolve      A, then B with opacity x p                              (Filmora Dissolve, EXP-001)
//   dip_to_black  p < 0.5: only A, brightness 1 - 2p; else only B, brightness 2p - 1   ("Fade")
//   wipe          A, then B revealed by a soft linear edge travelling in `direction`  (EXP-017: linear)
//   slide         A, then B moving in from the opposite side, eased              (easeInOutQuart)
//   push          A moving out and B moving in together, eased                  (EXP-017: easeInOutQuart)
//   zoom          B (scaling 1.15 -> 1), then A zooming 1 -> 1.6 and fading out, eased
// M11 (MeerCut's own designs, built from the same layer adjustments and the wipe shapes of compositing.h):
//   dip_to_white  p < 0.5: only A, towards white by 2p; else only B, from white (1 - (2p - 1))
//   flash         a cut at p = 0.5 under a white flash peaking there (both sides, smoothstep over 0.2 .. 0.8)
//   circle_open   B revealed by a circle growing from the centre (soft edge)
//   circle_close  A kept inside a circle shrinking to the centre, B underneath
//   clock         B revealed by a hand sweeping clockwise from 12 o'clock
//   barn_door     B revealed from the centre line outwards (vertical doors for left / right)
//   diamond       B revealed by a diamond growing from the centre
//   blinds        B revealed by 10 stripes opening together (vertical for left / right)
//   diagonal      B revealed by a straight edge travelling corner to corner
//   spin          A then B turning half a turn together, shrinking to 55 % mid-way, B fading in over 0.35 .. 0.65
//   zoom_in       B growing from 25 % to full size over A and fading in quickly, eased
//   blur          A then B, both blurred (60 px at 1080p at mid-way), B fading in over 0.25 .. 0.75
//   pixelate      a cut at p = 0.5 hidden under blocks growing to 81 px at 1080p and shrinking again
#pragma once

#include "fa/render/compositing.h"

#include <string>
#include <vector>

namespace fa {

struct TransitionInfo {
    const char* id;
    const char* label;
    const char* description;
    bool usesDirection;
    bool usesSoftness;
};

const std::vector<TransitionInfo>& transitionTypes();
const TransitionInfo* findTransition(const std::string& id);

// Extra placement applied on top of a clip's own transform (canvas fractions / multipliers).
struct LayerAdjust {
    double dx = 0, dy = 0;          // added to position (fractions of the canvas)
    double scale = 1;               // multiplies scale X and Y
    double rotation = 0;            // added to the rotation (degrees, clockwise) - title animations
    double opacity = 1;             // multiplies opacity
    double brightness = 1;
    double flash = 0;               // towards white, 0..1 (LayerDesc::flash)
    double blur = 0;                // Gaussian blur after the clip's effects, px at 1080p (fa.fx.blur.gaussian radius)
    double pixelate = 0;            // block size after the clip's effects, px at 1080p (fa.fx.pixelate; < 2 = none)
    WipeParams wipe;
    bool visible = true;
};

struct TransitionLayers {
    LayerAdjust a, b;
    bool bFirst = false;            // draw B below A (zoom)
};

double easeInOutQuart(double p);
TransitionLayers evaluateTransition(const TransitionRef& t, double progress);

}  // namespace fa
