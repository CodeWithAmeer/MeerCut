// Audio cleanup, loudness and dynamics (M6). Conventional, documented signal processing - none of it is "AI".
//
//   * Noise reduction: short-time spectral attenuation. STFT with a 2048-point periodic Hann window and a 512-sample
//     hop (75 % overlap; the windows sum to 2, so unity gains reconstruct the input exactly). The noise power of
//     every bin is estimated by minimum statistics: the power is smoothed over 5 frames x 5 bins, its minimum is
//     taken over +-70 frames (+-0.75 s at 48 kHz) and scaled by 2.70 (the bias of that minimum for stationary noise,
//     measured by simulation). Each bin gets the gain max(floor, 1 - alpha N / P) (P: the power smoothed over
//     3 frames x 3 bins), smoothed over 3 bins; strength s in (0, 1] sets alpha = 1 + 1.5 s and the floor
//     -6 - 18 s dB. Frames sit on a grid anchored to the clip's first sample and every output frame depends only on
//     input within +-38912 samples, so the result does not depend on how the clip is cut into blocks.
//     Suited to steady hiss / fan / room noise under speech; it does not remove clicks, wind gusts or other voices,
//     and a perfectly steady tone lasting longer than the 1.5 s window (a sustained note, a whine) looks like noise to
//     the estimator and is reduced too - meant for dialogue, not music.
//   * Hum removal: notches (Q 10: 5 Hz wide at 50 Hz) at the mains frequency (50 / 60 Hz) and its harmonics. The Q is
//     kept moderate so a notch settles (time constant 3056 samples at 50 Hz) well within the DSP warm-up.
//   * High-pass / low-pass: 4th-order Butterworth (24 dB / octave, -3 dB at the cutoff).
//   * Loudness: ITU-R BS.1770-4 / EBU R 128 integrated loudness (K-weighting, 400 ms blocks with 75 % overlap,
//     absolute gate -70 LUFS, relative gate -10 LU); normalisation applies target - measured as a fixed gain.
//   * Limiter: a sample-peak limiter (not true-peak) on the master mix; look-ahead and smoothing over the same
//     2400-sample (50 ms) window guarantee |output| <= ceiling without a recursive state. The smoothing sums in exact
//     fixed point, so a sample's gain depends only on the input within one window on either side: streamed playback
//     and export give the same samples.
//   * Ducking envelope: see duckDepth().
#pragma once

#include "fa/render/audio_dsp.h"

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace fa {

// ---- noise reduction -------------------------------------------------------------------------------------------

inline constexpr int kDenoiseFft = 2048;
inline constexpr int kDenoiseHop = 512;
inline constexpr int kDenoiseHistoryFrames = 70;
inline constexpr double kDenoiseBias = 2.70;
// Input needed on each side of the samples to be produced (frames reach +-(70 + 2 + 2) hops plus a window).
inline constexpr int kDenoiseMargin = (kDenoiseHistoryFrames + 4) * kDenoiseHop + kDenoiseFft;

// Denoise one channel. `in` holds samples [inStart, inStart + n) of a signal whose STFT frames start at multiples of
// the hop from `anchor` (the clip's first sample); `out` receives the same range. Output samples closer than
// kDenoiseMargin to either end of the input are not final (callers keep only the middle).
void denoiseChannel(const float* in, int64_t n, int64_t inStart, int64_t anchor, double strength, float* out);

// ---- filters ---------------------------------------------------------------------------------------------------

struct CleanupSettings {
    double denoise = 0;       // 0 = off, 0..1
    double humHz = 0;         // 0 = off, else 50 / 60
    int humHarmonics = 4;     // notches at humHz * 1 .. humHarmonics
    double highPassHz = 0;    // 0 = off
    double lowPassHz = 0;     // 0 = off
    bool active() const { return denoise > 0 || humHz > 0 || highPassHz > 0 || lowPassHz > 0; }
};
// The filter cascade of the settings at rate fs (hum notches, high-pass, low-pass), in processing order.
std::vector<Biquad> cleanupFilters(const CleanupSettings& s, double fs);

// ---- loudness ----------------------------------------------------------------------------------------------------

// BS.1770-4 K-weighting (pre-filter + RLB high-pass) at rate fs.
std::array<Biquad, 2> kWeighting(double fs);

class LoudnessMeter {
public:
    LoudnessMeter(int sampleRate, int channels);
    void add(const float* interleaved, int64_t frames);
    // Integrated loudness in LUFS (gated); nullopt-like -inf (returned as -HUGE_VAL) when nothing passes the gates.
    double integrated() const;
    double momentaryMax() const;   // loudest 400 ms block (LUFS)
private:
    int rate_, channels_;
    std::array<Biquad, 2> k_;
    std::vector<std::array<BiquadState, 2>> state_;
    int64_t subLen_;                 // 100 ms
    int64_t inSub_ = 0;
    std::vector<double> subAcc_;     // per channel
    std::vector<double> subBlocks_;  // sum over channels of the mean square of each 100 ms sub-block
};

// ---- limiter -------------------------------------------------------------------------------------------------------

inline constexpr int kLimiterWindow = 2400;   // samples at 48 kHz (scaled with the rate by the caller)

// Limit `in` (interleaved, `frames` frames) to |x| <= ceiling and write frames [from, from + count) to out. Needs
// window frames of input before `from` and after `from + count`.
void limitPeaks(const float* in, int64_t frames, int channels, int window, double ceiling, int64_t from, int64_t count,
                float* out);

// ---- ducking -------------------------------------------------------------------------------------------------------

// Depth (0 = no ducking, 1 = fully down) at timeline sample s given the priority source's sounding runs [a, b) in
// samples (sorted, not overlapping). Look-ahead attack: the depth rises over `attack` samples before a run starts, so
// the target is fully down when the priority sound begins; it stays down during the run and `hold` samples after,
// then falls back over `release` samples. Ramps are smoothsteps (continuous slope). Local and deterministic.
double duckDepth(const std::vector<std::pair<int64_t, int64_t>>& runs, int64_t s, int64_t attack, int64_t hold,
                 int64_t release);

}  // namespace fa
