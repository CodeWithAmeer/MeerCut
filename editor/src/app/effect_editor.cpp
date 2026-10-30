#include "effect_editor.h"

#include "edit_controller.h"
#include "effect_presets.h"
#include "theme.h"
#include "title_inspector.h"

#include "fa/model/edit_ops.h"
#include "fa/model/keyframes.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QInputDialog>
#include <QLineEdit>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace fa::ui {

namespace {

QToolButton* toolButton(QWidget* parent, const char* iconName, const QString& tip, const QString& name = {}) {
    auto* b = new QToolButton(parent);
    b->setIcon(icon(iconName, color::textDim));
    b->setIconSize(QSize(14, 14));
    b->setAutoRaise(true);
    b->setToolTip(tip);
    if (!name.isEmpty()) b->setObjectName(name);
    return b;
}

QString sectionName(EffectSection s) {
    switch (s) {
        case EffectSection::Color: return "Color";
        case EffectSection::Filters: return "Filters";
        case EffectSection::Effects: return "Effects";
        default: return {};
    }
}

}  // namespace

// ---- one numeric parameter ----------------------------------------------------------------------------------

EffectParamRow::EffectParamRow(Document* doc, EditController* edit, const ParamDef& def, std::string property, double scale,
                               QWidget* parent)
    : QWidget(parent), doc_(doc), edit_(edit), def_(def), property_(std::move(property)), scale_(scale) {
    auto* l = new QHBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(4);
    sliderScale_ = std::pow(10.0, std::max(0, def.decimals));
    slider_ = new QSlider(Qt::Horizontal, this);
    slider_->setRange(static_cast<int>(std::lround(def.min * scale_ * sliderScale_)), static_cast<int>(std::lround(def.max * scale_ * sliderScale_)));
    slider_->setObjectName(QString::fromStdString("fxslider:" + property_));
    spin_ = new QDoubleSpinBox(this);
    spin_->setObjectName(QString::fromStdString("fxspin:" + property_));
    spin_->setRange(def.min * scale_, def.max * scale_);
    spin_->setDecimals(def.decimals);
    spin_->setSingleStep(def.step * scale_);
    spin_->setSuffix(QString::fromStdString(def.unit));
    spin_->setKeyboardTracking(false);
    spin_->setMinimumWidth(72);
    key_ = toolButton(this, "keyOff", "Add / remove a keyframe at the playhead", QString::fromStdString("fxkey:" + property_));
    reset_ = toolButton(this, "undo", "Reset to " + QString::number(def.def * scale_), QString::fromStdString("fxreset:" + property_));
    l->addWidget(slider_, 1);
    l->addWidget(spin_);
    l->addWidget(key_);
    l->addWidget(reset_);
    if (!def.tooltip.empty()) setToolTip(QString::fromStdString(def.tooltip));
    key_->setVisible(def.animatable);
    connect(slider_, &QSlider::valueChanged, this, [this](int v) {
        if (updating_) return;
        const double d = v / sliderScale_;
        {
            QSignalBlocker b(spin_);
            spin_->setValue(d);
        }
        commit(d);
    });
    connect(spin_, &QDoubleSpinBox::valueChanged, this, [this](double d) {
        if (updating_) return;
        {
            QSignalBlocker b(slider_);
            slider_->setValue(static_cast<int>(std::lround(d * sliderScale_)));
        }
        commit(d);
    });
    connect(key_, &QToolButton::clicked, this, [this] {
        if (clipId_.empty()) return;
        if (!doc_->playheadInside(clipId_)) {
            emit edit_->message("Move the playhead over the clip to set a keyframe");
            return;
        }
        if (!colorType_.empty()) edit_->toggleColorKey(clipId_, colorType_, colorParam_);
        else edit_->toggleKeyframe(clipId_, property_);
    });
    connect(reset_, &QToolButton::clicked, this, [this] {
        if (clipId_.empty() || property_.empty()) return;
        const std::string prop = property_;
        const double v = def_.def;
        edit_->modifyClip(clipId_, "Reset " + def_.label, [prop, v](Clip& c) {
            if (Animatable<double>* a = keys::property(c, prop)) *a = Animatable<double>(v);
        });
    });
}

