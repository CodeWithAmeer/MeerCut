// fa-bench: per-stage timings of the rendering pipeline and the CPU vs GPU compositing comparison.
//   fa-bench <testmedia dir> [--frames N] [--json out.json] [--no-export] [--no-gpu]
// Stages: decode (CPU, FFmpeg), compositing scenes on every backend (CPU reference, GPU on the
// high-performance and on the power-saving adapter), x264 encode, a full export of an M2
// composition with each backend, and (M3, --text-only for just these) the text engine: layout,
// static / animated title frames, titles over video per backend, SRT import, export with titles
// (FA_BENCH_TEXT_BREAKDOWN=1 adds the cost of each text effect on a per-character animated frame). For GPU backends it also reports the GPU execution time (D3D11
// timestamp queries) and the utilisation of the adapter's engines measured by Windows' own
// "GPU Engine" performance counters (the same source as Task Manager), so GPU use is observed,
// not assumed from a successful initialisation.
// M4 (--effects-only for just these): every effect at 1080p on each backend (CPU, GPU high-performance and
// power-saving adapters, Auto), colour-grade and effect stacks, an adjustment layer, the texture reuse of a paused
// frame, and exports with effects; GPU effect passes and CPU fallbacks per frame are reported from the backend.
// M5 (--motion-only for just these): preview frames with retiming (2x, 0.5x blending, reverse, Bullet ramp with
// optical flow), stabilised frames, tracking and stabilisation analysis, the Render Preview cache (store and play
// back the heaviest case), and 1080p exports of each.
// M6 (--m6-only for just these): audio mixing of 4 tracks in playback-sized chunks with each audio feature (automation,
// EQ, noise reduction, hum / high-pass, ducking, limiter), loudness measurement, and an export of the M2 composition
// with each format / encoder (CPU and the hardware encoders); --m6-memory <seconds> exports a long timeline and reports
// the peak working set (run it for two lengths: it must not grow with the timeline).
#include "fa/media/media.h"
#include "fa/model/commands.h"
#include "fa/model/edit_ops.h"
#include "fa/model/stabilize_plan.h"
#include "fa/model/time_map.h"
#include "fa/playback/preview_renderer.h"
#include "fa/playback/render_preview.h"
#include "fa/vision/flow.h"
#include "fa/vision/stabilize.h"
#include "fa/vision/track.h"
#include "fa/model/serialize.h"
#include "fa/media/export_format.h"
#include "fa/render/audio_dsp.h"
#include "fa/render/audio_mix.h"
#include "fa/render/backend.h"
#include "fa/render/effects.h"
#include "fa/render/lut.h"
#include "fa/render/renderer.h"
#ifdef FA_HAS_TEXT
#include "fa/model/subtitles.h"
#include "fa/text/title_engine.h"
#endif

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <psapi.h>
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")
#endif

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

nlohmann::json g_json;

void report(const std::string& stage, double ms, const std::string& extra = {}) {
    std::printf("%-66s %9.2f ms %8.1f fps  %s\n", stage.c_str(), ms, 1000.0 / ms, extra.c_str());
    g_json["stages"].push_back({{"stage", stage}, {"ms_per_frame", ms}, {"fps", 1000.0 / ms}, {"note", extra}});
}

#ifdef _WIN32
// Utilisation of this process' GPU engines (Windows "GPU Engine" counters), averaged over an interval.
class GpuEngineUsage {
public:
    void start() {
        if (PdhOpenQueryW(nullptr, 0, &q_) != ERROR_SUCCESS) return;
        const std::wstring path = L"\\GPU Engine(pid_" + std::to_wstring(GetCurrentProcessId()) + L"_*)\\Utilization Percentage";
        DWORD len = 0;
        PdhExpandWildCardPathW(nullptr, path.c_str(), nullptr, &len, 0);
        std::wstring buf(len + 2, L'\0');
        if (len == 0 || PdhExpandWildCardPathW(nullptr, path.c_str(), buf.data(), &len, 0) != ERROR_SUCCESS) return;
        for (const wchar_t* p = buf.c_str(); *p; p += wcslen(p) + 1) {
            PDH_HCOUNTER c{};
            if (PdhAddEnglishCounterW(q_, p, 0, &c) == ERROR_SUCCESS) counters_.push_back({p, c});
        }
        PdhCollectQueryData(q_);
    }
    // engine type ("3D", "Copy", "VideoDecode", ...) + adapter LUID -> utilisation %
    std::map<std::string, double> stop() {
        std::map<std::string, double> out;
        if (!q_) return out;
        PdhCollectQueryData(q_);
        for (const auto& [name, c] : counters_) {
            PDH_FMT_COUNTERVALUE v{};
            if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &v) != ERROR_SUCCESS) continue;
            std::string n;   // instance names are ASCII
            for (wchar_t ch : name) n += static_cast<char>(ch & 0x7F);
            const auto luid = n.find("luid_"), eng = n.find("engtype_");
            if (luid == std::string::npos || eng == std::string::npos) continue;
            const std::string key = n.substr(eng + 8, n.find(')', eng) - eng - 8) + " @" + n.substr(luid + 5, 21);
            out[key] = std::max(out[key], v.doubleValue);
        }
        PdhCloseQuery(q_);
        q_ = nullptr;
        counters_.clear();
        return out;
    }

private:
    PDH_HQUERY q_ = nullptr;
    std::vector<std::pair<std::wstring, PDH_HCOUNTER>> counters_;
};
#endif

struct Backend {
    std::string label;
    std::unique_ptr<fa::RenderBackend> be;
};

fa::Clip transformed(double px, double py, double s, double rot) {
    fa::Clip c;
    c.transform.positionX.setConstant(px);
    c.transform.positionY.setConstant(py);
    c.transform.scaleX.setConstant(s);
    c.transform.scaleY.setConstant(s);
    c.transform.rotation.setConstant(rot);
    return c;
}

// The M2 benchmark composition: full-frame base, a rotated picture-in-picture with a feathered
// ellipse mask in Screen mode, and a rounded-rectangle-masked layer in Overlay mode.
std::vector<fa::LayerDesc> m2Scene(const fa::Image& a, const fa::Image& b, int w, int h) {
    fa::Clip c1 = transformed(0.18, -0.1, 0.6, 12);
    fa::Mask e;
    e.shape = fa::MaskShape::Ellipse;
    e.width.setConstant(0.9);
    e.height.setConstant(0.7);
    e.feather.setConstant(0.05);
    c1.masks.push_back(e);
    c1.blend = fa::BlendMode::Screen;
    fa::Clip c2 = transformed(-0.2, 0.15, 0.5, -8);
    fa::Mask r;
    r.shape = fa::MaskShape::Rectangle;
    r.width.setConstant(1.2);
    r.height.setConstant(0.8);
    r.cornerRadius = 0.2;
    r.feather.setConstant(0.01);
    c2.masks.push_back(r);
    c2.blend = fa::BlendMode::Overlay;
    c2.opacity.setConstant(0.85);
    return {fa::clipLayer(fa::Clip{}, fa::Time(0), &a, w, h), fa::clipLayer(c1, fa::Time(0), &b, w, h),
            fa::clipLayer(c2, fa::Time(0), &a, w, h)};
}

fa::Project exportProject(const std::filesystem::path& media) {
    fa::Project p;
    p.name = "bench";
    auto addSource = [&](const char* id, const char* file) {
        const auto path = media / file;
        const fa::MediaInfo mi = fa::probeMedia(path);
        fa::MediaSource s;
        s.id = id;
        s.path = path.string();
        s.duration = mi.video->duration;
        s.frameRate = mi.video->frameRate;
        s.width = mi.video->width;
        s.height = mi.video->height;
        s.hasAudio = false;
        p.sources.push_back(s);
    };
    addSource("src-c", "C_testsrc2_1080p30_10s.mp4");
    addSource("src-d", "D_greenscreen_disc_1080p30_10s.mp4");
    fa::Sequence s;
    s.id = "seq-1";
    s.name = "bench";
    fa::Track v1{"v1", fa::TrackKind::Video, "Video 1"}, v2{"v2", fa::TrackKind::Video, "Video 2"};
    fa::Clip base;
    base.id = "c-base";
    base.sourceId = "src-c";
    base.duration = fa::Rational(10);
    v1.clips.push_back(base);
    fa::Clip top = transformed(0.18, -0.1, 0.6, 12);
    top.id = "c-top";
    top.sourceId = "src-d";
    top.duration = fa::Rational(10);
    top.transform.rotation.setKey({fa::Rational(0), -10.0});
    top.transform.rotation.setKey({fa::Rational(10), 25.0});
    fa::Mask e;
    e.id = "m1";
    e.width.setConstant(0.9);
    e.height.setConstant(0.7);
    e.feather.setConstant(0.05);
    top.masks.push_back(e);
    top.blend = fa::BlendMode::Screen;
    v2.clips.push_back(top);
    s.tracks = {v1, v2};
    p.sequences.push_back(s);
    return p;
}


