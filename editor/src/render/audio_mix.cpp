#include "fa/render/audio_mix.h"

#include "fa/model/serialize.h"
#include "fa/model/time_map.h"
#include "fa/render/audio_cleanup.h"
#include "fa/render/audio_retime.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <optional>

namespace fa {

int64_t timeToSample(const Time& t, int sampleRate) { return (t * Rational(sampleRate)).floor(); }

std::pair<int64_t, int64_t> clipSampleRange(const Clip& clip, int sampleRate) {
    return {(clip.timelineStart * Rational(sampleRate)).ceil(), (clip.timelineEnd() * Rational(sampleRate)).ceil()};
}

namespace {

int64_t ceilSample(const Time& t, int rate) { return (t * Rational(rate)).ceil(); }
int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

bool eqActive(const ClipAudio& a) {
    if (!a.eqEnabled) return false;
    return std::any_of(a.eqGainsDb.begin(), a.eqGainsDb.end(), [](double g) { return std::fabs(g) > 1e-9; });
}

CleanupSettings cleanupOf(const ClipAudio& a) {
    CleanupSettings s;
    s.denoise = a.cleanup.denoise;
    s.humHz = a.cleanup.humHz;
    s.humHarmonics = a.cleanup.humHarmonics;
    s.highPassHz = a.cleanup.highPassHz;
    s.lowPassHz = a.cleanup.lowPassHz;
    return s;
}

bool dspActive(const ClipAudio& a) { return eqActive(a) || cleanupOf(a).active(); }

// Loudness normalisation gain (dB): target - measured, when on and measured.
double normalizeDb(const ClipAudio& a) {
    const AudioCleanup& k = a.cleanup;
    if (!k.normalize || !k.measuredLufs || !std::isfinite(*k.measuredLufs)) return 0.0;
    return std::clamp(k.targetLufs - *k.measuredLufs, -40.0, 40.0);
}

uint64_t mixHash(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ULL;
    return h ^ (h >> 31);
}

uint64_t hashDouble(uint64_t h, double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    return mixHash(h, bits);
}

uint64_t hashString(uint64_t h, const std::string& s) {
    uint64_t f = 0xCBF29CE484222325ULL;
    for (const unsigned char c : s) f = (f ^ c) * 0x100000001B3ULL;
    return mixHash(h, f ^ s.size());
}

uint64_t hashAnim(uint64_t h, const Animatable<double>& a) {
    h = hashDouble(h, a.constantValue());
    for (const auto& k : a.keys()) {
        h = hashString(h, k.time.toString());
        h = hashDouble(h, k.value);
        h = mixHash(h, static_cast<uint64_t>(k.interp) * 16 + static_cast<uint64_t>(k.tangent));
        h = hashDouble(hashDouble(h, k.inSlope), k.outSlope);
        h = hashDouble(hashDouble(h, k.inHandle), k.outHandle);
    }
    return h;
}

// Everything the DSP output of a clip depends on (raw audio placement + DSP settings).
uint64_t dspContentKey(const Clip& c, const std::string& path) {
    uint64_t h = hashString(0x51ED270B27C2AA21ULL, path);
    h = hashString(h, c.sourceIn.toString());
    h = hashString(h, c.timelineStart.toString());
    h = hashString(h, c.duration.toString());
    h = hashString(h, c.speed.speed.toString());
    h = mixHash(h, (c.speed.reverse ? 1u : 0u) | (c.speed.freeze ? 2u : 0u) | (c.speed.maintainPitch ? 4u : 0u));
    for (const auto& k : c.speed.ramp) {
        h = hashString(h, k.source.toString());
        h = mixHash(h, static_cast<uint64_t>(std::llround(k.speed * 1e9)));
    }
    h = mixHash(h, c.audio.eqEnabled);
    for (double g : c.audio.eqGainsDb) h = mixHash(h, static_cast<uint64_t>(std::llround((g + 100.0) * 1e9)));
    const AudioCleanup& k = c.audio.cleanup;
    h = hashDouble(h, k.denoise);
    h = hashDouble(h, k.humHz);
    h = mixHash(h, static_cast<uint64_t>(k.humHarmonics));
    h = hashDouble(h, k.highPassHz);
    h = hashDouble(h, k.lowPassHz);
    return h;
}

// The clip that a crossfade of the previous clip leads into, and its span.
std::optional<TransitionSpan> audioFadeSpan(const Track& tr, size_t i, bool tolerate) {
    if (!tr.clips[i].outTransition || !isAudioCrossfade(tr.clips[i].outTransition->type)) return std::nullopt;
    try {
        return transitionSpan(tr, i);
    } catch (const ModelError&) {
        if (tolerate) return std::nullopt;
        throw;
    }
}

CrossfadeShape shapeOf(const TransitionRef& t) {
    CrossfadeShape s = CrossfadeShape::EqualPower;
    crossfadeShapeFromId(t.type.substr(std::char_traits<char>::length(kAudioCrossfadePrefix)), s);
    return s;
}

// b continues a seamlessly: same media, contiguous in time and source, both plain 1x clips.
bool seamless(const Clip& a, const Clip& b) {
    return a.enabled && b.enabled && a.sourceId == b.sourceId && a.timelineEnd() == b.timelineStart && !isRetimed(a.speed) &&
           !isRetimed(b.speed) && a.sourceIn + a.duration == b.sourceIn;
}

// Volume (keyframes in source time), loudness normalisation and fades of a clip at an output sample.
class ClipGain {
public:
    ClipGain(const Clip& c, int rate)
        : c_(c), rate_(rate), start_(c.timelineStart.toDouble()), dur_(c.duration.toDouble()),
          fadeIn_(c.audio.fadeIn.toDouble()), fadeOut_(c.audio.fadeOut.toDouble()), animated_(c.volumeDb.isAnimated()),
          norm_(dbToGain(normalizeDb(c.audio))), constGain_(dbToGain(c.volumeDb.constantValue()) * norm_),
          srcIn_(c.sourceIn.toDouble()) {
        if (isRetimed(c.speed) && (animated_ || c.audio.balance.isAnimated())) mapper_.emplace(c);
    }
    double local(int64_t s) const { return static_cast<double>(s) / rate_ - start_; }
    double sourceSeconds(double local) const { return mapper_ ? mapper_->source(local) : srcIn_ + local; }
    double at(int64_t s) const {
        const double l = local(s);
        double g = animated_ ? dbToGain(c_.volumeDb.value(sourceSeconds(l))) * norm_ : constGain_;
        if (fadeIn_ > 0 && l < fadeIn_) g *= std::max(0.0, l / fadeIn_);
        if (fadeOut_ > 0 && l > dur_ - fadeOut_) g *= std::max(0.0, (dur_ - l) / fadeOut_);
        return g;
    }

private:
    const Clip& c_;
    double rate_, start_, dur_, fadeIn_, fadeOut_;
    bool animated_;
    double norm_, constGain_, srcIn_;
    std::optional<SourceMapper> mapper_;
};

bool audible(const Track& t, bool anySolo) {
    return t.kind == TrackKind::Audio && !t.muted && !(anySolo && !t.solo) && dbToGain(t.volumeDb) != 0.0;
}

}  // namespace

struct AudioMixer::Ducker {
    const Clip* clip = nullptr;
    const MediaSource* src = nullptr;
    const Track* track = nullptr;
    uint64_t key = 0;   // content + gain + threshold
    int64_t attack = 0, hold = 0, release = 0;   // samples
    double amountDb = 0, thresholdDb = 0;
};

AudioMixer::AudioMixer(std::filesystem::path projectFile, int sampleRate, int channels, size_t maxOpenDecoders)
    : projectFile_(std::move(projectFile)), rate_(sampleRate), channels_(channels),
      maxOpen_(maxOpenDecoders ? maxOpenDecoders : 1) {
    if (rate_ <= 0 || channels_ <= 0) throw MediaError("invalid mixer format");
}

AudioMixer::~AudioMixer() = default;

int64_t AudioMixer::totalSeeks() const {
    int64_t n = closedSeeks_;
    for (const auto& [_, e] : decoders_) n += e.decoder->seekCount();
    return n;
}

AudioDecoder& AudioMixer::decoderFor(const Clip& clip, const MediaSource& src, Read purpose) {
    const auto path = resolveMediaPath(src, projectFile_);
    const std::string key = purpose == Read::Analysis ? clip.id + "\x1f" "analysis" : clip.id;
    auto it = decoders_.find(key);
    if (it != decoders_.end() && it->second.path != path) {   // clip re-pointed (relink)
        closedSeeks_ += it->second.decoder->seekCount();
        decoders_.erase(it);
        it = decoders_.end();
    }
    if (it == decoders_.end()) {
        while (decoders_.size() >= maxOpen_) {
            auto lru = decoders_.begin();
            for (auto j = decoders_.begin(); j != decoders_.end(); ++j)
                if (j->second.lastUse < lru->second.lastUse) lru = j;
            closedSeeks_ += lru->second.decoder->seekCount();
            decoders_.erase(lru);
        }
        Entry e;
        e.decoder = std::make_unique<AudioDecoder>(path, rate_, channels_);
        e.path = path;
        it = decoders_.emplace(key, std::move(e)).first;
    }
    it->second.lastUse = ++tick_;
    return *it->second.decoder;
}

void AudioMixer::readClip(const Clip& c, const MediaSource& src, int64_t from, int count, float* out, Read purpose) {
    if (isRetimed(c.speed)) {   // M5: through the clip's speed map; nothing outside the clip
        if (c.speed.freeze) {
            std::fill(out, out + static_cast<size_t>(count) * channels_, 0.0f);
            return;
        }
        auto& map = purpose == Read::Dsp ? dspRetimers_ : purpose == Read::Analysis ? analysisRetimers_ : retimers_;
        auto& rt = map[c.id];
        if (!rt) rt = std::make_unique<AudioRetimer>(rate_, channels_);
        rt->render(c, decoderFor(c, src, purpose), from, count, out);
        return;
    }
    const int64_t offset = ((c.sourceIn - c.timelineStart) * Rational(rate_)).floor();
    decoderFor(c, src, purpose).read(from + offset, count, out);   // before / after the media: silence
}

void AudioMixer::readClipAudio(const Clip& c, const MediaSource& src, int64_t from, int count, float* out, Read purpose) {
    if (dspActive(c.audio)) readProcessed(c, src, from, count, out);
    else readClip(c, src, from, count, out, purpose);
}

void AudioMixer::clipAudio(const Project& project, const Clip& clip, int64_t from, int count, float* out) {
    const MediaSource* src = project.findSource(clip.sourceId);
    if (!src) throw MediaError("clip " + clip.id + " references unknown source " + clip.sourceId);
    if (clip.speed.freeze) {
        std::fill(out, out + static_cast<size_t>(count) * channels_, 0.0f);
        return;
    }
    readClipAudio(clip, *src, from, count, out, Read::Analysis);
}

void AudioMixer::readProcessed(const Clip& c, const MediaSource& src, int64_t from, int count, float* out) {
    const int64_t anchor = clipSampleRange(c, rate_).first;
    const uint64_t content = dspContentKey(c, resolveMediaPath(src, projectFile_).generic_string());
    const CleanupSettings cleanup = cleanupOf(c.audio);
    // the cascade: cleanup filters (hum, high-pass, low-pass), then the equaliser bands
    std::vector<Biquad> filters = cleanupFilters(cleanup, rate_);
    if (c.audio.eqEnabled)
        for (int b = 0; b < kAudioEqBands; ++b) {
            const Biquad f = Biquad::peaking(rate_, kEqFrequencies[static_cast<size_t>(b)], 1.41, c.audio.eqGainsDb[static_cast<size_t>(b)]);
            if (!f.identity()) filters.push_back(f);
        }
    // noise reduction first; the filters then warm up on final (denoised) input. Its margins are long, so its blocks
    // are 4x longer (the input read per output sample drops from 7.9x to 2.7x)
    const bool denoise = cleanup.denoise > 0;
    const int pre = denoise ? kDenoiseMargin + kDspWarmup : kDspWarmup, post = denoise ? kDenoiseMargin : 0;
    const int block = denoise ? 4 * kDspBlock : kDspBlock;
    const int64_t first = floorDiv(from - anchor, block), last = floorDiv(from + count - 1 - anchor, block);
    std::vector<float> chan, den;
    for (int64_t k = first; k <= last; ++k) {
        const uint64_t key = mixHash(content, static_cast<uint64_t>(k));
        auto it = std::find_if(dspCache_.begin(), dspCache_.end(), [&](const DspBlock& b) { return b.key == key; });
        if (it == dspCache_.end()) {
            const int64_t blockStart = anchor + k * block;
            const int total = pre + block + post;
            dspIn_.resize(static_cast<size_t>(total) * channels_);
            readClip(c, src, blockStart - pre, total, dspIn_.data(), Read::Dsp);
            DspBlock blk;
            blk.key = key;
            blk.samples.resize(static_cast<size_t>(block) * channels_);
            chan.resize(static_cast<size_t>(total));
            for (int ch = 0; ch < channels_; ++ch) {
                for (int i = 0; i < total; ++i) chan[static_cast<size_t>(i)] = dspIn_[static_cast<size_t>(i) * channels_ + ch];
                if (denoise) {
                    den.resize(chan.size());
                    denoiseChannel(chan.data(), total, blockStart - pre, anchor, cleanup.denoise, den.data());
                    chan.swap(den);
                }
                std::vector<BiquadState> st(filters.size());
                const int from0 = denoise ? kDenoiseMargin : 0;   // denoised samples before this are not final
                for (int i = from0; i < pre + block; ++i) {
                    double x = chan[static_cast<size_t>(i)];
                    for (size_t f = 0; f < filters.size(); ++f) x = st[f].process(filters[f], x);
                    if (i >= pre) blk.samples[static_cast<size_t>(i - pre) * channels_ + ch] = static_cast<float>(x);
                }
            }
            ++dspBlocks_;
            dspCache_.push_front(std::move(blk));
            if (dspCache_.size() > 32) dspCache_.pop_back();
            it = dspCache_.begin();
        } else if (it != dspCache_.begin()) {
            dspCache_.splice(dspCache_.begin(), dspCache_, it);
            it = dspCache_.begin();
        }
        const int64_t blockStart = anchor + k * block;
        const int64_t a = std::max(from, blockStart), b = std::min(from + count, blockStart + block);
        std::copy(it->samples.begin() + (a - blockStart) * channels_, it->samples.begin() + (b - blockStart) * channels_,
                  out + (a - from) * channels_);
    }
}

std::vector<std::pair<int64_t, int64_t>> AudioMixer::duckRuns(const Ducker& d, int64_t from, int64_t to) {
    std::vector<std::pair<int64_t, int64_t>> runs;
    const int blk = std::max(1, rate_ / 100);   // 10 ms
    const auto [cs, ce] = clipSampleRange(*d.clip, rate_);
    from = std::max(from, cs);
    to = std::min(to, ce);
    if (from >= to) return runs;
    std::optional<ClipGain> gain;
    for (int64_t k = floorDiv(from, blk), kEnd = floorDiv(to - 1, blk); k <= kEnd; ++k) {
        const uint64_t key = mixHash(d.key, static_cast<uint64_t>(k));
        bool on = false;
        if (auto it = duckActivity_.find(key); it != duckActivity_.end()) {
            on = it->second;
        } else {
            const int64_t a = std::max(k * blk, cs), b = std::min((k + 1) * blk, ce);
            analysisBuf_.resize(static_cast<size_t>(b - a) * channels_);
            readClipAudio(*d.clip, *d.src, a, static_cast<int>(b - a), analysisBuf_.data(), Read::Analysis);
            if (!gain) gain.emplace(*d.clip, rate_);
            std::vector<double> ms(static_cast<size_t>(channels_), 0.0);
            for (int64_t s = a; s < b; ++s) {
                const double g = gain->at(s);
                for (int ch = 0; ch < channels_; ++ch) {
                    const double v = analysisBuf_[static_cast<size_t>(s - a) * channels_ + ch] * g;
                    ms[static_cast<size_t>(ch)] += v * v;
                }
            }
            const double loudest = *std::max_element(ms.begin(), ms.end()) / static_cast<double>(b - a);
            on = loudest > 0 && 10.0 * std::log10(loudest) > d.thresholdDb;
            if (duckActivity_.size() > (1u << 20)) duckActivity_.clear();
            duckActivity_[key] = on;
            ++duckBlocks_;
        }
        if (!on) continue;
        const int64_t a = k * blk, b = (k + 1) * blk;
        if (!runs.empty() && runs.back().second == a) runs.back().second = b;
        else runs.emplace_back(a, b);
    }
    return runs;
}

void AudioMixer::mix(const Project& project, const Sequence& seq, int64_t start, int count, float* out,
                     bool tolerateErrors) {
    if (!seq.limiter) {
        lastEnd_ = -1;
        mixRaw(project, seq, start, count, out, tolerateErrors);
        return;
    }
    if (count <= 0) return;
    // master limiter: limitPeaks needs the raw mix one window on either side of the output; the raw mix is streamed
    // (continued from the previous request when this one follows it) so sequential playback does not re-read audio
    const int W = std::max(1, static_cast<int>(std::lround(kLimiterWindow * rate_ / 48000.0)));
    const int64_t a = start - W, b = start + count + W;
    const bool continues = lastEnd_ == start && rawProject_ == &project && rawSeq_ == &seq && rawStart_ <= a;
    if (!continues) {
        raw_.clear();
        rawStart_ = a;
    } else if (a > rawStart_) {
        raw_.erase(raw_.begin(), raw_.begin() + (a - rawStart_) * channels_);
        rawStart_ = a;
    }
    const int64_t have = rawStart_ + static_cast<int64_t>(raw_.size()) / channels_;
    if (have < b) {
        const size_t old = raw_.size();
        raw_.resize(static_cast<size_t>(b - rawStart_) * channels_);
        mixRaw(project, seq, have, static_cast<int>(b - have), raw_.data() + old, tolerateErrors);
    }
    limitPeaks(raw_.data(), static_cast<int64_t>(raw_.size()) / channels_, channels_, W, dbToGain(seq.limiterCeilingDb),
               start - rawStart_, count, out);
    lastEnd_ = start + count;
    rawProject_ = &project;
    rawSeq_ = &seq;
}

void AudioMixer::mixRaw(const Project& project, const Sequence& seq, int64_t start, int count, float* out,
                        bool tolerateErrors) {
    const size_t n = static_cast<size_t>(count) * channels_;
    std::fill(out, out + n, 0.0f);
    if (count <= 0) return;
    const int64_t end = start + count;
    const bool anySolo = std::any_of(seq.tracks.begin(), seq.tracks.end(),
                                     [](const Track& t) { return t.kind == TrackKind::Audio && t.solo; });
    const int declick = std::max(1, static_cast<int>(std::lround(kDeclickSamples48k * rate_ / 48000.0)));
    const double rate = rate_;
    auto fail = [&](const std::string& msg) {
        if (std::find(errors_.begin(), errors_.end(), msg) == errors_.end()) errors_.push_back(msg);
    };
    // priority clips (Auto Ducking) on audible tracks
    std::vector<Ducker> duckers;
    for (const Track& tr : seq.tracks) {
        if (!audible(tr, anySolo)) continue;
        for (const Clip& c : tr.clips) {
            const AudioDucking& d = c.audio.ducking;
            if (!d.enabled || !c.enabled || c.muted || c.speed.freeze) continue;
            const MediaSource* src = project.findSource(c.sourceId);
            if (!src) continue;
            Ducker k;
            k.clip = &c;
            k.src = src;
            k.track = &tr;
            k.key = hashDouble(hashAnim(mixHash(dspContentKey(c, resolveMediaPath(*src, projectFile_).generic_string()),
                                                0xD0C4ULL),
                                        c.volumeDb),
                               normalizeDb(c.audio));
            k.key = hashString(hashString(k.key, c.audio.fadeIn.toString()), c.audio.fadeOut.toString());
            k.key = hashDouble(k.key, d.thresholdDb);
            k.attack = std::llround(std::max(0.0, d.attackMs) * rate_ / 1000.0);
            k.hold = std::llround(std::max(0.0, d.holdMs) * rate_ / 1000.0);
            k.release = std::llround(std::max(0.0, d.releaseMs) * rate_ / 1000.0);
            k.amountDb = std::min(0.0, d.amountDb);
            k.thresholdDb = d.thresholdDb;
            duckers.push_back(k);
        }
    }
    std::vector<double> duckGain;
    for (const Track& tr : seq.tracks) {
        if (!audible(tr, anySolo)) continue;
        const double trackGain = dbToGain(tr.volumeDb);
        // ducking of this track by priority clips elsewhere (the deepest wins)
        duckGain.clear();
        for (const Ducker& d : duckers) {
            if (d.track == &tr) continue;
            const auto& targets = d.clip->audio.ducking.targets;
            if (!targets.empty() && std::find(targets.begin(), targets.end(), tr.id) == targets.end()) continue;
            std::vector<std::pair<int64_t, int64_t>> runs;
            try {
                runs = duckRuns(d, start - d.hold - d.release - rate_ / 100, end + d.attack + rate_ / 100);
            } catch (const std::exception& e) {
                if (!tolerateErrors) throw MediaError("ducking source " + d.clip->id + " (" + d.src->path + "): " + e.what());
                fail(d.src->path + ": " + e.what());
                continue;
            }
            if (runs.empty()) continue;
            if (duckGain.empty()) duckGain.assign(static_cast<size_t>(count), 1.0);
            for (int64_t s = start; s < end; ++s) {
                const double depth = duckDepth(runs, s, d.attack, d.hold, d.release);
                if (depth > 0) {
                    double& g = duckGain[static_cast<size_t>(s - start)];
                    g = std::min(g, dbToGain(d.amountDb * depth));
                }
            }
        }
        for (size_t i = 0; i < tr.clips.size(); ++i) {
            const Clip& c = tr.clips[i];
            if (!c.enabled || c.muted) continue;
            const bool retimed = isRetimed(c.speed);
            const auto [cs, ce] = clipSampleRange(c, rate_);
            // crossfade spans: into this clip (from the previous one) and out of it
            std::optional<TransitionSpan> inSpan, outSpan;
            if (i > 0 && tr.clips[i - 1].enabled) inSpan = audioFadeSpan(tr, i - 1, tolerateErrors);
            outSpan = audioFadeSpan(tr, i, tolerateErrors);
            if (outSpan && !tr.clips[i + 1].enabled) outSpan.reset();
            const int64_t xs = inSpan && !retimed ? std::min(cs, ceilSample(inSpan->range.start, rate_)) : cs;
            const int64_t xe = outSpan && !retimed ? std::max(ce, ceilSample(outSpan->range.end(), rate_)) : ce;
            const int64_t from = std::max(start, xs), to = std::min(end, xe);
            if (to <= from) continue;
            const MediaSource* src = project.findSource(c.sourceId);
            if (!src) {
                if (tolerateErrors) continue;
                throw MediaError("clip " + c.id + " references unknown source " + c.sourceId);
            }
            if (c.speed.freeze) continue;   // silent (M5)
            const int len = static_cast<int>(to - from);
            scratch_.resize(static_cast<size_t>(len) * channels_);
            try {
                readClipAudio(c, *src, from, len, scratch_.data(), Read::Play);
            } catch (const std::exception& e) {
                if (!tolerateErrors) throw MediaError("clip " + c.id + " (" + src->path + "): " + e.what());
                fail(src->path + ": " + e.what());
                continue;
            }
            // ---- per-sample gain and balance
            const ClipGain gain(c, rate_);
            const bool balAnimated = c.audio.balance.isAnimated();
            double balL = 1, balR = 1;
            balanceGains(c.audio.balance.constantValue(), balL, balR);
            const bool declickIn = c.audio.fadeIn <= Time(0) && !inSpan && !(i > 0 && seamless(tr.clips[i - 1], c));
            const bool declickOut =
                c.audio.fadeOut <= Time(0) && !outSpan && !(i + 1 < tr.clips.size() && seamless(c, tr.clips[i + 1]));
            const double inStart = inSpan ? inSpan->range.start.toDouble() : 0, inDur = inSpan ? inSpan->range.duration.toDouble() : 1;
            const double outStart = outSpan ? outSpan->range.start.toDouble() : 0, outDur = outSpan ? outSpan->range.duration.toDouble() : 1;
            const CrossfadeShape inShape = inSpan ? shapeOf(*tr.clips[i - 1].outTransition) : CrossfadeShape::EqualPower;
            const CrossfadeShape outShape = outSpan ? shapeOf(*c.outTransition) : CrossfadeShape::EqualPower;
            float* dst = out + (from - start) * channels_;
            const float* srcp = scratch_.data();
            for (int64_t s = from; s < to; ++s) {
                const double t = static_cast<double>(s) / rate;
                double g = gain.at(s) * trackGain;
                if (inSpan && t < inStart + inDur) {
                    double a = 0, b = 0;
                    crossfadeGains(inShape, (t - inStart) / inDur, a, b);
                    g *= b;
                }
                if (outSpan && t >= outStart) {
                    double a = 0, b = 0;
                    crossfadeGains(outShape, (t - outStart) / outDur, a, b);
                    g *= a;
                }
                if (declickIn && s - cs < declick) g *= 0.5 - 0.5 * std::cos(std::numbers::pi * (static_cast<double>(s - cs) + 0.5) / declick);
                if (declickOut && ce - s <= declick) g *= 0.5 - 0.5 * std::cos(std::numbers::pi * (static_cast<double>(ce - s) - 0.5) / declick);
                if (!duckGain.empty()) g *= duckGain[static_cast<size_t>(s - start)];
                if (balAnimated) balanceGains(c.audio.balance.value(gain.sourceSeconds(gain.local(s))), balL, balR);
                const size_t o = static_cast<size_t>(s - from) * channels_;
                if (channels_ == 2) {
                    dst[o] += static_cast<float>(srcp[o] * g * balL);
                    dst[o + 1] += static_cast<float>(srcp[o + 1] * g * balR);
                } else {
                    for (int ch = 0; ch < channels_; ++ch) dst[o + ch] += static_cast<float>(srcp[o + ch] * g);
                }
            }
        }
    }
}

double measureClipLoudness(const Project& project, const Sequence& seq, const Clip& clip,
                           const std::filesystem::path& projectFile, const std::atomic<bool>* cancel,
                           const std::function<void(double)>& progress) {
    AudioMixer mixer(projectFile, seq.sampleRate, 2);
    LoudnessMeter meter(seq.sampleRate, 2);
    const auto [cs, ce] = clipSampleRange(clip, seq.sampleRate);
    constexpr int kChunk = 65536;
    std::vector<float> buf(static_cast<size_t>(kChunk) * 2);
    for (int64_t pos = cs; pos < ce; pos += kChunk) {
        if (cancel && cancel->load()) return std::nan("");
        const int n = static_cast<int>(std::min<int64_t>(kChunk, ce - pos));
        mixer.clipAudio(project, clip, pos, n, buf.data());
        meter.add(buf.data(), n);
        if (progress) progress(static_cast<double>(pos + n - cs) / static_cast<double>(std::max<int64_t>(1, ce - cs)));
    }
    return meter.integrated();
}

}  // namespace fa
