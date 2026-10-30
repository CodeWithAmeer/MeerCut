#include "fa/model/commands.h"
#include "fa/model/time_map.h"

#include <algorithm>
#include <cmath>

namespace fa {

// ---- UndoStack -------------------------------------------------------------------------------

void UndoStack::executeMerged(std::unique_ptr<Command> cmd, const std::string& mergeKey) {
    const bool cleanAtTop = cleanIndex_ && *cleanIndex_ == done_.size();
    if (mergeKey.empty() || mergeKey != lastMergeKey_ || done_.empty() || cleanAtTop) {
        execute(std::move(cmd));
        lastMergeKey_ = mergeKey;
        return;
    }
    done_.back()->revert(project_);
    try {
        cmd->apply(project_);
    } catch (...) {
        done_.back()->apply(project_);   // restore the previous step
        throw;
    }
    done_.back() = std::move(cmd);
    undone_.clear();
}

void UndoStack::execute(std::unique_ptr<Command> cmd) {
    lastMergeKey_.clear();
    cmd->apply(project_);   // throws before any change on invalid edits
    if (cleanIndex_ && *cleanIndex_ > done_.size()) cleanIndex_.reset();   // clean state was in redo history
    done_.push_back(std::move(cmd));
    undone_.clear();
    if (done_.size() > limit_) {
        done_.erase(done_.begin());
        if (cleanIndex_) cleanIndex_ = *cleanIndex_ == 0 ? std::optional<size_t>{} : *cleanIndex_ - 1;
    }
}

void UndoStack::undo() {
    lastMergeKey_.clear();
    if (done_.empty()) throw EditError("nothing to undo");
    done_.back()->revert(project_);
    undone_.push_back(std::move(done_.back()));
    done_.pop_back();
}

void UndoStack::redo() {
    lastMergeKey_.clear();
    if (undone_.empty()) throw EditError("nothing to redo");
    undone_.back()->apply(project_);
    done_.push_back(std::move(undone_.back()));
    undone_.pop_back();
}

void UndoStack::clear() {
    lastMergeKey_.clear();
    done_.clear();
    undone_.clear();
    cleanIndex_ = 0;
}

namespace {

Sequence& needSequence(Project& p, const std::string& id) {
    Sequence* s = p.findSequence(id);
    if (!s) throw EditError("unknown sequence '" + id + "'");
    return *s;
}

Track& needTrack(Sequence& s, const std::string& id) {
    Track* t = s.findTrack(id);
    if (!t) throw EditError("unknown track '" + id + "'");
    return *t;
}

ClipLocation needClip(Project& p, Sequence& s, const std::string& clipId) {
    try {
        return p.locateClip(s, clipId);
    } catch (const ModelError& e) {
        throw EditError(e.what());
    }
}

void requireAligned(const Time& t, const Sequence& s, const char* what) {
    if (!isFrameAligned(t, s.frameRate))
        throw EditError(std::string(what) + " " + t.toString() + " s is not on a frame boundary of " +
                        s.frameRate.toString() + " fps");
}

// Throws if `c` would overlap any clip on `t` other than the one with id `ignoreId`.
void requireFree(const Track& t, const Clip& c, const std::string& ignoreId = {}) {
    for (const auto& o : t.clips)
        if (o.id != ignoreId && o.timelineRange().overlaps(c.timelineRange()))
            throw EditError("clip " + (c.id.empty() ? std::string("<new>") : c.id) + " would overlap " + o.id +
                            " on track " + t.id);
}

// M6: fades are timeline durations from the clip's edges; a shorter clip keeps them inside itself (the fade-in is
// kept first, the fade-out gets what is left).
void fitAudioFades(Clip& c) {
    ClipAudio& a = c.audio;
    if (a.fadeIn > c.duration) a.fadeIn = c.duration;
    if (a.fadeIn + a.fadeOut > c.duration) a.fadeOut = c.duration - a.fadeIn;
}

void requireSourceRange(const Project& p, const Sequence& s, const Clip& c) {
    if (c.isGenerated()) return;   // generated picture: any length, extends both ways (source time may be negative)
    const MediaSource* src = p.findSource(c.sourceId);
    if (!src) throw EditError("unknown source '" + c.sourceId + "'");
    if (c.sourceIn < Time(0)) throw EditError("source in-point before start of media");
    if (src->duration && exceedsMedia(c, s.frameRate, *src->duration))
        throw EditError("clip extends past the end of its media (" + c.sourceOut().toString() + " > " +
                        src->duration->toString() + " s)");
}

// Which clips a track kind accepts: media by stream, titles on video/subtitle tracks, adjustment layers on video.
void requireFitsTrack(const Project& p, const Track& t, const Clip& c) {
    if (c.isSticker()) {
        if (t.kind != TrackKind::Video) throw EditError("stickers go on video tracks");
        return;
    }
    if (c.isAdjustment()) {
        if (c.isTitle()) throw EditError("a clip cannot be both a title and an adjustment layer");
        if (t.kind != TrackKind::Video) throw EditError("adjustment layers go on video tracks");
        if (c.outTransition) throw EditError("adjustment layers cannot have transitions");
    } else if (c.isTitle()) {
        if (t.kind == TrackKind::Audio) throw EditError("titles go on video or subtitle tracks");
    } else {
        const MediaSource* src = p.findSource(c.sourceId);
        if (t.kind == TrackKind::Subtitle) throw EditError("subtitle tracks hold captions only");
        if (t.kind == TrackKind::Video && !src->hasVideo) throw EditError("source has no video for a video track");
        if (t.kind == TrackKind::Audio && !src->hasAudio) throw EditError("source has no audio for an audio track");
    }
}

// Memento of a set of tracks identified by id.
class TrackMemento {
public:
    void save(const Sequence& s, std::initializer_list<std::string> ids) {
        saved_.clear();
        for (const auto& id : ids)
            if (const Track* t = s.findTrack(id))
                if (std::none_of(saved_.begin(), saved_.end(), [&](const Track& x) { return x.id == id; }))
                    saved_.push_back(*t);
    }
    void restore(Sequence& s) const {
        for (const auto& t : saved_)
            if (Track* dst = s.findTrack(t.id)) *dst = t;
    }

private:
    std::vector<Track> saved_;
};

// ---- commands --------------------------------------------------------------------------------

class AddSourceCmd final : public Command {
public:
    explicit AddSourceCmd(MediaSource s) : src_(std::move(s)) {}
    void apply(Project& p) override {
        if (p.findSource(src_.id)) throw EditError("source id already exists: " + src_.id);
        p.sources.push_back(src_);
    }
    void revert(Project& p) override {
        std::erase_if(p.sources, [&](const MediaSource& s) { return s.id == src_.id; });
    }
    std::string label() const override { return "Import media"; }

private:
    MediaSource src_;
};

class TrackListCmd : public Command {
protected:
    explicit TrackListCmd(std::string seq) : seq_(std::move(seq)) {}
    void revert(Project& p) override { needSequence(p, seq_).tracks = before_; }
    void remember(Sequence& s) { before_ = s.tracks; }
    std::string seq_;
    std::vector<Track> before_;
};

class AddTrackCmd final : public TrackListCmd {
public:
    AddTrackCmd(std::string seq, Track t, std::optional<size_t> idx)
        : TrackListCmd(std::move(seq)), track_(std::move(t)), index_(idx) {}
    void apply(Project& p) override {
        Sequence& s = needSequence(p, seq_);
        const size_t idx = index_.value_or(s.tracks.size());
        if (idx > s.tracks.size()) throw EditError("track index out of range");
        remember(s);
        s.tracks.insert(s.tracks.begin() + static_cast<std::ptrdiff_t>(idx), track_);
    }
    std::string label() const override { return "Add track"; }

private:
    Track track_;
    std::optional<size_t> index_;
};

class RemoveTrackCmd final : public TrackListCmd {
public:
    RemoveTrackCmd(std::string seq, std::string track) : TrackListCmd(std::move(seq)), track_(std::move(track)) {}
    void apply(Project& p) override {
        Sequence& s = needSequence(p, seq_);
        needTrack(s, track_);
        remember(s);
        std::erase_if(s.tracks, [&](const Track& t) { return t.id == track_; });
    }
    std::string label() const override { return "Remove track"; }

private:
    std::string track_;
};

class TrackEditCmd : public Command {
protected:
    explicit TrackEditCmd(std::string seq) : seq_(std::move(seq)) {}
    void revert(Project& p) override { memento_.restore(needSequence(p, seq_)); }
    std::string seq_;
    TrackMemento memento_;
};

class InsertClipCmd final : public TrackEditCmd {
public:
    InsertClipCmd(std::string seq, std::string track, Clip c)
        : TrackEditCmd(std::move(seq)), track_(std::move(track)), clip_(std::move(c)) {}
    void apply(Project& p) override {
        Sequence& s = needSequence(p, seq_);
        Track& t = needTrack(s, track_);
        if (clip_.duration <= Time(0)) throw EditError("clip duration must be positive");
        if (clip_.timelineStart < Time(0)) throw EditError("clip cannot start before 0");
        requireAligned(clip_.timelineStart, s, "clip start");
        requireAligned(clip_.duration, s, "clip duration");
        requireSourceRange(p, s, clip_);
        requireFitsTrack(p, t, clip_);
        requireFree(t, clip_);
        memento_.save(s, {track_});
        t.clips.push_back(clip_);
        t.sortClips();
    }
    std::string label() const override { return "Insert clip"; }

private:
    std::string track_;
    Clip clip_;
};

class InsertClipsCmd final : public TrackEditCmd {
public:
    InsertClipsCmd(std::string seq, std::string track, std::vector<Clip> clips)
        : TrackEditCmd(std::move(seq)), track_(std::move(track)), clips_(std::move(clips)) {}
    void apply(Project& p) override {
        Sequence& s = needSequence(p, seq_);
        Track& t = needTrack(s, track_);
        for (const Clip& c : clips_) {
            if (c.duration <= Time(0)) throw EditError("clip duration must be positive");
            if (c.timelineStart < Time(0)) throw EditError("clip cannot start before 0");
            requireAligned(c.timelineStart, s, "clip start");
            requireAligned(c.duration, s, "clip duration");
            requireSourceRange(p, s, c);
            requireFitsTrack(p, t, c);
        }
        std::vector<const Clip*> all;   // existing + new, by start: neighbours must not overlap
        all.reserve(t.clips.size() + clips_.size());
        for (const auto& c : t.clips) all.push_back(&c);
        for (const auto& c : clips_) all.push_back(&c);
        std::sort(all.begin(), all.end(), [](const Clip* a, const Clip* b) { return a->timelineStart < b->timelineStart; });
        for (size_t i = 1; i < all.size(); ++i)
            if (all[i - 1]->timelineEnd() > all[i]->timelineStart)
                throw EditError("clip " + all[i]->id + " would overlap " + all[i - 1]->id + " on track " + t.id);
        memento_.save(s, {track_});
        t.clips.insert(t.clips.end(), clips_.begin(), clips_.end());
        t.sortClips();
    }
    std::string label() const override { return "Insert clips"; }

private:
    std::string track_;
    std::vector<Clip> clips_;
};

class RemoveClipCmd final : public TrackEditCmd {
public:
    RemoveClipCmd(std::string seq, std::string clip, bool ripple)
        : TrackEditCmd(std::move(seq)), clip_(std::move(clip)), ripple_(ripple) {}
    void apply(Project& p) override {
        Sequence& s = needSequence(p, seq_);
        ClipLocation loc = needClip(p, s, clip_);
        memento_.save(s, {loc.track->id});
        const Clip removed = loc.clip();
        loc.track->clips.erase(loc.track->clips.begin() + static_cast<std::ptrdiff_t>(loc.index));
        if (ripple_)
            for (auto& c : loc.track->clips)
                if (c.timelineStart >= removed.timelineEnd()) c.timelineStart -= removed.duration;
    }
    std::string label() const override { return ripple_ ? "Ripple delete" : "Delete clip"; }

private:
    std::string clip_;
    bool ripple_;
};

class MoveClipCmd final : public TrackEditCmd {
public:
    MoveClipCmd(std::string seq, std::string clip, Time start, std::optional<std::string> track)
        : TrackEditCmd(std::move(seq)), clip_(std::move(clip)), start_(start), track_(std::move(track)) {}
    void apply(Project& p) override {
        Sequence& s = needSequence(p, seq_);
        ClipLocation loc = needClip(p, s, clip_);
        Track& from = *loc.track;
        Track& to = track_ ? needTrack(s, *track_) : from;
        if (to.kind != from.kind) throw EditError("cannot move a clip to a track of another kind");
        if (start_ < Time(0)) throw EditError("clip cannot start before 0");
        requireAligned(start_, s, "clip start");
        Clip moved = loc.clip();
        moved.timelineStart = start_;
        requireFree(to, moved, moved.id);
        memento_.save(s, {from.id, to.id});
        from.clips.erase(from.clips.begin() + static_cast<std::ptrdiff_t>(loc.index));
        to.clips.push_back(moved);
        to.sortClips();
    }
    std::string label() const override { return "Move clip"; }

private:
    std::string clip_;
    Time start_;
    std::optional<std::string> track_;
};

class TrimClipCmd final : public TrackEditCmd {
public:
    TrimClipCmd(std::string seq, std::string clip, ClipEdge edge, Time t)
        : TrackEditCmd(std::move(seq)), clip_(std::move(clip)), edge_(edge), t_(t) {}
    void apply(Project& p) override {
        Sequence& s = needSequence(p, seq_);
        ClipLocation loc = needClip(p, s, clip_);
        requireAligned(t_, s, "trim point");
        Clip c = loc.clip();
        if (edge_ == ClipEdge::Head) {
            const Time delta = t_ - c.timelineStart;
            c.sourceIn = sourceInAfterHeadTrim(c, delta);   // keeps the speed map (M5)
            c.timelineStart = t_;
            c.duration -= delta;
        } else {
            const Time d = t_ - c.timelineStart;
            c.sourceIn = sourceInAfterDurationChange(c, d);   // reverse: the tail edge moves the in-point
            c.duration = d;
        }
        if (c.duration <= Time(0)) throw EditError("trim would make the clip empty");
        if (c.timelineStart < Time(0)) throw EditError("clip cannot start before 0");
        fitAudioFades(c);
        requireSourceRange(p, s, c);
        requireFree(*loc.track, c, c.id);
        memento_.save(s, {loc.track->id});
        loc.clip() = c;
    }
    std::string label() const override { return "Trim clip"; }

private:
    std::string clip_;
    ClipEdge edge_;
    Time t_;
};

class SplitClipCmd final : public TrackEditCmd {
public:
    SplitClipCmd(std::string seq, std::string clip, Time t, std::string newId, std::optional<std::string> rightLink)
        : TrackEditCmd(std::move(seq)), clip_(std::move(clip)), t_(t), newId_(std::move(newId)),
          rightLink_(std::move(rightLink)) {}
    void apply(Project& p) override {
        Sequence& s = needSequence(p, seq_);
        ClipLocation loc = needClip(p, s, clip_);
        requireAligned(t_, s, "split point");
        const Clip& c = loc.clip();
        if (!(c.timelineStart < t_ && t_ < c.timelineEnd())) throw EditError("split point must lie inside the clip");
        Clip left = c, right = c;
        left.duration = t_ - c.timelineStart;
        left.sourceIn = sourceInAfterDurationChange(c, left.duration);   // both halves keep the speed map (M5)
        left.outTransition.reset();
        right.id = newId_;
        if (rightLink_) right.linkId = *rightLink_;
        right.timelineStart = t_;
        right.sourceIn = sourceInAfterHeadTrim(c, left.duration);
        right.duration = c.duration - left.duration;
        left.audio.fadeOut = Time(0);   // M6: the fade-in stays at the left edge, the fade-out at the right one
        right.audio.fadeIn = Time(0);
        fitAudioFades(left);
        fitAudioFades(right);
        memento_.save(s, {loc.track->id});
        loc.clip() = left;
        loc.track->clips.insert(loc.track->clips.begin() + static_cast<std::ptrdiff_t>(loc.index + 1), right);
    }
    std::string label() const override { return "Split clip"; }

private:
    std::string clip_;
    Time t_;
    std::string newId_;
    std::optional<std::string> rightLink_;
};

class ModifyClipCmd final : public TrackEditCmd {
public:
    ModifyClipCmd(std::string seq, std::string clip, std::string label, std::function<void(Clip&)> f)
        : TrackEditCmd(std::move(seq)), clip_(std::move(clip)), label_(std::move(label)), f_(std::move(f)) {}
    void apply(Project& p) override {
        Sequence& s = needSequence(p, seq_);
        ClipLocation loc = needClip(p, s, clip_);
        Clip c = loc.clip();
        f_(c);
        if (c.id != clip_ || c.timelineStart != loc.clip().timelineStart || c.duration != loc.clip().duration)
            throw EditError("modify-clip may not change id or timing; use move/trim commands");
        const ClipSpeed& was = loc.clip().speed;   // the speed map changes timing: setSpeed / RetimeClip
        if (c.speed.speed != was.speed || c.speed.reverse != was.reverse || c.speed.freeze != was.freeze ||
            c.speed.ramp != was.ramp)
            throw EditError("modify-clip may not change the speed map; use the speed commands");
        if (c.audio.fadeIn < Time(0) || c.audio.fadeOut < Time(0) || c.audio.fadeIn + c.audio.fadeOut > c.duration)
            throw EditError("fades must fit inside the clip");
        requireSourceRange(p, s, c);
        memento_.save(s, {loc.track->id});
        loc.clip() = std::move(c);
    }
    std::string label() const override { return label_; }

private:
    std::string clip_;
    std::string label_;
    std::function<void(Clip&)> f_;
};

class RetimeClipCmd final : public TrackEditCmd {
public:
    RetimeClipCmd(std::string seq, std::string clip, ClipSpeed speed, Time duration)
        : TrackEditCmd(std::move(seq)), clip_(std::move(clip)), speed_(std::move(speed)), duration_(duration) {}
    void apply(Project& p) override {
        Sequence& s = needSequence(p, seq_);
        ClipLocation loc = needClip(p, s, clip_);
        requireAligned(duration_, s, "clip duration");
        if (duration_ <= Time(0)) throw EditError("a clip needs at least one frame");
        Clip c = loc.clip();
        c.speed = speed_;
        c.duration = duration_;
        fitAudioFades(c);
        if (const std::string why = speedProblem(c); !why.empty()) throw EditError(why);
        requireSourceRange(p, s, c);
        requireFree(*loc.track, c, c.id);
        memento_.save(s, {loc.track->id});
        loc.clip() = std::move(c);
    }
    std::string label() const override { return "Speed"; }

private:
    std::string clip_;
    ClipSpeed speed_;
    Time duration_;
};

class ModifyTrackCmd final : public TrackEditCmd {
public:
    ModifyTrackCmd(std::string seq, std::string track, std::string label, std::function<void(Track&)> f)
        : TrackEditCmd(std::move(seq)), track_(std::move(track)), label_(std::move(label)), f_(std::move(f)) {}
    void apply(Project& p) override {
        Sequence& s = needSequence(p, seq_);
        Track& t = needTrack(s, track_);
        Track changed = t;
        f_(changed);
        if (changed.id != t.id || changed.kind != t.kind || changed.clips != t.clips)
            throw EditError("modify-track may not change id, kind or clips");
        if (!std::isfinite(changed.volumeDb) || changed.volumeDb > 24.0)
            throw EditError("track volume must be finite and <= +24 dB");
        memento_.save(s, {track_});
        t = std::move(changed);
    }
    std::string label() const override { return label_; }

private:
    std::string track_;
    std::string label_;
    std::function<void(Track&)> f_;
};

class ShiftClipsCmd final : public TrackEditCmd {
public:
    ShiftClipsCmd(std::string seq, std::string track, Time from, Time delta)
        : TrackEditCmd(std::move(seq)), track_(std::move(track)), from_(from), delta_(delta) {}
    void apply(Project& p) override {
        Sequence& s = needSequence(p, seq_);
        Track& t = needTrack(s, track_);
        requireAligned(delta_, s, "shift amount");
        Track moved = t;
        for (auto& c : moved.clips)
            if (c.timelineStart >= from_) {
                c.timelineStart += delta_;
                if (c.timelineStart < Time(0)) throw EditError("ripple would move clip " + c.id + " before 0");
            }
        moved.sortClips();
        for (size_t i = 1; i < moved.clips.size(); ++i)
            if (moved.clips[i - 1].timelineEnd() > moved.clips[i].timelineStart)
                throw EditError("ripple would make clip " + moved.clips[i].id + " overlap " + moved.clips[i - 1].id);
        memento_.save(s, {track_});
        t = std::move(moved);
    }
    std::string label() const override { return "Ripple"; }

private:
    std::string track_;
    Time from_;
    Time delta_;
};

class MoveClipsByCmd final : public TrackEditCmd {
public:
    MoveClipsByCmd(std::string seq, std::string track, std::vector<std::string> ids, Time delta)
        : TrackEditCmd(std::move(seq)), track_(std::move(track)), ids_(std::move(ids)), delta_(delta) {}
    void apply(Project& p) override {
        Sequence& s = needSequence(p, seq_);
        Track& t = needTrack(s, track_);
        requireAligned(delta_, s, "move amount");
        Track moved = t;
        size_t found = 0;
        for (auto& c : moved.clips)
            if (std::find(ids_.begin(), ids_.end(), c.id) != ids_.end()) {
                c.timelineStart += delta_;
                ++found;
                if (c.timelineStart < Time(0)) throw EditError("clip " + c.id + " would start before 0");
            }
        if (found != ids_.size()) throw EditError("move: clip not found on track " + track_);
        moved.sortClips();
        for (size_t i = 1; i < moved.clips.size(); ++i)
            if (moved.clips[i - 1].timelineEnd() > moved.clips[i].timelineStart)
                throw EditError("clip " + moved.clips[i].id + " would overlap " + moved.clips[i - 1].id);
        memento_.save(s, {track_});
        t = std::move(moved);
    }
    std::string label() const override { return "Move clips"; }

private:
    std::string track_;
    std::vector<std::string> ids_;
    Time delta_;
};

class ModifySequenceCmd final : public Command {
public:
    ModifySequenceCmd(std::string seq, std::string label, std::function<void(Sequence&)> f)
        : seq_(std::move(seq)), label_(std::move(label)), f_(std::move(f)) {}
    void apply(Project& p) override {
        Sequence& s = needSequence(p, seq_);
        Sequence changed = s;
        f_(changed);
        if (changed.id != s.id || changed.tracks != s.tracks)
            throw EditError("modify-sequence may not change id or tracks");
        if (changed.width <= 0 || changed.height <= 0 || changed.width % 2 || changed.height % 2)
            throw EditError("sequence resolution must be positive and even");
        if (changed.sampleRate < 8000 || changed.sampleRate > 192000) throw EditError("unsupported sample rate");
        if (!(changed.limiterCeilingDb >= -20.0 && changed.limiterCeilingDb <= 0.0))
            throw EditError("limiter ceiling must be in [-20, 0] dBFS");
        for (const auto& t : changed.tracks)
            for (const auto& c : t.clips)
                if (!isFrameAligned(c.timelineStart, changed.frameRate) || !isFrameAligned(c.duration, changed.frameRate))
                    throw EditError("clip " + c.id + " is not frame-aligned at " + changed.frameRate.toString() + " fps");
        before_ = s;
        s = std::move(changed);
    }
    void revert(Project& p) override { needSequence(p, seq_) = before_; }
    std::string label() const override { return label_; }

private:
    std::string seq_;
    std::string label_;
    std::function<void(Sequence&)> f_;
    Sequence before_;
};

class ModifySourceCmd final : public Command {
public:
    ModifySourceCmd(std::string src, std::string label, std::function<void(MediaSource&)> f)
        : src_(std::move(src)), label_(std::move(label)), f_(std::move(f)) {}
    void apply(Project& p) override {
        MediaSource* s = p.findSource(src_);
        if (!s) throw EditError("unknown source '" + src_ + "'");
        MediaSource changed = *s;
        f_(changed);
        if (changed.id != s->id) throw EditError("modify-source may not change the id");
        if (changed.path.empty()) throw EditError("media path may not be empty");
        before_ = *s;
        *s = std::move(changed);
    }
    void revert(Project& p) override {
        if (MediaSource* s = p.findSource(src_)) *s = before_;
    }
    std::string label() const override { return label_; }

private:
    std::string src_;
    std::string label_;
    std::function<void(MediaSource&)> f_;
    MediaSource before_;
};

class CompositeCmd final : public Command {
public:
    CompositeCmd(std::string label, std::vector<std::unique_ptr<Command>> cmds)
        : label_(std::move(label)), cmds_(std::move(cmds)) {}
    void apply(Project& p) override {
        size_t done = 0;
        try {
            for (; done < cmds_.size(); ++done) cmds_[done]->apply(p);
        } catch (...) {
            while (done > 0) cmds_[--done]->revert(p);
            throw;
        }
    }
    void revert(Project& p) override {
        for (size_t i = cmds_.size(); i > 0; --i) cmds_[i - 1]->revert(p);
    }
    std::string label() const override { return label_; }

private:
    std::string label_;
    std::vector<std::unique_ptr<Command>> cmds_;
};

}  // namespace

std::unique_ptr<Command> makeAddSource(Project& p, MediaSource src) {
    if (src.id.empty()) src.id = p.newId("src");
    return std::make_unique<AddSourceCmd>(std::move(src));
}

std::unique_ptr<Command> makeAddTrack(Project& p, const std::string& sequenceId, TrackKind kind,
                                      std::optional<size_t> index, std::string name, std::string* trackIdOut) {
    Track t;
    t.id = p.newId(kind == TrackKind::Video ? "v" : kind == TrackKind::Audio ? "a" : "s");
    if (trackIdOut) *trackIdOut = t.id;
    t.kind = kind;
    t.name = std::move(name);
    return std::make_unique<AddTrackCmd>(sequenceId, std::move(t), index);
}

std::unique_ptr<Command> makeRemoveTrack(const std::string& sequenceId, const std::string& trackId) {
    return std::make_unique<RemoveTrackCmd>(sequenceId, trackId);
}

std::unique_ptr<Command> makeInsertClip(Project& p, const std::string& sequenceId, const std::string& trackId,
                                        Clip clip) {
    if (clip.id.empty()) clip.id = p.newId("clip");
    return std::make_unique<InsertClipCmd>(sequenceId, trackId, std::move(clip));
}

std::unique_ptr<Command> makeInsertClips(Project& p, const std::string& sequenceId, const std::string& trackId,
                                         std::vector<Clip> clips) {
    for (auto& c : clips)
        if (c.id.empty()) c.id = p.newId("clip");
    return std::make_unique<InsertClipsCmd>(sequenceId, trackId, std::move(clips));
}

std::unique_ptr<Command> makeRemoveClip(const std::string& sequenceId, const std::string& clipId, bool ripple) {
    return std::make_unique<RemoveClipCmd>(sequenceId, clipId, ripple);
}

std::unique_ptr<Command> makeMoveClip(const std::string& sequenceId, const std::string& clipId, Time newStart,
                                      std::optional<std::string> newTrackId) {
    return std::make_unique<MoveClipCmd>(sequenceId, clipId, newStart, std::move(newTrackId));
}

std::unique_ptr<Command> makeTrimClip(const std::string& sequenceId, const std::string& clipId, ClipEdge edge,
                                      Time newEdgeTime) {
    return std::make_unique<TrimClipCmd>(sequenceId, clipId, edge, newEdgeTime);
}

std::unique_ptr<Command> makeSplitClip(Project& p, const std::string& sequenceId, const std::string& clipId, Time t,
                                       std::optional<std::string> rightLinkId) {
    return std::make_unique<SplitClipCmd>(sequenceId, clipId, t, p.newId("clip"), std::move(rightLinkId));
}

std::unique_ptr<Command> makeModifyClip(const std::string& sequenceId, const std::string& clipId, std::string label,
                                        std::function<void(Clip&)> change) {
    return std::make_unique<ModifyClipCmd>(sequenceId, clipId, std::move(label), std::move(change));
}

std::unique_ptr<Command> makeRetimeClip(const std::string& sequenceId, const std::string& clipId, ClipSpeed speed,
                                        Time duration) {
    return std::make_unique<RetimeClipCmd>(sequenceId, clipId, std::move(speed), duration);
}

std::unique_ptr<Command> makeModifyTrack(const std::string& sequenceId, const std::string& trackId, std::string label,
                                         std::function<void(Track&)> change) {
    return std::make_unique<ModifyTrackCmd>(sequenceId, trackId, std::move(label), std::move(change));
}

std::unique_ptr<Command> makeShiftClips(const std::string& sequenceId, const std::string& trackId, Time from, Time delta) {
    return std::make_unique<ShiftClipsCmd>(sequenceId, trackId, from, delta);
}

std::unique_ptr<Command> makeMoveClipsBy(const std::string& sequenceId, const std::string& trackId,
                                         std::vector<std::string> clipIds, Time delta) {
    return std::make_unique<MoveClipsByCmd>(sequenceId, trackId, std::move(clipIds), delta);
}

std::unique_ptr<Command> makeModifySequence(const std::string& sequenceId, std::string label,
                                            std::function<void(Sequence&)> change) {
    return std::make_unique<ModifySequenceCmd>(sequenceId, std::move(label), std::move(change));
}

std::unique_ptr<Command> makeModifySource(const std::string& sourceId, std::string label,
                                          std::function<void(MediaSource&)> change) {
    return std::make_unique<ModifySourceCmd>(sourceId, std::move(label), std::move(change));
}

std::unique_ptr<Command> makeComposite(std::string label, std::vector<std::unique_ptr<Command>> commands) {
    return std::make_unique<CompositeCmd>(std::move(label), std::move(commands));
}

}  // namespace fa
