#include "upscale_panel.h"

#include "edit_controller.h"
#include "theme.h"

#include "fa/ai/models.h"
#include "fa/ai/prepare.h"
#include "fa/model/edit_ops.h"
#include "fa/render/frame_store.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace fa::ui {

UpscalePanel::UpscalePanel(Document* doc, EditController* editor, AnalysisService* analysis, QWidget* parent)
    : QWidget(parent), doc_(doc), edit_(editor), analysis_(analysis) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(12, 12, 12, 12);
    v->setSpacing(10);
    auto* form = new QFormLayout();
    mode_ = new QComboBox(this);
    mode_->setObjectName("upscaleMode");
    mode_->addItem("Off", static_cast<int>(UpscaleMode::Off));
    mode_->addItem("Lanczos (not AI)", static_cast<int>(UpscaleMode::Lanczos));
    mode_->addItem("AI (Real-ESRGAN)", static_cast<int>(UpscaleMode::Ai));
    mode_->setToolTip("Lanczos: a sharp classic resampling filter, immediate. AI: Real-ESRGAN restores detail; the frames "
                      "are prepared in the background (and before an export).");
    form->addRow("Upscale", mode_);
    model_ = new QComboBox(this);
    model_->setObjectName("upscaleModel");
    model_->addItem("General (photos, film)", "general");
    model_->addItem("Anime / cartoon (fast)", "anime");
    form->addRow("AI model", model_);
    scale_ = new QComboBox(this);
    scale_->setObjectName("upscaleScale");
    scale_->addItem("2x", 2);
    scale_->addItem("3x (anime model)", 3);
    scale_->addItem("4x", 4);
    form->addRow("At most", scale_);
    deflicker_ = new QDoubleSpinBox(this);
    deflicker_->setObjectName("upscaleDeflicker");
    deflicker_->setRange(0, 1);
    deflicker_->setSingleStep(0.1);
    deflicker_->setKeyboardTracking(false);
    deflicker_->setToolTip("Keep areas that do not change between frames still (AI detail can flicker otherwise)");
    form->addRow("Flicker reduction", deflicker_);
    v->addLayout(form);
    prepare_ = new QPushButton("Prepare AI Frames", this);
    prepare_->setObjectName("upscalePrepare");
    prepare_->setToolTip("Upscale every frame the clip shows now, in the background (the export does it too)");
    v->addWidget(prepare_);
    progress_ = new QProgressBar(this);
    progress_->setObjectName("upscaleProgress");
    v->addWidget(progress_);
    status_ = new QLabel(this);
    status_->setObjectName("upscaleStatus");
    status_->setWordWrap(true);
    v->addWidget(status_);
    auto* note = new QLabel("Runs on this PC (AI Tools > Model Manager installs Real-ESRGAN). The original media is not "
                            "changed; the enlarged frames are kept outside the project, so trims and moves reuse them.",
                            this);
    note->setWordWrap(true);
    note->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    v->addWidget(note);
    v->addStretch(1);

    connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
        if (updating_) return;
        const auto m = static_cast<UpscaleMode>(mode_->currentData().toInt());
        edit("Upscale", [m](ClipUpscale& u) { u.mode = m; });
    });
    connect(model_, &QComboBox::currentIndexChanged, this, [this] {
        if (updating_) return;
        const std::string id = model_->currentData().toString().toStdString();
        edit("Upscale Model", [id](ClipUpscale& u) {
            u.model = id;
            if (id == "general" && u.scale == 3) u.scale = 4;   // the general model has no 3x
        });
    });
    connect(scale_, &QComboBox::currentIndexChanged, this, [this] {
        if (updating_) return;
        const int s = scale_->currentData().toInt();
        edit("Upscale Factor", [s](ClipUpscale& u) { u.scale = s; });
    });
    connect(deflicker_, &QDoubleSpinBox::valueChanged, this, [this](double x) {
        if (!updating_) edit("Upscale Flicker Reduction", [x](ClipUpscale& u) { u.deflicker = x; }, "upscale.deflicker");
    });
    connect(prepare_, &QPushButton::clicked, this, [this] {
        if (analysis_->busy() && analysis_->kind() == "prepare") analysis_->cancel();
        else if (!analysis_->busy()) prepare();
    });
    connect(analysis_, &AnalysisService::started, this, [this] { refresh(); });
    connect(analysis_, &AnalysisService::progress, this, [this](int done, int total) {
        if (analysis_->kind() != "prepare") return;
        progress_->setRange(0, std::max(1, total));
        progress_->setValue(done);
    });
    connect(analysis_, &AnalysisService::prepareFinished, this, [this](const QString&, const QString& summary) {
        emit edit_->message(summary);
        doc_->refreshPictures();   // the preview redraws with the prepared frames
        refresh();
    });
    connect(analysis_, &AnalysisService::failed, this, [this](const QString&, const QString& msg) {
        if (analysis_->kind() == "prepare" && msg != "cancelled") emit edit_->message(msg);
        refresh();
    });
    connect(doc_, &Document::changed, this, [this] { refresh(); });
    connect(doc_, &Document::selectionChanged, this, [this] { refresh(); });
    refresh();
}

