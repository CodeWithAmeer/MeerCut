#include "fa/ai/matting.h"

#include "fa/media/media.h"
#include "fa/model/serialize.h"
#include "fa/model/time_map.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>

namespace fa::ai {

namespace {

const ModelInfo& cutoutModel(const AiCutout& c) {
    const ModelInfo* m = findModel(c.model);
    if (!m || m->runtime != ModelRuntime::Onnx) throw ModelError("unknown background-removal model \"" + c.model + "\"");
    return *m;
}

bool isRvm(const AiCutout& c) { return c.model.rfind("rvm-", 0) == 0; }

Tensor planes(const Image& img, bool batch, const float* mean = nullptr, const float* std = nullptr) {
    Tensor t;
    t.shape = batch ? std::vector<int64_t>{1, 3, img.height, img.width} : std::vector<int64_t>{3, img.height, img.width};
    const size_t n = static_cast<size_t>(img.width) * img.height;
    t.data.resize(3 * n);
    // v = (p / 255 - mean) / std as one multiply-add per sample, through a table per channel
    float lut[3][256];
    for (int c = 0; c < 3; ++c)
        for (int v = 0; v < 256; ++v) lut[c][v] = mean ? (v / 255.0f - mean[c]) / std[c] : v / 255.0f;
    float* r = t.data.data();
    float* g = r + n;
    float* b = g + n;
    const uint8_t* p = img.pixels.data();
    for (size_t i = 0; i < n; ++i, p += 4) {
        r[i] = lut[0][p[0]];
        g[i] = lut[1][p[1]];
        b[i] = lut[2][p[2]];
    }
    return t;
}

AlphaImage toAlpha(const float* p, int w, int h, bool logits) {
    AlphaImage a{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h)};
    for (size_t i = 0; i < a.a.size(); ++i) {
        float v = p[i];
        if (logits) v = 1.0f / (1.0f + std::exp(-v));
        a.a[i] = static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    return a;
}

// Bilinear resize of an RGBA picture (BiRefNet takes a fixed 1024 x 1024 input).
Image resizeRgba(const Image& src, int w, int h) {
    Image out(w, h);
    const double sx = static_cast<double>(src.width) / w, sy = static_cast<double>(src.height) / h;
    for (int y = 0; y < h; ++y) {
        const double fy = std::clamp((y + 0.5) * sy - 0.5, 0.0, src.height - 1.0);
        const int y0 = static_cast<int>(fy), y1 = std::min(src.height - 1, y0 + 1);
        const double ty = fy - y0;
        for (int x = 0; x < w; ++x) {
            const double fx = std::clamp((x + 0.5) * sx - 0.5, 0.0, src.width - 1.0);
            const int x0 = static_cast<int>(fx), x1 = std::min(src.width - 1, x0 + 1);
            const double tx = fx - x0;
            for (int c = 0; c < 4; ++c) {
                auto at = [&](int xx, int yy) { return src.pixels[(static_cast<size_t>(yy) * src.width + xx) * 4 + c]; };
                const double v = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
                out.pixels[(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<uint8_t>(std::lround(v));
            }
        }
    }
    return out;
}

AlphaImage resizeAlpha(const AlphaImage& a, int w, int h) {
    if (a.width == w && a.height == h) return a;
    AlphaImage out{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h)};
    const double sx = static_cast<double>(a.width) / w, sy = static_cast<double>(a.height) / h;
    for (int y = 0; y < h; ++y) {
        const double fy = std::clamp((y + 0.5) * sy - 0.5, 0.0, a.height - 1.0);
        const int y0 = static_cast<int>(fy), y1 = std::min(a.height - 1, y0 + 1);
        const double ty = fy - y0;
        for (int x = 0; x < w; ++x) {
            const double fx = std::clamp((x + 0.5) * sx - 0.5, 0.0, a.width - 1.0);
            const int x0 = static_cast<int>(fx), x1 = std::min(a.width - 1, x0 + 1);
            const double tx = fx - x0;
            auto at = [&](int xx, int yy) { return a.a[static_cast<size_t>(yy) * a.width + xx]; };
            const double v = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
            out.a[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(std::lround(v));
        }
    }
    return out;
}

// One model session with RVM's recurrent state (or a stateless BiRefNet session).
class Matter {
public:
    Matter(const AiCutout& c, Accelerator acc) : rvm_(isRvm(c)), quality_(c.quality), file_(cutoutModelFile(c)) {
        model_ = std::make_unique<OnnxModel>(file_, acc);
        reset();
    }
    const OnnxModel& model() const { return *model_; }
    const std::string& fallback() const { return model_->fallbackReason(); }
    void reset() {
        for (auto& r : rec_) r = Tensor{{1, 1, 1, 1}, {0.0f}};
    }
    AlphaImage run(const Image& img) {
        const auto t0 = std::chrono::steady_clock::now();
        auto ms = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        if (rvm_) {
            const Tensor src = planes(img, true);
            const Tensor ds{{1}, {rvmDownsampleRatio(img.width, img.height, quality_)}};
            const auto t1 = std::chrono::steady_clock::now();
            auto out = infer({{"src", &src}, {"r1i", &rec_[0]}, {"r2i", &rec_[1]}, {"r3i", &rec_[2]},
                              {"r4i", &rec_[3]}, {"downsample_ratio", &ds}},
                             {"pha", "r1o", "r2o", "r3o", "r4o"});
            const auto t2 = std::chrono::steady_clock::now();
            for (int i = 0; i < 4; ++i) rec_[i] = std::move(out[static_cast<size_t>(i) + 1]);
            AlphaImage a = toAlpha(out[0].data.data(), img.width, img.height, false);
            inferMs += ms(t1, t2);
            processMs += ms(t0, t1) + ms(t2, std::chrono::steady_clock::now());
            return a;
        }
        // BiRefNet: 1024 x 1024, ImageNet normalisation, one logit map
        static const float mean[3] = {0.485f, 0.456f, 0.406f}, stdv[3] = {0.229f, 0.224f, 0.225f};
        const Image small = resizeRgba(img, 1024, 1024);
        const Tensor in = planes(small, true, mean, stdv);
        const auto& names = model_->inputNames();
        const auto t1 = std::chrono::steady_clock::now();
        const auto out = infer({{names.front(), &in}}, {model_->outputNames().back()});
        const auto t2 = std::chrono::steady_clock::now();
        const AlphaImage a = resizeAlpha(toAlpha(out[0].data.data(), 1024, 1024, true), img.width, img.height);
        inferMs += ms(t1, t2);
        processMs += ms(t0, t1) + ms(t2, std::chrono::steady_clock::now());
        return a;
    }

    double inferMs = 0, processMs = 0;

private:
    // The GPU running out of memory (BiRefNet's fixed 1024 x 1024 input needs more than a 4 GB laptop GPU has) is
    // reported as GpuMemoryError: the CPU is never chosen silently (for BiRefNet it means minutes per second of video);
    // the caller asks the user and starts again on the CPU.
    std::vector<Tensor> infer(const std::vector<std::pair<std::string, const Tensor*>>& in, const std::vector<std::string>& out) {
        try {
            return model_->run(in, out);
        } catch (const std::exception& e) {
            const std::string what = e.what();
            const bool oom = what.find("memory") != std::string::npos || what.find("8007000E") != std::string::npos;
            if (!model_->onGpu() || !oom) throw;
            throw GpuMemoryError(model_->provider() + " does not have enough free memory for this model");
        }
    }

    bool rvm_;
    int quality_;
    std::filesystem::path file_;
    std::unique_ptr<OnnxModel> model_;
    Tensor rec_[4];
};

}  // namespace

struct MattingSession::Impl {
    Matter matter;
};

MattingSession::MattingSession(const AiCutout& cutout, Accelerator acc) : impl_(new Impl{Matter(cutout, acc)}) {}
MattingSession::~MattingSession() = default;
const std::string& MattingSession::provider() const { return impl_->matter.model().provider(); }
AlphaImage MattingSession::run(const Image& picture) { return impl_->matter.run(picture); }
void MattingSession::reset() { impl_->matter.reset(); }
double MattingSession::inferenceMs() const { return impl_->matter.inferMs; }
double MattingSession::processingMs() const { return impl_->matter.processMs; }

std::filesystem::path cutoutModelFile(const AiCutout& cutout) {
    const ModelInfo& m = cutoutModel(cutout);
    requireModel(m);
    return modelFilePath(m, 0);
}

bool cutoutFitsGpu(const AiCutout& cutout) {
    if (isRvm(cutout)) return true;
    const auto& caps = aiCapabilities();
    const int g = preferredDirectMlGpu();
    // BiRefNet (1024 x 1024): out of memory on the 4 GB RTX 2050 (measured); larger GPUs are not tested
    // What counts is the memory available to MeerCut now (other programs' use shrinks it), not only the card's total.
    if (g < 0 || caps.gpus[static_cast<size_t>(g)].memoryMb <= 4500) return false;
    const int64_t available = gpuMemoryAvailableMb(g);
    return available < 0 || available > 4500;
}

double cutoutCpuSecondsPerFrame(const AiCutout& cutout) {
    if (!isRvm(cutout)) return 41.0;   // BiRefNet tiny, 6 threads (AI_MODELS.md)
    static const double rvm[3] = {0.050, 0.082, 0.140};
    return rvm[std::clamp(cutout.quality, 0, 2)];
}

MatteSetKey cutoutKey(const std::filesystem::path& media, const AiCutout& cutout) {
    return cutoutSetKey(media, cutout, cutoutModel(cutout).files.front().sha256);
}

float rvmDownsampleRatio(int width, int height, int quality) {
    static const int target[3] = {320, 480, 640};
    const int t = target[std::clamp(quality, 0, 2)];
    return static_cast<float>(std::min(1.0, static_cast<double>(t) / std::max(1, std::max(width, height))));
}

MattingReport analyseMattes(const MattingJob& job, const std::function<void(int64_t, int64_t)>& progress,
                            const std::atomic<bool>* cancel) {
    const auto t0 = std::chrono::steady_clock::now();
    MattingReport rep;
    MatteStore store(cutoutKey(job.media, job.cutout));
    const int64_t first = std::max<int64_t>(0, job.firstFrame), last = std::max(first, job.lastFrame);
    const int64_t total = last - first + 1;
    // runs of frames still to analyse
    std::vector<std::pair<int64_t, int64_t>> runs;
    for (int64_t f = first; f <= last; ++f) {
        if (store.hasAnalysed(f)) {
            ++rep.framesCached;
            continue;
        }
        if (!runs.empty() && runs.back().second == f - 1) runs.back().second = f;
        else runs.push_back({f, f});
    }
    if (runs.empty()) {
        store.touch();
        if (progress) progress(total, total);
        return rep;
    }
    Matter matter(job.cutout, job.accelerator);
    rep.provider = matter.model().provider();
    rep.fallback = matter.fallback();
    VideoDecoder dec(job.media);
    const Rational fps = dec.info().frameRate.fps();
    int64_t done = rep.framesCached;
    const bool recurrent = isRvm(job.cutout);
    for (const auto& [a, b] : runs) {
        matter.reset();
        const int64_t start = recurrent ? std::max<int64_t>(0, a - 30) : a;   // settle the recurrent state first
        for (int64_t f = start; f <= b; ++f) {
            if (cancel && cancel->load()) {   // what was written stays valid; the next analysis continues from it
                store.touch();
                trimMatteCache(analysisCacheLimitBytes(), store.folder());
                rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                return rep;
            }
            const VideoFrame& vf = dec.frameAt(Time(f) / fps);
            const AlphaImage m = matter.run(vf.image);
            if (f < a) continue;
            store.write(f, m);
            ++rep.framesAnalysed;
            if (progress) progress(++done, total);
        }
    }
    store.touch();
    trimMatteCache(analysisCacheLimitBytes(), store.folder());   // the least recently used other sets go first
    rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return rep;
}

AlphaImage matteForPicture(const Image& picture, const AiCutout& cutout, Accelerator acc) {
    // one session kept for the inspector's previews (creating one costs about a second on DirectML)
    static std::mutex mutex;
    static std::unique_ptr<Matter> cached;
    static std::string cachedKey;
    const std::string key = cutout.model + "|" + std::to_string(cutout.quality) + "|" + acceleratorId(acc) + "|" +
                            cutoutModelFile(cutout).string();
    std::lock_guard lock(mutex);
    if (!cached || cachedKey != key) {
        cached.reset();
        cached = std::make_unique<Matter>(cutout, acc);
        cachedKey = key;
    }
    cached->reset();   // a single frame: no motion context
    return cached->run(picture);
}

std::pair<int64_t, int64_t> clipSourceFrames(const Clip& c, const MediaSource& src, const Track* track) {
    const Rational fps = src.frameRate ? src.frameRate->fps() : Rational(30);
    Time lo = c.sourceIn, hi = c.sourceOut();
    if (track) {   // transitions: the clip is shown before its start / after its end, from its handles
        Time from(0), to = c.duration;
        for (size_t i = 0; i < track->clips.size(); ++i) {
            std::optional<TransitionSpan> span;
            try {
                span = transitionSpan(*track, i);
            } catch (const fa::ModelError&) {   // the project model's error (not ai::ModelError)
                continue;   // a malformed transition is reported by validation, not here
            }
            if (!span) continue;
            if (span->to == &c) from = std::min(from, span->range.start - c.timelineStart);
            if (span->from == &c) to = std::max(to, span->range.end() - c.timelineStart);
        }
        for (const Time& local : {from, to}) {
            const Time s = sourceTimeAtLocal(c, local);
            lo = std::min(lo, s);
            hi = std::max(hi, s);
        }
    }
    const int64_t first = std::max<int64_t>(0, (lo * fps).floor());
    int64_t last = std::max(first, (hi * fps).ceil() - 1);
    if (src.duration) last = std::max(first, std::min(last, (*src.duration * fps).ceil() - 1));
    return {first, last};
}

bool clipInFrameRange(const Clip& clip, const Sequence& seq, int64_t fromFrame, int64_t toFrame) {
    const int64_t a = timeToFrame(clip.timelineStart, seq.frameRate, Rounding::Floor);
    const int64_t b = timeToFrame(clip.timelineEnd(), seq.frameRate, Rounding::Ceil);
    return b > fromFrame && (toFrame < 0 || a < toFrame);
}

std::vector<std::pair<int64_t, int64_t>> framesUsing(const Sequence& seq, int64_t fromFrame, int64_t toFrame,
                                                      const std::function<bool(const Clip&)>& uses) {
    std::vector<std::pair<int64_t, int64_t>> spans;
    for (const auto& t : seq.tracks) {
        if (t.kind != TrackKind::Video) continue;
        Time widen(0);
        for (const auto& c : t.clips)
            if (c.outTransition) widen = std::max(widen, c.outTransition->duration);
        const int64_t w = timeToFrame(widen, seq.frameRate, Rounding::Ceil);
        for (const auto& c : t.clips) {
            if (!uses(c)) continue;
            const int64_t a = std::max(fromFrame, timeToFrame(c.timelineStart, seq.frameRate, Rounding::Floor) - w);
            const int64_t b = std::min(toFrame, timeToFrame(c.timelineEnd(), seq.frameRate, Rounding::Ceil) + w);
            if (a < b) spans.emplace_back(a, b);
        }
    }
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<int64_t, int64_t>> merged;
    for (const auto& s : spans)
        if (!merged.empty() && s.first <= merged.back().second) merged.back().second = std::max(merged.back().second, s.second);
        else merged.push_back(s);
    return merged;
}

CutoutPreparation prepareCutouts(Project& project, const std::string& sequenceId, const std::filesystem::path& projectFile,
                                 const std::function<void(int64_t, int64_t)>& progress, const std::atomic<bool>* cancel,
                                 int64_t fromFrame, int64_t toFrame) {
    CutoutPreparation prep;
    Sequence* seq = nullptr;
    for (auto& s : project.sequences)
        if (sequenceId.empty() || s.id == sequenceId) {
            seq = &s;
            break;
        }
    if (!seq) return prep;
    struct Work {
        Clip* clip;
        std::filesystem::path media;
        int64_t first, last, missing;
    };
    std::vector<Work> work;
    int64_t total = 0;
    for (auto& t : seq->tracks) {
        if (t.kind != TrackKind::Video) continue;
        for (auto& c : t.clips) {
            if (!c.aiCutout.enabled || c.isGenerated() || !clipInFrameRange(c, *seq, fromFrame, toFrame)) continue;
            const MediaSource* src = project.findSource(c.sourceId);
            if (!src) continue;
            ++prep.clips;
            const auto media = resolveMediaPath(*src, projectFile);
            const ModelInfo& m = cutoutModel(c.aiCutout);
            requireModel(m);
            const std::string sha = m.files.front().sha256;
            if (c.aiCutout.modelSha.empty()) c.aiCutout.modelSha = sha;   // the installed model makes missing mattes
            const auto [first, last] = clipSourceFrames(c, *src, &t);
            auto missingFor = [&](const std::string& modelSha) {
                return (last - first + 1) - MatteStore(cutoutSetKey(media, c.aiCutout, modelSha)).count(first, last);
            };
            int64_t missing = missingFor(c.aiCutout.modelSha);
            if (missing > 0 && c.aiCutout.modelSha != sha) {   // made by another model version: analyse with this one
                c.aiCutout.modelSha = sha;
                missing = missingFor(sha);
            }
            work.push_back({&c, media, first, last, missing});
            total += missing;
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    int64_t done = 0;
    for (const Work& w : work) {
        if (w.missing <= 0) continue;
        MattingJob job{w.media, w.first, w.last, w.clip->aiCutout, Accelerator::Auto};
        const int64_t base = done;
        const MattingReport r = analyseMattes(job, [&](int64_t d, int64_t t) {
            (void)t;
            done = base + std::min<int64_t>(w.missing, std::max<int64_t>(0, d - ((w.last - w.first + 1) - w.missing)));
            if (progress) progress(done, total);
        }, cancel);
        prep.framesAnalysed += r.framesAnalysed;
        if (!r.provider.empty()) prep.provider = r.provider;
        if (cancel && cancel->load()) break;
    }
    prep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return prep;
}

}  // namespace fa::ai
