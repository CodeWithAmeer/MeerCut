// Direct3D 11 compositing backend (Windows).
//
// Two canvas textures (RGBA8) are used in ping-pong: every layer is one full-screen pass whose pixel
// shader reads the canvas so far (backdrop) and the layer's source texture and writes the next canvas.
// The shader mirrors the CPU reference (cpu_backend.cpp / compositing.h) operation for operation:
// same inverse placement at pixel centres, manual premultiplied bilinear with the crop rectangle,
// the same mask signed distances and erfc edge, wipe, the 14 blend formulas and the 8-bit rounding
// after each layer (the shader rounds explicitly, so the UNORM conversion is exact). Differences come
// only from float (GPU) vs double (CPU) coordinate arithmetic and stay within 1-2/255 (tested).
//
// Data flow per frame: source RGBA8 -> UpdateSubresource (upload), N passes on the GPU, one
// CopyResource into a staging texture and Map (readback) for the encoder / preview. Layers beyond the
// shader limits (more than 8 masks or 256 polygon points) are composited by the CPU reference on the
// read-back canvas and uploaded again, so results never silently differ.
#include "fa/render/backend.h"
#include "fa/render/effects.h"
#include "fa/render/lut.h"

#include "effect_math.h"

#if defined(FA_HAS_D3D11)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <list>
#include <mutex>
#include <stdexcept>
#include <string>

namespace fa {

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kMaxMasks = 8;
constexpr int kMaxPoints = 256;

const char* kCompositeHead = R"HLSL(
cbuffer Layer : register(b0) {
    float4 inv0;        // a b c -   canvas -> source
    float4 inv1;        // d e f -
    float4 crop;        // x0 y0 x1 y1 in source pixels
    int4   bbox;        // xs ys xe ye (canvas pixels, end exclusive)
    float4 canvas;      // cw ch sw sh
    float4 params;      // opacity brightness qScaleX qScaleY
    float4 params2;     // pixelUnit wipeProgress wipeSoftness flash
    float4 wipeDir;     // dirX dirY - -
    int4   info;        // blend maskCount plain wipeActive
    int4   info2;       // plainOpacity255 wipeShape wipeStripes -
    float4 maskA[8];    // cx cy w h            (layer heights)
    float4 maskB[8];    // rotation feather opacity cornerRadius
    int4   maskI[8];    // shape mode inverted pointOffset
    int4   maskN[8];    // pointCount - - -
    float4 points[256]; // xy (unit box)
};
Texture2D<float4> backdrop : register(t0);
Texture2D<float4> source   : register(t1);

)HLSL";

const char* kVertexShader = R"HLSL(
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

)HLSL";

// mask coverage (compositing.h), shared by the composite and the effect shaders
const char* kMaskFunctions = R"HLSL(
// erfc, Numerical Recipes "erfcc" (fractional error < 1.2e-7)
float erfcApprox(float x) {
    float z = abs(x);
    float t = 1.0 / (1.0 + 0.5 * z);
    float r = t * exp(-z * z - 1.26551223 + t * (1.00002368 + t * (0.37409196 + t * (0.09678418 +
              t * (-0.18628806 + t * (0.27886807 + t * (-1.13520398 + t * (1.48851587 +
              t * (-0.82215223 + t * 0.17087277)))))))));
    return x >= 0 ? r : 2.0 - r;
}

float sdBox(float2 p, float2 h, float r) {
    float2 q = abs(p) - h + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float sdEllipse(float2 p, float2 rr) {
    rr = max(rr, 1e-9);
    float k0 = length(p / rr);
    float k1 = length(p / (rr * rr));
    if (k1 < 1e-12) return -min(rr.x, rr.y);
    return k0 * (k0 - 1.0) / k1;
}

float sdPolygon(float2 p, int off, int n, float2 sc) {
    float d2 = 1e30;
    bool inside = false;
    int j = n - 1;
    for (int i = 0; i < n; ++i) {
        float2 a = points[off + i].xy * sc;
        float2 b = points[off + j].xy * sc;
        float2 e = b - a, w = p - a;
        float t = clamp(dot(w, e) / max(dot(e, e), 1e-18), 0.0, 1.0);
        float2 d = w - e * t;
        d2 = min(d2, dot(d, d));
        if (((a.y > p.y) != (b.y > p.y)) && (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)) inside = !inside;
        j = i;
    }
    float dd = sqrt(d2);
    return inside ? -dd : dd;
}

float maskDistance(int k, float2 q) {
    float r = -maskB[k].x * 3.14159265358979323846 / 180.0;
    float cs = cos(r), sn = sin(r);
    float2 v = q - maskA[k].xy;
    float2 p = float2(cs * v.x - sn * v.y, sn * v.x + cs * v.y);
    int shape = maskI[k].x;
    if (shape == 0) {
        float2 h = abs(maskA[k].zw) / 2;
        float rad = clamp(maskB[k].w, 0.0, 0.5) * 2 * min(h.x, h.y);
        return sdBox(p, h, rad);
    }
    if (shape == 1) return sdEllipse(p, abs(maskA[k].zw) / 2);
    if (maskN[k].x < 3) return 1e9;
    return sdPolygon(p, maskI[k].w, maskN[k].x, maskA[k].zw);
}

float maskCoverage(float2 q, float pixelUnit) {
    float acc = maskI[0].y == 0 ? 0.0 : 1.0;
    for (int k = 0; k < info.y; ++k) {
        float sigma = maskB[k].y > 0 ? maskB[k].y : 0.5 * pixelUnit;
        float d = maskDistance(k, q);
        float cov = 0.5 * erfcApprox(d / (sigma * 1.41421356237309505));
        if (maskI[k].z != 0) cov = 1.0 - cov;
        float v = cov * saturate(maskB[k].z);
        int mode = maskI[k].y;
        if (mode == 0) acc = min(1.0, acc + v);
        else if (mode == 1) acc = max(0.0, acc - v);
        else acc = min(acc, v);
    }
    return acc;
}

)HLSL";

const char* kCompositeBody = R"HLSL(
// wipeOrder / wipeFactor of compositing.cpp; info2.y = WipeShape, info2.z = stripes
float wipeOrder(float xn, float yn) {
    float aspect = canvas.x / canvas.y;
    float dx = (xn - 0.5) * aspect, dy = yn - 0.5;
    bool across = abs(wipeDir.x) >= abs(wipeDir.y);
    if (info2.y == 1) return sqrt(dx * dx + dy * dy) / sqrt(0.25 * aspect * aspect + 0.25);
    if (info2.y == 2) return (abs(dx) + abs(dy)) / (0.5 * aspect + 0.5);
    if (info2.y == 3) return 2.0 * abs((across ? xn : yn) - 0.5);
    if (info2.y == 4) {
        float a = atan2(dx, -dy);
        if (a < 0) a += 6.28318530717959;
        return a / 6.28318530717959;
    }
    if (info2.y == 5) {
        float f = (across ? xn : yn) * (float)max(1, info2.z);
        float t = f - floor(f);
        return (across ? wipeDir.x : wipeDir.y) < 0 ? 1.0 - t : t;
    }
    return 0.5 + (xn - 0.5) * wipeDir.x + (yn - 0.5) * wipeDir.y;
}

float wipeFactor(float xn, float yn) {
    float t = wipeOrder(xn, yn);
    float soft = max(0.0, params2.z);
    if (soft <= 1e-9) return t < params2.y ? 1.0 : 0.0;
    float edge = params2.y * (1.0 + soft);
    return saturate((edge - t) / soft);
}

float blendChannel(int m, float b, float s) {
    switch (m) {
        case 1: return b * s;
        case 2: return b + s - b * s;
        case 3: return b <= 0.5 ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
        case 4: return min(b, s);
        case 5: return max(b, s);
        case 6: return b <= 0 ? 0.0 : (s >= 1 ? 1.0 : min(1.0, b / (1 - s)));
        case 7: return b >= 1 ? 1.0 : (s <= 0 ? 0.0 : 1 - min(1.0, (1 - b) / s));
        case 8: return s <= 0.5 ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
        case 9: return s <= 0.5 ? 2 * b * s + b * b * (1 - 2 * s) : 2 * b * (1 - s) + sqrt(b) * (2 * s - 1);
        case 10: return abs(b - s);
        case 11: return b + s - 2 * b * s;
        case 12: return min(1.0, b + s);
        case 13: return max(0.0, b + s - 1);
        default: return s;
    }
}

float4 PSMain(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    float4 dst = backdrop.Load(int3(p, 0));
    if (info.z != 0) {   // plain layer: integer "over" exactly like the CPU fast path
        uint4 s8 = (uint4)(source.Load(int3(p, 0)) * 255.0 + 0.5);
        uint3 d8 = (uint3)(dst.rgb * 255.0 + 0.5);
        uint a = (s8.a * (uint)info2.x + 127) / 255;
        uint3 o = d8;
        if (a == 255) o = s8.rgb;
        else if (a > 0) o = (s8.rgb * a + d8 * (255 - a) + 127) / 255;
        return float4(o / 255.0, 1.0);
    }
    if (p.x < bbox.x || p.y < bbox.y || p.x >= bbox.z || p.y >= bbox.w) return dst;
    float px = pos.x, py = pos.y;   // pixel centre (x + 0.5, y + 0.5)
    float Sx = inv0.x * px + inv0.y * py + inv0.z;
    float Sy = inv1.x * px + inv1.y * py + inv1.z;
    float u = Sx - 0.5, v = Sy - 0.5;
    float fu = floor(u), fv = floor(v);
    int iu = (int)fu, iv = (int)fv;
    float wu = u - fu, wv = v - fv;
    int sw = (int)canvas.z, sh = (int)canvas.w;
    float4 acc = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        int tx = iu + (k & 1), ty = iv + (k >> 1);
        float w = ((k & 1) ? wu : 1 - wu) * ((k >> 1) ? wv : 1 - wv);
        if (w <= 0 || tx < 0 || ty < 0 || tx >= sw || ty >= sh) continue;
        if (tx + 0.5 < crop.x || tx + 0.5 > crop.z || ty + 0.5 < crop.y || ty + 0.5 > crop.w) continue;
        float4 t = source.Load(int3(tx, ty, 0));
        float a = t.a * w;
        acc.rgb += t.rgb * a;
        acc.a += a;
    }
    if (acc.a <= 1e-6) return dst;
    float alpha = acc.a * params.x;
    if (info.y > 0) alpha *= maskCoverage(float2((Sx - canvas.z * 0.5) * params.z, (Sy - canvas.w * 0.5) * params.w), params2.x);
    if (info.w != 0) alpha *= wipeFactor(px / canvas.x, py / canvas.y);
    if (alpha <= 1e-6) return dst;
    float3 s = min(1.0, acc.rgb / acc.a) * params.y;
    s += (1.0 - s) * params2.w;
    float3 r = float3(blendChannel(info.x, dst.r, s.r), blendChannel(info.x, dst.g, s.g), blendChannel(info.x, dst.b, s.b));
    float3 o = dst.rgb * (1 - alpha) + r * alpha;
    return float4(floor(saturate(o) * 255.0 + 0.5) / 255.0, 1.0);
}
)HLSL";

const std::string kShader = std::string(kCompositeHead) + kVertexShader + kMaskFunctions + kCompositeBody;


struct alignas(16) Constants {
    float inv0[4];
    float inv1[4];
    float crop[4];
    int32_t bbox[4];
    float canvas[4];
    float params[4];
    float params2[4];
    float wipeDir[4];
    int32_t info[4];
    int32_t info2[4];
    float maskA[kMaxMasks][4];
    float maskB[kMaxMasks][4];
    int32_t maskI[kMaxMasks][4];
    int32_t maskN[kMaxMasks][4];
    float points[kMaxPoints][4];
};
static_assert(sizeof(Constants) % 16 == 0);

// ---- effect shaders (M4): one pixel shader per pass, mirroring effects_cpu.cpp / effect_math.h -------------
const char* kFxHead = R"HLSL(
cbuffer Fx : register(b0) {
    float4 size;        // target w h, input w h
    float4 p0;
    float4 p1;
    float4 p2;
    float4 p3;
    int4   info;        // x: factor / radius   y: mask count   z, w: -
    float4 mixp;        // x: mix or opacity   y: pixelUnit (layer heights)   z, w: -
    float4 pv[8];       // more parameters (HSL bands, wheels)
    float4 weights[16]; // blur weights 0..63
    float4 maskA[8];
    float4 maskB[8];
    int4   maskI[8];
    int4   maskN[8];
    float4 points[256];
};
Texture2D<float4> tIn  : register(t0);
Texture2D<float4> tAux : register(t1);
)HLSL";

const char* kFxBody = R"HLSL(
float clamp01(float v) { return min(1.0, max(0.0, v)); }

float4 PSToFloat(float4 pos : SV_Position) : SV_Target { return tIn.Load(int3(int2(pos.xy), 0)); }

float4 PSToBytes(float4 pos : SV_Position) : SV_Target {
    float4 c = tIn.Load(int3(int2(pos.xy), 0));
    return floor(saturate(c) * 255.0 + 0.5) / 255.0;
}

float4 PSPremul(float4 pos : SV_Position) : SV_Target {
    float4 c = tIn.Load(int3(int2(pos.xy), 0));
    return float4(c.rgb * c.a, c.a);
}

float4 PSUnpremul(float4 pos : SV_Position) : SV_Target {
    float4 c = tIn.Load(int3(int2(pos.xy), 0));
    if (c.a > 1e-6) {
        float k = 1.0 / c.a;
        return float4(clamp01(c.r * k), clamp01(c.g * k), clamp01(c.b * k), c.a);
    }
    return float4(0, 0, 0, c.a);
}

// ---- Basic colour (effect_math.h basicTone / basicColor)
float contrastCurve(float x, float k) {
    x = clamp01(x);
    if (x <= 0.0 || x >= 1.0) return x;
    float a = pow(x, k), b = pow(1.0 - x, k);
    return a / (a + b);
}

// p0 = temperature tint exposure brightness, p1 = contrast highlights shadows whites, p2 = blacks gamma saturation vibrance
float basicTone(float c) {
    c = clamp01(c);
    float e = p0.z;
    if (e < 0) c *= pow(0.44, -2.0 * e);
    else if (e > 0) c = min(1.0, pow(1.075, 2.0 * e) * (1.0 - pow(1.0 - c, pow(2.30, 2.0 * e))));
    float br = p0.w;
    if (br > 0) c *= 1.0 + br;
    else if (br < 0) c += 0.31373 * br;
    c = clamp01(c);
    if (p1.x != 0) c = contrastCurve(c, p1.x > 0 ? pow(1.204, 2.0 * p1.x) : pow(0.830, -2.0 * p1.x));
    if (p1.y != 0) c += p1.y * c * c * (1.0 - c);
    if (p1.z != 0) c += p1.z * c * (1.0 - c) * (1.0 - c);
    if (p1.w != 0) c += 0.3 * p1.w * c * c * c;
    if (p2.x != 0) c += 0.3 * p2.x * (1.0 - c) * (1.0 - c) * (1.0 - c);
    c = clamp01(c);
    if (p2.y != 0) c = pow(c, pow(2.0, -p2.y));
    return c;
}

