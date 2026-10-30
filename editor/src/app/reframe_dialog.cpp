#include "reframe_dialog.h"

#include "theme.h"

#include "fa/ai/models.h"
#include "fa/media/media.h"
#include "fa/model/commands.h"
#include "fa/model/edit_ops.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <optional>

namespace fa::ui {

// The clip's frame with the crop the reframe will show: the window, rule-of-thirds guides inside it, the margin the
// subject keeps, and the detected subject.
class ReframeView : public QWidget {
public:
    explicit ReframeView(QWidget* parent) : QWidget(parent) {
        setMinimumSize(480, 220);
        setObjectName("reframeView");
    }
    QImage frame;
    ai::CropWindow crop;
    double margin = 0.1;
    std::optional<QPointF> subject;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(16, 16, 18));
        if (frame.isNull()) return;
        const QSizeF s = QSizeF(frame.size()).scaled(QSizeF(size()), Qt::KeepAspectRatio);
        const QRectF r((width() - s.width()) / 2, (height() - s.height()) / 2, s.width(), s.height());
        p.drawImage(r, frame);
        const QRectF c(r.left() + (crop.x - crop.w / 2) * r.width(), r.top() + (crop.y - crop.h / 2) * r.height(),
                       crop.w * r.width(), crop.h * r.height());
        // outside the crop: dimmed
        QPainterPath outside;
        outside.addRect(r);
        QPainterPath inside;
        inside.addRect(c);
        p.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 150));
        p.setPen(QPen(color::accent, 2));
        p.drawRect(c);
        p.setPen(QPen(QColor(255, 255, 255, 110), 1, Qt::DashLine));
        for (int k = 1; k <= 2; ++k) {
            p.drawLine(QPointF(c.left() + c.width() * k / 3, c.top()), QPointF(c.left() + c.width() * k / 3, c.bottom()));
            p.drawLine(QPointF(c.left(), c.top() + c.height() * k / 3), QPointF(c.right(), c.top() + c.height() * k / 3));
        }
        p.setPen(QPen(QColor(255, 210, 80, 140), 1, Qt::DotLine));
        p.drawRect(c.adjusted(c.width() * margin, c.height() * margin, -c.width() * margin, -c.height() * margin));
        if (subject) {
            const QPointF q(r.left() + subject->x() * r.width(), r.top() + subject->y() * r.height());
            p.setPen(QPen(QColor(80, 220, 120), 2));
            p.drawEllipse(q, 6, 6);
        }
    }
};

