// Rendering backends. The renderer describes each output frame as a stack of layers (LayerDesc,
// see compositing.h for the exact per-pixel semantics); a backend composites them.
//   * CPU backend: reference implementation, multi-threaded, always available.
//   * GPU backend: Direct3D 11 (Windows), same semantics; selected at run time when a usable
//     adapter exists, otherwise the CPU backend is used.
#pragma once

#include "fa/core/image.h"
#include "fa/render/compositing.h"

#include <memory>
#include <string>
#include <vector>

namespace fa {

class RenderBackend {
public:
    virtual ~RenderBackend() = default;
    virtual std::string name() const = 0;
    virtual void beginFrame(int width, int height, Rgba8 background) = 0;
    virtual void drawLayer(const LayerDesc& layer) = 0;   // bottom to top
    virtual const Image& endFrame() = 0;                  // valid until the next beginFrame
    // GPU execution time of the last frame in milliseconds (GPU backends with timing enabled), else < 0.
    virtual double lastGpuMilliseconds() const { return -1.0; }
    // Time each frame on the GPU (D3D11 timestamp queries; adds a small synchronisation cost).
    virtual void setTimingEnabled(bool) {}
    // Effect passes executed on the GPU so far, and layers whose effect stack ran on the CPU reference instead
    // (GPU backends; -1 = not a GPU backend). Lets tests prove where effects ran.
    virtual int64_t gpuEffectPasses() const { return -1; }
    virtual int64_t cpuEffectFallbacks() const { return -1; }
    // GPU: pictures copied to the GPU, and keyed pictures (LayerDesc::imageKey) reused without a copy.
    virtual int64_t textureUploads() const { return -1; }
    virtual int64_t textureReuses() const { return -1; }
};

std::unique_ptr<RenderBackend> makeCpuBackend();
// The CPU reference compositor for one layer on an opaque canvas (also used by the GPU backend for
// layers beyond its shader limits, so results stay exact).
void compositeLayerCpu(Image& canvas, const LayerDesc& layer);

// GPU backend (Direct3D 11). Returns nullptr (and the reason) when no suitable device exists.
// preferHighPerformance selects the discrete GPU on hybrid laptops (DXGI GPU preference);
// false selects the power-saving (integrated) GPU.
std::unique_ptr<RenderBackend> makeGpuBackend(std::string* whyUnavailable = nullptr, bool preferHighPerformance = true);

struct GpuAdapterInfo {
    std::string name;
    size_t dedicatedVideoMemory = 0;   // bytes
    bool software = false;             // WARP / Basic Render Driver
    bool highPerformanceChoice = false;
    bool powerSavingChoice = false;
};
std::vector<GpuAdapterInfo> listGpuAdapters();   // empty when the build has no GPU backend

enum class BackendChoice { Auto, Cpu, Gpu };
// Auto: GPU when available, CPU otherwise. `used` receives a human-readable description.
std::unique_ptr<RenderBackend> makeBackend(BackendChoice choice, std::string* used = nullptr);

// Description of the GPU that makeGpuBackend would use ("NVIDIA GeForce RTX 2050 (D3D11, feature level 11_1)"),
// or empty when none.
std::string gpuDescription();

}  // namespace fa
