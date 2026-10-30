// M7 local inference: what this PC can run AI models on (CPU, DirectML through Windows' own DirectML, Vulkan through
// ncnn / whisper.cpp), and a thin ONNX Runtime session. No ONNX Runtime, ncnn or DirectML type appears here.
#pragma once

#include "fa/ai/models.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fa::ai {

struct AiGpu {
    std::string name;
    bool discrete = false;
    uint64_t memoryMb = 0;       // dedicated video memory (DXGI) or the Vulkan device-local heap
    int dxgiIndex = -1;          // DirectML device id (adapter order of IDXGIFactory1::EnumAdapters1); -1 = none
    int vulkanIndex = -1;        // ncnn / ggml Vulkan device; -1 = none
};

struct AiCapabilities {
    std::string cpuName;
    int cpuThreads = 0;
    std::string onnxRuntimeVersion;              // the bundled ONNX Runtime
    std::vector<std::string> onnxProviders;      // as ONNX Runtime reports them, e.g. DmlExecutionProvider
    bool directMl = false;                       // the DirectML provider can be used (Windows' DirectML.dll found)
    std::string directMlVersion;                 // file version of System32\DirectML.dll
    bool vulkan = false;                         // at least one Vulkan compute device (ncnn)
    std::vector<AiGpu> gpus;                     // hardware adapters (software adapters left out)
    std::vector<std::string> notes;              // why something is unavailable
};
// Probed once (thread-safe; creating the Vulkan instance takes ~0.2 s), then cached.
const AiCapabilities& aiCapabilities();
std::string describeCapabilities(const AiCapabilities& c);   // multi-line text for the Model Manager / fa-probe --ai

// Where to run a model. Auto = the discrete GPU if there is one, else an integrated GPU, else the CPU.
enum class Accelerator { Auto, Cpu, Gpu };
const char* acceleratorId(Accelerator a);                    // "auto", "cpu", "gpu"
bool acceleratorFromId(const std::string& id, Accelerator& out);
// The GPU Auto / Gpu use for DirectML (index into aiCapabilities().gpus) or -1.
int preferredDirectMlGpu();
// Graphics memory this process may still use on a GPU now (index into aiCapabilities().gpus): the operating system's
// budget for it (which shrinks while other programs use the GPU) minus what it uses already; -1 when unknown. Asked
// before a large model is placed on the GPU - the card's total is never assumed to be MeerCut's alone.
int64_t gpuMemoryAvailableMb(int gpu);
int preferredVulkanGpu();

// Work a model would still have to do (frames not in the cache) and roughly how long it takes on the device it would
// use (M7 E: the warning before long preparation).
struct WorkEstimate {
    int64_t frames = 0;
    double seconds = 0;
    bool cpu = false;   // no usable GPU: the processor does it
};

struct Tensor {
    std::vector<int64_t> shape;
    std::vector<float> data;
    size_t count() const;
};

// One ONNX model loaded for inference. With Accelerator::Gpu / Auto the DirectML provider is used when it is available
// (else the CPU, and fallbackReason() says why); provider() names what runs it.
class OnnxModel {
public:
    OnnxModel(const std::filesystem::path& file, Accelerator acc, int gpu = -1);
    ~OnnxModel();
    OnnxModel(const OnnxModel&) = delete;
    OnnxModel& operator=(const OnnxModel&) = delete;

    const std::string& provider() const;          // "DirectML: NVIDIA GeForce RTX 2050" or "CPU (n threads)"
    bool onGpu() const;
    const std::string& fallbackReason() const;    // empty unless a GPU was asked for and the CPU is used
    const std::vector<std::string>& inputNames() const;
    const std::vector<std::string>& outputNames() const;
    std::vector<int64_t> inputShape(size_t i) const;   // -1 for dynamic dimensions

    // Run with float32 inputs (named) and return the named outputs (float32).
    std::vector<Tensor> run(const std::vector<std::pair<std::string, const Tensor*>>& inputs,
                            const std::vector<std::string>& outputs);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One ncnn model (param + bin). With Accelerator::Gpu / Auto it runs on a Vulkan device when there is one (else the
// CPU, with fallbackReason()). Tensors are CHW float32 (shape {C, H, W}).
class NcnnModel {
public:
    // registerLayers(net) may add custom layers (ncnn::Net*) before the model is loaded (e.g. RIFE's warp).
    NcnnModel(const std::filesystem::path& param, const std::filesystem::path& bin, Accelerator acc, int gpu = -1,
              const std::function<void(void*)>& registerLayers = {});
    ~NcnnModel();
    NcnnModel(const NcnnModel&) = delete;
    NcnnModel& operator=(const NcnnModel&) = delete;

    const std::string& provider() const;          // "Vulkan: NVIDIA GeForce RTX 2050" or "CPU (n threads)"
    bool onGpu() const;
    const std::string& fallbackReason() const;
    uint32_t gpuMemoryBudgetMb() const;           // the Vulkan device's heap budget (0 on the CPU)
    std::vector<Tensor> run(const std::vector<std::pair<std::string, const Tensor*>>& inputs,
                            const std::vector<std::string>& outputs);
    void* net();                                  // ncnn::Net*, for runners inside fa_ai

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fa::ai
