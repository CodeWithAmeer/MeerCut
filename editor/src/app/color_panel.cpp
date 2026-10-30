#include "color_panel.h"

#include "color_presets.h"
#include "edit_controller.h"
#include "effect_editor.h"
#include "lut_library.h"
#include "theme.h"

#include "fa/model/edit_ops.h"
#include "fa/render/curves.h"
#include "fa/render/lut.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>

#include <QAbstractSpinBox>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSlider>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fa::ui {

namespace {

constexpr double kSqrt3 = 1.7320508075688772;
const char* kBands[8] = {"red", "orange", "yellow", "green", "aqua", "blue", "purple", "magenta"};
const char* kBandLabels[8] = {"Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"};
const char* kWheels[3] = {"shadows", "midtones", "highlights"};
const char* kWheelLabels[3] = {"Shadows", "Midtones", "Highlights"};

QColor channelColor(const std::string& ch) {
    if (ch == "red") return QColor(0xE5, 0x48, 0x4D);
    if (ch == "green") return QColor(0x46, 0xA7, 0x58);
    if (ch == "blue") return QColor(0x3E, 0x7B, 0xFA);
    if (ch == "master") return color::text;
    return color::accent;
}

bool isIdentityTone(const std::vector<Vec2>& p) {
    const auto n = normalizeCurve(p);
    return n.empty() || (n.size() == 2 && n[0].x == 0 && n[0].y == 0 && n[1].x == 1 && n[1].y == 1);
}

bool isNeutralHue(const std::vector<Vec2>& p) {
    return std::all_of(p.begin(), p.end(), [](const Vec2& q) { return std::fabs(q.y - 0.5) < 1e-9; });
}

// Tone curve presets: plain mathematical shapes (no third-party looks).
struct CurvePreset {
    const char* name;
    std::vector<Vec2> points;
};
const std::vector<CurvePreset>& curvePresets() {
    static const std::vector<CurvePreset> v = {
        {"Linear", {{0, 0}, {1, 1}}},
        {"Medium Contrast", {{0, 0}, {0.25, 0.21}, {0.75, 0.79}, {1, 1}}},
        {"Strong Contrast", {{0, 0}, {0.25, 0.15}, {0.75, 0.85}, {1, 1}}},
        {"Lighten", {{0, 0}, {0.5, 0.62}, {1, 1}}},
        {"Darken", {{0, 0}, {0.5, 0.38}, {1, 1}}},
        {"Matte (lifted blacks)", {{0, 0.08}, {0.25, 0.27}, {0.75, 0.76}, {1, 0.94}}},
        {"Negative", {{0, 1}, {1, 0}}},
    };
    return v;
}

QLabel* dim(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    l->setWordWrap(true);
    return l;
}

QLabel* groupLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet(QString("color: %1; font-weight: 600; padding-top: 4px;").arg(color::accent.name()));
    return l;
}

QToolButton* smallButton(QWidget* parent, const char* iconName, const QString& tip, const QString& name) {
    auto* b = new QToolButton(parent);
    b->setIcon(icon(iconName, color::textDim));
    b->setIconSize(QSize(14, 14));
    b->setAutoRaise(true);
    b->setToolTip(tip);
    b->setObjectName(name);
    return b;
}

QToolButton* textButton(QWidget* parent, const QString& text, const QString& name, bool checkable = false) {
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setObjectName(name);
    b->setCheckable(checkable);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    return b;
}

QWidget* scrolledPage(QWidget* content) {
    auto* s = new QScrollArea();
    s->setWidgetResizable(true);
    s->setFrameShape(QFrame::NoFrame);
    s->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    s->setWidget(content);
    return s;
}

QIcon swatchIcon(const QColor& c, bool edited) {
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(edited ? QPen(Qt::white, 2) : Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(QRectF(3, 3, 16, 16));
    return QIcon(pm);
}

}  // namespace

// ---- curve editor ---------------------------------------------------------------------------------------------

CurveEditor::CurveEditor(bool periodic, QWidget* parent) : QWidget(parent), periodic_(periodic) {
    setFocusPolicy(Qt::ClickFocus);
    setMinimumSize(160, 140);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setToolTip(periodic ? "Hue curve: click to add a point, drag up or down to change the hues under it, double-click "
                          "or Delete to remove"
                        : "Click on the curve to add a point, drag to move it, double-click or Delete to remove it");
    if (!periodic) points_ = {{0, 0}, {1, 1}};
}

int CurveEditor::heightForWidth(int w) const { return std::clamp(periodic_ ? w * 9 / 16 : w * 4 / 5, 140, 320); }

void CurveEditor::setCurve(std::vector<Vec2> points, const QColor& color) {
    color_ = color;
    if (dragging_) return;
    points_ = normalizeCurve(std::move(points));
    if (selected_ >= static_cast<int>(points_.size())) selected_ = -1;
    update();
}

void CurveEditor::setGhosts(std::vector<std::pair<std::vector<Vec2>, QColor>> curves) {
    ghosts_ = std::move(curves);
    update();
}

QRectF CurveEditor::plot() const {
    const double strip = periodic_ ? 10.0 : 0.0;
    return QRectF(8, 8, width() - 16, height() - 16 - strip);
}

QPointF CurveEditor::toWidget(const Vec2& v) const {
    const QRectF r = plot();
    return {r.left() + v.x * r.width(), r.top() + (1.0 - v.y) * r.height()};
}

