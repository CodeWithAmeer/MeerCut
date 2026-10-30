#include "lut_library.h"

#include "fa/render/lut.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLinearGradient>
#include <QPainter>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace fa::ui {

namespace {
constexpr const char* kFormat = "meercut-luts";
constexpr const char* kLegacyFormat = "lumacut-luts";   // saved before the rename to MeerCut

std::filesystem::path toPath(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }
}  // namespace

LutLibrary::LutLibrary(QString file, QObject* parent) : QObject(parent), file_(std::move(file)) {
    if (file_.isEmpty()) file_ = qEnvironmentVariable("MEERCUT_LUTS");
    if (file_.isEmpty()) file_ = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("luts.json");
    load();
}

std::vector<LutLibrary::Entry> LutLibrary::entries() const {
    std::vector<Entry> v;
    for (const auto& look : builtinLooks())
        v.push_back({std::string(kBuiltinLutPrefix) + look.id, QString::fromStdString(look.name), QString::fromStdString(look.description), true});
    for (const auto& [name, path] : user_) v.push_back({path, name, QString::fromStdString(path), false});
    return v;
}

void LutLibrary::load() {
    user_.clear();
    QFile f(file_);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (const QString format = root.value("format").toString(); format != kFormat && format != kLegacyFormat) return;
    for (const QJsonValue& v : root.value("luts").toArray()) {
        const QJsonObject o = v.toObject();
        const std::string path = o.value("path").toString().toStdString();
        if (!path.empty()) user_.push_back({o.value("name").toString(), path});
    }
}

bool LutLibrary::save() {
    QJsonArray arr;
    for (const auto& [name, path] : user_) {
        QJsonObject o;
        o["name"] = name;
        o["path"] = QString::fromStdString(path);
        arr.append(o);
    }
    QJsonObject root;
    root["format"] = kFormat;
    root["version"] = 1;
    root["luts"] = arr;
    QDir().mkpath(QFileInfo(file_).absolutePath());
    QSaveFile f(file_);
    return f.open(QIODevice::WriteOnly) && f.write(QJsonDocument(root).toJson()) >= 0 && f.commit();
}

std::string LutLibrary::addFile(const QString& path, QString* error) {
    const QString abs = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const std::string ref = abs.toStdString();
    std::string err;
    if (!loadCube(toPath(ref), &err)) {
        if (error) *error = QFileInfo(path).fileName() + ": " + QString::fromStdString(err);
        return {};
    }
    for (const auto& [name, p] : user_)
        if (QString::fromStdString(p).compare(abs, Qt::CaseInsensitive) == 0) return p;   // already there
    user_.push_back({QFileInfo(path).completeBaseName(), ref});
    thumbs_.erase(ref);
    save();
    emit changed();
    return ref;
}

bool LutLibrary::remove(const std::string& reference) {
    const auto it = std::find_if(user_.begin(), user_.end(), [&](const auto& e) { return e.second == reference; });
    if (it == user_.end()) return false;
    user_.erase(it);
    save();
    emit changed();
    return true;
}

QString LutLibrary::nameOf(const std::string& reference) const {
    if (reference.empty()) return "None";
    for (const auto& e : entries())
        if (e.reference == reference) return e.name;
    return QFileInfo(QString::fromStdString(reference)).completeBaseName();
}

QImage LutLibrary::samplePicture(int w, int h) {
    // a small scene with sky, foliage, skin and neutral tones (drawn here, no third-party picture)
    QImage img(w, h, QImage::Format_RGBA8888);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient sky(0, 0, 0, h * 0.62);
    sky.setColorAt(0, QColor(0x3A, 0x6E, 0xA5));
    sky.setColorAt(1, QColor(0xCF, 0xE3, 0xF2));
    p.fillRect(QRectF(0, 0, w, h * 0.62), sky);
    QLinearGradient ground(0, h * 0.62, 0, h * 0.86);
    ground.setColorAt(0, QColor(0x5E, 0x8C, 0x31));
    ground.setColorAt(1, QColor(0x2D, 0x4A, 0x1A));
    p.fillRect(QRectF(0, h * 0.62, w, h * 0.24), ground);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xFF, 0xD2, 0x7A));
    p.drawEllipse(QPointF(w * 0.8, h * 0.22), h * 0.12, h * 0.12);
    p.setBrush(QColor(0xD9, 0xA0, 0x7A));
    p.drawEllipse(QPointF(w * 0.25, h * 0.55), h * 0.16, h * 0.2);
    p.setBrush(QColor(0xC0, 0x30, 0x30));
    p.drawRect(QRectF(w * 0.45, h * 0.5, w * 0.12, h * 0.22));
    QLinearGradient ramp(0, 0, w, 0);
    ramp.setColorAt(0, Qt::black);
    ramp.setColorAt(1, Qt::white);
    p.fillRect(QRectF(0, h * 0.86, w, h * 0.14), ramp);
    return img;
}

QImage LutLibrary::thumbnail(const std::string& reference, const std::filesystem::path& baseDir) {
    if (const auto it = thumbs_.find(reference); it != thumbs_.end()) return it->second;
    QImage img = samplePicture(96, 54);
    if (!reference.empty()) {
        std::string err;
        const auto lut = resolveLut(reference, baseDir, &err);
        if (!lut) return {};
        for (int y = 0; y < img.height(); ++y) {
            uchar* q = img.scanLine(y);
            for (int x = 0; x < img.width(); ++x, q += 4) {
                float r = q[0] / 255.0f, g = q[1] / 255.0f, b = q[2] / 255.0f;
                applyLut(*lut, LutDomainMode::Standard, r, g, b);
                q[0] = static_cast<uchar>(std::lround(r * 255));
                q[1] = static_cast<uchar>(std::lround(g * 255));
                q[2] = static_cast<uchar>(std::lround(b * 255));
            }
        }
    }
    thumbs_[reference] = img;
    return img;
}

}  // namespace fa::ui