// ---- M4: effects, filters and colour ------------------------------------------------------------------

// An instance with settings that change the picture: the type's defaults, or, where those are neutral (the colour
// tools), every zero-default slider at 30 % of its range, an S-curve and hue curves, a built-in look.
fa::EffectRef benchEffect(const fa::EffectDef& def, const std::string& id) {
    fa::EffectRef e = fa::makeEffect(def, id);
    if (def.usesLut) e.strings["lut"] = std::string(fa::kBuiltinLutPrefix) + fa::builtinLooks().front().id;
    if (!def.curves.empty()) {
        e.curves["master"] = {{0, 0}, {0.25, 0.18}, {0.75, 0.84}, {1, 1}};
        e.curves["red"] = {{0, 0.03}, {0.5, 0.52}, {1, 1}};
        e.curves["hueSat"] = {{0.0, 0.5}, {0.33, 0.7}, {0.66, 0.4}};
    }
    const auto stack = fa::evaluateEffects({e}, fa::Time(1), 1.0);
    if (!stack.empty() && def.isIdentity && def.isIdentity(stack[0]))
        for (const auto& pd : def.params)
            if (pd.kind == fa::ParamKind::Float && pd.def == 0 && pd.max > 0) e.params[pd.id].setConstant(0.3 * pd.max);
    return e;
}

fa::EffectRef effectOf(const std::string& type, const std::string& id) { return benchEffect(*fa::findEffectDef(type), id); }

std::vector<fa::EffectRef> gradeStack() {   // a typical Color-tab grade
    return {effectOf("fa.color.basic", "g1"), effectOf("fa.color.hsl", "g2"), effectOf("fa.color.curves", "g3"),
            effectOf("fa.color.wheels", "g4"), effectOf("fa.color.lut", "g5")};
}

std::vector<fa::EffectRef> effectsStack() {   // a masked blur, glow, vignette and grain
    fa::EffectRef blur = effectOf("fa.fx.blur.gaussian", "e1");
    fa::Mask m;
    m.id = "m";
    m.shape = fa::MaskShape::Ellipse;
    m.width.setConstant(1.0);
    m.height.setConstant(0.6);
    m.feather.setConstant(0.1);
    m.inverted = true;   // blur around a sharp centre
    blur.masks.push_back(m);
    return {blur, effectOf("fa.fx.glow", "e2"), effectOf("fa.fx.vignette", "e3"), effectOf("fa.fx.grain", "e4")};
}

void effectBenchmarks(const std::filesystem::path& media, int frames, std::vector<Backend>& backends, bool doExport) {
    std::printf("-- M4 effects, filters and colour --\n");
    const fa::Image a = fa::VideoDecoder(media / "C_testsrc2_1080p30_10s.mp4").frameAt(fa::Rational(2)).image;
    // Times frames of `layers` on a backend: at least 5, at most `frames`, about 1.5 s per case.
    auto run = [&](const std::string& name, Backend& b, const std::vector<fa::LayerDesc>& layers, int w, int h) {
        auto frame = [&] {
            b.be->beginFrame(w, h, fa::Rgba8{0, 0, 0, 255});
            for (const auto& l : layers) b.be->drawLayer(l);
            b.be->endFrame();
        };
        frame();   // warm-up: shaders, targets, tables
        const int64_t passes0 = b.be->gpuEffectPasses(), fallbacks0 = b.be->cpuEffectFallbacks();
        const int64_t up0 = b.be->textureUploads(), re0 = b.be->textureReuses();
        int n = 0;
        const auto t0 = Clock::now();
        while (n < 5 || (n < frames && msSince(t0) < 1500)) {
            frame();
            ++n;
        }
        const double ms = msSince(t0) / n;
        std::string note;
        char buf[160];
        if (b.label != "CPU") {
            if (passes0 >= 0) {
                std::snprintf(buf, sizeof buf, "%.1f GPU passes, %.1f CPU fallbacks / frame",
                              double(b.be->gpuEffectPasses() - passes0) / n, double(b.be->cpuEffectFallbacks() - fallbacks0) / n);
                note += buf;
            }
            if (up0 >= 0) {
                std::snprintf(buf, sizeof buf, "; uploads %.1f, reuses %.1f / frame", double(b.be->textureUploads() - up0) / n,
                              double(b.be->textureReuses() - re0) / n);
                note += buf;
            }
            b.be->setTimingEnabled(true);
            double gms = 0;
            int timed = 0;
            for (int i = 0; i < 5; ++i) {
                frame();
                if (b.be->lastGpuMilliseconds() >= 0) {
                    gms += b.be->lastGpuMilliseconds();
                    ++timed;
                }
            }
            b.be->setTimingEnabled(false);
            if (timed) {
                std::snprintf(buf, sizeof buf, "; GPU execution %.2f ms", gms / timed);
                note += buf;
            }
        }
        report(name + " [" + b.label + "]", ms, note);
        g_json["effects"].push_back({{"case", name}, {"backend", b.label}, {"ms_per_frame", ms}, {"note", note}});
    };
    auto withEffects = [&](std::vector<fa::EffectRef> fx, int w, int h, uint64_t key = 0) {
        fa::Clip c;
        c.effects = std::move(fx);
        fa::LayerDesc l = fa::clipLayer(c, fa::Time(1), &a, w, h);
        l.imageKey = key;
        return std::vector{l};
    };
    // 1. every effect type alone, 1080p (a decoded frame uploaded every frame, like playback)
    for (const auto& def : fa::effectDefs()) {
        const auto layers = withEffects({benchEffect(def, "fx")}, 1920, 1080);
        if (!layers[0].effects || fa::effectsAreIdentity(*layers[0].effects)) {
            std::printf("%-66s (no visible settings, skipped)\n", def.id.c_str());
            continue;
        }
        for (auto& b : backends) run("effect " + def.id + " 1080p", b, layers, 1920, 1080);
    }
    // 2. stacks, an adjustment layer, the preview size, and a paused frame (keyed: uploaded once)
    struct Case {
        std::string name;
        int w, h;
        std::vector<fa::LayerDesc> layers;
    };
    std::vector<Case> cases;
    cases.push_back({"plain frame 1080p (no effects, reference)", 1920, 1080, withEffects({}, 1920, 1080)});
    cases.push_back({"grade stack 1080p (Basic+HSL+Curves+Wheels+LUT)", 1920, 1080, withEffects(gradeStack(), 1920, 1080)});
    cases.push_back({"grade stack -> 960x540 preview", 960, 540, withEffects(gradeStack(), 960, 540)});
    cases.push_back({"grade stack 1080p, paused frame (keyed texture)", 1920, 1080,
                     withEffects(gradeStack(), 1920, 1080, 0x8000000000000123ULL)});
    cases.push_back({"effects stack 1080p (masked blur+glow+vignette+grain)", 1920, 1080, withEffects(effectsStack(), 1920, 1080)});
    {
        auto all = gradeStack();
        for (auto& e : effectsStack()) all.push_back(e);
        cases.push_back({"grade + effects stacks 1080p (9 effects)", 1920, 1080, withEffects(all, 1920, 1080)});
    }
    {
        fa::Clip adj;
        adj.adjustment = true;
        adj.effects = {effectOf("fa.color.basic", "a1"), effectOf("fa.filter.bw", "a2")};
        auto layers = withEffects({}, 1920, 1080);
        layers.push_back(fa::adjustmentLayer(adj, fa::Time(1)));
        cases.push_back({"video + adjustment layer 1080p (Basic + B&W on the canvas)", 1920, 1080, layers});
    }
    for (const auto& c : cases)
        for (auto& b : backends) run(c.name, b, c.layers, c.w, c.h);

    // 3. export 1080p30, 300 frames: video alone, graded, graded + effects
    if (!doExport) return;
    const auto dir = std::filesystem::temp_directory_path() / "fa_bench_fx_export";
    std::filesystem::create_directories(dir);
    for (int variant = 0; variant < 3; ++variant) {
        fa::Project p = exportProject(media);
        p.sequences[0].tracks.resize(1);
        auto& clip = p.sequences[0].tracks[0].clips[0];
        if (variant >= 1) clip.effects = gradeStack();
        if (variant == 2)
            for (auto& e : effectsStack()) clip.effects.push_back(e);
        const char* what[] = {"video only", "video + grade stack (5)", "video + grade + effects stacks (9)"};
        const auto projectFile = dir / "bench_fx.json";
        fa::saveProject(p, projectFile);
        for (fa::BackendChoice choice : {fa::BackendChoice::Cpu, fa::BackendChoice::Gpu, fa::BackendChoice::Auto}) {
            if (choice != fa::BackendChoice::Cpu && backends.size() < 2) continue;
            fa::RenderOptions o;
            o.backend = choice;
            o.includeAudio = false;
            const fa::RenderReport r = fa::renderProject(p, projectFile, dir / "bench_fx.mp4", o);
            char note[160];
            std::snprintf(note, sizeof note, "%.2f s total: decode %.2f, composite %.2f, encode %.2f", r.seconds, r.decodeSeconds,
                          r.compositeSeconds, r.encodeSeconds);
            const char* asked = choice == fa::BackendChoice::Cpu ? "CPU" : choice == fa::BackendChoice::Gpu ? "GPU" : "Auto";
            report(std::string("export 1080p30, 300 frames, ") + what[variant] + " [" + asked + ": " + r.backend + "]",
                   r.seconds * 1000.0 / r.framesWritten, note);
        }
    }
    std::filesystem::remove_all(dir);
}

