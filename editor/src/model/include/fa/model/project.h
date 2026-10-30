// Project / timeline document model (pure data, no I/O, no media access).
//
// Conventions
//   * All times are exact Rationals in seconds.
//   * Clip.timelineStart and Clip.duration must lie on frame boundaries of the owning
//     sequence's frame rate (frame-accurate edits); Clip.sourceIn may be any time.
//   * Track index 0 is the bottom of the stack; higher tracks are composited on top.
//   * Clip keyframes are expressed in SOURCE time (as Filmora does, EXP-002), so trimming a
//     clip's head does not move its animation relative to the picture.
//   * M0 clips play at speed 1: source time = sourceIn + (t - timelineStart).
#pragma once

#include "fa/anim/animatable.h"
#include "fa/core/image.h"
#include "fa/core/time.h"
#include "fa/model/title.h"

#include <array>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fa {

class ModelError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Subtitle tracks hold caption clips (title clips) and are composited above every video track.
enum class TrackKind { Video, Audio, Subtitle };

struct MediaSource {
    std::string id;
    std::string path;   // absolute, or relative to the project file's directory
    // Probe cache (optional; filled by tools/the renderer, used for validation).
    std::optional<Time> duration;
    std::optional<FrameRate> frameRate;
    int width = 0;
    int height = 0;
    bool hasVideo = true;
    bool hasAudio = false;
    // M7 Scene Detection: a library item for one scene of the file - it places only `range` (source time) and shows
    // `name`. Empty name = the file name; no range = the whole file.
    std::string name;
    std::optional<TimeRange> range;
    friend bool operator==(const MediaSource&, const MediaSource&) = default;
};

// Where a transition sits relative to the cut (Filmora: Overlap / Postfix / Prefix, EXP-001c).
enum class TransitionAlignment { Centered, StartsAtCut, EndsAtCut };

// Direction of movement for wipe / slide / push transitions (the incoming clip enters moving this way).
enum class TransitionDirection { Left, Right, Up, Down };

struct TransitionRef {
    std::string type;   // see kTransitionTypes in fa/render/transitions.h, e.g. "fa.transition.dissolve"
    Time duration;
    TransitionAlignment alignment = TransitionAlignment::Centered;
    TransitionDirection direction = TransitionDirection::Left;   // wipe, slide, push
    double softness = 0.1;                                       // wipe edge width (fraction of the frame)
    friend bool operator==(const TransitionRef&, const TransitionRef&) = default;
};

// ---- visual properties (M2) --------------------------------------------------------------------

// Layer blend modes (formulas on gamma-encoded RGB, as measured in Filmora, Stage 1 EXP-011).
enum class BlendMode {
    Normal, Multiply, Screen, Overlay, Darken, Lighten, ColorDodge, ColorBurn,
    HardLight, SoftLight, Difference, Exclusion, LinearDodge, LinearBurn
};
inline constexpr int kBlendModeCount = 14;
const char* blendModeId(BlendMode m);          // "normal", "multiply", ... (file format)
const char* blendModeLabel(BlendMode m);       // "Normal", "Multiply", ... (UI)
std::optional<BlendMode> blendModeFromId(const std::string& id);

// Geometry of a video clip on the canvas. Units:
//   position: offset of the layer centre from the canvas centre, fraction of canvas width / height
//             (+x right, +y down). 0,0 = centred. The UI shows pixels of the sequence resolution.
//   scale:    1.0 = 100 % = the source fitted into the canvas (letterboxed, Filmora default).
//   rotation: degrees, clockwise.
//   anchor:   pivot for scale/rotation as offset from the layer centre, fraction of layer width/height.
//   crop:     fraction of the source removed on each side (the remaining picture keeps its place;
//             cropped areas become transparent).
// All animatable values carry keyframes in SOURCE time (like opacity, Stage 1 EXP-002).
struct ClipTransform {
    Animatable<double> positionX{0.0};
    Animatable<double> positionY{0.0};
    Animatable<double> scaleX{1.0};
    Animatable<double> scaleY{1.0};
    Animatable<double> rotation{0.0};
    Animatable<double> anchorX{0.0};
    Animatable<double> anchorY{0.0};
    bool uniformScale = true;   // UI lock: width and height scale together
    bool flipH = false;
    bool flipV = false;
    double cropLeft = 0.0, cropTop = 0.0, cropRight = 0.0, cropBottom = 0.0;

