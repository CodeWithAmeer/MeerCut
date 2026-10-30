#include "fa/render/compositing.h"

#include <algorithm>
#include <cmath>

namespace fa {

void fitScale(int cw, int ch, int sw, int sh, LayerFit fit, double& sx, double& sy) {
    const double fx = double(cw) / sw, fy = double(ch) / sh;
    switch (fit) {
        case LayerFit::Fit: sx = sy = std::min(fx, fy); return;
        case LayerFit::Fill: sx = sy = std::max(fx, fy); return;
        case LayerFit::Stretch: sx = fx; sy = fy; return;
        case LayerFit::None: sx = sy = 1.0; return;
    }
    sx = sy = 1.0;
}

Affine2D Affine2D::fromParams(int cw, int ch, int sw, int sh, LayerFit fit, double posX, double posY, double scaleX,
                              double scaleY, double rotationDeg, double anchorX, double anchorY, bool flipH, bool flipV) {
    double bx = 1, by = 1;
    fitScale(cw, ch, sw, sh, fit, bx, by);
    const double lw = sw * bx, lh = sh * by;           // layer size on the canvas at 100 %
    const double ax = anchorX * lw, ay = anchorY * lh;  // pivot in layer pixels from the layer centre
    return translate(cw / 2.0 + posX * cw + ax, ch / 2.0 + posY * ch + ay) * rotateDeg(rotationDeg) *
           scale(scaleX, scaleY) * translate(-ax, -ay) * scale(flipH ? -1 : 1, flipV ? -1 : 1) * scale(bx, by) *
           translate(-sw / 2.0, -sh / 2.0);
}

Affine2D layerPlacement(const LayerDesc& l, int cw, int ch) {
    if (l.sourceToCanvas) return *l.sourceToCanvas;
    return Affine2D::fromParams(cw, ch, l.image->width, l.image->height, l.fit, 0, 0, 1, 1, 0, 0, 0, false, false);
}

float blendChannel(BlendMode m, float b, float s) {
    switch (m) {
        case BlendMode::Normal: return s;
        case BlendMode::Multiply: return b * s;
        case BlendMode::Screen: return b + s - b * s;
        case BlendMode::Overlay: return b <= 0.5f ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
        case BlendMode::Darken: return std::min(b, s);
        case BlendMode::Lighten: return std::max(b, s);
        case BlendMode::ColorDodge: return b <= 0 ? 0 : (s >= 1 ? 1 : std::min(1.0f, b / (1 - s)));
        case BlendMode::ColorBurn: return b >= 1 ? 1 : (s <= 0 ? 0 : 1 - std::min(1.0f, (1 - b) / s));
        case BlendMode::HardLight: return s <= 0.5f ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
        case BlendMode::SoftLight:   // Photoshop variant (EXP-011)
            return s <= 0.5f ? 2 * b * s + b * b * (1 - 2 * s) : 2 * b * (1 - s) + std::sqrt(b) * (2 * s - 1);
        case BlendMode::Difference: return std::fabs(b - s);
        case BlendMode::Exclusion: return b + s - 2 * b * s;
        case BlendMode::LinearDodge: return std::min(1.0f, b + s);
        case BlendMode::LinearBurn: return std::max(0.0f, b + s - 1);
    }
    return s;
}

namespace {

double lengthD(double x, double y) { return std::sqrt(x * x + y * y); }

double sdBox(double px, double py, double hx, double hy, double r) {
    const double qx = std::fabs(px) - hx + r, qy = std::fabs(py) - hy + r;
    return lengthD(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - r;
}

// Signed distance to an ellipse (Quilez' first-order approximation; exact on the axes).
double sdEllipse(double px, double py, double rx, double ry) {
    rx = std::max(rx, 1e-9);
    ry = std::max(ry, 1e-9);
    const double k0 = lengthD(px / rx, py / ry);
    const double k1 = lengthD(px / (rx * rx), py / (ry * ry));
    if (k1 < 1e-12) return -std::min(rx, ry);
    return k0 * (k0 - 1.0) / k1;
}

double sdPolygon(double px, double py, const std::vector<Vec2>& v, double sx, double sy) {
    const size_t n = v.size();
    double d2 = 1e300;
    bool inside = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double ax = v[i].x * sx, ay = v[i].y * sy, bx = v[j].x * sx, by = v[j].y * sy;
        const double ex = bx - ax, ey = by - ay, wx = px - ax, wy = py - ay;
        const double t = std::clamp((wx * ex + wy * ey) / std::max(ex * ex + ey * ey, 1e-18), 0.0, 1.0);
        const double dx = wx - ex * t, dy = wy - ey * t;
        d2 = std::min(d2, dx * dx + dy * dy);
        if (((ay > py) != (by > py)) && (px < (bx - ax) * (py - ay) / (by - ay) + ax)) inside = !inside;
    }
    const double d = std::sqrt(d2);
    return inside ? -d : d;
}

}  // namespace

float maskSignedDistance(const LayerMask& m, double qx, double qy) {
    // into the mask frame: translate to the centre, rotate by -rotation
    const double r = -m.rotationDeg * 3.14159265358979323846 / 180.0, cs = std::cos(r), sn = std::sin(r);
    const double x = qx - m.cx, y = qy - m.cy;
    const double px = cs * x - sn * y, py = sn * x + cs * y;
    switch (m.shape) {
        case MaskShape::Rectangle: {
            const double hx = std::fabs(m.w) / 2, hy = std::fabs(m.h) / 2;
            const double rad = std::clamp(m.cornerRadius, 0.0, 0.5) * 2 * std::min(hx, hy);
            return static_cast<float>(sdBox(px, py, hx, hy, rad));
        }
        case MaskShape::Ellipse: return static_cast<float>(sdEllipse(px, py, std::fabs(m.w) / 2, std::fabs(m.h) / 2));
        case MaskShape::Polygon:
            if (m.points.size() < 3) return 1e9f;
            return static_cast<float>(sdPolygon(px, py, m.points, m.w, m.h));
    }
    return 1e9f;
}

float maskCoverage(const std::vector<LayerMask>& masks, double qx, double qy, double pixelUnit) {
    if (masks.empty()) return 1.0f;
    float acc = masks.front().mode == MaskMode::Add ? 0.0f : 1.0f;
    for (const auto& m : masks) {
        const double sigma = m.feather > 0 ? m.feather : 0.5 * pixelUnit;
        const double d = maskSignedDistance(m, qx, qy);
        float cov = static_cast<float>(0.5 * std::erfc(d / (sigma * 1.41421356237309505)));
        if (m.inverted) cov = 1.0f - cov;
        const float v = cov * static_cast<float>(std::clamp(m.opacity, 0.0, 1.0));
        switch (m.mode) {
            case MaskMode::Add: acc = std::min(1.0f, acc + v); break;
            case MaskMode::Subtract: acc = std::max(0.0f, acc - v); break;
            case MaskMode::Intersect: acc = std::min(acc, v); break;
        }
    }
    return acc;
}

double wipeOrder(const WipeParams& w, double xn, double yn, double aspect) {
    constexpr double kTwoPi = 2.0 * 3.14159265358979323846;
    const double dx = (xn - 0.5) * aspect, dy = yn - 0.5;   // from the centre, in canvas heights
    const bool across = std::abs(w.dirX) >= std::abs(w.dirY);
    switch (w.shape) {
        case WipeShape::Circle: return std::sqrt(dx * dx + dy * dy) / std::sqrt(0.25 * aspect * aspect + 0.25);
        case WipeShape::Diamond: return (std::abs(dx) + std::abs(dy)) / (0.5 * aspect + 0.5);
        case WipeShape::Split: return 2.0 * std::abs((across ? xn : yn) - 0.5);
        case WipeShape::Clock: {
            double a = std::atan2(dx, -dy);   // 0 at 12 o'clock, growing clockwise
            if (a < 0) a += kTwoPi;
            return a / kTwoPi;
        }
        case WipeShape::Blinds: {
            const double f = (across ? xn : yn) * std::max(1, w.stripes);
            const double t = f - std::floor(f);
            return (across ? w.dirX : w.dirY) < 0 ? 1.0 - t : t;
        }
        case WipeShape::Linear: break;
    }
    // t = 0 at the edge where the reveal starts, 1 at the opposite edge
    return 0.5 + (xn - 0.5) * w.dirX + (yn - 0.5) * w.dirY;
}

float wipeFactor(const WipeParams& w, double xn, double yn, double aspect) {
    if (!w.active) return 1.0f;
    const double t = wipeOrder(w, xn, yn, aspect);
    const double soft = std::max(0.0, w.softness);
    if (soft <= 1e-9) return t < w.progress ? 1.0f : 0.0f;
    const double edge = w.progress * (1.0 + soft);
    return static_cast<float>(std::clamp((edge - t) / soft, 0.0, 1.0));
}

}  // namespace fa