void EffectParamRow::bindColorEffect(std::string effectType, std::string param) {
    colorType_ = std::move(effectType);
    colorParam_ = std::move(param);
    property_.clear();
    const QString name = QString::fromStdString(colorType_ + "/" + colorParam_);
    slider_->setObjectName("colorslider:" + name);
    spin_->setObjectName("colorspin:" + name);
    key_->setObjectName("colorkey:" + name);
    reset_->setObjectName("colorreset:" + name);
}

void EffectParamRow::setSliderVisible(bool on) { slider_->setVisible(on); }

void EffectParamRow::commit(double display) {
    if (clipId_.empty()) return;
    if (!colorType_.empty()) edit_->setColorParams(clipId_, colorType_, {{colorParam_, display / scale_}}, colorType_ + "/" + colorParam_);
    else edit_->setProperty(clipId_, property_, display / scale_);
}

void EffectParamRow::refresh(const Clip& clip, const Time& t, bool inside) {
    updating_ = true;
    if (!colorType_.empty()) {
        const EffectRef* e = EditController::colorEffect(clip, colorType_);
        property_ = e ? "fx." + e->id + "." + colorParam_ : std::string();
    }
    const Animatable<double>* a = property_.empty() ? nullptr : keys::property(clip, property_);
    const double v = (a ? a->value(t) : def_.def) * scale_;
    spin_->setValue(v);
    slider_->setValue(static_cast<int>(std::lround(v * sliderScale_)));
    const bool animated = a && a->isAnimated();
    const bool onKey = a && keys::hasKeyAt(*a, t);
    key_->setIcon(onKey && inside ? icon("keyOn", color::accent) : animated ? icon("keyAnim", color::accent) : icon("keyOff", color::textDim));
    key_->setEnabled(inside);
    reset_->setEnabled(animated || std::fabs(v - def_.def * scale_) > 1e-9);
    updating_ = false;
}

// ---- the stack -----------------------------------------------------------------------------------------------

EffectStackEditor::EffectStackEditor(Document* doc, EditController* edit, QWidget* parent)
    : QWidget(parent), doc_(doc), edit_(edit) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);
    auto* bar = new QHBoxLayout();
    add_ = new QToolButton(this);
    add_->setObjectName("addEffect");
    add_->setText("Add Effect");
    add_->setIcon(icon("plus"));
    add_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    add_->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(add_);
    for (EffectSection s : {EffectSection::Effects, EffectSection::Filters, EffectSection::Color}) {
        QMenu* sub = nullptr;
        for (const auto& d : effectDefs()) {
            if (d.section != s) continue;
            if (!sub) sub = menu->addMenu(sectionName(s));
            QAction* a = sub->addAction(QString::fromStdString(d.label).replace('&', "&&"));   // "Black & White": no mnemonic
            a->setToolTip(QString::fromStdString(d.description));
            const std::string type = d.id;
            connect(a, &QAction::triggered, this, [this, type] {
                if (!clipId_.empty()) edit_->addEffect(clipId_, type);
            });
        }
    }
    add_->setMenu(menu);
    copy_ = toolButton(this, "copy", "Copy this clip's effects", "copyEffects");
    paste_ = toolButton(this, "paste", "Paste effects onto this clip", "pasteEffects");
    connect(copy_, &QToolButton::clicked, this, [this] { edit_->copyEffects(clipId_); });
    connect(paste_, &QToolButton::clicked, this, [this] { edit_->pasteEffects(clipId_); });
    bar->addWidget(add_);
    bar->addStretch(1);
    bar->addWidget(copy_);
    bar->addWidget(paste_);
    lay->addLayout(bar);
    empty_ = new QLabel("No effects. Add one here, or drag an effect or filter from the Effects / Filters pages onto "
                        "the clip.", this);
    empty_->setWordWrap(true);
    empty_->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    lay->addWidget(empty_);
    stackLayout_ = new QVBoxLayout();
    stackLayout_->setSpacing(8);
    lay->addLayout(stackLayout_);
    lay->addStretch(1);
}

