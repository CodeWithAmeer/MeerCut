#include "fa/vision/scene_detect.h"

#include "fa/media/media.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>

namespace fa::vision {

namespace {

constexpr int kBins = 16;

// 3 x 16-bin histograms (hue weighted by saturation x value, saturation, value), normalised to sum 1 each.
std::vector<float> histogram(const Image& img, double& meanLuma) {
    std::vector<float> h(3 * kBins, 0.0f);
    const size_t n = static_cast<size_t>(img.width) * img.height;
    double luma = 0, wsum = 0;
    for (size_t i = 0; i < n; ++i) {
        const float r = img.pixels[i * 4] / 255.0f, g = img.pixels[i * 4 + 1] / 255.0f, b = img.pixels[i * 4 + 2] / 255.0f;
        const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
        const float v = mx, s = mx > 0 ? d / mx : 0;
        float hue = 0;
        if (d > 1e-6f) {
            if (mx == r) hue = std::fmod((g - b) / d + 6.0f, 6.0f);
            else if (mx == g) hue = (b - r) / d + 2.0f;
            else hue = (r - g) / d + 4.0f;
            hue /= 6.0f;
        }
        // grey and dark pixels carry no (reliable) hue: in near-black pixels the hue is noise
        h[std::min(kBins - 1, static_cast<int>(hue * kBins))] += s * v;
        wsum += s * v;
        h[kBins + std::min(kBins - 1, static_cast<int>(s * kBins))] += 1;
        h[2 * kBins + std::min(kBins - 1, static_cast<int>(v * kBins))] += 1;
        luma += 0.2126 * r + 0.7152 * g + 0.0722 * b;
    }
    for (int k = 0; k < kBins; ++k) {
        h[k] = wsum > 0 ? static_cast<float>(h[k] / wsum) : 0.0f;
        h[kBins + k] /= static_cast<float>(n);
        h[2 * kBins + k] /= static_cast<float>(n);
    }
    meanLuma = n ? luma / n : 0;
    return h;
}

// Histogram distance 0..1: half the L1 distance per histogram, the hue part weighted by how colourful both are.
double distance(const std::vector<float>& a, const std::vector<float>& b) {
    double dh = 0, ds = 0, dv = 0;
    for (int k = 0; k < kBins; ++k) {
        dh += std::fabs(a[k] - b[k]);
        ds += std::fabs(a[kBins + k] - b[kBins + k]);
        dv += std::fabs(a[2 * kBins + k] - b[2 * kBins + k]);
    }
    return 0.4 * dh / 2 + 0.2 * ds / 2 + 0.4 * dv / 2;
}

// Hue and saturation only (0..1): unchanged by a fade or a brightness change, which only scale the values.
double chromaDistance(const std::vector<float>& a, const std::vector<float>& b) {
    double dh = 0, ds = 0;
    for (int k = 0; k < kBins; ++k) {
        dh += std::fabs(a[k] - b[k]);
        ds += std::fabs(a[kBins + k] - b[kBins + k]);
    }
    return 0.5 * dh / 2 + 0.5 * ds / 2;
}

Image shrink(const Image& src, int width) {
    const int w = std::max(8, std::min(width, src.width));
    const int h = std::max(4, static_cast<int>(std::lround(static_cast<double>(src.height) * w / src.width)));
    Image out(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // box average over the source pixels this one covers
            const int x0 = x * src.width / w, x1 = std::max(x0 + 1, (x + 1) * src.width / w);
            const int y0 = y * src.height / h, y1 = std::max(y0 + 1, (y + 1) * src.height / h);
            int acc[4] = {0, 0, 0, 0}, cnt = 0;
            for (int yy = y0; yy < y1; yy += std::max(1, (y1 - y0) / 4))
                for (int xx = x0; xx < x1; xx += std::max(1, (x1 - x0) / 4), ++cnt)
                    for (int c = 0; c < 4; ++c) acc[c] += src.pixels[(static_cast<size_t>(yy) * src.width + xx) * 4 + c];
            for (int c = 0; c < 4; ++c) out.pixels[(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<uint8_t>(acc[c] / std::max(1, cnt));
        }
    return out;
}

// The picture's structure: 64 x 36 grey (box average), zero mean and unit contrast (a fade or a brightness change
// leaves it as it is; a picture with almost no contrast - black, a flat colour - stays near zero).
constexpr int kSigW = 64, kSigH = 36;
std::vector<float> signature(const Image& img) {
    std::vector<float> g(kSigW * kSigH);
    for (int y = 0; y < kSigH; ++y)
        for (int x = 0; x < kSigW; ++x) {
            const int x0 = x * img.width / kSigW, x1 = std::max(x0 + 1, (x + 1) * img.width / kSigW);
            const int y0 = y * img.height / kSigH, y1 = std::max(y0 + 1, (y + 1) * img.height / kSigH);
            double sum = 0;
            int cnt = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx, ++cnt) {
                    const uint8_t* p = &img.pixels[(static_cast<size_t>(yy) * img.width + xx) * 4];
                    sum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
                }
            g[static_cast<size_t>(y) * kSigW + x] = static_cast<float>(sum / std::max(1, cnt) / 255.0);
        }
    double mean = 0, var = 0;
    for (float v : g) mean += v;
    mean /= g.size();
    for (float v : g) var += (v - mean) * (v - mean);
    const double scale = 1.0 / (std::sqrt(var / g.size()) + 0.02);
    for (float& v : g) v = static_cast<float>((v - mean) * scale);
    return g;
}

double structureDistance(const std::vector<float>& a, const std::vector<float>& b) {
    double s = 0;
    for (size_t i = 0; i < a.size(); ++i) s += std::fabs(a[i] - b[i]);
    return s / static_cast<double>(a.size());
}

}  // namespace

double frameDifference(const Image& a, const Image& b) {
    double la = 0, lb = 0;
    const Image sa = a.width > 320 ? shrink(a, 160) : a, sb = b.width > 320 ? shrink(b, 160) : b;
    return distance(histogram(sa, la), histogram(sb, lb));
}

SceneDetector::SceneDetector(const SceneOptions& o, double fps) : opt_(o), fps_(fps > 0 ? fps : 30.0) {}

void SceneDetector::push(const Image& frame) {
    const Image small = frame.width > opt_.analysisWidth * 2 ? shrink(frame, opt_.analysisWidth) : frame;
    double luma = 0;
    hist_.push_back(histogram(small, luma));
    luma_.push_back(luma);
    diff_.push_back(hist_.size() < 2 ? 0.0 : distance(hist_[hist_.size() - 2], hist_.back()));
    sig_.push_back(signature(small));
    sdiff_.push_back(sig_.size() < 2 ? 0.0 : structureDistance(sig_[sig_.size() - 2], sig_.back()));
    cdiff_.push_back(hist_.size() < 2 ? 0.0 : chromaDistance(hist_[hist_.size() - 2], hist_.back()));
}

std::vector<SceneBoundary> SceneDetector::finish() {
    std::vector<SceneBoundary> out;
    const int64_t n = static_cast<int64_t>(diff_.size());
    if (n < 2) return out;
    const double s = std::clamp(opt_.sensitivity, 0.0, 1.0);
    // thresholds (measured on the hand-checked Sintel trailer and the edited test sequences, TEST_RESULTS.md):
    const double absMin = 0.45 - 0.15 * s;     // structure jump of a cut: at least this (0.45 .. 0.30)
    const double rel = 2.2 - 0.6 * s;          // structure and colour: this many times their local level (2.2 .. 1.6)
    const double strong = 3.5 - 0.8 * s;       // structure alone, this many times the local level (3.5 .. 2.7)
    const double colourMin = 0.35 - 0.1 * s;   // pictures without structure (flat colours): a colour jump this large
    const int64_t win = std::max<int64_t>(8, static_cast<int64_t>(std::lround(fps_ * 0.5)));
    const int64_t minGap = std::max<int64_t>(1, static_cast<int64_t>(std::lround(opt_.minSceneSeconds * fps_)));
    // local level: median of a difference around i (excluding i and its neighbours), robust to motion
    auto localLevel = [&](const std::vector<double>& d, int64_t i) {
        std::vector<double> v;
        for (int64_t k = std::max<int64_t>(1, i - win); k <= std::min(n - 1, i + win); ++k)
            if (std::llabs(k - i) > 1) v.push_back(d[static_cast<size_t>(k)]);
        if (v.empty()) return 0.0;
        std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
        return v[v.size() / 2];
    };
    auto same = [&](int64_t a, int64_t b) {   // two frames show the same picture (a flash before / after)
        return structureDistance(sig_[static_cast<size_t>(a)], sig_[static_cast<size_t>(b)]) < 0.25 &&
               distance(hist_[static_cast<size_t>(a)], hist_[static_cast<size_t>(b)]) < 0.1;
    };
    for (int64_t i = 1; i < n; ++i) {
        const double sd = sdiff_[static_cast<size_t>(i)], hd = diff_[static_cast<size_t>(i)];
        const double sr = sd / (localLevel(sdiff_, i) + 0.05), hr = hd / (localLevel(diff_, i) + 0.02);
        const bool structural = sd >= absMin && !(i > 1 && sdiff_[static_cast<size_t>(i - 1)] > sd) &&
                                !(i + 1 < n && sdiff_[static_cast<size_t>(i + 1)] >= sd) &&
                                ((sr >= rel && hr >= rel) || sr >= strong);
        // flat pictures (a colour field, a title background) have no structure to compare: a large jump of hue /
        // saturation alone (a fade or a brightness change leaves those as they are)
        const double cd = cdiff_[static_cast<size_t>(i)], cr = cd / (localLevel(cdiff_, i) + 0.02);
        const bool lit = luma_[static_cast<size_t>(i - 1)] >= 0.06 && luma_[static_cast<size_t>(i)] >= 0.06;   // real colour
        const bool colour = lit && cd >= colourMin && cr >= strong && !(i > 1 && cdiff_[static_cast<size_t>(i - 1)] > cd) &&
                            !(i + 1 < n && cdiff_[static_cast<size_t>(i + 1)] >= cd);
        static const bool debug = std::getenv("MEERCUT_SCENE_DEBUG") != nullptr;   // every candidate's numbers (stderr)
        if (debug && (sd >= 0.3 || hd >= 0.2))
            std::fprintf(stderr, "frame %lld: structure %.3f (x%.2f), colour %.3f (x%.2f), hue/sat %.3f (x%.2f), luma %.3f -> %s\n",
                         static_cast<long long>(i), sd, sr, hd, hr, cd, cr, luma_[static_cast<size_t>(i)],
                         structural ? "cut" : colour ? "colour cut" : "-");
        if (!structural && !colour) continue;
        // flash: the picture before it comes back within a few frames - at the flash's start (i - 1 matches
        // i + 1 .. i + 3) or at its end (i matches i - 2 .. i - 4, the picture from before the flash)
        bool flash = false;
        for (int64_t k = 1; k <= 3 && !flash; ++k) {
            if (i + k < n && same(i - 1, i + k)) flash = true;
            if (i - 1 - k >= 0 && same(i - 1 - k, i)) flash = true;
        }
        if (flash) continue;
        out.push_back({i, BoundaryKind::Cut, std::max(sr, structural ? 0.0 : cr)});
    }
    if (opt_.fades) {
        // fade through black / white: a run of near-black (or near-white) frames between brighter (darker) ones
        for (int64_t i = 1; i < n - 1; ++i) {
            const bool dark = luma_[static_cast<size_t>(i)] < 0.04, bright = luma_[static_cast<size_t>(i)] > 0.96;
            if (!dark && !bright) continue;
            int64_t a = i, b = i;
            while (b + 1 < n && (dark ? luma_[static_cast<size_t>(b + 1)] < 0.04 : luma_[static_cast<size_t>(b + 1)] > 0.96)) ++b;
            // a real fade: the luma moves steadily towards the extreme over several frames before the run
            int64_t ramp = 0;
            for (int64_t k = a - 1; k >= std::max<int64_t>(0, a - 3 * win); --k) {
                const double prev = luma_[static_cast<size_t>(k + 1)], cur = luma_[static_cast<size_t>(k)];
                if (dark ? cur > prev + 1e-4 : cur < prev - 1e-4) ++ramp;
                else break;
            }
            if (ramp >= 3 && b + 1 < n) {
                const int64_t at = b + 1;   // the first frame after the black (white) run starts the new scene
                const bool nearCut = std::any_of(out.begin(), out.end(), [&](const SceneBoundary& c) {
                    return std::llabs(c.frame - at) < minGap;
                });
                if (!nearCut) out.push_back({at, BoundaryKind::Fade, 1.0});
            }
            i = b;
        }
        std::sort(out.begin(), out.end(), [](const SceneBoundary& x, const SceneBoundary& y) { return x.frame < y.frame; });
    }
    // minimum scene length: of two boundaries closer than minGap keep the stronger
    std::vector<SceneBoundary> kept;
    for (const SceneBoundary& b : out) {
        if (b.frame < minGap) continue;
        if (!kept.empty() && b.frame - kept.back().frame < minGap) {
            if (b.score > kept.back().score) kept.back() = b;
            continue;
        }
        kept.push_back(b);
    }
    return kept;
}

SceneResult detectScenes(const std::filesystem::path& media, const SceneOptions& o, int64_t firstFrame, int64_t lastFrame,
                         const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    const auto t0 = std::chrono::steady_clock::now();
    VideoDecoder::Options dopt;
    dopt.maxWidth = std::max(64, o.analysisWidth * 2);
    dopt.maxHeight = std::max(36, o.analysisWidth * 2);
    VideoDecoder dec(media, dopt);
    const auto& info = dec.info();
    const Rational fps = info.frameRate.fps();
    SceneResult r;
    r.fps = fps.toDouble();
    int64_t total = info.duration ? (*info.duration * fps).floor() : 0;
    if (lastFrame >= 0) total = std::min(total, lastFrame + 1);
    SceneDetector det(o, r.fps);
    for (int64_t f = std::max<int64_t>(0, firstFrame); f < total; ++f) {
        if (cancel && cancel->load()) break;
        const VideoFrame& vf = dec.frameAt(Time(f) / fps);
        det.push(vf.image);
        ++r.frames;
        if (progress && (f % 30 == 0)) progress(total > 0 ? double(f - firstFrame) / std::max<int64_t>(1, total - firstFrame) : 0);
    }
    r.boundaries = det.finish();
    for (auto& b : r.boundaries) b.frame += std::max<int64_t>(0, firstFrame);
    // thumbnails: decode the first frame of every scene again (seek), small
    {
        VideoDecoder thumbs(media, dopt);
        std::vector<int64_t> starts{std::max<int64_t>(0, firstFrame)};
        for (const auto& b : r.boundaries) starts.push_back(b.frame);
        for (int64_t s : starts) r.thumbnails.push_back(shrink(thumbs.frameAt(Time(s) / fps).image, 192));
    }
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (progress) progress(1.0);
    return r;
}

}  // namespace fa::vision
