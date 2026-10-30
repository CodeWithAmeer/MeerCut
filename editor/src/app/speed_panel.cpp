#include "speed_panel.h"

#include "analysis_service.h"
#include "edit_controller.h"
#include "fa/ai/prepare.h"
#include "theme.h"

#include "fa/model/edit_ops.h"
#include "fa/model/time_map.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QStackedWidget>
#include <QTabWidget>
#include <QVBoxLayout>

#include <cmath>

namespace fa::ui {

namespace {

constexpr int kSliderMax = 1000;

QString timecode(const Time& t, const FrameRate& rate) {
    return QString::fromStdString(formatTimecode((t / rate.frameDuration()).round(), rate));
}

QString seconds(const Time& t) { return QString::number(t.toDouble(), 'f', 3) + " s"; }

Rational speedRational(double v) {   // two decimals, like the value box
    v = std::clamp(v, kMinSpeed, kMaxSpeed);
    return Rational(static_cast<int64_t>(std::llround(v * 100.0)), 100);
}

}  // namespace

std::optional<Time> parseDuration(const QString& text, const FrameRate& rate) {
    const QString t = text.trimmed();
    static const QRegularExpression tc(R"(^(\d+):(\d{1,2}):(\d{1,2})[:;](\d{1,3})$)");
    if (const auto m = tc.match(t); m.hasMatch()) {
        const int64_t secs = m.captured(1).toLongLong() * 3600 + m.captured(2).toLongLong() * 60 + m.captured(3).toLongLong();
        const int64_t frames = m.captured(4).toLongLong();
        const int64_t perSecond = static_cast<int64_t>(std::llround(rate.toDouble()));
        if (frames >= perSecond) return std::nullopt;
        return frameToTime(secs * perSecond + frames, rate);   // non-drop frame count, as formatTimecode writes it
    }
    static const QRegularExpression ms(R"(^(?:(\d+):)?(\d+(?:\.\d+)?)\s*s?$)");
    if (const auto m = ms.match(t); m.hasMatch()) {
        const double s = m.captured(1).toDouble() * 60.0 + m.captured(2).toDouble();
        if (!(s > 0)) return std::nullopt;
        return Time(static_cast<int64_t>(std::llround(s * 1000.0)), 1000);
    }
    return std::nullopt;
}

double SpeedPanel::sliderToSpeed(int pos) {
    const double a = std::clamp(pos, 0, kSliderMax) / static_cast<double>(kSliderMax);   // 0 -> 0.1x, 0.5 -> 1x, 1 -> 10x
    return std::pow(10.0, 2.0 * a - 1.0);
}

int SpeedPanel::speedToSlider(double speed) {
    return static_cast<int>(std::lround((std::log10(std::clamp(speed, kMinSpeed, kMaxSpeed)) + 1.0) / 2.0 * kSliderMax));
}

SpeedPanel::SpeedPanel(Document* doc, EditController* edit, QWidget* parent, AnalysisService* analysis)
    : QWidget(parent), doc_(doc), edit_(edit), analysis_(analysis) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName("speedSubTabs");
    tabs_->setDocumentMode(true);
    lay->addWidget(tabs_);

