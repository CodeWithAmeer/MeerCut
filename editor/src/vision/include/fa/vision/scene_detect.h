// M7 Scene Detection: shot boundaries in a video by classical signal processing. Two differences between consecutive
// frames: the picture's structure (a 64 x 36 grey image normalised to zero mean and unit contrast, so a fade or a
// brightness change does not count) and its colours (hue / saturation / value histograms). A cut is a structure jump
// that stands out from the differences around it (motion raises those) and is either a colour change too or a very
// large structure jump; a flash (the picture returns within a few frames) is not a cut; fades through black / white
// are found from the brightness. Deterministic, no learned model: the editor does not call it AI.
#pragma once

#include "fa/core/image.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace fa::vision {

struct SceneOptions {
    double sensitivity = 0.5;     // 0 (only obvious cuts) .. 1 (subtle cuts too)
    double minSceneSeconds = 0.15; // no two boundaries closer than this (montage shots can be a few frames long;
                                   // a flash of 1-3 frames is recognised as a flash, not as two cuts)
    bool fades = true;            // a fade through black / white is one boundary (at its darkest / brightest frame)
    int analysisWidth = 160;      // frames are decoded small for speed
};

enum class BoundaryKind { Cut, Fade };

struct SceneBoundary {
    int64_t frame = 0;            // first frame of the new scene (source frame index)
    BoundaryKind kind = BoundaryKind::Cut;
    double score = 0;             // how strong (the histogram difference relative to the local level)
};

// Frame-by-frame core (tests feed it frames directly): push frames in order, then finish().
class SceneDetector {
public:
    SceneDetector(const SceneOptions& o, double fps);
    void push(const Image& frame);
    std::vector<SceneBoundary> finish();
    const std::vector<double>& differences() const { return diff_; }   // colour, per frame (frame i vs i - 1)
    const std::vector<double>& structureDifferences() const { return sdiff_; }

private:
    SceneOptions opt_;
    double fps_;
    std::vector<std::vector<float>> hist_;   // per frame (kept small: 3 x 16 bins)
    std::vector<std::vector<float>> sig_;    // per frame: the normalised 64 x 36 structure
    std::vector<double> sdiff_;
    std::vector<double> cdiff_;   // hue / saturation only
    std::vector<double> luma_;               // mean luma per frame
    std::vector<double> diff_;
};

// How different two pictures are (0 = same colours .. 1 = nothing in common): the histogram distance the detector
// uses (AI Interpolation finds shot changes with detectScenes, which also looks at structure and local levels).
double frameDifference(const Image& a, const Image& b);

struct SceneResult {
    std::vector<SceneBoundary> boundaries;
    int64_t frames = 0;
    double fps = 0;
    std::vector<Image> thumbnails;   // first frame of each scene (scene 0 starts at frame 0), small
    double seconds = 0;              // analysis time
};

// Decodes the whole file (or [firstFrame, lastFrame]) and detects its scenes. Cancel returns what was found so far.
SceneResult detectScenes(const std::filesystem::path& media, const SceneOptions& o, int64_t firstFrame = 0,
                         int64_t lastFrame = -1, const std::function<void(double)>& progress = {},
                         const std::atomic<bool>* cancel = nullptr);

}  // namespace fa::vision