#ifdef FA_HAS_TEXT
// ---- M3: text engine ------------------------------------------------------------------------------

size_t peakWorkingSetMb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return pmc.PeakWorkingSetSize >> 20;
#endif
    return 0;
}

fa::TitleData benchTitle(const std::string& text, double size = 72) {
    fa::TitleData t;
    t.text = text;
    t.style.font = "Segoe UI";
    t.style.size = size;
    t.style.weight = 700;
    return t;
}

fa::TitleData styledTitle(const std::string& text) {   // every raster pass: outline, shadow, glow, background
    fa::TitleData t = benchTitle(text);
    t.style.outline = true;
    t.style.shadow = true;
    t.style.shadowBlur = 6;
    t.style.glow = true;
    t.style.background = true;
    return t;
}

// Burn-in benchmark project: 10 s of C_testsrc2, an animated title on Video 2 and 5 captions on a subtitle track.
fa::Project textExportProject(const std::filesystem::path& media, bool withText) {
    fa::Project p = exportProject(media);
    fa::Sequence& s = p.sequences[0];
    s.tracks.resize(1);   // the plain video only
    if (!withText) return p;
    fa::Track v2{"v2", fa::TrackKind::Video, "Video 2"};
    fa::Clip t;
    t.id = "title";
    t.title = styledTitle("MeerCut Titles");
    t.title->in = {"char_rise", fa::Rational(1)};
    t.title->loop = {"bounce", fa::Time(0)};
    t.title->out = {"fade", fa::Time(0)};
    t.duration = fa::Rational(10);
    t.transform.positionY.setConstant(-0.3);
    v2.clips.push_back(t);
    fa::Track subs{"s1", fa::TrackKind::Subtitle, "Subtitles 1"};
    for (int i = 0; i < 5; ++i) {
        fa::Clip c;
        c.id = "cap" + std::to_string(i);
        c.title = fa::TitleData{};
        c.title->text = "Caption number " + std::to_string(i + 1) + "\nwith a second line";
        c.title->followTrackStyle = true;
        c.timelineStart = fa::Rational(2 * i);
        c.duration = fa::Rational(19, 10);
        subs.clips.push_back(c);
    }
    s.tracks.push_back(v2);
    s.tracks.push_back(subs);
    return p;
}

void textBenchmarks(const std::filesystem::path& media, int frames, std::vector<Backend>& backends, bool doExport) {
    if (!fa::text::ensureGuiApplication()) {
        std::printf("text engine unavailable (no Qt GUI)\n");
        return;
    }
    using fa::Rational;
    using fa::Time;
    std::printf("-- M3 text engine --\n");
    // 1. layout (shaping, line breaking, grapheme units) - once per text/style, cached
    {
        fa::text::TitleEngine e;
        const auto t0 = Clock::now();
        const int n = 200;
        for (int i = 0; i < n; ++i) e.layout("Layout benchmark title number " + std::to_string(i) + "\nsecond line", benchTitle("").style);
        report("title layout (new text, 2 lines, 45 characters)", msSince(t0) / n, "cache miss: shaping + units");
        const auto t1 = Clock::now();
        for (int i = 0; i < 2000; ++i) e.layout("Layout benchmark title number 7\nsecond line", benchTitle("").style);
        report("title layout (cached)", msSince(t1) / 2000);
        const std::string arabic = "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85 "
                                   "\xf0\x9f\x98\x80 Mixed bidi text";
        const auto t2 = Clock::now();
        for (int i = 0; i < n; ++i) e.layout(arabic + " " + std::to_string(i), benchTitle("").style);
        report("title layout (Arabic + emoji + Latin, bidi)", msSince(t2) / n, "cache miss");
    }
    // 2. title frames at 1080p and preview size
    struct Case {
        std::string name;
        fa::TitleData title;
        Time at;
        int w, h;
    };
    fa::TitleData typed = benchTitle("Characters appear one at a time in place");
    typed.in = {"typewriter", Rational(2)};
    fa::TitleData rise = styledTitle("Per-character rise with every effect");
    rise.in = {"char_rise", Rational(2)};
    fa::TitleData blur = benchTitle("Blur reveal");
    blur.in = {"blur_in", Rational(2)};
    fa::TitleData slide = styledTitle("Whole-title slide");
    slide.in = {"slide_left", Rational(2)};
    const std::vector<Case> cases = {
        {"title frame 1080p, static (cached picture)", styledTitle("Static styled title"), Rational(1), 1920, 1080},
        {"title frame 1080p, whole-title slide (cached picture + placement)", slide, Rational(1), 1920, 1080},
        {"title frame 1080p, Type Writer (per-character re-compose)", typed, Rational(1), 1920, 1080},
        {"title frame 1080p, Character Rise + outline/shadow/glow/box", rise, Rational(1), 1920, 1080},
        {"title frame 960x540, Character Rise + outline/shadow/glow/box", rise, Rational(1), 960, 540},
        {"title frame 1080p, Blur In (Gaussian per frame)", blur, Rational(1), 1920, 1080},
    };
    if (std::getenv("FA_BENCH_TEXT_BREAKDOWN")) {   // per-effect cost of a per-character animated frame
        for (int fx = 0; fx < 6; ++fx) {
            fa::TitleData t = benchTitle("Per-character rise with every effect");
            t.in = {"char_rise", Rational(2)};
            const char* names[] = {"none", "outline", "shadow", "glow", "background", "all"};
            t.style.outline = fx == 1 || fx == 5;
            t.style.shadow = fx == 2 || fx == 5;
            t.style.shadowBlur = 6;
            t.style.glow = fx == 3 || fx == 5;
            t.style.background = fx == 4 || fx == 5;
            fa::text::TitleEngine e;
            e.render(t, t.style, Rational(1), Rational(5), 1920, 1080);
            const auto t0 = Clock::now();
            for (int n = 0; n < frames; ++n) e.render(t, t.style, Rational(1) + Rational(n, 1000), Rational(5), 1920, 1080);
            report(std::string("  breakdown: Character Rise 1080p, effect ") + names[fx], msSince(t0) / frames);
        }
    }
    for (const auto& c : cases) {
        fa::text::TitleEngine e;
        e.render(c.title, c.title.style, c.at, Rational(5), c.w, c.h);   // warm-up (layout)
        const auto t0 = Clock::now();
        for (int n = 0; n < frames; ++n) e.render(c.title, c.title.style, c.at + Rational(n, 1000), Rational(5), c.w, c.h);
        char note[96];
        std::snprintf(note, sizeof note, "%llu rasterised / %llu cache hits", static_cast<unsigned long long>(e.stats().rasterised),
                      static_cast<unsigned long long>(e.stats().staticHits));
        report(c.name, msSince(t0) / frames, note);
    }
    {   // first raster of a styled static title (what an inspector edit costs)
        fa::text::TitleEngine e;
        const auto t0 = Clock::now();
        const int n = 60;
        for (int i = 0; i < n; ++i) {
            fa::TitleData t = styledTitle("Styled title being edited");
            t.style.size = 60 + i % 30;   // a new style each time: new layout + raster
            e.render(t, t.style, Rational(1), Rational(5), 1920, 1080);
        }
        report("title edit 1080p: new style -> layout + raster (outline/shadow/glow/box)", msSince(t0) / n);
    }
    // 3. title over video composited on each backend (the renderFrame layer path)
    {
        fa::VideoDecoder dec(media / "C_testsrc2_1080p30_10s.mp4");
        const fa::Image a = dec.frameAt(Rational(2)).image;
        fa::text::TitleEngine e;
        fa::Clip tc;
        tc.title = rise;
        tc.duration = Rational(5);
        for (auto& [label, be] : backends) {
            auto frame = [&](int n) {
                const fa::TitleFrame f = e.render(rise, rise.style, Rational(1) + Rational(n, 1000), Rational(5), 1920, 1080);
                be->beginFrame(1920, 1080, fa::Rgba8{0, 0, 0, 255});
                be->drawLayer(fa::clipLayer(fa::Clip{}, Time(0), &a, 1920, 1080));
                be->drawLayer(fa::titleLayer(tc, Rational(1), f, 1920, 1080, fa::titleAdjust(f.anim, 1920, 1080)));
                be->endFrame();
            };
            frame(0);
            const auto t0 = Clock::now();
            for (int n = 0; n < frames; ++n) frame(n);
            report("video + animated title 1080p (engine + composite) [" + label + "]", msSince(t0) / frames);
        }
    }
    // 4. subtitles: parse + add 2000 captions (one undoable command)
    {
        std::string srt;
        for (int i = 0; i < 2000; ++i)
            srt += std::to_string(i + 1) + "\n" + fa::formatSrtTime(Rational(i * 3, 2)) + " --> " +
                   fa::formatSrtTime(Rational(i * 3 + 2, 2)) + "\n<i>Caption " + std::to_string(i + 1) + "</i>\nsecond line\n\n";
        const auto t0 = Clock::now();
        const fa::SubtitleDoc d = fa::parseSrt(srt);
        const double parseMs = msSince(t0);
        fa::Project p = textExportProject(media, false);
        const auto t1 = Clock::now();
        auto cmd = fa::edit::addCaptions(p, p.sequences[0].id, d.captions, d.style, d.subtitleY);
        cmd->apply(p);
        const double addMs = msSince(t1);
        const auto t2 = Clock::now();
        cmd->revert(p);
        const double undoMs = msSince(t2);
        char note[160];
        std::snprintf(note, sizeof note, "parse %.1f ms, add %.1f ms, undo %.1f ms (%zu captions)", parseMs, addMs, undoMs,
                      d.captions.size());
        std::printf("%-66s %s\n", "SRT import, 2000 captions", note);
        g_json["subtitles_2000"] = {{"parse_ms", parseMs}, {"add_ms", addMs}, {"undo_ms", undoMs}};
    }
    // 5. export with an animated title and captions vs the same video alone (1080p30, 300 frames)
    if (doExport) {
        const auto dir = std::filesystem::temp_directory_path() / "fa_bench_text_export";
        std::filesystem::create_directories(dir);
        for (const bool withText : {false, true}) {
            const fa::Project p = textExportProject(media, withText);
            const auto projectFile = dir / "bench_text.json";
            fa::saveProject(p, projectFile);
            for (fa::BackendChoice choice : {fa::BackendChoice::Cpu, fa::BackendChoice::Gpu}) {
                if (choice == fa::BackendChoice::Gpu && backends.size() < 2) continue;
                fa::RenderOptions o;
                o.backend = choice;
                o.includeAudio = false;
                o.titleRenderer = [] { return fa::text::makeTitleEngine(); };
                const fa::RenderReport r = fa::renderProject(p, projectFile, dir / "bench_text.mp4", o);
                char note[160];
                std::snprintf(note, sizeof note, "%.2f s total: decode %.2f, composite %.2f, encode %.2f", r.seconds,
                              r.decodeSeconds, r.compositeSeconds, r.encodeSeconds);
                report(std::string("export 1080p30, 300 frames, ") + (withText ? "video + animated title + 5 captions" : "video only") +
                           " [" + r.backend + "]",
                       r.seconds * 1000.0 / r.framesWritten, note);
            }
        }
        std::filesystem::remove_all(dir);
    }
    std::printf("peak working set of this process: %zu MB\n", peakWorkingSetMb());
    g_json["peak_working_set_mb"] = peakWorkingSetMb();
}
#endif

