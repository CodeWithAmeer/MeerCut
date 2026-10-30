#include "analysis_service.h"

#include "fa/ai/matting.h"
#include "fa/ai/prepare.h"

#include "fa/media/media.h"
#include "fa/render/audio_mix.h"
#include "fa/model/stabilize_plan.h"
#include "fa/model/time_map.h"
#include "fa/vision/stabilize.h"
#include "fa/vision/track.h"

#include <QMetaObject>

#include <algorithm>
#include <cmath>
#include <deque>

namespace fa::ui {

struct AnalysisService::Context {
    AnalysisService* self = nullptr;
    QString id;
    int total = 0;
    int done = 0;
    bool cancelled() const { return self->cancel_.load(); }
    void tick() {
        ++done;
        if (done % 5 == 0 || done == total) {
            const int d = done, t = total;
            QMetaObject::invokeMethod(self, [s = self, d, t] { emit s->progress(d, t); }, Qt::QueuedConnection);
        }
    }
    void fail(const QString& msg) {
        self->running_ = false;
        QMetaObject::invokeMethod(self, [s = self, i = id, msg] { emit s->failed(i, msg); }, Qt::QueuedConnection);
    }
};

AnalysisService::AnalysisService(QObject* parent) : QObject(parent) {
    qRegisterMetaType<fa::MotionTrack>();
    qRegisterMetaType<fa::Stabilization>();
}

AnalysisService::~AnalysisService() {
    cancel_ = true;
    join();
}

void AnalysisService::join() {
    if (worker_.joinable()) worker_.join();
}

void AnalysisService::cancel() { cancel_ = true; }

bool AnalysisService::launch(const std::string& clipId, const QString& kind, int total, std::function<void(Context&)> work) {
    if (running_) return false;
    join();
    running_ = true;
    cancel_ = false;
    clipId_ = clipId;
    kind_ = kind;
    worker_ = std::thread([this, clipId, total, work = std::move(work)] {
        Context ctx;
        ctx.self = this;
        ctx.id = QString::fromStdString(clipId);
        ctx.total = total;
        try {
            work(ctx);
        } catch (const std::exception& e) {
            ctx.fail(QString::fromUtf8(e.what()));
        }
    });
    emit started();
    return true;
}

bool AnalysisService::startCutout(const CutoutJob& job) {
    const int total = static_cast<int>(job.lastFrame - job.firstFrame + 1);
    return launch(job.clipId, "cutout", total, [this, job](Context& ctx) {
        const ai::MattingJob mj{job.media, job.firstFrame, job.lastFrame, job.cutout, job.accelerator};
        ai::MattingReport rep;
        try {
            rep = ai::analyseMattes(mj, [&](int64_t done, int64_t) {
                while (ctx.done < done) ctx.tick();
            }, &cancel_);
        } catch (const ai::GpuMemoryError& e) {   // the panel offers the CPU (with the time it takes)
            ctx.fail(QString("GPU_MEMORY:") + QString::fromUtf8(e.what()));
            return;
        }
        if (ctx.cancelled()) {
            ctx.fail("cancelled");
            return;
        }
        const std::string sha = ai::findModel(job.cutout.model)->files.front().sha256;
        QString summary = QString("Background Removal: %1 frames analysed, %2 already done, %3 s")
                              .arg(rep.framesAnalysed)
                              .arg(rep.framesCached)
                              .arg(rep.seconds, 0, 'f', 1);
        if (!rep.provider.empty()) summary += QString(" (%1)").arg(QString::fromStdString(rep.provider));
        if (!rep.fallback.empty()) summary += QString(" - %1").arg(QString::fromStdString(rep.fallback));
        running_ = false;
        QMetaObject::invokeMethod(this, [this, i = ctx.id, summary, sha] {
            emit cutoutFinished(i, summary, QString::fromStdString(sha));
        }, Qt::QueuedConnection);
    });
}

bool AnalysisService::startPrepare(const PrepareJob& job) {
    return launch(job.clipId, "prepare", 1000, [this, job](Context& ctx) {
        Project project = *job.snapshot.project;
        const ai::RenderPreparation r = ai::prepareForRender(project, job.snapshot.sequenceId, job.snapshot.projectFile,
                                                             job.fromFrame, job.toFrame,
                                                             [&](const char*, int64_t d, int64_t t) {
                                                                 // per stage 0..1000 (each stage starts again)
                                                                 const int target = t > 0 ? static_cast<int>(1000 * d / t) : 0;
                                                                 if (std::abs(target - ctx.done) < 5 && target != 1000) return;
                                                                 ctx.done = target;
                                                                 QMetaObject::invokeMethod(this, [this, target] { emit progress(target, 1000); },
                                                                                           Qt::QueuedConnection);
                                                             },
                                                             &cancel_);
        if (ctx.cancelled()) {
            ctx.fail("cancelled");
            return;
        }
        const QString summary = r.summary.empty() ? QString("AI frames: everything was prepared already")
                                                  : QString("AI frames: %1 in %2 s").arg(QString::fromStdString(r.summary)).arg(r.seconds, 0, 'f', 1);
        running_ = false;
        QMetaObject::invokeMethod(this, [this, i = ctx.id, summary] { emit prepareFinished(i, summary); }, Qt::QueuedConnection);
    });
}

bool AnalysisService::startLoudness(LoudnessJob job) {
    const Sequence* seq = nullptr;
    for (const auto& s : job.project.sequences)
        if (s.id == job.sequenceId) seq = &s;
    const Clip* clip = seq ? [&]() -> const Clip* {
        for (const auto& t : seq->tracks)
            for (const auto& c : t.clips)
                if (c.id == job.clipId) return &c;
        return nullptr;
    }() : nullptr;
    if (!clip) return false;
    const std::string id = job.clipId;
    return launch(id, "loudness", 100, [this, job = std::move(job)](Context& ctx) {
        const Sequence* s = nullptr;
        for (const auto& q : job.project.sequences)
            if (q.id == job.sequenceId) s = &q;
        const Clip* c = nullptr;
        for (const auto& t : s->tracks)
            for (const auto& x : t.clips)
                if (x.id == job.clipId) c = &x;
        int shown = 0;
        const double lufs = measureClipLoudness(job.project, *s, *c, job.projectFile, &cancel_, [&](double f) {
            while (shown < static_cast<int>(f * 100)) {
                ctx.tick();
                ++shown;
            }
        });
        if (ctx.cancelled() || std::isnan(lufs)) {
            ctx.fail("cancelled");
            return;
        }
        running_ = false;
        QMetaObject::invokeMethod(this, [this, i = ctx.id, lufs] { emit loudnessFinished(i, lufs); }, Qt::QueuedConnection);
    });
}

namespace {

VideoDecoder::Options analysisDecoder() {
    VideoDecoder::Options o;
    o.maxWidth = 960;
    o.maxHeight = 540;
    o.threads = 4;
    return o;
}

}  // namespace

bool AnalysisService::startTracking(const TrackingJob& job) {
    const int total = static_cast<int>(job.lastFrame - job.firstFrame + 1);
    return launch(job.clipId, "tracking", total, [this, job](Context& ctx) {
        VideoDecoder dec(job.media, analysisDecoder());
        const Rational fps = job.rate.fps();
        MotionTrack t;
        t.boxX = job.boxX;
        t.boxY = job.boxY;
        t.boxW = job.boxW;
        t.boxH = job.boxH;
        t.firstFrame = job.boxFrame;
        t.media = mediaIdentity(job.media);
        auto box = [&](const Image& img) {
            return vision::Box{job.boxX * img.width, job.boxY * img.height, job.boxW * img.width, job.boxH * img.height};
        };
        // forward from the box frame to the end of the range
        vision::RegionTracker fwd;
        {
            const Image& img = dec.frameAt(Time(job.boxFrame) / fps).image;
            if (!fwd.start(img, box(img)))
                return ctx.fail("The tracking box has too little detail to follow. Put it on a part of the object with "
                                "edges or texture.");
            t.samples.push_back({job.boxFrame, job.boxX, job.boxY, 1.0, 1.0, false});
            ctx.tick();
        }
        for (int64_t f = job.boxFrame + 1; f <= job.lastFrame; ++f) {
            if (ctx.cancelled()) return ctx.fail("cancelled");
            const Image& img = dec.frameAt(Time(f) / fps).image;
            const vision::TrackPoint p = fwd.next(img);
            t.samples.push_back({f, p.cx / img.width, p.cy / img.height, p.scale, p.confidence, p.lost});
            ctx.tick();
        }
        // backward from the box frame to the start: frames decoded forward in chunks, fed in reverse
        if (job.boxFrame > job.firstFrame) {
            vision::RegionTracker back;
            {
                const Image& img = dec.frameAt(Time(job.boxFrame) / fps).image;
                back.start(img, box(img));
            }
            std::vector<TrackingSample> before;
            for (int64_t end = job.boxFrame - 1; end >= job.firstFrame; end -= 32) {
                const int64_t from = std::max(job.firstFrame, end - 31);
                std::deque<Image> chunk;
                for (int64_t f = from; f <= end; ++f) {
                    if (ctx.cancelled()) return ctx.fail("cancelled");
                    chunk.push_back(dec.frameAt(Time(f) / fps).image);
                }
                for (int64_t f = end; f >= from; --f) {
                    const Image& img = chunk[static_cast<size_t>(f - from)];
                    const vision::TrackPoint p = back.next(img);
                    before.push_back({f, p.cx / img.width, p.cy / img.height, p.scale, p.confidence, p.lost});
                    ctx.tick();
                }
            }
            t.samples.insert(t.samples.begin(), before.rbegin(), before.rend());
        }
        running_ = false;
        QMetaObject::invokeMethod(this, [this, id = ctx.id, t] { emit trackingFinished(id, t); }, Qt::QueuedConnection);
    });
}

bool AnalysisService::startStabilization(const StabilizationJob& job) {
    const int total = static_cast<int>(job.lastFrame - job.firstFrame + 1);
    return launch(job.clipId, "stabilization", total, [this, job](Context& ctx) {
        VideoDecoder dec(job.media, analysisDecoder());
        const Rational fps = job.rate.fps();
        Stabilization st = job.settings;
        st.firstFrame = job.firstFrame;
        st.frameRate = fps;
        st.width = job.width;
        st.height = job.height;
        st.media = mediaIdentity(job.media);
        st.motion.clear();
        vision::MotionEstimator est;
        for (int64_t f = job.firstFrame; f <= job.lastFrame; ++f) {
            if (ctx.cancelled()) return ctx.fail("cancelled");
            const Image& img = dec.frameAt(Time(f) / fps).image;
            const vision::CameraStep s = est.next(img);
            st.motion.push_back({s.dx / img.width, s.dy / img.height, s.da, s.valid});
            ctx.tick();
        }
        planStabilization(st);
        running_ = false;
        QMetaObject::invokeMethod(this, [this, id = ctx.id, st] { emit stabilizationFinished(id, st); }, Qt::QueuedConnection);
    });
}

}  // namespace fa::ui
