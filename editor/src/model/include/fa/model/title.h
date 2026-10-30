// Text / title model (M3): titles are clips without media whose picture is generated from TitleData;
// captions are title clips on subtitle tracks that normally take the track's style.
//
// Units (all resolution independent, so a project looks the same at any sequence or preview size):
//   * size: MeerCut/Filmora font size. 1 unit = 110/48 px of em height at 1080p (EXP-003: Arial,
//     FontSize 48 rendered with a 110 px em in a 1080p export); scales with the canvas height.
//   * lengths (outline width, spacing, padding, radius, glow, blur): pixels at 1080p.
//   * shadowDistance: Filmora's unit, 3 px at 1080p per unit (EXP-003: Distance 8 -> 24 px).
//   * colours: 0xRRGGBB (as Filmora stores them, EXP-003); opacities 0..1.
#pragma once

#include "fa/core/time.h"

#include <cstdint>
#include <string>

namespace fa {

enum class TextAlign { Left, Center, Right };
// Which point of the text block the clip position refers to (titles: centre; captions: bottom).
enum class TextAnchor { Center, Bottom, Top };

inline constexpr double kTitlePxPerSizeAt1080 = 110.0 / 48.0;   // em px per size unit at 1080p (EXP-003)
inline constexpr double kShadowPxPerUnitAt1080 = 3.0;          // EXP-003: Distance 8 -> 24 px
inline constexpr size_t kMaxTitleChars = 1500;                 // Filmora's text box limit (EXP-003 part 2)

struct TextStyle {
    std::string font = "Segoe UI";
    double size = 48.0;
    int weight = 400;               // 100 (thin) .. 900 (black); 700 = bold
    bool italic = false;
    bool underline = false;
    uint32_t color = 0xFFFFFF;
    double opacity = 1.0;
    // gradient fill from `color` to `gradientColor` along `gradientAngle` (degrees, 90 = top -> bottom)
    bool gradient = false;
    uint32_t gradientColor = 0x2E9BFF;
    double gradientAngle = 90.0;
    TextAlign align = TextAlign::Center;
    TextAnchor anchor = TextAnchor::Center;
    double charSpacing = 0.0;       // extra space between characters, px at 1080p
    double lineSpacing = 1.0;       // multiple of the font's line height
    double wrapWidth = 0.0;         // wrap at this fraction of the canvas width; 0 = explicit line breaks only
    // outline drawn outside the glyphs (EXP-003: Border Size 5 -> ~5 px per side)
    bool outline = false;
    double outlineWidth = 5.0;
    uint32_t outlineColor = 0x000000;
    double outlineOpacity = 1.0;
    // drop shadow of fill + outline
    bool shadow = false;
    uint32_t shadowColor = 0x000000;
    double shadowOpacity = 0.75;
    double shadowDistance = 8.0;    // Filmora units (x3 px at 1080p)
    double shadowAngle = 90.0;      // direction the shadow falls, degrees clockwise from +x (90 = down)
    double shadowBlur = 3.0;        // Gaussian sigma, px at 1080p
    // soft glow around fill + outline
    bool glow = false;
    uint32_t glowColor = 0x40C8FF;
    double glowSize = 12.0;         // Gaussian sigma, px at 1080p
    double glowOpacity = 0.85;
    // box behind the text block
    bool background = false;
    uint32_t backgroundColor = 0x000000;
    double backgroundOpacity = 0.6;
    double backgroundPadding = 16.0;
    double backgroundRadius = 8.0;
    friend bool operator==(const TextStyle&, const TextStyle&) = default;
};

// One of the three independent animation slots (Filmora: inAnimation / loopAnimation / outAnimation,
// EXP-003). preset = "" means no animation; duration 0 = the preset's default length (In/Out) or
// period (Loop).
struct TextAnimation {
    std::string preset;
    Time duration;
    bool active() const { return !preset.empty(); }
    friend bool operator==(const TextAnimation&, const TextAnimation&) = default;
};

struct TitleData {
    std::string text = "Text Here";   // UTF-8, '\n' = line break
    TextStyle style;
    TextAnimation in, loop, out;
    bool followTrackStyle = false;    // captions: use the subtitle track's style instead of `style`
    std::string templateId;           // title template it was created from (informational)
    friend bool operator==(const TitleData&, const TitleData&) = default;
};

}  // namespace fa
