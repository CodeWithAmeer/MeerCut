// Video > Stabilization (M5): removes camera shake from the selected clip, like Filmora's Stabilization.
//   * "Stabilization" on: the clip's frames are analysed in the background (progress, Cancel); the camera motion is
//     stored with the clip (one undo step) and the correction applies at once in the player and the export.
//   * Smoothness Weak / Normal / Strong, Strength 0-100 %, Auto Zoom and Max Zoom change the result immediately from
//     the stored motion (no new analysis). Auto Zoom zooms in just enough to hide the moving borders, up to Max Zoom
//     (101-125 %, default 110 %); if the full correction needs more, the camera path is kept inside the frame (less
//     crop, a little less steady). Without Auto Zoom the borders show what is below.
//   * The status line reports the analysed frames, the zoom and how often the zoom limit weakened the correction; it
//     warns when the media changed since the analysis (Analyze again). Compare (before / after) in the player shows
//     the shaky original on the left.
#pragma once

#include "document.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;

namespace fa::ui {

class AnalysisService;
class EditController;

class StabilizationPanel : public QWidget {
    Q_OBJECT
public:
    StabilizationPanel(Document* doc, EditController* edit, AnalysisService* analysis, QWidget* parent = nullptr);
    void refresh();
    bool analyze();   // what Analyze / turning Stabilization on does (tests)

private:
    const Clip* clip() const;
    void applySettings(const std::string& mergeKey);

    Document* doc_;
    EditController* edit_;
    AnalysisService* analysis_;
    QCheckBox* enable_ = nullptr;
    QComboBox* level_ = nullptr;
    QSlider* strength_ = nullptr;
    QSpinBox* strengthValue_ = nullptr;
    QCheckBox* autoZoom_ = nullptr;
    QSpinBox* maxZoom_ = nullptr;
    QPushButton* analyze_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_ = nullptr;
    bool updating_ = false;
    int dragSerial_ = 0;   // one undo step per slider drag
};

}  // namespace fa::ui
