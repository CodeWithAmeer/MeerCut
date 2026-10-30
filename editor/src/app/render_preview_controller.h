// Render Preview in the editor (M5), like Filmora's: Playback > Render Preview renders the whole timeline in the
// background into the render cache (fa/playback/render_cache.h) with progress and Cancel (frames already rendered
// are skipped); the player then shows those frames instead of rendering them, so heavy parts (optical flow, stabilisation,
// effect stacks) play smoothly. A green bar under the timeline ruler marks the frames that are rendered for the
// current project and preview configuration; any edit removes the mark from exactly the frames it changes.
// Playback > Clear Render Preview Files empties the cache. Location: MEERCUT_RENDER_CACHE or the user cache folder;
// size limit: Edit > Preferences (default 8 GB) or MEERCUT_RENDER_CACHE_MB; least recently used frames go first.
#pragma once

#include "document.h"

#include "fa/playback/render_cache.h"

#include <QObject>
#include <QTimer>

#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace fa::ui {

class PlaybackController;

class RenderPreviewController : public QObject {
    Q_OBJECT
public:
    RenderPreviewController(Document* doc, PlaybackController* playback, QObject* parent = nullptr);
    ~RenderPreviewController() override;

    bool start(int64_t from, int64_t to);   // false while rendering or when the range is empty
    bool startForTimeline();                 // the whole timeline (frames already rendered are skipped)
    void cancel();
    bool busy() const { return running_.load(); }
    void clearCache();
    RenderCache& cache() { return *cache_; }
    // Runs of rendered frames [from, to) for the current document and preview configuration.
    const std::vector<std::pair<int64_t, int64_t>>& renderedRanges() const { return ranges_; }
    void refreshRanges();   // recompute now (normally debounced after edits and renders)

signals:
    void progress(qint64 done, qint64 total);
    void finished(bool completed, const QString& message);
    void rangesChanged();

private:
    void join();
    Document* doc_;
    PlaybackController* playback_;
    std::shared_ptr<RenderCache> cache_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    std::vector<std::pair<int64_t, int64_t>> ranges_;
    QTimer debounce_;
};

}  // namespace fa::ui
