#include "title_library.h"

#include "fa/model/serialize.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace fa::ui {

namespace {
constexpr const char* kFormat = "meercut-title-presets";
constexpr const char* kLegacyFormat = "lumacut-title-presets";   // saved before the rename to MeerCut
}

TitleLibrary::TitleLibrary(QString file, QObject* parent) : QObject(parent), file_(std::move(file)) {
    if (file_.isEmpty()) file_ = qEnvironmentVariable("MEERCUT_TITLE_PRESETS");
    if (file_.isEmpty())
        file_ = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("title_presets.json");
    load();
}

std::vector<const text::TitleTemplate*> TitleLibrary::all() const {
    std::vector<const text::TitleTemplate*> v;
    for (const auto& t : text::builtinTitleTemplates()) v.push_back(&t);
    for (const auto& t : user_) v.push_back(&t);
    return v;
}

const text::TitleTemplate* TitleLibrary::find(const std::string& id) const {
    if (const auto* t = text::findTitleTemplate(id)) return t;
    for (const auto& t : user_)
        if (t.id == id) return &t;
    return nullptr;
}

void TitleLibrary::load() {
    user_.clear();
    QFile f(file_);
    if (!f.exists()) return;
    if (!f.open(QIODevice::ReadOnly)) {
        error_ = "cannot read " + file_;
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (const QString format = root.value("format").toString(); format != kFormat && format != kLegacyFormat) {
        error_ = file_ + " is not a title preset file";
        return;
    }
    for (const QJsonValue& v : root.value("presets").toArray()) {
        const QJsonObject o = v.toObject();
        try {
            text::TitleTemplate t;
            t.id = o.value("id").toString().toStdString();
            t.name = o.value("name").toString().toStdString();
            t.category = "My Titles";
            t.description = "Your preset";
            t.caption = o.value("caption").toBool();
            t.duration = Rational::parse(o.value("duration").toString("5").toStdString());
            t.prototype = clipFromJsonString(QJsonDocument(o.value("clip").toObject()).toJson().toStdString());
            if (!t.prototype.title || t.id.rfind("user.", 0) != 0 || t.duration <= Time(0)) continue;
            next_ = std::max(next_, std::atoi(t.id.c_str() + 5) + 1);
            user_.push_back(std::move(t));
        } catch (const std::exception&) {
            error_ = "a preset in " + file_ + " could not be read";
        }
    }
}

bool TitleLibrary::save() {
    QJsonArray arr;
    for (const auto& t : user_) {
        QJsonObject o;
        o["id"] = QString::fromStdString(t.id);
        o["name"] = QString::fromStdString(t.name);
        o["caption"] = t.caption;
        o["duration"] = QString::fromStdString(t.duration.toString());
        o["clip"] = QJsonDocument::fromJson(QByteArray::fromStdString(clipToJsonString(t.prototype, -1))).object();
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

std::string TitleLibrary::addUserPreset(const QString& name, const Clip& clip, Time duration, bool caption) {
    if (!clip.title) {
        error_ = "not a title";
        return {};
    }
    text::TitleTemplate t;
    t.id = "user." + std::to_string(next_++);
    t.category = "My Titles";
    t.name = name.trimmed().isEmpty() ? std::string("My Title") : name.trimmed().toStdString();
    t.description = "Your preset";
    t.caption = caption;
    t.duration = duration > Time(0) ? duration : Time(5);
    // keep what defines the look; drop identity, timing and keyframes (presets are placed at the playhead)
    Clip p;
    p.title = clip.title;
    p.title->templateId = t.id;
    p.name = t.name;
    p.blend = clip.blend;
    p.opacity.setConstant(clip.opacity.value(clip.sourceIn));
    ClipTransform& tf = p.transform;
    const Time at = clip.sourceIn;
    tf.positionX.setConstant(clip.transform.positionX.value(at));
    tf.positionY.setConstant(clip.transform.positionY.value(at));
    tf.scaleX.setConstant(clip.transform.scaleX.value(at));
    tf.scaleY.setConstant(clip.transform.scaleY.value(at));
    tf.rotation.setConstant(clip.transform.rotation.value(at));
    tf.anchorX.setConstant(clip.transform.anchorX.value(at));
    tf.anchorY.setConstant(clip.transform.anchorY.value(at));
    tf.uniformScale = clip.transform.uniformScale;
    tf.flipH = clip.transform.flipH;
    tf.flipV = clip.transform.flipV;
    t.prototype = std::move(p);
    user_.push_back(t);
    if (!save()) {
        user_.pop_back();
        return {};
    }
    emit changed();
    return t.id;
}

bool TitleLibrary::removeUserPreset(const std::string& id) {
    for (auto it = user_.begin(); it != user_.end(); ++it)
        if (it->id == id) {
            const text::TitleTemplate keep = *it;
            const auto pos = user_.erase(it);
            if (!save()) {
                user_.insert(pos, keep);
                return false;
            }
            emit changed();
            return true;
        }
    return false;
}

bool TitleLibrary::renameUserPreset(const std::string& id, const QString& name) {
    for (auto& t : user_)
        if (t.id == id && !name.trimmed().isEmpty()) {
            const std::string old = t.name;
            t.name = name.trimmed().toStdString();
            t.prototype.name = t.name;
            if (!save()) {
                t.name = t.prototype.name = old;
                return false;
            }
            emit changed();
            return true;
        }
    return false;
}

}  // namespace fa::ui
