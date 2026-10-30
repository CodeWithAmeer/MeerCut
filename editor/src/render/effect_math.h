// Per-pixel and planning math of the effects, shared by the CPU reference and (as constants and kernels) the
// Direct3D 11 backend, whose HLSL mirrors these functions line by line (internal to fa_render).
//
// Colour values are gamma-encoded Rec.709 R'G'B' in 0..1, straight alpha (effects.h). The Basic colour models are
// fitted to Filmora's measured transfer curves (Stage 1 EXP-006, RMS against the measured curves in brackets):
//   temperature +50: R x1.121, G x0.992, B x0.869 (< 1 level)  tint +50: R x1.121, G x0.892, B x1.121 (< 1 level)
//   exposure -50: x0.44 (0.5)   exposure +50: min(1, 1.075 (1 - (1 - x)^2.30)) (1.9)
//   brightness +50: x1.5 (0.4)  brightness -50: -40/255 (0.4)
//   contrast +50 / -50: x^k / (x^k + (1 - x)^k), k = 1.204 / 0.830 (0.5)
//   highlights +50: + 0.5 x^2 (1 - x) (0.4)
// Slider values are normalised to -1..1 (UI -100..100); the measured +-50 points fix the curves, the rest of the
// range extends them geometrically (exponents and gains to the power 2v). Shadows, whites, blacks, gamma,
// saturation and vibrance were not measured: MeerCut designs (documented in ARCHITECTURE.md section 9).
// HSL and wheels follow EXP-015 / EXP-016 (see below); curves follow EXP-019 (curves.cpp).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fa::fxmath {

inline float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

constexpr float kLumaR = 0.2126f, kLumaG = 0.7152f, kLumaB = 0.0722f;   // BT.709
inline float luma(float r, float g, float b) { return kLumaR * r + kLumaG * g + kLumaB * b; }

struct BasicColor {   // normalised -1..1
    float temperature = 0, tint = 0, exposure = 0, brightness = 0, contrast = 0, highlights = 0, shadows = 0,
          whites = 0, blacks = 0, gamma = 0, saturation = 0, vibrance = 0;
};

// contrast S-curve around 0.5 (endpoints fixed)
inline float contrastCurve(float x, float k) {
    x = clamp01(x);
    if (x <= 0.0f || x >= 1.0f) return x;
    const float a = std::pow(x, k), b = std::pow(1.0f - x, k);
    return a / (a + b);
}

// The per-picture constants of the Basic curves (gains and exponents), computed once instead of per pixel.
struct BasicTerms {
    float wbR = 1, wbG = 1, wbB = 1;   // white balance gains
    float exposureGain = 1, exposureK = 1, contrastK = 1, gammaExp = 1;
};

inline BasicTerms basicTerms(const BasicColor& p) {
    BasicTerms t;
    t.wbR = (1.0f + 0.242f * p.temperature) * (1.0f + 0.242f * p.tint);
    t.wbG = (1.0f - 0.016f * p.temperature) * (1.0f - 0.216f * p.tint);
    t.wbB = (1.0f - 0.262f * p.temperature) * (1.0f + 0.242f * p.tint);
    if (p.exposure < 0) t.exposureGain = std::pow(0.44f, -2.0f * p.exposure);
    else if (p.exposure > 0) {
        t.exposureGain = std::pow(1.075f, 2.0f * p.exposure);
        t.exposureK = std::pow(2.30f, 2.0f * p.exposure);
    }
    if (p.contrast != 0) t.contrastK = p.contrast > 0 ? std::pow(1.204f, 2.0f * p.contrast) : std::pow(0.830f, -2.0f * p.contrast);
    if (p.gamma != 0) t.gammaExp = std::pow(2.0f, -p.gamma);
    return t;
}

