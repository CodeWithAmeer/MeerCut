// M7: everything an export or a Render Preview needs from the AI models, prepared before frames are drawn: the
// Background Removal mattes still missing, the AI Interpolation in-between frames and the AI-upscaled frames of the
// range. The preview, Render Preview and the export then draw the same prepared pictures (preview = export).
#pragma once

#include "fa/ai/inference.h"
#include "fa/model/project.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

namespace fa::ai {

struct RenderPreparation {
    int64_t mattes = 0;          // Background Removal frames analysed now
    int64_t neuralFrames = 0;    // AI Interpolation frames computed now
    int64_t upscaled = 0;        // AI Upscaling frames computed now
    double seconds = 0;
    std::string summary;         // for the status line ("" when nothing was needed)
};

// Prepares frames [fromFrame, toFrame) of the sequence (toFrame < 0: to its end). `project` is the copy that will be
// rendered (Background Removal records its model's hash in it). progress(stage, done, total) - stage is
// "Background Removal", "AI Interpolation" or "Video Upscaling". Cancel returns early (what was prepared is kept).
// Throws ModelError (a model missing or failing) / MediaError.
RenderPreparation prepareForRender(Project& project, const std::string& sequenceId, const std::filesystem::path& projectFile,
                                   int64_t fromFrame, int64_t toFrame,
                                   const std::function<void(const char*, int64_t, int64_t)>& progress = {},
                                   const std::atomic<bool>* cancel = nullptr, Accelerator acc = Accelerator::Auto);

// What preparing frames [fromFrame, toFrame) would still compute (frames already in the cache are not counted) and
// roughly how long that takes on the devices the models will use (per-frame times measured on the reference laptop,
// AI_MODELS.md, scaled by the picture size). Asked before an export or a Prepare button starts the work, so that
// hours of CPU processing never begin without the user's agreement. Does not decode anything.
struct PreparationEstimate {
    int64_t mattes = 0;
    int64_t neuralFrames = 0;
    int64_t upscaled = 0;
    double seconds = 0;
    bool cpu = false;          // some of the work runs on the CPU (no usable GPU, or the model does not fit it)
    std::string text;          // "" when nothing is to be done, else e.g. "48 upscaled frames: about 2 min (GPU)"
    int64_t frames() const { return mattes + neuralFrames + upscaled; }
};
PreparationEstimate estimatePreparation(const Project& project, const std::string& sequenceId,
                                        const std::filesystem::path& projectFile, int64_t fromFrame, int64_t toFrame);
// Whether the editor asks before starting it: more than a minute on the CPU or ten minutes on the GPU.
inline bool needsConfirmation(const PreparationEstimate& e) { return e.seconds > (e.cpu ? 60.0 : 600.0); }
// "about 40 s", "about 12 min", "about 3.5 h"
std::string durationText(double seconds);

}  // namespace fa::ai
