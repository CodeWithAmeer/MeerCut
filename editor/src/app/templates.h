// M9 templates: ready-made compositions (Filmora's Templates section) built from MeerCut's own titles, stickers and
// media placeholders. Inserting one pastes its layers onto free tracks at the playhead (one undo step); a placeholder
// becomes the user's video or picture with Replace Clip, keeping its place, size and crop.
#pragma once

#include "fa/model/edit_ops.h"

#include <string>
#include <vector>

namespace fa::ui {

struct TemplateInfo {
    std::string id;
    std::string name;
    std::string category;
    std::string description;
    double seconds = 5;
    bool trending = false;
};

const std::vector<TemplateInfo>& templateCatalog();
const TemplateInfo* findTemplate(const std::string& id);
const std::vector<std::string>& templateCategories();
// The template's clips starting at time 0, frame-aligned at `rate`, one pseudo-track ("tpl.<layer>") per layer from
// the bottom up. Paste them with edit::pasteClips(..., stackInOrder = true).
std::vector<edit::CopiedClip> templateClips(const std::string& id, const FrameRate& rate);

}  // namespace fa::ui
