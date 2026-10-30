#include "inspector_panel.h"
#include "cutout_panel.h"
#include "upscale_panel.h"
#include "animation_panel.h"

#include "fa/text/stickers.h"

#include <QColorDialog>

#include "stabilization_panel.h"
#include "tracking_panel.h"

#include "speed_panel.h"

#include "color_panel.h"
#include "edit_controller.h"
#include "effect_editor.h"
#include "theme.h"
#include "title_inspector.h"
#include "title_library.h"

#include "fa/model/edit_ops.h"
#include "fa/model/keyframes.h"
#include "analysis_service.h"
#include "fa/render/audio_dsp.h"
#include "fa/render/transitions.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fa::ui {

namespace {

QString tc(const Time& t, const FrameRate& r) {
    return QString::fromStdString(formatTimecode(timeToFrame(t, r, Rounding::Nearest), r));
}

QLabel* dimLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    l->setWordWrap(true);
    return l;
}

QWidget* scrolled(QWidget* content) {
    auto* s = new QScrollArea();
    s->setWidgetResizable(true);
    s->setFrameShape(QFrame::NoFrame);
    s->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    s->setWidget(content);
    return s;
}

}  // namespace

InspectorPanel::InspectorPanel(Document* doc, EditController* edit, AnalysisService* analysis, QWidget* parent)
    : QWidget(parent), doc_(doc), edit_(edit), analysis_(analysis) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    stack_ = new QStackedWidget(this);
    lay->addWidget(stack_);

    projectPage_ = buildProjectPage();
    transitionPage_ = buildTransitionPage();
    clipTabs_ = new QTabWidget(this);
    clipTabs_->setDocumentMode(true);
    videoSub_ = new QTabWidget(clipTabs_);
    videoSub_->setDocumentMode(true);
    videoSub_->setObjectName("videoSubTabs");
    videoSub_->addTab(scrolled(buildVideoBasic()), "Basic");
    videoSub_->addTab(scrolled(buildVideoMask()), "Mask");
    tracking_ = new TrackingPanel(doc_, edit_, analysis);
    videoSub_->addTab(scrolled(tracking_), "Tracking");
    stabilization_ = new StabilizationPanel(doc_, edit_, analysis);
    videoSub_->addTab(scrolled(stabilization_), "Stabilization");
    cutout_ = new CutoutPanel(doc_, edit_, analysis);   // M7 Background Removal
    cutoutPage_ = scrolled(cutout_);
    videoSub_->addTab(cutoutPage_, "AI Matting");   // M9: Filmora's name for it
    upscale_ = new UpscalePanel(doc_, edit_, analysis);   // M7 Video Upscaling
    videoSub_->addTab(scrolled(upscale_), "Upscale");
    titleInspector_ = new TitleInspector(doc_, edit_, clipTabs_);
    // M9: Filmora's order - Video, Audio, Speed, Animation, Color (and Effects for the applied effects)
    clipTabs_->addTab(titleInspector_, "Titles");
    clipTabs_->addTab(videoSub_, "Video");
    audioTab_ = scrolled(buildAudioPage());
    clipTabs_->addTab(audioTab_, "Audio");
    speed_ = new SpeedPanel(doc_, edit_, nullptr, analysis);
    clipTabs_->addTab(speed_, "Speed");
    animation_ = new AnimationPanel(doc_, edit_);
    clipTabs_->addTab(animation_, "Animation");
    color_ = new ColorPanel(doc_, edit_);
    clipTabs_->addTab(color_, "Color");
    effects_ = new EffectStackEditor(doc_, edit_);
    effectsTab_ = scrolled(effects_);
    clipTabs_->addTab(effectsTab_, "Effects");
    connect(titleInspector_, &TitleInspector::seekRequested, this, &InspectorPanel::seekRequested);
    connect(titleInspector_, &TitleInspector::savePresetRequested, this, [this](const QString& name) {
        const Track* tr = nullptr;
        const Clip* c = edit::findClip(doc_->sequence(), clipId_, &tr);
        TitleLibrary* lib = edit_->titleLibrary();
        if (!c || !c->title || !lib) return;
        if (lib->addUserPreset(name, *c, c->duration, tr->kind == TrackKind::Subtitle).empty())
            emit edit_->message("The preset could not be saved: " + lib->lastError());
        else
            emit edit_->info("Saved \"" + name + "\" to Titles > My Titles");
    });
    stack_->addWidget(scrolled(projectPage_));
    stack_->addWidget(scrolled(transitionPage_));
    stack_->addWidget(clipTabs_);

    connect(videoSub_, &QTabWidget::currentChanged, this, [this](int i) {
        if (updating_) return;
        if (i != 1) doc_->setActiveMask({});
        else if (const Clip* c = clip(); c && !c->masks.empty() && doc_->activeMask().empty())
            doc_->setActiveMask(c->masks.front().id);
        updateTrackingShown();
        emit editModeChanged();
    });
    connect(clipTabs_, &QTabWidget::currentChanged, this, [this] { updateTrackingShown(); });
    connect(stack_, &QStackedWidget::currentChanged, this, [this] { updateTrackingShown(); });
    connect(doc_, &Document::selectionChanged, this, &InspectorPanel::refresh);
    connect(doc_, &Document::changed, this, &InspectorPanel::refresh);
    connect(doc_, &Document::maskSelectionChanged, this, &InspectorPanel::refresh);
    connect(doc_, &Document::playheadChanged, this, &InspectorPanel::refreshValues);
    // The mouse wheel scrolls the inspector; it only changes a spin box / combo box / slider that has
    // keyboard focus (otherwise scrolling over the panel would silently edit whatever is under the cursor).
    for (QWidget* w : findChildren<QWidget*>())
        if (qobject_cast<QAbstractSpinBox*>(w) || qobject_cast<QComboBox*>(w) || qobject_cast<QSlider*>(w)) {
            w->setFocusPolicy(Qt::StrongFocus);
            w->installEventFilter(this);
        }
    refresh();
}

