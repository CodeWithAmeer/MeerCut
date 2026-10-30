// Motion tracking of a rectangular region (M5, OpenCV; independently written logic).
//
// Per frame the region's centre moves by the robust (RANSAC, translation + uniform scale) motion of feature points
// tracked inside it with pyramidal Lucas-Kanade; points whose forward-backward error exceeds 1 px are dropped, and the
// set is re-detected when it thins out. Confidence = share of consistent points, checked against the appearance of
// the region (normalised cross-correlation with the template from the first frame). When confidence collapses (the
// target is covered, leaves the picture or loses all texture) the tracker searches for the template in a widened
// window; if that fails too the frame is marked lost and the last good position is held. Nothing is claimed for
// occlusion-heavy or textureless scenes: the confidence and lost flags say what happened.
#pragma once

#include "fa/core/image.h"

#include <atomic>
#include <memory>
#include <functional>
#include <vector>

namespace fa::vision {

struct Box {
    double cx = 0, cy = 0;   // centre, picture pixels
    double w = 0, h = 0;     // size, picture pixels
};

struct TrackPoint {
    double cx = 0, cy = 0;   // region centre, picture pixels
    double scale = 1;        // size relative to the first frame
    double confidence = 0;   // 0..1
    bool lost = false;
};

// Feed frames one at a time (consecutive frames of one clip, all the same size).
class RegionTracker {
public:
    RegionTracker();
    ~RegionTracker();
    RegionTracker(const RegionTracker&) = delete;
    RegionTracker& operator=(const RegionTracker&) = delete;

    // First frame and the region to follow. Returns false if the region has too little texture to track.
    bool start(const Image& frame, const Box& region);
    TrackPoint next(const Image& frame);   // the region in the next frame

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fa::vision
