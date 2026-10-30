// CPU reference implementation of the effect stack (effects.h). The Direct3D 11 backend mirrors every step.
#include "fa/render/effects.h"
#include "fa/render/lut.h"

#include "effect_math.h"
#include "parallel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

namespace fa {

namespace {

using namespace fxmath;

// Float RGBA picture, straight alpha (4 floats per pixel).
struct FImage {
    int w = 0, h = 0;
    std::vector<float> px;
    void reset(int width, int height) {
        w = width;
        h = height;
        px.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0.0f);
    }
    float* row(int y) { return px.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4; }
    const float* row(int y) const { return px.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4; }
};

void toFloat(const Image& src, FImage& f) {
    f.reset(src.width, src.height);
    parallelRows(0, src.height, [&](int y) {
        const uint8_t* s = src.row(y);
        float* d = f.row(y);
        for (int i = 0; i < src.width * 4; ++i) d[i] = s[i] / 255.0f;
    });
}

void toBytes(const FImage& f, Image& out) {
    if (out.width != f.w || out.height != f.h) out.reset(f.w, f.h);
    parallelRows(0, f.h, [&](int y) {
        const float* s = f.row(y);
        uint8_t* d = out.row(y);
        for (int i = 0; i < f.w * 4; ++i) d[i] = static_cast<uint8_t>(std::floor(clamp01(s[i]) * 255.0f + 0.5f));
    });
}

void premultiply(FImage& f) {
    parallelRows(0, f.h, [&](int y) {
        float* p = f.row(y);
        for (int x = 0; x < f.w; ++x, p += 4) {
            p[0] *= p[3];
            p[1] *= p[3];
            p[2] *= p[3];
        }
    });
}

void unpremultiply(FImage& f) {
    parallelRows(0, f.h, [&](int y) {
        float* p = f.row(y);
        for (int x = 0; x < f.w; ++x, p += 4) {
            if (p[3] > 1e-6f) {
                const float k = 1.0f / p[3];
                p[0] = clamp01(p[0] * k);
                p[1] = clamp01(p[1] * k);
                p[2] = clamp01(p[2] * k);
            } else {
                p[0] = p[1] = p[2] = 0.0f;
            }
        }
    });
}

// ---- colour ------------------------------------------------------------------------------------------

BasicColor basicParams(const EffectInstance& e) {
    BasicColor p;
    p.temperature = e.value("temperature") / 100.0f;
    p.tint = e.value("tint") / 100.0f;
    p.exposure = e.value("exposure") / 100.0f;
    p.brightness = e.value("brightness") / 100.0f;
    p.contrast = e.value("contrast") / 100.0f;
    p.highlights = e.value("highlights") / 100.0f;
    p.shadows = e.value("shadows") / 100.0f;
    p.whites = e.value("whites") / 100.0f;
    p.blacks = e.value("blacks") / 100.0f;
    p.gamma = e.value("gamma") / 100.0f;
    p.saturation = e.value("saturation") / 100.0f;
    p.vibrance = e.value("vibrance") / 100.0f;
    return p;
}

void runBasic(FImage& img, const EffectInstance& e) {
    const BasicColor p = basicParams(e);
    const BasicTerms t = basicTerms(p);
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        for (int x = 0; x < img.w; ++x, q += 4) basicColor(q[0], q[1], q[2], p, t);
    });
}

HslParams hslParams(const EffectInstance& e) {
    HslParams p;
    static const char* bands[kHslBands] = {"red", "orange", "yellow", "green", "aqua", "blue", "purple", "magenta"};
    for (int i = 0; i < kHslBands; ++i) {
        const std::string b = bands[i];
        p.hue[i] = e.value(b + ".hue") / 100.0f;
        p.sat[i] = e.value(b + ".saturation") / 100.0f;
        p.lum[i] = e.value(b + ".luminance") / 100.0f;
    }
    return p;
}

void runHsl(FImage& img, const EffectInstance& e) {
    const HslParams p = hslParams(e);
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        for (int x = 0; x < img.w; ++x, q += 4) hslAdjust(q[0], q[1], q[2], p);
    });
}

// Curves: channel curve, then the master curve, per channel; then hue curves in HSV.
void runCurves(FImage& img, const EffectInstance& e) {
    if (!e.tables) return;
    const auto& tone = e.tables->tone;
    const auto& hue = e.tables->hue;
    const int n = EffectTables::kCurveSize;
    const float* T = &tone[0][0];
    const float* H = &hue[0][0];
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        for (int x = 0; x < img.w; ++x, q += 4) {
            for (int c = 0; c < 3; ++c) q[c] = lookup(T, n, 4, lookup(T + 1 + c, n, 4, q[c]));
            float h, sat, v;
            rgbToHsv(q[0], q[1], q[2], h, sat, v);
            if (sat > 0.0f) {
                const float hn = h / 360.0f;
                const float dh = lookup(H, n, 2, hn), sf = lookup(H + 1, n, 2, hn);
                if (dh != 0.0f || sf != 1.0f) hsvToRgb(h + 360.0f * dh, clamp01(sat * sf), v, q[0], q[1], q[2]);
            }
        }
    });
}

WheelParams wheelParams(const EffectInstance& e) {
    WheelParams p;
    static const char* w[3] = {"shadows", "midtones", "highlights"};
    for (int k = 0; k < 3; ++k) {
        const std::string b = w[k];
        p.v[k][0] = e.value(b + ".r");
        p.v[k][1] = e.value(b + ".g");
        p.v[k][2] = e.value(b + ".b");
        p.lum[k] = e.value(b + ".lum");
    }
    return p;
}

void runWheels(FImage& img, const EffectInstance& e) {
    const WheelParams p = wheelParams(e);
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        for (int x = 0; x < img.w; ++x, q += 4) wheels(q[0], q[1], q[2], p);
    });
}

void runLut(FImage& img, const EffectInstance& e) {
    const Lut3D& lut = *e.tables->lut;
    const float k = e.value("strength") / 100.0f;
    const LutDomainMode mode = e.value("domain") >= 0.5f ? LutDomainMode::FilmoraCompatible : LutDomainMode::Standard;
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        for (int x = 0; x < img.w; ++x, q += 4) {
            float r = q[0], g = q[1], b = q[2];
            applyLut(lut, mode, r, g, b);
            q[0] += (r - q[0]) * k;
            q[1] += (g - q[1]) * k;
            q[2] += (b - q[2]) * k;
        }
    });
}

