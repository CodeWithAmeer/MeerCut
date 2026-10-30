#include "fa/render/curves.h"

#include <algorithm>
#include <cmath>

namespace fa {

std::vector<Vec2> normalizeCurve(std::vector<Vec2> pts) {
    for (auto& p : pts) {
        p.x = std::clamp(p.x, 0.0, 1.0);
        p.y = std::clamp(p.y, 0.0, 1.0);
    }
    std::stable_sort(pts.begin(), pts.end(), [](const Vec2& a, const Vec2& b) { return a.x < b.x; });
    std::vector<Vec2> out;
    for (const auto& p : pts) {
        if (!out.empty() && std::fabs(out.back().x - p.x) < 1e-9) out.back() = p;
        else out.push_back(p);
    }
    return out;
}

namespace {

// Cubic Bezier through knots k[0..n-1] (x strictly increasing) with tangents m.
double bezier(const std::vector<Vec2>& k, const std::vector<double>& m, size_t i, double x) {
    const double dx = k[i + 1].x - k[i].x;
    const double t = std::clamp((x - k[i].x) / dx, 0.0, 1.0), u = 1.0 - t;
    const double p1 = k[i].y + m[i] * dx / 3.0, p2 = k[i + 1].y - m[i + 1] * dx / 3.0;
    return u * u * u * k[i].y + 3.0 * u * u * t * p1 + 3.0 * u * t * t * p2 + t * t * t * k[i + 1].y;
}

std::vector<double> tangents(const std::vector<Vec2>& k, bool naturalEnds) {
    const size_t n = k.size();
    std::vector<double> m(n, 0.0);
    if (n == 2) {
        m[0] = m[1] = (k[1].y - k[0].y) / (k[1].x - k[0].x);
        return m;
    }
    for (size_t i = 1; i + 1 < n; ++i) m[i] = (k[i + 1].y - k[i - 1].y) / (k[i + 1].x - k[i - 1].x);
    if (naturalEnds) {
        m[0] = (3.0 * (k[1].y - k[0].y) / (k[1].x - k[0].x) - m[1]) / 2.0;
        m[n - 1] = (3.0 * (k[n - 1].y - k[n - 2].y) / (k[n - 1].x - k[n - 2].x) - m[n - 2]) / 2.0;
    }
    return m;
}

}  // namespace

double evaluateCurve(std::vector<Vec2> pts, double x, bool periodic) {
    pts = normalizeCurve(std::move(pts));
    if (periodic) {
        if (pts.empty()) return 0.5;
        if (pts.size() == 1) return pts[0].y;
        // wrap: one copy of the last point before 0 and of the first after 1 (and their neighbours for tangents)
        std::vector<Vec2> k;
        k.push_back({pts[pts.size() - 2].x - 1.0, pts[pts.size() - 2].y});
        k.push_back({pts.back().x - 1.0, pts.back().y});
        k.insert(k.end(), pts.begin(), pts.end());
        k.push_back({pts[0].x + 1.0, pts[0].y});
        k.push_back({pts[1].x + 1.0, pts[1].y});
        const std::vector<double> m = tangents(k, false);
        x -= std::floor(x);
        for (size_t i = 1; i + 2 < k.size(); ++i)
            if (x >= k[i].x && x <= k[i + 1].x) return std::clamp(bezier(k, m, i, x), 0.0, 1.0);
        return 0.5;
    }
    if (pts.empty()) return std::clamp(x, 0.0, 1.0);
    if (pts.size() == 1) return pts[0].y;
    if (x <= pts.front().x) return pts.front().y;
    if (x >= pts.back().x) return pts.back().y;
    const std::vector<double> m = tangents(pts, true);
    for (size_t i = 0; i + 1 < pts.size(); ++i)
        if (x <= pts[i + 1].x) return std::clamp(bezier(pts, m, i, x), 0.0, 1.0);
    return pts.back().y;
}

std::vector<float> curveTable(const std::vector<Vec2>& points, int n, bool periodic) {
    std::vector<float> t(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) t[static_cast<size_t>(i)] = static_cast<float>(evaluateCurve(points, double(i) / (n - 1), periodic));
    return t;
}

}  // namespace fa
