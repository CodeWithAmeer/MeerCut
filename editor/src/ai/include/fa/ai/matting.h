// M7 Background Removal: running a matting model over a clip's source frames and storing the mattes
// (fa/render/matte_cache.h), which the renderer then composites. People: Robust Video Matting (recurrent, so the
// matte is temporally consistent; frames are analysed in source order). General objects: BiRefNet (frame by frame).
#pragma once

#include "fa/ai/inference.h"
#include "fa/core/image.h"
#include "fa/core/time.h"
#include "fa/model/project.h"
#include "fa/render/matte_cache.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace fa::ai {

// The cache key of a clip's cutout (its model's recorded SHA-256 identifies the model version).
MatteSetKey cutoutKey(const std::filesystem::path& media, const AiCutout& cutout);
// The model file of a cutout's model id (throws ModelError if the id is unknown or the model is not installed).
std::filesystem::path cutoutModelFile(const AiCutout& cutout);
// RVM downsample ratio for a picture and quality (0 fast, 1 standard, 2 high): the matting runs at ~320 / 480 / 640
// px on the long side, the model's own refiner restores full resolution.
float rvmDownsampleRatio(int width, int height, int quality);

// The GPU has too little free memory for the model (BiRefNet on a 4 GB GPU): nothing is switched to the CPU
// silently - the caller explains the cost and starts again with Accelerator::Cpu if the user agrees.
struct GpuMemoryError : ModelError {
    using ModelError::ModelError;
};

// Whether a model can be expected to fit the preferred GPU (BiRefNet needs more than 4 GB, measured), so that the
// editor asks before analysing on the CPU instead.
bool cutoutFitsGpu(const AiCutout& cutout);
// Measured on this project's reference laptop (i5-12450H): seconds per frame on the CPU, for the editor's estimate.
double cutoutCpuSecondsPerFrame(const AiCutout& cutout);

// One matting model session (RVM keeps its recurrent state between frames: feed frames in order; reset() at a jump).
class MattingSession {
public:
    MattingSession(const AiCutout& cutout, Accelerator acc = Accelerator::Auto);   // throws ModelError
    ~MattingSession();
    const std::string& provider() const;
    AlphaImage run(const Image& picture);
    void reset();
    double inferenceMs() const;    // totals so far (benchmarks)
    double processingMs() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct MattingJob {
    std::filesystem::path media;
    int64_t firstFrame = 0, lastFrame = 0;   // source frames (inclusive)
    AiCutout cutout;                         // model and quality
    Accelerator accelerator = Accelerator::Auto;
};

struct MattingReport {
    int64_t framesAnalysed = 0;   // newly computed (cached frames are reused)
    int64_t framesCached = 0;     // already in the cache
    double seconds = 0;
    std::string provider;         // what ran the model
    std::string fallback;         // why the GPU was not used, if it was asked for
};

// Analyses the missing frames of [firstFrame, lastFrame] and writes them to the cache. RVM starts 30 frames early
// (when the media has them) so its recurrent state has settled at the first stored frame; every run of missing frames
// starts that way (frames already analysed are never redone). Throws ModelError (GpuMemoryError) when the model is
// missing / fails; returns early (what was written stays valid) when cancelled. Afterwards the least recently used
// other matte sets are removed beyond the cache limit (analysisCacheLimitBytes).
MattingReport analyseMattes(const MattingJob& job, const std::function<void(int64_t done, int64_t total)>& progress = {},
                            const std::atomic<bool>* cancel = nullptr);

// One frame without temporal context (the inspector's preview before the whole clip is analysed). The session is kept
// between calls with the same model, quality and device.
AlphaImage matteForPicture(const Image& picture, const AiCutout& cutout, Accelerator acc = Accelerator::Auto);

// The source frames a clip shows: its whole source range (a reversed or retimed clip uses the same frames) and, given
// its track, the handle frames its transitions show beyond its ends (a dissolve plays the outgoing clip on past the
// cut).
std::pair<int64_t, int64_t> clipSourceFrames(const Clip& clip, const MediaSource& source, const Track* track = nullptr);

struct CutoutPreparation {
    int clips = 0;                 // clips with Background Removal on
    int64_t framesAnalysed = 0;    // frames that had no matte yet
    double seconds = 0;
    std::string provider;
};
// Before an export: every enabled Background Removal in the sequence gets its missing frames analysed, and its clip
// in `project` (a copy the export renders) the hash of the model used. Throws ModelError when a model is missing.
// Only clips that overlap timeline frames [fromFrame, toFrame) (toFrame < 0: to the end) - a Render Preview of a
// range does not analyse clips elsewhere.
CutoutPreparation prepareCutouts(Project& project, const std::string& sequenceId, const std::filesystem::path& projectFile,
                                 const std::function<void(int64_t done, int64_t total)>& progress = {},
                                 const std::atomic<bool>* cancel = nullptr, int64_t fromFrame = 0, int64_t toFrame = -1);
// Whether a clip overlaps timeline frames [fromFrame, toFrame) of a sequence (toFrame < 0: to the end).
bool clipInFrameRange(const Clip& clip, const Sequence& seq, int64_t fromFrame, int64_t toFrame);
// The timeline frames of [fromFrame, toFrame) where video clips that `uses` may be drawn - each such clip's range,
// widened by the longest transition on its track (a transition shows a clip's handle frames outside its range) -
// merged and sorted. Preparation and estimates walk only these frames instead of the whole range.
std::vector<std::pair<int64_t, int64_t>> framesUsing(const Sequence& seq, int64_t fromFrame, int64_t toFrame,
                                                      const std::function<bool(const Clip&)>& uses);

}  // namespace fa::ai