    bool isDefault() const { return *this == ClipTransform{}; }
    friend bool operator==(const ClipTransform&, const ClipTransform&) = default;
};

enum class MaskShape { Rectangle, Ellipse, Polygon };
enum class MaskMode { Add, Subtract, Intersect };   // how a mask combines with the masks above it

// Shape mask in layer space. Units are "layer heights" measured from the layer centre, so equal
// width and height give a circle/square on any aspect ratio (Filmora's default ellipse/rectangle:
// diameter/side = 0.5 x frame height, EXP-009/018).
struct Mask {
    std::string id;
    std::string name;
    MaskShape shape = MaskShape::Ellipse;
    MaskMode mode = MaskMode::Add;
    bool enabled = true;
    bool inverted = false;
    Animatable<double> centerX{0.0};
    Animatable<double> centerY{0.0};
    Animatable<double> width{0.5};
    Animatable<double> height{0.5};
    Animatable<double> rotation{0.0};    // degrees clockwise
    Animatable<double> feather{0.0};     // edge softness (Gaussian sigma), layer heights
    Animatable<double> opacity{1.0};
    double cornerRadius = 0.0;           // rectangle: fraction of the shorter side (0..0.5)
    std::vector<Vec2> points;            // polygon: vertices in the unit box [-0.5, 0.5]^2, scaled by width/height
    friend bool operator==(const Mask&, const Mask&) = default;
};

// One effect of a clip's (or adjustment layer's) effect stack (M4). Effects run in stack order on the clip's
// picture before it is placed and composited (adjustment layers: on the composite below). The effect type
// (see fa/render/effects.h) defines its parameters; missing parameters take the type's defaults.
//   id       unique within the clip ("fx1", ...): keyframe property ids are "fx.<id>.<param>"
//   mix      strength 0..1: output = mix(input, effect(input), mix * mask coverage)
//   strings  non-numeric settings (e.g. "lut": path of a .cube file)
//   curves   control points of curve parameters (x, y in 0..1), sorted by x
//   masks    selective effect: the effect only applies inside (M2 masks, in the picture's layer-height units)
struct EffectRef {
    std::string id;
    std::string type;
    int version = 1;
    bool enabled = true;
    Animatable<double> mix{1.0};
    std::map<std::string, Animatable<double>> params;
    std::map<std::string, std::string> strings;
    std::map<std::string, std::vector<Vec2>> curves;
    std::vector<Mask> masks;
    friend bool operator==(const EffectRef&, const EffectRef&) = default;
};

// M5: clip retiming (rules in fa/model/time_map.h).
struct SpeedKey {
    Time source;            // absolute source time of the key
    double speed = 1.0;     // source seconds per timeline second
    friend bool operator==(const SpeedKey&, const SpeedKey&) = default;
};
// How an output frame between two source frames is made: the nearest earlier frame, a mix of the two,
// motion-compensated (optical flow) interpolation at two quality levels (fa/vision/flow.h), or (M7) a neural
// in-between frame (RIFE, fa/ai/interpolate.h) computed before an export / Render Preview.
enum class FrameInterpolation { Sampling, Blending, OpticalFlow, OpticalFlowQuality, Neural };
struct ClipSpeed {
    Rational speed{1};             // uniform speed (ignored when `ramp` has keys)
    bool reverse = false;          // play the source range backwards
    bool freeze = false;           // a hold of the frame at sourceIn (Freeze Frame; silent)
    std::vector<SpeedKey> ramp;    // speed ramp (>= 2 keys, increasing source time); empty: uniform
    std::string rampPreset;        // the preset the ramp came from ("" = edited by hand)
    bool maintainPitch = true;     // audio keeps its pitch (time stretch) or follows the speed (resampling)
    FrameInterpolation interpolation = FrameInterpolation::Sampling;
    friend bool operator==(const ClipSpeed&, const ClipSpeed&) = default;
};