float4 PSBasic(float4 pos : SV_Position) : SV_Target {
    float4 c = tIn.Load(int3(int2(pos.xy), 0));
    float r = c.r, g = c.g, b = c.b;
    if (p0.x != 0 || p0.y != 0) {
        r *= (1.0 + 0.242 * p0.x) * (1.0 + 0.242 * p0.y);
        g *= (1.0 - 0.016 * p0.x) * (1.0 - 0.216 * p0.y);
        b *= (1.0 - 0.262 * p0.x) * (1.0 + 0.242 * p0.y);
    }
    r = basicTone(r);
    g = basicTone(g);
    b = basicTone(b);
    if (p2.z != 0 || p2.w != 0) {
        float L = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        float sat = max(r, max(g, b)) - min(r, min(g, b));
        float k = (1.0 + p2.z) * max(0.0, 1.0 + p2.w * (1.0 - sat) * (1.0 - sat));
        r = clamp01(L + (r - L) * k);
        g = clamp01(L + (g - L) * k);
        b = clamp01(L + (b - L) * k);
    }
    return float4(r, g, b, c.a);
}

// ---- spatial helpers (effects_cpu.cpp downsample / convolveSeparable / upsample)
float weightAt(int i) { return weights[i >> 2][i & 3]; }

float4 PSDown(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    int f = info.x;
    int sw = (int)size.z, sh = (int)size.w;
    float4 acc = 0;
    int n = 0;
    for (int yy = p.y * f; yy < min(sh, (p.y + 1) * f); ++yy)
        for (int xx = p.x * f; xx < min(sw, (p.x + 1) * f); ++xx) {
            acc += tIn.Load(int3(xx, yy, 0));
            ++n;
        }
    return acc / (float)n;
}

float4 PSBlurH(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    int r = info.x, w = (int)size.z;
    float4 acc = 0;
    for (int i = -r; i <= r; ++i) acc += tIn.Load(int3(clamp(p.x + i, 0, w - 1), p.y, 0)) * weightAt(abs(i));
    return acc;
}

float4 PSBlurV(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    int r = info.x, h = (int)size.w;
    float4 acc = 0;
    for (int i = -r; i <= r; ++i) acc += tIn.Load(int3(p.x, clamp(p.y + i, 0, h - 1), 0)) * weightAt(abs(i));
    return acc;
}

float4 PSUp(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    float f = (float)info.x;
    int sw = (int)size.z, sh = (int)size.w;
    float sy = ((float)p.y + 0.5) / f - 0.5, sx = ((float)p.x + 0.5) / f - 0.5;
    float fy0 = floor(sy), fx0 = floor(sx);
    float wy = sy - fy0, wx = sx - fx0;
    int y0 = clamp((int)fy0, 0, sh - 1), y1 = clamp((int)fy0 + 1, 0, sh - 1);
    int x0 = clamp((int)fx0, 0, sw - 1), x1 = clamp((int)fx0 + 1, 0, sw - 1);
    float4 a = tIn.Load(int3(x0, y0, 0)), b = tIn.Load(int3(x1, y0, 0));
    float4 c = tIn.Load(int3(x0, y1, 0)), d = tIn.Load(int3(x1, y1, 0));
    float4 top = a + (b - a) * wx, bottom = c + (d - c) * wx;
    return top + (bottom - top) * wy;
}

// ---- strength and effect masks: out = before + (fx - before) * mix * coverage   (tIn = fx, tAux = before)
float4 PSMix(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    float4 fx = tIn.Load(int3(p, 0)), b = tAux.Load(int3(p, 0));
    float k = mixp.x;
    if (info.y > 0) {
        float2 q = float2((pos.x - size.x * 0.5) / size.y, (pos.y - size.y * 0.5) / size.y);
        k *= maskCoverage(q, mixp.y);
    }
    return b + (fx - b) * k;
}

// ---- adjustment layer: canvas + (fx(canvas) - canvas) * opacity * masks, 8-bit (tIn = fx bytes, tAux = canvas)
float4 PSAdjustMix(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    float4 s = tIn.Load(int3(p, 0)), d = tAux.Load(int3(p, 0));
    float k = mixp.x;
    if (info.y > 0) {
        float2 q = float2((pos.x - size.x * 0.5) / size.y, (pos.y - size.y * 0.5) / size.y);
        k *= maskCoverage(q, mixp.y);
    }
    if (k >= 1.0) return float4(s.rgb, 1.0);
    if (k <= 0.0) return float4(d.rgb, 1.0);
    return float4(floor(saturate(d.rgb + (s.rgb - d.rgb) * k) * 255.0 + 0.5) / 255.0, 1.0);
}

// ---- HSV, HSL bands, wheels, curves, vignette, sharpen (effect_math.h)
void rgbToHsv(float r, float g, float b, out float h, out float s, out float v) {
    float mx = max(r, max(g, b)), mn = min(r, min(g, b)), d = mx - mn;
    v = mx;
    s = mx > 0.0 ? d / mx : 0.0;
    h = 0.0;
    if (d <= 0.0) return;
    if (mx == r) h = 60.0 * ((g - b) / d);
    else if (mx == g) h = 60.0 * ((b - r) / d + 2.0);
    else h = 60.0 * ((r - g) / d + 4.0);
    if (h < 0.0) h += 360.0;
}

float3 hsvToRgb(float h, float s, float v) {
    h = h - 360.0 * floor(h / 360.0);
    float c = v * s, hp = h / 60.0;
    float x = c * (1.0 - abs(hp - 2.0 * floor(hp / 2.0) - 1.0));
    float3 q = float3(0, 0, 0);
    if (hp < 1) q = float3(c, x, 0);
    else if (hp < 2) q = float3(x, c, 0);
    else if (hp < 3) q = float3(0, c, x);
    else if (hp < 4) q = float3(0, x, c);
    else if (hp < 5) q = float3(x, 0, c);
    else q = float3(c, 0, x);
    return q + (v - c);
}

float smoothstepf(float a, float b, float x) {
    float t = clamp01((x - a) / (b - a));
    return t * t * (3.0 - 2.0 * t);
}

// HSL: mirrors fxmath::hslAdjust (effect_math.h) operation by operation.
static const float kHslC[8] = {350, 30, 60, 120, 180, 231, 275, 310};
static const float kHslGain[8] = {31, 36, 36, 41, 36, 36, 36, 36};
float pvAt(int i) { return pv[i >> 2][i & 3]; }

float hslRangeWeight(int i, float h) {
    float c = kHslC[i];
    float d = h - c;
    d -= 360.0 * floor((d + 180.0) / 360.0);
    int j = d >= 0.0 ? (i + 1) % 8 : (i + 7) % 8;
    float nb = abs(kHslC[j] - c);
    if (nb > 180.0) nb = 360.0 - nb;
    float zero = min(39.5, nb), plateau = min(20.0, 0.5 * nb);
    float a = abs(d);
    return a <= plateau ? 1.0 : (a >= zero ? 0.0 : (zero - a) / (zero - plateau));
}

float4 PSHsl(float4 pos : SV_Position) : SV_Target {
    float4 c = tIn.Load(int3(int2(pos.xy), 0));
    float h, s, v;
    rgbToHsv(c.r, c.g, c.b, h, s, v);
    if (s <= 0.0) return c;
    float dh = 0, sf = 0, dl = 0;
    [loop] for (int i = 0; i < 8; ++i) {
        float c0 = kHslC[i];
        float c1 = kHslC[(i + 1) % 8];
        if (c1 <= c0) c1 += 360.0;
        float hh = h;
        if (hh < c0) hh += 360.0;
        if (hh >= c0 && hh < c1) {
            float t = (hh - c0) / (c1 - c0);
            int j = (i + 1) % 8;
            dh = (1.0 - t) * pvAt(i) * kHslGain[i] + t * pvAt(j) * kHslGain[j];
            break;
        }
    }
    [loop] for (int k = 0; k < 8; ++k) {
        float w = hslRangeWeight(k, h);
        sf += w * pvAt(8 + k);
        dl += w * pvAt(16 + k);
    }
    float3 q = c.rgb;
    if (dh != 0.0) q = hsvToRgb(h + dh, s, v);
    if (sf != 0.0) {
        float mx = max(q.r, max(q.g, q.b)), mn = min(q.r, min(q.g, q.b)), L = 0.5 * (mx + mn);
        float f = max(0.0, 1.0 + sf);
        if (f > 1.0) {
            if (mx > L) f = min(f, (1.0 - L) / (mx - L));
            if (mn < L) f = min(f, L / (L - mn));
        }
        q = L + f * (q - L);
    }
    if (dl != 0.0) {
        float mx = max(q.r, max(q.g, q.b)), mn = min(q.r, min(q.g, q.b)), L = 0.5 * (mx + mn), C = mx - mn;
        float den = 1.0 - abs(2.0 * L - 1.0);
        if (C > 0.0 && den > 1e-6) {
            float S = min(1.0, C / den);
            float gL = smoothstepf(0.081, 0.558, L) * (1.0 - 0.681 * smoothstepf(0.582, 1.0, L));
            float L2 = clamp01(L + 0.25 * dl * S * gL);
            float k2 = (1.0 - abs(2.0 * L2 - 1.0)) * S / C;
            q = L2 + (q - L) * k2;
        }
    }
    return float4(saturate(q), c.a);
}

float4 PSWheels(float4 pos : SV_Position) : SV_Target {
    float4 c = tIn.Load(int3(int2(pos.xy), 0));
    float x = clamp01(0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b);
    float w[3] = {1.0 - smoothstepf(0.03, 0.41, x), pow(4.0 * x * (1.0 - x), 3.7), smoothstepf(0.57, 1.09, x)};
    float A[3] = {0.38, 0.364, 0.536};
    float B[3] = {0.206, 0.150, 0.194};
    float3 d = 0;
    float lum = 0;
    [unroll] for (int k = 0; k < 3; ++k) {
        float4 q = pv[k];   // r g b lum
        d += w[k] * A[k] * q.xyz;
        lum += w[k] * A[k] * q.w;
    }
    float3 o = saturate(c.rgb + d);
    float3 s = float3(d.x != 0 ? (o.x - c.r) / d.x : 1.0, d.y != 0 ? (o.y - c.g) / d.y : 1.0, d.z != 0 ? (o.z - c.b) / d.z : 1.0);
    float3 xt = 0;
    [unroll] for (int j = 0; j < 3; ++j) {
        float3 rv = B[j] * w[j] * A[j] * pv[j].xyz * s;   // realised offsets of this wheel
        xt += float3(rv.y + rv.z, rv.x + rv.z, rv.x + rv.y);
    }
    return float4(saturate(o - xt + lum), c.a);
}

// table texture tAux: row 0 = tone curves (master, red, green, blue), row 1 = hue curves (offset, saturation factor)
float lookupT(float v, int row, int ch) {
    float pos = clamp01(v) * 1023.0;
    int i = min(1022, (int)pos);
    float f = pos - (float)i;
    float a = tAux.Load(int3(i, row, 0))[ch], b = tAux.Load(int3(i + 1, row, 0))[ch];
    return a + (b - a) * f;
}

float4 PSCurves(float4 pos : SV_Position) : SV_Target {
    float4 c = tIn.Load(int3(int2(pos.xy), 0));
    float3 q = float3(lookupT(lookupT(c.r, 0, 1), 0, 0), lookupT(lookupT(c.g, 0, 2), 0, 0), lookupT(lookupT(c.b, 0, 3), 0, 0));
    float h, s, v;
    rgbToHsv(q.r, q.g, q.b, h, s, v);
    if (s > 0.0) {
        float hn = h / 360.0;
        float dh = lookupT(hn, 1, 0), sf = lookupT(hn, 1, 1);
        if (dh != 0.0 || sf != 1.0) q = hsvToRgb(h + 360.0 * dh, clamp01(s * sf), v);
    }
    return float4(q, c.a);
}

float4 PSVignette(float4 pos : SV_Position) : SV_Target {
    float4 c = tIn.Load(int3(int2(pos.xy), 0));
    float u = pos.x / size.x - 0.5, v = pos.y / size.y - 0.5;
    float aspect = size.x / max(1.0, size.y);
    float ax = 1.0 + (aspect - 1.0) * (p0.z + 1.0) * 0.5;
    float d = sqrt((2.0 * u * ax) * (2.0 * u * ax) + (2.0 * v) * (2.0 * v)) / sqrt(ax * ax + 1.0);
    float inner = p0.y * (1.0 - p0.w);
    float m = smoothstepf(inner, inner + p0.w + 1e-3, d);
    float a = p0.x;
    float3 o = a < 0 ? saturate(c.rgb * (1.0 + a * m)) : saturate(c.rgb + (1.0 - c.rgb) * a * m);
    return float4(o, c.a);
}

// unsharp mask: tIn = picture, tAux = blurred
float4 PSSharpen(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    float4 c = tIn.Load(int3(p, 0)), b = tAux.Load(int3(p, 0));
    return float4(saturate(c.rgb + p0.x * (c.rgb - b.rgb)), c.a);
}

)HLSL";

// (split: MSVC limits one string literal to ~16 KB)
const char* kFxBodyLut = R"HLSL(
// LUT: mirrors fa::applyLut (lut.cpp): optional 1-D table, then tetrahedral 3-D table, domain modes, strength.
// p0 = (strength, filmora domain, 3-D size, 1-D size), p1/p2 = 3-D domain min/max, p3 / pv[0] = 1-D domain min/max
Texture3D<float4> tLut3 : register(t2);
Texture2D<float4> tLut1 : register(t3);

float3 lutAt(int3 i) { return tLut3.Load(int4(i, 0)).rgb; }

