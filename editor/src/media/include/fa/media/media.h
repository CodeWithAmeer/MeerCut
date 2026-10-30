// FFmpeg-backed media I/O: probing, frame-accurate video decoding, encoding (export formats: export_format.h).
//
// Public headers expose no FFmpeg types; all libav* objects are owned by RAII wrappers inside
// the implementation. Errors throw MediaError with the file, operation and FFmpeg message.
#pragma once

#include "fa/core/image.h"
#include "fa/core/time.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fa {

class MediaError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// FFmpeg library versions linked into this build, e.g. "libavcodec 62.x.y".
std::string ffmpegVersionInfo();
// Route FFmpeg's own log output (default: errors only).
void setFfmpegLogLevel(int avLogLevel);

// ---- probing ---------------------------------------------------------------------------------

struct VideoStreamInfo {
    int index = -1;
    std::string codec;
    int width = 0;
    int height = 0;
    std::string pixelFormat;
    FrameRate frameRate;           // avg_frame_rate (falls back to r_frame_rate)
    Rational timeBase{1, 1};
    Time startTime;                // stream start in seconds (container timeline)
    std::optional<Time> duration;
    std::optional<int64_t> frameCount;   // nb_frames when the container states it
    std::string colorSpace;        // e.g. "bt709", "unknown"
    std::string colorRange;        // "tv", "pc", "unknown"
    std::string transfer;          // "bt709", "smpte2084" (HDR PQ), "arib-std-b67" (HDR HLG), "unknown" ...
    std::string primaries;         // "bt709", "bt2020", "unknown" ...
    bool isHdr() const { return transfer == "smpte2084" || transfer == "arib-std-b67"; }   // converted to SDR (hdr.h)
};

struct AudioStreamInfo {
    int index = -1;
    std::string codec;
    int sampleRate = 0;
    int channels = 0;
    Time startTime;                // stream start in seconds (container timeline)
    std::optional<Time> duration;
};

struct MediaInfo {
    std::filesystem::path path;
    std::string container;
    std::optional<Time> duration;
    std::optional<VideoStreamInfo> video;   // best video stream
    std::optional<AudioStreamInfo> audio;   // best audio stream
};

MediaInfo probeMedia(const std::filesystem::path& file);

// ---- decoding --------------------------------------------------------------------------------

struct VideoFrame {
    Time time;            // presentation time relative to the stream start (source time)
    int64_t index = 0;    // round(time * frameRate)
    Image image;          // RGBA8, source resolution
};

// Decodes the video stream of one file. frameAt(t) returns the frame that is visible at source
// time t (the last frame whose presentation time is <= t). Sequential requests decode forward;
// backward or far-forward requests seek to the preceding keyframe and decode up to t.
class VideoDecoder {
public:
    struct Options {
        int threads = 0;          // 0 = FFmpeg auto
        int maxWidth = 0;         // >0: deliver frames scaled down to fit maxWidth x maxHeight (preview);
        int maxHeight = 0;        //     aspect ratio kept, never upscaled, even dimensions
        // Decode on the GPU (FFmpeg D3D11VA, Windows) when the codec/driver supports it; frames are
        // copied to system memory only when delivered. Falls back to software decoding automatically.
        bool hardware = false;
        int convertThreads = 0;   // YUV -> RGBA conversion threads (libswscale slices), 0 = automatic
    };

    explicit VideoDecoder(const std::filesystem::path& file);
    VideoDecoder(const std::filesystem::path& file, Options options);
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    const VideoStreamInfo& info() const;
    const std::filesystem::path& path() const;

    const VideoFrame& frameAt(const Time& sourceTime);

    // Statistics for diagnostics/tests.
    int64_t seekCount() const;
    int64_t decodedFrameCount() const;
    // True when frames are being decoded by the GPU (D3D11VA).
    bool hardwareActive() const;
    // "software", "D3D11VA on <adapter>", or why hardware decoding is not used.
    std::string decoderDescription() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Decodes the best audio stream of a file to interleaved float32 at a fixed output rate and channel
// count. Output sample k is source time k / sampleRate, where source time 0 is `origin` on the
// container timeline (default: the video stream's start when the file has video, else the audio
// stream's start) — so audio stays aligned with the file's video frames.
// Mono is copied to every output channel at unity gain; other layouts are converted by libswresample.
class AudioDecoder {
public:
    AudioDecoder(const std::filesystem::path& file, int sampleRate = 48000, int channels = 2,
                 std::optional<Time> origin = std::nullopt);
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    const AudioStreamInfo& info() const;
    int sampleRate() const;
    int channels() const;
    // Exact number of output samples available, if the stream states its duration.
    std::optional<int64_t> lengthSamples() const;