const Clip* EffectStackEditor::clip() const { return clipId_.empty() ? nullptr : edit::findClip(doc_->sequence(), clipId_); }

void EffectStackEditor::setClip(const std::string& clipId) {
    if (clipId != clipId_) signature_.clear();
    clipId_ = clipId;
    refresh();
}

void EffectStackEditor::refresh() {
    const Clip* c = clip();
    std::string sig;
    if (c)
        sig += "active=" + doc_->activeMask() + ";";   // the active effect mask shows its settings
        for (const auto& e : c->effects) {
            sig += e.id + ":" + e.type + ";";
            for (const auto& m : e.masks) sig += m.id + (m.inverted ? "i;" : ";");
            if (const auto it = e.strings.find("lut"); it != e.strings.end()) sig += it->second + ";";   // shown in the panel
        }
    if (sig != signature_ || panels_.empty() != (!c || c->effects.empty())) {
        signature_ = sig;
        rebuild();
    }
    refreshValues();
}

void EffectStackEditor::rebuild() {
    for (auto& p : panels_) delete p.frame;
    panels_.clear();
    const Clip* c = clip();
    empty_->setVisible(!c || c->effects.empty());
    copy_->setEnabled(c && !c->effects.empty());
    if (!c) return;
    for (size_t i = 0; i < c->effects.size(); ++i) {
        Panel p;
        p.effectId = c->effects[i].id;
        p.type = c->effects[i].type;
        p.frame = buildPanel(c->effects[i], i, c->effects.size(), p);
        stackLayout_->addWidget(p.frame);
        for (QWidget* w : p.frame->findChildren<QWidget*>())
            if (qobject_cast<QAbstractSpinBox*>(w) || qobject_cast<QComboBox*>(w) || qobject_cast<QSlider*>(w)) {
                w->setFocusPolicy(Qt::StrongFocus);
                w->installEventFilter(this);
            }
        panels_.push_back(std::move(p));
    }
}

