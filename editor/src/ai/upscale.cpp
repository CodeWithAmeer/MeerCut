#include "fa/ai/upscale.h"

#include "fa/ai/interpolate.h"
#include "fa/ai/matting.h"
#include "fa/ai/models.h"
#include "fa/media/media.h"
#include "fa/model/serialize.h"
#include "fa/render/frame_store.h"
#include "fa/render/timeline_eval.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>

namespace fa::ai {

namespace {

bool usesAiUpscale(const Clip& c) { return c.upscale.mode == UpscaleMode::Ai && !c.isGenerated(); }

const ModelInfo& esrgan() {
    const ModelInfo* m = findModel("realesrgan");
    if (!m) throw ModelError("Real-ESRGAN is not in the catalogue");
    return *m;
}

// The param / bin file indices of a model choice in the catalogue entry (x4plus; animevideov3 x2, x3, x4).
std::pair<size_t, int> fileIndexAndNetScale(const std::string& model, int scale) {
    if (model == "anime") {
        const int s = std::clamp(scale, 2, 4);
        return {static_cast<size_t>(2 + 2 * (s - 2)), s};
    }
    return {0, 4};
}

}  // namespace

std::string upscaleModelSha(const std::string& model, int scale) {
    const auto [i, ns] = fileIndexAndNetScale(model, scale);
    (void)ns;
    return esrgan().files[i + 1].sha256;   // the weights
}

Upscaler::Upscaler(const std::string& model, int scale, Accelerator acc, int tile, int pad) : scale_(std::clamp(scale, 2, 4)) {
    if (pad >= 0) pad_ = pad;
    const ModelInfo& m = esrgan();
    requireModel(m);
    const auto [i, ns] = fileIndexAndNetScale(model, scale_);
    netScale_ = ns;
    model_ = std::make_unique<NcnnModel>(modelFilePath(m, i), modelFilePath(m, i + 1), acc);
    if (tile > 0) {
        tile_ = tile;
    } else {
        // tiles sized to the device: the large model's activations at 4x are what fills memory
        const uint32_t budget = model_->onGpu() ? model_->gpuMemoryBudgetMb() : 4096;
        const bool big = model != "anime";
        tile_ = budget >= 3000 ? (big ? 192 : 384) : budget >= 1500 ? (big ? 128 : 256) : (big ? 64 : 128);
    }
}

Upscaler::~Upscaler() = default;
const std::string& Upscaler::provider() const { return model_->provider(); }
bool Upscaler::onGpu() const { return model_->onGpu(); }

Image Upscaler::upscale(const Image& src, const std::atomic<bool>* cancel) {
    const int w = src.width, h = src.height, ns = netScale_;
    const int pad = pad_;   // overlap: each tile sees this much context on every side; only its centre is kept
    Image big(w * ns, h * ns);
    for (int ty = 0; ty < h; ty += tile_)
        for (int tx = 0; tx < w; tx += tile_) {
            if (cancel && cancel->load()) return {};
            const int x0 = tx, y0 = ty, x1 = std::min(w, tx + tile_), y1 = std::min(h, ty + tile_);
            const int ax = std::max(0, x0 - pad), ay = std::max(0, y0 - pad);
            const int bx = std::min(w, x1 + pad), by = std::min(h, y1 + pad);
            const int iw = bx - ax, ih = by - ay;
            Tensor in;
            in.shape = {3, ih, iw};
            in.data.resize(static_cast<size_t>(3) * iw * ih);
            for (int y = 0; y < ih; ++y)
                for (int x = 0; x < iw; ++x) {
                    const uint8_t* p = &src.pixels[(static_cast<size_t>(ay + y) * w + ax + x) * 4];
                    for (int c = 0; c < 3; ++c) in.data[static_cast<size_t>(c) * iw * ih + static_cast<size_t>(y) * iw + x] = p[c] / 255.0f;
                }
            const Tensor out = model_->run({{"data", &in}}, {"output"}).front();
            const int ow = static_cast<int>(out.shape[2]), oh = static_cast<int>(out.shape[1]);
            if (ow != iw * ns || oh != ih * ns) throw ModelError("Real-ESRGAN returned an unexpected size");
            for (int y = (y0 - ay) * ns; y < (y1 - ay) * ns; ++y)
                for (int x = (x0 - ax) * ns; x < (x1 - ax) * ns; ++x) {
                    uint8_t* d = &big.pixels[(static_cast<size_t>(ay * ns + y) * (w * ns) + ax * ns + x) * 4];
                    for (int c = 0; c < 3; ++c) {
                        const float v = out.data[static_cast<size_t>(c) * ow * oh + static_cast<size_t>(y) * ow + x];
                        d[c] = static_cast<uint8_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
                    }
                    d[3] = 255;
                }
        }
    Image result = netScale_ == scale_ ? std::move(big) : resizeLanczos(big, w * scale_, h * scale_);
    // alpha: not part of the model; the source's, Lanczos-resized
    bool opaque = true;
    for (size_t i = 3; i < src.pixels.size() && opaque; i += 4) opaque = src.pixels[i] == 255;
    if (!opaque) {
        const Image a = resizeLanczos(src, result.width, result.height);
        for (size_t i = 3; i < result.pixels.size(); i += 4) result.pixels[i] = a.pixels[i];
    }
    return result;
}

double upscaleSecondsPerFrame(const std::string& model, int scale, double megapixels, bool gpu) {
    // per source megapixel, RTX 2050 (Vulkan) / i5-12450H (6 threads); the general model always runs at 4x
    const double mp = std::max(0.01, megapixels);
    // measured at 640 x 360 and 1280 x 720 (AI_MODELS.md): x4plus 16-18 s per megapixel on the RTX 2050, ~330 s on the
    // CPU (minutes per frame); animevideov3 0.49 / 0.58 / 0.73 s at 2x / 3x / 4x on the GPU (its 2x and 3x run the 4x
    // network inside the model, the larger output costs the rest), ~9.7 s on the CPU
    constexpr double kGeneralGpu = 18.0, kGeneralCpu = 330.0, kAnimeCpu = 9.7;
    constexpr double kAnimeGpu[3] = {0.49, 0.58, 0.73};
    if (model != "anime") return mp * (gpu ? kGeneralGpu : kGeneralCpu);
    return mp * (gpu ? kAnimeGpu[std::clamp(scale, 2, 4) - 2] : kAnimeCpu);
}

WorkEstimate estimateUpscales(const Project& project, const Sequence& seq, int64_t fromFrame, int64_t toFrame,
                              const std::filesystem::path& projectFile) {
    WorkEstimate w;
    if (!findModel("realesrgan")) return w;
    // the device is asked only when there is work (the first probe takes seconds; this runs on the GUI thread)
    int gpu = -1;   // -1: not asked yet
    std::map<std::string, std::string> identities;
    std::set<std::string> seen;
    for (const auto& [spanFrom, spanTo] : framesUsing(seq, fromFrame, toFrame, usesAiUpscale))
        for (int64_t f = spanFrom; f < spanTo; ++f)
            for (const TrackSample& s : evaluateVideoFrame(seq, f, &project))
                for (const ClipSample* c : {&s.a, s.b ? &*s.b : nullptr}) {
                    if (!c || !c->clip || c->clip->upscale.mode != UpscaleMode::Ai || c->clip->isGenerated()) continue;
                    const MediaSource* src = project.findSource(c->clip->sourceId);
                    if (!src || !src->frameRate || !src->width) continue;
                    const auto [tw, th] = upscaleTarget(*c->clip, src->width, src->height, seq.width, seq.height);
                    if (tw == 0) continue;
                    const ClipUpscale& u = c->clip->upscale;
                    const auto media = resolveMediaPath(*src, projectFile);
                    const std::u8string m8 = media.u8string();
                    std::string& id = identities[std::string(m8.begin(), m8.end())];
                    if (id.empty()) id = matteMediaIdentity(media);
                    const FrameStore store(FrameSetKey{"upscale", id, upscaleModelSha(u.model, u.scale),
                                                       u.model + "|" + std::to_string(u.scale) + "|" + std::to_string(tw) + "x" +
                                                           std::to_string(th) + "|" + std::to_string(u.deflicker)});
                    const Rational fps = src->frameRate->fps();
                    std::vector<Time> times{c->sourceTime};
                    if (c->blendTime) times.push_back(*c->blendTime);
                    for (const Time& t : times) {
                        const std::string name = std::to_string((t * fps).floor());
                        if (!seen.insert(store.folder().filename().string() + "/" + name).second || store.has(name)) continue;
                        ++w.frames;
                        if (gpu < 0) gpu = aiCapabilities().vulkan && preferredVulkanGpu() >= 0 ? 1 : 0;
                        // + decoding, flicker reduction, compressing and writing the enlarged frame (~0.05 s per
                        // megapixel of output, measured end to end)
                        w.seconds += upscaleSecondsPerFrame(u.model, u.scale, src->width * double(src->height) / 1e6, gpu == 1) +
                                     0.05 * tw * double(th) / 1e6;
                    }
                }
    w.cpu = gpu == 0;
    return w;
}

UpscalePreparation prepareUpscales(const Project& project, const Sequence& seq, int64_t fromFrame, int64_t toFrame,
                                   const std::filesystem::path& projectFile,
                                   const std::function<void(int64_t, int64_t)>& progress, const std::atomic<bool>* cancel,
                                   Accelerator acc) {
    const auto t0 = std::chrono::steady_clock::now();
    UpscalePreparation prep;
    // what the range shows: per (media, settings) the source times of every clip sample (and blend partner)
    struct Group {
        std::filesystem::path media;
        const Clip* clip = nullptr;
        std::set<Time> times;
    };
    std::map<std::string, Group> groups;
    std::set<std::string> clipIds;
    for (const auto& [spanFrom, spanTo] : framesUsing(seq, fromFrame, toFrame, usesAiUpscale))
        for (int64_t f = spanFrom; f < spanTo; ++f)
            for (const TrackSample& s : evaluateVideoFrame(seq, f, &project))
                for (const ClipSample* c : {&s.a, s.b ? &*s.b : nullptr}) {
                    if (!c || !c->clip || c->clip->upscale.mode != UpscaleMode::Ai || c->clip->isGenerated()) continue;
                    const MediaSource* src = project.findSource(c->clip->sourceId);
                    if (!src) continue;
                    const auto media = resolveMediaPath(*src, projectFile);
                    const auto [tw, th] = upscaleTarget(*c->clip, src->width, src->height, seq.width, seq.height);
                    if (tw == 0) continue;   // shown at or below its own size: nothing to upscale
                    const std::u8string m8 = media.u8string();
                    Group& g = groups[std::string(m8.begin(), m8.end()) + "|" + c->clip->upscale.model + "|" +
                                      std::to_string(c->clip->upscale.scale) + "|" + std::to_string(tw) + "x" + std::to_string(th) +
                                      "|" + std::to_string(c->clip->upscale.deflicker)];
                    g.media = media;
                    g.clip = c->clip;
                    g.times.insert(c->sourceTime);
                    if (c->blendTime) g.times.insert(*c->blendTime);
                    clipIds.insert(c->clip->id);
                }
    prep.clips = static_cast<int>(clipIds.size());
    registerModelIdentities();
    for (const auto& [k, g] : groups) prep.needed += static_cast<int64_t>(g.times.size());
    int64_t done = 0;
    std::unique_ptr<Upscaler> up;
    std::string upKey;
    for (const auto& [k, g] : groups) {
        const ClipUpscale& u = g.clip->upscale;
        VideoDecoder dec(g.media);
        const auto [tw, th] = upscaleTarget(*g.clip, dec.info().width, dec.info().height, seq.width, seq.height);
        FrameStore store(FrameSetKey{"upscale", matteMediaIdentity(g.media), upscaleModelSha(u.model, u.scale),
                                     u.model + "|" + std::to_string(u.scale) + "|" + std::to_string(tw) + "x" +
                                         std::to_string(th) + "|" + std::to_string(u.deflicker)});
        Image prevSrc, prevOut;
        int64_t prevIndex = -2;
        for (const Time& t : g.times) {
            if (cancel && cancel->load()) {
                prep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                return prep;
            }
            const VideoFrame& vf = dec.frameAt(t);
            const std::string name = std::to_string(vf.index);
            if (Image stored; store.read(name, stored)) {   // read, not only looked up: a damaged file is made again
                // the chain continues from the stored result (flicker reduction needs the previous output)
                prevOut = std::move(stored);
                prevSrc = vf.image;
                prevIndex = vf.index;
                if (progress) progress(++done, prep.needed);
                continue;
            }
            const std::string want = u.model + "|" + std::to_string(u.scale);
            if (!up || upKey != want) {
                up = std::make_unique<Upscaler>(u.model, u.scale, acc);
                upKey = want;
                prep.provider = up->provider();
            }
            Image out = up->upscale(vf.image, cancel);
            if (out.pixels.empty()) break;   // cancelled inside the picture
            if (out.width != tw || out.height != th) out = resizeLanczos(out, tw, th);
            // flicker: where the source did not change since the previous frame, keep the previous result
            if (u.deflicker > 0 && prevIndex == vf.index - 1 && prevOut.width == tw && prevSrc.width == vf.image.width) {
                const double tau = 6.0;   // source levels
                const int sw = vf.image.width, sh = vf.image.height;
                for (int y = 0; y < th; ++y) {
                    const int sy = std::min(sh - 1, y * sh / th);
                    for (int x = 0; x < tw; ++x) {
                        const int sx = std::min(sw - 1, x * sw / tw);
                        const size_t si = (static_cast<size_t>(sy) * sw + sx) * 4;
                        int d = 0;
                        for (int c = 0; c < 3; ++c) d = std::max(d, std::abs(vf.image.pixels[si + c] - prevSrc.pixels[si + c]));
                        const double wgt = u.deflicker * std::max(0.0, 1.0 - d / tau);
                        if (wgt <= 0) continue;
                        uint8_t* o = &out.pixels[(static_cast<size_t>(y) * tw + x) * 4];
                        const uint8_t* p = &prevOut.pixels[(static_cast<size_t>(y) * tw + x) * 4];
                        for (int c = 0; c < 3; ++c) o[c] = static_cast<uint8_t>(std::lround(o[c] + wgt * (p[c] - o[c])));
                    }
                }
            }
            store.write(name, out);
            ++prep.computed;
            prevSrc = vf.image;
            prevOut = std::move(out);
            prevIndex = vf.index;
            if (progress) progress(++done, prep.needed);
        }
        store.touch();
        trimFrameStores("upscale", analysisCacheLimitBytes(), store.folder());
    }
    prep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return prep;
}

}  // namespace fa::ai
