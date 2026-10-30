// User-level edit operations (what a menu item, shortcut or mouse gesture does), built from the
// primitive commands as ONE undoable step each.
//
// Rules shared by all operations
//   * Linked clips (same non-empty Clip::linkId, e.g. the video and audio of one imported file)
//     follow the operated clip: moves and trims apply the same time delta, splits split both
//     (each half-pair gets its own link), deletes remove both.
//   * Ripple edits keep linked pairs in sync: when clips are shifted, their linked partners on other
//     tracks are shifted by the same amount; if that is impossible the whole edit is refused.
//   * Clips on locked tracks are never modified; operations that would need to fail with EditError.
//   * Nothing is changed when an EditError is thrown.
#pragma once

#include "fa/model/commands.h"
#include "fa/model/subtitles.h"

#include <optional>
#include <string>
#include <vector>

namespace fa::edit {

struct ClipRef {
    std::string trackId;
    std::string clipId;
    friend bool operator==(const ClipRef&, const ClipRef&) = default;
};

// Applies commands to a scratch copy as they are added (so each step sees the previous ones), then
// yields one composite command for the real project and advances the real id counter.
class Builder {
public:
    explicit Builder(Project& real) : real_(real), scratch_(real) {}
    Project& scratch() { return scratch_; }
    Sequence& sequence(const std::string& id);
    void add(std::unique_ptr<Command> cmd);
    bool empty() const { return cmds_.empty(); }
    std::unique_ptr<Command> finish(std::string label);

private:
    Project& real_;
    Project scratch_;
    std::vector<std::unique_ptr<Command>> cmds_;
};

// Clips linked with `clipId` (including itself), with their tracks.
std::vector<ClipRef> linkGroup(const Sequence& seq, const std::string& clipId);
const Clip* findClip(const Sequence& seq, const std::string& clipId, const Track** trackOut = nullptr);

// Length of a source's media, and the longest frame-aligned clip that fits in it at `rate`.
Time mediaDuration(const MediaSource& src);
Time fullClipDuration(const MediaSource& src, const FrameRate& rate);

struct AddedClips {
    std::string videoClipId;   // empty if the source has no video
    std::string audioClipId;   // empty if the source has no audio
};

// Put a whole media file on the timeline at `at` (snapped to a frame): video on `videoTrackId`
// (default: first video track), audio on `audioTrackId` (M10: the audio track it was dropped on), by default on the
// audio track with the same index among audio tracks (default/missing: created). Video and audio are linked. If the
// space is occupied, the insertion point moves to the end of the clip under `at` and all unlocked tracks ripple right.
std::unique_ptr<Command> addMedia(Project& p, const std::string& seqId, const std::string& sourceId, Time at,
                                  std::string videoTrackId = {}, AddedClips* added = nullptr,
                                  std::string audioTrackId = {});

std::unique_ptr<Command> moveClip(Project& p, const std::string& seqId, const std::string& clipId, Time newStart,
                                  std::optional<std::string> newTrackId = std::nullopt);
std::unique_ptr<Command> trimClip(Project& p, const std::string& seqId, const std::string& clipId, ClipEdge edge,
                                  Time newEdgeTime);
// Split the given clips (and their partners) at t; clips not containing t are ignored.
// With an empty list, every clip under t on unlocked tracks is split (Filmora: split at playhead).
std::unique_ptr<Command> splitAt(Project& p, const std::string& seqId, Time t, std::vector<std::string> clipIds = {});
std::unique_ptr<Command> deleteClips(Project& p, const std::string& seqId, std::vector<std::string> clipIds,
                                     bool ripple);

// ---- gaps (M10, Clipchamp's "Delete this gap") ----------------------------------------------------

// The empty stretch of `track` that contains t: from the end of the clip before it (or 0) to the start of the next
// clip. Nothing when t is on a clip or after the last clip (the space after the last clip is not a gap).
std::optional<TimeRange> gapAt(const Track& track, Time t);
// Why the gap of track `trackId` at t cannot be deleted, as a sentence for the user; empty when it can.
std::string gapProblem(const Sequence& seq, const std::string& trackId, Time t);
// Delete the gap at t: the clips after it on its track, and their linked partners on other tracks, move left by the
// gap's length (one undo step); clips on other tracks that are not linked stay where they are, as with Ripple
// Delete. Refused (EditError with gapProblem's reason) when a locked track would change or a linked partner would
// run into a clip that stays.
std::unique_ptr<Command> deleteGap(Project& p, const std::string& seqId, const std::string& trackId, Time t);
// Set or remove (nullopt) the transition from clip into the adjacent following clip.
std::unique_ptr<Command> setTransition(Project& p, const std::string& seqId, const std::string& clipId,
                                       std::optional<TransitionRef> transition);
// Longest transition of the given alignment that fits between clip and its following neighbour.
std::optional<Time> maxTransitionDuration(const Sequence& seq, const std::string& clipId, TransitionAlignment a);
// Remove the A/V link of the clip's group (Filmora "Detach Audio").
std::unique_ptr<Command> unlink(Project& p, const std::string& seqId, const std::string& clipId);

// ---- speed (M5) ------------------------------------------------------------------------------

// Set the speed settings of a media clip and its linked partners (one undo step). The source range the clip plays
// is kept; the timeline duration follows the speed (whole frames: enough to show the range, never past the media)
// and the clips after it on the same tracks ripple by the difference (linked partners elsewhere follow).
// Freeze frames are made with freezeFrame. Throws EditError for invalid settings (speedProblem) or locked tracks.
std::unique_ptr<Command> setSpeed(Project& p, const std::string& seqId, const std::string& clipId, ClipSpeed speed,
                                  std::string label = "Speed");

inline const Time kDefaultFreezeDuration{5};   // MeerCut's default hold (Stage 1 did not measure Filmora's)

// Freeze Frame: hold the frame the clip shows at `at` for `hold` (whole frames) - the clip is split at `at`, a
// silent hold clip with the clip's effects, transform and masks is inserted, and everything after it on the clip's
// tracks (and linked partners) moves right by `hold`. Returns the hold clip's id via holdIdOut.
std::unique_ptr<Command> freezeFrame(Project& p, const std::string& seqId, const std::string& clipId, Time at,
                                     Time hold = kDefaultFreezeDuration, std::string* holdIdOut = nullptr);

// Change the length of a freeze frame (whole frames); the clips after it on its track ripple.
std::unique_ptr<Command> setHoldDuration(Project& p, const std::string& seqId, const std::string& clipId, Time duration);

// ---- titles and captions (M3) ----------------------------------------------------------------

inline const Time kDefaultTitleDuration{5};   // Filmora "Add Text": 5.000 s (EXP-003)

// Put a title clip on the timeline at `at` (snapped to a frame) for `duration` (whole frames, >= 1).
// `prototype` supplies the title, transform, opacity...; its id and timing are replaced. Target: `trackId`
// if given and free, otherwise the lowest free video track above Video 1 (or above `trackId`),
// otherwise a new video track on top - titles go above the footage, as in Filmora.
std::unique_ptr<Command> addTitle(Project& p, const std::string& seqId, Clip prototype, Time at, Time duration,
                                  std::string trackId = {}, std::string* clipIdOut = nullptr);
// The same placement for any generated clip (title or adjustment layer, M4).
std::unique_ptr<Command> addGenerated(Project& p, const std::string& seqId, Clip prototype, Time at, Time duration,
                                      std::string trackId = {}, std::string* clipIdOut = nullptr);
// The track addGenerated puts a clip covering `range` on (`trackId` as given to it); empty when it needs a new video
// track on top. M10: the timeline shows a dragged title / sticker / effect where it will really land.
std::string generatedTarget(const Sequence& seq, const std::string& trackId, const TimeRange& range);

// Put captions on subtitle tracks: times are snapped to the nearest frame (at least one frame long);
// captions that overlap an earlier one go to an additional subtitle track. New tracks get `style`
// and `subtitleY`; captions with their own style or position keep them.
std::unique_ptr<Command> addCaptions(Project& p, const std::string& seqId, std::vector<Caption> captions,
                                     const TextStyle& style, double subtitleY,
                                     std::vector<std::string>* trackIdsOut = nullptr);

// One caption at `at` on subtitle track `trackId` (empty: the first subtitle track, created if needed).
// `style` (caption templates): a new track takes it as its style; on an existing track with a different
// style the caption keeps it as its own style. Without it the caption follows the track's style.
std::unique_ptr<Command> addCaption(Project& p, const std::string& seqId, const std::string& text, Time at,
                                    Time duration, std::string trackId = {}, std::string* clipIdOut = nullptr,
                                    const TextStyle* style = nullptr, const std::string& templateId = {});

// ---- clipboard (M9) --------------------------------------------------------------------------

// A copied clip with the kind and id of the track it was on.
struct CopiedClip {
    Clip clip;
    std::string trackId;
    TrackKind kind = TrackKind::Video;
};

// What Copy / Cut take: the clips and their link partners, ordered by start time.
std::vector<CopiedClip> copyClips(const Sequence& seq, const std::vector<std::string>& clipIds);
// The clipboard form: {"format": "meercut-clips", "version": n, "clips": [{"track", "kind", "clip"}]} with each clip
// in the project file's clip format. Reading throws ModelError on anything else.
std::string clipboardToJson(const std::vector<CopiedClip>& clips);
std::vector<CopiedClip> clipboardFromJson(const std::string& text);

// Paste copied clips as one step: the earliest starts at `at` (frame-snapped) and the others keep their offsets, A/V
// links (new link ids) and their tracks where possible - each original track's clips go to that track when it still
// exists, is unlocked and free there, otherwise to the next free track of the kind above it, otherwise to a new track.
// Transitions to clips that were not copied are dropped. The media must be in the project (EditError otherwise).
// With stackInOrder (templates), each original track's clips go above the previous one's (layers keep their order).
std::unique_ptr<Command> pasteClips(Project& p, const std::string& seqId, const std::vector<CopiedClip>& clips, Time at,
                                    std::vector<std::string>* newIds = nullptr, bool stackInOrder = false);

}  // namespace fa::edit
