#include "fa/model/edit_ops.h"

#include "fa/model/serialize.h"
#include "fa/model/time_map.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <tuple>
#include <filesystem>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace fa::edit {

// ---- Builder ---------------------------------------------------------------------------------

Sequence& Builder::sequence(const std::string& id) {
    Sequence* s = scratch_.findSequence(id);
    if (!s) throw EditError("unknown sequence '" + id + "'");
    return *s;
}

void Builder::add(std::unique_ptr<Command> cmd) {
    cmd->apply(scratch_);   // throws EditError -> nothing has touched the real project
    cmds_.push_back(std::move(cmd));
}

std::unique_ptr<Command> Builder::finish(std::string label) {
    if (cmds_.empty()) throw EditError("nothing to do");
    real_.nextId = scratch_.nextId;   // ids handed out while building are now taken
    return makeComposite(std::move(label), std::move(cmds_));
}

// ---- queries ---------------------------------------------------------------------------------

const Clip* findClip(const Sequence& seq, const std::string& clipId, const Track** trackOut) {
    for (const auto& t : seq.tracks)
        for (const auto& c : t.clips)
            if (c.id == clipId) {
                if (trackOut) *trackOut = &t;
                return &c;
            }
    return nullptr;
}

std::vector<ClipRef> linkGroup(const Sequence& seq, const std::string& clipId) {
    const Track* tr = nullptr;
    const Clip* c = findClip(seq, clipId, &tr);
    if (!c) throw EditError("clip not found: " + clipId);
    std::vector<ClipRef> out{{tr->id, c->id}};
    if (c->linkId.empty()) return out;
    for (const auto& t : seq.tracks)
        for (const auto& o : t.clips)
            if (o.id != c->id && o.linkId == c->linkId) out.push_back({t.id, o.id});
    return out;
}

Time mediaDuration(const MediaSource& src) {
    if (!src.duration) throw EditError("media '" + src.path + "' has not been probed (unknown duration)");
    return *src.duration;
}

Time fullClipDuration(const MediaSource& src, const FrameRate& rate) {
    const int64_t frames = timeToFrame(mediaDuration(src), rate, Rounding::Floor);
    if (frames <= 0) throw EditError("media '" + src.path + "' is shorter than one frame");
    return frameToTime(frames, rate);
}

namespace {

void requireUnlocked(const Sequence& seq, const std::string& trackId) {
    const Track* t = seq.findTrack(trackId);
    if (!t) throw EditError("unknown track '" + trackId + "'");
    if (t->locked) throw EditError("track '" + (t->name.empty() ? t->id : t->name) + "' is locked");
}

std::string fileNameOf(const std::string& path) {
    const std::filesystem::path p(std::u8string(path.begin(), path.end()));
    const std::u8string n = p.filename().u8string();
    return std::string(n.begin(), n.end());
}

// Remove transitions that no longer join two adjacent clips (after moves, trims, deletes).
void dropBrokenTransitions(Builder& b, const std::string& seqId) {
    const Sequence& s = b.sequence(seqId);
    std::vector<std::string> broken;
    for (const auto& t : s.tracks)
        for (size_t i = 0; i < t.clips.size(); ++i) {
            if (!t.clips[i].outTransition) continue;
            try {
                transitionSpan(t, i);
            } catch (const ModelError&) {
                broken.push_back(t.clips[i].id);
            }
        }
    for (const auto& id : broken)
        b.add(makeModifyClip(seqId, id, "Remove transition", [](Clip& c) { c.outTransition.reset(); }));
}

std::string addTrackOfKind(Builder& b, const std::string& seqId, TrackKind kind) {
    Sequence& s = b.sequence(seqId);
    const auto n = std::count_if(s.tracks.begin(), s.tracks.end(), [&](const Track& t) { return t.kind == kind; });
    const std::string name = (kind == TrackKind::Video ? "Video " : "Audio ") + std::to_string(n + 1);
    const size_t index = s.tracks.size();
    b.add(makeAddTrack(b.scratch(), seqId, kind, index, name));
    return b.sequence(seqId).tracks[index].id;
}

// Ordinal of a track among the tracks of its kind (0 = first/bottom).
int ordinalOf(const Sequence& s, const std::string& trackId) {
    int k = 0;
    const Track* t = s.findTrack(trackId);
    for (const auto& o : s.tracks) {
        if (o.id == trackId) return k;
        if (t && o.kind == t->kind) ++k;
    }
    return -1;
}

std::string nthTrackOfKind(const Sequence& s, TrackKind kind, int n) {
    int k = 0;
    for (const auto& t : s.tracks)
        if (t.kind == kind && k++ == n) return t.id;
    return {};
}

bool regionFree(const Track& t, const TimeRange& r) {
    return std::none_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.timelineRange().overlaps(r); });
}

}  // namespace

// ---- operations ------------------------------------------------------------------------------

