// fa-ai-bench: M7 local AI benchmarks (time per frame, device, memory) on real media.
//   fa-ai-bench matte <model> <media> <seconds-from> <frames> <cpu|gpu> [quality 0..2]   matting time per frame
//                  (model: rvm-mobilenetv3 or birefnet-general-tiny)
//   fa-ai-bench speech <media> <from-s> <to-s> <model> <cpu|gpu> [language]   Speech to Text, real-time factor
//   fa-ai-bench wer <model> <cpu|gpu> <testmedia/ai>   WER / CER on LibriSpeech and FLEURS (es, fr, ar)
//   fa-ai-bench rife <media> <seconds-from> <pairs> <cpu|gpu> [max-width]   RIFE in-between frame (t = 0.5) per pair
//   fa-ai-bench upscale <general|anime> <scale> <media> <seconds-from> <frames> <cpu|gpu> [max-width]   Real-ESRGAN
//   fa-ai-bench example <kind> <testmedia/ai> <out.json>   writes an M7 example project: background (Background Removal
//                  over another video), subtitles (Speech to Text captions), scenes (Scene Detection splits), reframe
//                  (Auto Reframe to 9:16), slowmotion (blending / optical flow / RIFE), upscaling (Lanczos / Real-ESRGAN)
//   fa-ai-bench devices                                                         what fa_ai finds on this PC
// Peak working set (RAM) is printed for every run; VRAM is reported by the device budget where the runtime gives it.
#include "fa/ai/inference.h"
#include "fa/ai/captions.h"
#include "fa/ai/interpolate.h"
#include "fa/ai/reframe.h"
#include "fa/model/edit_ops.h"
#include "fa/model/serialize.h"
#include "fa/render/effects.h"
#include "fa/render/renderer.h"
#include "fa/vision/scene_detect.h"
#include "fa/ai/upscale.h"
#include "fa/ai/matting.h"
#include "fa/ai/models.h"
#include "fa/ai/speech.h"
#include "fa/media/media.h"

#include <nlohmann/json.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

using namespace fa;

namespace {

double peakMb() {
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.PeakWorkingSetSize / 1048576.0;
}

// Graphics memory this process uses on each adapter now (DXGI; Vulkan and DirectML allocations both count).
std::string vram() {
    std::string out;
    IDXGIFactory4* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&f)))) return "n/a";
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        IDXGIAdapter3* a3 = nullptr;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(a->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
            a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
            a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%sadapter %u: %.0f MB local + %.0f MB shared", out.empty() ? "" : "; ", i,
                          local.CurrentUsage / 1048576.0, shared.CurrentUsage / 1048576.0);
            out += buf;
            a3->Release();
        }
        a->Release();
    }
    f->Release();
    return out;
}

// The largest local graphics memory this process used on any adapter while it lived (sampled every 50 ms).
struct VramPeak {
    std::atomic<bool> stop{false};
    double peakMb = 0;
    std::thread t;
    VramPeak() {
        t = std::thread([this] {
            while (!stop) {
                IDXGIFactory4* f = nullptr;
                if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&f)))) {
                    IDXGIAdapter1* a = nullptr;
                    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
                        IDXGIAdapter3* a3 = nullptr;
                        if (SUCCEEDED(a->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3)))) {
                            DXGI_QUERY_VIDEO_MEMORY_INFO local{};
                            a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
                            peakMb = std::max(peakMb, local.CurrentUsage / 1048576.0);
                            a3->Release();
                        }
                        a->Release();
                    }
                    f->Release();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }
    ~VramPeak() {
        stop = true;
        t.join();
    }
};