bool InspectorPanel::eventFilter(QObject* o, QEvent* e) {
    if (e->type() == QEvent::Wheel) {
        auto* w = qobject_cast<QWidget*>(o);
        if (w && !w->hasFocus()) {
            e->ignore();   // propagates to the scroll area
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

QString InspectorPanel::videoSubTab() const {
    const int i = videoSub_->currentIndex();
    return i == 1 ? "mask" : i == 2 ? "tracking" : i == 3 ? "stabilization" : i == 4 ? "cutout" : i == 5 ? "upscale" : "basic";
}

void InspectorPanel::showCrop() {
    showVideoSubTab("basic");
    if (!cropBox_) return;
    for (QWidget* w = cropBox_->parentWidget(); w; w = w->parentWidget())
        if (auto* area = qobject_cast<QScrollArea*>(w)) {
            area->ensureWidgetVisible(cropBox_, 0, 40);
            break;
        }
    if (QWidget* first = cropBox_->findChild<QAbstractSpinBox*>()) first->setFocus();
}

void InspectorPanel::showVideoSubTab(const QString& name) {
    clipTabs_->setCurrentWidget(videoSub_);
    videoSub_->setCurrentIndex(name == "mask" ? 1 : name == "tracking" ? 2 : name == "stabilization" ? 3 : name == "cutout" ? 4 : name == "upscale" ? 5 : 0);
}

// The tracking box is shown in the player while Video > Tracking is the visible page.
void InspectorPanel::updateTrackingShown() {
    tracking_->setTabShown(stack_->currentIndex() == 2 && clipTabs_->currentWidget() == videoSub_ &&
                           videoSub_->currentIndex() == 2);
}

const Clip* InspectorPanel::clip() const {
    return clipId_.empty() ? nullptr : edit::findClip(doc_->sequence(), clipId_);
}

const Mask* InspectorPanel::activeMask() const {
    const Clip* c = clip();
    if (!c) return nullptr;
    for (const auto& m : c->masks)
        if (m.id == doc_->activeMask()) return &m;
    return nullptr;
}

std::string InspectorPanel::resolve(const std::string& property, bool mask) const {
    if (!mask) return property;
    const Mask* m = activeMask();
    return m ? "mask." + m->id + "." + property.substr(5) : std::string();   // "mask.centerX" -> "mask.<id>.centerX"
}

QGroupBox* InspectorPanel::group(const QString& title, QVBoxLayout* into) {
    auto* g = new QGroupBox(title);
    into->addWidget(g);
    return g;
}

QDoubleSpinBox* InspectorPanel::addSpin(QWidget* parent, const std::string& property, double min, double max,
                                        int decimals, const QString& suffix, double scale, bool mask,
                                        std::function<double()> dynamicScale) {
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(min, max);
    s->setDecimals(decimals);
    s->setSuffix(suffix);
    s->setKeyboardTracking(false);
    s->setAccelerated(true);
    s->setObjectName(QString::fromStdString("prop:" + property));
    s->setMinimumWidth(78);
    Field f;
    f.spin = s;
    f.property = property;
    f.scale = scale;
    f.dynamicScale = std::move(dynamicScale);
    fields_[property] = f;
    connect(s, &QDoubleSpinBox::valueChanged, this, [this, property, mask](double shown) {
        if (updating_ || clipId_.empty()) return;
        const Field& fd = fields_[property];
        const double sc = fd.dynamicScale ? fd.dynamicScale() : fd.scale;
        const double v = shown / sc;
        if (property.rfind("crop.", 0) == 0) {
            const std::string side = property.substr(5);
            edit_->modifyClip(clipId_, "Crop", [side, v](Clip& c) {
                double& slot = side == "left" ? c.transform.cropLeft : side == "top" ? c.transform.cropTop
                             : side == "right" ? c.transform.cropRight : c.transform.cropBottom;
                slot = std::clamp(v, 0.0, 0.95);
                if (c.transform.cropLeft + c.transform.cropRight >= 0.98) slot = 0.98 - (c.transform.cropLeft + c.transform.cropRight - slot);
                if (c.transform.cropTop + c.transform.cropBottom >= 0.98) slot = 0.98 - (c.transform.cropTop + c.transform.cropBottom - slot);
            }, clipId_ + "/" + property);
        } else if (property == "mask.cornerRadius") {
            const std::string id = doc_->activeMask();
            edit_->modifyClip(clipId_, "Mask corner radius", [id, v](Clip& c) {
                for (auto& m : c.masks)
                    if (m.id == id) m.cornerRadius = std::clamp(v, 0.0, 0.5);
            }, clipId_ + "/" + id + "/corner");
        } else {
            const std::string id = resolve(property, mask);
            if (!id.empty()) edit_->setProperty(clipId_, id, v);
        }
    });
    return s;
}

QWidget* InspectorPanel::keyRow(QWidget* parent, std::vector<std::string> properties, bool mask) {
    auto* w = new QWidget(parent);
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    KeyRow row;
    auto mk = [&](const char* ic, const QString& tip) {
        auto* b = new QToolButton(w);
        b->setIcon(icon(ic, color::textDim));
        b->setIconSize(QSize(14, 14));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        l->addWidget(b);
        return b;
    };
    row.prev = mk("prevKey", "Previous keyframe");
    row.diamond = mk("keyOff", "Add / remove a keyframe at the playhead");
    row.next = mk("nextKey", "Next keyframe");
    row.diamond->setObjectName(QString::fromStdString("key:" + properties.front()));
    row.properties = properties;
    row.mask = mask;
    keyRows_.push_back(row);
    connect(row.diamond, &QToolButton::clicked, this, [this, properties, mask] {
        if (clipId_.empty()) return;
        if (!doc_->playheadInside(clipId_)) {
            emit edit_->message("Move the playhead over the clip to set a keyframe");
            return;
        }
        std::vector<std::string> ids;
        for (const auto& p : properties) {
            const std::string id = resolve(p, mask);
            if (!id.empty()) ids.push_back(id);
        }
        const Time at = *doc_->clipTimeAtPlayhead(clipId_);
        edit_->modifyClip(clipId_, "Keyframe", [ids, at](Clip& c) {
            bool any = false;
            for (const auto& id : ids)
                if (const auto* a = keys::property(c, id); a && keys::hasKeyAt(*a, at)) any = true;
            for (const auto& id : ids) {
                Animatable<double>* a = keys::property(c, id);
                if (a && keys::hasKeyAt(*a, at) == any) keys::toggleKey(*a, at);
            }
            if (std::find(ids.begin(), ids.end(), "transform.scaleX") != ids.end() && c.transform.uniformScale &&
                keys::hasKeyAt(c.transform.scaleX, at) != keys::hasKeyAt(c.transform.scaleY, at))
                keys::toggleKey(c.transform.scaleY, at);
        });
    });
    auto jump = [this, properties, mask](bool forward) {
        const Clip* c = clip();
        if (!c) return;
        const auto now = doc_->clipTimeAtPlayhead(clipId_);
        if (!now) return;
        std::optional<Time> best;
        for (const auto& p : properties) {
            const Animatable<double>* a = keys::property(*c, resolve(p, mask));
            if (!a) continue;
            for (const auto& k : a->keys()) {
                if (forward && k.time > *now && (!best || k.time < *best)) best = k.time;
                if (!forward && k.time < *now && (!best || k.time > *best)) best = k.time;
            }
        }
        if (best) emit seekRequested(timeToFrame(keys::toTimeline(*c, *best), doc_->sequence().frameRate, Rounding::Nearest));
    };
    connect(row.prev, &QToolButton::clicked, this, [jump] { jump(false); });
    connect(row.next, &QToolButton::clicked, this, [jump] { jump(true); });
    return w;
}

// ---- pages -----------------------------------------------------------------------------------

QWidget* InspectorPanel::buildProjectPage() {
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(16, 14, 16, 14);
    auto* title = new QLabel("Project Info", w);
    title->setStyleSheet(QString("color: %1; font-weight: 600; font-size: 11pt;").arg(color::accent.name()));
    l->addWidget(title);
    l->addSpacing(10);
    projectForm_ = new QFormLayout();
    projectForm_->setHorizontalSpacing(28);
    projectForm_->setVerticalSpacing(14);
    l->addLayout(projectForm_);
    l->addSpacing(16);
    l->addWidget(dimLabel("Select a clip on the timeline to edit its properties, or a transition to change it.", w));
    l->addStretch(1);
    return w;
}

QWidget* InspectorPanel::buildTransitionPage() {
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(12, 12, 12, 12);
    auto* g = group("Transition", l);
    auto* f = new QFormLayout(g);
    transForm_ = f;
    transType_ = new QComboBox(g);
    fillTransitionTypes(false);
    transType_->setObjectName("transitionType");
    transDuration_ = new QDoubleSpinBox(g);
    transDuration_->setObjectName("transitionDuration");
    transDuration_->setRange(0.01, 3600);
    transDuration_->setDecimals(3);
    transDuration_->setSuffix(" s");
    transDuration_->setKeyboardTracking(false);
    transAlign_ = new QComboBox(g);
    transAlign_->setObjectName("transitionAlignment");
    transAlign_->addItem("Overlap (centred on the cut)", static_cast<int>(TransitionAlignment::Centered));
    transAlign_->addItem("Prefix (ends at the cut)", static_cast<int>(TransitionAlignment::EndsAtCut));
    transAlign_->addItem("Postfix (starts at the cut)", static_cast<int>(TransitionAlignment::StartsAtCut));
    transDir_ = new QComboBox(g);
    transDir_->setObjectName("transitionDirection");
    transDir_->addItem("Left", static_cast<int>(TransitionDirection::Left));
    transDir_->addItem("Right", static_cast<int>(TransitionDirection::Right));
    transDir_->addItem("Up", static_cast<int>(TransitionDirection::Up));
    transDir_->addItem("Down", static_cast<int>(TransitionDirection::Down));
    transSoft_ = new QDoubleSpinBox(g);
    transSoft_->setObjectName("transitionSoftness");
    transSoft_->setRange(0, 100);
    transSoft_->setDecimals(0);
    transSoft_->setSuffix(" %");
    transSoft_->setKeyboardTracking(false);
    auto* remove = new QPushButton(icon("delete"), "Remove transition", g);
    f->addRow("Type", transType_);
    f->addRow("Duration", transDuration_);
    f->addRow("Position", transAlign_);
    f->addRow("Direction", transDir_);
    f->addRow("Softness", transSoft_);
    f->addRow(remove);
    transInfo_ = dimLabel("", g);
    f->addRow(transInfo_);
    l->addStretch(1);
    auto apply = [this] {
        if (updating_) return;
        const std::string id = doc_->selectedTransition();
        const Clip* c = edit::findClip(doc_->sequence(), id);
        if (!c || !c->outTransition) return;
        TransitionRef t = *c->outTransition;
        t.type = transType_->currentData().toString().toStdString();
        const FrameRate r = doc_->sequence().frameRate;
        t.duration = frameToTime(std::max<int64_t>(1, std::llround(transDuration_->value() * r.toDouble())), r);
        t.alignment = static_cast<TransitionAlignment>(transAlign_->currentData().toInt());
        t.direction = static_cast<TransitionDirection>(transDir_->currentData().toInt());
        t.softness = transSoft_->value() / 100.0;
        edit_->updateTransitionSettings(id, t);
    };
    connect(transType_, &QComboBox::currentIndexChanged, this, apply);
    connect(transDuration_, &QDoubleSpinBox::valueChanged, this, apply);
    connect(transAlign_, &QComboBox::currentIndexChanged, this, apply);
    connect(transDir_, &QComboBox::currentIndexChanged, this, apply);
    connect(transSoft_, &QDoubleSpinBox::valueChanged, this, apply);
    connect(remove, &QPushButton::clicked, this, [this] {
        const std::string id = doc_->selectedTransition();
        if (edit_->removeTransition(id)) doc_->setSelection({id});
    });
    return w;
}

QWidget* InspectorPanel::buildVideoBasic() {
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(10, 10, 10, 10);
    l->setSpacing(10);
    auto seqW = [this] { return double(doc_->sequence().width); };
    auto seqH = [this] { return double(doc_->sequence().height); };

    auto* clipBox = group("Clip", l);
    auto* cf = new QFormLayout(clipBox);
    name_ = new QLineEdit(clipBox);
    clipInfo_ = dimLabel("", clipBox);
    enabled_ = new QCheckBox("Enabled", clipBox);
    cf->addRow("Name", name_);
    cf->addRow(clipInfo_);
    cf->addRow(enabled_);
    connect(name_, &QLineEdit::editingFinished, this, [this] {
        const Clip* c = clip();
        if (!updating_ && c && QString::fromUtf8(c->name) != name_->text()) edit_->renameClip(clipId_, name_->text());
    });
    connect(enabled_, &QCheckBox::toggled, this, [this](bool on) {
        const Clip* c = clip();
        if (!updating_ && c && c->enabled != on) edit_->toggleClipEnabled();
    });

    // M9: a sticker's colour and words
    stickerBox_ = group("Sticker", l);
    auto* sf = new QFormLayout(stickerBox_);
    auto* colorRow = new QWidget(stickerBox_);
    auto* crl = new QHBoxLayout(colorRow);
    crl->setContentsMargins(0, 0, 0, 0);
    stickerColor_ = new QPushButton("Color...", colorRow);
    stickerColor_->setObjectName("stickerColor");
    auto* stickerReset = new QPushButton("Original", colorRow);
    stickerReset->setObjectName("stickerColorReset");
    crl->addWidget(stickerColor_);
    crl->addWidget(stickerReset);
    crl->addStretch(1);
    stickerText_ = new QLineEdit(stickerBox_);
    stickerText_->setObjectName("stickerText");
    sf->addRow("Color", colorRow);
    sf->addRow("Text", stickerText_);
    auto setLook = [this](std::optional<uint32_t> color) {
        const Clip* c = clip();
        if (updating_ || !c || !c->sticker) return;
        edit_->setStickerLook(clipId_, color ? *color : c->sticker->color, stickerText_->text());
    };
    connect(stickerColor_, &QPushButton::clicked, this, [this, setLook] {
        const Clip* c = clip();
        if (!c || !c->sticker) return;
        const uint32_t cur = c->sticker->color;
        const QColor picked = QColorDialog::getColor(cur ? QColor::fromRgb(cur) : QColor(Qt::white), this, "Sticker color");
        if (picked.isValid()) setLook(picked.rgb() & 0xFFFFFF);
    });
    connect(stickerReset, &QPushButton::clicked, this, [setLook] { setLook(0u); });
    connect(stickerText_, &QLineEdit::editingFinished, this, [setLook] { setLook(std::nullopt); });

    // Transform
    auto* tg = group("Transform", l);
    transformBox_ = tg;
    auto* tf = new QFormLayout(tg);
    tf->setLabelAlignment(Qt::AlignLeft);
    auto* scaleRow = new QWidget(tg);
    auto* sl = new QHBoxLayout(scaleRow);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->addWidget(new QLabel("W", scaleRow));
    sl->addWidget(addSpin(scaleRow, "transform.scaleX", 1, 1000, 2, " %", 100));
    lockScale_ = new QToolButton(scaleRow);
    lockScale_->setCheckable(true);
    lockScale_->setIcon(icon("link"));
    lockScale_->setToolTip("Keep the aspect ratio (width and height scale together)");
    lockScale_->setObjectName("lockScale");
    sl->addWidget(lockScale_);
    sl->addWidget(new QLabel("H", scaleRow));
    sl->addWidget(addSpin(scaleRow, "transform.scaleY", 1, 1000, 2, " %", 100));
    sl->addWidget(keyRow(scaleRow, {"transform.scaleX", "transform.scaleY"}));
    tf->addRow("Scale", scaleRow);
    connect(lockScale_, &QToolButton::toggled, this, [this](bool on) {
        if (updating_) return;
        edit_->modifyClip(clipId_, on ? "Lock aspect ratio" : "Unlock aspect ratio", [on](Clip& c) {
            c.transform.uniformScale = on;
            if (on) c.transform.scaleY = c.transform.scaleX;
        });
    });
    auto* posRow = new QWidget(tg);
    auto* pl = new QHBoxLayout(posRow);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->addWidget(new QLabel("X", posRow));
    pl->addWidget(addSpin(posRow, "transform.positionX", -100000, 100000, 2, " px", 1, false, seqW));
    pl->addWidget(new QLabel("Y", posRow));
    pl->addWidget(addSpin(posRow, "transform.positionY", -100000, 100000, 2, " px", 1, false, seqH));
    pl->addWidget(keyRow(posRow, {"transform.positionX", "transform.positionY"}));
    tf->addRow("Position", posRow);
    auto* rotRow = new QWidget(tg);
    auto* rl = new QHBoxLayout(rotRow);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->addWidget(addSpin(rotRow, "transform.rotation", -36000, 36000, 2, QString::fromUtf8("\xC2\xB0"), 1), 1);
    rl->addWidget(keyRow(rotRow, {"transform.rotation"}));
    tf->addRow("Rotate", rotRow);
    auto* ancRow = new QWidget(tg);
    auto* al = new QHBoxLayout(ancRow);
    al->setContentsMargins(0, 0, 0, 0);
    al->addWidget(new QLabel("X", ancRow));
    al->addWidget(addSpin(ancRow, "transform.anchorX", -500, 500, 1, " %", 100));
    al->addWidget(new QLabel("Y", ancRow));
    al->addWidget(addSpin(ancRow, "transform.anchorY", -500, 500, 1, " %", 100));
    al->addWidget(keyRow(ancRow, {"transform.anchorX", "transform.anchorY"}));
    tf->addRow("Anchor", ancRow);
    auto* flipRow = new QWidget(tg);
    auto* fl = new QHBoxLayout(flipRow);
    fl->setContentsMargins(0, 0, 0, 0);
    auto mkBtn = [&](const char* ic, const QString& tip, bool checkable) {
        auto* b = new QToolButton(flipRow);
        b->setIcon(icon(ic));
        b->setIconSize(QSize(18, 18));
        b->setToolTip(tip);
        b->setCheckable(checkable);
        fl->addWidget(b);
        return b;
    };
    flipH_ = mkBtn("flipH", "Flip horizontally", true);
    flipV_ = mkBtn("flipV", "Flip vertically", true);
    auto* rotL = mkBtn("rotateLeft", QString::fromUtf8("Rotate 90\xC2\xB0 counter-clockwise"), false);
    auto* rotR = mkBtn("rotateRight", QString::fromUtf8("Rotate 90\xC2\xB0 clockwise"), false);
    flipH_->setObjectName("flipH");
    flipV_->setObjectName("flipV");
    fl->addStretch(1);
    tf->addRow("Flip", flipRow);
    connect(flipH_, &QToolButton::toggled, this, [this](bool on) {
        if (!updating_) edit_->modifyClip(clipId_, "Flip horizontally", [on](Clip& c) { c.transform.flipH = on; });
    });
    connect(flipV_, &QToolButton::toggled, this, [this](bool on) {
        if (!updating_) edit_->modifyClip(clipId_, "Flip vertically", [on](Clip& c) { c.transform.flipV = on; });
    });
    auto rotateBy = [this](double deg) {
        const Clip* c = clip();
        const auto t = doc_->clipTimeAtPlayhead(clipId_);
        if (c && t) edit_->setProperty(clipId_, "transform.rotation", c->transform.rotation.value(*t) + deg);
    };
    connect(rotL, &QToolButton::clicked, this, [rotateBy] { rotateBy(-90); });
    connect(rotR, &QToolButton::clicked, this, [rotateBy] { rotateBy(90); });
    auto* resetT = new QPushButton("Reset transform", tg);
    resetT->setObjectName("resetTransform");
    connect(resetT, &QPushButton::clicked, this, [this] { edit_->resetTransform(clipId_); });
    tf->addRow(resetT);

    // Crop
    auto* crop = group("Crop", l);
    cropBox_ = crop;
    auto* crf = new QFormLayout(crop);
    auto* c1 = new QWidget(crop);
    auto* c1l = new QHBoxLayout(c1);
    c1l->setContentsMargins(0, 0, 0, 0);
    c1l->addWidget(new QLabel("Left", c1));
    c1l->addWidget(addSpin(c1, "crop.left", 0, 95, 1, " %", 100));
    c1l->addWidget(new QLabel("Right", c1));
    c1l->addWidget(addSpin(c1, "crop.right", 0, 95, 1, " %", 100));
    auto* c2 = new QWidget(crop);
    auto* c2l = new QHBoxLayout(c2);
    c2l->setContentsMargins(0, 0, 0, 0);
    c2l->addWidget(new QLabel("Top", c2));
    c2l->addWidget(addSpin(c2, "crop.top", 0, 95, 1, " %", 100));
    c2l->addWidget(new QLabel("Bottom", c2));
    c2l->addWidget(addSpin(c2, "crop.bottom", 0, 95, 1, " %", 100));
    crf->addRow(c1);
    crf->addRow(c2);

    // Compositing
    auto* comp = group("Compositing", l);
    compositingBox_ = comp;
    auto* cpf = new QFormLayout(comp);
    blend_ = new QComboBox(comp);
    blend_->setObjectName("blendMode");
    for (int i = 0; i < kBlendModeCount; ++i)
        blend_->addItem(QString::fromUtf8(blendModeLabel(static_cast<BlendMode>(i))), i);
    cpf->addRow("Blend mode", blend_);
    connect(blend_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!updating_ && i >= 0) edit_->setBlendMode(clipId_, static_cast<BlendMode>(blend_->itemData(i).toInt()));
    });
    auto* opRow = new QWidget(comp);
    auto* ol = new QHBoxLayout(opRow);
    ol->setContentsMargins(0, 0, 0, 0);
    opacitySlider_ = new QSlider(Qt::Horizontal, opRow);
    opacitySlider_->setRange(0, 100);
    ol->addWidget(opacitySlider_, 1);
    ol->addWidget(addSpin(opRow, "opacity", 0, 100, 0, " %", 100));
    ol->addWidget(keyRow(opRow, {"opacity"}));
    cpf->addRow("Opacity", opRow);
    connect(opacitySlider_, &QSlider::valueChanged, this, [this](int v) {
        if (!updating_) fields_["opacity"].spin->setValue(v);   // routed through the spin box (merged undo)
    });
    l->addWidget(dimLabel("Tip: in the player, drag the picture to move it, its corners to scale, the round handle "
                          "to rotate (Shift: 15 degree steps) and Ctrl+drag the centre cross to move the anchor point. "
                          "The diamonds add keyframes at the playhead; once a property has keyframes, changing it "
                          "adds or updates the keyframe at the playhead.", w));
    l->addStretch(1);
    return w;
}

QWidget* InspectorPanel::buildVideoMask() {
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(10, 10, 10, 10);
    l->setSpacing(10);
    auto seqH = [this] { return double(doc_->sequence().height); };

    auto* lg = group("Masks", l);
    auto* ll = new QVBoxLayout(lg);
    auto* addRow = new QHBoxLayout();
    auto mkAdd = [&](const char* ic, const QString& text, MaskShape shape) {
        auto* b = new QToolButton(lg);
        b->setIcon(icon(ic));
        b->setText(text);
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setIconSize(QSize(22, 22));
        b->setObjectName("addMask" + text);
        connect(b, &QToolButton::clicked, this, [this, shape] {
            if (!clipId_.empty()) edit_->addMask(clipId_, shape);
        });
        addRow->addWidget(b);
    };
    mkAdd("maskRect", "Rectangle", MaskShape::Rectangle);
    mkAdd("maskEllipse", "Ellipse", MaskShape::Ellipse);
    mkAdd("maskCustom", "Custom", MaskShape::Polygon);
    addRow->addStretch(1);
    ll->addLayout(addRow);
    maskList_ = new QListWidget(lg);
    maskList_->setMaximumHeight(96);
    maskList_->setObjectName("maskList");
    ll->addWidget(maskList_);
    auto* del = new QPushButton(icon("delete"), "Delete mask", lg);
    ll->addWidget(del);
    connect(maskList_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (updating_) return;
        const Clip* c = clip();
        if (c && row >= 0 && row < static_cast<int>(c->masks.size())) doc_->setActiveMask(c->masks[row].id);
    });
    connect(del, &QPushButton::clicked, this, [this] {
        if (!doc_->activeMask().empty()) edit_->removeMask(clipId_, doc_->activeMask());
    });

    auto* pg = group("Mask settings", l);
    maskProps_ = pg;
    auto* pf = new QFormLayout(pg);
    auto* flags = new QWidget(pg);
    auto* fl = new QHBoxLayout(flags);
    fl->setContentsMargins(0, 0, 0, 0);
    maskEnabled_ = new QCheckBox("Enabled", flags);
    maskInvert_ = new QCheckBox("Invert", flags);
    maskInvert_->setObjectName("maskInvert");
    maskMode_ = new QComboBox(flags);
    maskMode_->addItem("Add", static_cast<int>(MaskMode::Add));
    maskMode_->addItem("Subtract", static_cast<int>(MaskMode::Subtract));
    maskMode_->addItem("Intersect", static_cast<int>(MaskMode::Intersect));
    fl->addWidget(maskEnabled_);
    fl->addWidget(maskInvert_);
    fl->addWidget(maskMode_);
    pf->addRow(flags);
    auto onMask = [this](const char* label, std::function<void(Mask&)> f) {
        const std::string id = doc_->activeMask();
        edit_->modifyClip(clipId_, label, [id, f](Clip& c) {
            for (auto& m : c.masks)
                if (m.id == id) f(m);
        });
    };
    connect(maskEnabled_, &QCheckBox::toggled, this, [this, onMask](bool on) {
        if (!updating_) onMask("Mask enabled", [on](Mask& m) { m.enabled = on; });
    });
    connect(maskInvert_, &QCheckBox::toggled, this, [this, onMask](bool on) {
        if (!updating_) onMask("Invert mask", [on](Mask& m) { m.inverted = on; });
    });
    connect(maskMode_, &QComboBox::currentIndexChanged, this, [this, onMask](int i) {
        if (updating_ || i < 0) return;
        const auto mode = static_cast<MaskMode>(maskMode_->itemData(i).toInt());
        onMask("Mask mode", [mode](Mask& m) { m.mode = mode; });
    });
    auto row2 = [&](const QString& label, const char* p1, const char* p2, double min, double max, const QString& suffix,
                    std::function<double()> dyn, double scale) {
        auto* r = new QWidget(pg);
        auto* rl = new QHBoxLayout(r);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->addWidget(new QLabel(p2 ? "X" : "", r));
        rl->addWidget(addSpin(r, p1, min, max, 1, suffix, scale, true, dyn));
        std::vector<std::string> props{p1};
        if (p2) {
            rl->addWidget(new QLabel(QString(p2).endsWith("height") ? "H" : "Y", r));
            rl->addWidget(addSpin(r, p2, min, max, 1, suffix, scale, true, dyn));
            props.push_back(p2);
        }
        rl->addWidget(keyRow(r, props, true));
        pf->addRow(label, r);
    };
    row2("Position", "mask.centerX", "mask.centerY", -100000, 100000, " px", seqH, 1);
    row2("Size", "mask.width", "mask.height", 0, 100000, " px", seqH, 1);
    row2("Rotate", "mask.rotation", nullptr, -36000, 36000, QString::fromUtf8("\xC2\xB0"), {}, 1);
    row2("Feather", "mask.feather", nullptr, 0, 100000, " px", seqH, 1);
    row2("Opacity", "mask.opacity", nullptr, 0, 100, " %", {}, 100);
    cornerRow_ = new QWidget(pg);
    auto* crl = new QHBoxLayout(cornerRow_);
    crl->setContentsMargins(0, 0, 0, 0);
    crl->addWidget(addSpin(cornerRow_, "mask.cornerRadius", 0, 50, 1, " %", 100, true), 1);
    pf->addRow("Corner radius", cornerRow_);
    maskHint_ = dimLabel("Drag the mask in the player to move it; drag its corners to resize and the round handle to "
                         "rotate. Custom masks: drag the points.", pg);
    pf->addRow(maskHint_);
    l->addStretch(1);
    return w;
}