std::unique_ptr<Command> addMedia(Project& p, const std::string& seqId, const std::string& sourceId, Time at,
                                  std::string videoTrackId, AddedClips* added, std::string audioTrackId) {
    Builder b(p);
    const MediaSource* src = b.scratch().findSource(sourceId);
    if (!src) throw EditError("unknown source '" + sourceId + "'");
    if (!src->hasVideo && !src->hasAudio) throw EditError("media has neither video nor audio");
    const FrameRate rate = b.sequence(seqId).frameRate;
    // a scene item (M7) places only its range: whole frames of the sequence, at least one
    const Time dur = src->range ? frameToTime(std::max<int64_t>(1, timeToFrame(src->range->duration, rate, Rounding::Floor)), rate)
                                : fullClipDuration(*src, rate);
    const Time in = src->range ? src->range->start : Time(0);
    const std::string itemName = src->name;
    Time t = frameToTime(timeToFrame(max(at, Time(0)), rate, Rounding::Floor), rate);
    const std::string srcPath = src->path;
    const bool hasVideo = src->hasVideo, hasAudio = src->hasAudio;

    // Target tracks (created on demand).
    std::string vt, at_;
    int ordinal = 0;
    if (hasVideo) {
        vt = videoTrackId.empty() ? nthTrackOfKind(b.sequence(seqId), TrackKind::Video, 0) : videoTrackId;
        if (vt.empty()) vt = addTrackOfKind(b, seqId, TrackKind::Video);
        const Track* vtr = b.sequence(seqId).findTrack(vt);
        if (!vtr || vtr->kind != TrackKind::Video) throw EditError("target is not a video track");
        ordinal = ordinalOf(b.sequence(seqId), vt);
    }
    if (hasAudio && !audioTrackId.empty()) {   // M10: the audio track it was dropped on
        const Track* atr = b.sequence(seqId).findTrack(audioTrackId);
        if (!atr || atr->kind != TrackKind::Audio) throw EditError("target is not an audio track");
        at_ = audioTrackId;
    } else if (hasAudio) {
        at_ = nthTrackOfKind(b.sequence(seqId), TrackKind::Audio, ordinal);
        while (at_.empty()) {
            addTrackOfKind(b, seqId, TrackKind::Audio);
            at_ = nthTrackOfKind(b.sequence(seqId), TrackKind::Audio, ordinal);
        }
    }
    std::vector<std::string> targets;
    if (!vt.empty()) targets.push_back(vt);
    if (!at_.empty()) targets.push_back(at_);
    for (const auto& id : targets) requireUnlocked(b.sequence(seqId), id);

    // Occupied? Insert after the clip under t and ripple every unlocked track to the right.
    auto allFree = [&] {
        const Sequence& s = b.sequence(seqId);
        return std::all_of(targets.begin(), targets.end(),
                           [&](const std::string& id) { return regionFree(*s.findTrack(id), {t, dur}); });
    };
    if (!allFree()) {
        for (bool moved = true; moved;) {
            moved = false;
            for (const auto& id : targets)
                if (const Clip* c = b.sequence(seqId).findTrack(id)->clipAt(t)) {
                    t = c->timelineEnd();
                    moved = true;
                }
        }
        const Sequence& s = b.sequence(seqId);
        std::vector<std::string> shiftTracks;
        for (const auto& tr : s.tracks)
            if (!tr.locked && std::any_of(tr.clips.begin(), tr.clips.end(),
                                          [&](const Clip& c) { return c.timelineStart >= t; }))
                shiftTracks.push_back(tr.id);
        for (const auto& id : shiftTracks) b.add(makeShiftClips(seqId, id, t, dur));
        if (!allFree()) throw EditError("no room to insert the clip (a locked track is in the way)");
    }

    const std::string link = (hasVideo && hasAudio) ? b.scratch().newId("link") : std::string();
    AddedClips ids;
    auto insert = [&](const std::string& track) {
        Clip c;
        c.id = b.scratch().newId("clip");
        c.sourceId = sourceId;
        c.name = itemName.empty() ? fileNameOf(srcPath) : itemName;
        c.timelineStart = t;
        c.sourceIn = in;
        c.duration = dur;
        c.linkId = link;
        b.add(makeInsertClip(b.scratch(), seqId, track, c));
        return c.id;
    };
    if (!vt.empty()) ids.videoClipId = insert(vt);
    if (!at_.empty()) ids.audioClipId = insert(at_);
    if (added) *added = ids;
    return b.finish("Add to timeline");
}

std::unique_ptr<Command> moveClip(Project& p, const std::string& seqId, const std::string& clipId, Time newStart,
                                  std::optional<std::string> newTrackId) {
    Builder b(p);
    const Sequence& s = b.sequence(seqId);
    const Track* tr = nullptr;
    const Clip* c = findClip(s, clipId, &tr);
    if (!c) throw EditError("clip not found: " + clipId);
    const Time delta = newStart - c->timelineStart;
    if (delta.isZero() && (!newTrackId || *newTrackId == tr->id)) throw EditError("clip did not move");
    const auto group = linkGroup(s, clipId);
    for (const auto& m : group) requireUnlocked(s, m.trackId);
    if (newTrackId) requireUnlocked(s, *newTrackId);

    std::vector<std::pair<std::string, Time>> partners;
    for (const auto& m : group)
        if (m.clipId != clipId) partners.push_back({m.clipId, findClip(s, m.clipId)->timelineStart + delta});
    b.add(makeMoveClip(seqId, clipId, newStart, newTrackId));
    for (const auto& [id, start] : partners) b.add(makeMoveClip(seqId, id, start));
    dropBrokenTransitions(b, seqId);
    return b.finish("Move clip");
}

std::unique_ptr<Command> trimClip(Project& p, const std::string& seqId, const std::string& clipId, ClipEdge edge,
                                  Time newEdgeTime) {
    Builder b(p);
    const Sequence& s = b.sequence(seqId);
    const Clip* c = findClip(s, clipId);
    if (!c) throw EditError("clip not found: " + clipId);
    const Time delta = newEdgeTime - (edge == ClipEdge::Head ? c->timelineStart : c->timelineEnd());
    if (delta.isZero()) throw EditError("trim did not change the clip");
    const auto group = linkGroup(s, clipId);
    std::vector<std::pair<std::string, Time>> edges;
    for (const auto& m : group) {
        requireUnlocked(s, m.trackId);
        const Clip* mc = findClip(s, m.clipId);
        edges.push_back({m.clipId, (edge == ClipEdge::Head ? mc->timelineStart : mc->timelineEnd()) + delta});
    }
    for (const auto& [id, e] : edges) b.add(makeTrimClip(seqId, id, edge, e));
    dropBrokenTransitions(b, seqId);
    return b.finish(edge == ClipEdge::Head ? "Trim start" : "Trim end");
}

