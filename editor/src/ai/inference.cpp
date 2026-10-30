// M7 device probing (DXGI, DirectML, Vulkan via ncnn) and the ONNX Runtime session wrapper.
#include "fa/ai/inference.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>

#include <gpu.h>   // ncnn: Vulkan device enumeration
#include <net.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <sstream>
#include <thread>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "version.lib")

namespace fa::ai {

namespace {

std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(0, n - 1)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string fileVersion(const std::wstring& path) {
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return {};
    std::vector<char> buf(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return {};
    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
           std::to_string(HIWORD(info->dwFileVersionLS)) + "." + std::to_string(LOWORD(info->dwFileVersionLS));
}

std::string cpuBrand() {
    int regs[4] = {};
    char brand[49] = {};
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000004) return "CPU";
    for (int i = 0; i < 3; ++i) {
        __cpuid(regs, 0x80000002 + i);
        std::memcpy(brand + 16 * i, regs, 16);
    }
    std::string s(brand);
    s.erase(0, s.find_first_not_of(' '));
    return s;
}

Ort::Env& ortEnv() {
    // MEERCUT_ORT_VERBOSE=1: ONNX Runtime's own log (node placement per provider) on stderr, for diagnosis
    static Ort::Env env = [] {
        Ort::Env e(std::getenv("MEERCUT_ORT_VERBOSE") ? ORT_LOGGING_LEVEL_VERBOSE : ORT_LOGGING_LEVEL_ERROR, "meercut");
        // M7 F: the official Windows build emits telemetry events that the operating system's diagnostic data may
        // collect; MeerCut runs models offline and turns them off
        e.DisableTelemetryEvents();
        return e;
    }();
    return env;
}

std::string u8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

AiCapabilities probe() {
    AiCapabilities c;
    c.cpuName = cpuBrand();
    c.cpuThreads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    c.onnxRuntimeVersion = Ort::GetVersionString();
    for (const auto& p : Ort::GetAvailableProviders()) c.onnxProviders.push_back(p);
    // DXGI hardware adapters, in the order DirectML device ids use
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            adapter->GetDesc1(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            AiGpu g;
            g.name = narrow(d.Description);
            g.memoryMb = d.DedicatedVideoMemory >> 20;
            g.discrete = d.DedicatedVideoMemory > (512ull << 20);
            g.dxgiIndex = static_cast<int>(i);
            c.gpus.push_back(g);
        }
    }
    // DirectML: the provider is in the bundled ONNX Runtime; its DirectML.dll is Windows' own (System32)
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    c.directMlVersion = fileVersion(std::wstring(sys) + L"\\DirectML.dll");
    const bool dmlProvider =
        std::find(c.onnxProviders.begin(), c.onnxProviders.end(), "DmlExecutionProvider") != c.onnxProviders.end();
    c.directMl = dmlProvider && !c.directMlVersion.empty() && !c.gpus.empty();
    if (!dmlProvider) c.notes.push_back("This ONNX Runtime has no DirectML provider: ONNX models run on the CPU.");
    else if (c.directMlVersion.empty()) c.notes.push_back("Windows' DirectML.dll was not found: ONNX models run on the CPU.");
    else if (c.gpus.empty()) c.notes.push_back("No hardware graphics adapter: ONNX models run on the CPU.");
    // Vulkan compute devices (ncnn): matched to the DXGI adapters by name
    if (ncnn::create_gpu_instance() == 0) {
        std::atexit([] { ncnn::destroy_gpu_instance(); });   // after every model created later has been destroyed
        const int n = ncnn::get_gpu_count();
        for (int i = 0; i < n; ++i) {
            const ncnn::GpuInfo& info = ncnn::get_gpu_info(i);
            const std::string name = info.device_name();
            if (info.type() == 3) continue;   // CPU implementations (e.g. llvmpipe)
            c.vulkan = true;
            auto it = std::find_if(c.gpus.begin(), c.gpus.end(),
                                   [&](const AiGpu& g) { return g.vulkanIndex < 0 && lower(g.name) == lower(name); });
            if (it == c.gpus.end()) {
                AiGpu g;
                g.name = name;
                g.discrete = info.type() == 0;
                c.gpus.push_back(g);
                it = c.gpus.end() - 1;
            }
            it->vulkanIndex = i;
            it->discrete = info.type() == 0;
            if (const ncnn::VulkanDevice* dev = ncnn::get_gpu_device(i); dev && it->memoryMb == 0)
                it->memoryMb = dev->get_heap_budget();
        }
    }
    if (!c.vulkan) c.notes.push_back("No Vulkan compute device: upscaling, AI interpolation and speech run on the CPU.");
    return c;
}

int preferred(bool vulkan) {
    const auto& c = aiCapabilities();
    int best = -1;
    for (size_t i = 0; i < c.gpus.size(); ++i) {
        const AiGpu& g = c.gpus[i];
        if ((vulkan ? g.vulkanIndex : g.dxgiIndex) < 0) continue;
        if (best < 0 || (g.discrete && !c.gpus[static_cast<size_t>(best)].discrete)) best = static_cast<int>(i);
    }
    return best;
}

}  // namespace

