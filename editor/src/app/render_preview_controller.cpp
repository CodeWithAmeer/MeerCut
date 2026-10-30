#include "render_preview_controller.h"

#include "dialogs.h"
#include "playback_controller.h"

#include "fa/ai/prepare.h"
#include "fa/playback/render_preview.h"
#include "fa/text/title_engine.h"

#include <QDir>
#include <QMetaObject>
#include <QStandardPaths>

#include <algorithm>

namespace fa::ui {

namespace {

std::filesystem::path pathOf(const QString& s) { return std::filesystem::path(s.toStdWString()); }

}  // namespace

RenderPreviewController::RenderPreviewController(Document* doc, PlaybackController* playback, QObject* parent)
    : QObject(parent), doc_(doc), playback_(playback) {
    QString dir = qEnvironmentVariable("MEERCUT_RENDER_CACHE");
    if (dir.isEmpty())
        dir = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath("render-preview");
    bool ok = false;
    qulonglong mb = qEnvironmentVariable("MEERCUT_RENDER_CACHE_MB").toULongLong(&ok);
    if (!ok || mb == 0) mb = static_cast<qulonglong>(loadRenderSettings().renderCacheLimitGb) * 1024;   // Preferences
    cache_ = std::make_shared<RenderCache>(pathOf(dir), static_cast<uint64_t>(mb) << 20);
    playback_->renderer().setRenderCache(cache_);
    debounce_.setSingleShot(true);
    debounce_.setInterval(150);
    connect(&debounce_, &QTimer::timeout, this, &RenderPreviewController::refreshRanges);
    connect(doc_, &Document::changed, this, [this] { debounce_.start(); });
    // the preview size, backend and decoding settings are part of the keys: re-check when a frame is shown
    connect(playback_, &PlaybackController::frameShown, this, [this] {
        if (cache_->frameCount() > 0 && !debounce_.isActive()) debounce_.start();
    });
    refreshRanges();
}

RenderPreviewController::~RenderPreviewController() {
    cancel_ = true;
    join();
}

void RenderPreviewController::join() {
    if (worker_.joinable()) worker_.join();
}

bool RenderPreviewController::startForTimeline() { return start(0, doc_->sequence().frameCount()); }

bool RenderPreviewController::start(int64_t from, int64_t to) {
    if (running_ || to <= from) return false;
    join();
    PreviewRenderer& live = playback_->renderer();
    RenderPreviewRequest req;
    req.snapshot = doc_->snapshot();
    req.from = from;
    req.to = to;
    live.maxSize(req.maxWidth, req.maxHeight);
    req.backend = live.backendChoice();
    req.hardwareDecoding = live.hardwareDecoding();
    req.titles = [] { return text::makeTitleEngine(); };
    req.expectBackend = live.backendName();
    running_ = true;
    cancel_ = false;
    worker_ = std::thread([this, req]() mutable {
        // M7: the AI pictures of the range (mattes, AI in-between frames, upscaled frames) prepared first, so the
        // rendered frames are the export's
        RenderPreviewResult r;
        try {
            auto project = std::make_shared<Project>(*req.snapshot.project);
            ai::prepareForRender(*project, req.snapshot.sequenceId, req.snapshot.projectFile, req.from, req.to, {}, &cancel_);
            req.snapshot.project = project;
        } catch (const std::exception& e) {
            r.error = std::string("preparing AI frames: ") + e.what();
        }
        r.cancelled = cancel_;
        if (r.error.empty() && !cancel_)
            r = renderPreviewRange(req, *cache_, cancel_, [this](int64_t done, int64_t total) {
            QMetaObject::invokeMethod(this, [this, done, total] { emit progress(done, total); }, Qt::QueuedConnection);
        });
        running_ = false;
        const bool completed = !r.cancelled && r.error.empty();
        const QString msg = !r.error.empty() ? QString("Render Preview failed: %1").arg(QString::fromStdString(r.error))
                            : r.cancelled    ? QString("Render Preview cancelled (%1 frames rendered)").arg(r.rendered)
                                             : QString("Render Preview: %1 frames rendered in %2 s (%3 were already rendered)")
                                                   .arg(r.rendered)
                                                   .arg(r.seconds, 0, 'f', 1)
                                                   .arg(r.alreadyCached);
        QMetaObject::invokeMethod(
            this,
            [this, completed, msg] {
                refreshRanges();
                emit finished(completed, msg);
            },
            Qt::QueuedConnection);
    });
    return true;
}

void RenderPreviewController::cancel() { cancel_ = true; }

void RenderPreviewController::clearCache() {
    cancel();
    join();
    cache_->clear();
    refreshRanges();
}

void RenderPreviewController::refreshRanges() {
    std::vector<std::pair<int64_t, int64_t>> runs;
    if (cache_->frameCount() > 0) {   // nothing to look up otherwise (editing without Render Preview costs nothing)
        PreviewRenderer& live = playback_->renderer();
        const Sequence& seq = doc_->sequence();
        int mw = 0, mh = 0, w = 0, h = 0;
        live.maxSize(mw, mh);
        PreviewRenderer::canvasSize(seq, mw, mh, w, h);
        const ProjectSnapshot snap = doc_->snapshot();
        const FrameKeyer keyer(*snap.project, seq, snap.projectFile,
                               FrameKeyer::Config{w, h, live.backendName(), live.hardwareDecoding()});
        for (int64_t f = 0, n = seq.frameCount(); f < n; ++f) {
            if (!cache_->contains(keyer.key(f))) continue;
            if (!runs.empty() && runs.back().second == f) runs.back().second = f + 1;
            else runs.push_back({f, f + 1});
        }
    }
    if (runs != ranges_) {
        ranges_ = std::move(runs);
        emit rangesChanged();
    }
}

}  // namespace fa::ui
