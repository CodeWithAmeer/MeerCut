#include "fa/ai/interpolate.h"

#include "fa/ai/matting.h"
#include "fa/ai/models.h"
#include "fa/ai/upscale.h"
#include "fa/media/media.h"
#include "fa/model/serialize.h"
#include "fa/render/frame_store.h"
#include "fa/render/timeline_eval.h"
#include "fa/vision/scene_detect.h"

#include <layer.h>
#include <net.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>

namespace fa::ai {

namespace {

bool usesNeural(const Clip& c) { return c.speed.interpolation == FrameInterpolation::Neural && !c.isGenerated(); }

// RIFE's custom layer "rife.Warp": the image sampled at (x + flow_x, y + flow_y), bilinear, coordinates clamped to
// the picture (the operation of rife-ncnn-vulkan's warp layer, written here from its definition). CPU only; with
// Vulkan on, ncnn runs this layer on the CPU between the GPU layers.
class RifeWarp : public ncnn::Layer {
public:
    RifeWarp() {
        one_blob_only = false;
        support_inplace = false;
    }
    int forward(const std::vector<ncnn::Mat>& bottom, std::vector<ncnn::Mat>& top, const ncnn::Option& opt) const override {
        const ncnn::Mat& image = bottom[0];
        const ncnn::Mat& flow = bottom[1];
        const int w = image.w, h = image.h, channels = image.c;
        ncnn::Mat& out = top[0];
        out.create(w, h, channels, image.elemsize, opt.blob_allocator);
        if (out.empty()) return -100;
        const float* fx = flow.channel(0);
        const float* fy = flow.channel(1);
#pragma omp parallel for num_threads(opt.num_threads)
        for (int q = 0; q < channels; ++q) {
            const float* src = image.channel(q);
            float* dst = out.channel(q);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    const float sx = x + fx[i], sy = y + fy[i];
                    const float x0f = std::floor(sx), y0f = std::floor(sy);
                    const float ax = sx - x0f, ay = sy - y0f;
                    const int x0 = std::clamp(static_cast<int>(x0f), 0, w - 1), x1 = std::clamp(static_cast<int>(x0f) + 1, 0, w - 1);
                    const int y0 = std::clamp(static_cast<int>(y0f), 0, h - 1), y1 = std::clamp(static_cast<int>(y0f) + 1, 0, h - 1);
                    const float v00 = src[y0 * w + x0], v01 = src[y0 * w + x1], v10 = src[y1 * w + x0], v11 = src[y1 * w + x1];
                    dst[i] = (1 - ay) * ((1 - ax) * v00 + ax * v01) + ay * ((1 - ax) * v10 + ax * v11);
                }
        }
        return 0;
    }
};

ncnn::Layer* rifeWarpCreator(void*) { return new RifeWarp; }

const ModelInfo& rifeModel() {
    const ModelInfo* m = findModel("rife-v4.6");
    if (!m) throw ModelError("the RIFE model is not in the catalogue");
    return *m;
}

}  // namespace

NeuralInterpolator::NeuralInterpolator(Accelerator acc) {
    const ModelInfo& m = rifeModel();
    requireModel(m);
    model_ = std::make_unique<NcnnModel>(modelFilePath(m, 0), modelFilePath(m, 1), acc, -1, [](void* net) {
        static_cast<ncnn::Net*>(net)->register_custom_layer("rife.Warp", rifeWarpCreator);
    });
}

NeuralInterpolator::~NeuralInterpolator() = default;

const std::string& NeuralInterpolator::provider() const { return model_->provider(); }
bool NeuralInterpolator::onGpu() const { return model_->onGpu(); }