inline float basicTone(float c, const BasicColor& p, const BasicTerms& t) {
    c = clamp01(c);
    if (p.exposure < 0) c *= t.exposureGain;
    else if (p.exposure > 0) c = std::min(1.0f, t.exposureGain * (1.0f - std::pow(1.0f - c, t.exposureK)));
    if (p.brightness > 0) c *= 1.0f + p.brightness;
    else if (p.brightness < 0) c += 0.31373f * p.brightness;   // -50 -> -40/255
    c = clamp01(c);
    if (p.contrast != 0) c = contrastCurve(c, t.contrastK);
    if (p.highlights != 0) c += p.highlights * c * c * (1.0f - c);
    if (p.shadows != 0) c += p.shadows * c * (1.0f - c) * (1.0f - c);
    if (p.whites != 0) c += 0.3f * p.whites * c * c * c;
    if (p.blacks != 0) c += 0.3f * p.blacks * (1.0f - c) * (1.0f - c) * (1.0f - c);
    c = clamp01(c);
    if (p.gamma != 0) c = std::pow(c, t.gammaExp);
    return c;
}

inline void basicColor(float& r, float& g, float& b, const BasicColor& p, const BasicTerms& t) {
    if (p.temperature != 0 || p.tint != 0) {
        r *= t.wbR;
        g *= t.wbG;
        b *= t.wbB;
    }
    r = basicTone(r, p, t);
    g = basicTone(g, p, t);
    b = basicTone(b, p, t);
    if (p.saturation != 0 || p.vibrance != 0) {
        const float L = luma(r, g, b);
        const float sat = std::max(r, std::max(g, b)) - std::min(r, std::min(g, b));
        const float k = (1.0f + p.saturation) * std::max(0.0f, 1.0f + p.vibrance * (1.0f - sat) * (1.0f - sat));
        r = clamp01(L + (r - L) * k);
        g = clamp01(L + (g - L) * k);
        b = clamp01(L + (b - L) * k);
    }
}

// ---- RGB <-> HSV (h in degrees 0..360) -----------------------------------------------------------------
inline void rgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
    const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
    v = mx;
    s = mx > 0.0f ? d / mx : 0.0f;
    if (d <= 0.0f) {
        h = 0.0f;
        return;
    }
    if (mx == r) h = 60.0f * ((g - b) / d);
    else if (mx == g) h = 60.0f * ((b - r) / d + 2.0f);
    else h = 60.0f * ((r - g) / d + 4.0f);
    if (h < 0.0f) h += 360.0f;
}

inline void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    h = h - 360.0f * std::floor(h / 360.0f);
    const float c = v * s, hp = h / 60.0f;
    const float x = c * (1.0f - std::fabs(hp - 2.0f * std::floor(hp / 2.0f) - 1.0f));
    float r1 = 0, g1 = 0, b1 = 0;
    if (hp < 1) r1 = c, g1 = x;
    else if (hp < 2) r1 = x, g1 = c;
    else if (hp < 3) g1 = c, b1 = x;
    else if (hp < 4) g1 = x, b1 = c;
    else if (hp < 5) r1 = x, b1 = c;
    else r1 = c, b1 = x;
    const float m = v - c;
    r = r1 + m;
    g = g1 + m;
    b = b1 + m;
}

