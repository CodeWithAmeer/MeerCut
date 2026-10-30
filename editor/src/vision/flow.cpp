#include "fa/vision/flow.h"

#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace fa::vision {

struct FlowField {
    int width = 0, height = 0;   // the frames' size
    int workW = 0, workH = 0;    // the flows' size
    double scale = 1;            // frame pixels per working pixel
    cv::Mat ab, ba;              // CV_32FC2, working pixels
    cv::Mat trust;               // CV_32F, 0..1 (forward-backward consistency)
    double meanMotion = 0, fallbackShare = 0, milliseconds = 0;
};

namespace {

cv::Mat wrap(const Image& img) {   // RGBA8, no copy
    return cv::Mat(img.height, img.width, CV_8UC4, const_cast<uint8_t*>(img.pixels.data()), static_cast<size_t>(img.stride()));
}

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

std::shared_ptr<const FlowField> estimateFlow(const Image& a, const Image& b, FlowQuality quality, FlowStats* stats) {
    if (a.width != b.width || a.height != b.height || a.width < 16 || a.height < 16) return nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        auto f = std::make_shared<FlowField>();
        f->width = a.width;
        f->height = a.height;
        f->workW = std::min(a.width, quality == FlowQuality::Fast ? 480 : 960);
        f->scale = static_cast<double>(a.width) / f->workW;
        f->workH = std::max(8, static_cast<int>(std::lround(a.height / f->scale)));
        const int workW = f->workW, workH = f->workH;
        cv::Mat ga, gb, sa, sb;
        cv::cvtColor(wrap(a), ga, cv::COLOR_RGBA2GRAY);
        cv::cvtColor(wrap(b), gb, cv::COLOR_RGBA2GRAY);
        cv::resize(ga, sa, cv::Size(workW, workH), 0, 0, cv::INTER_AREA);
        cv::resize(gb, sb, cv::Size(workW, workH), 0, 0, cv::INTER_AREA);
        auto dis = cv::DISOpticalFlow::create(quality == FlowQuality::Fast ? cv::DISOpticalFlow::PRESET_FAST
                                                                         : cv::DISOpticalFlow::PRESET_MEDIUM);
        dis->calc(sa, sb, f->ab);
        dis->calc(sb, sa, f->ba);
        // forward-backward consistency at the working size: |ab(p) + ba(p + ab(p))|
        f->trust.create(workH, workW, CV_32F);
        double motion = 0;
        int bad = 0;
        for (int y = 0; y < workH; ++y) {
            const cv::Vec2f* fw = f->ab.ptr<cv::Vec2f>(y);
            float* c = f->trust.ptr<float>(y);
            for (int x = 0; x < workW; ++x) {
                const int qx = std::clamp(static_cast<int>(std::lround(x + fw[x][0])), 0, workW - 1);
                const int qy = std::clamp(static_cast<int>(std::lround(y + fw[x][1])), 0, workH - 1);
                const cv::Vec2f g = f->ba.at<cv::Vec2f>(qy, qx);
                const float e = std::hypot(fw[x][0] + g[0], fw[x][1] + g[1]);
                // full trust below 0.5 px of disagreement, none above 1.5 px (working pixels)
                c[x] = std::clamp(1.5f - e, 0.0f, 1.0f);
                bad += c[x] < 0.5f;
                motion += std::hypot(fw[x][0], fw[x][1]);
            }
        }
        f->meanMotion = motion / (static_cast<double>(workW) * workH) * f->scale;
        f->fallbackShare = static_cast<double>(bad) / (static_cast<double>(workW) * workH);
        f->milliseconds = msSince(t0);
        if (stats) {
            stats->meanMotion = f->meanMotion;
            stats->fallbackShare = f->fallbackShare;
            stats->milliseconds = stats->flowMilliseconds = f->milliseconds;
        }
        return f;
    } catch (const cv::Exception&) {
        return nullptr;
    }
}

