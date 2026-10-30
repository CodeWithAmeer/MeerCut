// CPU reference backend: implements compositing.h exactly; rows are processed in parallel.
#include "fa/render/backend.h"
#include "fa/render/effects.h"

#include "parallel.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fa {

namespace {

// Minimal fork-join pool for row-parallel loops (one shared instance, created on first use).
class RowPool {
public:
    static RowPool& instance() {
        static RowPool pool;
        return pool;
    }
    // Run fn(row) for every row in [begin, end) using all workers plus the calling thread.
    // One job at a time: a caller that finds the pool busy (another thread composites concurrently, e.g.
    // export + preview, or browser thumbnails + preview) or calls it re-entrantly runs its rows itself.
    void parallelFor(int begin, int end, const std::function<void(int)>& fn) {
        thread_local bool inJob = false;
        std::unique_lock job(jobMutex_, std::defer_lock);
        if (end - begin < 16 || workers_.empty() || inJob || !job.try_lock()) {
            for (int y = begin; y < end; ++y) fn(y);
            return;
        }
        inJob = true;
        struct Reset {
            bool& flag;
            ~Reset() { flag = false; }
        } reset{inJob};
        std::unique_lock lk(m_);
        job_ = &fn;
        next_ = begin;
        end_ = end;
        active_ = static_cast<int>(workers_.size());
        ++generation_;
        lk.unlock();
        cv_.notify_all();
        work();
        lk.lock();
        done_.wait(lk, [&] { return active_ == 0; });
        job_ = nullptr;
    }
    ~RowPool() {
        {
            std::lock_guard lk(m_);
            quit_ = true;
            ++generation_;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

private:
    RowPool() {
        const unsigned n = std::max(1u, std::min(8u, std::thread::hardware_concurrency())) - 1;
        for (unsigned i = 0; i < n; ++i)
            workers_.emplace_back([this] {
                uint64_t seen = 0;
                for (;;) {
                    {
                        std::unique_lock lk(m_);
                        cv_.wait(lk, [&] { return quit_ || generation_ != seen; });
                        if (quit_) return;
                        seen = generation_;
                    }
                    work();
                    std::lock_guard lk(m_);
                    if (--active_ == 0) done_.notify_all();
                }
            });
    }
    void work() {
        const std::function<void(int)>* fn = job_;
        if (!fn) return;
        for (;;) {
            const int y = next_.fetch_add(4);
            if (y >= end_) return;
            for (int r = y; r < std::min(y + 4, end_.load()); ++r) (*fn)(r);
        }
    }
    std::vector<std::thread> workers_;
    std::mutex jobMutex_;   // held by the thread whose job the workers run
    std::mutex m_;
    std::condition_variable cv_, done_;
    const std::function<void(int)>* job_ = nullptr;
    std::atomic<int> next_{0}, end_{0};
    int active_ = 0;
    uint64_t generation_ = 0;
    bool quit_ = false;
};

inline uint8_t toByte(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

// Identity placement (same-size source, no transform), nothing but opacity: integer fast path.
bool isPlain(const LayerDesc& l, const Affine2D& m, int cw, int ch) {
    return l.image->width == cw && l.image->height == ch && m == Affine2D{} && l.cropL == 0 && l.cropT == 0 && l.cropR == 0 && l.cropB == 0 &&
           l.blend == BlendMode::Normal && l.brightness == 1.0 && l.flash == 0.0 && l.masks.empty() && !l.wipe.active;
}

// Composites one layer onto an opaque RGBA8 canvas (the reference implementation of compositing.h).
class LayerCompositor {
public:
    explicit LayerCompositor(Image& canvas) : canvas_(canvas) {}

    void draw(const LayerDesc& layer) {
        if (!layer.image || layer.image->empty()) return;
        const double opacity = std::clamp(layer.opacity, 0.0, 1.0);
        if (opacity <= 0.0) return;
        const int cw = canvas_.width, ch = canvas_.height;
        const Affine2D place = layerPlacement(layer, cw, ch);
        if (isPlain(layer, place, cw, ch)) {
            plainCopy(layer, opacity);
            return;
        }
        general(layer, place, opacity);
    }

private:
    // identity placement, normal blend: integer "over" (exact for opacity 1)
    void plainCopy(const LayerDesc& layer, double opacity) {
        const Image& src = *layer.image;
        const int op = static_cast<int>(std::lround(opacity * 255.0));
        RowPool::instance().parallelFor(0, canvas_.height, [&](int y) {
            uint8_t* d = canvas_.row(y);
            const uint8_t* s = src.row(y);
            for (int x = 0; x < canvas_.width; ++x, d += 4, s += 4) {
                const int a = (s[3] * op + 127) / 255;
                if (a == 255) {
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                } else if (a > 0) {
                    const int ia = 255 - a;
                    for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>((s[c] * a + d[c] * ia + 127) / 255);
                }
                d[3] = 255;
            }
        });
    }

    void general(const LayerDesc& l, const Affine2D& place, double opacity) {
        const Image& src = *l.image;
        const int cw = canvas_.width, ch = canvas_.height, sw = src.width, sh = src.height;
        const Affine2D inv = place.inverse();
        // crop rectangle in source pixels
        const double x0s = l.cropL * sw, x1s = sw - l.cropR * sw, y0s = l.cropT * sh, y1s = sh - l.cropB * sh;
        // canvas bounding box of the (cropped) layer, 1 px margin for bilinear edges
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
        // layer-height units: q = (S - centre) / sh scaled to layer height (uniform base: divide by sh)
        double fx = 1, fy = 1;
        fitScale(cw, ch, sw, sh, l.sourceToCanvas ? LayerFit::Fit : l.fit, fx, fy);
        const double qScaleX = fy > 0 ? (fx / fy) / sh : 1.0 / sh;   // non-uniform (stretch) bases keep units square
        const double qScaleY = 1.0 / sh;
        // one canvas pixel in layer heights (for anti-aliased hard mask edges)
        const double canvasPerSourcePx = std::sqrt(std::fabs(place.det()));
        const double pixelUnit = canvasPerSourcePx > 0 ? 1.0 / (canvasPerSourcePx * sh) : 1.0 / sh;
        const bool masked = !l.masks.empty();
        const float bright = static_cast<float>(l.brightness);
        const float flash = static_cast<float>(std::clamp(l.flash, 0.0, 1.0));
        const double aspect = double(cw) / double(ch);
        const float op = static_cast<float>(opacity);

        RowPool::instance().parallelFor(ys, ye, [&](int y) {
            uint8_t* drow = canvas_.row(y);
            const double py = y + 0.5;
            for (int x = xs; x < xe; ++x) {
                const double px = x + 0.5;
                const Vec2 S = inv.apply(px, py);
                // bilinear with transparent texels outside the crop rectangle (premultiplied)
                const double u = S.x - 0.5, v = S.y - 0.5;
                const double fu = std::floor(u), fv = std::floor(v);
                const int iu = static_cast<int>(fu), iv = static_cast<int>(fv);
                const double wu = u - fu, wv = v - fv;
                float acc[4] = {0, 0, 0, 0};
                for (int k = 0; k < 4; ++k) {
                    const int tx = iu + (k & 1), ty = iv + (k >> 1);
                    const double w = ((k & 1) ? wu : 1 - wu) * ((k >> 1) ? wv : 1 - wv);
                    if (w <= 0 || tx < 0 || ty < 0 || tx >= sw || ty >= sh) continue;
                    if (tx + 0.5 < x0s || tx + 0.5 > x1s || ty + 0.5 < y0s || ty + 0.5 > y1s) continue;
                    const uint8_t* t = src.row(ty) + tx * 4;
                    const float a = t[3] / 255.0f * static_cast<float>(w);
                    acc[0] += t[0] / 255.0f * a;
                    acc[1] += t[1] / 255.0f * a;
                    acc[2] += t[2] / 255.0f * a;
                    acc[3] += a;
                }
                if (acc[3] <= 1e-6f) continue;
                float alpha = acc[3] * op;
                if (masked) {
                    const double qx = (S.x - sw * 0.5) * qScaleX, qy = (S.y - sh * 0.5) * qScaleY;
                    alpha *= maskCoverage(l.masks, qx, qy, pixelUnit);
                }
                if (l.wipe.active) alpha *= wipeFactor(l.wipe, px / cw, py / ch, aspect);
                if (alpha <= 1e-6f) continue;
                uint8_t* d = drow + x * 4;
                for (int c = 0; c < 3; ++c) {
                    float s = std::min(1.0f, acc[c] / acc[3]) * bright;
                    s += (1.0f - s) * flash;
                    const float b = d[c] / 255.0f;
                    const float r = blendChannel(l.blend, b, s);
                    d[c] = toByte(b * (1 - alpha) + r * alpha);
                }
                d[3] = 255;
            }
        });
    }

    Image& canvas_;
};

class CpuBackend final : public RenderBackend {
public:
    std::string name() const override { return "cpu"; }

    void beginFrame(int width, int height, Rgba8 background) override {
        if (width <= 0 || height <= 0) throw std::invalid_argument("invalid canvas size");
        canvas_.reset(width, height, background);
    }
    void drawLayer(const LayerDesc& layer) override { compositeLayerCpu(canvas_, layer); }
    const Image& endFrame() override { return canvas_; }

private:
    Image canvas_;
};

}  // namespace

void parallelRows(int begin, int end, const std::function<void(int)>& fn) { RowPool::instance().parallelFor(begin, end, fn); }

void compositeLayerCpu(Image& canvas, const LayerDesc& layer) {
    if (layer.adjustment) {
        applyAdjustmentCpu(canvas, layer);
        return;
    }
    if (layer.effects && layer.image && !layer.image->empty() && !effectsAreIdentity(*layer.effects)) {
        Image processed;   // the effect stack runs on the picture first, then the processed picture is composited
        applyEffectsCpu(*layer.image, *layer.effects, processed);
        LayerDesc l = layer;
        l.image = &processed;
        l.effects.reset();
        LayerCompositor(canvas).draw(l);
        return;
    }
    LayerCompositor(canvas).draw(layer);
}

std::unique_ptr<RenderBackend> makeCpuBackend() { return std::make_unique<CpuBackend>(); }

}  // namespace fa