    // ---- Uniform Speed
    uniformStack_ = new QStackedWidget(tabs_);
    uniformPage_ = new QWidget(uniformStack_);
    auto* form = new QFormLayout(uniformPage_);
    form->setContentsMargins(12, 12, 12, 12);
    form->setVerticalSpacing(10);
    auto* speedRow = new QWidget(uniformPage_);
    auto* sh = new QHBoxLayout(speedRow);
    sh->setContentsMargins(0, 0, 0, 0);
    slider_ = new QSlider(Qt::Horizontal, speedRow);
    slider_->setObjectName("speedSlider");
    slider_->setRange(0, kSliderMax);
    slider_->setToolTip("Speed, 0.1x (slow motion) to 10x (fast motion); 1x in the middle");
    value_ = new QDoubleSpinBox(speedRow);
    value_->setObjectName("speedValue");
    value_->setRange(kMinSpeed, kMaxSpeed);
    value_->setDecimals(2);
    value_->setSingleStep(0.1);
    value_->setSuffix(" x");
    value_->setKeyboardTracking(false);
    sh->addWidget(slider_, 1);
    sh->addWidget(value_);
    form->addRow("Speed", speedRow);
    duration_ = new QLineEdit(uniformPage_);
    duration_->setObjectName("speedDuration");
    duration_->setToolTip("New length on the timeline (hh:mm:ss:ff or seconds); the speed follows");
    form->addRow("Duration", duration_);
    reverse_ = new QCheckBox("Reverse Speed", uniformPage_);
    reverse_->setObjectName("speedReverse");
    reverse_->setToolTip("Play the clip backwards (its linked audio plays backwards too)");
    form->addRow(reverse_);
    // Maintain Pitch and Frame Interpolation: below both tabs (they apply to ramps too)
    options_ = new QWidget(this);
    auto* of = new QFormLayout(options_);
    of->setContentsMargins(12, 4, 12, 12);
    pitch_ = new QCheckBox("Maintain Pitch", options_);
    pitch_->setObjectName("speedPitch");
    pitch_->setToolTip("Keep the audio's pitch when the speed changes (time stretch); off: the pitch follows the speed");
    of->addRow(pitch_);
    interpolation_ = new QComboBox(options_);
    interpolation_->setObjectName("speedInterpolation");
    interpolation_->addItem("Frame Sampling", static_cast<int>(FrameInterpolation::Sampling));
    interpolation_->addItem("Frame Blending", static_cast<int>(FrameInterpolation::Blending));
    interpolation_->addItem("Optical Flow (Fast)", static_cast<int>(FrameInterpolation::OpticalFlow));
    interpolation_->addItem("Optical Flow (Quality)", static_cast<int>(FrameInterpolation::OpticalFlowQuality));
    interpolation_->addItem("AI Interpolation (RIFE)", static_cast<int>(FrameInterpolation::Neural));
    interpolation_->setToolTip("Frame Sampling repeats or skips source frames; Frame Blending mixes neighbouring frames;\n"
                               "Optical Flow moves the pixels along the measured motion (not AI) and blends where it\n"
                               "cannot tell (occlusions); AI Interpolation computes in-between frames with the RIFE model\n"
                               "(prepared in the background and before an export; until then the preview uses optical flow)");
    of->addRow("Frame Interpolation", interpolation_);
    prepareAi_ = new QPushButton("Prepare AI Frames", options_);
    prepareAi_->setObjectName("speedPrepareAi");
    prepareAi_->setToolTip("Compute the clip's in-between frames with RIFE now (the export does it too)");
    prepareAi_->setVisible(false);   // shown for AI Interpolation only (refresh)
    aiNote_ = new QLabel(options_);
    aiNote_->setVisible(false);
    aiNote_->setObjectName("speedAiNote");
    aiNote_->setWordWrap(true);
    of->addRow(prepareAi_);
    of->addRow(aiNote_);
    reset_ = new QPushButton(icon("undo"), "Reset", uniformPage_);
    reset_->setObjectName("speedReset");
    reset_->setToolTip("Back to normal speed (1x forward)");
    form->addRow(reset_);
    info_ = new QLabel(uniformPage_);
    info_->setObjectName("speedInfo");
    info_->setWordWrap(true);
    info_->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    form->addRow(info_);
    uniformStack_->addWidget(uniformPage_);

    // ---- a freeze frame: its hold duration
    freezePage_ = new QWidget(uniformStack_);
    auto* ff = new QFormLayout(freezePage_);
    ff->setContentsMargins(12, 12, 12, 12);
    hold_ = new QLineEdit(freezePage_);
    hold_->setObjectName("holdDuration");
    hold_->setToolTip("How long the frame is held (hh:mm:ss:ff or seconds); the clips after it move");
    ff->addRow("Freeze duration", hold_);
    holdInfo_ = new QLabel(freezePage_);
    holdInfo_->setWordWrap(true);
    holdInfo_->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    ff->addRow(holdInfo_);
    uniformStack_->addWidget(freezePage_);
    tabs_->addTab(uniformStack_, "Uniform Speed");

