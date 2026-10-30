#include "fa/ai/captions.h"

#include "fa/model/keyframes.h"
#include "fa/model/serialize.h"
#include "fa/render/audio_mix.h"

#include <algorithm>

namespace fa::ai {

namespace {

Time secondsToTime(double s) { return Rational::fromDouble(s, 1000); }

}  // namespace

CaptionResult captionsForClip(const Project& project, const Clip& clip, const std::filesystem::path& projectFile,
                              const TranscribeOptions& options, const std::function<void(double)>& progress,
                              const std::atomic<bool>* cancel) {
    CaptionResult r;
    if (clip.speed.freeze || clip.isGenerated()) return r;
    const MediaSource* src = project.findSource(clip.sourceId);
    if (!src || !src->hasAudio) throw ModelError("the clip's media has no sound to transcribe");
    const auto media = resolveMediaPath(*src, projectFile);
    const double from = clip.sourceIn.toDouble(), to = clip.sourceOut().toDouble();
    r.transcript = transcribeFile(media, from, to, options, progress, cancel);
    for (const SpeechSegment& s : r.transcript.segments) {
        // source time -> timeline time through the speed map (reversed clips: the later source word plays first)
        Time a = keys::toTimeline(clip, clip.sourceIn + secondsToTime(s.start));
        Time b = keys::toTimeline(clip, clip.sourceIn + secondsToTime(s.end));
        if (b < a) std::swap(a, b);
        a = std::max(a, clip.timelineStart);
        b = std::min(b, clip.timelineEnd());
        if (!(a < b)) continue;
        Caption c;
        c.start = a;
        c.end = b;
        c.text = s.text;
        r.captions.push_back(std::move(c));
    }
    std::sort(r.captions.begin(), r.captions.end(), [](const Caption& x, const Caption& y) { return x.start < y.start; });
    return r;
}

CaptionResult captionsForRange(const Project& project, const Sequence& seq, Time from, Time to,
                               const std::filesystem::path& projectFile, const TranscribeOptions& options,
                               const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    CaptionResult r;
    if (!(from < to)) return r;
    // the mix as the player plays it, at 16 kHz mono (one channel: the mixer folds stereo to mono)
    AudioMixer mixer(projectFile, 16000, 1);
    const int64_t a = timeToSample(from, 16000), b = timeToSample(to, 16000);
    std::vector<float> x(static_cast<size_t>(std::max<int64_t>(0, b - a)));
    for (int64_t pos = 0; pos < static_cast<int64_t>(x.size()); pos += 16000) {
        if (cancel && cancel->load()) {
            r.transcript.cancelled = true;
            return r;
        }
        const int n = static_cast<int>(std::min<int64_t>(16000, static_cast<int64_t>(x.size()) - pos));
        mixer.mix(project, seq, a + pos, n, x.data() + pos, true);
    }
    r.transcript = transcribe(x, options, progress, cancel);
    for (const SpeechSegment& s : r.transcript.segments) {
        Caption c;
        c.start = from + secondsToTime(s.start);
        c.end = std::min(to, from + secondsToTime(s.end));
        c.text = s.text;
        if (c.start < c.end) r.captions.push_back(std::move(c));
    }
    return r;
}

}  // namespace fa::ai
