// Background thumbnails (media library, timeline filmstrips) and audio waveform peaks.
// Requests return immediately (cached result or empty) and schedule work on one worker thread;
// readiness is signalled on the GUI thread so views can repaint.
#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fa::ui {

class MediaService : public QObject {
    Q_OBJECT
public:
    explicit MediaService(QObject* parent = nullptr);
    ~MediaService() override;

    static constexpr int kThumbW = 192;
    static constexpr int kThumbH = 108;
    static constexpr int kPeaksPerSecond = 100;

    // Thumbnail of the frame shown at `seconds` (bucketed to `bucketSeconds`); null image until ready.
    QImage thumbnail(const QString& path, double seconds, double bucketSeconds = 1.0);
    // Peak amplitude (0..1) per 1/kPeaksPerSecond s; nullptr until ready. Pointer stays valid.
    const std::vector<float>* waveform(const QString& path);

signals:
    void thumbnailReady(const QString& path);
    void waveformReady(const QString& path);

private:
    struct Job {
        enum Kind { Thumb, Wave } kind;
        QString path;
        int64_t bucketMs = 0;
    };
    void run();
    QString thumbKey(const QString& path, int64_t bucketMs) const { return path + '#' + QString::number(bucketMs); }

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    bool quit_ = false;
    QHash<QString, QImage> thumbs_;                          // GUI thread only
    QHash<QString, bool> pendingThumbs_;                     // GUI thread only
    std::map<QString, std::unique_ptr<std::vector<float>>> waves_;   // GUI thread only
    QHash<QString, bool> pendingWaves_;
    std::thread worker_;
};

}  // namespace fa::ui
