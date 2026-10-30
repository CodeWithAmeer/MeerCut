// Background preview rendering (no Qt).
//
// Renders timeline frames on a worker thread with exactly the export semantics (renderFrame():
// timeline evaluation, frame selection, compositing, transitions, opacity) but on a smaller canvas;
// decoders deliver frames already scaled to that canvas. The GUI publishes immutable project
// snapshots; every snapshot change invalidates the frame cache.
//
// Scheduling: the most recent display request wins (scrubbing never queues up stale frames);
// when idle, frames in the prefetch window are rendered ahead (playback).
#pragma once

#include "fa/core/image.h"
#include "fa/model/project.h"
#include "fa/playback/render_cache.h"
#include "fa/render/backend.h"
#include "fa/render/title_renderer.h"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fa {

struct ProjectSnapshot {
    std::shared_ptr<const Project> project;
    std::string sequenceId;
    std::filesystem::path projectFile;   // base for relative media paths
    uint64_t version = 0;                // increases with every edit

    const Sequence* sequence() const { return project ? project->findSequence(sequenceId) : nullptr; }
};

struct PreviewFrame {
    int64_t index = -1;
    uint64_t version = 0;
    std::shared_ptr<const Image> image;
    std::vector<std::string> offline;    // media that could not be decoded (shown as placeholders)
    bool compare = false;                // left half without effects, right half with them (setCompare)
    bool fromRenderCache = false;        // shown from the Render Preview cache (M5) instead of rendered
};

class PreviewRenderer {
public:
    using Callback = std::function<void(const PreviewFrame&)>;   // invoked on the worker thread

    PreviewRenderer(int maxWidth = 960, int maxHeight = 540, size_t cacheFrames = 48);
    ~PreviewRenderer();
    PreviewRenderer(const PreviewRenderer&) = delete;
    PreviewRenderer& operator=(const PreviewRenderer&) = delete;

    void setCallback(Callback cb);
    void setSnapshot(ProjectSnapshot snapshot);
    void setMaxSize(int maxWidth, int maxHeight);   // preview quality (Full / 1/2 / 1/4 of the panel)
    // Compositing backend and decoder settings (applied by the worker before its next frame).
    void setBackend(BackendChoice choice);
    void setHardwareDecoding(bool on);
    // Text engine for titles and captions (created on the worker thread).
    void setTitleRendererFactory(TitleRendererFactory factory);
    std::string backendName() const;               // what the worker actually uses
    BackendChoice backendChoice() const;
    bool hardwareDecoding() const;
    void maxSize(int& maxWidth, int& maxHeight) const;
    // M5 Render Preview: frames whose content key is in the cache are loaded instead of rendered (not in compare
    // mode). The cache is filled by renderPreviewRange (render_preview.h); nullptr turns it off.
    void setRenderCache(std::shared_ptr<RenderCache> cache);
    uint64_t renderCacheHits() const;
    // The content key of a frame for the current snapshot and preview configuration (nullopt without a sequence).
    std::optional<FrameKey> frameKey(int64_t frame);
    // Before / after: the left half of every frame is rendered without any effect, colour grading or adjustment
    // layer, the right half as it is. Costs a second render per frame while on.
    void setCompare(bool on);
    // Canvas size used for a sequence (fits the sequence aspect into the max size, even dimensions).
    static void canvasSize(const Sequence& seq, int maxWidth, int maxHeight, int& w, int& h);

    void request(int64_t frame);                    // show this frame next (latest wins)
    void setPrefetch(int64_t from, int64_t count);  // render [from, from + count) ahead when idle
    void clearPrefetch();
    std::optional<PreviewFrame> cached(int64_t frame) const;   // current version only

    // Synchronous render for tests/snapshots (uses the worker's settings, bypasses the queue).
    PreviewFrame renderNow(int64_t frame);

    uint64_t framesRendered() const;

private:
    struct State;
    void run();
    PreviewFrame renderOne(const ProjectSnapshot& snap, int64_t frame, int w, int h);
    std::unique_ptr<State> s_;
    std::thread worker_;
};

}  // namespace fa
