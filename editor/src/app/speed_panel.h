// Speed tab (M5), organised like Filmora's: Uniform Speed (speed slider + value 0.1x .. 10x, duration, Reverse
// Speed, Reset), Speed Ramping (presets incl. "My Presets", the ramp graph, the selected key's speed and position,
// Reset, Save as Preset) and, under both, Maintain Pitch and Frame Interpolation; a hold duration for freeze frames.
// Every change is one undoable command through EditController::setClipSpeed (drags merge into one undo step); the
// clip's linked audio follows and the clips after it ripple (fa::edit::setSpeed).
#pragma once

#include "document.h"
#include "speed_ramp_editor.h"

#include <QWidget>

#include <functional>
#include <memory>
#include <string>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QStackedWidget;
class QTabWidget;

namespace fa::ui {

class AnalysisService;
class EditController;

class SpeedPanel : public QWidget {
    Q_OBJECT
public:
    SpeedPanel(Document* doc, EditController* edit, QWidget* parent = nullptr, AnalysisService* analysis = nullptr);
    // M7: asked before long AI preparation (tests answer it). Default: a message box.
    std::function<bool(const QString&)> confirmSlow;
    void refresh();   // from the selected clip
    QTabWidget* tabs() const { return tabs_; }
    RampGraph* rampGraph() const { return graph_; }
    // Save the selected clip's ramp as a user preset (the button asks for the name).
    bool saveRampPreset(const QString& name);
    SpeedPresetLibrary* presets() const { return presets_.get(); }

    // Slider position (0..1000) <-> speed: logarithmic, 1x in the middle.
    static double sliderToSpeed(int pos);
    static int speedToSlider(double speed);

private:
    const Clip* clip() const;
    void applySpeed(double speed, bool merge);
    void applyDuration(const QString& text);
    void applyFlags();
    void applyRamp(const RampKeys& keys, const std::string& mergeKey, const std::string& presetId);
    void applyRampPreset(int comboIndex);
    void fillRampPresets();
    RampKeys rampFractions(const Clip& c) const;   // the clip's keys as fractions of its source range
    Time shownSourceLength(const Clip& c) const;   // the source the clip plays, at most to the end of the media
    void showKeyFields(int selected);

    Document* doc_;
    EditController* edit_;
    AnalysisService* analysis_ = nullptr;
    QPushButton* prepareAi_ = nullptr;   // M7: AI Interpolation frames of the clip, in the background
    QLabel* aiNote_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QStackedWidget* uniformStack_ = nullptr;
    QWidget* uniformPage_ = nullptr;
    QWidget* freezePage_ = nullptr;
    QSlider* slider_ = nullptr;
    QDoubleSpinBox* value_ = nullptr;
    QLineEdit* duration_ = nullptr;
    QCheckBox* reverse_ = nullptr;
    QCheckBox* pitch_ = nullptr;
    QComboBox* interpolation_ = nullptr;
    QPushButton* reset_ = nullptr;
    QLabel* info_ = nullptr;
    QLineEdit* hold_ = nullptr;
    QLabel* holdInfo_ = nullptr;
    // Speed Ramping
    QWidget* rampPage_ = nullptr;
    QComboBox* rampPreset_ = nullptr;
    RampGraph* graph_ = nullptr;
    QDoubleSpinBox* keySpeed_ = nullptr;
    QDoubleSpinBox* keyPos_ = nullptr;
    QPushButton* keyDelete_ = nullptr;
    QWidget* keyFields_ = nullptr;   // speed / position / delete of the selected point
    QLabel* keyHint_ = nullptr;      // shown instead while no point is selected
    QPushButton* rampReset_ = nullptr;
    QPushButton* rampSave_ = nullptr;
    QLabel* rampInfo_ = nullptr;
    QWidget* options_ = nullptr;   // Maintain Pitch, Frame Interpolation (both tabs)
    std::unique_ptr<SpeedPresetLibrary> presets_;
    std::string clipId_;
    bool updating_ = false;
    int dragSerial_ = 0;
};

// "hh:mm:ss:ff" (frames at `rate`), "ss.sss" or "mm:ss.sss" -> time; nullopt if unreadable.
std::optional<Time> parseDuration(const QString& text, const FrameRate& rate);

}  // namespace fa::ui
