// Clip retiming (M5): how a clip's timeline time maps to the time of its source media.
//
// A clip occupies [timelineStart, timelineStart + duration) on the timeline and shows the source range
// [sourceIn, sourceOut()]. Inside, clip-local time tau = t - timelineStart maps to source time s(tau):
//   speed 1       s = sourceIn + tau                                          (the M0..M4 rule)
//   uniform       s = sourceIn + tau * speed                                  (exact rationals)
//   reverse       s = sourceIn + (duration - tau) * speed                     (the same range, backwards)
//   freeze        s = sourceIn                                                (a hold of one source frame)
//   ramp          speed keys (absolute source time, speed); between consecutive keys the speed is eased with
//                 smoothstep in OUTPUT time, keys anchored at their source positions, so a segment of source
//                 length D between speeds v0 and v1 takes 2 D / (v0 + v1) seconds. This reproduces Filmora's
//                 Bullet export frame for frame (Stage 1 EXP-004: 107 / 107 frames, 3.5595 s). Keys are absolute,
//                 so the two halves of a split ramp keep exactly the same map.
// Outside [0, duration) (transition handles) the map continues with the speed at the edge.
//
// Which source frame an output frame shows (EXP-004):
//   * uniform / reverse / freeze: an output frame covers [tau, tau + d) (d = 1 / sequence fps); it shows the source
//     frame visible at the LOWER end of the source interval that maps onto it: 2x -> 0, 2, 4 ...; 0.5x -> 0, 0, 1,
//     1 ...; reverse 1x -> 299, 298 ... 0 (reverse 2x shows the 2x frames in reverse order: 298, 296 ... 0);
//   * ramps: the source frame NEAREST to s(tau) (the measured Bullet export; floor matches only 53 / 107 frames).
//   * Frame Blending mixes the frame at the sample position with the next one by the fractional position.
// Ramps are evaluated in double precision from the exact output time (closed form, nothing accumulates).
#pragma once

#include "fa/model/project.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace fa {

// The speeds the engine accepts (uniform and ramps); outside this range edits are refused.
inline constexpr double kMinSpeed = 0.1;
inline constexpr double kMaxSpeed = 10.0;

bool isRetimed(const ClipSpeed& sp);   // anything but plain speed 1 forward

// Continuous source time at clip-local time `local` (may lie outside [0, duration): handles).
Time sourceTimeAtLocal(const Clip& c, const Time& local);
// Inverse (M6): the clip-local time at which source time `source` plays (exact for uniform / reverse, numeric for
// ramps; a freeze frame has no inverse and gives 0).
Time localAtSource(const Clip& c, const Time& source);
// Length of the source range the clip plays: [sourceIn, sourceIn + sourceLength(c)].
Time sourceLength(const Clip& c);
// Timeline duration that plays `length` seconds of source starting at `sourceIn` with these settings
// (uniform: length / speed; freeze: not defined -> throws; ramp: from the keys).
Time durationForSource(const ClipSpeed& sp, const Time& sourceIn, const Time& length);
// Speed (source seconds per timeline second) at clip-local time; 0 for a freeze; negative when reversed.
double speedAtLocal(const Clip& c, const Time& local);

// Fast double-precision form of the continuous map for per-sample use (audio): built once per clip.
class SourceMapper {
public:
    explicit SourceMapper(const Clip& c);
    ~SourceMapper();
    SourceMapper(SourceMapper&&) noexcept;
    double source(double localSeconds) const;   // source seconds
    double speed(double localSeconds) const;    // signed (negative when reversed), 0 for a freeze
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// What output frame [local, local + frameDuration) shows.
struct FrameSample {
    Time time;                    // source time of the sampled frame (the decoder shows the frame visible at it)
    std::optional<Time> next;     // Frame Blending / Optical Flow: the following source frame ...
    double weight = 0.0;          // ... and its weight (0..1)
};
// sourceRate: the media's frame rate (frame positions for ramps and blending); without it, ramps fall back to the
// continuous source time and nothing is blended.
FrameSample sampleFrame(const Clip& c, const Time& local, const Time& frameDuration,
                        const std::optional<FrameRate>& sourceRate);

// Edits that keep the map (trims, splits): sourceIn after the clip's head moves by `delta` on the timeline, and
// after its duration changes to `newDuration` (only reverse moves sourceIn for a tail change).
Time sourceInAfterHeadTrim(const Clip& c, const Time& delta);
Time sourceInAfterDurationChange(const Clip& c, const Time& newDuration);

// Whole-frame timeline duration for playing `length` of source from `sourceIn` with `sp` at `rate`: as many frames
// as needed to show the whole range (ceil), but never more than the media (if known) can fill from sourceIn.
Time frameDurationForSource(const ClipSpeed& sp, const Time& sourceIn, const Time& length, const FrameRate& rate,
                            const std::optional<Time>& mediaDuration);
// The last source instant a retimed clip shows (the sample of its last output frame); validation uses it.
Time lastSampledSource(const Clip& c, const FrameRate& rate);

// True if the clip needs source beyond `mediaDuration` (+ slack): at speed 1 its out-point, when retimed the
// source instant its last sampled frame shows.
bool exceedsMedia(const Clip& c, const FrameRate& rate, const Time& mediaDuration, const Time& slack = Time(0));

// Validation: why these settings cannot be used for this clip (empty: fine).
std::string speedProblem(const Clip& c);

// ---- M5 analysis data ---------------------------------------------------------------------------------------

// "path|size|modified" of a media file: stored with analysis results (tracking, stabilisation) so that a relinked or
// replaced file shows the results as stale instead of applying them to other pictures. Empty if the file is missing.
std::string mediaIdentity(const std::filesystem::path& file);
// The tracking sample of a source frame (the nearest analysed frame within +-1; nullptr outside the analysis).
const TrackingSample* trackSampleAt(const MotionTrack& track, int64_t frame);

// Ramp presets (fractions of the clip's source range, speeds). MeerCut's own shapes plus "bullet", which uses the
// key positions and speeds Stage 1 measured in Filmora's Bullet preset (EXP-004).
struct RampPreset {
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::pair<double, double>> keys;   // (fraction 0..1 of the source range, speed)
};
const std::vector<RampPreset>& rampPresets();
const RampPreset* findRampPreset(const std::string& id);
// Keys of a preset placed on the source range [sourceIn, sourceIn + length].
std::vector<SpeedKey> rampKeys(const std::vector<std::pair<double, double>>& fractions, const Time& sourceIn,
                               const Time& length);

}  // namespace fa
