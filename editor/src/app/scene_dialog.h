// AI Tools > Scene Detection (M7): the shots of a video found by signal analysis (colour-histogram changes, flash
// rejection, fades; fa/vision/scene_detect.h) - no learned model, so it is not called AI. The scenes are listed with
// thumbnails to review: a boundary can be removed (two scenes become one) or added at a frame. Then either Split
// Clip at Scenes (the selected timeline clip is cut at every boundary inside it, one undo step) or Add Scenes to
// Library (one library item per scene, each placing only its part of the file; one undo step).
#pragma once

#include "document.h"

#include "fa/vision/scene_detect.h"

#include <QDialog>
#include <QImage>

#include <atomic>
#include <thread>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;

namespace fa::ui {

class EditController;

class SceneDetectionDialog : public QDialog {
    Q_OBJECT
public:
    SceneDetectionDialog(Document* doc, EditController* edit, QWidget* parent = nullptr);
    ~SceneDetectionDialog() override;

    bool detect();                             // what Detect does (tests)
    bool busy() const { return worker_.joinable(); }
    const std::vector<int64_t>& sceneStarts() const { return starts_; }   // source frames; [0] = the range start
    bool removeBoundary(int scene);            // scene >= 1 joins the one before it
    bool addBoundary(int64_t frame);           // a new scene from this source frame
    bool splitClip();                          // Split Clip at Scenes
    bool addToLibrary();                       // Add Scenes to Library

private:
    void poll();
    void rebuildList();
    const MediaSource* source() const;
    const Clip* timelineClip() const;          // the selected clip of the chosen source, if any

    Document* doc_;
    EditController* edit_;
    QComboBox* source_ = nullptr;
    QCheckBox* clipOnly_ = nullptr;
    QSlider* sensitivity_ = nullptr;
    QDoubleSpinBox* minScene_ = nullptr;
    QCheckBox* fades_ = nullptr;
    QPushButton* detect_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_ = nullptr;
    QListWidget* list_ = nullptr;
    QPushButton* remove_ = nullptr;
    QSpinBox* addFrame_ = nullptr;
    QPushButton* add_ = nullptr;
    QPushButton* split_ = nullptr;
    QPushButton* library_ = nullptr;
    QTimer* timer_ = nullptr;

    std::thread worker_;
    std::atomic<bool> cancel_{false}, done_{false};
    std::atomic<int> permille_{0};
    std::string error_;
    vision::SceneResult result_;
    std::string sourceId_;                     // what the scenes belong to
    int64_t first_ = 0, last_ = -1;            // the analysed source frames
    double fps_ = 0;
    std::vector<int64_t> starts_;
    std::vector<QImage> thumbs_;
    std::vector<int> kinds_;                   // 0 start, 1 cut, 2 fade, 3 added by hand
};

}  // namespace fa::ui
