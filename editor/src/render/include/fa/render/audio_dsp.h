// Audio DSP building blocks for the mixer (M6). Everything here is deterministic: no state survives between calls
// unless the caller keeps it, and the block processor makes stateful filters independent of how a timeline is cut
// into mix requests (playback chunks, seeks, export).
//
// Gain laws (Stage 1 EXP-020 where Filmora was measured, own definitions elsewhere):
//   * volume in dB is a plain multiply: gain = 10^(dB / 20); -120 dB and below = silence
//   * fades are linear in amplitude over their duration, measured from the clip's timeline edges (EXP-020)
//   * balance b in [-1, 1]: the near channel stays at unity, the far channel is scaled by cos(pi |b| / 2) (EXP-020:
//     -50 -> -3.01 dB, -80 -> -10.2 dB); a mono source plays on both channels at unity and is balanced the same way
//   * crossfades (own definitions; x = 0 .. 1 across the transition, A = outgoing, B = incoming):
//       Constant Gain   A = 1 - x,                B = x                 midpoint -6.02 dB each (amplitudes sum to 1)
//       Equal Power     A = cos(pi x / 2),        B = sin(pi x / 2)     midpoint -3.01 dB each (powers sum to 1)
//       Exponential     A = (10^(-3x) - 10^-3) / (1 - 10^-3), B = 1 - A  the outgoing clip falls fast (dB-linear over
//                                                                        60 dB), the incoming one rises early;
//                                                                        midpoint A -30.3 dB, B -0.27 dB
//       Logarithmic     the time mirror: A(x) = 1 - E(1 - x), B = 1 - A  the outgoing clip holds, then falls late
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fa {

double dbToGain(double db);
double gainToDb(double gain);   // -inf for 0

enum class CrossfadeShape { ConstantGain, EqualPower, Exponential, Logarithmic };
const char* crossfadeShapeId(CrossfadeShape s);        // "constantGain", "equalPower", "exponential", "logarithmic"
const char* crossfadeShapeLabel(CrossfadeShape s);     // UI
bool crossfadeShapeFromId(const std::string& id, CrossfadeShape& out);
// Gains of the outgoing (A) and incoming (B) clip at position x in [0, 1].
void crossfadeGains(CrossfadeShape s, double x, double& a, double& b);

// Balance law (EXP-020): per-channel gains for b in [-1, 1].
void balanceGains(double b, double& left, double& right);

// ---- filters ------------------------------------------------------------------------------------------------

// RBJ "Audio EQ Cookbook" biquad (transposed direct form II, double precision).
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    static Biquad peaking(double fs, double f0, double q, double gainDb);
    static Biquad highPass(double fs, double f0, double q = 0.70710678118654752);
    static Biquad lowPass(double fs, double f0, double q = 0.70710678118654752);
    static Biquad notch(double fs, double f0, double q);
    bool identity() const { return b0 == 1 && b1 == 0 && b2 == 0 && a1 == 0 && a2 == 0; }
    double magnitudeDb(double fs, double f) const;   // response at frequency f (tests, UI curve)
};

struct BiquadState {
    double z1 = 0, z2 = 0;
    double process(const Biquad& f, double x) {
        const double y = f.b0 * x + z1;
        z1 = f.b1 * x - f.a1 * y + z2;
        z2 = f.b2 * x - f.a2 * y;
        return y;
    }
};

// Graphic equaliser: 10 peaking bands an octave apart (31 Hz .. 16 kHz), +-12 dB, Q = 1.41.
inline constexpr int kEqBands = 10;
inline constexpr std::array<double, kEqBands> kEqFrequencies{31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
inline constexpr double kEqMaxDb = 12.0;
struct EqPreset {
    std::string id;     // stable id stored in projects
    std::string name;   // UI
    std::array<double, kEqBands> gainsDb{};
};
const std::vector<EqPreset>& eqPresets();   // MeerCut's own presets ("flat" first)
const EqPreset* findEqPreset(const std::string& id);

}  // namespace fa