const Clip* UpscalePanel::clip() const {
    for (const auto& id : doc_->selection()) {
        const Track* tr = nullptr;
        const Clip* c = edit::findClip(doc_->sequence(), id, &tr);
        if (c && tr->kind == TrackKind::Video && !c->isGenerated()) return c;
    }
    return nullptr;
}

void UpscalePanel::edit(const char* label, const std::function<void(ClipUpscale&)>& change, const char* mergeKey) {
    const Clip* c = clip();
    if (!c) return;
    edit_->modifyClip(c->id, label, [change](Clip& x) { change(x.upscale); }, mergeKey ? mergeKey : "");
}

QString UpscalePanel::sizeText(const Clip& c) const {
    const MediaSource* src = doc_->project().findSource(c.sourceId);
    if (!src || !src->width) return {};
    const Sequence& seq = doc_->sequence();
    const auto [tw, th] = upscaleTarget(c, src->width, src->height, seq.width, seq.height);
    if (tw == 0)
        return QString("The clip (%1 x %2) is shown at or below its own size here: there is nothing to enlarge.")
            .arg(src->width)
            .arg(src->height);
    return QString("%1 x %2 enlarged to %3 x %4 (the size it is shown at, at most %5x).")
        .arg(src->width)
        .arg(src->height)
        .arg(tw)
        .arg(th)
        .arg(c.upscale.scale);
}

void UpscalePanel::refresh() {
    const Clip* c = clip();
    updating_ = true;
    for (QWidget* w : std::initializer_list<QWidget*>{mode_, model_, scale_, deflicker_, prepare_}) w->setEnabled(c != nullptr);
    if (c) {
        const ClipUpscale& u = c->upscale;
        mode_->setCurrentIndex(std::max(0, mode_->findData(static_cast<int>(u.mode))));
        model_->setCurrentIndex(std::max(0, model_->findData(QString::fromStdString(u.model))));
        scale_->setCurrentIndex(std::max(0, scale_->findData(u.scale)));
        deflicker_->setValue(u.deflicker);
        const bool ai = u.mode == UpscaleMode::Ai;
        model_->setEnabled(ai);
        deflicker_->setEnabled(ai);
        const bool running = analysis_->busy() && analysis_->kind() == "prepare";
        prepare_->setVisible(ai);
        prepare_->setText(running ? "Cancel" : "Prepare AI Frames");
        prepare_->setEnabled(running || !analysis_->busy());
        progress_->setVisible(ai);
        QString text = u.mode == UpscaleMode::Off ? QString("Off: the clip is drawn from its own frames.") : sizeText(*c);
        if (ai && !text.isEmpty()) {
            const ai::ModelInfo* m = ai::findModel("realesrgan");
            if (!m || ai::modelStatus(*m, false) != ai::ModelStatus::Installed)
                text += " Real-ESRGAN is not installed: AI Tools > Model Manager.";
            else if (running)
                text += " Preparing...";
            else
                text += " Frames not prepared yet are shown with the plain enlargement.";
        }
        status_->setText(text);
    } else {
        status_->setText("Select a video clip on the timeline.");
    }
    updating_ = false;
}

bool UpscalePanel::prepare() {
    const Clip* c = clip();
    if (!c || c->upscale.mode != UpscaleMode::Ai) return false;
    const ai::ModelInfo* m = ai::findModel("realesrgan");
    if (!m || ai::modelStatus(*m, false) != ai::ModelStatus::Installed) {
        emit edit_->message("Video Upscaling: install Real-ESRGAN first (AI Tools > Model Manager)");
        return false;
    }
    const Sequence& seq = doc_->sequence();
    AnalysisService::PrepareJob job;
    job.clipId = c->id;
    job.snapshot = doc_->snapshot();
    job.fromFrame = timeToFrame(c->timelineStart, seq.frameRate, Rounding::Floor);
    job.toFrame = timeToFrame(c->timelineEnd(), seq.frameRate, Rounding::Ceil);
    // long work only after a yes, with what it costs (Real-ESRGAN on a CPU is minutes per second of video)
    const ai::PreparationEstimate est =
        ai::estimatePreparation(*job.snapshot.project, job.snapshot.sequenceId, job.snapshot.projectFile, job.fromFrame, job.toFrame);
    if (est.frames() == 0) {
        emit edit_->message("Video Upscaling: every frame the clip shows is prepared already");
        return false;
    }
    if (ai::needsConfirmation(est)) {
        const QString text = QString("Preparing %1.%2 Start?")
                                 .arg(QString::fromStdString(est.text))
                                 .arg(est.cpu ? " There is no usable GPU for it, so this PC's processor does the work." : "");
        const bool yes = confirmSlow ? confirmSlow(text) : QMessageBox::question(this, "Video Upscaling", text) == QMessageBox::Yes;
        if (!yes) return false;
    }
    if (!analysis_->startPrepare(job)) {
        emit edit_->message("Another analysis is running");
        return false;
    }
    refresh();
    return true;
}

}  // namespace fa::ui
