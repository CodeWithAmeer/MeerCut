// Stereo level meter of the program mix (M6), next to the player's transport.
//
// Two horizontal bars (L above R), -60 .. +3 dBFS: the bar shows the peak with a fall-back of 24 dB/s, a thin marker
// the peak held for 1.5 s, and the darker inner bar the RMS level. "CLIP" lights when a sample exceeded full scale
// (|x| > 1) and stays lit until clicked. Levels come from AudioPlayer::takeLevels() (the mix before the monitor
// volume, i.e. what export writes).
#pragma once

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

namespace fa::ui {

class LevelMeter : public QWidget {
    Q_OBJECT
public:
    explicit LevelMeter(QWidget* parent = nullptr);
    static constexpr double kFloorDb = -60.0;
    static constexpr double kTopDb = 3.0;

    // A new measurement (linear 0.. ; peak and rms per channel).
    void addLevels(const float peak[2], const float rms[2], bool clipped);
    double peakDb(int channel) const { return peakDb_[channel]; }   // shown peak (after the fall-back)
    double heldDb(int channel) const { return holdDb_[channel]; }
    bool clipLatched() const { return clip_; }
    void resetClip();

    QSize sizeHint() const override { return {132, 22}; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    void tick();
    QRect clipRect() const;
    double peakDb_[2] = {kFloorDb, kFloorDb};
    double rmsDb_[2] = {kFloorDb, kFloorDb};
    double holdDb_[2] = {kFloorDb, kFloorDb};
    qint64 holdAt_[2] = {0, 0};
    bool clip_ = false;
    QTimer anim_;
    QElapsedTimer clock_;
    qint64 lastTick_ = 0;
};

}  // namespace fa::ui