Image NeuralInterpolator::interpolate(const Image& a, const Image& b, double t) {
    if (a.width != b.width || a.height != b.height || a.width <= 0) throw ModelError("interpolation needs two frames of one size");
    t = std::clamp(t, 0.0, 1.0);
    const int w = a.width, h = a.height;
    // RIFE v4 works on sizes that are multiples of 32: the frames are padded right and bottom with black
    const int pw = (w + 31) / 32 * 32, ph = (h + 31) / 32 * 32;
    auto planes = [&](const Image& img) {
        Tensor x;
        x.shape = {3, ph, pw};
        x.data.assign(static_cast<size_t>(3) * pw * ph, 0.0f);
        for (int y = 0; y < h; ++y)
            for (int xx = 0; xx < w; ++xx) {
                const uint8_t* p = &img.pixels[(static_cast<size_t>(y) * w + xx) * 4];
                const size_t i = static_cast<size_t>(y) * pw + xx;
                for (int c = 0; c < 3; ++c) x.data[static_cast<size_t>(c) * pw * ph + i] = p[c] / 255.0f;
            }
        return x;
    };
    const Tensor in0 = planes(a), in1 = planes(b);
    Tensor step;
    step.shape = {1, ph, pw};
    step.data.assign(static_cast<size_t>(pw) * ph, static_cast<float>(t));
    const auto out = model_->run({{"in0", &in0}, {"in1", &in1}, {"in2", &step}}, {"out0"});
    const Tensor& o = out.front();
    if (o.shape.size() != 3 || o.shape[0] < 3 || o.shape[1] < h || o.shape[2] < w) throw ModelError("RIFE returned an unexpected size");
    const int ow = static_cast<int>(o.shape[2]), oh = static_cast<int>(o.shape[1]);
    Image r(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * ow + x;
            uint8_t* p = &r.pixels[(static_cast<size_t>(y) * w + x) * 4];
            for (int c = 0; c < 3; ++c)
                p[c] = static_cast<uint8_t>(std::clamp(o.data[static_cast<size_t>(c) * ow * oh + i] * 255.0f + 0.5f, 0.0f, 255.0f));
            // alpha is not part of the model: mixed linearly
            const size_t k = (static_cast<size_t>(y) * w + x) * 4 + 3;
            p[3] = static_cast<uint8_t>(std::lround(a.pixels[k] * (1 - t) + b.pixels[k] * t));
        }
    return r;
}

std::string interpolationModelSha() {
    const ModelInfo& m = rifeModel();
    return m.files.size() > 1 ? m.files[1].sha256 : m.files.front().sha256;   // the weights
}

void registerModelIdentities() {
    setModelIdentity("rife", interpolationModelSha());
    for (const char* id : {"rvm-mobilenetv3", "birefnet-general-tiny"})   // Background Removal without a recorded model
        if (const ModelInfo* m = findModel(id); m && !m->files.empty()) setModelIdentity(std::string("cutout:") + id, m->files.front().sha256);
    for (const char* model : {"general", "anime"})
        for (int scale : {2, 3, 4}) setModelIdentity(std::string("upscale:") + model + ":" + std::to_string(scale), upscaleModelSha(model, scale));
}

double interpolationSecondsPerFrame(double megapixels, bool gpu) {
    // RIFE 4.6 (ncnn) per megapixel, warm: RTX 2050 (Vulkan) 0.22-0.24 s, i5-12450H (6 threads) 0.78-0.95 s, measured
    // at 640 x 360, 1280 x 720 and 1920 x 1080 (AI_MODELS.md)
    constexpr double kGpu = 0.23, kCpu = 0.95;
    return std::max(0.01, megapixels) * (gpu ? kGpu : kCpu);
}

WorkEstimate estimateNeuralFrames(const Project& project, const Sequence& seq, int64_t fromFrame, int64_t toFrame,
                                  const std::filesystem::path& projectFile) {
    WorkEstimate w;
    const ModelInfo* m = findModel("rife-v4.6");
    if (!m) return w;
    const std::string sha = interpolationModelSha();
    // the device is asked only when there is work (the first probe takes seconds; this runs on the GUI thread)
    int gpu = -1;   // -1: not asked yet
    std::map<std::string, std::string> identities;
    std::set<std::string> seen;
    for (const auto& [spanFrom, spanTo] : framesUsing(seq, fromFrame, toFrame, usesNeural))
        for (int64_t f = spanFrom; f < spanTo; ++f)
            for (const TrackSample& s : evaluateVideoFrame(seq, f, &project))
                for (const ClipSample* c : {&s.a, s.b ? &*s.b : nullptr}) {
                    if (!c || !c->clip || c->clip->speed.interpolation != FrameInterpolation::Neural || !c->blendTime ||
                        c->blendWeight <= 0.0 || c->clip->isGenerated())
                        continue;
                    const MediaSource* src = project.findSource(c->clip->sourceId);
                    if (!src || !src->frameRate) continue;
                    const Rational fps = src->frameRate->fps();
                    const auto media = resolveMediaPath(*src, projectFile);
                    const std::u8string m8 = media.u8string();
                    std::string& id = identities[std::string(m8.begin(), m8.end())];
                    if (id.empty()) id = matteMediaIdentity(media);
                    const std::string name =
                        interpolationFrameName((c->sourceTime * fps).floor(), (*c->blendTime * fps).floor(), c->blendWeight);
                    if (!seen.insert(id + "/" + name).second) continue;
                    if (FrameStore(FrameSetKey{"rife", id, sha, "v1"}).has(name)) continue;
                    ++w.frames;
                    if (gpu < 0) gpu = aiCapabilities().vulkan && preferredVulkanGpu() >= 0 ? 1 : 0;
                    // + decoding the pair, compressing and writing the frame: ~0.25 s per megapixel (measured end to
                    // end: 36 frames of 1280 x 720 prepared in 19.5 s including loading the model)
                    const double mp = src->width * double(src->height) / 1e6;
                    w.seconds += interpolationSecondsPerFrame(mp, gpu == 1) + 0.25 * mp;
                }
    w.cpu = gpu == 0;
    return w;
}