    // ---- Speed Ramping
    presets_ = std::make_unique<SpeedPresetLibrary>();
    rampPage_ = new QWidget(tabs_);
    auto* rv = new QVBoxLayout(rampPage_);
    rv->setContentsMargins(12, 12, 12, 12);
    rv->setSpacing(8);
    rampPreset_ = new QComboBox(rampPage_);
    rampPreset_->setObjectName("rampPreset");
    rampPreset_->setToolTip("A ready-made speed curve; edit it in the graph");
    rv->addWidget(rampPreset_);
    graph_ = new RampGraph(rampPage_);
    graph_->setToolTip("Click to add a point, drag to move it, double-click or right-click to remove it.\n"
                       "x: position in the clip's source; y: speed (0.1x to 10x)");
    rv->addWidget(graph_, 1);
    keyFields_ = new QWidget(rampPage_);
    auto* keyRow = new QHBoxLayout(keyFields_);
    keyRow->setContentsMargins(0, 0, 0, 0);
    keySpeed_ = new QDoubleSpinBox(rampPage_);
    keySpeed_->setObjectName("rampKeySpeed");
    keySpeed_->setRange(kMinSpeed, kMaxSpeed);
    keySpeed_->setDecimals(2);
    keySpeed_->setSingleStep(0.1);
    keySpeed_->setSuffix(" x");
    keySpeed_->setKeyboardTracking(false);
    keyPos_ = new QDoubleSpinBox(rampPage_);
    keyPos_->setObjectName("rampKeyPosition");
    keyPos_->setRange(0, 100);
    keyPos_->setDecimals(1);
    keyPos_->setSuffix(" %");
    keyPos_->setKeyboardTracking(false);
    keyDelete_ = new QPushButton(icon("delete"), "", rampPage_);
    keyDelete_->setObjectName("rampDeleteKey");
    keyDelete_->setToolTip("Remove the selected point");
    keyRow->addWidget(new QLabel("Speed", rampPage_));
    keyRow->addWidget(keySpeed_);
    keyRow->addWidget(new QLabel("at", rampPage_));
    keyRow->addWidget(keyPos_);
    keyRow->addWidget(keyDelete_);
    rv->addWidget(keyFields_);
    keyHint_ = new QLabel("Click a point on the curve to type its speed, or click empty space to add one.", rampPage_);
    keyHint_->setObjectName("rampKeyHint");
    keyHint_->setWordWrap(true);
    keyHint_->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    rv->addWidget(keyHint_);
    auto* btnRow = new QHBoxLayout();
    rampReset_ = new QPushButton(icon("undo"), "Reset", rampPage_);
    rampReset_->setObjectName("rampReset");
    rampReset_->setToolTip("No ramp: normal speed (1x)");
    rampSave_ = new QPushButton("Save as Preset", rampPage_);
    rampSave_->setObjectName("rampSavePreset");
    btnRow->addWidget(rampReset_);
    btnRow->addStretch(1);
    btnRow->addWidget(rampSave_);
    rv->addLayout(btnRow);
    rampInfo_ = new QLabel(rampPage_);
    rampInfo_->setObjectName("rampInfo");
    rampInfo_->setWordWrap(true);
    rampInfo_->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    rv->addWidget(rampInfo_);
    tabs_->addTab(rampPage_, "Speed Ramping");
    lay->addWidget(options_);
    fillRampPresets();

