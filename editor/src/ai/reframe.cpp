#include "fa/ai/reframe.h"

#include "fa/ai/faces.h"
#include "fa/ai/models.h"
#include "fa/media/media.h"
#include "fa/vision/scene_detect.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace fa::ai {

namespace {

struct Box {
    double x0, y0, x1, y1;
    double w() const { return x1 - x0; }
    double h() const { return y1 - y0; }
};

// Zero-phase Gaussian smoothing of a sampled signal (sigma in samples), edges by clamping.
std::vector<double> gaussian(const std::vector<double>& v, double sigma) {
    if (sigma < 0.3 || v.size() < 3) return v;
    const int r = static_cast<int>(std::ceil(3 * sigma));
    std::vector<double> k(static_cast<size_t>(2 * r + 1));
    double sum = 0;
    for (int i = -r; i <= r; ++i) sum += k[static_cast<size_t>(i + r)] = std::exp(-0.5 * i * i / (sigma * sigma));
    std::vector<double> out(v.size());
    const int n = static_cast<int>(v.size());
    for (int i = 0; i < n; ++i) {
        double s = 0;
        for (int j = -r; j <= r; ++j) s += k[static_cast<size_t>(j + r)] * v[static_cast<size_t>(std::clamp(i + j, 0, n - 1))];
        out[static_cast<size_t>(i)] = s / sum;
    }
    return out;
}

// A camera that stays still while the target is within +-dead of it, and otherwise follows keeping the target at the
// edge of the dead zone.
std::vector<double> deadZone(const std::vector<double>& target, double dead) {
    std::vector<double> c(target.size());
    if (target.empty()) return c;
    double cam = target.front();
    for (size_t i = 0; i < target.size(); ++i) {
        if (target[i] > cam + dead) cam = target[i] - dead;
        else if (target[i] < cam - dead) cam = target[i] + dead;
        c[i] = cam;
    }
    return c;
}

// Ramer-Douglas-Peucker: the indices of the samples needed to keep a polyline within `tol`.
void rdp(const std::vector<double>& t, const std::vector<double>& v, size_t a, size_t b, double tol, std::vector<bool>& keep) {
    if (b <= a + 1) return;
    double worst = -1;
    size_t at = a;
    for (size_t i = a + 1; i < b; ++i) {
        const double u = (t[i] - t[a]) / std::max(1e-12, t[b] - t[a]);
        const double d = std::abs(v[i] - (v[a] + u * (v[b] - v[a])));
        if (d > worst) {
            worst = d;
            at = i;
        }
    }
    if (worst > tol) {
        keep[at] = true;
        rdp(t, v, a, at, tol, keep);
        rdp(t, v, at, b, tol, keep);
    }
}

std::pair<double, double> fitSize(int canvasW, int canvasH, int srcW, int srcH) {
    const double as = static_cast<double>(srcW) / srcH, ac = static_cast<double>(canvasW) / canvasH;
    if (as > ac) return {static_cast<double>(canvasW), canvasW / as};
    return {canvasH * as, static_cast<double>(canvasH)};
}

}  // namespace

std::pair<int, int> reframeCanvas(int seqWidth, int seqHeight, double aspect) {
    const int shortSide = std::max(2, std::min(seqWidth, seqHeight));
    auto even = [](double x) { return std::max(2, static_cast<int>(std::lround(x / 2)) * 2); };
    if (aspect >= 1) return {even(shortSide * aspect), even(shortSide)};
    return {even(shortSide), even(shortSide / aspect)};
}

