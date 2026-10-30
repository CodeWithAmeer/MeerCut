// Effect presets (M4): an effect with its settings (values at the playhead, no keyframes or masks), saved by name
// from the Effects tab and offered on the Effects / Filters pages under "My Presets". Stored in
// effect_presets.json in the app data folder (MEERCUT_EFFECT_PRESETS overrides the file).
#pragma once

#include "fa/model/project.h"

#include <QObject>
#include <QString>

#include <string>
#include <vector>

namespace fa::ui {

struct EffectPreset {
    std::string id;     // "preset.<n>"
    std::string name;
    EffectRef effect;   // constants, no masks
};

class EffectPresetLibrary : public QObject {
    Q_OBJECT
public:
    explicit EffectPresetLibrary(QString file = {}, QObject* parent = nullptr);   // empty: default location
    const std::vector<EffectPreset>& presets() const { return presets_; }
    const EffectPreset* find(const std::string& id) const;
    // The effect baked at source time `t` (keyframes -> values; masks dropped).
    static EffectRef bake(const EffectRef& e, const Time& t);
    std::string add(const QString& name, const EffectRef& e, const Time& t);   // id, empty on failure (lastError)
    bool remove(const std::string& id);
    bool rename(const std::string& id, const QString& name);
    QString lastError() const { return error_; }

signals:
    void changed();

private:
    void load();
    bool save();
    QString file_;
    QString error_;
    std::vector<EffectPreset> presets_;
    int next_ = 1;
};

}  // namespace fa::ui