void runVignette(FImage& img, const EffectInstance& e) {
    const float amount = e.value("amount") / 100.0f, size = e.value("size") / 100.0f;
    const float roundness = e.value("roundness") / 100.0f, feather = e.value("feather") / 100.0f;
    const float aspect = static_cast<float>(img.w) / static_cast<float>(std::max(1, img.h));
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(img.h) - 0.5f;
        for (int x = 0; x < img.w; ++x, q += 4) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(img.w) - 0.5f;
            const float m = vignetteMask(u, v, aspect, size, roundness, feather);
            for (int c = 0; c < 3; ++c) q[c] = amount < 0 ? clamp01(q[c] * (1.0f + amount * m)) : clamp01(q[c] + (1.0f - q[c]) * amount * m);
        }
    });
}

// ---- spatial -----------------------------------------------------------------------------------------

// Box average of f x f blocks (partial blocks at the right/bottom edge average the pixels they contain).
void downsample(const FImage& src, FImage& dst, int f) {
    dst.reset((src.w + f - 1) / f, (src.h + f - 1) / f);
    parallelRows(0, dst.h, [&](int y) {
        float* d = dst.row(y);
        for (int x = 0; x < dst.w; ++x, d += 4) {
            float acc[4] = {0, 0, 0, 0};
            int n = 0;
            for (int yy = y * f; yy < std::min(src.h, (y + 1) * f); ++yy)
                for (int xx = x * f; xx < std::min(src.w, (x + 1) * f); ++xx) {
                    const float* s = src.row(yy) + static_cast<size_t>(xx) * 4;
                    for (int c = 0; c < 4; ++c) acc[c] += s[c];
                    ++n;
                }
            for (int c = 0; c < 4; ++c) d[c] = acc[c] / static_cast<float>(n);
        }
    });
}

// Bilinear upsampling by f with clamp-to-edge (pixel centres: small = (x + 0.5) / f - 0.5).
void upsample(const FImage& small, FImage& dst, int f) {
    parallelRows(0, dst.h, [&](int y) {
        float* d = dst.row(y);
        const float sy = (static_cast<float>(y) + 0.5f) / static_cast<float>(f) - 0.5f;
        const float fy0 = std::floor(sy);
        const float wy = sy - fy0;
        const int y0 = std::clamp(static_cast<int>(fy0), 0, small.h - 1), y1 = std::clamp(static_cast<int>(fy0) + 1, 0, small.h - 1);
        for (int x = 0; x < dst.w; ++x, d += 4) {
            const float sx = (static_cast<float>(x) + 0.5f) / static_cast<float>(f) - 0.5f;
            const float fx0 = std::floor(sx);
            const float wx = sx - fx0;
            const int x0 = std::clamp(static_cast<int>(fx0), 0, small.w - 1), x1 = std::clamp(static_cast<int>(fx0) + 1, 0, small.w - 1);
            const float* a = small.row(y0) + static_cast<size_t>(x0) * 4;
            const float* b = small.row(y0) + static_cast<size_t>(x1) * 4;
            const float* c = small.row(y1) + static_cast<size_t>(x0) * 4;
            const float* dd = small.row(y1) + static_cast<size_t>(x1) * 4;
            for (int k = 0; k < 4; ++k) {
                const float top = a[k] + (b[k] - a[k]) * wx, bottom = c[k] + (dd[k] - c[k]) * wx;
                d[k] = top + (bottom - top) * wy;
            }
        }
    });
}

// Separable convolution with symmetric weights, clamp-to-edge; horizontal then vertical.
void convolveSeparable(FImage& img, const std::vector<float>& w) {
    const int r = static_cast<int>(w.size()) - 1;
    FImage tmp;
    tmp.reset(img.w, img.h);
    parallelRows(0, img.h, [&](int y) {
        const float* s = img.row(y);
        float* d = tmp.row(y);
        for (int x = 0; x < img.w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int i = -r; i <= r; ++i) {
                const float k = w[static_cast<size_t>(std::abs(i))];
                const float* p = s + static_cast<size_t>(std::clamp(x + i, 0, img.w - 1)) * 4;
                for (int c = 0; c < 4; ++c) acc[c] += p[c] * k;
            }
            for (int c = 0; c < 4; ++c) d[static_cast<size_t>(x) * 4 + c] = acc[c];
        }
    });
    parallelRows(0, img.h, [&](int y) {
        float* d = img.row(y);
        for (int x = 0; x < img.w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int i = -r; i <= r; ++i) {
                const float k = w[static_cast<size_t>(std::abs(i))];
                const float* p = tmp.row(std::clamp(y + i, 0, img.h - 1)) + static_cast<size_t>(x) * 4;
                for (int c = 0; c < 4; ++c) acc[c] += p[c] * k;
            }
            for (int c = 0; c < 4; ++c) d[static_cast<size_t>(x) * 4 + c] = acc[c];
        }
    });
}

// Gaussian blur of a straight-alpha picture (filtered premultiplied).
void gaussian(FImage& img, float sigmaPx) {
    const BlurPlan plan = planGaussian(sigmaPx);
    if (plan.factor == 0) return;
    premultiply(img);
    if (plan.factor == 1) {
        convolveSeparable(img, plan.weights);
    } else {
        FImage small;
        downsample(img, small, plan.factor);
        convolveSeparable(small, plan.weights);
        upsample(small, img, plan.factor);
    }
    unpremultiply(img);
}

// sizes are px at 1080 lines of the picture height
float px1080(const FImage& img, float v) { return v * static_cast<float>(img.h) / 1080.0f; }

void runGaussian(FImage& img, const EffectInstance& e) { gaussian(img, px1080(img, e.value("radius")) / 3.0f); }

// unsharp mask on straight colour: c + amount (c - blur(c))
void runSharpen(FImage& img, const EffectInstance& e) {
    FImage blurred = img;
    gaussian(blurred, px1080(img, e.value("radius")));
    const float k = e.value("amount") / 100.0f;
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        const float* b = blurred.row(y);
        for (int x = 0; x < img.w; ++x, q += 4, b += 4)
            for (int c = 0; c < 3; ++c) q[c] = clamp01(q[c] + k * (q[c] - b[c]));
    });
}

// ---- E: effects library ---------------------------------------------------------------------------------

// Bilinear sample of a picture at pixel coordinates (pixel i's centre at i + 0.5), clamp-to-edge.
inline void sampleBilinear(const FImage& img, float x, float y, float out[4]) {
    const float sx = x - 0.5f, sy = y - 0.5f;
    const float fx0 = std::floor(sx), fy0 = std::floor(sy);
    const float wx = sx - fx0, wy = sy - fy0;
    const int x0 = std::clamp(static_cast<int>(fx0), 0, img.w - 1), x1 = std::clamp(static_cast<int>(fx0) + 1, 0, img.w - 1);
    const int y0 = std::clamp(static_cast<int>(fy0), 0, img.h - 1), y1 = std::clamp(static_cast<int>(fy0) + 1, 0, img.h - 1);
    const float* a = img.row(y0) + static_cast<size_t>(x0) * 4;
    const float* b = img.row(y0) + static_cast<size_t>(x1) * 4;
    const float* c = img.row(y1) + static_cast<size_t>(x0) * 4;
    const float* d = img.row(y1) + static_cast<size_t>(x1) * 4;
    for (int k = 0; k < 4; ++k) {
        const float top = a[k] + (b[k] - a[k]) * wx, bottom = c[k] + (d[k] - c[k]) * wx;
        out[k] = top + (bottom - top) * wy;
    }
}

