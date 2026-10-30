#include "animation_panel.h"

#include "edit_controller.h"
#include "theme.h"

#include "fa/model/edit_ops.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace fa::ui {

namespace {
constexpr int kRoleId = Qt::UserRole + 1;
constexpr int kThumb = 84;
constexpr double kDemoSeconds = 2.4;   // the demo clip the thumbnails animate

// A small landscape (sky, sun, two mountains) - the picture the presets move in the thumbnails.
QImage landscape() {
    static const QImage img = [] {
        QImage im(160, 90, QImage::Format_ARGB32_Premultiplied);
        QPainter p(&im);
        p.setRenderHint(QPainter::Antialiasing);
        QLinearGradient sky(0, 0, 0, 90);
        sky.setColorAt(0, QColor(70, 130, 200));
        sky.setColorAt(1, QColor(240, 190, 140));
        p.fillRect(im.rect(), sky);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 230, 150));
        p.drawEllipse(QPointF(118, 30), 12, 12);
        QPainterPath m1;
        m1.moveTo(-10, 90);
        m1.lineTo(55, 32);
        m1.lineTo(110, 90);
        p.setBrush(QColor(60, 80, 110));
        p.drawPath(m1);
        QPainterPath m2;
        m2.moveTo(60, 90);
        m2.lineTo(120, 45);
        m2.lineTo(180, 90);
        p.setBrush(QColor(40, 60, 85));
        p.drawPath(m2);
        return im;
    }();
    return img;
}
}  // namespace

AnimationPanel::AnimationPanel(Document* doc, EditController* edit, QWidget* parent) : QWidget(parent), doc_(doc), edit_(edit) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);
    auto* title = new QLabel("Motion Presets", this);
    title->setStyleSheet(QString("color: %1; font-weight: 600;").arg(color::accent.name()));
    lay->addWidget(title);
    auto* row = new QHBoxLayout();
    slots_ = new QButtonGroup(this);
    slots_->setExclusive(true);
    const std::pair<const char*, MotionSlot> names[] = {{"In", MotionSlot::In}, {"Out", MotionSlot::Out}, {"Combo", MotionSlot::Combo}};
    for (const auto& [name, s] : names) {
        auto* b = new QPushButton(name, this);
        b->setObjectName(QString("motionSlot") + name);
        b->setCheckable(true);
        b->setMinimumWidth(64);
        slots_->addButton(b, static_cast<int>(s));
        row->addWidget(b);
    }
    row->addStretch(1);
    slots_->button(0)->setChecked(true);
    lay->addLayout(row);
    list_ = new QListWidget(this);
    list_->setObjectName("motionPresets");
    list_->setViewMode(QListView::IconMode);
    list_->setIconSize(QSize(kThumb, kThumb * 9 / 16 + 8));
    list_->setGridSize(QSize(kThumb + 14, kThumb * 9 / 16 + 36));
    list_->setMovement(QListView::Static);
    list_->setResizeMode(QListView::Adjust);
    list_->setMouseTracking(true);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->viewport()->installEventFilter(this);
    lay->addWidget(list_, 1);
    status_ = new QLabel(this);
    status_->setObjectName("motionStatus");
    status_->setWordWrap(true);
    status_->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    lay->addWidget(status_);
    hoverTimer_ = new QTimer(this);
    hoverTimer_->setInterval(1000 / 20);
    connect(hoverTimer_, &QTimer::timeout, this, [this] {
        if (!hoverItem_) return;
        ++hoverStep_;
        hoverItem_->setIcon(thumbnail(hoverItem_->data(kRoleId).toString(), std::fmod(hoverStep_ / 20.0, kDemoSeconds + 0.6)));
    });
    connect(slots_, &QButtonGroup::idClicked, this, [this](int id) { setSlot(static_cast<MotionSlot>(id)); });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        if (!updating_) apply(it->data(kRoleId).toString());
    });
    connect(doc_, &Document::selectionChanged, this, &AnimationPanel::refresh);
    connect(doc_, &Document::changed, this, &AnimationPanel::refresh);
    refresh();
}

const Clip* AnimationPanel::clip() const {
    const Track* tr = nullptr;
    const Clip* c = edit::findClip(doc_->sequence(), doc_->primarySelection(), &tr);
    return c && tr->kind == TrackKind::Video && !c->isAdjustment() ? c : nullptr;
}

void AnimationPanel::setSlot(MotionSlot s) {
    slot_ = s;
    if (auto* b = slots_->button(static_cast<int>(s))) b->setChecked(true);
    refresh();
}

