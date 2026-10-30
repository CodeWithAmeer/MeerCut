#include "speed_ramp_editor.h"

#include "theme.h"

#include "fa/model/time_map.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace fa::ui {

namespace {

double speedToY(double v) { return (std::log10(std::clamp(v, kMinSpeed, kMaxSpeed)) + 1.0) / 2.0; }   // 0..1
double yToSpeed(double y) { return std::pow(10.0, std::clamp(y, 0.0, 1.0) * 2.0 - 1.0); }

}  // namespace

RampGraph::RampGraph(QWidget* parent) : QWidget(parent) {
    setObjectName("rampGraph");
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(170);
    setMouseTracking(true);
    keys_ = {{0.0, 1.0}, {1.0, 1.0}};
}

void RampGraph::setKeys(RampKeys keys) {
    if (dragging_) return;
    keys_ = std::move(keys);
    if (selected_ >= static_cast<int>(keys_.size())) selected_ = -1;
    update();
}

void RampGraph::setSelected(int index) {
    selected_ = index >= 0 && index < static_cast<int>(keys_.size()) ? index : -1;
    emit selectionChanged(selected_);
    update();
}

QRectF RampGraph::plot() const { return QRectF(34, 10, width() - 44, height() - 30); }

QPointF RampGraph::toWidget(double f, double v) const {
    const QRectF r = plot();
    return {r.left() + f * r.width(), r.bottom() - speedToY(v) * r.height()};
}

std::pair<double, double> RampGraph::fromWidget(const QPointF& p) const {
    const QRectF r = plot();
    return {std::clamp((p.x() - r.left()) / r.width(), 0.0, 1.0), yToSpeed((r.bottom() - p.y()) / r.height())};
}

int RampGraph::hit(const QPointF& p) const {
    int best = -1;
    double bestD = 9.0;
    for (size_t i = 0; i < keys_.size(); ++i) {
        const double d = QLineF(p, toWidget(keys_[i].first, keys_[i].second)).length();
        if (d < bestD) {
            bestD = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void RampGraph::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), color::panel.darker(115));
    const QRectF r = plot();
    p.setPen(QPen(color::border, 1));
    p.drawRect(r);
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.8);
    p.setFont(f);
    for (double v : {0.1, 0.5, 1.0, 2.0, 5.0, 10.0}) {   // speed grid
        const double y = toWidget(0, v).y();
        p.setPen(QPen(v == 1.0 ? color::textDim : color::border, 1, v == 1.0 ? Qt::DashLine : Qt::SolidLine));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        p.setPen(color::textDim);
        p.drawText(QRectF(0, y - 8, 30, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(v) + "x");
    }
    p.drawText(QRectF(r.left(), r.bottom() + 2, r.width(), 16), Qt::AlignLeft, "0 %");
    p.drawText(QRectF(r.left(), r.bottom() + 2, r.width(), 16), Qt::AlignRight, "100 % of the source");
    // the curve, from the speed map itself
    if (keys_.size() >= 2) {
        Clip probe;
        probe.speed.ramp = rampKeys(keys_, Time(0), Time(100));
        probe.duration = durationForSource(probe.speed, Time(0), Time(100));
        const SourceMapper m(probe);
        const double d = probe.duration.toDouble();
        QPainterPath path;
        for (int i = 0; i <= 240; ++i) {
            const double t = d * i / 240.0;
            const QPointF q = toWidget(m.source(t) / 100.0, m.speed(t));
            if (i == 0) path.moveTo(q);
            else path.lineTo(q);
        }
        p.setPen(QPen(color::accent, 2));
        p.drawPath(path);
    }
    for (size_t i = 0; i < keys_.size(); ++i) {
        const QPointF q = toWidget(keys_[i].first, keys_[i].second);
        const bool sel = static_cast<int>(i) == selected_;
        p.setPen(QPen(sel ? Qt::white : color::accent, sel ? 2 : 1.5));
        p.setBrush(sel ? color::accent : color::panel);
        p.drawEllipse(q, sel ? 6 : 5, sel ? 6 : 5);
    }
}

