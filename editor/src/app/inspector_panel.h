// Property inspector (right panel), organised like Filmora's: Titles / Video / Audio tabs, Basic / Mask
// sub-tabs, collapsible groups, and a keyframe diamond on every animatable row.
//   * nothing selected   -> Project Info (name, location, resolution, frame rate, colour, audio, duration)
//   * a transition       -> Transition settings (type, duration, position, direction, softness)
//   * a video clip       -> Video: Basic (Transform, Crop, Compositing) and Mask; Audio for audio clips
//   * a title            -> Titles (text inspector: Text / Animation) and Video (Transform, Compositing)
//   * a caption          -> Titles (text inspector with the Caption group)
// Every change runs one undoable command through EditController (slider/spin drags merge into one
// undo step). Animated properties are edited at the playhead (auto-key).
#pragma once

#include "document.h"

#include <QWidget>

#include <functional>
#include <map>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QStackedWidget;
class QTabWidget;
class QToolButton;
class QVBoxLayout;

namespace fa::ui {

class AnalysisService;
class ColorPanel;
class EditController;
class StabilizationPanel;
class CutoutPanel;
class UpscalePanel;
class AnimationPanel;
class TrackingPanel;
class SpeedPanel;
class EffectStackEditor;
class TitleInspector;

class InspectorPanel : public QWidget {
    Q_OBJECT
public:
    InspectorPanel(Document* doc, EditController* edit, AnalysisService* analysis, QWidget* parent = nullptr);
    // Which sub-tab of the Video tab is shown ("basic" / "mask" / "tracking" / "stabilization"); the monitor edits masks in "mask"
    // and the tracking box in "tracking".
    QString videoSubTab() const;
    TitleInspector* titleInspector() const { return titleInspector_; }
    EffectStackEditor* effectEditor() const { return effects_; }
    ColorPanel* colorPanel() const { return color_; }
    SpeedPanel* speedPanel() const { return speed_; }
    TrackingPanel* trackingPanel() const { return tracking_; }
    StabilizationPanel* stabilizationPanel() const { return stabilization_; }
    CutoutPanel* cutoutPanel() const { return cutout_; }
    UpscalePanel* upscalePanel() const { return upscale_; }
    void showVideoSubTab(const QString& name);
    void showCrop();
    AnimationPanel* animationPanel() const { return animation_; }   // M9   // M9 Crop and Zoom (Alt+C): Video > Basic scrolled to the crop fields   // "basic" / "mask" / "tracking" / "stabilization" / "cutout"
    QTabWidget* clipTabs() const { return clipTabs_; }

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

signals:
    void seekRequested(qint64 frame);
    void editModeChanged();   // Basic <-> Mask (the monitor switches its handles)

private:
    struct Field {
        QDoubleSpinBox* spin = nullptr;
        std::string property;   // keys::property id ("mask.<id>.x" is resolved from the active mask)
        double scale = 1.0;     // display = value * scale
        std::function<double()> dynamicScale;   // overrides scale when set (e.g. sequence width)
    };
    struct KeyRow {
        QToolButton* diamond = nullptr;
        QToolButton* prev = nullptr;
        QToolButton* next = nullptr;
        std::vector<std::string> properties;   // toggled together (e.g. Position X and Y)
        bool mask = false;
    };

    QWidget* buildProjectPage();
    QWidget* buildTransitionPage();
    QWidget* buildVideoBasic();
    QWidget* buildVideoMask();
    QWidget* buildAudioPage();
    QGroupBox* group(const QString& title, QVBoxLayout* into);
    QDoubleSpinBox* addSpin(QWidget* parent, const std::string& property, double min, double max, int decimals,
                            const QString& suffix, double scale, bool mask = false,
                            std::function<double()> dynamicScale = {});
    QWidget* keyRow(QWidget* parent, std::vector<std::string> properties, bool mask = false);
    std::string resolve(const std::string& property, bool mask) const;   // "mask.cx" -> "mask.<id>.centerX"

    void refresh();
    void refreshValues();
    const Clip* clip() const;
    const Mask* activeMask() const;

