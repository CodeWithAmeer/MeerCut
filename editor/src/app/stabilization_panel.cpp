#include "stabilization_panel.h"

#include "analysis_service.h"
#include "edit_controller.h"
#include "theme.h"

#include "fa/model/edit_ops.h"
#include "fa/model/stabilize_plan.h"
#include "fa/model/time_map.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace fa::ui {

StabilizationPanel::StabilizationPanel(Document* doc, EditController* edit, AnalysisService* analysis, QWidget* parent)
    : QWidget(parent), doc_(doc), edit_(edit), analysis_(analysis) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(12, 12, 12, 12);
    v->setSpacing(10);
    enable_ = new QCheckBox("Stabilization", this);
    enable_->setObjectName("stabEnable");
    enable_->setToolTip("Analyses the clip's camera motion and removes the shake");
    v->addWidget(enable_);
    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    level_ = new QComboBox(this);
    level_->setObjectName("stabLevel");
    level_->addItem("Weak", static_cast<int>(StabilizeLevel::Weak));
    level_->addItem("Normal", static_cast<int>(StabilizeLevel::Normal));
    level_->addItem("Strong", static_cast<int>(StabilizeLevel::Strong));
    level_->setToolTip("How much of the camera movement is smoothed away (Strong also calms slow drifts)");
    form->addRow("Smoothness", level_);
    auto* sr = new QHBoxLayout();
    strength_ = new QSlider(Qt::Horizontal, this);
    strength_->setObjectName("stabStrength");
    strength_->setRange(0, 100);
    strengthValue_ = new QSpinBox(this);
    strengthValue_->setObjectName("stabStrengthValue");
    strengthValue_->setRange(0, 100);
    strengthValue_->setSuffix(" %");
    sr->addWidget(strength_, 1);
    sr->addWidget(strengthValue_);
    form->addRow("Strength", sr);
    v->addLayout(form);
    autoZoom_ = new QCheckBox("Auto Zoom (hide the borders)", this);
    autoZoom_->setObjectName("stabAutoZoom");
    autoZoom_->setToolTip("Zooms in just enough that the moving picture always covers the frame");
    v->addWidget(autoZoom_);
    auto* zf = new QFormLayout();
    maxZoom_ = new QSpinBox(this);
    maxZoom_->setObjectName("stabMaxZoom");
    maxZoom_->setRange(101, 125);
    maxZoom_->setSuffix(" %");
    maxZoom_->setValue(110);
    maxZoom_->setToolTip("The most Auto Zoom may crop. If the full correction needs more, the camera path is kept inside "
                         "the frame instead: less crop, a little less steady");
    zf->addRow("Max Zoom", maxZoom_);
    v->addLayout(zf);
    auto* row = new QHBoxLayout();
    analyze_ = new QPushButton("Analyze", this);
    analyze_->setObjectName("stabAnalyze");
    progress_ = new QProgressBar(this);
    progress_->setObjectName("stabProgress");
    row->addWidget(analyze_);
    row->addWidget(progress_, 1);
    v->addLayout(row);
    status_ = new QLabel(this);
    status_->setObjectName("stabStatus");
    status_->setWordWrap(true);
    v->addWidget(status_);
    auto* hint = new QLabel("Compare (before / after) in the player shows the original on the left.", this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    v->addWidget(hint);
    v->addStretch(1);

    connect(enable_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_) return;
        const Clip* c = clip();
        if (!c) return;
        if (on) {
            if (!analyze()) refresh();
        } else {
            if (analysis_->busy() && analysis_->kind() == "stabilization" && analysis_->clipId() == c->id) analysis_->cancel();
            edit_->setStabilization(c->id, std::nullopt);
            refresh();
        }
    });
    connect(level_, &QComboBox::currentIndexChanged, this, [this] {
        if (!updating_) applySettings({});
    });
    connect(autoZoom_, &QCheckBox::toggled, this, [this] {
        maxZoom_->setEnabled(autoZoom_->isChecked());
        if (!updating_) applySettings({});
    });
    connect(maxZoom_, &QSpinBox::valueChanged, this, [this] {
        if (!updating_) applySettings({});
    });
    connect(strength_, &QSlider::valueChanged, this, [this](int value) {
        if (updating_) return;
        updating_ = true;
        strengthValue_->setValue(value);
        updating_ = false;
        const Clip* c = clip();
        applySettings(c && strength_->isSliderDown() ? "stab-strength:" + c->id + ":" + std::to_string(dragSerial_) : std::string());
    });
    connect(strength_, &QSlider::sliderReleased, this, [this] { ++dragSerial_; });
    connect(strengthValue_, &QSpinBox::valueChanged, this, [this](int value) {
        if (updating_) return;
        updating_ = true;
        strength_->setValue(value);
        updating_ = false;
        applySettings({});
    });
    connect(analyze_, &QPushButton::clicked, this, [this] {
        if (analysis_->busy() && analysis_->kind() == "stabilization") analysis_->cancel();
        else if (!analysis_->busy()) analyze();
    });
    connect(analysis_, &AnalysisService::started, this, [this] { refresh(); });
    connect(analysis_, &AnalysisService::progress, this, [this](int done, int total) {
        if (analysis_->kind() != "stabilization") return;
        progress_->setRange(0, total);
        progress_->setValue(done);
    });
    connect(analysis_, &AnalysisService::stabilizationFinished, this, [this](const QString& id, const Stabilization& s) {
        edit_->setStabilization(id.toStdString(), s);
        refresh();
    });
    connect(analysis_, &AnalysisService::failed, this, [this](const QString&, const QString& msg) {
        if (analysis_->kind() == "stabilization" && msg != "cancelled") emit edit_->message(msg);
        refresh();
    });
    connect(doc_, &Document::changed, this, [this] { refresh(); });
    connect(doc_, &Document::selectionChanged, this, [this] { refresh(); });
    refresh();
}

