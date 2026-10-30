#include "cutout_panel.h"

#include "analysis_service.h"
#include "edit_controller.h"
#include "theme.h"

#include "fa/ai/inference.h"
#include "fa/ai/matting.h"
#include "fa/ai/models.h"
#include "fa/media/media.h"
#include "fa/model/edit_ops.h"
#include "fa/model/time_map.h"
#include "fa/render/matte_cache.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fa::ui {

namespace {

QDoubleSpinBox* spin(QWidget* parent, const char* name, double lo, double hi, double step, int decimals, const char* suffix,
                     const char* tip) {
    auto* s = new QDoubleSpinBox(parent);
    s->setObjectName(name);
    s->setRange(lo, hi);
    s->setSingleStep(step);
    s->setDecimals(decimals);
    if (suffix) s->setSuffix(suffix);
    s->setToolTip(tip);
    s->setKeyboardTracking(false);
    return s;
}

}  // namespace

CutoutPanel::CutoutPanel(Document* doc, EditController* editor, AnalysisService* analysis, QWidget* parent)
    : QWidget(parent), doc_(doc), edit_(editor), analysis_(analysis) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(12, 12, 12, 12);
    v->setSpacing(10);
    enable_ = new QCheckBox("Remove Background (AI)", this);
    enable_->setObjectName("cutoutEnable");
    enable_->setToolTip("Cut the subject out with a matting model that runs on this PC");
    v->addWidget(enable_);
    auto* form = new QFormLayout();
    model_ = new QComboBox(this);
    model_->setObjectName("cutoutModel");
    model_->addItem("People (Robust Video Matting)", "rvm-mobilenetv3");
    model_->addItem("General objects (BiRefNet, slow)", "birefnet-general-tiny");
    model_->setItemData(1, "Any main subject, frame by frame at 1024 x 1024. Needs a GPU with more than 4 GB of memory; "
                           "on this kind of laptop it runs on the CPU at about 40 s per frame - for stills and short "
                           "shots.", Qt::ToolTipRole);
    model_->setToolTip("People: video matting that follows the person through the shot. General objects: any "
                       "foreground object, frame by frame (slower).");
    form->addRow("Model", model_);
    quality_ = new QComboBox(this);
    quality_->setObjectName("cutoutQuality");
    quality_->addItem("Fast", 0);
    quality_->addItem("Standard", 1);
    quality_->addItem("High", 2);
    quality_->setToolTip("How finely the model looks at the picture (People): High keeps more hair detail, takes longer");
    form->addRow("Quality", quality_);
    v->addLayout(form);
    auto* row = new QHBoxLayout();
    preview_ = new QPushButton("Preview This Frame", this);
    preview_->setObjectName("cutoutPreview");
    preview_->setToolTip("Run the model on the frame under the playhead only, to judge the result first");
    analyse_ = new QPushButton("Analyse Clip", this);
    analyse_->setObjectName("cutoutAnalyse");
    analyse_->setToolTip("Matte every frame the clip uses (in the background); needed for playback and export");
    row->addWidget(preview_);
    row->addWidget(analyse_);
    v->addLayout(row);
    progress_ = new QProgressBar(this);
    progress_->setObjectName("cutoutProgress");
    v->addWidget(progress_);
    status_ = new QLabel(this);
    status_->setObjectName("cutoutStatus");
    status_->setWordWrap(true);
    v->addWidget(status_);

    auto* refine = new QGroupBox("Refine", this);
    auto* rf = new QFormLayout(refine);
    threshold_ = spin(refine, "cutoutThreshold", 0, 0.9, 0.05, 2, nullptr,
                      "Treat faint, semi-transparent parts of the matte as background (0 = keep the matte as it is)");
    edge_ = spin(refine, "cutoutEdge", -10, 10, 1, 0, " px", "Shrink (-) or grow (+) the subject's edge");
    feather_ = spin(refine, "cutoutFeather", 0, 20, 0.5, 1, " px", "Soften the edge");
    spill_ = spin(refine, "cutoutSpill", 0, 1, 0.1, 2, nullptr,
                  "Remove the old background's colour from soft edges such as hair");
    smoothing_ = spin(refine, "cutoutSmoothing", 0, 1, 0.1, 2, nullptr,
                      "Calm frame-to-frame flicker of the edge (moving parts are not smoothed)");
    rf->addRow("Threshold", threshold_);
    rf->addRow("Edge", edge_);
    rf->addRow("Feather", feather_);
    rf->addRow("Spill cleanup", spill_);
    rf->addRow("Flicker smoothing", smoothing_);
    v->addWidget(refine);

    auto* bg = new QGroupBox("Background", this);
    auto* bf = new QFormLayout(bg);
    background_ = new QComboBox(bg);
    background_->setObjectName("cutoutBackground");
    background_->addItem("Transparent (tracks below show)", 0);
    background_->addItem("Colour", 1);
    bf->addRow("Replace with", background_);
    colour_ = new QPushButton("Choose Colour...", bg);
    colour_->setObjectName("cutoutColour");
    bf->addRow("", colour_);
    addMedia_ = new QPushButton("Add Background Media...", bg);
    addMedia_->setObjectName("cutoutAddMedia");
    addMedia_->setToolTip("Put a video on a new track below this clip, over the clip's time");
    bf->addRow("", addMedia_);
    v->addWidget(bg);
    auto* note = new QLabel("Runs on this PC (AI Tools > Model Manager installs the models). The original media is "
                            "not changed; the analysis is kept per source frame, so trimming or moving the clip does "
                            "not need a new one.",
                            this);
    note->setWordWrap(true);
    note->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    v->addWidget(note);
    v->addStretch(1);

    connect(enable_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) edit("Background Removal", [on](AiCutout& c) { c.enabled = on; });
    });
    connect(model_, &QComboBox::currentIndexChanged, this, [this] {
        if (updating_) return;
        const std::string id = model_->currentData().toString().toStdString();
        edit("Background Removal Model", [id](AiCutout& c) {
            c.model = id;
            c.modelSha.clear();   // the mattes of another model are another analysis
        });
    });
    connect(quality_, &QComboBox::currentIndexChanged, this, [this] {
        if (updating_) return;
        const int q = quality_->currentData().toInt();
        edit("Background Removal Quality", [q](AiCutout& c) {
            c.quality = q;
            c.modelSha.clear();
        });
    });
    auto bindSpin = [this](QDoubleSpinBox* s, const char* label, const char* key, double AiCutout::*field) {
        connect(s, &QDoubleSpinBox::valueChanged, this, [this, s, label, key, field](double x) {
            if (!updating_) edit(label, [field, x](AiCutout& c) { c.*field = x; }, key);
            (void)s;
        });
    };
    bindSpin(threshold_, "Cutout Threshold", "cutout.threshold", &AiCutout::threshold);
    bindSpin(edge_, "Cutout Edge", "cutout.edge", &AiCutout::edge);
    bindSpin(feather_, "Cutout Feather", "cutout.feather", &AiCutout::feather);
    bindSpin(spill_, "Cutout Spill", "cutout.spill", &AiCutout::spill);
    bindSpin(smoothing_, "Cutout Smoothing", "cutout.smoothing", &AiCutout::smoothing);
    connect(background_, &QComboBox::currentIndexChanged, this, [this] {
        if (updating_) return;
        const bool fill = background_->currentData().toInt() == 1;
        edit("Cutout Background", [fill](AiCutout& c) {
            if (!fill) c.background.reset();
            else if (!c.background) c.background = Rgba8{20, 120, 60, 255};
        });
    });
    connect(colour_, &QPushButton::clicked, this, [this] {
        const Clip* c = clip();
        if (!c) return;
        const Rgba8 cur = c->aiCutout.background.value_or(Rgba8{20, 120, 60, 255});
        const QColor q = QColorDialog::getColor(QColor(cur.r, cur.g, cur.b), this, "Background Colour");
        if (!q.isValid()) return;
        const Rgba8 col{static_cast<uint8_t>(q.red()), static_cast<uint8_t>(q.green()), static_cast<uint8_t>(q.blue()), 255};
        edit("Cutout Background Colour", [col](AiCutout& a) { a.background = col; });
    });
    connect(addMedia_, &QPushButton::clicked, this, [this] {
        if (const Clip* c = clip()) emit addBackgroundMediaRequested(QString::fromStdString(c->id));
    });
    connect(preview_, &QPushButton::clicked, this, [this] {
        if (!analysis_->busy()) previewFrame();
    });
    connect(analyse_, &QPushButton::clicked, this, [this] {
        if (analysis_->busy() && analysis_->kind() == "cutout") analysis_->cancel();
        else if (!analysis_->busy()) analyse();
    });
    connect(analysis_, &AnalysisService::started, this, [this] { refresh(); });
    connect(analysis_, &AnalysisService::progress, this, [this](int done, int total) {
        if (analysis_->kind() != "cutout") return;
        progress_->setRange(0, total);
        progress_->setValue(done);
    });
    connect(analysis_, &AnalysisService::cutoutFinished, this,
            [this](const QString& clipId, const QString& summary, const QString& modelSha) {
                const std::string sha = modelSha.toStdString();
                edit_->modifyClip(clipId.toStdString(), "Background Removal Analysis", [sha](Clip& c) {
                    c.aiCutout.modelSha = sha;
                    c.aiCutout.enabled = true;
                });
                emit edit_->message(summary);
                refresh();
            });
    connect(analysis_, &AnalysisService::failed, this, [this](const QString&, const QString& msg) {
        if (analysis_->kind() == "cutout" && msg.startsWith("GPU_MEMORY:")) {
            // never continue on the CPU without asking: for some models that is hours
            const int64_t frames = lastJob_.lastFrame - lastJob_.firstFrame + 1;
            const double s = frames * ai::cutoutCpuSecondsPerFrame(lastJob_.cutout);
            const QString text = QString("%1.\n\nOn the CPU, %2 frame(s) take about %3 and use several GB of memory. "
                                         "Run on the CPU?")
                                     .arg(msg.mid(11))
                                     .arg(frames)
                                     .arg(s < 120 ? QString("%1 s").arg(std::ceil(s)) : QString("%1 min").arg(std::ceil(s / 60)));
            const bool yes = confirmSlow ? confirmSlow(text)
                                         : QMessageBox::question(this, "Background Removal", text) == QMessageBox::Yes;
            if (yes) {
                AnalysisService::CutoutJob job = lastJob_;
                job.accelerator = ai::Accelerator::Cpu;
                QTimer::singleShot(0, this, [this, job] {
                    lastJob_ = job;
                    if (analysis_->startCutout(job)) refresh();
                });
            } else {
                emit edit_->message("Background Removal: nothing analysed (" + msg.mid(11) + ")");
            }
        } else if (analysis_->kind() == "cutout" && msg != "cancelled") {
            emit edit_->message(msg);
        }
        refresh();
    });
    connect(doc_, &Document::changed, this, [this] { refresh(); });
    connect(doc_, &Document::selectionChanged, this, [this] { refresh(); });
    refresh();
}