QWidget* InspectorPanel::buildAudioPage() {
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(10, 10, 10, 10);
    l->setSpacing(10);

    // Volume and balance are animatable (keyframes at the playhead, also drawn as the envelope on the timeline).
    auto* adj = group("Adjustment", l);
    auto* af = new QFormLayout(adj);
    auto sliderRow = [&](QSlider*& slider, int lo, int hi, QDoubleSpinBox* spin, const std::string& property,
                         const char* name) {
        auto* row = new QWidget(adj);
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        slider = new QSlider(Qt::Horizontal, row);
        slider->setRange(lo, hi);
        slider->setObjectName(name);
        rl->addWidget(slider, 1);
        spin->setParent(row);
        rl->addWidget(spin);
        rl->addWidget(keyRow(row, {property}));
        return row;
    };
    volumeSpin_ = addSpin(adj, "audio.volume", -60.0, 12.0, 1, " dB", 1.0);
    volumeSpin_->setSingleStep(0.5);
    af->addRow("Volume", sliderRow(volume_, -600, 120, volumeSpin_, "audio.volume", "audioVolumeSlider"));
    balanceSpin_ = addSpin(adj, "audio.balance", -100.0, 100.0, 0, "", 100.0);
    balanceSpin_->setToolTip("-100 = left only, 0 = centre, 100 = right only");
    af->addRow("Balance", sliderRow(balance_, -100, 100, balanceSpin_, "audio.balance", "audioBalanceSlider"));
    connect(volume_, &QSlider::valueChanged, this, [this](int v) {
        if (!updating_) volumeSpin_->setValue(v / 10.0);
    });
    connect(volumeSpin_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        QSignalBlocker b(volume_);
        volume_->setValue(static_cast<int>(std::lround(v * 10)));
    });
    connect(balance_, &QSlider::valueChanged, this, [this](int v) {
        if (!updating_) balanceSpin_->setValue(v);
    });
    connect(balanceSpin_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        QSignalBlocker b(balance_);
        balance_->setValue(static_cast<int>(std::lround(v)));
    });

    auto mkFade = [&](const char* name) {
        auto* s = new QDoubleSpinBox(adj);
        s->setObjectName(name);
        s->setRange(0, 3600);
        s->setDecimals(2);
        s->setSingleStep(0.1);
        s->setSuffix(" s");
        s->setKeyboardTracking(false);
        return s;
    };
    fadeIn_ = mkFade("audioFadeIn");
    fadeOut_ = mkFade("audioFadeOut");
    af->addRow("Fade In", fadeIn_);
    af->addRow("Fade Out", fadeOut_);
    auto applyFades = [this] {
        if (updating_ || clipId_.empty()) return;
        edit_->setClipFades(clipId_, Time::fromDouble(fadeIn_->value(), 1000), Time::fromDouble(fadeOut_->value(), 1000),
                            clipId_ + "/fades");
    };
    connect(fadeIn_, &QDoubleSpinBox::valueChanged, this, applyFades);
    connect(fadeOut_, &QDoubleSpinBox::valueChanged, this, applyFades);
    af->addRow(dimLabel("Fades are linear in amplitude from the clip's edges (or drag the handles at the clip's top "
                        "corners on the timeline). Linked video and audio move, trim and split together; Detach Audio "
                        "(Ctrl+Alt+D) edits them separately.", adj));

    // Equaliser: ten octave bands (MeerCut's own presets).
    auto* eqBox = group("Equalizer", l);
    auto* el = new QVBoxLayout(eqBox);
    auto* top = new QHBoxLayout();
    eqEnabled_ = new QCheckBox("Enabled", eqBox);
    eqEnabled_->setObjectName("audioEqEnabled");
    eqPreset_ = new QComboBox(eqBox);
    eqPreset_->setObjectName("audioEqPreset");
    eqPreset_->addItem("Custom", QString());
    for (const auto& p : eqPresets()) eqPreset_->addItem(QString::fromStdString(p.name), QString::fromStdString(p.id));
    top->addWidget(eqEnabled_);
    top->addWidget(eqPreset_, 1);
    el->addLayout(top);
    auto* bands = new QHBoxLayout();
    bands->setSpacing(2);
    for (int i = 0; i < kEqBands; ++i) {
        auto* col = new QVBoxLayout();
        auto* value = new QLabel("0.0", eqBox);
        value->setAlignment(Qt::AlignHCenter);
        value->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(color::textDim.name()));
        auto* sl = new QSlider(Qt::Vertical, eqBox);
        sl->setRange(-120, 120);
        sl->setMinimumHeight(110);
        sl->setObjectName(QString("audioEqBand%1").arg(i));
        const double f = kEqFrequencies[static_cast<size_t>(i)];
        auto* freq = new QLabel(f >= 1000 ? QString("%1k").arg(f / 1000) : QString::number(std::lround(f)), eqBox);
        freq->setAlignment(Qt::AlignHCenter);
        freq->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(color::textDim.name()));
        col->addWidget(value);
        col->addWidget(sl, 1, Qt::AlignHCenter);
        col->addWidget(freq);
        bands->addLayout(col);
        eqBands_.push_back(sl);
        eqValues_.push_back(value);
        connect(sl, &QSlider::valueChanged, this, [this, i](int v) {
            eqValues_[static_cast<size_t>(i)]->setText(QString::number(v / 10.0, 'f', 1));
            if (!updating_) applyEq(i, QString());
        });
    }
    el->addLayout(bands);
    auto* reset = new QPushButton("Reset", eqBox);
    reset->setObjectName("audioEqReset");
    el->addWidget(reset, 0, Qt::AlignRight);
    el->addWidget(dimLabel("Peaking filters an octave apart, +-12 dB. Moving a band switches the preset to Custom.", eqBox));
    connect(eqEnabled_, &QCheckBox::toggled, this, [this] {
        if (!updating_) applyEq(-1, eqPreset_->currentData().toString());
    });
    connect(eqPreset_, &QComboBox::currentIndexChanged, this, [this] {
        if (updating_) return;
        const QString id = eqPreset_->currentData().toString();
        if (const EqPreset* p = findEqPreset(id.toStdString())) {
            updating_ = true;
            for (size_t i = 0; i < eqBands_.size(); ++i) eqBands_[i]->setValue(static_cast<int>(std::lround(p->gainsDb[i] * 10)));
            eqEnabled_->setChecked(true);
            updating_ = false;
        }
        applyEq(-1, id);
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        if (!clipId_.empty()) edit_->setClipEq(clipId_, false, {}, {});
    });

    // Cleanup: conventional DSP (fa/render/audio_cleanup.h) - labelled as such, not "AI".
    auto* clBox = group("Cleanup", l);
    auto* cf = new QFormLayout(clBox);
    auto* dnRow = new QHBoxLayout();
    denoise_ = new QSlider(Qt::Horizontal, clBox);
    denoise_->setObjectName("audioDenoise");
    denoise_->setRange(0, 100);
    denoiseValue_ = new QLabel("Off", clBox);
    denoiseValue_->setMinimumWidth(36);
    dnRow->addWidget(denoise_, 1);
    dnRow->addWidget(denoiseValue_);
    cf->addRow("Noise reduction", dnRow);
    hum_ = new QComboBox(clBox);
    hum_->setObjectName("audioHum");
    hum_->addItem("Off", 0.0);
    hum_->addItem("50 Hz", 50.0);
    hum_->addItem("60 Hz", 60.0);
    cf->addRow("Hum removal", hum_);
    auto mkHz = [&](const char* name, double off, double hi) {
        auto* s = new QDoubleSpinBox(clBox);
        s->setObjectName(name);
        s->setRange(off, hi);   // the minimum = Off
        s->setDecimals(0);
        s->setSuffix(" Hz");
        s->setSpecialValueText("Off");
        s->setKeyboardTracking(false);
        return s;
    };
    highPass_ = mkHz("audioHighPass", 10, 1000);   // Off, 20 .. 1000 Hz
    highPass_->setSingleStep(10);
    lowPass_ = mkHz("audioLowPass", 500, 22000);   // Off, 1000 .. 22000 Hz
    lowPass_->setSingleStep(500);
    cf->addRow("High-pass", highPass_);
    cf->addRow("Low-pass", lowPass_);
    auto* ldRow = new QHBoxLayout();
    normalize_ = new QCheckBox("Normalize to", clBox);
    normalize_->setObjectName("audioNormalize");
    targetLufs_ = new QDoubleSpinBox(clBox);
    targetLufs_->setObjectName("audioTargetLufs");
    targetLufs_->setRange(-40, -5);
    targetLufs_->setDecimals(1);
    targetLufs_->setSuffix(" LUFS");
    targetLufs_->setKeyboardTracking(false);
    measure_ = new QPushButton("Measure", clBox);
    measure_->setObjectName("audioMeasure");
    ldRow->addWidget(normalize_);
    ldRow->addWidget(targetLufs_, 1);
    cf->addRow("Loudness", ldRow);
    measured_ = dimLabel("", clBox);
    measured_->setObjectName("audioMeasured");
    cf->addRow(measure_, measured_);
    cf->addRow(dimLabel("Conventional signal processing, not AI: spectral noise reduction for steady hiss and room noise "
                        "(not clicks, wind or other voices), hum notches, 24 dB/octave filters, loudness per ITU-R "
                        "BS.1770-4 (Measure analyses the clip after cleanup and EQ).", clBox));
    connect(denoise_, &QSlider::valueChanged, this, [this](int v) {
        denoiseValue_->setText(v == 0 ? QString("Off") : QString("%1 %").arg(v));
        if (!updating_) applyCleanup();
    });
    connect(hum_, &QComboBox::currentIndexChanged, this, [this] {
        if (!updating_) applyCleanup();
    });
    for (QDoubleSpinBox* s : {highPass_, lowPass_, targetLufs_})
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] {
            if (!updating_) applyCleanup();
        });
    connect(normalize_, &QCheckBox::toggled, this, [this] {
        if (!updating_) applyCleanup();
    });
    connect(measure_, &QPushButton::clicked, this, [this] {
        if (clipId_.empty() || !analysis_) return;
        AnalysisService::LoudnessJob job;
        job.clipId = clipId_;
        job.project = doc_->project();
        job.sequenceId = doc_->sequenceId();
        job.projectFile = doc_->filePath().isEmpty() ? std::filesystem::path() : std::filesystem::path(doc_->filePath().toStdWString());
        if (!analysis_->startLoudness(std::move(job))) {
            emit edit_->message("Another analysis is running");
            return;
        }
        measure_->setEnabled(false);
        measured_->setText("Measuring...");
    });
    if (analysis_) {
        connect(analysis_, &AnalysisService::loudnessFinished, this, [this](const QString& id, double lufs) {
            if (const Clip* c = edit::findClip(doc_->sequence(), id.toStdString())) {
                AudioCleanup k = c->audio.cleanup;
                k.measuredLufs = std::isfinite(lufs) ? lufs : -70.0;   // silence: below the absolute gate
                edit_->setClipCleanup(id.toStdString(), k);
            }
            measure_->setEnabled(true);
            refresh();
        });
        connect(analysis_, &AnalysisService::failed, this, [this](const QString&, const QString& msg) {
            if (analysis_->kind() != "loudness") return;
            measure_->setEnabled(true);
            measured_->setText("Not measured: " + msg);
        });
    }

    // Auto Ducking: this clip lowers other tracks while it has sound.
    auto* dkBox = group("Auto Ducking", l);
    auto* df = new QFormLayout(dkBox);
    duckEnabled_ = new QCheckBox("Lower other tracks while this clip has sound", dkBox);
    duckEnabled_->setObjectName("audioDuckEnabled");
    df->addRow(duckEnabled_);
    duckTargets_ = new QListWidget(dkBox);
    duckTargets_->setObjectName("audioDuckTargets");
    duckTargets_->setMaximumHeight(72);
    df->addRow("Tracks", duckTargets_);
    df->addRow(dimLabel("None ticked: every other audio track.", dkBox));
    auto mkDuck = [&](const char* name, double lo, double hi, int decimals, const QString& suffix, double step) {
        auto* s = new QDoubleSpinBox(dkBox);
        s->setObjectName(name);
        s->setRange(lo, hi);
        s->setDecimals(decimals);
        s->setSuffix(suffix);
        s->setSingleStep(step);
        s->setKeyboardTracking(false);
        return s;
    };
    duckAmount_ = mkDuck("audioDuckAmount", -60, 0, 1, " dB", 1);
    duckThreshold_ = mkDuck("audioDuckThreshold", -90, 0, 1, " dBFS", 1);
    duckAttack_ = mkDuck("audioDuckAttack", 0, 10000, 0, " ms", 50);
    duckHold_ = mkDuck("audioDuckHold", 0, 10000, 0, " ms", 50);
    duckRelease_ = mkDuck("audioDuckRelease", 0, 10000, 0, " ms", 50);
    df->addRow("Lower by", duckAmount_);
    df->addRow("Threshold", duckThreshold_);
    df->addRow("Attack", duckAttack_);
    df->addRow("Hold", duckHold_);
    df->addRow("Release", duckRelease_);
    df->addRow(dimLabel("Signal based: the clip has sound where a 10 ms block of it (after cleanup, EQ, volume and fades) "
                        "is above the threshold. The other tracks are fully down when the sound starts (the attack "
                        "begins earlier), stay down for the hold, then return over the release.", dkBox));
    connect(duckEnabled_, &QCheckBox::toggled, this, [this] {
        if (!updating_) applyDucking();
    });
    connect(duckTargets_, &QListWidget::itemChanged, this, [this] {
        if (!updating_) applyDucking();
    });
    for (QDoubleSpinBox* s : {duckAmount_, duckThreshold_, duckAttack_, duckHold_, duckRelease_})
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] {
            if (!updating_) applyDucking();
        });

    // Master: the sequence's limiter (the whole mix, playback and export).
    auto* msBox = group("Master", l);
    auto* mf = new QFormLayout(msBox);
    auto* lmRow = new QHBoxLayout();
    limiter_ = new QCheckBox("Limiter", msBox);
    limiter_->setObjectName("audioLimiter");
    limiterCeiling_ = new QDoubleSpinBox(msBox);
    limiterCeiling_->setObjectName("audioLimiterCeiling");
    limiterCeiling_->setRange(-20, 0);
    limiterCeiling_->setDecimals(1);
    limiterCeiling_->setSuffix(" dBFS");
    limiterCeiling_->setKeyboardTracking(false);
    lmRow->addWidget(limiter_);
    lmRow->addWidget(limiterCeiling_, 1);
    mf->addRow(lmRow);
    mf->addRow(dimLabel("A sample-peak limiter on the whole mix (not true-peak): 50 ms look-ahead, peaks stay at or "
                        "under the ceiling in playback and export.", msBox));
    connect(limiter_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) edit_->setLimiter(on, limiterCeiling_->value());
    });
    connect(limiterCeiling_, &QDoubleSpinBox::valueChanged, this, [this](double db) {   // a drag is one undo step
        if (!updating_) edit_->setLimiter(limiter_->isChecked(), db, "limiter/ceiling");
    });

    // The clip's track: gain, mute and solo (also in the timeline track header).
    auto* trBox = group("Track", l);
    auto* tf = new QFormLayout(trBox);
    trackName_ = dimLabel("", trBox);
    trackVolume_ = new QDoubleSpinBox(trBox);
    trackVolume_->setObjectName("trackVolume");
    trackVolume_->setRange(-60.0, 12.0);
    trackVolume_->setDecimals(1);
    trackVolume_->setSingleStep(0.5);
    trackVolume_->setSuffix(" dB");
    trackVolume_->setKeyboardTracking(false);
    auto* flags = new QHBoxLayout();
    trackMute_ = new QCheckBox("Mute", trBox);
    trackMute_->setObjectName("trackMute");
    trackSolo_ = new QCheckBox("Solo", trBox);
    trackSolo_->setObjectName("trackSolo");
    flags->addWidget(trackMute_);
    flags->addWidget(trackSolo_);
    flags->addStretch(1);
    tf->addRow(trackName_);
    tf->addRow("Track volume", trackVolume_);
    tf->addRow(flags);
    auto trackOf = [this]() -> std::string {
        const Track* tr = nullptr;
        return !clipId_.empty() && edit::findClip(doc_->sequence(), clipId_, &tr) ? tr->id : std::string();
    };
    connect(trackVolume_, &QDoubleSpinBox::valueChanged, this, [this, trackOf](double db) {
        if (updating_) return;
        const std::string t = trackOf();
        if (!t.empty()) edit_->modifyTrack(t, "Track volume", [db](Track& x) { x.volumeDb = db; }, t + "/volume");
    });
    connect(trackMute_, &QCheckBox::toggled, this, [this, trackOf](bool on) {
        if (updating_) return;
        const std::string t = trackOf();
        if (!t.empty()) edit_->setTrackFlag(t, "muted", on);
    });
    connect(trackSolo_, &QCheckBox::toggled, this, [this, trackOf](bool on) {
        if (updating_) return;
        const std::string t = trackOf();
        if (!t.empty()) edit_->setTrackFlag(t, "solo", on);
    });
    l->addStretch(1);
    return w;
}