// M5 motion tracking of a region of the clip's picture (fa/vision/track.h): one sample per analysed source frame.
struct TrackingSample {
    int64_t frame = 0;          // source frame index
    double x = 0.5, y = 0.5;    // region centre, fractions of the picture (0..1)
    double scale = 1.0;         // region size relative to the first frame
    double confidence = 0.0;    // 0..1
    bool lost = false;          // the target could not be found (the last good position is held)
    friend bool operator==(const TrackingSample&, const TrackingSample&) = default;
};
struct MotionTrack {
    double boxX = 0.5, boxY = 0.5, boxW = 0.25, boxH = 0.25;   // the region on its first frame (picture fractions)
    int64_t firstFrame = 0;                                  // source frame the region was placed on
    std::vector<TrackingSample> samples;                     // increasing frame
    std::string media;   // identity of the analysed file (mediaIdentity); a different one now = stale data
    friend bool operator==(const MotionTrack&, const MotionTrack&) = default;
};

// M5 stabilisation (fa/model/stabilize_plan.h): the camera motion of every analysed source frame plus the settings.
// The per-frame corrections and the zoom are derived from them (planStabilization) whenever either changes, so
// Weak / Normal / Strong, the strength and Auto Zoom need no new analysis. Corrections are applied by clipPlacement:
// picture point u (centred) -> zoom * (R(da) u + d), before the clip transform.
enum class StabilizeLevel { Weak, Normal, Strong };
struct CameraMotion {        // from the previous analysed frame to this one
    double dx = 0, dy = 0;   // picture fractions (of the width / height)
    double da = 0;           // radians, about the picture centre (clockwise on screen)
    bool valid = false;      // false: not measurable (too few features), treated as no motion
    friend bool operator==(const CameraMotion&, const CameraMotion&) = default;
};
struct FrameCorrection {
    double dx = 0, dy = 0;   // picture fractions
    double da = 0;           // radians
    friend bool operator==(const FrameCorrection&, const FrameCorrection&) = default;
};
struct Stabilization {
    StabilizeLevel level = StabilizeLevel::Normal;
    double strength = 1.0;      // 0..1
    bool autoZoom = true;       // zoom in just enough that no border shows (at most maxZoom)
    double maxZoom = 1.10;      // Auto Zoom limit (1 .. kMaxStabilizeZoom): beyond it the path is kept inside the frame
    int64_t firstFrame = 0;     // source frame of motion[0]
    Rational frameRate{30};     // the media's frame rate (source time -> analysed frame)
    int width = 0, height = 0;  // the media's picture size (pixels; rotation needs the aspect)
    std::string media;          // mediaIdentity at analysis; a different one now = stale data
    std::vector<CameraMotion> motion;
    // derived by planStabilization (not stored in the project file)
    std::vector<FrameCorrection> corrections;   // one per motion entry
    double zoom = 1.0;
    double limitedShare = 0.0;  // share of frames whose correction the zoom limit reduced
    friend bool operator==(const Stabilization&, const Stabilization&) = default;
};

// M6 audio of a clip (audio clips; a video clip's sound is its linked audio clip). Rules in fa/render/audio_dsp.h and
// fa/render/audio_mix.h. Fades are in timeline time from the clip's edges, linear in amplitude (EXP-020); balance
// keyframes are in source time like every other clip keyframe.
inline constexpr int kAudioEqBands = 10;
// Cleanup of a clip's audio (M6): conventional DSP (fa/render/audio_cleanup.h), applied before the equaliser.
struct AudioCleanup {
    double denoise = 0;          // spectral noise reduction strength 0 (off) .. 1
    double humHz = 0;            // hum removal: 0 = off, 50 or 60 (notches at the harmonics too)
    int humHarmonics = 4;        // 1 .. 8
    double highPassHz = 0;       // 0 = off, 20 .. 1000 Hz (24 dB / octave)
    double lowPassHz = 0;        // 0 = off, 1000 .. 22000 Hz
    // Loudness normalisation: a fixed gain of targetLufs - measuredLufs, measured on demand (BS.1770-4 integrated
    // loudness of the clip after cleanup and EQ, before its volume). Without a measurement it does nothing.
    bool normalize = false;
    double targetLufs = -16.0;
    std::optional<double> measuredLufs;
    friend bool operator==(const AudioCleanup&, const AudioCleanup&) = default;
};