// ---- M5: speed, time remapping, tracking, stabilisation, Render Preview --------------------------------------

fa::Project oneClip(const std::filesystem::path& media, const char* file, int seconds = 10) {
    fa::Project p;
    p.name = "bench-m5";
    const auto path = media / file;
    const fa::MediaInfo mi = fa::probeMedia(path);
    fa::MediaSource s;
    s.id = "src";
    s.path = path.string();
    s.duration = mi.video->duration;
    s.frameRate = mi.video->frameRate;
    s.width = mi.video->width;
    s.height = mi.video->height;
    s.hasAudio = mi.audio.has_value();
    p.sources.push_back(s);
    fa::Sequence q;
    q.id = "seq-1";
    q.name = "bench";
    fa::Track v1{"v1", fa::TrackKind::Video, "Video 1"};
    fa::Clip c;
    c.id = "c";
    c.sourceId = "src";
    c.duration = fa::Rational(seconds);
    v1.clips.push_back(c);
    q.tracks = {v1};
    p.sequences.push_back(q);
    return p;
}

// The clip's speed through the same edit operation the editor uses (durations, ripple, linked audio).
void retime(fa::Project& p, const fa::ClipSpeed& sp) {
    fa::UndoStack st(p);
    st.execute(fa::edit::setSpeed(p, "seq-1", "c", sp));
}

