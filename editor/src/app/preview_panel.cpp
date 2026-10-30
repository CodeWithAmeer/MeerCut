#include "panels.h"

#include "level_meter.h"

#include "playback_controller.h"
#include "theme.h"

#include <QComboBox>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace fa::ui {

PreviewPanel::PreviewPanel(Document* doc, EditController* edit, PlaybackController* playback, QWidget* parent)
    : QWidget(parent), doc_(doc), playback_(playback) {
    auto* lay = new QVBoxLayout(this);
    layout_ = lay;
    lay->setContentsMargins(8, 6, 8, 6);
    lay->setSpacing(6);

    auto* head = new QHBoxLayout();
    auto* title = new QLabel("Timeline Player", this);
    title->setStyleSheet(QString("color: %1; font-weight: 600;").arg(color::accent.name()));
    quality_ = new QComboBox(this);
    quality_->addItems({"Full", "1/2", "1/4"});
    quality_->setToolTip("Preview quality (export is always full quality)");
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(quality_);
    lay->addLayout(head);

    canvas_ = new PreviewCanvas(doc_, edit, playback_, this);
    canvas_->setMinimumSize(320, 180);
    canvas_->setAccessibleName("Preview");
    canvas_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    lay->addWidget(canvas_, 1);
    canvas_->installEventFilter(this);

    auto* scrubRow = new QHBoxLayout();
    scrub_ = new QSlider(Qt::Horizontal, this);
    scrub_->setToolTip("Scrub the timeline");
    scrub_->setAccessibleName("Scrubber");
    time_ = new QLabel(this);
    time_->setStyleSheet(QString("color: %1; font-family: Consolas, monospace;").arg(color::text.name()));
    scrubRow->addWidget(scrub_, 1);
    scrubRow->addWidget(time_);
    lay->addLayout(scrubRow);

    auto* transport = new QHBoxLayout();
    auto mk = [this](const char* ic, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setIcon(icon(ic));
        b->setIconSize(QSize(22, 22));
        b->setToolTip(tip);
        return b;
    };
    auto* startB = mk("start", "Go to start (Home)");
    auto* prevB = mk("prev", "Previous frame (Left)");
    play_ = mk("play", "Play / Pause (Space)");
    play_->setIconSize(QSize(28, 28));
    play_->setObjectName("playButton");
    play_->setAccessibleName("Play");
    auto* stopB = mk("stop", "Stop (return to start)");
    auto* nextB = mk("next", "Next frame (Right)");
    auto* endB = mk("end", "Go to end (End)");
    auto* snap = mk("camera", "Save a snapshot of the current frame (full resolution PNG)");
    auto* compare = mk("compare", "Compare: before (left, without effects and colour) and after (right)");
    compare->setObjectName("compareButton");
    compare->setCheckable(true);
    compare->setIconSize(QSize(18, 18));
    head->insertWidget(2, compare);   // header row, before the quality menu (the transport row is full)
    meter_ = new LevelMeter(this);   // M6: program level of what is playing
    head->insertWidget(2, meter_);
    head->insertSpacing(3, 8);
    meterPoll_ = new QTimer(this);
    meterPoll_->setInterval(33);
    connect(meterPoll_, &QTimer::timeout, this, [this] {
        const AudioPlayer::Levels lv = playback_->audio().takeLevels();
        if (lv.samples > 0) meter_->addLevels(lv.peak, lv.rms, lv.clipped);
    });
    auto* full = mk("fullscreen", "Full screen player (Esc or double-click to leave)");
    full->setObjectName("fullscreenButton");
    auto* volIcon = new QLabel(this);
    volIcon->setPixmap(icon("volume").pixmap(18, 18));
    auto* vol = new QSlider(Qt::Horizontal, this);
    vol->setRange(0, 100);
    vol->setValue(100);
    vol->setFixedWidth(90);
    vol->setToolTip("Monitor volume (does not change the project)");
    transport->addWidget(startB);
    transport->addWidget(prevB);
    transport->addWidget(play_);
    transport->addWidget(stopB);
    transport->addWidget(nextB);
    transport->addWidget(endB);
    transport->addStretch(1);
    transport->addWidget(snap);
    transport->addWidget(full);
    transport->addSpacing(8);
    transport->addWidget(volIcon);
    transport->addWidget(vol);
    lay->addLayout(transport);

    connect(startB, &QToolButton::clicked, playback_, &PlaybackController::toStart);
    connect(endB, &QToolButton::clicked, playback_, &PlaybackController::toEnd);
    connect(prevB, &QToolButton::clicked, playback_, [this] { playback_->step(-1); });
    connect(nextB, &QToolButton::clicked, playback_, [this] { playback_->step(1); });
    connect(play_, &QToolButton::clicked, playback_, &PlaybackController::togglePlay);
    connect(stopB, &QToolButton::clicked, playback_, &PlaybackController::stop);
    connect(snap, &QToolButton::clicked, this, &PreviewPanel::snapshotRequested);
    connect(full, &QToolButton::clicked, this, &PreviewPanel::toggleFullscreen);
    connect(compare, &QToolButton::toggled, this, [this](bool on) {
        canvas_->setCompare(on);
        playback_->setCompare(on);
    });
    connect(vol, &QSlider::valueChanged, this, [this](int v) { playback_->setVolume(v / 100.0); });
    connect(quality_, &QComboBox::currentIndexChanged, this, [this](int) { updatePreviewSize(); });
    connect(playback_, &PlaybackController::playingChanged, this, [this](bool on) {
        play_->setIcon(icon(on ? "pause" : "play"));
        if (on) {
            playback_->audio().takeLevels();   // drop what is left from before
            meterPoll_->start();
        } else {
            meterPoll_->stop();
        }
    });
    connect(playback_, &PlaybackController::frameShown, this,
            [this](const QImage& img, qint64, const QStringList& off) { canvas_->setFrame(img, off); });
    connect(scrub_, &QSlider::sliderPressed, this, [this] { scrubbing_ = true; });
    connect(scrub_, &QSlider::sliderReleased, this, [this] { scrubbing_ = false; });
    connect(scrub_, &QSlider::valueChanged, this, [this](int v) {
        if (scrub_->isSliderDown()) playback_->seek(v);
    });
    connect(doc_, &Document::playheadChanged, this, &PreviewPanel::updateTime);
    connect(doc_, &Document::changed, this, &PreviewPanel::updateTime);
    updateTime();
}