void RampGraph::mousePressEvent(QMouseEvent* e) {
    setFocus();
    const QPointF pos = e->position();
    const int h = hit(pos);
    if (e->button() == Qt::RightButton) {
        if (h > 0 && h + 1 < static_cast<int>(keys_.size())) {
            selected_ = h;
            removeSelected();
        }
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    ++serial_;
    if (h >= 0) {
        setSelected(h);
        dragging_ = true;
        return;
    }
    if (!plot().adjusted(-4, -4, 4, 4).contains(pos) || static_cast<int>(keys_.size()) >= kMaxKeys) return;
    auto [f, v] = fromWidget(pos);
    for (const auto& k : keys_)
        if (std::fabs(k.first - f) < 0.01) return;   // too close to an existing key
    // the new key sits on the current curve (so adding a point alone changes nothing), then follows the mouse
    Clip probe;
    probe.speed.ramp = rampKeys(keys_, Time(0), Time(100));
    probe.duration = durationForSource(probe.speed, Time(0), Time(100));
    const SourceMapper m(probe);
    double lo = 0, hi = probe.duration.toDouble();
    for (int it = 0; it < 60; ++it) {
        const double mid = (lo + hi) / 2;
        (m.source(mid) < f * 100.0 ? lo : hi) = mid;
    }
    v = m.speed(lo);
    const auto it = std::lower_bound(keys_.begin(), keys_.end(), std::make_pair(f, 0.0));
    selected_ = static_cast<int>(keys_.insert(it, {f, v}) - keys_.begin());
    dragging_ = true;
    emit selectionChanged(selected_);
    emit edited(keys_, serial_);
    update();
}

void RampGraph::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_ || selected_ < 0) return;
    auto [f, v] = fromWidget(e->position());
    const int n = static_cast<int>(keys_.size());
    if (selected_ == 0) f = 0.0;
    else if (selected_ == n - 1) f = 1.0;
    else f = std::clamp(f, keys_[selected_ - 1].first + 0.005, keys_[selected_ + 1].first - 0.005);
    v = std::round(v * 100.0) / 100.0;
    keys_[selected_] = {f, std::clamp(v, kMinSpeed, kMaxSpeed)};
    emit edited(keys_, serial_);
    emit selectionChanged(selected_);
    update();
}

void RampGraph::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; }

void RampGraph::mouseDoubleClickEvent(QMouseEvent* e) {
    const int h = hit(e->position());
    if (h > 0 && h + 1 < static_cast<int>(keys_.size())) {
        selected_ = h;
        removeSelected();
    }
}

void RampGraph::keyPressEvent(QKeyEvent* e) {
    if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && selected_ > 0 &&
        selected_ + 1 < static_cast<int>(keys_.size())) {
        removeSelected();
        return;
    }
    QWidget::keyPressEvent(e);
}

void RampGraph::removeSelected() {
    if (selected_ <= 0 || selected_ + 1 >= static_cast<int>(keys_.size())) return;
    keys_.erase(keys_.begin() + selected_);
    selected_ = -1;
    ++serial_;
    emit selectionChanged(-1);
    emit edited(keys_, serial_);
    update();
}

// ---- user presets -------------------------------------------------------------------------------

SpeedPresetLibrary::SpeedPresetLibrary() {
    file_ = qEnvironmentVariable("MEERCUT_SPEED_PRESETS");
    if (file_.isEmpty()) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dir);
        file_ = QDir(dir).filePath("speed_presets.json");
    }
    load();
}

void SpeedPresetLibrary::load() {
    presets_.clear();
    QFile f(file_);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
    for (const auto& item : arr) {
        const QJsonObject o = item.toObject();
        Preset p;
        p.name = o["name"].toString();
        for (const auto& k : o["keys"].toArray()) {
            const QJsonArray pair = k.toArray();
            if (pair.size() == 2) p.keys.push_back({pair[0].toDouble(), pair[1].toDouble()});
        }
        if (!p.name.isEmpty() && p.keys.size() >= 2) presets_.push_back(std::move(p));
    }
}

bool SpeedPresetLibrary::save() {
    QJsonArray arr;
    for (const auto& p : presets_) {
        QJsonArray keys;
        for (const auto& [f, v] : p.keys) keys.append(QJsonArray{f, v});
        arr.append(QJsonObject{{"name", p.name}, {"keys", keys}});
    }
    QFile f(file_);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error_ = "cannot write " + file_;
        return false;
    }
    f.write(QJsonDocument(arr).toJson());
    return true;
}

bool SpeedPresetLibrary::add(const QString& name, const RampKeys& keys) {
    if (name.trimmed().isEmpty() || keys.size() < 2) {
        error_ = "a preset needs a name and at least two points";
        return false;
    }
    auto it = std::find_if(presets_.begin(), presets_.end(), [&](const Preset& p) { return p.name == name.trimmed(); });
    if (it != presets_.end()) it->keys = keys;
    else presets_.push_back({name.trimmed(), keys});
    return save();
}

bool SpeedPresetLibrary::remove(const QString& name) {
    const auto n = presets_.size();
    std::erase_if(presets_, [&](const Preset& p) { return p.name == name; });
    return presets_.size() != n && save();
}

}  // namespace fa::ui