// Auto ducking (M6): while this clip has sound, the target audio tracks are lowered. Signal based: the clip "has
// sound" in a 10 ms block whose RMS (after cleanup, EQ, volume and fades) exceeds the threshold.
struct AudioDucking {
    bool enabled = false;
    std::vector<std::string> targets;   // audio track ids; empty = every other audio track
    double amountDb = -12.0;            // how far the targets go down (-60 .. 0)
    double thresholdDb = -40.0;         // dBFS
    double attackMs = 250;              // the targets go down over this long, finishing as the sound starts
    double holdMs = 300;                // and stay down this long after it stops
    double releaseMs = 500;             // then come back up over this long
    friend bool operator==(const AudioDucking&, const AudioDucking&) = default;
};

// M7 Background Removal of a video clip: an AI matting model's alpha, analysed per source frame and cached outside the
// project (fa/render/matte_cache.h), refined by these settings when the frame is drawn. The media is never changed.
struct AiCutout {
    bool enabled = false;
    std::string model = "rvm-mobilenetv3";   // fa_ai model id: "rvm-mobilenetv3" (people) or "birefnet-general-tiny"
    int quality = 1;                         // 0 fast, 1 standard, 2 high (the matting resolution)
    std::string modelSha;                    // SHA-256 of the model file that made the mattes (set by the analysis)
    double threshold = 0.0;                  // 0 .. 0.9: alpha below it -> 0, the rest stretched
    double edge = 0.0;                       // -10 .. 10 px: shrink (<0) / grow (>0) the subject
    double feather = 0.0;                    // 0 .. 20 px: soften the edge
    double spill = 0.5;                      // 0 .. 1: remove the old background's colour from soft edges
    double smoothing = 0.5;                  // 0 .. 1: calm frame-to-frame flicker of the matte
    std::optional<Rgba8> background;         // fill the removed background with a colour (else transparent)
    bool isDefault() const { return *this == AiCutout{}; }
    friend bool operator==(const AiCutout&, const AiCutout&) = default;
};

struct ClipAudio {
    Time fadeIn;                         // 0 = none
    Time fadeOut;
    Animatable<double> balance{0.0};     // -1 (left) .. 1 (right), EXP-020 law
    bool eqEnabled = false;
    std::string eqPreset;                // a built-in preset id, or empty for custom gains
    std::array<double, kAudioEqBands> eqGainsDb{};   // 31 Hz .. 16 kHz, +-12 dB
    AudioCleanup cleanup;
    AudioDucking ducking;
    bool isDefault() const { return *this == ClipAudio{}; }
    friend bool operator==(const ClipAudio&, const ClipAudio&) = default;
};

// M7 Video Upscaling: the clip's frames enlarged before compositing - Lanczos (no model) or Real-ESRGAN ("general"
// or "anime", prepared before an export / Render Preview). Only enlarges up to the size the clip is shown at.
enum class UpscaleMode { Off, Lanczos, Ai };
struct ClipUpscale {
    UpscaleMode mode = UpscaleMode::Off;
    int scale = 2;                    // 2, 3 (anime model only) or 4
    std::string model = "general";   // AI: "general" (realesrgan-x4plus) or "anime" (realesr-animevideov3)
    double deflicker = 0.5;           // AI: 0..1, how much a static area keeps the previous frame's result
    bool isDefault() const { return *this == ClipUpscale{}; }
    friend bool operator==(const ClipUpscale&, const ClipUpscale&) = default;
};