constexpr float kPi = 3.14159265358979f;

// Remaps every pixel from a sample position (premultiplied, bilinear): `where(x, y, sx, sy)` with pixel centres.
template <class F>
void remap(FImage& img, F where) {
    FImage src = img;
    premultiply(src);
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        for (int x = 0; x < img.w; ++x, q += 4) {
            float sx, sy;
            where(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, sx, sy);
            sampleBilinear(src, sx, sy, q);
        }
    });
    unpremultiply(img);
}

void runDirectional(FImage& img, const EffectInstance& e) {
    const float len = px1080(img, e.value("length"));
    const float a = e.value("angle") * kPi / 180.0f;
    const float dx = std::cos(a), dy = -std::sin(a);
    const int n = std::clamp(static_cast<int>(std::ceil(len)) + 1, 2, 64);
    FImage src = img;
    premultiply(src);
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        for (int x = 0; x < img.w; ++x, q += 4) {
            float acc[4] = {0, 0, 0, 0}, s[4];
            for (int i = 0; i < n; ++i) {
                const float t = (static_cast<float>(i) / static_cast<float>(n - 1) - 0.5f) * len;
                sampleBilinear(src, static_cast<float>(x) + 0.5f + dx * t, static_cast<float>(y) + 0.5f + dy * t, s);
                for (int k = 0; k < 4; ++k) acc[k] += s[k];
            }
            for (int k = 0; k < 4; ++k) q[k] = acc[k] / static_cast<float>(n);
        }
    });
    unpremultiply(img);
}

void runGlow(FImage& img, const EffectInstance& e) {
    const float th = e.value("threshold") / 100.0f, k = e.value("intensity") / 100.0f;
    const float span = std::max(1e-3f, 1.0f - th);
    FImage bright = img;
    parallelRows(0, img.h, [&](int y) {
        float* q = bright.row(y);
        for (int x = 0; x < img.w; ++x, q += 4)
            for (int c = 0; c < 3; ++c) q[c] = std::max(0.0f, q[c] - th) / span;
    });
    gaussian(bright, px1080(img, e.value("radius")) / 3.0f);
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        const float* g = bright.row(y);
        for (int x = 0; x < img.w; ++x, q += 4, g += 4)
            for (int c = 0; c < 3; ++c) q[c] = 1.0f - (1.0f - q[c]) * (1.0f - clamp01(k * g[c]));
    });
}

int pixelBlock(const FImage& img, const EffectInstance& e) {
    return std::max(1, static_cast<int>(std::lround(px1080(img, e.value("size")))));
}

void runPixelate(FImage& img, const EffectInstance& e) {
    const int b = pixelBlock(img, e);
    if (b <= 1) return;
    const int bw = (img.w + b - 1) / b, bh = (img.h + b - 1) / b;
    parallelRows(0, bh, [&](int by) {
        for (int bx = 0; bx < bw; ++bx) {
            float acc[4] = {0, 0, 0, 0};
            int n = 0;
            const int y1 = std::min(img.h, (by + 1) * b), x1 = std::min(img.w, (bx + 1) * b);
            for (int yy = by * b; yy < y1; ++yy)
                for (int xx = bx * b; xx < x1; ++xx) {
                    const float* p = img.row(yy) + static_cast<size_t>(xx) * 4;
                    acc[0] += p[0] * p[3];
                    acc[1] += p[1] * p[3];
                    acc[2] += p[2] * p[3];
                    acc[3] += p[3];
                    ++n;
                }
            float v[4];
            for (int k = 0; k < 4; ++k) v[k] = acc[k] / static_cast<float>(n);
            if (v[3] > 1e-6f) {
                const float inv = 1.0f / v[3];
                v[0] = clamp01(v[0] * inv), v[1] = clamp01(v[1] * inv), v[2] = clamp01(v[2] * inv);
            } else {
                v[0] = v[1] = v[2] = 0.0f;
            }
            for (int yy = by * b; yy < y1; ++yy)
                for (int xx = bx * b; xx < x1; ++xx) std::memcpy(img.row(yy) + static_cast<size_t>(xx) * 4, v, sizeof v);
        }
    });
}

// grain value in [-1, 1) (sum of two uniforms) at cell (cx, cy) for channel k
inline float grainAt(uint32_t cx, uint32_t cy, uint32_t seed, uint32_t k) {
    return noise01(cx, cy, seed, 2 * k) + noise01(cx, cy, seed, 2 * k + 1) - 1.0f;
}

void runGrain(FImage& img, const EffectInstance& e) {
    const float amount = e.value("amount") / 100.0f;
    const float cell = std::max(1.0f, px1080(img, e.value("size")));
    const bool colour = e.value("color") >= 0.5f;
    const uint32_t seed = e.seed;
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        const float v = (static_cast<float>(y) + 0.5f) / cell - 0.5f;
        const float fv = std::floor(v), wv = v - fv;
        const uint32_t cy = static_cast<uint32_t>(static_cast<int>(fv) + 1);
        for (int x = 0; x < img.w; ++x, q += 4) {
            const float u = (static_cast<float>(x) + 0.5f) / cell - 0.5f;
            const float fu = std::floor(u), wu = u - fu;
            const uint32_t cx = static_cast<uint32_t>(static_cast<int>(fu) + 1);
            const float lum = luma(q[0], q[1], q[2]);
            const float w = amount * 0.35f * (0.4f + 2.4f * lum * (1.0f - lum));
            for (int c = 0; c < 3; ++c) {
                const uint32_t k = colour ? static_cast<uint32_t>(c) : 0U;
                const float g00 = grainAt(cx, cy, seed, k), g10 = grainAt(cx + 1, cy, seed, k);
                const float g01 = grainAt(cx, cy + 1, seed, k), g11 = grainAt(cx + 1, cy + 1, seed, k);
                const float top = g00 + (g10 - g00) * wu, bottom = g01 + (g11 - g01) * wu;
                q[c] = clamp01(q[c] + w * (top + (bottom - top) * wv));
            }
        }
    });
}

