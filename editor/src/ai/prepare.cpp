#include "fa/ai/prepare.h"

#include "fa/ai/interpolate.h"
#include "fa/ai/matting.h"
#include "fa/ai/models.h"
#include "fa/ai/upscale.h"
#include "fa/model/serialize.h"
#include "fa/render/matte_cache.h"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace fa::ai {

RenderPreparation prepareForRender(Project& project, const std::string& sequenceId, const std::filesystem::path& projectFile,
                                   int64_t fromFrame, int64_t toFrame,
                                   const std::function<void(const char*, int64_t, int64_t)>& progress,
                                   const std::atomic<bool>* cancel, Accelerator acc) {
    const auto t0 = std::chrono::steady_clock::now();
    RenderPreparation r;
    registerModelIdentities();
    const CutoutPreparation cut = prepareCutouts(project, sequenceId, projectFile, [&](int64_t d, int64_t t) {
        if (progress) progress("Background Removal", d, t);
    }, cancel, fromFrame, toFrame);
    r.mattes = cut.framesAnalysed;
    if (cancel && cancel->load()) return r;
    const Sequence* seq = nullptr;
    for (const auto& s : project.sequences)
        if (sequenceId.empty() || s.id == sequenceId) {
            seq = &s;
            break;
        }
    if (!seq) return r;
    const int64_t end = toFrame >= 0 ? toFrame : (seq->duration() * seq->frameRate.fps()).ceil();
    const NeuralPreparation neural = prepareNeuralFrames(project, *seq, fromFrame, end, projectFile, [&](int64_t d, int64_t t) {
        if (progress) progress("AI Interpolation", d, t);
    }, cancel, acc);
    r.neuralFrames = neural.computed;
    if (cancel && cancel->load()) return r;
    const UpscalePreparation up = prepareUpscales(project, *seq, fromFrame, end, projectFile, [&](int64_t d, int64_t t) {
        if (progress) progress("Video Upscaling", d, t);
    }, cancel, acc);
    r.upscaled = up.computed;
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::string s;
    auto add = [&](int64_t n, const char* what) {
        if (n) s += (s.empty() ? "" : ", ") + std::to_string(n) + " " + what;
    };
    add(r.mattes, "matte(s)");
    add(r.neuralFrames, "AI in-between frame(s)");
    add(r.upscaled, "upscaled frame(s)");
    if (!s.empty()) r.summary = "prepared " + s;
    return r;
}

std::string durationText(double seconds) {
    char b[64];
    if (seconds < 90) std::snprintf(b, sizeof b, "about %.0f s", std::max(1.0, seconds));
    else if (seconds < 5400) std::snprintf(b, sizeof b, "about %.0f min", seconds / 60);
    else std::snprintf(b, sizeof b, "about %.1f h", seconds / 3600);
    return b;
}

PreparationEstimate estimatePreparation(const Project& project, const std::string& sequenceId,
                                        const std::filesystem::path& projectFile, int64_t fromFrame, int64_t toFrame) {
    PreparationEstimate e;
    const Sequence* seq = nullptr;
    for (const auto& s : project.sequences)
        if (sequenceId.empty() || s.id == sequenceId) {
            seq = &s;
            break;
        }
    if (!seq) return e;
    // Background Removal: the frames each enabled clip still lacks (as prepareCutouts counts them)
    double matteSeconds = 0;
    bool matteCpu = false;
    for (const auto& t : seq->tracks) {
        if (t.kind != TrackKind::Video) continue;
        for (const auto& c : t.clips) {
            if (!c.aiCutout.enabled || c.isGenerated() || !clipInFrameRange(c, *seq, fromFrame, toFrame)) continue;
            const MediaSource* src = project.findSource(c.sourceId);
            const ModelInfo* m = findModel(c.aiCutout.model);
            if (!src || !m || m->files.empty()) continue;
            AiCutout cut = c.aiCutout;
            if (cut.modelSha.empty()) cut.modelSha = m->files.front().sha256;
            const auto media = resolveMediaPath(*src, projectFile);
            const auto [first, last] = clipSourceFrames(c, *src, &t);
            const int64_t missing = (last - first + 1) - MatteStore(cutoutKey(media, cut)).count(first, last);
            if (missing <= 0) continue;
            e.mattes += missing;
            const bool gpu = aiCapabilities().directMl && preferredDirectMlGpu() >= 0 && cutoutFitsGpu(cut);
            // RVM on DirectML (RTX 2050, 1280 x 720): 35 / 50 / 70 ms by quality; BiRefNet: only where it fits
            static const double rvmGpu[3] = {0.035, 0.050, 0.070};
            const double perFrame = !gpu ? cutoutCpuSecondsPerFrame(cut)
                                    : cut.model == "rvm-mobilenetv3" ? rvmGpu[std::clamp(cut.quality, 0, 2)]
                                                                     : 1.0;
            matteSeconds += missing * (perFrame + 0.04);   // + decoding and storing (96 frames end to end: 11.6 s)
            matteCpu = matteCpu || !gpu;
        }
    }
    const int64_t end = toFrame >= 0 ? toFrame : (seq->duration() * seq->frameRate.fps()).ceil();
    const WorkEstimate neural = estimateNeuralFrames(project, *seq, fromFrame, end, projectFile);
    const WorkEstimate up = estimateUpscales(project, *seq, fromFrame, end, projectFile);
    e.neuralFrames = neural.frames;
    e.upscaled = up.frames;
    // + loading each model and its first frame (1-4 s measured; 7.5 s for the first Vulkan use after a reboot)
    e.seconds = matteSeconds + neural.seconds + up.seconds + 4.0 * ((e.mattes > 0) + (neural.frames > 0) + (up.frames > 0));
    e.cpu = matteCpu || neural.cpu || up.cpu;
    if (e.frames() == 0) return e;
    std::string what;
    auto add = [&](int64_t n, const char* label) {
        if (n) what += (what.empty() ? "" : ", ") + std::to_string(n) + " " + label;
    };
    add(e.mattes, "Background Removal frame(s)");
    add(e.neuralFrames, "AI in-between frame(s)");
    add(e.upscaled, "upscaled frame(s)");
    e.text = what + ": " + durationText(e.seconds) + (e.cpu ? " (on the CPU)" : " (GPU)");
    return e;
}

}  // namespace fa::ai