    connect(graph_, &RampGraph::edited, this, [this](const RampKeys& keys, int serial) {
        if (const Clip* c = clip()) applyRamp(keys, "ramp-edit:" + c->id + ":" + std::to_string(serial), {});
    });
    connect(graph_, &RampGraph::selectionChanged, this, [this](int i) { showKeyFields(i); });
    auto editKey = [this] {
        const int i = graph_->selected();
        const Clip* c = clip();
        if (updating_ || !c || i < 0) return;
        RampKeys keys = graph_->keys();
        keys[i].second = keySpeed_->value();
        if (i > 0 && i + 1 < static_cast<int>(keys.size()))
            keys[i].first = std::clamp(keyPos_->value() / 100.0, keys[i - 1].first + 0.005, keys[i + 1].first - 0.005);
        applyRamp(keys, {}, {});
    };
    connect(keySpeed_, &QDoubleSpinBox::valueChanged, this, editKey);
    connect(keyPos_, &QDoubleSpinBox::valueChanged, this, editKey);
    connect(keyDelete_, &QPushButton::clicked, this, [this] {
        const int i = graph_->selected();
        RampKeys keys = graph_->keys();
        if (i <= 0 || i + 1 >= static_cast<int>(keys.size())) return;
        keys.erase(keys.begin() + i);
        graph_->setSelected(-1);
        applyRamp(keys, {}, {});
    });
    connect(rampPreset_, &QComboBox::activated, this, [this](int index) { applyRampPreset(index); });
    connect(rampReset_, &QPushButton::clicked, this, [this] {
        const Clip* c = clip();
        if (!c) return;
        ClipSpeed sp = c->speed;
        sp.ramp.clear();
        sp.rampPreset.clear();
        sp.speed = Rational(1);
        if (!(sp == c->speed)) edit_->setClipSpeed(c->id, sp);
    });
    connect(rampSave_, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Save Speed Preset", "Name:", QLineEdit::Normal, "My ramp", &ok);
        if (ok) saveRampPreset(name);
    });

    connect(slider_, &QSlider::valueChanged, this, [this](int pos) {
        if (updating_) return;
        applySpeed(sliderToSpeed(pos), slider_->isSliderDown());
    });
    connect(slider_, &QSlider::sliderReleased, this, [this] { ++dragSerial_; });
    connect(value_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!updating_) applySpeed(v, false);
    });
    connect(duration_, &QLineEdit::editingFinished, this, [this] {
        if (!updating_) applyDuration(duration_->text());
    });
    connect(reverse_, &QCheckBox::toggled, this, [this] { applyFlags(); });
    connect(pitch_, &QCheckBox::toggled, this, [this] { applyFlags(); });
    connect(interpolation_, &QComboBox::currentIndexChanged, this, [this] { applyFlags(); });
    connect(prepareAi_, &QPushButton::clicked, this, [this] {
        const Clip* c = clip();
        if (!analysis_ || !c) return;
        if (analysis_->busy() && analysis_->kind() == "prepare") {
            analysis_->cancel();
            return;
        }
        const Sequence& seq = doc_->sequence();
        AnalysisService::PrepareJob job;
        job.clipId = c->id;
        job.snapshot = doc_->snapshot();
        job.fromFrame = timeToFrame(c->timelineStart, seq.frameRate, Rounding::Floor);
        job.toFrame = timeToFrame(c->timelineEnd(), seq.frameRate, Rounding::Ceil);
        // long work only after a yes, with what it costs
        const ai::PreparationEstimate est = ai::estimatePreparation(*job.snapshot.project, job.snapshot.sequenceId,
                                                                    job.snapshot.projectFile, job.fromFrame, job.toFrame);
        if (est.frames() == 0) {
            emit edit_->message("AI Interpolation: every in-between frame of the clip is prepared already");
            return;
        }
        if (ai::needsConfirmation(est)) {
            const QString text = QString("Preparing %1.%2 Start?")
                                     .arg(QString::fromStdString(est.text))
                                     .arg(est.cpu ? " There is no usable GPU for it, so this PC's processor does the work." : "");
            if (!(confirmSlow ? confirmSlow(text) : QMessageBox::question(this, "AI Interpolation", text) == QMessageBox::Yes))
                return;
        }
        if (!analysis_->startPrepare(job)) emit edit_->message("Another analysis is running");
    });
    if (analysis_) {
        connect(analysis_, &AnalysisService::started, this, [this] { refresh(); });
        connect(analysis_, &AnalysisService::progress, this, [this](int done, int total) {
            if (analysis_->kind() == "prepare" && aiNote_->isVisible())
                aiNote_->setText(QString("Preparing AI frames... %1 %").arg(100 * done / std::max(1, total)));
        });
        connect(analysis_, &AnalysisService::prepareFinished, this, [this](const QString&, const QString& summary) {
            emit edit_->message(summary);
            doc_->refreshPictures();
            refresh();
        });
        connect(analysis_, &AnalysisService::failed, this, [this](const QString&, const QString& msg) {
            if (analysis_->kind() == "prepare" && msg != "cancelled") emit edit_->message(msg);
            refresh();
        });
    }
    connect(reset_, &QPushButton::clicked, this, [this] {
        if (const Clip* c = clip()) edit_->resetClipSpeed(c->id);
    });
    connect(hold_, &QLineEdit::editingFinished, this, [this] {
        const Clip* c = clip();
        if (updating_ || !c) return;
        const FrameRate rate = doc_->sequence().frameRate;
        auto d = parseDuration(hold_->text(), rate);
        if (!d) {
            refresh();
            return;
        }
        const Time frames = rate.frameDuration() * Rational(std::max<int64_t>(1, (*d / rate.frameDuration()).round()));
        if (frames != c->duration) edit_->setHoldDuration(c->id, frames);
        refresh();
    });
    connect(doc_, &Document::changed, this, [this] { refresh(); });
    connect(doc_, &Document::selectionChanged, this, [this] { refresh(); });
    refresh();
}

