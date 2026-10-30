// M7 automatic captions: speech to text (fa/ai/speech.h) turned into timeline captions (fa/model/subtitles.h) that
// the editor puts on a subtitle track as ordinary, editable captions (SRT / ASS export as for imported ones).
#pragma once

#include "fa/ai/speech.h"
#include "fa/model/project.h"
#include "fa/model/subtitles.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <vector>

namespace fa::ai {

struct CaptionResult {
    std::vector<Caption> captions;   // timeline times, sorted
    Transcript transcript;           // what the recogniser reported (segments in its own time base)
};

// What a clip says: its media's audio over the source range the clip uses, transcribed at normal speed, each caption
// placed where those words play on the timeline (through the clip's speed map: retimed, ramped and reversed clips stay
// in sync). A freeze frame has no speech. Throws ModelError / MediaError.
CaptionResult captionsForClip(const Project& project, const Clip& clip, const std::filesystem::path& projectFile,
                              const TranscribeOptions& options, const std::function<void(double)>& progress = {},
                              const std::atomic<bool>* cancel = nullptr);

// What the viewer hears in [from, to) of a sequence: the mixed audio (clip volumes, mutes, speed changes as played)
// transcribed; captions at timeline times.
CaptionResult captionsForRange(const Project& project, const Sequence& seq, Time from, Time to,
                               const std::filesystem::path& projectFile, const TranscribeOptions& options,
                               const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

}  // namespace fa::ai