QIcon AnimationPanel::thumbnail(const QString& presetId, double seconds) const {
    const int w = kThumb, h = kThumb * 9 / 16;
    QPixmap pm(w * 2, (h + 8) * 2);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF frame(0, 4, w, h);
    p.fillRect(frame, QColor(18, 20, 24));
    if (presetId.isEmpty()) {   // None
        icon("delete", color::textDim).paint(&p, QRect(w / 2 - 12, 4 + h / 2 - 12, 24, 24));
        return QIcon(pm);
    }
    Clip demo;   // the preset applied to a demo clip, evaluated at `seconds`
    demo.duration = Rational::fromDouble(kDemoSeconds, 1000);
    try {
        applyMotionPreset(demo, slot_, presetId.toStdString());
    } catch (const std::exception&) {
    }
    const Time t = Rational::fromDouble(std::min(seconds, kDemoSeconds), 1000);
    const ClipTransform& tf = demo.transform;
    p.save();
    p.setClipRect(frame);
    p.translate(frame.center() + QPointF(tf.positionX.value(t) * w, tf.positionY.value(t) * h));
    p.rotate(tf.rotation.value(t));
    p.scale(tf.scaleX.value(t), tf.scaleY.value(t));
    p.setOpacity(std::clamp(demo.opacity.value(t), 0.0, 1.0));
    p.drawImage(QRectF(-w / 2.0, -h / 2.0, w, h), landscape());
    p.restore();
    return QIcon(pm);
}

void AnimationPanel::refresh() {
    updating_ = true;
    hoverItem_ = nullptr;
    hoverTimer_->stop();
    const Clip* c = clip();
    const std::string current = !c ? std::string() : slot_ == MotionSlot::In ? c->motionIn : slot_ == MotionSlot::Out ? c->motionOut : c->motionCombo;
    list_->clear();
    auto add = [&](const QString& id, const QString& name) {
        // the resting thumbnail: In presets at their end, Out presets at their start, Combo half way
        const double rest = slot_ == MotionSlot::Out ? 0.0 : slot_ == MotionSlot::In ? kDemoSeconds : kDemoSeconds / 2;
        auto* it = new QListWidgetItem(thumbnail(id, rest), name);
        it->setData(kRoleId, id);
        list_->addItem(it);
        if (id.toStdString() == current) list_->setCurrentItem(it);
    };
    add(QString(), "None");
    for (const auto& m : motionPresets())
        if (m.slot == slot_) add(QString::fromStdString(m.id), QString::fromStdString(m.name));
    list_->setEnabled(c != nullptr);
    status_->setText(!c ? QString("Select a video clip, picture, title or sticker on the timeline.")
                        : QString("Hover a preset to see it, click to apply. In and Out presets take %1 s at the clip's start "
                                  "and end; Combo moves through the whole clip. They are keyframes: fine-tune them in "
                                  "Video > Basic.").arg(motionLength(c->duration).toDouble(), 0, 'f', 2));
    updating_ = false;
}

bool AnimationPanel::apply(const QString& presetId) {
    const Clip* c = clip();
    if (!c) return false;
    const MotionSlot s = slot_;
    const std::string id = presetId.toStdString();
    const char* label = s == MotionSlot::In ? "In animation" : s == MotionSlot::Out ? "Out animation" : "Combo animation";
    return edit_->modifyClip(c->id, label, [s, id](Clip& x) { applyMotionPreset(x, s, id); });
}

bool AnimationPanel::eventFilter(QObject* o, QEvent* e) {
    if (o == list_->viewport()) {
        // the resting thumbnail back on the item the mouse left (not a refresh: that would delete the items)
        auto rest = [this](QListWidgetItem* it) {
            const double t = slot_ == MotionSlot::Out ? 0.0 : slot_ == MotionSlot::In ? kDemoSeconds : kDemoSeconds / 2;
            it->setIcon(thumbnail(it->data(kRoleId).toString(), t));
        };
        if (e->type() == QEvent::MouseMove) {
            QListWidgetItem* it = list_->itemAt(static_cast<QMouseEvent*>(e)->position().toPoint());
            if (it != hoverItem_) {
                if (hoverItem_) rest(hoverItem_);
                hoverItem_ = it;
                hoverStep_ = 0;
                if (it) hoverTimer_->start();
                else hoverTimer_->stop();
            }
        } else if (e->type() == QEvent::Leave && hoverItem_) {
            rest(hoverItem_);
            hoverItem_ = nullptr;
            hoverTimer_->stop();
        }
    }
    return QWidget::eventFilter(o, e);
}

}  // namespace fa::ui