AutoReframeDialog::AutoReframeDialog(Document* doc, QWidget* parent) : QDialog(parent), doc_(doc) {
    setWindowTitle("Auto Reframe");
    setObjectName("autoReframe");
    resize(980, 720);
    auto* lay = new QHBoxLayout(this);
    auto* left = new QVBoxLayout();
    auto* form = new QFormLayout();
    aspect_ = new QComboBox(this);
    aspect_->setObjectName("reframeAspect");
    aspect_->addItem("9:16 Vertical", 9.0 / 16);
    aspect_->addItem("1:1 Square", 1.0);
    aspect_->addItem("4:5 Portrait", 4.0 / 5);
    aspect_->addItem("16:9 Widescreen", 16.0 / 9);
    aspect_->addItem("Custom", 0.0);
    form->addRow("Aspect ratio", aspect_);
    auto* custom = new QHBoxLayout();
    customW_ = new QSpinBox(this);
    customW_->setObjectName("reframeCustomW");
    customW_->setRange(1, 100);
    customW_->setValue(3);
    customH_ = new QSpinBox(this);
    customH_->setObjectName("reframeCustomH");
    customH_->setRange(1, 100);
    customH_->setValue(4);
    custom->addWidget(customW_);
    custom->addWidget(new QLabel(":", this));
    custom->addWidget(customH_);
    form->addRow("Custom", custom);
    scope_ = new QComboBox(this);
    scope_->setObjectName("reframeScope");
    scope_->addItem("Every video clip of the sequence", "all");
    scope_->addItem("The selected clip", "selected");
    form->addRow("Reframe", scope_);
    subject_ = new QComboBox(this);
    subject_->setObjectName("reframeSubject");
    subject_->addItem("People's faces (AI face detection)", static_cast<int>(ai::ReframeSubject::Faces));
    subject_->addItem("The clip's motion track (tracking, not AI)", static_cast<int>(ai::ReframeSubject::MotionTrack));
    subject_->addItem("A fixed focal point", static_cast<int>(ai::ReframeSubject::FocalPoint));
    subject_->setToolTip("Faces: the YuNet face detector (a neural network) finds the people. Motion track: the region "
                        "followed with Video > Motion Tracking (classic tracking, not AI). Focal point: a fixed place.");
    form->addRow("Follow", subject_);
    auto spin = [this](const char* name, double lo, double hi, double v, double step, const char* suffix) {
        auto* s = new QDoubleSpinBox(this);
        s->setObjectName(name);
        s->setRange(lo, hi);
        s->setValue(v);
        s->setSingleStep(step);
        s->setSuffix(suffix);
        return s;
    };
    focusX_ = spin("reframeFocusX", 0, 100, 50, 1, " %");
    focusY_ = spin("reframeFocusY", 0, 100, 45, 1, " %");
    auto* focus = new QHBoxLayout();
    focus->addWidget(focusX_);
    focus->addWidget(focusY_);
    form->addRow("Focal point (x, y)", focus);
    margin_ = spin("reframeMargin", 0, 40, 10, 1, " %");
    margin_->setToolTip("The subject stays at least this far inside the frame's edges");
    form->addRow("Margin", margin_);
    zoom_ = spin("reframeZoom", 100, 300, 100, 5, " %");
    form->addRow("Zoom", zoom_);
    smoothness_ = new QSlider(Qt::Horizontal, this);
    smoothness_->setObjectName("reframeSmoothness");
    smoothness_->setRange(0, 100);
    smoothness_->setValue(50);
    form->addRow("Smoothness", smoothness_);
    hold_ = spin("reframeHold", 0, 10, 1, 0.5, " s");
    hold_->setToolTip("When the subject leaves, the frame stays this long, then returns to the focal point");
    form->addRow("Subject lost: hold", hold_);
    left->addLayout(form);
    auto* row = new QHBoxLayout();
    analyse_ = new QPushButton("Analyse", this);
    analyse_->setObjectName("reframeAnalyse");
    progress_ = new QProgressBar(this);
    progress_->setObjectName("reframeProgress");
    progress_->setRange(0, 1000);
    row->addWidget(analyse_);
    row->addWidget(progress_, 1);
    left->addLayout(row);
    status_ = new QLabel("Runs on this PC. The media is not changed: Apply writes keyframes on the clips' transform "
                         "(one undo step).",
                         this);
    status_->setObjectName("reframeStatus");
    status_->setWordWrap(true);
    left->addWidget(status_);
    left->addStretch(1);
    auto* buttons = new QHBoxLayout();
    apply_ = new QPushButton("Apply", this);
    apply_->setObjectName("reframeApply");
    apply_->setEnabled(false);
    auto* close = new QPushButton("Close", this);
    buttons->addStretch(1);
    buttons->addWidget(apply_);
    buttons->addWidget(close);
    left->addLayout(buttons);
    lay->addLayout(left, 0);
    auto* right = new QVBoxLayout();
    clipList_ = new QComboBox(this);
    clipList_->setObjectName("reframeClip");
    right->addWidget(clipList_);
    view_ = new ReframeView(this);
    right->addWidget(view_, 1);
    scrub_ = new QSlider(Qt::Horizontal, this);
    scrub_->setObjectName("reframeScrub");
    scrub_->setRange(0, 0);
    right->addWidget(scrub_);
    lay->addLayout(right, 1);

    timer_ = new QTimer(this);
    timer_->setInterval(100);
    connect(timer_, &QTimer::timeout, this, &AutoReframeDialog::poll);
    connect(analyse_, &QPushButton::clicked, this, [this] {
        if (busy()) cancel_ = true;
        else analyse();
    });
    connect(apply_, &QPushButton::clicked, this, [this] { apply(); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    for (QDoubleSpinBox* s : {focusX_, focusY_, margin_, zoom_, hold_})
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] { replan(); });
    connect(smoothness_, &QSlider::valueChanged, this, [this] { replan(); });
    connect(aspect_, &QComboBox::currentIndexChanged, this, [this] { replan(); });
    connect(customW_, &QSpinBox::valueChanged, this, [this] { replan(); });
    connect(customH_, &QSpinBox::valueChanged, this, [this] { replan(); });
    connect(clipList_, &QComboBox::currentIndexChanged, this, [this] {
        auto it = paths_.find(clipList_->currentData().toString().toStdString());
        scrub_->setRange(0, it == paths_.end() ? 0 : std::max<int>(0, static_cast<int>(it->second.samples.size()) - 1));
        showFrame();
    });
    connect(scrub_, &QSlider::valueChanged, this, [this] { showFrame(); });
    if (doc_->selection().size() == 1) scope_->setCurrentIndex(1);
}