double since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// Frames of a file from `from` seconds, decoded at most maxWidth wide (0 = the source size).
std::vector<Image> decodeFrames(const std::filesystem::path& media, double from, int count, int maxWidth) {
    VideoDecoder::Options o;
    o.maxWidth = maxWidth;
    o.maxHeight = maxWidth;
    VideoDecoder dec(media, o);
    const Rational fps = dec.info().frameRate.fps();
    const int64_t first = static_cast<int64_t>(from * fps.toDouble() + 0.5);
    std::vector<Image> out;
    for (int i = 0; i < count; ++i) out.push_back(dec.frameAt(Time(first + i) / fps).image);
    return out;
}

int rife(const std::filesystem::path& media, double from, int pairs, ai::Accelerator acc, int maxWidth) {
    const auto f = decodeFrames(media, from, pairs + 1, maxWidth);
    VramPeak vp;
    const auto t0 = std::chrono::steady_clock::now();
    ai::NeuralInterpolator net(acc);
    const double load = since(t0);
    std::vector<double> ms;
    for (int i = 0; i < pairs; ++i) {
        const auto t = std::chrono::steady_clock::now();
        net.interpolate(f[static_cast<size_t>(i)], f[static_cast<size_t>(i) + 1], 0.5);
        ms.push_back(since(t));
    }
    double warm = 0;
    for (size_t i = 1; i < ms.size(); ++i) warm += ms[i];
    if (ms.size() > 1) warm /= double(ms.size() - 1);
    std::printf("RIFE 4.6 %d x %d on %s: model load %.0f ms, first frame %.0f ms, warm %.1f ms/frame (%zu frames); peak RAM "
                "%.0f MB, peak local VRAM %.0f MB\n",
                f[0].width, f[0].height, net.provider().c_str(), load, ms.front(), warm, ms.size() - 1, peakMb(), vp.peakMb);
    return 0;
}

int upscale(const std::string& model, int scale, const std::filesystem::path& media, double from, int frames, ai::Accelerator acc,
            int maxWidth) {
    const auto f = decodeFrames(media, from, frames, maxWidth);
    VramPeak vp;
    const auto t0 = std::chrono::steady_clock::now();
    ai::Upscaler up(model, scale, acc);
    const double load = since(t0);
    std::vector<double> ms;
    Image last;
    for (const Image& im : f) {
        const auto t = std::chrono::steady_clock::now();
        last = up.upscale(im);
        ms.push_back(since(t));
    }
    double warm = 0;
    for (size_t i = 1; i < ms.size(); ++i) warm += ms[i];
    if (ms.size() > 1) warm /= double(ms.size() - 1);
    std::printf("Real-ESRGAN %s %dx %d x %d -> %d x %d on %s (tile %d): model load %.0f ms, first frame %.0f ms, warm %.0f "
                "ms/frame (%zu frames); peak RAM %.0f MB, peak local VRAM %.0f MB\n",
                model.c_str(), scale, f[0].width, f[0].height, last.width, last.height, up.provider().c_str(), up.tile(), load,
                ms.front(), warm, ms.size() - 1, peakMb(), vp.peakMb);
    return 0;
}

// A project with one clip of `file` (video and its sound on two tracks) on a w x h sequence; media paths relative to
// the project's folder, so the example opens on any checkout with the test media.
Project exampleProject(const std::filesystem::path& file, const std::filesystem::path& projectFile, Time in, Time length, int w, int h) {
    const MediaInfo info = probeMedia(file);
    Project p;
    MediaSource m;
    m.id = "m1";
    const std::u8string rel = std::filesystem::relative(file, projectFile.parent_path()).generic_u8string();
    m.path = std::string(rel.begin(), rel.end());
    m.duration = info.duration;
    m.frameRate = info.video->frameRate;
    m.width = info.video->width;
    m.height = info.video->height;
    m.hasAudio = info.audio.has_value();
    p.sources.push_back(m);
    Sequence q;
    q.id = "seq1";
    q.name = "Sequence 1";
    q.width = w;
    q.height = h;
    q.frameRate = FrameRate(24);
    Track v{"v1", TrackKind::Video, "Video 1"}, a{"a1", TrackKind::Audio, "Audio 1"};
    Clip c;
    c.id = "c1";
    c.sourceId = "m1";
    c.sourceIn = in;
    c.duration = length;
    v.clips.push_back(c);
    q.tracks.push_back(v);
    if (m.hasAudio) {
        c.id = "c1a";
        a.clips.push_back(c);
        q.tracks.push_back(a);
    }
    p.sequences.push_back(q);
    return p;
}

