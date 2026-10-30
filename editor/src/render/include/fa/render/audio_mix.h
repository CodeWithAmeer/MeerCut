// Timeline audio: mixes the audio tracks of a sequence into interleaved float samples.
//
// Rules (shared by export, headless rendering and playback, so all of them produce the same samples):
//   * Output sample n covers time n / sampleRate. A clip contributes to sample n iff
//     start <= n / rate < end (the same half-open rule as video frames), i.e. samples
//     [ceil(start * rate), ceil(end * rate)) - extended by audio crossfades (below).
//   * The clip plays source sample  n + floor((sourceIn - start) * rate)  of its media, decoded at the
//     sequence rate (AudioDecoder; source time 0 = the file's video start, so A/V stay aligned).
//   * Retimed clips (M5 speed, reverse, freeze, ramps) play through an AudioRetimer (audio_retime.h): the same
//     speed map as the video, freeze frames silent, pitch kept or following the speed (Maintain Pitch).
//   * Per clip (M6), in this order: clip DSP (cleanup, then equaliser) -> gain -> balance -> sum.
//     gain(n) = 10^((volume(source time of n) + track volume + normalisation) / 20) * fades(n) * crossfade(n)
//               * declick(n) * ducking(n)
//       volume:    the clip's Volume in dB; its keyframes are in source time (they follow trims, splits, retiming)
//       normalisation: targetLufs - measuredLufs when loudness normalisation is on and the clip was measured
//       fades:     linear in amplitude from the clip's timeline edges over Fade In / Fade Out (EXP-020)
//       crossfade: an audio transition (fa.audio.crossfade.<shape>) between two adjacent clips plays both over its
//                  span (the outgoing clip continues on its tail handle, the incoming one starts early on its head
//                  handle; where the media has no handle the missing part is silent; retimed clips are not
//                  extended) with the shape's gains (audio_dsp.h)
//       declick:   a 2 ms raised-cosine ramp at a clip edge that would otherwise start or stop the waveform
//                  abruptly (no fade or crossfade there, not a seamless continuation of the same media)
//       ducking:   while a priority clip (Auto Ducking) on another audible track has sound, the tracks it targets are
//                  lowered by its amount; "has sound" = a 10 ms block (on a grid anchored at timeline sample 0) of the
//                  priority clip's output (after DSP, volume, fades and normalisation) above its threshold; the
//                  depth follows duckDepth() (audio_cleanup.h): look-ahead attack, hold, release. Several priority
//                  clips: the deepest wins. Track volume, mute and solo of the priority clip's track: muted or
//                  solo-silenced priority clips do not duck.
//     balance: EXP-020 law (near channel unity, far channel cos(pi |b| / 2)), keyframes in source time.
//   * Track: Volume in dB (constant), Mute; while any audio track is soloed only soloed tracks play.
//     Disabled clips are silent.
//   * Clip DSP (cleanup, equaliser) runs on fixed blocks anchored to the clip's start, each processed from a reset
//     state after a warm-up (32768 samples; with noise reduction 65536-sample blocks that also read 39936 samples
//     before the warm-up and after the block), so the result does
//     not depend on how the timeline is cut into mix requests (playback chunks, seeks, export). Processed blocks are
//     cached.
//   * Tracks are summed. The master limiter (Sequence::limiter, off by default) is a sample-peak limiter whose gain at
//     a sample depends only on the mix within one 50 ms window on either side (audio_cleanup.h); the mixer streams the
//     raw mix that far ahead of the output, so sequential requests (playback, export) and fresh ones give the same
//     samples. Edits during playback reach the limited output after at most that window. Without the limiter, export
//     clips at +-1 in the encoder and playback clamps.
#pragma once

#include "fa/media/media.h"
#include "fa/model/project.h"
#include "fa/render/audio_dsp.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fa {

// First and one-past-last output sample of a clip (its own range, without crossfade extensions).
std::pair<int64_t, int64_t> clipSampleRange(const Clip& clip, int sampleRate);
// floor(t * rate): the sample at which time t starts.
int64_t timeToSample(const Time& t, int sampleRate);

inline constexpr int kDeclickSamples48k = 96;   // 2 ms at 48 kHz (scaled with the rate)
inline constexpr int kDspBlock = 16384;         // clip DSP block (samples; 4x with noise reduction)
inline constexpr int kDspWarmup = 32768;        // reset-state warm-up before each block

