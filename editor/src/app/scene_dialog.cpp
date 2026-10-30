#include "scene_dialog.h"

#include "edit_controller.h"
#include "theme.h"

#include "fa/media/media.h"
#include "fa/model/commands.h"
#include "fa/model/edit_ops.h"
#include "fa/model/keyframes.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fa::ui {

namespace {

QImage toQImage(const Image& img) {
    if (img.width <= 0) return {};
    return QImage(img.pixels.data(), img.width, img.height, img.width * 4, QImage::Format_RGBA8888).copy();
}

QString clockOf(double s) {
    return QString("%1:%2.%3")
        .arg(static_cast<int>(s) / 60)
        .arg(static_cast<int>(s) % 60, 2, 10, QChar('0'))
        .arg(static_cast<int>((s - std::floor(s)) * 100), 2, 10, QChar('0'));
}

}  // namespace

SceneDetectionDialog::SceneDetectionDialog(Document* doc, EditController* edit, QWidget* parent)
    : QDialog(parent), doc_(doc), edit_(edit) {
    setWindowTitle("Scene Detection");
    setObjectName("sceneDetection");
    resize(900, 680);
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    source_ = new QComboBox(this);
    source_->setObjectName("sceneSource");
    for (const auto& s : doc_->project().sources)
        if (s.hasVideo && !s.range)
            source_->addItem(QFileInfo(QString::fromStdString(s.path)).fileName(), QString::fromStdString(s.id));
    form->addRow("Video", source_);
    clipOnly_ = new QCheckBox("Only the part the selected timeline clip uses", this);
    clipOnly_->setObjectName("sceneClipOnly");
    form->addRow("", clipOnly_);
    sensitivity_ = new QSlider(Qt::Horizontal, this);
    sensitivity_->setObjectName("sceneSensitivity");
    sensitivity_->setRange(0, 100);
    sensitivity_->setValue(50);
    sensitivity_->setToolTip("Higher finds subtler cuts (and risks more false ones in fast motion)");
    form->addRow("Sensitivity", sensitivity_);
    minScene_ = new QDoubleSpinBox(this);
    minScene_->setObjectName("sceneMinLength");
    minScene_->setRange(0.05, 10);
    minScene_->setValue(vision::SceneOptions{}.minSceneSeconds);
    minScene_->setSingleStep(0.05);
    minScene_->setDecimals(2);
    minScene_->setSuffix(" s");
    form->addRow("Shortest scene", minScene_);
    fades_ = new QCheckBox("Fades to and from black or white are boundaries", this);
    fades_->setObjectName("sceneFades");
    fades_->setChecked(true);
    form->addRow("", fades_);
    lay->addLayout(form);
    auto* row = new QHBoxLayout();
    detect_ = new QPushButton("Detect Scenes", this);
    detect_->setObjectName("sceneDetect");
    progress_ = new QProgressBar(this);
    progress_->setObjectName("sceneProgress");
    progress_->setRange(0, 1000);
    row->addWidget(detect_);
    row->addWidget(progress_, 1);
    lay->addLayout(row);
    status_ = new QLabel("Finds cuts and fades by comparing the colours of consecutive frames (signal analysis; no AI "
                         "model). Review the scenes before using them.",
                         this);
    status_->setObjectName("sceneStatus");
    status_->setWordWrap(true);
    lay->addWidget(status_);
    list_ = new QListWidget(this);
    list_->setObjectName("sceneList");
    list_->setViewMode(QListView::IconMode);
    list_->setIconSize(QSize(160, 90));
    list_->setGridSize(QSize(190, 140));
    list_->setResizeMode(QListView::Adjust);
    list_->setMovement(QListView::Static);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    lay->addWidget(list_, 1);
    auto* edits = new QHBoxLayout();
    remove_ = new QPushButton("Remove Boundary", this);
    remove_->setObjectName("sceneRemove");
    remove_->setToolTip("Joins the selected scene to the one before it");
    addFrame_ = new QSpinBox(this);
    addFrame_->setObjectName("sceneAddFrame");
    addFrame_->setRange(0, 0);
    addFrame_->setPrefix("frame ");
    add_ = new QPushButton("Add Boundary", this);
    add_->setObjectName("sceneAdd");
    edits->addWidget(remove_);
    edits->addSpacing(16);
    edits->addWidget(addFrame_);
    edits->addWidget(add_);
    edits->addStretch(1);
    split_ = new QPushButton("Split Clip at Scenes", this);
    split_->setObjectName("sceneSplit");
    library_ = new QPushButton("Add Scenes to Library", this);
    library_->setObjectName("sceneLibrary");
    auto* close = new QPushButton("Close", this);
    edits->addWidget(split_);
    edits->addWidget(library_);
    edits->addWidget(close);
    lay->addLayout(edits);
    for (QWidget* w : std::initializer_list<QWidget*>{remove_, addFrame_, add_, split_, library_}) w->setEnabled(false);

    // the selected timeline clip chooses the video
    for (const auto& id : doc_->selection())
        if (const Clip* c = edit::findClip(doc_->sequence(), id); c && !c->isGenerated()) {
            const int i = source_->findData(QString::fromStdString(c->sourceId));
            if (i >= 0) {
                source_->setCurrentIndex(i);
                clipOnly_->setChecked(true);
            }
            break;
        }
    timer_ = new QTimer(this);
    timer_->setInterval(100);
    connect(timer_, &QTimer::timeout, this, &SceneDetectionDialog::poll);
    connect(detect_, &QPushButton::clicked, this, [this] {
        if (busy()) cancel_ = true;
        else detect();
    });
    connect(remove_, &QPushButton::clicked, this, [this] { removeBoundary(list_->currentRow()); });
    connect(add_, &QPushButton::clicked, this, [this] { addBoundary(addFrame_->value()); });
    connect(split_, &QPushButton::clicked, this, [this] { splitClip(); });
    connect(library_, &QPushButton::clicked, this, [this] { addToLibrary(); });
    connect(list_, &QListWidget::currentRowChanged, this, [this](int r) { remove_->setEnabled(r >= 1); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
}

SceneDetectionDialog::~SceneDetectionDialog() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

const MediaSource* SceneDetectionDialog::source() const {
    return doc_->project().findSource((sourceId_.empty() ? source_->currentData().toString().toStdString() : sourceId_));
}

const Clip* SceneDetectionDialog::timelineClip() const {
    const std::string sid = sourceId_.empty() ? source_->currentData().toString().toStdString() : sourceId_;
    for (const auto& id : doc_->selection()) {
        const Track* tr = nullptr;
        const Clip* c = edit::findClip(doc_->sequence(), id, &tr);
        if (c && tr->kind == TrackKind::Video && c->sourceId == sid) return c;
    }
    return nullptr;
}

bool SceneDetectionDialog::detect() {
    if (busy()) return false;
    sourceId_.clear();
    const MediaSource* src = source();
    if (!src || !src->frameRate) {
        status_->setText("Choose a video.");
        return false;
    }
    const Rational fps = src->frameRate->fps();
    int64_t first = 0, last = -1;
    if (clipOnly_->isChecked()) {
        const Clip* c = timelineClip();
        if (!c) {
            status_->setText("Select a clip of this video on the timeline, or analyse the whole video.");
            return false;
        }
        first = std::max<int64_t>(0, (c->sourceIn * fps).floor());
        last = std::max(first, (c->sourceOut() * fps).ceil() - 1);
    }
    vision::SceneOptions o;
    o.sensitivity = sensitivity_->value() / 100.0;
    o.minSceneSeconds = minScene_->value();
    o.fades = fades_->isChecked();
    const std::filesystem::path media = doc_->mediaFile(*src);
    sourceId_ = src->id;
    first_ = first;
    last_ = last;
    fps_ = fps.toDouble();
    cancel_ = false;
    done_ = false;
    permille_ = 0;
    error_.clear();
    list_->clear();
    detect_->setText("Cancel");
    status_->setText("Analysing...");
    worker_ = std::thread([this, media, o, first, last] {
        try {
            result_ = vision::detectScenes(media, o, first, last, [this](double p) { permille_ = static_cast<int>(p * 1000); }, &cancel_);
        } catch (const std::exception& e) {
            error_ = e.what();
        }
        done_ = true;
    });
    timer_->start();
    return true;
}

void SceneDetectionDialog::poll() {
    progress_->setValue(permille_);
    if (!done_) return;
    timer_->stop();
    worker_.join();
    detect_->setText("Detect Scenes");
    if (!error_.empty() || cancel_) {
        status_->setText(error_.empty() ? "Cancelled." : QString::fromStdString("Failed: " + error_));
        sourceId_.clear();
        return;
    }
    if (last_ < 0) last_ = first_ + result_.frames - 1;
    starts_ = {first_};
    kinds_ = {0};
    thumbs_.clear();
    thumbs_.push_back(result_.thumbnails.empty() ? QImage() : toQImage(result_.thumbnails[0]));
    for (size_t i = 0; i < result_.boundaries.size(); ++i) {
        starts_.push_back(result_.boundaries[i].frame);
        kinds_.push_back(result_.boundaries[i].kind == vision::BoundaryKind::Fade ? 2 : 1);
        thumbs_.push_back(i + 1 < result_.thumbnails.size() ? toQImage(result_.thumbnails[i + 1]) : QImage());
    }
    addFrame_->setRange(static_cast<int>(first_ + 1), static_cast<int>(last_));
    rebuildList();
    status_->setText(QString("%1 scenes in %2 frames (%3 s of video analysed in %4 s). Check them: remove a wrong "
                             "boundary, add a missed one.")
                         .arg(starts_.size())
                         .arg(result_.frames)
                         .arg(result_.frames / std::max(1.0, fps_), 0, 'f', 1)
                         .arg(result_.seconds, 0, 'f', 1));
}

void SceneDetectionDialog::rebuildList() {
    list_->clear();
    static const char* kind[] = {"Start", "Cut", "Fade", "Added"};
    for (size_t i = 0; i < starts_.size(); ++i) {
        const int64_t end = i + 1 < starts_.size() ? starts_[i + 1] : last_ + 1;
        auto* it = new QListWidgetItem(QString("Scene %1 - %2\n%3 (frame %4), %5 s")
                                           .arg(i + 1)
                                           .arg(kind[kinds_[i]])
                                           .arg(clockOf(starts_[i] / fps_))
                                           .arg(starts_[i])
                                           .arg((end - starts_[i]) / fps_, 0, 'f', 1));
        if (!thumbs_[i].isNull()) it->setIcon(QIcon(QPixmap::fromImage(thumbs_[i])));
        list_->addItem(it);
    }
    const bool any = starts_.size() > 1;
    add_->setEnabled(!starts_.empty());
    addFrame_->setEnabled(!starts_.empty());
    library_->setEnabled(any);
    split_->setEnabled(any && timelineClip() != nullptr);
    remove_->setEnabled(list_->currentRow() >= 1);
}

bool SceneDetectionDialog::removeBoundary(int scene) {
    if (scene < 1 || scene >= static_cast<int>(starts_.size())) return false;
    starts_.erase(starts_.begin() + scene);
    kinds_.erase(kinds_.begin() + scene);
    thumbs_.erase(thumbs_.begin() + scene);
    rebuildList();
    return true;
}

bool SceneDetectionDialog::addBoundary(int64_t frame) {
    if (starts_.empty() || frame <= first_ || frame > last_) return false;
    if (std::find(starts_.begin(), starts_.end(), frame) != starts_.end()) return false;
    const auto at = std::upper_bound(starts_.begin(), starts_.end(), frame) - starts_.begin();
    QImage thumb;
    try {
        if (const MediaSource* src = source()) {
            VideoDecoder::Options o;
            o.maxWidth = 192;
            o.maxHeight = 108;
            VideoDecoder dec(doc_->mediaFile(*src), o);
            thumb = toQImage(dec.frameAt(Time(frame) / src->frameRate->fps()).image);
        }
    } catch (const std::exception&) {
    }
    starts_.insert(starts_.begin() + at, frame);
    kinds_.insert(kinds_.begin() + at, 3);
    thumbs_.insert(thumbs_.begin() + at, thumb);
    rebuildList();
    return true;
}

bool SceneDetectionDialog::splitClip() {
    const Clip* c = timelineClip();
    const MediaSource* src = source();
    if (!c || !src || starts_.size() < 2) return false;
    const FrameRate rate = doc_->sequence().frameRate;
    const Rational fps = src->frameRate->fps();
    // timeline times of the boundaries inside the clip (through its speed changes), latest first so the original
    // clip id stays on the left part at every step
    std::vector<Time> cuts;
    for (size_t i = 1; i < starts_.size(); ++i) {
        const Time t = keys::toTimeline(*c, Time(starts_[i]) / fps);
        const Time snapped = frameToTime(timeToFrame(t, rate, Rounding::Nearest), rate);
        if (c->timelineStart < snapped && snapped < c->timelineEnd()) cuts.push_back(snapped);
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    if (cuts.empty()) {
        status_->setText("No boundary falls inside the selected clip.");
        return false;
    }
    std::reverse(cuts.begin(), cuts.end());
    Project scratch = doc_->project();
    std::vector<std::unique_ptr<Command>> steps;
    const std::string clipId = c->id;
    try {
        for (const Time& t : cuts) {
            auto cmd = edit::splitAt(scratch, doc_->sequenceId(), t, {clipId});
            cmd->apply(scratch);
            steps.push_back(std::move(cmd));
        }
    } catch (const std::exception& e) {
        status_->setText(QString::fromUtf8(e.what()));
        return false;
    }
    QString err;
    if (!doc_->execute(makeComposite("Split Clip at Scenes", std::move(steps)), &err)) {
        status_->setText(err);
        return false;
    }
    status_->setText(QString("Split the clip at %1 scene boundaries (one undo step).").arg(cuts.size()));
    emit edit_->message(status_->text());
    split_->setEnabled(false);
    return true;
}

bool SceneDetectionDialog::addToLibrary() {
    const MediaSource* src = source();
    if (!src || starts_.size() < 2) return false;
    const Rational fps = src->frameRate->fps();
    const QString base = QFileInfo(QString::fromStdString(src->path)).completeBaseName();
    Project scratch = doc_->project();
    std::vector<std::unique_ptr<Command>> steps;
    for (size_t i = 0; i < starts_.size(); ++i) {
        const int64_t end = i + 1 < starts_.size() ? starts_[i + 1] : last_ + 1;
        MediaSource s = *src;
        s.id.clear();
        s.range = TimeRange{Time(starts_[i]) / fps, Time(end - starts_[i]) / fps};
        s.name = QString("%1 - Scene %2").arg(base).arg(i + 1).toStdString();
        auto cmd = makeAddSource(scratch, s);
        cmd->apply(scratch);
        steps.push_back(std::move(cmd));
    }
    QString err;
    if (!doc_->execute(makeComposite("Add Scenes to Library", std::move(steps)), &err)) {
        status_->setText(err);
        return false;
    }
    status_->setText(QString("Added %1 scenes to the library: each places only its part of the video.").arg(starts_.size()));
    emit edit_->message(status_->text());
    return true;
}

}  // namespace fa::ui