const Clip* SpeedPanel::clip() const {
    for (const auto& id : doc_->selection()) {
        const Clip* c = edit::findClip(doc_->sequence(), id);
        if (c && !c->isGenerated()) return c;
    }
    return nullptr;
}

void SpeedPanel::applySpeed(double v, bool merge) {
    const Clip* c = clip();
    if (!c) return;
    ClipSpeed sp = c->speed;
    sp.ramp.clear();
    sp.rampPreset.clear();
    sp.speed = speedRational(v);
    if (sp == c->speed) return;
    edit_->setClipSpeed(c->id, sp, merge ? "speed-drag:" + c->id + ":" + std::to_string(dragSerial_) : std::string());
}

void SpeedPanel::applyDuration(const QString& text) {
    const Clip* c = clip();
    if (!c) return;
    const FrameRate rate = doc_->sequence().frameRate;
    const auto d = parseDuration(text, rate);
    if (!d) {
        refresh();
        return;
    }
    const Time frames = rate.frameDuration() * Rational(std::max<int64_t>(1, (*d / rate.frameDuration()).round()));
    const Time length = sourceLength(*c);
    if (frames == c->duration || length.isZero()) {
        refresh();
        return;
    }
    ClipSpeed sp = c->speed;
    sp.ramp.clear();
    sp.rampPreset.clear();
    Rational v = length / frames;   // exact: plays the same source range in the new length
    if (v.toDouble() < kMinSpeed || v.toDouble() > kMaxSpeed) {
        emit edit_->message(QString("That length needs %1x; the speed range is 0.1x to 10x").arg(v.toDouble(), 0, 'f', 2));
        v = speedRational(v.toDouble());
    }
    sp.speed = v;
    edit_->setClipSpeed(c->id, sp);
    refresh();
}

void SpeedPanel::applyFlags() {
    const Clip* c = clip();
    if (updating_ || !c) return;
    ClipSpeed sp = c->speed;
    sp.reverse = reverse_->isChecked();
    sp.maintainPitch = pitch_->isChecked();
    sp.interpolation = static_cast<FrameInterpolation>(interpolation_->currentData().toInt());
    if (sp.reverse && sp.ramp.size() >= 2) {   // reverse works with a uniform speed
        sp.ramp.clear();
        sp.rampPreset.clear();
    }
    if (sp == c->speed) return;
    edit_->setClipSpeed(c->id, sp);
}