AutoReframeDialog::~AutoReframeDialog() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

ai::ReframeOptions AutoReframeDialog::options() const {
    ai::ReframeOptions o;
    const double a = aspect_->currentData().toDouble();
    o.aspect = a > 0 ? a : static_cast<double>(customW_->value()) / customH_->value();
    o.subject = static_cast<ai::ReframeSubject>(subject_->currentData().toInt());
    o.focusX = focusX_->value() / 100;
    o.focusY = focusY_->value() / 100;
    o.margin = margin_->value() / 100;
    o.zoom = zoom_->value() / 100;
    o.smoothness = smoothness_->value() / 100.0;
    o.holdSeconds = hold_->value();
    return o;
}

std::vector<const Clip*> AutoReframeDialog::clips() const {
    std::vector<const Clip*> out;
    const bool selected = scope_->currentData().toString() == "selected";
    for (const auto& tr : doc_->sequence().tracks) {
        if (tr.kind != TrackKind::Video) continue;
        for (const auto& c : tr.clips) {
            if (c.isGenerated() || c.speed.freeze) continue;
            if (selected && std::find(doc_->selection().begin(), doc_->selection().end(), c.id) == doc_->selection().end()) continue;
            const MediaSource* src = doc_->project().findSource(c.sourceId);
            if (src && src->hasVideo && src->frameRate) out.push_back(&c);
        }
    }
    return out;
}

bool AutoReframeDialog::analyse() {
    if (busy()) return false;
    const ai::ReframeOptions o = options();
    if (o.subject == ai::ReframeSubject::Faces) {
        const ai::ModelInfo* m = ai::findModel("yunet-face");
        if (ai::modelStatus(*m, false) != ai::ModelStatus::Installed) {
            status_->setText("Install the face detection model first (AI Tools > Model Manager), or follow a motion "
                             "track or a focal point.");
            return false;
        }
    }
    struct Job {
        std::string id;
        std::filesystem::path media;
        Time in, out;
        std::optional<MotionTrack> track;
    };
    std::vector<Job> jobs;
    media_.clear();
    for (const Clip* c : clips()) {
        const MediaSource* src = doc_->project().findSource(c->sourceId);
        jobs.push_back({c->id, doc_->mediaFile(*src), c->sourceIn, c->sourceOut(), c->tracking});
        media_[c->id] = jobs.back().media;
    }
    if (jobs.empty()) {
        status_->setText(scope_->currentData().toString() == "selected" ? "Select a video clip on the timeline."
                                                                         : "The sequence has no video clips.");
        return false;
    }
    if (o.subject == ai::ReframeSubject::MotionTrack &&
        std::none_of(jobs.begin(), jobs.end(), [](const Job& j) { return j.track.has_value(); })) {
        status_->setText("No clip has a motion track: track the subject first (Video > Motion Tracking).");
        return false;
    }
    cancel_ = false;
    done_ = false;
    permille_ = 0;
    error_.clear();
    pending_.clear();
    analyse_->setText("Cancel");
    apply_->setEnabled(false);
    status_->setText(QString("Analysing %1 clip(s)...").arg(jobs.size()));
    worker_ = std::thread([this, jobs, o] {
        try {
            for (size_t i = 0; i < jobs.size() && !cancel_; ++i) {
                const Job& j = jobs[i];
                auto p = ai::analyseReframe(j.media, j.in, j.out, o, j.track ? &*j.track : nullptr, [&](double f) {
                    permille_ = static_cast<int>((i + f) / jobs.size() * 1000);
                }, &cancel_);
                pending_[j.id] = std::move(p);
            }
        } catch (const std::exception& e) {
            error_ = e.what();
        }
        done_ = true;
    });
    timer_->start();
    return true;
}

