#include "fa/model/project.h"

#include "fa/model/time_map.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>
#include <sstream>

namespace fa {

const Clip* Track::clipAt(const Time& t) const {
    // clips are sorted and non-overlapping: last clip starting at or before t
    auto it = std::upper_bound(clips.begin(), clips.end(), t,
                               [](const Time& x, const Clip& c) { return x < c.timelineStart; });
    if (it == clips.begin()) return nullptr;
    --it;
    return it->timelineRange().contains(t) ? &*it : nullptr;
}

void Track::sortClips() {
    std::stable_sort(clips.begin(), clips.end(),
                     [](const Clip& a, const Clip& b) { return a.timelineStart < b.timelineStart; });
}

Time Sequence::duration() const {
    Time end(0);
    for (const auto& tr : tracks)
        for (const auto& c : tr.clips) end = max(end, c.timelineEnd());
    return end;
}

int64_t Sequence::frameCount() const { return timeToFrame(duration(), frameRate, Rounding::Ceil); }

Track* Sequence::findTrack(const std::string& trackId) {
    for (auto& t : tracks)
        if (t.id == trackId) return &t;
    return nullptr;
}
const Track* Sequence::findTrack(const std::string& trackId) const {
    return const_cast<Sequence*>(this)->findTrack(trackId);
}

std::string Project::newId(const std::string& prefix) { return prefix + "-" + std::to_string(nextId++); }

MediaSource* Project::findSource(const std::string& id) {
    for (auto& s : sources)
        if (s.id == id) return &s;
    return nullptr;
}
const MediaSource* Project::findSource(const std::string& id) const {
    return const_cast<Project*>(this)->findSource(id);
}
Sequence* Project::findSequence(const std::string& id) {
    for (auto& s : sequences)
        if (s.id == id) return &s;
    return nullptr;
}
const Sequence* Project::findSequence(const std::string& id) const {
    return const_cast<Project*>(this)->findSequence(id);
}

ClipLocation Project::locateClip(Sequence& seq, const std::string& clipId) {
    for (auto& tr : seq.tracks)
        for (size_t i = 0; i < tr.clips.size(); ++i)
            if (tr.clips[i].id == clipId) return {&tr, i};
    throw ModelError("clip not found: " + clipId);
}

namespace {
struct BlendName {
    BlendMode mode;
    const char* id;
    const char* label;
};
constexpr BlendName kBlendNames[] = {
    {BlendMode::Normal, "normal", "Normal"},           {BlendMode::Multiply, "multiply", "Multiply"},
    {BlendMode::Screen, "screen", "Screen"},           {BlendMode::Overlay, "overlay", "Overlay"},
    {BlendMode::Darken, "darken", "Darken"},           {BlendMode::Lighten, "lighten", "Lighten"},
    {BlendMode::ColorDodge, "colorDodge", "Color Dodge"}, {BlendMode::ColorBurn, "colorBurn", "Color Burn"},
    {BlendMode::HardLight, "hardLight", "Hard Light"}, {BlendMode::SoftLight, "softLight", "Soft Light"},
    {BlendMode::Difference, "difference", "Difference"}, {BlendMode::Exclusion, "exclusion", "Exclusion"},
    {BlendMode::LinearDodge, "linearDodge", "Linear Dodge (Add)"}, {BlendMode::LinearBurn, "linearBurn", "Linear Burn"},
};
static_assert(std::size(kBlendNames) == kBlendModeCount);
}  // namespace

const char* blendModeId(BlendMode m) {
    for (const auto& b : kBlendNames)
        if (b.mode == m) return b.id;
    return "normal";
}
const char* blendModeLabel(BlendMode m) {
    for (const auto& b : kBlendNames)
        if (b.mode == m) return b.label;
    return "Normal";
}
std::optional<BlendMode> blendModeFromId(const std::string& id) {
    for (const auto& b : kBlendNames)
        if (id == b.id) return b.mode;
    return std::nullopt;
}

std::optional<TransitionSpan> transitionSpan(const Track& track, size_t i) {
    const Clip& a = track.clips.at(i);
    if (!a.outTransition) return std::nullopt;
    const TransitionRef& tr = *a.outTransition;
    if (i + 1 >= track.clips.size() || track.clips[i + 1].timelineStart != a.timelineEnd())
        throw ModelError("transition on clip " + a.id + " has no adjacent following clip");
    const Clip& b = track.clips[i + 1];
    if (tr.duration <= Time(0)) throw ModelError("transition on clip " + a.id + " has non-positive duration");
    const Time cut = a.timelineEnd();
    Time start;
    switch (tr.alignment) {
        case TransitionAlignment::Centered: start = cut - tr.duration * Rational(1, 2); break;
        case TransitionAlignment::EndsAtCut: start = cut - tr.duration; break;
        case TransitionAlignment::StartsAtCut: start = cut; break;
    }
    const TimeRange span{start, tr.duration};
    if (span.start < a.timelineStart || span.end() > b.timelineEnd())
        throw ModelError("transition on clip " + a.id + " (" + tr.duration.toString() +
                         " s) is longer than the clips it joins");
    return TransitionSpan{span, &a, &b};
}

std::vector<ValidationIssue> validate(const Project& p) {
    std::vector<ValidationIssue> out;
    auto err = [&](std::string where, std::string msg) {
        out.push_back({ValidationIssue::Severity::Error, std::move(where), std::move(msg)});
    };
    auto warn = [&](std::string where, std::string msg) {
        out.push_back({ValidationIssue::Severity::Warning, std::move(where), std::move(msg)});
    };

    std::set<std::string> ids;
    auto uniqueId = [&](const std::string& where, const std::string& id) {
        if (id.empty()) err(where, "empty id");
        else if (!ids.insert(id).second) err(where, "duplicate id '" + id + "'");
    };

    for (const auto& s : p.sources) {
        const std::string where = "source " + s.id;
        uniqueId(where, s.id);
        if (s.path.empty()) err(where, "empty media path");
        if (s.duration && *s.duration <= Time(0)) err(where, "non-positive duration");
        if (s.range && (s.range->start < Time(0) || s.range->duration <= Time(0)))
            err(where, "a scene range must start at or after 0 and be longer than 0");
    }
    if (p.sequences.empty()) warn("project", "project has no sequences");

    for (const auto& seq : p.sequences) {
        const std::string sw = "sequence " + seq.id;
        uniqueId(sw, seq.id);
        if (seq.width <= 0 || seq.height <= 0) err(sw, "invalid resolution");
        // M8: bounds a damaged or hostile project cannot exceed (every buffer is sized from these)
        if (seq.width > kMaxCanvasSide || seq.height > kMaxCanvasSide)
            err(sw, "resolution above " + std::to_string(kMaxCanvasSide) + " pixels per side");
        if (seq.width % 2 || seq.height % 2) err(sw, "resolution must be even (4:2:0 output)");
        if (seq.sampleRate < 8000 || seq.sampleRate > 384000) err(sw, "sample rate must be in [8000, 384000] Hz");
        if (!(seq.frameRate.toDouble() > 0.0 && seq.frameRate.toDouble() <= 1000.0)) err(sw, "frame rate must be in (0, 1000] fps");
        if (!(seq.limiterCeilingDb >= -20.0 && seq.limiterCeilingDb <= 0.0)) err(sw, "limiter ceiling must be in [-20, 0] dBFS");
        for (const auto* m : {&seq.markIn, &seq.markOut})
            if (*m && (**m < Time(0) || !isFrameAligned(**m, seq.frameRate))) err(sw, "In / Out marks must be frame-aligned and >= 0");
        if (seq.markIn && seq.markOut && !(*seq.markIn < *seq.markOut)) err(sw, "the In mark must come before the Out mark");
        for (const auto& tr : seq.tracks) {
            const std::string tw = sw + " / track " + tr.id;
            uniqueId(tw, tr.id);
            if (!std::isfinite(tr.volumeDb) || tr.volumeDb > 24.0) err(tw, "track volume must be finite and <= +24 dB");
            for (size_t i = 0; i < tr.clips.size(); ++i) {
                const Clip& c = tr.clips[i];
                const std::string cw = tw + " / clip " + c.id;
                uniqueId(cw, c.id);
                const MediaSource* src = c.isGenerated() ? nullptr : p.findSource(c.sourceId);
                if (c.isSticker()) {
                    if (!c.sourceId.empty()) err(cw, "a sticker cannot also reference media");
                    if (c.isTitle() || c.isAdjustment()) err(cw, "a sticker cannot also be a title or an adjustment layer");
                    if (tr.kind != TrackKind::Video) err(cw, "stickers belong on video tracks");
                    if (c.sticker->id.empty()) err(cw, "sticker without an id");
                    if (c.sticker->color > 0xFFFFFF) err(cw, "sticker colour out of range");
                    if (c.sticker->text.size() > kMaxTitleChars) err(cw, "sticker text too long");
                } else if (c.isAdjustment()) {
                    if (!c.sourceId.empty()) err(cw, "an adjustment layer cannot also reference media");
                } else if (c.isTitle()) {
                    if (!c.sourceId.empty()) err(cw, "a title clip cannot also reference media");
                    if (tr.kind == TrackKind::Audio) err(cw, "title clips belong on video or subtitle tracks");
                    if (c.title->text.size() > kMaxTitleChars * 4) warn(cw, "title text is very long");
                    if (!(c.title->style.size > 0.0)) err(cw, "font size must be positive");
                    if (c.title->style.size > 10000.0) err(cw, "font size above 10000");
                } else {
                    if (!src) err(cw, "unknown source '" + c.sourceId + "'");
                    if (tr.kind == TrackKind::Subtitle) err(cw, "subtitle tracks hold caption (title) clips only");
                }
                if (c.duration <= Time(0)) err(cw, "non-positive duration " + c.duration.toString());
                if (c.colorMark < 0 || c.colorMark > kColorMarkCount)
                    err(cw, "colour mark " + std::to_string(c.colorMark) + " out of range (0.." + std::to_string(kColorMarkCount) + ")");
                if (c.timelineStart < Time(0)) err(cw, "negative timeline start");
                if (c.sourceIn < Time(0) && !c.isGenerated()) err(cw, "negative source in-point");
                {
                    std::set<std::string> fxIds;
                    for (const auto& e : c.effects) {
                        if (e.id.empty()) err(cw, "effect without an id");
                        else if (!fxIds.insert(e.id).second) err(cw, "duplicate effect id '" + e.id + "'");
                        if (e.type.empty()) err(cw, "effect without a type");
                    }
                }
                if (c.isAdjustment()) {
                    if (c.isTitle()) err(cw, "a clip cannot be both a title and an adjustment layer");
                    if (tr.kind != TrackKind::Video) err(cw, "adjustment layers belong on video tracks");
                    if (c.outTransition) err(cw, "adjustment layers cannot have transitions");
                }
                if (!isFrameAligned(c.timelineStart, seq.frameRate))
                    err(cw, "timeline start " + c.timelineStart.toString() + " s is not on a frame boundary of " +
                                seq.frameRate.toString() + " fps");
                if (!isFrameAligned(c.duration, seq.frameRate))
                    err(cw, "duration " + c.duration.toString() + " s is not a whole number of frames at " +
                                seq.frameRate.toString() + " fps");
                if (src && src->duration && exceedsMedia(c, seq.frameRate, *src->duration))
                    err(cw, "source out-point " + c.sourceOut().toString() + " s exceeds source duration " +
                                src->duration->toString() + " s");
                if (const std::string sp = speedProblem(c); !sp.empty()) err(cw, sp);
                if (src && tr.kind == TrackKind::Video && !src->hasVideo) err(cw, "source has no video stream");
                if (i > 0 && tr.clips[i - 1].timelineStart > c.timelineStart) err(cw, "clips not sorted by start");
                if (i > 0 && tr.clips[i - 1].timelineEnd() > c.timelineStart)
                    err(cw, "overlaps previous clip " + tr.clips[i - 1].id);
                if (c.outTransition && c.outTransition->duration <= Time(0)) err(cw, "transition duration <= 0");
                if (c.outTransition && i + 1 < tr.clips.size() && tr.clips[i + 1].isAdjustment())
                    err(cw, "transitions cannot lead into an adjustment layer");
                {   // M6 audio
                    bool volumeOk = std::isfinite(c.volumeDb.constantValue()) && c.volumeDb.constantValue() <= 24.0;
                    for (const auto& k : c.volumeDb.keys()) volumeOk = volumeOk && std::isfinite(k.value) && k.value <= 24.0;
                    if (!volumeOk) err(cw, "clip volume must be finite and <= +24 dB");
                    const ClipAudio& a = c.audio;
                    if (a.fadeIn < Time(0) || a.fadeOut < Time(0)) err(cw, "fade durations must not be negative");
                    if (a.fadeIn + a.fadeOut > c.duration) err(cw, "fade in and fade out together are longer than the clip");
                    bool balanceOk = a.balance.constantValue() >= -1.0 && a.balance.constantValue() <= 1.0;
                    for (const auto& k : a.balance.keys()) balanceOk = balanceOk && k.value >= -1.0 && k.value <= 1.0;
                    if (!balanceOk) err(cw, "balance must be in [-1, 1]");
                    for (double g : a.eqGainsDb)
                        if (!(g >= -12.0 && g <= 12.0)) err(cw, "equaliser band gains must be in [-12, +12] dB");
                    const AudioCleanup& k = a.cleanup;
                    if (!(k.denoise >= 0.0 && k.denoise <= 1.0)) err(cw, "noise reduction must be in [0, 1]");
                    if (!(k.humHz == 0.0 || k.humHz == 50.0 || k.humHz == 60.0)) err(cw, "hum removal is off, 50 or 60 Hz");
                    if (k.humHarmonics < 1 || k.humHarmonics > 8) err(cw, "hum harmonics must be 1 .. 8");
                    if (!(k.highPassHz == 0.0 || (k.highPassHz >= 20.0 && k.highPassHz <= 1000.0)))
                        err(cw, "high-pass cutoff must be off or 20 .. 1000 Hz");
                    if (!(k.lowPassHz == 0.0 || (k.lowPassHz >= 1000.0 && k.lowPassHz <= 22000.0)))
                        err(cw, "low-pass cutoff must be off or 1000 .. 22000 Hz");
                    if (!(k.targetLufs >= -40.0 && k.targetLufs <= -5.0)) err(cw, "loudness target must be in [-40, -5] LUFS");
                    if (k.measuredLufs && std::isnan(*k.measuredLufs)) err(cw, "measured loudness is not a number");
                    const AudioDucking& d = a.ducking;
                    if (!(d.amountDb >= -60.0 && d.amountDb <= 0.0)) err(cw, "ducking amount must be in [-60, 0] dB");
                    if (!(d.thresholdDb >= -90.0 && d.thresholdDb <= 0.0)) err(cw, "ducking threshold must be in [-90, 0] dBFS");
                    for (double ms : {d.attackMs, d.holdMs, d.releaseMs})
                        if (!(ms >= 0.0 && ms <= 10000.0)) err(cw, "ducking times must be in [0, 10000] ms");
                    if (d.enabled)
                        for (const auto& t : d.targets) {
                            const Track* target = seq.findTrack(t);
                            if (!target) warn(cw, "ducking target track " + t + " does not exist");
                            else if (target->kind != TrackKind::Audio) err(cw, "ducking targets must be audio tracks");
                        }
                    if (c.outTransition) {
                        const bool audioFade = isAudioCrossfade(c.outTransition->type);
                        if (tr.kind == TrackKind::Audio && !audioFade)
                            err(cw, "audio tracks take audio crossfades only (" + c.outTransition->type + ")");
                        if (tr.kind != TrackKind::Audio && audioFade) err(cw, "audio crossfades belong on audio tracks");
                    }
                }
                if (!c.aiCutout.isDefault()) {   // M7 Background Removal
                    const AiCutout& a = c.aiCutout;
                    if (tr.kind != TrackKind::Video || c.isGenerated()) err(cw, "background removal needs a video clip with media");
                    if (a.model != "rvm-mobilenetv3" && a.model != "birefnet-general-tiny")
                        err(cw, "unknown background removal model " + a.model);
                    if (a.quality < 0 || a.quality > 2) err(cw, "background removal quality must be 0, 1 or 2");
                    if (!(a.threshold >= 0.0 && a.threshold <= 0.9)) err(cw, "cutout threshold must be in [0, 0.9]");
                    if (!(a.edge >= -10.0 && a.edge <= 10.0)) err(cw, "cutout edge must be in [-10, 10] px");
                    if (!(a.feather >= 0.0 && a.feather <= 20.0)) err(cw, "cutout feather must be in [0, 20] px");
                    if (!(a.spill >= 0.0 && a.spill <= 1.0)) err(cw, "cutout spill cleanup must be in [0, 1]");
                    if (!(a.smoothing >= 0.0 && a.smoothing <= 1.0)) err(cw, "cutout smoothing must be in [0, 1]");
                    if (!a.modelSha.empty() && a.modelSha.size() != 64) err(cw, "cutout model hash must be a SHA-256");
                }
                if (!c.upscale.isDefault()) {   // M7 Video Upscaling
                    const ClipUpscale& u = c.upscale;
                    if (tr.kind != TrackKind::Video || c.isGenerated()) err(cw, "upscaling needs a video clip with media");
                    if (u.model != "general" && u.model != "anime") err(cw, "unknown upscaling model " + u.model);
                    if (u.scale < 2 || u.scale > 4) err(cw, "upscaling factor must be 2, 3 or 4");
                    if (u.scale == 3 && u.model == "general" && u.mode == UpscaleMode::Ai)
                        err(cw, "the general upscaling model has no 3x (use 2x or 4x, or the anime model)");
                    if (!(u.deflicker >= 0.0 && u.deflicker <= 1.0)) err(cw, "upscaling flicker reduction must be in [0, 1]");
                }
                {
                    const auto& tf = c.transform;
                    for (double v : {tf.cropLeft, tf.cropTop, tf.cropRight, tf.cropBottom})
                        if (!(v >= 0.0 && v < 1.0)) err(cw, "crop values must be in [0, 1)");
                    if (tf.cropLeft + tf.cropRight >= 1.0 || tf.cropTop + tf.cropBottom >= 1.0)
                        err(cw, "crop removes the whole picture");
                    for (const auto& m : c.masks) {
                        if (m.shape == MaskShape::Polygon && m.points.size() < 3)
                            err(cw, "polygon mask '" + m.id + "' needs at least 3 points");
                        if (m.cornerRadius < 0.0 || m.cornerRadius > 0.5) err(cw, "mask corner radius must be in [0, 0.5]");
                    }
                }
                for (const auto& k : c.opacity.keys())
                    if (k.value < 0.0 || k.value > 1.0) warn(cw, "opacity keyframe outside [0, 1]");
            }
        }
    }
    return out;
}

bool isAudioCrossfade(const std::string& transitionType) {
    return transitionType.rfind(kAudioCrossfadePrefix, 0) == 0 && transitionType.size() > std::char_traits<char>::length(kAudioCrossfadePrefix);
}

bool hasErrors(const std::vector<ValidationIssue>& issues) {
    return std::any_of(issues.begin(), issues.end(),
                       [](const ValidationIssue& i) { return i.severity == ValidationIssue::Severity::Error; });
}

std::string formatIssues(const std::vector<ValidationIssue>& issues) {
    std::ostringstream os;
    for (const auto& i : issues)
        os << (i.severity == ValidationIssue::Severity::Error ? "error: " : "warning: ") << i.where << ": "
           << i.message << "\n";
    return os.str();
}

}  // namespace fa