void runRgbSplit(FImage& img, const EffectInstance& e) {
    const float d = px1080(img, e.value("offset"));
    const float a = e.value("angle") * kPi / 180.0f;
    const float dx = std::cos(a) * d, dy = -std::sin(a) * d;
    FImage src = img;
    premultiply(src);
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        for (int x = 0; x < img.w; ++x, q += 4) {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            float r[4], g[4], b[4];
            sampleBilinear(src, px - dx, py - dy, r);
            sampleBilinear(src, px, py, g);
            sampleBilinear(src, px + dx, py + dy, b);
            q[0] = r[0];
            q[1] = g[1];
            q[2] = b[2];
            q[3] = std::max(r[3], std::max(g[3], b[3]));
        }
    });
    unpremultiply(img);
}

void runEdge(FImage& img, const EffectInstance& e) {
    const int k = std::max(1, static_cast<int>(std::lround(static_cast<float>(img.h) / 1080.0f)));
    const int mode = static_cast<int>(std::lround(e.value("mode")));
    const float s = e.value("strength") / 100.0f;
    std::vector<float> lum(static_cast<size_t>(img.w) * img.h);
    parallelRows(0, img.h, [&](int y) {
        const float* p = img.row(y);
        for (int x = 0; x < img.w; ++x, p += 4) lum[static_cast<size_t>(y) * img.w + x] = luma(p[0], p[1], p[2]);
    });
    auto L = [&](int x, int y) {
        return lum[static_cast<size_t>(std::clamp(y, 0, img.h - 1)) * img.w + std::clamp(x, 0, img.w - 1)];
    };
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        for (int x = 0; x < img.w; ++x, q += 4) {
            const float gx = (L(x + k, y - k) + 2.0f * L(x + k, y) + L(x + k, y + k)) - (L(x - k, y - k) + 2.0f * L(x - k, y) + L(x - k, y + k));
            const float gy = (L(x - k, y + k) + 2.0f * L(x, y + k) + L(x + k, y + k)) - (L(x - k, y - k) + 2.0f * L(x, y - k) + L(x + k, y - k));
            const float m = clamp01(std::sqrt(gx * gx + gy * gy) * 0.25f * s);
            if (mode == 1) q[0] = q[1] = q[2] = 1.0f - m;
            else if (mode == 2) {
                for (int c = 0; c < 3; ++c) q[c] = clamp01(q[c] * m * 2.0f);
            } else q[0] = q[1] = q[2] = m;
        }
    });
}

void runChromatic(FImage& img, const EffectInstance& e) {
    const float a = e.value("amount") / 100.0f * 0.02f;
    const float cx = 0.5f * static_cast<float>(img.w), cy = 0.5f * static_cast<float>(img.h);
    FImage src = img;
    premultiply(src);
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        for (int x = 0; x < img.w; ++x, q += 4) {
            const float px = static_cast<float>(x) + 0.5f - cx, py = static_cast<float>(y) + 0.5f - cy;
            float r[4], g[4], b[4];
            sampleBilinear(src, cx + px * (1.0f - a), cy + py * (1.0f - a), r);
            sampleBilinear(src, cx + px, cy + py, g);
            sampleBilinear(src, cx + px * (1.0f + a), cy + py * (1.0f + a), b);
            q[0] = r[0];
            q[1] = g[1];
            q[2] = b[2];
            q[3] = std::max(r[3], std::max(g[3], b[3]));
        }
    });
    unpremultiply(img);
}

void runWave(FImage& img, const EffectInstance& e) {
    const float amp = px1080(img, e.value("amplitude"));
    const float lambda = std::max(1.0f, px1080(img, e.value("wavelength")));
    const float phase = e.value("speed") * static_cast<float>(e.seconds);
    const bool vertical = e.value("direction") >= 0.5f;
    remap(img, [&](float x, float y, float& sx, float& sy) {
        const float t = 2.0f * kPi * ((vertical ? x : y) / lambda + phase);
        const float off = amp * std::sin(t);
        sx = vertical ? x : x + off;
        sy = vertical ? y + off : y;
    });
}

void runBulge(FImage& img, const EffectInstance& e) {
    const float h = static_cast<float>(img.h);
    const float R = std::max(1.0f, e.value("radius") / 100.0f * h);
    const float cx = 0.5f * static_cast<float>(img.w) + e.value("centerX") / 100.0f * h * 0.5f;
    const float cy = 0.5f * h + e.value("centerY") / 100.0f * h * 0.5f;
    const float ex = std::pow(2.0f, e.value("amount") / 100.0f);   // > 1 bulges, < 1 pinches
    remap(img, [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy, r = std::sqrt(dx * dx + dy * dy);
        sx = x;
        sy = y;
        if (r < R && r > 1e-4f) {
            const float k = std::pow(r / R, ex - 1.0f);   // source radius / radius
            sx = cx + dx * k;
            sy = cy + dy * k;
        }
    });
}

// Separable min (erode) or max (dilate) of channel 3 over a (2r+1)^2 square, clamp-to-edge.
void morphAlpha(FImage& img, int r, bool dilate) {
    if (r <= 0) return;
    FImage tmp = img;
    auto pick = [dilate](float a, float b) { return dilate ? std::max(a, b) : std::min(a, b); };
    parallelRows(0, img.h, [&](int y) {
        const float* s = img.row(y);
        float* d = tmp.row(y);
        for (int x = 0; x < img.w; ++x) {
            float v = s[static_cast<size_t>(x) * 4 + 3];
            for (int i = -r; i <= r; ++i) v = pick(v, s[static_cast<size_t>(std::clamp(x + i, 0, img.w - 1)) * 4 + 3]);
            d[static_cast<size_t>(x) * 4 + 3] = v;
        }
    });
    parallelRows(0, img.h, [&](int y) {
        float* d = img.row(y);
        for (int x = 0; x < img.w; ++x) {
            float v = tmp.row(y)[static_cast<size_t>(x) * 4 + 3];
            for (int i = -r; i <= r; ++i) v = pick(v, tmp.row(std::clamp(y + i, 0, img.h - 1))[static_cast<size_t>(x) * 4 + 3]);
            d[static_cast<size_t>(x) * 4 + 3] = v;
        }
    });
}

// Gaussian of every channel without premultiplying (for a matte stored in all four channels).
void blurPlain(FImage& img, float sigmaPx) {
    const BlurPlan plan = planGaussian(sigmaPx);
    if (plan.factor == 0) return;
    if (plan.factor == 1) {
        convolveSeparable(img, plan.weights);
    } else {
        FImage small;
        downsample(img, small, plan.factor);
        convolveSeparable(small, plan.weights);
        upsample(small, img, plan.factor);
    }
}

ChromaKeyParams keyParams(const EffectInstance& e) {
    const uint32_t rgb = static_cast<uint32_t>(std::lround(e.value("color")));
    return chromaKeyParams(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f,
                           e.value("tolerance") / 100.0f, e.value("pedestal") / 100.0f);
}

int keyThickness(const FImage& img, const EffectInstance& e) {
    return static_cast<int>(std::lround(e.value("thickness") * 0.5f * static_cast<float>(img.h) / 1080.0f));
}