void motionBenchmarks(const std::filesystem::path& media, int frames, bool doExport) {
    std::printf("-- M5 speed, time remapping, tracking, stabilisation, Render Preview --\n");
    const auto dir = std::filesystem::temp_directory_path() / "fa_bench_m5";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    // preview frames (960x540, the player's path: decode at preview size, retime, interpolate, composite)
    auto previewCase = [&](const std::string& name, const fa::Project& p, fa::BackendChoice be,
                           std::shared_ptr<fa::RenderCache> cache = nullptr) {
        fa::PreviewRenderer r(960, 540, 4);
        r.setBackend(be);
        if (cache) r.setRenderCache(cache);
        r.setSnapshot(fa::ProjectSnapshot{std::make_shared<const fa::Project>(p), "seq-1", "", 1});
        r.renderNow(0);   // backend, decoders
        const int64_t n = std::min<int64_t>(frames, p.sequences[0].frameCount() - 1);
        const auto t0 = Clock::now();
        for (int64_t f = 1; f <= n; ++f) r.renderNow(f);
        const double ms = msSince(t0) / static_cast<double>(n);
        report("preview 960x540 " + name + " [" + r.backendName() + "]", ms,
               cache ? "render cache hits " + std::to_string(r.renderCacheHits()) : std::string());
        return ms;
    };
    const fa::Project plain = oneClip(media, "C_testsrc2_1080p30_10s.mp4");
    fa::Project twice = plain, half = plain, reverse = plain, bulletFlow = plain, bulletQuality = plain;
    {
        fa::ClipSpeed s;
        s.speed = fa::Rational(2);
        retime(twice, s);
        s.speed = fa::Rational(1, 2);
        s.interpolation = fa::FrameInterpolation::Blending;
        retime(half, s);
        fa::ClipSpeed r;
        r.reverse = true;
        retime(reverse, r);
        fa::ClipSpeed b;
        const fa::RampPreset* bullet = fa::findRampPreset("bullet");
        b.ramp = fa::rampKeys(bullet->keys, fa::Time(0), fa::Rational(10));
        b.rampPreset = "bullet";
        b.interpolation = fa::FrameInterpolation::OpticalFlow;
        retime(bulletFlow, b);
        b.interpolation = fa::FrameInterpolation::OpticalFlowQuality;
        retime(bulletQuality, b);
    }
    for (fa::BackendChoice be : {fa::BackendChoice::Cpu, fa::BackendChoice::Auto}) {
        previewCase("1x (reference)", plain, be);
        previewCase("2x", twice, be);
        previewCase("0.5x frame blending", half, be);
        previewCase("reverse", reverse, be);
        previewCase("Bullet ramp + optical flow (fast)", bulletFlow, be);
    }
    previewCase("Bullet ramp + optical flow (quality)", bulletQuality, fa::BackendChoice::Auto);

    // one optical-flow in-between frame at export (1080p) and preview (960x540) size: total and flow estimation alone
    for (const auto& [w, h] : {std::pair{1920, 1080}, std::pair{960, 540}}) {
        fa::VideoDecoder::Options o;
        o.maxWidth = w;
        o.maxHeight = h;
        fa::VideoDecoder dec(media / "L_track_1080p30_5s.mp4", o);
        const fa::Image a = dec.frameAt(fa::Rational(40, 30)).image;
        const fa::Image b = dec.frameAt(fa::Rational(41, 30)).image;
        for (fa::vision::FlowQuality q : {fa::vision::FlowQuality::Fast, fa::vision::FlowQuality::Quality}) {
            fa::Image out;
            fa::vision::FlowStats st;
            fa::vision::interpolateFrames(a, b, 0.5, out, q, &st);   // warm
            double total = 0, flow = 0;
            const int n = 10;
            for (int i = 0; i < n; ++i) {
                fa::vision::interpolateFrames(a, b, 0.25 + 0.05 * i, out, q, &st);
                total += st.milliseconds;
                flow += st.flowMilliseconds;
            }
            char note[96];
            std::snprintf(note, sizeof note, "flow estimation %.2f ms, warp + blend %.2f ms", flow / n, (total - flow) / n);
            report("optical flow in-between frame " + std::to_string(a.width) + "x" + std::to_string(a.height) +
                       (q == fa::vision::FlowQuality::Fast ? " (fast)" : " (quality)"), total / n, note);
        }
    }

    // analysis
    {
        fa::VideoDecoder::Options o;
        o.maxWidth = 960;
        o.maxHeight = 540;
        fa::VideoDecoder dec(media / "L_track_1080p30_5s.mp4", o);
        fa::vision::RegionTracker tr;
        const fa::Image& first = dec.frameAt(fa::Time(0)).image;
        tr.start(first, fa::vision::Box{first.width / 2.0, first.height / 2.0, 80, 80});
        const auto t0 = Clock::now();
        for (int f = 1; f < 150; ++f) tr.next(dec.frameAt(fa::Rational(f, 30)).image);
        report("motion tracking analysis (960x540 decode + track)", msSince(t0) / 149);
    }
    fa::Stabilization stab;
    {
        fa::VideoDecoder::Options o;
        o.maxWidth = 960;
        o.maxHeight = 540;
        fa::VideoDecoder dec(media / "M_shaky_1080p30_5s.mp4", o);
        fa::vision::MotionEstimator est;
        const auto t0 = Clock::now();
        for (int f = 0; f < 150; ++f) {
            const fa::Image& img = dec.frameAt(fa::Rational(f, 30)).image;
            const fa::vision::CameraStep s = est.next(img);
            stab.motion.push_back({s.dx / img.width, s.dy / img.height, s.da, s.valid});
        }
        report("stabilisation analysis (960x540 decode + camera motion)", msSince(t0) / 150);
        stab.width = 1920;
        stab.height = 1080;
        stab.frameRate = fa::Rational(30);
        const auto t1 = Clock::now();
        for (int i = 0; i < 10; ++i) fa::planStabilization(stab);
        report("stabilisation plan, 150 frames (Normal)", msSince(t1) / 10, "whole clip, not per frame");
    }
    {   // compositing alone (1080p, one full-frame picture): a static zoom vs the per-frame stabilisation correction
        const fa::Image pic = fa::VideoDecoder(media / "M_shaky_1080p30_5s.mp4").frameAt(fa::Rational(1)).image;
        std::vector<Backend> bes;
        bes.push_back({"CPU", fa::makeCpuBackend()});
        std::string why;
        if (auto g = fa::makeGpuBackend(&why, true)) bes.push_back({"GPU high-perf", std::move(g)});
        fa::Clip still, moving;
        still.transform.uniformScale = true;
        still.transform.scaleX.setConstant(stab.zoom);
        moving.stabilization = stab;
        fa::planStabilization(*moving.stabilization);
        const std::vector<std::pair<std::string, const fa::Clip*>> cases = {{"static zoom", &still}, {"stabilised", &moving}};
        for (auto& b : bes)
            for (const auto& [label, clip] : cases) {
                auto frame = [&](int f) {
                    b.be->beginFrame(1920, 1080, fa::Rgba8{0, 0, 0, 255});
                    b.be->drawLayer(fa::clipLayer(*clip, fa::Rational(f % 150, 30), &pic, 1920, 1080));
                    b.be->endFrame();
                };
                frame(0);
                const auto t0 = Clock::now();
                for (int f = 1; f <= 60; ++f) frame(f);
                const double ms = msSince(t0) / 60;
                std::string note;
                b.be->setTimingEnabled(true);
                double gms = 0;
                int timed = 0;
                for (int i = 0; i < 5; ++i) {
                    frame(i);
                    if (b.be->lastGpuMilliseconds() >= 0) {
                        gms += b.be->lastGpuMilliseconds();
                        ++timed;
                    }
                }
                b.be->setTimingEnabled(false);
                if (timed) {
                    char buf[64];
                    std::snprintf(buf, sizeof buf, "GPU execution %.2f ms", gms / timed);
                    note = buf;
                }
                report("composite 1080p, one picture, " + label + " [" + b.label + "]", ms, note);
            }
    }
    fa::Project shaky = oneClip(media, "M_shaky_1080p30_5s.mp4", 5);
    shaky.sequences[0].tracks[0].clips[0].stabilization = stab;
    for (fa::BackendChoice be : {fa::BackendChoice::Cpu, fa::BackendChoice::Auto}) previewCase("stabilised", shaky, be);

    // Render Preview cache: storing and playing back the heaviest case
    {
        auto cache = std::make_shared<fa::RenderCache>(dir / "cache");
        fa::PreviewRenderer probe(960, 540, 2);
        probe.setBackend(fa::BackendChoice::Auto);
        probe.setSnapshot(fa::ProjectSnapshot{std::make_shared<const fa::Project>(bulletQuality), "seq-1", "", 1});
        probe.renderNow(0);
        fa::RenderPreviewRequest req;
        req.snapshot = fa::ProjectSnapshot{std::make_shared<const fa::Project>(bulletQuality), "seq-1", "", 1};
        req.from = 0;
        req.to = std::min<int64_t>(frames + 1, bulletQuality.sequences[0].frameCount());
        req.backend = fa::BackendChoice::Auto;
        req.expectBackend = probe.backendName();
        std::atomic<bool> cancel{false};
        const fa::RenderPreviewResult rr = fa::renderPreviewRange(req, *cache, cancel);
        char note[160];
        std::snprintf(note, sizeof note, "%lld frames, %.1f MB on disk (%.0f KB per frame)", static_cast<long long>(rr.rendered),
                      cache->sizeBytes() / 1048576.0, cache->sizeBytes() / 1024.0 / std::max<int64_t>(1, rr.rendered));
        report("Render Preview: Bullet + optical flow (quality), render + store", rr.seconds * 1000 / std::max<int64_t>(1, rr.rendered), note);
        previewCase("Bullet + optical flow (quality) from the render cache", bulletQuality, fa::BackendChoice::Auto, cache);
    }

    if (doExport) {   // 1080p exports (Auto backend)
        auto exportCase = [&](const std::string& name, const fa::Project& p, fa::BackendChoice be = fa::BackendChoice::Auto) {
            const auto projectFile = dir / "bench_m5.json";
            fa::saveProject(p, projectFile);
            fa::RenderOptions o;
            o.includeAudio = true;
            o.backend = be;
            const fa::RenderReport r = fa::renderProject(p, projectFile, dir / "bench_m5.mp4", o);
            char note[200];
            std::snprintf(note, sizeof note, "%lld frames, %.2f s total: decode %.2f, composite %.2f, encode %.2f",
                          static_cast<long long>(r.framesWritten), r.seconds, r.decodeSeconds, r.compositeSeconds, r.encodeSeconds);
            report("export 1080p30 " + name + " [" + r.backend + "]", r.seconds * 1000.0 / std::max<int64_t>(1, r.framesWritten), note);
        };
        // light cases first, then the M_shaky group back to back, the heavy optical-flow export last (a hot laptop
        // throttles: measured stabilised-export composite 28.7 ms after it vs 18.5 ms for a static zoom before)
        exportCase("1x (reference)", plain);
        exportCase("2x, pitch kept", twice);
        exportCase("reverse", reverse);
        exportCase("unchanged M_shaky (reference)", oneClip(media, "M_shaky_1080p30_5s.mp4", 5));
        fa::Project zoomed = oneClip(media, "M_shaky_1080p30_5s.mp4", 5);   // the same zoom, no per-frame correction
        zoomed.sequences[0].tracks[0].clips[0].transform.scaleX.setConstant(stab.zoom);
        zoomed.sequences[0].tracks[0].clips[0].transform.scaleY.setConstant(stab.zoom);
        exportCase("static zoom (reference for stabilised)", zoomed);
        exportCase("stabilised", shaky);
        exportCase("stabilised", shaky, fa::BackendChoice::Cpu);
        exportCase("Bullet ramp + optical flow (fast)", bulletFlow);
    }
    std::filesystem::remove_all(dir);
}

// ---- M6: audio mixing / DSP throughput, export speed per encoder, memory on long timelines ------------------------

size_t workingSetMb(bool peak) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        return (peak ? pmc.PeakWorkingSetSize : pmc.WorkingSetSize) >> 20;
#endif
    return 0;
}

// The largest working set while it exists (sampled every 10 ms): the process peak cannot be reset between cases.
class WorkingSetPeak {
public:
    WorkingSetPeak() : thread_([this] {
        while (!stop_) {
            max_ = std::max(max_.load(), workingSetMb(false));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }) {}
    ~WorkingSetPeak() { stopNow(); }
    size_t stopNow() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        return max_ = std::max(max_.load(), workingSetMb(false));
    }

private:
    std::atomic<bool> stop_{false};
    std::atomic<size_t> max_{0};
    std::thread thread_;
};