// A title clip (white, bold) on a new top track, `y` from -0.5 (top) to 0.5 (bottom), `x` likewise.
void addLabel(Project& p, const std::string& text, Time start, Time length, double x, double y, int size = 28) {
    Sequence& q = p.sequences[0];
    Track t{"lbl" + std::to_string(q.tracks.size()), TrackKind::Video, "Labels"};
    Clip c;
    c.id = "label" + std::to_string(q.tracks.size());
    c.timelineStart = start;
    c.duration = length;
    TitleData d;
    d.text = text;
    d.style.size = size;
    d.style.weight = 700;
    d.style.wrapWidth = 0.9;   // long labels wrap instead of leaving the picture
    c.title = d;
    c.transform.positionX.setConstant(x);
    c.transform.positionY.setConstant(y);
    t.clips.push_back(c);
    q.tracks.push_back(t);
}

// A source of another file in an example project (path relative to the project).
std::string addSource(Project& p, const std::filesystem::path& file, const std::filesystem::path& projectFile) {
    const MediaInfo info = probeMedia(file);
    MediaSource m;
    m.id = "m" + std::to_string(p.sources.size() + 1);
    const std::u8string rel = std::filesystem::relative(file, projectFile.parent_path()).generic_u8string();
    m.path = std::string(rel.begin(), rel.end());
    m.duration = info.duration;
    m.frameRate = info.video->frameRate;
    m.width = info.video->width;
    m.height = info.video->height;
    m.hasAudio = info.audio.has_value();
    p.sources.push_back(m);
    return m.id;
}

