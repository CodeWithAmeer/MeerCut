// Timeline evaluation: which source frames make up output frame n.
//
// Pure function of the model (no media access), so frame selection, cuts and transitions are
// unit-testable. Rules:
//   * Output frame n covers [n/fps, (n+1)/fps); it shows what is on the timeline at t = n/fps.
//   * A clip occupies [start, start + duration); at time t it shows the source frame chosen by its speed map
//     (fa/model/time_map.h: sampleFrame; speed 1: sourceIn + (t - start)). Disabled clips and hidden tracks
//     contribute nothing.
//   * A transition on clip A (outTransition) into the adjacent clip B spans
//       Centered:    [cut - D/2, cut + D/2]   (Filmora "Overlap", default)
//       EndsAtCut:   [cut - D,   cut]         (Filmora "Prefix")
//       StartsAtCut: [cut,       cut + D]     (Filmora "Postfix")
//     Inside the span both clips are sampled at sourceTimeAt(t) — beyond their trimmed range
//     where needed (handles) — and mixed with weight mix = (t - spanStart) / D for B
//     (Stage 1 EXP-001a/b/c). Source times outside the media are clamped by the frame provider,
//     which holds the edge frame exactly like Filmora does when handles are missing.
#pragma once

#include "fa/model/project.h"

#include <optional>
#include <vector>

namespace fa {

struct ClipSample {
    const Clip* clip = nullptr;
    Time sourceTime;
    std::optional<Time> blendTime;   // M5 Frame Blending / Optical Flow: the next source frame ...
    double blendWeight = 0.0;        // ... and its weight
};

struct TrackSample {
    size_t trackIndex = 0;          // 0 = bottom
    const Track* track = nullptr;
    ClipSample a;                   // the clip on screen (outgoing clip inside a transition)
    std::optional<ClipSample> b;    // incoming clip inside a transition
    double mix = 0.0;               // weight of b (0..1)
    const TransitionRef* transition = nullptr;
};

// TransitionSpan / transitionSpan() live in fa/model/project.h (pure model logic).

// Visible video layers for output frame `frame`, bottom to top. `project` supplies the media frame rates that
// speed ramps and frame blending need (without it, ramps use the continuous source time and nothing blends).
std::vector<TrackSample> evaluateVideoFrame(const Sequence& seq, int64_t frame, const Project* project = nullptr);

}  // namespace fa
