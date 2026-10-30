#include "tracking_panel.h"

#include "analysis_service.h"
#include "edit_controller.h"
#include "theme.h"

#include "fa/model/edit_ops.h"
#include "fa/model/time_map.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace fa::ui {

TrackingPanel::TrackingPanel(Document* doc, EditController* edit, AnalysisService* analysis, QWidget* parent)
    : QWidget(parent), doc_(doc), edit_(edit), analysis_(analysis) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(12, 12, 12, 12);
    v->setSpacing(10);
    enable_ = new QCheckBox("Motion Tracking", this);
    enable_->setObjectName("trackingEnable");
    enable_->setToolTip("Shows a tracking box in the player: put it on the object to follow");
    v->addWidget(enable_);
    auto* hint = new QLabel("Move the box onto the object in the player and resize it from its corners; the object "
                            "should stay visible while it is tracked.",
                            this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    v->addWidget(hint);
    auto* row = new QHBoxLayout();
    start_ = new QPushButton("Start Tracking", this);
    start_->setObjectName("trackingStart");
    progress_ = new QProgressBar(this);
    progress_->setObjectName("trackingProgress");
    progress_->setTextVisible(true);
    row->addWidget(start_);
    row->addWidget(progress_, 1);
    v->addLayout(row);
    status_ = new QLabel(this);
    status_->setObjectName("trackingStatus");
    status_->setWordWrap(true);
    v->addWidget(status_);
    auto* linkLabel = new QLabel("Link Element", this);
    v->addWidget(linkLabel);
    link_ = new QComboBox(this);
    link_->setObjectName("trackingLink");
    link_->setToolTip("What follows the tracked object");
    link_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);   // long names do not widen the panel
    link_->setMinimumContentsLength(12);
    v->addWidget(link_);
    linkButton_ = new QPushButton("Link", this);
    linkButton_->setObjectName("trackingLinkButton");
    linkButton_->setToolTip("The chosen element follows the tracked object (position keyframes on every frame)");
    v->addWidget(linkButton_);
    remove_ = new QPushButton(icon("delete"), "Remove Tracking", this);
    remove_->setObjectName("trackingRemove");
    v->addWidget(remove_);
    v->addStretch(1);

    connect(enable_, &QCheckBox::toggled, this, [this] {
        if (!updating_) syncBox();
    });
    connect(start_, &QPushButton::clicked, this, [this] {
        if (analysis_->busy() && analysis_->kind() == "tracking") analysis_->cancel();
        else if (!analysis_->busy()) startTracking();
    });
    connect(linkButton_, &QPushButton::clicked, this, [this] { linkSelected(); });
    connect(remove_, &QPushButton::clicked, this, [this] {
        if (const Clip* c = clip()) edit_->setTracking(c->id, std::nullopt);
    });
    connect(analysis_, &AnalysisService::started, this, [this] { refresh(); });
    connect(analysis_, &AnalysisService::progress, this, [this](int done, int total) {
        if (analysis_->kind() != "tracking") return;
        progress_->setRange(0, total);
        progress_->setValue(done);
    });
    connect(analysis_, &AnalysisService::trackingFinished, this, [this](const QString& id, const MotionTrack& t) {
        edit_->setTracking(id.toStdString(), t);
        refresh();
    });
    connect(analysis_, &AnalysisService::failed, this, [this](const QString&, const QString& msg) {
        if (analysis_->kind() == "tracking" && msg != "cancelled") emit edit_->message(msg);
        refresh();
    });
    connect(doc_, &Document::changed, this, [this] { refresh(); });
    connect(doc_, &Document::selectionChanged, this, [this] {
        refresh();
        syncBox();
    });
    connect(doc_, &Document::playheadChanged, this, [this] { syncBox(); });
    refresh();
}

const Clip* TrackingPanel::clip() const {
    for (const auto& id : doc_->selection()) {
        const Track* tr = nullptr;
        const Clip* c = edit::findClip(doc_->sequence(), id, &tr);
        if (c && tr->kind == TrackKind::Video && !c->isGenerated()) return c;
    }
    return nullptr;
}

void TrackingPanel::setTabShown(bool shown) {
    shown_ = shown;
    syncBox();
}

// The editable box in the player: on while the tab is shown and Motion Tracking is on; it shows the tracked region at
// the playhead when there is a result, the region still to analyse otherwise.
void TrackingPanel::syncBox() {
    const Clip* c = clip();
    if (!c || !shown_ || !enable_->isChecked()) {
        doc_->setTrackingBox(std::nullopt);
        return;
    }
    Document::TrackingBox b;
    b.clipId = c->id;
    if (const auto& cur = doc_->trackingBox(); cur && cur->clipId == c->id) b = *cur;
    else if (c->tracking) {
        b.x = c->tracking->boxX;
        b.y = c->tracking->boxY;
        b.w = c->tracking->boxW;
        b.h = c->tracking->boxH;
    }
    doc_->setTrackingBox(b);
}