const Clip* StabilizationPanel::clip() const {
    for (const auto& id : doc_->selection()) {
        const Track* tr = nullptr;
        const Clip* c = edit::findClip(doc_->sequence(), id, &tr);
        if (c && tr->kind == TrackKind::Video && !c->isGenerated()) return c;
    }
    return nullptr;
}

bool StabilizationPanel::analyze() {
    const Clip* c = clip();
    if (!c) return false;
    const MediaSource* src = doc_->project().findSource(c->sourceId);
    if (!src || !src->frameRate || src->width <= 0 || src->height <= 0) {
        emit edit_->message("The clip's media has no frame rate or picture size to analyse");
        return false;
    }
    const Rational fps = src->frameRate->fps();
    AnalysisService::StabilizationJob job;
    job.clipId = c->id;
    job.media = doc_->mediaFile(*src);
    job.rate = *src->frameRate;
    job.width = src->width;
    job.height = src->height;
    job.firstFrame = std::max<int64_t>(0, (c->sourceIn * fps).floor());   // the clip's source range
    job.lastFrame = std::max(job.firstFrame, (c->sourceOut() * fps).ceil() - 1);
    if (src->duration) job.lastFrame = std::max(job.firstFrame, std::min(job.lastFrame, (*src->duration * fps).ceil() - 1));
    job.settings.level = static_cast<StabilizeLevel>(level_->currentData().toInt());
    job.settings.strength = strength_->value() / 100.0;
    job.settings.autoZoom = autoZoom_->isChecked();
    job.settings.maxZoom = maxZoom_->value() / 100.0;
    if (!analysis_->startStabilization(job)) {
        emit edit_->message("Another analysis is running");
        return false;
    }
    progress_->setRange(0, static_cast<int>(job.lastFrame - job.firstFrame + 1));
    progress_->setValue(0);
    refresh();
    return true;
}

void StabilizationPanel::applySettings(const std::string& mergeKey) {
    const Clip* c = clip();
    if (!c || !c->stabilization) return;   // before the analysis the controls only choose the settings to use
    edit_->setStabilizationSettings(c->id, static_cast<StabilizeLevel>(level_->currentData().toInt()),
                                    strength_->value() / 100.0, autoZoom_->isChecked(), maxZoom_->value() / 100.0, mergeKey);
}

void StabilizationPanel::refresh() {
    const Clip* c = clip();
    setEnabled(c != nullptr);
    updating_ = true;
    const bool running = analysis_->busy() && analysis_->kind() == "stabilization";
    const bool mine = running && c && analysis_->clipId() == c->id;
    const Stabilization* s = c && c->stabilization ? &*c->stabilization : nullptr;
    enable_->setChecked(s || mine);
    if (s) {   // otherwise the controls keep the settings chosen for the next analysis
        level_->setCurrentIndex(std::max(0, level_->findData(static_cast<int>(s->level))));
        strength_->setValue(static_cast<int>(std::lround(s->strength * 100)));
        strengthValue_->setValue(strength_->value());
        autoZoom_->setChecked(s->autoZoom);
        maxZoom_->setValue(static_cast<int>(std::lround(s->maxZoom * 100)));
    } else if (!c) {
        level_->setCurrentIndex(1);
        strength_->setValue(100);
        strengthValue_->setValue(100);
        autoZoom_->setChecked(true);
        maxZoom_->setValue(110);
    }
    maxZoom_->setEnabled(autoZoom_->isChecked());
    analyze_->setText(running ? "Cancel" : s ? "Analyze Again" : "Analyze");
    analyze_->setEnabled(running || !analysis_->busy());
    progress_->setVisible(running);
    bool stale = false;
    QString status;
    if (mine) {
        status = "Analysing the camera motion...";
    } else if (s) {
        if (const MediaSource* src = doc_->project().findSource(c->sourceId)) stale = s->media != mediaIdentity(doc_->mediaFile(*src));
        int invalid = 0;
        for (const CameraMotion& m : s->motion) invalid += m.valid ? 0 : 1;
        status = stale ? QString("The media changed after the analysis: the correction no longer fits. Analyze again.")
                       : QString("Analysed %1 frames. Zoom %2 %%3%4")
                             .arg(s->motion.size())
                             .arg(s->zoom * 100, 0, 'f', 1)
                             .arg(s->limitedShare > 0 ? QString("; Max Zoom kept the camera path inside the frame on %1 % of the frames")
                                                            .arg(s->limitedShare * 100, 0, 'f', 0)
                                                      : QString())
                             .arg(invalid > 1 ? QString("; %1 frames without measurable motion").arg(invalid - 1) : QString());
    } else {
        status = "Not stabilised.";
    }
    status_->setText(status);
    status_->setStyleSheet(stale ? "color: #ff8a65;" : QString("color: %1;").arg(color::textDim.name()));
    updating_ = false;
}

}  // namespace fa::ui