Vec2 CurveEditor::fromWidget(const QPointF& p) const {
    const QRectF r = plot();
    return {(p.x() - r.left()) / r.width(), 1.0 - (p.y() - r.top()) / r.height()};
}

int CurveEditor::hit(const QPointF& p) const {
    int best = -1;
    double bestD = 8.0;
    for (size_t i = 0; i < points_.size(); ++i) {
        const QPointF q = toWidget(points_[i]);
        const double d = std::hypot(q.x() - p.x(), q.y() - p.y());
        if (d <= bestD) {
            bestD = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void CurveEditor::moveSelected(const QPointF& p) {
    if (selected_ < 0) return;
    const size_t i = static_cast<size_t>(selected_);
    const Vec2 v = fromWidget(p);
    const double lo = i > 0 ? points_[i - 1].x + 0.01 : 0.0;
    const double hi = i + 1 < points_.size() ? points_[i + 1].x - 0.01 : 1.0;
    points_[i].x = std::clamp(v.x, lo, std::max(lo, hi));
    points_[i].y = std::clamp(v.y, 0.0, 1.0);
}

void CurveEditor::removeSelected() {
    if (selected_ < 0) return;
    const int n = static_cast<int>(points_.size());
    if (!periodic_ && (selected_ == 0 || selected_ == n - 1)) return;   // tone curves keep their end points
    points_.erase(points_.begin() + selected_);
    selected_ = -1;
    emit edited(points_, ++serial_);
    update();
}

void CurveEditor::mousePressEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    const int h = hit(pos);
    if (e->button() == Qt::RightButton) {
        if (h >= 0) {
            selected_ = h;
            removeSelected();
        }
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    ++serial_;
    if (h >= 0) {
        selected_ = h;
        dragging_ = true;
        update();
        return;
    }
    const Vec2 v = fromWidget(pos);
    if (v.x < 0 || v.x > 1 || v.y < -0.05 || v.y > 1.05) return;
    if (static_cast<int>(points_.size()) >= kMaxPoints) return;
    for (const auto& q : points_)
        if (std::fabs(q.x - v.x) < 0.01) return;   // too close to an existing point
    const Vec2 add{v.x, evaluateCurve(points_, v.x, periodic_)};
    const auto it = std::lower_bound(points_.begin(), points_.end(), add, [](const Vec2& a, const Vec2& b) { return a.x < b.x; });
    selected_ = static_cast<int>(points_.insert(it, add) - points_.begin());
    dragging_ = true;
    emit edited(points_, serial_);
    update();
}

void CurveEditor::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_) return;
    moveSelected(e->position());
    emit edited(points_, serial_);
    update();
}

void CurveEditor::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; }

void CurveEditor::mouseDoubleClickEvent(QMouseEvent* e) {
    const int h = hit(e->position());
    if (h < 0) return;
    dragging_ = false;
    selected_ = h;
    removeSelected();
}

void CurveEditor::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) removeSelected();
    else QWidget::keyPressEvent(e);
}

void CurveEditor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), color::track);
    const QRectF r = plot();
    p.setPen(QPen(color::border, 1));
    for (int i = 0; i <= 4; ++i) {
        const double x = r.left() + r.width() * i / 4.0, y = r.top() + r.height() * i / 4.0;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
    p.setPen(QPen(color::textDim, 1, Qt::DashLine));
    if (periodic_) {
        p.drawLine(toWidget({0, 0.5}), toWidget({1, 0.5}));
        QLinearGradient g(r.left(), 0, r.right(), 0);
        for (int i = 0; i <= 12; ++i) g.setColorAt(i / 12.0, QColor::fromHsvF(i == 12 ? 0.0f : float(i) / 12.0f, 0.8f, 0.9f));
        p.fillRect(QRectF(r.left(), r.bottom() + 4, r.width(), 6), g);
    } else {
        p.drawLine(toWidget({0, 0}), toWidget({1, 1}));
    }
    auto path = [&](const std::vector<Vec2>& pts) {
        QPainterPath pp;
        const int n = std::max(2, static_cast<int>(r.width()));
        for (int i = 0; i <= n; ++i) {
            const double x = double(i) / n;
            const QPointF q = toWidget({x, evaluateCurve(pts, x, periodic_)});
            if (i == 0) pp.moveTo(q);
            else pp.lineTo(q);
        }
        return pp;
    };
    for (const auto& [pts, c] : ghosts_) {
        QColor g = c;
        g.setAlpha(110);
        p.setPen(QPen(g, 1.2));
        p.drawPath(path(pts));
    }
    p.setPen(QPen(color_.isValid() ? color_ : color::text, 2));
    p.drawPath(path(points_));
    for (size_t i = 0; i < points_.size(); ++i) {
        const QPointF q = toWidget(points_[i]);
        p.setPen(QPen(color_.isValid() ? color_ : color::text, 1.5));
        p.setBrush(static_cast<int>(i) == selected_ ? QBrush(color::accent) : QBrush(color::window));
        p.drawEllipse(q, 4.5, 4.5);
    }
}

// ---- colour wheel ---------------------------------------------------------------------------------------------

ColorWheel::ColorWheel(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(116, 116);
    setToolTip("Drag to tint this tonal range; double-click to reset the tint");
}

QPointF ColorWheel::chromaOf(double r, double g, double b) { return {r - (g + b) / 2.0, kSqrt3 / 2.0 * (g - b)}; }

