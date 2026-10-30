// Text engine (M3): lays out and rasterises titles and captions with Qt's text stack —
// QTextLayout (Unicode bidi, HarfBuzz shaping, line breaking, font fallback incl. colour emoji) and
// QPainter (anti-aliased glyphs, outlines from glyph paths, gradients) — plus our own Gaussian blur
// for shadows, glows and blur animations.
//
//   * Layout is computed once per text/style at a 1080p reference with design metrics (no hinting), so
//     line breaks and proportions are identical at every output size (preview and export).
//   * Animation units are grapheme clusters (QTextBoundaryFinder), words and lines of the shaped
//     layout; a unit owns the glyphs shaped for it (ligatures and Arabic joining are kept: glyphs are
//     never re-shaped per character).
//   * Static frames are cached; frames with per-unit animation re-compose the cached glyph data
//     (no re-shaping, no re-layout).
// Needs a QGuiApplication. Not thread-safe: use one engine per rendering thread.
#pragma once

#include "fa/render/title_renderer.h"

#include <memory>
#include <string>
#include <vector>

namespace fa::text {

struct UnitBox {
    double x = 0, y = 0, w = 0, h = 0;   // px at 1080p, relative to the text block's top-left
};

struct LayoutInfo {
    double blockW = 0, blockH = 0;       // px at 1080p
    double emPx = 0;                     // em size at 1080p
    TextUnitCounts counts;
    std::vector<UnitBox> chars;          // per grapheme cluster (spaces have empty boxes)
    std::vector<UnitBox> lines;
    std::vector<std::string> fontsUsed;  // families actually used (fallback visible here)
    bool rightToLeft = false;            // first paragraph direction
};

class TitleEngine final : public TitleRenderer {
public:
    TitleEngine();
    ~TitleEngine() override;
    TitleFrame render(const TitleData& title, const TextStyle& style, const Time& local, const Time& duration,
                      int canvasWidth, int canvasHeight, double shownScale = 1.0) override;
    TitleFrame renderSticker(const StickerData& sticker, const Time& local, const Time& duration, int canvasWidth,
                             int canvasHeight, double shownScale = 1.0) override;   // M9
    // Layout of a title at a canvas aspect ratio (wrap widths depend on it).
    LayoutInfo layout(const std::string& text, const TextStyle& style, double aspect = 16.0 / 9.0);

    struct Stats {
        uint64_t layouts = 0;        // layouts built (cache misses)
        uint64_t staticHits = 0;     // frames served from the static picture cache
        uint64_t rasterised = 0;     // frames drawn
    };
    const Stats& stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

std::unique_ptr<TitleRenderer> makeTitleEngine();

// Creates a QGuiApplication for command-line tools if none exists (Windows platform plugin; no window
// is shown). Returns false if Qt's GUI layer cannot start.
bool ensureGuiApplication();

}  // namespace fa::text