void InspectorPanel::applyEq(int changedBand, const QString& preset) {
    if (clipId_.empty()) return;
    std::array<double, kAudioEqBands> g{};
    for (size_t i = 0; i < eqBands_.size() && i < g.size(); ++i) g[i] = eqBands_[i]->value() / 10.0;
    const bool enabled = changedBand >= 0 ? true : eqEnabled_->isChecked();   // moving a band turns the EQ on
    edit_->setClipEq(clipId_, enabled, changedBand >= 0 ? std::string() : preset.toStdString(), g, clipId_ + "/eq");
}

void InspectorPanel::applyCleanup() {
    const Clip* c = clip();
    if (!c) return;
    AudioCleanup k = c->audio.cleanup;
    k.denoise = denoise_->value() / 100.0;
    k.humHz = hum_->currentData().toDouble();
    k.highPassHz = highPass_->value() <= highPass_->minimum() ? 0.0 : std::max(20.0, highPass_->value());
    k.lowPassHz = lowPass_->value() <= lowPass_->minimum() ? 0.0 : std::max(1000.0, lowPass_->value());
    k.normalize = normalize_->isChecked();
    k.targetLufs = targetLufs_->value();
    edit_->setClipCleanup(clipId_, k, clipId_ + "/cleanup");
}

void InspectorPanel::applyDucking() {
    const Clip* c = clip();
    if (!c) return;
    AudioDucking d = c->audio.ducking;
    d.enabled = duckEnabled_->isChecked();
    d.targets.clear();
    for (int i = 0; i < duckTargets_->count(); ++i)
        if (duckTargets_->item(i)->checkState() == Qt::Checked)
            d.targets.push_back(duckTargets_->item(i)->data(Qt::UserRole).toString().toStdString());
    d.amountDb = duckAmount_->value();
    d.thresholdDb = duckThreshold_->value();
    d.attackMs = duckAttack_->value();
    d.holdMs = duckHold_->value();
    d.releaseMs = duckRelease_->value();
    edit_->setClipDucking(clipId_, d, clipId_ + "/ducking");
}