const AiCapabilities& aiCapabilities() {
    static std::once_flag once;
    static AiCapabilities c;
    std::call_once(once, [] { c = probe(); });
    return c;
}

std::string describeCapabilities(const AiCapabilities& c) {
    std::ostringstream s;
    s << "CPU: " << c.cpuName << " (" << c.cpuThreads << " threads)\n";
    s << "ONNX Runtime " << c.onnxRuntimeVersion << ", providers:";
    for (const auto& p : c.onnxProviders) s << " " << p;
    s << "\nDirectML: " << (c.directMl ? "available" : "not available")
      << (c.directMlVersion.empty() ? "" : " (Windows DirectML.dll " + c.directMlVersion + ")") << "\n";
    s << "Vulkan compute: " << (c.vulkan ? "available" : "not available") << "\n";
    for (size_t i = 0; i < c.gpus.size(); ++i) {
        const AiGpu& g = c.gpus[i];
        const int64_t free = gpuMemoryAvailableMb(static_cast<int>(i));
        s << "GPU: " << g.name << (g.discrete ? " [discrete]" : " [integrated]") << ", " << g.memoryMb << " MB"
          << (free >= 0 ? ", " + std::to_string(free) + " MB available now" : std::string())
          << (g.dxgiIndex >= 0 ? ", DirectML device " + std::to_string(g.dxgiIndex) : std::string())
          << (g.vulkanIndex >= 0 ? ", Vulkan device " + std::to_string(g.vulkanIndex) : std::string()) << "\n";
    }
    for (const auto& n : c.notes) s << "Note: " << n << "\n";
    return s.str();
}

int64_t gpuMemoryAvailableMb(int gpu) {
    const auto& caps = aiCapabilities();
    if (gpu < 0 || gpu >= static_cast<int>(caps.gpus.size()) || caps.gpus[static_cast<size_t>(gpu)].dxgiIndex < 0) return -1;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        factory->EnumAdapters1(static_cast<UINT>(caps.gpus[static_cast<size_t>(gpu)].dxgiIndex), &adapter) == DXGI_ERROR_NOT_FOUND ||
        FAILED(adapter.As(&adapter3)))
        return -1;
    // the local segment group: a discrete GPU's own memory; on an integrated GPU (UMA) DXGI reports the share of system
    // memory it may use there
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return -1;
    return info.Budget > info.CurrentUsage ? static_cast<int64_t>((info.Budget - info.CurrentUsage) >> 20) : 0;
}

const char* acceleratorId(Accelerator a) {
    switch (a) {
    case Accelerator::Auto: return "auto";
    case Accelerator::Cpu: return "cpu";
    case Accelerator::Gpu: return "gpu";
    }
    return "auto";
}

bool acceleratorFromId(const std::string& id, Accelerator& out) {
    for (Accelerator a : {Accelerator::Auto, Accelerator::Cpu, Accelerator::Gpu})
        if (id == acceleratorId(a)) {
            out = a;
            return true;
        }
    return false;
}

int preferredDirectMlGpu() { return aiCapabilities().directMl ? preferred(false) : -1; }
int preferredVulkanGpu() { return preferred(true); }

size_t Tensor::count() const {
    size_t n = 1;
    for (int64_t d : shape) n *= static_cast<size_t>(std::max<int64_t>(d, 0));
    return n;
}