void ColorWheel::offsetsOf(QPointF c, double mean, double& r, double& g, double& b) {
    r = std::clamp(mean + 2.0 * c.x() / 3.0, -1.0, 1.0);
    g = std::clamp(mean - c.x() / 3.0 + c.y() / kSqrt3, -1.0, 1.0);
    b = std::clamp(mean - c.x() / 3.0 - c.y() / kSqrt3, -1.0, 1.0);
}

void ColorWheel::setOffsets(double r, double g, double b) {
    if (dragging_) return;
    chroma_ = chromaOf(r, g, b);
    update();
}

QPointF ColorWheel::centre() const { return {width() / 2.0, height() / 2.0}; }
double ColorWheel::radius() const { return std::min(width(), height()) / 2.0 - 6.0; }

QPointF ColorWheel::puckPosition() const {
    QPointF c = chroma_;
    const double len = std::hypot(c.x(), c.y());
    if (len > 1.0) c /= len;
    return centre() + QPointF(c.x() * radius(), -c.y() * radius());
}

QPointF ColorWheel::fromWidget(const QPointF& p) const {
    QPointF c((p.x() - centre().x()) / radius(), -(p.y() - centre().y()) / radius());
    const double len = std::hypot(c.x(), c.y());
    if (len > 1.0) c /= len;
    return c;
}

void ColorWheel::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    dragging_ = true;
    chroma_ = fromWidget(e->position());
    emit tintEdited(chroma_, ++serial_);
    update();
}

void ColorWheel::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_) return;
    chroma_ = fromWidget(e->position());
    emit tintEdited(chroma_, serial_);
    update();
}

void ColorWheel::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; }

void ColorWheel::mouseDoubleClickEvent(QMouseEvent*) {
    dragging_ = false;
    chroma_ = {};
    emit tintEdited(chroma_, ++serial_);
    update();
}

void ColorWheel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF c = centre();
    const double R = radius();
    QConicalGradient hue(c, 0);   // counter-clockwise from 3 o'clock: red 0, green 120, blue 240 deg
    for (int i = 0; i <= 12; ++i) hue.setColorAt(i / 12.0, QColor::fromHsvF(i == 12 ? 0.0f : float(i) / 12.0f, 0.75f, 0.85f));
    p.setPen(Qt::NoPen);
    p.setBrush(hue);
    p.drawEllipse(c, R, R);
    QRadialGradient fade(c, R);
    fade.setColorAt(0.0, QColor(0x6E, 0x6E, 0x6E, 255));
    fade.setColorAt(1.0, QColor(0x6E, 0x6E, 0x6E, 0));
    p.setBrush(fade);
    p.drawEllipse(c, R, R);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(color::border, 1.5));
    p.drawEllipse(c, R, R);
    p.setPen(QPen(QColor(255, 255, 255, 90), 1));
    p.drawLine(c - QPointF(5, 0), c + QPointF(5, 0));
    p.drawLine(c - QPointF(0, 5), c + QPointF(0, 5));
    const QPointF q = puckPosition();
    p.setPen(QPen(Qt::white, 2));
    p.setBrush(QColor(0x20, 0x20, 0x20, 160));
    p.drawEllipse(q, 6, 6);
}

// ---- the panel ------------------------------------------------------------------------------------------------

ColorPanel::ColorPanel(Document* doc, EditController* edit, QWidget* parent) : QWidget(parent), doc_(doc), edit_(edit) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(buildPresetBar());
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName("colorSubTabs");
    tabs_->setDocumentMode(true);
    tabs_->addTab(scrolledPage(buildBasic()), "Basic");
    tabs_->addTab(scrolledPage(buildHsl()), "HSL");
    tabs_->addTab(scrolledPage(buildCurves()), "Curves");
    tabs_->addTab(scrolledPage(buildWheels()), "Wheels");
    lay->addWidget(tabs_, 1);
    for (QWidget* w : findChildren<QWidget*>())
        if (qobject_cast<QAbstractSpinBox*>(w) || qobject_cast<QComboBox*>(w) || qobject_cast<QSlider*>(w)) {
            w->setFocusPolicy(Qt::StrongFocus);
            w->installEventFilter(this);
        }
    if (ColorPresetLibrary* lib = edit_->colorPresets()) connect(lib, &ColorPresetLibrary::changed, this, &ColorPanel::refreshPresets);
    refreshPresets();
}