// Cleanup, loudness, ducking and master settings of the selected audio clip (called with updating_ set).
void InspectorPanel::refreshAudioExtras(const Clip& c, const Track& track) {
    const AudioCleanup& k = c.audio.cleanup;
    denoise_->setValue(static_cast<int>(std::lround(k.denoise * 100)));
    denoiseValue_->setText(denoise_->value() == 0 ? QString("Off") : QString("%1 %").arg(denoise_->value()));
    hum_->setCurrentIndex(std::max(0, hum_->findData(k.humHz)));
    highPass_->setValue(k.highPassHz > 0 ? k.highPassHz : highPass_->minimum());
    lowPass_->setValue(k.lowPassHz > 0 ? k.lowPassHz : lowPass_->minimum());
    normalize_->setChecked(k.normalize);
    targetLufs_->setValue(k.targetLufs);
    if (analysis_ && analysis_->busy() && analysis_->kind() == "loudness") {
        measured_->setText("Measuring...");
    } else if (k.measuredLufs) {
        QString text = QString("Measured %1 LUFS").arg(*k.measuredLufs, 0, 'f', 1);
        if (k.normalize) text += QString(" - gain %1 dB").arg(std::clamp(k.targetLufs - *k.measuredLufs, -40.0, 40.0), 0, 'f', 1);
        measured_->setText(text + "  (re-measure after changing the clip)");
    } else {
        measured_->setText(k.normalize ? "Not measured yet: press Measure" : QString());
    }
    const AudioDucking& d = c.audio.ducking;
    duckEnabled_->setChecked(d.enabled);
    duckTargets_->clear();
    for (const Track& t : doc_->sequence().tracks) {
        if (t.kind != TrackKind::Audio || t.id == track.id) continue;
        auto* it = new QListWidgetItem(QString::fromUtf8(t.name.empty() ? t.id : t.name), duckTargets_);
        it->setData(Qt::UserRole, QString::fromStdString(t.id));
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(std::find(d.targets.begin(), d.targets.end(), t.id) != d.targets.end() ? Qt::Checked : Qt::Unchecked);
    }
    duckAmount_->setValue(d.amountDb);
    duckThreshold_->setValue(d.thresholdDb);
    duckAttack_->setValue(d.attackMs);
    duckHold_->setValue(d.holdMs);
    duckRelease_->setValue(d.releaseMs);
    limiter_->setChecked(doc_->sequence().limiter);
    limiterCeiling_->setValue(doc_->sequence().limiterCeilingDb);
}

