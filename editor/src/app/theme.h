// Visual identity: original dark theme, colours and programmatically drawn icons (no third-party art).
#pragma once

#include <QColor>
#include <QIcon>
#include <QImage>
#include <QString>

class QApplication;

namespace fa::ui {

inline constexpr const char* kAppName = "MeerCut";   // the editor's name (Lumacut until 2026-10-28)
inline constexpr const char* kAuthorName = "CodeWithAmeer";   // its author ...
inline constexpr const char* kAuthorUrl = "https://github.com/CodeWithAmeer";   // ... and MeerCut's official GitHub

namespace color {
inline const QColor window{0x16, 0x18, 0x1D};
inline const QColor panel{0x1D, 0x20, 0x26};
inline const QColor panelHeader{0x23, 0x27, 0x2E};
inline const QColor border{0x2E, 0x33, 0x3B};
inline const QColor text{0xD9, 0xDE, 0xE6};
inline const QColor textDim{0x8A, 0x93, 0xA3};
inline const QColor accent{0xF2, 0xA9, 0x3B};        // amber
inline const QColor accentBright{0xFF, 0xC4, 0x66};
inline const QColor playhead{0xFF, 0x5A, 0x5F};
inline const QColor videoClip{0x4A, 0x5C, 0xC9};
inline const QColor videoClipDark{0x2F, 0x3A, 0x85};
inline const QColor audioClip{0x2F, 0x8F, 0x62};
inline const QColor audioClipDark{0x1E, 0x5C, 0x40};
inline const QColor waveform{0x9F, 0xE8, 0xC3};
inline const QColor transition{0xE0, 0x86, 0xF0};
inline const QColor track{0x1A, 0x1D, 0x22};
inline const QColor trackAlt{0x1F, 0x22, 0x28};
// M9: clip colour marks 1..13 (Clip::colorMark), muted so that clip names stay readable on them
inline const QColor colorMarks[13] = {{0x9C, 0x3B, 0x52}, {0xA8, 0x4E, 0x2C}, {0x9A, 0x7A, 0x2A}, {0x2B, 0x86, 0x7A},
                                      {0x23, 0x86, 0x9E}, {0x3D, 0x5B, 0xB5}, {0x6E, 0x4E, 0xB8}, {0x6B, 0x70, 0x78},
                                      {0x5E, 0x75, 0x2E}, {0x3B, 0x85, 0x50}, {0x8F, 0x83, 0x2E}, {0xA0, 0x3C, 0x3C},
                                      {0x8C, 0x5A, 0x24}};
}  // namespace color

void applyTheme(QApplication& app);

// Icons drawn with QPainter (crisp at any DPI). Names: play, pause, stop, prev, next, start, end, undo,
// redo, delete, split, ripple, magnet, link, zoomIn, zoomOut, fit, camera, volume, mute, eye, eyeOff,
// lock, unlock, plus, import, export, media, transitions, dissolve, save, open, newfile, gear, logo.
QIcon icon(const QString& name, const QColor& tint = color::text);
// One icon drawn at size x size pixels (transparent background): the application icon file is made from "logo".
QImage renderIcon(const QString& name, int size, const QColor& tint = color::text);

}  // namespace fa::ui