ReframePath analyseReframe(const std::filesystem::path& media, Time sourceIn, Time sourceOut, const ReframeOptions& o,
                           const MotionTrack* track, const std::function<void(double)>& progress,
                           const std::atomic<bool>* cancel) {
    const auto t0 = std::chrono::steady_clock::now();
    ReframePath path;
    VideoDecoder dec(media);
    const VideoStreamInfo& info = dec.info();
    path.sourceWidth = info.width;
    path.sourceHeight = info.height;
    const double fps = info.frameRate.toDouble();
    path.fps = fps;
    const double from = sourceIn.toDouble(), to = sourceOut.toDouble();
    const int64_t firstFrame = static_cast<int64_t>(std::floor(from * fps + 1e-6));
    const int64_t lastFrame = std::max(firstFrame, static_cast<int64_t>(std::ceil(to * fps - 1e-6)) - 1);
    const int every = std::max(1, static_cast<int>(std::lround(o.sampleSeconds * fps)));
    std::unique_ptr<FaceDetector> faces;
    if (o.subject == ReframeSubject::Faces) {
        faces = std::make_unique<FaceDetector>(o.accelerator, 640);
        path.provider = faces->provider();
    }
    vision::SceneOptions so;
    so.sensitivity = 0.5;
    so.minSceneSeconds = 0.5;
    vision::SceneDetector scenes(so, fps);
    std::optional<std::pair<double, double>> previous;
    const double cropW = std::min(1.0, (o.aspect / (static_cast<double>(info.width) / info.height)));
    for (int64_t f = firstFrame; f <= lastFrame; ++f) {
        if (cancel && cancel->load()) break;
        const Image& pic = dec.frameAt(Time(f) / info.frameRate.fps()).image;
        scenes.push(pic);
        if ((f - firstFrame) % every == 0 || f == lastFrame) {
            ReframeSample s;
            s.t = std::max(0.0, f / fps - from);
            if (o.subject == ReframeSubject::FocalPoint) {
                s.x = o.focusX;
                s.y = o.focusY;
                s.found = true;
            } else if (o.subject == ReframeSubject::MotionTrack) {
                if (track) {
                    const TrackingSample* best = nullptr;
                    for (const auto& ts : track->samples)
                        if (!best || std::abs(ts.frame - f) < std::abs(best->frame - f)) best = &ts;
                    if (best && !best->lost && std::abs(best->frame - f) <= every) {
                        s.x = best->x;
                        s.y = best->y;
                        s.found = true;
                    }
                }
            } else {
                auto found = faces->detect(pic, 0.6);
                if (!found.empty()) {
                    // the main face: the one nearest the subject so far (identity), else the largest
                    auto weight = [&](const Face& a) {
                        double w = a.w * a.h * a.score;
                        if (previous) {
                            const double d = std::hypot(a.x + a.w / 2 - previous->first, a.y + a.h / 2 - previous->second);
                            w /= (1 + 25 * d * d);
                        }
                        return w;
                    };
                    std::sort(found.begin(), found.end(), [&](const Face& a, const Face& b) { return weight(a) > weight(b); });
                    Box b{found[0].x, found[0].y, found[0].x + found[0].w, found[0].y + found[0].h};
                    // others that fit in the crop with the main face join it (two people talking stay together)
                    for (size_t i = 1; i < found.size(); ++i) {
                        const Face& g = found[i];
                        if (g.w * g.h < 0.25 * found[0].w * found[0].h) continue;
                        Box u{std::min(b.x0, g.x), std::min(b.y0, g.y), std::max(b.x1, g.x + g.w), std::max(b.y1, g.y + g.h)};
                        if (u.w() <= cropW * (1 - 2 * o.margin)) b = u;
                    }
                    s.x = (b.x0 + b.x1) / 2;
                    // head room: the face sits in the upper part of the frame, the body below it
                    s.y = std::min(1.0, (b.y0 + b.y1) / 2 + 0.5 * found[0].h);
                    s.size = found[0].h;
                    s.found = true;
                    previous = std::make_pair(s.x, (b.y0 + b.y1) / 2);
                }
            }
            path.found += s.found;
            path.samples.push_back(s);
        }
        if (progress) progress(static_cast<double>(f - firstFrame + 1) / (lastFrame - firstFrame + 1));
    }
    for (const auto& b : scenes.finish()) path.cuts.push_back(b.frame / fps);   // frames counted from firstFrame
    planReframe(path, o);
    path.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return path;
}

void planReframe(ReframePath& path, const ReframeOptions& o) {
    const double as = static_cast<double>(path.sourceWidth) / std::max(1, path.sourceHeight);
    const double zoom = std::max(1.0, o.zoom);
    if (o.aspect < as) {
        path.cropHeight = 1 / zoom;
        path.cropWidth = o.aspect / as / zoom;
    } else {
        path.cropWidth = 1 / zoom;
        path.cropHeight = as / o.aspect / zoom;
    }
    const size_t n = path.samples.size();
    path.centre.assign(n, {o.focusX, o.focusY});
    if (!n) return;
    // targets: a lost subject holds its last place for holdSeconds, then the focal point
    std::vector<double> tx(n), ty(n), times(n);
    std::optional<size_t> lastFound;
    for (size_t i = 0; i < n; ++i) {
        const ReframeSample& s = path.samples[i];
        times[i] = s.t;
        // a shot change forgets the subject of the previous shot
        if (lastFound && std::any_of(path.cuts.begin(), path.cuts.end(),
                                     [&](double c) { return c > path.samples[*lastFound].t && c <= s.t; }))
            lastFound.reset();
        if (s.found) {
            tx[i] = s.x;
            ty[i] = s.y;
            lastFound = i;
        } else if (lastFound && s.t - path.samples[*lastFound].t <= o.holdSeconds) {
            tx[i] = path.samples[*lastFound].x;
            ty[i] = path.samples[*lastFound].y;
        } else {
            tx[i] = o.focusX;
            ty[i] = o.focusY;
        }
    }
    const double dt = n > 1 ? std::max(1e-3, (times.back() - times.front()) / (n - 1)) : 0.2;
    const double sigma = (0.15 + 1.2 * o.smoothness) / dt;   // samples
    const double halfW = path.cropWidth / 2, halfH = path.cropHeight / 2;
    // each shot on its own (a cut is a cut, not a pan)
    size_t a = 0;
    while (a < n) {
        size_t b = a + 1;
        while (b < n && !std::any_of(path.cuts.begin(), path.cuts.end(),
                                     [&](double c) { return c > times[b - 1] && c <= times[b]; }))
            ++b;
        for (int axis = 0; axis < 2; ++axis) {
            const std::vector<double>& target = axis == 0 ? tx : ty;
            const double half = axis == 0 ? halfW : halfH;
            std::vector<double> seg(target.begin() + static_cast<std::ptrdiff_t>(a), target.begin() + static_cast<std::ptrdiff_t>(b));
            seg = gaussian(seg, sigma * 0.5);
            seg = deadZone(seg, half * (0.05 + 0.3 * o.smoothness));
            seg = gaussian(seg, sigma);
            for (size_t i = 0; i < seg.size(); ++i) {
                const ReframeSample& s = path.samples[a + i];
                double c = seg[i];
                // the subject stays inside the margins (fast moves win over smoothness)
                if (s.found) {
                    const double room = half * (1 - 2 * o.margin);
                    const double p = axis == 0 ? s.x : s.y;
                    c = std::clamp(c, p - room, p + room);
                }
                c = std::clamp(c, half, 1 - half);   // inside the picture
                if (axis == 0) path.centre[a + i].first = c;
                else path.centre[a + i].second = c;
            }
        }
        a = b;
    }
}