bool warpWithFlow(const Image& a, const Image& b, const FlowField& flow, double weight, Image& out, FlowStats* stats) {
    if (a.width != b.width || a.height != b.height || a.width != flow.width || a.height != flow.height) return false;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        // One pass at full size (no full-size flow, map or blend buffers): per output pixel, the working-size flows and
        // trust are sampled bilinearly (as an INTER_LINEAR resize would: source = (x + 0.5) / scale - 0.5, edges
        // replicated), both frames are sampled where the pixel was / will be, and the warped mix is blended with the
        // plain mix by the trust:  out = T * (wb A(p - wa FA) + wa B(p - wb FB)) + (1 - T) * (wb A(p) + wa B(p)).
        const int W = a.width, H = a.height, workW = flow.workW, workH = flow.workH;
        const float wa = static_cast<float>(std::clamp(weight, 0.0, 1.0)), wb = 1.0f - wa, s = static_cast<float>(flow.scale);
        std::vector<int> fx0(static_cast<size_t>(W)), fx1(static_cast<size_t>(W));
        std::vector<float> ffx(static_cast<size_t>(W));
        for (int x = 0; x < W; ++x) {   // horizontal sampling positions in the working-size maps (same for every row)
            const float u = std::clamp((x + 0.5f) / s - 0.5f, 0.0f, workW - 1.0f);
            fx0[static_cast<size_t>(x)] = static_cast<int>(u);
            fx1[static_cast<size_t>(x)] = std::min(fx0[static_cast<size_t>(x)] + 1, workW - 1);
            ffx[static_cast<size_t>(x)] = u - static_cast<float>(fx0[static_cast<size_t>(x)]);
        }
        Image result(W, H);
        const uint8_t* pa = a.pixels.data();
        const uint8_t* pb = b.pixels.data();
        uint8_t* po = result.pixels.data();
        const size_t stride = static_cast<size_t>(W) * 4;
        auto sample = [&](const uint8_t* img, float x, float y, float* c) {   // bilinear RGBA, edges replicated
            x = std::clamp(x, 0.0f, W - 1.0f);
            y = std::clamp(y, 0.0f, H - 1.0f);
            const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
            const int x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
            const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
            const uint8_t* r0 = img + static_cast<size_t>(y0) * stride;
            const uint8_t* r1 = img + static_cast<size_t>(y1) * stride;
            for (int k = 0; k < 4; ++k) {
                const float top = r0[x0 * 4 + k] + (r0[x1 * 4 + k] - r0[x0 * 4 + k]) * fx;
                const float bot = r1[x0 * 4 + k] + (r1[x1 * 4 + k] - r1[x0 * 4 + k]) * fx;
                c[k] = top + (bot - top) * fy;
            }
        };
        cv::parallel_for_(cv::Range(0, H), [&](const cv::Range& rows) {
            for (int y = rows.start; y < rows.end; ++y) {
                const float v = std::clamp((y + 0.5f) / s - 0.5f, 0.0f, workH - 1.0f);
                const int y0 = static_cast<int>(v), y1 = std::min(y0 + 1, workH - 1);
                const float fy = v - static_cast<float>(y0);
                const cv::Vec2f* fa0 = flow.ab.ptr<cv::Vec2f>(y0);
                const cv::Vec2f* fa1 = flow.ab.ptr<cv::Vec2f>(y1);
                const cv::Vec2f* fb0 = flow.ba.ptr<cv::Vec2f>(y0);
                const cv::Vec2f* fb1 = flow.ba.ptr<cv::Vec2f>(y1);
                const float* t0r = flow.trust.ptr<float>(y0);
                const float* t1r = flow.trust.ptr<float>(y1);
                const uint8_t* ra = pa + static_cast<size_t>(y) * stride;
                const uint8_t* rb = pb + static_cast<size_t>(y) * stride;
                uint8_t* ro = po + static_cast<size_t>(y) * stride;
                for (int x = 0; x < W; ++x) {
                    const int x0 = fx0[static_cast<size_t>(x)], x1 = fx1[static_cast<size_t>(x)];
                    const float fx = ffx[static_cast<size_t>(x)];
                    auto lerp2 = [&](const cv::Vec2f* r0, const cv::Vec2f* r1) {
                        const cv::Vec2f top = r0[x0] + (r0[x1] - r0[x0]) * fx;
                        const cv::Vec2f bot = r1[x0] + (r1[x1] - r1[x0]) * fx;
                        return top + (bot - top) * fy;
                    };
                    const cv::Vec2f fA = lerp2(fa0, fa1) * s, fB = lerp2(fb0, fb1) * s;   // frame pixels
                    const float tt = t0r[x0] + (t0r[x1] - t0r[x0]) * fx, tb = t1r[x0] + (t1r[x1] - t1r[x0]) * fx;
                    const float trust = tt + (tb - tt) * fy;
                    float ca[4], cb[4];
                    sample(pa, x - wa * fA[0], y - wa * fA[1], ca);
                    sample(pb, x - wb * fB[0], y - wb * fB[1], cb);
                    for (int k = 0; k < 4; ++k) {
                        const float warped = wb * ca[k] + wa * cb[k];
                        const float plain = wb * ra[x * 4 + k] + wa * rb[x * 4 + k];
                        ro[x * 4 + k] = cv::saturate_cast<uint8_t>(plain + (warped - plain) * trust);
                    }
                }
            }
        });
        out = std::move(result);
        if (stats) {
            stats->meanMotion = flow.meanMotion;
            stats->fallbackShare = flow.fallbackShare;
            stats->milliseconds = msSince(t0);
            stats->flowMilliseconds = 0;
        }
        return true;
    } catch (const cv::Exception&) {
        return false;
    }
}

bool interpolateFrames(const Image& a, const Image& b, double w, Image& out, FlowQuality quality, FlowStats* stats) {
    const auto t0 = std::chrono::steady_clock::now();
    FlowStats est;
    const auto flow = estimateFlow(a, b, quality, &est);
    if (!flow || !warpWithFlow(a, b, *flow, w, out)) return false;
    if (stats) {
        *stats = est;
        stats->milliseconds = msSince(t0);
    }
    return true;
}

}  // namespace fa::vision