std::unique_ptr<Command> splitAt(Project& p, const std::string& seqId, Time t, std::vector<std::string> clipIds) {
    Builder b(p);
    const Sequence& s = b.sequence(seqId);
    auto inside = [&](const Clip& c) { return c.timelineStart < t && t < c.timelineEnd(); };
    std::vector<std::string> targets;
    auto addTarget = [&](const std::string& id) {
        if (std::find(targets.begin(), targets.end(), id) == targets.end()) targets.push_back(id);
    };
    if (clipIds.empty()) {
        for (const auto& tr : s.tracks)
            if (!tr.locked)
                for (const auto& c : tr.clips)
                    if (inside(c)) addTarget(c.id);
    } else {
        for (const auto& id : clipIds)
            for (const auto& m : linkGroup(s, id)) {
                const Clip* c = findClip(s, m.clipId);
                if (inside(*c)) {
                    requireUnlocked(s, m.trackId);
                    addTarget(m.clipId);
                }
            }
    }
    if (targets.empty()) throw EditError("no clip under the playhead to split");
    std::map<std::string, std::string> newLinks;   // old linkId -> link of right halves
    for (const auto& id : targets) {
        const std::string link = findClip(b.sequence(seqId), id)->linkId;
        std::optional<std::string> right;
        if (!link.empty()) {
            auto it = newLinks.find(link);
            if (it == newLinks.end()) it = newLinks.emplace(link, b.scratch().newId("link")).first;
            right = it->second;
        }
        b.add(makeSplitClip(b.scratch(), seqId, id, t, right));
    }
    return b.finish("Split");
}

std::unique_ptr<Command> deleteClips(Project& p, const std::string& seqId, std::vector<std::string> clipIds,
                                     bool ripple) {
    Builder b(p);
    std::set<std::string> done;
    for (const auto& requested : clipIds) {
        if (done.count(requested)) continue;
        const Sequence& s = b.sequence(seqId);
        const auto group = linkGroup(s, requested);
        struct Removed {
            std::string track;
            Time end;
            Time dur;
        };
        std::vector<Removed> removed;
        for (const auto& m : group) {
            requireUnlocked(s, m.trackId);
            const Clip* c = findClip(s, m.clipId);
            removed.push_back({m.trackId, c->timelineEnd(), c->duration});
            done.insert(m.clipId);
        }
        for (const auto& m : group) b.add(makeRemoveClip(seqId, m.clipId, false));
        if (!ripple) continue;

        // clips to shift: everything after each removed range on its track, plus linked partners
        const Sequence& after = b.sequence(seqId);
        std::map<std::string, std::pair<Time, std::vector<std::string>>> moves;   // track -> (delta, clips)
        std::map<std::string, Time> clipDelta;
        std::vector<std::string> queue;
        for (const auto& r : removed) {
            const Track* t = after.findTrack(r.track);
            for (const auto& c : t->clips)
                if (c.timelineStart >= r.end) {
                    if (clipDelta.count(c.id) && clipDelta[c.id] != -r.dur)
                        throw EditError("ripple delete would move clip " + c.id + " by two different amounts");
                    if (clipDelta.emplace(c.id, -r.dur).second) queue.push_back(c.id);
                }
        }
        for (size_t qi = 0; qi < queue.size(); ++qi) {
            const std::string id = queue[qi];
            for (const auto& m : linkGroup(after, id)) {
                if (clipDelta.count(m.clipId)) {
                    if (clipDelta[m.clipId] != clipDelta[id])
                        throw EditError("ripple delete would break the sync of linked clip " + m.clipId);
                    continue;
                }
                clipDelta[m.clipId] = clipDelta[id];
                queue.push_back(m.clipId);
            }
        }
        for (const auto& [id, delta] : clipDelta) {
            const Track* tr = nullptr;
            findClip(after, id, &tr);
            if (tr->locked) throw EditError("ripple delete would move clips on locked track '" + tr->name + "'");
            auto& entry = moves[tr->id];
            entry.first = delta;
            entry.second.push_back(id);
        }
        for (auto& [track, mv] : moves) {
            try {
                b.add(makeMoveClipsBy(seqId, track, mv.second, mv.first));
            } catch (const EditError& e) {
                throw EditError(std::string("ripple delete is not possible without breaking A/V sync: ") + e.what());
            }
        }
    }
    dropBrokenTransitions(b, seqId);
    return b.finish(ripple ? "Ripple delete" : "Delete");
}

// ---- gaps (M10) ------------------------------------------------------------------------------

std::optional<TimeRange> gapAt(const Track& track, Time t) {
    if (t < Time(0)) return std::nullopt;
    Time from(0);
    std::optional<Time> to;
    for (const auto& c : track.clips) {
        if (c.timelineRange().contains(t)) return std::nullopt;   // on a clip
        if (c.timelineEnd() <= t) from = max(from, c.timelineEnd());
        else if (!to || c.timelineStart < *to) to = c.timelineStart;   // the first clip after t
    }
    if (!to || *to <= from) return std::nullopt;   // after the last clip
    return TimeRange{from, *to - from};
}

