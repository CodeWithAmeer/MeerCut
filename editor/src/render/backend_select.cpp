#include "fa/render/backend.h"

#include <exception>

namespace fa {

#if !defined(FA_HAS_D3D11)
std::unique_ptr<RenderBackend> makeGpuBackend(std::string* why, bool) {
    if (why) *why = "this build has no GPU backend";
    return nullptr;
}
std::string gpuDescription() { return {}; }
std::vector<GpuAdapterInfo> listGpuAdapters() { return {}; }
#endif

namespace {

// Identity placement, Normal blend, no masks/crop/wipe: the CPU's integer fast path handles it at
// memory speed, faster than uploading it to a GPU and reading the result back (fa-bench).
bool plainLayer(const LayerDesc& l, int cw, int ch) {
    if (l.adjustment || l.effects) return false;   // effect stacks are GPU work (M4)
    if (!l.image || l.image->empty()) return true;
    return l.image->width == cw && l.image->height == ch && layerPlacement(l, cw, ch) == Affine2D{} && l.cropL == 0 &&
           l.cropT == 0 && l.cropR == 0 && l.cropB == 0 && l.blend == BlendMode::Normal && l.brightness == 1.0 &&
           l.flash == 0.0 && l.masks.empty() && !l.wipe.active;
}

// GPU compositing with the CPU reference as fallback. Layers are recorded during the frame (their
// images stay valid until endFrame()), so the frame can be composited by either backend:
//   * hybrid (BackendChoice::Auto): frames that are only plain copies, or small canvases, stay on the
//     CPU; frames with real compositing work (transforms, masks, blend modes, transitions) go to the GPU;
//   * GPU (BackendChoice::Gpu): every frame on the GPU.
// When the GPU fails (device removed or reset by the driver, out of memory), the frame is composited
// on the CPU and the CPU is used from then on. Both backends produce the same pixels (GPU parity tests).
// In hybrid mode the GPU device is created on the first frame that needs it, so projects without
// compositing work never pay for the GPU initialisation (about 0.3 s).
class GpuWithFallback final : public RenderBackend {
public:
    GpuWithFallback(std::unique_ptr<RenderBackend> gpu, bool hybrid, std::string adapter = {})
        : gpu_(std::move(gpu)), cpu_(makeCpuBackend()), hybrid_(hybrid), pending_(gpu_ == nullptr),
          adapter_(std::move(adapter)) {}

    std::string name() const override {
        if (!gpu_ && !pending_) return "cpu (GPU failed: " + failure_ + ")";
        const std::string g = gpu_ ? gpu_->name() : "gpu: " + adapter_;
        return hybrid_ ? "auto: " + g + " for compositing work, cpu for plain frames" : g;
    }
    double lastGpuMilliseconds() const override { return gpu_ && lastOnGpu_ ? gpu_->lastGpuMilliseconds() : -1.0; }
    int64_t gpuEffectPasses() const override { return gpu_ ? gpu_->gpuEffectPasses() : 0; }
    int64_t cpuEffectFallbacks() const override { return gpu_ ? gpu_->cpuEffectFallbacks() : 0; }
    int64_t textureUploads() const override { return gpu_ ? gpu_->textureUploads() : 0; }
    int64_t textureReuses() const override { return gpu_ ? gpu_->textureReuses() : 0; }
    void setTimingEnabled(bool on) override {
        timing_ = on;
        if (gpu_) gpu_->setTimingEnabled(on);
    }

    void beginFrame(int width, int height, Rgba8 background) override {
        w_ = width;
        h_ = height;
        bg_ = background;
        layers_.clear();
    }
    void drawLayer(const LayerDesc& layer) override { layers_.push_back(layer); }

    const Image& endFrame() override {
        lastOnGpu_ = false;
        const bool want = (gpu_ || pending_) && wantsGpu();
        if (want && pending_) {   // first frame with compositing work: create the device now
            pending_ = false;
            std::string why;
            gpu_ = makeGpuBackend(&why);
            if (!gpu_) failure_ = why;
            else gpu_->setTimingEnabled(timing_);
        }
        if (want && gpu_) {
            try {
                gpu_->beginFrame(w_, h_, bg_);
                for (const auto& l : layers_) gpu_->drawLayer(l);
                const Image& img = gpu_->endFrame();
                lastOnGpu_ = true;
                ++gpuFrames_;
                return img;
            } catch (const std::exception& e) {
                failure_ = e.what();
                gpu_.reset();
            }
        }
        cpu_->beginFrame(w_, h_, bg_);
        for (const auto& l : layers_) cpu_->drawLayer(l);
        ++cpuFrames_;
        return cpu_->endFrame();
    }

private:
    bool wantsGpu() const {
        if (!hybrid_) return true;
        for (const auto& l : layers_)   // effect stacks: GPU at any size (float passes are far cheaper there)
            if (l.adjustment || l.effects) return true;
        if (static_cast<int64_t>(w_) * h_ < 1280LL * 720) return false;   // preview-size canvases: CPU is as fast
        for (const auto& l : layers_)
            if (!plainLayer(l, w_, h_)) return true;
        return false;
    }

    std::unique_ptr<RenderBackend> gpu_, cpu_;
    bool hybrid_;
    bool pending_ = false;   // hybrid: GPU not created yet
    bool timing_ = false;
    std::string adapter_;
    std::vector<LayerDesc> layers_;
    int w_ = 0, h_ = 0;
    Rgba8 bg_;
    std::string failure_;
    bool lastOnGpu_ = false;
    int64_t gpuFrames_ = 0, cpuFrames_ = 0;
};

}  // namespace

std::unique_ptr<RenderBackend> makeBackend(BackendChoice choice, std::string* used) {
    if (choice == BackendChoice::Auto) {   // lazy: only check that a hardware adapter exists
        for (const auto& a : listGpuAdapters())
            if (a.highPerformanceChoice && !a.software) {
                auto b = std::make_unique<GpuWithFallback>(nullptr, true, a.name);
                if (used) *used = b->name();
                return b;
            }
    }
    if (choice != BackendChoice::Cpu) {
        std::string why;
        if (auto g = makeGpuBackend(&why)) {
            auto b = std::make_unique<GpuWithFallback>(std::move(g), choice == BackendChoice::Auto);
            if (used) *used = b->name();
            return b;
        }
        if (choice == BackendChoice::Gpu) {
            if (used) *used = "cpu (GPU requested but unavailable: " + why + ")";
        } else if (used) {
            *used = "cpu (" + why + ")";
        }
    } else if (used) {
        *used = "cpu";
    }
    return makeCpuBackend();
}

}  // namespace fa
