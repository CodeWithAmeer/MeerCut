// M7 AI frame interpolation with RIFE v4.6 (ncnn, Vulkan GPU or CPU): a frame at any time step between two source
// frames, used by the M5 speed settings' "AI Interpolation (RIFE)" mode for slow motion. Neural frames are computed
// before an export or Render Preview (prepareNeuralFrames) and kept in a cache outside the project, so the export is
// deterministic and the editor's preview shows them where they exist (optical flow stands in elsewhere: neural
// inference is not run during realtime playback). Across a shot change no frame is invented: the nearer source frame
// is shown.
#pragma once

#include "fa/ai/inference.h"
#include "fa/core/image.h"
#include "fa/model/project.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace fa::ai {

class NeuralInterpolator {
public:
    explicit NeuralInterpolator(Accelerator acc = Accelerator::Auto);   // throws ModelError (model missing)
    ~NeuralInterpolator();
    const std::string& provider() const;
    bool onGpu() const;
    // The picture at time step t (0 = a, 1 = b) between two frames of the same size (RGBA; alpha mixed linearly).
    Image interpolate(const Image& a, const Image& b, double t);

private:
    std::unique_ptr<NcnnModel> model_;
};

// The model's identity in cache keys (the hash of its weights).
std::string interpolationModelSha();
// Tells the renderer which model weights prepared pictures belong to (RIFE and every Real-ESRGAN choice), so it uses
// only pictures made by the installed models. Called by the editor and fa-render at start and by every preparation.
void registerModelIdentities();

// Seconds RIFE takes for one in-between frame of `megapixels` (measured on the reference laptop, AI_MODELS.md).
double interpolationSecondsPerFrame(double megapixels, bool gpu);
// The in-between frames [fromFrame, toFrame) still need (not in the cache) and the time they take - without decoding
// (frame numbers from the media's frame rate). For the warning before long work.
WorkEstimate estimateNeuralFrames(const Project& project, const Sequence& seq, int64_t fromFrame, int64_t toFrame,
                                  const std::filesystem::path& projectFile);

struct NeuralPreparation {
    int64_t needed = 0;      // neural frames the range uses
    int64_t computed = 0;    // computed now (the rest were cached)
    int64_t sceneCuts = 0;   // frame pairs across a shot change (no frame invented)
    double seconds = 0;
    std::string provider;
};

// Computes every neural in-between frame that frames [fromFrame, toFrame) of the sequence need (clips with
// FrameInterpolation::Neural) and stores it in the cache. progress(done, total). Throws ModelError / MediaError.
NeuralPreparation prepareNeuralFrames(const Project& project, const Sequence& seq, int64_t fromFrame, int64_t toFrame,
                                      const std::filesystem::path& projectFile,
                                      const std::function<void(int64_t, int64_t)>& progress = {},
                                      const std::atomic<bool>* cancel = nullptr, Accelerator acc = Accelerator::Auto);

}  // namespace fa::ai