const Clip* CutoutPanel::clip() const {
    for (const auto& id : doc_->selection()) {
        const Track* tr = nullptr;
        const Clip* c = edit::findClip(doc_->sequence(), id, &tr);
        if (c && tr->kind == TrackKind::Video && !c->isGenerated()) return c;
    }
    return nullptr;
}

void CutoutPanel::edit(const char* label, const std::function<void(AiCutout&)>& change, const char* mergeKey) {
    const Clip* c = clip();
    if (!c) return;
    edit_->modifyClip(c->id, label, [change](Clip& x) { change(x.aiCutout); }, mergeKey ? mergeKey : "");
}

QString CutoutPanel::coverageText(const Clip& c) const {
    const MediaSource* src = doc_->project().findSource(c.sourceId);
    if (!src || !src->frameRate) return "The clip's media has no frame rate.";
    const ai::ModelInfo* m = ai::findModel(c.aiCutout.model);
    if (!m) return "Unknown model.";
    if (ai::modelStatus(*m, false) != ai::ModelStatus::Installed)
        return QString("The model \"%1\" is not installed: AI Tools > Model Manager.").arg(QString::fromStdString(m->name));
    if (c.aiCutout.modelSha.empty()) return "Not analysed yet: Preview This Frame, then Analyse Clip.";
    const Rational fps = src->frameRate->fps();
    const int64_t first = std::max<int64_t>(0, (c.sourceIn * fps).floor());
    int64_t last = std::max(first, (c.sourceOut() * fps).ceil() - 1);
    if (src->duration) last = std::max(first, std::min(last, (*src->duration * fps).ceil() - 1));
    const MatteStore store(cutoutSetKey(doc_->mediaFile(*src), c.aiCutout, c.aiCutout.modelSha));
    const int64_t have = store.count(first, last), total = last - first + 1;
    if (have == total) return QString("Analysed: all %1 frames (%2).").arg(total).arg(QString::fromStdString(m->name));
    return QString("Analysed %1 of %2 frames: playback shows the original where frames are missing; export analyses "
                   "them first.").arg(have).arg(total);
}