int example(const std::string& kind, const std::filesystem::path& media, const std::filesystem::path& out) {
    const auto t0 = std::chrono::steady_clock::now();
    Project p;
    if (kind == "background") {   // two people of Tears of Steel cut out (RVM) over the Sintel trailer's desert
        p = exampleProject(media / "tears_of_steel_60s.mov", out, Time(10), Time(4), 1280, 720);
        Sequence& q = p.sequences[0];
        Clip& people = q.tracks[0].clips[0];
        people.aiCutout.enabled = true;
        people.aiCutout.quality = 1;
        const std::string desert = addSource(p, media / "sintel_trailer_720p.mp4", out);
        Track below{"v0", TrackKind::Video, "Background"};
        Clip bg;
        bg.id = "bg";
        bg.sourceId = desert;
        bg.sourceIn = Time(75, 2);
        bg.duration = Time(4);
        below.clips.push_back(bg);
        q.tracks.insert(q.tracks.begin(), below);   // tracks are listed bottom first
        addLabel(p, "Background Removal (AI: Robust Video Matting)", Time(0), Time(4), 0, -0.43, 30);
    } else if (kind == "slowmotion") {   // the trailer's city shot at 0.25x three times: blending, optical flow, RIFE
        p = exampleProject(media / "sintel_trailer_720p.mp4", out, Time(590, 24), Time(2), 1280, 720);
        Sequence& q = p.sequences[0];
        q.tracks.resize(1);   // no sound: it would be slowed three times over
        Clip base = q.tracks[0].clips[0];
        q.tracks[0].clips.clear();
        const FrameInterpolation modes[3] = {FrameInterpolation::Blending, FrameInterpolation::OpticalFlowQuality,
                                             FrameInterpolation::Neural};
        const char* names[3] = {"0.25x - Frame Blending (M5, not AI)", "0.25x - Optical Flow (M5, not AI)",
                                "0.25x - AI Interpolation (RIFE, M7)"};
        for (int i = 0; i < 3; ++i) {
            Clip c = base;
            c.id = "slow" + std::to_string(i);
            c.timelineStart = Time(2 * i);
            c.speed.speed = Rational(1, 4);
            c.speed.interpolation = modes[i];
            c.duration = Time(2);   // 12 source frames at 0.25x
            q.tracks[0].clips.push_back(c);
            addLabel(p, names[i], Time(2 * i), Time(2), 0, -0.43, 30);
        }
    } else if (kind == "upscaling") {   // a 320 x 180 copy of the desert shot on a 1280 x 720 canvas: Lanczos on the
                                        // left, Real-ESRGAN (general model) on the right
        const auto low = media / "sintel_desert_320x180.mp4";
        if (!std::filesystem::exists(low)) {   // made once from the trailer (lossless), like the other derived media
            Project lp = exampleProject(media / "sintel_trailer_720p.mp4", media / "x.json", Time(75, 2), Time(4), 320, 180);
            lp.sequences[0].tracks.resize(1);
            RenderOptions o;
            o.includeAudio = false;
            o.encoder.crf = 0;
            renderProject(lp, media / "x.json", low, o);
        }
        p = exampleProject(low, out, Time(0), Time(4), 1280, 720);
        Sequence& q = p.sequences[0];
        q.tracks.resize(1);
        q.tracks[0].clips[0].upscale.mode = UpscaleMode::Lanczos;
        Track right{"v2", TrackKind::Video, "Video 2"};
        Clip ai = q.tracks[0].clips[0];
        ai.id = "ai";
        ai.upscale.mode = UpscaleMode::Ai;
        ai.upscale.model = "general";
        ai.upscale.scale = 4;
        ai.transform.cropLeft = 0.5;   // the right half
        right.clips.push_back(ai);
        q.tracks.push_back(right);
        addLabel(p, "Lanczos (not AI)", Time(0), Time(4), -0.25, -0.43, 28);
        addLabel(p, "Real-ESRGAN x4plus (AI)", Time(0), Time(4), 0.25, -0.43, 28);
    } else if (kind == "subtitles") {   // the bridge dialogue of Tears of Steel, transcribed (whisper-base) into captions
        p = exampleProject(media / "tears_of_steel_60s.mov", out, Time(0), Time(16), 1280, 720);
        ai::TranscribeOptions o;
        o.language = "en";
        const ai::CaptionResult r = ai::captionsForClip(p, p.sequences[0].tracks[0].clips[0], out, o);
        UndoStack st(p);
        st.execute(edit::addCaptions(p, "seq1", r.captions, Track::defaultSubtitleStyle(), Track().subtitleY));
        std::printf("%zu captions (%s, %s)\n", r.captions.size(), r.transcript.language.c_str(), r.transcript.provider.c_str());
    } else if (kind == "scenes") {   // the trailer's frames 300-479, split at every detected boundary; every second
                                     // scene in black and white so that the splits can be seen
        const auto file = media / "sintel_trailer_720p.mp4";
        p = exampleProject(file, out, Time(300, 24), Time(180, 24), 1280, 720);
        const vision::SceneResult r = vision::detectScenes(file, vision::SceneOptions{}, 300, 479);
        UndoStack st(p);
        for (auto it = r.boundaries.rbegin(); it != r.boundaries.rend(); ++it)
            if (it->frame > 300) st.execute(edit::splitAt(p, "seq1", Time(it->frame - 300, 24)));
        auto& clips = p.sequences[0].tracks[0].clips;
        for (size_t i = 1; i < clips.size(); i += 2) {
            EffectRef bw = makeEffect(*findEffectDef("fa.color.basic"), "bw");
            bw.params["saturation"] = Animatable<double>(-100.0);
            clips[i].effects.push_back(bw);
        }
        std::printf("%zu boundaries:", r.boundaries.size());
        for (const auto& b : r.boundaries) std::printf(" %lld", static_cast<long long>(b.frame));
        std::printf(" -> %zu clips\n", clips.size());
    } else if (kind == "reframe") {   // Tears of Steel 28-36 s (faces, a shot change) reframed to 9:16 following faces
        const auto file = media / "tears_of_steel_60s.mov";
        p = exampleProject(file, out, Time(28), Time(8), 1280, 720);
        ai::ReframeOptions o;
        ai::ReframePath path = ai::analyseReframe(file, Time(28), Time(36), o);
        ai::planReframe(path, o);
        const auto [cw, ch] = ai::reframeCanvas(1280, 720, o.aspect);
        p.sequences[0].width = cw;
        p.sequences[0].height = ch;
        Clip& c = p.sequences[0].tracks[0].clips[0];
        c.transform = ai::reframeTransform(path, c.sourceIn, cw, ch, c.transform);
        std::printf("faces in %d of %zu samples, %zu shot changes, canvas %d x %d (%s)\n", path.found, path.samples.size(),
                    path.cuts.size(), cw, ch, path.provider.c_str());
    } else {
        std::cerr << "unknown example " << kind << "\n";
        return 2;
    }
    if (kind == "subtitles") addLabel(p, "Speech to Text (AI: whisper base) - captions uncorrected", Time(0), Time(16), 0, -0.43, 26);
    if (kind == "scenes") addLabel(p, "Scene Detection (not AI) - split at every cut", Time(0), Time(15, 2), 0, -0.43, 26);
    if (kind == "reframe") addLabel(p, "Auto Reframe 9:16 (AI: YuNet faces)", Time(0), Time(8), 0, -0.44, 18);
    const auto issues = validate(p);
    if (hasErrors(issues)) {
        std::cerr << formatIssues(issues) << "\n";
        return 1;
    }
    std::ofstream(out, std::ios::binary) << projectToJsonString(p);
    std::printf("%s written in %.1f s\n", out.string().c_str(), since(t0) / 1000);
    return 0;
}

