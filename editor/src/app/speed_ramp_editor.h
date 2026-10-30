// Speed Ramping editor (M5): the speed curve of a clip as an interactive graph, and the user's own ramp presets.
//
// RampGraph: x = position in the clip's source range (0 .. 100 %), y = speed on a log scale (0.1x .. 10x, 1x in the
// middle). The curve is drawn from the real speed map (smoothstep in output time between keys, time_map.h), not as
// straight lines. A click on empty space adds a key on the curve and drags it; dragging moves a key (between its
// neighbours; the first and last keys stay at 0 % and 100 %); double-click, right-click or Delete removes the selected
// key (not the end keys). One gesture (press to release) emits one serial = one undo step.
#pragma once

#include <QString>
#include <QWidget>

#include <utility>
#include <vector>

namespace fa::ui {

using RampKeys = std::vector<std::pair<double, double>>;   // (fraction 0..1 of the source range, speed)

class RampGraph : public QWidget {
    Q_OBJECT
public:
    explicit RampGraph(QWidget* parent = nullptr);
    void setKeys(RampKeys keys);   // ignored while the user drags
    const RampKeys& keys() const { return keys_; }
    int selected() const { return selected_; }
    void setSelected(int index);
    bool dragging() const { return dragging_; }
    QPointF toWidget(double fraction, double speed) const;
    std::pair<double, double> fromWidget(const QPointF& p) const;
    static constexpr int kMaxKeys = 12;

signals:
    void edited(const RampKeys& keys, int serial);
    void selectionChanged(int index);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    QSize sizeHint() const override { return {280, 190}; }

private:
    QRectF plot() const;
    int hit(const QPointF& p) const;
    void removeSelected();

    RampKeys keys_;
    int selected_ = -1;
    bool dragging_ = false;
    int serial_ = 0;
};

// Ramp presets the user saved (Speed Ramping > My Presets): speed_presets.json in the application-data folder
// (MEERCUT_SPEED_PRESETS points somewhere else; the tests use it).
class SpeedPresetLibrary {
public:
    struct Preset {
        QString name;
        RampKeys keys;
    };
    SpeedPresetLibrary();
    const std::vector<Preset>& presets() const { return presets_; }
    bool add(const QString& name, const RampKeys& keys);   // replaces a preset of the same name
    bool remove(const QString& name);
    QString file() const { return file_; }
    QString lastError() const { return error_; }

private:
    void load();
    bool save();
    QString file_;
    QString error_;
    std::vector<Preset> presets_;
};

}  // namespace fa::ui
