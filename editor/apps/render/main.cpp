// fa-render: headless renderer.  fa-render <project.json> <output file> [options]
#include "fa/ai/interpolate.h"
#include "fa/ai/prepare.h"
#include "fa/media/media.h"
#include "fa/model/edit_ops.h"
#include "fa/model/subtitles.h"
#include "fa/model/serialize.h"
#include "fa/render/renderer.h"
#if defined(FA_HAS_TEXT)
#include "fa/text/title_engine.h"
#endif

#include <chrono>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

enum Exit { kOk = 0, kUsage = 1, kInvalidProject = 2, kRenderFailed = 3 };

void usage() {
    std::cerr <<
        R"(usage: fa-render <project.json> <output file> [options]

Renders a sequence of an fa-project file. The format follows the output's extension (.mp4 H.264 + AAC, .mov ProRes,
.mkv AV1, .webm VP9, .gif, .wav, .flac) unless --format is given; `fa-probe --encoders` lists what works here.

options:
  --format <id>         mp4-h264, mp4-hevc, mov-hevc, mp4-av1, mkv-av1, webm-vp9, mov-prores, gif, wav, flac
  --hw <h>              video encoder: cpu (default), auto, nvenc, qsv, amf (falls back to cpu, reported)
  --bit-depth <8|10>    video bit depth (HEVC, AV1, VP9; ProRes is always 10)
  --bitrate <kbit/s>    average video bit rate instead of constant quality
  --audio-codec <c>     aac, libopus, pcm_s16le, pcm_s24le, flac (default: the format's)
  --audio-bitrate <k>   lossy audio bit rate in kbit/s (default 192)
  --audio-rate <hz>     audio sample rate (default: the sequence's)
  --audio-channels <n>  2 stereo (default) or 1 mono
  --size <w>x<h>        output picture size (default: the sequence's; scaled with Lanczos)
  --fps <rate>          output frame rate, e.g. 24000/1001, 30000/1001, 60000/1001, 25 (default: the sequence's)
  --prores-profile <n>  0 Proxy, 1 LT, 2 422 (default), 3 HQ
  --creation-time <t>   container creation_time (ISO 8601 UTC; default: now)
  --sequence <id>       sequence to render (default: first)
  --frames <a>:<b>      render frames a..b inclusive (default: whole sequence)
  --crf <n>             x264 constant rate factor (default 18; 0 = lossless-ish high quality)
  --preset <name>       x264 preset (default medium)
  --codec-opt <k>=<v>   extra encoder option, repeatable (e.g. threads=8)
  --no-audio            video-only output
  --map <file.csv>      write the per-frame source map (for verification)
  --subtitles <file>    burn in an SRT / ASS / SSA file: its captions are added to the sequence on
                        subtitle tracks before rendering (repeatable; unsupported styling is listed)
  --subtitle-codepage <n>  Windows code page for subtitle files that are not UTF-8/UTF-16
                        (default 0 = the system code page, e.g. 1252 or 1256)
  --allow-unsupported   render projects that use effects/transitions this build cannot render
                        (they are skipped with warnings instead of failing)
  --backend <b>         compositing: cpu (default), gpu (Direct3D 11, falls back to cpu) or auto
  --list-gpus           print the graphics adapters and the one the GPU backend would use
  --check               validate the project and its media, do not render
  --quiet               no progress output
  --version             print version information

exit codes: 0 ok, 1 usage error, 2 project invalid / cannot be rendered, 3 render failed
)";
}

}  // namespace

int main(int argc, char** argv) {
    // M8: DLLs loaded from now on come only from this program's folder and System32 - never from the current
    // directory or PATH (every dependency ships next to the program; the rest are Windows' own)
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    std::vector<std::string> args(argv + 1, argv + argc);
    std::vector<std::string> positional;
    fa::RenderOptions opt;
#if defined(FA_HAS_TEXT)
    if (fa::text::ensureGuiApplication()) opt.titleRenderer = [] { return fa::text::makeTitleEngine(); };
#endif
    bool quiet = false, checkOnly = false;
    std::vector<std::string> subtitleFiles;
    unsigned subtitleCodePage = 0;

    try {
        for (size_t i = 0; i < args.size(); ++i) {
            const std::string& a = args[i];
            auto value = [&]() -> const std::string& {
                if (i + 1 >= args.size()) throw std::invalid_argument("missing value for " + a);
                return args[++i];
            };
            if (a == "--help" || a == "-h") {
                usage();
                return kOk;
            } else if (a == "--version") {
                std::cout << "fa-render 0.1.0 (M0)\n" << fa::ffmpegVersionInfo() << "\n";
                return kOk;
            } else if (a == "--sequence") {
                opt.sequenceId = value();
            } else if (a == "--frames") {
                const std::string& v = value();
                const auto colon = v.find(':');
                if (colon == std::string::npos) throw std::invalid_argument("--frames expects a:b");
                opt.firstFrame = std::stoll(v.substr(0, colon));
                opt.lastFrame = std::stoll(v.substr(colon + 1));
            } else if (a == "--crf") {
                opt.encoder.crf = std::stoi(value());
            } else if (a == "--preset") {
                opt.encoder.preset = value();
            } else if (a == "--codec-opt") {
                const std::string& v = value();
                const auto eq = v.find('=');
                if (eq == std::string::npos) throw std::invalid_argument("--codec-opt expects key=value");
                opt.encoder.codecOptions[v.substr(0, eq)] = v.substr(eq + 1);
            } else if (a == "--format") {
                opt.format = value();
            } else if (a == "--hw") {
                if (!fa::hardwareFromId(value(), opt.hardware)) throw std::invalid_argument("--hw expects cpu, auto, nvenc, qsv or amf");
            } else if (a == "--bit-depth") {
                opt.encoder.bitDepth = std::stoi(value());
            } else if (a == "--bitrate") {
                opt.encoder.bitrate = std::stoll(value()) * 1000;
            } else if (a == "--audio-codec") {
                opt.audioCodec = value();
            } else if (a == "--audio-rate") {
                opt.audioSampleRate = std::stoi(value());
            } else if (a == "--audio-channels") {
                opt.audioChannels = std::stoi(value());
            } else if (a == "--audio-bitrate") {
                opt.audioBitrate = std::stoi(value()) * 1000;
            } else if (a == "--size") {
                const std::string& v = value();
                const auto x = v.find('x');
                if (x == std::string::npos) throw std::invalid_argument("--size expects <w>x<h>");
                opt.outputWidth = std::stoi(v.substr(0, x));
                opt.outputHeight = std::stoi(v.substr(x + 1));
            } else if (a == "--fps") {
                opt.outputRate = fa::FrameRate(fa::Rational::parse(value()));
            } else if (a == "--prores-profile") {
                opt.encoder.proresProfile = std::stoi(value());
            } else if (a == "--creation-time") {
                opt.encoder.creationTime = value();
            } else if (a == "--no-audio") {
                opt.includeAudio = false;
            } else if (a == "--map") {
                opt.frameMapCsv = value();
            } else if (a == "--allow-unsupported") {
                opt.allowUnsupported = true;
            } else if (a == "--backend") {
                const std::string& v = value();
                if (v == "cpu") opt.backend = fa::BackendChoice::Cpu;
                else if (v == "gpu") opt.backend = fa::BackendChoice::Gpu;
                else if (v == "auto") opt.backend = fa::BackendChoice::Auto;
                else throw std::invalid_argument("--backend expects cpu, gpu or auto");
            } else if (a == "--list-gpus") {
                for (const auto& g : fa::listGpuAdapters())
                    std::cout << g.name << " (" << (g.dedicatedVideoMemory >> 20) << " MB)" << (g.software ? " [software]" : "")
                              << (g.highPerformanceChoice ? " [high performance: used by --backend gpu]" : "")
                              << (g.powerSavingChoice ? " [power saving]" : "") << "\n";
                std::string why;
                if (!fa::makeGpuBackend(&why)) std::cout << "GPU backend unavailable: " << why << "\n";
                return kOk;
            } else if (a == "--subtitles") {
                subtitleFiles.push_back(value());
            } else if (a == "--subtitle-codepage") {
                subtitleCodePage = static_cast<unsigned>(std::stoul(value()));
            } else if (a == "--check") {
                checkOnly = true;
            } else if (a == "--quiet") {
                quiet = true;
            } else if (!a.empty() && a[0] == '-') {
                throw std::invalid_argument("unknown option " + a);
            } else {
                positional.push_back(a);
            }
        }
        if (positional.size() != (checkOnly ? 1u : 2u)) throw std::invalid_argument("expected <project.json> <output file>");
    } catch (const std::exception& e) {
        std::cerr << "fa-render: " << e.what() << "\n\n";
        usage();
        return kUsage;
    }

    const std::filesystem::path projectFile = std::filesystem::absolute(positional[0]);
    fa::Project project;
    try {
        project = fa::loadProject(projectFile);
    } catch (const std::exception& e) {
        std::cerr << "fa-render: cannot load project: " << e.what() << "\n";
        return kInvalidProject;
    }

    for (const auto& file : subtitleFiles) {   // burn-in: captions on subtitle tracks of the rendered sequence
        try {
            std::ifstream in(std::filesystem::path(std::u8string(file.begin(), file.end())), std::ios::binary);
            if (!in) throw std::runtime_error("cannot read " + file);
            std::ostringstream bytes;
            bytes << in.rdbuf();
            const fa::SubtitleDoc doc = fa::parseSubtitles(bytes.str(), file, subtitleCodePage);
            if (project.sequences.empty()) throw std::runtime_error("the project has no sequence");
            std::string seqId = opt.sequenceId.empty() ? project.sequences.front().id : opt.sequenceId;
            auto cmd = fa::edit::addCaptions(project, seqId, doc.captions, doc.style, doc.subtitleY);
            cmd->apply(project);
            if (!quiet)
                std::cerr << "subtitles: " << doc.captions.size() << " captions from " << file << " (" << doc.encoding << ")\n";
            for (const auto& w : doc.warnings) std::cerr << "subtitles: not imported exactly: " << w << "\n";
        } catch (const std::exception& e) {
            std::cerr << "fa-render: subtitles " << file << ": " << e.what() << "\n";
            return kInvalidProject;
        }
    }

    if (checkOnly) {
        fa::DecoderPool pool(projectFile);
        const auto issues = fa::preflight(project, projectFile, opt, pool);
        std::cerr << fa::formatIssues(issues);
        const bool bad = fa::hasErrors(issues);
        std::cerr << (bad ? "project cannot be rendered\n" : "project OK\n");
        return bad ? kInvalidProject : kOk;
    }

    if (!quiet) {
        auto last = std::chrono::steady_clock::now();
        opt.progress = [last](int64_t done, int64_t total) mutable {
            const auto now = std::chrono::steady_clock::now();
            if (done == total || now - last > std::chrono::milliseconds(500)) {
                last = now;
                std::fprintf(stderr, "\rrendering frame %lld / %lld (%3.0f%%)", static_cast<long long>(done),
                             static_cast<long long>(total), 100.0 * done / total);
                if (done == total) std::fprintf(stderr, "\n");
            }
        };
    }

    const std::filesystem::path output = std::filesystem::absolute(positional[1]);
    try {
        // M7: every AI picture the export needs (mattes, AI in-between frames, upscaled frames) prepared first
        // (cached; the editor reuses them)
        fa::ai::registerModelIdentities();
        if (!quiet) {   // what the AI preparation will cost (the command line asked for it: it is not refused)
            const fa::ai::PreparationEstimate est = fa::ai::estimatePreparation(
                project, opt.sequenceId, projectFile, opt.onlyClips.empty() ? opt.firstFrame : 0,
                opt.onlyClips.empty() && opt.lastFrame >= 0 ? opt.lastFrame + 1 : -1);
            if (!est.text.empty()) std::cerr << "AI preparation: " << est.text << "\n";
        }
        const fa::ai::RenderPreparation prep = fa::ai::prepareForRender(
            project, opt.sequenceId, projectFile, opt.onlyClips.empty() ? opt.firstFrame : 0,
            opt.onlyClips.empty() && opt.lastFrame >= 0 ? opt.lastFrame + 1 : -1, [&](const char* stage, int64_t d, int64_t t) {
                if (!quiet && (d == t || d % 25 == 0))
                    std::fprintf(stderr, "\r%s: frame %lld / %lld", stage, static_cast<long long>(d), static_cast<long long>(t));
            });
        if (!quiet && !prep.summary.empty()) std::cerr << "\nAI: " << prep.summary << " in " << prep.seconds << " s\n";
        const fa::RenderReport r = fa::renderProject(project, projectFile, output, opt);
        for (const auto& w : r.warnings) std::cerr << "warning: " << w << "\n";
        if (!quiet)
            std::cerr << "wrote " << r.framesWritten << " frames to " << output.string() << " in " << r.seconds << " s ("
                      << (r.seconds > 0 ? r.framesWritten / r.seconds : 0.0) << " fps, " << r.seeks << " seeks; "
                      << r.backend << "; " << r.format << " " << (r.videoEncoder.empty() ? "no video" : r.videoEncoder)
                      << (r.hardwareEncoder ? " (hardware)" : "") << (r.audioEncoder.empty() ? "" : " + " + r.audioEncoder)
                      << ")\n";
        if (!quiet && !r.encoderNote.empty()) std::cerr << "encoder: " << r.encoderNote << "\n";
        return kOk;
    } catch (const fa::RenderError& e) {
        std::cerr << "\nfa-render: " << e.what() << "\n";
        return std::string(e.what()).rfind("project cannot be rendered", 0) == 0 ? kInvalidProject : kRenderFailed;
    } catch (const std::exception& e) {
        std::cerr << "\nfa-render: render failed: " << e.what() << "\n";
        return kRenderFailed;
    }
}