namespace {

std::string quoted(const Clip& c) { return "'" + (c.name.empty() ? c.id : c.name) + "'"; }

// What Delete Gap moves, per track: the clips after the gap on its track and their linked partners, all left by the
// gap's length. Throws EditError with the reason for the user when a locked track would change or a moved clip
// would run into one that stays (only a linked partner can: the gap's own track has room by definition).
std::map<std::string, std::vector<std::string>> gapMoves(const Sequence& s, const Track& track, const TimeRange& gap) {
    if (track.locked) throw EditError("Can't delete this gap: " + track.name + " is locked");
    // linked partners through one index (the timeline asks while the mouse rests on a gap: thousands of clips)
    std::unordered_map<std::string, std::vector<const Clip*>> byLink;
    for (const auto& t : s.tracks)
        for (const auto& c : t.clips)
            if (!c.linkId.empty()) byLink[c.linkId].push_back(&c);
    std::unordered_set<std::string> moving;
    std::vector<const Clip*> queue;
    for (const auto& c : track.clips)
        if (c.timelineStart >= gap.end() && moving.insert(c.id).second) queue.push_back(&c);
    for (size_t i = 0; i < queue.size(); ++i) {
        if (queue[i]->linkId.empty()) continue;
        for (const Clip* m : byLink[queue[i]->linkId])
            if (moving.insert(m->id).second) queue.push_back(m);
    }
    std::map<std::string, std::vector<std::string>> out;
    for (const auto& t : s.tracks) {
        struct Span {
            Time start, end;
            const Clip* clip;
            bool moves;
        };
        std::vector<Span> spans;
        std::vector<std::string> ids;
        for (const auto& c : t.clips) {
            const bool mv = moving.count(c.id) > 0;
            if (mv) ids.push_back(c.id);
            const Time start = mv ? c.timelineStart - gap.duration : c.timelineStart;
            spans.push_back({start, start + c.duration, &c, mv});
        }
        if (ids.empty()) continue;
        if (t.locked)
            throw EditError("Can't delete this gap: linked clips on " + t.name + " would move, but " + t.name + " is locked");
        std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.start < b.start; });
        for (const auto& sp : spans)
            if (sp.start < Time(0))
                throw EditError("Can't delete this gap: the linked clip " + quoted(*sp.clip) + " on " + t.name +
                                " would start before 0");
        for (size_t i = 1; i < spans.size(); ++i)
            if (spans[i - 1].end > spans[i].start) {   // a moved clip and one that stays
                const Span& mv = spans[i - 1].moves ? spans[i - 1] : spans[i];
                const Span& still = spans[i - 1].moves ? spans[i] : spans[i - 1];
                throw EditError("Can't delete this gap: the linked clip " + quoted(*mv.clip) + " would overlap " +
                                quoted(*still.clip) + " on " + t.name);
            }
        out[t.id] = std::move(ids);
    }
    return out;
}

}  // namespace

std::string gapProblem(const Sequence& seq, const std::string& trackId, Time t) {
    const Track* tr = seq.findTrack(trackId);
    if (!tr) return "There is no such track";
    const auto gap = gapAt(*tr, t);
    if (!gap) return "There is no gap here";
    try {
        gapMoves(seq, *tr, *gap);
    } catch (const EditError& e) {
        return e.what();
    }
    return {};
}

std::unique_ptr<Command> deleteGap(Project& p, const std::string& seqId, const std::string& trackId, Time t) {
    Builder b(p);
    const Sequence& s = b.sequence(seqId);
    const Track* tr = s.findTrack(trackId);
    if (!tr) throw EditError("unknown track '" + trackId + "'");
    const auto gap = gapAt(*tr, t);
    if (!gap) throw EditError("There is no gap here");
    const auto moves = gapMoves(s, *tr, *gap);
    for (const auto& [track, ids] : moves) b.add(makeMoveClipsBy(seqId, track, ids, -gap->duration));
    dropBrokenTransitions(b, seqId);
    return b.finish("Delete gap");
}

std::optional<Time> maxTransitionDuration(const Sequence& seq, const std::string& clipId, TransitionAlignment a) {
    for (const auto& t : seq.tracks)
        for (size_t i = 0; i < t.clips.size(); ++i) {
            if (t.clips[i].id != clipId) continue;
            if (i + 1 >= t.clips.size() || t.clips[i + 1].timelineStart != t.clips[i].timelineEnd()) return std::nullopt;
            const Time da = t.clips[i].duration, db = t.clips[i + 1].duration;
            switch (a) {
                case TransitionAlignment::Centered: return min(da, db) * Rational(2);
                case TransitionAlignment::EndsAtCut: return da;
                case TransitionAlignment::StartsAtCut: return db;
            }
        }
    return std::nullopt;
}

std::unique_ptr<Command> setTransition(Project& p, const std::string& seqId, const std::string& clipId,
                                       std::optional<TransitionRef> transition) {
    Builder b(p);
    const Track* tr = nullptr;
    if (!findClip(b.sequence(seqId), clipId, &tr)) throw EditError("clip not found: " + clipId);
    requireUnlocked(b.sequence(seqId), tr->id);
    if (transition && !isFrameAligned(transition->duration, b.sequence(seqId).frameRate))
        throw EditError("transition duration must be a whole number of frames");
    if (transition && (tr->kind == TrackKind::Audio) != isAudioCrossfade(transition->type))   // M6
        throw EditError(tr->kind == TrackKind::Audio ? "audio tracks take audio crossfades only"
                                                     : "audio crossfades belong on audio tracks");
    b.add(makeModifyClip(seqId, clipId, transition ? "Set transition" : "Remove transition",
                         [transition](Clip& c) { c.outTransition = transition; }));
    if (transition) {
        const Track* t = b.sequence(seqId).findTrack(tr->id);
        for (size_t i = 0; i < t->clips.size(); ++i)
            if (t->clips[i].id == clipId) {
                const bool adjacentAdjustment = t->clips[i].isAdjustment() ||
                                                (i + 1 < t->clips.size() && t->clips[i + 1].isAdjustment());
                if (adjacentAdjustment) throw EditError("transitions cannot involve adjustment layers");
                try {
                    transitionSpan(*t, i);
                } catch (const ModelError& e) {
                    throw EditError(e.what());
                }
            }
    }
    return b.finish(transition ? "Set transition" : "Remove transition");
}

