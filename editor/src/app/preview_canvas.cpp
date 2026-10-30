#include "preview_canvas.h"

#include "edit_controller.h"
#include "playback_controller.h"
#include "theme.h"

#include "fa/model/edit_ops.h"
#include "fa/model/keyframes.h"
#include "fa/model/time_map.h"
#include "fa/render/renderer.h"
#include "fa/render/text_animation.h"
#include "fa/text/stickers.h"
#include "fa/text/title_engine.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

namespace fa::ui {

namespace {

constexpr double kHandleR = 5.0;     // drawn half-size (px)
constexpr double kHitR = 9.0;        // grab distance (px)
constexpr double kRotateGap = 26.0;  // rotation handle distance above the frame (px)

Clip* mutableClip(Project& p, const std::string& seqId, const std::string& clipId) {
    for (auto& s : p.sequences)
        if (s.id == seqId)
            for (auto& t : s.tracks)
                for (auto& c : t.clips)
                    if (c.id == clipId) return &c;
    return nullptr;
}

double len(const QPointF& v) { return std::hypot(v.x(), v.y()); }
double dot(const QPointF& a, const QPointF& b) { return a.x() * b.x() + a.y() * b.y(); }
QPointF unit(const QPointF& v, const QPointF& fallback) {
    const double l = len(v);
    return l > 1e-9 ? v / l : fallback;
}
double deg(double rad) { return rad * 180.0 / std::numbers::pi; }
double wrap180(double d) {
    while (d > 180) d -= 360;
    while (d <= -180) d += 360;
    return d;
}

}  // namespace

PreviewCanvas::PreviewCanvas(Document* doc, EditController* edit, PlaybackController* playback, QWidget* parent)
    : QWidget(parent), doc_(doc), edit_(edit), playback_(playback) {
    setMouseTracking(true);
    connect(doc_, &Document::selectionChanged, this, qOverload<>(&QWidget::update));
    connect(doc_, &Document::maskSelectionChanged, this, qOverload<>(&QWidget::update));
    connect(doc_, &Document::changed, this, qOverload<>(&QWidget::update));
    connect(doc_, &Document::playheadChanged, this, qOverload<>(&QWidget::update));
    connect(doc_, &Document::trackingBoxChanged, this, qOverload<>(&QWidget::update));
}

PreviewCanvas::~PreviewCanvas() = default;

void PreviewCanvas::setFrame(const QImage& img, const QStringList& offline) {
    image_ = img;
    offline_ = offline;
    update();
}

void PreviewCanvas::setCompare(bool on) {
    compare_ = on;
    update();
}

// ---- geometry --------------------------------------------------------------------------------

QRectF PreviewCanvas::frameRect() const {
    const Sequence& s = doc_->sequence();
    const QSizeF sz = QSizeF(std::max(1, s.width), std::max(1, s.height)).scaled(size(), Qt::KeepAspectRatio);
    return QRectF((width() - sz.width()) / 2, (height() - sz.height()) / 2, sz.width(), sz.height());
}

QPointF PreviewCanvas::toWidget(const QPointF& q) const {
    const QRectF r = frameRect();
    const Sequence& s = doc_->sequence();
    return {r.x() + q.x() * r.width() / s.width, r.y() + q.y() * r.height() / s.height};
}

QPointF PreviewCanvas::toSeq(const QPointF& w) const {
    const QRectF r = frameRect();
    const Sequence& s = doc_->sequence();
    return {(w.x() - r.x()) * s.width / r.width(), (w.y() - r.y()) * s.height / r.height()};
}

namespace {

// M10: the part of a sticker's picture that has art in it (fractions of the picture), over its whole animation, with a
// small margin: the box in the player hugs the sticker instead of its transparent square. Cached per look and length.
QRectF stickerArt(const StickerData& s, double seconds, bool fullFrame, double aspect) {
    static std::map<std::string, QRectF> cache;
    const std::string key = s.id + '\x1e' + s.text + '\x1e' + std::to_string(std::llround(seconds * 1000)) + '\x1e' +
                            std::to_string(std::llround(aspect * 1000));
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
    const int w = fullFrame ? 320 : 256;
    const int h = fullFrame ? std::max(1, static_cast<int>(std::lround(320 / std::max(0.1, aspect)))) : 256;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    const double span = std::clamp(seconds, 0.0, 4.0);   // the animations play their story within the first seconds
    for (int k = 0; k <= 12; ++k) {
        const QImage img = text::stickerImage(s, span * k / 12.0, seconds, w, h);
        for (int y = 0; y < img.height(); ++y) {
            const uchar* row = img.constScanLine(y);
            for (int x = 0; x < img.width(); ++x)
                if (row[x * 4 + 3] > 16) {
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                    y0 = std::min(y0, y);
                    y1 = std::max(y1, y);
                }
        }
    }
    QRectF art(0, 0, 1, 1);
    if (x1 >= x0 && y1 >= y0) {
        const double m = 2.0;   // pixels of margin at this size
        art = QRectF(QPointF(std::max(0.0, (x0 - m) / w), std::max(0.0, (y0 - m) / h)),
                     QPointF(std::min(1.0, (x1 + 1 + m) / w), std::min(1.0, (y1 + 1 + m) / h)));
    }
    if (cache.size() > 256) cache.clear();
    cache.emplace(key, art);
    return art;
}

}  // namespace

std::optional<PreviewCanvas::Target> PreviewCanvas::target() const {
    if (playback_->isPlaying()) return std::nullopt;
    const std::string id = doc_->primarySelection();
    if (id.empty() || !doc_->playheadInside(id)) return std::nullopt;
    const Track* tr = nullptr;
    const Clip* c = edit::findClip(doc_->sequence(), id, &tr);
    if (!c || !tr) return std::nullopt;
    return makeTarget(*c, *tr, doc_->playheadTime());
}

std::optional<PreviewCanvas::Target> PreviewCanvas::makeTarget(const Clip& c, const Track& tr, const Time& now) const {
    Target t;
    t.clip = &c;
    t.t = c.sourceTimeAt(now);
    t.locked = tr.locked;
    if (c.isTitle()) {   // the text block as laid out by the engine (same layout as the renders)
        if (tr.kind != TrackKind::Video && tr.kind != TrackKind::Subtitle) return std::nullopt;
        const Sequence& s = doc_->sequence();
        t.title = true;
        t.caption = tr.kind == TrackKind::Subtitle;
        t.trackStyle = tr.subtitleStyle;
        t.subtitleY = tr.subtitleY;
        const TextStyle& st = t.caption ? captionStyle(tr, c) : c.title->style;
        if (!titleEngine_) {
            text::ensureGuiApplication();
            const_cast<PreviewCanvas*>(this)->titleEngine_ = std::make_unique<text::TitleEngine>();
        }
        const text::LayoutInfo L = titleEngine_->layout(c.title->text, st, double(s.width) / std::max(1, s.height));
        if (c.title->text.empty() || L.blockW <= 0 || L.blockH <= 0) return std::nullopt;
        const double k = s.height / 1080.0;
        t.blockW = L.blockW * k;
        t.blockH = L.blockH * k;
        t.pad = st.background ? st.backgroundPadding * k : 0.0;
        t.refX = t.blockW / 2;
        t.refY = st.anchor == TextAnchor::Bottom ? t.blockH : st.anchor == TextAnchor::Top ? 0.0 : t.blockH / 2;
        t.adj = titleAdjust(evaluateTitleAnimation(*c.title, now - c.timelineStart, c.duration, L.counts), s.width, s.height);
        t.srcW = std::max(1, static_cast<int>(std::lround(t.blockW)));
        t.srcH = std::max(1, static_cast<int>(std::lround(t.blockH)));
        return t;
    }
    if (tr.kind != TrackKind::Video) return std::nullopt;
    if (c.isAdjustment()) {   // its masks are in canvas-height units around the canvas centre (like a full-frame clip)
        t.adjustment = true;
        t.srcW = doc_->sequence().width;
        t.srcH = doc_->sequence().height;
        return t;
    }
    if (c.isSticker()) {   // M10: the renderer's picture - square at the canvas height, or the canvas - placed like media
        const text::StickerInfo* info = text::findSticker(c.sticker->id);
        if (!info) return std::nullopt;
        const Sequence& s = doc_->sequence();
        t.srcW = info->fullFrame ? s.width : s.height;
        t.srcH = s.height;
        t.art = stickerArt(*c.sticker, c.duration.toDouble(), info->fullFrame, double(t.srcW) / std::max(1, t.srcH));
        return t;
    }
    const MediaSource* src = doc_->project().findSource(c.sourceId);
    if (!src || !src->hasVideo || src->width <= 0 || src->height <= 0) return std::nullopt;
    t.srcW = src->width;
    t.srcH = src->height;
    return t;
}

Vec2 PreviewCanvas::basePosition(const Clip& c, const Target& tg) const {
    if (!tg.caption || !c.title) return {};
    return captionBasePosition(tg.subtitleY, c.title->followTrackStyle ? tg.trackStyle : c.title->style, c);
}

const Mask* PreviewCanvas::targetMask(const Clip& c) const { return findActiveMask(c, doc_->activeMask()); }

PreviewCanvas::LayerGeo PreviewCanvas::layerGeo(const Clip& c, const Target& tg) const {
    const Sequence& s = doc_->sequence();
    LayerGeo g;
    auto m = [&](double x, double y) {
        const Vec2 v = g.a.apply(x, y);
        return QPointF(v.x, v.y);
    };
    const ClipTransform& tf = c.transform;
    if (tg.title) {   // block space -> canvas, exactly as titlePlacement places the rendered picture
        TitleFrame f;
        f.refX = tg.refX;
        f.refY = tg.refY;
        f.blockW = tg.blockW;
        f.blockH = tg.blockH;
        g.a = titlePlacement(c, tg.t, f, s.width, s.height, tg.adj, basePosition(c, tg));
        const double p = tg.pad;
        g.corners[0] = m(-p, -p);
        g.corners[1] = m(tg.blockW + p, -p);
        g.corners[2] = m(tg.blockW + p, tg.blockH + p);
        g.corners[3] = m(-p, tg.blockH + p);
        const double fx = tf.flipH ? -1 : 1, fy = tf.flipV ? -1 : 1;
        g.pivot = m(tg.refX + fx * tf.anchorX.value(tg.t) * tg.blockW, tg.refY + fy * tf.anchorY.value(tg.t) * tg.blockH);
        return g;
    }
    g.a = clipPlacement(c, tg.t, tg.srcW, tg.srcH, s.width, s.height);
    double x0 = tf.cropLeft * tg.srcW, x1 = tg.srcW * (1 - tf.cropRight);
    double y0 = tf.cropTop * tg.srcH, y1 = tg.srcH * (1 - tf.cropBottom);
    // M10: a sticker's box is its art, within the crop (the whole crop when the crop leaves none of it)
    const double ax0 = std::max(x0, tg.art.left() * tg.srcW), ax1 = std::min(x1, tg.art.right() * tg.srcW);
    const double ay0 = std::max(y0, tg.art.top() * tg.srcH), ay1 = std::min(y1, tg.art.bottom() * tg.srcH);
    if (ax1 > ax0 && ay1 > ay0) {
        x0 = ax0;
        x1 = ax1;
        y0 = ay0;
        y1 = ay1;
    }
    g.corners[0] = m(x0, y0);
    g.corners[1] = m(x1, y0);
    g.corners[2] = m(x1, y1);
    g.corners[3] = m(x0, y1);
    const double fx = tf.flipH ? -1 : 1, fy = tf.flipV ? -1 : 1;
    g.pivot = m(tg.srcW / 2.0 + fx * tf.anchorX.value(tg.t) * tg.srcW, tg.srcH / 2.0 + fy * tf.anchorY.value(tg.t) * tg.srcH);
    return g;
}

bool PreviewCanvas::trackingMode(const Target& tg) const {
    const auto& b = doc_->trackingBox();
    return b && tg.clip && b->clipId == tg.clip->id && !tg.title && !tg.adjustment && tg.srcW > 0 && tg.srcH > 0;
}

QPointF PreviewCanvas::boxToSeq(const Clip& c, const Target& tg, double fx, double fy) const {
    const Vec2 v = layerGeo(c, tg).a.apply(fx * tg.srcW, fy * tg.srcH);
    return {v.x, v.y};
}

QPointF PreviewCanvas::seqToBox(const Clip& c, const Target& tg, const QPointF& q) const {
    const Vec2 v = layerGeo(c, tg).a.inverse().apply(q.x(), q.y());
    return {v.x / tg.srcW, v.y / tg.srcH};
}

std::optional<QRectF> PreviewCanvas::trackedRegion(const Clip& c, const Target& tg) const {
    if (!c.tracking) return std::nullopt;
    const MediaSource* src = doc_->project().findSource(c.sourceId);
    if (!src || !src->frameRate) return std::nullopt;
    const TrackingSample* s = trackSampleAt(*c.tracking, (tg.t * src->frameRate->fps()).floor());
    if (!s) return std::nullopt;
    const double w = c.tracking->boxW * s->scale, h = c.tracking->boxH * s->scale;
    return QRectF(s->x - w / 2, s->y - h / 2, w, h);
}

PreviewCanvas::MaskGeo PreviewCanvas::maskGeo(const Clip& c, const Mask& mk, const Target& tg) const {
    const Affine2D a = layerGeo(c, tg).a;
    const double cx = mk.centerX.value(tg.t), cy = mk.centerY.value(tg.t);
    const double w = std::fabs(mk.width.value(tg.t)), h = std::fabs(mk.height.value(tg.t));
    const double r = mk.rotation.value(tg.t) * std::numbers::pi / 180.0, cs = std::cos(r), sn = std::sin(r);
    auto local = [&](double px, double py) {   // mask frame -> sequence px
        const double qx = cx + cs * px - sn * py, qy = cy + sn * px + cs * py;
        const Vec2 v = a.apply(tg.srcW / 2.0 + qx * tg.srcH, tg.srcH / 2.0 + qy * tg.srcH);
        return QPointF(v.x, v.y);
    };
    MaskGeo g;
    g.center = local(0, 0);
    g.corners[0] = local(-w / 2, -h / 2);
    g.corners[1] = local(w / 2, -h / 2);
    g.corners[2] = local(w / 2, h / 2);
    g.corners[3] = local(-w / 2, h / 2);
    switch (mk.shape) {
        case MaskShape::Ellipse:
            for (int i = 0; i < 96; ++i) {
                const double t = i * 2 * std::numbers::pi / 96;
                g.outline << local(w / 2 * std::cos(t), h / 2 * std::sin(t));
            }
            break;
        case MaskShape::Rectangle: {
            const double hx = w / 2, hy = h / 2;
            const double rad = std::clamp(mk.cornerRadius, 0.0, 0.5) * 2 * std::min(hx, hy);
            const double cxs[] = {hx - rad, -hx + rad, -hx + rad, hx - rad}, cys[] = {-hy + rad, -hy + rad, hy - rad, hy - rad};
            const double start[] = {-90, 180, 90, 0};
            for (int k = 0; k < 4; ++k)
                for (int i = 0; i <= 8; ++i) {
                    const double t = (start[k] - i * 90.0 / 8) * std::numbers::pi / 180;
                    g.outline << local(cxs[k] + rad * std::cos(t), cys[k] + rad * std::sin(t));
                }
            break;
        }
        case MaskShape::Polygon:
            for (const Vec2& p : mk.points) {
                g.points.push_back(local(p.x * mk.width.value(tg.t), p.y * mk.height.value(tg.t)));
                g.outline << g.points.back();
            }
            break;
    }
    return g;
}

std::map<QString, QPointF> PreviewCanvas::handles() const {
    std::map<QString, QPointF> h;
    const auto tg = target();
    if (!tg || tg->locked) return h;
    const Clip& c = live_ ? *live_ : *tg->clip;
    auto rotateHandle = [&](const QPointF* corners, const QString& name) {
        QPointF w[4];
        for (int i = 0; i < 4; ++i) w[i] = toWidget(corners[i]);
        const QPointF topMid = (w[0] + w[1]) / 2, mid = (w[0] + w[1] + w[2] + w[3]) / 4;
        h[name] = topMid + unit(topMid - mid, QPointF(0, -1)) * kRotateGap;
    };
    if (trackingMode(*tg)) {   // the tracking box replaces the other handles while it is placed
        const Document::TrackingBox& b = *doc_->trackingBox();
        const double x0 = b.x - b.w / 2, x1 = b.x + b.w / 2, y0 = b.y - b.h / 2, y1 = b.y + b.h / 2;
        h["k:tl"] = toWidget(boxToSeq(c, *tg, x0, y0));
        h["k:tr"] = toWidget(boxToSeq(c, *tg, x1, y0));
        h["k:br"] = toWidget(boxToSeq(c, *tg, x1, y1));
        h["k:bl"] = toWidget(boxToSeq(c, *tg, x0, y1));
        h["k:center"] = toWidget(boxToSeq(c, *tg, b.x, b.y));
        return h;
    }
    if (const Mask* m = targetMask(c)) {
        const MaskGeo g = maskGeo(c, *m, *tg);
        const char* names[] = {"m:tl", "m:tr", "m:br", "m:bl"};
        for (int i = 0; i < 4; ++i) h[names[i]] = toWidget(g.corners[i]);
        h["m:center"] = toWidget(g.center);
        rotateHandle(g.corners, "m:rotate");
        for (size_t i = 0; i < g.points.size(); ++i) h[QString("m:p%1").arg(i)] = toWidget(g.points[i]);
        return h;
    }
    if (tg->adjustment) return h;   // an adjustment layer has no picture to move or scale
    const LayerGeo g = layerGeo(c, *tg);
    const char* names[] = {"tl", "tr", "br", "bl"};
    for (int i = 0; i < 4; ++i) h[names[i]] = toWidget(g.corners[i]);
    if (!c.transform.uniformScale) {
        const char* edges[] = {"t", "r", "b", "l"};
        for (int i = 0; i < 4; ++i) h[edges[i]] = toWidget((g.corners[i] + g.corners[(i + 1) % 4]) / 2);
    }
    rotateHandle(g.corners, "rotate");
    h["anchor"] = toWidget(g.pivot);
    h["center"] = toWidget((g.corners[0] + g.corners[1] + g.corners[2] + g.corners[3]) / 4);
    return h;
}

std::optional<QPointF> PreviewCanvas::handlePos(const QString& name) const {
    const auto h = handles();
    const auto it = h.find(name);
    if (it == h.end()) return std::nullopt;
    return it->second;
}

bool PreviewCanvas::handlesVisible() const { return !handles().empty(); }

QString PreviewCanvas::handleAt(const QPointF& pos, Qt::KeyboardModifiers mods) const {
    const auto h = handles();
    if (h.empty()) return {};
    QString best;
    double bestD = kHitR;
    for (const auto& [name, pt] : h) {
        if (name == "center" || name == "m:center" || name == "k:center") continue;   // the centre is grabbed as "move"
        if (name == "anchor" && !(mods & Qt::ControlModifier)) continue;   // Ctrl+drag moves the anchor
        const double d = len(pos - pt);
        // points and rotation take priority over corners when they overlap
        const double bias = name.startsWith("m:p") || name.endsWith("rotate") ? 2.0 : name.endsWith("anchor") ? 0.5 : 0;
        if (d - bias < bestD) {
            bestD = d - bias;
            best = name;
        }
    }
    if (!best.isEmpty()) return best;
    const auto tg = target();
    const Clip& c = live_ ? *live_ : *tg->clip;
    if (trackingMode(*tg)) {
        QPolygonF box;
        for (const char* n : {"k:tl", "k:tr", "k:br", "k:bl"}) box << h.at(n);
        return box.containsPoint(pos, Qt::OddEvenFill) ? "k:move" : QString();
    }
    if (const Mask* m = targetMask(c)) {
        const MaskGeo g = maskGeo(c, *m, *tg);
        QPolygonF box;
        for (const auto& q : g.corners) box << toWidget(q);
        QPolygonF outline;
        for (const auto& q : g.outline) outline << toWidget(q);
        if (outline.containsPoint(pos, Qt::OddEvenFill) || box.containsPoint(pos, Qt::OddEvenFill)) return "m:move";
        return {};
    }
    if (tg->adjustment) return {};
    const LayerGeo g = layerGeo(c, *tg);
    QPolygonF quad;
    for (const auto& q : g.corners) quad << toWidget(q);
    return quad.containsPoint(pos, Qt::OddEvenFill) ? "move" : QString();
}

// ---- gestures --------------------------------------------------------------------------------

void PreviewCanvas::updateGesture(const QPointF& pos, Qt::KeyboardModifiers mods) {
    if (!clip0_) return;
    const Clip& c0 = *clip0_;
    const Target& tg = target0_;
    const Time t = tg.t;
    const Sequence& s = doc_->sequence();
    const double cw = s.width, ch = s.height;
    const QPointF m = toSeq(pos);
    const LayerGeo g0 = layerGeo(c0, tg);
    const bool shift = mods & Qt::ShiftModifier;
    infoPos_ = pos;

    if (grab_.startsWith("k:")) {   // ---- tracking box: UI state, shown at once, no undo entry
        if (!box0_) return;
        const Document::TrackingBox& b0 = *box0_;
        Document::TrackingBox b = b0;
        const QPointF q = seqToBox(c0, tg, m), q0 = seqToBox(c0, tg, press_);
        if (grab_ == "k:move") {
            QPointF d = q - q0;
            if (shift) (std::fabs(d.x()) > std::fabs(d.y()) ? d.ry() : d.rx()) = 0;
            b.x = std::clamp(b0.x + d.x(), b0.w / 2, 1 - b0.w / 2);
            b.y = std::clamp(b0.y + d.y(), b0.h / 2, 1 - b0.h / 2);
        } else {   // corner: the opposite corner stays where it is
            const bool left = grab_ == "k:tl" || grab_ == "k:bl", top = grab_ == "k:tl" || grab_ == "k:tr";
            const double ox = left ? b0.x + b0.w / 2 : b0.x - b0.w / 2, oy = top ? b0.y + b0.h / 2 : b0.y - b0.h / 2;
            const double minW = 16.0 / tg.srcW, minH = 16.0 / tg.srcH;   // at least 16 source pixels
            const double nx = std::clamp(q.x(), 0.0, 1.0), ny = std::clamp(q.y(), 0.0, 1.0);
            double lx = std::min(nx, ox), hx = std::max(nx, ox), ly = std::min(ny, oy), hy = std::max(ny, oy);
            if (hx - lx < minW) (nx < ox ? lx : hx) = nx < ox ? hx - minW : lx + minW;
            if (hy - ly < minH) (ny < oy ? ly : hy) = ny < oy ? hy - minH : ly + minH;
            b.x = (lx + hx) / 2;
            b.y = (ly + hy) / 2;
            b.w = hx - lx;
            b.h = hy - ly;
        }
        info_ = QString("Tracking box %1 x %2 px").arg(b.w * tg.srcW, 0, 'f', 0).arg(b.h * tg.srcH, 0, 'f', 0);
        doc_->setTrackingBox(b);
        return;
    }

    if (grab_.startsWith("m:")) {   // ---- mask
        const std::string maskId = doc_->activeMask();
        const Mask* mk = findActiveMask(c0, maskId);
        if (!mk) return;
        const Affine2D inv = g0.a.inverse();
        auto toQ = [&](const QPointF& p) {
            const Vec2 sp = inv.apply(p.x(), p.y());
            return QPointF((sp.x - tg.srcW / 2.0) / tg.srcH, (sp.y - tg.srcH / 2.0) / tg.srcH);
        };
        const QPointF c(mk->centerX.value(t), mk->centerY.value(t));
        const double w0 = mk->width.value(t), h0 = mk->height.value(t), rot0 = mk->rotation.value(t);
        const double r = -rot0 * std::numbers::pi / 180.0, cs = std::cos(r), sn = std::sin(r);
        auto toLocal = [&](const QPointF& q) {
            const QPointF d = q - c;
            return QPointF(cs * d.x() - sn * d.y(), sn * d.x() + cs * d.y());
        };
        const QPointF q = toQ(m), q0 = toQ(press_);
        auto onMask = [maskId](std::function<void(Mask&)> f) {
            return [maskId, f](Clip& cl) {
                if (Mask* x = findActiveMask(cl, maskId)) f(*x);
            };
        };
        if (grab_ == "m:move") {
            QPointF d = q - q0;
            if (shift) (std::fabs(d.x()) > std::fabs(d.y()) ? d.ry() : d.rx()) = 0;
            const QPointF n = c + d;
            change_ = onMask([t, n](Mask& x) {
                keys::setValueAt(x.centerX, t, n.x());
                keys::setValueAt(x.centerY, t, n.y());
            });
            label_ = "Move mask";
            info_ = QString("X %1  Y %2 px").arg(n.x() * ch, 0, 'f', 0).arg(n.y() * ch, 0, 'f', 0);
        } else if (grab_ == "m:rotate") {
            const QPointF a = q - c, b = q0 - c;
            double rot = rot0 + wrap180(deg(std::atan2(a.y(), a.x()) - std::atan2(b.y(), b.x())));
            if (shift) rot = std::round(rot / 15.0) * 15.0;
            change_ = onMask([t, rot](Mask& x) { keys::setValueAt(x.rotation, t, rot); });
            label_ = "Rotate mask";
            info_ = QString::fromUtf8("%1\xC2\xB0").arg(rot, 0, 'f', 1);
        } else if (grab_.startsWith("m:p")) {
            const int i = grab_.mid(3).toInt();
            const QPointF p = toLocal(q);
            const double ux = std::fabs(w0) > 1e-9 ? p.x() / w0 : 0, uy = std::fabs(h0) > 1e-9 ? p.y() / h0 : 0;
            change_ = onMask([i, ux, uy](Mask& x) {
                if (i >= 0 && i < static_cast<int>(x.points.size())) x.points[static_cast<size_t>(i)] = {ux, uy};
            });
            label_ = "Move mask point";
            info_.clear();
        } else {   // corner: resize about the centre
            const QPointF p = toLocal(q), p0 = toLocal(q0);
            double nw = std::max(0.005, 2 * std::fabs(p.x())), nh = std::max(0.005, 2 * std::fabs(p.y()));
            if (shift) {   // keep the proportions
                const double k = len(p) / std::max(1e-9, len(p0));
                nw = std::max(0.005, std::fabs(w0) * k);
                nh = std::max(0.005, std::fabs(h0) * k);
            }
            change_ = onMask([t, nw, nh](Mask& x) {
                keys::setValueAt(x.width, t, nw);
                keys::setValueAt(x.height, t, nh);
            });
            label_ = "Resize mask";
            info_ = QString("W %1  H %2 px").arg(nw * ch, 0, 'f', 0).arg(nh * ch, 0, 'f', 0);
        }
        return;
    }

    // ---- transform
    const ClipTransform& tf = c0.transform;
    const double px0 = tf.positionX.value(t), py0 = tf.positionY.value(t);
    const double sx0 = tf.scaleX.value(t), sy0 = tf.uniformScale ? sx0 : tf.scaleY.value(t);
    const double rot0 = tf.rotation.value(t);
    const bool uniform = tf.uniformScale;
    const Vec2 base0 = basePosition(c0, tg);   // captions on the subtitle line get their own position when moved
    if (grab_ == "move") {
        QPointF d = m - press_;
        if (shift) (std::fabs(d.x()) > std::fabs(d.y()) ? d.ry() : d.rx()) = 0;
        const double nx = px0 + base0.x + d.x() / cw, ny = py0 + base0.y + d.y() / ch;
        change_ = [t, nx, ny](Clip& cl) {
            keys::setValueAt(cl.transform.positionX, t, nx);
            keys::setValueAt(cl.transform.positionY, t, ny);
        };
        label_ = "Move";
        info_ = QString("X %1  Y %2 px").arg(nx * cw, 0, 'f', 0).arg(ny * ch, 0, 'f', 0);
    } else if (grab_ == "rotate") {
        const QPointF a = m - g0.pivot, b = press_ - g0.pivot;
        double rot = rot0 + wrap180(deg(std::atan2(a.y(), a.x()) - std::atan2(b.y(), b.x())));
        if (shift) rot = std::round(rot / 15.0) * 15.0;
        change_ = [t, rot](Clip& cl) { keys::setValueAt(cl.transform.rotation, t, rot); };
        label_ = "Rotate";
        info_ = QString::fromUtf8("%1\xC2\xB0").arg(rot, 0, 'f', 1);
    } else if (grab_ == "anchor") {
        const Affine2D inv = g0.a.inverse();
        const Vec2 sp = inv.apply(m.x(), m.y());
        const double fx = tf.flipH ? -1 : 1, fy = tf.flipV ? -1 : 1;
        const double cx = tg.title ? tg.refX : tg.srcW / 2.0, cy = tg.title ? tg.refY : tg.srcH / 2.0;
        const double lw = tg.title ? tg.blockW : tg.srcW, lh = tg.title ? tg.blockH : tg.srcH;
        const double ax1 = fx * (sp.x - cx) / lw, ay1 = fy * (sp.y - cy) / lh;
        const double ax0 = tf.anchorX.value(t), ay0 = tf.anchorY.value(t);
        // keep the picture in place: C' = C + d - RS d, d = old anchor - new anchor (layer px)
        double bx = 1, by = 1;   // titles: block px are canvas px
        if (!tg.title) fitScale(s.width, s.height, tg.srcW, tg.srcH, LayerFit::Fit, bx, by);
        const double dx = (ax0 - ax1) * lw * bx, dy = (ay0 - ay1) * lh * by;
        const Vec2 rsd = (Affine2D::rotateDeg(rot0) * Affine2D::scale(sx0, sy0)).apply(dx, dy);
        const double nx = px0 + base0.x + (dx - rsd.x) / cw, ny = py0 + base0.y + (dy - rsd.y) / ch;
        change_ = [t, ax1, ay1, nx, ny](Clip& cl) {
            keys::setValueAt(cl.transform.anchorX, t, ax1);
            keys::setValueAt(cl.transform.anchorY, t, ay1);
            keys::setValueAt(cl.transform.positionX, t, nx);
            keys::setValueAt(cl.transform.positionY, t, ny);
        };
        label_ = "Move anchor point";
        info_ = QString("Anchor %1 %  %2 %").arg(ax1 * 100, 0, 'f', 1).arg(ay1 * 100, 0, 'f', 1);
    } else {   // scale: corners (both axes) or edges (one axis)
        const QPointF ux = unit(g0.corners[1] - g0.corners[0], QPointF(1, 0));
        const QPointF uy = unit(g0.corners[3] - g0.corners[0], QPointF(0, 1));
        const QPointF a = m - g0.pivot, b = press_ - g0.pivot;
        auto ratio = [](double now, double then) { return std::fabs(then) < 1e-6 ? 1.0 : now / then; };
        double nsx = sx0, nsy = sy0;
        const bool corner = grab_.size() == 2;
        if (corner && uniform) {
            nsx = nsy = sx0 * len(a) / std::max(1e-6, len(b));
        } else {
            if (corner || grab_ == "l" || grab_ == "r") nsx = sx0 * ratio(dot(a, ux), dot(b, ux));
            if (corner || grab_ == "t" || grab_ == "b") nsy = sy0 * ratio(dot(a, uy), dot(b, uy));
        }
        nsx = std::clamp(nsx, 0.01, 10.0);
        nsy = std::clamp(nsy, 0.01, 10.0);
        change_ = [t, nsx, nsy, uniform](Clip& cl) {
            keys::setValueAt(cl.transform.scaleX, t, nsx);
            if (uniform || nsy != cl.transform.scaleY.value(t)) keys::setValueAt(cl.transform.scaleY, t, nsy);
        };
        label_ = "Scale";
        info_ = uniform ? QString("Scale %1 %").arg(nsx * 100, 0, 'f', 1)
                        : QString("W %1 %  H %2 %").arg(nsx * 100, 0, 'f', 1).arg(nsy * 100, 0, 'f', 1);
    }
}

void PreviewCanvas::applyPreview() {
    if (!clip0_ || !change_) return;
    live_ = *clip0_;
    change_(*live_);
    const std::string seqId = doc_->sequenceId(), id = clipId_;
    const auto ch = change_;
    doc_->previewEdit([seqId, id, ch](Project& p) {
        if (Clip* c = mutableClip(p, seqId, id)) ch(*c);
    });
    update();
}

void PreviewCanvas::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    QString h = handleAt(e->position(), e->modifiers());
    const auto tg0 = target();
    const bool tracking = tg0 && trackingMode(*tg0);
    if ((h.isEmpty() || h == "move") && doc_->activeMask().empty() && !tracking) {
        // click on a picture in the player: select the top-most clip under the cursor (a title in front of the
        // selected footage takes the click), then move it; the handles of the selection keep priority
        const Sequence& s = doc_->sequence();
        const Time now = doc_->playheadTime();
        std::string hit;
        auto test = [&](const Track& tr) {
            for (const auto& c : tr.clips) {
                if (!c.enabled || !c.timelineRange().contains(now) || c.isAdjustment()) continue;   // not by click
                const auto tg = makeTarget(c, tr, now);
                if (!tg) continue;
                const LayerGeo g = layerGeo(c, *tg);
                QPolygonF quad;
                for (const auto& q : g.corners) quad << toWidget(q);
                if (quad.containsPoint(e->position(), Qt::OddEvenFill)) hit = c.id;
            }
        };
        for (auto it = s.tracks.rbegin(); it != s.tracks.rend() && hit.empty(); ++it)   // captions are drawn on top
            if (it->kind == TrackKind::Subtitle && !it->hidden) test(*it);
        for (auto it = s.tracks.rbegin(); it != s.tracks.rend() && hit.empty(); ++it)
            if (it->kind == TrackKind::Video && !it->hidden) test(*it);
        if (!hit.empty() && hit != doc_->primarySelection()) {
            doc_->setSelection({hit});
            h = handleAt(e->position(), e->modifiers());
        }
    }
    const auto tg = target();
    if (h.isEmpty() || !tg || tg->locked) return;
    grab_ = h;
    clipId_ = doc_->primarySelection();
    clip0_ = *tg->clip;
    target0_ = *tg;
    target0_.clip = nullptr;   // the document's clip may be replaced while dragging
    press_ = toSeq(e->position());
    change_ = nullptr;
    box0_ = h.startsWith("k:") ? doc_->trackingBox() : std::nullopt;
    info_.clear();
}

