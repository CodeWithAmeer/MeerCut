// Interface between the renderer and the text engine (M3). The engine (fa_text, Qt) turns a title
// into an RGBA picture at canvas resolution (M10: at the size it is shown, when its clip enlarges it); the renderer
// places that picture with the clip's transform like any other layer (masks, blend modes, opacity, CPU or GPU
// backend), so preview and export composite titles through exactly the same path as video.
#pragma once

#include "fa/core/image.h"
#include "fa/model/project.h"
#include "fa/model/title.h"
#include "fa/render/text_animation.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace fa {

struct TitleFrame {
    const Image* image = nullptr;   // straight-alpha RGBA; null = nothing visible
    double refX = 0.0, refY = 0.0;  // image pixels of the text block's reference point (TextStyle::anchor)
    double blockW = 0.0, blockH = 0.0;   // text block size in canvas pixels (the transform's anchor is relative to it)
    double pixelScale = 1.0;        // canvas pixels per image pixel: < 1 when drawn enlarged (M10), > 1 for oversized
                                    // titles rendered reduced
    TitleAnimState anim;            // whole-title animation still to apply when compositing (units already drawn)
    uint64_t contentKey = 0;        // non-zero: the same key is the same picture (static titles; lets GPU uploads be reused)
};

class TitleRenderer {
public:
    virtual ~TitleRenderer() = default;
    // `local` = time since the clip's start on the timeline; `duration` = the clip's length. `shownScale` (M10) = the
    // scale the clip shows the title at (its Scale): above 1 the engine draws the text that much larger
    // (pixelScale < 1), so enlarged text stays sharp instead of being a stretched picture. The returned image stays
    // valid until the next call.
    virtual TitleFrame render(const TitleData& title, const TextStyle& style, const Time& local, const Time& duration,
                              int canvasWidth, int canvasHeight, double shownScale = 1.0) = 0;
    // M9: a sticker's picture (square stickers: canvas height x canvas height; full-frame ones: the canvas; M10: larger
    // by `shownScale` when shown enlarged), placed by the renderer like a media frame. Engines without stickers draw
    // nothing.
    virtual TitleFrame renderSticker(const StickerData& /*sticker*/, const Time& /*local*/, const Time& /*duration*/,
                                     int /*canvasWidth*/, int /*canvasHeight*/, double /*shownScale*/ = 1.0) {
        return {};
    }
};

using TitleRendererFactory = std::function<std::unique_ptr<TitleRenderer>()>;

}  // namespace fa
