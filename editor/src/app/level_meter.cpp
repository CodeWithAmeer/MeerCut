#include "level_meter.h"

#include "theme.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace fa::ui {

namespace {
constexpr double kFallDbPerSec = 24.0;
constexpr qint64 kHoldMs = 1500;

double toDb(float linear) { return linear > 0 ? 20.0 * std::log10(static_cast<double>(linear)) : -1000.0; }
}  // namespace

LevelMeter::LevelMeter(QWidget* parent) : QWidget(parent) {
    setObjectName("levelMeter");
    setMinimumSize(110, 20);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setToolTip("Program level (L / R, -60 .. +3 dBFS): bar = peak, inner bar = RMS, marker = held peak. "
               "CLIP lights when the mix exceeds full scale; click it to reset.");
    clock_.start();
    anim_.setInterval(33);
    connect(&anim_, &QTimer::timeout, this, &LevelMeter::tick);
}

void LevelMeter::addLevels(const float peak[2], const float rms[2], bool clipped) {
    const qint64 now = clock_.elapsed();
    for (int c = 0; c < 2; ++c) {
        const double pk = std::max(kFloorDb, toDb(peak[c]));
        if (pk >= peakDb_[c]) peakDb_[c] = pk;   // rises at once, falls back in tick()
        rmsDb_[c] = std::max(kFloorDb, toDb(rms[c]));
        if (pk >= holdDb_[c]) {
            holdDb_[c] = pk;
            holdAt_[c] = now;
        }
    }
    if (clipped) clip_ = true;
    if (!anim_.isActive()) {
        lastTick_ = now;
        anim_.start();
    }
    update();
}

void LevelMeter::resetClip() {
    clip_ = false;
    update();
}

void LevelMeter::tick() {
    const qint64 now = clock_.elapsed();
    const double dt = (now - lastTick_) / 1000.0;
    lastTick_ = now;
    bool active = false;
    for (int c = 0; c < 2; ++c) {
        peakDb_[c] = std::max(kFloorDb, peakDb_[c] - kFallDbPerSec * dt);
        rmsDb_[c] = std::max(kFloorDb, rmsDb_[c] - kFallDbPerSec * dt);
        if (now - holdAt_[c] > kHoldMs) holdDb_[c] = std::max(kFloorDb, holdDb_[c] - kFallDbPerSec * dt);
        active |= peakDb_[c] > kFloorDb || holdDb_[c] > kFloorDb;
    }
    if (!active) anim_.stop();
    update();
}

QRect LevelMeter::clipRect() const { return QRect(width() - 30, 0, 30, height()); }

void LevelMeter::mousePressEvent(QMouseEvent* e) {
    if (clipRect().contains(e->pos())) resetClip();
}

void LevelMeter::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const QRect bars(0, 0, width() - 34, height());
    p.fillRect(bars, QColor(12, 13, 16));
    const int gap = 2, bh = (bars.height() - 3 * gap) / 2;
    auto xOf = [&](double db) {
        const double f = (std::clamp(db, kFloorDb, kTopDb) - kFloorDb) / (kTopDb - kFloorDb);
        return bars.x() + static_cast<int>(std::lround(f * bars.width()));
    };
    const int x0 = xOf(-18.0), x1 = xOf(-6.0), x2 = xOf(0.0);
    for (int c = 0; c < 2; ++c) {
        const int y = bars.y() + gap + c * (bh + gap);
        const int xp = xOf(peakDb_[c]);
        // green to -18 dB, yellow to -6 dB, orange to 0 dB, red above
        auto seg = [&](int from, int to, const QColor& col) {
            const int a = std::max(from, bars.x()), b = std::min(to, xp);
            if (b > a) p.fillRect(QRect(a, y, b - a, bh), col);
        };
        seg(bars.x(), x0, QColor(60, 190, 110));
        seg(x0, x1, QColor(200, 200, 70));
        seg(x1, x2, QColor(240, 150, 50));
        seg(x2, bars.right() + 1, QColor(235, 60, 60));
        const int xr = xOf(rmsDb_[c]);
        if (xr > bars.x()) p.fillRect(QRect(bars.x(), y + bh / 3, xr - bars.x(), bh - 2 * (bh / 3)), QColor(0, 0, 0, 90));
        if (holdDb_[c] > kFloorDb) p.fillRect(QRect(xOf(holdDb_[c]) - 1, y, 2, bh), QColor(240, 240, 240));
    }
    p.setPen(QColor(255, 255, 255, 60));   // 0 dBFS line
    p.drawLine(x2, bars.top(), x2, bars.bottom());
    const QRect cr = clipRect();
    p.fillRect(cr, clip_ ? QColor(220, 40, 40) : QColor(40, 20, 22));
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.75);
    f.setBold(true);
    p.setFont(f);
    p.setPen(clip_ ? QColor(255, 255, 255) : color::textDim);
    p.drawText(cr, Qt::AlignCenter, "CLIP");
}

}  // namespace fa::ui