float keyFeather(const FImage& img, const EffectInstance& e) { return px1080(img, e.value("feather") * 0.17f); }

void runChromaKey(FImage& img, const EffectInstance& e) {
    const ChromaKeyParams k = keyParams(e);
    const float spill = e.value("spill") / 100.0f;
    FImage matte;
    matte.reset(img.w, img.h);
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        float* m = matte.row(y);
        for (int x = 0; x < img.w; ++x, q += 4, m += 4) {
            float proj = 0;
            const float a = chromaMatte(q[0], q[1], q[2], k, proj);
            m[0] = m[1] = m[2] = m[3] = a;
            suppressSpill(q[0], q[1], q[2], k, proj, spill);
        }
    });
    const int t = keyThickness(img, e);
    morphAlpha(matte, std::abs(t), t > 0);
    blurPlain(matte, keyFeather(img, e));
    const bool view = e.value("alphaView") >= 0.5f;
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        const float* m = matte.row(y);
        for (int x = 0; x < img.w; ++x, q += 4, m += 4) {
            const float a = clamp01(m[3]) * q[3];
            if (view) {
                q[0] = q[1] = q[2] = a;
                q[3] = 1.0f;
            } else {
                q[3] = a;
            }
        }
    });
}

void runBw(FImage& img, const EffectInstance& e) {
    const float wr = e.value("red") / 100.0f, wg = e.value("green") / 100.0f, wb = e.value("blue") / 100.0f;
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        for (int x = 0; x < img.w; ++x, q += 4) q[0] = q[1] = q[2] = clamp01(wr * q[0] + wg * q[1] + wb * q[2]);
    });
}

void runSepia(FImage& img, const EffectInstance& e) {
    const uint32_t rgb = static_cast<uint32_t>(std::lround(e.value("tone")));
    const float t[3] = {((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f};
    const float ty = luma(t[0], t[1], t[2]), s = e.value("strength") / 100.0f;
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        for (int x = 0; x < img.w; ++x, q += 4) {
            const float l = luma(q[0], q[1], q[2]), w = 4.0f * l * (1.0f - l) * s;
            for (int c = 0; c < 3; ++c) q[c] = clamp01(l + (t[c] - ty) * w);
        }
    });
}

// ---- M11 effects (MeerCut designs); the GPU passes repeat each operation in the same order ------------------

constexpr float kTwoPi = 2.0f * kPi;

void colorOf(const EffectInstance& e, const char* pid, float out[3]) {
    const uint32_t rgb = static_cast<uint32_t>(std::lround(e.value(pid)));
    out[0] = ((rgb >> 16) & 255) / 255.0f;
    out[1] = ((rgb >> 8) & 255) / 255.0f;
    out[2] = (rgb & 255) / 255.0f;
}

// the centre of the swirl, zoom blur and spotlight (as the bulge's): offsets in % of half the picture height
void centerOf(const FImage& img, const EffectInstance& e, float& cx, float& cy) {
    const float h = static_cast<float>(img.h);
    cx = 0.5f * static_cast<float>(img.w) + e.value("centerX") / 100.0f * h * 0.5f;
    cy = 0.5f * h + e.value("centerY") / 100.0f * h * 0.5f;
}

void runMirror(FImage& img, const EffectInstance& e) {
    const int mode = static_cast<int>(std::lround(e.value("mode")));
    const float W = static_cast<float>(img.w), H = static_cast<float>(img.h);
    remap(img, [&](float x, float y, float& sx, float& sy) {
        sx = x;
        sy = y;
        if ((mode == 0 || mode == 4) && x > W * 0.5f) sx = W - x;
        if (mode == 1 && x < W * 0.5f) sx = W - x;
        if ((mode == 2 || mode == 4) && y > H * 0.5f) sy = H - y;
        if (mode == 3 && y < H * 0.5f) sy = H - y;
    });
}

void runKaleidoscope(FImage& img, const EffectInstance& e) {
    const int n = std::max(2, static_cast<int>(std::lround(e.value("segments"))));
    const float ang = e.value("angle") * kPi / 180.0f, zoom = std::max(0.05f, e.value("zoom") / 100.0f);
    const float sector = kTwoPi / static_cast<float>(n);
    const float cx = 0.5f * static_cast<float>(img.w), cy = 0.5f * static_cast<float>(img.h);
    remap(img, [&](float x, float y, float& sx, float& sy) {
        const float dx = (x - cx) / zoom, dy = (y - cy) / zoom;
        const float r = std::sqrt(dx * dx + dy * dy);
        float a = std::atan2(dy, dx) - ang;
        a -= sector * std::floor(a / sector);
        if (a > 0.5f * sector) a = sector - a;   // every other wedge mirrored: no seams
        a += ang;
        sx = cx + r * std::cos(a);
        sy = cy + r * std::sin(a);
    });
}

void runSwirl(FImage& img, const EffectInstance& e) {
    float cx, cy;
    centerOf(img, e, cx, cy);
    const float R = std::max(1.0f, e.value("radius") / 100.0f * static_cast<float>(img.h));
    const float ang = e.value("angle") * kPi / 180.0f;
    remap(img, [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy;
        const float r = std::sqrt(dx * dx + dy * dy);
        sx = x;
        sy = y;
        if (r < R) {
            const float t = 1.0f - r / R;
            float th = ang * t * t;
            th -= kTwoPi * std::floor(th / kTwoPi + 0.5f);   // within +-pi, where every GPU's sin / cos is exact
            const float cs = std::cos(th), sn = std::sin(th);
            sx = cx + dx * cs + dy * sn;
            sy = cy - dx * sn + dy * cs;
        }
    });
}

// radius r (in half diagonals) is sampled at r (1 - k/2 (1 - r^2)): the centre magnified, the corners in place
void runFisheye(FImage& img, const EffectInstance& e) {
    const float k = e.value("amount") / 100.0f;
    const float W = static_cast<float>(img.w), H = static_cast<float>(img.h);
    const float cx = 0.5f * W, cy = 0.5f * H, rn = 0.5f * std::sqrt(W * W + H * H);
    remap(img, [&](float x, float y, float& sx, float& sy) {
        const float dx = (x - cx) / rn, dy = (y - cy) / rn;
        const float f = 1.0f - 0.5f * k * (1.0f - (dx * dx + dy * dy));
        sx = cx + dx * f * rn;
        sy = cy + dy * f * rn;
    });
}