bool ColorPanel::eventFilter(QObject* o, QEvent* e) {
    if (e->type() == QEvent::Wheel) {
        auto* w = qobject_cast<QWidget*>(o);
        if (w && !w->hasFocus()) {
            e->ignore();
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

const Clip* ColorPanel::clip() const { return clipId_.empty() ? nullptr : edit::findClip(doc_->sequence(), clipId_); }

void ColorPanel::setClip(const std::string& clipId) {
    clipId_ = clipId;
    refresh();
}

QWidget* ColorPanel::header(QWidget* parent, const std::string& type, const QString& title) {
    auto* w = new QWidget(parent);
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 6, 0, 0);
    Header hd;
    hd.type = type;
    hd.enabled = new QCheckBox(title, w);
    hd.enabled->setObjectName(QString::fromStdString("colorenabled:" + type));
    QFont bold = hd.enabled->font();
    bold.setBold(true);
    hd.enabled->setFont(bold);
    hd.reset = smallButton(w, "undo", "Reset " + title, QString::fromStdString("colorresetall:" + type));
    h->addWidget(hd.enabled, 1);
    h->addWidget(hd.reset);
    connect(hd.enabled, &QCheckBox::toggled, this, [this, type](bool on) {
        if (updating_) return;
        const Clip* c = clip();
        if (const EffectRef* e = c ? EditController::colorEffect(*c, type) : nullptr) edit_->setEffectEnabled(clipId_, e->id, on);
    });
    connect(hd.reset, &QToolButton::clicked, this, [this, type, title] {
        const Clip* c = clip();
        const EffectRef* e = c ? EditController::colorEffect(*c, type) : nullptr;
        if (!e) return;
        edit_->modifyEffect(clipId_, e->id, "Reset " + title.toStdString(), [](EffectRef& x) {
            if (const EffectDef* def = findEffectDef(x.type)) {
                EffectRef fresh = makeEffect(*def, x.id, true);
                fresh.enabled = x.enabled;
                fresh.masks = x.masks;
                fresh.strings = x.strings;
                x = fresh;
            }
        });
    });
    headers_.push_back(hd);
    return w;
}

EffectParamRow* ColorPanel::row(QWidget* parent, const std::string& type, const std::string& param) {
    const EffectDef* def = findEffectDef(type);
    const int i = def ? def->paramIndex(param) : -1;
    if (i < 0) return nullptr;
    ParamDef pd = def->params[static_cast<size_t>(i)];
    pd.def = def->neutralValue(param);   // an untouched control means "no change"
    auto* r = new EffectParamRow(doc_, edit_, pd, std::string(), 1.0, parent);
    r->bindColorEffect(type, param);
    rows_.push_back(r);
    return r;
}

QWidget* ColorPanel::buildPresetBar() {
    auto* w = new QWidget(this);
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(10, 8, 10, 4);
    h->setSpacing(4);
    presetCombo_ = new QComboBox(w);
    presetCombo_->setObjectName("colorPresetCombo");
    presetCombo_->setToolTip("Your saved colour settings");
    applyPreset_ = textButton(w, "Apply", "applyColorPreset");
    applyPreset_->setToolTip("Replace this clip's Color settings with the preset");
    deletePreset_ = smallButton(w, "delete", "Delete the preset", "deleteColorPreset");
    savePreset_ = textButton(w, "Save as Custom", "saveColorPreset");
    savePreset_->setToolTip("Save this clip's Color settings (values at the playhead) as a preset");
    h->addWidget(presetCombo_, 1);
    h->addWidget(applyPreset_);
    h->addWidget(deletePreset_);
    h->addWidget(savePreset_);
    connect(presetCombo_, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(applyPreset_, &QToolButton::clicked, this, [this] { applyPreset(presetCombo_->currentData().toString().toStdString()); });
    connect(deletePreset_, &QToolButton::clicked, this, [this] {
        if (ColorPresetLibrary* lib = edit_->colorPresets()) lib->remove(presetCombo_->currentData().toString().toStdString());
    });
    connect(savePreset_, &QToolButton::clicked, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Save as Custom", "Preset name:", QLineEdit::Normal, "My Color", &ok);
        if (ok) saveCustomPreset(name);
    });
    return w;
}

void ColorPanel::refreshPresets() {
    const QString keep = presetCombo_->currentData().toString();
    QSignalBlocker b(presetCombo_);
    presetCombo_->clear();
    ColorPresetLibrary* lib = edit_->colorPresets();
    if (!lib || lib->presets().empty()) {
        presetCombo_->addItem(lib ? "No custom presets yet" : "Presets unavailable", QString());
    } else {
        for (const auto& p : lib->presets()) presetCombo_->addItem(QString::fromStdString(p.name), QString::fromStdString(p.id));
        const int i = presetCombo_->findData(keep);
        presetCombo_->setCurrentIndex(i >= 0 ? i : presetCombo_->count() - 1);
    }
    refresh();
}

bool ColorPanel::saveCustomPreset(const QString& name) {
    ColorPresetLibrary* lib = edit_->colorPresets();
    const Clip* c = clip();
    const auto t = doc_->clipTimeAtPlayhead(clipId_);
    if (!lib || !c || !t) return false;
    const std::string id = lib->add(name, *c, *t);
    if (id.empty()) {
        emit edit_->message("The colour preset could not be saved: " + lib->lastError());
        return false;
    }
    presetCombo_->setCurrentIndex(presetCombo_->findData(QString::fromStdString(id)));
    emit edit_->info("Saved colour preset \"" + name.trimmed() + "\"");
    return true;
}

bool ColorPanel::applyPreset(const std::string& presetId) {
    ColorPresetLibrary* lib = edit_->colorPresets();
    const ColorPreset* p = lib ? lib->find(presetId) : nullptr;
    if (!p || clipId_.empty()) return false;
    return edit_->applyColorEffects(clipId_, p->effects, "Apply color preset \"" + p->name + "\"");
}

QWidget* ColorPanel::buildBasic() {
    auto* w = new QWidget();
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(10, 4, 10, 10);
    v->setSpacing(4);
    auto section = [&](const std::string& type, const QString& title) {
        v->addWidget(header(w, type, title));
        const EffectDef* def = findEffectDef(type);
        auto* grid = new QGridLayout();
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(3);
        int r = 0;
        std::string group;
        for (const ParamDef& pd : def->params) {
            if (type == "fa.color.basic" && pd.group != group) {
                group = pd.group;
                grid->addWidget(groupLabel(QString::fromStdString(group), w), r++, 0, 1, 2);
            }
            grid->addWidget(dim(QString::fromStdString(pd.label), w), r, 0);
            grid->addWidget(row(w, type, pd.id), r++, 1);
        }
        grid->setColumnStretch(1, 1);
        v->addLayout(grid);
    };
    v->addWidget(buildLutSection(w));
    section("fa.color.basic", "Color Adjust");
    section("fa.fx.sharpen", "Sharpen");
    section("fa.fx.vignette", "Vignette");
    v->addStretch(1);
    return w;
}

QWidget* ColorPanel::buildLutSection(QWidget* parent) {
    auto* box = new QWidget(parent);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(3);
    v->addWidget(header(box, "fa.color.lut", "LUT"));
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(3);
    lutCombo_ = new QComboBox(box);
    lutCombo_->setObjectName("lutCombo");
    lutCombo_->setIconSize(QSize(48, 27));
    lutCombo_->setToolTip("Built-in looks and your imported .cube files (thumbnails show each on a sample picture)");
    auto* import = new QToolButton(box);
    import->setObjectName("importLut");
    import->setText("Import .cube...");
    import->setToolTip("Add a 3-D / 1-D .cube LUT to your library and apply it");
    auto* pick = new QHBoxLayout();
    pick->addWidget(lutCombo_, 1);
    pick->addWidget(import);
    grid->addWidget(dim("LUT", box), 0, 0);
    grid->addLayout(pick, 0, 1);
    grid->addWidget(dim("Strength", box), 1, 0);
    grid->addWidget(row(box, "fa.color.lut", "strength"), 1, 1);
    lutDomain_ = new QComboBox(box);
    lutDomain_->setObjectName("lutDomain");
    const EffectDef* def = findEffectDef("fa.color.lut");
    const ParamDef& dp = def->params[static_cast<size_t>(def->paramIndex("domain"))];
    for (const auto& ch : dp.choices) lutDomain_->addItem(QString::fromStdString(ch));
    lutDomain_->setToolTip(QString::fromStdString(dp.tooltip));
    grid->addWidget(dim("Domain", box), 2, 0);
    grid->addWidget(lutDomain_, 2, 1);
    grid->setColumnStretch(1, 1);
    v->addLayout(grid);
    auto* status = new QHBoxLayout();
    lutStatus_ = new QLabel(box);
    lutStatus_->setObjectName("lutStatus");
    lutStatus_->setWordWrap(true);
    lutRelink_ = new QToolButton(box);
    lutRelink_->setObjectName("relinkLut");
    lutRelink_->setText("Relink...");
    lutRelink_->setToolTip("Choose the .cube file again (every clip using it is updated)");
    status->addWidget(lutStatus_, 1);
    status->addWidget(lutRelink_);
    v->addLayout(status);
    connect(lutCombo_, &QComboBox::activated, this, [this](int i) {
        if (updating_ || i < 0 || clipId_.empty()) return;
        const std::string ref = lutCombo_->itemData(i).toString().toStdString();
        const Clip* c = clip();
        if (ref.empty() && (!c || !EditController::colorEffect(*c, "fa.color.lut"))) return;   // nothing to remove
        edit_->setColorLut(clipId_, ref);
    });
    connect(import, &QToolButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, "Import LUT", {}, "Cube LUT (*.cube *.CUBE)");
        if (!f.isEmpty()) importLut(f);
    });
    connect(lutRelink_, &QToolButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, "Relink LUT", {}, "Cube LUT (*.cube *.CUBE)");
        if (!f.isEmpty()) relinkMissingLut(f);
    });
    connect(lutDomain_, &QComboBox::activated, this, [this](int i) {
        if (!updating_ && i >= 0) edit_->setColorParams(clipId_, "fa.color.lut", {{"domain", double(i)}});
    });
    if (LutLibrary* lib = edit_->lutLibrary()) connect(lib, &LutLibrary::changed, this, [this] {
            lutComboKey_.clear();
            refresh();
        });
    return box;
}

