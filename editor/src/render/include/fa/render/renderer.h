// Headless renderer: project -> frames -> an export format (H.264 / HEVC / AV1 / VP9 / ProRes / GIF / audio only).
#pragma once

#include "fa/media/export_format.h"
#include "fa/media/media.h"
#include "fa/model/project.h"
#include "fa/render/backend.h"
#include "fa/render/timeline_eval.h"
#include "fa/render/title_renderer.h"
#include "fa/render/transitions.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <set>
#include <vector>

namespace fa {

class RenderError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Thrown when RenderOptions::cancel was set; the partial output file has been removed.
class RenderCancelled : public RenderError {
public:
    RenderCancelled() : RenderError("render cancelled") {}
};

// Supplies decoded source frames for clips. Implementations may cache decoders per clip.
class FrameProvider {
public:
    struct Result {
        const Image* image = nullptr;   // valid until the next request for the same clip
        Time requestedTime;             // source time asked for
        Time frameTime;                 // presentation time of the frame returned
        int64_t frameIndex = 0;         // source frame number returned
        bool clamped = false;           // request fell outside the media; edge frame held
        // Non-zero: a promise that the same key means the same pixels (file, decoder, frame, size), so a GPU
        // backend may keep the uploaded texture (a paused frame being graded). 0 = no promise, upload every time.
        uint64_t contentKey = 0;
        bool matteMissing = false;      // M7: the clip removes its background but this frame has no matte yet
        bool upscaleMissing = false;    // M7: AI upscaling is on but this frame was not prepared (drawn plain)
        bool neuralMissing = false;     // M7: AI interpolation is on but this in-between frame was not prepared
    };
    virtual ~FrameProvider() = default;
    virtual Result frame(const Clip& clip, const MediaSource& source, const Time& sourceTime) = 0;
    // M7: the sequence size the next frames are composited for (upscaled clips are prepared for it). Default: ignored.
    virtual void canvas(int /*width*/, int /*height*/) {}
    // M7 AI Interpolation: the prepared in-between picture of source frames a and b at time step t, or nullptr when it
    // was not prepared; `cut` = the pair spans a shot change (nothing is invented: the nearer frame is shown).
    virtual const Image* neuralFrame(const Clip& /*clip*/, const MediaSource& /*source*/, int64_t /*a*/, int64_t /*b*/,
                                     double /*t*/, bool& cut) {
        cut = false;
        return nullptr;
    }
    // Folder that relative asset references (effect LUT files) are resolved against: the project file's folder.
    virtual std::filesystem::path assetDir() const { return {}; }
};

// FFmpeg-backed provider: one VideoDecoder per clip (sequential access stays seek-free),
// least-recently-used decoders are closed beyond maxOpenDecoders to bound memory.
class DecoderPool final : public FrameProvider {
public:
    DecoderPool(std::filesystem::path projectFile, size_t maxOpenDecoders = 6,
                VideoDecoder::Options decoderOptions = {});
    ~DecoderPool() override;
    Result frame(const Clip& clip, const MediaSource& source, const Time& sourceTime) override;
    std::filesystem::path assetDir() const override;
    void canvas(int width, int height) override {
        canvasW_ = width;
        canvasH_ = height;
    }
    const Image* neuralFrame(const Clip& clip, const MediaSource& source, int64_t a, int64_t b, double t, bool& cut) override;
    const MediaInfo& mediaInfo(const MediaSource& source);   // probed once per source
    int64_t totalSeeks() const;
    // M7: clips whose Background Removal had no matte for a frame that was drawn (they were drawn without it).
    // Export refuses such frames (renderProject throws); the preview shows the original and the inspector says why.
    const std::set<std::string>& clipsMissingMattes() const { return missingMattes_; }
    void clearMissingMattes() { missingMattes_.clear(); }
    // M7: clips whose AI upscaling / AI interpolation frames were not prepared (drawn plain / with optical flow).
    const std::set<std::string>& clipsMissingUpscales() const { return missingUpscales_; }
    const std::set<std::string>& clipsMissingNeuralFrames() const { return missingNeural_; }

private:
    struct Entry;
    std::filesystem::path projectFile_;
    size_t maxOpen_;
    VideoDecoder::Options decoderOptions_;
    uint64_t tick_ = 0;
    std::map<std::string, std::unique_ptr<Entry>> decoders_;   // by clip id
    std::map<std::string, MediaInfo> probes_;                  // by source id
    int64_t closedSeeks_ = 0;
    std::set<std::string> missingMattes_, missingUpscales_, missingNeural_;
    int canvasW_ = 0, canvasH_ = 0;
    Image neural_;   // the last neural frame read
};

struct RenderOptions {
    std::string sequenceId;              // empty = first sequence
    int64_t firstFrame = 0;
    int64_t lastFrame = -1;              // inclusive; -1 = end of sequence
    EncoderSettings encoder;             // preset / quality / bit rate / bit depth; size, rate, codec and muxer are set
                                         // from the format and the fields below
    // M6 export formats (fa/media/export_format.h)
    std::string format;                  // export format id; empty = from the output's extension (.mp4 = mp4-h264)
    HardwareEncoding hardware = HardwareEncoding::Cpu;   // video encoder choice (the export window defaults to Auto)
    std::string audioCodec;              // empty = the format's default
    int outputWidth = 0, outputHeight = 0;   // 0 = the sequence's size; else the picture is scaled (Lanczos)
    std::optional<FrameRate> outputRate;     // export frame rate; default = the sequence's
    // Export only these clips (and their linked partners): the others are left out and the range is from the first
    // kept clip's start to the last one's end (firstFrame / lastFrame are ignored). Empty = the whole sequence.
    std::vector<std::string> onlyClips;
    // Quality preset 0 (smallest) .. 4 (best), mapped to the chosen encoder's own scale (nativeQuality) once the
    // encoder is known; -1 = use encoder.crf as given. Ignored when encoder.bitrate > 0.
    int qualityLevel = -1;
    // Audio output: sample rate (0 = the sequence's; the mix is made at this rate) and channels (2 stereo, 1 mono:
    // the sources are folded down to one channel before mixing).
    int audioSampleRate = 0;
    int audioChannels = 2;
    bool allowUnsupported = false;       // downgrade unsupported effects/transitions to warnings
    bool includeAudio = true;            // AAC track with the mixed audio tracks (silence if there are none)
    int audioBitrate = 192000;
    const std::atomic<bool>* cancel = nullptr;   // set to true to abort (RenderCancelled, output removed)
    BackendChoice backend = BackendChoice::Auto;  // compositing: Auto = GPU for frames with compositing work
    bool hardwareDecode = false;                  // D3D11VA decoding (falls back to software per file)
    TitleRendererFactory titleRenderer;           // text engine for titles/captions (required if the project has any)
    std::filesystem::path frameMapCsv;   // optional per-frame source map for verification
    std::function<void(int64_t done, int64_t total)> progress;
};

struct RenderReport {
    int64_t framesWritten = 0;
    double seconds = 0.0;
    std::vector<std::string> warnings;
    int64_t seeks = 0;
    int64_t audioSamples = 0;
    std::string format;          // export format id used
    std::string videoEncoder;    // e.g. "libx264", "h264_nvenc"; empty for audio-only files
    bool hardwareEncoder = false;
    std::string encoderNote;     // what was tried and why it fell back to the CPU, if it did
    std::string audioEncoder;    // e.g. "aac"; empty without audio
    // M6 level check of the exported mix: largest |sample| and how many samples exceeded full scale (|x| > 1, clipped
    // by the encoder / player). A warning is added when any did.
    float audioPeak = 0.0f;
    int64_t audioClippedSamples = 0;
    std::string backend;   // compositing backend actually used
    // Where the time went (seconds): source decoding, compositing (incl. GPU upload/readback), encoding.
    double decodeSeconds = 0, compositeSeconds = 0, encodeSeconds = 0;
};

// Checks the project can be rendered (model validation, media present and long enough,
// supported features). Returns issues; errors make renderProject() refuse to start.
std::vector<ValidationIssue> preflight(const Project& project, const std::filesystem::path& projectFile,
                                       const RenderOptions& options, DecoderPool& pool);

RenderReport renderProject(const Project& project, const std::filesystem::path& projectFile,
                           const std::filesystem::path& output, const RenderOptions& options);

// Picture pixels -> picture pixels: the stabilisation correction (and zoom) of the frame shown at sourceTime for a
// w x h picture; identity when the stabilisation has no corrections (M5, fa/model/stabilize_plan.h).
Affine2D stabilizationTransform(const Stabilization& s, const Time& sourceTime, int w, int h);
// Placement of a clip's picture on a canvas at a source time (stabilisation + clip transform + transition adjust).
Affine2D clipPlacement(const Clip& clip, const Time& sourceTime, int srcW, int srcH, int canvasW, int canvasH,
                       const LayerAdjust& adjust = {});
// Full layer description of a clip at a source time (transform, crop, opacity, blend, masks, effect stack).
// Effect problems (unknown types, missing LUT files) are appended to `problems`.
// Relative LUT references resolve against `assetDir`.
LayerDesc clipLayer(const Clip& clip, const Time& sourceTime, const Image* image, int canvasW, int canvasH,
                    const LayerAdjust& adjust = {}, std::vector<std::string>* problems = nullptr,
                    const std::filesystem::path& assetDir = {});
// Adjustment layer of an adjustment clip at a source time: its effects, opacity and canvas-space masks (M4).
LayerDesc adjustmentLayer(const Clip& clip, const Time& sourceTime, const LayerAdjust& adjust = {},
                          std::vector<std::string>* problems = nullptr, const std::filesystem::path& assetDir = {});

// Title / caption layers (M3). `basePosition` is added to the clip's position (captions on subtitle
// tracks sit at the track's subtitle line). The picture's reference point lands on the position; the
// clip's anchor is relative to the text block.
Affine2D titlePlacement(const Clip& clip, const Time& sourceTime, const TitleFrame& frame, int canvasW, int canvasH,
                        const LayerAdjust& adjust = {}, Vec2 basePosition = {});
LayerDesc titleLayer(const Clip& clip, const Time& sourceTime, const TitleFrame& frame, int canvasW, int canvasH,
                     const LayerAdjust& adjust = {}, Vec2 basePosition = {});
// The whole-title animation part of a title frame as a layer adjustment (canvas fractions).
LayerAdjust titleAdjust(const TitleAnimState& anim, int canvasW, int canvasH);
// Style and base position of a caption clip on a subtitle track.
const TextStyle& captionStyle(const Track& track, const Clip& clip);
Vec2 captionBasePosition(const Track& track, const Clip& clip);
Vec2 captionBasePosition(double subtitleY, const TextStyle& style, const Clip& clip);

// Composites a transition between two still pictures at progress p (0..1) with default clip settings;
// used for the transition browser thumbnails (same evaluateTransition/clipLayer path as renderFrame).
const Image& renderTransitionPreview(const TransitionRef& transition, double progress, const Image& a, const Image& b,
                                     int width, int height, RenderBackend& backend);
// Composites a title clip (prototype: title, transform, opacity...) over a still picture at clip-local time
// `local` of a clip lasting `duration`; used for the title browser thumbnails (same titleLayer path as
// renderFrame). `basePosition` places captions on the subtitle line.
const Image& renderTitlePreview(const Clip& title, const Time& local, const Time& duration, const Image& background,
                                int width, int height, RenderBackend& backend, TitleRenderer& titles,
                                Vec2 basePosition = {});

// Composites one output frame. The preview uses the same function with a smaller canvas
// (canvasWidth/Height, 0 = sequence size), so preview and export evaluate identical semantics.
// Titles and captions need `titles` (the text engine); without it they are an error (RenderError).
// Subtitle tracks are composited above all video tracks.
const Image& renderFrame(const Sequence& seq, int64_t frame, const Project& project, FrameProvider& provider,
                         RenderBackend& backend, std::vector<TrackSample>* samplesOut = nullptr,
                         std::vector<FrameProvider::Result>* framesOut = nullptr, int canvasWidth = 0,
                         int canvasHeight = 0, TitleRenderer* titles = nullptr,
                         std::vector<std::string>* problems = nullptr);

}  // namespace fa
