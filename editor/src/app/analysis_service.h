// Background analysis of a clip's frames (M5): motion tracking and stabilisation run on a worker thread with
// progress and cancellation; the GUI thread only receives the result (and stores it with one undoable command).
// One analysis runs at a time. Frames are decoded at most 960 pixels wide; results are in picture fractions, so they
// do not depend on the analysis size.
#pragma once

#include "fa/ai/inference.h"
#include "fa/model/project.h"
#include "fa/playback/preview_renderer.h"

#include <QObject>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>

namespace fa::ui {

class AnalysisService : public QObject {
    Q_OBJECT
public:
    explicit AnalysisService(QObject* parent = nullptr);
    ~AnalysisService() override;

    struct TrackingJob {
        std::string clipId;
        std::filesystem::path media;
        int64_t firstFrame = 0;   // the source frames to analyse (inclusive)
        int64_t lastFrame = 0;
        int64_t boxFrame = 0;     // the frame the box was placed on: tracking runs forward and backward from it
        FrameRate rate;           // the media's frame rate
        double boxX = 0.5, boxY = 0.5, boxW = 0.25, boxH = 0.25;   // picture fractions
    };
    bool startTracking(const TrackingJob& job);   // false while another analysis runs

    struct StabilizationJob {
        std::string clipId;
        std::filesystem::path media;
        int64_t firstFrame = 0;   // the source frames to analyse (inclusive)
        int64_t lastFrame = 0;
        FrameRate rate;           // the media's frame rate
        int width = 0, height = 0;   // the media's picture size
        Stabilization settings;   // level, strength, Auto Zoom (kept in the result)
    };
    bool startStabilization(const StabilizationJob& job);

    // M6: integrated loudness (BS.1770-4) of an audio clip after its cleanup and EQ (fa::measureClipLoudness).
    struct LoudnessJob {
        std::string clipId;
        Project project;          // a copy: the GUI may edit meanwhile
        std::string sequenceId;
        std::filesystem::path projectFile;
    };
    bool startLoudness(LoudnessJob job);

    // M7: Background Removal - the matting model over the clip's source frames (fa::ai::analyseMattes); the mattes go
    // to the analysis cache, the clip gets the model's hash (cutoutFinished).
    struct CutoutJob {
        std::string clipId;
        std::filesystem::path media;
        int64_t firstFrame = 0, lastFrame = 0;
        AiCutout cutout;
        ai::Accelerator accelerator = ai::Accelerator::Auto;   // Cpu only after the user agreed to the cost
    };
    bool startCutout(const CutoutJob& job);
    // M7: AI Interpolation / Video Upscaling (and Background Removal) pictures of a timeline range, prepared
    struct PrepareJob {
        std::string clipId;          // the clip the request came from (reported back)
        ProjectSnapshot snapshot;
        int64_t fromFrame = 0, toFrame = -1;
    };
    bool startPrepare(const PrepareJob& job);

    void cancel();
    bool busy() const { return running_.load(); }
    std::string clipId() const { return clipId_; }
    QString kind() const { return kind_; }   // "tracking" / "stabilization" / "loudness": the running (or last) one

signals:
    void started();   // an analysis began (kind() says which)
    void progress(int done, int total);
    void trackingFinished(const QString& clipId, const fa::MotionTrack& track);
    void stabilizationFinished(const QString& clipId, const fa::Stabilization& result);
    void loudnessFinished(const QString& clipId, double lufs);   // -inf for silence
    void prepareFinished(const QString& clipId, const QString& summary);
    void cutoutFinished(const QString& clipId, const QString& summary, const QString& modelSha);
    void failed(const QString& clipId, const QString& message);   // also on cancellation ("cancelled")

private:
    // Runs `work` on the worker thread; it reports progress with tick() and returns early when cancelled().
    struct Context;
    bool launch(const std::string& clipId, const QString& kind, int total, std::function<void(Context&)> work);
    void join();
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    std::string clipId_;
    QString kind_;
};

}  // namespace fa::ui

Q_DECLARE_METATYPE(fa::MotionTrack)
Q_DECLARE_METATYPE(fa::Stabilization)
