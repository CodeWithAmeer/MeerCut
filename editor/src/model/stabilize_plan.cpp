#include "fa/model/stabilize_plan.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace fa {

double stabilizeSigma(StabilizeLevel level) {
    switch (level) {
    case StabilizeLevel::Weak: return 4.0;
    case StabilizeLevel::Normal: return 12.0;
    case StabilizeLevel::Strong: return 30.0;
    }
    return 12.0;
}

const char* stabilizeLevelId(StabilizeLevel level) {
    switch (level) {
    case StabilizeLevel::Weak: return "weak";
    case StabilizeLevel::Normal: return "normal";
    case StabilizeLevel::Strong: return "strong";
    }
    return "normal";
}

std::optional<StabilizeLevel> stabilizeLevelFromId(const std::string& id) {
    if (id == "weak") return StabilizeLevel::Weak;
    if (id == "normal") return StabilizeLevel::Normal;
    if (id == "strong") return StabilizeLevel::Strong;
    return std::nullopt;
}

namespace {

std::vector<double> gaussian(const std::vector<double>& v, double sigma) {
    const size_t n = v.size();
    const int r = std::max(1, static_cast<int>(std::ceil(3 * sigma)));
    std::vector<double> kernel(2 * static_cast<size_t>(r) + 1);
    for (int k = -r; k <= r; ++k) kernel[static_cast<size_t>(k + r)] = std::exp(-0.5 * k * k / (sigma * sigma));
    std::vector<double> o(n);
    for (size_t i = 0; i < n; ++i) {
        double s = 0, w = 0;
        for (int k = -r; k <= r; ++k) {
            const long long j = static_cast<long long>(i) + k;
            if (j < 0 || j >= static_cast<long long>(n)) continue;
            s += v[static_cast<size_t>(j)] * kernel[static_cast<size_t>(k + r)];
            w += kernel[static_cast<size_t>(k + r)];
        }
        o[i] = s / w;
    }
    return o;
}

// Pixel-space correction of one frame.
struct Px {
    double dx = 0, dy = 0, da = 0;
};

// Does the picture (W x H, corrected by c and zoomed by z about the centre) cover the whole frame?
bool covers(const Px& c, double z, double W, double H) {
    const double cs = std::cos(c.da), sn = std::sin(c.da);
    for (const double qx : {-W / 2, W / 2})
        for (const double qy : {-H / 2, H / 2}) {
            // frame corner q comes from picture point R(-da) (q / z - d)
            const double ux = qx / z - c.dx, uy = qy / z - c.dy;
            const double vx = cs * ux + sn * uy, vy = -sn * ux + cs * uy;
            if (std::fabs(vx) > W / 2 + 1e-9 || std::fabs(vy) > H / 2 + 1e-9) return false;
        }
    return true;
}

// Smallest zoom that covers the frame (binary search; > limit when the limit is not enough).
double neededZoom(const Px& c, double W, double H, double limit) {
    if (covers(c, 1.0, W, H)) return 1.0;
    if (!covers(c, limit, W, H)) return limit * 2;
    double lo = 1.0, hi = limit;
    for (int it = 0; it < 40; ++it) {
        const double mid = (lo + hi) / 2;
        (covers(c, mid, W, H) ? hi : lo) = mid;
    }
    return hi;
}

}  // namespace