float4 PSLut(float4 pos : SV_Position) : SV_Target {
    float4 c = tIn.Load(int3(int2(pos.xy), 0));
    float3 v = c.rgb;
    bool filmora = p0.y >= 0.5;
    int n1 = (int)p0.w;
    if (n1 > 0) {
        float3 dmin = p3.xyz, dmax = pv[0].xyz;
        float3 o;
        [unroll] for (int ch = 0; ch < 3; ++ch) {
            float x = v[ch];
            if (!filmora) x = (x - dmin[ch]) / (dmax[ch] - dmin[ch]);
            float q = clamp(x, 0.0, 1.0) * float(n1 - 1);
            int i = min(n1 - 2, (int)q);
            float f = q - float(i);
            float a = tLut1.Load(int3(i, 0, 0))[ch], b = tLut1.Load(int3(i + 1, 0, 0))[ch];
            o[ch] = a + (b - a) * f;
            if (filmora) o[ch] = dmin[ch] + o[ch] * (dmax[ch] - dmin[ch]);
        }
        v = o;
    }
    int n = (int)p0.z;
    if (n > 0) {
        float3 x = v;
        if (!filmora) x = (x - p1.xyz) / (p2.xyz - p1.xyz);
        float3 q = clamp(x, 0.0, 1.0) * float(n - 1);
        int3 i = min(int3(n - 2, n - 2, n - 2), (int3)q);
        float3 f = q - float3(i);
        float fr = f.x, fg = f.y, fb = f.z;
        float3 c000 = lutAt(i), c111 = lutAt(i + int3(1, 1, 1));
        float3 cA, cB;
        float w0, wA, wB, w1;
        if (fr > fg) {
            if (fg > fb) { cA = lutAt(i + int3(1, 0, 0)); cB = lutAt(i + int3(1, 1, 0)); w0 = 1 - fr; wA = fr - fg; wB = fg - fb; w1 = fb; }
            else if (fr > fb) { cA = lutAt(i + int3(1, 0, 0)); cB = lutAt(i + int3(1, 0, 1)); w0 = 1 - fr; wA = fr - fb; wB = fb - fg; w1 = fg; }
            else { cA = lutAt(i + int3(0, 0, 1)); cB = lutAt(i + int3(1, 0, 1)); w0 = 1 - fb; wA = fb - fr; wB = fr - fg; w1 = fg; }
        } else {
            if (fb > fg) { cA = lutAt(i + int3(0, 0, 1)); cB = lutAt(i + int3(0, 1, 1)); w0 = 1 - fb; wA = fb - fg; wB = fg - fr; w1 = fr; }
            else if (fb > fr) { cA = lutAt(i + int3(0, 1, 0)); cB = lutAt(i + int3(0, 1, 1)); w0 = 1 - fg; wA = fg - fb; wB = fb - fr; w1 = fr; }
            else { cA = lutAt(i + int3(0, 1, 0)); cB = lutAt(i + int3(1, 1, 0)); w0 = 1 - fg; wA = fg - fr; wB = fr - fb; w1 = fb; }
        }
        v = w0 * c000 + wA * cA + wB * cB + w1 * c111;
        if (filmora) v = p1.xyz + v * (p2.xyz - p1.xyz);
    }
    v = saturate(v);
    return float4(c.rgb + (v - c.rgb) * p0.x, c.a);
}

)HLSL";

const char* kFxBodyLibrary = R"HLSL(
// ---- M4 checkpoint E: effects library (mirrors effects_cpu.cpp operation by operation) ----------------------
static const float kPiF = 3.14159265358979;

// bilinear sample of tIn at pixel coordinates (centre of pixel i at i + 0.5), clamp-to-edge
float4 sampleLin(float2 p) {
    float2 sp = p - 0.5;
    float2 f0 = floor(sp);
    float2 wv = sp - f0;
    int2 mx = int2((int)size.z - 1, (int)size.w - 1);
    int2 i0 = clamp((int2)f0, int2(0, 0), mx), i1 = clamp((int2)f0 + int2(1, 1), int2(0, 0), mx);
    float4 a = tIn.Load(int3(i0.x, i0.y, 0)), b = tIn.Load(int3(i1.x, i0.y, 0));
    float4 c = tIn.Load(int3(i0.x, i1.y, 0)), d = tIn.Load(int3(i1.x, i1.y, 0));
    float4 top = a + (b - a) * wv.x, bottom = c + (d - c) * wv.x;
    return top + (bottom - top) * wv.y;
}
float lumaF(float3 c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }

// p0 = (dx, dy, length), info.x = samples (premultiplied in and out)
float4 PSDirBlur(float4 pos : SV_Position) : SV_Target {
    float4 acc = 0;
    int n = info.x;
    [loop] for (int i = 0; i < n; ++i) {
        float t = ((float)i / (float)(n - 1) - 0.5) * p0.z;
        acc += sampleLin(float2(pos.x + p0.x * t, pos.y + p0.y * t));
    }
    return acc / (float)n;
}

// p0 = (threshold, span, intensity)
float4 PSGlowBright(float4 pos : SV_Position) : SV_Target {
    float4 c = tIn.Load(int3(int2(pos.xy), 0));
    return float4(max(0.0, c.r - p0.x) / p0.y, max(0.0, c.g - p0.x) / p0.y, max(0.0, c.b - p0.x) / p0.y, c.a);
}
float4 PSGlowCombine(float4 pos : SV_Position) : SV_Target {
    float4 c = tIn.Load(int3(int2(pos.xy), 0));
    float4 g = tAux.Load(int3(int2(pos.xy), 0));
    float3 o;
    o.r = 1.0 - (1.0 - c.r) * (1.0 - clamp01(p0.z * g.r));
    o.g = 1.0 - (1.0 - c.g) * (1.0 - clamp01(p0.z * g.g));
    o.b = 1.0 - (1.0 - c.b) * (1.0 - clamp01(p0.z * g.b));
    return float4(o, c.a);
}

// info.x = block size (straight in, straight out)
float4 PSBlockAvg(float4 pos : SV_Position) : SV_Target {
    int b = info.x;
    int bx = (int)pos.x, by = (int)pos.y;
    int x1 = min((int)size.z, (bx + 1) * b), y1 = min((int)size.w, (by + 1) * b);
    float4 acc = 0;
    int n = 0;
    [loop] for (int yy = by * b; yy < y1; ++yy) {
        [loop] for (int xx = bx * b; xx < x1; ++xx) {
            float4 q = tIn.Load(int3(xx, yy, 0));
            acc.r += q.r * q.a;
            acc.g += q.g * q.a;
            acc.b += q.b * q.a;
            acc.a += q.a;
            ++n;
        }
    }
    float4 v = acc / (float)n;
    if (v.a > 1e-6) {
        float inv = 1.0 / v.a;
        v.r = clamp01(v.r * inv);
        v.g = clamp01(v.g * inv);
        v.b = clamp01(v.b * inv);
    } else {
        v.rgb = 0;
    }
    return v;
}
float4 PSBlockUp(float4 pos : SV_Position) : SV_Target { return tIn.Load(int3((int)pos.x / info.x, (int)pos.y / info.x, 0)); }

uint hashU(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float noise01(uint x, uint y, uint seed, uint k) { return (float)hashU(x + hashU(y + hashU(seed + k * 0x9E3779B9u))) * (1.0 / 4294967296.0); }
float grainAt(uint cx, uint cy, uint seed, uint k) { return noise01(cx, cy, seed, 2 * k) + noise01(cx, cy, seed, 2 * k + 1) - 1.0; }

// p0 = (amount, cell, colour), info.y = seed
float4 PSGrain(float4 pos : SV_Position) : SV_Target {
    float4 q = tIn.Load(int3(int2(pos.xy), 0));
    uint seed = asuint(info.y);
    float v = pos.y / p0.y - 0.5;
    float fv = floor(v), wv = v - fv;
    uint cy = (uint)((int)fv + 1);
    float u = pos.x / p0.y - 0.5;
    float fu = floor(u), wu = u - fu;
    uint cx = (uint)((int)fu + 1);
    float lum = lumaF(q.rgb);
    float w = p0.x * 0.35 * (0.4 + 2.4 * lum * (1.0 - lum));
    [unroll] for (int c = 0; c < 3; ++c) {
        uint k = p0.z >= 0.5 ? (uint)c : 0u;
        float g00 = grainAt(cx, cy, seed, k), g10 = grainAt(cx + 1, cy, seed, k);
        float g01 = grainAt(cx, cy + 1, seed, k), g11 = grainAt(cx + 1, cy + 1, seed, k);
        float top = g00 + (g10 - g00) * wu, bottom = g01 + (g11 - g01) * wu;
        q[c] = clamp01(q[c] + w * (top + (bottom - top) * wv));
    }
    return q;
}

// p0 = (dx, dy) (premultiplied in and out)
float4 PSRgbSplit(float4 pos : SV_Position) : SV_Target {
    float4 r = sampleLin(pos.xy - p0.xy), g = sampleLin(pos.xy), b = sampleLin(pos.xy + p0.xy);
    return float4(r.r, g.g, b.b, max(r.a, max(g.a, b.a)));
}

// info.x = pixel step, info.y = mode, p0.x = strength (straight)
float lumAt(int x, int y) {
    int2 mx = int2((int)size.z - 1, (int)size.w - 1);
    return lumaF(tIn.Load(int3(clamp(x, 0, mx.x), clamp(y, 0, mx.y), 0)).rgb);
}
float4 PSEdge(float4 pos : SV_Position) : SV_Target {
    int k = info.x;
    int x = (int)pos.x, y = (int)pos.y;
    float4 q = tIn.Load(int3(x, y, 0));
    float gx = (lumAt(x + k, y - k) + 2.0 * lumAt(x + k, y) + lumAt(x + k, y + k)) - (lumAt(x - k, y - k) + 2.0 * lumAt(x - k, y) + lumAt(x - k, y + k));
    float gy = (lumAt(x - k, y + k) + 2.0 * lumAt(x, y + k) + lumAt(x + k, y + k)) - (lumAt(x - k, y - k) + 2.0 * lumAt(x, y - k) + lumAt(x + k, y - k));
    float m = clamp01(sqrt(gx * gx + gy * gy) * 0.25 * p0.x);
    if (info.y == 1) q.rgb = 1.0 - m;
    else if (info.y == 2) q.rgb = float3(clamp01(q.r * m * 2.0), clamp01(q.g * m * 2.0), clamp01(q.b * m * 2.0));
    else q.rgb = m;
    return q;
}

// p0 = (a, cx, cy) (premultiplied)
float4 PSChromatic(float4 pos : SV_Position) : SV_Target {
    float2 c = p0.yz;
    float2 d = pos.xy - c;
    float4 r = sampleLin(c + d * (1.0 - p0.x)), g = sampleLin(c + d), b = sampleLin(c + d * (1.0 + p0.x));
    return float4(r.r, g.g, b.b, max(r.a, max(g.a, b.a)));
}

// p0 = (amplitude, wavelength, phase, vertical) (premultiplied)
float4 PSWave(float4 pos : SV_Position) : SV_Target {
    bool vertical = p0.w >= 0.5;
    float t = 2.0 * kPiF * ((vertical ? pos.x : pos.y) / p0.y + p0.z);
    float off = p0.x * sin(t);
    return sampleLin(vertical ? float2(pos.x, pos.y + off) : float2(pos.x + off, pos.y));
}

// p0 = (radius, cx, cy, exponent) (premultiplied)
float4 PSBulge(float4 pos : SV_Position) : SV_Target {
    float2 d = pos.xy - p0.yz;
    float r = sqrt(d.x * d.x + d.y * d.y);
    float2 sp = pos.xy;
    if (r < p0.x && r > 1e-4) {
        float k = pow(r / p0.x, p0.w - 1.0);
        sp = p0.yz + d * k;
    }
    return sampleLin(sp);
}

// chroma key: pv[0] = (kb, kr, k2, ky), pv[1] = (s, o, pedestal, spill)
float keyMatte(float3 c, out float proj) {
    float y = lumaF(c);
    float cb = (c.b - y) / 1.8556, cr = (c.r - y) / 1.5748;
    proj = (cb * pv[0].x + cr * pv[0].y) / pv[0].z;
    float perp = abs(cb * pv[0].y - cr * pv[0].x) / pv[0].z;
    float x = 1.0 - proj + 0.2 * perp;
    float cut = min(0.95, 0.15 + pv[1].z * (1.33 + 0.34 * max(0.0, 1.0 - abs(y - pv[0].w) / 0.3)));
    return x < cut ? 0.0 : clamp01(pv[1].x * (x - pv[1].y));
}
float4 PSKeyMatte(float4 pos : SV_Position) : SV_Target {
    float proj;
    float a = keyMatte(tIn.Load(int3(int2(pos.xy), 0)).rgb, proj);
    return float4(a, a, a, a);
}
float4 PSKeySpill(float4 pos : SV_Position) : SV_Target {
    float4 q = tIn.Load(int3(int2(pos.xy), 0));
    float proj;
    keyMatte(q.rgb, proj);
    float amount = pv[1].w;
    if (amount > 0.0 && proj > 0.0) {
        float y = lumaF(q.rgb);
        float cb = (q.b - y) / 1.8556, cr = (q.r - y) / 1.5748;
        cb -= amount * proj * pv[0].x;
        cr -= amount * proj * pv[0].y;
        q.r = clamp01(y + 1.5748 * cr);
        q.b = clamp01(y + 1.8556 * cb);
        q.g = clamp01((y - 0.2126 * q.r - 0.0722 * q.b) / 0.7152);
    }
    return q;
}
// separable min / max of the alpha channel: info.x = radius, info.y = dilate, info.z = vertical
float4 PSMorph(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    int2 mx = int2((int)size.z - 1, (int)size.w - 1);
    float4 q = tIn.Load(int3(p, 0));
    float v = q.a;
    [loop] for (int i = -info.x; i <= info.x; ++i) {
        int2 s = info.z != 0 ? int2(p.x, clamp(p.y + i, 0, mx.y)) : int2(clamp(p.x + i, 0, mx.x), p.y);
        float a = tIn.Load(int3(s, 0)).a;
        v = info.y != 0 ? max(v, a) : min(v, a);
    }
    return float4(q.rgb, v);
}
// colour from tIn (spill-suppressed), matte from tAux.a; info.x = show matte
float4 PSKeyApply(float4 pos : SV_Position) : SV_Target {
    float4 q = tIn.Load(int3(int2(pos.xy), 0));
    float a = clamp01(tAux.Load(int3(int2(pos.xy), 0)).a) * q.a;
    if (info.x != 0) return float4(a, a, a, 1.0);
    return float4(q.rgb, a);
}

// p0 = (red, green, blue weights)
float4 PSBw(float4 pos : SV_Position) : SV_Target {
    float4 q = tIn.Load(int3(int2(pos.xy), 0));
    float v = clamp01(p0.x * q.r + p0.y * q.g + p0.z * q.b);
    return float4(v, v, v, q.a);
}
// p0 = (tone rgb, tone luma), p1.x = strength
float4 PSSepia(float4 pos : SV_Position) : SV_Target {
    float4 q = tIn.Load(int3(int2(pos.xy), 0));
    float l = lumaF(q.rgb), w = 4.0 * l * (1.0 - l) * p1.x;
    return float4(clamp01(l + (p0.x - p0.w) * w), clamp01(l + (p0.y - p0.w) * w), clamp01(l + (p0.z - p0.w) * w), q.a);
}
)HLSL";

const char* kFxBodyM11 = R"HLSL(
// ---- M11 effects (mirror the M11 section of effects_cpu.cpp operation by operation) -------------------------
float ssF(float a, float b, float x) { float t = clamp01((x - a) / (b - a)); return t * t * (3.0 - 2.0 * t); }

