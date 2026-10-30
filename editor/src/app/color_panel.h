// Color tab (M4): Basic (Color Adjust, Sharpen, Vignette), HSL (8 colour ranges), Curves (master / R / G / B
// tone curves and hue-vs-hue / hue-vs-saturation curves in an interactive editor) and Color Wheels (shadows,
// midtones, highlights), plus custom colour presets. Every control edits the clip's first effect of its type
// (EditController::colorTabTypes); the first change adds that effect with neutral settings, in one undo step.
// The same effects appear in the Effects tab, where they can be reordered, bypassed or masked.
#pragma once

#include "document.h"

#include "fa/render/effects.h"

#include <QWidget>

#include <array>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QTabWidget;
class QToolButton;

namespace fa::ui {

class EditController;
class EffectParamRow;

// Interactive curve graph. Tone curves: x, y in 0..1, the first and last points can move but not be deleted.
// Hue curves (periodic): x = hue / 360, y = 0.5 is neutral, every point can be deleted. A click adds a point on
// the curve and drags it; dragging moves a point between its neighbours; double-click, right-click or Delete
// removes the selected point. One gesture (press to release) emits one serial = one undo step.
class CurveEditor : public QWidget {
    Q_OBJECT
public:
    explicit CurveEditor(bool periodic, QWidget* parent = nullptr);
    void setCurve(std::vector<Vec2> points, const QColor& color);   // ignored while the user drags
    void setGhosts(std::vector<std::pair<std::vector<Vec2>, QColor>> curves);   // the other channels, faint
    const std::vector<Vec2>& points() const { return points_; }
    int selected() const { return selected_; }
    bool periodic() const { return periodic_; }
    QPointF toWidget(const Vec2& v) const;
    Vec2 fromWidget(const QPointF& p) const;
    static constexpr int kMaxPoints = 16;

signals:
    void edited(const std::vector<Vec2>& points, int serial);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    QSize sizeHint() const override { return {260, 200}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;

private:
    QRectF plot() const;
    int hit(const QPointF& p) const;
    void moveSelected(const QPointF& p);
    void removeSelected();
    bool periodic_;
    std::vector<Vec2> points_;
    QColor color_;
    std::vector<std::pair<std::vector<Vec2>, QColor>> ghosts_;
    int selected_ = -1;
    bool dragging_ = false;
    int serial_ = 0;
};

// Colour wheel. The puck shows the tint: the chroma part of the wheel's R, G, B offsets,
// x = r - (g + b) / 2, y = sqrt(3) / 2 (g - b) (red at 0 deg, green at 120, blue at 240, as on the hue circle;
// an R offset of 1 alone sits on the rim). Dragging changes the tint and keeps the offsets' mean (what Filmora
// stores as the wheel's lightness, EXP-016); double-click resets the tint.
class ColorWheel : public QWidget {
    Q_OBJECT
public:
    explicit ColorWheel(QWidget* parent = nullptr);
    void setOffsets(double r, double g, double b);   // ignored while the user drags
    QPointF chroma() const { return chroma_; }
    QPointF puckPosition() const;                     // widget coordinates
    static QPointF chromaOf(double r, double g, double b);
    static void offsetsOf(QPointF chroma, double mean, double& r, double& g, double& b);   // each clamped to -1..1

signals:
    void tintEdited(QPointF chroma, int serial);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    QSize sizeHint() const override { return {116, 116}; }
    QSize minimumSizeHint() const override { return {96, 96}; }

private:
    QPointF centre() const;
    double radius() const;
    QPointF fromWidget(const QPointF& p) const;
    QPointF chroma_;
    bool dragging_ = false;
    int serial_ = 0;
};

class ColorPanel : public QWidget {
    Q_OBJECT
public:
    ColorPanel(Document* doc, EditController* edit, QWidget* parent = nullptr);
    void setClip(const std::string& clipId);
    void refresh();   // document changed or playhead moved

    QTabWidget* tabs() const { return tabs_; }
    CurveEditor* toneCurve() const { return tone_; }
    CurveEditor* hueCurve() const { return hue_; }
    ColorWheel* wheel(int i) const { return wheels_[static_cast<size_t>(i)]; }
    const std::string& toneChannel() const { return toneChannel_; }
    const std::string& hueChannel() const { return hueChannel_; }
    int hslBand() const { return band_; }
    void selectToneChannel(const std::string& id);
    void selectHueChannel(const std::string& id);
    void selectHslBand(int band);
    // What "Save as Custom" and "Apply" do (after asking for the name / with the chosen preset).
    bool saveCustomPreset(const QString& name);
    bool applyPreset(const std::string& presetId);
    // LUT section (Basic): what "Import .cube" and "Relink" do after the file dialog.
    bool importLut(const QString& path);
    bool relinkMissingLut(const QString& path);
    QComboBox* lutCombo() const { return lutCombo_; }
    QLabel* lutStatus() const { return lutStatus_; }

protected:
    bool eventFilter(QObject* o, QEvent* e) override;   // the mouse wheel only edits a focused field (M2 rule)

private:
    struct Header {
        std::string type;
        QCheckBox* enabled = nullptr;
        QToolButton* reset = nullptr;
    };
    QWidget* buildBasic();
    QWidget* buildHsl();
    QWidget* buildCurves();
    QWidget* buildWheels();
    QWidget* buildPresetBar();
    QWidget* header(QWidget* parent, const std::string& type, const QString& title);
    EffectParamRow* row(QWidget* parent, const std::string& type, const std::string& param);
    void refreshPresets();
    void refreshHslSwatches(const Clip& c, const Time& t);
    const Clip* clip() const;

    Document* doc_;
    EditController* edit_;
    std::string clipId_;
    QTabWidget* tabs_ = nullptr;
    std::vector<Header> headers_;
    std::vector<EffectParamRow*> rows_;
    // HSL
    int band_ = 0;
    std::array<QToolButton*, 8> swatches_{};
    std::array<EffectParamRow*, 3> hslRows_{};
    QLabel* bandLabel_ = nullptr;
    // curves
    CurveEditor* tone_ = nullptr;
    CurveEditor* hue_ = nullptr;
    std::string toneChannel_ = "master";
    std::string hueChannel_ = "hueHue";
    std::vector<QToolButton*> channelButtons_;
    QComboBox* curvePreset_ = nullptr;
    // wheels
    std::array<ColorWheel*, 3> wheels_{};
    // presets
    QComboBox* presetCombo_ = nullptr;
    // LUT
    QWidget* buildLutSection(QWidget* parent);
    void refreshLut(const Clip* c);
    QComboBox* lutCombo_ = nullptr;
    QComboBox* lutDomain_ = nullptr;
    QLabel* lutStatus_ = nullptr;
    QToolButton* lutRelink_ = nullptr;
    QString lutComboKey_;   // what the chooser lists (rebuilt only when that changes)
    QToolButton* applyPreset_ = nullptr;
    QToolButton* deletePreset_ = nullptr;
    QToolButton* savePreset_ = nullptr;
    bool updating_ = false;
};

}  // namespace fa::ui