void runZoomBlur(FImage& img, const EffectInstance& e) {
    float cx, cy;
    centerOf(img, e, cx, cy);
    const float k = e.value("amount") / 100.0f * 0.5f;
    const int n = zoomBlurSamples(k, img.w, img.h);
    FImage src = img;
    premultiply(src);
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = 0; x < img.w; ++x, q += 4) {
            const float px = static_cast<float>(x) + 0.5f;
            float acc[4] = {0, 0, 0, 0}, s[4];
            for (int i = 0; i < n; ++i) {
                const float f = 1.0f - k * static_cast<float>(i) / static_cast<float>(n - 1);
                sampleBilinear(src, cx + (px - cx) * f, cy + (py - cy) * f, s);
                for (int c = 0; c < 4; ++c) acc[c] += s[c];
            }
            for (int c = 0; c < 4; ++c) q[c] = acc[c] / static_cast<float>(n);
        }
    });
    unpremultiply(img);
}

void runTiltShift(FImage& img, const EffectInstance& e) {
    FImage blurred = img;
    gaussian(blurred, px1080(img, e.value("blur")) / 3.0f);
    const float yc = 0.5f + e.value("position") / 200.0f, inner = e.value("width") / 200.0f;
    const float outer = inner + e.value("feather") / 200.0f + 1e-3f;
    const float H = static_cast<float>(img.h);
    parallelRows(0, img.h, [&](int y) {
        const float m = smoothstepf(inner, outer, std::fabs((static_cast<float>(y) + 0.5f) / H - yc));
        float* q = img.row(y);
        const float* b = blurred.row(y);
        for (int x = 0; x < img.w; ++x, q += 4, b += 4)
            for (int c = 0; c < 4; ++c) q[c] = q[c] + (b[c] - q[c]) * m;
    });
}

void runShake(FImage& img, const EffectInstance& e) {
    const ShakeFrame f = shakeFrame(e.seconds, px1080(img, e.value("amount")), e.value("speed"), e.value("rotation"),
                                    e.value("zoom"));
    const float cx = 0.5f * static_cast<float>(img.w), cy = 0.5f * static_cast<float>(img.h);
    remap(img, [&](float x, float y, float& sx, float& sy) {
        const float dx = (x - cx - f.ox) / f.zoom, dy = (y - cy - f.oy) / f.zoom;
        sx = cx + dx * f.cr + dy * f.sr;
        sy = cy - dx * f.sr + dy * f.cr;
    });
}

void runGlitch(FImage& img, const EffectInstance& e) {
    const float a = e.value("amount") / 100.0f, W = static_cast<float>(img.w), scale = static_cast<float>(img.h) / 1080.0f;
    const uint32_t slot = glitchSlot(e.seconds, e.value("speed"));
    const int bands = std::max(2, static_cast<int>(std::lround(e.value("bands"))));
    FImage src = img;
    premultiply(src);
    parallelRows(0, img.h, [&](int y) {
        const uint32_t band = static_cast<uint32_t>(y * bands) / static_cast<uint32_t>(img.h);
        float shift = 0.0f, split = a * 2.0f * scale;
        if (noise01(band, slot, kGlitchSeed, 0) < a * 0.6f) {
            shift = (noise01(band, slot, kGlitchSeed, 1) - 0.5f) * 2.0f * a * 0.2f * W;
            split = a * 10.0f * scale;
        }
        float* q = img.row(y);
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = 0; x < img.w; ++x, q += 4) {
            const float px = static_cast<float>(x) + 0.5f + shift;
            float r[4], g[4], b[4];
            sampleBilinear(src, px + split, py, r);
            sampleBilinear(src, px, py, g);
            sampleBilinear(src, px - split, py, b);
            q[0] = r[0];
            q[1] = g[1];
            q[2] = b[2];
            q[3] = std::max(r[3], std::max(g[3], b[3]));
        }
    });
    unpremultiply(img);
}

// the tracking band's strength on a row (0 outside the band)
float vhsBand(const VhsFrame& f, float py, float tracking) {
    return std::max(0.0f, 1.0f - std::fabs(py - f.bandY) / f.bandH) * tracking;
}

void runVhs(FImage& img, const EffectInstance& e) {
    const float bleed = e.value("amount") / 100.0f, lines = e.value("scanlines") / 100.0f;
    const float noise = e.value("noise") / 100.0f, tracking = e.value("tracking") / 100.0f;
    const float scale = static_cast<float>(img.h) / 1080.0f, shift = bleed * 5.0f * scale;
    const float period = std::max(2.0f, std::round(3.0f * scale));   // whole rows: the lines show at every size
    const VhsFrame f = vhsFrame(e.seconds, img.h);
    // 1: colour bleed and the band's row jitter (premultiplied resampling)
    FImage src = img;
    premultiply(src);
    parallelRows(0, img.h, [&](int y) {
        const float py = static_cast<float>(y) + 0.5f;
        const float band = vhsBand(f, py, tracking);
        const float jitter = band * 12.0f * scale * (noise01(static_cast<uint32_t>(y), f.slot, kVhsSeed, 2) - 0.5f) * 2.0f;
        float* q = img.row(y);
        for (int x = 0; x < img.w; ++x, q += 4) {
            const float px = static_cast<float>(x) + 0.5f + jitter;
            float r[4], g[4], b[4];
            sampleBilinear(src, px + shift, py, r);
            sampleBilinear(src, px, py, g);
            sampleBilinear(src, px - shift, py, b);
            q[0] = r[0];
            q[1] = g[1];
            q[2] = b[2];
            q[3] = std::max(r[3], std::max(g[3], b[3]));
        }
    });
    unpremultiply(img);
    // 2: washed-out colour, scan lines and noise (straight colour)
    const uint32_t seed = e.seed;
    parallelRows(0, img.h, [&](int y) {
        const float py = static_cast<float>(y) + 0.5f;
        const float band = vhsBand(f, py, tracking);
        const float phase = std::fmod(static_cast<float>(y), period) / period;
        const float scan = 1.0f - lines * 0.3f * (0.5f + 0.5f * std::cos(kTwoPi * phase));
        float* q = img.row(y);
        for (int x = 0; x < img.w; ++x, q += 4) {
            const float l = luma(q[0], q[1], q[2]);
            const float nz = (noise01(static_cast<uint32_t>(x), static_cast<uint32_t>(y), seed, 7) - 0.5f) *
                             (noise * 0.25f + band * 0.5f);
            for (int c = 0; c < 3; ++c) q[c] = clamp01((l + (q[c] - l) * (1.0f - 0.2f * bleed)) * scan + nz);
        }
    });
}

