// Transport: play/pause/stop/seek/step. During playback the audio clock (AudioPlayer) decides which
// frame is shown; frames come from the PreviewRenderer (prefetched ahead). Paused seeks request exactly
// the playhead frame, so the preview is always the same frame export would produce there.
#pragma once

#include "document.h"

#include "fa/playback/audio_player.h"
#include "fa/playback/preview_renderer.h"

#include <QImage>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace fa::ui {

class PlaybackController : public QObject {
    Q_OBJECT
public:
    explicit PlaybackController(Document* doc, QObject* parent = nullptr,
                                AudioPlayer::Backend audioBackend = AudioPlayer::Backend::Default);
    ~PlaybackController() override;

    bool isPlaying() const { return playing_; }
    int64_t frameCount() const;
    PreviewRenderer& renderer() { return *renderer_; }
    AudioPlayer& audio() { return *audio_; }
    QImage currentImage() const { return image_; }
    QStringList offlineMessages() const { return offline_; }   // of the frame on screen (offline media, missing LUTs)
    int64_t currentImageFrame() const { return imageFrame_; }
    uint64_t currentImageVersion() const { return imageVersion_; }   // project version it was rendered from
    bool currentImageCompare() const { return imageCompare_; }       // rendered as a before / after split
    int64_t droppedFrames() const { return dropped_; }

    void setPreviewMaxSize(int w, int h);
    void setRenderSettings(BackendChoice backend, bool hardwareDecode);
    QString backendName() const { return QString::fromStdString(renderer_->backendName()); }
    void setVolume(double linear);
    void setCompare(bool on);   // before / after split in the player
    bool compare() const { return compare_; }

public slots:
    void play();
    void pause();
    void togglePlay();
    void stop();                 // pause and return to the start of the timeline
    void seek(qint64 frame);
    void step(int frames);
    void toStart();
    void toEnd();

signals:
    void frameShown(const QImage& image, qint64 frame, const QStringList& offline);
    void playingChanged(bool playing);
    void statusMessage(const QString& msg);

private:
    void onTick();
    void onDocumentChanged();
    void showFrame(const PreviewFrame& f);
    void requestCurrent();

    Document* doc_;
    std::unique_ptr<PreviewRenderer> renderer_;
    std::unique_ptr<AudioPlayer> audio_;
    QTimer timer_;
    bool playing_ = false;
    int64_t startFrame_ = 0;
    QImage image_;
    bool compare_ = false;
    bool imageCompare_ = false;
    QStringList offline_;
    int64_t imageFrame_ = -1;
    uint64_t imageVersion_ = 0;
    int64_t dropped_ = 0;
    int64_t lastTickFrame_ = -1;
};

}  // namespace fa::ui