void PreviewPanel::updateTime() {
    const Sequence& s = doc_->sequence();
    const int64_t total = s.frameCount();
    if (!scrub_->isSliderDown()) {
        QSignalBlocker b(scrub_);
        scrub_->setRange(0, std::max<int64_t>(0, total - 1));
        scrub_->setValue(static_cast<int>(doc_->playhead()));
    }
    time_->setText(QString::fromStdString(formatTimecode(doc_->playhead(), s.frameRate)) + "  /  " +
                   QString::fromStdString(formatTimecode(total, s.frameRate)));
}

void PreviewPanel::updatePreviewSize() {
    const int div = quality_->currentIndex() == 2 ? 4 : quality_->currentIndex() == 1 ? 2 : 1;
    const qreal dpr = devicePixelRatioF();
    playback_->setPreviewMaxSize(std::max(64, static_cast<int>(canvas_->width() * dpr / div)),
                                 std::max(36, static_cast<int>(canvas_->height() * dpr / div)));
}

void PreviewPanel::toggleFullscreen() {
    if (!fullscreen_) {
        fullscreen_ = new QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint);
        fullscreen_->setWindowTitle("Player");
        fullscreen_->setStyleSheet("background: black;");
        auto* l = new QVBoxLayout(fullscreen_);
        l->setContentsMargins(0, 0, 0, 0);
        layout_->removeWidget(canvas_);
        l->addWidget(canvas_);
        fullscreen_->installEventFilter(this);
        fullscreen_->showFullScreen();
        fullscreen_->activateWindow();
    } else {
        QWidget* w = fullscreen_;
        fullscreen_ = nullptr;
        w->layout()->removeWidget(canvas_);
        canvas_->setParent(this);
        layout_->insertWidget(1, canvas_, 1);
        canvas_->show();
        w->deleteLater();
        window()->activateWindow();
    }
    updatePreviewSize();
}

bool PreviewPanel::eventFilter(QObject* o, QEvent* e) {
    if (o == canvas_ && e->type() == QEvent::Resize) updatePreviewSize();
    if (fullscreen_ && (o == fullscreen_ || o == canvas_)) {
        if (e->type() == QEvent::KeyPress) {
            const int k = static_cast<QKeyEvent*>(e)->key();
            if (k == Qt::Key_Escape || k == Qt::Key_F) {
                toggleFullscreen();
                return true;
            }
            if (k == Qt::Key_Space) {
                playback_->togglePlay();
                return true;
            }
            if (k == Qt::Key_Left || k == Qt::Key_Right) {
                playback_->step(k == Qt::Key_Left ? -1 : 1);
                return true;
            }
        } else if (e->type() == QEvent::MouseButtonDblClick) {
            toggleFullscreen();
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

void PreviewPanel::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    updatePreviewSize();
}

}  // namespace fa::ui