std::unique_ptr<Command> unlink(Project& p, const std::string& seqId, const std::string& clipId) {
    Builder b(p);
    const auto group = linkGroup(b.sequence(seqId), clipId);
    if (group.size() < 2) throw EditError("clip is not linked");
    for (const auto& m : group) b.add(makeModifyClip(seqId, m.clipId, "Unlink", [](Clip& c) { c.linkId.clear(); }));
    return b.finish("Detach audio");
}

}  // namespace fa::edit

// ---- titles and captions (M3) ----------------------------------------------------------------

namespace fa::edit {

namespace {

std::string titleName(const TitleData& t) {
    std::string first = t.text.substr(0, t.text.find('\n'));
    if (first.size() > 40) first = first.substr(0, 40) + "...";
    return first.empty() ? std::string("Title") : first;
}

Time nearestFrameTime(const Time& t, const FrameRate& r) {
    return frameToTime(timeToFrame(max(t, Time(0)), r, Rounding::Nearest), r);
}

std::string addSubtitleTrack(Builder& b, const std::string& seqId, const TextStyle& style, double y) {
    Sequence& s = b.sequence(seqId);
    const auto n = std::count_if(s.tracks.begin(), s.tracks.end(), [](const Track& t) { return t.kind == TrackKind::Subtitle; });
    const size_t index = s.tracks.size();
    b.add(makeAddTrack(b.scratch(), seqId, TrackKind::Subtitle, index, "Subtitles " + std::to_string(n + 1)));
    const std::string id = b.sequence(seqId).tracks[index].id;
    b.add(makeModifyTrack(seqId, id, "Subtitle style", [style, y](Track& t) {
        t.subtitleStyle = style;
        t.subtitleY = y;
    }));
    return id;
}

}  // namespace

std::unique_ptr<Command> addTitle(Project& p, const std::string& seqId, Clip proto, Time at, Time duration,
                                  std::string trackId, std::string* clipIdOut) {
    if (!proto.title) throw EditError("not a title");
    return addGenerated(p, seqId, std::move(proto), at, duration, std::move(trackId), clipIdOut);
}

std::unique_ptr<Command> addGenerated(Project& p, const std::string& seqId, Clip proto, Time at, Time duration,
                                      std::string trackId, std::string* clipIdOut) {
    if (!proto.isGenerated()) throw EditError("not a title, sticker or adjustment layer");
    Builder b(p);
    const FrameRate rate = b.sequence(seqId).frameRate;
    const Time t = frameToTime(timeToFrame(max(at, Time(0)), rate, Rounding::Floor), rate);
    const Time dur = frameToTime(std::max<int64_t>(1, timeToFrame(duration, rate, Rounding::Nearest)), rate);
    const TimeRange range{t, dur};
    if (!trackId.empty()) {
        const Track* tr = b.sequence(seqId).findTrack(trackId);
        if (!tr || tr->kind != TrackKind::Video) throw EditError("titles and adjustment layers go on video tracks");
    }
    std::string target = generatedTarget(b.sequence(seqId), trackId, range);
    if (target.empty()) target = addTrackOfKind(b, seqId, TrackKind::Video);
    Clip c = std::move(proto);
    c.id = b.scratch().newId("clip");
    c.sourceId.clear();
    if (c.name.empty()) c.name = c.title ? titleName(*c.title) : c.sticker ? std::string("Sticker") : std::string("Adjustment Layer");
    c.timelineStart = t;
    c.sourceIn = Time(0);
    c.duration = dur;
    c.linkId.clear();
    c.outTransition.reset();
    if (clipIdOut) *clipIdOut = c.id;
    const char* label = c.isTitle()           ? "Add title"
                        : c.isSticker()       ? "Add sticker"
                        : !c.effects.empty() ? "Add effect"   // M10: an effect / filter dropped as a layer
                                              : "Add adjustment layer";
    b.add(makeInsertClip(b.scratch(), seqId, target, c));
    return b.finish(label);
}

std::string generatedTarget(const Sequence& s, const std::string& trackId, const TimeRange& range) {
    auto freeVideo = [&](const std::string& id) {
        const Track* tr = s.findTrack(id);
        return tr && tr->kind == TrackKind::Video && !tr->locked && regionFree(*tr, range);
    };
    if (!trackId.empty() && freeVideo(trackId)) return trackId;
    // the lowest free video track above Video 1 (or above the requested track)
    const Track* requested = trackId.empty() ? nullptr : s.findTrack(trackId);
    const int from = requested && requested->kind == TrackKind::Video ? ordinalOf(s, trackId) + 1 : 1;
    for (int k = from;; ++k) {
        const std::string id = nthTrackOfKind(s, TrackKind::Video, k);
        if (id.empty()) return {};
        if (freeVideo(id)) return id;
    }
}

std::unique_ptr<Command> addCaptions(Project& p, const std::string& seqId, std::vector<Caption> captions,
                                     const TextStyle& style, double subtitleY, std::vector<std::string>* trackIdsOut) {
    if (captions.empty()) throw EditError("no captions to add");
    Builder b(p);
    const FrameRate rate = b.sequence(seqId).frameRate;
    std::stable_sort(captions.begin(), captions.end(), [](const Caption& x, const Caption& y) { return x.start < y.start; });
    std::vector<std::pair<std::string, Time>> lanes;   // subtitle track id, end of its last caption
    std::map<std::string, std::vector<Clip>> perLane;  // inserted with one command per track
    for (const auto& cap : captions) {
        const Time start = nearestFrameTime(cap.start, rate);
        Time end = nearestFrameTime(cap.end, rate);
        if (end <= start) end = start + rate.frameDuration();
        std::string lane;
        for (auto& [id, last] : lanes)
            if (last <= start) {
                lane = id;
                last = end;
                break;
            }
        if (lane.empty()) {
            lane = addSubtitleTrack(b, seqId, style, subtitleY);
            lanes.push_back({lane, end});
        }
        Clip c;
        c.id = b.scratch().newId("clip");
        TitleData t;
        t.text = cap.text;
        t.followTrackStyle = !cap.style;
        t.style = cap.style ? *cap.style : style;
        if (!cap.fadeIn.isZero()) t.in = {"fade", cap.fadeIn};
        if (!cap.fadeOut.isZero()) t.out = {"fade", cap.fadeOut};
        c.title = std::move(t);
        c.name = titleName(*c.title);
        c.timelineStart = start;
        c.duration = end - start;
        if (cap.position) {
            c.transform.positionX.setConstant(cap.position->x - 0.5);
            c.transform.positionY.setConstant(cap.position->y - 0.5);
        }
        if (cap.rotation) c.transform.rotation.setConstant(*cap.rotation);
        if (cap.scale) {
            c.transform.uniformScale = cap.scale->x == cap.scale->y;
            c.transform.scaleX.setConstant(cap.scale->x);
            c.transform.scaleY.setConstant(cap.scale->y);
        }
        perLane[lane].push_back(std::move(c));
    }
    for (const auto& lane : lanes) b.add(makeInsertClips(b.scratch(), seqId, lane.first, std::move(perLane[lane.first])));
    if (trackIdsOut)
        for (const auto& lane : lanes) trackIdsOut->push_back(lane.first);
    return b.finish(captions.size() == 1 ? "Add caption" : "Import " + std::to_string(captions.size()) + " captions");
}

std::unique_ptr<Command> addCaption(Project& p, const std::string& seqId, const std::string& text, Time at,
                                    Time duration, std::string trackId, std::string* clipIdOut, const TextStyle* style,
                                    const std::string& templateId) {
    Builder b(p);
    const FrameRate rate = b.sequence(seqId).frameRate;
    const Time t = frameToTime(timeToFrame(max(at, Time(0)), rate, Rounding::Floor), rate);
    Time dur = frameToTime(std::max<int64_t>(1, timeToFrame(duration, rate, Rounding::Nearest)), rate);
    if (trackId.empty())
        for (const auto& tr : b.sequence(seqId).tracks)
            if (tr.kind == TrackKind::Subtitle) {
                trackId = tr.id;
                break;
            }
    if (trackId.empty()) trackId = addSubtitleTrack(b, seqId, style ? *style : Track::defaultSubtitleStyle(), 0.92);
    const Track* tr = b.sequence(seqId).findTrack(trackId);
    if (!tr || tr->kind != TrackKind::Subtitle) throw EditError("not a subtitle track");
    requireUnlocked(b.sequence(seqId), trackId);
    for (const auto& o : tr->clips) {   // shorten to the next caption if needed
        if (o.timelineRange().contains(t)) throw EditError("there is already a caption at this time");
        if (o.timelineStart > t && o.timelineStart < t + dur) dur = o.timelineStart - t;
    }
    Clip c;
    c.id = b.scratch().newId("clip");
    TitleData td;
    td.text = text;
    td.followTrackStyle = !style || *style == tr->subtitleStyle;
    td.style = style ? *style : tr->subtitleStyle;
    td.templateId = templateId;
    c.title = std::move(td);
    c.name = titleName(*c.title);
    c.timelineStart = t;
    c.duration = dur;
    if (clipIdOut) *clipIdOut = c.id;
    b.add(makeInsertClip(b.scratch(), seqId, trackId, c));
    return b.finish("Add caption");
}


// ---- speed (M5) ------------------------------------------------------------------------------

namespace {

// Ripple: on each listed track every clip starting at or after `from` moves by `delta`; linked partners of moved
// clips on other tracks move by the same amount (refused if that would move a clip by two different amounts).
void addRipple(Builder& b, const std::string& seqId, const std::vector<std::tuple<std::string, Time, Time>>& shifts,
               const std::set<std::string>& exclude, const char* what) {
    const Sequence& s = b.sequence(seqId);
    std::map<std::string, Time> clipDelta;
    std::vector<std::string> queue;
    for (const auto& [track, from, delta] : shifts) {
        if (delta.isZero()) continue;
        const Track* t = s.findTrack(track);
        if (!t) continue;
        for (const auto& c : t->clips) {
            if (c.timelineStart < from || exclude.count(c.id)) continue;
            if (clipDelta.count(c.id) && clipDelta[c.id] != delta)
                throw EditError(std::string(what) + " would move clip " + c.id + " by two different amounts");
            if (clipDelta.emplace(c.id, delta).second) queue.push_back(c.id);
        }
    }
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        const std::string id = queue[qi];
        for (const auto& m : linkGroup(s, id)) {
            if (exclude.count(m.clipId)) continue;
            if (clipDelta.count(m.clipId)) {
                if (clipDelta[m.clipId] != clipDelta[id])
                    throw EditError(std::string(what) + " would break the sync of linked clip " + m.clipId);
                continue;
            }
            clipDelta[m.clipId] = clipDelta[id];
            queue.push_back(m.clipId);
        }
    }
    std::map<std::string, std::pair<Time, std::vector<std::string>>> moves;
    for (const auto& [id, delta] : clipDelta) {
        const Track* tr = nullptr;
        findClip(s, id, &tr);
        if (tr->locked) throw EditError(std::string(what) + " would move clips on locked track '" + tr->name + "'");
        auto& e = moves[tr->id];
        e.first = delta;
        e.second.push_back(id);
    }
    for (auto& [track, mv] : moves) b.add(makeMoveClipsBy(seqId, track, mv.second, mv.first));
}

}  // namespace