void SpeedPanel::showKeyFields(int i) {
    const int n = static_cast<int>(graph_->keys().size());
    const bool on = i >= 0 && i < n;
    keyFields_->setVisible(on);
    keyHint_->setVisible(!on);
    if (!on) return;
    keyPos_->setEnabled(i > 0 && i + 1 < n);   // the end points stay at 0 % and 100 %
    keyDelete_->setEnabled(keyPos_->isEnabled());
    const bool was = updating_;
    updating_ = true;
    keySpeed_->setValue(graph_->keys()[i].second);
    keyPos_->setValue(graph_->keys()[i].first * 100.0);
    updating_ = was;
}

Time SpeedPanel::shownSourceLength(const Clip& c) const {
    Time len = sourceLength(c);
    if (const MediaSource* src = doc_->project().findSource(c.sourceId); src && src->duration)
        len = min(len, *src->duration - c.sourceIn);   // the last whole frame may reach a few ms past the media
    return len;
}

RampKeys SpeedPanel::rampFractions(const Clip& c) const {
    RampKeys out;
    if (c.speed.ramp.size() < 2) return {{0.0, c.speed.speed.toDouble()}, {1.0, c.speed.speed.toDouble()}};
    const double in = c.sourceIn.toDouble(), len = std::max(1e-9, sourceLength(c).toDouble());
    for (const SpeedKey& k : c.speed.ramp) out.push_back({std::clamp((k.source.toDouble() - in) / len, 0.0, 1.0), k.speed});
    return out;
}

void SpeedPanel::applyRamp(const RampKeys& keys, const std::string& mergeKey, const std::string& presetId) {
    const Clip* c = clip();
    if (!c || keys.size() < 2) return;
    ClipSpeed sp = c->speed;
    sp.reverse = false;   // ramps play forward
    const Time len = sourceLength(*c);
    sp.ramp = rampKeys(keys, c->sourceIn, len);
    sp.rampPreset = presetId;
    bool flat = true;   // a curve that is 1x everywhere is no ramp
    for (const auto& k : keys) flat = flat && std::fabs(k.second - 1.0) < 1e-9;
    if (flat) {
        sp.ramp.clear();
        sp.rampPreset.clear();
        sp.speed = Rational(1);
    }
    if (sp == c->speed) return;
    edit_->setClipSpeed(c->id, sp, mergeKey);
}

void SpeedPanel::fillRampPresets() {
    const bool was = updating_;
    updating_ = true;
    rampPreset_->clear();
    rampPreset_->addItem("None (normal speed)", QString());
    for (const RampPreset& p : rampPresets())
        rampPreset_->addItem(QString::fromStdString(p.name), QString::fromStdString("builtin:" + p.id));
    if (!presets_->presets().empty()) {
        rampPreset_->insertSeparator(rampPreset_->count());
        for (const auto& p : presets_->presets()) rampPreset_->addItem("My Presets: " + p.name, "user:" + p.name);
    }
    rampPreset_->addItem("Customize (edited by hand)", QString("custom"));
    updating_ = was;
}

void SpeedPanel::applyRampPreset(int index) {
    const Clip* c = clip();
    if (!c || index < 0) return;
    const QString id = rampPreset_->itemData(index).toString();
    if (id.isEmpty()) {   // None
        ClipSpeed sp = c->speed;
        sp.ramp.clear();
        sp.rampPreset.clear();
        sp.speed = Rational(1);
        if (!(sp == c->speed)) edit_->setClipSpeed(c->id, sp);
        return;
    }
    if (id.startsWith("builtin:")) {
        const RampPreset* p = findRampPreset(id.mid(8).toStdString());
        if (p) applyRamp(p->keys, {}, p->id);
        return;
    }
    if (id.startsWith("user:")) {
        for (const auto& p : presets_->presets())
            if (p.name == id.mid(5)) applyRamp(p.keys, {}, {});
    }
}

