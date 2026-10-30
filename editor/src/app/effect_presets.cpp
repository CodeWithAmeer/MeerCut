#include "effect_presets.h"

#include "fa/model/serialize.h"
#include "fa/render/effects.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cstdlib>

namespace fa::ui {

namespace {
constexpr const char* kFormat = "meercut-effect-presets";
constexpr const char* kLegacyFormat = "lumacut-effect-presets";   // saved before the rename to MeerCut

QJsonObject effectJson(const EffectRef& e) {
    Clip holder;   // the clip JSON format carries effects
    holder.adjustment = true;
    holder.effects = {e};
    return QJsonDocument::fromJson(QByteArray::fromStdString(clipToJsonString(holder, -1))).object();
}
}  // namespace

EffectPresetLibrary::EffectPresetLibrary(QString file, QObject* parent) : QObject(parent), file_(std::move(file)) {
    if (file_.isEmpty()) file_ = qEnvironmentVariable("MEERCUT_EFFECT_PRESETS");
    if (file_.isEmpty())
        file_ = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("effect_presets.json");
    load();
}

const EffectPreset* EffectPresetLibrary::find(const std::string& id) const {
    for (const auto& p : presets_)
        if (p.id == id) return &p;
    return nullptr;
}

EffectRef EffectPresetLibrary::bake(const EffectRef& e, const Time& t) {
    EffectRef c = e;
    for (auto& [id, a] : c.params) a.setConstant(a.value(t));
    c.mix.setConstant(c.mix.value(t));
    c.masks.clear();
    c.enabled = true;
    return c;
}

void EffectPresetLibrary::load() {
    presets_.clear();
    QFile f(file_);
    if (!f.exists()) return;
    if (!f.open(QIODevice::ReadOnly)) {
        error_ = "cannot read " + file_;
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (const QString format = root.value("format").toString(); format != kFormat && format != kLegacyFormat) {
        error_ = file_ + " is not an effect preset file";
        return;
    }
    for (const QJsonValue& v : root.value("presets").toArray()) {
        const QJsonObject o = v.toObject();
        try {
            EffectPreset p;
            p.id = o.value("id").toString().toStdString();
            p.name = o.value("name").toString().toStdString();
            const Clip holder = clipFromJsonString(QJsonDocument(o.value("clip").toObject()).toJson().toStdString());
            if (p.id.rfind("preset.", 0) != 0 || holder.effects.size() != 1) continue;
            p.effect = holder.effects.front();
            next_ = std::max(next_, std::atoi(p.id.c_str() + 7) + 1);
            presets_.push_back(std::move(p));
        } catch (const std::exception&) {
            error_ = "a preset in " + file_ + " could not be read";
        }
    }
}

bool EffectPresetLibrary::save() {
    QJsonArray arr;
    for (const auto& p : presets_) {
        QJsonObject o;
        o["id"] = QString::fromStdString(p.id);
        o["name"] = QString::fromStdString(p.name);
        o["clip"] = effectJson(p.effect);
        arr.append(o);
    }
    QJsonObject root;
    root["format"] = kFormat;
    root["version"] = 1;
    root["presets"] = arr;
    QDir().mkpath(QFileInfo(file_).absolutePath());
    QSaveFile f(file_);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(root).toJson()) < 0 || !f.commit()) {
        error_ = "cannot write " + file_;
        return false;
    }
    return true;
}

std::string EffectPresetLibrary::add(const QString& name, const EffectRef& e, const Time& t) {
    if (!findEffectDef(e.type)) {
        error_ = "unknown effect";
        return {};
    }
    EffectPreset p;
    p.id = "preset." + std::to_string(next_++);
    p.name = name.trimmed().isEmpty() ? findEffectDef(e.type)->label : name.trimmed().toStdString();
    p.effect = bake(e, t);
    presets_.push_back(p);
    if (!save()) {
        presets_.pop_back();
        return {};
    }
    emit changed();
    return p.id;
}

bool EffectPresetLibrary::remove(const std::string& id) {
    const auto it = std::find_if(presets_.begin(), presets_.end(), [&](const EffectPreset& p) { return p.id == id; });
    if (it == presets_.end()) return false;
    const EffectPreset keep = *it;
    const auto pos = presets_.erase(it);
    if (!save()) {
        presets_.insert(pos, keep);
        return false;
    }
    emit changed();
    return true;
}

bool EffectPresetLibrary::rename(const std::string& id, const QString& name) {
    for (auto& p : presets_)
        if (p.id == id && !name.trimmed().isEmpty()) {
            const std::string old = p.name;
            p.name = name.trimmed().toStdString();
            if (!save()) {
                p.name = old;
                return false;
            }
            emit changed();
            return true;
        }
    return false;
}

}  // namespace fa::ui