// `tracks` audio tracks `seconds` long, each of 10 s clips alternating A (1 kHz tone) and B (1.5 kHz tone).
fa::Project audioProject(const std::filesystem::path& media, int tracks, int seconds) {
    fa::Project p;
    p.name = "bench audio";
    for (const char* id : {"A", "B"}) {
        fa::MediaSource s;
        s.id = id;
        s.path = (media / (id[0] == 'A' ? "A_red_1080p30_10s.mp4" : "B_blue_1080p30_10s.mp4")).string();
        s.duration = fa::Rational(10);
        s.frameRate = fa::FrameRate::fps30();
        s.width = 1920;
        s.height = 1080;
        s.hasAudio = true;
        p.sources.push_back(s);
    }
    fa::Sequence s;
    s.id = "seq-1";
    s.name = "bench audio";
    for (int t = 0; t < tracks; ++t) {
        fa::Track tr{"a" + std::to_string(t + 1), fa::TrackKind::Audio, "Audio " + std::to_string(t + 1)};
        for (int k = 0; k * 10 < seconds; ++k) {
            fa::Clip c;
            c.id = "c" + std::to_string(t) + "-" + std::to_string(k);
            c.sourceId = (k + t) % 2 ? "B" : "A";
            c.timelineStart = fa::Rational(k * 10);
            c.duration = fa::Rational(std::min(10, seconds - k * 10));
            c.volumeDb.setConstant(-12.0);
            tr.clips.push_back(c);
        }
        s.tracks.push_back(tr);
    }
    p.sequences.push_back(s);
    return p;
}

void m6Benchmarks(const std::filesystem::path& media, bool doExport) {
    // 1. mixing 60 s of 4 tracks in 1024-sample chunks (the playback pattern): ms per chunk and the real-time factor
    struct Case {
        std::string name;
        std::function<void(fa::Project&)> setup;
    };
    const std::vector<Case> cases = {
        {"volume only", [](fa::Project&) {}},
        {"+ volume automation, fades, balance",
         [](fa::Project& p) {
             for (auto& t : p.sequences[0].tracks)
                 for (auto& c : t.clips) {
                     c.volumeDb.setKey({c.sourceIn, -12.0});
                     c.volumeDb.setKey({c.sourceIn + fa::Rational(5), -6.0});
                     c.audio.fadeIn = fa::Rational(1);
                     c.audio.fadeOut = fa::Rational(1);
                     c.audio.balance.setConstant(0.3);
                 }
         }},
        {"+ 10-band EQ on every clip",
         [](fa::Project& p) {
             for (auto& t : p.sequences[0].tracks)
                 for (auto& c : t.clips) {
                     c.audio.eqEnabled = true;
                     c.audio.eqGainsDb = fa::findEqPreset("voice-clarity")->gainsDb;
                 }
         }},
        {"+ noise reduction 0.5 on one track",
         [](fa::Project& p) {
             for (auto& c : p.sequences[0].tracks[0].clips) c.audio.cleanup.denoise = 0.5;
         }},
        {"+ noise reduction 0.5 on every clip",
         [](fa::Project& p) {
             for (auto& t : p.sequences[0].tracks)
                 for (auto& c : t.clips) c.audio.cleanup.denoise = 0.5;
         }},
        {"+ hum removal and high-pass on every clip",
         [](fa::Project& p) {
             for (auto& t : p.sequences[0].tracks)
                 for (auto& c : t.clips) {
                     c.audio.cleanup.humHz = 50;
                     c.audio.cleanup.highPassHz = 80;
                 }
         }},
        {"+ ducking (track 1 ducks the others)",
         [](fa::Project& p) {
             for (auto& c : p.sequences[0].tracks[0].clips) c.audio.ducking.enabled = true;
         }},
        {"+ master limiter (clips at +6 dB)",
         [](fa::Project& p) {
             p.sequences[0].limiter = true;
             for (auto& t : p.sequences[0].tracks)
                 for (auto& c : t.clips) c.volumeDb.setConstant(6.0);
         }},
    };
    constexpr int kChunk = 1024, kSeconds = 60;
    for (const auto& cs : cases) {
        fa::Project p = audioProject(media, 4, kSeconds);
        cs.setup(p);
        fa::AudioMixer mixer("", 48000, 2);
        std::vector<float> buf(kChunk * 2);
        const size_t before = workingSetMb(false);
        WorkingSetPeak peak;
        const auto t0 = Clock::now();
        int64_t chunks = 0;
        for (int64_t pos = 0; pos < int64_t(kSeconds) * 48000; pos += kChunk, ++chunks)
            mixer.mix(p, p.sequences[0], pos, kChunk, buf.data());
        const double ms = msSince(t0);
        const size_t most = peak.stopNow();
        char note[160];
        std::snprintf(note, sizeof note, "%.0fx real time (%d s in %.2f s); working set up to +%zu MB", kSeconds * 1000.0 / ms,
                      kSeconds, ms / 1000, most > before ? most - before : size_t(0));
        report("mix 4 tracks x 60 s, 1024-sample chunks " + cs.name, ms / double(chunks), note);
        g_json["m6_mix"].push_back({{"case", cs.name}, {"realtime_factor", kSeconds * 1000.0 / ms}, {"seconds", ms / 1000},
                                    {"working_set_growth_mb", most > before ? most - before : size_t(0)}});
    }
    // loudness measurement (BS.1770-4 after cleanup and EQ) of a 10 s clip, 6 times: 60 s of audio
    {
        const fa::Project p = audioProject(media, 1, 10);
        const auto t0 = Clock::now();
        double lufs = 0;
        for (int k = 0; k < 6; ++k) lufs = fa::measureClipLoudness(p, p.sequences[0], p.sequences[0].tracks[0].clips[0], "");
        const double ms = msSince(t0);
        char note[96];
        std::snprintf(note, sizeof note, "%.0fx real time; %.2f LUFS", 60000.0 / ms, lufs);
        report("loudness measurement, 10 s clip", ms / 6, note);
        g_json["m6_loudness_realtime_factor"] = 60000.0 / ms;
    }
    if (!doExport) return;

    // 2. export speed per format and encoder: the M2 composition (1080p30, 300 frames) with a stereo audio track
    struct Enc {
        const char* format;
        fa::HardwareEncoding hw;
        const char* label;
    };
    const Enc encs[] = {
        {"mp4-h264", fa::HardwareEncoding::Cpu, "MP4 H.264, CPU"},
        {"mp4-h264", fa::HardwareEncoding::Nvenc, "MP4 H.264, NVENC"},
        {"mp4-h264", fa::HardwareEncoding::Qsv, "MP4 H.264, Quick Sync"},
        {"mp4-hevc", fa::HardwareEncoding::Cpu, "MP4 HEVC, CPU"},
        {"mp4-hevc", fa::HardwareEncoding::Nvenc, "MP4 HEVC, NVENC"},
        {"mp4-hevc", fa::HardwareEncoding::Qsv, "MP4 HEVC, Quick Sync"},
        {"mp4-av1", fa::HardwareEncoding::Cpu, "MP4 AV1, CPU"},
        {"webm-vp9", fa::HardwareEncoding::Cpu, "WebM VP9, CPU"},
        {"webm-vp9", fa::HardwareEncoding::Qsv, "WebM VP9, Quick Sync"},
        {"mov-prores", fa::HardwareEncoding::Cpu, "MOV ProRes 422, CPU"},
        {"gif", fa::HardwareEncoding::Cpu, "GIF 640x360, CPU"},
        {"mp4-h264", fa::HardwareEncoding::Cpu, "MP4 H.264 lossless (reference)"},
    };
    // FA_BENCH_KEEP=<dir> keeps the files (e.g. to measure each encoder's PSNR against the lossless reference)
    const std::string keep = std::getenv("FA_BENCH_KEEP") ? std::getenv("FA_BENCH_KEEP") : "";
    const auto dir = keep.empty() ? std::filesystem::temp_directory_path() / "fa_bench_m6" : std::filesystem::path(keep);
    std::filesystem::create_directories(dir);
    fa::Project p = exportProject(media);
    {
        fa::MediaSource a;
        a.id = "src-a";
        a.path = (media / "A_red_1080p30_10s.mp4").string();
        a.duration = fa::Rational(10);
        a.hasAudio = true;
        p.sources.push_back(a);
        fa::Track at{"a1", fa::TrackKind::Audio, "Audio 1"};
        fa::Clip c;
        c.id = "c-audio";
        c.sourceId = "src-a";
        c.duration = fa::Rational(10);
        at.clips.push_back(c);
        p.sequences[0].tracks.push_back(at);
    }
    for (const auto& e : encs) {
        fa::RenderOptions opt;
        opt.format = e.format;
        opt.hardware = e.hw;
        const bool reference = std::string(e.label).find("reference") != std::string::npos;
        opt.qualityLevel = reference ? -1 : 2;   // "Standard" in the export window
        if (reference) opt.encoder.crf = 0;
        const bool gif = std::string(e.format) == "gif";
        if (gif) opt.outputWidth = 640, opt.outputHeight = 360;
        std::string file;
        for (const char ch : std::string(e.label)) file += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
        const auto out = dir / (file + fa::findExportFormat(e.format)->extension);
        const std::string stage = std::string("export M2 composition 1080p30 + audio, 300 frames: ") + e.label;
        try {
            WorkingSetPeak peak;
            const auto t0 = Clock::now();
            const fa::RenderReport r = fa::renderProject(p, "", out, opt);
            const double ms = msSince(t0);
            const size_t most = peak.stopNow();
            char note[220];
            std::snprintf(note, sizeof note, "%.1f fps; %s%s%s; %.1f MB; working set up to %zu MB", r.framesWritten * 1000.0 / ms,
                          r.videoEncoder.c_str(), r.hardwareEncoder ? " (hardware)" : "",
                          r.audioEncoder.empty() ? "" : (" + " + r.audioEncoder).c_str(),
                          std::filesystem::file_size(out) / 1048576.0, most);
            report(stage, ms / double(std::max<int64_t>(1, r.framesWritten)), note);
            g_json["m6_export"].push_back({{"case", e.label}, {"file", out.filename().string()}, {"encoder", r.videoEncoder},
                                           {"hardware", r.hardwareEncoder}, {"fps", r.framesWritten * 1000.0 / ms},
                                           {"bytes", std::filesystem::file_size(out)}, {"working_set_mb", most},
                                           {"note", r.encoderNote}});
        } catch (const std::exception& ex) {
            std::printf("%-66s not available: %s\n", stage.c_str(), ex.what());
            g_json["m6_export"].push_back({{"case", e.label}, {"error", ex.what()}});
        }
    }
    if (keep.empty()) std::filesystem::remove_all(dir);
    std::printf("peak working set of this process: %zu MB\n", workingSetMb(true));
    g_json["peak_working_set_mb"] = workingSetMb(true);
}

