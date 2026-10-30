#include "media_service.h"

#include "fa/media/media.h"

#include <QMetaObject>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <list>

namespace fa::ui {

namespace {
std::filesystem::path toPath(const QString& s) {
    const std::string u = s.toUtf8().toStdString();
    return std::filesystem::path(std::u8string(u.begin(), u.end()));
}
}  // namespace

MediaService::MediaService(QObject* parent) : QObject(parent) {
    worker_ = std::thread([this] { run(); });
}

MediaService::~MediaService() {
    {
        std::lock_guard lk(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

QImage MediaService::thumbnail(const QString& path, double seconds, double bucketSeconds) {
    const int64_t bucketMs = static_cast<int64_t>(std::floor(std::max(0.0, seconds) / bucketSeconds) * bucketSeconds * 1000.0);
    const QString key = thumbKey(path, bucketMs);
    auto it = thumbs_.find(key);
    if (it != thumbs_.end()) return it.value();
    if (!pendingThumbs_.contains(key)) {
        pendingThumbs_.insert(key, true);
        {
            std::lock_guard lk(m_);
            jobs_.push_front({Job::Thumb, path, bucketMs});   // newest first: what is on screen now
        }
        cv_.notify_one();
    }
    return {};
}

const std::vector<float>* MediaService::waveform(const QString& path) {
    auto it = waves_.find(path);
    if (it != waves_.end()) return it->second.get();
    if (!pendingWaves_.contains(path)) {
        pendingWaves_.insert(path, true);
        {
            std::lock_guard lk(m_);
            jobs_.push_back({Job::Wave, path, 0});
        }
        cv_.notify_one();
    }
    return nullptr;
}

void MediaService::run() {
    struct Dec {
        QString path;
        std::unique_ptr<VideoDecoder> dec;
    };
    std::list<Dec> decoders;   // small LRU of open video decoders
    for (;;) {
        Job job;
        {
            std::unique_lock lk(m_);
            cv_.wait(lk, [&] { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            job = jobs_.front();
            jobs_.pop_front();
        }
        if (job.kind == Job::Thumb) {
            QImage img;
            try {
                auto it = std::find_if(decoders.begin(), decoders.end(), [&](const Dec& d) { return d.path == job.path; });
                if (it == decoders.end()) {
                    VideoDecoder::Options opt;
                    opt.maxWidth = kThumbW;
                    opt.maxHeight = kThumbH;
                    opt.threads = 2;
                    decoders.push_front({job.path, std::make_unique<VideoDecoder>(toPath(job.path), opt)});
                    if (decoders.size() > 4) decoders.pop_back();
                    it = decoders.begin();
                } else {
                    decoders.splice(decoders.begin(), decoders, it);
                    it = decoders.begin();
                }
                const VideoFrame& f = it->dec->frameAt(Rational(job.bucketMs, 1000));
                QImage view(f.image.pixels.data(), f.image.width, f.image.height, f.image.stride(), QImage::Format_RGBA8888);
                img = view.copy();
            } catch (const std::exception&) {
                img = QImage(16, 9, QImage::Format_RGBA8888);
                img.fill(QColor(90, 16, 24));   // offline/undecodable placeholder
            }
            const QString key = thumbKey(job.path, job.bucketMs);
            const QString path = job.path;
            QMetaObject::invokeMethod(
                this,
                [this, key, img, path] {
                    thumbs_.insert(key, img);
                    pendingThumbs_.remove(key);
                    emit thumbnailReady(path);
                },
                Qt::QueuedConnection);
        } else {
            auto peaks = std::make_unique<std::vector<float>>();
            try {
                constexpr int kRate = 4000;
                constexpr int kPer = kRate / kPeaksPerSecond;   // 40 samples per peak
                AudioDecoder dec(toPath(job.path), kRate, 1);
                const int64_t total = dec.lengthSamples().value_or(kRate * 3600LL);
                std::vector<float> buf(kRate);
                for (int64_t pos = 0; pos < total; pos += kRate) {
                    const int n = static_cast<int>(std::min<int64_t>(kRate, total - pos));
                    dec.read(pos, n, buf.data());
                    for (int i = 0; i < n; i += kPer) {
                        float pk = 0;
                        for (int j = i; j < std::min(n, i + kPer); ++j) pk = std::max(pk, std::fabs(buf[j]));
                        peaks->push_back(std::min(1.0f, pk));
                    }
                    std::lock_guard lk(m_);
                    if (quit_) return;
                }
            } catch (const std::exception&) {
                peaks->clear();
            }
            const QString path = job.path;
            auto* raw = peaks.release();
            QMetaObject::invokeMethod(
                this,
                [this, path, raw] {
                    waves_[path].reset(raw);
                    pendingWaves_.remove(path);
                    emit waveformReady(path);
                },
                Qt::QueuedConnection);
        }
    }
}

}  // namespace fa::ui
