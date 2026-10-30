#include "autosave.h"

#include "document.h"

#include "fa/model/serialize.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>

namespace fa::ui {

namespace {

constexpr int kKeepDiscarded = 10;

constexpr const char* kCopyExt = ".mcproj";
constexpr const char* kLegacyCopyExt = ".lcproj";   // copies Lumacut made before the rename

bool writeAtomically(const QString& path, const QByteArray& bytes) {
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit();
}

}  // namespace

QString AutosaveManager::folder() {
    const QString env = qEnvironmentVariable("MEERCUT_AUTOSAVE_DIR");
    if (!env.isEmpty()) return QDir::cleanPath(env);
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/MeerCut/autosave";
}

AutosaveManager::AutosaveManager(Document* doc, QObject* parent) : QObject(parent), doc_(doc) {
    QDir().mkpath(folder());
    // unique per window, also for two windows of one process opened within the same second
    static std::atomic<int> counter{0};
    id_ = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + "-" + QString::number(QCoreApplication::applicationPid()) +
          "-" + QString::number(++counter);
    lock_ = std::make_unique<QLockFile>(base(id_) + ".lock");
    lock_->setStaleLockTime(0);   // stale only when its process is gone, never by age
    lock_->tryLock(0);
    connect(&timer_, &QTimer::timeout, this, [this] { autosaveNow(); });
    // saved, or undone back to the saved state: the copy is not needed any more
    connect(doc_, &Document::fileStateChanged, this, [this] {
        if (!doc_->isModified()) clearOwn();
    });
    setIntervalSeconds(interval_);
}

AutosaveManager::~AutosaveManager() = default;   // a lock released here without endSession() = an abnormal end

void AutosaveManager::setIntervalSeconds(int seconds) {
    interval_ = std::max(0, seconds);
    if (interval_ > 0) timer_.start(interval_ * 1000);
    else timer_.stop();
}

bool AutosaveManager::autosaveNow() {
    if (!doc_->isModified() || doc_->version() == writtenVersion_) return false;
    const std::string json = projectToJsonString(*doc_->snapshot().project);
    QJsonObject meta;
    meta["originalPath"] = doc_->filePath();
    meta["displayName"] = doc_->displayName();
    meta["savedAt"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    meta["appVersion"] = QCoreApplication::applicationVersion();
    // the copy first, then its description: a description always points at a complete copy
    if (!writeAtomically(base(id_) + kCopyExt, QByteArray::fromStdString(json)) ||
        !writeAtomically(base(id_) + ".json", QJsonDocument(meta).toJson()))
        return false;
    writtenVersion_ = doc_->version();
    emit autosaved(base(id_) + kCopyExt);
    return true;
}

std::vector<AutosaveManager::Session> AutosaveManager::recoverableSessions() const {
    std::vector<Session> out;
    const QDir dir(folder());
    for (const QFileInfo& fi : dir.entryInfoList({"*.json"}, QDir::Files)) {
        const QString id = fi.completeBaseName();
        const QString copy = QFileInfo::exists(base(id) + kCopyExt) ? base(id) + kCopyExt : base(id) + kLegacyCopyExt;
        if (id == id_ || !QFileInfo::exists(copy)) continue;
        QLockFile probe(base(id) + ".lock");
        probe.setStaleLockTime(0);
        if (!probe.tryLock(0)) continue;   // that MeerCut is still running
        probe.unlock();
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonObject meta = QJsonDocument::fromJson(f.readAll()).object();
        Session s;
        s.id = id;
        s.projectFile = copy;
        s.originalPath = meta["originalPath"].toString();
        s.displayName = meta["displayName"].toString("Untitled");
        s.savedAt = QDateTime::fromString(meta["savedAt"].toString(), Qt::ISODate);
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const Session& a, const Session& b) { return a.savedAt > b.savedAt; });
    return out;
}

bool AutosaveManager::recover(const Session& s, QString* error) {
    if (!doc_->openRecovered(s.projectFile, s.originalPath, error)) return false;
    QFile::remove(s.projectFile);
    QFile::remove(base(s.id) + ".json");
    writtenVersion_ = 0;
    autosaveNow();   // the recovered changes are this session's unsaved changes now
    return true;
}

bool AutosaveManager::discard(const Session& s) {
    const QString to = folder() + "/discarded";
    QDir().mkpath(to);
    bool ok = true;
    for (const char* ext : {kCopyExt, kLegacyCopyExt, ".json"}) {
        if (!QFileInfo::exists(base(s.id) + ext)) continue;
        const QString target = QString("%1/%2%3").arg(to, s.id, ext);
        QFile::remove(target);
        ok = QFile::rename(base(s.id) + ext, target) && ok;
    }
    // keep the newest kKeepDiscarded copies
    QDir d(to);
    const QFileInfoList old = d.entryInfoList({QString("*") + kCopyExt, QString("*") + kLegacyCopyExt}, QDir::Files, QDir::Time);
    for (int i = kKeepDiscarded; i < old.size(); ++i) {
        QFile::remove(old[i].absoluteFilePath());
        QFile::remove(old[i].absolutePath() + "/" + old[i].completeBaseName() + ".json");
    }
    return ok;
}

void AutosaveManager::clearOwn() {
    QFile::remove(base(id_) + kCopyExt);
    QFile::remove(base(id_) + ".json");
    writtenVersion_ = 0;
}

void AutosaveManager::endSession() {
    timer_.stop();
    clearOwn();
    if (lock_) lock_->unlock();
}

}  // namespace fa::ui