bool EffectStackEditor::eventFilter(QObject* o, QEvent* e) {
    if (e->type() == QEvent::Wheel) {
        auto* w = qobject_cast<QWidget*>(o);
        if (w && !w->hasFocus()) {
            e->ignore();   // propagates to the scroll area
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

QWidget* EffectStackEditor::buildPanel(const EffectRef& e, size_t index, size_t count, Panel& p) {
    auto* frame = new QFrame(this);
    frame->setObjectName(QString::fromStdString("effectPanel:" + e.id));
    frame->setFrameShape(QFrame::StyledPanel);
    auto* v = new QVBoxLayout(frame);
    v->setContentsMargins(8, 6, 8, 8);
    v->setSpacing(4);
    const EffectDef* def = findEffectDef(e.type);
    // header: bypass, name, order, duplicate, reset, delete
    auto* head = new QHBoxLayout();
    const QString name = def ? QString::fromStdString(def->label) : QString::fromStdString(e.type + " (not available)");
    p.enabled = new QCheckBox(QString(name).replace('&', "&&"), frame);   // a literal "&", not a mnemonic
    p.enabled->setObjectName(QString::fromStdString("fxenabled:" + e.id));
    p.enabled->setToolTip("Uncheck to bypass the effect");
    QFont bold = p.enabled->font();
    bold.setBold(true);
    p.enabled->setFont(bold);
    head->addWidget(p.enabled, 1);
    const std::string id = e.id;
    auto* up = toolButton(frame, "up", "Move up (runs earlier)", QString::fromStdString("fxup:" + id));
    auto* down = toolButton(frame, "down", "Move down (runs later)", QString::fromStdString("fxdown:" + id));
    auto* dup = toolButton(frame, "copy", "Duplicate", QString::fromStdString("fxdup:" + id));
    auto* reset = toolButton(frame, "undo", "Reset all settings", QString::fromStdString("fxresetall:" + id));
    auto* save = toolButton(frame, "save", "Save these settings as a preset (Effects / Filters page, My Presets)",
                            QString::fromStdString("fxsave:" + id));
    save->setVisible(def && def->section != EffectSection::Color && edit_->effectPresets());
    auto* del = toolButton(frame, "delete", "Delete", QString::fromStdString("fxdelete:" + id));
    up->setEnabled(index > 0);
    down->setEnabled(index + 1 < count);
    for (QToolButton* b : {up, down, dup, save, reset, del}) head->addWidget(b);
    v->addLayout(head);
    connect(p.enabled, &QCheckBox::toggled, this, [this, id](bool on) {
        if (!updating_) edit_->setEffectEnabled(clipId_, id, on);
    });
    connect(up, &QToolButton::clicked, this, [this, id, index] { edit_->moveEffect(clipId_, id, static_cast<int>(index) - 1); });
    connect(down, &QToolButton::clicked, this, [this, id, index] { edit_->moveEffect(clipId_, id, static_cast<int>(index) + 1); });
    connect(dup, &QToolButton::clicked, this, [this, id] { edit_->duplicateEffect(clipId_, id); });
    connect(reset, &QToolButton::clicked, this, [this, id] { edit_->resetEffect(clipId_, id); });
    connect(save, &QToolButton::clicked, this, [this, id] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Save as preset", "Preset name:", QLineEdit::Normal, QString(), &ok);
        if (ok) savePreset(id, name);
    });
    connect(del, &QToolButton::clicked, this, [this, id] { edit_->removeEffect(clipId_, id); });
    if (!def) return frame;
    if (!def->description.empty()) frame->setToolTip(QString::fromStdString(def->description));
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);
    int row = 0;
    auto label = [&](const QString& text) {
        auto* l = new QLabel(text, frame);
        l->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
        return l;
    };
    std::string group;
    for (const ParamDef& pd : def->params) {
        if (!pd.group.empty() && pd.group != group && def->params.size() > 4) {   // group headings for long schemas
            group = pd.group;
            auto* g = new QLabel(QString::fromStdString(group), frame);
            g->setStyleSheet(QString("color: %1; font-weight: 600; padding-top: 4px;").arg(color::accent.name()));
            grid->addWidget(g, row++, 0, 1, 2);
        }
        const std::string prop = "fx." + id + "." + pd.id;
        grid->addWidget(label(QString::fromStdString(pd.label)), row, 0);
        if (pd.kind == ParamKind::Float || pd.kind == ParamKind::Int || pd.kind == ParamKind::Angle) {
            auto* r = new EffectParamRow(doc_, edit_, pd, prop, 1.0, frame);
            r->setClip(clipId_);
            p.rows.push_back(r);
            grid->addWidget(r, row++, 1);
        } else if (pd.kind == ParamKind::Bool) {
            auto* cb = new QCheckBox(frame);
            cb->setObjectName(QString::fromStdString("fxbool:" + prop));
            const std::string pid = pd.id;
            connect(cb, &QCheckBox::toggled, this, [this, id, pid](bool on) {
                if (!updating_) edit_->setEffectConstant(clipId_, id, pid, on ? 1.0 : 0.0);
            });
            p.constants.push_back({pd.id, cb});
            grid->addWidget(cb, row++, 1);
        } else if (pd.kind == ParamKind::Choice) {
            auto* combo = new QComboBox(frame);
            combo->setObjectName(QString::fromStdString("fxchoice:" + prop));
            for (const auto& ch : pd.choices) combo->addItem(QString::fromStdString(ch));
            const std::string pid = pd.id;
            const double base = pd.min;
            connect(combo, &QComboBox::currentIndexChanged, this, [this, id, pid, base](int i) {
                if (!updating_ && i >= 0) edit_->setEffectConstant(clipId_, id, pid, base + i);
            });
            p.constants.push_back({pd.id, combo});
            grid->addWidget(combo, row++, 1);
        } else if (pd.kind == ParamKind::Color) {
            auto* btn = new ColorButton(frame);
            btn->setObjectName(QString::fromStdString("fxcolor:" + prop));
            const std::string pid = pd.id;
            connect(btn, &ColorButton::colorPicked, this, [this, id, pid](uint32_t rgb) {
                if (!updating_) edit_->setEffectConstant(clipId_, id, pid, double(rgb));
            });
            p.constants.push_back({pd.id, btn});
            grid->addWidget(btn, row++, 1);
        }
    }
    if (!def->curves.empty()) {
        auto* note = label("Edit the curves in the Color tab > Curves.");
        note->setObjectName(QString::fromStdString("fxcurvesnote:" + id));
        grid->addWidget(note, row++, 0, 1, 2);
    }
    if (def->usesLut) {
        const auto it = e.strings.find("lut");
        auto* note = label("LUT: " + (it == e.strings.end() || it->second.empty() ? QString("none") : QString::fromStdString(it->second)) +
                           " - choose it in the Color tab > Basic.");
        note->setWordWrap(true);
        note->setObjectName(QString::fromStdString("fxlutnote:" + id));
        grid->addWidget(note, row++, 0, 1, 2);
    }
    // strength (mix) for every effect
    ParamDef mix;
    mix.id = "mix";
    mix.label = "Strength";
    mix.min = 0;
    mix.max = 1;
    mix.def = 1;
    mix.step = 0.01;
    mix.unit = " %";
    mix.decimals = 0;
    mix.tooltip = "How much of the effect is applied";
    grid->addWidget(label("Strength"), row, 0);
    auto* mr = new EffectParamRow(doc_, edit_, mix, "fx." + id + ".mix", 100.0, frame);
    mr->setClip(clipId_);
    p.rows.push_back(mr);
    grid->addWidget(mr, row++, 1);
    // selective effect: masks limit where it applies; the active one is drawn and edited in the player
    grid->addWidget(label("Masks"), row, 0);
    auto* maskBar = new QHBoxLayout();
    maskBar->setSpacing(2);
    auto* addRect = toolButton(frame, "maskRect", "Apply the effect only inside a rectangle", QString::fromStdString("fxmaskrect:" + id));
    auto* addEllipse = toolButton(frame, "maskEllipse", "Apply the effect only inside an ellipse", QString::fromStdString("fxmaskellipse:" + id));
    connect(addRect, &QToolButton::clicked, this, [this, id] { edit_->addEffectMask(clipId_, id, MaskShape::Rectangle); });
    connect(addEllipse, &QToolButton::clicked, this, [this, id] { edit_->addEffectMask(clipId_, id, MaskShape::Ellipse); });
    maskBar->addWidget(addRect);
    maskBar->addWidget(addEllipse);
    const std::string active = doc_->activeMask();
    const Mask* activeMk = nullptr;
    for (size_t i = 0; i < e.masks.size(); ++i) {
        const std::string ref = id + "/" + e.masks[i].id;
        auto* chip = new QToolButton(frame);
        chip->setObjectName(QString::fromStdString("fxmask:" + ref));
        chip->setText(QString("%1 %2").arg(QString::fromStdString(e.masks[i].name)).arg(i + 1));
        chip->setCheckable(true);
        chip->setChecked(ref == active);
        chip->setToolTip("Select to show and edit this mask in the player");
        if (ref == active) activeMk = &e.masks[i];
        connect(chip, &QToolButton::clicked, this, [this, ref](bool on) { doc_->setActiveMask(on ? ref : std::string()); });
        maskBar->addWidget(chip);
    }
    maskBar->addStretch(1);
    grid->addLayout(maskBar, row++, 1);
    if (activeMk) {   // the active mask's settings
        const std::string ref = id + "/" + activeMk->id, mid = activeMk->id, prefix = "fx." + id + ".mask." + mid + ".";
        auto* inv = new QCheckBox("Invert (apply outside)", frame);
        inv->setObjectName(QString::fromStdString("fxmaskinvert:" + ref));
        inv->setChecked(activeMk->inverted);
        connect(inv, &QCheckBox::toggled, this, [this, id, mid](bool on) {
            if (!updating_)
                edit_->modifyEffect(clipId_, id, "Invert mask", [mid, on](EffectRef& x) {
                    for (auto& m : x.masks)
                        if (m.id == mid) m.inverted = on;
                });
        });
        auto* delMask = toolButton(frame, "delete", "Delete this mask", QString::fromStdString("fxmaskdelete:" + ref));
        connect(delMask, &QToolButton::clicked, this, [this, id, mid] { edit_->removeEffectMask(clipId_, id, mid); });
        auto* invRow = new QHBoxLayout();
        invRow->addWidget(inv, 1);
        invRow->addWidget(delMask);
        grid->addLayout(invRow, row++, 1);
        const double seqH = doc_->sequence().height;
        ParamDef feather;
        feather.id = "feather";
        feather.label = "Feather";
        feather.min = 0;
        feather.max = 0.5;
        feather.def = 0;
        feather.step = 1.0 / seqH;
        feather.unit = " px";
        feather.decimals = 0;
        grid->addWidget(label("Feather"), row, 0);
        auto* fr = new EffectParamRow(doc_, edit_, feather, prefix + "feather", seqH, frame);
        fr->setClip(clipId_);
        p.rows.push_back(fr);
        grid->addWidget(fr, row++, 1);
        ParamDef opacity;
        opacity.id = "opacity";
        opacity.label = "Opacity";
        opacity.min = 0;
        opacity.max = 1;
        opacity.def = 1;
        opacity.step = 0.01;
        opacity.unit = " %";
        opacity.decimals = 0;
        grid->addWidget(label("Opacity"), row, 0);
        auto* orow = new EffectParamRow(doc_, edit_, opacity, prefix + "opacity", 100.0, frame);
        orow->setClip(clipId_);
        p.rows.push_back(orow);
        grid->addWidget(orow, row++, 1);
    }
    grid->setColumnStretch(1, 1);
    v->addLayout(grid);
    return frame;
}

bool EffectStackEditor::savePreset(const std::string& effectId, const QString& name) {
    EffectPresetLibrary* lib = edit_->effectPresets();
    const Clip* c = clip();
    const auto t = doc_->clipTimeAtPlayhead(clipId_);
    if (!lib || !c || !t) return false;
    for (const auto& e : c->effects)
        if (e.id == effectId) {
            if (lib->add(name, e, *t).empty()) {
                emit edit_->message("The preset could not be saved: " + lib->lastError());
                return false;
            }
            emit edit_->info("Saved preset \"" + name.trimmed() + "\" (My Presets)");
            return true;
        }
    return false;
}

void EffectStackEditor::refreshValues() {
    const Clip* c = clip();
    paste_->setEnabled(c && edit_->canPasteEffects());
    if (!c) return;
    const auto t = doc_->clipTimeAtPlayhead(clipId_);
    if (!t) return;
    const bool inside = doc_->playheadInside(clipId_);
    updating_ = true;
    for (auto& p : panels_) {
        const EffectRef* e = nullptr;
        for (const auto& x : c->effects)
            if (x.id == p.effectId) e = &x;
        if (!e) continue;
        p.enabled->setChecked(e->enabled);
        for (auto* r : p.rows) {
            r->setClip(clipId_);
            r->refresh(*c, *t, inside);
        }
        for (auto& [pid, w] : p.constants) {
            const auto it = e->params.find(pid);
            const double v = it == e->params.end() ? 0.0 : it->second.value(*t);
            if (auto* cb = qobject_cast<QCheckBox*>(w)) cb->setChecked(v != 0.0);
            else if (auto* combo = qobject_cast<QComboBox*>(w)) {
                const EffectDef* def = findEffectDef(e->type);
                const int i = def ? def->paramIndex(pid) : -1;
                combo->setCurrentIndex(static_cast<int>(std::lround(v - (i >= 0 ? def->params[static_cast<size_t>(i)].min : 0))));
            } else if (auto* btn = qobject_cast<ColorButton*>(w)) {
                btn->setColor(static_cast<uint32_t>(v));
            }
        }
    }
    updating_ = false;
}

}  // namespace fa::ui