// sin and cos to float precision on every GPU (some GPUs' built-ins are good to only ~1e-4, a visible shift at a
// radius of hundreds of pixels): x reduced to a quarter turn, then Taylor polynomials (error < 3e-8 on +-pi/4)
void sinCosF(float x, out float s, out float c) {
    x -= (2.0 * kPiF) * floor(x / (2.0 * kPiF) + 0.5);
    float k = floor(x * 0.636619772 + 0.5);
    float r = x - k * 1.57079632679;
    float r2 = r * r;
    float sr = r * (1.0 + r2 * (-1.66666667e-1 + r2 * (8.33333333e-3 + r2 * (-1.98412698e-4 + r2 * 2.75573192e-6))));
    float cr = 1.0 + r2 * (-0.5 + r2 * (4.16666667e-2 + r2 * (-1.38888889e-3 + r2 * 2.48015873e-5)));
    int q = ((int)k) & 3;
    s = q == 0 ? sr : (q == 1 ? cr : (q == 2 ? -sr : -cr));
    c = q == 0 ? cr : (q == 1 ? -sr : (q == 2 ? -cr : sr));
}

// info.x = mode (premultiplied)
float4 PSMirror(float4 pos : SV_Position) : SV_Target {
    float W = size.z, H = size.w;
    float2 s = pos.xy;
    if ((info.x == 0 || info.x == 4) && pos.x > W * 0.5) s.x = W - pos.x;
    if (info.x == 1 && pos.x < W * 0.5) s.x = W - pos.x;
    if ((info.x == 2 || info.x == 4) && pos.y > H * 0.5) s.y = H - pos.y;
    if (info.x == 3 && pos.y < H * 0.5) s.y = H - pos.y;
    return sampleLin(s);
}
// p0 = (cx, cy, zoom, angle), p1.x = sector (premultiplied)
float4 PSKaleido(float4 pos : SV_Position) : SV_Target {
    float dx = (pos.x - p0.x) / p0.z, dy = (pos.y - p0.y) / p0.z;
    float r = sqrt(dx * dx + dy * dy);
    float a = atan2(dy, dx) - p0.w;
    a -= p1.x * floor(a / p1.x);
    if (a > 0.5 * p1.x) a = p1.x - a;
    a += p0.w;
    float sn, cs;
    sinCosF(a, sn, cs);
    return sampleLin(float2(p0.x + r * cs, p0.y + r * sn));
}
// p0 = (cx, cy, radius, angle) (premultiplied)
float4 PSSwirl(float4 pos : SV_Position) : SV_Target {
    float dx = pos.x - p0.x, dy = pos.y - p0.y;
    float r = sqrt(dx * dx + dy * dy);
    float2 s = pos.xy;
    if (r < p0.z) {
        float t = 1.0 - r / p0.z;
        float th = p0.w * t * t;
        float sn, cs;
        sinCosF(th, sn, cs);
        s = float2(p0.x + dx * cs + dy * sn, p0.y - dx * sn + dy * cs);
    }
    return sampleLin(s);
}
// p0 = (cx, cy, half diagonal, k) (premultiplied)
float4 PSFisheye(float4 pos : SV_Position) : SV_Target {
    float dx = (pos.x - p0.x) / p0.z, dy = (pos.y - p0.y) / p0.z;
    float f = 1.0 - 0.5 * p0.w * (1.0 - (dx * dx + dy * dy));
    return sampleLin(float2(p0.x + dx * f * p0.z, p0.y + dy * f * p0.z));
}
// p0 = (cx, cy, k), info.x = samples (premultiplied)
float4 PSZoomBlur(float4 pos : SV_Position) : SV_Target {
    float4 acc = 0;
    int n = info.x;
    [loop] for (int i = 0; i < n; ++i) {
        float f = 1.0 - p0.z * (float)i / (float)(n - 1);
        acc += sampleLin(float2(p0.x + (pos.x - p0.x) * f, p0.y + (pos.y - p0.y) * f));
    }
    return acc / (float)n;
}
// tIn = straight picture, tAux = its blur; p0 = (band centre, inner, outer)
float4 PSTiltMix(float4 pos : SV_Position) : SV_Target {
    float4 q = tIn.Load(int3(int2(pos.xy), 0)), b = tAux.Load(int3(int2(pos.xy), 0));
    float m = ssF(p0.y, p0.z, abs(pos.y / size.y - p0.x));
    return q + (b - q) * m;
}
// p0 = (cx, cy, ox, oy), p1 = (cos, sin, zoom) (premultiplied)
float4 PSShake(float4 pos : SV_Position) : SV_Target {
    float dx = (pos.x - p0.x - p0.z) / p1.z, dy = (pos.y - p0.y - p0.w) / p1.z;
    return sampleLin(float2(p0.x + dx * p1.x + dy * p1.y, p0.y - dx * p1.y + dy * p1.x));
}
// info.x = slot, info.y = bands; p0 = (amount, width, scale) (premultiplied)
float4 PSGlitch(float4 pos : SV_Position) : SV_Target {
    uint band = ((uint)pos.y * (uint)info.y) / (uint)size.w;
    uint slot = asuint(info.x);
    float a = p0.x, shift = 0.0, split = a * 2.0 * p0.z;
    if (noise01(band, slot, 0x2545F491u, 0) < a * 0.6) {
        shift = (noise01(band, slot, 0x2545F491u, 1) - 0.5) * 2.0 * a * 0.2 * p0.y;
        split = a * 10.0 * p0.z;
    }
    float px = pos.x + shift;
    float4 r = sampleLin(float2(px + split, pos.y)), g = sampleLin(float2(px, pos.y)), b = sampleLin(float2(px - split, pos.y));
    return float4(r.r, g.g, b.b, max(r.a, max(g.a, b.a)));
}
// p0 = (shift, bandY, bandH, tracking), p1 = (scale, bleed, lines, noise), p2.x = period; info.x = slot, info.y = seed
float vhsBand(float py) { return max(0.0, 1.0 - abs(py - p0.y) / p0.z) * p0.w; }
float4 PSVhsShift(float4 pos : SV_Position) : SV_Target {
    float band = vhsBand(pos.y);
    float jitter = band * 12.0 * p1.x * (noise01((uint)pos.y, asuint(info.x), 0x68E31DA4u, 2) - 0.5) * 2.0;
    float px = pos.x + jitter;
    float4 r = sampleLin(float2(px + p0.x, pos.y)), g = sampleLin(float2(px, pos.y)), b = sampleLin(float2(px - p0.x, pos.y));
    return float4(r.r, g.g, b.b, max(r.a, max(g.a, b.a)));
}
float4 PSVhsTone(float4 pos : SV_Position) : SV_Target {
    float4 q = tIn.Load(int3(int2(pos.xy), 0));
    float band = vhsBand(pos.y);
    float phase = fmod(pos.y - 0.5, p2.x) / p2.x;
    float scan = 1.0 - p1.z * 0.3 * (0.5 + 0.5 * cos((2.0 * kPiF) * phase));
    float l = lumaF(q.rgb);
    float nz = (noise01((uint)pos.x, (uint)pos.y, asuint(info.y), 7) - 0.5) * (p1.w * 0.25 + band * 0.5);
    float3 o = (l + (q.rgb - l) * (1.0 - 0.2 * p1.y)) * scan + nz;
    return float4(clamp01(o.r), clamp01(o.g), clamp01(o.b), q.a);
}
// p0 = (age, dust, vignette, flicker factor), p1 = (cx, cy, half diagonal), pv[i] = scratch (x, half width, opacity);
// info.x = film frame, info.y = dust cell (px)
float4 PSOldFilm(float4 pos : SV_Position) : SV_Target {
    float4 q = tIn.Load(int3(int2(pos.xy), 0));
    float age = p0.x;
    float l = lumaF(q.rgb);
    float3 sep = float3(l * 1.07 + 0.03, l * 0.89 + 0.02, l * 0.70);
    float s = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) s += pv[i].z * max(0.0, 1.0 - abs(pos.x - pv[i].x) / pv[i].y);
    uint cell = (uint)info.y;
    bool speck = noise01((uint)pos.x / cell, (uint)pos.y / cell, asuint(info.x) * 7919u + 0x1B873593u, 8) < p0.y * 0.004;
    float dx = pos.x - p1.x, dy = pos.y - p1.y;
    float r = sqrt(dx * dx + dy * dy) / p1.z;
    float v = 1.0 - p0.z * 0.7 * ssF(0.45, 1.0, r);
    float3 o = q.rgb + (sep - q.rgb) * (age * 0.85);
    o = 0.06 * age + o * (1.0 - 0.12 * age);
    o *= p0.w;
    o += (0.95 - o) * min(1.0, s);
    if (speck) o *= 0.25;
    o *= v;
    return float4(clamp01(o.r), clamp01(o.g), clamp01(o.b), q.a);
}
// p0 = (x1, y1, s1, x2), p1 = (y2, s2, amount x pulse), p2 = colour
float4 PSLightLeak(float4 pos : SV_Position) : SV_Target {
    float4 q = tIn.Load(int3(int2(pos.xy), 0));
    float d1x = pos.x - p0.x, d1y = pos.y - p0.y, d2x = pos.x - p0.w, d2y = pos.y - p1.x;
    float m = clamp01((exp(-(d1x * d1x + d1y * d1y) / (2.0 * p0.z * p0.z)) +
                       0.7 * exp(-(d2x * d2x + d2y * d2y) / (2.0 * p1.y * p1.y))) * p1.z);
    float3 o = 1.0 - (1.0 - q.rgb) * (1.0 - p2.rgb * m);
    return float4(o, q.a);
}
// p0 = (cell, cos, sin, style)
float4 PSHalftone(float4 pos : SV_Position) : SV_Target {
    float4 q = tIn.Load(int3(int2(pos.xy), 0));
    float u = (pos.x * p0.y + pos.y * p0.z) / p0.x, v = (-pos.x * p0.z + pos.y * p0.y) / p0.x;
    float fu = u - floor(u) - 0.5, fv = v - floor(v) - 0.5;
    float d = sqrt(fu * fu + fv * fv);
    float r = 0.5642 * sqrt(1.0 - clamp01(lumaF(q.rgb)));
    float aa = 1.0 / p0.x;
    float cov = clamp01((r - d) / aa + 0.5) * min(1.0, r / aa);
    float3 o;
    if (p0.w < 0.5) o = 1.0 - cov;
    else if (p0.w < 1.5) o = 1.0 + (q.rgb - 1.0) * cov;
    else o = float3(0.94, 0.92, 0.85) + (float3(0.12, 0.12, 0.14) - float3(0.94, 0.92, 0.85)) * cov;
    return float4(o, q.a);
}
// p0.x = levels - 1
float4 PSPosterize(float4 pos : SV_Position) : SV_Target {
    float4 q = tIn.Load(int3(int2(pos.xy), 0));
    return float4(floor(q.rgb * p0.x + 0.5) / p0.x, q.a);
}
// p0 = (dx, dy, k, style) (straight)
float4 PSEmboss(float4 pos : SV_Position) : SV_Target {
    float4 q = tIn.Load(int3(int2(pos.xy), 0));
    float g = (lumaF(sampleLin(pos.xy - p0.xy).rgb) - lumaF(sampleLin(pos.xy + p0.xy).rgb)) * p0.z;
    float3 o = p0.w < 0.5 ? float3(0.5 + g, 0.5 + g, 0.5 + g) : q.rgb + g;
    return float4(clamp01(o.r), clamp01(o.g), clamp01(o.b), q.a);
}
// p0.x = amount
float4 PSInvert(float4 pos : SV_Position) : SV_Target {
    float4 q = tIn.Load(int3(int2(pos.xy), 0));
    return float4(q.rgb + (1.0 - 2.0 * q.rgb) * p0.x, q.a);
}
// p0 = (bar height, bar width, H, W), p1 = (colour, opacity)
float4 PSLetterbox(float4 pos : SV_Position) : SV_Target {
    float4 q = tIn.Load(int3(int2(pos.xy), 0));
    float fx = pos.x - 0.5, fy = pos.y - 0.5;
    float cov = p0.x > 0.0 ? clamp01(p0.x - fy) + clamp01((fy + 1.0) - (p0.z - p0.x))
                           : clamp01(p0.y - fx) + clamp01((fx + 1.0) - (p0.w - p0.y));
    cov = min(1.0, cov) * p1.w;
    return float4(q.rgb + (p1.rgb - q.rgb) * cov, q.a + (1.0 - q.a) * cov);
}
// p0 = (cx, cy, radius, outer), p1.x = darkness
float4 PSSpotlight(float4 pos : SV_Position) : SV_Target {
    float4 q = tIn.Load(int3(int2(pos.xy), 0));
    float dy = pos.y - p0.y, dx = pos.x - p0.x;
    float m = 1.0 - p1.x * ssF(p0.z, p0.w, sqrt(dx * dx + dy * dy));
    return float4(q.rgb * m, q.a);
}
// p0 = shadows, p1 = highlights, p2.x = strength
float4 PSDuotone(float4 pos : SV_Position) : SV_Target {
    float4 q = tIn.Load(int3(int2(pos.xy), 0));
    float l = lumaF(q.rgb);
    float3 t = p0.rgb + (p1.rgb - p0.rgb) * l;
    return float4(q.rgb + (t - q.rgb) * p2.x, q.a);
}
)HLSL";

enum FxPs { kToFloat, kToBytes, kPremul, kUnpremul, kBasic, kDown, kBlurH, kBlurV, kUp, kMix, kAdjustMix,
            kHsl, kWheels, kCurves, kVignette, kSharpen, kLut, kDirBlur, kGlowBright, kGlowCombine, kBlockAvg,
            kBlockUp, kGrain, kRgbSplit, kEdge, kChromatic, kWave, kBulge, kKeyMatte, kKeySpill, kMorph, kKeyApply, kBw,
            kSepia, kMirror, kKaleido, kSwirl, kFisheye, kZoomBlur, kTiltMix, kShake, kGlitch, kVhsShift, kVhsTone,
            kOldFilm, kLightLeak, kHalftone, kPosterize, kEmboss, kInvert, kLetterbox, kSpotlight, kDuotone, kFxCount };
const char* const kFxEntry[kFxCount] = {"PSToFloat", "PSToBytes", "PSPremul", "PSUnpremul", "PSBasic",  "PSDown",
                                        "PSBlurH",   "PSBlurV",   "PSUp",     "PSMix",      "PSAdjustMix",
                                        "PSHsl",     "PSWheels",  "PSCurves", "PSVignette", "PSSharpen",
                                        "PSLut",     "PSDirBlur", "PSGlowBright", "PSGlowCombine", "PSBlockAvg",
                                        "PSBlockUp", "PSGrain",   "PSRgbSplit", "PSEdge",      "PSChromatic", "PSWave",
                                        "PSBulge",   "PSKeyMatte", "PSKeySpill", "PSMorph",    "PSKeyApply", "PSBw",
                                        "PSSepia",   "PSMirror",  "PSKaleido", "PSSwirl",    "PSFisheye", "PSZoomBlur",
                                        "PSTiltMix", "PSShake",   "PSGlitch",  "PSVhsShift", "PSVhsTone", "PSOldFilm",
                                        "PSLightLeak", "PSHalftone", "PSPosterize", "PSEmboss", "PSInvert", "PSLetterbox",
                                        "PSSpotlight", "PSDuotone"};