ClipTransform reframeTransform(const ReframePath& path, Time sourceIn, int canvasW, int canvasH, const ClipTransform& base,
                               double tolerance) {
    ClipTransform t = base;
    const auto [fw, fh] = fitSize(canvasW, canvasH, path.sourceWidth, path.sourceHeight);
    const double scale = canvasW / (fw * path.cropWidth);
    t.scaleX = Animatable<double>(scale);
    t.scaleY = Animatable<double>(scale);
    t.uniformScale = true;
    t.anchorX = Animatable<double>(0.0);
    t.anchorY = Animatable<double>(0.0);
    (void)fh;
    const size_t n = path.centre.size();
    std::vector<double> times(n), px(n), py(n);
    for (size_t i = 0; i < n; ++i) {
        times[i] = path.samples[i].t;
        px[i] = -(path.centre[i].first - 0.5) / path.cropWidth;
        py[i] = -(path.centre[i].second - 0.5) / path.cropHeight;
    }
    // the shots: [bounds[k], bounds[k + 1]) in samples; a shot after a cut starts on the cut itself
    std::vector<size_t> bounds = {0};
    std::vector<double> keyTime = times;
    for (size_t i = 1; i < n; ++i)
        for (double c : path.cuts)
            if (c > times[i - 1] && c <= times[i]) {
                bounds.push_back(i);
                keyTime[i] = std::max(times[i - 1] + 1e-3, c - 0.25 / std::max(1.0, path.fps));   // safely before the cut frame
                break;
            }
    bounds.push_back(n);
    auto build = [&](const std::vector<double>& v, double tol) {
        Animatable<double> a(n ? v.front() : 0.0);
        if (n < 2) return a;
        const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
        if (*hi - *lo <= tol) {
            a.setConstant((*hi + *lo) / 2);
            return a;
        }
        std::vector<bool> keep(n, false), hold(n, false);
        for (size_t k = 0; k + 1 < bounds.size(); ++k) {
            const size_t first = bounds[k], last = bounds[k + 1] - 1;
            keep[first] = keep[last] = true;
            hold[last] = k + 2 < bounds.size();   // the last key of a shot holds until the next shot's first
            rdp(times, v, first, last, tol, keep);
        }
        for (size_t i = 0; i < n; ++i) {
            if (!keep[i]) continue;
            Keyframe<double> key;
            key.time = sourceIn + Rational::fromDouble(keyTime[i], 100000);
            key.value = v[i];
            key.interp = hold[i] ? Interpolation::Hold : Interpolation::Linear;
            a.setKey(key);
        }
        return a;
    };
    t.positionX = build(px, tolerance);
    t.positionY = build(py, tolerance * canvasW / std::max(1, canvasH));
    return t;
}

CropWindow cropWindowAt(const ClipTransform& t, double s, int srcW, int srcH, int canvasW, int canvasH) {
    const auto [fw, fh] = fitSize(canvasW, canvasH, srcW, srcH);
    const double sx = t.scaleX.value(s), sy = t.uniformScale ? sx : t.scaleY.value(s);
    CropWindow c;
    c.w = canvasW / (fw * sx);
    c.h = canvasH / (fh * sy);
    c.x = 0.5 - t.positionX.value(s) * c.w;
    c.y = 0.5 - t.positionY.value(s) * c.h;
    return c;
}

}  // namespace fa::ai