// One run per length (`fa-bench <testmedia dir> --m6-memory <seconds>`): export a long timeline (a video track and two
// audio tracks with EQ and ducking, 640x360 H.264 ultrafast + AAC) and report the peak working set. It must not grow
// with the length: nothing buffers the whole timeline (frames and audio are streamed).
void m6Memory(const std::filesystem::path& media, int seconds) {
    fa::Project p = audioProject(media, 2, seconds);
    fa::Sequence& s = p.sequences[0];
    fa::Track v{"v1", fa::TrackKind::Video, "Video 1"};
    for (const auto& c : s.tracks[0].clips) {
        fa::Clip vc = c;
        vc.id = "v" + c.id;
        vc.volumeDb.setConstant(0.0);
        v.clips.push_back(vc);
    }
    for (auto& c : s.tracks[0].clips) c.audio.ducking.enabled = true;
    for (auto& c : s.tracks[1].clips) {
        c.audio.eqEnabled = true;
        c.audio.eqGainsDb = fa::findEqPreset("voice-clarity")->gainsDb;
    }
    s.tracks.insert(s.tracks.begin(), v);
    s.width = 640;
    s.height = 360;
    s.limiter = true;
    fa::RenderOptions opt;
    opt.encoder.preset = "ultrafast";
    const auto out = std::filesystem::temp_directory_path() / "fa_bench_m6_memory.mp4";
    const auto t0 = Clock::now();
    const fa::RenderReport r = fa::renderProject(p, "", out, opt);
    const double sec = msSince(t0) / 1000;
    std::printf("memory run: %d s timeline, %lld frames, %lld audio samples in %.1f s; peak working set %zu MB\n", seconds,
                static_cast<long long>(r.framesWritten), static_cast<long long>(r.audioSamples), sec, workingSetMb(true));
    g_json["m6_memory"] = {{"timeline_seconds", seconds}, {"frames", r.framesWritten}, {"audio_samples", r.audioSamples},
                           {"render_seconds", sec}, {"peak_working_set_mb", workingSetMb(true)}};
    std::filesystem::remove(out);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: fa-bench <testmedia dir> [--frames N] [--json out.json] [--no-export] [--no-gpu] "
                     "[--export-only | --text-only | --effects-only | --motion-only | --m6-only | --m6-memory <seconds>]\n";
        return 1;
    }
    const std::filesystem::path media = argv[1];
    int frames = 120;
    std::string jsonOut;
    bool doExport = true, doGpu = true, exportOnly = false, textOnly = false, effectsOnly = false, motionOnly = false,
         m6Only = false;
    int m6MemorySeconds = 0;
    int exportRounds = 1;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = std::stoi(argv[++i]);
        else if (a == "--json" && i + 1 < argc) jsonOut = argv[++i];
        else if (a == "--no-export") doExport = false;
        else if (a == "--no-gpu") doGpu = false;
        else if (a == "--export-only") exportOnly = true;
        else if (a == "--text-only") textOnly = true;
        else if (a == "--effects-only") effectsOnly = true;
        else if (a == "--motion-only") motionOnly = true;
        else if (a == "--m6-only") m6Only = true;
        else if (a == "--m6-memory" && i + 1 < argc) m6MemorySeconds = std::stoi(argv[++i]);
        else if (a == "--export-rounds" && i + 1 < argc) exportRounds = std::stoi(argv[++i]);
    }
    g_json["frames"] = frames;
    for (const auto& g : fa::listGpuAdapters()) {
        std::printf("adapter: %s (%zu MB)%s%s%s\n", g.name.c_str(), g.dedicatedVideoMemory >> 20, g.software ? " [software]" : "",
                    g.highPerformanceChoice ? " [high performance]" : "", g.powerSavingChoice ? " [power saving]" : "");
        g_json["adapters"].push_back(g.name);
    }
    const auto src = media / "C_testsrc2_1080p30_10s.mp4";   // detailed moving 1080p content
    auto effectBackends = [&] {
        std::vector<Backend> bes;
        bes.push_back({"CPU", fa::makeCpuBackend()});
        if (!doGpu) return bes;
        std::string why;
        if (auto g = fa::makeGpuBackend(&why, true)) bes.push_back({"GPU high-perf", std::move(g)});
        if (auto g = fa::makeGpuBackend(&why, false)) bes.push_back({"GPU power-saving", std::move(g)});
        bes.push_back({"Auto", fa::makeBackend(fa::BackendChoice::Auto)});
        return bes;
    };
    if (m6Only || m6MemorySeconds > 0) {
        if (m6MemorySeconds > 0) m6Memory(media, m6MemorySeconds);
        else m6Benchmarks(media, doExport);
        if (!jsonOut.empty())
            if (std::FILE* f = std::fopen(jsonOut.c_str(), "w")) {
                std::fputs(g_json.dump(2).c_str(), f);
                std::fclose(f);
            }
        return 0;
    }
    if (motionOnly) {
        motionBenchmarks(media, frames, doExport);
        if (!jsonOut.empty())
            if (std::FILE* f = std::fopen(jsonOut.c_str(), "w")) {
                std::fputs(g_json.dump(2).c_str(), f);
                std::fclose(f);
            }
        return 0;
    }
    if (effectsOnly) {
        auto bes = effectBackends();
        effectBenchmarks(media, frames, bes, doExport);
        if (!jsonOut.empty())
            if (std::FILE* f = std::fopen(jsonOut.c_str(), "w")) {
                std::fputs(g_json.dump(2).c_str(), f);
                std::fclose(f);
            }
        return 0;
    }
#ifdef FA_HAS_TEXT
    if (textOnly) {
        std::vector<Backend> bes;
        bes.push_back({"CPU", fa::makeCpuBackend()});
        std::string why;
        if (doGpu)
            if (auto g = fa::makeGpuBackend(&why, true)) bes.push_back({"GPU high-perf", std::move(g)});
        textBenchmarks(media, frames, bes, doExport);
        if (!jsonOut.empty())
            if (std::FILE* f = std::fopen(jsonOut.c_str(), "w")) {
                std::fputs(g_json.dump(2).c_str(), f);
                std::fclose(f);
            }
        return 0;
    }