// M9: a sticker - a picture the engine draws (fa/text/stickers.h catalogue), placed with the clip's transform like
// media. Full-frame stickers (backgrounds, overlays, media placeholders) are drawn at the canvas size, others square.
struct StickerData {
    std::string id;          // catalogue id, e.g. "emoji.smile", "bg.sunset", "placeholder"
    uint32_t color = 0;      // 0xRRGGBB for stickers that can be recoloured; 0 = the sticker's own colours
    std::string text;        // the word of badges / buttons / bubbles; empty = the sticker's own
    friend bool operator==(const StickerData&, const StickerData&) = default;
};

struct Clip {
    std::string id;
    std::string sourceId;        // empty for title clips
    std::string name;
    Time timelineStart;
    Time sourceIn;
    Time duration;
    bool enabled = true;
    Animatable<double> opacity{1.0};       // 0..1, keyframes in source time
    std::vector<EffectRef> effects;
    std::optional<TransitionRef> outTransition;   // transition into the next clip on the track
    std::string linkId;          // clips sharing a non-empty linkId move/trim/split/delete together (A/V link)
    // audio clips: gain in dB (Filmora "Volume", EXP-020: plain dB multiply); M6: keyframes (source time) = automation
    Animatable<double> volumeDb{0.0};
    ClipAudio audio;             // M6: fades, balance, equaliser (audio clips)
    AiCutout aiCutout;           // M7: AI background removal (video clips)
    ClipUpscale upscale;         // M7: video upscaling (video clips)
    ClipTransform transform;     // video clips (M2)
    BlendMode blend = BlendMode::Normal;
    std::vector<Mask> masks;     // applied top to bottom
    std::optional<TitleData> title;   // M3: a title / caption clip (no media); its picture is generated
    std::optional<StickerData> sticker;   // M9: a sticker clip (no media); its picture is generated
    bool adjustment = false;          // M4: adjustment layer (no media): its effects apply to the tracks below
    ClipSpeed speed;                  // M5: speed, reverse, freeze, ramps (video and its linked audio share them)
    std::optional<MotionTrack> tracking;   // M5: motion tracking result (video clips)
    std::optional<Stabilization> stabilization;   // M5: video stabilisation (video clips)
    int colorMark = 0;                // M9: timeline colour label (0 = none, 1..kColorMarkCount)
    // M9: the motion presets the clip's keyframes came from (fa/model/motion_presets.h), "" = none
    std::string motionIn, motionOut, motionCombo;
    bool muted = false;               // M9: the clip's sound is silent (Mute; linked partners are muted together)

    bool isTitle() const { return title.has_value(); }
    bool isAdjustment() const { return adjustment; }
    bool isSticker() const { return sticker.has_value(); }
    bool isGenerated() const { return isTitle() || isAdjustment() || isSticker(); }   // no media source
    Time timelineEnd() const { return timelineStart + duration; }
    Time sourceOut() const;   // end of the source range played (sourceIn + duration at speed 1; time_map.h)
    TimeRange timelineRange() const { return {timelineStart, duration}; }
    // Source time at timeline time t (continuous map, M5 speed included; time_map.h).
    Time sourceTimeAt(const Time& t) const;
    friend bool operator==(const Clip&, const Clip&) = default;
};

struct Track {
    std::string id;
    TrackKind kind = TrackKind::Video;
    std::string name;
    bool hidden = false;   // video: not composited
    bool muted = false;    // audio
    bool solo = false;     // audio (M6): while any audio track is soloed, only soloed tracks play
    bool locked = false;   // UI: clips on a locked track cannot be edited
    double volumeDb = 0.0; // audio: track gain in dB
    std::vector<Clip> clips;   // sorted by timelineStart, non-overlapping
    // Subtitle tracks: style shared by captions with followTrackStyle, and where captions sit
    // (y of the anchor point as a fraction of the frame height, measured from the top).
    TextStyle subtitleStyle = defaultSubtitleStyle();
    double subtitleY = 0.92;

    static TextStyle defaultSubtitleStyle() {
        TextStyle s;
        s.size = 20.0;
        s.weight = 600;
        s.outline = true;
        s.outlineWidth = 3.0;
        s.anchor = TextAnchor::Bottom;
        s.wrapWidth = 0.85;
        return s;
    }