bool ColorPanel::importLut(const QString& path) {
    LutLibrary* lib = edit_->lutLibrary();
    if (!lib || clipId_.empty()) return false;
    QString err;
    const std::string ref = lib->addFile(path, &err);
    if (ref.empty()) {
        emit edit_->message("The LUT could not be imported: " + err);
        return false;
    }
    return edit_->setColorLut(clipId_, ref);
}

bool ColorPanel::relinkMissingLut(const QString& path) {
    const Clip* c = clip();
    const EffectRef* e = c ? EditController::colorEffect(*c, "fa.color.lut") : nullptr;
    const auto it = e ? e->strings.find("lut") : std::map<std::string, std::string>::const_iterator();
    if (!e || it == e->strings.end() || it->second.empty()) return false;
    std::string err;
    const std::string abs = QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toStdString();
    if (!loadCube(std::filesystem::path(std::u8string(abs.begin(), abs.end())), &err)) {
        emit edit_->message("That file is not a usable LUT: " + QString::fromStdString(err));
        return false;
    }
    if (LutLibrary* lib = edit_->lutLibrary()) lib->addFile(path, nullptr);
    const std::string old = it->second;   // copied: the edit replaces the clip
    return edit_->relinkLut(old, abs) > 0;
}

void ColorPanel::refreshLut(const Clip* c) {
    LutLibrary* lib = edit_->lutLibrary();
    const EffectRef* e = c ? EditController::colorEffect(*c, "fa.color.lut") : nullptr;
    std::string ref;
    if (e)
        if (const auto it = e->strings.find("lut"); it != e->strings.end()) ref = it->second;
    // chooser: None, built-in looks, imported files, and the clip's own reference when it is not in the library
    std::vector<LutLibrary::Entry> entries = lib ? lib->entries() : std::vector<LutLibrary::Entry>{};
    const bool listed = ref.empty() || std::any_of(entries.begin(), entries.end(), [&](const auto& x) { return x.reference == ref; });
    QString key = QString::number(entries.size()) + "|" + (listed ? QString() : QString::fromStdString(ref));
    for (const auto& x : entries) key += QString::fromStdString(x.reference) + ";";
    if (key != lutComboKey_) {
        lutComboKey_ = key;
        lutCombo_->clear();
        lutCombo_->addItem("None", QString());
        bool sep = false;
        for (const auto& x : entries) {
            if (!x.builtin && !sep) {
                lutCombo_->insertSeparator(lutCombo_->count());
                sep = true;
            }
            const QImage t = lib->thumbnail(x.reference);
            lutCombo_->addItem(t.isNull() ? QIcon() : QIcon(QPixmap::fromImage(t)), x.name, QString::fromStdString(x.reference));
            lutCombo_->setItemData(lutCombo_->count() - 1, x.description, Qt::ToolTipRole);
        }
        if (!listed) {
            lutCombo_->insertSeparator(lutCombo_->count());
            lutCombo_->addItem((lib ? lib->nameOf(ref) : QString::fromStdString(ref)) + " (this project)", QString::fromStdString(ref));
        }
    }
    lutCombo_->setCurrentIndex(std::max(0, lutCombo_->findData(QString::fromStdString(ref))));
    const EffectDef* def = findEffectDef("fa.color.lut");
    lutDomain_->setCurrentIndex(e ? static_cast<int>(std::lround(effectParam(*e, *def, "domain", c->sourceIn))) : 0);
    lutDomain_->setEnabled(e != nullptr);
    // status: what the table is, or why it cannot be used
    bool missing = false;
    QString text;
    if (!ref.empty()) {
        std::string err;
        if (const auto lut = resolveLut(ref, edit_->assetDir(), &err)) {
            QStringList parts;
            if (lut->size1d) parts << QString("1-D %1").arg(lut->size1d);
            if (lut->size) parts << QString("%1³ tetrahedral").arg(lut->size);
            text = (lut->title.empty() ? QString() : QString::fromStdString(lut->title) + " · ") + parts.join(" + ");
        } else {
            missing = true;
            text = "Not applied: " + QString::fromStdString(err) + " (" + QString::fromStdString(ref) + ")";
        }
    }
    lutStatus_->setText(text);
    lutStatus_->setStyleSheet(missing ? "color: #ff6b6b;" : QString("color: %1;").arg(color::textDim.name()));
    lutRelink_->setVisible(missing);
}

