// Video > Background Removal (M7): an AI cutout of the selected video clip, like Filmora's AI Portrait / Smart Cutout.
//   1. Model: People (Robust Video Matting, temporally stable) or General objects (BiRefNet, optional download).
//   2. Preview This Frame: the frame under the playhead is analysed in the background (with RVM's warm-up), to judge
//      the result before the whole clip.
//   3. Analyse Clip: every source frame the clip uses is matted in the background (progress, Cancel); the mattes are
//      cached outside the project per source frame, so trims, splits, moves and speed changes need no new analysis.
//   4. Refine (applied when frames are drawn, no re-analysis): threshold, shrink / grow, feather, spill cleanup,
//      flicker smoothing; background: transparent (the tracks below show) or a colour. "Add Background Media" puts a
//      picture or video on a track below the clip.
// Every change is an undo step (slider drags merge); the original media is never changed.
#pragma once

#include "analysis_service.h"
#include "document.h"

#include "fa/ai/inference.h"

#include <QWidget>

#include <functional>
#include <string>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;

namespace fa::ui {

class AnalysisService;
class EditController;

class CutoutPanel : public QWidget {
    Q_OBJECT
public:
    CutoutPanel(Document* doc, EditController* edit, AnalysisService* analysis, QWidget* parent = nullptr);
    void refresh();
    bool analyse();        // what Analyse Clip does (tests)
    bool previewFrame();   // what Preview This Frame does (tests): the frame under the playhead, in the background
    // Asked before a model that does not fit the GPU runs on the CPU (the text gives the time estimate); tests replace
    // it. Default: a question box. Nothing slow starts without a yes.
    std::function<bool(const QString&)> confirmSlow;

signals:
    void addBackgroundMediaRequested(const QString& clipId);   // the main window imports and places it below

private:
    const Clip* clip() const;
    void edit(const char* label, const std::function<void(AiCutout&)>& change, const char* mergeKey = nullptr);
    QString coverageText(const Clip& c) const;
    bool startJob(int64_t first, int64_t last, ai::Accelerator acc);

    Document* doc_;
    EditController* edit_;
    AnalysisService* analysis_;
    QCheckBox* enable_ = nullptr;
    QComboBox* model_ = nullptr;
    QComboBox* quality_ = nullptr;
    QPushButton* preview_ = nullptr;
    QPushButton* analyse_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_ = nullptr;
    QDoubleSpinBox* threshold_ = nullptr;
    QDoubleSpinBox* edge_ = nullptr;
    QDoubleSpinBox* feather_ = nullptr;
    QDoubleSpinBox* spill_ = nullptr;
    QDoubleSpinBox* smoothing_ = nullptr;
    QComboBox* background_ = nullptr;
    QPushButton* colour_ = nullptr;
    QPushButton* addMedia_ = nullptr;
    bool updating_ = false;
    AnalysisService::CutoutJob lastJob_;   // started again on the CPU after a GPU memory failure, if the user agrees
};

}  // namespace fa::ui
