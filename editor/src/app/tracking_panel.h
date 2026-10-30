// Video > Tracking (M5): motion tracking of a region of the selected clip, like Filmora's Motion Tracking.
//   1. "Motion Tracking" on: a tracking box appears in the player (move it, resize it from its corners) on the frame
//      under the playhead.
//   2. Start: the clip's source is analysed in the background (forward and backward from that frame) with progress and
//      Cancel; the result (one sample per source frame with a confidence) is stored on the clip - one undo step.
//   3. Link Element: a title or clip above it follows the tracked point (position keyframes on every frame), or a
//      mosaic (pixelated rectangle on an adjustment layer) covers the tracked region.
// The status line reports frames tracked, the average confidence and lost frames, and warns when the media changed
// since the analysis (Track again).
#pragma once

#include "document.h"

#include <QWidget>

#include <string>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;

namespace fa::ui {

class AnalysisService;
class EditController;

class TrackingPanel : public QWidget {
    Q_OBJECT
public:
    TrackingPanel(Document* doc, EditController* edit, AnalysisService* analysis, QWidget* parent = nullptr);
    void refresh();
    bool startTracking();   // what Start does (tests)
    bool linkSelected();    // what Link does with the chosen element (tests)
    QComboBox* linkCombo() const { return link_; }
    bool tabShown() const { return shown_; }
    void setTabShown(bool shown);   // the inspector says whether the Tracking tab is visible (box in the player)

private:
    const Clip* clip() const;
    void syncBox();

    Document* doc_;
    EditController* edit_;
    AnalysisService* analysis_;
    QCheckBox* enable_ = nullptr;
    QPushButton* start_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_ = nullptr;
    QComboBox* link_ = nullptr;
    QPushButton* linkButton_ = nullptr;
    QPushButton* remove_ = nullptr;
    bool shown_ = false;
    bool updating_ = false;
};

}  // namespace fa::ui
