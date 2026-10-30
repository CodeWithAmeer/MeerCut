// Editing actions shared by menus, toolbar buttons, shortcuts, the timeline context menu and the
// inspector. Each action builds one undoable command (fa::edit) and runs it through the Document.
#pragma once

#include "document.h"

#include "fa/model/edit_ops.h"
#include "fa/model/keyframes.h"

#include <QObject>
#include <QPointF>
#include <QSizeF>
#include <QPointer>
#include <QWidget>

#include <filesystem>
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fa::ui {

class ColorPresetLibrary;
class EffectPresetLibrary;
class LutLibrary;
class TitleLibrary;

class EditController : public QObject {
    Q_OBJECT
public:
    enum class MatchPolicy { Ask, Always, Never };   // first clip with different fps/size than the project

    EditController(Document* doc, QWidget* dialogParent, QObject* parent = nullptr);
    void setMatchPolicy(MatchPolicy p) { match_ = p; }

    // Media -> timeline. `at` defaults to the playhead; videoTrackId defaults to the first video track, audioTrackId
    // (M10: the audio track the media was dropped on) to the audio track paired with the video track.
    bool addToTimeline(const std::string& sourceId, std::optional<Time> at = std::nullopt, std::string videoTrackId = {},
                       std::string audioTrackId = {});

    bool splitAtPlayhead();
    bool deleteSelection(bool ripple);
    bool trimToPlayhead(ClipEdge edge);
    bool detachAudio();
    bool toggleClipEnabled();
    bool moveClip(const std::string& clipId, Time newStart, std::optional<std::string> trackId);
    bool trimClip(const std::string& clipId, ClipEdge edge, Time newEdge);

    // Transitions (Dissolve). Default duration: 1 s, limited to what fits.
    bool addTransition(const std::string& clipId, std::optional<Time> duration = std::nullopt,
                       TransitionAlignment alignment = TransitionAlignment::Centered,
                       const std::string& type = "fa.transition.dissolve");
    bool updateTransition(const std::string& clipId, Time duration, TransitionAlignment alignment);
    bool removeTransition(const std::string& clipId);
    // Clip whose end is the cut nearest to timeline time t on the track (for dropping transitions).
    std::string cutBefore(const std::string& trackId, Time t, Time tolerance) const;

    // ---- M2: animatable properties and keyframes (property ids: fa::keys::properties) ----
    // Set a property at the playhead: auto-keys when the property is animated, otherwise changes its
    // constant value. Consecutive changes of the same property merge into one undo step.
    bool setProperty(const std::string& clipId, const std::string& propertyId, double value);
    bool toggleKeyframe(const std::string& clipId, const std::string& propertyId);   // at the playhead
    bool setKeyframeStyle(const std::string& clipId, const Time& sourceTime, keys::Style style);
    bool moveKeyframes(const std::string& clipId, const Time& fromSource, const Time& toSource);
    bool deleteKeyframes(const std::string& clipId, const Time& sourceTime);
    // Arbitrary change of one clip (timing must stay); mergeKey groups repeated edits into one undo step.
    bool modifyClip(const std::string& clipId, const std::string& label, std::function<void(Clip&)> change,
                    const std::string& mergeKey = {});

    // ---- M5: speed (clip and its linked audio; the clips after it ripple) ----
    bool setClipSpeed(const std::string& clipId, const ClipSpeed& speed, const std::string& mergeKey = {});
    bool resetClipSpeed(const std::string& clipId);
    bool toggleReverse(const std::string& clipId = {});   // the selected media clip by default
    // Hold the frame under the playhead of the selected (or given) video clip for `hold` (default 5 s).
    bool freezeFrameAtPlayhead(const std::string& clipId = {}, std::optional<Time> hold = std::nullopt);
    bool setHoldDuration(const std::string& clipId, Time duration);

    // ---- M5: motion tracking ----
    bool setTracking(const std::string& clipId, std::optional<MotionTrack> track);   // store / remove, one undo step
    // `target` (a title or video clip on a track above, overlapping in time) follows the tracked region: position
    // keyframes on every frame they overlap, keeping the target's current offset from the tracked point.
    bool attachToTracking(const std::string& trackedClipId, const std::string& targetClipId);
    // A pixelated rectangle (adjustment layer with Pixelate and a keyframed mask) that follows the tracked region.
    bool addTrackingMosaic(const std::string& trackedClipId, std::string* adjustmentIdOut = nullptr);
    // Canvas position (sequence pixels) of the tracked point at each output frame of the clip; frames without a
    // tracking sample are left out. Also the region size in canvas pixels at the first frame.
    std::vector<std::pair<int64_t, QPointF>> trackedPath(const std::string& clipId, QSizeF* regionSize = nullptr) const;

    // ---- M5: stabilisation ----
    bool setStabilization(const std::string& clipId, std::optional<Stabilization> result);   // store / remove
    // Level / strength / Auto Zoom of an analysed clip: re-planned at once from the stored motion (no new analysis).
    bool setStabilizationSettings(const std::string& clipId, StabilizeLevel level, double strength, bool autoZoom,
                                  double maxZoom, const std::string& mergeKey = {});
    bool setBlendMode(const std::string& clipId, BlendMode mode);
    std::string addMask(const std::string& clipId, MaskShape shape);   // returns the new mask id
    bool removeMask(const std::string& clipId, const std::string& maskId);
    bool resetTransform(const std::string& clipId);
    bool updateTransitionSettings(const std::string& clipId, const TransitionRef& settings);

    bool setClipOpacity(const std::string& clipId, double opacity);
    bool setClipVolume(const std::string& clipId, double db);
    bool renameClip(const std::string& clipId, const QString& name);
    bool setTrackFlag(const std::string& trackId, const QString& flag, bool value);   // hidden|muted|solo|locked
    bool setTrackVolume(const std::string& trackId, double db);
    bool addTrack(TrackKind kind);
    // M7 Background Removal: a picture or video on a new video track directly below the clip, over the clip's time
    // (a picture lasts the whole clip, a video at most its own length). One undo step.
    bool addBackgroundMedia(const std::string& clipId, const std::string& sourceId);
    bool modifyTrack(const std::string& trackId, const std::string& label, std::function<void(Track&)> change,
                     const std::string& mergeKey = {});

    // ---- M6: audio ----
    // Fades of an audio clip (clamped so that both fit inside it; one undo step per merge key).
    bool setClipFades(const std::string& clipId, Time fadeIn, Time fadeOut, const std::string& mergeKey = {});
    // Equaliser of an audio clip: `preset` is a built-in preset id ("" = custom gains).
    bool setClipEq(const std::string& clipId, bool enabled, const std::string& preset,
                   const std::array<double, kAudioEqBands>& gainsDb, const std::string& mergeKey = {});
    // Crossfade from this audio clip into the next one on its track (fa.audio.crossfade.<shape>), 1 s by default.
    bool addAudioCrossfade(const std::string& clipId, const std::string& shapeId = "equalPower",
                           std::optional<Time> duration = std::nullopt);
    // Cleanup / loudness and Auto Ducking settings of an audio clip; the master limiter of the sequence.
    bool setClipCleanup(const std::string& clipId, const AudioCleanup& cleanup, const std::string& mergeKey = {});
    bool setClipDucking(const std::string& clipId, const AudioDucking& ducking, const std::string& mergeKey = {});
    bool setLimiter(bool on, double ceilingDb, const std::string& mergeKey = {});
    // In / Out marks at the playhead (the export range); clearMarks removes both. One undo step each.
    bool setMarkIn();
    bool setMarkOut();
    bool clearMarks();

    // ---- M3: titles and captions ----
    void setTitleLibrary(TitleLibrary* lib) { titles_ = lib; }
    TitleLibrary* titleLibrary() const { return titles_; }
    // Add a title from a template (built-in or user preset) at `at` (default: the playhead). Title templates
    // go on `trackId` if it is a free video track, otherwise above the footage; caption templates go on
    // `trackId` if it is a subtitle track, otherwise on the first subtitle track. Selects the new clip.
    bool addTitleFromTemplate(const std::string& templateId, std::optional<Time> at = std::nullopt,
                              std::string trackId = {});
    // Change the text style of a title or caption. Captions that follow their track's style change the
    // track's style (all such captions); mergeKey groups repeated edits into one undo step.
    bool modifyTitleStyle(const std::string& clipId, const std::string& label, std::function<void(TextStyle&)> change,
                          const std::string& mergeKey = {});
    // Copy the clip's In/Loop/Out animations to every other caption on its subtitle track (one undo step).
    bool applyAnimationsToTrack(const std::string& clipId);

    // ---- M4: effect stacks (every action is one undo step) ----
    // Adds an effect of `type` with its defaults at `index` (-1 = end); returns its id ("" on failure).
    std::string addEffect(const std::string& clipId, const std::string& type, int index = -1);
    bool removeEffect(const std::string& clipId, const std::string& effectId);
    bool moveEffect(const std::string& clipId, const std::string& effectId, int newIndex);
    std::string duplicateEffect(const std::string& clipId, const std::string& effectId);
    bool setEffectEnabled(const std::string& clipId, const std::string& effectId, bool enabled);
    bool resetEffect(const std::string& clipId, const std::string& effectId);   // defaults, strength 100 %, no keys
    // Non-animated settings (choices, colours, switches): constant value; numbers use setProperty("fx.<id>.<p>").
    bool setEffectConstant(const std::string& clipId, const std::string& effectId, const std::string& param, double value);
    bool modifyEffect(const std::string& clipId, const std::string& effectId, const std::string& label,
                      std::function<void(EffectRef&)> change, const std::string& mergeKey = {});
    // Copy the clip's effect stack to the clipboard (JSON) / append the clipboard's effects (new ids).
    bool copyEffects(const std::string& clipId);
    bool pasteEffects(const std::string& clipId);
    bool canPasteEffects() const;
    // Adjustment layer at `at` (default: playhead) for `duration` (default 5 s), above the footage.
    bool addAdjustmentLayer(std::optional<Time> at = std::nullopt, Time duration = Time(5), std::string trackId = {});
    // M10: an effect or filter from the library as its own clip above the video (an adjustment layer holding just that
    // effect, named after it), placed like a title: on trackId if free there, else on a free video track above, else
    // on a new one. Selects it and returns its id ("" if it could not be added).
    std::string addEffectLayer(const EffectRef& effect, const std::string& name, std::optional<Time> at = std::nullopt,
                               Time duration = Time(5), std::string trackId = {});
    // Selective effects (M4): a rectangle / ellipse mask on one effect; it becomes the active mask "<effect>/<mask>".
    std::string addEffectMask(const std::string& clipId, const std::string& effectId, MaskShape shape);
    bool removeEffectMask(const std::string& clipId, const std::string& effectId, const std::string& maskId);

    // ---- M4: the Color tab ----
    // The Color tab edits the clip's first effect of each of these types, in this canonical stack order (ahead of
    // the clip's other effects). The first change creates the effect with neutral settings at its place, in the
    // same undo step as the change. Values are set at the playhead (auto-key when animated); consecutive changes
    // with the same non-empty mergeKey form one undo step.
    static const std::vector<std::string>& colorTabTypes();
    static const EffectRef* colorEffect(const Clip& c, const std::string& type);   // first of the type, or null
    bool setColorParams(const std::string& clipId, const std::string& type, const std::map<std::string, double>& values,
                        const std::string& mergeKey = {});
    bool setColorCurve(const std::string& clipId, const std::string& curveId, const std::vector<Vec2>& points,
                       const std::string& mergeKey = {});
    bool toggleColorKey(const std::string& clipId, const std::string& type, const std::string& param);
    // Replaces the clip's Color-tab effects (first of each type) with `effects` (a colour preset): one undo step.
    bool applyColorEffects(const std::string& clipId, const std::vector<EffectRef>& effects, const std::string& label);
    void setColorPresets(ColorPresetLibrary* lib) { colorPresets_ = lib; }
    ColorPresetLibrary* colorPresets() const { return colorPresets_; }

    // ---- M4: LUTs ----
    // Sets the LUT of the clip's LUT effect ("" = none, "builtin:<id>", or a .cube path; a path inside the project
    // folder is stored relative to it), adding the effect at its canonical place: one undo step.
    bool setColorLut(const std::string& clipId, const std::string& reference);
    // Points every effect of the sequence that uses `oldReference` at `newReference` (one undo step); returns the
    // number of effects changed.
    int relinkLut(const std::string& oldReference, const std::string& newReference);
    // Folder that relative LUT references resolve against (the project file's folder, else the working folder).
    std::filesystem::path assetDir() const;
    void setLutLibrary(LutLibrary* lib) { luts_ = lib; }
    LutLibrary* lutLibrary() const { return luts_; }

    // ---- M4: effects browser ----
    // Appends a copy of `effect` (new id per clip) to each clip (video tracks only): one undo step.
    bool addEffectToClips(const std::vector<std::string>& clipIds, const EffectRef& effect);
    void setEffectPresets(EffectPresetLibrary* lib) { effectPresets_ = lib; }
    EffectPresetLibrary* effectPresets() const { return effectPresets_; }

    // ---- M9: clipboard and the clip menu's commands ----
    // Cut / Copy put the selected clips (with their A/V partners) on the system clipboard; Paste puts them at the
    // playhead (Filmora: Ctrl+X / Ctrl+C / Ctrl+V), Duplicate right after themselves (Ctrl+D). The new clips are
    // selected.
    bool copySelection();
    bool cutSelection();
    bool paste(std::optional<Time> at = std::nullopt);
    bool canPaste() const;
    bool duplicateSelection();
    bool selectAll();
    bool setColorMark(int mark);                     // the selected clips (0 removes the mark)
    bool selectSameColorMark();                      // every clip with the primary selection's mark
    bool toggleMute();                               // the selected clips and their partners
    bool rotate(double degrees);                     // adds to the rotation of the selected video clips
    bool cropToFit();                                // fills the frame with the selected video clip (no bars)
    bool setSpeedFactor(const Rational& speed);      // uniform speed of the selected media clip
    bool selectClipRange();                          // In / Out marks at the selected clip
    // Replace the media of a clip, keeping its place, length (shortened if the new media is shorter), transform,
    // effects, masks and animations; generated placeholder clips become media clips.
    bool replaceClip(const std::string& clipId, const std::string& sourceId);

    // ---- M9: the Stickers and Templates pages ----
    // A sticker at `at` (default: the playhead) on `trackId` or the first free video track above the footage, at its
    // catalogue size; selected. Returns the clip id (empty on failure).
    std::string addSticker(const std::string& stickerId, std::optional<Time> at = std::nullopt, std::string trackId = {});
    // A template's layers on free tracks at `at`, in order (one undo step); selected. Returns the new clip ids.
    std::vector<std::string> addTemplate(const std::string& templateId, std::optional<Time> at = std::nullopt);
    bool setStickerLook(const std::string& clipId, uint32_t color, const QString& text);   // tint (0 = own) and words

    // ---- M10: gaps (Clipchamp's "Delete this gap") ----
    // Closes the gap of track `trackId` that contains `at` (see edit::deleteGap; one undo step).
    bool deleteGap(const std::string& trackId, Time at);
    // Why that gap cannot be deleted (empty: it can), as a sentence for the user.
    QString gapProblem(const std::string& trackId, Time at) const;

signals:
    void message(const QString& text);   // a problem (shown as a warning)
    void info(const QString& text);      // a confirmation (plain status message)

private:
    bool run(std::unique_ptr<Command> cmd);
    std::string selectedOrUnderPlayhead() const;
    void maybeMatchSequence(const MediaSource& src);

    Document* doc_;
    QPointer<QWidget> dialogParent_;
    MatchPolicy match_ = MatchPolicy::Ask;
    TitleLibrary* titles_ = nullptr;
    ColorPresetLibrary* colorPresets_ = nullptr;
    LutLibrary* luts_ = nullptr;
    EffectPresetLibrary* effectPresets_ = nullptr;
};

}  // namespace fa::ui
