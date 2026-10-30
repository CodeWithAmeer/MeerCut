#include "fa/playback/render_preview.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

namespace fa {

RenderPreviewResult renderPreviewRange(const RenderPreviewRequest& req, RenderCache& cache, const std::atomic<bool>& cancel,
                                       const std::function<void(int64_t, int64_t)>& progress) {
    RenderPreviewResult res;
    const auto t0 = std::chrono::steady_clock::now();
    auto finish = [&] {
        res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return res;
    };
    const Sequence* seq = req.snapshot.sequence();
    if (!seq) {
        res.error = "no sequence";
        return finish();
    }
    const int64_t from = std::max<int64_t>(0, req.from), to = std::min(req.to, seq->frameCount());
    int w = 0, h = 0;
    PreviewRenderer::canvasSize(*seq, req.maxWidth, req.maxHeight, w, h);
    const FrameKeyer keyer(*req.snapshot.project, *seq, req.snapshot.projectFile,
                           FrameKeyer::Config{w, h, req.expectBackend, req.hardwareDecoding});
    std::vector<std::pair<int64_t, FrameKey>> todo;
    for (int64_t f = from; f < to; ++f) {
        const FrameKey k = keyer.key(f);
        if (cache.contains(k)) ++res.alreadyCached;
        else todo.push_back({f, k});
    }
    const int64_t total = to - from;
    int64_t done = res.alreadyCached;
    if (progress) progress(done, total);
    if (todo.empty()) return finish();

    PreviewRenderer r(req.maxWidth, req.maxHeight, 24);
    r.setBackend(req.backend);
    r.setHardwareDecoding(req.hardwareDecoding);
    r.setTitleRendererFactory(req.titles);
    std::mutex m;
    std::condition_variable cv;
    std::optional<PreviewFrame> arrived;
    r.setCallback([&](const PreviewFrame& f) {
        std::lock_guard lk(m);
        arrived = f;
        cv.notify_all();
    });
    r.setSnapshot(req.snapshot);
    bool checkedBackend = false;
    for (size_t i = 0; i < todo.size(); ++i) {
        if (cancel) {
            res.cancelled = true;
            break;
        }
        const auto [frame, key] = todo[i];
        r.setPrefetch(frame + 1, 8);   // the worker renders ahead while this thread stores
        std::optional<PreviewFrame> got = r.cached(frame);
        if (!got) {
            {
                std::lock_guard lk(m);
                arrived.reset();
            }
            r.request(frame);
            // the display callback brings it - or, if a prefetch render of it was already under way, the renderer's
            // memory cache has it without a callback
            while (!got && !cancel) {
                std::unique_lock lk(m);
                if (arrived && arrived->index == frame) {
                    got = arrived;
                    break;
                }
                cv.wait_for(lk, std::chrono::milliseconds(20));
                if (arrived && arrived->index == frame) got = arrived;
                lk.unlock();
                if (!got) got = r.cached(frame);
            }
            if (!got) {
                res.cancelled = true;
                break;
            }
        }
        if (!checkedBackend) {
            if (r.backendName() != req.expectBackend) {
                res.error = "the preview's compositing backend (" + req.expectBackend + ") is not available for rendering (" +
                            r.backendName() + ")";
                break;
            }
            checkedBackend = true;
        }
        if (!got->image || got->image->width != w || got->image->height != h) {
            res.error = "unexpected frame size";
            break;
        }
        if (!cache.store(key, *got->image)) {
            res.error = "the render cache could not be written (" + cache.dir().string() + ")";
            break;
        }
        ++res.rendered;
        ++done;
        if (progress) progress(done, total);
    }
    r.setCallback(nullptr);
    return finish();
}

}  // namespace fa
