// fa-probe: print what the engine sees in a media file (JSON).  fa-probe <file> [--frames N]
//          fa-probe --encoders: the export formats and the encoders this build and this machine can use (M6); every
//          encoder is checked with a real session (hardware encoders are never assumed from the GPU model).
//          fa-probe --ai [--verify]: the AI devices / providers (M7) and the model catalogue with each model's
//          status in the model folder (--verify also checks every file's SHA-256).
#include "fa/ai/inference.h"
#include "fa/ai/models.h"
#include "fa/media/export_format.h"
#include "fa/media/media.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

int printEncoders() {
    nlohmann::json j;
    j["ffmpeg"] = fa::ffmpegVersionInfo();
    nlohmann::json enc = nlohmann::json::array();
    for (const auto& p : fa::probeEncoders()) {
        nlohmann::json e = {{"encoder", p.encoder}, {"compiled", p.compiled}, {"works8bit", p.works8}};
        for (const auto& s : fa::videoEncoderSpecs())
            if (s.encoder == p.encoder) {
                e["codec"] = s.codec;
                e["family"] = fa::hardwareId(s.family);
                e["library"] = s.library;
                if (std::find(s.bitDepths.begin(), s.bitDepths.end(), 10) != s.bitDepths.end()) e["works10bit"] = p.works10;
            }
        if (!p.detail.empty()) e["detail"] = p.detail;
        enc.push_back(e);
    }
    j["encoders"] = enc;
    nlohmann::json formats = nlohmann::json::array();
    for (const auto& f : fa::exportFormats()) {
        nlohmann::json x = {{"id", f.id},       {"label", f.label},           {"extension", f.extension},
                            {"muxer", f.muxer}, {"videoCodec", f.videoCodec}, {"audioCodecs", f.audioCodecs},
                            {"bitDepths", f.bitDepths}};
        if (!f.videoCodec.empty()) {
            try {
                x["cpuEncoder"] = fa::chooseVideoEncoder(f.videoCodec, fa::HardwareEncoding::Cpu, f.bitDepths.front(), 1920,
                                                         1080, fa::FrameRate::fps30()).encoder;
                const auto a = fa::chooseVideoEncoder(f.videoCodec, fa::HardwareEncoding::Auto, f.bitDepths.front(), 1920, 1080,
                                                      fa::FrameRate::fps30());
                x["autoEncoder1080p30"] = a.encoder;
                x["autoNote"] = a.note;
            } catch (const std::exception& e) {
                x["error"] = e.what();
            }
        }
        formats.push_back(x);
    }
    j["formats"] = formats;
    std::cout << j.dump(2) << "\n";
    return 0;
}

int printAi(bool verify) {
    nlohmann::json j;
    const fa::ai::AiCapabilities& c = fa::ai::aiCapabilities();
    j["cpu"] = {{"name", c.cpuName}, {"threads", c.cpuThreads}};
    j["onnxRuntime"] = {{"version", c.onnxRuntimeVersion}, {"providers", c.onnxProviders}};
    j["directMl"] = {{"available", c.directMl}, {"windowsDirectMlDll", c.directMlVersion}};
    j["vulkan"] = c.vulkan;
    nlohmann::json gpus = nlohmann::json::array();
    for (const auto& g : c.gpus)
        gpus.push_back({{"name", g.name}, {"discrete", g.discrete}, {"memoryMb", g.memoryMb}, {"directMlDevice", g.dxgiIndex},
                        {"vulkanDevice", g.vulkanIndex}});
    j["gpus"] = gpus;
    j["notes"] = c.notes;
    j["modelFolder"] = fa::ai::modelsDirectory().u8string();
    nlohmann::json models = nlohmann::json::array();
    for (const auto& m : fa::ai::modelCatalog())
        models.push_back({{"id", m.id}, {"name", m.name}, {"runtime", fa::ai::runtimeName(m.runtime)},
                          {"version", m.version}, {"downloadBytes", m.downloadBytes()}, {"codeLicence", m.codeLicence},
                          {"weightsLicence", m.weightsLicence}, {"source", m.source},
                          {"status", fa::ai::statusName(fa::ai::modelStatus(m, verify))}});
    j["models"] = models;
    std::cout << j.dump(2) << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // M8: DLLs loaded from now on come only from this program's folder and System32 - never from the current
    // directory or PATH (every dependency ships next to the program; the rest are Windows' own)
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (argc < 2) {
        std::cerr << "usage: fa-probe <media file> [--frames N]   (N: also decode the first N frames)\n"
                     "       fa-probe --encoders                (export formats and the encoders that work here)\n"
                     "       fa-probe --ai [--verify]           (AI devices and the installed models)\n";
        return 1;
    }
    try {
        if (std::string(argv[1]) == "--encoders") return printEncoders();
        if (std::string(argv[1]) == "--ai") return printAi(argc > 2 && std::string(argv[2]) == "--verify");
        const fa::MediaInfo mi = fa::probeMedia(argv[1]);
        nlohmann::json j;
        j["path"] = mi.path.string();
        j["container"] = mi.container;
        if (mi.duration) j["duration"] = {{"exact", mi.duration->toString()}, {"seconds", mi.duration->toDouble()}};
        if (mi.video) {
            const auto& v = *mi.video;
            j["video"] = {{"index", v.index},       {"codec", v.codec},           {"width", v.width},
                          {"height", v.height},     {"pixelFormat", v.pixelFormat}, {"frameRate", v.frameRate.toString()},
                          {"timeBase", v.timeBase.toString()}, {"startTime", v.startTime.toString()},
                          {"colorSpace", v.colorSpace}, {"colorRange", v.colorRange}};
            if (v.duration) j["video"]["duration"] = v.duration->toString();
            if (v.frameCount) j["video"]["frameCount"] = *v.frameCount;
        }
        if (mi.audio)
            j["audio"] = {{"index", mi.audio->index}, {"codec", mi.audio->codec}, {"sampleRate", mi.audio->sampleRate},
                          {"channels", mi.audio->channels}};
        if (argc >= 4 && std::string(argv[2]) == "--frames" && mi.video) {
            const int n = std::stoi(argv[3]);
            fa::VideoDecoder dec(argv[1]);
            nlohmann::json frames = nlohmann::json::array();
            for (int i = 0; i < n; ++i) {
                const auto& f = dec.frameAt(fa::frameToTime(i, mi.video->frameRate));
                const auto c = f.image.at(f.image.width / 2, f.image.height / 2);
                frames.push_back({{"request", i}, {"index", f.index}, {"time", f.time.toString()},
                                  {"centre", {c.r, c.g, c.b}}});
            }
            j["frames"] = frames;
        }
        std::cout << j.dump(2) << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fa-probe: " << e.what() << "\n";
        return 2;
    }
}