void AutoReframeDialog::poll() {
    progress_->setValue(permille_);
    if (!done_) return;
    timer_->stop();
    worker_.join();
    analyse_->setText("Analyse");
    if (!error_.empty() || cancel_) {
        status_->setText(error_.empty() ? "Cancelled; nothing changed." : QString::fromStdString("Failed: " + error_));
        progress_->setValue(0);
        return;
    }
    paths_ = std::move(pending_);
    pending_.clear();
    int samples = 0, found = 0;
    double seconds = 0;
    std::string provider;
    clipList_->blockSignals(true);
    clipList_->clear();
    for (const auto& [id, p] : paths_) {
        samples += static_cast<int>(p.samples.size());
        found += p.found;
        seconds += p.seconds;
        if (!p.provider.empty()) provider = p.provider;
        const Clip* c = edit::findClip(doc_->sequence(), id);
        clipList_->addItem(QString::fromStdString(c ? c->name : id), QString::fromStdString(id));
    }
    clipList_->blockSignals(false);
    replan();
    clipList_->setCurrentIndex(0);
    scrub_->setRange(0, paths_.empty() ? 0 : std::max<int>(0, static_cast<int>(paths_.begin()->second.samples.size()) - 1));
    showFrame();
    status_->setText(QString("%1 clip(s) analysed in %2 s%3: the subject found in %4 of %5 samples. Adjust, then Apply.")
                         .arg(paths_.size())
                         .arg(seconds, 0, 'f', 1)
                         .arg(provider.empty() ? QString() : " (" + QString::fromStdString(provider) + ")")
                         .arg(found)
                         .arg(samples));
    apply_->setEnabled(!paths_.empty());
}

void AutoReframeDialog::replan() {
    const ai::ReframeOptions o = options();
    for (auto& [id, p] : paths_) ai::planReframe(p, o);
    showFrame();
}

void AutoReframeDialog::showFrame() {
    const std::string id = clipList_->currentData().toString().toStdString();
    auto it = paths_.find(id);
    const Clip* c = edit::findClip(doc_->sequence(), id);
    if (it == paths_.end() || !c || it->second.samples.empty()) {
        view_->frame = QImage();
        view_->update();
        return;
    }
    const ai::ReframePath& p = it->second;
    const size_t i = static_cast<size_t>(std::clamp(scrub_->value(), 0, static_cast<int>(p.samples.size()) - 1));
    const double t = p.samples[i].t;
    try {
        VideoDecoder::Options dopt;
        dopt.maxWidth = 960;
        dopt.maxHeight = 540;
        VideoDecoder dec(media_[id], dopt);
        const Image& img = dec.frameAt(c->sourceIn + Rational::fromDouble(t, 100000)).image;
        view_->frame = QImage(img.pixels.data(), img.width, img.height, img.width * 4, QImage::Format_RGBA8888).copy();
    } catch (const std::exception&) {
        view_->frame = QImage();
    }
    // exactly what Apply writes: the thinned keyframes evaluated at this time
    const ai::ReframeOptions o = options();
    const auto [w, h] = ai::reframeCanvas(doc_->sequence().width, doc_->sequence().height, o.aspect);
    const ClipTransform tr = ai::reframeTransform(p, c->sourceIn, w, h, c->transform);
    view_->crop = ai::cropWindowAt(tr, (c->sourceIn + Rational::fromDouble(t, 100000)).toDouble(), p.sourceWidth,
                                   p.sourceHeight, w, h);
    view_->margin = o.margin;
    view_->subject.reset();
    if (p.samples[i].found) view_->subject = QPointF(p.samples[i].x, p.samples[i].y);
    view_->update();
}

bool AutoReframeDialog::apply() {
    if (paths_.empty()) return false;
    const ai::ReframeOptions o = options();
    const auto [w, h] = ai::reframeCanvas(doc_->sequence().width, doc_->sequence().height, o.aspect);
    std::vector<std::unique_ptr<Command>> steps;
    steps.push_back(makeModifySequence(doc_->sequenceId(), "Auto Reframe", [w, h](Sequence& s) {
        s.width = w;
        s.height = h;
    }));
    int n = 0;
    for (const auto& [id, p] : paths_) {
        const Clip* c = edit::findClip(doc_->sequence(), id);
        if (!c) continue;
        const ClipTransform t = ai::reframeTransform(p, c->sourceIn, w, h, c->transform);
        steps.push_back(makeModifyClip(doc_->sequenceId(), id, "Auto Reframe", [t](Clip& x) { x.transform = t; }));
        ++n;
    }
    QString err;
    if (!doc_->execute(makeComposite("Auto Reframe", std::move(steps)), &err)) {
        status_->setText(err);
        return false;
    }
    status_->setText(QString("Reframed %1 clip(s) for %2 x %3 (one undo step). The keyframes are on each clip's "
                             "position; edit them in the inspector.")
                         .arg(n)
                         .arg(w)
                         .arg(h));
    return true;
}

}  // namespace fa::ui