// The Type list of the transition page: picture transitions, or the crossfade shapes for an audio transition.
void InspectorPanel::fillTransitionTypes(bool audio) {
    if (transType_->count() > 0 && transitionTypesAudio_ == audio) return;
    transitionTypesAudio_ = audio;
    QSignalBlocker b(transType_);
    transType_->clear();
    if (audio) {
        for (CrossfadeShape sh : {CrossfadeShape::ConstantGain, CrossfadeShape::EqualPower, CrossfadeShape::Exponential,
                                  CrossfadeShape::Logarithmic})
            transType_->addItem(QString::fromUtf8(crossfadeShapeLabel(sh)),
                                QString::fromStdString(std::string(kAudioCrossfadePrefix) + crossfadeShapeId(sh)));
    } else {
        for (const auto& t : transitionTypes()) transType_->addItem(QString::fromUtf8(t.label), QString::fromUtf8(t.id));
    }
}

// ---- refresh ---------------------------------------------------------------------------------

void InspectorPanel::refresh() {
    updating_ = true;
    const Sequence& s = doc_->sequence();
    // transition selected
    if (!doc_->selectedTransition().empty()) {
        const Clip* c = edit::findClip(s, doc_->selectedTransition());
        if (c && c->outTransition) {
            clipId_.clear();
            const TransitionRef& t = *c->outTransition;
            const bool audio = isAudioCrossfade(t.type);
            fillTransitionTypes(audio);
            transType_->setCurrentIndex(std::max(0, transType_->findData(QString::fromStdString(t.type))));
            transDuration_->setValue(t.duration.toDouble());
            transAlign_->setCurrentIndex(transAlign_->findData(static_cast<int>(t.alignment)));
            transDir_->setCurrentIndex(transDir_->findData(static_cast<int>(t.direction)));
            transSoft_->setValue(t.softness * 100);
            const TransitionInfo* info = audio ? nullptr : findTransition(t.type);
            transDir_->setEnabled(info && info->usesDirection);
            transSoft_->setEnabled(info && info->usesSoftness);
            transForm_->setRowVisible(transDir_, !audio);   // picture-only settings
            transForm_->setRowVisible(transSoft_, !audio);
            if (audio)
                transInfo_->setText("Audio crossfade: the outgoing clip fades out while the incoming one fades in. Constant "
                                    "Gain: amplitudes sum to 1 (-6 dB in the middle); Equal Power: powers sum to 1 (-3 dB); "
                                    "Exponential: the outgoing clip drops quickly; Logarithmic: it holds, then drops late.");
            else
                transInfo_->setText(info ? QString::fromUtf8(info->description) : QString());
            stack_->setCurrentIndex(1);
            updating_ = false;
            return;
        }
    }
    const std::string id = doc_->primarySelection();
    const Track* tr = nullptr;
    const Clip* c = id.empty() ? nullptr : edit::findClip(s, id, &tr);
    clipId_ = c ? id : std::string();
    if (!c) {   // Project Info
        while (projectForm_->rowCount() > 0) projectForm_->removeRow(0);
        auto row = [this](const QString& k, const QString& v) {
            auto* kl = dimLabel(k, nullptr);
            auto* vl = new QLabel(v);
            vl->setTextInteractionFlags(Qt::TextSelectableByMouse);
            projectForm_->addRow(kl, vl);
        };
        row("Project Name", doc_->displayName());
        row("Project Files Location",
            doc_->filePath().isEmpty() ? "Not saved yet" : QDir::toNativeSeparators(QFileInfo(doc_->filePath()).absolutePath()));
        row("Resolution", QString("%1 x %2").arg(s.width).arg(s.height));
        row("Frame Rate", (s.frameRate.fps().isInteger() ? QString::number(s.frameRate.fps().num())
                                                         : QString::number(s.frameRate.toDouble(), 'f', 3)) + "fps");
        row("Color Space", "SDR - Rec.709");
        row("Sample Rate", QString("%1Hz").arg(s.sampleRate));
        row("Duration", QString::fromStdString(formatTimecode(s.frameCount(), s.frameRate)));
        stack_->setCurrentIndex(0);
        updating_ = false;
        return;
    }
    stack_->setCurrentIndex(2);
    const bool video = tr->kind == TrackKind::Video;
    const bool isTitle = c->isTitle();
    const bool adjustment = c->isAdjustment();
    const int ti = clipTabs_->indexOf(titleInspector_), vi = clipTabs_->indexOf(videoSub_), ai = clipTabs_->indexOf(audioTab_);
    const int ei = clipTabs_->indexOf(effectsTab_), ci = clipTabs_->indexOf(color_), si = clipTabs_->indexOf(speed_);
    clipTabs_->setTabVisible(ti, isTitle);
    clipTabs_->setTabVisible(vi, video);
    clipTabs_->setTabVisible(ci, video);
    clipTabs_->setTabVisible(ei, video);   // media, titles and adjustment layers on video tracks
    clipTabs_->setTabVisible(ai, tr->kind == TrackKind::Audio);
    clipTabs_->setTabVisible(si, !c->isGenerated());   // M5: media clips (video and audio)
    clipTabs_->setTabVisible(clipTabs_->indexOf(animation_), video && !adjustment);   // M9 motion presets
    if (id != lastClipId_ || !clipTabs_->isTabVisible(clipTabs_->currentIndex()))
        clipTabs_->setCurrentIndex(isTitle ? ti : adjustment ? ei : video ? vi : ai);
    if (video) {
        effects_->setClip(id);
        color_->setClip(id);
    }
    if (transformBox_) transformBox_->setVisible(!adjustment);   // an adjustment layer covers the whole frame
    lastClipId_ = id;
    videoSub_->setTabVisible(1, !isTitle);   // masks and crop: media clips only
    videoSub_->setTabVisible(2, !c->isGenerated());   // M5 motion tracking and stabilisation: media clips
    videoSub_->setTabVisible(3, !c->isGenerated());
    videoSub_->setTabVisible(4, !c->isGenerated());   // M7 Background Removal: media clips
    videoSub_->setTabVisible(5, !c->isGenerated());   // M7 Video Upscaling: media clips
    if ((isTitle && videoSub_->currentIndex() == 1) || (c->isGenerated() && videoSub_->currentIndex() >= 2))
        videoSub_->setCurrentIndex(0);
    if (cropBox_) cropBox_->setVisible(!isTitle && !adjustment);
    if (stickerBox_) {   // M9
        const text::StickerInfo* info = c->sticker ? text::findSticker(c->sticker->id) : nullptr;
        stickerBox_->setVisible(info && (info->tintable || !info->defaultText.empty()));
        if (info) {
            stickerColor_->setEnabled(info->tintable);
            const QColor col = c->sticker->color ? QColor::fromRgb(c->sticker->color) : color::panelHeader;
            stickerColor_->setStyleSheet(QString("QPushButton { border-left: 14px solid %1; }").arg(col.name()));
            stickerText_->setEnabled(!info->defaultText.empty());
            stickerText_->setPlaceholderText(QString::fromStdString(info->defaultText));
            if (!stickerText_->hasFocus()) stickerText_->setText(QString::fromStdString(c->sticker->text));
        }
    }
    if (blend_) {
        blend_->setEnabled(!adjustment);
        blend_->setToolTip(adjustment ? "An adjustment layer mixes its result with Normal blending (opacity and masks)" : QString());
    }
    if (isTitle) titleInspector_->setClip(id);
    if (!isTitle && !doc_->activeMask().empty() && !isEffectMaskRef(doc_->activeMask()) && videoSub_->currentIndex() != 1)
        videoSub_->setCurrentIndex(1);
    const MediaSource* src = doc_->project().findSource(c->sourceId);
    name_->setText(QString::fromUtf8(c->name));
    clipInfo_->setText(QString("Source: %1\nStart %2   Duration %3   Source in %4%5")
                           .arg(isTitle ? QString("generated title") : adjustment ? QString("adjustment layer")
                                : c->sticker ? QString("sticker (%1)").arg(QString::fromStdString(c->sticker->id))
                                : src   ? QFileInfo(QString::fromUtf8(src->path)).fileName()
                                        : QString("?"))
                           .arg(tc(c->timelineStart, s.frameRate))
                           .arg(tc(c->duration, s.frameRate))
                           .arg(tc(c->sourceIn, s.frameRate))
                           .arg(c->linkId.empty() ? "" : "\nLinked with its audio/video partner"));
    enabled_->setChecked(c->enabled);
    lockScale_->setChecked(c->transform.uniformScale);
    fields_["transform.scaleY"].spin->setEnabled(!c->transform.uniformScale);
    flipH_->setChecked(c->transform.flipH);
    flipV_->setChecked(c->transform.flipV);
    blend_->setCurrentIndex(blend_->findData(static_cast<int>(c->blend)));
    // masks
    maskList_->clear();
    int activeRow = -1;
    for (size_t i = 0; i < c->masks.size(); ++i) {
        const Mask& m = c->masks[i];
        const QString shape = m.shape == MaskShape::Rectangle ? "Rectangle" : m.shape == MaskShape::Ellipse ? "Ellipse" : "Custom";
        auto* it = new QListWidgetItem(icon(m.shape == MaskShape::Rectangle ? "maskRect" : m.shape == MaskShape::Ellipse ? "maskEllipse" : "maskCustom"),
                                       QString("%1 %2%3").arg(shape).arg(i + 1).arg(m.enabled ? "" : " (off)"));
        maskList_->addItem(it);
        if (m.id == doc_->activeMask()) activeRow = static_cast<int>(i);
    }
    maskList_->setCurrentRow(activeRow);
    const Mask* am = activeMask();
    maskProps_->setEnabled(am != nullptr);
    if (am) {
        maskEnabled_->setChecked(am->enabled);
        maskInvert_->setChecked(am->inverted);
        maskMode_->setCurrentIndex(maskMode_->findData(static_cast<int>(am->mode)));
        cornerRow_->setEnabled(am->shape == MaskShape::Rectangle);
    }
    // audio: fades, equaliser and the clip's track (volume / balance follow the playhead in refreshValues)
    fadeIn_->setMaximum(c->duration.toDouble());
    fadeOut_->setMaximum(c->duration.toDouble());
    fadeIn_->setValue(c->audio.fadeIn.toDouble());
    fadeOut_->setValue(c->audio.fadeOut.toDouble());
    eqEnabled_->setChecked(c->audio.eqEnabled);
    eqPreset_->setCurrentIndex(std::max(0, eqPreset_->findData(QString::fromStdString(c->audio.eqPreset))));
    for (size_t i = 0; i < eqBands_.size(); ++i) {
        eqBands_[i]->setValue(static_cast<int>(std::lround(c->audio.eqGainsDb[i] * 10)));
        eqValues_[i]->setText(QString::number(c->audio.eqGainsDb[i], 'f', 1));
    }
    trackName_->setText(QString("Track: %1").arg(QString::fromUtf8(tr->name)));
    trackVolume_->setValue(tr->volumeDb);
    trackMute_->setChecked(tr->muted);
    trackSolo_->setChecked(tr->solo);
    refreshAudioExtras(*c, *tr);
    updating_ = false;
    refreshValues();
}