void runOldFilm(FImage& img, const EffectInstance& e) {
    const float age = e.value("amount") / 100.0f, dust = e.value("dust") / 100.0f, vig = e.value("vignette") / 100.0f;
    const float scale = static_cast<float>(img.h) / 1080.0f;
    const FilmFrame f = oldFilmFrame(e.seconds, img.w, scale, e.value("scratches") / 100.0f, e.value("flicker") / 100.0f);
    const int cell = std::max(1, static_cast<int>(std::lround(3.0f * scale)));
    const float W = static_cast<float>(img.w), H = static_cast<float>(img.h);
    const float cx = 0.5f * W, cy = 0.5f * H, rn = 0.5f * std::sqrt(W * W + H * H);
    const uint32_t dustSeed = f.slot * 7919U + kFilmSeed;
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = 0; x < img.w; ++x, q += 4) {
            const float px = static_cast<float>(x) + 0.5f;
            const float l = luma(q[0], q[1], q[2]);
            const float sep[3] = {l * 1.07f + 0.03f, l * 0.89f + 0.02f, l * 0.70f};
            float s = 0.0f;
            for (int i = 0; i < 4; ++i) s += f.o[i] * std::max(0.0f, 1.0f - std::fabs(px - f.x[i]) / f.w[i]);
            const bool speck = noise01(static_cast<uint32_t>(x / cell), static_cast<uint32_t>(y / cell), dustSeed, 8) <
                               dust * 0.004f;
            const float dx = px - cx, dy = py - cy;
            const float r = std::sqrt(dx * dx + dy * dy) / rn;
            const float v = 1.0f - vig * 0.7f * smoothstepf(0.45f, 1.0f, r);
            for (int c = 0; c < 3; ++c) {
                float o = q[c] + (sep[c] - q[c]) * (age * 0.85f);
                o = 0.06f * age + o * (1.0f - 0.12f * age);
                o *= f.flicker;
                o += (0.95f - o) * std::min(1.0f, s);
                if (speck) o *= 0.25f;
                o *= v;
                q[c] = clamp01(o);
            }
        }
    });
}

void runLightLeak(FImage& img, const EffectInstance& e) {
    const LeakFrame f = lightLeakFrame(e.seconds, e.value("speed"), e.value("size"), img.w, img.h);
    const float k = e.value("amount") / 100.0f * f.pulse;
    float col[3];
    colorOf(e, "color", col);
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = 0; x < img.w; ++x, q += 4) {
            const float px = static_cast<float>(x) + 0.5f;
            const float d1x = px - f.x1, d1y = py - f.y1, d2x = px - f.x2, d2y = py - f.y2;
            const float m = clamp01((std::exp(-(d1x * d1x + d1y * d1y) / (2.0f * f.s1 * f.s1)) +
                                     0.7f * std::exp(-(d2x * d2x + d2y * d2y) / (2.0f * f.s2 * f.s2))) *
                                    k);
            for (int c = 0; c < 3; ++c) q[c] = 1.0f - (1.0f - q[c]) * (1.0f - col[c] * m);
        }
    });
}

void runHalftone(FImage& img, const EffectInstance& e) {
    const float S = std::max(2.0f, px1080(img, e.value("size")));
    const float a = e.value("angle") * kPi / 180.0f, ca = std::cos(a), sa = std::sin(a);
    const int style = static_cast<int>(std::lround(e.value("style")));
    const float aa = 1.0f / S;   // one pixel in cell units
    const float paper[3] = {0.94f, 0.92f, 0.85f}, ink[3] = {0.12f, 0.12f, 0.14f};
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = 0; x < img.w; ++x, q += 4) {
            const float px = static_cast<float>(x) + 0.5f;
            const float u = (px * ca + py * sa) / S, v = (-px * sa + py * ca) / S;
            const float fu = u - std::floor(u) - 0.5f, fv = v - std::floor(v) - 0.5f;
            const float d = std::sqrt(fu * fu + fv * fv);
            // a dot's area is the tone's darkness (pi r^2 = 1 - luma while the dots do not touch)
            const float r = 0.5642f * std::sqrt(1.0f - clamp01(luma(q[0], q[1], q[2])));
            const float cov = clamp01((r - d) / aa + 0.5f) * std::min(1.0f, r / aa);   // no dot where r = 0
            for (int c = 0; c < 3; ++c)
                q[c] = style == 0 ? 1.0f - cov : style == 1 ? 1.0f + (q[c] - 1.0f) * cov : paper[c] + (ink[c] - paper[c]) * cov;
        }
    });
}

void runPosterize(FImage& img, const EffectInstance& e) {
    const float n = static_cast<float>(std::max(2L, std::lround(e.value("levels")))) - 1.0f;
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        for (int x = 0; x < img.w; ++x, q += 4)
            for (int c = 0; c < 3; ++c) q[c] = std::floor(q[c] * n + 0.5f) / n;
    });
}

void runEmboss(FImage& img, const EffectInstance& e) {
    const float k = e.value("strength") / 100.0f * 2.0f, a = e.value("angle") * kPi / 180.0f;
    const float step = std::max(1.0f, static_cast<float>(img.h) / 1080.0f);
    const float dx = std::cos(a) * step, dy = -std::sin(a) * step;
    const bool grey = e.value("style") < 0.5f;
    const FImage src = img;   // straight colour, as the GPU pass reads it
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = 0; x < img.w; ++x, q += 4) {
            const float px = static_cast<float>(x) + 0.5f;
            float s0[4], s1[4];
            sampleBilinear(src, px - dx, py - dy, s0);
            sampleBilinear(src, px + dx, py + dy, s1);
            const float g = (luma(s0[0], s0[1], s0[2]) - luma(s1[0], s1[1], s1[2])) * k;
            for (int c = 0; c < 3; ++c) q[c] = clamp01(grey ? 0.5f + g : q[c] + g);
        }
    });
}

void runInvert(FImage& img, const EffectInstance& e) {
    const float a = e.value("amount") / 100.0f;
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        for (int x = 0; x < img.w; ++x, q += 4)
            for (int c = 0; c < 3; ++c) q[c] = q[c] + (1.0f - 2.0f * q[c]) * a;
    });
}

// bars covering the picture outside the chosen aspect ratio (anti-aliased: the share of each pixel they cover)
void runLetterbox(FImage& img, const EffectInstance& e) {
    const float ra = letterboxRatio(static_cast<int>(std::lround(e.value("ratio"))));
    const float W = static_cast<float>(img.w), H = static_cast<float>(img.h), op = e.value("opacity") / 100.0f;
    float col[3];
    colorOf(e, "color", col);
    const float hb = ra >= W / H ? 0.5f * (H - W / ra) : 0.0f, wb = ra >= W / H ? 0.0f : 0.5f * (W - H * ra);
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        const float fy = static_cast<float>(y);
        for (int x = 0; x < img.w; ++x, q += 4) {
            const float fx = static_cast<float>(x);
            float cov = hb > 0.0f ? clamp01(hb - fy) + clamp01((fy + 1.0f) - (H - hb))
                                  : clamp01(wb - fx) + clamp01((fx + 1.0f) - (W - wb));
            cov = std::min(1.0f, cov) * op;
            for (int c = 0; c < 3; ++c) q[c] = q[c] + (col[c] - q[c]) * cov;
            q[3] = q[3] + (1.0f - q[3]) * cov;
        }
    });
}

