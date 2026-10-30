// AI Tools > Auto Reframe (M7): the sequence re-framed for another aspect ratio, each video clip given a crop that
// follows its subject (faces found by YuNet, the clip's M5 motion track, or a fixed focal point). Analyse runs in the
// background; the result is shown on the clip's frames with the crop, rule-of-thirds guides and the margin, and
// smoothness, margin, zoom, hold and focal point re-plan it at once (no new analysis). Apply sets the new canvas and
// writes position keyframes and a scale on each clip's transform - one undo step, saved with the project, editable
// like any keyframes, and the same in the preview, Render Preview and export. The media is never changed.
#pragma once

#include "document.h"

#include "fa/ai/reframe.h"

#include <QDialog>
#include <QImage>

#include <atomic>
#include <map>
#include <thread>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;

namespace fa::ui {

class ReframeView;

class AutoReframeDialog : public QDialog {
    Q_OBJECT
public:
    AutoReframeDialog(Document* doc, QWidget* parent = nullptr);
    ~AutoReframeDialog() override;

    bool analyse();                          // what Analyse does (tests)
    bool busy() const { return worker_.joinable(); }
    bool apply();                            // what Apply does (tests)
    ai::ReframeOptions options() const;
    const std::map<std::string, ai::ReframePath>& paths() const { return paths_; }

private:
    void poll();
    void replan();
    void showFrame();
    std::vector<const Clip*> clips() const;

    Document* doc_;
    QComboBox* aspect_ = nullptr;
    QSpinBox* customW_ = nullptr;
    QSpinBox* customH_ = nullptr;
    QComboBox* scope_ = nullptr;
    QComboBox* subject_ = nullptr;
    QDoubleSpinBox* focusX_ = nullptr;
    QDoubleSpinBox* focusY_ = nullptr;
    QDoubleSpinBox* margin_ = nullptr;
    QDoubleSpinBox* zoom_ = nullptr;
    QSlider* smoothness_ = nullptr;
    QDoubleSpinBox* hold_ = nullptr;
    QPushButton* analyse_ = nullptr;
    QPushButton* apply_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_ = nullptr;
    QComboBox* clipList_ = nullptr;
    QSlider* scrub_ = nullptr;
    ReframeView* view_ = nullptr;
    QTimer* timer_ = nullptr;

    std::thread worker_;
    std::atomic<bool> cancel_{false}, done_{false};
    std::atomic<int> permille_{0};
    std::string error_;
    std::map<std::string, ai::ReframePath> paths_, pending_;   // clip id -> path
    std::map<std::string, std::filesystem::path> media_;
};

}  // namespace fa::ui
