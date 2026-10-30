// M9: the inspector's Animation tab (Filmora: Motion Presets In / Out / Combo). Each preset writes ordinary keyframes
// (fa/model/motion_presets.h); thumbnails show the preset moving a small landscape (hover plays it).
#pragma once

#include "document.h"
#include "fa/model/motion_presets.h"

#include <QWidget>

class QButtonGroup;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QTimer;

namespace fa::ui {

class EditController;

class AnimationPanel : public QWidget {
    Q_OBJECT
public:
    AnimationPanel(Document* doc, EditController* edit, QWidget* parent = nullptr);
    QListWidget* list() const { return list_; }
    MotionSlot slot() const { return slot_; }
    void setSlot(MotionSlot s);
    bool apply(const QString& presetId);   // "" = None; the selected video clip
    void refresh();

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    const Clip* clip() const;
    QIcon thumbnail(const QString& presetId, double seconds) const;
    Document* doc_;
    EditController* edit_;
    MotionSlot slot_ = MotionSlot::In;
    QButtonGroup* slots_;
    QListWidget* list_;
    QLabel* status_;
    QTimer* hoverTimer_;
    QListWidgetItem* hoverItem_ = nullptr;
    int hoverStep_ = 0;
    bool updating_ = false;
};

}  // namespace fa::ui