int matte(const std::string& model, const std::filesystem::path& media, double from, int frames, ai::Accelerator acc,
          int quality) {
    AiCutout cut;
    cut.enabled = true;
    cut.model = model;
    cut.quality = quality;
    const auto ts = std::chrono::steady_clock::now();
    ai::MattingSession s(cut, acc);
    std::printf("%s on %s, quality %d: session created in %.0f ms\n", model.c_str(), s.provider().c_str(), quality, since(ts));
    VideoDecoder dec(media);
    const Rational fps = dec.info().frameRate.fps();
    const int64_t f0 = static_cast<int64_t>(from * fps.toDouble());
    double decodeMs = 0, inferMs = 0, firstMs = 0, inf0 = 0, proc0 = 0;
    for (int i = 0; i < frames; ++i) {
        auto t = std::chrono::steady_clock::now();
        const Image& img = dec.frameAt(Time(f0 + i) / fps).image;
        decodeMs += since(t);
        t = std::chrono::steady_clock::now();
        const AlphaImage a = s.run(img);
        const double ms = since(t);
        if (i == 0) {
            firstMs = ms;
            inf0 = s.inferenceMs();
            proc0 = s.processingMs();
        } else {
            inferMs += ms;
        }
        if (a.empty()) return 1;
        if (i == frames - 1 && a.width == 1280 && a.height == 534) {   // Tears of Steel bridge shot (10-12 s)
            auto mean = [&](int x0, int y0, int x1, int y1) {
                double sum = 0;
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x) sum += a.a[static_cast<size_t>(y) * a.width + x] / 255.0;
                return sum / ((x1 - x0) * (y1 - y0));
            };
            std::printf("  last frame: people %.3f / %.3f, sky %.3f\n", mean(390, 200, 450, 330), mean(770, 200, 860, 330),
                        mean(0, 0, 200, 80));
        }
    }
    const int warm = std::max(1, frames - 1);
    std::printf("  %dx%d: first frame %.1f ms (cold: graph compile), warm %.1f ms/frame = %.1f fps; decoding %.1f ms/frame; "
                "peak RAM %.0f MB\n",
                dec.info().width, dec.info().height, firstMs, inferMs / warm, 1000.0 / std::max(1e-9, inferMs / warm),
                decodeMs / frames, peakMb());
    std::printf("  graphics memory in use by this process: %s\n", vram().c_str());
    std::printf("  warm, inside the session: %.1f ms/frame inference, %.1f ms/frame pre/post-processing\n",
                (s.inferenceMs() - inf0) / warm, (s.processingMs() - proc0) / warm);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: fa-ai-bench matte <model> <media> <seconds-from> <frames> <cpu|gpu> [quality] | inputs <model> "
                     "| devices\n";
        return 2;
    }
    try {
        const std::string mode = argv[1];
        if (const char* m = std::getenv("MEERCUT_MODELS"); !m || !*m) ai::setModelsDirectoryOverride(FA_MODELS_DIR);
        if (mode == "inputs" && argc >= 3) {   // an ONNX model's inputs and their shapes (-1 = dynamic)
            const ai::ModelInfo* m = ai::findModel(argv[2]);
            if (!m) throw std::runtime_error("unknown model");
            ai::OnnxModel om(ai::modelFilePath(*m, 0), ai::Accelerator::Cpu);
            for (size_t i = 0; i < om.inputNames().size(); ++i) {
                std::printf("%s:", om.inputNames()[i].c_str());
                for (int64_t d : om.inputShape(i)) std::printf(" %lld", static_cast<long long>(d));
                std::printf("\n");
            }
            return 0;
        }
        if (mode == "onnx" && argc >= 5) {   // onnx <model-id> <cpu|gpu0|gpu1..> <runs>: zero input of the fixed shape
            const ai::ModelInfo* m = ai::findModel(argv[2]);
            if (!m) throw std::runtime_error("unknown model");
            const std::string dev = argv[3];
            const auto t0 = std::chrono::steady_clock::now();
            ai::OnnxModel om(ai::modelFilePath(*m, 0), dev == "cpu" ? ai::Accelerator::Cpu : ai::Accelerator::Gpu,
                             dev == "cpu" ? -1 : std::stoi(dev.substr(3)));
            std::printf("%s on %s: session %.0f ms\n", m->id.c_str(), om.provider().c_str(), since(t0));
            ai::Tensor in;
            for (int64_t d : om.inputShape(0)) in.shape.push_back(d);
            in.data.assign(static_cast<size_t>(in.count()), 0.5f);
            for (int i = 0; i < std::stoi(argv[4]); ++i) {
                const auto t = std::chrono::steady_clock::now();
                om.run({{om.inputNames().front(), &in}}, {om.outputNames().back()});
                std::printf("  run %d: %.0f ms, peak RAM %.0f MB\n", i + 1, since(t), peakMb());
            }
            return 0;
        }
        if (mode == "speech" && argc >= 7) {   // speech <media> <from> <to> <model> <cpu|gpu> [language]
            ai::TranscribeOptions o;
            o.model = argv[5];
            ai::acceleratorFromId(argv[6], o.accelerator);
            if (argc > 7) o.language = argv[7];
            VramPeak vp;
            const ai::Transcript t = ai::transcribeFile(argv[2], std::stod(argv[3]), std::stod(argv[4]), o);
            std::printf("peak local graphics memory during the run: %.0f MB\n", vp.peakMb);
            for (const auto& s : t.segments) std::printf("  [%7.2f - %7.2f] %s\n", s.start, s.end, s.text.c_str());
            std::printf("%s on %s%s: %.1f s of audio in %.1f s (%.1fx real time), language %s, %zu segments; peak RAM %.0f MB; "
                        "graphics memory %s\n",
                        o.model.c_str(), t.provider.c_str(), t.fallback.empty() ? "" : (" (" + t.fallback + ")").c_str(),
                        t.audioSeconds, t.seconds, t.audioSeconds / std::max(1e-9, t.seconds), t.language.c_str(),
                        t.segments.size(), peakMb(), vram().c_str());
            return 0;
        }
        if (mode == "wer" && argc >= 5) {   // wer <model> <cpu|gpu> <testmedia/ai folder>: accuracy on the references
            ai::TranscribeOptions o;
            o.model = argv[2];
            ai::acceleratorFromId(argv[3], o.accelerator);
            const std::filesystem::path root = argv[4];
            struct Set {
                const char* dir;
                const char* textKey;
                int count;
            };
            for (const Set set : {Set{"librispeech", "text", 12}, Set{"fleurs/es_419", "raw", 10}, Set{"fleurs/fr_fr", "raw", 10},
                                  Set{"fleurs/ar_eg", "raw", 10}}) {
                std::ifstream in(root / set.dir / "transcripts.json", std::ios::binary);
                const nlohmann::json all = nlohmann::json::parse(in);
                std::vector<std::pair<std::string, std::string>> items;
                for (const auto& [name, v] : all.items()) items.push_back({name, v[set.textKey].get<std::string>()});
                std::sort(items.begin(), items.end());
                double ref = 0, werSum = 0, cerSum = 0, audio = 0, secs = 0;
                int n = 0, detected = 0;
                std::string lang;
                for (const auto& [name, text] : items) {
                    if (n == set.count) break;
                    const ai::Transcript t = ai::transcribeFile(root / set.dir / name, 0, 0, o);
                    std::string hyp;
                    for (const auto& s : t.segments) hyp += " " + s.text;
                    const std::string norm = ai::normaliseForScoring(text);
                    const double words = static_cast<double>(std::count(norm.begin(), norm.end(), ' ') + 1);
                    werSum += ai::wordErrorRate(text, hyp) * words;
                    cerSum += ai::charErrorRate(text, hyp);
                    ref += words;
                    audio += t.audioSeconds;
                    secs += t.seconds;
                    lang = t.language;
                    ++n;
                    detected += t.language == std::string(set.dir).substr(std::string(set.dir).find('/') + 1, 2) ||
                                (std::string(set.dir) == "librispeech" && t.language == "en");
                }
                std::printf("%-14s %-16s %2d recordings %6.1f s: WER %.3f, CER %.3f, language %d/%d, %.1fx real time\n",
                            o.model.c_str(), set.dir, n, audio, werSum / ref, cerSum / n, detected, n, audio / secs);
            }
            return 0;
        }
        if (mode == "devices") {
            std::cout << ai::describeCapabilities(ai::aiCapabilities()) << "\n";
            return 0;
        }
        if (mode == "example" && argc >= 5) return example(argv[2], argv[3], argv[4]);
        if (mode == "rife" && argc >= 6) {
            ai::Accelerator acc = ai::Accelerator::Auto;
            ai::acceleratorFromId(argv[5], acc);
            return rife(argv[2], std::stod(argv[3]), std::stoi(argv[4]), acc, argc > 6 ? std::stoi(argv[6]) : 0);
        }
        if (mode == "upscale" && argc >= 8) {
            ai::Accelerator acc = ai::Accelerator::Auto;
            ai::acceleratorFromId(argv[7], acc);
            return upscale(argv[2], std::stoi(argv[3]), argv[4], std::stod(argv[5]), std::stoi(argv[6]), acc,
                           argc > 8 ? std::stoi(argv[8]) : 0);
        }
        if (mode == "matte" && argc >= 7) {
            ai::Accelerator acc = ai::Accelerator::Auto;
            ai::acceleratorFromId(argv[6], acc);
            return matte(argv[2], argv[3], std::stod(argv[4]), std::stoi(argv[5]), acc, argc > 7 ? std::stoi(argv[7]) : 1);
        }
        std::cerr << "unknown mode\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
