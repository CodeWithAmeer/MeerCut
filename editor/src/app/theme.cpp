#include "theme.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QStyleFactory>

#include <cmath>
#include <map>

namespace fa::ui {

void applyTheme(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, color::window);
    p.setColor(QPalette::WindowText, color::text);
    p.setColor(QPalette::Base, color::panel);
    p.setColor(QPalette::AlternateBase, color::panelHeader);
    p.setColor(QPalette::Text, color::text);
    p.setColor(QPalette::Button, color::panelHeader);
    p.setColor(QPalette::ButtonText, color::text);
    p.setColor(QPalette::Highlight, color::accent);
    p.setColor(QPalette::HighlightedText, QColor(0x1A, 0x14, 0x08));
    p.setColor(QPalette::ToolTipBase, color::panelHeader);
    p.setColor(QPalette::ToolTipText, color::text);
    p.setColor(QPalette::PlaceholderText, color::textDim);
    p.setColor(QPalette::Disabled, QPalette::Text, color::textDim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, color::textDim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, color::textDim);
    app.setPalette(p);

    const QString accent = color::accent.name();
    app.setStyleSheet(QStringLiteral(R"(
        QMainWindow, QDialog { background: %1; }
        QMenuBar { background: %1; color: %3; padding: 2px 4px; }
        QMenuBar::item { padding: 4px 10px; border-radius: 4px; }
        QMenuBar::item:selected { background: %2; }
        QMenu { background: %2; border: 1px solid %4; padding: 4px; }
        QMenu::item { padding: 5px 24px 5px 22px; border-radius: 3px; }
        QMenu::item:selected { background: %5; color: #1A1408; }
        QMenu::item:disabled { color: %6; }
        QMenu::separator { height: 1px; background: %4; margin: 4px 8px; }
        QToolTip { background: %2; color: %3; border: 1px solid %4; padding: 4px; }
        QSplitter::handle { background: %1; }
        QSplitter::handle:hover { background: %4; }
        QToolButton { border: none; border-radius: 4px; padding: 4px; color: %3; }
        QToolButton:hover { background: %4; }
        QToolButton:checked { background: #3A3220; color: %5; }
        QToolButton:disabled { color: %6; }
        QPushButton { background: %2; border: 1px solid %4; border-radius: 4px; padding: 5px 14px; color: %3; }
        QPushButton:hover { border-color: %5; }
        QPushButton:disabled { color: %6; }
        QPushButton#accentButton { background: %5; color: #1A1408; border: none; font-weight: 600; padding: 6px 18px; }
        QPushButton#accentButton:hover { background: %7; }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QTimeEdit {
            background: #14161A; border: 1px solid %4; border-radius: 4px; padding: 4px 6px; color: %3;
            selection-background-color: %5; selection-color: #1A1408; }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: %5; }
        QComboBox QAbstractItemView { background: %2; border: 1px solid %4; selection-background-color: %5; }
        QSlider::groove:horizontal { height: 4px; background: %4; border-radius: 2px; }
        QSlider::sub-page:horizontal { background: %5; border-radius: 2px; }
        QSlider::handle:horizontal { width: 12px; margin: -5px 0; border-radius: 6px; background: %3; }
        QScrollBar:horizontal { height: 10px; background: %1; }
        QScrollBar:vertical { width: 10px; background: %1; }
        QScrollBar::handle { background: %4; border-radius: 4px; min-width: 24px; min-height: 24px; }
        QScrollBar::handle:hover { background: #4A515C; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: none; }
        QTabBar::tab { background: transparent; color: %6; padding: 6px 12px; border: none; }
        QTabBar::tab:selected { color: %5; border-bottom: 2px solid %5; }
        QTabBar::tab:hover { color: %3; }
        QTabWidget::pane { border: none; }
        QListView, QTreeView { background: %2; border: none; outline: none; }
        QListView::item:selected { background: #3A3220; border-radius: 4px; color: %5; }
        QGroupBox { border: 1px solid %4; border-radius: 6px; margin-top: 14px; padding: 8px; }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: %3; }
        QCheckBox::indicator { width: 14px; height: 14px; }
        QProgressBar { background: #14161A; border: 1px solid %4; border-radius: 4px; text-align: center; color: %3; }
        QProgressBar::chunk { background: %5; border-radius: 3px; }
        QStatusBar { background: %1; color: %6; }
        QLabel#panelTitle { color: %6; font-weight: 600; letter-spacing: 1px; }
        QFrame#panel { background: %2; border-radius: 6px; }
    )")
                           .arg(color::window.name(), color::panel.name(), color::text.name(), color::border.name(),
                                accent, color::textDim.name(), color::accentBright.name()));
}

namespace {

void drawIcon(QPainter& p, const QString& n, const QColor& c) {
    // 24x24 design grid
    QPen pen(c, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    auto fillPath = [&](const QPainterPath& path) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPath(path);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
    };
    auto tri = [&](QPointF a, QPointF b, QPointF d) {
        QPainterPath path;
        path.moveTo(a);
        path.lineTo(b);
        path.lineTo(d);
        path.closeSubpath();
        fillPath(path);
    };
    if (n == "play") tri({7, 5}, {19, 12}, {7, 19});
    else if (n == "pause") {
        fillPath([] { QPainterPath q; q.addRoundedRect(6, 5, 4, 14, 1, 1); q.addRoundedRect(14, 5, 4, 14, 1, 1); return q; }());
    } else if (n == "stop") {
        fillPath([] { QPainterPath q; q.addRoundedRect(6, 6, 12, 12, 2, 2); return q; }());
    } else if (n == "prev") {
        tri({17, 6}, {9, 12}, {17, 18});
        p.drawLine(QPointF(7, 6), QPointF(7, 18));
    } else if (n == "next") {
        tri({7, 6}, {15, 12}, {7, 18});
        p.drawLine(QPointF(17, 6), QPointF(17, 18));
    } else if (n == "start") {
        tri({19, 6}, {12, 12}, {19, 18});
        tri({12, 6}, {5, 12}, {12, 18});
    } else if (n == "end") {
        tri({5, 6}, {12, 12}, {5, 18});
        tri({12, 6}, {19, 12}, {12, 18});
    } else if (n == "undo" || n == "redo") {
        if (n == "redo") {
            p.translate(24, 0);
            p.scale(-1, 1);
        }
        QPainterPath a;
        a.moveTo(9, 9);
        a.lineTo(15, 9);
        a.cubicTo(19, 9, 19, 18, 15, 18);
        a.lineTo(8, 18);
        p.drawPath(a);
        tri({4, 9}, {9, 5}, {9, 13});
    } else if (n == "delete") {
        p.drawLine(QPointF(5, 7), QPointF(19, 7));
        p.drawLine(QPointF(10, 4.5), QPointF(14, 4.5));
        QPainterPath b;
        b.moveTo(7, 7);
        b.lineTo(8, 19);
        b.lineTo(16, 19);
        b.lineTo(17, 7);
        p.drawPath(b);
        p.drawLine(QPointF(10.5, 10), QPointF(10.5, 16));
        p.drawLine(QPointF(13.5, 10), QPointF(13.5, 16));
    } else if (n == "split") {
        p.drawLine(QPointF(12, 3), QPointF(12, 21));
        p.drawEllipse(QPointF(6.5, 16.5), 2.5, 2.5);
        p.drawEllipse(QPointF(17.5, 16.5), 2.5, 2.5);
        p.drawLine(QPointF(8.3, 14.6), QPointF(15.8, 6));
        p.drawLine(QPointF(15.7, 14.6), QPointF(8.2, 6));
    } else if (n == "ripple") {
        p.drawRect(QRectF(3, 8, 6, 8));
        p.drawRect(QRectF(15, 8, 6, 8));
        tri({14, 12}, {10.5, 9}, {10.5, 15});
    } else if (n == "magnet") {
        QPainterPath m;
        m.moveTo(6, 5);
        m.lineTo(6, 13);
        m.cubicTo(6, 21, 18, 21, 18, 13);
        m.lineTo(18, 5);
        p.drawPath(m);
        p.drawLine(QPointF(4, 8), QPointF(8, 8));
        p.drawLine(QPointF(16, 8), QPointF(20, 8));
    } else if (n == "link") {
        p.drawRoundedRect(QRectF(3, 9, 10, 6), 3, 3);
        p.drawRoundedRect(QRectF(11, 9, 10, 6), 3, 3);
    } else if (n == "zoomIn" || n == "zoomOut") {
        p.drawEllipse(QPointF(10, 10), 6, 6);
        p.drawLine(QPointF(14.5, 14.5), QPointF(20, 20));
        p.drawLine(QPointF(7, 10), QPointF(13, 10));
        if (n == "zoomIn") p.drawLine(QPointF(10, 7), QPointF(10, 13));
    } else if (n == "fit") {
        p.drawLine(QPointF(4, 12), QPointF(20, 12));
        tri({3, 12}, {8, 8}, {8, 16});
        tri({21, 12}, {16, 8}, {16, 16});
        p.drawLine(QPointF(3, 5), QPointF(3, 19));
        p.drawLine(QPointF(21, 5), QPointF(21, 19));
    } else if (n == "camera") {
        p.drawRoundedRect(QRectF(3, 7, 18, 12), 2, 2);
        p.drawEllipse(QPointF(12, 13), 3.5, 3.5);
        p.drawLine(QPointF(8, 7), QPointF(9.5, 4.5));
        p.drawLine(QPointF(9.5, 4.5), QPointF(14.5, 4.5));
        p.drawLine(QPointF(14.5, 4.5), QPointF(16, 7));
    } else if (n == "volume" || n == "mute") {
        QPainterPath s;
        s.moveTo(4, 9);
        s.lineTo(8, 9);
        s.lineTo(13, 5);
        s.lineTo(13, 19);
        s.lineTo(8, 15);
        s.lineTo(4, 15);
        s.closeSubpath();
        fillPath(s);
        if (n == "volume") {
            p.drawArc(QRectF(11, 8, 6, 8), -60 * 16, 120 * 16);
            p.drawArc(QRectF(11, 5, 10, 14), -60 * 16, 120 * 16);
        } else {
            p.drawLine(QPointF(16, 9), QPointF(21, 15));
            p.drawLine(QPointF(21, 9), QPointF(16, 15));
        }
    } else if (n == "eye" || n == "eyeOff") {
        QPainterPath e;
        e.moveTo(3, 12);
        e.cubicTo(7, 5, 17, 5, 21, 12);
        e.cubicTo(17, 19, 7, 19, 3, 12);
        p.drawPath(e);
        p.drawEllipse(QPointF(12, 12), 2.5, 2.5);
        if (n == "eyeOff") p.drawLine(QPointF(4, 20), QPointF(20, 4));
    } else if (n == "lock" || n == "unlock") {
        p.drawRoundedRect(QRectF(5, 11, 14, 9), 2, 2);
        QPainterPath s;
        if (n == "lock") {
            s.moveTo(8, 11);
            s.lineTo(8, 8);
            s.cubicTo(8, 3, 16, 3, 16, 8);
            s.lineTo(16, 11);
        } else {
            s.moveTo(8, 11);
            s.lineTo(8, 8);
            s.cubicTo(8, 3, 16, 3, 16, 7);
        }
        p.drawPath(s);
    } else if (n == "plus") {
        p.drawLine(QPointF(12, 5), QPointF(12, 19));
        p.drawLine(QPointF(5, 12), QPointF(19, 12));
    } else if (n == "import" || n == "export") {
        p.drawLine(QPointF(12, 3), QPointF(12, 14));
        if (n == "import") tri({12, 16}, {8, 11}, {16, 11});
        else tri({12, 2}, {8, 7}, {16, 7});
        QPainterPath t;
        t.moveTo(4, 13);
        t.lineTo(4, 20);
        t.lineTo(20, 20);
        t.lineTo(20, 13);
        p.drawPath(t);
    } else if (n == "media") {
        p.drawRoundedRect(QRectF(3, 5, 18, 14), 2, 2);
        for (int i = 0; i < 4; ++i) {
            p.drawPoint(QPointF(6 + i * 4, 7.5));
            p.drawPoint(QPointF(6 + i * 4, 16.5));
        }
        tri({10, 9.5}, {15, 12}, {10, 14.5});
    } else if (n == "transitions" || n == "dissolve") {
        QPainterPath a;
        a.moveTo(3, 5);
        a.lineTo(12, 12);
        a.lineTo(3, 19);
        a.closeSubpath();
        QPainterPath b;
        b.moveTo(21, 5);
        b.lineTo(12, 12);
        b.lineTo(21, 19);
        b.closeSubpath();
        p.drawPath(a);
        fillPath(b);
    } else if (n == "save") {
        p.drawRoundedRect(QRectF(4, 4, 16, 16), 2, 2);
        p.drawRect(QRectF(8, 4, 8, 5));
        p.drawRect(QRectF(7, 13, 10, 7));
    } else if (n == "open") {
        QPainterPath f;
        f.moveTo(3, 18);
        f.lineTo(3, 6);
        f.lineTo(9, 6);
        f.lineTo(11, 8);
        f.lineTo(19, 8);
        f.lineTo(19, 11);
        p.drawPath(f);
        QPainterPath g;
        g.moveTo(3, 18);
        g.lineTo(6, 11);
        g.lineTo(22, 11);
        g.lineTo(19, 18);
        g.closeSubpath();
        p.drawPath(g);
    } else if (n == "newfile") {
        QPainterPath f;
        f.moveTo(6, 3);
        f.lineTo(14, 3);
        f.lineTo(19, 8);
        f.lineTo(19, 21);
        f.lineTo(6, 21);
        f.closeSubpath();
        p.drawPath(f);
        p.drawLine(QPointF(12.5, 11), QPointF(12.5, 17));
        p.drawLine(QPointF(9.5, 14), QPointF(15.5, 14));
    } else if (n == "gear") {
        p.drawEllipse(QPointF(12, 12), 3, 3);
        for (int i = 0; i < 8; ++i) {
            const double a = i * 3.14159265 / 4;
            p.drawLine(QPointF(12 + 6 * std::cos(a), 12 + 6 * std::sin(a)), QPointF(12 + 8.5 * std::cos(a), 12 + 8.5 * std::sin(a)));
        }
        p.drawEllipse(QPointF(12, 12), 6, 6);
    } else if (n == "keyOn" || n == "keyOff" || n == "keyAnim") {
        QPainterPath d;
        d.moveTo(12, 5);
        d.lineTo(19, 12);
        d.lineTo(12, 19);
        d.lineTo(5, 12);
        d.closeSubpath();
        if (n == "keyOn") fillPath(d);
        else p.drawPath(d);
        if (n == "keyAnim") p.drawPoint(QPointF(12, 12));
    } else if (n == "prevKey") {
        tri({15, 7}, {9, 12}, {15, 17});
    } else if (n == "nextKey") {
        tri({9, 7}, {15, 12}, {9, 17});
    } else if (n == "flipH" || n == "flipV") {
        if (n == "flipV") {
            p.translate(24, 0);
            p.rotate(90);
        }
        p.drawLine(QPointF(12, 3), QPointF(12, 21));
        QPainterPath l;
        l.moveTo(9, 6);
        l.lineTo(3, 18);
        l.lineTo(9, 18);
        l.closeSubpath();
        fillPath(l);
        QPainterPath r;
        r.moveTo(15, 6);
        r.lineTo(21, 18);
        r.lineTo(15, 18);
        r.closeSubpath();
        p.drawPath(r);
    } else if (n == "rotateLeft" || n == "rotateRight") {
        if (n == "rotateRight") {
            p.translate(24, 0);
            p.scale(-1, 1);
        }
        p.drawArc(QRectF(5, 5, 14, 14), 90 * 16, -270 * 16);
        tri({12, 1.5}, {7.5, 5}, {12, 8.5});
    } else if (n == "maskRect") {
        p.drawRect(QRectF(4, 6, 16, 12));
    } else if (n == "maskEllipse") {
        p.drawEllipse(QPointF(12, 12), 8.5, 6.5);
    } else if (n == "maskCustom") {
        QPainterPath s;
        s.moveTo(12, 4);
        s.lineTo(20, 10);
        s.lineTo(17, 20);
        s.lineTo(7, 20);
        s.lineTo(4, 10);
        s.closeSubpath();
        p.drawPath(s);
        for (QPointF q : {QPointF(12, 4), QPointF(20, 10), QPointF(17, 20), QPointF(7, 20), QPointF(4, 10)})
            p.drawEllipse(q, 1.2, 1.2);
    } else if (n == "speed") {   // M5: a gauge with its needle
        p.drawArc(QRectF(3, 6, 18, 18), 0, 180 * 16);
        p.drawLine(QPointF(12, 15), QPointF(17, 9));
        p.drawLine(QPointF(4, 15), QPointF(6, 15));
        p.drawLine(QPointF(18, 15), QPointF(20, 15));
    } else if (n == "render") {   // M5 Render Preview: a film frame with a play triangle, a rendered line under it
        p.drawRoundedRect(QRectF(3, 4, 18, 13), 2, 2);
        QPainterPath tri;
        tri.moveTo(10, 7.5);
        tri.lineTo(15.5, 10.5);
        tri.lineTo(10, 13.5);
        tri.closeSubpath();
        p.drawPath(tri);
        p.drawLine(QPointF(3, 20.5), QPointF(21, 20.5));
    } else if (n == "freeze") {   // a frame with a snowflake
        p.drawRect(QRectF(3, 4, 18, 16));
        for (int i = 0; i < 3; ++i) {
            p.save();
            p.translate(12, 12);
            p.rotate(60 * i);
            p.drawLine(QPointF(0, -5.5), QPointF(0, 5.5));
            p.restore();
        }
    } else if (n == "reverse") {   // two arrow heads pointing left
        QPainterPath a;
        a.moveTo(12, 6);
        a.lineTo(5, 12);
        a.lineTo(12, 18);
        a.moveTo(19, 6);
        a.lineTo(12, 12);
        a.lineTo(19, 18);
        p.drawPath(a);
    } else if (n == "compare") {   // a frame split in two halves, the right one filled
        p.drawRect(QRectF(4, 6, 16, 12));
        p.drawLine(QPointF(12, 4), QPointF(12, 20));
        p.save();
        p.setBrush(c);
        p.drawRect(QRectF(12, 6, 8, 12));
        p.restore();
    } else if (n == "fullscreen") {
        for (int i = 0; i < 4; ++i) {
            p.save();
            p.translate(12, 12);
            p.rotate(90 * i);
            p.drawLine(QPointF(-8, -4), QPointF(-8, -8));
            p.drawLine(QPointF(-8, -8), QPointF(-4, -8));
            p.restore();
        }
    } else if (n == "title") {   // T
        p.drawLine(QPointF(5, 5), QPointF(19, 5));
        p.drawLine(QPointF(12, 5), QPointF(12, 20));
        p.drawLine(QPointF(9, 20), QPointF(15, 20));
    } else if (n == "up" || n == "down") {   // chevron
        if (n == "down") {
            p.translate(0, 24);
            p.scale(1, -1);
        }
        p.drawLine(QPointF(6, 14), QPointF(12, 8));
        p.drawLine(QPointF(12, 8), QPointF(18, 14));
    } else if (n == "copy") {   // two sheets
        p.drawRoundedRect(QRectF(8, 8, 11, 12), 1.5, 1.5);
        p.drawLine(QPointF(5, 16), QPointF(5, 5));
        p.drawLine(QPointF(5, 5), QPointF(14, 5));
    } else if (n == "paste") {   // clipboard
        p.drawRoundedRect(QRectF(6, 6, 12, 14), 1.5, 1.5);
        p.drawRoundedRect(QRectF(9, 4, 6, 4), 1, 1);
    } else if (n == "effects") {   // sparkle
        p.drawLine(QPointF(12, 3), QPointF(12, 21));
        p.drawLine(QPointF(3, 12), QPointF(21, 12));
        p.drawLine(QPointF(6, 6), QPointF(18, 18));
        p.drawLine(QPointF(18, 6), QPointF(6, 18));
        fillPath([] { QPainterPath q; q.addEllipse(QPointF(12, 12), 3.5, 3.5); return q; }());
    } else if (n == "filters") {   // three overlapping circles
        p.drawEllipse(QPointF(9, 10), 5, 5);
        p.drawEllipse(QPointF(15, 10), 5, 5);
        p.drawEllipse(QPointF(12, 15), 5, 5);
    } else if (n == "adjustment") {   // half-filled square
        p.drawRoundedRect(QRectF(4, 4, 16, 16), 2, 2);
        fillPath([] { QPainterPath q; q.moveTo(4, 20); q.lineTo(20, 4); q.lineTo(20, 20); q.closeSubpath(); return q; }());
    } else if (n == "alignLeft" || n == "alignCenter" || n == "alignRight") {   // four text lines
        const double widths[4] = {16, 10, 14, 8};
        for (int i = 0; i < 4; ++i) {
            const double w = widths[i], y = 6 + 4 * i;
            const double x0 = n == "alignLeft" ? 4 : n == "alignRight" ? 20 - w : 12 - w / 2;
            p.drawLine(QPointF(x0, y), QPointF(x0 + w, y));
        }
    } else if (n == "captions") {   // speech box with lines
        p.drawRoundedRect(QRectF(3, 5, 18, 13), 2, 2);
        p.drawLine(QPointF(7, 10), QPointF(17, 10));
        p.drawLine(QPointF(7, 14), QPointF(13, 14));
    } else if (n == "music") {   // M9: two beamed notes
        p.drawLine(QPointF(9, 6), QPointF(9, 17));
        p.drawLine(QPointF(19, 4), QPointF(19, 15));
        p.drawLine(QPointF(9, 6), QPointF(19, 4));
        p.drawLine(QPointF(9, 9), QPointF(19, 7));
        p.drawEllipse(QPointF(6.5, 17.5), 2.6, 2.1);
        p.drawEllipse(QPointF(16.5, 15.5), 2.6, 2.1);
    } else if (n == "sticker") {   // M9: a sticker with a peeled corner and a smile
        QPainterPath s;
        s.moveTo(4, 4);
        s.lineTo(20, 4);
        s.lineTo(20, 14);
        s.lineTo(14, 20);
        s.lineTo(4, 20);
        s.closeSubpath();
        p.drawPath(s);
        p.drawLine(QPointF(14, 20), QPointF(14, 14));
        p.drawLine(QPointF(14, 14), QPointF(20, 14));
        p.drawArc(QRectF(7, 7, 8, 7), 200 * 16, 140 * 16);
    } else if (n == "crop") {   // M9: two crop corners
        p.drawLine(QPointF(7, 3), QPointF(7, 17));
        p.drawLine(QPointF(7, 17), QPointF(21, 17));
        p.drawLine(QPointF(3, 7), QPointF(17, 7));
        p.drawLine(QPointF(17, 7), QPointF(17, 21));
    } else if (n == "template") {   // M9: a layout of three blocks
        p.drawRoundedRect(QRectF(3.5, 4.5, 17, 15), 2, 2);
        p.drawLine(QPointF(12, 4.5), QPointF(12, 19.5));
        p.drawLine(QPointF(12, 12), QPointF(20.5, 12));
    } else if (n == "logo") {
        // amber rounded square with a light "cut" stroke through a play shape
        p.setPen(Qt::NoPen);
        p.setBrush(color::accent);
        p.drawRoundedRect(QRectF(1.5, 1.5, 21, 21), 5, 5);
        p.setBrush(QColor(0x1A, 0x14, 0x08));
        QPainterPath t;
        t.moveTo(8, 6);
        t.lineTo(18, 12);
        t.lineTo(8, 18);
        t.closeSubpath();
        p.drawPath(t);
        p.setPen(QPen(color::accent, 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(5, 17), QPointF(19, 7));
    }
}

}  // namespace

QImage renderIcon(const QString& name, int size, const QColor& tint) {
    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 24.0, size / 24.0);
    drawIcon(p, name, tint);
    p.end();
    return img;
}

QIcon icon(const QString& name, const QColor& tint) {
    static std::map<QString, QIcon> cache;
    const QString key = name + tint.name(QColor::HexArgb);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    QIcon result;
    for (int size : {16, 24, 32, 48}) {
        for (auto mode : {QIcon::Normal, QIcon::Disabled}) {
            QPixmap pm(size * 2, size * 2);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(size * 2 / 24.0, size * 2 / 24.0);
            drawIcon(p, name, mode == QIcon::Disabled ? color::textDim.darker(130) : tint);
            p.end();
            pm.setDevicePixelRatio(2.0);
            result.addPixmap(pm, mode);
        }
    }
    cache.emplace(key, result);
    return result;
}

}  // namespace fa::ui
