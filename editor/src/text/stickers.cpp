// M9 stickers: every picture is drawn here with QPainter (original designs, no bitmaps). Square stickers are drawn
// in a 1000 x 1000 design space scaled to the picture; full-frame ones in picture pixels. Animation is a function of
// the time into the clip, so preview and export draw the same frame for the same time.
#include "fa/text/stickers.h"

#include <QFont>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <numbers>
#include <tuple>

namespace fa::text {

namespace {

constexpr double kPi = std::numbers::pi;

// ---- small helpers ------------------------------------------------------------------------------

double clamp01(double x) { return std::clamp(x, 0.0, 1.0); }
double easeOutCubic(double x) { x = clamp01(x); return 1 - std::pow(1 - x, 3); }
double easeInOut(double x) { x = clamp01(x); return x < 0.5 ? 4 * x * x * x : 1 - std::pow(-2 * x + 2, 3) / 2; }
double easeOutBack(double x) {
    x = clamp01(x);
    const double c1 = 1.70158, c3 = c1 + 1;
    return 1 + c3 * std::pow(x - 1, 3) + c1 * std::pow(x - 1, 2);
}
// deterministic pseudo-random numbers in [0, 1) (particles look the same in preview and export)
double rnd(int i, int k) {
    uint32_t h = static_cast<uint32_t>(i) * 0x9E3779B1u ^ static_cast<uint32_t>(k + 17) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return (h & 0xFFFFFF) / double(0x1000000);
}
QColor rgb(uint32_t c) { return QColor((c >> 16) & 255, (c >> 8) & 255, c & 255); }
QColor tintOr(const StickerData& s, QColor def) { return s.color ? rgb(s.color) : def; }
QString textOr(const StickerData& s, const StickerInfo& info) {
    return QString::fromStdString(s.text.empty() ? info.defaultText : s.text);
}
QFont boldFont(double px, int weight = QFont::Black) {
    QFont f("Segoe UI");
    f.setStyleHint(QFont::SansSerif);
    f.setWeight(static_cast<QFont::Weight>(weight));
    f.setPixelSize(std::max(1, static_cast<int>(px)));
    return f;
}
// the first `frac` of a path (for "drawn on" lines and arrows)
QPainterPath partial(const QPainterPath& p, double frac) {
    QPainterPath out;
    frac = clamp01(frac);
    if (frac <= 0) return out;
    const int n = 160;
    const int last = std::max(1, static_cast<int>(n * frac));
    out.moveTo(p.pointAtPercent(0));
    for (int i = 1; i <= last; ++i) out.lineTo(p.pointAtPercent(std::min(1.0, i / double(n))));
    out.lineTo(p.pointAtPercent(frac));
    return out;
}
// pop-in at the start of the clip: 0 -> 1.08 -> 1 within `len` seconds
double popIn(double t, double len = 0.45) { return t >= len ? 1.0 : easeOutBack(t / len); }
void scaleAbout(QPainter& p, double k, QPointF c = {500, 500}) {
    p.translate(c);
    p.scale(k, k);
    p.translate(-c);
}
QPainterPath star(QPointF c, double outer, double inner, int points = 5, double rot = -kPi / 2) {
    QPainterPath path;
    for (int i = 0; i < points * 2; ++i) {
        const double r = i % 2 ? inner : outer;
        const double a = rot + i * kPi / points;
        const QPointF q(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
        if (i == 0) path.moveTo(q);
        else path.lineTo(q);
    }
    path.closeSubpath();
    return path;
}
QPainterPath heart(QPointF c, double size) {
    QPainterPath h;
    const double s = size / 2;
    h.moveTo(c.x(), c.y() + s * 0.95);
    h.cubicTo(c.x() - s * 1.6, c.y() + s * 0.05, c.x() - s * 0.9, c.y() - s * 1.15, c.x(), c.y() - s * 0.42);
    h.cubicTo(c.x() + s * 0.9, c.y() - s * 1.15, c.x() + s * 1.6, c.y() + s * 0.05, c.x(), c.y() + s * 0.95);
    return h;
}
void softShadow(QPainter& p, const QPainterPath& path, double dy = 14, int alpha = 70) {
    p.save();
    p.translate(0, dy);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, alpha));
    p.drawPath(path);
    p.restore();
}
void outlinedText(QPainter& p, const QRectF& r, const QString& text, const QFont& f, QColor fill, QColor outline, double ow,
                  int flags = Qt::AlignCenter) {
    QPainterPath path;
    const QFontMetricsF fm(f);
    const double w = fm.horizontalAdvance(text);
    double x = r.center().x() - w / 2;
    if (flags & Qt::AlignLeft) x = r.left();
    const double y = r.center().y() + (fm.ascent() - fm.descent()) / 2;
    path.addText(QPointF(x, y), f, text);
    if (ow > 0) {
        p.setPen(QPen(outline, ow, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawPath(path);
}
// font size that fits `text` into `width`
double fitPx(const QString& text, double width, double maxPx, int weight = QFont::Black) {
    double px = maxPx;
    while (px > 8 && QFontMetricsF(boldFont(px, weight)).horizontalAdvance(text) > width) px *= 0.92;
    return px;
}

// ---- the drawings ---------------------------------------------------------------------------------
// Square stickers: painter in a 1000 x 1000 design space. Full-frame stickers: (w, h) pixels.

using Draw = std::function<void(QPainter&, const StickerData&, const StickerInfo&, double t, double len, double w, double h)>;

void emojiFace(QPainter& p, double t, const std::function<void(QPainter&)>& features, QColor face = QColor(255, 202, 40)) {
    scaleAbout(p, popIn(t) * (1 + 0.03 * std::sin(t * 4.0)));
    QPainterPath disc;
    disc.addEllipse(QPointF(500, 500), 400, 400);
    softShadow(p, disc, 18, 60);
    QRadialGradient g(QPointF(420, 380), 520);
    g.setColorAt(0, face.lighter(118));
    g.setColorAt(1, face.darker(112));
    p.setPen(QPen(face.darker(150), 14));
    p.setBrush(g);
    p.drawPath(disc);
    features(p);
}
void eyes(QPainter& p, double blink = 1.0) {
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(70, 45, 20));
    p.drawEllipse(QPointF(360, 420), 42, 62 * blink);
    p.drawEllipse(QPointF(640, 420), 42, 62 * blink);
}
void mouthArc(QPainter& p, double y, double w, double depth, double pen = 30) {
    QPainterPath m;
    m.moveTo(500 - w, y);
    m.quadTo(500, y + depth, 500 + w, y);
    p.setPen(QPen(QColor(70, 45, 20), pen, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawPath(m);
}

void thumbsUp(QPainter& p, QColor c) {
    QPainterPath hand;
    hand.addRoundedRect(QRectF(380, 430, 330, 380), 70, 70);   // fist
    QPainterPath thumb;
    thumb.moveTo(430, 450);
    thumb.cubicTo(420, 330, 470, 190, 560, 190);
    thumb.cubicTo(640, 190, 620, 330, 600, 450);
    thumb.closeSubpath();
    QPainterPath cuff;
    cuff.addRoundedRect(QRectF(250, 470, 140, 340), 30, 30);
    QPainterPath all = hand.united(thumb);
    softShadow(p, all.united(cuff));
    p.setPen(QPen(c.darker(150), 14, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QLinearGradient g(0, 190, 0, 810);
    g.setColorAt(0, c.lighter(120));
    g.setColorAt(1, c.darker(108));
    p.setBrush(g);
    p.drawPath(all);
    p.setBrush(c.darker(135));
    p.drawPath(cuff);
    p.setPen(QPen(c.darker(150), 10, Qt::SolidLine, Qt::RoundCap));
    for (int i = 0; i < 3; ++i) p.drawLine(QPointF(620, 540 + i * 85), QPointF(705, 540 + i * 85));   // fingers
}

void bell(QPainter& p, QColor c, double swing) {
    p.save();
    p.translate(500, 230);
    p.rotate(swing);
    p.translate(-500, -230);
    QPainterPath b;
    b.moveTo(320, 680);
    b.cubicTo(330, 620, 360, 560, 360, 460);
    b.cubicTo(360, 330, 430, 250, 500, 250);
    b.cubicTo(570, 250, 640, 330, 640, 460);
    b.cubicTo(640, 560, 670, 620, 680, 680);
    b.closeSubpath();
    softShadow(p, b);
    QLinearGradient g(300, 250, 700, 700);
    g.setColorAt(0, c.lighter(130));
    g.setColorAt(1, c.darker(115));
    p.setPen(QPen(c.darker(160), 12));
    p.setBrush(g);
    p.drawPath(b);
    p.drawEllipse(QPointF(500, 225), 34, 34);
    p.setBrush(c.darker(130));
    p.drawEllipse(QPointF(500, 720), 62, 46);
    p.restore();
}

void roundButton(QPainter& p, const QRectF& r, QColor fill, const QString& text, QColor textColor, double radius) {
    QPainterPath body;
    body.addRoundedRect(r, radius, radius);
    softShadow(p, body, 12, 70);
    QLinearGradient g(r.topLeft(), r.bottomLeft());
    g.setColorAt(0, fill.lighter(112));
    g.setColorAt(1, fill.darker(110));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawPath(body);
    const QFont f = boldFont(fitPx(text, r.width() * 0.8, r.height() * 0.5));
    outlinedText(p, r, text, f, textColor, QColor(0, 0, 0, 0), 0);
}

void cursorHand(QPainter& p, QPointF tip, double scale, double pressed) {
    p.save();
    p.translate(tip);
    p.scale(scale * (1 - 0.12 * pressed), scale * (1 - 0.12 * pressed));
    QPainterPath arrow;
    arrow.moveTo(0, 0);
    arrow.lineTo(0, 120);
    arrow.lineTo(32, 92);
    arrow.lineTo(56, 146);
    arrow.lineTo(78, 136);
    arrow.lineTo(54, 84);
    arrow.lineTo(96, 84);
    arrow.closeSubpath();
    p.setPen(QPen(QColor(20, 20, 20), 8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::white);
    p.drawPath(arrow);
    p.restore();
}

void arrowHead(QPainter& p, QPointF tip, double angle, double size, QColor c) {
    QPainterPath h;
    h.moveTo(tip);
    h.lineTo(tip + QPointF(std::cos(angle + 2.6) * size, std::sin(angle + 2.6) * size));
    h.lineTo(tip + QPointF(std::cos(angle - 2.6) * size, std::sin(angle - 2.6) * size));
    h.closeSubpath();
    p.setPen(QPen(c, 10, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(c);
    p.drawPath(h);
}
// a path drawn on over `drawSeconds`, with an arrow head at its end once complete
void drawnArrow(QPainter& p, const QPainterPath& path, double t, QColor c, double width, double drawSeconds = 0.7, bool head = true) {
    const double frac = easeOutCubic(t / drawSeconds);
    QPainterPath part = partial(path, frac);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 60), width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.save();
    p.translate(0, 10);
    p.drawPath(part);
    p.restore();
    p.setPen(QPen(c, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(part);
    if (head && frac > 0.02) {
        const QPointF a = path.pointAtPercent(std::max(0.0, frac - 0.02)), b = path.pointAtPercent(frac);
        arrowHead(p, b, std::atan2(b.y() - a.y(), b.x() - a.x()), width * 2.6, c);
    }
}

void burstShape(QPainter& p, QPointF c, double r, int spikes, QColor fill, QColor edge, double wobble) {
    QPainterPath b;
    for (int i = 0; i < spikes * 2; ++i) {
        const double rr = (i % 2 ? r * 0.72 : r) * (1 + wobble * std::sin(i * 1.7));
        const double a = i * kPi / spikes;
        const QPointF q(c.x() + rr * std::cos(a), c.y() + rr * std::sin(a));
        if (!i) b.moveTo(q);
        else b.lineTo(q);
    }
    b.closeSubpath();
    softShadow(p, b);
    p.setPen(QPen(edge, 16, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(fill);
    p.drawPath(b);
}

void sparkle(QPainter& p, QPointF c, double r, QColor col, double alpha) {
    if (r <= 0 || alpha <= 0) return;
    QColor k = col;
    k.setAlphaF(clamp01(alpha));
    QPainterPath s;
    s.moveTo(c.x(), c.y() - r);
    s.quadTo(c.x(), c.y(), c.x() + r, c.y());
    s.quadTo(c.x(), c.y(), c.x(), c.y() + r);
    s.quadTo(c.x(), c.y(), c.x() - r, c.y());
    s.quadTo(c.x(), c.y(), c.x(), c.y() - r);
    p.setPen(Qt::NoPen);
    p.setBrush(k);
    p.drawPath(s);
    QRadialGradient g(c, r * 0.6);
    QColor glow = col;
    glow.setAlphaF(clamp01(alpha) * 0.6);
    g.setColorAt(0, glow);
    g.setColorAt(1, QColor(0, 0, 0, 0));
    p.setBrush(g);
    p.drawEllipse(c, r * 0.6, r * 0.6);
}

void fireworkBurst(QPainter& p, QPointF c, double r, double phase, QColor col, int seed) {
    // phase 0..1: rays fly out, then fade with falling sparks
    const int rays = 18;
    const double grow = easeOutCubic(phase / 0.45);
    const double fade = phase < 0.45 ? 1.0 : 1.0 - (phase - 0.45) / 0.55;
    for (int i = 0; i < rays; ++i) {
        const double a = i * 2 * kPi / rays + rnd(seed, i) * 0.2;
        const double len = r * (0.75 + 0.25 * rnd(seed, i + 40)) * grow;
        const double fall = phase > 0.45 ? (phase - 0.45) * r * 0.25 : 0.0;
        const QPointF end(c.x() + std::cos(a) * len, c.y() + std::sin(a) * len + fall);
        const QPointF start(c.x() + std::cos(a) * len * 0.55, c.y() + std::sin(a) * len * 0.55 + fall * 0.5);
        QColor k = i % 3 == 0 ? col.lighter(140) : col;
        k.setAlphaF(clamp01(fade));
        p.setPen(QPen(k, r * 0.035, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(start, end);
        sparkle(p, end, r * 0.06, Qt::white, fade);
    }
}

void gradientFill(QPainter& p, double w, double h, QColor a, QColor b, double t, bool animated) {
    const double shift = animated ? 0.15 * std::sin(t * 0.6) : 0.0;
    QLinearGradient g(QPointF(w * (0.0 + shift), 0), QPointF(w * (1.0 + shift), h));
    g.setColorAt(0, a);
    g.setColorAt(1, b);
    p.fillRect(QRectF(0, 0, w, h), g);
    QRadialGradient v(QPointF(w / 2, h / 2), std::hypot(w, h) * 0.6);   // soft vignette
    v.setColorAt(0.55, QColor(0, 0, 0, 0));
    v.setColorAt(1, QColor(0, 0, 0, 110));
    p.fillRect(QRectF(0, 0, w, h), v);
}

// ---- M11 shapes ---------------------------------------------------------------------------------------------------

// a puffy cloud centred on c, about 1.8 s wide
QPainterPath cloudPath(QPointF c, double s) {
    QPainterPath path;
    path.addEllipse(QPointF(c.x() - 0.55 * s, c.y() + 0.1 * s), 0.38 * s, 0.32 * s);
    path.addEllipse(QPointF(c.x() - 0.1 * s, c.y() - 0.2 * s), 0.5 * s, 0.45 * s);
    path.addEllipse(QPointF(c.x() + 0.45 * s, c.y()), 0.42 * s, 0.38 * s);
    path.addRoundedRect(QRectF(c.x() - 0.9 * s, c.y(), 1.75 * s, 0.42 * s), 0.21 * s, 0.21 * s);
    path.setFillRule(Qt::WindingFill);
    return path.simplified();
}
void drawCloud(QPainter& p, QPointF c, double s, QColor top, QColor bottom) {
    const QPainterPath cl = cloudPath(c, s);
    softShadow(p, cl, 12, 45);
    QLinearGradient g(c.x(), c.y() - 0.7 * s, c.x(), c.y() + 0.45 * s);
    g.setColorAt(0, top);
    g.setColorAt(1, bottom);
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawPath(cl);
}
// an eighth note: head at h, stem up
void eighthNote(QPainter& p, QPointF h, double s, QColor c) {
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.translate(h);
    p.rotate(-22);
    p.drawEllipse(QPointF(0, 0), 0.62 * s, 0.44 * s);
    p.restore();
    p.setPen(QPen(c, 0.16 * s, Qt::SolidLine, Qt::FlatCap));
    p.drawLine(QPointF(h.x() + 0.52 * s, h.y() - 0.1 * s), QPointF(h.x() + 0.52 * s, h.y() - 2.2 * s));
    QPainterPath flag;
    flag.moveTo(h.x() + 0.52 * s, h.y() - 2.2 * s);
    flag.cubicTo(h.x() + 0.9 * s, h.y() - 1.6 * s, h.x() + 1.5 * s, h.y() - 1.5 * s, h.x() + 1.2 * s, h.y() - 0.8 * s);
    p.setPen(QPen(c, 0.18 * s, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawPath(flag);
}
// expanding, fading arcs (sound, signal) on the right of c (side -1: left)
void soundArcs(QPainter& p, QPointF c, double r0, double t, QColor col, int side = 1) {
    for (int i = 0; i < 3; ++i) {
        const double ph = std::fmod(t * 1.2 + i / 3.0, 1.0);
        QColor k = col;
        k.setAlphaF(clamp01(1 - ph) * 0.9);
        p.setPen(QPen(k, 26, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        const double r = r0 + ph * 160;
        const QRectF box(c.x() - r, c.y() - r, 2 * r, 2 * r);
        p.drawArc(box, side > 0 ? -40 * 16 : 140 * 16, 80 * 16);
    }
}

struct Entry {
    StickerInfo info;
    Draw draw;
};

std::vector<Entry> buildCatalog() {
    std::vector<Entry> v;
    auto add = [&](StickerInfo i, Draw d) { v.push_back({std::move(i), std::move(d)}); };
    auto sq = [](std::string id, std::string name, std::string cat, bool anim, bool tint = false, std::string text = {},
                 double scale = 0.45) {
        StickerInfo i;
        i.id = std::move(id);
        i.name = std::move(name);
        i.category = std::move(cat);
        i.animated = anim;
        i.tintable = tint;
        i.defaultText = std::move(text);
        i.defaultScale = scale;
        return i;
    };
    auto full = [](std::string id, std::string name, std::string cat, bool anim, bool tint = false, std::string text = {}) {
        StickerInfo i;
        i.id = std::move(id);
        i.name = std::move(name);
        i.category = std::move(cat);
        i.fullFrame = true;
        i.animated = anim;
        i.tintable = tint;
        i.defaultText = std::move(text);
        i.defaultScale = 1.0;
        return i;
    };

    // ---- Trending
    add(sq("trend.sparkle_burst", "Sparkle Burst", "Trending", true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        const QColor gold = tintOr(s, QColor(255, 206, 84));
        for (int i = 0; i < 12; ++i) {
            const double a = i * kPi / 6 + t * 0.4;
            const double pulse = 0.5 + 0.5 * std::sin(t * 3 + i);
            const double r1 = 160, r2 = 300 + 110 * pulse;
            QColor k = gold;
            k.setAlphaF(0.5 + 0.5 * pulse);
            p.setPen(QPen(k, 18, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(500 + std::cos(a) * r1, 500 + std::sin(a) * r1), QPointF(500 + std::cos(a) * r2, 500 + std::sin(a) * r2));
        }
        sparkle(p, {500, 500}, 190 * popIn(t), gold.lighter(130), 1);
        for (int i = 0; i < 6; ++i)
            sparkle(p, {500 + 380 * (rnd(i, 1) - 0.5) * 2, 500 + 380 * (rnd(i, 2) - 0.5) * 2}, 60,
                    Qt::white, 0.5 + 0.5 * std::sin(t * 4 + i * 1.3));
    });
    add(sq("trend.vinyl", "Vinyl Record", "Trending", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        p.save();
        p.translate(500, 500);
        p.rotate(t * 120);
        p.translate(-500, -500);
        QPainterPath disc;
        disc.addEllipse(QPointF(500, 500), 420, 420);
        softShadow(p, disc, 16, 80);
        QRadialGradient g(QPointF(500, 500), 420);
        g.setColorAt(0, QColor(40, 40, 44));
        g.setColorAt(1, QColor(12, 12, 14));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawPath(disc);
        p.setBrush(Qt::NoBrush);
        for (int r = 180; r < 410; r += 22) {
            p.setPen(QPen(QColor(255, 255, 255, 18 + (r % 3) * 6), 3));
            p.drawEllipse(QPointF(500, 500), r, r);
        }
        p.setPen(QPen(QColor(255, 255, 255, 60), 10, Qt::SolidLine, Qt::RoundCap));   // highlight
        p.drawArc(QRectF(150, 150, 700, 700), 30 * 16, 40 * 16);
        p.setPen(Qt::NoPen);
        p.setBrush(tintOr(s, QColor(228, 72, 64)));
        p.drawEllipse(QPointF(500, 500), 150, 150);
        p.setBrush(QColor(255, 236, 200));
        p.drawEllipse(QPointF(500, 445), 45, 18);
        p.setBrush(QColor(10, 10, 10));
        p.drawEllipse(QPointF(500, 500), 22, 22);
        p.restore();
    });
    add(sq("trend.equalizer", "Equalizer", "Trending", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        const QColor a = tintOr(s, QColor(64, 220, 200)), b = a.lighter(150);
        const int n = 11;
        for (int i = 0; i < n; ++i) {
            const double level = 0.25 + 0.75 * std::abs(std::sin(t * (2.2 + 0.37 * i) + i * 0.9)) * popIn(t);
            const double hgt = 620 * level, x = 140 + i * 66;
            QLinearGradient g(0, 810 - hgt, 0, 810);
            g.setColorAt(0, b);
            g.setColorAt(1, a);
            QPainterPath bar;
            bar.addRoundedRect(QRectF(x, 810 - hgt, 46, hgt), 20, 20);
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawPath(bar);
        }
    });
    add(sq("trend.neon_ring", "Neon Ring", "Trending", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        const QColor c = tintOr(s, QColor(70, 190, 255));
        scaleAbout(p, popIn(t));
        for (int g = 6; g >= 1; --g) {   // glow
            QColor k = c;
            k.setAlpha(18);
            p.setPen(QPen(k, 18 + g * 16));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QPointF(500, 500), 340, 340);
        }
        p.setPen(QPen(c.lighter(160), 22));
        p.drawEllipse(QPointF(500, 500), 340, 340);
        QPen dash(Qt::white, 10, Qt::DashLine, Qt::RoundCap);
        dash.setDashOffset(-t * 6);
        p.setPen(dash);
        p.drawEllipse(QPointF(500, 500), 300, 300);
    });
    add(full("trend.bokeh", "Bokeh Lights", "Trending", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double w, double h) {
        const QColor c = tintOr(s, QColor(255, 196, 110));
        for (int i = 0; i < 26; ++i) {
            const double r = (0.03 + 0.07 * rnd(i, 3)) * h;
            const double x = std::fmod(rnd(i, 1) * w + t * (8 + 20 * rnd(i, 4)), w + 2 * r) - r;
            const double y = rnd(i, 2) * h + std::sin(t * 0.7 + i) * h * 0.02;
            QRadialGradient g(QPointF(x, y), r);
            QColor k = c;
            k.setAlphaF(0.18 + 0.25 * (0.5 + 0.5 * std::sin(t * 1.5 + i)));
            g.setColorAt(0, k);
            g.setColorAt(0.8, QColor(k.red(), k.green(), k.blue(), k.alpha() / 2));
            g.setColorAt(1, QColor(0, 0, 0, 0));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawEllipse(QPointF(x, y), r, r);
        }
    });
    add(full("trend.light_rays", "Light Rays", "Trending", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double w, double h) {
        const QColor c = tintOr(s, QColor(255, 230, 170));
        for (int i = 0; i < 6; ++i) {
            const double x = w * (0.1 + 0.16 * i) + std::sin(t * 0.5 + i) * w * 0.02;
            QPainterPath beam;
            beam.moveTo(x, -h * 0.05);
            beam.lineTo(x + w * 0.06, -h * 0.05);
            beam.lineTo(x + w * 0.22, h * 1.05);
            beam.lineTo(x + w * 0.10, h * 1.05);
            beam.closeSubpath();
            QLinearGradient g(0, 0, 0, h);
            QColor k = c;
            k.setAlphaF(0.10 + 0.08 * (0.5 + 0.5 * std::sin(t * 1.2 + i * 2)));
            g.setColorAt(0, k);
            g.setColorAt(1, QColor(0, 0, 0, 0));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawPath(beam);
        }
    });

    // ---- Subscribe
    add(sq("sub.button", "Subscribe Button", "Subscribe", true, true, "SUBSCRIBE", 0.5), [](QPainter& p, const StickerData& s, const StickerInfo& info, double t, double, double, double) {
        // pops in, the pointer clicks it at 1.2 s and it turns into "SUBSCRIBED"
        scaleAbout(p, popIn(t));
        const bool done = t > 1.3;
        const double press = std::clamp(1.0 - std::abs(t - 1.25) / 0.1, 0.0, 1.0);
        const QRectF r(110, 380, 780, 240);
        p.save();
        scaleAbout(p, 1 - 0.05 * press, r.center());
        roundButton(p, r, done ? QColor(110, 110, 116) : tintOr(s, QColor(222, 45, 45)),
                    done ? QStringLiteral("SUBSCRIBED") : textOr(s, info), Qt::white, 40);
        p.restore();
        if (t > 0.5) {
            const double in = easeOutCubic((t - 0.5) / 0.6);
            cursorHand(p, QPointF(720 + (1 - in) * 220, 520 + (1 - in) * 260), 1.6, press);
        }
    });
    add(sq("sub.pill", "Subscribe Pill", "Subscribe", true, true, "SUBSCRIBE", 0.5), [](QPainter& p, const StickerData& s, const StickerInfo& info, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const QRectF r(90, 400, 820, 200);
        QPainterPath pill;
        pill.addRoundedRect(r, 100, 100);
        softShadow(p, pill);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(28, 30, 36, 235));
        p.drawPath(pill);
        p.setBrush(tintOr(s, QColor(232, 52, 52)));
        p.drawEllipse(QPointF(r.left() + 100, r.center().y()), 74, 74);
        p.setBrush(Qt::white);   // a play triangle in the round badge
        QPainterPath tri;
        tri.moveTo(r.left() + 82, r.center().y() - 34);
        tri.lineTo(r.left() + 82, r.center().y() + 34);
        tri.lineTo(r.left() + 128, r.center().y());
        tri.closeSubpath();
        p.drawPath(tri);
        const QRectF tr(r.left() + 200, r.top(), r.width() - 240, r.height());
        const QString text = textOr(s, info);
        outlinedText(p, tr, text, boldFont(fitPx(text, tr.width(), 96)), Qt::white, QColor(0, 0, 0, 0), 0);
    });
    add(sq("sub.bell", "Notification Bell", "Subscribe", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const double swing = 18 * std::sin(t * 9) * std::exp(-std::fmod(t, 2.0) * 1.5);
        bell(p, tintOr(s, QColor(255, 200, 50)), swing);
        // ring waves
        for (int i = 0; i < 2; ++i) {
            const double ph = std::fmod(t * 1.2 + i * 0.5, 1.0);
            QColor k(255, 255, 255, static_cast<int>(200 * (1 - ph)));
            p.setPen(QPen(k, 16, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            const double r = 280 + ph * 120;
            p.drawArc(QRectF(500 - r, 450 - r, 2 * r, 2 * r), 20 * 16, 40 * 16);
            p.drawArc(QRectF(500 - r, 450 - r, 2 * r, 2 * r), 120 * 16, 40 * 16);
        }
    });
    add(sq("sub.follow", "Subscribe + Follow", "Subscribe", true, true, "Follow For More", 0.55), [](QPainter& p, const StickerData& s, const StickerInfo& info, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const QColor red = tintOr(s, QColor(225, 42, 42));
        outlinedText(p, QRectF(60, 320, 880, 230), "SUBSCRIBE", boldFont(fitPx("SUBSCRIBE", 860, 210)), Qt::white, red.darker(140), 26);
        const double slide = easeOutCubic((t - 0.35) / 0.5);
        if (slide > 0) {
            const QRectF band(110 + (1 - slide) * -300, 560, 640, 120);
            p.setPen(Qt::NoPen);
            p.setBrush(red);
            QPainterPath ribbon;
            ribbon.moveTo(band.left() + 30, band.top());
            ribbon.lineTo(band.right(), band.top());
            ribbon.lineTo(band.right() - 30, band.bottom());
            ribbon.lineTo(band.left(), band.bottom());
            ribbon.closeSubpath();
            p.drawPath(ribbon);
            const QString text = textOr(s, info);
            outlinedText(p, band, text, boldFont(fitPx(text, band.width() * 0.85, 70), QFont::Bold), Qt::white, QColor(0, 0, 0, 0), 0);
        }
    });
    add(sq("sub.actions", "Like / Comment / Share", "Subscribe", true), [](QPainter& p, const StickerData&, const StickerInfo&, double t, double, double, double) {
        const QColor cols[3] = {QColor(232, 60, 60), QColor(60, 140, 240), QColor(40, 190, 120)};
        for (int i = 0; i < 3; ++i) {
            const double k = popIn(t - i * 0.18);
            const QPointF c(230 + i * 270, 500);
            p.save();
            scaleAbout(p, std::max(0.0, k), c);
            QPainterPath disc;
            disc.addEllipse(c, 115, 115);
            softShadow(p, disc);
            p.setPen(Qt::NoPen);
            p.setBrush(cols[i]);
            p.drawPath(disc);
            p.setBrush(Qt::white);
            p.setPen(QPen(Qt::white, 16, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            if (i == 0) p.drawPath(heart(c + QPointF(0, 8), 120));
            if (i == 1) {   // comment bubble
                QPainterPath b;
                b.addRoundedRect(QRectF(c.x() - 60, c.y() - 50, 120, 85), 22, 22);
                b.moveTo(c.x() - 25, c.y() + 30);
                b.lineTo(c.x() - 45, c.y() + 62);
                b.lineTo(c.x() + 5, c.y() + 34);
                p.drawPath(b);
            }
            if (i == 2) {   // share arrow
                p.setBrush(Qt::NoBrush);
                QPainterPath a;
                a.moveTo(c.x() - 55, c.y() + 45);
                a.quadTo(c.x() - 40, c.y() - 30, c.x() + 30, c.y() - 30);
                p.drawPath(a);
                arrowHead(p, QPointF(c.x() + 58, c.y() - 30), 0, 45, Qt::white);
            }
            p.restore();
        }
    });

    // ---- Arrow
    add(sq("arrow.straight", "Straight Arrow", "Arrow", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        QPainterPath a;
        a.moveTo(150, 650);
        a.lineTo(820, 350);
        drawnArrow(p, a, t, tintOr(s, QColor(255, 210, 60)), 50);
    });
    add(sq("arrow.curved", "Curved Arrow", "Arrow", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        QPainterPath a;
        a.moveTo(160, 760);
        a.cubicTo(180, 300, 560, 180, 820, 330);
        drawnArrow(p, a, t, tintOr(s, QColor(255, 255, 255)), 44);
    });
    add(sq("arrow.hand", "Hand-drawn Arrow", "Arrow", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        QPainterPath a;
        a.moveTo(140, 420);
        a.cubicTo(260, 300, 300, 640, 440, 560);
        a.cubicTo(560, 490, 520, 360, 640, 400);
        a.cubicTo(720, 430, 760, 520, 840, 560);
        drawnArrow(p, a, t, tintOr(s, QColor(255, 80, 90)), 34, 0.9);
    });
    add(sq("arrow.double", "Double Arrow", "Arrow", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        const QColor c = tintOr(s, QColor(80, 200, 255));
        QPainterPath a;
        a.moveTo(500, 500);
        a.lineTo(860, 500);
        QPainterPath b;
        b.moveTo(500, 500);
        b.lineTo(140, 500);
        drawnArrow(p, a, t, c, 46, 0.5);
        drawnArrow(p, b, t, c, 46, 0.5);
    });
    add(sq("arrow.circular", "Circular Arrow", "Arrow", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        QPainterPath a;
        a.arcMoveTo(QRectF(180, 180, 640, 640), 200);
        a.arcTo(QRectF(180, 180, 640, 640), 200, -300);
        drawnArrow(p, a, t, tintOr(s, QColor(120, 230, 120)), 46, 0.8);
    });
    add(sq("arrow.chevrons", "Chevrons", "Arrow", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        const QColor c = tintOr(s, QColor(255, 255, 255));
        for (int i = 0; i < 3; ++i) {
            const double a = 0.25 + 0.75 * (0.5 + 0.5 * std::sin(t * 6 - i * 1.1));
            QColor k = c;
            k.setAlphaF(a);
            p.setPen(QPen(k, 60, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            const double x = 260 + i * 190;
            QPainterPath ch;
            ch.moveTo(x, 300);
            ch.lineTo(x + 140, 500);
            ch.lineTo(x, 700);
            p.drawPath(ch);
        }
    });

    // ---- Line
    add(sq("line.underline", "Brush Underline", "Line", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        QPainterPath a;
        a.moveTo(80, 560);
        a.cubicTo(300, 470, 650, 470, 920, 520);
        p.setPen(QPen(tintOr(s, QColor(255, 214, 0)), 70, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(partial(a, easeOutCubic(t / 0.6)));
    });
    add(sq("line.circle", "Circle Highlight", "Line", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        QPainterPath a;   // a hand-drawn loop that overshoots its start
        a.moveTo(560, 160);
        a.cubicTo(860, 170, 900, 760, 520, 830);
        a.cubicTo(140, 880, 100, 300, 420, 190);
        a.cubicTo(520, 160, 640, 170, 700, 230);
        p.setPen(QPen(tintOr(s, QColor(255, 70, 70)), 30, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(partial(a, easeOutCubic(t / 0.8)));
    });
    add(sq("line.box", "Box Frame", "Line", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        QPainterPath a;
        a.addRoundedRect(QRectF(130, 230, 740, 540), 30, 30);
        p.setPen(QPen(tintOr(s, QColor(255, 255, 255)), 22, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(partial(a, easeOutCubic(t / 0.8)));
    });
    add(sq("line.zigzag", "Zigzag", "Line", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        QPainterPath a;
        a.moveTo(100, 500);
        for (int i = 1; i <= 8; ++i) a.lineTo(100 + i * 100, i % 2 ? 400 : 600);
        p.setPen(QPen(tintOr(s, QColor(80, 220, 255)), 32, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(partial(a, easeOutCubic(t / 0.7)));
    });
    add(sq("line.dots", "Dotted Divider", "Line", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        const QColor c = tintOr(s, QColor(255, 255, 255));
        for (int i = 0; i < 9; ++i) {
            const double k = popIn(t - i * 0.06, 0.3);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawEllipse(QPointF(100 + i * 100, 500), 26 * std::max(0.0, k), 26 * std::max(0.0, k));
        }
    });

    // ---- Emoji (original faces)
    add(sq("emoji.smile", "Smile", "Emoji", true), [](QPainter& p, const StickerData&, const StickerInfo&, double t, double, double, double) {
        emojiFace(p, t, [&](QPainter& q) {
            eyes(q, std::fmod(t, 3.0) > 2.85 ? 0.15 : 1.0);
            mouthArc(q, 600, 190, 150);
        });
    });
    add(sq("emoji.laugh", "Laugh", "Emoji", true), [](QPainter& p, const StickerData&, const StickerInfo&, double t, double, double, double) {
        p.save();
        p.translate(500, 500);
        p.rotate(6 * std::sin(t * 9));
        p.translate(-500, -500);
        emojiFace(p, t, [&](QPainter& q) {
            q.setPen(QPen(QColor(70, 45, 20), 30, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));   // squeezed eyes
            q.setBrush(Qt::NoBrush);
            for (int side : {-1, 1}) {
                QPainterPath e;
                e.moveTo(500 + side * 80, 400);
                e.lineTo(500 + side * 190, 440);
                e.lineTo(500 + side * 80, 480);
                q.drawPath(e);
            }
            QPainterPath m;
            m.moveTo(300, 560);
            m.lineTo(700, 560);
            m.quadTo(690, 790, 500, 790);
            m.quadTo(310, 790, 300, 560);
            q.setPen(QPen(QColor(70, 45, 20), 14));
            q.setBrush(QColor(120, 30, 30));
            q.drawPath(m);
            q.setPen(Qt::NoPen);
            q.setBrush(QColor(110, 190, 255));   // tears
            q.drawEllipse(QPointF(200, 520 + 30 * std::fmod(t * 1.5, 1.0)), 40, 60);
            q.drawEllipse(QPointF(800, 520 + 30 * std::fmod(t * 1.5 + 0.5, 1.0)), 40, 60);
        });
        p.restore();
    });
    add(sq("emoji.wow", "Wow", "Emoji", true), [](QPainter& p, const StickerData&, const StickerInfo&, double t, double, double, double) {
        emojiFace(p, t, [&](QPainter& q) {
            q.setPen(Qt::NoPen);
            q.setBrush(Qt::white);
            q.drawEllipse(QPointF(360, 410), 80, 90);
            q.drawEllipse(QPointF(640, 410), 80, 90);
            q.setBrush(QColor(70, 45, 20));
            q.drawEllipse(QPointF(360, 420), 36, 40);
            q.drawEllipse(QPointF(640, 420), 36, 40);
            const double o = 1 + 0.15 * std::sin(t * 5);
            q.drawEllipse(QPointF(500, 660), 70 * o, 90 * o);
        });
    });
    add(sq("emoji.love", "Love Eyes", "Emoji", true), [](QPainter& p, const StickerData&, const StickerInfo&, double t, double, double, double) {
        emojiFace(p, t, [&](QPainter& q) {
            const double beat = 1 + 0.12 * std::max(0.0, std::sin(t * 7));
            q.setPen(Qt::NoPen);
            q.setBrush(QColor(235, 40, 70));
            q.drawPath(heart(QPointF(360, 410), 170 * beat));
            q.drawPath(heart(QPointF(640, 410), 170 * beat));
            mouthArc(q, 610, 170, 140);
        });
    });
    add(sq("emoji.cool", "Cool", "Emoji", true), [](QPainter& p, const StickerData&, const StickerInfo&, double t, double, double, double) {
        emojiFace(p, t, [&](QPainter& q) {
            const double drop = easeOutCubic((t - 0.3) / 0.4);
            q.save();
            q.translate(0, (1 - drop) * -300);
            q.setPen(Qt::NoPen);
            q.setBrush(QColor(20, 20, 24));
            QPainterPath g;
            g.addRoundedRect(QRectF(220, 340, 250, 150), 40, 40);
            g.addRoundedRect(QRectF(530, 340, 250, 150), 40, 40);
            g.addRect(QRectF(460, 370, 80, 26));
            q.drawPath(g);
            q.setBrush(QColor(255, 255, 255, 90));
            q.drawRect(QRectF(260, 360, 70, 20));
            q.drawRect(QRectF(570, 360, 70, 20));
            q.restore();
            mouthArc(q, 640, 140, 70);
        });
    });
    add(sq("emoji.wink", "Wink", "Emoji", true), [](QPainter& p, const StickerData&, const StickerInfo&, double t, double, double, double) {
        emojiFace(p, t, [&](QPainter& q) {
            q.setPen(Qt::NoPen);
            q.setBrush(QColor(70, 45, 20));
            q.drawEllipse(QPointF(360, 420), 42, 62);
            const bool winking = std::fmod(t, 2.0) > 0.6;
            if (winking) mouthArc(q, 430, 80, -40, 26);   // closed eye
            else q.drawEllipse(QPointF(640, 420), 42, 62);
            QPainterPath m;
            m.moveTo(340, 620);
            m.quadTo(520, 760, 680, 580);
            q.setPen(QPen(QColor(70, 45, 20), 30, Qt::SolidLine, Qt::RoundCap));
            q.setBrush(Qt::NoBrush);
            q.drawPath(m);
        });
    });
    add(sq("emoji.sad", "Sad", "Emoji", true), [](QPainter& p, const StickerData&, const StickerInfo&, double t, double, double, double) {
        emojiFace(p, t, [&](QPainter& q) {
            eyes(q);
            mouthArc(q, 700, 150, -110);
            q.setPen(Qt::NoPen);
            q.setBrush(QColor(110, 190, 255));
            q.drawEllipse(QPointF(360, 520 + 120 * std::fmod(t * 0.8, 1.0)), 28, 44);
        });
    });
    add(sq("emoji.angry", "Angry", "Emoji", true), [](QPainter& p, const StickerData&, const StickerInfo&, double t, double, double, double) {
        p.save();
        p.translate(8 * std::sin(t * 40), 0);
        emojiFace(p, t, [&](QPainter& q) {
            eyes(q);
            q.setPen(QPen(QColor(70, 45, 20), 32, Qt::SolidLine, Qt::RoundCap));
            q.drawLine(QPointF(270, 300), QPointF(430, 360));
            q.drawLine(QPointF(730, 300), QPointF(570, 360));
            mouthArc(q, 700, 130, -80);
        }, QColor(255, 120, 60));
        p.restore();
    });

    // ---- Love
    add(sq("love.heart", "Beating Heart", "Love", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        const double beat = popIn(t) * (1 + 0.08 * std::max(0.0, std::sin(t * 7.5)) + 0.05 * std::max(0.0, std::sin(t * 7.5 - 1.2)));
        scaleAbout(p, beat);
        const QPainterPath h = heart(QPointF(500, 520), 700);
        softShadow(p, h);
        QRadialGradient g(QPointF(400, 380), 600);
        const QColor c = tintOr(s, QColor(232, 36, 76));
        g.setColorAt(0, c.lighter(135));
        g.setColorAt(1, c.darker(115));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawPath(h);
        p.setBrush(QColor(255, 255, 255, 110));
        p.drawEllipse(QPointF(340, 360), 60, 40);
    });
    add(sq("love.outline", "Heart Outline", "Love", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        p.setPen(QPen(tintOr(s, QColor(255, 255, 255)), 40, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(partial(heart(QPointF(500, 520), 700), easeOutCubic(t / 0.9)));
    });
    add(sq("love.floating", "Floating Hearts", "Love", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        const QColor c = tintOr(s, QColor(255, 70, 110));
        for (int i = 0; i < 9; ++i) {
            const double ph = std::fmod(t * 0.45 + rnd(i, 1), 1.0);
            const double x = 200 + 600 * rnd(i, 2) + 50 * std::sin(t * 2 + i);
            const double y = 900 - ph * 800;
            QColor k = c;
            k.setAlphaF(clamp01(std::sin(ph * kPi)));
            p.setPen(Qt::NoPen);
            p.setBrush(k);
            p.drawPath(heart(QPointF(x, y), 90 + 90 * rnd(i, 3)));
        }
    });
    add(sq("love.sparkle", "Heart Sparkle", "Love", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const QColor c = tintOr(s, QColor(255, 120, 170));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPath(heart(QPointF(500, 520), 520));
        for (int i = 0; i < 5; ++i) {
            const double a = i * 2 * kPi / 5 + t;
            sparkle(p, {500 + std::cos(a) * 360, 520 + std::sin(a) * 360}, 70, Qt::white, 0.5 + 0.5 * std::sin(t * 4 + i));
        }
    });

    // ---- Like
    add(sq("like.thumb", "Thumbs Up", "Like", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        const double bounce = popIn(t) * (1 + 0.06 * std::abs(std::sin(t * 4)));
        p.save();
        p.translate(500, 800);
        p.rotate(-8 * std::sin(std::min(t, 1.0) * kPi));
        p.translate(-500, -800);
        scaleAbout(p, bounce);
        thumbsUp(p, tintOr(s, QColor(255, 196, 60)));
        p.restore();
    });
    add(sq("like.stars", "Five Stars", "Like", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        const QColor c = tintOr(s, QColor(255, 200, 40));
        for (int i = 0; i < 5; ++i) {
            const double k = popIn(t - i * 0.15, 0.35);
            if (k <= 0) continue;
            const QPointF ctr(120 + i * 190, 500);
            p.save();
            scaleAbout(p, k, ctr);
            const QPainterPath st = star(ctr, 90, 40);
            softShadow(p, st, 8, 60);
            p.setPen(QPen(c.darker(140), 8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(c);
            p.drawPath(st);
            p.restore();
        }
    });
    add(sq("like.check", "Check Badge", "Like", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const QColor c = tintOr(s, QColor(46, 184, 104));
        QPainterPath disc;
        disc.addEllipse(QPointF(500, 500), 330, 330);
        softShadow(p, disc);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPath(disc);
        QPainterPath ck;
        ck.moveTo(340, 510);
        ck.lineTo(450, 620);
        ck.lineTo(670, 380);
        p.setPen(QPen(Qt::white, 60, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(partial(ck, easeOutCubic((t - 0.25) / 0.4)));
    });

    // ---- Particle
    add(sq("particle.sparkles", "Sparkles", "Particle", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        const QColor c = tintOr(s, QColor(255, 236, 150));
        for (int i = 0; i < 14; ++i) {
            const double tw = std::sin(t * (2.5 + rnd(i, 5) * 3) + i * 2.1);
            sparkle(p, {100 + 800 * rnd(i, 1), 100 + 800 * rnd(i, 2)}, 40 + 80 * rnd(i, 3), i % 4 ? c : QColor(Qt::white), std::max(0.0, tw));
        }
    });
    add(full("particle.confetti", "Confetti", "Particle", true), [](QPainter& p, const StickerData&, const StickerInfo&, double t, double, double w, double h) {
        const QColor cols[5] = {QColor(255, 82, 82), QColor(255, 196, 0), QColor(60, 200, 255), QColor(120, 220, 110), QColor(200, 110, 255)};
        for (int i = 0; i < 90; ++i) {
            const double speed = 0.18 + 0.2 * rnd(i, 1);
            const double y = std::fmod(rnd(i, 2) + t * speed, 1.0) * (h * 1.1) - h * 0.05;
            const double x = rnd(i, 3) * w + std::sin(t * 2 + i) * w * 0.02;
            p.save();
            p.translate(x, y);
            p.rotate(t * 200 * (rnd(i, 4) - 0.5) + i * 37);
            p.scale(1.0, std::cos(t * 4 + i));
            p.setPen(Qt::NoPen);
            p.setBrush(cols[i % 5]);
            const double sz = h * 0.012;
            p.drawRect(QRectF(-sz, -sz * 0.5, sz * 2, sz));
            p.restore();
        }
    });
    add(full("particle.snow", "Snowfall", "Particle", true), [](QPainter& p, const StickerData&, const StickerInfo&, double t, double, double w, double h) {
        for (int i = 0; i < 120; ++i) {
            const double r = h * (0.002 + 0.006 * rnd(i, 1));
            const double y = std::fmod(rnd(i, 2) + t * (0.05 + 0.08 * rnd(i, 3)), 1.0) * (h + 2 * r) - r;
            const double x = rnd(i, 4) * w + std::sin(t + i) * w * 0.015;
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 150 + static_cast<int>(100 * rnd(i, 5))));
            p.drawEllipse(QPointF(x, y), r, r);
        }
    });
    add(full("particle.dust", "Golden Dust", "Particle", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double w, double h) {
        const QColor c = tintOr(s, QColor(255, 210, 120));
        for (int i = 0; i < 70; ++i) {
            const double x = std::fmod(rnd(i, 1) * w + t * w * 0.02 * (rnd(i, 2) - 0.3), w);
            const double y = std::fmod(rnd(i, 3) * h - t * h * 0.015 * rnd(i, 4) + h, h);
            sparkle(p, {x, y}, h * (0.004 + 0.01 * rnd(i, 5)), c, 0.3 + 0.7 * std::max(0.0, std::sin(t * 2 + i)));
        }
    });

    // ---- Firework
    add(sq("firework.burst", "Firework", "Firework", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double, double) {
        const double cycle = 1.8;
        const double ph = std::fmod(t, cycle) / cycle;
        fireworkBurst(p, {500, 480}, 420, ph, tintOr(s, QColor(255, 120, 60)), static_cast<int>(t / cycle));
    });
    add(sq("firework.double", "Double Firework", "Firework", true), [](QPainter& p, const StickerData&, const StickerInfo&, double t, double, double, double) {
        const double cycle = 2.0;
        fireworkBurst(p, {340, 400}, 280, std::fmod(t, cycle) / cycle, QColor(80, 200, 255), static_cast<int>(t / cycle));
        fireworkBurst(p, {660, 600}, 260, std::fmod(t + 0.8, cycle) / cycle, QColor(255, 90, 180), static_cast<int>((t + 0.8) / cycle) + 7);
    });

    // ---- Badge / callout
    add(sq("badge.live", "LIVE", "Badge", true, true, "LIVE", 0.35), [](QPainter& p, const StickerData& s, const StickerInfo& info, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const QRectF r(150, 380, 700, 240);
        QPainterPath b;
        b.addRoundedRect(r, 40, 40);
        softShadow(p, b);
        p.setPen(Qt::NoPen);
        p.setBrush(tintOr(s, QColor(228, 34, 34)));
        p.drawPath(b);
        const double pulse = 0.5 + 0.5 * std::sin(t * 6);
        p.setBrush(QColor(255, 255, 255, 120 + static_cast<int>(135 * pulse)));
        p.drawEllipse(QPointF(r.left() + 120, r.center().y()), 40 + 8 * pulse, 40 + 8 * pulse);
        const QRectF tr(r.left() + 190, r.top(), r.width() - 220, r.height());
        const QString text = textOr(s, info);
        outlinedText(p, tr, text, boldFont(fitPx(text, tr.width(), 150)), Qt::white, QColor(0, 0, 0, 0), 0);
    });
    add(sq("badge.new", "NEW Burst", "Badge", true, true, "NEW", 0.35), [](QPainter& p, const StickerData& s, const StickerInfo& info, double t, double, double, double) {
        p.save();
        p.translate(500, 500);
        p.rotate(-12 + 4 * std::sin(t * 3));
        p.translate(-500, -500);
        scaleAbout(p, popIn(t));
        burstShape(p, {500, 500}, 400, 16, tintOr(s, QColor(255, 210, 40)), QColor(40, 30, 20), 0);
        const QString text = textOr(s, info);
        outlinedText(p, QRectF(150, 350, 700, 300), text, boldFont(fitPx(text, 560, 230)), QColor(40, 30, 20), QColor(0, 0, 0, 0), 0);
        p.restore();
    });
    add(sq("badge.sale", "SALE Tag", "Badge", true, true, "SALE", 0.35), [](QPainter& p, const StickerData& s, const StickerInfo& info, double t, double, double, double) {
        p.save();
        p.translate(500, 180);
        p.rotate(10 * std::sin(t * 3) * std::exp(-t * 0.5) - 8);
        p.translate(-500, -180);
        scaleAbout(p, popIn(t));
        QPainterPath tag;
        tag.moveTo(300, 280);
        tag.lineTo(500, 160);
        tag.lineTo(700, 280);
        tag.lineTo(700, 820);
        tag.lineTo(300, 820);
        tag.closeSubpath();
        tag.addEllipse(QPointF(500, 290), 36, 36);
        tag.setFillRule(Qt::OddEvenFill);
        softShadow(p, tag);
        p.setPen(Qt::NoPen);
        p.setBrush(tintOr(s, QColor(232, 52, 80)));
        p.drawPath(tag);
        p.save();
        p.translate(500, 560);
        p.rotate(-90);
        const QString text = textOr(s, info);
        outlinedText(p, QRectF(-250, -120, 500, 240), text, boldFont(fitPx(text, 460, 190)), Qt::white, QColor(0, 0, 0, 0), 0);
        p.restore();
        p.restore();
    });
    add(sq("badge.omg", "OMG", "Badge", true, true, "OMG!", 0.4), [](QPainter& p, const StickerData& s, const StickerInfo& info, double t, double, double, double) {
        scaleAbout(p, popIn(t) * (1 + 0.04 * std::sin(t * 10)));
        burstShape(p, {500, 500}, 420, 12, tintOr(s, QColor(255, 236, 60)), QColor(20, 20, 20), 0.08);
        const QString text = textOr(s, info);
        outlinedText(p, QRectF(120, 330, 760, 340), text, boldFont(fitPx(text, 640, 260)), QColor(232, 40, 40), Qt::white, 24);
    });
    add(sq("badge.wow", "WOW", "Badge", true, true, "WOW", 0.4), [](QPainter& p, const StickerData& s, const StickerInfo& info, double t, double, double, double) {
        p.save();
        p.translate(500, 500);
        p.rotate(-10);
        p.translate(-500, -500);
        scaleAbout(p, popIn(t) * (1 + 0.05 * std::sin(t * 8)));
        burstShape(p, {500, 500}, 400, 14, tintOr(s, QColor(80, 180, 255)), Qt::white, 0.1);
        const QString text = textOr(s, info);
        outlinedText(p, QRectF(120, 330, 760, 340), text, boldFont(fitPx(text, 620, 250)), Qt::white, QColor(20, 40, 90), 26);
        p.restore();
    });
    add(sq("bubble.speech", "Speech Bubble", "Badge", true, true, "Hello!", 0.45), [](QPainter& p, const StickerData& s, const StickerInfo& info, double t, double, double, double) {
        scaleAbout(p, popIn(t), {300, 800});
        QPainterPath b;
        b.addRoundedRect(QRectF(100, 180, 800, 500), 200, 200);
        QPainterPath tail;
        tail.moveTo(280, 620);
        tail.lineTo(220, 860);
        tail.lineTo(430, 660);
        tail.closeSubpath();
        b = b.united(tail);
        softShadow(p, b);
        p.setPen(QPen(QColor(30, 30, 34), 16, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(tintOr(s, QColor(255, 255, 255)));
        p.drawPath(b);
        const QString text = textOr(s, info);
        outlinedText(p, QRectF(160, 220, 680, 420), text, boldFont(fitPx(text, 620, 170), QFont::Bold), QColor(30, 30, 34), QColor(0, 0, 0, 0), 0);
    });
    add(sq("bubble.thought", "Thought Bubble", "Badge", true, true, "Hmm...", 0.45), [](QPainter& p, const StickerData& s, const StickerInfo& info, double t, double, double, double) {
        scaleAbout(p, popIn(t), {200, 900});
        const QColor fill = tintOr(s, QColor(255, 255, 255));
        QPainterPath cloud;
        const QPointF cs[] = {{300, 400}, {460, 300}, {640, 320}, {760, 440}, {680, 600}, {480, 640}, {300, 580}};
        const double rs[] = {150, 170, 160, 150, 150, 160, 150};
        for (int i = 0; i < 7; ++i) {
            QPainterPath c;
            c.addEllipse(cs[i], rs[i], rs[i] * 0.85);
            cloud = cloud.united(c);
        }
        softShadow(p, cloud);
        p.setPen(QPen(QColor(30, 30, 34), 14));
        p.setBrush(fill);
        p.drawPath(cloud);
        p.drawEllipse(QPointF(240, 780), 50, 44);
        p.drawEllipse(QPointF(170, 870), 28, 24);
        const QString text = textOr(s, info);
        outlinedText(p, QRectF(220, 330, 560, 280), text, boldFont(fitPx(text, 520, 150), QFont::Bold), QColor(30, 30, 34), QColor(0, 0, 0, 0), 0);
    });

    // ---- Shape (templates use them too)
    add(sq("shape.circle", "Circle", "Shape", false, true, {}, 0.3), [](QPainter& p, const StickerData& s, const StickerInfo&, double, double, double, double) {
        p.setPen(Qt::NoPen);
        p.setBrush(tintOr(s, QColor(255, 255, 255)));
        p.drawEllipse(QPointF(500, 500), 480, 480);
    });
    add(sq("shape.rounded_rect", "Rounded Rectangle", "Shape", false, true, {}, 0.3), [](QPainter& p, const StickerData& s, const StickerInfo&, double, double, double, double) {
        p.setPen(Qt::NoPen);
        p.setBrush(tintOr(s, QColor(255, 255, 255)));
        p.drawRoundedRect(QRectF(20, 220, 960, 560), 90, 90);
    });
    add(sq("shape.star", "Star", "Shape", false, true, {}, 0.3), [](QPainter& p, const StickerData& s, const StickerInfo&, double, double, double, double) {
        p.setPen(Qt::NoPen);
        p.setBrush(tintOr(s, QColor(255, 210, 40)));
        p.drawPath(star({500, 530}, 480, 200));
    });
    add(sq("shape.ring", "Ring", "Shape", false, true, {}, 0.3), [](QPainter& p, const StickerData& s, const StickerInfo&, double, double, double, double) {
        p.setPen(QPen(tintOr(s, QColor(255, 255, 255)), 60));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(500, 500), 440, 440);
    });
    add(sq("countdown.ring", "Countdown Ring", "Shape", true, true, {}, 0.5), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double len, double, double) {
        // a ring that empties once per second (countdown templates show a number over it)
        const double ph = std::fmod(t, 1.0);
        p.setPen(QPen(QColor(255, 255, 255, 50), 46));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(500, 500), 400, 400);
        p.setPen(QPen(tintOr(s, QColor(255, 255, 255)), 46, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(QRectF(100, 100, 800, 800), 90 * 16, static_cast<int>(-360 * 16 * (1 - ph)));
        (void)len;
    });

    // ---- Background (full frame)
    auto bg = [&](std::string id, std::string name, QColor a, QColor b) {
        add(full(std::move(id), std::move(name), "Background", true), [a, b](QPainter& p, const StickerData&, const StickerInfo&, double t, double, double w, double h) {
            gradientFill(p, w, h, a, b, t, true);
        });
    };
    add(full("bg.solid", "Solid Color", "Background", false, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double, double, double w, double h) {
        p.fillRect(QRectF(0, 0, w, h), tintOr(s, QColor(24, 26, 32)));
    });
    bg("bg.sunset", "Sunset Gradient", QColor(255, 120, 80), QColor(120, 40, 140));
    bg("bg.ocean", "Ocean Gradient", QColor(30, 160, 210), QColor(20, 40, 120));
    bg("bg.night", "Night Gradient", QColor(40, 40, 80), QColor(8, 8, 20));
    bg("bg.mint", "Mint Gradient", QColor(110, 230, 190), QColor(30, 120, 140));
    bg("bg.peach", "Peach Gradient", QColor(255, 222, 190), QColor(250, 160, 160));
    add(full("bg.grid", "Tech Grid", "Background", true, true), [](QPainter& p, const StickerData& s, const StickerInfo&, double t, double, double w, double h) {
        p.fillRect(QRectF(0, 0, w, h), QColor(10, 14, 28));
        const QColor c = tintOr(s, QColor(60, 160, 255));
        const double step = h / 12.0, off = std::fmod(t * step * 0.5, step);
        for (double x = -step + off; x < w + step; x += step) {
            QColor k = c;
            k.setAlpha(50);
            p.setPen(QPen(k, std::max(1.0, h / 540)));
            p.drawLine(QPointF(x, 0), QPointF(x, h));
        }
        for (double y = -step + off; y < h + step; y += step) {
            QColor k = c;
            k.setAlpha(50);
            p.setPen(QPen(k, std::max(1.0, h / 540)));
            p.drawLine(QPointF(0, y), QPointF(w, y));
        }
        QRadialGradient g(QPointF(w / 2, h / 2), h * 0.8);
        QColor glow = c;
        glow.setAlpha(70);
        g.setColorAt(0, glow);
        g.setColorAt(1, QColor(0, 0, 0, 0));
        p.fillRect(QRectF(0, 0, w, h), g);
    });

    // ---- media placeholder (templates): replaced by the user's own video or picture (Replace Clip)
    add(full("placeholder", "Media Placeholder", "Placeholder", false, true, "Replace with your media"), [](QPainter& p, const StickerData& s, const StickerInfo& info, double, double, double w, double h) {
        const QColor c = tintOr(s, QColor(52, 56, 66));
        QLinearGradient g(0, 0, w, h);
        g.setColorAt(0, c.lighter(115));
        g.setColorAt(1, c.darker(115));
        p.fillRect(QRectF(0, 0, w, h), g);
        const double m = h * 0.04;
        QPen dash(QColor(255, 255, 255, 120), std::max(2.0, h / 200), Qt::DashLine);
        p.setPen(dash);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(m, m, w - 2 * m, h - 2 * m), h * 0.03, h * 0.03);
        // a film frame icon and the hint
        const double s0 = h * 0.12;
        const QRectF icon(w / 2 - s0, h / 2 - s0 * 1.1, s0 * 2, s0 * 1.4);
        p.setPen(QPen(QColor(255, 255, 255, 170), std::max(2.0, h / 160)));
        p.drawRoundedRect(icon, s0 * 0.15, s0 * 0.15);
        QPainterPath tri;
        tri.moveTo(icon.center().x() - s0 * 0.25, icon.center().y() - s0 * 0.32);
        tri.lineTo(icon.center().x() - s0 * 0.25, icon.center().y() + s0 * 0.32);
        tri.lineTo(icon.center().x() + s0 * 0.35, icon.center().y());
        tri.closeSubpath();
        p.setBrush(QColor(255, 255, 255, 170));
        p.drawPath(tri);
        const QString text = textOr(s, info);
        p.setFont(boldFont(h * 0.045, QFont::DemiBold));
        p.setPen(QColor(255, 255, 255, 190));
        p.drawText(QRectF(0, icon.bottom() + h * 0.03, w, h * 0.1), Qt::AlignHCenter | Qt::AlignTop, text);
    });

    // ================================ M11 (MeerCut designs) ================================
    using SD = const StickerData&;
    using SI = const StickerInfo&;
    // ---- Weather
    add(sq("weather.sun", "Sun", "Weather", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const QColor c = tintOr(s, QColor(255, 196, 40));
        p.save();
        p.translate(500, 500);
        p.rotate(t * 20);
        p.setPen(Qt::NoPen);
        p.setBrush(c.lighter(110));
        for (int i = 0; i < 12; ++i) {
            const double len = 380 + 30 * std::sin(t * 3 + i);
            QPainterPath ray;
            ray.moveTo(-34, -250);
            ray.lineTo(0, -len);
            ray.lineTo(34, -250);
            ray.closeSubpath();
            p.drawPath(ray);
            p.rotate(30);
        }
        p.restore();
        QPainterPath disc;
        disc.addEllipse(QPointF(500, 500), 230, 230);
        softShadow(p, disc, 10, 50);
        QRadialGradient g(QPointF(440, 430), 300);
        g.setColorAt(0, c.lighter(130));
        g.setColorAt(1, c.darker(108));
        p.setBrush(g);
        p.drawPath(disc);
    });
    add(sq("weather.cloud", "Cloud", "Weather", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const QColor c = tintOr(s, QColor(255, 255, 255));
        drawCloud(p, QPointF(500 + 40 * std::sin(t * 0.8), 520), 300, c, c.darker(118));
    });
    add(sq("weather.rain", "Rain Cloud", "Weather", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const QColor drop = tintOr(s, QColor(70, 150, 255));
        for (int i = 0; i < 9; ++i) {   // drops falling from the cloud, each on its own cycle
            const double fall = std::fmod(t * 520 + rnd(i, 1) * 320, 330);
            QColor k = drop;
            k.setAlphaF(clamp01(1 - fall / 330) * 0.95);
            p.setPen(Qt::NoPen);
            p.setBrush(k);
            const QPointF c(250 + i * 62 + 18 * rnd(i, 2), 560 + fall);
            QPainterPath d;
            d.moveTo(c.x(), c.y() - 36);
            d.quadTo(c.x() + 16, c.y() + 2, c.x(), c.y() + 12);
            d.quadTo(c.x() - 16, c.y() + 2, c.x(), c.y() - 36);
            p.drawPath(d);
        }
        drawCloud(p, QPointF(500, 380), 280, QColor(200, 210, 225), QColor(130, 145, 170));
    });
    add(sq("weather.snowflake", "Snowflake", "Weather", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        p.translate(500, 500);
        p.rotate(t * 25);
        const QColor c = tintOr(s, QColor(225, 242, 255));
        p.setPen(QPen(QColor(60, 120, 190, 90), 52, Qt::SolidLine, Qt::RoundCap));   // a soft rim under the arms
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 0; i < 6; ++i) {
                p.drawLine(QPointF(0, 0), QPointF(0, -360));
                p.drawLine(QPointF(0, -210), QPointF(-90, -290));
                p.drawLine(QPointF(0, -210), QPointF(90, -290));
                p.drawLine(QPointF(0, -110), QPointF(-60, -160));
                p.drawLine(QPointF(0, -110), QPointF(60, -160));
                p.rotate(60);
            }
            p.setPen(QPen(c, 30, Qt::SolidLine, Qt::RoundCap));
        }
    });
    add(sq("weather.lightning", "Lightning", "Weather", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const double flash = std::pow(std::max(0.0, std::sin(t * 9)), 4);
        QPainterPath bolt;
        bolt.moveTo(560, 80);
        bolt.lineTo(300, 520);
        bolt.lineTo(470, 520);
        bolt.lineTo(380, 920);
        bolt.lineTo(720, 420);
        bolt.lineTo(545, 420);
        bolt.lineTo(660, 80);
        bolt.closeSubpath();
        const QColor c = tintOr(s, QColor(255, 220, 50));
        QColor glow = c;
        glow.setAlphaF(0.25 + 0.45 * flash);
        p.setPen(QPen(glow, 60 + 40 * flash, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(bolt);
        softShadow(p, bolt, 10, 60);
        p.setPen(QPen(c.darker(150), 14, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(flash > 0.5 ? QColor(255, 255, 235) : c);
        p.drawPath(bolt);
    });
    // ---- Travel
    add(sq("travel.plane", "Airplane", "Travel", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        QPainterPath route;
        route.moveTo(120, 820);
        route.cubicTo(320, 860, 380, 420, 600, 420);
        route.cubicTo(760, 420, 820, 260, 880, 160);
        const double k = easeInOut(std::fmod(t / 3.0, 1.0) * 1.15);
        QPen dash(QColor(255, 255, 255, 220), 14, Qt::DashLine, Qt::RoundCap);
        dash.setDashPattern({2.0, 2.5});
        p.setPen(dash);
        p.setBrush(Qt::NoBrush);
        p.drawPath(partial(route, k));
        const QPointF at = route.pointAtPercent(clamp01(k));
        p.save();
        p.translate(at);
        p.rotate(-route.angleAtPercent(clamp01(k)) + 90);   // the plane's nose along the route
        QPainterPath plane;   // seen from above, nose up
        plane.moveTo(0, -120);
        plane.cubicTo(22, -110, 24, -60, 22, -20);
        plane.lineTo(130, 30);
        plane.lineTo(130, 58);
        plane.lineTo(22, 34);
        plane.lineTo(18, 90);
        plane.lineTo(56, 118);
        plane.lineTo(56, 136);
        plane.lineTo(0, 122);
        plane.lineTo(-56, 136);
        plane.lineTo(-56, 118);
        plane.lineTo(-18, 90);
        plane.lineTo(-22, 34);
        plane.lineTo(-130, 58);
        plane.lineTo(-130, 30);
        plane.lineTo(-22, -20);
        plane.cubicTo(-24, -60, -22, -110, 0, -120);
        softShadow(p, plane, 12, 60);
        p.setPen(QPen(QColor(40, 60, 90), 6));
        p.setBrush(tintOr(s, QColor(245, 248, 252)));
        p.drawPath(plane);
        p.restore();
    });
    add(sq("travel.pin", "Map Pin", "Travel", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        const double drop = t < 0.6 ? easeOutBack(t / 0.6) : 1.0;
        const double ring = std::fmod(t * 0.9, 1.0);
        p.setPen(QPen(QColor(255, 255, 255, static_cast<int>(200 * (1 - ring))), 10));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(500, 860), 60 + 220 * ring, 18 + 66 * ring);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 70));
        p.drawEllipse(QPointF(500, 860), 70, 20);
        p.translate(0, (drop - 1) * 500);
        QPainterPath pin;
        pin.moveTo(500, 850);
        pin.cubicTo(400, 700, 280, 560, 280, 400);
        pin.cubicTo(280, 270, 380, 170, 500, 170);
        pin.cubicTo(620, 170, 720, 270, 720, 400);
        pin.cubicTo(720, 560, 600, 700, 500, 850);
        pin.addEllipse(QPointF(500, 400), 80, 80);
        pin.setFillRule(Qt::OddEvenFill);
        softShadow(p, pin, 14, 70);
        QLinearGradient g(280, 170, 720, 850);
        const QColor c = tintOr(s, QColor(235, 64, 52));
        g.setColorAt(0, c.lighter(125));
        g.setColorAt(1, c.darker(115));
        p.setBrush(g);
        p.drawPath(pin);
    });
    add(sq("travel.globe", "Globe", "Travel", true, false), [](QPainter& p, SD, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        QPainterPath ball;
        ball.addEllipse(QPointF(500, 500), 330, 330);
        softShadow(p, ball, 14, 60);
        QRadialGradient sea(QPointF(410, 400), 420);
        sea.setColorAt(0, QColor(90, 180, 255));
        sea.setColorAt(1, QColor(20, 80, 170));
        p.setPen(Qt::NoPen);
        p.setBrush(sea);
        p.drawPath(ball);
        p.save();
        p.setClipPath(ball);
        p.setBrush(QColor(80, 190, 110));
        const double shift = std::fmod(t * 90, 900);   // continents drifting by as the globe turns
        for (int i = 0; i < 5; ++i) {
            const double x = std::fmod(160 + i * 180 + shift, 900) + 50;
            p.drawEllipse(QPointF(x, 300 + 110 * std::sin(i * 2.1)), 70 + 30 * rnd(i, 3), 50 + 40 * rnd(i, 4));
            p.drawEllipse(QPointF(x + 50, 360 + 110 * std::sin(i * 2.1)), 60, 70 + 30 * rnd(i, 5));
        }
        p.setPen(QPen(QColor(255, 255, 255, 70), 6));
        p.setBrush(Qt::NoBrush);
        for (int i = 0; i < 6; ++i) {   // meridians sliding round
            const double a = std::fmod(t * 0.8 + i * kPi / 6, kPi);
            const double rx = 330 * std::fabs(std::cos(a));
            p.drawEllipse(QPointF(500, 500), rx, 330);
        }
        for (double y : {-200.0, -100.0, 0.0, 100.0, 200.0}) {   // parallels
            const double rx = std::sqrt(330.0 * 330.0 - y * y);
            p.drawLine(QPointF(500 - rx, 500 + y), QPointF(500 + rx, 500 + y));
        }
        p.restore();
        p.setPen(QPen(QColor(255, 255, 255, 120), 10));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(500, 500), 330, 330);
    });
    add(sq("travel.suitcase", "Suitcase", "Travel", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        p.translate(0, -std::fabs(std::sin(t * 4)) * 40);
        const QColor c = tintOr(s, QColor(52, 152, 219));
        p.setPen(QPen(QColor(60, 60, 70), 28, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(400, 230, 200, 120), 40, 40);   // handle
        QPainterPath body;
        body.addRoundedRect(QRectF(220, 320, 560, 480), 50, 50);
        softShadow(p, body, 16, 60);
        p.setPen(Qt::NoPen);
        QLinearGradient g(220, 320, 780, 800);
        g.setColorAt(0, c.lighter(115));
        g.setColorAt(1, c.darker(115));
        p.setBrush(g);
        p.drawPath(body);
        p.setBrush(c.darker(140));
        p.drawRect(QRectF(330, 320, 36, 480));
        p.drawRect(QRectF(634, 320, 36, 480));
        p.setBrush(QColor(255, 210, 60));   // travel stickers
        p.drawPath(star(QPointF(480, 470), 60, 26));
        p.setBrush(QColor(240, 90, 90));
        p.drawEllipse(QPointF(530, 640), 50, 50);
        p.setBrush(QColor(255, 255, 255));
        p.drawRoundedRect(QRectF(410, 580, 70, 110), 12, 12);
        p.setBrush(QColor(40, 40, 50));
        p.drawEllipse(QPointF(300, 820), 26, 26);
        p.drawEllipse(QPointF(700, 820), 26, 26);
    });
    // ---- Music
    add(sq("music.notes", "Music Notes", "Music", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        const QColor c = tintOr(s, QColor(255, 255, 255));
        for (int i = 0; i < 4; ++i) {   // notes rising and fading, each on its own cycle
            const double ph = std::fmod(t * 0.35 + i * 0.25, 1.0);
            QColor k = c;
            k.setAlphaF(clamp01(std::sin(ph * kPi)) * 0.95);
            const QPointF head(240 + i * 170 + 40 * std::sin(t * 2 + i), 900 - ph * 640);
            eighthNote(p, head, 70 + 10 * (i % 3), k);
        }
    });
    add(sq("music.headphones", "Headphones", "Music", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t) * (1 + 0.025 * std::sin(t * 8)));
        const QColor c = tintOr(s, QColor(40, 44, 52));
        p.setPen(QPen(c, 50, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(250, 200, 500, 520), 10 * 16, 160 * 16);
        p.setPen(Qt::NoPen);
        const QColor accent(240, 70, 110);
        for (int side : {-1, 1}) {
            const double x = 500 + side * 245;
            QPainterPath cup;
            cup.addRoundedRect(QRectF(x - 75, 430, 150, 250), 60, 60);
            softShadow(p, cup, 12, 60);
            p.setBrush(c);
            p.drawPath(cup);
            p.setBrush(accent);
            p.drawRoundedRect(QRectF(x - 45 + side * 12, 470, 90, 170), 40, 40);
        }
        soundArcs(p, QPointF(560, 555), 280, t, QColor(255, 255, 255), 1);
        soundArcs(p, QPointF(440, 555), 280, t, QColor(255, 255, 255), -1);
    });
    add(sq("music.mic", "Microphone", "Music", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const QColor c = tintOr(s, QColor(70, 74, 86));
        p.setPen(QPen(c.darker(130), 26, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(330, 320, 340, 340), 200 * 16, 140 * 16);   // the holder
        p.drawLine(QPointF(500, 660), QPointF(500, 820));
        p.drawLine(QPointF(400, 830), QPointF(600, 830));
        QPainterPath head;
        head.addRoundedRect(QRectF(400, 150, 200, 360), 100, 100);
        softShadow(p, head, 12, 60);
        QLinearGradient g(400, 150, 600, 510);
        g.setColorAt(0, QColor(220, 225, 235));
        g.setColorAt(1, QColor(140, 145, 160));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawPath(head);
        p.setPen(QPen(QColor(90, 95, 110, 160), 6));
        for (int y = 200; y < 330; y += 26) p.drawLine(QPointF(420, y), QPointF(580, y));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRect(QRectF(400, 380, 200, 40));
        soundArcs(p, QPointF(640, 300), 120, t, tintOr(s, QColor(255, 255, 255)), 1);
    });
    // ---- Gaming
    add(sq("gaming.controller", "Game Controller", "Gaming", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        p.translate(500, 520);
        p.rotate(4 * std::sin(t * 3));
        p.translate(-500, -520);
        QPainterPath body;
        body.moveTo(300, 360);
        body.cubicTo(380, 330, 620, 330, 700, 360);
        body.cubicTo(820, 400, 900, 680, 840, 740);
        body.cubicTo(790, 790, 720, 720, 660, 640);
        body.lineTo(340, 640);
        body.cubicTo(280, 720, 210, 790, 160, 740);
        body.cubicTo(100, 680, 180, 400, 300, 360);
        softShadow(p, body, 16, 70);
        const QColor c = tintOr(s, QColor(60, 64, 80));
        QLinearGradient g(160, 330, 840, 790);
        g.setColorAt(0, c.lighter(130));
        g.setColorAt(1, c.darker(115));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawPath(body);
        p.setBrush(QColor(230, 232, 238));   // the cross
        p.drawRoundedRect(QRectF(255, 440, 120, 40), 10, 10);
        p.drawRoundedRect(QRectF(295, 400, 40, 120), 10, 10);
        const QColor buttons[4] = {QColor(76, 175, 80), QColor(244, 67, 54), QColor(33, 150, 243), QColor(255, 193, 7)};
        const QPointF at[4] = {{690, 510}, {750, 450}, {630, 450}, {690, 390}};
        for (int i = 0; i < 4; ++i) {
            const bool lit = static_cast<int>(t * 3) % 4 == i;
            p.setBrush(lit ? buttons[i].lighter(150) : buttons[i]);
            p.drawEllipse(at[i], lit ? 36 : 30, lit ? 36 : 30);
        }
        p.setBrush(QColor(30, 32, 40));
        p.drawEllipse(QPointF(420, 580), 46, 46);
        p.drawEllipse(QPointF(580, 580), 46, 46);
    });
    add(sq("gaming.health", "Health Bar", "Gaming", true, false), [](QPainter& p, SD, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const double level = 0.2 + 0.8 * (0.5 + 0.5 * std::cos(t * 1.4));   // drains and refills
        const QPainterPath h = heart(QPointF(190, 500), 230);
        softShadow(p, h, 10, 60);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(232, 36, 76));
        p.drawPath(h);
        QPainterPath frame;
        frame.addRoundedRect(QRectF(320, 430, 600, 140), 30, 30);
        softShadow(p, frame, 10, 60);
        p.setBrush(QColor(30, 30, 36));
        p.drawPath(frame);
        const QColor fill = QColor::fromHsvF(static_cast<float>(0.33 * clamp01((level - 0.15) / 0.7)), 0.85f, 0.95f);
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(340, 450, 560 * level, 100), 20, 20);
        p.setPen(QPen(QColor(30, 30, 36), 8));
        for (int i = 1; i < 10; ++i) p.drawLine(QPointF(340 + i * 56, 450), QPointF(340 + i * 56, 550));
        p.setPen(QPen(QColor(255, 255, 255, 220), 8));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(320, 430, 600, 140), 30, 30);
    });
    add(sq("gaming.coin", "Spinning Coin", "Gaming", true, false), [](QPainter& p, SD, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const double turn = std::cos(t * 4);
        const double rx = std::max(18.0, 300 * std::fabs(turn));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(170, 110, 20));   // the edge
        p.drawEllipse(QPointF(500 + (turn > 0 ? 14 : -14) * (1 - std::fabs(turn)), 500), rx, 300);
        QRadialGradient g(QPointF(440, 420), 380);
        g.setColorAt(0, QColor(255, 236, 140));
        g.setColorAt(1, QColor(230, 160, 30));
        p.setBrush(g);
        p.drawEllipse(QPointF(500, 500), rx * 0.94, 282);
        if (std::fabs(turn) > 0.35) {
            p.save();
            p.translate(500, 500);
            p.scale(std::fabs(turn), 1.0);
            p.setBrush(QColor(200, 130, 20, 200));
            p.drawPath(star(QPointF(0, 0), 150, 66));
            p.restore();
        }
        const double glint = std::fmod(t * 0.7, 1.0);
        sparkle(p, QPointF(650, 280), 70 * std::sin(glint * kPi), QColor(255, 255, 255), std::sin(glint * kPi));
    });
    add(sq("gaming.trophy", "Trophy", "Gaming", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const QColor c = tintOr(s, QColor(250, 196, 40));
        QPainterPath cup;
        cup.moveTo(310, 200);
        cup.lineTo(690, 200);
        cup.cubicTo(690, 460, 600, 560, 540, 580);
        cup.lineTo(540, 690);
        cup.lineTo(640, 760);
        cup.lineTo(640, 820);
        cup.lineTo(360, 820);
        cup.lineTo(360, 760);
        cup.lineTo(460, 690);
        cup.lineTo(460, 580);
        cup.cubicTo(400, 560, 310, 460, 310, 200);
        softShadow(p, cup, 16, 70);
        p.setPen(QPen(c.darker(125), 30, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(200, 240, 200, 220), 90 * 16, 180 * 16);   // handles
        p.drawArc(QRectF(600, 240, 200, 220), -90 * 16, 180 * 16);
        QLinearGradient g(310, 200, 690, 820);
        g.setColorAt(0, c.lighter(135));
        g.setColorAt(0.5, c);
        g.setColorAt(1, c.darker(125));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawPath(cup);
        p.save();   // a shine sweeping across
        p.setClipPath(cup);
        const double x = -300 + std::fmod(t * 0.6, 1.0) * 1400;
        QPainterPath band;
        band.moveTo(x, 150);
        band.lineTo(x + 90, 150);
        band.lineTo(x - 210, 880);
        band.lineTo(x - 300, 880);
        band.closeSubpath();
        p.setBrush(QColor(255, 255, 255, 120));
        p.drawPath(band);
        p.restore();
        p.setBrush(QColor(255, 255, 255, 230));
        p.drawPath(star(QPointF(500, 360), 70, 30));
    });
    // ---- Holiday
    add(sq("holiday.gift", "Gift Box", "Holiday", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const double hop = std::pow(std::max(0.0, std::sin(t * 3)), 6);
        p.translate(500, 800);
        p.rotate(3 * std::sin(t * 6) * hop);
        p.translate(-500, -800);
        const QColor box = tintOr(s, QColor(229, 57, 53));
        const QColor ribbon(255, 213, 79);
        QPainterPath b;
        b.addRect(QRectF(250, 450, 500, 360));
        softShadow(p, b, 16, 70);
        p.setPen(Qt::NoPen);
        p.setBrush(box);
        p.drawPath(b);
        p.setBrush(ribbon);
        p.drawRect(QRectF(465, 450, 70, 360));
        p.translate(0, -60 * hop);   // the lid hops
        p.setBrush(box.darker(115));
        p.drawRect(QRectF(220, 360, 560, 110));
        p.setBrush(ribbon);
        p.drawRect(QRectF(465, 360, 70, 110));
        p.setPen(QPen(ribbon.darker(115), 34, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(330, 250, 170, 120));
        p.drawEllipse(QRectF(500, 250, 170, 120));
        p.setPen(Qt::NoPen);
        p.setBrush(ribbon);
        p.drawEllipse(QPointF(500, 350), 44, 36);
    });
    add(sq("holiday.balloons", "Balloons", "Holiday", true, false), [](QPainter& p, SD, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const QColor cols[3] = {QColor(239, 83, 80), QColor(255, 202, 40), QColor(66, 165, 245)};
        const QPointF base[3] = {{340, 360}, {520, 280}, {680, 380}};
        const QPointF knot(500, 900);
        for (int i = 0; i < 3; ++i) {
            const QPointF c(base[i].x() + 26 * std::sin(t * 1.5 + i * 2), base[i].y() + 14 * std::sin(t * 1.2 + i));
            const QPointF bottom(c.x(), c.y() + 175);
            QPainterPath string;
            string.moveTo(bottom);
            string.quadTo(QPointF((bottom.x() + knot.x()) / 2 + 40 * std::sin(t * 2 + i), (bottom.y() + knot.y()) / 2), knot);
            p.setPen(QPen(QColor(255, 255, 255, 200), 6));
            p.setBrush(Qt::NoBrush);
            p.drawPath(string);
            QPainterPath balloon;
            balloon.addEllipse(c, 130, 170);
            softShadow(p, balloon, 12, 50);
            QRadialGradient g(QPointF(c.x() - 50, c.y() - 70), 230);
            g.setColorAt(0, cols[i].lighter(140));
            g.setColorAt(1, cols[i].darker(110));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawPath(balloon);
            p.setBrush(cols[i].darker(115));
            QPainterPath tie;
            tie.moveTo(bottom.x() - 20, bottom.y() + 22);
            tie.lineTo(bottom.x() + 20, bottom.y() + 22);
            tie.lineTo(bottom.x(), bottom.y() - 4);
            tie.closeSubpath();
            p.drawPath(tie);
            p.setBrush(QColor(255, 255, 255, 120));
            p.drawEllipse(QPointF(c.x() - 55, c.y() - 80), 30, 46);
        }
    });
    add(sq("holiday.tree", "Christmas Tree", "Holiday", true, false), [](QPainter& p, SD, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(121, 85, 72));
        p.drawRect(QRectF(450, 780, 100, 120));
        const double tiers[3][3] = {{500, 170, 230}, {500, 330, 300}, {500, 500, 370}};   // apex x, apex y, half width
        for (const auto& tier : tiers) {
            QPainterPath tri;
            tri.moveTo(tier[0], tier[1]);
            tri.lineTo(tier[0] + tier[2], tier[1] + 320);
            tri.lineTo(tier[0] - tier[2], tier[1] + 320);
            tri.closeSubpath();
            softShadow(p, tri, 10, 40);
            QLinearGradient g(tier[0], tier[1], tier[0], tier[1] + 320);
            g.setColorAt(0, QColor(76, 175, 80));
            g.setColorAt(1, QColor(27, 94, 32));
            p.setBrush(g);
            p.drawPath(tri);
        }
        const QColor lights[4] = {QColor(255, 82, 82), QColor(255, 235, 59), QColor(64, 196, 255), QColor(255, 255, 255)};
        for (int i = 0; i < 14; ++i) {   // twinkling lights
            const double y = 300 + i * 34;
            const double half = 60 + (y - 300) * 0.55;
            const double x = 500 + (rnd(i, 7) * 2 - 1) * half * 0.8;
            QColor k = lights[i % 4];
            k.setAlphaF(0.35 + 0.65 * (0.5 + 0.5 * std::sin(t * 4 + i * 1.7)));
            p.setBrush(k);
            p.drawEllipse(QPointF(x, y), 16, 16);
        }
        p.save();
        p.translate(500, 160);
        p.rotate(10 * std::sin(t * 2));
        p.setBrush(QColor(255, 214, 0));
        p.drawPath(star(QPointF(0, 0), 80, 34));
        p.restore();
    });
    add(sq("holiday.pumpkin", "Pumpkin", "Holiday", true, false), [](QPainter& p, SD, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(85, 139, 47));
        p.drawRoundedRect(QRectF(470, 170, 60, 130), 20, 20);
        QPainterPath body;
        for (int i = -2; i <= 2; ++i) body.addEllipse(QPointF(500 + i * 95, 540), 170 - std::abs(i) * 20, 280);
        body.setFillRule(Qt::WindingFill);
        softShadow(p, body.simplified(), 14, 70);
        for (int i : {-2, 2, -1, 1, 0}) {   // the ribs, back to front
            QRadialGradient g(QPointF(500 + i * 95 - 40, 470), 300);
            g.setColorAt(0, QColor(255, 167, 38));
            g.setColorAt(1, QColor(214, 96, 0));
            p.setBrush(g);
            p.drawEllipse(QPointF(500 + i * 95, 540), 170 - std::abs(i) * 20, 280);
        }
        const double flicker = 0.75 + 0.25 * std::sin(t * 11) * std::sin(t * 7.3);
        const QColor glow(255, 230, 90, static_cast<int>(255 * flicker));
        p.setBrush(glow);
        QPainterPath face;
        face.moveTo(370, 470);
        face.lineTo(440, 470);
        face.lineTo(405, 400);
        face.closeSubpath();
        face.moveTo(560, 470);
        face.lineTo(630, 470);
        face.lineTo(595, 400);
        face.closeSubpath();
        face.moveTo(340, 600);
        for (int i = 0; i <= 8; ++i) face.lineTo(340 + i * 40, i % 2 ? 640 : 610);
        face.lineTo(660, 650);
        face.quadTo(500, 760, 340, 650);
        face.closeSubpath();
        p.drawPath(face);
    });
    add(sq("holiday.cake", "Birthday Cake", "Holiday", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const QColor icing = tintOr(s, QColor(244, 143, 177));
        QPainterPath cake;
        cake.addRoundedRect(QRectF(220, 560, 560, 260), 30, 30);
        cake.addRoundedRect(QRectF(300, 400, 400, 180), 26, 26);
        softShadow(p, cake.simplified(), 14, 60);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(161, 110, 76));
        p.drawRoundedRect(QRectF(220, 560, 560, 260), 30, 30);
        p.drawRoundedRect(QRectF(300, 400, 400, 180), 26, 26);
        p.setBrush(icing);
        for (const auto& [x0, y0, wd] : {std::tuple{220.0, 560.0, 560.0}, {300.0, 400.0, 400.0}}) {   // icing with drips
            QPainterPath ic;
            ic.moveTo(x0, y0 + 30);
            ic.quadTo(x0, y0, x0 + 30, y0);
            ic.lineTo(x0 + wd - 30, y0);
            ic.quadTo(x0 + wd, y0, x0 + wd, y0 + 30);
            for (int i = 6; i >= 0; --i) {
                const double x = x0 + wd * i / 6.0;
                ic.lineTo(x, y0 + 50 + (i % 2 ? 40 : 0));
            }
            ic.closeSubpath();
            p.drawPath(ic);
        }
        for (int i = 0; i < 18; ++i) {   // sprinkles
            p.setBrush(QColor::fromHsvF(static_cast<float>(rnd(i, 9)), 0.7f, 1.0f));
            p.drawRoundedRect(QRectF(240 + rnd(i, 10) * 520, 640 + rnd(i, 11) * 150, 26, 10), 5, 5);
        }
        for (int i = 0; i < 3; ++i) {   // candles and their flames
            const double x = 400 + i * 100;
            p.setBrush(QColor(144, 202, 249));
            p.drawRoundedRect(QRectF(x - 14, 290, 28, 120), 8, 8);
            const double f = 1 + 0.15 * std::sin(t * 12 + i * 2) + 0.08 * std::sin(t * 19 + i);
            QPainterPath flame;
            flame.moveTo(x, 290 - 80 * f);
            flame.quadTo(x + 30, 260, x, 285);
            flame.quadTo(x - 30, 260, x, 290 - 80 * f);
            p.setBrush(QColor(255, 193, 7));
            p.drawPath(flame);
        }
    });
    // ---- Sale
    add(sq("sale.ribbon", "Price Ribbon", "Sale", true, true, "$19.99", 0.4), [](QPainter& p, SD s, SI info, double t, double, double, double) {
        scaleAbout(p, popIn(t) * (1 + 0.02 * std::sin(t * 5)));
        const QColor c = tintOr(s, QColor(0, 150, 136));
        p.setPen(Qt::NoPen);
        p.setBrush(c.darker(150));
        for (int side : {-1, 1}) {   // folded tails behind
            QPainterPath tail;
            const double x = 500 + side * 330;
            tail.moveTo(x, 420);
            tail.lineTo(x + side * 150, 420);
            tail.lineTo(x + side * 90, 510);
            tail.lineTo(x + side * 150, 600);
            tail.lineTo(x, 600);
            tail.closeSubpath();
            p.drawPath(tail);
        }
        QPainterPath band;
        band.addRect(QRectF(160, 370, 680, 200));
        softShadow(p, band, 14, 70);
        QLinearGradient g(160, 370, 160, 570);
        g.setColorAt(0, c.lighter(125));
        g.setColorAt(1, c.darker(110));
        p.setBrush(g);
        p.drawPath(band);
        const QString text = textOr(s, info);
        outlinedText(p, QRectF(180, 370, 640, 200), text, boldFont(fitPx(text, 600, 150)), Qt::white, QColor(0, 0, 0, 0), 0);
    });
    add(sq("sale.percent", "Percent Stamp", "Sale", true, true, "-50%", 0.4), [](QPainter& p, SD s, SI info, double t, double, double, double) {
        const double k = t < 0.5 ? easeOutCubic(t / 0.5) : 1.0;   // stamped down
        p.translate(500, 500);
        p.rotate(-15 + 12 * (1 - k));
        p.scale(1.6 - 0.6 * k, 1.6 - 0.6 * k);
        p.translate(-500, -500);
        const QColor c = tintOr(s, QColor(216, 27, 96));
        QPainterPath disc;
        disc.addEllipse(QPointF(500, 500), 360, 360);
        softShadow(p, disc, 14, 70);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPath(disc);
        p.save();
        p.translate(500, 500);
        p.rotate(t * 30);
        QPen dash(QColor(255, 255, 255, 220), 14, Qt::DashLine, Qt::RoundCap);
        p.setPen(dash);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(0, 0), 310, 310);
        p.restore();
        const QString text = textOr(s, info);
        outlinedText(p, QRectF(170, 350, 660, 300), text, boldFont(fitPx(text, 560, 240)), Qt::white, QColor(0, 0, 0, 0), 0);
    });
    add(sq("sale.cart", "Shopping Cart", "Sale", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        const double in = t < 0.8 ? easeOutBack(t / 0.8) : 1.0;
        p.translate((in - 1) * 700, 0);
        const QColor c = tintOr(s, QColor(255, 255, 255));
        p.setPen(QPen(c, 30, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath frame;
        frame.moveTo(120, 260);
        frame.lineTo(240, 260);
        frame.lineTo(330, 660);
        frame.lineTo(780, 660);
        p.drawPath(frame);
        QPainterPath basket;
        basket.moveTo(260, 340);
        basket.lineTo(860, 340);
        basket.lineTo(790, 580);
        basket.lineTo(315, 580);
        basket.closeSubpath();
        p.setBrush(QColor(255, 255, 255, 50));
        p.drawPath(basket);
        p.setPen(QPen(c, 12));
        for (int i = 1; i < 5; ++i) p.drawLine(QPointF(260 + i * 120, 340), QPointF(315 + i * 95, 580));
        p.drawLine(QPointF(285, 460), QPointF(825, 460));
        const double spin = -t * 360;   // the wheels turn as it rolls
        for (const double x : {380.0, 730.0}) {
            p.save();
            p.translate(x, 740);
            p.rotate(spin);
            p.setPen(QPen(c, 14));
            p.setBrush(QColor(60, 60, 70));
            p.drawEllipse(QPointF(0, 0), 52, 52);
            p.drawLine(QPointF(-40, 0), QPointF(40, 0));
            p.drawLine(QPointF(0, -40), QPointF(0, 40));
            p.restore();
        }
    });
    // ---- Tech
    add(sq("tech.loading", "Loading", "Tech", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        const QColor c = tintOr(s, QColor(255, 255, 255));
        const double head = std::fmod(t * 1.4, 1.0) * 12;
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 12; ++i) {
            const double age = std::fmod(head - i + 12, 12.0) / 12.0;   // 0 = the leading dot
            QColor k = c;
            k.setAlphaF(1.0 - 0.8 * age);
            p.setBrush(k);
            const double a = i * kPi / 6 - kPi / 2;
            p.drawEllipse(QPointF(500 + 300 * std::cos(a), 500 + 300 * std::sin(a)), 50 - 18 * age, 50 - 18 * age);
        }
    });
    add(sq("tech.notification", "Message Notification", "Tech", true, true, "1 new message", 0.55), [](QPainter& p, SD s, SI info, double t, double, double, double) {
        const double in = t < 0.6 ? easeOutBack(t / 0.6) : 1.0;
        p.translate(0, (in - 1) * 500);
        QPainterPath card;
        card.addRoundedRect(QRectF(80, 380, 840, 240), 50, 50);
        softShadow(p, card, 18, 80);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(250, 250, 252));
        p.drawPath(card);
        const QColor c = tintOr(s, QColor(76, 175, 80));
        p.setBrush(c);
        p.drawRoundedRect(QRectF(120, 420, 160, 160), 40, 40);
        p.setBrush(Qt::white);   // a speech bubble glyph
        p.drawEllipse(QPointF(200, 490), 52, 40);
        QPainterPath tail;
        tail.moveTo(176, 520);
        tail.lineTo(166, 552);
        tail.lineTo(206, 528);
        tail.closeSubpath();
        p.drawPath(tail);
        const double pulse = 0.5 + 0.5 * std::sin(t * 5);
        p.setBrush(QColor(244, 67, 54));
        p.drawEllipse(QPointF(270, 420), 22 + 6 * pulse, 22 + 6 * pulse);
        const QString text = textOr(s, info);
        const QRectF tr(320, 400, 560, 200);
        QFont f = boldFont(fitPx(text, tr.width(), 80, QFont::DemiBold), QFont::DemiBold);
        p.setFont(f);
        p.setPen(QColor(30, 32, 40));
        p.drawText(tr, Qt::AlignLeft | Qt::AlignVCenter, text);
    });
    add(sq("tech.battery", "Battery", "Tech", true, false), [](QPainter& p, SD, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        const double level = std::fmod(t * 0.35, 1.0);   // charging up, again and again
        QPainterPath shell;
        shell.addRoundedRect(QRectF(170, 330, 600, 340), 50, 50);
        softShadow(p, shell, 14, 60);
        p.setPen(QPen(QColor(255, 255, 255), 30, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(QColor(30, 32, 40, 200));
        p.drawPath(shell);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255));
        p.drawRoundedRect(QRectF(780, 430, 60, 140), 18, 18);
        const int segs = 1 + static_cast<int>(level * 4.0);
        const QColor fill = segs <= 1 ? QColor(244, 67, 54) : segs == 2 ? QColor(255, 193, 7) : QColor(76, 175, 80);
        p.setBrush(fill);
        for (int i = 0; i < segs && i < 4; ++i) p.drawRoundedRect(QRectF(215 + i * 140, 375, 115, 250), 18, 18);
        QPainterPath bolt;
        bolt.moveTo(500, 380);
        bolt.lineTo(420, 520);
        bolt.lineTo(490, 520);
        bolt.lineTo(460, 640);
        bolt.lineTo(580, 470);
        bolt.lineTo(510, 470);
        bolt.lineTo(550, 380);
        bolt.closeSubpath();
        p.setBrush(QColor(255, 255, 255, 230));
        p.drawPath(bolt);
    });
    add(sq("tech.wifi", "Wi-Fi Signal", "Tech", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        const QColor c = tintOr(s, QColor(255, 255, 255));
        const int lit = static_cast<int>(t * 2.5) % 5;   // 0..3 arcs light up in turn, then a pause
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPointF(500, 760), 60, 60);
        for (int i = 0; i < 3; ++i) {
            QColor k = c;
            k.setAlphaF(i < lit ? 1.0 : 0.25);
            p.setPen(QPen(k, 70, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            const double r = 190 + i * 160;
            p.drawArc(QRectF(500 - r, 760 - r, 2 * r, 2 * r), 45 * 16, 90 * 16);
        }
    });
    // ---- Callout
    add(sq("callout.cross", "Cross Out", "Callout", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        const QColor c = tintOr(s, QColor(229, 57, 53));
        p.setPen(QPen(c, 70, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        QPainterPath a, b;
        a.moveTo(220, 230);
        a.quadTo(500, 470, 790, 780);
        b.moveTo(780, 220);
        b.quadTo(480, 520, 230, 790);
        p.drawPath(partial(a, easeOutCubic(t / 0.35)));
        p.drawPath(partial(b, easeOutCubic((t - 0.3) / 0.35)));
    });
    add(sq("callout.magnifier", "Magnifier", "Callout", true, true), [](QPainter& p, SD s, SI, double t, double, double, double) {
        scaleAbout(p, popIn(t));
        p.translate(30 * std::sin(t * 1.6), 20 * std::sin(t * 2.1));
        const QColor c = tintOr(s, QColor(40, 44, 52));
        p.setPen(QPen(c, 70, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(640, 640), QPointF(850, 850));
        QPainterPath lens;
        lens.addEllipse(QPointF(430, 430), 260, 260);
        softShadow(p, lens, 14, 60);
        QRadialGradient g(QPointF(360, 360), 330);
        g.setColorAt(0, QColor(220, 240, 255, 150));
        g.setColorAt(1, QColor(120, 180, 230, 110));
        p.setPen(QPen(c, 46));
        p.setBrush(g);
        p.drawPath(lens);
        p.setPen(QPen(QColor(255, 255, 255, 190), 22, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(250, 250, 360, 360), static_cast<int>((120 + 10 * std::sin(t * 3)) * 16), 50 * 16);   // the glint
    });
    return v;
}

const std::vector<Entry>& entries() {
    static const std::vector<Entry> e = buildCatalog();
    return e;
}

}  // namespace

const std::vector<StickerInfo>& stickerCatalog() {
    static const std::vector<StickerInfo> v = [] {
        std::vector<StickerInfo> out;
        for (const auto& e : entries()) out.push_back(e.info);
        return out;
    }();
    return v;
}

const StickerInfo* findSticker(const std::string& id) {
    for (const auto& i : stickerCatalog())
        if (i.id == id) return &i;
    return nullptr;
}

const std::vector<std::string>& stickerCategories() {
    static const std::vector<std::string> c = {"Trending", "Subscribe", "Arrow",   "Line",    "Callout",  "Emoji",
                                               "Love",     "Like",      "Weather", "Travel",  "Music",    "Gaming",
                                               "Holiday",  "Sale",      "Tech",    "Particle", "Firework", "Badge",
                                               "Shape",    "Background", "Placeholder"};
    return c;
}

QImage stickerImage(const StickerData& sticker, double seconds, double clipSeconds, int w, int h) {
    QImage img(std::max(1, w), std::max(1, h), QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);
    const Entry* entry = nullptr;
    for (const auto& e : entries())
        if (e.info.id == sticker.id) entry = &e;
    if (entry) {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        if (entry->info.fullFrame) {
            entry->draw(p, sticker, entry->info, seconds, clipSeconds, w, h);
        } else {   // centred square design space
            const double side = std::min(w, h);
            p.translate((w - side) / 2.0, (h - side) / 2.0);
            p.scale(side / 1000.0, side / 1000.0);
            entry->draw(p, sticker, entry->info, seconds, clipSeconds, 1000, 1000);
        }
    }
    return img.convertToFormat(QImage::Format_RGBA8888);   // straight alpha, as the renderer expects
}

}  // namespace fa::text