void CutoutPanel::refresh() {
    const Clip* c = clip();
    updating_ = true;
    const bool has = c != nullptr;
    for (QWidget* w : std::initializer_list<QWidget*>{enable_, model_, quality_, preview_, analyse_, threshold_, edge_,
                                                      feather_, spill_, smoothing_, background_, colour_, addMedia_})
        w->setEnabled(has);
    if (c) {
        const AiCutout& a = c->aiCutout;
        enable_->setChecked(a.enabled);
        model_->setCurrentIndex(std::max(0, model_->findData(QString::fromStdString(a.model))));
        quality_->setCurrentIndex(std::max(0, quality_->findData(a.quality)));
        threshold_->setValue(a.threshold);
        edge_->setValue(a.edge);
        feather_->setValue(a.feather);
        spill_->setValue(a.spill);
        smoothing_->setValue(a.smoothing);
        background_->setCurrentIndex(a.background ? 1 : 0);
        colour_->setEnabled(a.background.has_value());
        const bool running = analysis_->busy() && analysis_->kind() == "cutout";
        analyse_->setText(running ? "Cancel" : "Analyse Clip");
        analyse_->setEnabled(running || !analysis_->busy());
        preview_->setEnabled(!analysis_->busy());
        status_->setText(running ? "Analysing..." : coverageText(*c));
    } else {
        status_->setText("Select a video clip on the timeline.");
    }
    updating_ = false;
}