    const Clip* clipAt(const Time& t) const;   // clip whose range contains t, or nullptr
    void sortClips();
    friend bool operator==(const Track&, const Track&) = default;
};

// M9: number of timeline colour marks a clip can carry (Clip::colorMark 1..kColorMarkCount).
inline constexpr int kColorMarkCount = 13;

// M8: the largest canvas side a project may declare (validation refuses more; GPU textures go to 16384).
inline constexpr int kMaxCanvasSide = 16384;

struct Sequence {
    std::string id;
    std::string name;
    int width = 1920;
    int height = 1080;
    FrameRate frameRate = FrameRate::fps30();
    int sampleRate = 48000;
    std::vector<Track> tracks;
    // M6 master bus: a sample-peak limiter on the mix (playback and export), off by default.
    bool limiter = false;
    double limiterCeilingDb = -1.0;   // -20 .. 0 dBFS
    // M6 In / Out marks (the export window's "In to Out" range), frame-aligned; either may be unset.
    std::optional<Time> markIn;
    std::optional<Time> markOut;

    Time duration() const;           // end of the last clip on any track
    int64_t frameCount() const;      // number of frames needed to cover duration()
    Track* findTrack(const std::string& id);
    const Track* findTrack(const std::string& id) const;
    friend bool operator==(const Sequence&, const Sequence&) = default;
};

struct ClipLocation {
    Track* track = nullptr;
    size_t index = 0;
    Clip& clip() const { return track->clips[index]; }
};

struct Project {
    // 2 = M2 (transforms, masks, blend modes, transition parameters); 3 = M3 (titles, subtitle tracks);
    // 4 = M4 (effects); 5 = M5 (speed, tracking, stabilisation); 6 = M6 (audio automation, fades, balance, EQ, solo,
    // audio crossfades). Every older version still loads and keeps its behaviour.
    // 10 = M9 (clip colour marks, clip mute, sticker clips).
    static constexpr int kFormatVersion = 10;

    std::string name;
    std::vector<MediaSource> sources;
    std::vector<Sequence> sequences;
    uint64_t nextId = 1;   // deterministic id generator state (persisted)

    std::string newId(const std::string& prefix);   // "<prefix>-<n>"

    MediaSource* findSource(const std::string& id);
    const MediaSource* findSource(const std::string& id) const;
    Sequence* findSequence(const std::string& id);
    const Sequence* findSequence(const std::string& id) const;
    // Locate a clip anywhere in a sequence; throws ModelError if not found.
    ClipLocation locateClip(Sequence& seq, const std::string& clipId);

    friend bool operator==(const Project&, const Project&) = default;
};

// Time range covered by a transition between clip `from` and the adjacent clip `to`.
struct TransitionSpan {
    TimeRange range;
    const Clip* from = nullptr;
    const Clip* to = nullptr;
};

// Span of clip.outTransition on a track, or nullopt when the clip has none.
// Throws ModelError if the transition is malformed (no adjacent clip, span outside both clips).
std::optional<TransitionSpan> transitionSpan(const Track& track, size_t clipIndex);

// M6: audio crossfades are transitions on audio tracks whose type is "fa.audio.crossfade.<shape>"
// (shapes: constantGain, equalPower, exponential, logarithmic; curves in fa/render/audio_dsp.h).
inline constexpr const char* kAudioCrossfadePrefix = "fa.audio.crossfade.";
bool isAudioCrossfade(const std::string& transitionType);

struct ValidationIssue {
    enum class Severity { Warning, Error } severity = Severity::Error;
    std::string where;     // e.g. "sequence seq-1 / track v1 / clip c-3"
    std::string message;
};

// Structural checks: unique ids, known source references, positive durations, frame-aligned
// edit points, no overlapping clips, clip ranges inside known source durations.
std::vector<ValidationIssue> validate(const Project& project);
bool hasErrors(const std::vector<ValidationIssue>& issues);
std::string formatIssues(const std::vector<ValidationIssue>& issues);

}  // namespace fa