QWidget* ColorPanel::buildHsl() {
    auto* w = new QWidget();
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(10, 4, 10, 10);
    v->setSpacing(6);
    v->addWidget(header(w, "fa.color.hsl", "HSL"));
    auto* sw = new QHBoxLayout();
    sw->setSpacing(2);
    for (int i = 0; i < 8; ++i) {
        auto* b = new QToolButton(w);
        b->setObjectName(QString("hslband:") + kBands[i]);
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setAutoRaise(true);
        b->setIconSize(QSize(22, 22));
        b->setToolTip(kBandLabels[i]);
        b->setChecked(i == 0);
        connect(b, &QToolButton::clicked, this, [this, i] { selectHslBand(i); });
        swatches_[static_cast<size_t>(i)] = b;
        sw->addWidget(b);
    }
    sw->addStretch(1);
    v->addLayout(sw);
    bandLabel_ = groupLabel("Red", w);
    v->addWidget(bandLabel_);
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    const char* labels[3] = {"Hue", "Saturation", "Luminance"};
    const char* suffix[3] = {".hue", ".saturation", ".luminance"};
    for (int k = 0; k < 3; ++k) {
        grid->addWidget(dim(labels[k], w), k, 0);
        hslRows_[static_cast<size_t>(k)] = row(w, "fa.color.hsl", std::string("red") + suffix[k]);
        grid->addWidget(hslRows_[static_cast<size_t>(k)], k, 1);
    }
    grid->setColumnStretch(1, 1);
    v->addLayout(grid);
    v->addWidget(dim("Each range blends smoothly into its neighbours; neutral greys are never changed.", w));
    v->addStretch(1);
    return w;
}

void ColorPanel::selectHslBand(int band) {
    if (band < 0 || band >= 8) return;
    band_ = band;
    const char* suffix[3] = {".hue", ".saturation", ".luminance"};
    for (int k = 0; k < 3; ++k) hslRows_[static_cast<size_t>(k)]->bindColorEffect("fa.color.hsl", std::string(kBands[band]) + suffix[k]);
    swatches_[static_cast<size_t>(band)]->setChecked(true);
    bandLabel_->setText(kBandLabels[band]);
    refresh();
}

