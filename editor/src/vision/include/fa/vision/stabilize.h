// Video stabilisation analysis (M5, OpenCV; independently written logic).
//
//   1. Camera motion between consecutive frames: corner features spread over the picture, tracked with pyramidal
//      Lucas-Kanade (forward-backward checked), fitted with a RANSAC similarity (translation, rotation, scale).
//   2. The smoothing of the camera path, the corrections and the zoom are planned in the model
//      (fa/model/stabilize_plan.h), so the settings can change without a new analysis; the renderer applies a
//      frame's correction as part of the clip's placement (CPU and GPU alike).
#pragma once

#include "fa/core/image.h"

#include <memory>
#include <vector>

namespace fa::vision {

struct CameraStep {
    double dx = 0, dy = 0;   // pixels
    double da = 0;           // radians
    double ds = 1;           // scale
    bool valid = false;      // false: too few features, treated as no motion
};

// Feed consecutive frames; returns the motion from the previous frame to this one.
class MotionEstimator {
public:
    MotionEstimator();
    ~MotionEstimator();
    MotionEstimator(const MotionEstimator&) = delete;
    MotionEstimator& operator=(const MotionEstimator&) = delete;
    CameraStep next(const Image& frame);   // the first frame returns an invalid (zero) step

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fa::vision
