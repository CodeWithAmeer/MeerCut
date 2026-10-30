// Motion-compensated frame interpolation (M5 "Optical Flow" frame interpolation; OpenCV DIS optical flow).
//
// interpolateFrames(a, b, w) builds the picture between two consecutive source frames at weight w (0 = a, 1 = b):
//   1. dense optical flow a -> b and b -> a (OpenCV DIS) on a downscaled grey image (estimateFlow),
//   2. both frames are warped to the in-between time (a by w * flow(a->b), b by (1 - w) * flow(b->a)) and mixed,
//   3. where the two flows disagree (occlusions, motion boundaries, flow failures) the pixel falls back to the plain
//      blend of the two frames - a ghost is better than a tear (warpWithFlow).
// The flow between two frames serves every in-between time, so slow motion estimates it once per source pair
// (the renderer keeps the last few). It never fails silently: it returns false / nullptr (and leaves `out` untouched)
// if OpenCV throws or the inputs differ, and the caller then blends. This is not AI interpolation and is not
// presented as such.
#pragma once

#include "fa/core/image.h"

#include <memory>

namespace fa::vision {

enum class FlowQuality { Fast, Quality };   // flow computed at 480 or 960 pixels wide

struct FlowStats {
    double meanMotion = 0;        // mean flow length, pixels of the output picture
    double fallbackShare = 0;     // share of pixels that used the plain blend (inconsistent flow)
    double milliseconds = 0;
    double flowMilliseconds = 0;   // of which: the two optical flows and their consistency check
};

// The motion between two frames of one size (working-size flows both ways and the per-pixel trust).
struct FlowField;
std::shared_ptr<const FlowField> estimateFlow(const Image& a, const Image& b, FlowQuality quality = FlowQuality::Fast,
                                              FlowStats* stats = nullptr);
// The in-between picture at weight w from a flow estimated for these two frames.
bool warpWithFlow(const Image& a, const Image& b, const FlowField& flow, double w, Image& out, FlowStats* stats = nullptr);

bool interpolateFrames(const Image& a, const Image& b, double w, Image& out, FlowQuality quality = FlowQuality::Fast,
                       FlowStats* stats = nullptr);

}  // namespace fa::vision
