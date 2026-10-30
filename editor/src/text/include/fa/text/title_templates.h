// Title templates (M3): a small set of original, fully working MeerCut presets in Filmora-like categories
// (no Filmora assets). A template is a clip prototype — title text, style, In/Loop/Out animations and
// placement — plus its default length. User presets ("My Titles") are stored as JSON in the user's
// application data folder by the editor.
#pragma once

#include "fa/model/project.h"

#include <string>
#include <vector>

namespace fa::text {

struct TitleTemplate {
    std::string id;
    std::string category;      // one of titleCategories(): "Basic", "Lower Thirds", ... (M11: "Social Media", "Callouts",
                               // "Quotes", "Promo", "Gaming", "Elegant"), "My Titles"
    std::string name;
    std::string description;
    Clip prototype;            // title + transform (placement) + opacity; timing is set when added
    Time duration{5};
    bool caption = false;      // "Subtitles": added as a caption on a subtitle track
};

const std::vector<std::string>& titleCategories();
const std::vector<TitleTemplate>& builtinTitleTemplates();
const TitleTemplate* findTitleTemplate(const std::string& id);
// The template Filmora's "Add Text" corresponds to (EXP-003 default render).
const TitleTemplate& defaultTitleTemplate();

}  // namespace fa::text