struct OnnxModel::Impl {
    std::unique_ptr<Ort::Session> session;
    std::string provider, fallback;
    bool gpu = false;
    std::vector<std::string> inputs, outputs;
};

OnnxModel::OnnxModel(const std::filesystem::path& file, Accelerator acc, int gpu) : impl_(std::make_unique<Impl>()) {
    const auto& caps = aiCapabilities();
    auto make = [&](bool useGpu, int gpuIndex) {
        Ort::SessionOptions opt;
        opt.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (useGpu) {   // DirectML: no memory pattern, sequential execution (provider requirements)
            opt.DisableMemPattern();
            opt.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
            const OrtDmlApi* dml = nullptr;
            Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dml)));
            Ort::ThrowOnError(dml->SessionOptionsAppendExecutionProvider_DML(opt, caps.gpus[static_cast<size_t>(gpuIndex)].dxgiIndex));
        } else {
            opt.SetIntraOpNumThreads(std::max(1, caps.cpuThreads / 2));
        }
        return std::make_unique<Ort::Session>(ortEnv(), file.wstring().c_str(), opt);
    };
    bool wantGpu = acc != Accelerator::Cpu;
    int g = gpu >= 0 ? gpu : preferredDirectMlGpu();
    if (wantGpu && (g < 0 || g >= static_cast<int>(caps.gpus.size()) || caps.gpus[static_cast<size_t>(g)].dxgiIndex < 0 ||
                    !caps.directMl)) {
        if (acc == Accelerator::Gpu)
            impl_->fallback = caps.notes.empty() ? "DirectML is not available on this PC" : caps.notes.front();
        wantGpu = false;
    }
    if (wantGpu) {
        try {
            impl_->session = make(true, g);
            impl_->gpu = true;
            impl_->provider = "DirectML: " + caps.gpus[static_cast<size_t>(g)].name;
        } catch (const std::exception& e) {
            impl_->fallback = std::string("DirectML could not load the model (") + e.what() + "); using the CPU";
        }
    }
    if (!impl_->session) {
        try {
            impl_->session = make(false, -1);
        } catch (const Ort::Exception& e) {
            throw ModelError("cannot load " + u8(file) + ": " + e.what());
        }
        impl_->provider = "CPU (" + std::to_string(std::max(1, caps.cpuThreads / 2)) + " threads)";
    }
    Ort::AllocatorWithDefaultOptions alloc;
    for (size_t i = 0; i < impl_->session->GetInputCount(); ++i)
        impl_->inputs.push_back(impl_->session->GetInputNameAllocated(i, alloc).get());
    for (size_t i = 0; i < impl_->session->GetOutputCount(); ++i)
        impl_->outputs.push_back(impl_->session->GetOutputNameAllocated(i, alloc).get());
}

OnnxModel::~OnnxModel() = default;

const std::string& OnnxModel::provider() const { return impl_->provider; }
bool OnnxModel::onGpu() const { return impl_->gpu; }
const std::string& OnnxModel::fallbackReason() const { return impl_->fallback; }
const std::vector<std::string>& OnnxModel::inputNames() const { return impl_->inputs; }
const std::vector<std::string>& OnnxModel::outputNames() const { return impl_->outputs; }

std::vector<int64_t> OnnxModel::inputShape(size_t i) const {
    return impl_->session->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
}

std::vector<Tensor> OnnxModel::run(const std::vector<std::pair<std::string, const Tensor*>>& inputs,
                                   const std::vector<std::string>& outputs) {
    const Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<Ort::Value> values;
    std::vector<const char*> inNames, outNames;
    for (const auto& [name, t] : inputs) {
        values.push_back(Ort::Value::CreateTensor<float>(mem, const_cast<float*>(t->data.data()), t->data.size(),
                                                         t->shape.data(), t->shape.size()));
        inNames.push_back(name.c_str());
    }
    for (const auto& n : outputs) outNames.push_back(n.c_str());
    std::vector<Ort::Value> res;
    try {
        res = impl_->session->Run(Ort::RunOptions{nullptr}, inNames.data(), values.data(), values.size(), outNames.data(),
                                  outNames.size());
    } catch (const Ort::Exception& e) {
        throw ModelError(std::string("inference failed: ") + e.what());
    }
    std::vector<Tensor> out;
    for (auto& v : res) {
        Tensor t;
        const auto info = v.GetTensorTypeAndShapeInfo();
        t.shape = info.GetShape();
        const float* p = v.GetTensorData<float>();
        t.data.assign(p, p + info.GetElementCount());
        out.push_back(std::move(t));
    }
    return out;
}