QWidget* ColorPanel::buildCurves() {
    auto* w = new QWidget();
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(10, 4, 10, 10);
    v->setSpacing(6);
    v->addWidget(header(w, "fa.color.curves", "Curves"));
    auto channelRow = [&](const std::vector<std::pair<std::string, QString>>& chans) {
        auto* h = new QHBoxLayout();
        h->setSpacing(2);
        auto* group = new QButtonGroup(w);   // tone and hue channels are separate choices
        for (const auto& [id, label] : chans) {
            auto* b = textButton(w, label, QString::fromStdString("curvechannel:" + id), true);
            group->addButton(b);
            const std::string ch = id;
            connect(b, &QToolButton::clicked, this, [this, ch] {
                if (ch == "hueHue" || ch == "hueSat") selectHueChannel(ch);
                else selectToneChannel(ch);
            });
            channelButtons_.push_back(b);
            h->addWidget(b);
        }
        h->addStretch(1);
        return h;
    };
    v->addLayout(channelRow({{"master", "Master"}, {"red", "Red"}, {"green", "Green"}, {"blue", "Blue"}}));
    tone_ = new CurveEditor(false, w);
    tone_->setObjectName("toneCurveEditor");
    v->addWidget(tone_);
    auto* tools = new QHBoxLayout();
    curvePreset_ = new QComboBox(w);
    curvePreset_->setObjectName("curvePreset");
    curvePreset_->addItem("Curve preset...");
    for (const auto& p : curvePresets()) curvePreset_->addItem(p.name);
    curvePreset_->setToolTip("Apply a curve shape to the selected channel");
    auto* resetTone = smallButton(w, "undo", "Reset the selected channel", "curveResetChannel");
    tools->addWidget(curvePreset_, 1);
    tools->addWidget(resetTone);
    v->addLayout(tools);
    connect(curvePreset_, &QComboBox::activated, this, [this](int i) {
        if (i <= 0) return;
        edit_->setColorCurve(clipId_, toneChannel_, curvePresets()[static_cast<size_t>(i - 1)].points);
        QSignalBlocker b(curvePreset_);
        curvePreset_->setCurrentIndex(0);
    });
    connect(resetTone, &QToolButton::clicked, this, [this] { edit_->setColorCurve(clipId_, toneChannel_, {{0, 0}, {1, 1}}); });
    connect(tone_, &CurveEditor::edited, this, [this](const std::vector<Vec2>& pts, int serial) {
        edit_->setColorCurve(clipId_, toneChannel_, pts, "curve/" + toneChannel_ + "#" + std::to_string(serial));
    });
    v->addWidget(groupLabel("Hue Curves", w));
    v->addLayout(channelRow({{"hueHue", "Hue vs Hue"}, {"hueSat", "Hue vs Saturation"}}));
    hue_ = new CurveEditor(true, w);
    hue_->setObjectName("hueCurveEditor");
    v->addWidget(hue_);
    auto* resetHue = smallButton(w, "undo", "Reset the selected hue curve", "curveResetHue");
    auto* hueTools = new QHBoxLayout();
    hueTools->addWidget(dim("Up shifts the hue (vs Hue) or adds saturation (vs Saturation) of the colours under the point.", w), 1);
    hueTools->addWidget(resetHue);
    v->addLayout(hueTools);
    connect(resetHue, &QToolButton::clicked, this, [this] { edit_->setColorCurve(clipId_, hueChannel_, {}); });
    connect(hue_, &CurveEditor::edited, this, [this](const std::vector<Vec2>& pts, int serial) {
        edit_->setColorCurve(clipId_, hueChannel_, pts, "curve/" + hueChannel_ + "#" + std::to_string(serial));
    });
    v->addStretch(1);
    return w;
}

void ColorPanel::selectToneChannel(const std::string& id) {
    toneChannel_ = id;
    refresh();
}

void ColorPanel::selectHueChannel(const std::string& id) {
    hueChannel_ = id;
    refresh();
}

QWidget* ColorPanel::buildWheels() {
    auto* w = new QWidget();
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(10, 4, 10, 10);
    v->setSpacing(6);
    v->addWidget(header(w, "fa.color.wheels", "Color Wheels"));
    for (int k = 0; k < 3; ++k) {
        const std::string wk = kWheels[k];
        v->addWidget(groupLabel(kWheelLabels[k], w));
        auto* h = new QHBoxLayout();
        auto* wheel = new ColorWheel(w);
        wheel->setObjectName(QString::fromStdString("colorwheel:" + wk));
        wheels_[static_cast<size_t>(k)] = wheel;
        h->addWidget(wheel, 0, Qt::AlignTop);
        // beside the wheel: the R / G / B offsets as number fields (Filmora's numeric fields)
        auto* grid = new QGridLayout();
        grid->setHorizontalSpacing(6);
        grid->setVerticalSpacing(2);
        const std::pair<const char*, const char*> fields[3] = {{".r", "Red"}, {".g", "Green"}, {".b", "Blue"}};
        for (int i = 0; i < 3; ++i) {
            grid->addWidget(dim(fields[i].second, w), i, 0);
            EffectParamRow* r = row(w, "fa.color.wheels", wk + fields[i].first);
            r->setSliderVisible(false);
            grid->addWidget(r, i, 1);
        }
        grid->setRowStretch(3, 1);
        h->addLayout(grid, 1);
        v->addLayout(h);
        // under it: the range's luminance as a full-width slider row
        auto* lum = new QHBoxLayout();
        lum->addWidget(dim("Luminance", w));
        lum->addWidget(row(w, "fa.color.wheels", wk + ".lum"), 1);
        v->addLayout(lum);
        connect(wheel, &ColorWheel::tintEdited, this, [this, wk](QPointF chroma, int serial) {
            const Clip* c = clip();
            const auto t = doc_->clipTimeAtPlayhead(clipId_);
            if (!c || !t) return;
            const EffectDef* def = findEffectDef("fa.color.wheels");
            const EffectRef* e = EditController::colorEffect(*c, "fa.color.wheels");
            auto val = [&](const std::string& p) { return e ? effectParam(*e, *def, p, *t) : 0.0; };
            double r = val(wk + ".r"), g = val(wk + ".g"), b = val(wk + ".b");
            ColorWheel::offsetsOf(chroma, (r + g + b) / 3.0, r, g, b);
            edit_->setColorParams(clipId_, "fa.color.wheels", {{wk + ".r", r}, {wk + ".g", g}, {wk + ".b", b}},
                                  "wheel/" + wk + "#" + std::to_string(serial));
        });
    }
    v->addWidget(dim("Shadows, midtones and highlights each blend smoothly into the next.", w));
    v->addStretch(1);
    return w;
}