bool SpeedPanel::saveRampPreset(const QString& name) {
    const Clip* c = clip();
    if (!c || c->speed.ramp.size() < 2) {
        emit edit_->message("Make a speed ramp first");
        return false;
    }
    if (!presets_->add(name, rampFractions(*c))) {
        emit edit_->message("The preset could not be saved: " + presets_->lastError());
        return false;
    }
    fillRampPresets();
    refresh();
    emit edit_->info("Saved \"" + name + "\" to Speed Ramping > My Presets");
    return true;
}

void SpeedPanel::refresh() {
    const Clip* c = clip();
    setEnabled(c != nullptr);
    if (!c) return;
    updating_ = true;
    clipId_ = c->id;
    const FrameRate rate = doc_->sequence().frameRate;
    const ClipSpeed& sp = c->speed;
    options_->setVisible(!sp.freeze);
    tabs_->setTabVisible(tabs_->indexOf(rampPage_), !sp.freeze);
    if (sp.freeze) {
        uniformStack_->setCurrentWidget(freezePage_);
        hold_->setText(timecode(c->duration, rate));
        holdInfo_->setText(QString("Holds the source frame at %1. Freeze frames are silent.").arg(seconds(c->sourceIn)));
        updating_ = false;
        return;
    }
    uniformStack_->setCurrentWidget(uniformPage_);
    const bool ramp = sp.ramp.size() >= 2;
    const double v = sp.speed.toDouble();
    slider_->setValue(speedToSlider(v));
    value_->setValue(v);
    slider_->setEnabled(true);
    duration_->setText(timecode(c->duration, rate));
    reverse_->setChecked(sp.reverse);
    pitch_->setChecked(sp.maintainPitch);
    interpolation_->setCurrentIndex(std::max(0, interpolation_->findData(static_cast<int>(sp.interpolation))));
    {   // M7: AI Interpolation
        const bool neural = sp.interpolation == FrameInterpolation::Neural;
        const bool running = analysis_ && analysis_->busy() && analysis_->kind() == "prepare";
        prepareAi_->setVisible(neural && analysis_);
        aiNote_->setVisible(neural);
        prepareAi_->setText(running ? "Cancel" : "Prepare AI Frames");
        prepareAi_->setEnabled(analysis_ && (running || !analysis_->busy()));
        if (neural && !running)
            aiNote_->setText(isRetimed(sp) ? "RIFE frames are shown where they have been prepared (here, or by the export); "
                                             "frames not prepared yet are shown with optical flow. Shot changes are never "
                                             "blended."
                                           : "AI Interpolation only adds frames when the clip is slowed down or ramped.");
    }
    const Time len = shownSourceLength(*c);
    QString text = QString("Plays source %1 to %2 (%3).")
                       .arg(seconds(c->sourceIn), seconds(c->sourceIn + len), seconds(len));
    if (ramp) text += " This clip has a speed ramp: changing the speed here replaces it with a uniform speed.";
    text += " Speed range 0.1x to 10x.";
    info_->setText(text);
    // Speed Ramping
    graph_->setKeys(rampFractions(*c));
    int pi = 0;   // None
    if (ramp) {
        pi = rampPreset_->findData(QString::fromStdString("builtin:" + sp.rampPreset));
        if (sp.rampPreset.empty() || pi < 0) pi = rampPreset_->findData(QString("custom"));
    }
    rampPreset_->setCurrentIndex(std::max(0, pi));
    showKeyFields(graph_->selected());
    rampSave_->setEnabled(ramp);
    rampInfo_->setText(ramp ? QString("Duration %1 for %2 of source. Points sit at source positions; between them the speed "
                                      "eases smoothly over time.")
                                  .arg(timecode(c->duration, rate), seconds(shownSourceLength(*c)))
                            : QString("Pick a preset or click the graph to add speed points."));
    updating_ = false;
}

}  // namespace fa::ui