struct NcnnModel::Impl {
    ncnn::Net net;
    std::string provider, fallback;
    bool gpu = false;
    uint32_t budget = 0;
};

NcnnModel::NcnnModel(const std::filesystem::path& param, const std::filesystem::path& bin, Accelerator acc, int gpu,
                     const std::function<void(void*)>& registerLayers)
    : impl_(std::make_unique<Impl>()) {
    const auto& caps = aiCapabilities();
    int g = gpu >= 0 ? gpu : preferredVulkanGpu();
    const bool usable = g >= 0 && g < static_cast<int>(caps.gpus.size()) && caps.gpus[static_cast<size_t>(g)].vulkanIndex >= 0;
    if (acc != Accelerator::Cpu && usable) {
        const int vk = caps.gpus[static_cast<size_t>(g)].vulkanIndex;
        impl_->net.opt.use_vulkan_compute = true;
        impl_->net.set_vulkan_device(vk);
        impl_->gpu = true;
        impl_->provider = "Vulkan: " + caps.gpus[static_cast<size_t>(g)].name;
        if (const ncnn::VulkanDevice* dev = ncnn::get_gpu_device(vk)) impl_->budget = dev->get_heap_budget();
    } else {
        if (acc == Accelerator::Gpu) impl_->fallback = "no Vulkan compute device on this PC; using the CPU";
        impl_->net.opt.use_vulkan_compute = false;
        impl_->net.opt.num_threads = std::max(1, caps.cpuThreads / 2);
        impl_->provider = "CPU (" + std::to_string(impl_->net.opt.num_threads) + " threads)";
    }
    if (registerLayers) registerLayers(&impl_->net);
    // FILE* (opened with the wide path) so that any user folder name works
    FILE* fp = _wfopen(param.wstring().c_str(), L"rb");
    FILE* fb = _wfopen(bin.wstring().c_str(), L"rb");
    const bool ok = fp && fb && impl_->net.load_param(fp) == 0 && impl_->net.load_model(fb) == 0;
    if (fp) std::fclose(fp);
    if (fb) std::fclose(fb);
    if (!ok) throw ModelError("cannot load the ncnn model " + u8(param));
}

NcnnModel::~NcnnModel() = default;

const std::string& NcnnModel::provider() const { return impl_->provider; }
bool NcnnModel::onGpu() const { return impl_->gpu; }
const std::string& NcnnModel::fallbackReason() const { return impl_->fallback; }
uint32_t NcnnModel::gpuMemoryBudgetMb() const { return impl_->budget; }
void* NcnnModel::net() { return &impl_->net; }

std::vector<Tensor> NcnnModel::run(const std::vector<std::pair<std::string, const Tensor*>>& inputs,
                                   const std::vector<std::string>& outputs) {
    ncnn::Extractor ex = impl_->net.create_extractor();
    for (const auto& [name, t] : inputs) {
        if (t->shape.size() != 3) throw ModelError("ncnn inputs are CHW");
        const int c = static_cast<int>(t->shape[0]), h = static_cast<int>(t->shape[1]), w = static_cast<int>(t->shape[2]);
        ncnn::Mat m(w, h, c);
        for (int k = 0; k < c; ++k)
            std::memcpy(m.channel(k), t->data.data() + static_cast<size_t>(k) * w * h, sizeof(float) * w * h);
        if (ex.input(name.c_str(), m) != 0) throw ModelError("the model has no input " + name);
    }
    std::vector<Tensor> out;
    for (const auto& name : outputs) {
        ncnn::Mat m;
        if (ex.extract(name.c_str(), m) != 0) throw ModelError("inference failed (output " + name + ")");
        Tensor t;
        t.shape = {m.c, m.h, m.w};
        t.data.resize(static_cast<size_t>(m.c) * m.h * m.w);
        for (int k = 0; k < m.c; ++k)
            std::memcpy(t.data.data() + static_cast<size_t>(k) * m.w * m.h, m.channel(k), sizeof(float) * m.w * m.h);
        out.push_back(std::move(t));
    }
    return out;
}

}  // namespace fa::ai