NeuralPreparation prepareNeuralFrames(const Project& project, const Sequence& seq, int64_t fromFrame, int64_t toFrame,
                                      const std::filesystem::path& projectFile,
                                      const std::function<void(int64_t, int64_t)>& progress, const std::atomic<bool>* cancel,
                                      Accelerator acc) {
    const auto t0 = std::chrono::steady_clock::now();
    NeuralPreparation prep;
    struct Need {
        std::filesystem::path media;
        Time a, b;
        double t;
    };
    std::vector<Need> needs;
    for (const auto& [spanFrom, spanTo] : framesUsing(seq, fromFrame, toFrame, usesNeural))
        for (int64_t f = spanFrom; f < spanTo; ++f)
            for (const TrackSample& s : evaluateVideoFrame(seq, f, &project))
                for (const ClipSample* c : {&s.a, s.b ? &*s.b : nullptr}) {
                    if (!c || !c->clip || c->clip->speed.interpolation != FrameInterpolation::Neural || !c->blendTime ||
                        c->blendWeight <= 0.0 || c->clip->isGenerated())
                        continue;
                    const MediaSource* src = project.findSource(c->clip->sourceId);
                    if (src) needs.push_back({resolveMediaPath(*src, projectFile), c->sourceTime, *c->blendTime, c->blendWeight});
                }
    prep.needed = static_cast<int64_t>(needs.size());
    registerModelIdentities();
    if (needs.empty()) return prep;
    const std::string sha = interpolationModelSha();
    // Shot changes: the Scene Detection of checkpoint D over the source range each media's pairs come from (with a
    // second of context on both sides for its local levels); a pair that spans a cut gets no invented frame.
    std::map<std::filesystem::path, std::set<int64_t>> cuts;
    {
        std::map<std::filesystem::path, std::pair<Time, Time>> spans;
        for (const Need& n : needs) {
            auto [it, fresh] = spans.try_emplace(n.media, n.a, n.b);
            if (!fresh) {
                it->second.first = std::min({it->second.first, n.a, n.b});
                it->second.second = std::max({it->second.second, n.a, n.b});
            }
        }
        for (const auto& [media, span] : spans) {
            VideoDecoder probe(media);
            const Rational fps = probe.info().frameRate.fps();
            const int64_t second = fps.ceil();
            const int64_t first = std::max<int64_t>(0, (std::min(span.first, span.second) * fps).floor() - second);
            const int64_t last = (std::max(span.first, span.second) * fps).ceil() + second;
            const vision::SceneResult scenes = vision::detectScenes(media, vision::SceneOptions{}, first, last, {}, cancel);
            for (const vision::SceneBoundary& b : scenes.boundaries)
                if (b.kind == vision::BoundaryKind::Cut) cuts[media].insert(b.frame);
        }
    }
    std::unique_ptr<NeuralInterpolator> rife;
    std::map<std::filesystem::path, std::unique_ptr<VideoDecoder>> decoders;
    int64_t done = 0;
    std::set<std::string> seen;
    Image check;
    for (const Need& n : needs) {
        if (cancel && cancel->load()) break;
        auto& dec = decoders[n.media];
        if (!dec) dec = std::make_unique<VideoDecoder>(n.media);
        FrameStore store(FrameSetKey{"rife", matteMediaIdentity(n.media), sha, "v1"});
        const VideoFrame& fa = dec->frameAt(n.a);
        const Image a = fa.image;
        const int64_t ia = fa.index;
        const VideoFrame& fb = dec->frameAt(n.b);
        const int64_t ib = fb.index;
        const std::string name = interpolationFrameName(ia, ib, n.t);
        // already there (a damaged file is computed again: it is read, not only looked up)
        const bool cached = store.isMarked(name) || store.read(name, check);
        if (!seen.insert(store.folder().filename().string() + "/" + name).second || cached) {
            if (progress) progress(++done, prep.needed);
            continue;
        }
        const auto& mc = cuts[n.media];
        const auto k = mc.upper_bound(std::min(ia, ib));
        if (k != mc.end() && *k <= std::max(ia, ib)) {   // a shot change between the two frames: nothing is invented
            store.mark(name);
            ++prep.sceneCuts;
        } else {
            if (!rife) {
                rife = std::make_unique<NeuralInterpolator>(acc);
                prep.provider = rife->provider();
            }
            store.write(name, rife->interpolate(a, fb.image, quantiseStep(n.t) / 256.0));
            ++prep.computed;
        }
        store.touch();
        if (progress) progress(++done, prep.needed);
    }
    trimFrameStores("rife", analysisCacheLimitBytes());
    prep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return prep;
}

}  // namespace fa::ai
