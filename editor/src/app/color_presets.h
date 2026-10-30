// Custom colour presets (M4, Color tab "Save as Custom"): the Color-tab effects of a clip (basic, HSL, curves,
// wheels, sharpen, vignette) with their values at the playhead, saved by name in color_presets.json in the app
// data folder (MEERCUT_COLOR_PRESETS overrides the file). Applying one replaces the target clip's Color-tab
// effects in one undo step. There are no built-in colour presets: MeerCut ships no third-party looks.
#pragma once

#include "fa/model/project.h"

#include <QObject>
#include <QString>

#include <string>
#include <vector>

namespace fa::ui {

struct ColorPreset {
    std::string id;     // "color.<n>"
    std::string name;
    std::vector<EffectRef> effects;   // constants, no masks
};

class ColorPresetLibrary : public QObject {
    Q_OBJECT
public:
    explicit ColorPresetLibrary(QString file = {}, QObject* parent = nullptr);   // empty: default location

    const std::vector<ColorPreset>& presets() const { return presets_; }
    const ColorPreset* find(const std::string& id) const;
    // The clip's Color-tab effects baked at source time `t` (keyframes -> values, masks dropped).
    static std::vector<EffectRef> captureColor(const Clip& clip, const Time& t);
    // Saves the clip's Color-tab settings; returns the preset id, empty on failure (lastError()).
    std::string add(const QString& name, const Clip& clip, const Time& t);
    bool remove(const std::string& id);
    QString file() const { return file_; }
    QString lastError() const { return error_; }

signals:
    void changed();

private:
    void load();
    bool save();
    QString file_;
    QString error_;
    std::vector<ColorPreset> presets_;
    int next_ = 1;
};

}  // namespace fa::ui
