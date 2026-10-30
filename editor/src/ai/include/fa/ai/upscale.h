// M7 Video Upscaling with Real-ESRGAN (ncnn, Vulkan GPU or CPU): a clip's frames enlarged by a model before an
// export or Render Preview (prepareUpscales) and kept in the analysis cache; the renderer shows them in place of the
// decoded frames. Pictures are processed in overlapping tiles (memory stays bounded at any size), at exactly the size
// the clip needs on the canvas (never more than the chosen factor), with the colours and the alpha of the source.
// Flicker: where the source does not change between frames, the previous result is kept (static areas stay still).
// The non-AI alternative, Lanczos, needs no model and no preparation (fa/render/frame_store.h: resizeLanczos).
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

class Upscaler {
public:
    // model: "general" (realesrgan-x4plus, 4x; 2x = 4x then Lanczos down) or "anime" (realesr-animevideov3, 2x / 3x
    // / 4x natively, much faster). Throws ModelError when Real-ESRGAN is not installed.
    // tile / pad: the tile size and the context each tile sees on every side (0 = sized to the device / the default).
    Upscaler(const std::string& model, int scale, Accelerator acc = Accelerator::Auto, int tile = 0, int pad = -1);
    ~Upscaler();
    const std::string& provider() const;
    bool onGpu() const;
    int tile() const { return tile_; }
    int pad() const { return pad_; }
    int scale() const { return scale_; }
    // The picture enlarged by scale() (alpha: Lanczos-resized from the source).
    Image upscale(const Image& picture, const std::atomic<bool>* cancel = nullptr);

private:
    std::unique_ptr<NcnnModel> model_;
    int scale_ = 2, netScale_ = 4, tile_ = 192, pad_ = 16;   // 16: no seam left at tile borders (measured, AI_MODELS.md)
};

// The SHA-256 of the weights a model choice uses (cache identity).
std::string upscaleModelSha(const std::string& model, int scale);

// Seconds Real-ESRGAN takes for one frame of `megapixels` (the source picture) with a model choice (measured on the
// reference laptop, AI_MODELS.md).
double upscaleSecondsPerFrame(const std::string& model, int scale, double megapixels, bool gpu);
// The upscaled frames [fromFrame, toFrame) still need (not in the cache) and the time they take, without decoding.
WorkEstimate estimateUpscales(const Project& project, const Sequence& seq, int64_t fromFrame, int64_t toFrame,
                              const std::filesystem::path& projectFile);

struct UpscalePreparation {
    int clips = 0;
    int64_t needed = 0;      // frames the range uses
    int64_t computed = 0;    // newly upscaled (the rest were cached)
    double seconds = 0;
    std::string provider;
};

// Upscales every source frame that frames [fromFrame, toFrame) of the sequence show from clips with AI upscaling
// (in source order, so the flicker reduction sees the previous frame) and stores them. Throws ModelError / MediaError.
UpscalePreparation prepareUpscales(const Project& project, const Sequence& seq, int64_t fromFrame, int64_t toFrame,
                                   const std::filesystem::path& projectFile,
                                   const std::function<void(int64_t, int64_t)>& progress = {},
                                   const std::atomic<bool>* cancel = nullptr, Accelerator acc = Accelerator::Auto);

}  // namespace fa::ai
