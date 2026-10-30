#include "fa/media/hdr.h"

#include <algorithm>
#include <cmath>

namespace fa {

namespace {
constexpr float kRefWhiteNits = 203.0f;   // ITU-R BT.2408 HDR reference white
constexpr float kKnee = 0.75f;
constexpr float kHlgPeakNits = 1000.0f;   // nominal HLG display for the OOTF
constexpr float kHlgGamma = 1.2f;
// BT.2020 -> BT.709 primaries, linear light
constexpr float k2020to709[3][3] = {{1.6605f, -0.5876f, -0.0728f}, {-0.1246f, 1.1329f, -0.0083f}, {-0.0182f, -0.1006f, 1.1187f}};
}  // namespace

HdrTransfer hdrTransferFromName(const std::string& n) {
    if (n == "smpte2084") return HdrTransfer::PQ;
    if (n == "arib-std-b67") return HdrTransfer::HLG;
    return HdrTransfer::None;
}

const char* hdrTransferLabel(HdrTransfer t) {
    switch (t) {
        case HdrTransfer::PQ: return "PQ (SMPTE ST 2084)";
        case HdrTransfer::HLG: return "HLG (ARIB STD-B67)";
        default: return "SDR";
    }
}

float HdrToSdr::pqToNits(float e) {
    constexpr double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0, c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0,
                     c3 = 2392.0 / 4096.0 * 32.0;
    const double p = std::pow(std::clamp(double(e), 0.0, 1.0), 1.0 / m2);
    return static_cast<float>(10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1));
}

float HdrToSdr::hlgToScene(float e) {
    constexpr double a = 0.17883277, b = 1.0 - 4.0 * a, c = 0.55991073;
    const double x = std::clamp(double(e), 0.0, 1.0);
    return static_cast<float>(x <= 0.5 ? x * x / 3.0 : (std::exp((x - c) / a) + b) / 12.0);
}

HdrToSdr::HdrToSdr(HdrTransfer transfer, bool bt2020, float peakNits)
    : transfer_(transfer), bt2020_(bt2020), peak_(std::max(peakNits, 300.0f) / kRefWhiteNits) {
    eotf_.resize(65536);
    for (int i = 0; i < 65536; ++i) {
        const float s = float(i) / 65535.0f;
        eotf_[static_cast<size_t>(i)] = transfer == HdrTransfer::HLG ? hlgToScene(s) : pqToNits(s) / kRefWhiteNits;
    }
    // indexed by sqrt(L): gamma 1/2.4 is steep near black, a table on L would band the shadows
    oetf_.resize(16384);
    for (int i = 0; i < 16384; ++i)
        oetf_[static_cast<size_t>(i)] = static_cast<uint8_t>(std::lround(255.0 * std::pow(i / 16383.0, 2.0 / 2.4)));
}

float HdrToSdr::toneMap(float x) const {
    if (x <= kKnee) return std::max(x, 0.0f);
    const float a = 1.0f - kKnee;
    const float u = (x - kKnee) / a, U = (peak_ - kKnee) / a;
    return std::min(1.0f, kKnee + a * u * (1.0f + u / (U * U)) / (1.0f + u));
}

namespace {
// display light relative to reference white (HLG: scene light -> BT.2100 OOTF on a 1000-nit display)
inline void toDisplay(HdrTransfer t, float& r, float& g, float& b) {
    if (t != HdrTransfer::HLG) return;
    const float ys = 0.2627f * r + 0.6780f * g + 0.0593f * b;
    const float k = kHlgPeakNits / kRefWhiteNits * std::pow(std::max(ys, 1e-6f), kHlgGamma - 1.0f);
    r *= k, g *= k, b *= k;
}
}  // namespace

std::array<float, 3> HdrToSdr::convert(float r, float g, float b) const {
    float v[3];
    const float in[3] = {r, g, b};
    for (int c = 0; c < 3; ++c) v[c] = transfer_ == HdrTransfer::HLG ? hlgToScene(in[c]) : pqToNits(in[c]) / kRefWhiteNits;
    toDisplay(transfer_, v[0], v[1], v[2]);
    float o[3] = {v[0], v[1], v[2]};
    if (bt2020_)
        for (int c = 0; c < 3; ++c) o[c] = k2020to709[c][0] * v[0] + k2020to709[c][1] * v[1] + k2020to709[c][2] * v[2];
    for (float& x : o) x = std::max(x, 0.0f);
    const float m = std::max({o[0], o[1], o[2]});
    const float s = m > 0.0f ? toneMap(m) / m : 0.0f;
    std::array<float, 3> out;
    for (int c = 0; c < 3; ++c) out[c] = static_cast<float>(std::pow(std::clamp(o[c] * s, 0.0f, 1.0f), 1.0 / 2.4));
    return out;
}

void HdrToSdr::convertRow(const uint16_t* p, uint8_t* q, int width) const {
    for (int x = 0; x < width; ++x, p += 3, q += 4) {
        float v[3] = {eotf_[p[0]], eotf_[p[1]], eotf_[p[2]]};
        toDisplay(transfer_, v[0], v[1], v[2]);
        float o[3] = {v[0], v[1], v[2]};
        if (bt2020_)
            for (int c = 0; c < 3; ++c) o[c] = k2020to709[c][0] * v[0] + k2020to709[c][1] * v[1] + k2020to709[c][2] * v[2];
        for (float& y : o) y = std::max(y, 0.0f);
        const float m = std::max({o[0], o[1], o[2]});
        const float s = m > 0.0f ? toneMap(m) / m : 0.0f;
        for (int c = 0; c < 3; ++c) q[c] = oetf_[static_cast<size_t>(std::lround(std::sqrt(std::clamp(o[c] * s, 0.0f, 1.0f)) * 16383.0f))];
        q[3] = 255;
    }
}

}  // namespace fa
