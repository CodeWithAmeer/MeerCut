// M7 Speech to Text with whisper.cpp (offline, CPU or Vulkan GPU): transcribes audio into timed segments that become
// editable captions. Long recordings are processed in chunks of a few minutes cut at quiet moments, so memory stays
// bounded; segments over silence are dropped and every segment's edges are snapped to where its words are audible
// (whisper places them loosely, often in the pause before). Speaker identification and translation are not provided;
// no word-level timing is offered (captions are timed per phrase).
#pragma once

#include "fa/ai/inference.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace fa::ai {

struct SpeechSegment {
    double start = 0, end = 0;   // seconds from the start of the transcribed audio
    std::string text;            // UTF-8, trimmed
    double noSpeech = 0;         // whisper's no-speech probability of the window
};

struct TranscribeOptions {
    std::string model = "whisper-base";   // a whisper model id of the catalogue
    std::string language = "auto";        // ISO 639-1 code ("en", "es", "ar", ...) or "auto" (detected)
    Accelerator accelerator = Accelerator::Auto;
    int maxCaptionChars = 84;             // segments split at word boundaries to at most this (two caption lines)
    double chunkSeconds = 300;            // long audio is transcribed in chunks of about this length
    // Silence: a segment is dropped unless its loudest 20 ms is at least 3x the recording's noise floor (10th
    // percentile of its frame levels) and at least this absolute RMS (-80 dBFS: digital silence and near-silence).
    double silenceRms = 0.0001;
};

struct Transcript {
    std::vector<SpeechSegment> segments;
    std::string language;        // detected (or the one asked for)
    double audioSeconds = 0;
    double seconds = 0;          // processing time
    std::string provider;        // "Vulkan: NVIDIA GeForce RTX 2050" / "CPU (n threads)"
    std::string fallback;        // why the GPU was not used, if it was asked for
    bool cancelled = false;      // the segments so far are kept
};

// Transcribes 16 kHz mono samples. progress(0..1); cancel stops between chunks / decoder windows.
Transcript transcribe(const std::vector<float>& mono16k, const TranscribeOptions& options,
                      const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr);
// Decodes [from, to) seconds of a media file's audio at 16 kHz mono and transcribes it (chunked; times relative to
// `from`). Throws MediaError / ModelError.
Transcript transcribeFile(const std::filesystem::path& media, double from, double to, const TranscribeOptions& options,
                          const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

// The languages whisper knows: {code, English name}.
std::vector<std::pair<std::string, std::string>> speechLanguages();

// Accuracy measures for the tests and benchmarks: text normalised (lower case, punctuation removed, whitespace
// collapsed; letters of any script kept), then the edit distance over words (WER) or characters without spaces (CER).
std::string normaliseForScoring(const std::string& utf8);
double wordErrorRate(const std::string& reference, const std::string& hypothesis);
double charErrorRate(const std::string& reference, const std::string& hypothesis);

}  // namespace fa::ai