std::unique_ptr<Command> setSpeed(Project& p, const std::string& seqId, const std::string& clipId, ClipSpeed speed,
                                  std::string label) {
    Builder b(p);
    const Sequence& s = b.sequence(seqId);
    const Clip* c = findClip(s, clipId);
    if (!c) throw EditError("clip not found: " + clipId);
    if (c->isGenerated()) throw EditError("titles and adjustment layers have no speed");
    if (speed.freeze) throw EditError("use Freeze Frame to hold a frame");
    const MediaSource* src = b.scratch().findSource(c->sourceId);
    if (!src) throw EditError("unknown source '" + c->sourceId + "'");
    const auto group = linkGroup(s, clipId);
    struct Change {
        std::string id, track;
        Time oldEnd, newDuration;
    };
    std::vector<Change> changes;
    for (const auto& m : group) {
        requireUnlocked(s, m.trackId);
        const Clip* mc = findClip(s, m.clipId);
        if (mc->speed.freeze) throw EditError("a freeze frame has no speed; delete it or change its duration");
        Time length = sourceLength(*mc);   // the source range the clip plays now is kept
        if (src->duration) length = min(length, *src->duration - mc->sourceIn);
        Clip probe = *mc;
        probe.speed = speed;
        probe.duration = frameDurationForSource(speed, mc->sourceIn, length, s.frameRate, src->duration);
        if (const std::string why = speedProblem(probe); !why.empty()) throw EditError(why);
        changes.push_back({m.clipId, m.trackId, mc->timelineEnd(), probe.duration});
    }
    const Time delta = changes.front().newDuration - c->duration;
    for (const auto& ch : changes)
        if (ch.newDuration - findClip(s, ch.id)->duration != delta)
            throw EditError("linked clips play different source ranges; detach the audio first");
    std::set<std::string> members;
    std::vector<std::tuple<std::string, Time, Time>> shifts;
    for (const auto& ch : changes) {
        members.insert(ch.id);
        shifts.emplace_back(ch.track, ch.oldEnd, delta);
    }
    auto modify = [&] {
        for (const auto& ch : changes) b.add(makeRetimeClip(seqId, ch.id, speed, ch.newDuration));
    };
    if (delta > Time(0)) {   // longer: make room first
        addRipple(b, seqId, shifts, members, "the speed change");
        modify();
    } else {
        modify();
        addRipple(b, seqId, shifts, members, "the speed change");
    }
    dropBrokenTransitions(b, seqId);
    if (b.empty()) throw EditError("the speed did not change");
    return b.finish(std::move(label));
}