void planStabilization(Stabilization& s) {
    const size_t n = s.motion.size();
    s.corrections.assign(n, FrameCorrection{});
    s.zoom = 1.0;
    s.limitedShare = 0.0;
    if (n == 0 || s.width <= 0 || s.height <= 0) return;
    const double W = s.width, H = s.height;
    // camera path in pixels (accumulated motion of the picture content)
    std::vector<double> px(n, 0), py(n, 0), pa(n, 0);
    for (size_t i = 1; i < n; ++i) {
        const CameraMotion& m = s.motion[i];
        px[i] = px[i - 1] + (m.valid ? m.dx * W : 0);
        py[i] = py[i - 1] + (m.valid ? m.dy * H : 0);
        pa[i] = pa[i - 1] + (m.valid ? m.da : 0);
    }
    const double sigma = stabilizeSigma(s.level), k = std::clamp(s.strength, 0.0, 1.0);
    const auto sx = gaussian(px, sigma), sy = gaussian(py, sigma), sa = gaussian(pa, sigma);
    std::vector<Px> c(n);
    for (size_t i = 0; i < n; ++i) c[i] = {k * (sx[i] - px[i]), k * (sy[i] - py[i]), k * (sa[i] - pa[i])};

    if (s.autoZoom) {
        const double limit = std::clamp(s.maxZoom, 1.0, kMaxStabilizeZoom);
        double full = 1.0;
        for (const Px& p : c) full = std::max(full, neededZoom(p, W, H, limit));
        std::vector<char> limited(n, 0);   // frames whose correction the limit reduced
        if (full > limit) {
            // The full correction needs more zoom than allowed: keep the corrected camera path inside the band the zoom
            // leaves (|correction| <= headroom on each axis) and make it as smooth as the band allows - alternately
            // clamp it into the band and smooth it, then clamp once more. Where the band allows, the correction stays
            // complete; elsewhere the path follows the camera only as far as it must.
            const double hx = W / 2 * (1 - 1 / limit), hy = H / 2 * (1 - 1 / limit);
            std::vector<double> qx(n), qy(n);
            for (size_t i = 0; i < n; ++i) {
                qx[i] = px[i] + c[i].dx;
                qy[i] = py[i] + c[i].dy;
            }
            auto intoBand = [&] {
                for (size_t i = 0; i < n; ++i) {
                    qx[i] = std::clamp(qx[i], px[i] - hx, px[i] + hx);
                    qy[i] = std::clamp(qy[i], py[i] - hy, py[i] + hy);
                }
            };
            const double inner = std::max(2.0, sigma / 4);
            for (int it = 0; it < 40; ++it) {
                intoBand();
                qx = gaussian(qx, inner);
                qy = gaussian(qy, inner);
            }
            intoBand();
            for (size_t i = 0; i < n; ++i) {
                const Px banded{qx[i] - px[i], qy[i] - py[i], c[i].da};
                if (std::hypot(banded.dx - c[i].dx, banded.dy - c[i].dy) > 0.5) limited[i] = 1;
                c[i] = banded;
            }
            // rotation is not in the band: a frame whose rotation still needs more zoom gets a weaker correction,
            // eased in and out (minimum over +-9 frames, then a Gaussian of sigma 3 - never above what each frame
            // allows, because the minimum window covers the kernel's support)
            std::vector<double> share(n, 1.0);
            bool weaker = false;
            for (size_t i = 0; i < n; ++i) {
                if (neededZoom(c[i], W, H, limit) <= limit) continue;
                double lo = 0, hi = 1;
                for (int it = 0; it < 30; ++it) {
                    const double mid = (lo + hi) / 2;
                    (neededZoom({c[i].dx * mid, c[i].dy * mid, c[i].da * mid}, W, H, limit) <= limit ? lo : hi) = mid;
                }
                share[i] = lo;
                weaker = true;
            }
            if (weaker) {
                std::vector<double> eased(n);
                for (size_t i = 0; i < n; ++i) {
                    double m = 1.0;
                    for (long long j = static_cast<long long>(i) - 9; j <= static_cast<long long>(i) + 9; ++j)
                        if (j >= 0 && j < static_cast<long long>(n)) m = std::min(m, share[static_cast<size_t>(j)]);
                    eased[i] = m;
                }
                eased = gaussian(eased, 3.0);
                for (size_t i = 0; i < n; ++i) {
                    if (eased[i] < 0.999) limited[i] = 1;
                    c[i] = {c[i].dx * eased[i], c[i].dy * eased[i], c[i].da * eased[i]};
                }
            }
        }
        double z = 1.0;
        for (const Px& p : c) z = std::max(z, neededZoom(p, W, H, limit));
        s.zoom = std::min(z, limit);
        s.limitedShare = static_cast<double>(std::count(limited.begin(), limited.end(), 1)) / static_cast<double>(n);
    }
    for (size_t i = 0; i < n; ++i) s.corrections[i] = {c[i].dx / W, c[i].dy / H, c[i].da};
}

const FrameCorrection* stabilizationAt(const Stabilization& s, const Time& sourceTime) {
    if (s.corrections.empty()) return nullptr;
    const int64_t f = (sourceTime * s.frameRate).floor() - s.firstFrame;
    const int64_t last = static_cast<int64_t>(s.corrections.size()) - 1;
    return &s.corrections[static_cast<size_t>(std::clamp<int64_t>(f, 0, last))];
}

}  // namespace fa