class AudioMixer {
public:
    AudioMixer(std::filesystem::path projectFile, int sampleRate, int channels = 2, size_t maxOpenDecoders = 8);
    ~AudioMixer();
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    int sampleRate() const { return rate_; }
    int channels() const { return channels_; }

    // Overwrite out[0 .. count*channels) with the mix of samples [start, start + count).
    // With tolerateErrors, clips whose media cannot be opened or decoded are skipped (playback of
    // projects with offline media); their messages are collected in errors(). Otherwise errors throw.
    void mix(const Project& project, const Sequence& seq, int64_t start, int count, float* out,
             bool tolerateErrors = false);

    // One clip's audio after its DSP (cleanup and EQ), before any gain, for timeline samples [from, from + count):
    // what loudness normalisation measures.
    void clipAudio(const Project& project, const Clip& clip, int64_t from, int count, float* out);

    const std::vector<std::string>& errors() const { return errors_; }
    int64_t totalSeeks() const;
    int64_t dspBlocksProcessed() const { return dspBlocks_; }   // diagnostics / tests
    int64_t duckBlocksAnalysed() const { return duckBlocks_; }  // diagnostics / tests

private:
    enum class Read { Play, Dsp, Analysis };   // separate decoder / retimer state per purpose (no seeking back and forth)
    struct Entry {
        std::unique_ptr<AudioDecoder> decoder;
        std::filesystem::path path;
        uint64_t lastUse = 0;
    };
    struct DspBlock {
        uint64_t key = 0;   // clip content hash + block index
        std::vector<float> samples;   // kDspBlock * channels
    };
    struct Ducker;   // a priority clip of the current mix request
    AudioDecoder& decoderFor(const Clip& clip, const MediaSource& src, Read purpose);
    // The clip's raw audio for timeline samples [from, from + count): the media through the speed map, silence
    // outside the clip's own range for retimed clips, the media's handles for plain clips.
    void readClip(const Clip& clip, const MediaSource& src, int64_t from, int count, float* out, Read purpose);
    // The clip's audio after its DSP (cleanup, equaliser), from cached anchored blocks.
    void readProcessed(const Clip& clip, const MediaSource& src, int64_t from, int count, float* out);
    // readProcessed when the clip has DSP, else readClip.
    void readClipAudio(const Clip& clip, const MediaSource& src, int64_t from, int count, float* out, Read purpose);
    // The mix without the master limiter.
    void mixRaw(const Project& project, const Sequence& seq, int64_t start, int count, float* out, bool tolerateErrors);
    // Sounding runs [a, b) of a priority clip within [from, to) (whole 10 ms blocks).
    std::vector<std::pair<int64_t, int64_t>> duckRuns(const Ducker& d, int64_t from, int64_t to);

    std::filesystem::path projectFile_;
    int rate_;
    int channels_;
    size_t maxOpen_;
    uint64_t tick_ = 0;
    std::map<std::string, Entry> decoders_;   // by clip id (+ purpose)
    std::map<std::string, std::unique_ptr<class AudioRetimer>> retimers_;      // by clip id (stretch state)
    std::map<std::string, std::unique_ptr<class AudioRetimer>> dspRetimers_;   // separate state for DSP blocks
    std::map<std::string, std::unique_ptr<class AudioRetimer>> analysisRetimers_;   // ducking analysis
    std::list<DspBlock> dspCache_;   // most recent first
    std::unordered_map<uint64_t, bool> duckActivity_;   // priority clip content + 10 ms block -> sounding
    std::vector<float> scratch_, dspIn_, analysisBuf_;
    // master limiter stream: the raw mix of [rawStart_, rawStart_ + raw_.size() / channels), the request it served
    std::vector<float> raw_;
    int64_t rawStart_ = 0;
    int64_t lastEnd_ = -1;
    const Project* rawProject_ = nullptr;
    const Sequence* rawSeq_ = nullptr;
    std::vector<std::string> errors_;
    int64_t closedSeeks_ = 0;
    int64_t dspBlocks_ = 0;
    int64_t duckBlocks_ = 0;
};

// Integrated loudness (ITU-R BS.1770-4, LUFS) of a clip's audio after its cleanup and EQ, before its volume: what
// loudness normalisation corrects. -inf for silence. Throws MediaError when the media cannot be read; returns NaN when
// cancelled. progress(fraction) is called now and then.
double measureClipLoudness(const Project& project, const Sequence& seq, const Clip& clip,
                           const std::filesystem::path& projectFile, const std::atomic<bool>* cancel = nullptr,
                           const std::function<void(double)>& progress = {});

}  // namespace fa
