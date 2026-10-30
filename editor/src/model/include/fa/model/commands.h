// Undoable edit commands.
//
// Every edit goes through a Command executed by an UndoStack. A command validates its
// preconditions BEFORE touching the project (throwing EditError leaves the project unchanged),
// then records a memento of exactly the state it modifies (one or two tracks, the track list, or
// the source list), so revert() restores it bit-for-bit. New ids are allocated when the command
// is constructed, so redo reproduces identical ids.
#pragma once

#include "fa/model/project.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fa {

class EditError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Command {
public:
    virtual ~Command() = default;
    virtual void apply(Project& p) = 0;    // throws EditError when the edit is not allowed
    virtual void revert(Project& p) = 0;
    virtual std::string label() const = 0;
};

class UndoStack {
public:
    explicit UndoStack(Project& p, size_t limit = 500) : project_(p), limit_(limit) {}

    // Applies the command; on success it becomes the newest undo step and clears redo history.
    void execute(std::unique_ptr<Command> cmd);
    // Like execute(), but if the newest undo step was executed with the same non-empty mergeKey and
    // nothing else happened since, the two become ONE undo step (e.g. dragging a slider): the previous
    // command is reverted and the new one applied on the state before it. Commands used this way must
    // set absolute values (not deltas).
    void executeMerged(std::unique_ptr<Command> cmd, const std::string& mergeKey);
    bool canUndo() const { return !done_.empty(); }
    bool canRedo() const { return !undone_.empty(); }
    void undo();
    void redo();
    std::string undoLabel() const { return canUndo() ? done_.back()->label() : std::string(); }
    std::string redoLabel() const { return canRedo() ? undone_.back()->label() : std::string(); }
    size_t undoCount() const { return done_.size(); }
    size_t redoCount() const { return undone_.size(); }
    bool isClean() const { return cleanIndex_ && *cleanIndex_ == done_.size(); }
    void setClean() { cleanIndex_ = done_.size(); }
    void clear();

private:
    Project& project_;
    size_t limit_;
    std::vector<std::unique_ptr<Command>> done_;
    std::vector<std::unique_ptr<Command>> undone_;
    std::optional<size_t> cleanIndex_ = 0;
    std::string lastMergeKey_;
};

// ---- concrete commands -----------------------------------------------------------------------

std::unique_ptr<Command> makeAddSource(Project& p, MediaSource src);   // src.id empty -> allocated
std::unique_ptr<Command> makeAddTrack(Project& p, const std::string& sequenceId, TrackKind kind,
                                      std::optional<size_t> index = std::nullopt, std::string name = {},
                                      std::string* trackIdOut = nullptr);   // the new track's id (for composites)
std::unique_ptr<Command> makeRemoveTrack(const std::string& sequenceId, const std::string& trackId);

// Insert a clip (clip.id empty -> allocated). Fails if it overlaps another clip on the track.
std::unique_ptr<Command> makeInsertClip(Project& p, const std::string& sequenceId, const std::string& trackId,
                                        Clip clip);
// Insert many clips on one track in one step (subtitle import): one track snapshot for undo instead of one
// per clip, overlaps checked in one sorted pass. Empty ids are allocated.
std::unique_ptr<Command> makeInsertClips(Project& p, const std::string& sequenceId, const std::string& trackId,
                                         std::vector<Clip> clips);
// Remove a clip; with ripple, later clips on the same track move left by its duration.
std::unique_ptr<Command> makeRemoveClip(const std::string& sequenceId, const std::string& clipId, bool ripple);
// Move a clip to a new start (and optionally another track of the same kind).
std::unique_ptr<Command> makeMoveClip(const std::string& sequenceId, const std::string& clipId, Time newStart,
                                      std::optional<std::string> newTrackId = std::nullopt);
// Move the head (start) or tail (end) edit point of a clip to a timeline position. Head trims
// shift sourceIn by the same amount, so the picture under each remaining frame is unchanged.
enum class ClipEdge { Head, Tail };
std::unique_ptr<Command> makeTrimClip(const std::string& sequenceId, const std::string& clipId, ClipEdge edge,
                                      Time newEdgeTime);
// Split at timeline time t (strictly inside the clip). The right part gets a new id and the
// outgoing transition; keyframes (source time) stay valid in both parts. The right part's linkId is
// `rightLinkId` (so the two halves of a linked A/V pair form two new pairs) or the original one.
std::unique_ptr<Command> makeSplitClip(Project& p, const std::string& sequenceId, const std::string& clipId,
                                       Time t, std::optional<std::string> rightLinkId = std::nullopt);
// Arbitrary property change on one clip (opacity keys, effects, name, ...); timing must not change.
std::unique_ptr<Command> makeModifyClip(const std::string& sequenceId, const std::string& clipId,
                                        std::string label, std::function<void(Clip&)> change);
// M5: new speed settings and the timeline duration that goes with them (whole frames). Refused if the clip would
// overlap the next clip, run past its media or the settings are invalid (speedProblem).
std::unique_ptr<Command> makeRetimeClip(const std::string& sequenceId, const std::string& clipId, ClipSpeed speed,
                                        Time duration);
// Track property change (name, hidden, muted, locked, volume); the clip list must not change.
std::unique_ptr<Command> makeModifyTrack(const std::string& sequenceId, const std::string& trackId,
                                         std::string label, std::function<void(Track&)> change);
// Ripple shift: every clip on the track that starts at or after `from` moves by `delta`
// (negative = left). Fails if a clip would start before 0 or overlap an unmoved clip.
std::unique_ptr<Command> makeShiftClips(const std::string& sequenceId, const std::string& trackId, Time from,
                                        Time delta);
// Move the listed clips of one track by `delta`. Fails on overlap with unmoved clips or start < 0.
std::unique_ptr<Command> makeMoveClipsBy(const std::string& sequenceId, const std::string& trackId,
                                         std::vector<std::string> clipIds, Time delta);
// Sequence settings (name, resolution, frame rate, sample rate). Changing the frame rate is only
// allowed while every clip edit point stays frame-aligned.
std::unique_ptr<Command> makeModifySequence(const std::string& sequenceId, std::string label,
                                            std::function<void(Sequence&)> change);
// Re-point a media source to another file (relink missing media); probe fields may be updated too.
std::unique_ptr<Command> makeModifySource(const std::string& sourceId, std::string label,
                                          std::function<void(MediaSource&)> change);

// Several commands as one undo step. Applies them in order; if one fails, the already applied ones
// are reverted (the project is left unchanged) and the error is rethrown.
std::unique_ptr<Command> makeComposite(std::string label, std::vector<std::unique_ptr<Command>> commands);

}  // namespace fa
