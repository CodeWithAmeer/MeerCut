#include "color_presets.h"

#include "edit_controller.h"

#include "fa/model/serialize.h"

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
constexpr const char* kFormat = "meercut-color-presets";
constexpr const char* kLegacyFormat = "lumacut-color-presets";   // saved before the rename to MeerCut
}

ColorPresetLibrary::ColorPresetLibrary(QString file, QObject* parent) : QObject(parent), file_(std::move(file)) {
    if (file_.isEmpty()) file_ = qEnvironmentVariable("MEERCUT_COLOR_PRESETS");
    if (file_.isEmpty())
        file_ = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("color_presets.json");
    load();
}

const ColorPreset* ColorPresetLibrary::find(const std::string& id) const {
    for (const auto& p : presets_)
        if (p.id == id) return &p;
    return nullptr;
}

std::vector<EffectRef> ColorPresetLibrary::captureColor(const Clip& clip, const Time& t) {
    std::vector<EffectRef> out;
    for (const std::string& type : EditController::colorTabTypes()) {
        const EffectRef* e = EditController::colorEffect(clip, type);
        if (!e) continue;
        EffectRef c = *e;
        for (auto& [id, a] : c.params) a.setConstant(a.value(t));
        c.mix.setConstant(c.mix.value(t));
        c.masks.clear();
        out.push_back(std::move(c));
    }
    return out;
}

void ColorPresetLibrary::load() {
    presets_.clear();
    QFile f(file_);
    if (!f.exists()) return;
    if (!f.open(QIODevice::ReadOnly)) {
        error_ = "cannot read " + file_;
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (const QString format = root.value("format").toString(); format != kFormat && format != kLegacyFormat) {
        error_ = file_ + " is not a colour preset file";
        return;
    }
    for (const QJsonValue& v : root.value("presets").toArray()) {
        const QJsonObject o = v.toObject();
        try {
            ColorPreset p;
            p.id = o.value("id").toString().toStdString();
            p.name = o.value("name").toString().toStdString();
            p.effects = clipFromJsonString(QJsonDocument(o.value("clip").toObject()).toJson().toStdString()).effects;
            if (p.id.rfind("color.", 0) != 0 || p.effects.empty()) continue;
            next_ = std::max(next_, std::atoi(p.id.c_str() + 6) + 1);
            presets_.push_back(std::move(p));
        } catch (const std::exception&) {
            error_ = "a preset in " + file_ + " could not be read";
        }
    }
}

bool ColorPresetLibrary::save() {
    QJsonArray arr;
    for (const auto& p : presets_) {
        Clip holder;   // the clip JSON format carries the effects
        holder.adjustment = true;
        holder.effects = p.effects;
        QJsonObject o;
        o["id"] = QString::fromStdString(p.id);
        o["name"] = QString::fromStdString(p.name);
        o["clip"] = QJsonDocument::fromJson(QByteArray::fromStdString(clipToJsonString(holder, -1))).object();
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

std::string ColorPresetLibrary::add(const QString& name, const Clip& clip, const Time& t) {
    ColorPreset p;
    p.effects = captureColor(clip, t);
    if (p.effects.empty()) {
        error_ = "the clip has no Color tab settings";
        return {};
    }
    p.id = "color." + std::to_string(next_++);
    p.name = name.trimmed().isEmpty() ? std::string("My Color") : name.trimmed().toStdString();
    presets_.push_back(p);
    if (!save()) {
        presets_.pop_back();
        return {};
    }
    emit changed();
    return p.id;
}

bool ColorPresetLibrary::remove(const std::string& id) {
    for (auto it = presets_.begin(); it != presets_.end(); ++it)
        if (it->id == id) {
            const ColorPreset keep = *it;
            const auto pos = presets_.erase(it);
            if (!save()) {
                presets_.insert(pos, keep);
                return false;
            }
            emit changed();
            return true;
        }
    return false;
}

}  // namespace fa::ui
