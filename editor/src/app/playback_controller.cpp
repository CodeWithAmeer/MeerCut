#include "playback_controller.h"

#include "fa/render/audio_mix.h"
#include "fa/text/title_engine.h"

#include <QMetaObject>

namespace fa::ui {

namespace {

QImage toQImage(const std::shared_ptr<const Image>& img) {
    if (!img || img->empty()) return {};
    // Share the pixels: the QImage keeps a reference to the immutable frame alive.
    auto* keep = new std::shared_ptr<const Image>(img);
    return QImage(
        img->pixels.data(), img->width, img->height, img->stride(), QImage::Format_RGBA8888,
        [](void* p) { delete static_cast<std::shared_ptr<const Image>*>(p); }, keep);
}

QStringList toList(const std::vector<std::string>& v) {
    QStringList l;
    for (const auto& s : v) l << QString::fromUtf8(s);
    return l;
}

}  // namespace

PlaybackController::PlaybackController(Document* doc, QObject* parent, AudioPlayer::Backend audioBackend)
    : QObject(parent), doc_(doc), renderer_(std::make_unique<PreviewRenderer>(960, 540, 60)),
      audio_(std::make_unique<AudioPlayer>(audioBackend)) {
    renderer_->setCallback([this](const PreviewFrame& f) {
        QMetaObject::invokeMethod(this, [this, f] { showFrame(f); }, Qt::QueuedConnection);
    });
    renderer_->setTitleRendererFactory([] { return text::makeTitleEngine(); });
    renderer_->setSnapshot(doc_->snapshot());
    connect(doc_, &Document::changed, this, &PlaybackController::onDocumentChanged);
    connect(doc_, &Document::previewChanged, this, &PlaybackController::onDocumentChanged);
    connect(doc_, &Document::playheadChanged, this, [this](qint64) {
        if (!playing_) requestCurrent();
    });
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(5);
    connect(&timer_, &QTimer::timeout, this, &PlaybackController::onTick);
    if (!audio_->deviceAvailable())
        QMetaObject::invokeMethod(
            this, [this] { emit statusMessage("No audio device: playback runs silently - " + QString::fromStdString(audio_->lastError())); },
            Qt::QueuedConnection);
    requestCurrent();
}

PlaybackController::~PlaybackController() {
    timer_.stop();
    audio_->stop();
    renderer_->setCallback(nullptr);
}

int64_t PlaybackController::frameCount() const { return doc_->sequence().frameCount(); }

void PlaybackController::setPreviewMaxSize(int w, int h) {
    renderer_->setMaxSize(w, h);
    requestCurrent();
}

void PlaybackController::setRenderSettings(BackendChoice backend, bool hardwareDecode) {
    renderer_->setBackend(backend);
    renderer_->setHardwareDecoding(hardwareDecode);
    requestCurrent();
}

void PlaybackController::setVolume(double v) { audio_->setVolume(static_cast<float>(v)); }

void PlaybackController::onDocumentChanged() {
    const ProjectSnapshot snap = doc_->snapshot();
    renderer_->setSnapshot(snap);
    if (playing_) audio_->updateSnapshot(snap);
    else requestCurrent();
}

void PlaybackController::requestCurrent() { renderer_->request(doc_->playhead()); }

void PlaybackController::setCompare(bool on) {
    if (on == compare_) return;
    compare_ = on;
    renderer_->setCompare(on);
    requestCurrent();
}

void PlaybackController::showFrame(const PreviewFrame& f) {
    if (f.version != doc_->version()) return;   // stale: rendered before the latest edit
    if (f.index != doc_->playhead()) return;    // late: the playhead has already moved on
    if (f.compare != compare_) return;          // rendered before compare was switched
    image_ = toQImage(f.image);
    imageFrame_ = f.index;
    imageVersion_ = f.version;
    imageCompare_ = f.compare;
    offline_ = toList(f.offline);
    emit frameShown(image_, f.index, offline_);
}

void PlaybackController::play() {
    if (playing_) return;
    const int64_t total = frameCount();
    if (total <= 0) {
        emit statusMessage("The timeline is empty");
        return;
    }
    if (doc_->playhead() >= total - 1) doc_->setPlayhead(0);   // at the end: start over
    startFrame_ = doc_->playhead();
    const Sequence& seq = doc_->sequence();
    audio_->start(doc_->snapshot(), timeToSample(frameToTime(startFrame_, seq.frameRate), seq.sampleRate));
    renderer_->setPrefetch(startFrame_, 30);
    playing_ = true;
    lastTickFrame_ = -1;
    dropped_ = 0;
    timer_.start();
    emit playingChanged(true);
}

void PlaybackController::pause() {
    if (!playing_) return;
    timer_.stop();
    audio_->stop();
    renderer_->clearPrefetch();
    playing_ = false;
    emit playingChanged(false);
    requestCurrent();   // make sure the exact playhead frame is displayed
}

void PlaybackController::togglePlay() {
    if (playing_) pause();
    else play();
}

void PlaybackController::stop() {
    pause();
    doc_->setPlayhead(0);
}

void PlaybackController::seek(qint64 frame) {
    const int64_t total = frameCount();
    frame = std::clamp<int64_t>(frame, 0, std::max<int64_t>(0, total - 1));
    const bool wasPlaying = playing_;
    if (wasPlaying) pause();
    doc_->setPlayhead(frame);
    requestCurrent();
    if (wasPlaying) play();
}

void PlaybackController::step(int frames) { seek(doc_->playhead() + frames); }
void PlaybackController::toStart() { seek(0); }
void PlaybackController::toEnd() { seek(frameCount() - 1); }

void PlaybackController::onTick() {
    const Sequence& seq = doc_->sequence();
    const int64_t total = seq.frameCount();
    const int64_t pos = audio_->position();
    const int64_t frame = timeToFrame(Rational(pos, seq.sampleRate), seq.frameRate);
    if (frame >= total) {
        pause();
        doc_->setPlayhead(std::max<int64_t>(0, total - 1));
        return;
    }
    if (frame == lastTickFrame_) return;
    if (lastTickFrame_ >= 0 && frame > lastTickFrame_ + 1 && imageFrame_ != lastTickFrame_) dropped_ += frame - lastTickFrame_ - 1;
    lastTickFrame_ = frame;
    doc_->setPlayhead(frame);
    renderer_->setPrefetch(frame, 30);
    if (auto cached = renderer_->cached(frame)) showFrame(*cached);
    else renderer_->request(frame);   // late: shown when ready unless a newer frame is due by then
}

}  // namespace fa::ui
