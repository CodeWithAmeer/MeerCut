// Retimed audio of one clip (M5), used by the AudioMixer for clips whose speed is not plain 1x forward.
//
// Policy (shared by playback and export):
//   * The clip's speed map (fa/model/time_map.h) decides which source instant every output sample plays, so audio
//     stays locked to the video frames of the same clip.
//   * Freeze frames are silent (Filmora's frozen sections are silent).
//   * Maintain Pitch OFF (and reverse at 1x): the source is resampled along the map - the pitch follows the speed
//     (2x: a 1 kHz tone plays at 2 kHz, Stage 1 EXP-004). Windowed-sinc interpolation, low-passed when sped up.
//   * Maintain Pitch ON with a speed other than 1x: WSOLA time stretch anchored to the map. Output frames of 32 ms
//     (Hann, 50 % overlap, so the windows sum to 1) are taken from the source where the map puts them, shifted by at
//     most +-10 ms to continue the previous frame's waveform (normalised cross-correlation). No latency and no drift:
//     the timing error is bounded by the search range. (SoundTouch was evaluated first and dropped: its streaming
//     output is offset by a tempo-dependent 20-70 ms and it can skip short sounds above 1x.)
//   * Reverse plays the source samples backwards (with Maintain Pitch, reversed and time-stretched).
#pragma once

#include "fa/media/media.h"
#include "fa/model/project.h"

#include <memory>
#include <vector>

namespace fa {

class AudioRetimer {
public:
    AudioRetimer(int sampleRate, int channels);
    ~AudioRetimer();
    AudioRetimer(const AudioRetimer&) = delete;
    AudioRetimer& operator=(const AudioRetimer&) = delete;

    // Write the clip's samples for timeline samples [from, from + count) into out (count * channels floats,
    // overwritten; silence outside the clip). Sequential calls continue the stretch state; others restart it.
    void render(const Clip& clip, AudioDecoder& source, int64_t from, int count, float* out);

private:
    void resampled(const Clip& clip, AudioDecoder& src, int64_t from, int count, float* out);
    void stretched(const Clip& clip, AudioDecoder& src, int64_t from, int count, float* out);

    int rate_;
    int channels_;
    struct Stretch;
    std::unique_ptr<Stretch> st_;
    std::vector<float> src_, mono_;
    std::vector<float> buf_;
};

}  // namespace fa
