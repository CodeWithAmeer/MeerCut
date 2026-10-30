#include "fa/render/renderer.h"

#include "fa/media/export_format.h"
#include "fa/media/hdr.h"
#include "fa/model/serialize.h"
#include "fa/model/stabilize_plan.h"
#include "fa/model/time_map.h"
#include "fa/render/audio_mix.h"
#include "fa/render/effects.h"
#include "fa/render/frame_store.h"
#include "fa/render/matte_cache.h"
#include "fa/render/transitions.h"
#include "fa/vision/flow.h"
#include "parallel.h"

#include <chrono>
#include <cmath>
#include <deque>
#include <cstdio>
#include <fstream>
#include <algorithm>
#include <mutex>
#include <numbers>
#include <set>

namespace fa {

namespace {


std::string clipContext(const Sequence& seq, int64_t frame, const TrackSample* s, const ClipSample* c,
                        const MediaSource* src) {
    std::string out = "frame " + std::to_string(frame) + " (" + formatTimecode(frame, seq.frameRate) + ")";
    if (s && s->track) out += ", track " + s->track->id;
    if (c && c->clip) out += ", clip " + c->clip->id + " at source time " + c->sourceTime.toString() + " s";
    if (src) out += ", media '" + src->path + "'";
    return out;
}

const Sequence& pickSequence(const Project& p, const std::string& id) {
    if (p.sequences.empty()) throw RenderError("project has no sequences");
    if (id.empty()) return p.sequences.front();
    const Sequence* s = p.findSequence(id);
    if (!s) throw RenderError("sequence '" + id + "' not found");
    return *s;
}

}  // namespace

// ---- DecoderPool -----------------------------------------------------------------------------

struct DecoderPool::Entry {
    std::unique_ptr<VideoDecoder> decoder;
    std::filesystem::path path;
    uint64_t fileKey = 0;   // path, size and modification time of the file the decoder opened
    uint64_t lastUse = 0;
    // M5 reverse playback: a decoder only steps forward cheaply, so frames are decoded forward in chunks and
    // served backwards (one seek per chunk instead of one per frame); bounded to ~160 MB per clip.
    struct Cached {
        Time time;
        Image image;
    };
    std::map<int64_t, Cached> back;
    size_t frameBytes = 0;
    // M7 Background Removal: the media's identity (matte cache key) and the last frame with its matte applied
    std::string identity;
    Image cutout;
    int64_t cutoutFrame = -1;
    uint64_t cutoutParams = 0;
    const Image* cutoutFrom = nullptr;
    bool cutoutOk = false;
    // M7 Video Upscaling: the last frame enlarged (Lanczos at draw time, or prepared by Real-ESRGAN)
    Image upscaled;
    int64_t upscaledFrame = -1;
    std::string upscaledKey;
    const Image* upscaledFrom = nullptr;
    bool upscaledOk = false;
    bool upscaledPlain = false;   // Lanczos with nothing to enlarge at this (preview) size: the decoded frame
};

namespace {

uint64_t cutoutParamsHash(const AiCutout& c) {
    std::string s = c.model + "|" + c.modelSha + "|" + std::to_string(c.quality) + "|" + std::to_string(c.threshold) + "|" +
                    std::to_string(c.edge) + "|" + std::to_string(c.feather) + "|" + std::to_string(c.spill) + "|" +
                    std::to_string(c.smoothing);
    if (c.background) s += "|" + std::to_string(c.background->r) + "," + std::to_string(c.background->g) + "," + std::to_string(c.background->b);
    uint64_t h = 1469598103934665603ull;
    for (unsigned char ch : s) h = (h ^ ch) * 1099511628211ull;
    return h;
}

}  // namespace

DecoderPool::DecoderPool(std::filesystem::path projectFile, size_t maxOpenDecoders,
                         VideoDecoder::Options decoderOptions)
    : projectFile_(std::move(projectFile)), maxOpen_(maxOpenDecoders ? maxOpenDecoders : 1),
      decoderOptions_(decoderOptions) {}

std::filesystem::path DecoderPool::assetDir() const {   // like resolveMediaPath
    return projectFile_.has_parent_path() ? projectFile_.parent_path() : std::filesystem::current_path();
}

DecoderPool::~DecoderPool() = default;

const MediaInfo& DecoderPool::mediaInfo(const MediaSource& source) {
    auto it = probes_.find(source.id);
    if (it == probes_.end()) it = probes_.emplace(source.id, probeMedia(resolveMediaPath(source, projectFile_))).first;
    return it->second;
}

int64_t DecoderPool::totalSeeks() const {
    int64_t n = closedSeeks_;
    for (const auto& [_, e] : decoders_) n += e->decoder->seekCount();
    return n;
}

FrameProvider::Result DecoderPool::frame(const Clip& clip, const MediaSource& source, const Time& sourceTime) {
    const auto path = resolveMediaPath(source, projectFile_);
    auto it = decoders_.find(clip.id);
    if (it != decoders_.end() && it->second->path != path) {   // media relinked
        closedSeeks_ += it->second->decoder->seekCount();
        decoders_.erase(it);
        it = decoders_.end();
    }
    if (it == decoders_.end()) {
        while (decoders_.size() >= maxOpen_) {   // evict least recently used
            auto lru = decoders_.begin();
            for (auto j = decoders_.begin(); j != decoders_.end(); ++j)
                if (j->second->lastUse < lru->second->lastUse) lru = j;
            closedSeeks_ += lru->second->decoder->seekCount();
            decoders_.erase(lru);
        }
        auto e = std::make_unique<Entry>();
        e->decoder = std::make_unique<VideoDecoder>(path, decoderOptions_);
        e->path = path;
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        const auto mtime = std::filesystem::last_write_time(path, ec).time_since_epoch().count();
        e->fileKey = std::hash<std::wstring>{}(path.wstring()) ^ (static_cast<uint64_t>(size) * 0x9E3779B97F4A7C15ULL) ^
                     (static_cast<uint64_t>(mtime) * 0xC2B2AE3D27D4EB4FULL);
        it = decoders_.emplace(clip.id, std::move(e)).first;
    }
    Entry& e = *it->second;
    e.lastUse = ++tick_;

    Result r;
    r.requestedTime = sourceTime;
    Time t = sourceTime;
    if (t < Time(0)) {
        t = Time(0);
        r.clamped = true;
    }
    const auto& vi = e.decoder->info();
    if (vi.duration && t >= *vi.duration) {   // past the last frame: hold it (no far-forward seeks)
        t = *vi.duration - vi.frameRate.frameDuration() * Rational(1, 2);
        r.clamped = true;
    }
    if (clip.speed.reverse && !clip.speed.freeze) {   // M5: reversed clips read backwards through a chunk cache
        const Rational fps = vi.frameRate.fps();
        const int64_t idx = std::max<int64_t>(0, (t * fps).floor());
        auto hit = e.back.find(idx);
        if (hit == e.back.end()) {
            const size_t bytes = e.frameBytes ? e.frameBytes : static_cast<size_t>(1920) * 1080 * 4;
            const int64_t chunk = std::clamp<int64_t>(static_cast<int64_t>((160u << 20) / bytes), 4, 64);
            e.back.clear();
            for (int64_t i = std::max<int64_t>(0, idx - chunk + 1); i <= idx; ++i) {
                const VideoFrame& f = e.decoder->frameAt(Time(i) / fps);
                e.frameBytes = f.image.pixels.size();
                e.back[f.index] = Entry::Cached{f.time, f.image};
            }
            hit = e.back.find(idx);
            if (hit == e.back.end()) hit = std::prev(e.back.end());   // variable frame rate: the last one decoded
        }
        r.image = &hit->second.image;
        r.frameTime = hit->second.time;
        r.frameIndex = hit->first;
    } else {
        e.back.clear();
        const VideoFrame& f = e.decoder->frameAt(t);
        r.image = &f.image;
        r.frameTime = f.time;
        r.frameIndex = f.index;
    }
    uint64_t upscaleKey = 0;
    if (const ClipUpscale& up = clip.upscale; up.mode != UpscaleMode::Off && canvasW_ > 0) {   // M7: before the cutout
        const VideoStreamInfo& vi2 = e.decoder->info();
        const auto [tw, th] = upscaleTarget(clip, vi2.width, vi2.height, canvasW_, canvasH_);
        if (tw > 0) {
            const std::string key = std::to_string(static_cast<int>(up.mode)) + "|" + up.model + "|" + std::to_string(up.scale) +
                                    "|" + std::to_string(tw) + "x" + std::to_string(th) + "|" + std::to_string(up.deflicker);
            upscaleKey = std::hash<std::string>{}(key) | 1;
            // (a frame that was missing is looked up again: it may have been prepared since, without any edit)
            if (!e.upscaledOk || e.upscaledFrame != r.frameIndex || e.upscaledKey != key || e.upscaledFrom != r.image) {
                e.upscaledOk = false;
                e.upscaledPlain = false;
                // a preview decoder delivers smaller pictures: the enlarged frame is drawn at that size class too
                int dw = tw, dh = th;
                if (decoderOptions_.maxWidth > 0 && decoderOptions_.maxHeight > 0) {
                    const double k = std::min({1.0, double(decoderOptions_.maxWidth) / tw, double(decoderOptions_.maxHeight) / th});
                    dw = std::max(2, static_cast<int>(tw * k));
                    dh = std::max(2, static_cast<int>(th * k));
                }
                if (up.mode == UpscaleMode::Lanczos) {
                    // at a small preview size there may be nothing to enlarge: the decoded frame is drawn as it is
                    if (dw > r.image->width) e.upscaled = resizeLanczos(*r.image, dw, dh);
                    else e.upscaledPlain = true;
                    e.upscaledOk = true;
                } else {   // the prepared Real-ESRGAN frame (also when the preview is small: its detail is the export's)
                    if (e.identity.empty()) e.identity = matteMediaIdentity(path);
                    const std::string sha = modelIdentity("upscale:" + up.model + ":" + std::to_string(up.scale));
                    const FrameStore store(FrameSetKey{"upscale", e.identity, sha,
                                                       up.model + "|" + std::to_string(up.scale) + "|" + std::to_string(tw) +
                                                           "x" + std::to_string(th) + "|" + std::to_string(up.deflicker)});
                    Image stored;
                    if (!sha.empty() && store.read(std::to_string(r.frameIndex), stored)) {
                        e.upscaled = stored.width == dw && stored.height == dh ? std::move(stored) : resizeBilinear(stored, dw, dh);
                        e.upscaledOk = true;
                    }
                }
                e.upscaledFrame = r.frameIndex;
                e.upscaledKey = key;
                e.upscaledFrom = r.image;
            }
            if (e.upscaledOk) {
                if (!e.upscaledPlain) r.image = &e.upscaled;
            } else {
                r.upscaleMissing = true;
                missingUpscales_.insert(clip.id);
                upscaleKey ^= 0x2545F4914F6CDD1DULL;
            }
        }
    }
    uint64_t cutoutKey = 0;
    if (const AiCutout& cut = clip.aiCutout; cut.enabled) {   // M7: the matte goes into the picture's alpha
        cutoutKey = cutoutParamsHash(cut);
        // (a missing matte is looked up again: the analysis may have written it since)
        if (!e.cutoutOk || e.cutoutFrame != r.frameIndex || e.cutoutParams != cutoutKey || e.cutoutFrom != r.image) {
            e.cutoutOk = false;
            // the model the clip's mattes were made with; a clip that does not record it (a project written by hand
            // or by a tool) uses the installed model's, as the export does
            const std::string sha = !cut.modelSha.empty() ? cut.modelSha : modelIdentity("cutout:" + cut.model);
            if (!sha.empty()) {
                if (e.identity.empty()) e.identity = matteMediaIdentity(path);
                const MatteStore store(MatteSetKey{e.identity, cut.model, sha, cut.quality});
                AlphaImage m, prev, next;
                if (store.read(r.frameIndex, m)) {
                    const bool smooth = cut.smoothing > 0;
                    const bool hp = smooth && store.read(r.frameIndex - 1, prev), hn = smooth && store.read(r.frameIndex + 1, next);
                    e.cutout = *r.image;
                    applyCutout(e.cutout, m, hp ? &prev : nullptr, hn ? &next : nullptr, cut);
                    e.cutoutOk = true;
                }
            }
            e.cutoutFrame = r.frameIndex;
            e.cutoutParams = cutoutKey;
            e.cutoutFrom = r.image;
        }
        if (e.cutoutOk) {
            r.image = &e.cutout;
        } else {
            r.matteMissing = true;
            missingMattes_.insert(clip.id);
            cutoutKey ^= 0x5bd1e995ull;   // the uncut picture: a different key from the cut one
        }
    }
    const Image& shown = *r.image;
    uint64_t k = e.fileKey;   // + decoder kind, frame and size (hardware and software decoders may differ by a level)
    k = k * 0x9E3779B97F4A7C15ULL ^ static_cast<uint64_t>(r.frameIndex + 1) ^ (e.decoder->hardwareActive() ? 1ULL << 62 : 0);
    k = k * 0x9E3779B97F4A7C15ULL ^ (static_cast<uint64_t>(shown.width) << 32 | static_cast<uint32_t>(shown.height));
    if (cutoutKey) k = k * 0x9E3779B97F4A7C15ULL ^ cutoutKey;
    if (upscaleKey) k = k * 0x9E3779B97F4A7C15ULL ^ upscaleKey;
    r.contentKey = (k << 1) | 0x8000000000000000ULL;   // never 0; even, unlike title keys
    return r;
}

const Image* DecoderPool::neuralFrame(const Clip& clip, const MediaSource& source, int64_t a, int64_t b, double t, bool& cut) {
    cut = false;
    const std::string sha = modelIdentity("rife");
    const auto path = resolveMediaPath(source, projectFile_);
    auto it = decoders_.find(clip.id);
    std::string identity = it != decoders_.end() ? it->second->identity : std::string();
    if (identity.empty()) {
        identity = matteMediaIdentity(path);
        if (it != decoders_.end()) it->second->identity = identity;
    }
    const FrameStore store(FrameSetKey{"rife", identity, sha, "v1"});
    const std::string name = interpolationFrameName(a, b, t);
    if (!sha.empty() && store.isMarked(name)) {
        cut = true;
        return nullptr;
    }
    if (!sha.empty() && store.read(name, neural_)) return &neural_;
    missingNeural_.insert(clip.id);
    return nullptr;
}

// ---- frame composition -----------------------------------------------------------------------

namespace {

// Optical flow between two source frames serves every in-between time, so slow motion estimates it once per pair:
// the last few flows are kept, keyed by the two frames' content keys (file, frame, decoder kind and size) and the
// quality. Preview and export threads share them (identical pictures give identical flows).
std::shared_ptr<const vision::FlowField> flowBetween(uint64_t keyA, uint64_t keyB, vision::FlowQuality quality, const Image& a,
                                                     const Image& b) {
    if (!keyA || !keyB) return vision::estimateFlow(a, b, quality);
    struct Entry {
        uint64_t a, b;
        vision::FlowQuality quality;
        std::shared_ptr<const vision::FlowField> flow;
    };
    static std::mutex m;
    static std::deque<Entry> recent;   // most recent first
    {
        std::lock_guard lk(m);
        for (const Entry& e : recent)
            if (e.a == keyA && e.b == keyB && e.quality == quality) return e.flow;
    }
    auto flow = vision::estimateFlow(a, b, quality);
    if (flow) {
        std::lock_guard lk(m);
        recent.push_front({keyA, keyB, quality, flow});
        if (recent.size() > 6) recent.pop_back();
    }
    return flow;
}

}  // namespace

Affine2D stabilizationTransform(const Stabilization& s, const Time& sourceTime, int w, int h) {
    const FrameCorrection* c = stabilizationAt(s, sourceTime);
    if (!c) return {};
    const double cx = w / 2.0, cy = h / 2.0;
    return Affine2D::translate(cx, cy) * Affine2D::scale(s.zoom, s.zoom) * Affine2D::translate(c->dx * w, c->dy * h) *
           Affine2D::rotateDeg(c->da * 180.0 / std::numbers::pi) * Affine2D::translate(-cx, -cy);
}

Affine2D clipPlacement(const Clip& clip, const Time& sourceTime, int srcW, int srcH, int canvasW, int canvasH,
                       const LayerAdjust& adj) {
    const ClipTransform& t = clip.transform;
    const double sx = t.scaleX.value(sourceTime) * adj.scale;
    const double sy = (t.uniformScale ? t.scaleX.value(sourceTime) : t.scaleY.value(sourceTime)) * adj.scale;
    const Affine2D a = Affine2D::fromParams(canvasW, canvasH, srcW, srcH, LayerFit::Fit, t.positionX.value(sourceTime) + adj.dx,
                                            t.positionY.value(sourceTime) + adj.dy, sx, sy, t.rotation.value(sourceTime) + adj.rotation,
                                            t.anchorX.value(sourceTime), t.anchorY.value(sourceTime), t.flipH, t.flipV);
    // M5: the stabilisation correction moves the picture before the clip transform places it
    return clip.stabilization ? a * stabilizationTransform(*clip.stabilization, sourceTime, srcW, srcH) : a;
}

namespace {

std::vector<LayerMask> layerMasks(const std::vector<Mask>& masks, const Time& t) {
    std::vector<LayerMask> out;
    for (const Mask& m : masks) {
        if (!m.enabled) continue;
        LayerMask lm;
        lm.shape = m.shape;
        lm.mode = m.mode;
        lm.inverted = m.inverted;
        lm.cx = m.centerX.value(t);
        lm.cy = m.centerY.value(t);
        lm.w = m.width.value(t);
        lm.h = m.height.value(t);
        lm.rotationDeg = m.rotation.value(t);
        lm.feather = std::max(0.0, m.feather.value(t));
        lm.opacity = std::clamp(m.opacity.value(t), 0.0, 1.0);
        lm.cornerRadius = m.cornerRadius;
        lm.points = m.points;
        out.push_back(std::move(lm));
    }
    return out;
}

std::shared_ptr<const std::vector<EffectInstance>> effectStack(const Clip& clip, const Time& sourceTime,
                                                               std::vector<std::string>* problems,
                                                               const std::filesystem::path& assetDir) {
    if (clip.effects.empty()) return nullptr;
    auto fx = std::make_shared<std::vector<EffectInstance>>(
        evaluateEffects(clip.effects, sourceTime, (sourceTime - clip.sourceIn).toDouble(), problems, assetDir));
    if (problems)
        for (const auto& e : *fx)
            if (!e.problem.empty()) problems->push_back("clip " + clip.id + ": " + e.problem);
    if (fx->empty() || effectsAreIdentity(*fx)) return nullptr;
    return fx;
}

// A transition's blur / pixelation (LayerAdjust), run on the layer after the clip's own effects: the same effects
// as the Effects page, so both backends draw them exactly as they draw those.
std::shared_ptr<const std::vector<EffectInstance>> withTransitionEffects(std::shared_ptr<const std::vector<EffectInstance>> stack,
                                                                         const LayerAdjust& adj) {
    std::vector<EffectRef> extra;
    auto add = [&](const char* type, const char* param, double value) {
        if (const EffectDef* d = findEffectDef(type)) {
            EffectRef e = makeEffect(*d, std::string("transition.") + param);
            e.params[param] = Animatable<double>(value);
            extra.push_back(std::move(e));
        }
    };
    if (adj.blur > 0.05) add("fa.fx.blur.gaussian", "radius", std::min(adj.blur, 200.0));
    if (adj.pixelate >= 2.0) add("fa.fx.pixelate", "size", std::min(adj.pixelate, 200.0));
    if (extra.empty()) return stack;
    auto out = std::make_shared<std::vector<EffectInstance>>(stack ? *stack : std::vector<EffectInstance>{});
    for (auto& e : evaluateEffects(extra, Time(0), 0.0)) out->push_back(std::move(e));
    return out;
}

}  // namespace

LayerDesc adjustmentLayer(const Clip& clip, const Time& sourceTime, const LayerAdjust& adj, std::vector<std::string>* problems,
                          const std::filesystem::path& assetDir) {
    LayerDesc l;
    l.adjustment = true;
    l.opacity = std::clamp(clip.opacity.value(sourceTime), 0.0, 1.0) * adj.opacity;
    l.masks = layerMasks(clip.masks, sourceTime);
    l.effects = effectStack(clip, sourceTime, problems, assetDir);
    return l;
}

LayerDesc clipLayer(const Clip& clip, const Time& sourceTime, const Image* image, int canvasW, int canvasH,
                    const LayerAdjust& adj, std::vector<std::string>* problems, const std::filesystem::path& assetDir) {
    if (clip.isAdjustment()) return adjustmentLayer(clip, sourceTime, adj, problems, assetDir);
    LayerDesc l;
    l.image = image;
    l.opacity = std::clamp(clip.opacity.value(sourceTime), 0.0, 1.0) * adj.opacity;
    if (image && !image->empty())
        l.sourceToCanvas = clipPlacement(clip, sourceTime, image->width, image->height, canvasW, canvasH, adj);
    const ClipTransform& t = clip.transform;
    l.cropL = t.cropLeft;
    l.cropT = t.cropTop;
    l.cropR = t.cropRight;
    l.cropB = t.cropBottom;
    l.blend = clip.blend;
    l.brightness = adj.brightness;
    l.flash = std::clamp(adj.flash, 0.0, 1.0);
    l.wipe = adj.wipe;
    l.masks = layerMasks(clip.masks, sourceTime);
    l.effects = withTransitionEffects(effectStack(clip, sourceTime, problems, assetDir), adj);
    return l;
}

LayerAdjust titleAdjust(const TitleAnimState& a, int cw, int ch) {
    LayerAdjust adj;
    const double px = ch / 1080.0;   // animation offsets are px at 1080p
    adj.dx = a.dx * px / cw;
    adj.dy = a.dy * px / ch;
    adj.scale = a.scale;
    adj.rotation = a.rotation;
    adj.opacity = std::clamp(a.opacity, 0.0, 1.0);
    return adj;
}

Affine2D titlePlacement(const Clip& clip, const Time& sourceTime, const TitleFrame& f, int cw, int ch,
                        const LayerAdjust& adj, Vec2 base) {
    const ClipTransform& t = clip.transform;
    const double sx = t.scaleX.value(sourceTime) * adj.scale;
    const double sy = (t.uniformScale ? t.scaleX.value(sourceTime) : t.scaleY.value(sourceTime)) * adj.scale;
    const double ax = t.anchorX.value(sourceTime) * f.blockW, ay = t.anchorY.value(sourceTime) * f.blockH;
    const double px = t.positionX.value(sourceTime) + adj.dx + base.x, py = t.positionY.value(sourceTime) + adj.dy + base.y;
    return Affine2D::translate(cw / 2.0 + px * cw + ax, ch / 2.0 + py * ch + ay) *
           Affine2D::rotateDeg(t.rotation.value(sourceTime) + adj.rotation) * Affine2D::scale(sx, sy) *
           Affine2D::translate(-ax, -ay) * Affine2D::scale(t.flipH ? -1 : 1, t.flipV ? -1 : 1) *
           Affine2D::scale(f.pixelScale, f.pixelScale) * Affine2D::translate(-f.refX, -f.refY);
}

LayerDesc titleLayer(const Clip& clip, const Time& sourceTime, const TitleFrame& f, int cw, int ch,
                     const LayerAdjust& adj, Vec2 base) {
    LayerDesc l = clipLayer(clip, sourceTime, f.image, cw, ch, adj);   // opacity, crop, blend, masks, wipe
    if (f.image && !f.image->empty()) l.sourceToCanvas = titlePlacement(clip, sourceTime, f, cw, ch, adj, base);
    return l;
}

const TextStyle& captionStyle(const Track& track, const Clip& clip) {
    return clip.title && !clip.title->followTrackStyle ? clip.title->style : track.subtitleStyle;
}

Vec2 captionBasePosition(const Track& track, const Clip& clip) {
    return captionBasePosition(track.subtitleY, captionStyle(track, clip), clip);
}

Vec2 captionBasePosition(double subtitleY, const TextStyle& st, const Clip& clip) {
    const ClipTransform& t = clip.transform;
    if (t.positionX.isAnimated() || t.positionY.isAnimated() || t.positionX.constantValue() != 0.0 ||
        t.positionY.constantValue() != 0.0)
        return {};   // the caption has its own position (e.g. ASS \pos)
    const double y = st.anchor == TextAnchor::Top ? 1.0 - subtitleY : st.anchor == TextAnchor::Center ? 0.5 : subtitleY;
    return {0.0, y - 0.5};
}

namespace {

// Frame Blending (M5): a = a + (b - a) * w per channel, straight alpha, rounded.
void blendFrames(Image& a, const Image& b, double weight) {
    const int w = static_cast<int>(std::lround(std::clamp(weight, 0.0, 1.0) * 256.0));
    parallelRows(0, a.height, [&](int y) {
        uint8_t* pa = a.row(y);
        const uint8_t* pb = b.row(y);
        for (int i = 0; i < a.width * 4; ++i) pa[i] = static_cast<uint8_t>((pa[i] * (256 - w) + pb[i] * w + 128) >> 8);
    });
}

LayerAdjust combine(const LayerAdjust& a, const LayerAdjust& b) {
    LayerAdjust r = a;
    r.dx += b.dx;
    r.dy += b.dy;
    r.scale *= b.scale;
    r.rotation += b.rotation;
    r.opacity *= b.opacity;
    r.brightness *= b.brightness;
    r.flash = 1 - (1 - r.flash) * (1 - b.flash);
    r.blur = std::max(r.blur, b.blur);
    r.pixelate = std::max(r.pixelate, b.pixelate);
    if (b.wipe.active) r.wipe = b.wipe;
    r.visible = a.visible && b.visible;
    return r;
}

}  // namespace

namespace {

// M10: the scale a title or sticker is shown at (its clip's Scale, the larger axis). The text engine draws it at that
// size; before, a canvas-sized picture was stretched by the compositor and enlarged text looked soft and pixelated.
double shownScale(const Clip& c, const Time& sourceTime) {
    const ClipTransform& t = c.transform;
    const double sx = std::abs(t.scaleX.value(sourceTime));
    const double sy = t.uniformScale ? sx : std::abs(t.scaleY.value(sourceTime));
    return std::max(sx, sy);
}

}  // namespace

const Image& renderFrame(const Sequence& seq, int64_t frame, const Project& project, FrameProvider& provider,
                         RenderBackend& backend, std::vector<TrackSample>* samplesOut,
                         std::vector<FrameProvider::Result>* framesOut, int canvasWidth, int canvasHeight,
                         TitleRenderer* titles, std::vector<std::string>* problems) {
    std::vector<TrackSample> samples = evaluateVideoFrame(seq, frame, &project);
    const int cw = canvasWidth > 0 ? canvasWidth : seq.width, ch = canvasHeight > 0 ? canvasHeight : seq.height;
    const Time now = frameToTime(frame, seq.frameRate);
    provider.canvas(seq.width, seq.height);   // M7: upscaled clips are prepared for the sequence size
    backend.beginFrame(cw, ch, Rgba8{0, 0, 0, 255});
    // Title clips: the engine renders the picture; the layer is placed with titlePlacement.
    std::vector<TitleFrame> titleFrames;   // kept alive until the layers are drawn
    titleFrames.reserve(4);
    auto renderTitle = [&](const Clip& c, const TextStyle& style) -> const TitleFrame& {
        if (!titles) throw RenderError("frame " + std::to_string(frame) + ": clip " + c.id + " is a title, but no text engine is available");
        titleFrames.push_back(titles->render(*c.title, style, now - c.timelineStart, c.duration, cw, ch,
                                             shownScale(c, c.sourceTimeAt(now))));
        // the engine's image stays valid until its next call: copy it if several titles are on screen
        return titleFrames.back();
    };
    std::deque<Image> titleImages;   // stable addresses: layers point into it (any number of titles per frame)
    std::deque<Image> blendImages;   // M5 Frame Blending: mixed source frames (stable addresses as above)
    auto layerFor = [&](const ClipSample& cs, const FrameProvider::Result& r, const LayerAdjust& adj) {
        if (cs.clip->isAdjustment()) return adjustmentLayer(*cs.clip, cs.sourceTime, adj, problems, provider.assetDir());
        if (cs.clip->isTitle()) {
            const TitleFrame& tf = titleFrames.at(static_cast<size_t>(r.frameIndex));
            LayerDesc l = titleLayer(*cs.clip, cs.sourceTime, tf, cw, ch, combine(adj, titleAdjust(tf.anim, cw, ch)));
            l.effects = effectStack(*cs.clip, cs.sourceTime, problems, provider.assetDir());   // on the title picture
            l.imageKey = tf.contentKey;
            return l;
        }
        LayerDesc l = clipLayer(*cs.clip, cs.sourceTime, r.image, cw, ch, adj, problems, provider.assetDir());
        if (r.image) l.imageKey = r.contentKey;   // the provider's promise (0: none)
        return l;
    };
    for (const TrackSample& s : samples) {
        auto fetch = [&](const ClipSample& c) {
            if (c.clip->isAdjustment()) {   // no picture: works on the canvas
                FrameProvider::Result r;
                r.requestedTime = r.frameTime = c.sourceTime;
                r.frameIndex = -1;
                if (framesOut) framesOut->push_back(r);   // one entry per clip sample (frame map)
                return r;
            }
            if (c.clip->isSticker()) {   // M9: generated picture, placed like media (layerFor's media branch)
                if (!titles)
                    throw RenderError("frame " + std::to_string(frame) + ": clip " + c.clip->id +
                                      " is a sticker, but no sticker engine is available");
                titleFrames.push_back(titles->renderSticker(*c.clip->sticker, now - c.clip->timelineStart, c.clip->duration, cw,
                                                            ch, shownScale(*c.clip, c.sourceTime)));
                FrameProvider::Result r;
                if (titleFrames.back().image) {
                    titleImages.push_back(*titleFrames.back().image);
                    titleFrames.back().image = &titleImages.back();
                }
                r.image = titleFrames.back().image;
                r.contentKey = titleFrames.back().contentKey;
                r.requestedTime = r.frameTime = c.sourceTime;
                r.frameIndex = static_cast<int64_t>(titleFrames.size() - 1);
                if (framesOut) framesOut->push_back(r);
                return r;
            }
            if (c.clip->isTitle()) {   // generated picture
                const TitleFrame& tf = renderTitle(*c.clip, c.clip->title->style);
                FrameProvider::Result r;
                if (tf.image) {
                    titleImages.push_back(*tf.image);
                    titleFrames.back().image = &titleImages.back();
                }
                r.image = titleFrames.back().image;
                r.requestedTime = r.frameTime = c.sourceTime;
                r.frameIndex = static_cast<int64_t>(titleFrames.size() - 1);   // index into titleFrames (not a media frame)
                if (framesOut) framesOut->push_back(r);
                return r;
            }
            const MediaSource* src = project.findSource(c.clip->sourceId);
            if (!src) throw RenderError(clipContext(seq, frame, &s, &c, nullptr) + ": unknown source");
            FrameProvider::Result r;
            try {
                r = provider.frame(*c.clip, *src, c.sourceTime);
                if (c.blendTime && c.blendWeight > 0.0 && r.image) {   // M5: mix with the next source frame
                    blendImages.push_back(*r.image);   // a copy: the provider reuses its frame for the next request
                    Image& mixed = blendImages.back();
                    const uint64_t keyA = r.contentKey;
                    const FrameProvider::Result rn = provider.frame(*c.clip, *src, *c.blendTime);
                    if (rn.image && rn.image->width == mixed.width && rn.image->height == mixed.height) {
                        const FrameInterpolation fi = c.clip->speed.interpolation;
                        bool flowed = false;
                        bool neural = false;
                        if (fi == FrameInterpolation::Neural) {   // M7: the prepared RIFE frame
                            bool cut = false;
                            if (const Image* nf = provider.neuralFrame(*c.clip, *src, r.frameIndex, rn.frameIndex,
                                                                       c.blendWeight, cut)) {
                                mixed = nf->width == mixed.width && nf->height == mixed.height
                                            ? *nf
                                            : resizeBilinear(*nf, mixed.width, mixed.height);
                                flowed = neural = true;
                            } else if (cut) {   // a shot change between the two frames: the nearer one, nothing mixed
                                if (c.blendWeight >= 0.5) mixed = *rn.image;
                                flowed = neural = true;
                            } else {
                                r.neuralMissing = true;   // not prepared: optical flow stands in (preview only)
                            }
                        }
                        if (!flowed && (fi == FrameInterpolation::OpticalFlow || fi == FrameInterpolation::OpticalFlowQuality ||
                                        fi == FrameInterpolation::Neural)) {
                            // motion-compensated; falls back to the blend if the flow fails
                            const auto quality = fi == FrameInterpolation::OpticalFlow ? vision::FlowQuality::Fast
                                                                                       : vision::FlowQuality::Quality;
                            (void)neural;
                            if (const auto flow = flowBetween(keyA, rn.contentKey, quality, mixed, *rn.image)) {
                                Image between;
                                flowed = vision::warpWithFlow(mixed, *rn.image, *flow, c.blendWeight, between);
                                if (flowed) mixed = std::move(between);
                            }
                        }
                        if (!flowed) blendFrames(mixed, *rn.image, c.blendWeight);
                        uint64_t k = keyA * 0x9E3779B97F4A7C15ULL ^ rn.contentKey;
                        k = k * 0x9E3779B97F4A7C15ULL ^ static_cast<uint64_t>(std::llround(c.blendWeight * 65536.0));
                        r.contentKey = keyA && rn.contentKey ? ((k << 1) | 0x8000000000000000ULL) : 0;
                    }
                    r.image = &mixed;
                }
            } catch (const std::exception& e) {
                throw RenderError(clipContext(seq, frame, &s, &c, src) + ": " + e.what());
            }
            if (framesOut) framesOut->push_back(r);
            return r;
        };
        const FrameProvider::Result ra = fetch(s.a);
        if (!s.b) {
            const LayerDesc l = layerFor(s.a, ra, LayerAdjust{});
            if (l.image || (l.adjustment && l.effects)) backend.drawLayer(l);
            continue;
        }
        const FrameProvider::Result rb = fetch(*s.b);
        // Transition between A and B (EXP-001: progress linear over the span).
        const TransitionLayers tl = s.transition ? evaluateTransition(*s.transition, s.mix)
                                                 : evaluateTransition(TransitionRef{"fa.transition.dissolve"}, s.mix);
        const LayerDesc la = layerFor(s.a, ra, tl.a);
        const LayerDesc lb = layerFor(*s.b, rb, tl.b);
        if (tl.bFirst) {
            if (tl.b.visible && lb.image) backend.drawLayer(lb);
            if (tl.a.visible && la.image) backend.drawLayer(la);
        } else {
            if (tl.a.visible && la.image) backend.drawLayer(la);
            if (tl.b.visible && lb.image) backend.drawLayer(lb);
        }
    }
    // Captions: subtitle tracks above every video track, in track order.
    for (const Track& tr : seq.tracks) {
        if (tr.kind != TrackKind::Subtitle || tr.hidden) continue;
        const Clip* c = tr.clipAt(now);
        if (!c || !c->enabled || !c->title) continue;
        const TitleFrame& tf = renderTitle(*c, captionStyle(tr, *c));
        if (!tf.image) continue;
        const Time st = c->sourceTimeAt(now);
        const LayerDesc l = titleLayer(*c, st, tf, cw, ch, titleAdjust(tf.anim, cw, ch), captionBasePosition(tr, *c));
        backend.drawLayer(l);
        if (framesOut) {
            FrameProvider::Result r;
            r.image = tf.image;
            r.requestedTime = r.frameTime = st;
            r.frameIndex = -1;
            framesOut->push_back(r);
        }
    }
    if (samplesOut) *samplesOut = std::move(samples);
    return backend.endFrame();
}

const Image& renderTransitionPreview(const TransitionRef& transition, double progress, const Image& a, const Image& b,
                                     int width, int height, RenderBackend& backend) {
    const Clip ca, cb;   // default transform, opacity 1, Normal blend, no masks
    const TransitionLayers tl = evaluateTransition(transition, std::clamp(progress, 0.0, 1.0));
    backend.beginFrame(width, height, Rgba8{0, 0, 0, 255});
    const LayerDesc la = clipLayer(ca, Time(0), &a, width, height, tl.a);
    const LayerDesc lb = clipLayer(cb, Time(0), &b, width, height, tl.b);
    if (tl.bFirst) {
        if (tl.b.visible) backend.drawLayer(lb);
        if (tl.a.visible) backend.drawLayer(la);
    } else {
        if (tl.a.visible) backend.drawLayer(la);
        if (tl.b.visible) backend.drawLayer(lb);
    }
    return backend.endFrame();
}

const Image& renderTitlePreview(const Clip& title, const Time& local, const Time& duration, const Image& background,
                                int width, int height, RenderBackend& backend, TitleRenderer& titles, Vec2 base) {
    backend.beginFrame(width, height, Rgba8{0, 0, 0, 255});
    const Clip bg;
    backend.drawLayer(clipLayer(bg, Time(0), &background, width, height));
    if (title.title) {
        const TitleFrame tf = titles.render(*title.title, title.title->style, local, duration, width, height);
        if (tf.image && !tf.image->empty())
            backend.drawLayer(titleLayer(title, title.sourceIn + local, tf, width, height, titleAdjust(tf.anim, width, height), base));
    }
    return backend.endFrame();
}

// ---- preflight -------------------------------------------------------------------------------

std::vector<ValidationIssue> preflight(const Project& project, const std::filesystem::path& projectFile,
                                       const RenderOptions& options, DecoderPool& pool) {
    std::vector<ValidationIssue> issues = validate(project);
    auto err = [&](std::string where, std::string msg) {
        issues.push_back({ValidationIssue::Severity::Error, std::move(where), std::move(msg)});
    };
    auto warn = [&](std::string where, std::string msg) {
        issues.push_back({ValidationIssue::Severity::Warning, std::move(where), std::move(msg)});
    };
    (void)projectFile;

    const Sequence* seqp = nullptr;
    try {
        seqp = &pickSequence(project, options.sequenceId);
    } catch (const RenderError& e) {
        err("project", e.what());
        return issues;
    }
    const Sequence& seq = *seqp;
    const std::string sw = "sequence " + seq.id;
    const int64_t total = seq.frameCount();
    if (total <= 0) err(sw, "sequence is empty (no clips)");
    if (options.firstFrame < 0 || (options.lastFrame >= 0 && options.lastFrame < options.firstFrame) ||
        (total > 0 && options.firstFrame >= total))
        err(sw, "invalid frame range " + std::to_string(options.firstFrame) + ":" + std::to_string(options.lastFrame) +
                    " (sequence has " + std::to_string(total) + " frames)");

    std::set<std::string> probed;
    bool hasTitles = false;
    for (const Track& tr : seq.tracks)
        for (const Clip& c : tr.clips) hasTitles = hasTitles || c.isTitle() || c.isSticker();
    if (hasTitles && !options.titleRenderer)
        err(sw, "the project contains titles, captions or stickers, but this renderer has no text engine");
    for (const Track& tr : seq.tracks) {
        const std::string tw = sw + " / track " + tr.id;
        if (tr.kind == TrackKind::Subtitle) continue;   // captions need no media
        if (tr.kind == TrackKind::Audio) {
            if (tr.muted || tr.clips.empty()) continue;
            if (!options.includeAudio) {
                warn(tw, "audio export is switched off: this track is not in the output");
                continue;
            }
            for (const Clip& c : tr.clips) {
                if (!c.enabled) continue;
                const std::string cw = tw + " / clip " + c.id;
                const MediaSource* src = project.findSource(c.sourceId);
                if (!src) continue;   // reported by validate()
                const auto path = resolveMediaPath(*src, projectFile);
                if (!std::filesystem::exists(path)) {
                    if (probed.insert(src->id).second) err("source " + src->id, "media file not found: " + path.string());
                    continue;
                }
                try {
                    const MediaInfo& mi = pool.mediaInfo(*src);
                    if (!mi.audio) {
                        err(cw, "media '" + src->path + "' has no audio stream");
                        continue;
                    }
                    const Time origin = mi.video ? mi.video->startTime : mi.audio->startTime;
                    if (mi.audio->duration) {
                        const Time avail = mi.audio->startTime + *mi.audio->duration - origin;
                        const Time slack(1, 20);   // 50 ms: audio streams often end slightly before the video
                        if (exceedsMedia(c, seq.frameRate, avail, slack))
                            warn(cw, "audio media ends at " + std::to_string(avail.toDouble()) +
                                         " s; the rest of the clip is silent");
                    }
                } catch (const MediaError& e) {
                    if (probed.insert(src->id + "#err").second) err("source " + src->id, e.what());
                }
            }
            continue;
        }
        std::vector<TimeRange> spans;
        for (size_t i = 0; i < tr.clips.size(); ++i) {
            const Clip& c = tr.clips[i];
            const std::string cw = tw + " / clip " + c.id;
            if (c.outTransition) {
                try {
                    if (auto span = transitionSpan(tr, i)) {
                        for (const auto& other : spans)
                            if (other.overlaps(span->range)) err(cw, "transition overlaps another transition");
                        spans.push_back(span->range);
                    }
                } catch (const ModelError& e) {
                    err(cw, e.what());
                }
                if (!isFrameAligned(c.outTransition->duration, seq.frameRate))
                    err(cw, "transition duration is not a whole number of frames");
                if (isAudioCrossfade(c.outTransition->type)) {   // M6: mixed by the AudioMixer
                    CrossfadeShape shape;
                    if (!crossfadeShapeFromId(c.outTransition->type.substr(std::char_traits<char>::length(kAudioCrossfadePrefix)), shape))
                        err(cw, "unknown audio crossfade '" + c.outTransition->type + "'");
                } else if (!findTransition(c.outTransition->type)) {
                    const std::string msg = "transition type '" + c.outTransition->type +
                                            "' is not supported by this renderer";
                    if (options.allowUnsupported) warn(cw, msg + "; rendering a hard cut");
                    else err(cw, msg);
                }
            }
            {
                std::vector<std::string> fxProblems;
                const auto dir = projectFile.has_parent_path() ? projectFile.parent_path() : std::filesystem::current_path();
                for (const auto& e : evaluateEffects(c.effects, c.sourceIn, 0.0, &fxProblems, dir))
                    if (!e.problem.empty()) fxProblems.push_back(e.problem);   // e.g. a LUT file that is missing
                for (const auto& msg : fxProblems) {
                    if (options.allowUnsupported) warn(cw, msg + "; ignored");
                    else err(cw, msg);
                }
            }
            if (c.isGenerated()) continue;   // generated picture
            const MediaSource* src = project.findSource(c.sourceId);
            if (!src || tr.hidden || !c.enabled) continue;
            const auto path = resolveMediaPath(*src, projectFile);
            if (!std::filesystem::exists(path)) {
                if (probed.insert(src->id).second) err("source " + src->id, "media file not found: " + path.string());
                continue;
            }
            try {
                const MediaInfo& mi = pool.mediaInfo(*src);
                if (!mi.video) {
                    err("source " + src->id, "media has no video stream: " + path.string());
                    continue;
                }
                std::optional<Time> dur = mi.video->duration ? mi.video->duration : mi.duration;
                // Allow half a frame of slack for container rounding.
                const Time slack = mi.video->frameRate.frameDuration() * Rational(1, 2);
                if (dur && exceedsMedia(c, seq.frameRate, *dur, slack))
                    err(cw, "needs media up to " + c.sourceOut().toString() + " s (" +
                                std::to_string(c.sourceOut().toDouble()) + ") but '" + src->path + "' is only " +
                                std::to_string(dur->toDouble()) + " s long");
                if (mi.video->isHdr() && probed.insert(src->id + "#hdr").second)
                    warn("source " + src->id, std::string(hdrTransferLabel(hdrTransferFromName(mi.video->transfer))) +
                                                  " HDR video is rendered as SDR Rec.709 with a simple tone mapping "
                                                  "(MeerCut does not output HDR)");
                if (mi.video->frameRate != seq.frameRate && probed.insert(src->id + "#rate").second)
                    warn("source " + src->id, "frame rate " + mi.video->frameRate.toString() +
                                                  " differs from the sequence (" + seq.frameRate.toString() +
                                                  "); frames are sampled by time (nearest preceding frame)");
            } catch (const MediaError& e) {
                if (probed.insert(src->id + "#err").second) err("source " + src->id, e.what());
            }
        }
    }
    return issues;
}

// ---- renderProject ---------------------------------------------------------------------------

namespace {

// Measures the time spent decoding source frames (for the stage breakdown in RenderReport).
class TimedProvider final : public FrameProvider {
public:
    explicit TimedProvider(FrameProvider& inner) : inner_(inner) {}
    std::filesystem::path assetDir() const override { return inner_.assetDir(); }
    void canvas(int w, int h) override { inner_.canvas(w, h); }
    const Image* neuralFrame(const Clip& c, const MediaSource& s, int64_t a, int64_t b, double t, bool& cut) override {
        return inner_.neuralFrame(c, s, a, b, t, cut);
    }
    Result frame(const Clip& clip, const MediaSource& source, const Time& sourceTime) override {
        const auto t0 = std::chrono::steady_clock::now();
        Result r = inner_.frame(clip, source, sourceTime);
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return r;
    }
    double seconds = 0;

private:
    FrameProvider& inner_;
};

double since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

namespace {

// The project with only the given clips (and their linked partners) on the rendered sequence; transitions survive
// only between two kept, adjacent clips. Sets the frame range to the kept clips' span.
Project keepOnlyClips(const Project& project, RenderOptions& options) {
    Project out = project;
    Sequence* seq = nullptr;
    for (auto& s : out.sequences)
        if (options.sequenceId.empty() || s.id == options.sequenceId) {
            seq = &s;
            break;
        }
    if (!seq) throw RenderError("sequence '" + options.sequenceId + "' not found");
    std::set<std::string> keep(options.onlyClips.begin(), options.onlyClips.end()), links;
    for (const auto& t : seq->tracks)
        for (const auto& c : t.clips)
            if (keep.count(c.id) && !c.linkId.empty()) links.insert(c.linkId);
    for (const auto& t : seq->tracks)
        for (const auto& c : t.clips)
            if (!c.linkId.empty() && links.count(c.linkId)) keep.insert(c.id);
    std::optional<Time> start, end;
    for (auto& t : seq->tracks) {
        std::vector<Clip> kept;
        for (size_t i = 0; i < t.clips.size(); ++i) {
            if (!keep.count(t.clips[i].id)) continue;
            Clip c = t.clips[i];
            const bool nextKept = i + 1 < t.clips.size() && keep.count(t.clips[i + 1].id);
            if (!nextKept) c.outTransition.reset();
            start = start ? min(*start, c.timelineStart) : c.timelineStart;
            end = end ? max(*end, c.timelineEnd()) : c.timelineEnd();
            kept.push_back(std::move(c));
        }
        t.clips = std::move(kept);
    }
    if (!start) throw RenderError("none of the selected clips is on the exported sequence");
    options.firstFrame = timeToFrame(*start, seq->frameRate, Rounding::Floor);
    options.lastFrame = timeToFrame(*end, seq->frameRate, Rounding::Ceil) - 1;
    options.onlyClips.clear();
    return out;
}

}  // namespace

RenderReport renderProject(const Project& project, const std::filesystem::path& projectFile,
                           const std::filesystem::path& output, const RenderOptions& options) {
    if (!options.onlyClips.empty()) {   // M6: "Selected clips" export
        RenderOptions o = options;
        const Project only = keepOnlyClips(project, o);
        return renderProject(only, projectFile, output, o);
    }
    const auto t0 = std::chrono::steady_clock::now();
    VideoDecoder::Options decOpt;
    decOpt.threads = 4;   // x264 is the bottleneck; 4 decode threads keep memory bounded
    decOpt.hardware = options.hardwareDecode;
    DecoderPool pool(projectFile, 6, decOpt);
    const auto issues = preflight(project, projectFile, options, pool);
    if (hasErrors(issues)) throw RenderError("project cannot be rendered:\n" + formatIssues(issues));

    RenderReport report;
    for (const auto& i : issues)
        if (i.severity == ValidationIssue::Severity::Warning) report.warnings.push_back(i.where + ": " + i.message);

    const Sequence& seq = pickSequence(project, options.sequenceId);
    const int64_t total = seq.frameCount();
    const int64_t first = options.firstFrame;
    const int64_t last = options.lastFrame < 0 ? total - 1 : std::min(options.lastFrame, total - 1);
    if (last < first) throw RenderError("nothing to export: the range is empty");
    // The exported time range, and the output frame grid (the sequence's own, or another frame rate: the timeline is
    // evaluated by time, so the same clips, transitions and retimed media are sampled at the output instants).
    const Time rangeStart = frameToTime(first, seq.frameRate), rangeEnd = frameToTime(last + 1, seq.frameRate);
    const FrameRate outRate = options.outputRate ? *options.outputRate : seq.frameRate;
    Sequence outSeq = seq;
    outSeq.frameRate = outRate;
    const int64_t outFirst = (rangeStart * outRate.fps()).ceil();
    const int64_t outLast = (rangeEnd * outRate.fps()).ceil() - 1;

    // Format, encoders and stream settings.
    const ExportFormat* format = options.format.empty() ? exportFormatForPath(output.string()) : findExportFormat(options.format);
    if (!format && !options.format.empty()) throw RenderError("unknown export format '" + options.format + "'");
    if (!format) format = findExportFormat("mp4-h264");
    EncoderSettings enc = options.encoder;
    enc.muxer = format->muxer;
    enc.width = options.outputWidth > 0 ? options.outputWidth : seq.width;
    enc.height = options.outputHeight > 0 ? options.outputHeight : seq.height;
    enc.frameRate = outRate;
    const bool video = !format->videoCodec.empty();
    if (format->videoCodec == "prores") enc.bitDepth = 10;
    if (video && std::find(format->bitDepths.begin(), format->bitDepths.end(), enc.bitDepth) == format->bitDepths.end())
        throw RenderError(format->label + " is not offered with " + std::to_string(enc.bitDepth) + "-bit video");
    enc.audio = options.includeAudio && !format->audioCodecs.empty();
    if (!video && !enc.audio) throw RenderError(format->label + " is an audio format: turn the audio on");
    enc.audioCodec = !options.audioCodec.empty() ? options.audioCodec : format->audioCodecs.empty() ? std::string() : format->audioCodecs.front();
    if (enc.audio && std::find(format->audioCodecs.begin(), format->audioCodecs.end(), enc.audioCodec) == format->audioCodecs.end())
        throw RenderError("audio codec " + enc.audioCodec + " cannot go into " + format->label);
    const int audioRate = options.audioSampleRate > 0 ? options.audioSampleRate : seq.sampleRate;
    const int channels = options.audioChannels;
    if (channels != 1 && channels != 2) throw RenderError("audio must be mono or stereo");
    if (audioRate < 8000 || audioRate > 192000) throw RenderError("unsupported audio sample rate " + std::to_string(audioRate));
    enc.audioSampleRate = audioRate;
    enc.audioChannels = channels;
    enc.audioBitrate = options.audioBitrate;
    report.format = format->id;
    if (video) {   // hardware when asked for and a session at this size opens, else the CPU encoder (reported)
        try {
            const EncoderChoice choice = chooseVideoEncoder(format->videoCodec, options.hardware, enc.bitDepth, enc.width,
                                                            enc.height, outRate);
            enc.codec = choice.encoder;
            if (options.qualityLevel >= 0) enc.crf = nativeQuality(enc.codec, options.qualityLevel);
            report.videoEncoder = choice.encoder;
            report.hardwareEncoder = choice.hardware;
            report.encoderNote = choice.note;
        } catch (const MediaError& e) {
            throw RenderError(e.what());
        }
    } else {
        enc.codec.clear();
    }
    report.audioEncoder = enc.audio ? enc.audioCodec : std::string();

    std::unique_ptr<AudioMixer> mixer;
    if (enc.audio) mixer = std::make_unique<AudioMixer>(projectFile, audioRate, channels);   // mixed at the output rate
    std::vector<float> audioBuf;
    int64_t audioPos = 0;
    // Mix and encode audio up to (not including) output sample `upTo`.
    auto writeAudioUpTo = [&](VideoEncoder& encoder, int64_t upTo) {
        constexpr int kChunk = 4096;
        while (audioPos < upTo) {
            const int n = static_cast<int>(std::min<int64_t>(kChunk, upTo - audioPos));
            audioBuf.resize(static_cast<size_t>(n) * channels);
            try {
                mixer->mix(project, seq, audioPos, n, audioBuf.data());
            } catch (const std::exception& e) {
                throw RenderError("audio at " + std::to_string(double(audioPos) / audioRate) + " s: " + e.what());
            }
            for (float x : audioBuf) {
                const float ax = std::fabs(x);
                report.audioPeak = std::max(report.audioPeak, ax);
                if (ax > 1.0f) ++report.audioClippedSamples;
            }
            encoder.writeAudio(audioBuf.data(), n);
            audioPos += n;
        }
    };

    if (output.has_parent_path()) std::filesystem::create_directories(output.parent_path());
    std::filesystem::path partial = output;   // the finished file replaces `output` only at the very end
    partial += ".partial" + output.extension().string();

    std::ofstream map;
    if (!options.frameMapCsv.empty()) {
        map.open(options.frameMapCsv, std::ios::trunc);
        if (!map) throw RenderError("cannot write frame map " + options.frameMapCsv.string());
        map << "frame,time,track,role,clip,source,source_time,src_frame,src_frame_time,clamped,weight\n";
    }

    std::unique_ptr<RenderBackend> backend;
    if (video) backend = makeBackend(options.backend, &report.backend);
    std::unique_ptr<TitleRenderer> titles = options.titleRenderer ? options.titleRenderer() : nullptr;
    TimedProvider timed(pool);
    std::vector<TrackSample> samples;
    std::vector<FrameProvider::Result> frames;
    auto render = [&](int64_t n) -> const Image& {
        frames.clear();
        const auto tf = std::chrono::steady_clock::now();
        const double dec0 = timed.seconds;
        const Image& img = renderFrame(outSeq, n, project, timed, *backend, &samples, &frames, 0, 0, titles.get());
        report.compositeSeconds += since(tf) - (timed.seconds - dec0);
        for (const auto& f : frames) {   // M7: never export an AI clip without its prepared pictures
            if (f.matteMissing)
                throw RenderError("frame " + std::to_string(n) + " (" + formatTimecode(n, outRate) + "): a clip with "
                                  "Background Removal has no matte for this frame - analyse the clip first (the export "
                                  "window and fa-render do it automatically)");
            if (f.upscaleMissing || f.neuralMissing)
                throw RenderError("frame " + std::to_string(n) + " (" + formatTimecode(n, outRate) + "): a clip with " +
                                  (f.upscaleMissing ? "AI upscaling" : "AI interpolation") + " was not prepared for this "
                                  "frame - the export window and fa-render prepare it automatically");
        }
        return img;
    };
    if (format->videoCodec == "gif") {   // one palette for the whole GIF, from up to 24 frames spread over the range
        std::vector<Image> picks;
        const int64_t count = outLast - outFirst + 1, step = std::max<int64_t>(1, count / 24);
        for (int64_t n = outFirst; n <= outLast; n += step) {
            if (options.cancel && options.cancel->load()) throw RenderCancelled();
            picks.push_back(render(n));
        }
        std::vector<const Image*> ptrs;
        for (const auto& p : picks) ptrs.push_back(&p);
        enc.palette = buildGifPalette(ptrs);
    }

    auto encodeAll = [&](const EncoderSettings& settings) {
        audioPos = timeToSample(rangeStart, audioRate);
        report.audioPeak = 0;
        report.audioClippedSamples = 0;
        report.framesWritten = 0;
        VideoEncoder encoder(partial, settings);
        for (int64_t n = outFirst; n <= outLast; ++n) {
            if (options.cancel && options.cancel->load()) throw RenderCancelled();
            if (video) {
                const Image& img = render(n);
                const auto te = std::chrono::steady_clock::now();
                try {
                    encoder.writeFrame(img);
                } catch (const MediaError& e) {
                    throw RenderError("frame " + std::to_string(n) + " (" + formatTimecode(n, outRate) +
                                      "): encoding failed: " + e.what());
                }
                report.encodeSeconds += since(te);
                ++report.framesWritten;
            }
            if (mixer) writeAudioUpTo(encoder, timeToSample(frameToTime(n + 1, outRate), audioRate));
            if (video && map.is_open()) {   // (an ofstream that was never opened still converts to true)
                size_t k = 0;
                const Time t = frameToTime(n, outRate);
                for (const auto& s : samples) {
                    auto row = [&](const ClipSample& c, const char* role, double w) {
                        const auto& r = frames.at(k++);
                        map << n << ',' << t.toString() << ',' << s.track->id << ',' << role << ',' << c.clip->id << ','
                            << c.clip->sourceId << ',' << c.sourceTime.toString() << ',' << r.frameIndex << ','
                            << r.frameTime.toString() << ',' << (r.clamped ? 1 : 0) << ',' << w << '\n';
                    };
                    row(s.a, "a", s.b ? 1.0 - s.mix : 1.0);
                    if (s.b) row(*s.b, "b", s.mix);
                }
                if (samples.empty()) map << n << ',' << t.toString() << ",,gap,,,,,,,0\n";
            }
            if (options.progress) options.progress(n - outFirst + 1, outLast - outFirst + 1);
        }
        encoder.finish();
    };
    try {
        encodeAll(enc);
    } catch (const RenderCancelled&) {
        throw;
    } catch (const std::exception& e) {
        if (!report.hardwareEncoder) throw;
        // the hardware encoder failed after its test session: export again with the CPU encoder, and say so
        const std::string failed = enc.codec;
        enc.codec = chooseVideoEncoder(format->videoCodec, HardwareEncoding::Cpu, enc.bitDepth, enc.width, enc.height, outRate).encoder;
        if (options.qualityLevel >= 0) enc.crf = nativeQuality(enc.codec, options.qualityLevel);
        report.encoderNote += (report.encoderNote.empty() ? "" : "; ") + failed + " failed (" + e.what() +
                              "), exported again with " + enc.codec;
        report.videoEncoder = enc.codec;
        report.hardwareEncoder = false;
        report.compositeSeconds = report.encodeSeconds = 0;
        encodeAll(enc);
    }
    std::error_code ec;
    std::filesystem::rename(partial, output, ec);   // replaces an existing file in one step (MoveFileEx)
    if (ec) {
        std::error_code ignore;
        std::filesystem::remove(partial, ignore);
        throw RenderError("cannot move the finished file into place (an existing " + output.filename().string() +
                          " is left unchanged): " + ec.message());
    }
    report.seeks = pool.totalSeeks() + (mixer ? mixer->totalSeeks() : 0);
    report.audioSamples = mixer ? audioPos - timeToSample(rangeStart, audioRate) : 0;
    if (report.audioClippedSamples > 0)
        report.warnings.push_back("audio clipped: " + std::to_string(report.audioClippedSamples) +
                                  " samples exceeded full scale (peak " +
                                  std::to_string(20.0 * std::log10(static_cast<double>(report.audioPeak))) +
                                  " dBFS); lower the clip or track volume");
    if (!report.hardwareEncoder && options.hardware != HardwareEncoding::Cpu && video)   // asked for hardware, got the CPU
        report.warnings.push_back("video encoder: " + report.encoderNote);
    report.decodeSeconds = timed.seconds;
    report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return report;
}

}  // namespace fa