std::unique_ptr<Command> setHoldDuration(Project& p, const std::string& seqId, const std::string& clipId, Time duration) {
    Builder b(p);
    const Sequence& s = b.sequence(seqId);
    const Track* track = nullptr;
    const Clip* c = findClip(s, clipId, &track);
    if (!c) throw EditError("clip not found: " + clipId);
    if (!c->speed.freeze) throw EditError("only a freeze frame has a hold duration");
    if (!isFrameAligned(duration, s.frameRate) || duration <= Time(0)) throw EditError("the hold must be whole frames");
    requireUnlocked(s, track->id);
    const Time delta = duration - c->duration;
    if (delta.isZero()) throw EditError("the hold did not change");
    const ClipSpeed sp = c->speed;
    const std::vector<std::tuple<std::string, Time, Time>> shifts = {{track->id, c->timelineEnd(), delta}};
    if (delta > Time(0)) {
        addRipple(b, seqId, shifts, {clipId}, "the new hold");
        b.add(makeRetimeClip(seqId, clipId, sp, duration));
    } else {
        b.add(makeRetimeClip(seqId, clipId, sp, duration));
        addRipple(b, seqId, shifts, {clipId}, "the new hold");
    }
    return b.finish("Freeze duration");
}

std::unique_ptr<Command> freezeFrame(Project& p, const std::string& seqId, const std::string& clipId, Time at,
                                     Time hold, std::string* holdIdOut) {
    Builder b(p);
    const Sequence& s0 = b.sequence(seqId);
    const Track* track = nullptr;
    const Clip* c = findClip(s0, clipId, &track);
    if (!c) throw EditError("clip not found: " + clipId);
    if (track->kind != TrackKind::Video || c->isGenerated()) throw EditError("Freeze Frame needs a video clip");
    if (!isFrameAligned(at, s0.frameRate) || !isFrameAligned(hold, s0.frameRate) || hold <= Time(0))
        throw EditError("the freeze point and its duration must be whole frames");
    if (at < c->timelineStart || !(at < c->timelineEnd())) throw EditError("the playhead is not on the clip");
    requireUnlocked(s0, track->id);
    const MediaSource* src = b.scratch().findSource(c->sourceId);
    const std::optional<FrameRate> rate = src ? src->frameRate : std::nullopt;
    Clip frozen = *c;   // effects, transform, masks, opacity, blend: the hold looks like the clip
    frozen.sourceIn = sampleFrame(*c, at - c->timelineStart, s0.frameRate.frameDuration(), rate).time;
    frozen.speed = ClipSpeed{};
    frozen.speed.freeze = true;
    frozen.timelineStart = at;
    frozen.duration = hold;
    frozen.linkId.clear();
    frozen.outTransition.reset();
    frozen.id = b.scratch().newId("clip");
    const std::string trackId = track->id;
    std::vector<ClipRef> group = linkGroup(s0, clipId);
    if (c->timelineStart < at) b.add(splitAt(b.scratch(), seqId, at, {clipId}));
    // everything from `at` on the clip's tracks moves right by `hold` (the right half and its linked audio too)
    std::vector<std::tuple<std::string, Time, Time>> shifts;
    for (const auto& m : group) shifts.emplace_back(m.trackId, at, hold);
    addRipple(b, seqId, shifts, {}, "Freeze Frame");
    b.add(makeInsertClip(b.scratch(), seqId, trackId, frozen));
    if (holdIdOut) *holdIdOut = frozen.id;
    return b.finish("Freeze Frame");
}