void ColorPanel::refreshHslSwatches(const Clip& c, const Time& t) {
    const EffectRef* e = EditController::colorEffect(c, "fa.color.hsl");
    const EffectDef* def = findEffectDef("fa.color.hsl");
    for (int i = 0; i < 8; ++i) {
        bool edited = false;
        if (e)
            for (const char* s : {".hue", ".saturation", ".luminance"})
                edited = edited || effectParam(*e, *def, std::string(kBands[i]) + s, t) != 0.0;
        // swatch colour: the band centre hue
        static const int hues[8] = {350, 30, 60, 120, 180, 231, 275, 310};
        swatches_[static_cast<size_t>(i)]->setIcon(swatchIcon(QColor::fromHsv(hues[i], 200, 230), edited));
    }
}

void ColorPanel::refresh() {
    const Clip* c = clip();
    ColorPresetLibrary* lib = edit_->colorPresets();
    const bool havePreset = lib && lib->find(presetCombo_->currentData().toString().toStdString());
    applyPreset_->setEnabled(havePreset && c);
    deletePreset_->setEnabled(havePreset);
    savePreset_->setEnabled(lib && c && !ColorPresetLibrary::captureColor(*c, c->sourceIn).empty());
    if (!c) return;
    const auto t = doc_->clipTimeAtPlayhead(clipId_);
    if (!t) return;
    const bool inside = doc_->playheadInside(clipId_);
    updating_ = true;
    for (auto* r : rows_) {
        r->setClip(clipId_);
        r->refresh(*c, *t, inside);
    }
    for (auto& h : headers_) {
        const EffectRef* e = EditController::colorEffect(*c, h.type);
        h.enabled->setEnabled(e != nullptr);
        h.enabled->setChecked(e ? e->enabled : true);
        h.enabled->setToolTip(e ? "Uncheck to bypass" : "Change a setting to add this to the clip's effects");
        h.reset->setEnabled(e != nullptr);
    }
    // curves
    const EffectRef* cv = EditController::colorEffect(*c, "fa.color.curves");
    const EffectDef* cdef = findEffectDef("fa.color.curves");
    auto curveOf = [&](const std::string& id) -> std::vector<Vec2> {
        if (cv)
            if (const auto it = cv->curves.find(id); it != cv->curves.end()) return it->second;
        for (const auto& cd : cdef->curves)
            if (cd.id == id) return cd.defaults;
        return {};
    };
    tone_->setCurve(curveOf(toneChannel_), channelColor(toneChannel_));
    std::vector<std::pair<std::vector<Vec2>, QColor>> ghosts;
    for (const char* ch : {"master", "red", "green", "blue"})
        if (ch != toneChannel_ && !isIdentityTone(curveOf(ch))) ghosts.push_back({curveOf(ch), channelColor(ch)});
    tone_->setGhosts(std::move(ghosts));
    hue_->setCurve(curveOf(hueChannel_), color::accent);
    const std::string otherHue = hueChannel_ == "hueHue" ? "hueSat" : "hueHue";
    std::vector<std::pair<std::vector<Vec2>, QColor>> hueGhosts;
    if (!isNeutralHue(curveOf(otherHue))) hueGhosts.push_back({curveOf(otherHue), color::textDim});
    hue_->setGhosts(std::move(hueGhosts));
    for (QToolButton* b : channelButtons_) {
        const std::string id = b->objectName().toStdString().substr(std::string("curvechannel:").size());
        b->setChecked(id == toneChannel_ || id == hueChannel_);
    }
    // wheels
    const EffectRef* wh = EditController::colorEffect(*c, "fa.color.wheels");
    const EffectDef* wdef = findEffectDef("fa.color.wheels");
    for (int k = 0; k < 3; ++k) {
        auto val = [&](const char* s) { return wh ? effectParam(*wh, *wdef, std::string(kWheels[k]) + s, *t) : 0.0; };
        wheels_[static_cast<size_t>(k)]->setOffsets(val(".r"), val(".g"), val(".b"));
    }
    refreshHslSwatches(*c, *t);
    refreshLut(c);
    updating_ = false;
}

}  // namespace fa::ui
