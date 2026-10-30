// Compositing semantics shared by all render backends (CPU reference and GPU).
//
// A layer is a source image placed on the canvas by an affine map, optionally cropped, masked,
// wiped (transition), darkened (brightness) and blended onto what is below:
//
//   for each canvas pixel centre P:
//     S   = sourceToCanvas^-1 (P)                       source pixel coordinates (pixel centres at +0.5)
//     src = bilinear(image, S) with texels outside the crop rectangle treated as transparent,
//           interpolated premultiplied, then un-premultiplied
//     m   = masks(q(S))                                  q = layer-height units from the layer centre
//     w   = wipe(P)                                      canvas-space linear reveal
//     a   = src.a * opacity * m * w
//     dst = dst * (1 - a) + B(dst, src.rgb * brightness) * a       (encoded RGB, 0..1, canvas opaque)
//
// B is the blend formula of the layer's mode (W3C / Photoshop definitions; Soft Light = Photoshop
// variant, matching Stage 1 EXP-011). Masks: signed distance d (negative inside) with a Gaussian
// edge profile coverage = 0.5 * erfc(d / (sigma * sqrt 2)), sigma = feather (or half a canvas pixel
// for a crisp, anti-aliased edge); inverted -> 1 - coverage; scaled by mask opacity; combined top to
// bottom: Add = min(1, acc + v), Subtract = max(0, acc - v), Intersect = min(acc, v); the first mask
// starts from 0 for Add and from 1 otherwise.
//
// M4: a layer may carry an effect stack (effects.h) that runs on its picture before placement; an adjustment layer
// has no picture: it runs its stack on the canvas composited so far and mixes the result back with
//   dst = dst + (fx(dst) - dst) * opacity * masks(q)     q = canvas-height units from the canvas centre
// (8-bit result, rounded), so everything below it is adjusted and nothing above.
#pragma once

#include "fa/core/image.h"
#include "fa/model/project.h"

#include <cmath>
#include <memory>
#include <optional>
#include <vector>

namespace fa {

enum class LayerFit { Fit, Fill, Stretch, None };   // how a source maps onto the canvas before transforms

// (x, y) -> (a x + b y + c, d x + e y + f)
struct Affine2D {
    double a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;

    Vec2 apply(double x, double y) const { return {a * x + b * y + c, d * x + e * y + f}; }
    Affine2D operator*(const Affine2D& o) const {   // this after o
        return {a * o.a + b * o.d, a * o.b + b * o.e, a * o.c + b * o.f + c,
                d * o.a + e * o.d, d * o.b + e * o.e, d * o.c + e * o.f + f};
    }
    double det() const { return a * e - b * d; }
    Affine2D inverse() const {
        const double k = det();
        if (std::fabs(k) < 1e-12) return {0, 0, -1e9, 0, 0, -1e9};   // degenerate: maps everything far away
        return {e / k, -b / k, (b * f - e * c) / k, -d / k, a / k, (d * c - a * f) / k};
    }
    static Affine2D translate(double x, double y) { return {1, 0, x, 0, 1, y}; }
    static Affine2D scale(double x, double y) { return {x, 0, 0, 0, y, 0}; }
    static Affine2D rotateDeg(double deg) {   // clockwise on screen (y points down)
        const double r = deg * 3.14159265358979323846 / 180.0, cs = std::cos(r), sn = std::sin(r);
        return {cs, -sn, 0, sn, cs, 0};
    }

    // Source pixel coordinates -> canvas pixel coordinates for a clip transform (see ClipTransform):
    //   fit the source into the canvas, flip, scale and rotate about the anchor, then position.
    // pos* are fractions of the canvas size, anchor* fractions of the layer size, rotation in degrees.
    static Affine2D fromParams(int canvasW, int canvasH, int srcW, int srcH, LayerFit fit, double posX, double posY,
                               double scaleX, double scaleY, double rotationDeg, double anchorX, double anchorY,
                               bool flipH, bool flipV);
    friend bool operator==(const Affine2D&, const Affine2D&) = default;
};

// Base fit scale factors (canvas px per source px) for a layer fit mode.
void fitScale(int canvasW, int canvasH, int srcW, int srcH, LayerFit fit, double& sx, double& sy);

struct LayerMask {
    MaskShape shape = MaskShape::Ellipse;
    MaskMode mode = MaskMode::Add;
    bool inverted = false;
    double cx = 0, cy = 0, w = 0.5, h = 0.5;   // layer heights
    double rotationDeg = 0;
    double feather = 0;                         // sigma, layer heights
    double opacity = 1;
    double cornerRadius = 0;                    // fraction of min(w, h)
    std::vector<Vec2> points;                   // polygon, unit box
};

// The order in which a wipe reveals the canvas (wipeOrder: 0 = first, 1 = last).
enum class WipeShape {
    Linear,    // a straight edge travelling along (dirX, dirY)
    Circle,    // from the centre outwards (iris)
    Diamond,   // from the centre outwards, |x| + |y|
    Split,     // from the centre line outwards (barn doors): vertical line when |dirX| > |dirY|
    Clock,     // a hand sweeping clockwise from 12 o'clock
    Blinds,    // `stripes` stripes opening together, across dirX / dirY
};

struct WipeParams {
    bool active = false;
    WipeShape shape = WipeShape::Linear;
    double dirX = -1, dirY = 0;   // the reveal travels this way (unit axis vector)
    double progress = 0;          // 0..1
    double softness = 0.1;        // edge width, fraction of the frame
    int stripes = 10;             // Blinds
};

struct EffectInstance;   // effects.h

struct LayerDesc {
    const Image* image = nullptr;                // straight-alpha RGBA8 source
    double opacity = 1.0;                        // 0..1, multiplies source alpha
    LayerFit fit = LayerFit::Fit;                // used when sourceToCanvas is not set
    std::optional<Affine2D> sourceToCanvas;      // placement; default = fit, centred, no transform
    double cropL = 0, cropT = 0, cropR = 0, cropB = 0;   // fractions of the source
    BlendMode blend = BlendMode::Normal;
    double brightness = 1.0;                     // RGB multiplier (dip-to-black transition)
    double flash = 0.0;                          // 0..1 towards white, after the brightness (dip-to-white, flash)
    std::vector<LayerMask> masks;
    WipeParams wipe;
    std::shared_ptr<const std::vector<EffectInstance>> effects;   // run on the picture first (or on the canvas)
    bool adjustment = false;                     // adjustment layer: no image; effects apply to the canvas
    // Non-zero: identifies the image's pixels (same key = same pixels), so a GPU backend can keep the uploaded
    // texture (static titles, a paused frame being graded). 0 = upload every time.
    uint64_t imageKey = 0;
};

// ---- per-pixel math shared by the CPU backend and mirrored in the GPU shaders ------------------

float blendChannel(BlendMode m, float b, float s);   // b = backdrop (dst), s = source; 0..1
float maskSignedDistance(const LayerMask& m, double qx, double qy);
// Combined mask coverage at layer-height coordinates (qx, qy); pixelUnit = one canvas pixel in layer heights.
float maskCoverage(const std::vector<LayerMask>& masks, double qx, double qy, double pixelUnit);
// xn, yn = canvas position 0..1; aspect = canvas width / height (the round shapes are round on the canvas)
double wipeOrder(const WipeParams& w, double xn, double yn, double aspect);
float wipeFactor(const WipeParams& w, double xn, double yn, double aspect);
// Placement used for a layer (explicit transform or the fit default).
Affine2D layerPlacement(const LayerDesc& l, int canvasW, int canvasH);

}  // namespace fa