#endif

    // 1. decoding (CPU, FFmpeg)
    if (!exportOnly) {
        fa::VideoDecoder dec(src);
        dec.frameAt(fa::Time(0));
        const auto t0 = Clock::now();
        for (int n = 1; n <= frames; ++n) dec.frameAt(fa::frameToTime(n, fa::FrameRate::fps30()));
        report("decode 1080p + RGBA (CPU)", msSince(t0) / frames);
    }
    if (!exportOnly) {
        fa::VideoDecoder::Options o;
        o.maxWidth = 960;
        o.maxHeight = 540;
        o.threads = 4;
        fa::VideoDecoder dec(src, o);
        dec.frameAt(fa::Time(0));
        const auto t0 = Clock::now();
        for (int n = 1; n <= frames; ++n) dec.frameAt(fa::frameToTime(n, fa::FrameRate::fps30()));
        report("decode -> 960x540 RGBA (CPU)", msSince(t0) / frames);
    }

    // 1b. hardware decoding (FFmpeg D3D11VA): full size and preview size
    if (!exportOnly) {
        for (int preview = 0; preview < 2; ++preview) {
            fa::VideoDecoder::Options o;
            o.hardware = true;
            if (preview) {
                o.maxWidth = 960;
                o.maxHeight = 540;
            }
            fa::VideoDecoder dec(src, o);
            dec.frameAt(fa::Time(0));
            const auto t0 = Clock::now();
            for (int n = 1; n <= frames; ++n) dec.frameAt(fa::frameToTime(n, fa::FrameRate::fps30()));
            const double ms = msSince(t0) / frames;
            report(std::string(preview ? "decode -> 960x540 RGBA" : "decode 1080p + RGBA") + " (hardware request)", ms,
                   dec.decoderDescription() + (dec.hardwareActive() ? "" : " [not active]"));
        }
#ifdef _WIN32
        // observed decoder engine use (VideoDecode engine of the adapter)
        fa::VideoDecoder::Options o;
        o.hardware = true;
        fa::VideoDecoder dec(src, o);
        GpuEngineUsage u;
        u.start();
        const auto t0 = Clock::now();
        int n = 0;
        while (msSince(t0) < 2000) dec.frameAt(fa::frameToTime(n++ % 290, fa::FrameRate::fps30()));
        std::printf("GPU engine utilisation while decoding %d frames with D3D11VA:\n", n);
        for (const auto& [k, v] : u.stop())
            if (v > 0.05) {
                std::printf("    %-40s %6.1f %%\n", k.c_str(), v);
                g_json["hw_decode_usage"][k] = v;
            }
#endif
    }

    // 2. compositing scenes on every backend
    fa::VideoDecoder dec(src);
    const fa::Image a = dec.frameAt(fa::Rational(2)).image;
    const fa::Image b = fa::VideoDecoder(media / "D_greenscreen_disc_1080p30_10s.mp4").frameAt(fa::Rational(2)).image;
    std::vector<Backend> backends;
    backends.push_back({"CPU", fa::makeCpuBackend()});
    if (doGpu) {
        std::string why;
        if (auto g = fa::makeGpuBackend(&why, true)) backends.push_back({"GPU high-perf", std::move(g)});
        else std::printf("GPU (high performance) unavailable: %s\n", why.c_str());
        if (auto g = fa::makeGpuBackend(&why, false)) backends.push_back({"GPU power-saving", std::move(g)});
        else std::printf("GPU (power saving) unavailable: %s\n", why.c_str());
    }
    struct Scene {
        std::string name;
        int w, h;
        std::function<std::vector<fa::LayerDesc>(int, int)> layers;
    };
    const std::vector<Scene> scenes = {
        {"1 layer 1080p (identity)", 1920, 1080, [&](int w, int h) { return std::vector{fa::clipLayer(fa::Clip{}, fa::Time(0), &a, w, h)}; }},
        {"1 layer -> 960x540 (fit)", 960, 540, [&](int w, int h) { return std::vector{fa::clipLayer(fa::Clip{}, fa::Time(0), &a, w, h)}; }},
        {"3 layers 1080p (opacity 0.5)", 1920, 1080,
         [&](int w, int h) {
             fa::Clip half;
             half.opacity.setConstant(0.5);
             return std::vector{fa::clipLayer(fa::Clip{}, fa::Time(0), &a, w, h), fa::clipLayer(half, fa::Time(0), &b, w, h),
                                fa::clipLayer(half, fa::Time(0), &a, w, h)};
         }},
        {"M2 scene 1080p (3 layers: rotate, masks, Screen/Overlay)", 1920, 1080, [&](int w, int h) { return m2Scene(a, b, w, h); }},
        {"M2 scene -> 960x540 preview", 960, 540, [&](int w, int h) { return m2Scene(a, b, w, h); }},
        {"Zoom transition 1080p (p = 0.5)", 1920, 1080,
         [&](int w, int h) {
             const fa::TransitionLayers tl = fa::evaluateTransition(fa::TransitionRef{"fa.transition.zoom", fa::Time(1)}, 0.5);
             return std::vector{fa::clipLayer(fa::Clip{}, fa::Time(0), &b, w, h, tl.b), fa::clipLayer(fa::Clip{}, fa::Time(0), &a, w, h, tl.a)};
         }},
    };
    for (const auto& sc : scenes) {
        if (exportOnly) break;
        const auto layers = sc.layers(sc.w, sc.h);
        for (auto& [label, be] : backends) {
            auto frame = [&] {
                be->beginFrame(sc.w, sc.h, fa::Rgba8{0, 0, 0, 255});
                for (const auto& l : layers) be->drawLayer(l);
                be->endFrame();
            };
            frame();   // warm-up: allocations, textures
            const auto t0 = Clock::now();
            for (int n = 0; n < frames; ++n) frame();
            const double ms = msSince(t0) / frames;
            std::string note;
            if (label != "CPU") {   // GPU execution time of a frame (excludes upload + readback waits)
                be->setTimingEnabled(true);
                double gms = 0;
                for (int n = 0; n < 10; ++n) {
                    frame();
                    gms += be->lastGpuMilliseconds();
                }
                be->setTimingEnabled(false);
                char buf[96];
                std::snprintf(buf, sizeof buf, "GPU execution %.2f ms", gms / 10);
                note = buf;
            }
            report(sc.name + " [" + label + "]", ms, note);
        }
    }

#ifdef _WIN32
    // 3. observed GPU use while compositing the M2 scene for ~2 s on each GPU backend
    for (auto& [label, be] : backends) {
        if (label == "CPU" || exportOnly) continue;
        const auto layers = m2Scene(a, b, 1920, 1080);
        GpuEngineUsage u;
        u.start();
        const auto t0 = Clock::now();
        int n = 0;
        while (msSince(t0) < 2000) {
            be->beginFrame(1920, 1080, fa::Rgba8{0, 0, 0, 255});
            for (const auto& l : layers) be->drawLayer(l);
            be->endFrame();
            ++n;
        }
        const auto usage = u.stop();
        std::printf("GPU engine utilisation during %d frames of the M2 scene [%s] (%s):\n", n, label.c_str(), be->name().c_str());
        nlohmann::json ju;
        for (const auto& [k, v] : usage)
            if (v > 0.05) {
                std::printf("    %-40s %6.1f %%\n", k.c_str(), v);
                ju[k] = v;
            }
        if (usage.empty()) std::printf("    (performance counters unavailable)\n");
        g_json["gpu_usage"][label] = ju;
    }
#endif

    // 4. encode (x264 medium)
    if (!exportOnly) {
        const auto out = std::filesystem::temp_directory_path() / "fa_bench_encode.mp4";
        fa::EncoderSettings s;
        s.width = 1920;
        s.height = 1080;
        fa::VideoEncoder enc(out, s);
        const auto t0 = Clock::now();
        for (int n = 0; n < frames; ++n) enc.writeFrame(a);
        enc.finish();
        report("encode 1080p x264 medium", msSince(t0) / frames);
        std::filesystem::remove(out);
    }

    // 5. full export of the M2 composition (10 s, 1080p30: decode + composite + encode)
    if (doExport) {
        const auto dir = std::filesystem::temp_directory_path() / "fa_bench_export";
        std::filesystem::create_directories(dir);
        const fa::Project p = exportProject(media);
        const auto projectFile = dir / "bench.json";
        fa::saveProject(p, projectFile);
        std::vector<fa::BackendChoice> order;
        for (int r = 0; r < exportRounds; ++r) {   // alternate the order to even out thermal effects
            order.push_back(r % 2 ? fa::BackendChoice::Gpu : fa::BackendChoice::Cpu);
            order.push_back(r % 2 ? fa::BackendChoice::Cpu : fa::BackendChoice::Gpu);
        }
        for (fa::BackendChoice choice : order) {
            if (choice == fa::BackendChoice::Gpu && !doGpu) continue;
            fa::RenderOptions o;
            o.backend = choice;
            o.includeAudio = false;
            const fa::RenderReport r = fa::renderProject(p, projectFile, dir / "bench.mp4", o);
            char note[160];
            std::snprintf(note, sizeof note, "%.2f s total: decode %.2f, composite %.2f, encode %.2f", r.seconds, r.decodeSeconds,
                          r.compositeSeconds, r.encodeSeconds);
            report("export M2 composition 1080p30, 300 frames [" + r.backend + "]", r.seconds * 1000.0 / r.framesWritten, note);
        }
        std::filesystem::remove_all(dir);
    }

    if (!exportOnly) {
        auto bes = effectBackends();
        effectBenchmarks(media, frames, bes, doExport);
    }

#ifdef FA_HAS_TEXT
    if (!exportOnly) {
        std::vector<Backend> textBackends;
        for (auto& be : backends)
            if (be.label != "GPU power-saving") textBackends.push_back(std::move(be));
        textBenchmarks(media, frames, textBackends, doExport);
    }
#endif
    if (!jsonOut.empty()) {
        if (std::FILE* f = std::fopen(jsonOut.c_str(), "w")) {
            std::fputs(g_json.dump(2).c_str(), f);
            std::fclose(f);
        }
    }
    return 0;
}
