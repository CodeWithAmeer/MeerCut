#include "fa/render/audio_dsp.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>

namespace fa {

double dbToGain(double db) { return db <= -120.0 ? 0.0 : std::pow(10.0, db / 20.0); }

double gainToDb(double gain) { return gain <= 0 ? -std::numeric_limits<double>::infinity() : 20.0 * std::log10(gain); }

const char* crossfadeShapeId(CrossfadeShape s) {
    switch (s) {
    case CrossfadeShape::ConstantGain: return "constantGain";
    case CrossfadeShape::EqualPower: return "equalPower";
    case CrossfadeShape::Exponential: return "exponential";
    case CrossfadeShape::Logarithmic: return "logarithmic";
    }
    return "equalPower";
}

const char* crossfadeShapeLabel(CrossfadeShape s) {
    switch (s) {
    case CrossfadeShape::ConstantGain: return "Constant Gain";
    case CrossfadeShape::EqualPower: return "Equal Power";
    case CrossfadeShape::Exponential: return "Exponential";
    case CrossfadeShape::Logarithmic: return "Logarithmic";
    }
    return "Equal Power";
}

bool crossfadeShapeFromId(const std::string& id, CrossfadeShape& out) {
    for (CrossfadeShape s : {CrossfadeShape::ConstantGain, CrossfadeShape::EqualPower, CrossfadeShape::Exponential,
                             CrossfadeShape::Logarithmic})
        if (id == crossfadeShapeId(s)) {
            out = s;
            return true;
        }
    return false;
}

namespace {

double expFall(double x) {   // 1 at x = 0, 0 at x = 1, dB-linear over 60 dB in between
    constexpr double floor = 1e-3;
    return (std::pow(10.0, -3.0 * x) - floor) / (1.0 - floor);
}

}  // namespace

void crossfadeGains(CrossfadeShape s, double x, double& a, double& b) {
    x = std::clamp(x, 0.0, 1.0);
    switch (s) {
    case CrossfadeShape::ConstantGain:
        a = 1.0 - x;
        b = x;
        return;
    case CrossfadeShape::EqualPower:
        a = std::cos(std::numbers::pi * x / 2);
        b = std::sin(std::numbers::pi * x / 2);
        return;
    case CrossfadeShape::Exponential:
        a = expFall(x);
        b = 1.0 - a;
        return;
    case CrossfadeShape::Logarithmic:
        a = 1.0 - expFall(1.0 - x);
        b = 1.0 - a;
        return;
    }
}

void balanceGains(double b, double& left, double& right) {
    b = std::clamp(b, -1.0, 1.0);
    const double far = std::cos(std::numbers::pi * std::fabs(b) / 2);
    left = b > 0 ? far : 1.0;
    right = b < 0 ? far : 1.0;
}

// ---- RBJ cookbook biquads -------------------------------------------------------------------------------------

namespace {

Biquad normalised(double b0, double b1, double b2, double a0, double a1, double a2) {
    Biquad f;
    f.b0 = b0 / a0;
    f.b1 = b1 / a0;
    f.b2 = b2 / a0;
    f.a1 = a1 / a0;
    f.a2 = a2 / a0;
    return f;
}

}  // namespace

Biquad Biquad::peaking(double fs, double f0, double q, double gainDb) {
    if (std::fabs(gainDb) < 1e-9 || f0 <= 0 || f0 >= fs / 2) return {};
    const double A = std::pow(10.0, gainDb / 40.0), w0 = 2 * std::numbers::pi * f0 / fs, alpha = std::sin(w0) / (2 * q);
    const double c = std::cos(w0);
    return normalised(1 + alpha * A, -2 * c, 1 - alpha * A, 1 + alpha / A, -2 * c, 1 - alpha / A);
}

Biquad Biquad::highPass(double fs, double f0, double q) {
    if (f0 <= 0) return {};
    f0 = std::min(f0, fs * 0.49);
    const double w0 = 2 * std::numbers::pi * f0 / fs, alpha = std::sin(w0) / (2 * q), c = std::cos(w0);
    return normalised((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}

Biquad Biquad::lowPass(double fs, double f0, double q) {
    if (f0 >= fs / 2) return {};
    f0 = std::max(f0, 1.0);
    const double w0 = 2 * std::numbers::pi * f0 / fs, alpha = std::sin(w0) / (2 * q), c = std::cos(w0);
    return normalised((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}

Biquad Biquad::notch(double fs, double f0, double q) {
    if (f0 <= 0 || f0 >= fs / 2) return {};
    const double w0 = 2 * std::numbers::pi * f0 / fs, alpha = std::sin(w0) / (2 * q), c = std::cos(w0);
    return normalised(1, -2 * c, 1, 1 + alpha, -2 * c, 1 - alpha);
}

double Biquad::magnitudeDb(double fs, double f) const {
    const std::complex<double> z = std::polar(1.0, -2 * std::numbers::pi * f / fs);   // z^-1
    const std::complex<double> num = b0 + b1 * z + b2 * z * z, den = 1.0 + a1 * z + a2 * z * z;
    return 20.0 * std::log10(std::abs(num / den));
}

// ---- EQ presets (MeerCut's own) --------------------------------------------------------------------------------

const std::vector<EqPreset>& eqPresets() {
    //                                          31   62  125  250  500   1k   2k   4k   8k  16k
    static const std::vector<EqPreset> presets = {
        {"flat", "Flat", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {"voice-clarity", "Voice Clarity", {-6, -4, -2, 0, 0, 1, 3, 3, 2, 0}},
        {"warm-voice", "Warm Voice", {-3, 0, 2, 2, 1, 0, -1, -1, -2, -3}},
        {"podcast", "Podcast", {-8, -5, -1, 1, 1, 1, 2, 2, 0, -2}},
        {"bass-boost", "Bass Boost", {5, 6, 4, 2, 0, 0, 0, 0, 0, 0}},
        {"bass-cut", "Bass Cut", {-12, -9, -5, -2, 0, 0, 0, 0, 0, 0}},
        {"treble-boost", "Treble Boost", {0, 0, 0, 0, 0, 0, 2, 4, 5, 5}},
        {"treble-cut", "Treble Cut", {0, 0, 0, 0, 0, 0, -2, -4, -6, -8}},
        {"telephone", "Telephone", {-12, -12, -12, -6, 2, 4, 2, -6, -12, -12}},
        {"music-presence", "Music Presence", {2, 2, 1, 0, -1, 0, 1, 2, 2, 1}},
    };
    return presets;
}

const EqPreset* findEqPreset(const std::string& id) {
    for (const auto& p : eqPresets())
        if (p.id == id) return &p;
    return nullptr;
}

}  // namespace fa