bool TrackingPanel::startTracking() {
    const Clip* c = clip();
    if (!c) return false;
    const MediaSource* src = doc_->project().findSource(c->sourceId);
    if (!src || !src->frameRate) {
        emit edit_->message("The clip's media has no frame rate to track");
        return false;
    }
    const auto& box = doc_->trackingBox();
    if (!box || box->clipId != c->id) {
        emit edit_->message("Turn on Motion Tracking and put the box on the object first");
        return false;
    }
    const Rational fps = src->frameRate->fps();
    const FrameRate seqRate = doc_->sequence().frameRate;
    AnalysisService::TrackingJob job;
    job.clipId = c->id;
    job.media = doc_->mediaFile(*src);
    job.rate = *src->frameRate;
    // the clip's whole source range (a reversed clip plays the same range backwards)
    job.firstFrame = std::max<int64_t>(0, (c->sourceIn * fps).floor());
    job.lastFrame = std::max(job.firstFrame, (c->sourceOut() * fps).ceil() - 1);
    if (src->duration) job.lastFrame = std::max(job.firstFrame, std::min(job.lastFrame, (*src->duration * fps).ceil() - 1));
    // the box sits on the source frame under the playhead (clamped to the clip)
    Time t = doc_->playheadTime();
    if (t < c->timelineStart) t = c->timelineStart;
    if (!(t < c->timelineEnd())) t = c->timelineEnd() - seqRate.frameDuration();
    job.boxFrame = std::clamp((sampleFrame(*c, t - c->timelineStart, seqRate.frameDuration(), src->frameRate).time * fps).floor(),
                              job.firstFrame, job.lastFrame);
    job.boxX = box->x;
    job.boxY = box->y;
    job.boxW = box->w;
    job.boxH = box->h;
    if (!analysis_->startTracking(job)) {
        emit edit_->message("Another analysis is running");
        return false;
    }
    progress_->setRange(0, static_cast<int>(job.lastFrame - job.firstFrame + 1));
    progress_->setValue(0);
    refresh();
    return true;
}

bool TrackingPanel::linkSelected() {
    const Clip* c = clip();
    if (!c) return false;
    const QString what = link_->currentData().toString();
    if (what == "mosaic") return edit_->addTrackingMosaic(c->id);
    if (what.isEmpty()) {
        emit edit_->message("Choose the element that should follow the tracked object");
        return false;
    }
    return edit_->attachToTracking(c->id, what.toStdString());
}

void TrackingPanel::refresh() {
    const Clip* c = clip();
    setEnabled(c != nullptr);
    updating_ = true;
    const bool running = analysis_->busy() && analysis_->kind() == "tracking";
    start_->setText(running ? "Cancel" : c && c->tracking ? "Track Again" : "Start Tracking");
    start_->setEnabled(running || !analysis_->busy());   // one analysis at a time
    progress_->setVisible(running);
    QString status;
    bool stale = false;
    if (c && c->tracking) {
        const MotionTrack& t = *c->tracking;
        double conf = 0;
        int lost = 0;
        for (const auto& s : t.samples) {
            conf += s.confidence;
            lost += s.lost;
        }
        if (const MediaSource* src = doc_->project().findSource(c->sourceId))
            stale = t.media != mediaIdentity(doc_->mediaFile(*src));
        status = stale ? QString("The media changed after the analysis: the tracking data no longer fits. Track again.")
                       : QString("Tracked %1 frames, average confidence %2%3.")
                             .arg(t.samples.size())
                             .arg(t.samples.empty() ? 0.0 : conf / t.samples.size(), 0, 'f', 2)
                             .arg(lost ? QString(", target lost on %1 frames (the last position is held there)").arg(lost)
                                       : QString());
    } else if (running) {
        status = "Tracking...";
    } else {
        status = "No tracking data yet.";
    }
    status_->setText(status);
    status_->setStyleSheet(stale ? "color: #ff8a65;" : QString("color: %1;").arg(color::textDim.name()));
    // elements that can follow: titles and video clips on tracks above, overlapping in time
    const QString keep = link_->currentData().toString();
    link_->clear();
    link_->addItem("Choose an element...", QString());
    if (c) {
        const Sequence& seq = doc_->sequence();
        const Track* own = nullptr;
        edit::findClip(seq, c->id, &own);
        bool above = false;
        for (const Track& tr : seq.tracks) {
            if (&tr == own) {
                above = true;
                continue;
            }
            if (!above || tr.kind != TrackKind::Video) continue;
            for (const Clip& o : tr.clips) {
                if (o.isAdjustment() || !(o.timelineStart < c->timelineEnd() && c->timelineStart < o.timelineEnd())) continue;
                const QString name = o.isTitle() ? "Title: " + QString::fromStdString(o.title->text).left(30)
                                                 : QString::fromStdString(o.name.empty() ? o.id : o.name);
                link_->addItem(name, QString::fromStdString(o.id));
            }
        }
        link_->addItem("Add a mosaic (pixelate the tracked region)", QString("mosaic"));
    }
    if (const int i = link_->findData(keep); i >= 0) link_->setCurrentIndex(i);
    const bool usable = c && c->tracking && !stale;
    link_->setEnabled(usable);
    linkButton_->setEnabled(usable);
    remove_->setEnabled(c && c->tracking);
    updating_ = false;
}

}  // namespace fa::ui
