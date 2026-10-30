// The open project: model + undo stack + file state + selection + playhead (GUI thread only).
// Every change of the model goes through execute()/undo()/redo(), which publish a new immutable
// ProjectSnapshot for the background preview, audio and export workers.
#pragma once

#include "fa/model/commands.h"
#include "fa/playback/preview_renderer.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fa::ui {

struct SequenceSettings {
    int width = 1920;
    int height = 1080;
    FrameRate frameRate = FrameRate::fps30();
    int sampleRate = 48000;
};

// The active mask: "<maskId>" is one of the clip's own masks, "<effectId>/<maskId>" a mask of one of its effects
// (a selective effect). Keyframe properties: "mask.<id>.<prop>" and "fx.<effectId>.mask.<id>.<prop>".
const Mask* findActiveMask(const Clip& c, const std::string& ref);
Mask* findActiveMask(Clip& c, const std::string& ref);
inline bool isEffectMaskRef(const std::string& ref) { return ref.find('/') != std::string::npos; }
std::string maskPropertyPrefix(const std::string& ref);

class Document : public QObject {
    Q_OBJECT
public:
    explicit Document(QObject* parent = nullptr);

    // ---- lifecycle ----
    void newProject(const SequenceSettings& settings);
    bool open(const QString& path, QString* error);
    bool save(const QString& path, QString* error);
    // M8 crash recovery: opens an autosaved copy as unsaved changes of `originalPath` (empty: a project never saved).
    bool openRecovered(const QString& copy, const QString& originalPath, QString* error);
    QString filePath() const { return path_; }
    bool isModified() const { return recovered_ || !stack_->isClean(); }
    QString displayName() const;

    // ---- model ----
    const Project& project() const { return project_; }
    const std::string& sequenceId() const { return sequenceId_; }
    const Sequence& sequence() const;
    UndoStack& undoStack() { return *stack_; }
    // For BUILDING edit commands only (they allocate ids from the project's counter). Content changes
    // must go through execute(), never through this reference.
    Project& commandTarget() { return project_; }
    uint64_t version() const { return version_; }
    ProjectSnapshot snapshot() const;

    // Run an edit. On failure nothing changes, errorOccurred is emitted and false returned.
    bool execute(std::unique_ptr<Command> cmd, QString* error = nullptr);
    // M7: prepared AI pictures arrived (no edit, no undo step): the preview draws the frame again
    void refreshPictures();
    // Same, but consecutive edits with the same mergeKey form one undo step (sliders, spin boxes).
    bool executeMerged(std::unique_ptr<Command> cmd, const std::string& mergeKey, QString* error = nullptr);

    // Live preview of an edit in progress (dragging handles in the monitor): the change is applied to
    // a copy that only the renderers see; nothing enters the undo history. Commit with execute(),
    // or call cancelPreview().
    void previewEdit(const std::function<void(Project&)>& change);
    void cancelPreview();
    bool hasPreviewEdit() const { return preview_ != nullptr; }
    bool undo();
    bool redo();

    // Probe files and add them as media sources (one undo step). Returns the new/existing source ids.
    // `notices`: imported, but worth knowing (e.g. HDR video, which is converted to SDR).
    std::vector<std::string> importMedia(const QStringList& files, QStringList* errors, QStringList* notices = nullptr);
    // Sources whose files do not exist on disk.
    std::vector<std::string> missingSources() const;
    bool isSourceUsed(const std::string& sourceId) const;

    // ---- selection / playhead ----
    const std::vector<std::string>& selection() const { return selection_; }
    // A selected transition is identified by its outgoing clip (the clip that owns outTransition).
    const std::string& selectedTransition() const { return selectedTransition_; }
    void setSelectedTransition(const std::string& clipId);
    // Mask being edited in the inspector / monitor (empty = transform editing).
    const std::string& activeMask() const { return activeMask_; }
    void setActiveMask(const std::string& maskId);
    // Source time of the selected clip at the playhead, clamped into the clip (for keyframes).
    std::optional<Time> clipTimeAtPlayhead(const std::string& clipId) const;
    bool playheadInside(const std::string& clipId) const;
    void setSelection(std::vector<std::string> clipIds);
    std::string primarySelection() const { return selection_.empty() ? std::string() : selection_.front(); }
    int64_t playhead() const { return playhead_; }
    void setPlayhead(int64_t frame);
    Time playheadTime() const { return frameToTime(playhead_, sequence().frameRate); }

    // M5: the motion-tracking region being placed in the player (UI state, not part of the project)
    struct TrackingBox {
        std::string clipId;
        double x = 0.5, y = 0.5, w = 0.25, h = 0.25;   // picture fractions
        friend bool operator==(const TrackingBox&, const TrackingBox&) = default;
    };
    const std::optional<TrackingBox>& trackingBox() const { return trackingBox_; }
    void setTrackingBox(std::optional<TrackingBox> box);
    // The file of a media source (relative paths resolved against the project file).
    std::filesystem::path mediaFile(const MediaSource& src) const;
    // The same as a clean Qt path ("/" separators), for thumbnails, waveforms and comparisons in the interface
    QString mediaFilePath(const MediaSource& src) const;

signals:
    void trackingBoxChanged();
    void changed();              // model content changed (edit, undo, redo, new, open)
    void selectionChanged();
    void playheadChanged(qint64 frame);
    void previewChanged();       // transient preview edit started/updated/cancelled (renderers only)
    void maskSelectionChanged();
    void fileStateChanged();     // path or modified flag
    void errorOccurred(const QString& message);

private:
    std::optional<TrackingBox> trackingBox_;
    void reset(Project p, QString path);
    void afterChange();
    void bumpVersionKeepingSnapshot();

    Project project_;
    std::unique_ptr<UndoStack> stack_;
    std::string sequenceId_;
    QString path_;
    bool recovered_ = false;   // opened from an autosaved copy: unsaved until saved
    uint64_t version_ = 1;
    mutable std::shared_ptr<const Project> snapshot_;
    mutable uint64_t snapshotVersion_ = 0;
    std::vector<std::string> selection_;
    std::string selectedTransition_;
    std::string activeMask_;
    std::shared_ptr<const Project> preview_;
    int64_t playhead_ = 0;
};

}  // namespace fa::ui