// ---- clipboard (M9) --------------------------------------------------------------------------

std::vector<CopiedClip> copyClips(const Sequence& seq, const std::vector<std::string>& clipIds) {
    std::set<std::string> ids;
    for (const auto& id : clipIds)
        for (const auto& r : linkGroup(seq, id)) ids.insert(r.clipId);
    std::vector<CopiedClip> out;
    for (const auto& t : seq.tracks)
        for (const auto& c : t.clips)
            if (ids.count(c.id)) out.push_back({c, t.id, t.kind});
    std::stable_sort(out.begin(), out.end(),
                     [](const CopiedClip& a, const CopiedClip& b) { return a.clip.timelineStart < b.clip.timelineStart; });
    return out;
}

std::string clipboardToJson(const std::vector<CopiedClip>& clips) {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& c : clips)
        list.push_back({{"track", c.trackId},
                        {"kind", c.kind == TrackKind::Audio ? "audio" : c.kind == TrackKind::Subtitle ? "subtitle" : "video"},
                        {"clip", nlohmann::json::parse(clipToJsonString(c.clip, -1))}});
    return nlohmann::json{{"format", "meercut-clips"}, {"version", Project::kFormatVersion}, {"clips", std::move(list)}}.dump();
}

std::vector<CopiedClip> clipboardFromJson(const std::string& text) {
    std::vector<CopiedClip> out;
    try {
        const nlohmann::json j = nlohmann::json::parse(text);
        if (const std::string format = j.value("format", ""); format != "meercut-clips" && format != "lumacut-clips")
            throw ModelError("not MeerCut clips");   // (lumacut-clips: copied before the rename)
        if (j.value("version", 0) > Project::kFormatVersion) throw ModelError("clips from a newer MeerCut");
        for (const auto& e : j.at("clips")) {
            const std::string kind = e.at("kind").get<std::string>();
            out.push_back({clipFromJsonString(e.at("clip").dump()), e.at("track").get<std::string>(),
                           kind == "audio" ? TrackKind::Audio : kind == "subtitle" ? TrackKind::Subtitle : TrackKind::Video});
        }
    } catch (const nlohmann::json::exception& e) {
        throw ModelError(std::string("clipboard: ") + e.what());
    }
    return out;
}

std::unique_ptr<Command> pasteClips(Project& p, const std::string& seqId, const std::vector<CopiedClip>& clips, Time at,
                                    std::vector<std::string>* newIds, bool stackInOrder) {
    if (clips.empty()) throw EditError("nothing to paste");
    Builder b(p);
    const FrameRate rate = b.sequence(seqId).frameRate;
    const Time t0 = frameToTime(timeToFrame(max(at, Time(0)), rate, Rounding::Floor), rate);
    Time earliest = clips.front().clip.timelineStart;
    for (const auto& cc : clips) {
        earliest = min(earliest, cc.clip.timelineStart);
        if (!cc.clip.isGenerated() && !b.scratch().findSource(cc.clip.sourceId))
            throw EditError("the copied media is not in this project");
    }
    std::vector<std::string> order;   // original tracks in the order their clips come
    std::map<std::string, std::vector<const CopiedClip*>> groups;
    for (const auto& cc : clips) {
        if (!groups.count(cc.trackId)) order.push_back(cc.trackId);
        groups[cc.trackId].push_back(&cc);
    }
    std::map<std::string, std::string> links;
    std::set<std::string> used;
    std::vector<std::string> ids;
    std::map<TrackKind, int> floor;   // stackInOrder: the next layer goes above this ordinal
    for (const auto& orig : order) {
        const auto& g = groups[orig];
        const TrackKind kind = g.front()->kind;
        std::vector<TimeRange> ranges;
        for (const CopiedClip* cc : g) ranges.push_back({t0 + (cc->clip.timelineStart - earliest), cc->clip.duration});
        auto fits = [&](const Track& tr) {
            if (tr.kind != kind || tr.locked || used.count(tr.id)) return false;
            return std::all_of(ranges.begin(), ranges.end(), [&](const TimeRange& r) { return regionFree(tr, r); });
        };
        std::string target;
        const int above = stackInOrder && floor.count(kind) ? floor[kind] + 1 : 0;
        {
            const Sequence& s = b.sequence(seqId);
            const Track* own = s.findTrack(orig);
            if (own && fits(*own) && ordinalOf(s, orig) >= above) target = orig;
            for (int k = std::max(above, own && own->kind == kind ? ordinalOf(s, orig) + 1 : 0); target.empty(); ++k) {
                const std::string id = nthTrackOfKind(s, kind, k);
                if (id.empty()) break;
                if (fits(*s.findTrack(id))) target = id;
            }
        }
        if (target.empty())
            target = kind == TrackKind::Subtitle ? addSubtitleTrack(b, seqId, Track::defaultSubtitleStyle(), 0.92)
                                                 : addTrackOfKind(b, seqId, kind);
        used.insert(target);
        floor[kind] = ordinalOf(b.sequence(seqId), target);
        std::vector<Clip> batch;
        for (const CopiedClip* cc : g) {
            Clip c = cc->clip;
            c.id = b.scratch().newId("clip");
            c.timelineStart = t0 + (cc->clip.timelineStart - earliest);
            if (!c.linkId.empty()) {
                auto it = links.find(c.linkId);
                if (it == links.end()) it = links.emplace(c.linkId, b.scratch().newId("link")).first;
                c.linkId = it->second;
            }
            ids.push_back(c.id);
            batch.push_back(std::move(c));
        }
        b.add(makeInsertClips(b.scratch(), seqId, target, std::move(batch)));
    }
    dropBrokenTransitions(b, seqId);
    if (newIds) *newIds = ids;
    return b.finish(ids.size() == 1 ? "Paste clip" : "Paste clips");
}

}  // namespace fa::edit