void InspectorPanel::refreshValues() {
    if (effects_ && clipTabs_->isTabVisible(clipTabs_->indexOf(effectsTab_))) effects_->refreshValues();
    if (color_ && clipTabs_->isTabVisible(clipTabs_->indexOf(color_))) color_->refresh();
    const Clip* c = clip();
    if (!c) return;
    const auto t = doc_->clipTimeAtPlayhead(clipId_);
    if (!t) return;
    updating_ = true;
    const bool inside = doc_->playheadInside(clipId_);
    for (auto& [prop, f] : fields_) {
        const double sc = f.dynamicScale ? f.dynamicScale() : f.scale;
        double v = 0;
        bool known = true;
        if (prop.rfind("crop.", 0) == 0) {
            const std::string side = prop.substr(5);
            v = side == "left" ? c->transform.cropLeft : side == "top" ? c->transform.cropTop
              : side == "right" ? c->transform.cropRight : c->transform.cropBottom;
        } else if (prop == "mask.cornerRadius") {
            const Mask* m = activeMask();
            known = m != nullptr;
            v = m ? m->cornerRadius : 0;
        } else {
            const bool isMask = prop.rfind("mask.", 0) == 0;
            const Animatable<double>* a = keys::property(*c, resolve(prop, isMask));
            known = a != nullptr;
            v = a ? a->value(*t) : 0;
        }
        if (known) f.spin->setValue(v * sc);
    }
    {
        QSignalBlocker b(opacitySlider_);
        opacitySlider_->setValue(static_cast<int>(std::lround(c->opacity.value(*t) * 100)));
    }
    {
        QSignalBlocker b1(volume_), b2(balance_);
        volume_->setValue(static_cast<int>(std::lround(c->volumeDb.value(*t) * 10)));
        balance_->setValue(static_cast<int>(std::lround(c->audio.balance.value(*t) * 100)));
    }
    for (auto& row : keyRows_) {
        bool animated = false, onKey = false;
        for (const auto& p : row.properties) {
            const Animatable<double>* a = keys::property(*c, resolve(p, row.mask));
            if (!a) continue;
            animated |= a->isAnimated();
            onKey |= keys::hasKeyAt(*a, *t);
        }
        row.diamond->setIcon(onKey && inside ? icon("keyOn", color::accent)
                             : animated      ? icon("keyAnim", color::accent)
                                             : icon("keyOff", color::textDim));
        row.diamond->setEnabled(inside);
        row.prev->setEnabled(animated);
        row.next->setEnabled(animated);
    }
    updating_ = false;
}

}  // namespace fa::ui