inline float smoothstepf(float a, float b, float x) {
    const float t = clamp01((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

// ---- HSL (Stage 1 EXP-015, fitted per colour patch of the measured 216-patch chart): 8 colour ranges.
//   hue:        rotation by gain_i * hue_i degrees (at +-100: red 31, green 41 measured, the others 36), weighted by
//               triangles between neighbouring centres (they sum to 1: ranges blend). RMS red 1.32, green 1.39 levels
//   saturation: HSL saturation: the channels move towards (or away from) the HSL lightness grey L = (max + min) / 2
//               by the factor 1 + sat, kept in gamut. A range's weight is 1 within 20 deg of its centre and falls
//               linearly to 0 at 39.5 deg (measured on red and blue; never beyond a neighbouring centre).
//               RMS red 1.33, blue 1.27 (keeping the HSV value instead: 21)
//   luminance:  HSL lightness with the HSL saturation kept: L += 0.25 lum S g(L), same weights as saturation,
//               g(L) = smoothstep(0.081, 0.558, L) (1 - 0.681 smoothstep(0.582, 1, L)). RMS 1.46 (red +50; the
//               darkening side mirrors it, not measured)
// Order: hue, saturation, luminance. Neutral greys are in no range.
constexpr int kHslBands = 8;
constexpr float kHslCenters[kHslBands] = {350, 30, 60, 120, 180, 231, 275, 310};   // red orange yellow green aqua blue purple magenta
constexpr float kHslHueGain[kHslBands] = {31, 36, 36, 41, 36, 36, 36, 36};

// Hue weights: triangles between neighbouring centres.
inline void hslHueWeights(float h, float tri[kHslBands]) {
    for (int i = 0; i < kHslBands; ++i) tri[i] = 0.0f;
    for (int i = 0; i < kHslBands; ++i) {
        const float c0 = kHslCenters[i];
        float c1 = kHslCenters[(i + 1) % kHslBands];
        if (c1 <= c0) c1 += 360.0f;
        float hh = h;
        if (hh < c0) hh += 360.0f;
        if (hh >= c0 && hh < c1) {
            const float t = (hh - c0) / (c1 - c0);
            tri[i] = 1.0f - t;
            tri[(i + 1) % kHslBands] = t;
            return;
        }
    }
}

// Saturation / luminance weight of range i at hue h (degrees).
inline float hslRangeWeight(int i, float h) {
    const float c = kHslCenters[i];
    float d = h - c;
    d -= 360.0f * std::floor((d + 180.0f) / 360.0f);   // -180..180
    const int j = d >= 0.0f ? (i + 1) % kHslBands : (i + kHslBands - 1) % kHslBands;
    float nb = std::fabs(kHslCenters[j] - c);
    if (nb > 180.0f) nb = 360.0f - nb;
    const float zero = std::min(39.5f, nb), plateau = std::min(20.0f, 0.5f * nb);
    const float a = std::fabs(d);
    return a <= plateau ? 1.0f : a >= zero ? 0.0f : (zero - a) / (zero - plateau);
}

struct HslParams {   // normalised -1..1 per band
    float hue[kHslBands] = {}, sat[kHslBands] = {}, lum[kHslBands] = {};
};

inline void hslAdjust(float& r, float& g, float& b, const HslParams& p) {
    float h, s, v;
    rgbToHsv(r, g, b, h, s, v);
    if (s <= 0.0f) return;   // neutral colours are not in any range
    float tri[kHslBands];
    hslHueWeights(h, tri);
    float dh = 0, sf = 0, dl = 0;
    for (int i = 0; i < kHslBands; ++i) {
        dh += tri[i] * p.hue[i] * kHslHueGain[i];
        const float w = hslRangeWeight(i, h);
        sf += w * p.sat[i];
        dl += w * p.lum[i];
    }
    if (dh != 0.0f) hsvToRgb(h + dh, s, v, r, g, b);   // max and min stay: HSV and HSL agree on rotation
    if (sf != 0.0f) {
        const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), L = 0.5f * (mx + mn);
        float f = std::max(0.0f, 1.0f + sf);
        if (f > 1.0f) {   // more saturation stops at the gamut boundary (no hue change)
            if (mx > L) f = std::min(f, (1.0f - L) / (mx - L));
            if (mn < L) f = std::min(f, L / (L - mn));
        }
        r = L + f * (r - L);
        g = L + f * (g - L);
        b = L + f * (b - L);
    }
    if (dl != 0.0f) {
        const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), L = 0.5f * (mx + mn), C = mx - mn;
        const float den = 1.0f - std::fabs(2.0f * L - 1.0f);
        if (C > 0.0f && den > 1e-6f) {
            const float S = std::min(1.0f, C / den);
            const float gL = smoothstepf(0.081f, 0.558f, L) * (1.0f - 0.681f * smoothstepf(0.582f, 1.0f, L));
            const float L2 = clamp01(L + 0.25f * dl * S * gL);
            const float k = (1.0f - std::fabs(2.0f * L2 - 1.0f)) * S / C;   // new chroma / old chroma
            r = L2 + (r - L) * k;
            g = L2 + (g - L) * k;
            b = L2 + (b - L) * k;
        }
    }
    r = clamp01(r);
    g = clamp01(g);
    b = clamp01(b);
}

// ---- colour wheels (Stage 1 EXP-016, fitted on the grey ramp, RGB RMS 0.59 / 0.70 / 0.76 levels): per wheel an
// R, G, B offset v (-1..1, Filmora's numeric fields) and a luminance l. Tonal weight from the pixel's luma x:
//   shadows 1 - smoothstep(0.03, 0.41, x), midtones (4x(1 - x))^3.7, highlights smoothstep(0.57, 1.09, x)
// The offset of a channel moves it by d_c = sum_k w_k A_k v_kc, clipped to 0..1 (realised change r_c); the other
// channels then move the opposite way by B_k times the *realised* change: Filmora's highlights wheel stops lowering
// G/B where R clips near white (an unclipped cross term fitted 1.60 levels RMS there). Luminance adds w_k A_k l_k.
//   A / B: shadows 0.38 / 0.206, midtones 0.364 / 0.150, highlights 0.536 / 0.194
struct WheelParams {
    float v[3][3] = {};   // [shadows, midtones, highlights][r, g, b]
    float lum[3] = {};
};

inline void wheels(float& r, float& g, float& b, const WheelParams& p) {
    const float x = clamp01(luma(r, g, b));
    const float w[3] = {1.0f - smoothstepf(0.03f, 0.41f, x), std::pow(4.0f * x * (1.0f - x), 3.7f), smoothstepf(0.57f, 1.09f, x)};
    const float A[3] = {0.38f, 0.364f, 0.536f}, B[3] = {0.206f, 0.150f, 0.194f};
    const float in[3] = {r, g, b};
    float d[3] = {0, 0, 0}, s[3], out[3], lum = 0;
    for (int k = 0; k < 3; ++k) {
        for (int c = 0; c < 3; ++c) d[c] += w[k] * A[k] * p.v[k][c];
        lum += w[k] * A[k] * p.lum[k];
    }
    for (int c = 0; c < 3; ++c) {
        out[c] = clamp01(in[c] + d[c]);
        s[c] = d[c] != 0.0f ? (out[c] - in[c]) / d[c] : 1.0f;   // realised fraction of the offset
    }
    for (int c = 0; c < 3; ++c) {
        float cross = 0;
        for (int k = 0; k < 3; ++k)
            for (int o = 0; o < 3; ++o)
                if (o != c) cross += B[k] * w[k] * A[k] * p.v[k][o] * s[o];
        out[c] = clamp01(out[c] - cross + lum);
    }
    r = out[0];
    g = out[1];
    b = out[2];
}

// ---- tables (curves): value 0..1 -> linear interpolation in a table of n entries (identical on the GPU)
inline float lookup(const float* table, int n, int stride, float v) {
    const float pos = clamp01(v) * static_cast<float>(n - 1);
    const int i = std::min(n - 2, static_cast<int>(pos));
    const float f = pos - static_cast<float>(i);
    return table[i * stride] + (table[(i + 1) * stride] - table[i * stride]) * f;
}

// ---- vignette (MeerCut design): elliptical distance (roundness -1 = frame shaped, +1 = circle), smooth edge
// between size (1 - feather) and that + feather; amount < 0 darkens, > 0 lightens.
inline float vignetteMask(float u, float v, float aspect, float size, float roundness, float feather) {
    const float ax = 1.0f + (aspect - 1.0f) * (roundness + 1.0f) * 0.5f;   // x stretch towards a circle
    const float d = std::sqrt((2.0f * u * ax) * (2.0f * u * ax) + (2.0f * v) * (2.0f * v)) / std::sqrt(ax * ax + 1.0f);
    const float inner = size * (1.0f - feather);
    return smoothstepf(inner, inner + feather + 1e-3f, d);
}

// ---- noise (film grain): integer hash, identical on the GPU (HLSL uint arithmetic wraps the same way)
inline uint32_t hashU(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
inline float noise01(uint32_t x, uint32_t y, uint32_t seed, uint32_t k) {
    return static_cast<float>(hashU(x + hashU(y + hashU(seed + k * 0x9E3779B9U)))) * (1.0f / 4294967296.0f);
}

// ---- chroma key (Stage 1 EXP-005, fitted on the 32-patch chart: default, tolerance 0/100, pedestal 0 within
// 0.02-0.04 alpha RMS). The pixel's chroma (Cb, Cr) is split into the part along the key colour's chroma (proj,
// 1 = as much as the key) and the part across it (perp); the matte measure x = 1 - proj + 0.2 perp.
//   ramp:     alpha = clamp(s (x - o)), s = 1 + 2.15 t', o = -0.1 + 0.808 t', t' = max(0, tolerance - 0.2) / 0.8
//             (tolerance 0..20 % behave alike, as measured)
//   pedestal: alpha = 0 below the cut x < min(0.95, 0.15 + p (1.33 + 0.34 max(0, 1 - |Y - Ykey| / 0.3))): a hard
//             cut (the measured "step"), higher for colours as bright as the key
//   not reproduced: at pedestal 100 Filmora also dims neutral grey/white (0.23 / 0.52); MeerCut keeps them
struct ChromaKeyParams {
    float kb = 0, kr = 0, k2 = 1, ky = 0;   // key Cb, Cr, |CbCr|^2, luma
    float s = 1, o = -0.1f, pedestal = 0.3f;
};
inline ChromaKeyParams chromaKeyParams(float kr8, float kg8, float kb8, float tolerance, float pedestal) {
    ChromaKeyParams p;
    p.ky = luma(kr8, kg8, kb8);
    p.kb = (kb8 - p.ky) / 1.8556f;
    p.kr = (kr8 - p.ky) / 1.5748f;
    p.k2 = std::max(1e-6f, p.kb * p.kb + p.kr * p.kr);
    const float t = std::max(0.0f, tolerance - 0.2f) / 0.8f;
    p.s = 1.0f + 2.15f * t;
    p.o = -0.1f + 0.808f * t;
    p.pedestal = pedestal;
    return p;
}
// matte (1 = keep) and the pixel's key-direction chroma part (for spill suppression)
inline float chromaMatte(float r, float g, float b, const ChromaKeyParams& k, float& proj) {
    const float y = luma(r, g, b);
    const float cb = (b - y) / 1.8556f, cr = (r - y) / 1.5748f;
    proj = (cb * k.kb + cr * k.kr) / k.k2;
    const float perp = std::fabs(cb * k.kr - cr * k.kb) / k.k2;
    const float x = 1.0f - proj + 0.2f * perp;
    const float cut = std::min(0.95f, 0.15f + k.pedestal * (1.33f + 0.34f * std::max(0.0f, 1.0f - std::fabs(y - k.ky) / 0.3f)));
    return x < cut ? 0.0f : clamp01(k.s * (x - k.o));
}
// removes `amount` of the key-direction chroma (keeps luma): green fringes turn neutral
inline void suppressSpill(float& r, float& g, float& b, const ChromaKeyParams& k, float proj, float amount) {
    if (amount <= 0.0f || proj <= 0.0f) return;
    const float y = luma(r, g, b);
    float cb = (b - y) / 1.8556f, cr = (r - y) / 1.5748f;
    cb -= amount * proj * k.kb;
    cr -= amount * proj * k.kr;
    r = clamp01(y + 1.5748f * cr);
    b = clamp01(y + 1.8556f * cb);
    g = clamp01((y - kLumaR * r - kLumaB * b) / kLumaG);
}

// ---- Gaussian blur plan: large sigmas run on a box-downsampled copy (factor 2^n) so the kernel stays <= 12 sigma
// wide on both backends; weights are computed here once and uploaded to the GPU unchanged.
struct BlurPlan {
    int factor = 0;               // 0 = no blur; 1, 2, 4, ... downsampling factor
    float sigma = 0;              // in the (downsampled) picture
    int radius = 0;               // taps on each side
    std::vector<float> weights;   // weights[0..radius], symmetric, sum over -radius..radius = 1
};

inline BlurPlan planGaussian(float sigmaPx) {
    BlurPlan p;
    if (!(sigmaPx >= 0.3f)) return p;
    p.factor = 1;
    while (sigmaPx / static_cast<float>(p.factor) > 12.0f) p.factor *= 2;
    p.sigma = sigmaPx / static_cast<float>(p.factor);
    p.radius = static_cast<int>(std::ceil(3.0f * p.sigma));
    p.weights.resize(static_cast<size_t>(p.radius) + 1);
    double sum = 0;
    for (int i = 0; i <= p.radius; ++i) {
        const double w = std::exp(-0.5 * double(i) * double(i) / (double(p.sigma) * double(p.sigma)));
        p.weights[static_cast<size_t>(i)] = static_cast<float>(w);
        sum += i == 0 ? w : 2 * w;
    }
    for (auto& w : p.weights) w = static_cast<float>(w / sum);
    return p;
}

// ---- M11 effects (MeerCut designs): values computed once per frame, used by the CPU reference and passed to the
// GPU passes as constants, so both draw from the same numbers.

constexpr uint32_t kGlitchSeed = 0x2545F491U, kVhsSeed = 0x68E31DA4U, kFilmSeed = 0x1B873593U;

// Zoom blur: samples on the line from a pixel to the centre (enough for the longest streak, at most 64).
inline int zoomBlurSamples(float k, int w, int h) {
    const float halfDiagonal = 0.5f * std::sqrt(static_cast<float>(w) * static_cast<float>(w) +
                                                static_cast<float>(h) * static_cast<float>(h));
    return std::clamp(static_cast<int>(std::ceil(k * halfDiagonal)) + 1, 2, 64);
}

// Camera shake: a smooth wobble in -1..1 (three detuned sines); `axis` 0, 1, 2 are independent.
inline float shakeNoise(double seconds, double hz, int axis) {
    static const double kPhase[3] = {0.3, 1.7, 4.1};
    const double w = 2.0 * 3.14159265358979323846 * hz * seconds, ph = kPhase[axis % 3];
    return static_cast<float>(
        (std::sin(w + ph) + 0.6 * std::sin(2.13 * w + 2.0 * ph) + 0.35 * std::sin(3.71 * w + 3.0 * ph)) / 1.95);
}
struct ShakeFrame {
    float ox = 0, oy = 0;   // the picture's offset, px
    float cr = 1, sr = 0;   // cos / sin of its rotation
    float zoom = 1;
};
inline ShakeFrame shakeFrame(double seconds, float amountPx, float hz, float rotationDeg, float zoomPercent) {
    ShakeFrame f;
    f.ox = amountPx * shakeNoise(seconds, hz, 0);
    f.oy = amountPx * shakeNoise(seconds, hz, 1);
    const double r = rotationDeg * shakeNoise(seconds, hz, 2) * 3.14159265358979323846 / 180.0;
    f.cr = static_cast<float>(std::cos(r));
    f.sr = static_cast<float>(std::sin(r));
    f.zoom = 1.0f + zoomPercent / 100.0f;
    return f;
}

// Glitch: the pattern changes `perSecond` times a second.
inline uint32_t glitchSlot(double seconds, float perSecond) {
    return static_cast<uint32_t>(static_cast<int64_t>(std::floor(std::max(0.0, seconds) * perSecond)));
}

// VHS: a band of tracking distortion rolling down the picture (centre and half height in px) and the jitter slot.
struct VhsFrame {
    float bandY = 0, bandH = 1;
    uint32_t slot = 0;
};
inline VhsFrame vhsFrame(double seconds, int h) {
    VhsFrame f;
    const double s = std::max(0.0, seconds);
    f.bandY = static_cast<float>(((s * 0.11 - std::floor(s * 0.11)) * 1.2 - 0.1) * h);
    f.bandH = std::max(1.0f, 0.05f * static_cast<float>(h));
    f.slot = static_cast<uint32_t>(static_cast<int64_t>(std::floor(s * 30.0)));
    return f;
}

// Old film: the film frame (24 a second), its flicker and four scratch lines (x, half width px, opacity).
struct FilmFrame {
    uint32_t slot = 0;
    float flicker = 1;
    float x[4] = {}, w[4] = {1, 1, 1, 1}, o[4] = {};
};
inline FilmFrame oldFilmFrame(double seconds, int w, float scale, float scratches, float flicker) {
    FilmFrame f;
    f.slot = static_cast<uint32_t>(static_cast<int64_t>(std::floor(std::max(0.0, seconds) * 24.0)));
    f.flicker = 1.0f + flicker * 0.12f * (noise01(f.slot, 1U, kFilmSeed, 3U) * 2.0f - 1.0f);
    for (uint32_t i = 0; i < 4; ++i) {
        f.x[i] = noise01(f.slot, i, kFilmSeed, 4U) * static_cast<float>(w);
        f.w[i] = std::max(0.5f, (0.6f + 1.4f * noise01(f.slot, i, kFilmSeed, 5U)) * scale);
        f.o[i] = noise01(f.slot, i, kFilmSeed, 6U) > 0.5f ? 0.5f * scratches : 0.0f;
    }
    return f;
}

// Light leak: two drifting glows (centres and sigmas in px) and their brightness pulse.
struct LeakFrame {
    float x1 = 0, y1 = 0, s1 = 1, x2 = 0, y2 = 0, s2 = 1, pulse = 1;
};
inline LeakFrame lightLeakFrame(double seconds, float speed, float sizePercent, int w, int h) {
    LeakFrame f;
    const double t = std::max(0.0, seconds) * speed;
    f.x1 = static_cast<float>(w * (0.2 + 0.15 * std::sin(t * 1.3)));
    f.y1 = static_cast<float>(h * (0.3 + 0.2 * std::sin(t * 0.9 + 1.0)));
    f.x2 = static_cast<float>(w * (0.85 + 0.1 * std::sin(t * 1.1 + 2.0)));
    f.y2 = static_cast<float>(h * (0.75 + 0.15 * std::cos(t * 0.8)));
    f.s1 = std::max(1.0f, sizePercent / 100.0f * static_cast<float>(h) * 0.45f);
    f.s2 = std::max(1.0f, sizePercent / 100.0f * static_cast<float>(h) * 0.3f);
    f.pulse = static_cast<float>(0.75 + 0.25 * std::sin(t * 1.7));
    return f;
}

// Letterbox: the aspect ratios of the Ratio choice.
inline float letterboxRatio(int choice) {
    static const float r[7] = {2.39f, 2.0f, 1.85f, 4.0f / 3.0f, 1.0f, 0.8f, 9.0f / 16.0f};
    return r[std::clamp(choice, 0, 6)];
}

}  // namespace fa::fxmath