void runSpotlight(FImage& img, const EffectInstance& e) {
    float cx, cy;
    centerOf(img, e, cx, cy);
    const float R = std::max(1.0f, e.value("size") / 100.0f * static_cast<float>(img.h));
    const float outer = R * (1.0f + e.value("feather") / 100.0f) + 1.0f, k = e.value("darkness") / 100.0f;
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        const float dy = static_cast<float>(y) + 0.5f - cy;
        for (int x = 0; x < img.w; ++x, q += 4) {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            const float m = 1.0f - k * smoothstepf(R, outer, std::sqrt(dx * dx + dy * dy));
            for (int c = 0; c < 3; ++c) q[c] = q[c] * m;
        }
    });
}

void runDuotone(FImage& img, const EffectInstance& e) {
    float sh[3], hi[3];
    colorOf(e, "shadows", sh);
    colorOf(e, "highlights", hi);
    const float s = e.value("strength") / 100.0f;
    parallelRows(0, img.h, [&](int y) {
        float* q = img.row(y);
        for (int x = 0; x < img.w; ++x, q += 4) {
            const float l = luma(q[0], q[1], q[2]);
            for (int c = 0; c < 3; ++c) {
                const float t = sh[c] + (hi[c] - sh[c]) * l;
                q[c] = q[c] + (t - q[c]) * s;
            }
        }
    });
}

using CpuFn = void (*)(FImage&, const EffectInstance&);

const std::map<std::string, CpuFn>& cpuImpls() {
    static const std::map<std::string, CpuFn> m = {
        {"fa.color.basic", runBasic},
        {"fa.fx.blur.gaussian", runGaussian},
        {"fa.color.hsl", runHsl},
        {"fa.color.curves", runCurves},
        {"fa.color.wheels", runWheels},
        {"fa.fx.vignette", runVignette},
        {"fa.color.lut", runLut},
        {"fa.filter.look", runLut},
        {"fa.fx.blur.directional", runDirectional},
        {"fa.fx.glow", runGlow},
        {"fa.fx.pixelate", runPixelate},
        {"fa.fx.grain", runGrain},
        {"fa.fx.rgbsplit", runRgbSplit},
        {"fa.fx.edge", runEdge},
        {"fa.fx.chromatic", runChromatic},
        {"fa.fx.wave", runWave},
        {"fa.fx.bulge", runBulge},
        {"fa.fx.chromakey", runChromaKey},
        {"fa.filter.bw", runBw},
        {"fa.filter.sepia", runSepia},
        {"fa.fx.sharpen", runSharpen},
        {"fa.fx.mirror", runMirror},
        {"fa.fx.kaleidoscope", runKaleidoscope},
        {"fa.fx.swirl", runSwirl},
        {"fa.fx.fisheye", runFisheye},
        {"fa.fx.blur.zoom", runZoomBlur},
        {"fa.fx.tiltshift", runTiltShift},
        {"fa.fx.shake", runShake},
        {"fa.fx.glitch", runGlitch},
        {"fa.fx.vhs", runVhs},
        {"fa.fx.oldfilm", runOldFilm},
        {"fa.fx.lightleak", runLightLeak},
        {"fa.fx.halftone", runHalftone},
        {"fa.fx.posterize", runPosterize},
        {"fa.fx.emboss", runEmboss},
        {"fa.fx.invert", runInvert},
        {"fa.fx.letterbox", runLetterbox},
        {"fa.fx.spotlight", runSpotlight},
        {"fa.filter.duotone", runDuotone},
    };
    return m;
}

// out = in + (fx - in) * mix * coverage
void mixWithMasks(const FImage& in, FImage& fx, const EffectInstance& e) {
    if (e.mix >= 1.0f && e.masks.empty()) return;
    const double pixelUnit = 1.0 / std::max(1, fx.h);
    parallelRows(0, fx.h, [&](int y) {
        const float* a = in.row(y);
        float* b = fx.row(y);
        const double qy = ((y + 0.5) - fx.h * 0.5) / fx.h;
        for (int x = 0; x < fx.w; ++x, a += 4, b += 4) {
            float k = e.mix;
            if (!e.masks.empty()) k *= maskCoverage(e.masks, ((x + 0.5) - fx.w * 0.5) / fx.h, qy, pixelUnit);
            for (int c = 0; c < 4; ++c) b[c] = a[c] + (b[c] - a[c]) * k;
        }
    });
}

}  // namespace

void applyAdjustmentCpu(Image& canvas, const LayerDesc& l) {
    const float opacity = static_cast<float>(std::clamp(l.opacity, 0.0, 1.0));
    if (!l.effects || opacity <= 0.0f || effectsAreIdentity(*l.effects)) return;
    Image fx;
    applyEffectsCpu(canvas, *l.effects, fx);
    const int cw = canvas.width, ch = canvas.height;
    const double pixelUnit = 1.0 / std::max(1, ch);
    parallelRows(0, ch, [&](int y) {
        uint8_t* d = canvas.row(y);
        const uint8_t* s = fx.row(y);
        const double qy = ((y + 0.5) - ch * 0.5) / ch;
        for (int x = 0; x < cw; ++x, d += 4, s += 4) {
            float k = opacity;
            if (!l.masks.empty()) k *= maskCoverage(l.masks, ((x + 0.5) - cw * 0.5) / ch, qy, pixelUnit);
            if (k >= 1.0f) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            } else if (k > 0.0f) {
                for (int c = 0; c < 3; ++c) {
                    const float a = d[c] / 255.0f, b = s[c] / 255.0f;
                    d[c] = static_cast<uint8_t>(std::floor(clamp01(a + (b - a) * k) * 255.0f + 0.5f));
                }
            }
            d[3] = 255;
        }
    });
}

bool effectHasCpuImplementation(const std::string& type) { return cpuImpls().count(type) != 0; }

void applyEffectsCpu(const Image& src, const std::vector<EffectInstance>& stack, Image& out) {
    FImage cur;
    toFloat(src, cur);
    FImage before;
    for (const EffectInstance& e : stack) {
        if (!e.problem.empty() || e.mix <= 0.0f) continue;
        if (e.def->isIdentity && e.def->isIdentity(e)) continue;
        const auto it = cpuImpls().find(e.def->id);
        if (it == cpuImpls().end()) throw std::runtime_error("effect '" + e.def->id + "' has no CPU implementation");
        const bool needBefore = e.mix < 1.0f || !e.masks.empty();
        if (needBefore) before = cur;
        it->second(cur, e);
        if (needBefore) mixWithMasks(before, cur, e);
    }
    toBytes(cur, out);
}

}  // namespace fa
