// M8 autosave and crash recovery. While the project has unsaved changes, a copy of it is written every
// intervalSeconds() to the autosave folder (QSaveFile: a copy is never half-written), with a small description beside
// it (the project's own file, its name, the time). A lock file marks the session as running: a session whose lock is
// free at the next start ended without closing normally (a crash, a power cut, the process killed) and its copy is
// offered for recovery. Saving the project, undoing back to the saved state or closing MeerCut normally removes the
// session's copy. Copies the user discards are moved to "discarded" (the newest 10 are kept), not deleted at once.
#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <vector>

class QLockFile;

namespace fa::ui {

class Document;

class AutosaveManager : public QObject {
    Q_OBJECT
public:
    struct Session {
        QString id;
        QString projectFile;    // the autosaved copy
        QString originalPath;   // the project's own file; empty for a project never saved
        QString displayName;
        QDateTime savedAt;
    };

    explicit AutosaveManager(Document* doc, QObject* parent = nullptr);
    ~AutosaveManager() override;

    // MEERCUT_AUTOSAVE_DIR, else %LOCALAPPDATA%/MeerCut/autosave
    static QString folder();
    int intervalSeconds() const { return interval_; }
    void setIntervalSeconds(int seconds);   // 0 = off (saved in the settings by the caller)

    // Writes the copy now if the project changed since the last copy and has unsaved changes; true when written.
    bool autosaveNow();
    // Other sessions that did not end normally, newest first (sessions still running are never listed).
    std::vector<Session> recoverableSessions() const;
    // Opens a session's copy into the document (as unsaved changes of its original file); its copy is then removed.
    bool recover(const Session& s, QString* error);
    // Moves a session's copy to discarded/.
    bool discard(const Session& s);
    // This session ends normally: its copy and its lock are removed.
    void endSession();
    QString sessionId() const { return id_; }

signals:
    void autosaved(const QString& file);

private:
    void clearOwn();
    QString base(const QString& id) const { return QString("%1/%2").arg(folder(), id); }

    Document* doc_;
    QString id_;
    std::unique_ptr<QLockFile> lock_;
    QTimer timer_;
    int interval_ = 60;
    uint64_t writtenVersion_ = 0;
};

}  // namespace fa::ui
