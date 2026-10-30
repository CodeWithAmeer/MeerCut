// M7 Auto Reframe: a clip re-framed for another aspect ratio (16:9, 9:16, 1:1, 4:5, custom) by a crop that follows
// the subject. The subject is found by face detection (YuNet), by the clip's M5 motion track, or is a fixed focal
// point. The path is smoothed (a calm virtual camera with a dead zone), kept inside the picture, cut where the shot
// changes, held for a moment when the subject leaves, then eased back to the focal point. The result is ordinary
// position keyframes and a scale on the clip's transform: editable, undoable, saved with the project, and drawn by the
// same renderer in the preview, Render Preview and export. The media is never changed.
#pragma once

#include "fa/ai/inference.h"
#include "fa/model/project.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace fa::ai {

enum class ReframeSubject { Faces, MotionTrack, FocalPoint };

struct ReframeOptions {
    double aspect = 9.0 / 16.0;    // target width / height
    ReframeSubject subject = ReframeSubject::Faces;
    double focusX = 0.5, focusY = 0.45;   // the focal point (source fractions): FocalPoint, and where the crop rests
                                          // when there is no subject
    double margin = 0.1;           // the subject stays at least this fraction of the crop away from its edges
    double zoom = 1.0;             // beyond filling the frame (1 = the crop is as large as the picture allows)
    double smoothness = 0.5;       // 0 follows closely .. 1 a very calm camera
    double holdSeconds = 1.0;      // the subject lost: hold this long, then ease back to the focal point
    double sampleSeconds = 0.2;    // detection interval
    Accelerator accelerator = Accelerator::Auto;
};

struct ReframeSample {
    double t = 0;                  // seconds from the clip's sourceIn (source time)
    double x = 0.5, y = 0.5;       // subject centre (source fractions)
    double size = 0;               // subject height (source fraction; faces)
    bool found = false;
};

struct ReframePath {
    int sourceWidth = 0, sourceHeight = 0;
    double fps = 25;
    std::vector<ReframeSample> samples;           // what was detected
    std::vector<double> cuts;                     // shot changes inside the clip (seconds from sourceIn)
    std::vector<std::pair<double, double>> centre;   // the crop centre at each sample (source fractions), smoothed
    double cropWidth = 1, cropHeight = 1;         // the crop window (source fractions)
    std::string provider;                         // the face detector's device
    double seconds = 0;                           // analysis time
    int found = 0;                                // samples with a subject
};

// The canvas for an aspect, the same pixel count class as the sequence: the short side kept (even sizes).
std::pair<int, int> reframeCanvas(int seqWidth, int seqHeight, double aspect);

// Analyses a clip's source range [sourceIn, sourceOut) of `media`. `track`: the clip's M5 motion track (subject
// MotionTrack). Cancel returns the samples so far (the path is then not applied). Throws ModelError / MediaError.
ReframePath analyseReframe(const std::filesystem::path& media, Time sourceIn, Time sourceOut, const ReframeOptions& o,
                           const MotionTrack* track = nullptr, const std::function<void(double)>& progress = {},
                           const std::atomic<bool>* cancel = nullptr);

// Smooths the samples into the crop path (exposed for tests: no decoding). Fills path.centre / crop size.
void planReframe(ReframePath& path, const ReframeOptions& o);

// The clip transform that shows the crop on a canvas of canvasW x canvasH: the scale filling the canvas and position
// keyframes (source time) following the path, thinned to the keys needed (within `tolerance` of the canvas width).
// Rotation, flips and crop of `base` are kept; its position and scale are replaced.
ClipTransform reframeTransform(const ReframePath& path, Time sourceIn, int canvasW, int canvasH,
                               const ClipTransform& base, double tolerance = 0.002);

// Where the crop window of a transform is at source time t (source fractions: centre x, y, width, height): what the
// canvas shows of the picture. For the dialog's guides and for tests.
struct CropWindow {
    double x = 0.5, y = 0.5, w = 1, h = 1;
};
CropWindow cropWindowAt(const ClipTransform& t, double sourceSeconds, int sourceW, int sourceH, int canvasW, int canvasH);

}  // namespace fa::ai
