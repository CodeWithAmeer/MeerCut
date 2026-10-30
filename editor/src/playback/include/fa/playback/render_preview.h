// Render Preview (M5): renders a range of the timeline ahead of playback into the RenderCache, in the background,
// with progress and cancellation. It uses its own PreviewRenderer configured exactly like the interactive one (same
// preview size, compositing backend, hardware decoding and text engine), so every stored frame is the frame the
// player would have rendered, stored under the key the player computes. Frames already in the cache are skipped.
#pragma once

#include "fa/playback/preview_renderer.h"
#include "fa/playback/render_cache.h"

#include <atomic>
#include <functional>
#include <string>

namespace fa {

struct RenderPreviewRequest {
    ProjectSnapshot snapshot;
    int64_t from = 0, to = 0;     // frames [from, to)
    int maxWidth = 960, maxHeight = 540;   // the player's preview size
    BackendChoice backend = BackendChoice::Auto;
    bool hardwareDecoding = false;
    TitleRendererFactory titles;
    std::string expectBackend;    // the player's backendName(): keys are made for it, and the job refuses to store
                                  // frames from a different backend (pixels may differ by a level)
};

struct RenderPreviewResult {
    int64_t rendered = 0;         // frames rendered and stored
    int64_t alreadyCached = 0;    // frames skipped because the cache had them
    bool cancelled = false;
    std::string error;            // empty on success
    double seconds = 0;
};

RenderPreviewResult renderPreviewRange(const RenderPreviewRequest& request, RenderCache& cache,
                                       const std::atomic<bool>& cancel,
                                       const std::function<void(int64_t done, int64_t total)>& progress = {});

}  // namespace fa