    Document* doc_;
    EditController* edit_;
    QStackedWidget* stack_ = nullptr;
    QWidget* projectPage_ = nullptr;
    QWidget* transitionPage_ = nullptr;
    QTabWidget* clipTabs_ = nullptr;
    QTabWidget* videoSub_ = nullptr;
    QWidget* audioTab_ = nullptr;
    TitleInspector* titleInspector_ = nullptr;
    EffectStackEditor* effects_ = nullptr;
    ColorPanel* color_ = nullptr;
    QWidget* effectsTab_ = nullptr;
    SpeedPanel* speed_ = nullptr;   // M5
    TrackingPanel* tracking_ = nullptr;   // M5: Video > Tracking
    StabilizationPanel* stabilization_ = nullptr;   // M5: Video > Stabilization
    CutoutPanel* cutout_ = nullptr;                 // M7: Video > AI Cutout (Background Removal)
    UpscalePanel* upscale_ = nullptr;               // M7: Video > Upscale (Video Upscaling)
    QWidget* cutoutPage_ = nullptr;
    void updateTrackingShown();
    std::string lastClipId_;   // clip shown at the previous refresh (keeps the chosen tab while editing it)
    std::map<std::string, Field> fields_;     // key: property id (mask-relative ids start with "mask.")
    std::vector<KeyRow> keyRows_;
    std::string clipId_;
    bool updating_ = false;

    // project info
    QFormLayout* projectForm_ = nullptr;
    // clip basics
    QLineEdit* name_ = nullptr;
    QLabel* clipInfo_ = nullptr;
    QCheckBox* enabled_ = nullptr;
    QToolButton* lockScale_ = nullptr;
    QToolButton* flipH_ = nullptr;
    QToolButton* flipV_ = nullptr;
    QComboBox* blend_ = nullptr;
    QSlider* opacitySlider_ = nullptr;
    QWidget* compositingBox_ = nullptr;
    QWidget* transformBox_ = nullptr;
    QWidget* cropBox_ = nullptr;
    QWidget* stickerBox_ = nullptr;          // M9
    QPushButton* stickerColor_ = nullptr;
    QLineEdit* stickerText_ = nullptr;
    AnimationPanel* animation_ = nullptr;
    // masks
    QListWidget* maskList_ = nullptr;
    QWidget* maskProps_ = nullptr;
    QCheckBox* maskEnabled_ = nullptr;
    QCheckBox* maskInvert_ = nullptr;
    QComboBox* maskMode_ = nullptr;
    QWidget* cornerRow_ = nullptr;
    QLabel* maskHint_ = nullptr;
    // transition
    QComboBox* transType_ = nullptr;
    QDoubleSpinBox* transDuration_ = nullptr;
    QComboBox* transAlign_ = nullptr;
    QComboBox* transDir_ = nullptr;
    QDoubleSpinBox* transSoft_ = nullptr;
    QLabel* transInfo_ = nullptr;
    QFormLayout* transForm_ = nullptr;
    // audio (M6: automation, balance, fades, equaliser, the clip's track)
    QSlider* volume_ = nullptr;
    QDoubleSpinBox* volumeSpin_ = nullptr;
    QSlider* balance_ = nullptr;
    QDoubleSpinBox* balanceSpin_ = nullptr;
    QDoubleSpinBox* fadeIn_ = nullptr;
    QDoubleSpinBox* fadeOut_ = nullptr;
    QCheckBox* eqEnabled_ = nullptr;
    QComboBox* eqPreset_ = nullptr;
    std::vector<QSlider*> eqBands_;
    std::vector<QLabel*> eqValues_;
    QLabel* trackName_ = nullptr;
    QDoubleSpinBox* trackVolume_ = nullptr;
    QCheckBox* trackMute_ = nullptr;
    QCheckBox* trackSolo_ = nullptr;
    // M6 C: cleanup, loudness, ducking, master limiter
    QSlider* denoise_ = nullptr;
    QLabel* denoiseValue_ = nullptr;
    QComboBox* hum_ = nullptr;
    QDoubleSpinBox* highPass_ = nullptr;
    QDoubleSpinBox* lowPass_ = nullptr;
    QCheckBox* normalize_ = nullptr;
    QDoubleSpinBox* targetLufs_ = nullptr;
    QPushButton* measure_ = nullptr;
    QLabel* measured_ = nullptr;
    QCheckBox* duckEnabled_ = nullptr;
    QListWidget* duckTargets_ = nullptr;
    QDoubleSpinBox* duckAmount_ = nullptr;
    QDoubleSpinBox* duckThreshold_ = nullptr;
    QDoubleSpinBox* duckAttack_ = nullptr;
    QDoubleSpinBox* duckHold_ = nullptr;
    QDoubleSpinBox* duckRelease_ = nullptr;
    QCheckBox* limiter_ = nullptr;
    QDoubleSpinBox* limiterCeiling_ = nullptr;
    AnalysisService* analysis_ = nullptr;
    void applyCleanup();
    void applyDucking();
    void refreshAudioExtras(const Clip& c, const Track& track);
    void applyEq(int changedBand, const QString& preset);
    void fillTransitionTypes(bool audio);
    bool transitionTypesAudio_ = false;
};

}  // namespace fa::ui
