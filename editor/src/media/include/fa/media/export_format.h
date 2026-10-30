// Export formats, their codecs, and which encoders this build and this machine can actually use (M6 D).
//
// The formats are fixed by MeerCut; whether a format can be exported is decided at run time: an encoder must be
// compiled into the deployed FFmpeg AND open a real session (a few frames are encoded) - hardware encoders (NVENC,
// Quick Sync, AMF) are never assumed from the GPU model, and AV1 hardware encoding is used only when such a session
// succeeds. A request for hardware falls back to the CPU encoder with the reason reported.
#pragma once

#include "fa/media/media.h"

#include <string>
#include <vector>

namespace fa {

enum class HardwareEncoding { Auto, Cpu, Nvenc, Qsv, Amf };
const char* hardwareLabel(HardwareEncoding h);   // "Auto", "CPU (software)", "NVIDIA NVENC", "Intel Quick Sync", "AMD AMF"
const char* hardwareId(HardwareEncoding h);      // "auto", "cpu", "nvenc", "qsv", "amf"
bool hardwareFromId(const std::string& id, HardwareEncoding& out);

// A format of the export window.
struct ExportFormat {
    std::string id;          // stable id: "mp4-h264", "mp4-hevc", "mov-hevc", "mp4-av1", "mkv-av1", "webm-vp9",
                             // "mov-prores", "gif", "wav", "flac"
    std::string label;       // UI name
    std::string extension;   // ".mp4" ...
    std::string muxer;       // FFmpeg muxer: "mp4", "mov", "matroska", "webm", "gif", "wav", "flac"
    std::string videoCodec;  // "h264", "hevc", "av1", "vp9", "prores", "gif"; empty = audio only
    std::vector<std::string> audioCodecs;   // allowed audio codecs (FFmpeg encoder names), first = default; empty = none
    std::vector<int> bitDepths;              // video bit depths offered (8, 10)
};
const std::vector<ExportFormat>& exportFormats();
const ExportFormat* findExportFormat(const std::string& id);
// The format a file name implies (".mp4" -> "mp4-h264", ".mov" -> "mov-prores", ".mkv" -> "mkv-av1", ...), or null.
const ExportFormat* exportFormatForPath(const std::string& path);

// One video encoder implementation of a codec.
struct VideoEncoderSpec {
    std::string encoder;          // FFmpeg encoder name, e.g. "libx264", "h264_nvenc", "hevc_qsv", "libsvtav1"
    std::string codec;            // "h264" ...
    HardwareEncoding family;      // Cpu, Nvenc, Qsv or Amf
    std::vector<int> bitDepths;   // what MeerCut asks of it
    std::string library;          // implementation / licence note for the compatibility matrix
};
const std::vector<VideoEncoderSpec>& videoEncoderSpecs();   // CPU encoders first, then NVENC, QSV, AMF per codec

// What a test session found (cached for the process; see probeEncoders).
struct EncoderProbe {
    std::string encoder;
    bool compiled = false;    // present in the deployed FFmpeg
    bool works8 = false;      // a 8-bit session opened and encoded frames here
    bool works10 = false;     // a 10-bit session (where MeerCut asks for one)
    std::string detail;       // why not, or the pixel formats used
};
// Probes every known video encoder (and the audio encoders) once per process: compiled-in check plus a real
// session on a 256x144 picture for each bit depth. Thread-safe; the first call can take a second or two.
const std::vector<EncoderProbe>& probeEncoders();
const EncoderProbe* encoderProbe(const std::string& encoder);
bool audioEncoderAvailable(const std::string& encoder);   // compiled in (aac, libopus, pcm_*, flac)
// One encoder at one bit depth: compiled in and its 256x144 test session works (cached; no full probe).
bool encoderWorks(const std::string& encoder, int bitDepth);
// The format's bit depths that some encoder allowed by the preference can actually make here (e.g. this build's x265
// and libvpx are 8-bit only, so 10-bit HEVC / VP9 need a hardware encoder).
std::vector<int> availableBitDepths(const ExportFormat& format, HardwareEncoding preference);

// The encoder an export uses: Cpu -> the CPU encoder; a family -> its encoder if a session at the export's size and
// bit depth opens, else the CPU encoder; Auto -> NVENC, then Quick Sync, then AMF, then CPU. `note` says what was
// tried and why it fell back. Throws MediaError when not even the CPU encoder can be used.
struct EncoderChoice {
    std::string encoder;
    bool hardware = false;
    std::string note;
};
EncoderChoice chooseVideoEncoder(const std::string& codec, HardwareEncoding preference, int bitDepth, int width,
                                 int height, const FrameRate& rate);

// Native constant-quality value of an encoder for a quality level 0 (smallest file) .. 4 (best): the mapping the
// export window's High / Standard / Low presets use (x264 / x265 CRF, SVT-AV1 / VP9 CRF, NVENC CQ, QSV ICQ; VP9 on
// Quick Sync ignores ICQ, so the encoder turns the value into a VBR target bit rate, see video_encoder.cpp).
int nativeQuality(const std::string& encoder, int level);

}  // namespace fa
