// Timeline audio playback and the playback clock (no Qt).
//
// A feeder thread mixes the sequence (AudioMixer — the same mixer export uses) into a lock-free ring
// buffer; the audio device callback drains it. The clock is the number of samples the device has
// actually taken from the ring (underruns output silence and do NOT advance it), so video that follows
// this clock stays in sync with what is heard. Without a usable device (or in tests: Backend::Null)
// the clock runs on std::chrono::steady_clock instead.
#pragma once

#include "fa/playback/preview_renderer.h"

#include <atomic>
#include <memory>
#include <string>

namespace fa {

class AudioPlayer {
public:
    enum class Backend { Default, Null };   // Null: no sound card (tests, headless machines)

    explicit AudioPlayer(Backend backend = Backend::Default);
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool deviceAvailable() const;
    std::string deviceDescription() const;
    std::string lastError() const;

    // Start playback of the snapshot's sequence from output sample `startSample`.
    void start(const ProjectSnapshot& snapshot, int64_t startSample);
    void stop();
    bool playing() const;
    // Replace the project while playing (edits during playback); audio already queued (<= ~0.2 s) stays.
    void updateSnapshot(const ProjectSnapshot& snapshot);

    // Current playback position in output samples (start + samples heard so far).
    int64_t position() const;
    int sampleRate() const;
    void setVolume(float linear);   // monitor volume, 0..1 (does not change the project)
    float volume() const;
    int64_t underrunSamples() const;
    std::vector<std::string> mediaErrors() const;   // offline media skipped by the mixer

    // Level meter (M6): the mix as the device takes it (before the monitor volume, i.e. what export writes), since
    // the previous call. Peak = largest |sample|, rms over the samples taken; clipped = a sample beyond full scale
    // (|x| > 1, which an integer or limited output would clip). Calling it resets the accumulators.
    struct Levels {
        float peak[2] = {0, 0};
        float rms[2] = {0, 0};
        bool clipped = false;
        int64_t samples = 0;
    };
    Levels takeLevels();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fa