void PreviewCanvas::mouseMoveEvent(QMouseEvent* e) {
    if (grab_.isEmpty()) {
        const QString h = handleAt(e->position(), e->modifiers());
        if (h.isEmpty()) unsetCursor();
        else if (h.endsWith("move")) setCursor(Qt::SizeAllCursor);
        else if (h.endsWith("rotate")) setCursor(Qt::CrossCursor);
        else if (h.endsWith("anchor") || h.startsWith("m:p")) setCursor(Qt::PointingHandCursor);
        else if (h == "t" || h == "b") setCursor(Qt::SizeVerCursor);
        else if (h == "l" || h == "r") setCursor(Qt::SizeHorCursor);
        else setCursor(h.endsWith("tr") || h.endsWith("bl") ? Qt::SizeBDiagCursor : Qt::SizeFDiagCursor);
        return;
    }
    if ((e->buttons() & Qt::LeftButton) == 0) return;
    updateGesture(e->position(), e->modifiers());
    applyPreview();
}

void PreviewCanvas::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || grab_.isEmpty()) return;
    const auto change = change_;
    const std::string id = clipId_;
    const QString label = label_;
    grab_.clear();
    clip0_.reset();
    live_.reset();
    box0_.reset();
    change_ = nullptr;
    info_.clear();
    if (change) {
        if (!edit_->modifyClip(id, label.toStdString(), change)) doc_->cancelPreview();
    }
    update();
}

