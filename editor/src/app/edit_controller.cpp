#include "edit_controller.h"

#include "templates.h"
#include "title_library.h"

#include "fa/text/stickers.h"

#include "fa/model/serialize.h"
#include "fa/model/stabilize_plan.h"
#include "fa/model/time_map.h"
#include "fa/render/curves.h"
#include "fa/render/effects.h"
#include "fa/render/lut.h"
#include "fa/render/renderer.h"

#include <QDir>
#include <QFileInfo>

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>

#include <QMessageBox>

#include <algorithm>
#include <set>
#include <cmath>
#include <QPushButton>

namespace fa::ui {

EditController::EditController(Document* doc, QWidget* dialogParent, QObject* parent)
    : QObject(parent), doc_(doc), dialogParent_(dialogParent) {}

bool EditController::run(std::unique_ptr<Command> cmd) {
    QString err;
    if (!doc_->execute(std::move(cmd), &err)) {
        emit message(err);
        return false;
    }
    return true;
}

namespace {
bool timelineEmpty(const Sequence& s) {
    for (const auto& t : s.tracks)
        if (!t.clips.empty()) return false;
    return true;
}
QString fpsText(const FrameRate& r) {
    return r.fps().isInteger() ? QString::number(r.fps().num()) : QString::number(r.toDouble(), 'f', 3);
}
}  // namespace

void EditController::maybeMatchSequence(const MediaSource& src) {
    const Sequence& s = doc_->sequence();
    if (!src.hasVideo || !src.frameRate || !timelineEmpty(s) || match_ == MatchPolicy::Never) return;
    const bool rateDiffers = *src.frameRate != s.frameRate;
    const bool sizeDiffers = src.width > 0 && src.height > 0 && (src.width != s.width || src.height != s.height) &&
                             src.width % 2 == 0 && src.height % 2 == 0;
    if (!rateDiffers && !sizeDiffers) return;
    bool doMatch = match_ == MatchPolicy::Always;
    if (match_ == MatchPolicy::Ask) {
        QMessageBox box(QMessageBox::Question, "Project settings",
                        QString("The media is %1x%2 at %3 fps; the project is %4x%5 at %6 fps.\n\n"
                                "Match the project settings to the media?")
                            .arg(src.width)
                            .arg(src.height)
                            .arg(fpsText(*src.frameRate))
                            .arg(s.width)
                            .arg(s.height)
                            .arg(fpsText(s.frameRate)),
                        QMessageBox::NoButton, dialogParent_);
        QPushButton* matchBtn = box.addButton("Match to media", QMessageBox::AcceptRole);
        box.addButton("Keep project settings", QMessageBox::RejectRole);
        box.setDefaultButton(matchBtn);
        box.exec();
        doMatch = box.clickedButton() == matchBtn;
    }
    if (!doMatch) return;
    const FrameRate rate = *src.frameRate;
    const int w = sizeDiffers ? src.width : s.width, h = sizeDiffers ? src.height : s.height;
    run(makeModifySequence(doc_->sequenceId(), "Match project to media", [rate, w, h](Sequence& q) {
        q.frameRate = rate;
        q.width = w;
        q.height = h;
    }));
}

bool EditController::addToTimeline(const std::string& sourceId, std::optional<Time> at, std::string videoTrackId,
                                   std::string audioTrackId) {
    const MediaSource* src = doc_->project().findSource(sourceId);
    if (!src) return false;
    maybeMatchSequence(*src);
    const Time t = at ? *at : doc_->playheadTime();
    edit::AddedClips added;
    Project& p = doc_->commandTarget();
    std::unique_ptr<Command> cmd;
    try {
        cmd = edit::addMedia(p, doc_->sequenceId(), sourceId, t, std::move(videoTrackId), &added, std::move(audioTrackId));
    } catch (const std::exception& e) {
        emit message(QString::fromUtf8(e.what()));
        return false;
    }
    if (!run(std::move(cmd))) return false;
    doc_->setSelection({added.videoClipId.empty() ? added.audioClipId : added.videoClipId});
    return true;
}

std::string EditController::selectedOrUnderPlayhead() const {
    if (!doc_->primarySelection().empty()) return doc_->primarySelection();
    const Sequence& s = doc_->sequence();
    const Time t = doc_->playheadTime();
    for (auto it = s.tracks.rbegin(); it != s.tracks.rend(); ++it)   // topmost track first
        if (it->kind == TrackKind::Video)
            if (const Clip* c = it->clipAt(t)) return c->id;
    for (const auto& tr : s.tracks)
        if (const Clip* c = tr.clipAt(t)) return c->id;
    return {};
}

#define FA_TRY_BUILD(expr)                              \
    std::unique_ptr<Command> cmd;                       \
    try {                                               \
        cmd = (expr);                                   \
    } catch (const std::exception& e) {                 \
        emit message(QString::fromUtf8(e.what()));      \
        return false;                                   \
    }                                                   \
    return run(std::move(cmd))

namespace {
Project& mut(Document* d) { return d->commandTarget(); }
}  // namespace

bool EditController::splitAtPlayhead() {
    std::vector<std::string> ids = doc_->selection();
    FA_TRY_BUILD(edit::splitAt(mut(doc_), doc_->sequenceId(), doc_->playheadTime(), ids));
}

bool EditController::deleteSelection(bool ripple) {
    if (const std::string tid = doc_->selectedTransition(); !tid.empty()) {   // Delete on a selected transition
        const bool ok = removeTransition(tid);
        if (ok) doc_->setSelectedTransition({});
        return ok;
    }
    if (doc_->selection().empty()) {
        emit message("Select a clip first");
        return false;
    }
    FA_TRY_BUILD(edit::deleteClips(mut(doc_), doc_->sequenceId(), doc_->selection(), ripple));
}

bool EditController::trimToPlayhead(ClipEdge edge) {
    const std::string id = selectedOrUnderPlayhead();
    if (id.empty()) {
        emit message("No clip under the playhead");
        return false;
    }
    FA_TRY_BUILD(edit::trimClip(mut(doc_), doc_->sequenceId(), id, edge, doc_->playheadTime()));
}

bool EditController::detachAudio() {
    const std::string id = doc_->primarySelection();
    if (id.empty()) return false;
    FA_TRY_BUILD(edit::unlink(mut(doc_), doc_->sequenceId(), id));
}

bool EditController::toggleClipEnabled() {
    const std::string id = doc_->primarySelection();
    if (id.empty()) return false;
    const Clip* c = edit::findClip(doc_->sequence(), id);
    const bool enable = !c->enabled;
    std::vector<std::unique_ptr<Command>> cmds;
    for (const auto& m : edit::linkGroup(doc_->sequence(), id))
        cmds.push_back(makeModifyClip(doc_->sequenceId(), m.clipId, enable ? "Enable clip" : "Disable clip",
                                      [enable](Clip& x) { x.enabled = enable; }));
    return run(makeComposite(enable ? "Enable clip" : "Disable clip", std::move(cmds)));
}

bool EditController::moveClip(const std::string& clipId, Time newStart, std::optional<std::string> trackId) {
    FA_TRY_BUILD(edit::moveClip(mut(doc_), doc_->sequenceId(), clipId, newStart, std::move(trackId)));
}

bool EditController::trimClip(const std::string& clipId, ClipEdge edge, Time newEdge) {
    FA_TRY_BUILD(edit::trimClip(mut(doc_), doc_->sequenceId(), clipId, edge, newEdge));
}

bool EditController::addTransition(const std::string& clipId, std::optional<Time> duration, TransitionAlignment a,
                                   const std::string& type) {
    const auto maxD = edit::maxTransitionDuration(doc_->sequence(), clipId, a);
    if (!maxD) {
        emit message("A transition needs a following clip that starts exactly where this clip ends");
        return false;
    }
    const FrameRate rate = doc_->sequence().frameRate;
    Time d = duration ? *duration : Time(1);
    d = min(d, *maxD);
    d = frameToTime(timeToFrame(d, rate, Rounding::Floor), rate);
    if (d <= Time(0)) {
        emit message("The clips are too short for a transition");
        return false;
    }
    TransitionRef ref{type, d, a};
    if (const Clip* c = edit::findClip(doc_->sequence(), clipId); c && c->outTransition) {   // keep settings
        ref.direction = c->outTransition->direction;
        ref.softness = c->outTransition->softness;
    }
    FA_TRY_BUILD(edit::setTransition(mut(doc_), doc_->sequenceId(), clipId, ref));
}

bool EditController::updateTransition(const std::string& clipId, Time duration, TransitionAlignment a) {
    const Clip* c = edit::findClip(doc_->sequence(), clipId);
    return addTransition(clipId, duration, a, c && c->outTransition ? c->outTransition->type : "fa.transition.dissolve");
}

bool EditController::removeTransition(const std::string& clipId) {
    FA_TRY_BUILD(edit::setTransition(mut(doc_), doc_->sequenceId(), clipId, std::nullopt));
}

std::string EditController::cutBefore(const std::string& trackId, Time t, Time tolerance) const {
    const Track* tr = doc_->sequence().findTrack(trackId);
    if (!tr) return {};
    std::string best;
    Time bestDist = tolerance;
    for (size_t i = 0; i + 1 < tr->clips.size(); ++i) {
        const Clip& c = tr->clips[i];
        if (tr->clips[i + 1].timelineStart != c.timelineEnd()) continue;
        Time dist = c.timelineEnd() - t;
        if (dist < Time(0)) dist = -dist;
        if (dist <= bestDist) {
            bestDist = dist;
            best = c.id;
        }
    }
    return best;
}

bool EditController::setClipSpeed(const std::string& clipId, const ClipSpeed& speed, const std::string& mergeKey) {
    if (clipId.empty()) return false;
    std::unique_ptr<Command> cmd;
    try {
        cmd = edit::setSpeed(mut(doc_), doc_->sequenceId(), clipId, speed);
    } catch (const std::exception& e) {
        emit message(QString::fromUtf8(e.what()));
        return false;
    }
    QString err;
    const bool ok = mergeKey.empty() ? doc_->execute(std::move(cmd), &err)
                                     : doc_->executeMerged(std::move(cmd), mergeKey, &err);
    if (!ok) emit message(err);
    return ok;
}

bool EditController::resetClipSpeed(const std::string& clipId) {
    const Clip* c = edit::findClip(doc_->sequence(), clipId);
    if (!c) return false;
    if (c->speed == ClipSpeed{}) return true;
    return setClipSpeed(clipId, ClipSpeed{});
}

bool EditController::toggleReverse(const std::string& clipId) {
    std::string id = clipId;
    if (id.empty())
        for (const auto& sel : doc_->selection())
            if (const Clip* c = edit::findClip(doc_->sequence(), sel); c && !c->isGenerated() && !c->speed.freeze) id = sel;
    const Clip* c = edit::findClip(doc_->sequence(), id);
    if (!c || c->isGenerated() || c->speed.freeze) {
        emit message("Select a video or audio clip to reverse");
        return false;
    }
    ClipSpeed sp = c->speed;
    sp.reverse = !sp.reverse;
    if (sp.reverse && sp.ramp.size() >= 2) {   // reverse works with a uniform speed
        sp.ramp.clear();
        sp.rampPreset.clear();
    }
    return setClipSpeed(id, sp);
}

bool EditController::freezeFrameAtPlayhead(const std::string& clipId, std::optional<Time> hold) {
    std::string id = clipId;
    if (id.empty()) {   // the selected video clip under the playhead
        for (const auto& sel : doc_->selection()) {
            const Track* tr = nullptr;
            const Clip* c = edit::findClip(doc_->sequence(), sel, &tr);
            if (c && tr->kind == TrackKind::Video && !c->isGenerated() && c->timelineStart <= doc_->playheadTime() &&
                doc_->playheadTime() < c->timelineEnd())
                id = sel;
        }
    }
    if (id.empty()) {
        emit message("Select a video clip under the playhead to freeze a frame");
        return false;
    }
    std::string holdId;
    std::unique_ptr<Command> cmd;
    try {
        cmd = edit::freezeFrame(mut(doc_), doc_->sequenceId(), id, doc_->playheadTime(),
                                hold.value_or(edit::kDefaultFreezeDuration), &holdId);
    } catch (const std::exception& e) {
        emit message(QString::fromUtf8(e.what()));
        return false;
    }
    if (!run(std::move(cmd))) return false;
    doc_->setSelection({holdId});
    return true;
}

bool EditController::setHoldDuration(const std::string& clipId, Time duration) {
    FA_TRY_BUILD(edit::setHoldDuration(mut(doc_), doc_->sequenceId(), clipId, duration));
}

// ---- M5: motion tracking -------------------------------------------------------------------------

bool EditController::setTracking(const std::string& clipId, std::optional<MotionTrack> track) {
    const bool had = [&] {
        const Clip* c = edit::findClip(doc_->sequence(), clipId);
        return c && c->tracking;
    }();
    if (!track && !had) return true;
    return modifyClip(clipId, track ? "Motion tracking" : "Remove tracking",
                      [t = std::move(track)](Clip& c) { c.tracking = t; });
}

std::vector<std::pair<int64_t, QPointF>> EditController::trackedPath(const std::string& clipId, QSizeF* regionSize) const {
    std::vector<std::pair<int64_t, QPointF>> out;
    const Sequence& seq = doc_->sequence();
    const Clip* c = edit::findClip(seq, clipId);
    if (!c || !c->tracking) return out;
    const MediaSource* src = doc_->project().findSource(c->sourceId);
    if (!src || src->width <= 0 || src->height <= 0) return out;
    const FrameRate rate = seq.frameRate;
    const Time d = rate.frameDuration();
    const int64_t k0 = timeToFrame(c->timelineStart, rate, Rounding::Ceil), k1 = timeToFrame(c->timelineEnd(), rate, Rounding::Ceil);
    const Rational fps = src->frameRate ? src->frameRate->fps() : rate.fps();
    for (int64_t k = k0; k < k1; ++k) {
        const Time t = frameToTime(k, rate);
        const int64_t f = (sampleFrame(*c, t - c->timelineStart, d, src->frameRate).time * fps).floor();
        const TrackingSample* s = trackSampleAt(*c->tracking, f);
        if (!s) continue;
        const Affine2D a = clipPlacement(*c, c->sourceTimeAt(t), src->width, src->height, seq.width, seq.height);
        const Vec2 p = a.apply(s->x * src->width, s->y * src->height);
        if (regionSize && out.empty()) {
            const double sx = std::hypot(a.a, a.d), sy = std::hypot(a.b, a.e);
            *regionSize = QSizeF(c->tracking->boxW * src->width * sx * s->scale, c->tracking->boxH * src->height * sy * s->scale);
        }
        out.push_back({k, QPointF(p.x, p.y)});
    }
    return out;
}

bool EditController::attachToTracking(const std::string& trackedClipId, const std::string& targetClipId) {
    const Sequence& seq = doc_->sequence();
    const Clip* target = edit::findClip(seq, targetClipId);
    if (!target) return false;
    const auto path = trackedPath(trackedClipId);
    if (path.empty()) {
        emit message("Track the clip first (Video > Tracking > Start)");
        return false;
    }
    const FrameRate rate = seq.frameRate;
    std::vector<std::pair<Time, QPointF>> keys;   // target source time -> position (canvas fractions from the centre)
    for (const auto& [k, p] : path) {
        const Time t = frameToTime(k, rate);
        if (t < target->timelineStart || !(t < target->timelineEnd())) continue;
        keys.push_back({target->sourceTimeAt(t), QPointF(p.x() / seq.width - 0.5, p.y() / seq.height - 0.5)});
    }
    if (keys.empty()) {
        emit message("That element does not overlap the tracked clip in time");
        return false;
    }
    // keep the element where it is relative to the tracked point at the first shared frame
    const Time t0 = keys.front().first;
    const QPointF offset(target->transform.positionX.value(t0) - keys.front().second.x(),
                         target->transform.positionY.value(t0) - keys.front().second.y());
    return modifyClip(targetClipId, "Follow tracking", [keys, offset](Clip& c) {
        c.transform.positionX.clearKeys();
        c.transform.positionY.clearKeys();
        for (const auto& [t, p] : keys) {
            Keyframe<double> kx, ky;
            kx.time = ky.time = t;
            kx.value = p.x() + offset.x();
            ky.value = p.y() + offset.y();
            c.transform.positionX.setKey(kx);
            c.transform.positionY.setKey(ky);
        }
    });
}

bool EditController::addTrackingMosaic(const std::string& trackedClipId, std::string* adjustmentIdOut) {
    const Sequence& seq = doc_->sequence();
    const Track* tr = nullptr;
    const Clip* c = edit::findClip(seq, trackedClipId, &tr);
    QSizeF size;
    const auto path = trackedPath(trackedClipId, &size);
    if (!c || path.empty()) {
        emit message("Track the clip first (Video > Tracking > Start)");
        return false;
    }
    const double H = seq.height, W = seq.width;
    Clip proto;
    proto.adjustment = true;
    proto.name = "Mosaic (tracking)";
    EffectRef px;
    px.id = "fx1";
    px.type = "fa.fx.pixelate";
    px.params["size"] = Animatable<double>(32.0);   // block size at 1080p: a clearly readable mosaic
    proto.effects.push_back(px);
    Mask m;
    m.id = "m1";
    m.name = "Tracked region";
    m.shape = MaskShape::Rectangle;
    m.width = Animatable<double>(size.width() / H);   // adjustment-layer masks: canvas heights from the centre
    m.height = Animatable<double>(size.height() / H);
    const FrameRate rate = seq.frameRate;
    for (const auto& [k, p] : path) {
        Keyframe<double> kx, ky;
        kx.time = ky.time = frameToTime(k, rate) - c->timelineStart;   // the layer starts with the clip, source in 0
        kx.value = (p.x() - W / 2) / H;
        ky.value = (p.y() - H / 2) / H;
        m.centerX.setKey(kx);
        m.centerY.setKey(ky);
    }
    proto.masks.push_back(m);
    std::string id;
    std::unique_ptr<Command> cmd;
    try {
        cmd = edit::addGenerated(mut(doc_), doc_->sequenceId(), proto, c->timelineStart, c->duration, tr->id, &id);
    } catch (const std::exception& e) {
        emit message(QString::fromUtf8(e.what()));
        return false;
    }
    if (!run(std::move(cmd))) return false;
    if (adjustmentIdOut) *adjustmentIdOut = id;
    doc_->setSelection({id});
    return true;
}

// ---- M5: stabilisation ------------------------------------------------------------------------------

bool EditController::setStabilization(const std::string& clipId, std::optional<Stabilization> result) {
    const Clip* c = edit::findClip(doc_->sequence(), clipId);
    if (!c) return false;
    if (!result && !c->stabilization) return true;
    return modifyClip(clipId, result ? "Stabilization" : "Remove stabilization",
                      [r = std::move(result)](Clip& cl) { cl.stabilization = r; });
}

bool EditController::setStabilizationSettings(const std::string& clipId, StabilizeLevel level, double strength,
                                              bool autoZoom, double maxZoom, const std::string& mergeKey) {
    const Clip* c = edit::findClip(doc_->sequence(), clipId);
    if (!c || !c->stabilization) return false;
    strength = std::clamp(strength, 0.0, 1.0);
    maxZoom = std::clamp(maxZoom, 1.0, kMaxStabilizeZoom);
    const Stabilization& s = *c->stabilization;
    if (s.level == level && s.strength == strength && s.autoZoom == autoZoom && s.maxZoom == maxZoom) return true;
    return modifyClip(
        clipId, "Stabilization settings",
        [level, strength, autoZoom, maxZoom](Clip& cl) {
            if (!cl.stabilization) return;
            cl.stabilization->level = level;
            cl.stabilization->strength = strength;
            cl.stabilization->autoZoom = autoZoom;
            cl.stabilization->maxZoom = maxZoom;
            planStabilization(*cl.stabilization);
        },
        mergeKey);
}

bool EditController::modifyClip(const std::string& clipId, const std::string& label, std::function<void(Clip&)> change,
                                const std::string& mergeKey) {
    if (clipId.empty()) return false;
    const Track* tr = nullptr;
    if (!edit::findClip(doc_->sequence(), clipId, &tr)) return false;
    if (tr->locked) {
        emit message("The track is locked");
        return false;
    }
    QString err;
    auto cmd = makeModifyClip(doc_->sequenceId(), clipId, label, std::move(change));
    const bool ok = mergeKey.empty() ? doc_->execute(std::move(cmd), &err)
                                     : doc_->executeMerged(std::move(cmd), mergeKey, &err);
    if (!ok) emit message(err);
    return ok;
}

bool EditController::setProperty(const std::string& clipId, const std::string& propertyId, double value) {
    const auto t = doc_->clipTimeAtPlayhead(clipId);
    if (!t) return false;
    const Time at = *t;
    return modifyClip(
        clipId, "Change " + propertyId,
        [propertyId, value, at](Clip& c) {
            Animatable<double>* a = keys::property(c, propertyId);
            if (!a) throw EditError("unknown property " + propertyId);
            keys::setValueAt(*a, at, value);
            // with the aspect lock on, height follows width (kept in sync for when the lock is released)
            if (propertyId == "transform.scaleX" && c.transform.uniformScale)
                keys::setValueAt(c.transform.scaleY, at, value);
        },
        clipId + "/" + propertyId);
}

bool EditController::toggleKeyframe(const std::string& clipId, const std::string& propertyId) {
    if (!doc_->playheadInside(clipId)) {
        emit message("Move the playhead over the clip to set a keyframe");
        return false;
    }
    const Time at = *doc_->clipTimeAtPlayhead(clipId);
    return modifyClip(clipId, "Keyframe", [propertyId, at](Clip& c) {
        Animatable<double>* a = keys::property(c, propertyId);
        if (!a) throw EditError("unknown property " + propertyId);
        keys::toggleKey(*a, at);
        if (propertyId == "transform.scaleX" && c.transform.uniformScale) {
            if (keys::hasKeyAt(*a, at) != keys::hasKeyAt(c.transform.scaleY, at)) keys::toggleKey(c.transform.scaleY, at);
        }
    });
}

bool EditController::setKeyframeStyle(const std::string& clipId, const Time& t, keys::Style style) {
    return modifyClip(clipId, std::string("Keyframe: ") + keys::styleLabel(style),
                      [t, style](Clip& c) { keys::setStyleAt(c, t, style); });
}

bool EditController::moveKeyframes(const std::string& clipId, const Time& from, const Time& to) {
    if (from == to) return false;
    return modifyClip(clipId, "Move keyframes", [from, to](Clip& c) {
        try {
            keys::moveKeysAt(c, from, to);
        } catch (const std::runtime_error& e) {
            throw EditError(e.what());
        }
    });
}

bool EditController::deleteKeyframes(const std::string& clipId, const Time& t) {
    return modifyClip(clipId, "Delete keyframes", [t](Clip& c) { keys::removeKeysAt(c, t); });
}

bool EditController::setBlendMode(const std::string& clipId, BlendMode mode) {
    return modifyClip(clipId, std::string("Blend mode: ") + blendModeLabel(mode), [mode](Clip& c) { c.blend = mode; });
}

std::string EditController::addMask(const std::string& clipId, MaskShape shape) {
    const std::string id = doc_->commandTarget().newId("mask");
    const char* name = shape == MaskShape::Rectangle ? "Rectangle" : shape == MaskShape::Ellipse ? "Ellipse" : "Custom";
    const bool ok = modifyClip(clipId, std::string("Add ") + name + " mask", [id, shape, name](Clip& c) {
        Mask m;
        m.id = id;
        m.name = name;
        m.shape = shape;
        if (shape == MaskShape::Polygon) {   // editable pentagon; drag its points in the monitor
            for (int i = 0; i < 5; ++i) {
                const double a = -3.14159265358979 / 2 + i * 2 * 3.14159265358979 / 5;
                m.points.push_back({0.5 * std::cos(a), 0.5 * std::sin(a)});
            }
        }
        c.masks.push_back(std::move(m));
    });
    if (!ok) return {};
    doc_->setActiveMask(id);
    return id;
}

bool EditController::removeMask(const std::string& clipId, const std::string& maskId) {
    const bool ok = modifyClip(clipId, "Delete mask", [maskId](Clip& c) {
        std::erase_if(c.masks, [&](const Mask& m) { return m.id == maskId; });
    });
    if (ok && doc_->activeMask() == maskId) doc_->setActiveMask({});
    return ok;
}

bool EditController::resetTransform(const std::string& clipId) {
    return modifyClip(clipId, "Reset transform", [](Clip& c) { c.transform = ClipTransform{}; });
}

bool EditController::updateTransitionSettings(const std::string& clipId, const TransitionRef& settings) {
    const auto maxD = edit::maxTransitionDuration(doc_->sequence(), clipId, settings.alignment);
    if (!maxD) return false;
    TransitionRef t = settings;
    const FrameRate rate = doc_->sequence().frameRate;
    t.duration = frameToTime(std::max<int64_t>(1, timeToFrame(min(t.duration, *maxD), rate, Rounding::Floor)), rate);
    FA_TRY_BUILD(edit::setTransition(mut(doc_), doc_->sequenceId(), clipId, t));
}

bool EditController::setClipOpacity(const std::string& clipId, double opacity) {
    opacity = std::clamp(opacity, 0.0, 1.0);
    return run(makeModifyClip(doc_->sequenceId(), clipId, "Opacity", [opacity](Clip& c) {
        c.opacity.clearKeys();
        c.opacity.setConstant(opacity);
    }));
}

bool EditController::setClipVolume(const std::string& clipId, double db) {
    return run(makeModifyClip(doc_->sequenceId(), clipId, "Volume", [db](Clip& c) {
        c.volumeDb.clearKeys();   // a plain volume (automation is edited through keyframes)
        c.volumeDb.setConstant(db);
    }));
}

bool EditController::setClipFades(const std::string& clipId, Time fadeIn, Time fadeOut, const std::string& mergeKey) {
    return modifyClip(clipId, "Audio fade", [fadeIn, fadeOut](Clip& c) {
        const Time zero(0);
        Time in = std::clamp(fadeIn, zero, c.duration);
        Time out = std::clamp(fadeOut, zero, c.duration);
        if (in + out > c.duration) {   // the value that changed gives way
            if (in != c.audio.fadeIn) in = c.duration - out;
            else out = c.duration - in;
        }
        c.audio.fadeIn = in;
        c.audio.fadeOut = out;
    }, mergeKey);
}

bool EditController::setClipEq(const std::string& clipId, bool enabled, const std::string& preset,
                               const std::array<double, kAudioEqBands>& gainsDb, const std::string& mergeKey) {
    std::array<double, kAudioEqBands> g = gainsDb;
    for (double& v : g) v = std::clamp(v, -12.0, 12.0);
    return modifyClip(clipId, "Equalizer", [enabled, preset, g](Clip& c) {
        c.audio.eqEnabled = enabled;
        c.audio.eqPreset = preset;
        c.audio.eqGainsDb = g;
    }, mergeKey);
}

bool EditController::setClipCleanup(const std::string& clipId, const AudioCleanup& cleanup, const std::string& mergeKey) {
    return modifyClip(clipId, "Audio cleanup", [cleanup](Clip& c) { c.audio.cleanup = cleanup; }, mergeKey);
}

bool EditController::setClipDucking(const std::string& clipId, const AudioDucking& ducking, const std::string& mergeKey) {
    return modifyClip(clipId, "Auto ducking", [ducking](Clip& c) { c.audio.ducking = ducking; }, mergeKey);
}

bool EditController::setLimiter(bool on, double ceilingDb, const std::string& mergeKey) {
    ceilingDb = std::clamp(ceilingDb, -20.0, 0.0);
    QString err;
    auto cmd = makeModifySequence(doc_->sequenceId(), "Master limiter", [on, ceilingDb](Sequence& s) {
        s.limiter = on;
        s.limiterCeilingDb = ceilingDb;
    });
    const bool ok = mergeKey.empty() ? doc_->execute(std::move(cmd), &err) : doc_->executeMerged(std::move(cmd), mergeKey, &err);
    if (!ok) emit message(err);
    return ok;
}

bool EditController::setMarkIn() {
    const Time at = frameToTime(doc_->playhead(), doc_->sequence().frameRate);
    const auto out = doc_->sequence().markOut;
    QString err;
    const bool ok = doc_->execute(makeModifySequence(doc_->sequenceId(), "Mark In", [at, out](Sequence& s) {
        s.markIn = at;
        if (out && !(at < *out)) s.markOut.reset();   // an Out mark before the new In mark is dropped
    }), &err);
    if (!ok) emit message(err);
    return ok;
}

bool EditController::setMarkOut() {
    const Time at = frameToTime(doc_->playhead() + 1, doc_->sequence().frameRate);   // the Out mark includes this frame
    const auto in = doc_->sequence().markIn;
    QString err;
    const bool ok = doc_->execute(makeModifySequence(doc_->sequenceId(), "Mark Out", [at, in](Sequence& s) {
        s.markOut = at;
        if (in && !(*in < at)) s.markIn.reset();
    }), &err);
    if (!ok) emit message(err);
    return ok;
}

bool EditController::clearMarks() {
    if (!doc_->sequence().markIn && !doc_->sequence().markOut) return false;
    QString err;
    const bool ok = doc_->execute(makeModifySequence(doc_->sequenceId(), "Clear In / Out", [](Sequence& s) {
        s.markIn.reset();
        s.markOut.reset();
    }), &err);
    if (!ok) emit message(err);
    return ok;
}

bool EditController::addAudioCrossfade(const std::string& clipId, const std::string& shapeId, std::optional<Time> duration) {
    const Track* tr = nullptr;
    if (!edit::findClip(doc_->sequence(), clipId, &tr) || tr->kind != TrackKind::Audio) {
        emit message("Audio crossfades go between two audio clips on the same track");
        return false;
    }
    return addTransition(clipId, duration, TransitionAlignment::Centered, kAudioCrossfadePrefix + shapeId);
}

bool EditController::renameClip(const std::string& clipId, const QString& name) {
    const std::string n = name.toUtf8().toStdString();
    return run(makeModifyClip(doc_->sequenceId(), clipId, "Rename clip", [n](Clip& c) { c.name = n; }));
}

bool EditController::setTrackFlag(const std::string& trackId, const QString& flag, bool value) {
    const QString label = (value ? "" : "Un") + QString(flag == "hidden" ? "hide" : flag == "muted" ? "mute" : flag == "solo" ? "solo" : "lock");
    return run(makeModifyTrack(doc_->sequenceId(), trackId, label.toStdString(), [flag, value](Track& t) {
        if (flag == "hidden") t.hidden = value;
        else if (flag == "muted") t.muted = value;
        else if (flag == "solo") t.solo = value;   // M6
        else if (flag == "locked") t.locked = value;
    }));
}

bool EditController::setTrackVolume(const std::string& trackId, double db) {
    return run(makeModifyTrack(doc_->sequenceId(), trackId, "Track volume", [db](Track& t) { t.volumeDb = db; }));
}

bool EditController::addBackgroundMedia(const std::string& clipId, const std::string& sourceId) {
    const Sequence& seq = doc_->sequence();
    const Track* tr = nullptr;
    const Clip* c = edit::findClip(seq, clipId, &tr);
    const MediaSource* src = doc_->project().findSource(sourceId);
    if (!c || !tr || !src) return false;
    if (!src->hasVideo) {
        emit message("The background must be a video");
        return false;
    }
    size_t index = 0;
    while (index < seq.tracks.size() && seq.tracks[index].id != tr->id) ++index;   // the new track takes this index: below
    Clip bg;
    bg.sourceId = sourceId;
    bg.name = "Background";
    bg.timelineStart = c->timelineStart;
    bg.duration = c->duration;
    if (src->duration && src->frameRate) {   // a video: at most its own length, whole frames of the sequence
        const Rational fps = seq.frameRate.fps();
        const Time avail = Time((*src->duration * fps).floor()) / fps;
        if (avail > Time(0) && avail < bg.duration) bg.duration = avail;
    }
    Project& p = doc_->commandTarget();
    std::string trackId;
    std::vector<std::unique_ptr<Command>> steps;
    steps.push_back(makeAddTrack(p, doc_->sequenceId(), TrackKind::Video, index, "Background", &trackId));
    steps.push_back(makeInsertClip(p, doc_->sequenceId(), trackId, bg));
    return run(makeComposite("Add Background Media", std::move(steps)));
}

bool EditController::addTrack(TrackKind kind) {
    const Sequence& s = doc_->sequence();
    const auto n = std::count_if(s.tracks.begin(), s.tracks.end(), [&](const Track& t) { return t.kind == kind; });
    const std::string name = (kind == TrackKind::Video ? "Video " : "Audio ") + std::to_string(n + 1);
    return run(makeAddTrack(mut(doc_), doc_->sequenceId(), kind, std::nullopt, name));
}

bool EditController::modifyTrack(const std::string& trackId, const std::string& label, std::function<void(Track&)> change,
                                 const std::string& mergeKey) {
    const Track* tr = doc_->sequence().findTrack(trackId);
    if (!tr) return false;
    if (tr->locked) {
        emit message("The track is locked");
        return false;
    }
    QString err;
    auto cmd = makeModifyTrack(doc_->sequenceId(), trackId, label, std::move(change));
    const bool ok = mergeKey.empty() ? doc_->execute(std::move(cmd), &err)
                                     : doc_->executeMerged(std::move(cmd), mergeKey, &err);
    if (!ok) emit message(err);
    return ok;
}

bool EditController::addTitleFromTemplate(const std::string& templateId, std::optional<Time> at, std::string trackId) {
    const text::TitleTemplate* t = titles_ ? titles_->find(templateId) : text::findTitleTemplate(templateId);
    if (!t || !t->prototype.title) {
        emit message("Unknown title template");
        return false;
    }
    const Time when = at ? *at : doc_->playheadTime();
    const Sequence& seq = doc_->sequence();
    const Track* target = trackId.empty() ? nullptr : seq.findTrack(trackId);
    std::string newId;
    std::unique_ptr<Command> cmd;
    try {
        if (t->caption) {
            const std::string tid = target && target->kind == TrackKind::Subtitle ? trackId : std::string();
            cmd = edit::addCaption(mut(doc_), doc_->sequenceId(), t->prototype.title->text, when, t->duration, tid, &newId,
                                   &t->prototype.title->style, t->id);
        } else {
            const std::string tid = target && target->kind == TrackKind::Video ? trackId : std::string();
            cmd = edit::addTitle(mut(doc_), doc_->sequenceId(), t->prototype, when, t->duration, tid, &newId);
        }
    } catch (const std::exception& e) {
        emit message(QString::fromUtf8(e.what()));
        return false;
    }
    if (!run(std::move(cmd))) return false;
    doc_->setSelection({newId});
    return true;
}

bool EditController::applyAnimationsToTrack(const std::string& clipId) {
    const Track* tr = nullptr;
    const Clip* src = edit::findClip(doc_->sequence(), clipId, &tr);
    if (!src || !src->title || !tr) return false;
    if (tr->locked) {
        emit message("The track is locked");
        return false;
    }
    const TextAnimation in = src->title->in, loop = src->title->loop, out = src->title->out;
    std::vector<std::unique_ptr<Command>> cmds;
    for (const auto& c : tr->clips)
        if (c.id != clipId && c.title)
            cmds.push_back(makeModifyClip(doc_->sequenceId(), c.id, "Animation", [in, loop, out](Clip& cl) {
                cl.title->in = in;
                cl.title->loop = loop;
                cl.title->out = out;
            }));
    if (cmds.empty()) return true;
    const size_t n = cmds.size();
    if (!run(makeComposite("Apply animation to all captions", std::move(cmds)))) return false;
    emit info(QString("Animation applied to %1 other caption(s)").arg(n));
    return true;
}

namespace {
constexpr const char* kMimeEffects = "application/x-meercut-effects";

EffectRef* findEffect(Clip& c, const std::string& id) {
    for (auto& e : c.effects)
        if (e.id == id) return &e;
    return nullptr;
}
}  // namespace

std::string EditController::addEffect(const std::string& clipId, const std::string& type, int index) {
    const EffectDef* def = findEffectDef(type);
    const Clip* c = edit::findClip(doc_->sequence(), clipId);
    if (!def || !c) {
        emit message(def ? QString("Select a clip first") : QString("Unknown effect"));
        return {};
    }
    const std::string id = keys::newEffectId(*c);
    EffectRef e = makeEffect(*def, id);
    const bool ok = modifyClip(clipId, "Add " + def->label, [e, index](Clip& cl) {
        const auto pos = index < 0 || index >= static_cast<int>(cl.effects.size()) ? cl.effects.end() : cl.effects.begin() + index;
        cl.effects.insert(pos, e);
    });
    return ok ? id : std::string();
}

bool EditController::modifyEffect(const std::string& clipId, const std::string& effectId, const std::string& label,
                                  std::function<void(EffectRef&)> change, const std::string& mergeKey) {
    return modifyClip(clipId, label, [effectId, change](Clip& c) {
        if (EffectRef* e = findEffect(c, effectId)) change(*e);
    }, mergeKey.empty() ? std::string() : clipId + "/" + effectId + "/" + mergeKey);
}

bool EditController::removeEffect(const std::string& clipId, const std::string& effectId) {
    return modifyClip(clipId, "Delete effect", [effectId](Clip& c) {
        std::erase_if(c.effects, [&](const EffectRef& e) { return e.id == effectId; });
    });
}

bool EditController::moveEffect(const std::string& clipId, const std::string& effectId, int newIndex) {
    const Clip* c = edit::findClip(doc_->sequence(), clipId);
    if (!c) return false;
    const int n = static_cast<int>(c->effects.size());
    if (newIndex < 0 || newIndex >= n) return false;
    return modifyClip(clipId, "Reorder effects", [effectId, newIndex](Clip& cl) {
        auto it = std::find_if(cl.effects.begin(), cl.effects.end(), [&](const EffectRef& e) { return e.id == effectId; });
        if (it == cl.effects.end()) return;
        EffectRef e = *it;
        cl.effects.erase(it);
        cl.effects.insert(cl.effects.begin() + std::min<int>(newIndex, static_cast<int>(cl.effects.size())), e);
    });
}

std::string EditController::duplicateEffect(const std::string& clipId, const std::string& effectId) {
    const Clip* c = edit::findClip(doc_->sequence(), clipId);
    if (!c) return {};
    const std::string id = keys::newEffectId(*c);
    const bool ok = modifyClip(clipId, "Duplicate effect", [effectId, id](Clip& cl) {
        for (size_t i = 0; i < cl.effects.size(); ++i)
            if (cl.effects[i].id == effectId) {
                EffectRef copy = cl.effects[i];
                copy.id = id;
                cl.effects.insert(cl.effects.begin() + static_cast<std::ptrdiff_t>(i) + 1, copy);
                return;
            }
    });
    return ok ? id : std::string();
}

bool EditController::setEffectEnabled(const std::string& clipId, const std::string& effectId, bool enabled) {
    return modifyEffect(clipId, effectId, enabled ? "Enable effect" : "Bypass effect", [enabled](EffectRef& e) { e.enabled = enabled; });
}

bool EditController::resetEffect(const std::string& clipId, const std::string& effectId) {
    return modifyEffect(clipId, effectId, "Reset effect", [](EffectRef& e) {
        if (const EffectDef* def = findEffectDef(e.type)) {
            EffectRef fresh = makeEffect(*def, e.id);
            fresh.enabled = e.enabled;
            fresh.strings = e.strings;   // keep the chosen LUT file
            fresh.masks = e.masks;       // and the selection
            e = fresh;
        }
    });
}

bool EditController::setEffectConstant(const std::string& clipId, const std::string& effectId, const std::string& param, double value) {
    return modifyEffect(clipId, effectId, "Change " + param, [param, value](EffectRef& e) { e.params[param] = Animatable<double>(value); },
                        param);
}

// ---- the Color tab -------------------------------------------------------------------------------------------

const std::vector<std::string>& EditController::colorTabTypes() {
    static const std::vector<std::string> v = {"fa.color.lut",    "fa.color.basic", "fa.color.hsl",  "fa.color.curves",
                                               "fa.color.wheels", "fa.fx.sharpen",  "fa.fx.vignette"};
    return v;
}

namespace {
int colorRank(const std::string& type) {
    const auto& v = EditController::colorTabTypes();
    const auto it = std::find(v.begin(), v.end(), type);
    return it == v.end() ? 1000 : static_cast<int>(it - v.begin());
}

EffectRef* firstOfType(Clip& c, const std::string& type) {
    for (auto& e : c.effects)
        if (e.type == type) return &e;
    return nullptr;
}

// Inserts `e` at its canonical place: Color-tab effects in Color-tab order, ahead of the clip's other effects.
EffectRef& insertColorEffect(Clip& c, EffectRef e) {
    const int rank = colorRank(e.type);
    const auto pos = std::find_if(c.effects.begin(), c.effects.end(), [&](const EffectRef& x) { return colorRank(x.type) > rank; });
    return *c.effects.insert(pos, std::move(e));
}

// The clip's first effect of `type`; a new one with neutral settings when it has none.
EffectRef& ensureColorEffect(Clip& c, const std::string& type) {
    if (EffectRef* e = firstOfType(c, type)) return *e;
    const EffectDef* def = findEffectDef(type);
    if (!def) throw EditError("unknown effect " + type);
    return insertColorEffect(c, makeEffect(*def, keys::newEffectId(c), true));
}
}  // namespace

const EffectRef* EditController::colorEffect(const Clip& c, const std::string& type) {
    for (const auto& e : c.effects)
        if (e.type == type) return &e;
    return nullptr;
}

bool EditController::setColorParams(const std::string& clipId, const std::string& type, const std::map<std::string, double>& values,
                                    const std::string& mergeKey) {
    const EffectDef* def = findEffectDef(type);
    const auto t = doc_->clipTimeAtPlayhead(clipId);
    if (!def || !t || values.empty()) return false;
    for (const auto& [p, v] : values)
        if (def->paramIndex(p) < 0) return false;
    const Time at = *t;
    return modifyClip(
        clipId, "Change " + def->label,
        [type, values, at](Clip& c) {
            EffectRef& e = ensureColorEffect(c, type);
            for (const auto& [p, v] : values) keys::setValueAt(e.params[p], at, v);
        },
        mergeKey.empty() ? std::string() : clipId + "/color/" + mergeKey);
}

bool EditController::setColorCurve(const std::string& clipId, const std::string& curveId, const std::vector<Vec2>& points,
                                   const std::string& mergeKey) {
    const EffectDef* def = findEffectDef("fa.color.curves");
    if (!def || std::none_of(def->curves.begin(), def->curves.end(), [&](const CurveDef& c) { return c.id == curveId; })) return false;
    const std::vector<Vec2> pts = normalizeCurve(points);
    return modifyClip(
        clipId, "Edit curve",
        [curveId, pts](Clip& c) { ensureColorEffect(c, "fa.color.curves").curves[curveId] = pts; },
        mergeKey.empty() ? std::string() : clipId + "/color/" + mergeKey);
}

bool EditController::toggleColorKey(const std::string& clipId, const std::string& type, const std::string& param) {
    const EffectDef* def = findEffectDef(type);
    if (!def || def->paramIndex(param) < 0) return false;
    if (!doc_->playheadInside(clipId)) {
        emit message("Move the playhead over the clip to set a keyframe");
        return false;
    }
    const Time at = *doc_->clipTimeAtPlayhead(clipId);
    return modifyClip(clipId, "Keyframe", [type, param, at](Clip& c) { keys::toggleKey(ensureColorEffect(c, type).params[param], at); });
}

bool EditController::applyColorEffects(const std::string& clipId, const std::vector<EffectRef>& effects, const std::string& label) {
    for (const auto& e : effects)
        if (colorRank(e.type) >= 1000 || !findEffectDef(e.type)) {
            emit message(QString::fromStdString("Not a Color tab effect: " + e.type));
            return false;
        }
    return modifyClip(clipId, label, [effects](Clip& c) {
        for (const std::string& type : colorTabTypes())   // the effects the Color tab shows
            if (EffectRef* e = firstOfType(c, type)) {
                const std::string id = e->id;
                std::erase_if(c.effects, [&](const EffectRef& x) { return x.id == id; });
            }
        for (EffectRef e : effects) {
            e.id = keys::newEffectId(c);
            insertColorEffect(c, std::move(e));
        }
    });
}

bool EditController::addEffectToClips(const std::vector<std::string>& clipIds, const EffectRef& effect) {
    const EffectDef* def = findEffectDef(effect.type);
    if (!def) {
        emit message("Unknown effect");
        return false;
    }
    std::vector<std::unique_ptr<Command>> cmds;
    for (const auto& id : clipIds) {
        const Track* tr = nullptr;
        const Clip* c = edit::findClip(doc_->sequence(), id, &tr);
        if (!c || tr->kind != TrackKind::Video) continue;
        if (tr->locked) {
            emit message("The track is locked");
            continue;
        }
        EffectRef e = effect;
        e.id = keys::newEffectId(*c);
        cmds.push_back(makeModifyClip(doc_->sequenceId(), id, "Add " + def->label, [e](Clip& cl) { cl.effects.push_back(e); }));
    }
    if (cmds.empty()) return false;
    const size_t n = cmds.size();
    if (!run(n == 1 ? std::move(cmds.front()) : makeComposite("Add " + def->label + " to " + std::to_string(n) + " clips", std::move(cmds))))
        return false;
    emit info(QString::fromStdString("Added " + def->label) + (n > 1 ? QString(" to %1 clips").arg(n) : QString()));
    return true;
}

std::filesystem::path EditController::assetDir() const {
    const QString f = doc_->filePath();
    if (f.isEmpty()) return std::filesystem::current_path();
    const std::string dir = QFileInfo(f).absolutePath().toStdString();
    return std::filesystem::path(std::u8string(dir.begin(), dir.end()));
}

bool EditController::setColorLut(const std::string& clipId, const std::string& reference) {
    std::string ref = reference;
    if (!ref.empty() && ref.rfind(kBuiltinLutPrefix, 0) != 0 && !doc_->filePath().isEmpty()) {
        // inside the project folder: relative, so the project and its LUTs can move together
        const QDir dir(QFileInfo(doc_->filePath()).absolutePath());
        const QString rel = dir.relativeFilePath(QString::fromStdString(ref));
        if (!rel.startsWith("..") && !QDir::isAbsolutePath(rel)) ref = rel.toStdString();
    }
    return modifyClip(clipId, ref.empty() ? "Remove LUT" : "Apply LUT",
                      [ref](Clip& c) { ensureColorEffect(c, "fa.color.lut").strings["lut"] = ref; });
}

int EditController::relinkLut(const std::string& oldReference, const std::string& newReference) {
    std::vector<std::unique_ptr<Command>> cmds;
    for (const auto& tr : doc_->sequence().tracks)
        for (const auto& c : tr.clips) {
            int n = 0;
            for (const auto& e : c.effects)
                if (const auto it = e.strings.find("lut"); it != e.strings.end() && it->second == oldReference) ++n;
            if (n == 0) continue;
            cmds.push_back(makeModifyClip(doc_->sequenceId(), c.id, "Relink LUT", [oldReference, newReference](Clip& cl) {
                for (auto& e : cl.effects)
                    if (auto it = e.strings.find("lut"); it != e.strings.end() && it->second == oldReference) it->second = newReference;
            }));
        }
    if (cmds.empty()) return 0;
    const int n = static_cast<int>(cmds.size());
    if (!run(makeComposite("Relink LUT", std::move(cmds)))) return 0;
    emit info(QString("Relinked the LUT in %1 clip(s)").arg(n));
    return n;
}

bool EditController::copyEffects(const std::string& clipId) {
    const Clip* c = edit::findClip(doc_->sequence(), clipId);
    if (!c || c->effects.empty()) {
        emit message("The clip has no effects to copy");
        return false;
    }
    Clip holder;   // the clip JSON format carries the effects (ids, keyframes, masks)
    holder.effects = c->effects;
    holder.adjustment = true;
    auto* m = new QMimeData();
    m->setData(kMimeEffects, QByteArray::fromStdString(clipToJsonString(holder, -1)));
    m->setText(QString("%1 MeerCut effect(s)").arg(c->effects.size()));
    QGuiApplication::clipboard()->setMimeData(m);
    emit info(QString("Copied %1 effect(s)").arg(c->effects.size()));
    return true;
}

bool EditController::canPasteEffects() const {
    const QMimeData* m = QGuiApplication::clipboard()->mimeData();
    return m && m->hasFormat(kMimeEffects);
}

bool EditController::pasteEffects(const std::string& clipId) {
    const QMimeData* m = QGuiApplication::clipboard()->mimeData();
    const Clip* c = edit::findClip(doc_->sequence(), clipId);
    if (!c || !m || !m->hasFormat(kMimeEffects)) return false;
    std::vector<EffectRef> fx;
    try {
        fx = clipFromJsonString(m->data(kMimeEffects).toStdString()).effects;
    } catch (const std::exception& e) {
        emit message(QString::fromUtf8(e.what()));
        return false;
    }
    Clip probe = *c;   // allocate fresh ids against the target clip
    for (auto& e : fx) {
        e.id = keys::newEffectId(probe);
        probe.effects.push_back(e);
    }
    return modifyClip(clipId, "Paste effects", [fx](Clip& cl) { cl.effects.insert(cl.effects.end(), fx.begin(), fx.end()); });
}

std::string EditController::addEffectMask(const std::string& clipId, const std::string& effectId, MaskShape shape) {
    const Clip* c0 = edit::findClip(doc_->sequence(), clipId);
    if (!c0 || std::none_of(c0->effects.begin(), c0->effects.end(), [&](const EffectRef& e) { return e.id == effectId; })) return {};
    const std::string id = doc_->commandTarget().newId("mask");
    const std::string name = shape == MaskShape::Rectangle ? "Rectangle" : "Ellipse";
    const bool ok = modifyClip(clipId, "Add " + name + " mask to effect", [effectId, id, shape, name](Clip& c) {
        for (auto& e : c.effects)
            if (e.id == effectId) {
                Mask m;
                m.id = id;
                m.name = name;
                m.shape = shape;
                e.masks.push_back(std::move(m));
            }
    });
    if (!ok) return {};
    doc_->setActiveMask(effectId + "/" + id);
    return id;
}

bool EditController::removeEffectMask(const std::string& clipId, const std::string& effectId, const std::string& maskId) {
    const bool ok = modifyClip(clipId, "Delete effect mask", [effectId, maskId](Clip& c) {
        for (auto& e : c.effects)
            if (e.id == effectId) std::erase_if(e.masks, [&](const Mask& m) { return m.id == maskId; });
    });
    if (ok && doc_->activeMask() == effectId + "/" + maskId) doc_->setActiveMask({});
    return ok;
}

std::string EditController::addEffectLayer(const EffectRef& effect, const std::string& name, std::optional<Time> at,
                                           Time duration, std::string trackId) {
    if (effect.type.empty() || !findEffectDef(effect.type)) {
        emit message("Unknown effect: " + QString::fromStdString(effect.type));
        return {};
    }
    Clip proto;
    proto.adjustment = true;
    proto.name = name.empty() ? findEffectDef(effect.type)->label : name;
    EffectRef fx = effect;
    fx.id = "fx1";   // the layer's only effect
    proto.effects = {fx};
    std::string id;
    std::unique_ptr<Command> cmd;
    try {
        cmd = edit::addGenerated(mut(doc_), doc_->sequenceId(), proto, at ? *at : doc_->playheadTime(), duration,
                                 std::move(trackId), &id);
    } catch (const std::exception& e) {
        emit message(QString::fromUtf8(e.what()));
        return {};
    }
    if (!run(std::move(cmd))) return {};
    doc_->setSelection({id});
    return id;
}

bool EditController::addAdjustmentLayer(std::optional<Time> at, Time duration, std::string trackId) {
    Clip proto;
    proto.adjustment = true;
    proto.name = "Adjustment Layer";
    std::string id;
    std::unique_ptr<Command> cmd;
    try {
        cmd = edit::addGenerated(mut(doc_), doc_->sequenceId(), proto, at ? *at : doc_->playheadTime(), duration, trackId, &id);
    } catch (const std::exception& e) {
        emit message(QString::fromUtf8(e.what()));
        return false;
    }
    if (!run(std::move(cmd))) return false;
    doc_->setSelection({id});
    return true;
}

bool EditController::modifyTitleStyle(const std::string& clipId, const std::string& label,
                                      std::function<void(TextStyle&)> change, const std::string& mergeKey) {
    const Track* tr = nullptr;
    const Clip* c = edit::findClip(doc_->sequence(), clipId, &tr);
    if (!c || !c->title) return false;
    if (tr->kind == TrackKind::Subtitle && c->title->followTrackStyle)
        return modifyTrack(tr->id, label, [change](Track& t) { change(t.subtitleStyle); },
                           mergeKey.empty() ? std::string() : tr->id + "/" + mergeKey);
    return modifyClip(clipId, label, [change](Clip& cl) {
        if (cl.title) change(cl.title->style);
    }, mergeKey.empty() ? std::string() : clipId + "/" + mergeKey);
}

// ---- M9: clipboard and the clip menu's commands ---------------------------------------------------

namespace {
constexpr const char* kMimeClips = "application/x-meercut-clips";
}  // namespace

bool EditController::copySelection() {
    if (doc_->selection().empty()) {
        emit message("Select a clip first");
        return false;
    }
    std::vector<edit::CopiedClip> clips;
    try {
        clips = edit::copyClips(doc_->sequence(), doc_->selection());
    } catch (const std::exception& e) {
        emit message(QString::fromUtf8(e.what()));
        return false;
    }
    const std::string json = edit::clipboardToJson(clips);   // the clip JSON carries effects, keys, masks, speed ...
    auto* m = new QMimeData();
    m->setData(kMimeClips, QByteArray::fromStdString(json));
    m->setText(QString("%1 MeerCut clip(s)").arg(clips.size()));
    QGuiApplication::clipboard()->setMimeData(m);
    emit info(QString("Copied %1 clip(s)").arg(clips.size()));
    return true;
}

bool EditController::cutSelection() {
    if (!copySelection()) return false;
    return deleteSelection(false);
}

bool EditController::canPaste() const {
    const QMimeData* m = QGuiApplication::clipboard()->mimeData();
    return m && m->hasFormat(kMimeClips);
}

bool EditController::paste(std::optional<Time> at) {
    const QMimeData* m = QGuiApplication::clipboard()->mimeData();
    if (!m || !m->hasFormat(kMimeClips)) {
        emit message("Nothing to paste: copy a clip first (Ctrl+C)");
        return false;
    }
    std::vector<edit::CopiedClip> clips;
    try {
        clips = edit::clipboardFromJson(m->data(kMimeClips).toStdString());
    } catch (const std::exception& e) {
        emit message(QString("The clipboard holds no usable clips: ") + e.what());
        return false;
    }
    std::vector<std::string> ids;
    std::unique_ptr<Command> cmd;
    try {
        cmd = edit::pasteClips(mut(doc_), doc_->sequenceId(), clips, at ? *at : doc_->playheadTime(), &ids);
    } catch (const std::exception& e) {
        emit message(QString::fromUtf8(e.what()));
        return false;
    }
    if (!run(std::move(cmd))) return false;
    doc_->setSelection(ids);
    return true;
}

bool EditController::duplicateSelection() {
    if (doc_->selection().empty()) {
        emit message("Select a clip first");
        return false;
    }
    Time end(0);
    for (const auto& c : edit::copyClips(doc_->sequence(), doc_->selection())) end = max(end, c.clip.timelineEnd());
    if (!copySelection()) return false;
    return paste(end);
}

bool EditController::selectAll() {
    std::vector<std::string> ids;
    for (const auto& t : doc_->sequence().tracks)
        if (!t.locked)
            for (const auto& c : t.clips) ids.push_back(c.id);
    doc_->setSelection(ids);
    return !ids.empty();
}

bool EditController::setColorMark(int mark) {
    if (doc_->selection().empty() || mark < 0 || mark > kColorMarkCount) return false;
    std::vector<std::unique_ptr<Command>> cmds;
    for (const auto& id : doc_->selection())
        cmds.push_back(makeModifyClip(doc_->sequenceId(), id, "Color mark", [mark](Clip& c) { c.colorMark = mark; }));
    return run(makeComposite(mark ? "Color mark" : "Remove color mark", std::move(cmds)));
}

bool EditController::selectSameColorMark() {
    const Clip* c = edit::findClip(doc_->sequence(), doc_->primarySelection());
    if (!c || c->colorMark == 0) {
        emit message("Give the clip a color mark first");
        return false;
    }
    std::vector<std::string> ids;
    for (const auto& t : doc_->sequence().tracks)
        for (const auto& o : t.clips)
            if (o.colorMark == c->colorMark) ids.push_back(o.id);
    doc_->setSelection(ids);
    return true;
}

bool EditController::toggleMute() {
    if (doc_->selection().empty()) return false;
    std::set<std::string> ids;
    for (const auto& id : doc_->selection())
        for (const auto& r : edit::linkGroup(doc_->sequence(), id)) ids.insert(r.clipId);
    bool anyAudible = false;
    for (const auto& id : ids)
        if (const Clip* c = edit::findClip(doc_->sequence(), id); c && !c->muted) anyAudible = true;
    std::vector<std::unique_ptr<Command>> cmds;
    for (const auto& id : ids)
        cmds.push_back(makeModifyClip(doc_->sequenceId(), id, "Mute", [anyAudible](Clip& c) { c.muted = anyAudible; }));
    if (!run(makeComposite(anyAudible ? "Mute" : "Unmute", std::move(cmds)))) return false;
    emit info(anyAudible ? "Muted" : "Unmuted");
    return true;
}

namespace {
std::vector<std::string> selectedVideoClips(const Document* doc) {
    std::vector<std::string> out;
    for (const auto& id : doc->selection()) {
        const Track* tr = nullptr;
        if (const Clip* c = edit::findClip(doc->sequence(), id, &tr); c && tr->kind == TrackKind::Video && !c->isAdjustment())
            out.push_back(id);
    }
    return out;
}
}  // namespace

bool EditController::rotate(double degrees) {
    const auto ids = selectedVideoClips(doc_);
    if (ids.empty()) {
        emit message("Select a video clip first");
        return false;
    }
    std::vector<std::unique_ptr<Command>> cmds;
    for (const auto& id : ids)
        cmds.push_back(makeModifyClip(doc_->sequenceId(), id, "Rotate", [degrees](Clip& c) {
            Animatable<double>& r = c.transform.rotation;
            if (!r.isAnimated()) {
                r.setConstant(std::fmod(r.constantValue() + degrees, 360.0));
                return;
            }
            auto keys = r.keys();
            for (auto& k : keys) {
                k.value += degrees;
                r.setKey(k);
            }
        }));
    return run(makeComposite("Rotate", std::move(cmds)));
}

bool EditController::cropToFit() {
    const auto ids = selectedVideoClips(doc_);
    const Sequence& s = doc_->sequence();
    std::vector<std::unique_ptr<Command>> cmds;
    for (const auto& id : ids) {
        const Clip* c = edit::findClip(s, id);
        const MediaSource* src = c ? doc_->project().findSource(c->sourceId) : nullptr;
        if (!src || src->width <= 0 || src->height <= 0) continue;
        // scale 1 shows the whole picture inside the frame (bars on the long side); fill = the ratio of the aspects
        const double ma = double(src->width) / src->height, ca = double(s.width) / s.height;
        const double fill = std::max(ma / ca, ca / ma);
        cmds.push_back(makeModifyClip(doc_->sequenceId(), id, "Crop to Fit", [fill](Clip& x) {
            for (Animatable<double>* a : {&x.transform.scaleX, &x.transform.scaleY}) {
                a->clearKeys();
                a->setConstant(fill);
            }
            for (Animatable<double>* a : {&x.transform.positionX, &x.transform.positionY}) {
                a->clearKeys();
                a->setConstant(0.0);
            }
        }));
    }
    if (cmds.empty()) {
        emit message("Select a video clip first");
        return false;
    }
    return run(makeComposite("Crop to Fit", std::move(cmds)));
}

bool EditController::setSpeedFactor(const Rational& speed) {
    const std::string id = doc_->primarySelection();
    const Clip* c = edit::findClip(doc_->sequence(), id);
    if (!c || c->isGenerated()) {
        emit message("Select a video or audio clip first");
        return false;
    }
    ClipSpeed sp = c->speed;
    sp.freeze = false;
    sp.ramp.clear();
    sp.rampPreset.clear();
    sp.speed = speed;
    return setClipSpeed(id, sp);
}

bool EditController::selectClipRange() {
    const Clip* c = edit::findClip(doc_->sequence(), doc_->primarySelection());
    if (!c) {
        emit message("Select a clip first");
        return false;
    }
    const Time in = c->timelineStart, out = c->timelineEnd();
    return run(makeModifySequence(doc_->sequenceId(), "Select clip range", [in, out](Sequence& q) {
        q.markIn = in;
        q.markOut = out;
    }));
}

bool EditController::replaceClip(const std::string& clipId, const std::string& sourceId) {
    const Track* tr = nullptr;
    const Clip* c = edit::findClip(doc_->sequence(), clipId, &tr);
    const MediaSource* src = doc_->project().findSource(sourceId);
    if (!c || !src) return false;
    if (tr->kind == TrackKind::Video && !src->hasVideo) {
        emit message("Replace a video clip with a video or picture");
        return false;
    }
    if (tr->kind == TrackKind::Audio && !src->hasAudio) {
        emit message("Replace an audio clip with audio");
        return false;
    }
    if (tr->kind == TrackKind::Subtitle || c->isAdjustment()) {
        emit message("This clip cannot be replaced with media");
        return false;
    }
    Time length = c->duration;
    try {
        length = min(length, edit::fullClipDuration(*src, doc_->sequence().frameRate));
    } catch (const std::exception& e) {
        emit message(QString::fromUtf8(e.what()));
        return false;
    }
    const std::string sid = sourceId;
    const QString name = QFileInfo(QString::fromUtf8(src->path)).fileName();
    std::vector<std::unique_ptr<Command>> cmds;
    if (length < c->duration) cmds.push_back(makeTrimClip(doc_->sequenceId(), clipId, ClipEdge::Tail, c->timelineStart + length));
    cmds.push_back(makeModifyClip(doc_->sequenceId(), clipId, "Replace clip", [sid, name](Clip& x) {
        x.sourceId = sid;
        x.sourceIn = Time(0);
        x.title.reset();
        x.sticker.reset();   // a template's media placeholder becomes the media
        x.adjustment = false;
        x.speed = ClipSpeed{};
        x.tracking.reset();
        x.stabilization.reset();
        x.name = name.toStdString();
    }));
    if (!run(makeComposite("Replace clip", std::move(cmds)))) return false;
    emit info("Replaced with " + name);
    return true;
}

std::string EditController::addSticker(const std::string& stickerId, std::optional<Time> at, std::string trackId) {
    const text::StickerInfo* sinfo = text::findSticker(stickerId);
    if (!sinfo) {
        emit message("Unknown sticker: " + QString::fromStdString(stickerId));
        return {};
    }
    Clip proto;
    proto.sticker = StickerData{stickerId, 0, {}};
    proto.name = sinfo->name;
    proto.transform.scaleX.setConstant(sinfo->defaultScale);
    proto.transform.scaleY.setConstant(sinfo->defaultScale);
    std::string id;
    std::unique_ptr<Command> cmd;
    try {
        cmd = edit::addGenerated(mut(doc_), doc_->sequenceId(), proto, at ? *at : doc_->playheadTime(),
                                 Time::fromDouble(sinfo->defaultSeconds, 1000), std::move(trackId), &id);
    } catch (const std::exception& e) {
        emit message(QString::fromUtf8(e.what()));
        return {};
    }
    if (!run(std::move(cmd))) return {};
    doc_->setSelection({id});
    return id;
}

std::vector<std::string> EditController::addTemplate(const std::string& templateId, std::optional<Time> at) {
    const TemplateInfo* tpl = findTemplate(templateId);
    const auto clips = templateClips(templateId, doc_->sequence().frameRate);
    if (!tpl || clips.empty()) {
        emit message("Unknown template: " + QString::fromStdString(templateId));
        return {};
    }
    std::vector<std::string> ids;
    std::unique_ptr<Command> cmd;
    try {
        cmd = edit::pasteClips(mut(doc_), doc_->sequenceId(), clips, at ? *at : doc_->playheadTime(), &ids, true);
    } catch (const std::exception& e) {
        emit message(QString::fromUtf8(e.what()));
        return {};
    }
    if (!run(std::move(cmd))) return {};
    doc_->setSelection(ids);
    bool mediaSlots = false;
    for (const auto& c : clips) mediaSlots = mediaSlots || (c.clip.sticker && c.clip.sticker->id == "placeholder");
    emit info(QString::fromStdString(tpl->name) + " added" +
              (mediaSlots ? QString(" - drop your videos on the grey slots, or right-click a slot > Replace Clip") : QString()));
    return ids;
}

// ---- M10: gaps ------------------------------------------------------------------------------------------------

bool EditController::deleteGap(const std::string& trackId, Time at) {
    const Track* tr = doc_->sequence().findTrack(trackId);
    const auto gap = tr ? edit::gapAt(*tr, at) : std::nullopt;
    std::unique_ptr<Command> cmd;
    try {
        cmd = edit::deleteGap(mut(doc_), doc_->sequenceId(), trackId, at);
    } catch (const std::exception& e) {
        emit message(QString::fromUtf8(e.what()));
        return false;
    }
    if (!run(std::move(cmd))) return false;
    if (gap) emit info(QString("Deleted a gap of %1 s").arg(gap->duration.toDouble(), 0, 'f', 2));
    return true;
}

QString EditController::gapProblem(const std::string& trackId, Time at) const {
    return QString::fromStdString(edit::gapProblem(doc_->sequence(), trackId, at));
}

bool EditController::setStickerLook(const std::string& clipId, uint32_t color, const QString& text) {
    const std::string t = text.toStdString();
    return modifyClip(clipId, "Sticker", [color, t](Clip& c) {
        if (!c.sticker) return;
        c.sticker->color = color & 0xFFFFFF;
        c.sticker->text = t;
    });
}

}  // namespace fa::ui