struct alignas(16) FxConstants {
    float size[4];
    float p0[4];
    float p1[4];
    float p2[4];
    float p3[4];
    int32_t info[4];
    float mixp[4];
    float pv[32];
    float weights[64];
    float maskA[kMaxMasks][4];
    float maskB[kMaxMasks][4];
    int32_t maskI[kMaxMasks][4];
    int32_t maskN[kMaxMasks][4];
    float points[kMaxPoints][4];
};
static_assert(sizeof(FxConstants) % 16 == 0);

// Effect types with GPU passes (the others run on the CPU reference for that layer).
bool gpuSupports(const std::string& type) {
    static const char* ids[] = {"fa.color.basic", "fa.fx.blur.gaussian", "fa.color.hsl", "fa.color.wheels",
                                "fa.color.curves", "fa.fx.vignette", "fa.fx.sharpen", "fa.color.lut",
                                "fa.filter.look", "fa.fx.blur.directional", "fa.fx.glow", "fa.fx.pixelate", "fa.fx.grain",
                                "fa.fx.rgbsplit", "fa.fx.edge", "fa.fx.chromatic", "fa.fx.wave", "fa.fx.bulge",
                                "fa.fx.chromakey", "fa.filter.bw", "fa.filter.sepia",
                                // M11
                                "fa.fx.mirror", "fa.fx.kaleidoscope", "fa.fx.swirl", "fa.fx.fisheye", "fa.fx.blur.zoom",
                                "fa.fx.tiltshift", "fa.fx.shake", "fa.fx.glitch", "fa.fx.vhs", "fa.fx.oldfilm",
                                "fa.fx.lightleak", "fa.fx.halftone", "fa.fx.posterize", "fa.fx.emboss", "fa.fx.invert",
                                "fa.fx.letterbox", "fa.fx.spotlight", "fa.filter.duotone"};
    for (const char* id : ids)
        if (type == id) return true;
    return false;
}

// Fills the mask part of the constants (same layout and units as the composite pass). False: beyond the limits.
template <typename C>
bool packMasks(C& c, const std::vector<LayerMask>& masks) {
    size_t totalPoints = 0;
    for (const auto& m : masks) totalPoints += m.shape == MaskShape::Polygon ? m.points.size() : 0;
    if (masks.size() > size_t(kMaxMasks) || totalPoints > size_t(kMaxPoints)) return false;
    int off = 0;
    for (size_t k = 0; k < masks.size(); ++k) {
        const LayerMask& m = masks[k];
        c.maskA[k][0] = float(m.cx);
        c.maskA[k][1] = float(m.cy);
        c.maskA[k][2] = float(m.w);
        c.maskA[k][3] = float(m.h);
        c.maskB[k][0] = float(m.rotationDeg);
        c.maskB[k][1] = float(m.feather);
        c.maskB[k][2] = float(m.opacity);
        c.maskB[k][3] = float(m.cornerRadius);
        c.maskI[k][0] = static_cast<int32_t>(m.shape);
        c.maskI[k][1] = static_cast<int32_t>(m.mode);
        c.maskI[k][2] = m.inverted ? 1 : 0;
        c.maskI[k][3] = off;
        const int n = m.shape == MaskShape::Polygon ? static_cast<int>(m.points.size()) : 0;
        c.maskN[k][0] = n;
        for (int i = 0; i < n; ++i) {
            c.points[off + i][0] = float(m.points[static_cast<size_t>(i)].x);
            c.points[off + i][1] = float(m.points[static_cast<size_t>(i)].y);
        }
        off += n;
    }
    return true;
}

const std::string kFxShader =
    std::string(kFxHead) + kVertexShader + kMaskFunctions + kFxBody + kFxBodyLut + kFxBodyLibrary + kFxBodyM11;

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string hr(HRESULT h) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(h));
    return buf;
}

void check(HRESULT h, const char* what) {
    if (FAILED(h)) throw std::runtime_error(std::string("Direct3D 11: ") + what + " failed (" + hr(h) + ")");
}

const char* levelName(D3D_FEATURE_LEVEL l) {
    switch (l) {
        case D3D_FEATURE_LEVEL_11_1: return "11_1";
        case D3D_FEATURE_LEVEL_11_0: return "11_0";
        case D3D_FEATURE_LEVEL_10_1: return "10_1";
        case D3D_FEATURE_LEVEL_10_0: return "10_0";
        default: return "?";
    }
}

// Compiled shader bytecode, shared by all backend instances (compiled once per process).
struct Bytecode {
    ComPtr<ID3DBlob> vs, ps;
    ComPtr<ID3DBlob> fx[kFxCount];
    std::string error;
};
const Bytecode& bytecode() {
    static Bytecode bc;
    static std::once_flag once;
    std::call_once(once, [] {
        const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS;
        ComPtr<ID3DBlob> err;
        if (FAILED(D3DCompile(kShader.c_str(), kShader.size(), "fa_composite.hlsl", nullptr, nullptr, "VSMain", "vs_4_0", flags, 0,
                              &bc.vs, &err))) {
            bc.error = err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) : "vs compile failed";
            return;
        }
        err.Reset();
        if (FAILED(D3DCompile(kShader.c_str(), kShader.size(), "fa_composite.hlsl", nullptr, nullptr, "PSMain", "ps_4_0", flags, 0,
                              &bc.ps, &err))) {
            bc.error = err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) : "ps compile failed";
            bc.vs.Reset();
            return;
        }
        for (int i = 0; i < kFxCount; ++i) {
            err.Reset();
            if (FAILED(D3DCompile(kFxShader.c_str(), kFxShader.size(), "fa_effects.hlsl", nullptr, nullptr, kFxEntry[i], "ps_4_0",
                                  flags, 0, &bc.fx[i], &err))) {
                bc.error = std::string(kFxEntry[i]) + ": " +
                           (err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) : "compile failed");
                bc.vs.Reset();
                return;
            }
        }
    });
    return bc;
}

struct Adapter {
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
};

// Hardware adapters in the requested preference order (DXGI 1.6), else in enumeration order.
std::vector<Adapter> adapters(bool highPerformance) {
    std::vector<Adapter> out;
    ComPtr<IDXGIFactory1> f1;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f1)))) return out;
    ComPtr<IDXGIFactory6> f6;
    f1.As(&f6);
    for (UINT i = 0;; ++i) {
        Adapter a;
        HRESULT h;
        if (f6) {
            h = f6->EnumAdapterByGpuPreference(i, highPerformance ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE : DXGI_GPU_PREFERENCE_MINIMUM_POWER,
                                               IID_PPV_ARGS(&a.adapter));
        } else {
            h = f1->EnumAdapters1(i, &a.adapter);
        }
        if (h == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(h)) continue;
        a.adapter->GetDesc1(&a.desc);
        out.push_back(std::move(a));
    }
    return out;
}

class D3D11Backend final : public RenderBackend {
public:
    static std::unique_ptr<D3D11Backend> create(bool highPerformance, std::string* why) {
        const Bytecode& bc = bytecode();
        if (!bc.vs || !bc.ps) {
            if (why) *why = "shader compilation failed: " + bc.error;
            return nullptr;
        }
        std::string lastError = "no hardware Direct3D 11 adapter";
        for (auto& a : adapters(highPerformance)) {
            if (a.desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            auto b = std::unique_ptr<D3D11Backend>(new D3D11Backend());
            const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                                D3D_FEATURE_LEVEL_10_0};
            D3D_FEATURE_LEVEL got{};
            HRESULT h = D3D11CreateDevice(a.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED,
                                          levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &b->dev_, &got, &b->ctx_);
            if (FAILED(h)) {
                lastError = "D3D11CreateDevice failed on " + narrow(a.desc.Description) + " (" + hr(h) + ")";
                continue;
            }
            try {
                b->init(bc);
            } catch (const std::exception& e) {
                lastError = e.what();
                continue;
            }
            b->description_ = narrow(a.desc.Description) + " (Direct3D 11, feature level " + levelName(got) + ", " +
                              std::to_string(a.desc.DedicatedVideoMemory / (1024 * 1024)) + " MB)";
            return b;
        }
        if (why) *why = lastError;
        return nullptr;
    }

    std::string name() const override { return "gpu: " + description_; }
    double lastGpuMilliseconds() const override { return gpuMs_; }
    int64_t gpuEffectPasses() const override { return fxPasses_; }
    int64_t textureUploads() const override { return uploads_; }
    int64_t textureReuses() const override { return reuses_; }
    int64_t cpuEffectFallbacks() const override { return cpuEffectLayers_; }
    void setTimingEnabled(bool on) override { timing_ = on; }

    void beginFrame(int width, int height, Rgba8 bg) override {
        if (width <= 0 || height <= 0) throw std::invalid_argument("invalid canvas size");
        if (width != w_ || height != h_) resize(width, height);
        const float c[4] = {bg.r / 255.0f, bg.g / 255.0f, bg.b / 255.0f, bg.a / 255.0f};
        ctx_->ClearRenderTargetView(canvas_[0].rtv.Get(), c);
        cur_ = 0;
        nextSource_ = 0;
        if (timing_) {
            ctx_->Begin(qDisjoint_.Get());
            ctx_->End(qBegin_.Get());
        }
    }

    void drawLayer(const LayerDesc& l) override {
        if (l.adjustment) {
            drawAdjustment(l);
            return;
        }
        if (!l.image || l.image->empty()) return;
        const double opacity = std::clamp(l.opacity, 0.0, 1.0);
        if (opacity <= 0.0) return;
        const Image& src = *l.image;
        ID3D11ShaderResourceView* srcView = nullptr;
        if (l.effects && !effectsAreIdentity(*l.effects)) {
            if (stackOnGpu(*l.effects)) {
                srcView = runEffects(l.imageKey ? uploadKeyed(src, l.imageKey) : upload(src), src.width, src.height, *l.effects);
            } else {   // an effect without GPU passes: the CPU reference runs this layer's stack
                applyEffectsCpu(src, *l.effects, cpuProcessed_);
                srcView = upload(cpuProcessed_);
                ++cpuEffectLayers_;
            }
            restoreCompositeState();
        }
        const int cw = w_, ch = h_, sw = src.width, sh = src.height;
        const Affine2D place = layerPlacement(l, cw, ch);
        Constants c{};
        c.canvas[0] = float(cw);
        c.canvas[1] = float(ch);
        c.canvas[2] = float(sw);
        c.canvas[3] = float(sh);
        const bool plain = src.width == cw && src.height == ch && place == Affine2D{} && l.cropL == 0 && l.cropT == 0 &&
                           l.cropR == 0 && l.cropB == 0 && l.blend == BlendMode::Normal && l.brightness == 1.0 &&
                           l.flash == 0.0 && l.masks.empty() && !l.wipe.active;
        if (plain) {
            c.info[2] = 1;
            c.info2[0] = static_cast<int32_t>(std::lround(opacity * 255.0));
        } else {
            const Affine2D inv = place.inverse();
            c.inv0[0] = float(inv.a);
            c.inv0[1] = float(inv.b);
            c.inv0[2] = float(inv.c);
            c.inv1[0] = float(inv.d);
            c.inv1[1] = float(inv.e);
            c.inv1[2] = float(inv.f);
            const double x0s = l.cropL * sw, x1s = sw - l.cropR * sw, y0s = l.cropT * sh, y1s = sh - l.cropB * sh;
            c.crop[0] = float(x0s);
            c.crop[1] = float(y0s);
            c.crop[2] = float(x1s);
            c.crop[3] = float(y1s);
            // same canvas bounding box as the CPU backend (pixels outside it are left untouched)
            double bx0 = 1e18, by0 = 1e18, bx1 = -1e18, by1 = -1e18;
            for (const auto& [sx, sy] : {std::pair{x0s, y0s}, {x1s, y0s}, {x0s, y1s}, {x1s, y1s}}) {
                const Vec2 p = place.apply(sx, sy);
                bx0 = std::min(bx0, p.x);
                by0 = std::min(by0, p.y);
                bx1 = std::max(bx1, p.x);
                by1 = std::max(by1, p.y);
            }
            const int xs = std::max(0, static_cast<int>(std::floor(bx0)) - 1);
            const int xe = std::min(cw, static_cast<int>(std::ceil(bx1)) + 1);
            const int ys = std::max(0, static_cast<int>(std::floor(by0)) - 1);
            const int ye = std::min(ch, static_cast<int>(std::ceil(by1)) + 1);
            if (xs >= xe || ys >= ye) return;
            c.bbox[0] = xs;
            c.bbox[1] = ys;
            c.bbox[2] = xe;
            c.bbox[3] = ye;
            double fx = 1, fy = 1;
            fitScale(cw, ch, sw, sh, l.sourceToCanvas ? LayerFit::Fit : l.fit, fx, fy);
            const double canvasPerSourcePx = std::sqrt(std::fabs(place.det()));
            c.params[0] = float(opacity);
            c.params[1] = float(l.brightness);
            c.params[2] = float(fy > 0 ? (fx / fy) / sh : 1.0 / sh);
            c.params[3] = float(1.0 / sh);
            c.params2[0] = float(canvasPerSourcePx > 0 ? 1.0 / (canvasPerSourcePx * sh) : 1.0 / sh);
            c.params2[1] = float(l.wipe.progress);
            c.params2[2] = float(l.wipe.softness);
            c.params2[3] = float(std::clamp(l.flash, 0.0, 1.0));
            c.wipeDir[0] = float(l.wipe.dirX);
            c.wipeDir[1] = float(l.wipe.dirY);
            c.info[0] = static_cast<int32_t>(l.blend);
            c.info[3] = l.wipe.active ? 1 : 0;
            c.info2[1] = static_cast<int32_t>(l.wipe.shape);
            c.info2[2] = l.wipe.stripes;
            size_t totalPoints = 0;
            for (const auto& m : l.masks) totalPoints += m.shape == MaskShape::Polygon ? m.points.size() : 0;
            if (l.masks.size() > size_t(kMaxMasks) || totalPoints > size_t(kMaxPoints)) {
                compositeOnCpu(l);
                return;
            }
            c.info[1] = static_cast<int32_t>(l.masks.size());
            int off = 0;
            for (size_t k = 0; k < l.masks.size(); ++k) {
                const LayerMask& m = l.masks[k];
                c.maskA[k][0] = float(m.cx);
                c.maskA[k][1] = float(m.cy);
                c.maskA[k][2] = float(m.w);
                c.maskA[k][3] = float(m.h);
                c.maskB[k][0] = float(m.rotationDeg);
                c.maskB[k][1] = float(m.feather);
                c.maskB[k][2] = float(m.opacity);
                c.maskB[k][3] = float(m.cornerRadius);
                c.maskI[k][0] = static_cast<int32_t>(m.shape);
                c.maskI[k][1] = static_cast<int32_t>(m.mode);
                c.maskI[k][2] = m.inverted ? 1 : 0;
                c.maskI[k][3] = off;
                const int n = m.shape == MaskShape::Polygon ? static_cast<int>(m.points.size()) : 0;
                c.maskN[k][0] = n;
                for (int i = 0; i < n; ++i) {
                    c.points[off + i][0] = float(m.points[static_cast<size_t>(i)].x);
                    c.points[off + i][1] = float(m.points[static_cast<size_t>(i)].y);
                }
                off += n;
            }
        }
        if (!srcView) srcView = l.imageKey ? uploadKeyed(src, l.imageKey) : upload(src);
        ctx_->UpdateSubresource(cb_.Get(), 0, nullptr, &c, 0, 0);
        // unbind the backdrop before it becomes the render target of a later pass
        ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
        ctx_->PSSetShaderResources(0, 2, none);
        const int next = cur_ ^ 1;
        ID3D11RenderTargetView* rtv = canvas_[next].rtv.Get();
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        ID3D11ShaderResourceView* views[2] = {canvas_[cur_].srv.Get(), srcView};
        ctx_->PSSetShaderResources(0, 2, views);
        ctx_->Draw(3, 0);
        cur_ = next;
    }