void PreviewCanvas::leaveEvent(QEvent*) {
    if (grab_.isEmpty()) unsetCursor();
}

// ---- painting --------------------------------------------------------------------------------

void PreviewCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x0C, 0x0D, 0x10));
    const QRectF fr = frameRect();
    if (!image_.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(fr, image_);
    }
    if (compare_ && !image_.isNull()) {   // before (left, no effects) | after (right)
        const double x = fr.left() + fr.width() / 2;
        p.setPen(QPen(QColor(255, 255, 255, 220), 1.5));
        p.drawLine(QPointF(x, fr.top()), QPointF(x, fr.bottom()));
        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        const QFontMetrics fm(f);
        for (const auto& [text, right] : {std::pair<QString, bool>{"Before", false}, {"After", true}}) {
            const int tw = fm.horizontalAdvance(text) + 12;
            const QRectF box(right ? x + 8 : x - 8 - tw, fr.top() + 8, tw, fm.height() + 4);
            p.fillRect(box, QColor(0, 0, 0, 150));
            p.setPen(Qt::white);
            p.drawText(box, Qt::AlignCenter, text);
        }
    }
    if (const auto tg = target()) {
        const Clip& c = live_ ? *live_ : *tg->clip;
        p.setRenderHint(QPainter::Antialiasing);
        p.save();
        p.setClipRect(rect());
        const LayerGeo g = layerGeo(c, *tg);
        QPolygonF quad;
        for (const auto& q : g.corners) quad << toWidget(q);
        const bool tracking = trackingMode(*tg);
        const Mask* mk = tracking ? nullptr : targetMask(c);
        p.setBrush(Qt::NoBrush);
        p.setPen(mk || tracking ? QPen(QColor(255, 255, 255, 90), 1, Qt::DashLine) : QPen(color::accent, 1.5));
        if (!tg->adjustment || mk) p.drawPolygon(quad);
        const auto hs = handles();
        auto square = [&](const QPointF& c0) {
            p.setPen(QPen(color::accent, 1.5));
            p.setBrush(QColor(255, 255, 255));
            p.drawRect(QRectF(c0.x() - kHandleR, c0.y() - kHandleR, 2 * kHandleR, 2 * kHandleR));
        };
        auto rotateKnob = [&](const QString& name, const QPointF& from) {
            const auto it = hs.find(name);
            if (it == hs.end()) return;
            p.setPen(QPen(color::accent, 1.2));
            p.drawLine(from, it->second);
            p.setBrush(color::accent);
            p.drawEllipse(it->second, kHandleR, kHandleR);
        };
        if (tracking) {
            if (const auto r = trackedRegion(c, *tg)) {   // where the analysis found the object on this frame
                QPolygonF tr;
                for (const QPointF& f : {r->topLeft(), r->topRight(), r->bottomRight(), r->bottomLeft()})
                    tr << toWidget(boxToSeq(c, *tg, f.x(), f.y()));
                p.setPen(QPen(QColor(110, 220, 140), 1.5, Qt::DashLine));
                p.setBrush(Qt::NoBrush);
                p.drawPolygon(tr);
            }
            if (!hs.empty()) {
                QPolygonF box;
                for (const char* n : {"k:tl", "k:tr", "k:br", "k:bl"}) box << hs.at(n);
                p.setPen(QPen(color::accentBright, 2));
                p.setBrush(QColor(242, 169, 59, 30));
                p.drawPolygon(box);
                for (const char* n : {"k:tl", "k:tr", "k:br", "k:bl"}) square(hs.at(n));
                const QPointF cc = hs.at("k:center");
                p.setPen(QPen(QColor(255, 255, 255, 220), 1.2));
                p.drawLine(cc - QPointF(6, 0), cc + QPointF(6, 0));
                p.drawLine(cc - QPointF(0, 6), cc + QPointF(0, 6));
            }
        } else if (mk) {
            const MaskGeo mg = maskGeo(c, *mk, *tg);
            QPolygonF outline;
            for (const auto& q : mg.outline) outline << toWidget(q);
            p.setPen(QPen(color::accentBright, 2));
            p.setBrush(QColor(242, 169, 59, 28));
            p.drawPolygon(outline);
            QPolygonF box;
            for (const auto& q : mg.corners) box << toWidget(q);
            if (!hs.empty()) {
                p.setPen(QPen(QColor(255, 255, 255, 140), 1, Qt::DotLine));
                p.setBrush(Qt::NoBrush);
                p.drawPolygon(box);
                for (const char* n : {"m:tl", "m:tr", "m:br", "m:bl"}) square(hs.at(n));
                rotateKnob("m:rotate", (box[0] + box[1]) / 2);
                for (size_t i = 0; i < mg.points.size(); ++i) {
                    p.setPen(QPen(QColor(20, 20, 24), 1.2));
                    p.setBrush(color::accentBright);
                    p.drawEllipse(hs.at(QString("m:p%1").arg(i)), kHandleR, kHandleR);
                }
                const QPointF cc = hs.at("m:center");
                p.setPen(QPen(QColor(255, 255, 255, 200), 1.2));
                p.drawLine(cc - QPointF(5, 0), cc + QPointF(5, 0));
                p.drawLine(cc - QPointF(0, 5), cc + QPointF(0, 5));
            }
        } else if (!hs.empty()) {
            for (const char* n : {"tl", "tr", "br", "bl"}) square(hs.at(n));
            for (const char* n : {"t", "r", "b", "l"})
                if (hs.count(n)) square(hs.at(n));
            rotateKnob("rotate", (quad[0] + quad[1]) / 2);
            const QPointF an = hs.at("anchor");
            p.setPen(QPen(QColor(255, 255, 255), 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(an, 4.5, 4.5);
            p.drawLine(an - QPointF(8, 0), an + QPointF(8, 0));
            p.drawLine(an - QPointF(0, 8), an + QPointF(0, 8));
        }
        if (!info_.isEmpty()) {
            QFont f = font();
            f.setPointSizeF(f.pointSizeF() * 0.9);
            p.setFont(f);
            const QRectF tr = p.fontMetrics().boundingRect(info_).adjusted(-6, -3, 6, 3);
            QRectF box(infoPos_ + QPointF(16, 18), tr.size());
            if (box.right() > width()) box.moveRight(infoPos_.x() - 10);
            if (box.bottom() > height()) box.moveBottom(infoPos_.y() - 10);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 190));
            p.drawRoundedRect(box, 4, 4);
            p.setPen(QColor(240, 240, 240));
            p.drawText(box, Qt::AlignCenter, info_);
        }
        p.restore();
    }
    if (!offline_.isEmpty()) {
        p.setPen(QColor(255, 210, 210));
        p.fillRect(QRect(0, height() - 26 - 18 * (offline_.size() - 1), width(), 26 + 18 * (offline_.size() - 1)),
                   QColor(120, 20, 30, 200));
        p.drawText(rect().adjusted(8, 0, -8, -6), Qt::AlignBottom | Qt::AlignLeft, "Offline / missing: " + offline_.join("\n"));
    }
}

}  // namespace fa::ui