    // Write `count` frames (count * channels floats) starting at output sample `start`.
    // Samples before the start or after the end of the stream are silence.
    void read(int64_t start, int count, float* out);

    int64_t seekCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- encoding --------------------------------------------------------------------------------

struct EncoderSettings {
    int width = 1920;                    // output picture size; frames of another size are scaled (Lanczos)
    int height = 1080;
    FrameRate frameRate = FrameRate::fps30();
    std::string codec = "libx264";       // FFmpeg video encoder (see export_format.h); empty = audio-only file
    std::string muxer;                   // FFmpeg muxer ("mp4", "mov", "matroska", "webm", "gif", "wav", "flac");
                                         // empty = from the file extension
    std::string preset = "medium";       // x264 speed names (ultrafast .. veryslow), mapped per encoder
    int crf = 18;                        // constant quality in the encoder's own scale (export_format.h nativeQuality)
    int64_t bitrate = 0;                 // average video bit rate (bit/s); > 0 replaces constant quality
    int64_t maxBitrate = 0;              // peak video bit rate (bit/s) with a 2 s buffer; 0 = none
    int bitDepth = 8;                    // 8 or 10 (4:2:0); ProRes is always 10-bit 4:2:2
    int proresProfile = 2;               // prores_ks: 0 Proxy, 1 LT, 2 422, 3 HQ
    std::vector<uint32_t> palette;       // "gif": 256 colours 0xRRGGBB (buildGifPalette); required
    bool dither = true;                  // "gif": ordered (Bayer 8x8) dithering
    int gopSize = 0;                     // 0 = encoder default
    std::map<std::string, std::string> codecOptions;   // extra AVOptions for the codec
    // Audio. When enabled, feed interleaved float samples with writeAudio().
    bool audio = false;
    std::string audioCodec = "aac";      // "aac", "libopus", "pcm_s16le", "pcm_s24le", "flac"
    int audioSampleRate = 48000;
    int audioChannels = 2;
    int audioBitrate = 192000;           // lossy codecs
    int audioBits = 16;                  // "flac": 16 or 24
    // Container metadata.
    std::string creationTime;            // ISO 8601 UTC, e.g. "2026-10-21T10:00:00.000000Z"; empty = now
    std::string title;                   // optional title tag
};

// GIF palette: 256 colours (0xRRGGBB) by median cut over the given frames (a sample of the whole export), so one
// palette serves every frame (no flicker between frames). Deterministic.
std::vector<uint32_t> buildGifPalette(const std::vector<const Image*>& frames, int colors = 256);

// Writes RGBA frames with a video encoder (H.264, HEVC, AV1, VP9, ProRes, GIF; BT.709 limited range, tagged,
// chroma sited left) and optionally audio (AAC, Opus, PCM, FLAC) into a container (MP4, MOV, MKV, WebM, GIF, WAV,
// FLAC), or audio alone. Frame n gets pts n in a 1/frameRate time base (exact CFR; MP4 / MOV tracks use a
// 1000 x fps time scale like Filmora's exports); audio sample k gets pts k in a 1/sampleRate time base, so video frame
// n and audio sample k are in sync when k = n * rate / fps. Callers should interleave: after frame n, write the audio
// up to the start of frame n + 1. MP4 files get the mp42 brand and creation_time tags (EXP-001 parity).
class VideoEncoder {
public:
    VideoEncoder(const std::filesystem::path& file, const EncoderSettings& settings);
    ~VideoEncoder();   // aborts (deletes partial file) if finish() was not called
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    void writeFrame(const Image& rgba);   // scaled to the output size when it differs; not for audio-only files
    void writeAudio(const float* interleaved, int64_t frames);   // requires settings.audio
    void finish();                         // flush encoders, write trailer
    int64_t framesWritten() const;
    int64_t audioSamplesWritten() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fa
