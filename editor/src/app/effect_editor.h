// Effect stack editor (M4): the inspector's Effects tab. Every control is generated from the effect type's
// parameter schema (fa/render/effects.h): numbers -> slider + number field + keyframe diamond + reset,
// switches -> check box, choices -> combo box, colours -> colour button. Each effect panel has bypass, strength,
// move up/down, duplicate, reset and delete; the stack can be copied and pasted between clips. Every change is
// one undoable command (slider drags merge into one step); animated parameters get a key at the playhead.
#pragma once

#include "document.h"

#include "fa/render/effects.h"

#include <QWidget>

#include <functional>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSlider;
class QToolButton;
class QVBoxLayout;

namespace fa::ui {

class ColorButton;
class EditController;

// One numeric parameter: slider + number field + keyframe diamond + reset.
class EffectParamRow : public QWidget {
    Q_OBJECT
public:
    // property: keyframe property id ("fx.<id>.<param>"); scale: display = value * scale
    EffectParamRow(Document* doc, EditController* edit, const ParamDef& def, std::string property, double scale,
                   QWidget* parent = nullptr);
    void setClip(const std::string& clipId) { clipId_ = clipId; }
    // Color tab rows: edit parameter `param` of the clip's first effect of `effectType`, creating it on the first
    // change (EditController::setColorParams); without such an effect the row shows the neutral value. Widget
    // names become "color<slider|spin|key|reset>:<type>/<param>".
    void bindColorEffect(std::string effectType, std::string param);
    void refresh(const Clip& clip, const Time& t, bool playheadInside);
    QDoubleSpinBox* spin() const { return spin_; }
    QToolButton* keyButton() const { return key_; }
    QToolButton* resetButton() const { return reset_; }
    void setSliderVisible(bool on);   // compact rows: number field, keyframe and reset only

private:
    void commit(double display);
    Document* doc_;
    EditController* edit_;
    ParamDef def_;
    std::string property_;
    std::string colorType_, colorParam_;   // Color tab binding
    double scale_;
    double sliderScale_;
    std::string clipId_;
    QSlider* slider_;
    QDoubleSpinBox* spin_;
    QToolButton* key_;
    QToolButton* reset_;
    bool updating_ = false;
};

class EffectStackEditor : public QWidget {
    Q_OBJECT
public:
    // `sections`: which effects the editor lists (the Effects tab shows the whole stack)
    EffectStackEditor(Document* doc, EditController* edit, QWidget* parent = nullptr);
    void setClip(const std::string& clipId);
    void refresh();         // rebuilds the panels when the stack's structure changed, else updates values
    void refreshValues();   // playhead moved
    bool savePreset(const std::string& effectId, const QString& name);   // what the save button does

protected:
    bool eventFilter(QObject* o, QEvent* e) override;   // the wheel only edits a focused field (M2 rule)

private:
    struct Panel {
        std::string effectId;
        std::string type;
        QWidget* frame = nullptr;
        QCheckBox* enabled = nullptr;
        std::vector<EffectParamRow*> rows;
        std::vector<std::pair<std::string, QWidget*>> constants;   // param id -> check box / combo / colour button
    };
    void rebuild();
    QWidget* buildPanel(const EffectRef& e, size_t index, size_t count, Panel& p);
    const Clip* clip() const;

    Document* doc_;
    EditController* edit_;
    std::string clipId_;
    std::string signature_;
    QVBoxLayout* stackLayout_;
    QLabel* empty_;
    QToolButton* add_;
    QToolButton* copy_;
    QToolButton* paste_;
    std::vector<Panel> panels_;
    bool updating_ = false;
};

}  // namespace fa::ui
