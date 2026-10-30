// Video > Upscale (M7): a clip's frames enlarged before they are composited, for low-resolution footage in a larger
// project. Lanczos (not AI: a sharp classic filter, drawn at once) or Real-ESRGAN (AI: general or anime/cartoon model,
// prepared in the background, kept in the analysis cache). Only enlarges up to the size the clip is shown at; the
// frames are prepared for exactly that size. Flicker reduction keeps static areas still between frames.
#pragma once

#include "analysis_service.h"
#include "document.h"

#include <QWidget>

#include <functional>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;

namespace fa::ui {

class EditController;

class UpscalePanel : public QWidget {
    Q_OBJECT
public:
    UpscalePanel(Document* doc, EditController* editor, AnalysisService* analysis, QWidget* parent = nullptr);
    void refresh();
    bool prepare();   // what Prepare does (tests): AI-upscale the clip's frames in the background
    // Asked before long work (minutes on the GPU, or the CPU); tests answer it. Default: a message box.
    std::function<bool(const QString&)> confirmSlow;

private:
    const Clip* clip() const;
    void edit(const char* label, const std::function<void(ClipUpscale&)>& change, const char* mergeKey = nullptr);
    QString sizeText(const Clip& c) const;

    Document* doc_;
    EditController* edit_;
    AnalysisService* analysis_;
    QComboBox* mode_ = nullptr;
    QComboBox* model_ = nullptr;
    QComboBox* scale_ = nullptr;
    QDoubleSpinBox* deflicker_ = nullptr;
    QPushButton* prepare_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_ = nullptr;
    bool updating_ = false;
};

}  // namespace fa::ui