    const Image& endFrame() override {
        if (timing_) ctx_->End(qEnd_.Get());
        ctx_->CopyResource(staging_.Get(), canvas_[cur_].tex.Get());
        if (timing_) ctx_->End(qDisjoint_.Get());
        readback();
        if (timing_) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 t0 = 0, t1 = 0;
            while (ctx_->GetData(qDisjoint_.Get(), &dj, sizeof dj, 0) == S_FALSE) Sleep(0);
            while (ctx_->GetData(qBegin_.Get(), &t0, sizeof t0, 0) == S_FALSE) Sleep(0);
            while (ctx_->GetData(qEnd_.Get(), &t1, sizeof t1, 0) == S_FALSE) Sleep(0);
            gpuMs_ = dj.Disjoint || dj.Frequency == 0 ? -1.0 : double(t1 - t0) * 1000.0 / double(dj.Frequency);
        }
        return out_;
    }

private:
    struct Canvas {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
    };
    struct Source {
        int w = 0, h = 0;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
    };

    D3D11Backend() = default;

    void init(const Bytecode& bc) {
        check(dev_->CreateVertexShader(bc.vs->GetBufferPointer(), bc.vs->GetBufferSize(), nullptr, &vs_), "CreateVertexShader");
        check(dev_->CreatePixelShader(bc.ps->GetBufferPointer(), bc.ps->GetBufferSize(), nullptr, &ps_), "CreatePixelShader");
        for (int i = 0; i < kFxCount; ++i)
            check(dev_->CreatePixelShader(bc.fx[i]->GetBufferPointer(), bc.fx[i]->GetBufferSize(), nullptr, &fxPs_[i]),
                  "CreatePixelShader (effects)");
        {
            D3D11_BUFFER_DESC fd{};
            fd.ByteWidth = sizeof(FxConstants);
            fd.Usage = D3D11_USAGE_DEFAULT;
            fd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            check(dev_->CreateBuffer(&fd, nullptr, &fxCb_), "CreateBuffer (effects)");
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(Constants);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(dev_->CreateBuffer(&bd, nullptr, &cb_), "CreateBuffer");
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        check(dev_->CreateQuery(&qd, &qDisjoint_), "CreateQuery");
        qd.Query = D3D11_QUERY_TIMESTAMP;
        check(dev_->CreateQuery(&qd, &qBegin_), "CreateQuery");
        check(dev_->CreateQuery(&qd, &qEnd_), "CreateQuery");
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->PSSetShader(ps_.Get(), nullptr, 0);
        ID3D11Buffer* cbs[1] = {cb_.Get()};
        ctx_->PSSetConstantBuffers(0, 1, cbs);
    }

    void resize(int w, int h) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (auto& cv : canvas_) {
            cv = {};
            check(dev_->CreateTexture2D(&td, nullptr, &cv.tex), "CreateTexture2D (canvas)");
            check(dev_->CreateRenderTargetView(cv.tex.Get(), nullptr, &cv.rtv), "CreateRenderTargetView");
            check(dev_->CreateShaderResourceView(cv.tex.Get(), nullptr, &cv.srv), "CreateShaderResourceView");
        }
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_.Reset();
        check(dev_->CreateTexture2D(&td, nullptr, &staging_), "CreateTexture2D (staging)");
        D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
        ctx_->RSSetViewports(1, &vp);
        w_ = w;
        h_ = h;
        out_.reset(w, h);
    }

    // Upload a source picture into a texture of the per-frame pool (one per layer in the frame).
    // Keyed pictures (LayerDesc::imageKey): uploaded once into a default-usage texture and reused while the key
    // keeps coming back (static titles, a paused frame being graded); the 8 most recent are kept.
    struct KeyedTexture {
        uint64_t key = 0;
        int w = 0, h = 0;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
    };
    std::list<KeyedTexture> keyed_;
    int64_t uploads_ = 0, reuses_ = 0;

    ID3D11ShaderResourceView* uploadKeyed(const Image& img, uint64_t key) {
        for (auto it = keyed_.begin(); it != keyed_.end(); ++it)
            if (it->key == key && it->w == img.width && it->h == img.height) {
                keyed_.splice(keyed_.begin(), keyed_, it);
                ++reuses_;
                return keyed_.front().srv.Get();
            }
        KeyedTexture t;
        t.key = key;
        t.w = img.width;
        t.h = img.height;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(img.width);
        td.Height = static_cast<UINT>(img.height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{img.pixels.data(), static_cast<UINT>(img.stride()), 0};
        check(dev_->CreateTexture2D(&td, &init, &t.tex), "CreateTexture2D (keyed source)");
        check(dev_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv), "CreateShaderResourceView (keyed source)");
        ++uploads_;
        keyed_.push_front(std::move(t));
        if (keyed_.size() > 8) keyed_.pop_back();
        return keyed_.front().srv.Get();
    }

    ID3D11ShaderResourceView* upload(const Image& img) {
        ++uploads_;
        if (nextSource_ >= sources_.size()) sources_.emplace_back();
        Source& s = sources_[nextSource_++];
        if (s.w != img.width || s.h != img.height) {
            s = {};
            D3D11_TEXTURE2D_DESC td{};
            td.Width = static_cast<UINT>(img.width);
            td.Height = static_cast<UINT>(img.height);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC;   // written by the CPU once per frame (Map + WRITE_DISCARD)
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            check(dev_->CreateTexture2D(&td, nullptr, &s.tex), "CreateTexture2D (source)");
            check(dev_->CreateShaderResourceView(s.tex.Get(), nullptr, &s.srv), "CreateShaderResourceView (source)");
            s.w = img.width;
            s.h = img.height;
        }
        // one copy straight into the driver's upload memory (UpdateSubresource would stage it twice)
        D3D11_MAPPED_SUBRESOURCE m{};
        check(ctx_->Map(s.tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map (upload)");
        const size_t rowBytes = static_cast<size_t>(img.stride());
        if (m.RowPitch == rowBytes) {
            std::memcpy(m.pData, img.pixels.data(), rowBytes * static_cast<size_t>(img.height));
        } else {
            for (int y = 0; y < img.height; ++y)
                std::memcpy(static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, img.row(y), rowBytes);
        }
        ctx_->Unmap(s.tex.Get(), 0);
        return s.srv.Get();
    }

    void readback() {
        D3D11_MAPPED_SUBRESOURCE m{};
        check(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m), "Map (readback)");
        for (int y = 0; y < h_; ++y)
            std::memcpy(out_.row(y), static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                        static_cast<size_t>(out_.stride()));
        ctx_->Unmap(staging_.Get(), 0);
    }

    // Exact fallback for layers beyond the shader limits: CPU reference on the read-back canvas.
    void compositeOnCpu(const LayerDesc& l) {
        ctx_->CopyResource(staging_.Get(), canvas_[cur_].tex.Get());
        readback();
        compositeLayerCpu(out_, l);
        ctx_->UpdateSubresource(canvas_[cur_].tex.Get(), 0, nullptr, out_.pixels.data(), static_cast<UINT>(out_.stride()), 0);
    }

    // ---- effects (M4) -------------------------------------------------------------------------------
    struct Target {   // render target + shader view
        int w = 0, h = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
    };

    // Pool of render targets reused across frames; `slot` picks one of several of the same size and format.
    Target& target(int w, int h, DXGI_FORMAT format, int slot) {
        int seen = 0;
        for (auto& t : targets_)
            if (t.w == w && t.h == h && t.format == format && seen++ == slot) return t;
        Target t;
        t.w = w;
        t.h = h;
        t.format = format;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(dev_->CreateTexture2D(&td, nullptr, &t.tex), "CreateTexture2D (effect target)");
        check(dev_->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv), "CreateRenderTargetView (effect)");
        check(dev_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv), "CreateShaderResourceView (effect)");
        targets_.push_back(std::move(t));
        return targets_.back();
    }
    Target& floatTarget(int w, int h, int slot) { return target(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, slot); }

    void pass(FxPs ps, Target& dst, ID3D11ShaderResourceView* in0, ID3D11ShaderResourceView* in1, FxConstants& c,
              ID3D11ShaderResourceView* in2 = nullptr, ID3D11ShaderResourceView* in3 = nullptr) {
        c.size[0] = float(dst.w);
        c.size[1] = float(dst.h);
        ID3D11ShaderResourceView* none[4] = {nullptr, nullptr, nullptr, nullptr};
        ctx_->PSSetShaderResources(0, 4, none);
        ID3D11RenderTargetView* rtv = dst.rtv.Get();
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{0, 0, float(dst.w), float(dst.h), 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ctx_->PSSetShader(fxPs_[ps].Get(), nullptr, 0);
        ctx_->UpdateSubresource(fxCb_.Get(), 0, nullptr, &c, 0, 0);
        ID3D11Buffer* cbs[1] = {fxCb_.Get()};
        ctx_->PSSetConstantBuffers(0, 1, cbs);
        ID3D11ShaderResourceView* views[4] = {in0, in1, in2, in3};
        ctx_->PSSetShaderResources(0, 4, views);
        ctx_->Draw(3, 0);
        ctx_->PSSetShaderResources(0, 4, none);
        ++fxPasses_;
    }

    void restoreCompositeState() {
        ctx_->PSSetShader(ps_.Get(), nullptr, 0);
        ID3D11Buffer* cbs[1] = {cb_.Get()};
        ctx_->PSSetConstantBuffers(0, 1, cbs);
        D3D11_VIEWPORT vp{0, 0, float(w_), float(h_), 0, 1};
        ctx_->RSSetViewports(1, &vp);
    }

    static bool stackOnGpu(const std::vector<EffectInstance>& stack) {
        for (const auto& e : stack) {
            if (!e.problem.empty() || e.mix <= 0.0f) continue;
            if (!gpuSupports(e.def->id)) return false;
            if (e.masks.size() > size_t(kMaxMasks)) return false;
            size_t pts = 0;
            for (const auto& m : e.masks) pts += m.shape == MaskShape::Polygon ? m.points.size() : 0;
            if (pts > size_t(kMaxPoints)) return false;
        }
        return true;
    }

    // Runs the stack on an RGBA8 picture (w x h) and returns the view of the 8-bit result.
    // Float targets: A and B alternate as current / scratch, BEFORE keeps an effect's input for its mix pass.
    ID3D11ShaderResourceView* runEffects(ID3D11ShaderResourceView* src8, int w, int h, const std::vector<EffectInstance>& stack) {
        FxConstants c{};
        Target& A = floatTarget(w, h, 0);
        Target& B = floatTarget(w, h, 1);
        Target& BEFORE = floatTarget(w, h, 2);
        c.size[2] = float(w);
        c.size[3] = float(h);
        Target* cur = &A;
        pass(kToFloat, *cur, src8, nullptr, c);
        for (const EffectInstance& e : stack) {
            if (!e.problem.empty() || e.mix <= 0.0f) continue;
            if (e.def->isIdentity && e.def->isIdentity(e)) continue;
            const bool mixed = e.mix < 1.0f || !e.masks.empty();
            if (mixed) ctx_->CopyResource(BEFORE.tex.Get(), cur->tex.Get());
            Target* work = cur == &A ? &B : &A;
            Target* out = runEffect(e, *cur, *work, w, h);
            if (mixed) {
                FxConstants m{};
                m.size[2] = float(w);
                m.size[3] = float(h);
                m.mixp[0] = e.mix;
                m.mixp[1] = float(1.0 / std::max(1, h));
                m.info[1] = static_cast<int32_t>(e.masks.size());
                packMasks(m, e.masks);
                Target* dst = out == &A ? &B : &A;
                pass(kMix, *dst, out->srv.Get(), BEFORE.srv.Get(), m);
                out = dst;
            }
            cur = out;
        }
        Target& bytes = target(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        pass(kToBytes, bytes, cur->srv.Get(), nullptr, c);
        return bytes.srv.Get();
    }

    // One effect from `in` (float target); returns the target holding the result (`in` or `work`).
    Target* runEffect(const EffectInstance& e, Target& in, Target& work, int w, int h) {
        FxConstants c{};
        c.size[2] = float(w);
        c.size[3] = float(h);
        const std::string& id = e.def->id;
        if (id == "fa.color.basic") {
            const auto v = [&](const char* n) { return e.value(n) / 100.0f; };
            c.p0[0] = v("temperature");
            c.p0[1] = v("tint");
            c.p0[2] = v("exposure");
            c.p0[3] = v("brightness");
            c.p1[0] = v("contrast");
            c.p1[1] = v("highlights");
            c.p1[2] = v("shadows");
            c.p1[3] = v("whites");
            c.p2[0] = v("blacks");
            c.p2[1] = v("gamma");
            c.p2[2] = v("saturation");
            c.p2[3] = v("vibrance");
            pass(kBasic, work, in.srv.Get(), nullptr, c);
            return &work;
        }
        if (id == "fa.fx.blur.gaussian") {
            gaussian(in, work, w, h, e.value("radius") * float(h) / 1080.0f / 3.0f);
            return &in;
        }
        if (id == "fa.color.hsl") {
            static const char* bands[8] = {"red", "orange", "yellow", "green", "aqua", "blue", "purple", "magenta"};
            for (int i = 0; i < 8; ++i) {
                const std::string b = bands[i];
                c.pv[i] = e.value(b + ".hue") / 100.0f;
                c.pv[8 + i] = e.value(b + ".saturation") / 100.0f;
                c.pv[16 + i] = e.value(b + ".luminance") / 100.0f;
            }
            pass(kHsl, work, in.srv.Get(), nullptr, c);
            return &work;
        }
        if (id == "fa.color.wheels") {
            static const char* wh[3] = {"shadows", "midtones", "highlights"};
            for (int k = 0; k < 3; ++k) {
                const std::string b = wh[k];
                c.pv[k * 4 + 0] = e.value(b + ".r");
                c.pv[k * 4 + 1] = e.value(b + ".g");
                c.pv[k * 4 + 2] = e.value(b + ".b");
                c.pv[k * 4 + 3] = e.value(b + ".lum");
            }
            pass(kWheels, work, in.srv.Get(), nullptr, c);
            return &work;
        }
        if (id == "fa.color.curves") {
            if (!e.tables) return &in;
            pass(kCurves, work, in.srv.Get(), curveTexture(e.tables), c);
            return &work;
        }
        if (id == "fa.color.lut" || id == "fa.filter.look") {
            if (!e.tables || !e.tables->lut) return &in;
            const Lut3D& lut = *e.tables->lut;
            const LutTextures& t = lutTextures(e.tables->lut);
            c.p0[0] = e.value("strength") / 100.0f;
            c.p0[1] = e.value("domain") >= 0.5f ? 1.0f : 0.0f;
            c.p0[2] = float(lut.size);
            c.p0[3] = float(lut.size1d);
            for (int k = 0; k < 3; ++k) {
                c.p1[k] = lut.domainMin[static_cast<size_t>(k)];
                c.p2[k] = lut.domainMax[static_cast<size_t>(k)];
                c.p3[k] = lut.domainMin1d[static_cast<size_t>(k)];
                c.pv[k] = lut.domainMax1d[static_cast<size_t>(k)];
            }
            pass(kLut, work, in.srv.Get(), nullptr, c, t.table3.Get(), t.table1.Get());
            return &work;
        }
        const float scale = float(h) / 1080.0f;   // sizes are px at 1080 lines
        // premultiplied remap: in -> (premul) work -> (ps) in -> (unpremul) work
        auto premulPass = [&](FxPs ps, FxConstants& k) {
            pass(kPremul, work, in.srv.Get(), nullptr, k);
            pass(ps, in, work.srv.Get(), nullptr, k);
            pass(kUnpremul, work, in.srv.Get(), nullptr, k);
            return &work;
        };
        if (id == "fa.fx.blur.directional") {
            const float len = e.value("length") * scale, a = e.value("angle") * 3.14159265358979f / 180.0f;
            c.p0[0] = std::cos(a);
            c.p0[1] = -std::sin(a);
            c.p0[2] = len;
            c.info[0] = std::clamp(static_cast<int>(std::ceil(len)) + 1, 2, 64);
            return premulPass(kDirBlur, c);
        }
        if (id == "fa.fx.glow") {
            const float th = e.value("threshold") / 100.0f;
            c.p0[0] = th;
            c.p0[1] = std::max(1e-3f, 1.0f - th);
            c.p0[2] = e.value("intensity") / 100.0f;
            Target& g = floatTarget(w, h, 3);
            pass(kGlowBright, g, in.srv.Get(), nullptr, c);
            gaussian(g, work, w, h, e.value("radius") * scale / 3.0f);
            pass(kGlowCombine, work, in.srv.Get(), g.srv.Get(), c);
            return &work;
        }
        if (id == "fa.fx.pixelate") {
            const int b = std::max(1, static_cast<int>(std::lround(e.value("size") * scale)));
            if (b <= 1) return &in;
            c.info[0] = b;
            Target& blocks = floatTarget((w + b - 1) / b, (h + b - 1) / b, 0);   // ("small" is a Windows macro)
            pass(kBlockAvg, blocks, in.srv.Get(), nullptr, c);
            pass(kBlockUp, work, blocks.srv.Get(), nullptr, c);
            return &work;
        }
        if (id == "fa.fx.grain") {
            c.p0[0] = e.value("amount") / 100.0f;
            c.p0[1] = std::max(1.0f, e.value("size") * scale);
            c.p0[2] = e.value("color");
            c.info[1] = static_cast<int32_t>(e.seed);
            pass(kGrain, work, in.srv.Get(), nullptr, c);
            return &work;
        }
        if (id == "fa.fx.rgbsplit") {
            const float d = e.value("offset") * scale, a = e.value("angle") * 3.14159265358979f / 180.0f;
            c.p0[0] = std::cos(a) * d;
            c.p0[1] = -std::sin(a) * d;
            return premulPass(kRgbSplit, c);
        }
        if (id == "fa.fx.edge") {
            c.info[0] = std::max(1, static_cast<int>(std::lround(float(h) / 1080.0f)));
            c.info[1] = static_cast<int32_t>(std::lround(e.value("mode")));
            c.p0[0] = e.value("strength") / 100.0f;
            pass(kEdge, work, in.srv.Get(), nullptr, c);
            return &work;
        }
        if (id == "fa.fx.chromatic") {
            c.p0[0] = e.value("amount") / 100.0f * 0.02f;
            c.p0[1] = 0.5f * float(w);
            c.p0[2] = 0.5f * float(h);
            return premulPass(kChromatic, c);
        }
        if (id == "fa.fx.wave") {
            c.p0[0] = e.value("amplitude") * scale;
            c.p0[1] = std::max(1.0f, e.value("wavelength") * scale);
            c.p0[2] = e.value("speed") * static_cast<float>(e.seconds);
            c.p0[3] = e.value("direction");
            return premulPass(kWave, c);
        }
        if (id == "fa.fx.bulge") {
            c.p0[0] = std::max(1.0f, e.value("radius") / 100.0f * float(h));
            c.p0[1] = 0.5f * float(w) + e.value("centerX") / 100.0f * float(h) * 0.5f;
            c.p0[2] = 0.5f * float(h) + e.value("centerY") / 100.0f * float(h) * 0.5f;
            c.p0[3] = std::pow(2.0f, e.value("amount") / 100.0f);
            return premulPass(kBulge, c);
        }
        if (id == "fa.fx.chromakey") {
            const uint32_t rgb = static_cast<uint32_t>(std::lround(e.value("color")));
            const fxmath::ChromaKeyParams k = fxmath::chromaKeyParams(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f,
                                                                      (rgb & 255) / 255.0f, e.value("tolerance") / 100.0f,
                                                                      e.value("pedestal") / 100.0f);
            c.pv[0] = k.kb;
            c.pv[1] = k.kr;
            c.pv[2] = k.k2;
            c.pv[3] = k.ky;
            c.pv[4] = k.s;
            c.pv[5] = k.o;
            c.pv[6] = k.pedestal;
            c.pv[7] = e.value("spill") / 100.0f;
            Target& M = floatTarget(w, h, 3);
            Target& T = floatTarget(w, h, 4);
            pass(kKeyMatte, M, in.srv.Get(), nullptr, c);
            pass(kKeySpill, work, in.srv.Get(), nullptr, c);
            const int t = static_cast<int>(std::lround(e.value("thickness") * 0.5f * scale));
            if (t != 0) {
                FxConstants m = c;
                m.info[0] = std::abs(t);
                m.info[1] = t > 0 ? 1 : 0;
                m.info[2] = 0;
                pass(kMorph, T, M.srv.Get(), nullptr, m);
                m.info[2] = 1;
                pass(kMorph, M, T.srv.Get(), nullptr, m);
            }
            gaussianPlain(M, T, w, h, e.value("feather") * 0.17f * scale);
            FxConstants a = c;
            a.info[0] = e.value("alphaView") >= 0.5f ? 1 : 0;
            pass(kKeyApply, in, work.srv.Get(), M.srv.Get(), a);
            return &in;
        }
        if (id == "fa.filter.bw") {
            c.p0[0] = e.value("red") / 100.0f;
            c.p0[1] = e.value("green") / 100.0f;
            c.p0[2] = e.value("blue") / 100.0f;
            pass(kBw, work, in.srv.Get(), nullptr, c);
            return &work;
        }
        if (id == "fa.filter.sepia") {
            const uint32_t rgb = static_cast<uint32_t>(std::lround(e.value("tone")));
            c.p0[0] = ((rgb >> 16) & 255) / 255.0f;
            c.p0[1] = ((rgb >> 8) & 255) / 255.0f;
            c.p0[2] = (rgb & 255) / 255.0f;
            c.p0[3] = fxmath::luma(c.p0[0], c.p0[1], c.p0[2]);
            c.p1[0] = e.value("strength") / 100.0f;
            pass(kSepia, work, in.srv.Get(), nullptr, c);
            return &work;
        }
        if (id == "fa.fx.vignette") {
            c.p0[0] = e.value("amount") / 100.0f;
            c.p0[1] = e.value("size") / 100.0f;
            c.p0[2] = e.value("roundness") / 100.0f;
            c.p0[3] = e.value("feather") / 100.0f;
            pass(kVignette, work, in.srv.Get(), nullptr, c);
            return &work;
        }
        if (id == "fa.fx.sharpen") {
            Target& blurred = floatTarget(w, h, 3);
            ctx_->CopyResource(blurred.tex.Get(), in.tex.Get());
            gaussian(blurred, work, w, h, e.value("radius") * float(h) / 1080.0f);
            c.p0[0] = e.value("amount") / 100.0f;
            pass(kSharpen, work, in.srv.Get(), blurred.srv.Get(), c);
            return &work;
        }
        // ---- M11: constants computed exactly as the CPU reference computes them (effects_cpu.cpp)
        constexpr float kPiC = 3.14159265358979f;
        const float W = float(w), H = float(h);
        auto rgbOf = [&](const char* pid, float* out) {
            const uint32_t rgb = static_cast<uint32_t>(std::lround(e.value(pid)));
            out[0] = ((rgb >> 16) & 255) / 255.0f;
            out[1] = ((rgb >> 8) & 255) / 255.0f;
            out[2] = (rgb & 255) / 255.0f;
        };
        auto centre = [&](float* out) {
            out[0] = 0.5f * W + e.value("centerX") / 100.0f * H * 0.5f;
            out[1] = 0.5f * H + e.value("centerY") / 100.0f * H * 0.5f;
        };
        if (id == "fa.fx.mirror") {
            c.info[0] = static_cast<int32_t>(std::lround(e.value("mode")));
            return premulPass(kMirror, c);
        }
        if (id == "fa.fx.kaleidoscope") {
            const int n = std::max(2, static_cast<int>(std::lround(e.value("segments"))));
            c.p0[0] = 0.5f * W;
            c.p0[1] = 0.5f * H;
            c.p0[2] = std::max(0.05f, e.value("zoom") / 100.0f);
            c.p0[3] = e.value("angle") * kPiC / 180.0f;
            c.p1[0] = 2.0f * kPiC / static_cast<float>(n);
            return premulPass(kKaleido, c);
        }
        if (id == "fa.fx.swirl") {
            centre(c.p0);
            c.p0[2] = std::max(1.0f, e.value("radius") / 100.0f * H);
            c.p0[3] = e.value("angle") * kPiC / 180.0f;
            return premulPass(kSwirl, c);
        }
        if (id == "fa.fx.fisheye") {
            c.p0[0] = 0.5f * W;
            c.p0[1] = 0.5f * H;
            c.p0[2] = 0.5f * std::sqrt(W * W + H * H);
            c.p0[3] = e.value("amount") / 100.0f;
            return premulPass(kFisheye, c);
        }
        if (id == "fa.fx.blur.zoom") {
            centre(c.p0);
            const float k = e.value("amount") / 100.0f * 0.5f;
            c.p0[2] = k;
            c.info[0] = fxmath::zoomBlurSamples(k, w, h);
            return premulPass(kZoomBlur, c);
        }
        if (id == "fa.fx.tiltshift") {
            Target& blurred = floatTarget(w, h, 3);
            ctx_->CopyResource(blurred.tex.Get(), in.tex.Get());
            gaussian(blurred, work, w, h, e.value("blur") * H / 1080.0f / 3.0f);
            const float inner = e.value("width") / 200.0f;
            c.p0[0] = 0.5f + e.value("position") / 200.0f;
            c.p0[1] = inner;
            c.p0[2] = inner + e.value("feather") / 200.0f + 1e-3f;
            pass(kTiltMix, work, in.srv.Get(), blurred.srv.Get(), c);
            return &work;
        }
        if (id == "fa.fx.shake") {
            const fxmath::ShakeFrame f = fxmath::shakeFrame(e.seconds, e.value("amount") * H / 1080.0f, e.value("speed"),
                                                            e.value("rotation"), e.value("zoom"));
            c.p0[0] = 0.5f * W;
            c.p0[1] = 0.5f * H;
            c.p0[2] = f.ox;
            c.p0[3] = f.oy;
            c.p1[0] = f.cr;
            c.p1[1] = f.sr;
            c.p1[2] = f.zoom;
            return premulPass(kShake, c);
        }
        if (id == "fa.fx.glitch") {
            c.info[0] = static_cast<int32_t>(fxmath::glitchSlot(e.seconds, e.value("speed")));
            c.info[1] = std::max(2, static_cast<int>(std::lround(e.value("bands"))));
            c.p0[0] = e.value("amount") / 100.0f;
            c.p0[1] = W;
            c.p0[2] = H / 1080.0f;
            return premulPass(kGlitch, c);
        }
        if (id == "fa.fx.vhs") {
            const fxmath::VhsFrame f = fxmath::vhsFrame(e.seconds, h);
            const float sc = H / 1080.0f, bleed = e.value("amount") / 100.0f;
            c.p0[0] = bleed * 5.0f * sc;
            c.p0[1] = f.bandY;
            c.p0[2] = f.bandH;
            c.p0[3] = e.value("tracking") / 100.0f;
            c.p1[0] = sc;
            c.p1[1] = bleed;
            c.p1[2] = e.value("scanlines") / 100.0f;
            c.p1[3] = e.value("noise") / 100.0f;
            c.p2[0] = std::max(2.0f, std::round(3.0f * sc));
            c.info[0] = static_cast<int32_t>(f.slot);
            c.info[1] = static_cast<int32_t>(e.seed);
            premulPass(kVhsShift, c);   // straight result in `work`
            pass(kVhsTone, in, work.srv.Get(), nullptr, c);
            return &in;
        }
        if (id == "fa.fx.oldfilm") {
            const float sc = H / 1080.0f;
            const fxmath::FilmFrame f =
                fxmath::oldFilmFrame(e.seconds, w, sc, e.value("scratches") / 100.0f, e.value("flicker") / 100.0f);
            c.p0[0] = e.value("amount") / 100.0f;
            c.p0[1] = e.value("dust") / 100.0f;
            c.p0[2] = e.value("vignette") / 100.0f;
            c.p0[3] = f.flicker;
            c.p1[0] = 0.5f * W;
            c.p1[1] = 0.5f * H;
            c.p1[2] = 0.5f * std::sqrt(W * W + H * H);
            for (int i = 0; i < 4; ++i) {
                c.pv[i * 4 + 0] = f.x[i];
                c.pv[i * 4 + 1] = f.w[i];
                c.pv[i * 4 + 2] = f.o[i];
            }
            c.info[0] = static_cast<int32_t>(f.slot);
            c.info[1] = std::max(1, static_cast<int>(std::lround(3.0f * sc)));
            pass(kOldFilm, work, in.srv.Get(), nullptr, c);
            return &work;
        }
        if (id == "fa.fx.lightleak") {
            const fxmath::LeakFrame f = fxmath::lightLeakFrame(e.seconds, e.value("speed"), e.value("size"), w, h);
            c.p0[0] = f.x1;
            c.p0[1] = f.y1;
            c.p0[2] = f.s1;
            c.p0[3] = f.x2;
            c.p1[0] = f.y2;
            c.p1[1] = f.s2;
            c.p1[2] = e.value("amount") / 100.0f * f.pulse;
            rgbOf("color", c.p2);
            pass(kLightLeak, work, in.srv.Get(), nullptr, c);
            return &work;
        }
        if (id == "fa.fx.halftone") {
            const float a = e.value("angle") * kPiC / 180.0f;
            c.p0[0] = std::max(2.0f, e.value("size") * H / 1080.0f);
            c.p0[1] = std::cos(a);
            c.p0[2] = std::sin(a);
            c.p0[3] = static_cast<float>(std::lround(e.value("style")));
            pass(kHalftone, work, in.srv.Get(), nullptr, c);
            return &work;
        }
        if (id == "fa.fx.posterize") {
            c.p0[0] = static_cast<float>(std::max(2L, std::lround(e.value("levels")))) - 1.0f;
            pass(kPosterize, work, in.srv.Get(), nullptr, c);
            return &work;
        }
        if (id == "fa.fx.emboss") {
            const float a = e.value("angle") * kPiC / 180.0f, step = std::max(1.0f, H / 1080.0f);
            c.p0[0] = std::cos(a) * step;
            c.p0[1] = -std::sin(a) * step;
            c.p0[2] = e.value("strength") / 100.0f * 2.0f;
            c.p0[3] = e.value("style");
            pass(kEmboss, work, in.srv.Get(), nullptr, c);
            return &work;
        }
        if (id == "fa.fx.invert") {
            c.p0[0] = e.value("amount") / 100.0f;
            pass(kInvert, work, in.srv.Get(), nullptr, c);
            return &work;
        }
        if (id == "fa.fx.letterbox") {
            const float ra = fxmath::letterboxRatio(static_cast<int>(std::lround(e.value("ratio"))));
            c.p0[0] = ra >= W / H ? 0.5f * (H - W / ra) : 0.0f;
            c.p0[1] = ra >= W / H ? 0.0f : 0.5f * (W - H * ra);
            c.p0[2] = H;
            c.p0[3] = W;
            rgbOf("color", c.p1);
            c.p1[3] = e.value("opacity") / 100.0f;
            pass(kLetterbox, work, in.srv.Get(), nullptr, c);
            return &work;
        }
        if (id == "fa.fx.spotlight") {
            centre(c.p0);
            const float R = std::max(1.0f, e.value("size") / 100.0f * H);
            c.p0[2] = R;
            c.p0[3] = R * (1.0f + e.value("feather") / 100.0f) + 1.0f;
            c.p1[0] = e.value("darkness") / 100.0f;
            pass(kSpotlight, work, in.srv.Get(), nullptr, c);
            return &work;
        }
        if (id == "fa.filter.duotone") {
            rgbOf("shadows", c.p0);
            rgbOf("highlights", c.p1);
            c.p2[0] = e.value("strength") / 100.0f;
            pass(kDuotone, work, in.srv.Get(), nullptr, c);
            return &work;
        }
        throw std::runtime_error("no GPU passes for effect " + id);
    }

    // Gaussian blur in place on `img` (float, straight alpha); `work` is a scratch target of the same size.
    void gaussian(Target& img, Target& work, int w, int h, float sigma) {
        const fxmath::BlurPlan plan = fxmath::planGaussian(sigma);
        if (plan.factor == 0) return;
        FxConstants c{};
        c.size[2] = float(w);
        c.size[3] = float(h);
        pass(kPremul, work, img.srv.Get(), nullptr, c);
        for (size_t i = 0; i < plan.weights.size() && i < 64; ++i) c.weights[i] = plan.weights[i];
        c.info[0] = plan.radius;
        if (plan.factor == 1) {
            pass(kBlurH, img, work.srv.Get(), nullptr, c);
            pass(kBlurV, work, img.srv.Get(), nullptr, c);
        } else {
            const int sw = (w + plan.factor - 1) / plan.factor, sh = (h + plan.factor - 1) / plan.factor;
            Target& s0 = floatTarget(sw, sh, 0);
            Target& s1 = floatTarget(sw, sh, 1);
            FxConstants d = c;
            d.info[0] = plan.factor;
            pass(kDown, s0, work.srv.Get(), nullptr, d);
            c.size[2] = float(sw);
            c.size[3] = float(sh);
            pass(kBlurH, s1, s0.srv.Get(), nullptr, c);
            pass(kBlurV, s0, s1.srv.Get(), nullptr, c);
            FxConstants u = c;
            u.info[0] = plan.factor;
            pass(kUp, work, s0.srv.Get(), nullptr, u);
        }
        c.size[2] = float(w);
        c.size[3] = float(h);
        pass(kUnpremul, img, work.srv.Get(), nullptr, c);
    }

    // Gaussian of every channel without premultiplying (a matte held in all channels); result back in `img`.
    void gaussianPlain(Target& img, Target& work, int w, int h, float sigma) {
        const fxmath::BlurPlan plan = fxmath::planGaussian(sigma);
        if (plan.factor == 0) return;
        FxConstants c{};
        c.size[2] = float(w);
        c.size[3] = float(h);
        for (size_t i = 0; i < plan.weights.size() && i < 64; ++i) c.weights[i] = plan.weights[i];
        c.info[0] = plan.radius;
        if (plan.factor == 1) {
            pass(kBlurH, work, img.srv.Get(), nullptr, c);
            pass(kBlurV, img, work.srv.Get(), nullptr, c);
            return;
        }
        const int sw = (w + plan.factor - 1) / plan.factor, sh = (h + plan.factor - 1) / plan.factor;
        Target& s0 = floatTarget(sw, sh, 0);
        Target& s1 = floatTarget(sw, sh, 1);
        FxConstants d = c;
        d.info[0] = plan.factor;
        pass(kDown, s0, img.srv.Get(), nullptr, d);
        c.size[2] = float(sw);
        c.size[3] = float(sh);
        pass(kBlurH, s1, s0.srv.Get(), nullptr, c);
        pass(kBlurV, s0, s1.srv.Get(), nullptr, c);
        FxConstants u = c;
        u.info[0] = plan.factor;
        pass(kUp, img, s0.srv.Get(), nullptr, u);
    }

    // 1024 x 2 float texture of a Curves effect's tables (cached while the tables are alive).
    ID3D11ShaderResourceView* curveTexture(const std::shared_ptr<const EffectTables>& t) {
        for (auto& [tables, view] : curveTextures_)
            if (tables == t) return view.Get();
        const int n = EffectTables::kCurveSize;
        std::vector<float> data(static_cast<size_t>(n) * 2 * 4, 0.0f);
        for (int i = 0; i < n; ++i) {
            for (int k = 0; k < 4; ++k) data[static_cast<size_t>(i) * 4 + k] = t->tone[static_cast<size_t>(i)][static_cast<size_t>(k)];
            data[static_cast<size_t>(n + i) * 4 + 0] = t->hue[static_cast<size_t>(i)][0];
            data[static_cast<size_t>(n + i) * 4 + 1] = t->hue[static_cast<size_t>(i)][1];
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(n);
        td.Height = 2;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{data.data(), static_cast<UINT>(n * 16), 0};
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> view;
        check(dev_->CreateTexture2D(&td, &init, &tex), "CreateTexture2D (curves)");
        check(dev_->CreateShaderResourceView(tex.Get(), nullptr, &view), "CreateShaderResourceView (curves)");
        if (curveTextures_.size() >= 16) curveTextures_.erase(curveTextures_.begin());
        curveTextures_.push_back({t, view});
        return view.Get();
    }

    // A LUT's tables as textures: 3-D (size^3, float RGBA) and 1-D (size1d x 1); cached per table.
    struct LutTextures {
        ComPtr<ID3D11ShaderResourceView> table3, table1;
    };
    const LutTextures& lutTextures(const std::shared_ptr<const Lut3D>& lut) {
        for (auto& [l, t] : lutTextures_)
            if (l == lut) return t;
        LutTextures t;
        if (lut->size > 0) {
            const UINT n = static_cast<UINT>(lut->size);
            std::vector<float> data(static_cast<size_t>(n) * n * n * 4, 1.0f);
            for (size_t i = 0; i < static_cast<size_t>(n) * n * n; ++i)
                for (int k = 0; k < 3; ++k) data[i * 4 + static_cast<size_t>(k)] = lut->table[i * 3 + static_cast<size_t>(k)];
            D3D11_TEXTURE3D_DESC td{};
            td.Width = td.Height = td.Depth = n;
            td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{data.data(), n * 16, n * n * 16};
            ComPtr<ID3D11Texture3D> tex;
            check(dev_->CreateTexture3D(&td, &init, &tex), "CreateTexture3D (LUT)");
            check(dev_->CreateShaderResourceView(tex.Get(), nullptr, &t.table3), "CreateShaderResourceView (LUT)");
        }
        if (lut->size1d > 0) {
            const UINT n = static_cast<UINT>(lut->size1d);
            std::vector<float> data(static_cast<size_t>(n) * 4, 1.0f);
            for (size_t i = 0; i < n; ++i)
                for (int k = 0; k < 3; ++k) data[i * 4 + static_cast<size_t>(k)] = lut->table1d[i * 3 + static_cast<size_t>(k)];
            D3D11_TEXTURE2D_DESC td{};
            td.Width = n;
            td.Height = 1;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{data.data(), n * 16, 0};
            ComPtr<ID3D11Texture2D> tex;
            check(dev_->CreateTexture2D(&td, &init, &tex), "CreateTexture2D (1-D LUT)");
            check(dev_->CreateShaderResourceView(tex.Get(), nullptr, &t.table1), "CreateShaderResourceView (1-D LUT)");
        }
        if (lutTextures_.size() >= 8) lutTextures_.erase(lutTextures_.begin());
        lutTextures_.push_back({lut, t});
        return lutTextures_.back().second;
    }

    void drawAdjustment(const LayerDesc& l) {
        const double opacity = std::clamp(l.opacity, 0.0, 1.0);
        if (!l.effects || opacity <= 0.0 || effectsAreIdentity(*l.effects)) return;
        if (!stackOnGpu(*l.effects) || l.masks.size() > size_t(kMaxMasks)) {
            ++cpuEffectLayers_;
            compositeOnCpu(l);   // CPU reference on the read-back canvas
            return;
        }
        FxConstants m{};
        if (!packMasks(m, l.masks)) {   // too many polygon points
            ++cpuEffectLayers_;
            compositeOnCpu(l);
            return;
        }
        ID3D11ShaderResourceView* fx = runEffects(canvas_[cur_].srv.Get(), w_, h_, *l.effects);
        m.size[2] = float(w_);
        m.size[3] = float(h_);
        m.mixp[0] = float(opacity);
        m.mixp[1] = float(1.0 / std::max(1, h_));
        m.info[1] = static_cast<int32_t>(l.masks.size());
        const int next = cur_ ^ 1;
        Target dst;   // the next canvas as a pass target
        dst.w = w_;
        dst.h = h_;
        dst.rtv = canvas_[next].rtv;
        pass(kAdjustMix, dst, fx, canvas_[cur_].srv.Get(), m);
        cur_ = next;
        restoreCompositeState();
    }

    ComPtr<ID3D11PixelShader> fxPs_[kFxCount];
    ComPtr<ID3D11Buffer> fxCb_;
    std::deque<Target> targets_;
    std::vector<std::pair<std::shared_ptr<const EffectTables>, ComPtr<ID3D11ShaderResourceView>>> curveTextures_;
    std::deque<std::pair<std::shared_ptr<const Lut3D>, LutTextures>> lutTextures_;   // deque: references stay valid   // stable addresses: passes hold references to several targets
    Image cpuProcessed_;
    int64_t fxPasses_ = 0, cpuEffectLayers_ = 0;

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11Query> qDisjoint_, qBegin_, qEnd_;
    Canvas canvas_[2];
    ComPtr<ID3D11Texture2D> staging_;
    std::vector<Source> sources_;
    size_t nextSource_ = 0;
    int w_ = 0, h_ = 0, cur_ = 0;
    Image out_;
    std::string description_;
    bool timing_ = false;
    double gpuMs_ = -1.0;
};

}  // namespace

std::unique_ptr<RenderBackend> makeGpuBackend(std::string* why, bool preferHighPerformance) {
    // FA_GPU_PREFERENCE=power-saving|high-performance overrides the adapter choice
    if (const char* env = std::getenv("FA_GPU_PREFERENCE")) {
        const std::string v = env;
        if (v == "power-saving") preferHighPerformance = false;
        else if (v == "high-performance") preferHighPerformance = true;
    }
    try {
        return D3D11Backend::create(preferHighPerformance, why);
    } catch (const std::exception& e) {
        if (why) *why = e.what();
        return nullptr;
    }
}

std::string gpuDescription() {
    std::string why;
    auto b = D3D11Backend::create(true, &why);
    return b ? b->name().substr(5) : std::string();
}

std::vector<GpuAdapterInfo> listGpuAdapters() {
    std::vector<GpuAdapterInfo> out;
    const auto hp = adapters(true), lp = adapters(false);
    auto firstHardware = [](const std::vector<Adapter>& v) -> const Adapter* {
        for (const auto& a : v)
            if (!(a.desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return &a;
        return nullptr;
    };
    const Adapter* hpFirst = firstHardware(hp);
    const Adapter* lpFirst = firstHardware(lp);
    for (const auto& a : hp) {
        GpuAdapterInfo i;
        i.name = narrow(a.desc.Description);
        i.dedicatedVideoMemory = a.desc.DedicatedVideoMemory;
        i.software = (a.desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        i.highPerformanceChoice = hpFirst && std::memcmp(&hpFirst->desc.AdapterLuid, &a.desc.AdapterLuid, sizeof(LUID)) == 0;
        i.powerSavingChoice = lpFirst && std::memcmp(&lpFirst->desc.AdapterLuid, &a.desc.AdapterLuid, sizeof(LUID)) == 0;
        out.push_back(std::move(i));
    }
    return out;
}

}  // namespace fa

#endif  // FA_HAS_D3D11