bool CutoutPanel::startJob(int64_t first, int64_t last, ai::Accelerator acc) {
    const Clip* c = clip();
    if (!c) return false;
    const MediaSource* src = doc_->project().findSource(c->sourceId);
    if (!src || !src->frameRate) {
        emit edit_->message("The clip's media has no frame rate");
        return false;
    }
    const ai::ModelInfo* m = ai::findModel(c->aiCutout.model);
    if (!m || ai::modelStatus(*m, false) != ai::ModelStatus::Installed) {
        emit edit_->message("Background Removal: install the model first (AI Tools > Model Manager)");
        return false;
    }
    if (analysis_->busy()) {
        emit edit_->message("Another analysis is running");
        return false;
    }
    AnalysisService::CutoutJob job;
    job.clipId = c->id;
    job.media = doc_->mediaFile(*src);
    job.cutout = c->aiCutout;
    job.firstFrame = first;
    job.lastFrame = last;
    job.accelerator = acc;
    // a model this PC's GPU cannot hold: the CPU only after a yes, with the time it takes
    if (acc != ai::Accelerator::Cpu && !ai::cutoutFitsGpu(c->aiCutout)) {
        const int64_t frames = last - first + 1;
        const double s = frames * ai::cutoutCpuSecondsPerFrame(c->aiCutout);
        const QString text = QString("%1 does not fit this PC's graphics memory, so it would run on the CPU: %2 frame(s) "
                                     "take about %3 and need about 5 GB of memory. Start?")
                                 .arg(QString::fromStdString(m->name))
                                 .arg(frames)
                                 .arg(s < 120 ? QString("%1 s").arg(std::ceil(s)) : QString("%1 min").arg(std::ceil(s / 60)));
        const bool yes = confirmSlow ? confirmSlow(text)
                                     : QMessageBox::question(this, "Background Removal", text) == QMessageBox::Yes;
        if (!yes) return false;
        job.accelerator = ai::Accelerator::Cpu;
    }
    lastJob_ = job;
    if (!analysis_->startCutout(job)) {
        emit edit_->message("Another analysis is running");
        return false;
    }
    progress_->setRange(0, static_cast<int>(last - first + 1));
    progress_->setValue(0);
    refresh();
    return true;
}

bool CutoutPanel::analyse() {
    const Clip* c = clip();
    if (!c) return false;
    const MediaSource* src = doc_->project().findSource(c->sourceId);
    if (!src || !src->frameRate) {
        emit edit_->message("The clip's media has no frame rate");
        return false;
    }
    const Track* track = nullptr;
    edit::findClip(doc_->sequence(), c->id, &track);
    const auto [first, last] = ai::clipSourceFrames(*c, *src, track);   // with the frames its transitions show
    return startJob(first, last, ai::Accelerator::Auto);
}

bool CutoutPanel::previewFrame() {
    const Clip* c = clip();
    if (!c) return false;
    const MediaSource* src = doc_->project().findSource(c->sourceId);
    if (!src || !src->frameRate) return false;
    // the source frame under the playhead (clamped to the clip), analysed in the background like a clip of one frame
    const FrameRate seqRate = doc_->sequence().frameRate;
    Time t = doc_->playheadTime();
    if (t < c->timelineStart) t = c->timelineStart;
    if (!(t < c->timelineEnd())) t = c->timelineEnd() - seqRate.frameDuration();
    const Rational fps = src->frameRate->fps();
    const int64_t frame = (sampleFrame(*c, t - c->timelineStart, seqRate.frameDuration(), src->frameRate).time * fps).floor();
    return startJob(frame, frame, ai::Accelerator::Auto);
}

}  // namespace fa::ui
