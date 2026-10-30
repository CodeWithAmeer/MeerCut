// Subtitle files: SRT and ASS/SSA import and export (M3).
//
// Support boundaries (everything outside them is reported in SubtitleDoc::warnings, never silently
// dropped):
//   SRT  - numbered or unnumbered blocks, "HH:MM:SS,mmm --> HH:MM:SS,mmm" (also '.' and 1-3 digit
//          fractions), CRLF/LF/CR, UTF-8 / UTF-16 (BOM or detected) / system code page; whole-caption
//          <b> <i> <u> <font color> and {\anN}; tags around part of a caption are removed (warning);
//          display coordinates "X1:..." are ignored (warning).
//   ASS / SSA - [Script Info] PlayResX/PlayResY/WrapStyle, [V4+ Styles] and [V4 Styles], [Events]
//          Dialogue lines with the declared Format order. Style: font, size, primary/outline/back
//          colours with alpha, bold/italic/underline, spacing, BorderStyle 1 (outline + shadow) and 3
//          (opaque box), Outline, Shadow, Alignment 1-9 (SSA 1-11), MarginV. Override tags applied to
//          the whole line: \b \i \u \an \a \pos \fad \c \1c \3c \fs \fn \frz \fr \fscx \fscy \bord \shad,
//          plus \N \n \h. Not supported (counted per tag in the warnings): \move \fade \t \k \K \kf \ko
//          \clip \iclip \org \frx \fry \be \blur \fax \fay \alpha \1a-\4a \2c \4c \q \r \s \p (drawings
//          are removed), mid-line style changes, Comment/Picture/Sound/Movie/Command events, layers,
//          styles' ScaleX/ScaleY/Angle/StrikeOut/Encoding, MarginL/MarginR.
#pragma once

#include "fa/model/project.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fa {

struct Caption {
    Time start;
    Time end;
    std::string text;                      // UTF-8, '\n' = line break
    std::optional<TextStyle> style;        // set when the caption differs from the document style
    std::optional<Vec2> position;          // \pos: anchor point as a fraction of the frame (0..1)
    std::optional<double> rotation;        // \frz (converted to clockwise degrees)
    std::optional<Vec2> scale;             // \fscx/\fscy as factors
    Time fadeIn, fadeOut;                  // \fad
    friend bool operator==(const Caption&, const Caption&) = default;
};

struct SubtitleDoc {
    std::vector<Caption> captions;         // in file order
    TextStyle style = Track::defaultSubtitleStyle();   // document default style (ASS "Default" / most used)
    double subtitleY = 0.92;               // anchor y of bottom-aligned captions, fraction of the height
    std::string encoding;                  // what the text was decoded from
    std::vector<std::string> warnings;     // everything not imported exactly
};

// Text decoding: BOM (UTF-8, UTF-16LE/BE), else valid UTF-8, else UTF-16 without BOM (detected by
// zero bytes), else the given Windows code page (0 = the system ANSI code page, e.g. 1252 or 1256).
std::string decodeSubtitleText(std::string_view bytes, std::string* encoding = nullptr, unsigned codePage = 0);

SubtitleDoc parseSrt(std::string_view bytes, unsigned codePage = 0);
SubtitleDoc parseAss(std::string_view bytes, unsigned codePage = 0);
// By extension (.srt / .ass / .ssa) or content sniffing.
SubtitleDoc parseSubtitles(std::string_view bytes, const std::string& fileName, unsigned codePage = 0);

// Writers (UTF-8 with BOM for ASS, without for SRT; CRLF line ends). Times are written rounded to
// the nearest millisecond (SRT) / centisecond (ASS).
std::string writeSrt(const std::vector<Caption>& captions, const TextStyle& documentStyle);
std::string writeAss(const std::vector<Caption>& captions, const TextStyle& documentStyle, double subtitleY,
                     int playResX, int playResY);

// Captions of one subtitle track (time order), as a writer input.
std::vector<Caption> captionsOfTrack(const Track& track);

std::string formatSrtTime(const Time& t);   // HH:MM:SS,mmm
std::string formatAssTime(const Time& t);   // H:MM:SS.cc

}  // namespace fa
