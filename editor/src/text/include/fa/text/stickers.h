// M9 stickers: MeerCut's own animated vector stickers, drawn by QPainter at the size they are shown (no bitmaps, no
// downloaded packs). A sticker clip (fa::StickerData) names one of them by id; the renderer places its picture with
// the clip's transform like media. Square stickers are drawn at the canvas height; full-frame ones (backgrounds,
// overlays, media placeholders) at the canvas size.
#pragma once

#include "fa/model/project.h"

#include <QImage>
#include <QString>

#include <string>
#include <vector>

namespace fa::text {

struct StickerInfo {
    std::string id;            // "emoji.smile"
    std::string name;          // shown in the Stickers page
    std::string category;      // "Emoji", "Arrow", ...
    bool fullFrame = false;    // drawn at the canvas size (backgrounds, overlays, placeholders)
    bool animated = false;     // the picture changes over time
    bool tintable = false;     // StickerData::color recolours it
    std::string defaultText;   // stickers that show a word (badges, buttons, bubbles); empty = none
    double defaultSeconds = 5.0;
    double defaultScale = 0.45;   // the clip's transform scale when added (full-frame: 1)
};

const std::vector<StickerInfo>& stickerCatalog();
const StickerInfo* findSticker(const std::string& id);
// The categories in the order the Stickers page lists them.
const std::vector<std::string>& stickerCategories();

// The sticker's picture at `seconds` into a clip of `clipSeconds`, w x h, straight-alpha RGBA8888. Unknown ids give a
// transparent picture.
QImage stickerImage(const StickerData& sticker, double seconds, double clipSeconds, int w, int h);

}  // namespace fa::text
