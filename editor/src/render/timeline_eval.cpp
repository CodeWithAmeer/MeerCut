#include "fa/render/timeline_eval.h"

#include "fa/model/time_map.h"

namespace fa {


std::vector<TrackSample> evaluateVideoFrame(const Sequence& seq, int64_t frame, const Project* project) {
    const Time t = frameToTime(frame, seq.frameRate);
    const Time d = seq.frameRate.frameDuration();
    auto sample = [&](const Clip* c) {   // the source frame(s) this output frame shows (M5 speed map)
        ClipSample s{c, c->sourceTimeAt(t)};
        if (!c->isGenerated() && (isRetimed(c->speed) || c->speed.interpolation != FrameInterpolation::Sampling)) {
            std::optional<FrameRate> rate;
            if (project)
                if (const MediaSource* src = project->findSource(c->sourceId)) rate = src->frameRate;
            const FrameSample f = sampleFrame(*c, t - c->timelineStart, d, rate);
            s.sourceTime = f.time;
            s.blendTime = f.next;
            s.blendWeight = f.weight;
        }
        return s;
    };
    std::vector<TrackSample> out;
    for (size_t ti = 0; ti < seq.tracks.size(); ++ti) {
        const Track& tr = seq.tracks[ti];
        if (tr.kind != TrackKind::Video || tr.hidden) continue;

        bool done = false;
        // A transition span can reach outside the clip that owns it, so check spans first.
        for (size_t ci = 0; ci < tr.clips.size() && !done; ++ci) {
            const auto span = transitionSpan(tr, ci);
            if (!span || !span->range.contains(t)) continue;
            const Clip* a = span->from;
            const Clip* b = span->to;
            TrackSample s;
            s.trackIndex = ti;
            s.track = &tr;
            s.transition = &*a->outTransition;
            const double mix = ((t - span->range.start) / span->range.duration).toDouble();
            if (a->enabled && b->enabled) {
                s.a = sample(a);
                s.b = sample(b);
                s.mix = mix;
            } else if (a->enabled || b->enabled) {   // one side disabled: show the other side's own range
                const Clip* only = a->enabled ? a : b;
                if (!only->timelineRange().contains(t)) {
                    done = true;
                    break;
                }
                s.a = sample(only);
                s.transition = nullptr;
            } else {
                done = true;
                break;
            }
            out.push_back(s);
            done = true;
        }
        if (done) continue;

        const Clip* c = tr.clipAt(t);
        if (!c || !c->enabled) continue;
        TrackSample s;
        s.trackIndex = ti;
        s.track = &tr;
        s.a = sample(c);
        out.push_back(s);
    }
    return out;
}

}  // namespace fa
