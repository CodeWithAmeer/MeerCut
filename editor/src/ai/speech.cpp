#include "fa/ai/speech.h"

#include "fa/media/media.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <ggml-backend.h>
#include <whisper.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace fa::ai {

namespace {

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

double rms(const float* x, size_t n) {
    if (!n) return 0;
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += double(x[i]) * x[i];
    return std::sqrt(s / n);
}

// whisper.cpp reads the model through this loader (a wide path, so any folder name works)
struct Loader {
    std::ifstream in;
    static size_t read(void* ctx, void* out, size_t n) {
        auto* l = static_cast<Loader*>(ctx);
        l->in.read(static_cast<char*>(out), static_cast<std::streamsize>(n));
        return static_cast<size_t>(l->in.gcount());
    }
    static bool eof(void* ctx) { return static_cast<Loader*>(ctx)->in.eof(); }
    static void close(void* ctx) { static_cast<Loader*>(ctx)->in.close(); }
};

struct Context {
    whisper_context* ctx = nullptr;
    std::string provider, fallback;
    ~Context() {
        if (ctx) whisper_free(ctx);
    }
};

// whisper.cpp and ggml log every layer and device at load time; only their warnings and errors reach stderr
void quietLogs() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto cb = [](ggml_log_level level, const char* text, void*) {
            if (level >= GGML_LOG_LEVEL_WARN && level != GGML_LOG_LEVEL_CONT) std::fputs(text, stderr);
        };
        whisper_log_set(cb, nullptr);
        ggml_log_set(cb, nullptr);
    });
}

std::unique_ptr<Context> openModel(const TranscribeOptions& o) {
    quietLogs();
    const ModelInfo* m = findModel(o.model);
    if (!m || m->runtime != ModelRuntime::Whisper) throw ModelError("unknown speech model \"" + o.model + "\"");
    requireModel(*m);
    auto c = std::make_unique<Context>();
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = false;
    // the preferred GPU among ggml's GPU devices (matched by name; ggml lists Vulkan devices itself)
    if (o.accelerator != Accelerator::Cpu) {
        const auto& caps = aiCapabilities();
        const int pref = preferredVulkanGpu();
        const std::string want = pref >= 0 ? caps.gpus[static_cast<size_t>(pref)].name : std::string();
        int gpuIndex = -1, firstGpu = -1, k = 0;
        std::string desc;
        for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
            ggml_backend_dev_t d = ggml_backend_dev_get(i);
            if (ggml_backend_dev_type(d) != GGML_BACKEND_DEVICE_TYPE_GPU) continue;
            const std::string dd = ggml_backend_dev_description(d);
            if (firstGpu < 0) {
                firstGpu = k;
                desc = dd;
            }
            if (!want.empty() && dd.find(want) != std::string::npos) {
                gpuIndex = k;
                desc = dd;
            }
            ++k;
        }
        if (gpuIndex < 0) gpuIndex = firstGpu;
        if (gpuIndex >= 0) {
            cp.use_gpu = true;
            cp.gpu_device = gpuIndex;
            c->provider = "Vulkan: " + desc;
        } else if (o.accelerator == Accelerator::Gpu) {
            c->fallback = "no Vulkan GPU for whisper.cpp; using the CPU";
        }
    }
    Loader l;
    l.in.open(modelFilePath(*m, 0), std::ios::binary);
    if (!l.in) throw ModelError("cannot read the speech model " + m->name);
    whisper_model_loader wl{&l, &Loader::read, &Loader::eof, &Loader::close};
    c->ctx = whisper_init_with_params(&wl, cp);
    if (!c->ctx && cp.use_gpu) {   // the GPU could not take the model: the CPU
        c->fallback = "the GPU could not load the speech model; using the CPU";
        l.in.close();
        l.in.clear();
        l.in.open(modelFilePath(*m, 0), std::ios::binary);
        cp.use_gpu = false;
        c->ctx = whisper_init_with_params(&wl, cp);
    }
    if (!c->ctx) throw ModelError("whisper.cpp could not load " + m->name);
    if (!cp.use_gpu)
        c->provider = "CPU (" + std::to_string(std::max(1, std::min(8, aiCapabilities().cpuThreads / 2))) + " threads)";
    return c;
}

struct RunState {
    const std::function<void(double)>* progress = nullptr;
    const std::atomic<bool>* cancel = nullptr;
    double base = 0, span = 1;
};

// Splits a segment's text at spaces into pieces of at most maxChars (balanced: as many pieces as needed, about equal
// length) and times them: each inner boundary at the quietest 20 ms frame within 0.35 s of its proportional position.
std::vector<SpeechSegment> splitSegment(const SpeechSegment& s, int maxChars, const std::vector<double>& level, size_t frame) {
    const std::string& t = s.text;
    if (maxChars <= 0 || static_cast<int>(t.size()) <= maxChars) return {s};
    std::vector<std::string> words;
    {
        std::istringstream in(t);
        std::string w;
        while (in >> w) words.push_back(w);
    }
    const size_t pieces = (t.size() + static_cast<size_t>(maxChars) - 1) / static_cast<size_t>(maxChars);
    const double target = static_cast<double>(t.size()) / static_cast<double>(pieces);
    std::vector<std::string> texts;
    std::string cur;
    for (const std::string& w : words) {
        const size_t len = cur.empty() ? w.size() : cur.size() + 1 + w.size();
        // close the piece when it would exceed the limit, or has reached its share and more pieces are expected
        if (!cur.empty() && (static_cast<int>(len) > maxChars || (cur.size() >= target && texts.size() + 1 < pieces))) {
            texts.push_back(cur);
            cur = w;
        } else {
            cur = cur.empty() ? w : cur + " " + w;
        }
    }
    if (!cur.empty()) texts.push_back(cur);
    std::vector<SpeechSegment> out;
    size_t chars = 0, total = 0;
    for (const auto& x : texts) total += x.size();
    double prevEnd = s.start;
    for (size_t i = 0; i < texts.size(); ++i) {
        chars += texts[i].size();
        double end = s.end;
        if (i + 1 < texts.size()) {
            const double guess = s.start + (s.end - s.start) * static_cast<double>(chars) / static_cast<double>(total);
            const auto lo = static_cast<size_t>(std::max(prevEnd + 0.2, guess - 0.35) * 16000 / frame);
            const auto hi = static_cast<size_t>(std::min(s.end - 0.2, guess + 0.35) * 16000 / frame);
            size_t best = static_cast<size_t>(guess * 16000 / frame);
            for (size_t k = lo; k <= hi && k < level.size(); ++k)
                if (level[k] < level[std::min(best, level.size() - 1)]) best = k;
            end = std::clamp(best * frame / 16000.0, prevEnd + 0.1, s.end);
        }
        SpeechSegment p = s;
        p.start = prevEnd;
        p.end = end;
        p.text = texts[i];
        out.push_back(std::move(p));
        prevEnd = end;
    }
    return out;
}

// One whisper_full pass over samples; appends segments offset by `offset` seconds.
void runChunk(Context& c, const float* x, size_t n, double offset, const TranscribeOptions& o, RunState& rs,
              Transcript& out) {
    whisper_full_params fp = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    fp.beam_search.beam_size = 5;
    fp.n_threads = std::max(1, std::min(8, aiCapabilities().cpuThreads / 2));
    fp.language = o.language.empty() ? "auto" : o.language.c_str();
    fp.translate = false;
    fp.no_context = false;
    fp.print_progress = false;
    fp.print_realtime = false;
    fp.print_timestamps = false;
    fp.print_special = false;
    // whisper's own sentence segments: their edges come from its timestamp tokens, which are far steadier than the
    // per-token times a length-limited split (max_len) uses - those jump across pauses. Long sentences are split into
    // captions below, at the pauses between words.
    fp.token_timestamps = false;
    fp.max_len = 0;
    fp.split_on_word = false;
    fp.suppress_nst = true;
    fp.no_speech_thold = 0.6f;
    fp.progress_callback = [](whisper_context*, whisper_state*, int p, void* user) {
        auto* s = static_cast<RunState*>(user);
        if (s->progress && *s->progress) (*s->progress)(s->base + s->span * p / 100.0);
    };
    fp.progress_callback_user_data = &rs;
    fp.abort_callback = [](void* user) {
        const auto* s = static_cast<RunState*>(user);
        return s->cancel && s->cancel->load();
    };
    fp.abort_callback_user_data = &rs;
    if (whisper_full(c.ctx, fp, x, static_cast<int>(n)) != 0) {
        if (rs.cancel && rs.cancel->load()) {
            out.cancelled = true;
            return;
        }
        throw ModelError("speech recognition failed");
    }
    if (out.language.empty()) out.language = whisper_lang_str(whisper_full_lang_id(c.ctx));
    // The recording's own levels decide what is silence (a quiet recording is not silent): 20 ms frames, the noise
    // floor = the 10th percentile of their RMS. A frame is speech when it is clearly above the floor.
    const size_t frame = 320;
    std::vector<double> level((n + frame - 1) / frame);
    for (size_t k = 0; k < level.size(); ++k) level[k] = rms(x + k * frame, std::min(frame, n - k * frame));
    std::vector<double> sorted = level;
    std::sort(sorted.begin(), sorted.end());
    const double floorRms = sorted.empty() ? 0.0 : sorted[sorted.size() / 10];
    const double speechMin = std::max(o.silenceRms, 3.0 * floorRms);
    static const bool debug = std::getenv("MEERCUT_STT_DEBUG") != nullptr;   // every segment and why it is dropped
    const int count = whisper_full_n_segments(c.ctx);
    for (int i = 0; i < count; ++i) {
        SpeechSegment s;
        s.start = whisper_full_get_segment_t0(c.ctx, i) * 0.01;
        s.end = whisper_full_get_segment_t1(c.ctx, i) * 0.01;
        s.text = trim(whisper_full_get_segment_text(c.ctx, i));
        s.noSpeech = whisper_full_get_segment_no_speech_prob(c.ctx, i);
        const size_t fa = std::min(level.size(), static_cast<size_t>(std::max(0.0, s.start) * 16000 / frame));
        const size_t fb = std::min(level.size(), static_cast<size_t>(std::max(0.0, s.end) * 16000 / frame + 1));
        // the segment's speech frames: whisper places segment edges loosely (often in the pause before the words)
        size_t first = fb, last = fa;
        double peak = 0;
        for (size_t k = fa; k < fb; ++k) peak = std::max(peak, level[k]);
        const double on = std::max(speechMin, 0.1 * peak);
        for (size_t k = fa; k < fb; ++k)
            if (level[k] >= on) {
                first = std::min(first, k);
                last = k;
            }
        if (debug)
            std::fprintf(stderr, "whisper segment %.2f-%.2f peak %.4f floor %.4f no-speech %.2f%s: %s\n", s.start, s.end, peak,
                         floorRms, s.noSpeech, first >= fb ? " (silence)" : "", s.text.c_str());
        if (s.text.empty() || s.end <= s.start) continue;
        if (s.text.front() == '[' || s.text.front() == '(') continue;   // [BLANK_AUDIO], (music): not speech
        if (first >= fb || peak < speechMin) continue;   // whisper sometimes writes text over silence: the audio decides
        // speech rises and falls with syllables and pauses; a steady sound (noise, hum) does not: its loud frames are
        // barely louder than its quiet ones
        {
            std::vector<double> seg(level.begin() + static_cast<std::ptrdiff_t>(fa), level.begin() + static_cast<std::ptrdiff_t>(fb));
            std::sort(seg.begin(), seg.end());
            const double hi = seg[seg.size() * 9 / 10], lo = seg[seg.size() * 3 / 10];
            if (debug) std::fprintf(stderr, "  modulation %.2f\n", hi / std::max(1e-12, lo));
            if (seg.size() >= 10 && hi < 2.0 * lo) continue;
        }
        // whisper often stretches a segment over the pause to the next one, so its end can hold the first syllable of
        // the next sentence (or its start the last one of the previous): runs of speech (gaps under 0.5 s merged), and
        // a short run at an edge that a long pause separates from the rest belongs to the neighbour
        {
            std::vector<std::pair<size_t, size_t>> runs;   // [first, last] frames
            for (size_t k = first; k <= last; ++k) {
                if (level[k] < on) continue;
                if (!runs.empty() && k - runs.back().second <= 25) runs.back().second = k;
                else runs.push_back({k, k});
            }
            auto shortRun = [&](const std::pair<size_t, size_t>& r) { return (r.second - r.first + 1) * frame < 9600; };   // 0.6 s
            auto longGap = [&](size_t a, size_t b) { return (b - a) * frame >= 12800; };                              // 0.8 s
            if (runs.size() >= 2 && shortRun(runs.back()) && longGap(runs[runs.size() - 2].second, runs.back().first) &&
                (fb - runs.back().second) * frame <= 4000)
                runs.pop_back();
            if (runs.size() >= 2 && shortRun(runs.front()) && longGap(runs.front().second, runs[1].first) &&
                (runs.front().first - fa) * frame <= 4000)
                runs.erase(runs.begin());
            first = runs.front().first;
            last = runs.back().second;
        }
        // snap the edges to the speech (only ever inwards), keeping 0.1 s before and 0.15 s after the words
        s.start = std::max(s.start, first * frame / 16000.0 - 0.1);
        s.end = std::min(s.end, (last + 1) * frame / 16000.0 + 0.15);
        if (s.end <= s.start) continue;
        // a sentence longer than a caption: pieces of at most maxCaptionChars at word boundaries, each boundary at the
        // quietest moment near its share of the time (by characters)
        for (SpeechSegment& piece : splitSegment(s, o.maxCaptionChars, level, frame)) {
            piece.start += offset;
            piece.end += offset;
            out.segments.push_back(std::move(piece));
        }
    }
}

// The quietest 0.5 s within [from, to) (samples): a cut there does not split a word.
size_t quietestPoint(const std::vector<float>& x, size_t from, size_t to) {
    const size_t win = 8000;
    size_t best = (from + to) / 2;
    double bestE = 1e30;
    for (size_t s = from; s + win <= std::min(to, x.size()); s += 1600) {
        const double e = rms(&x[s], win);
        if (e < bestE) {
            bestE = e;
            best = s + win / 2;
        }
    }
    return best;
}

std::wstring lowerAndStrip(const std::wstring& in) {
    std::wstring w = in;
    if (!w.empty()) CharLowerBuffW(w.data(), static_cast<DWORD>(w.size()));
    std::vector<WORD> types(w.size());
    if (!w.empty()) GetStringTypeW(CT_CTYPE1, w.data(), static_cast<int>(w.size()), types.data());
    std::wstring out;
    for (size_t i = 0; i < w.size(); ++i) {
        const wchar_t ch = w[i];
        if ((ch >= 0x064B && ch <= 0x0652) || ch == 0x0640 || ch == 0x0670) continue;   // Arabic diacritics, tatweel
        if (ch == L'\'' || ch == 0x2019) continue;                                      // apostrophes join words
        if (types[i] & (C1_PUNCT | C1_CNTRL)) out += L' ';
        else if (types[i] & C1_SPACE) out += L' ';
        else out += ch;
    }
    return out;
}

std::vector<std::wstring> words(const std::wstring& s) {
    std::vector<std::wstring> w;
    std::wistringstream in(s);
    std::wstring t;
    while (in >> t) w.push_back(t);
    return w;
}

std::wstring numberWords(int n) {   // 0 .. 9999 in English words (LibriSpeech references spell numbers)
    static const wchar_t* ones[] = {L"zero", L"one", L"two", L"three", L"four", L"five", L"six", L"seven", L"eight",
                                    L"nine", L"ten", L"eleven", L"twelve", L"thirteen", L"fourteen", L"fifteen",
                                    L"sixteen", L"seventeen", L"eighteen", L"nineteen"};
    static const wchar_t* tens[] = {L"", L"", L"twenty", L"thirty", L"forty", L"fifty", L"sixty", L"seventy", L"eighty", L"ninety"};
    if (n < 20) return ones[n];
    if (n < 100) return std::wstring(tens[n / 10]) + (n % 10 ? std::wstring(L" ") + ones[n % 10] : L"");
    if (n < 1000) return std::wstring(ones[n / 100]) + L" hundred" + (n % 100 ? L" " + numberWords(n % 100) : L"");
    return numberWords(n / 1000) + L" thousand" + (n % 1000 ? L" " + numberWords(n % 1000) : L"");
}

std::wstring expandEnglish(const std::vector<std::wstring>& in) {
    std::wstring out;
    for (const auto& w : in) {
        std::wstring x = w;
        if (x == L"mr") x = L"mister";
        else if (x == L"mrs") x = L"missus";
        else if (x == L"dr") x = L"doctor";
        else if (x == L"st") x = L"saint";
        else if (!x.empty() && std::all_of(x.begin(), x.end(), iswdigit) && x.size() <= 4) x = numberWords(std::stoi(x));
        out += (out.empty() ? L"" : L" ") + x;
    }
    return out;
}

template <typename T>
size_t editDistance(const std::vector<T>& a, const std::vector<T>& b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

}  // namespace

Transcript transcribe(const std::vector<float>& x, const TranscribeOptions& o, const std::function<void(double)>& progress,
                      const std::atomic<bool>* cancel) {
    const auto t0 = std::chrono::steady_clock::now();
    Transcript out;
    out.audioSeconds = x.size() / 16000.0;
    auto c = openModel(o);
    out.provider = c->provider;
    out.fallback = c->fallback;
    if (o.language != "auto") out.language = o.language;
    RunState rs{&progress, cancel, 0, 1};
    const size_t chunk = static_cast<size_t>(std::max(30.0, o.chunkSeconds) * 16000);
    size_t start = 0;
    while (start < x.size() && !out.cancelled) {
        size_t end = std::min(x.size(), start + chunk);
        if (end < x.size()) end = quietestPoint(x, end - 160000 > start ? end - 160000 : start, std::min(x.size(), end + 160000));
        rs.base = static_cast<double>(start) / x.size();
        rs.span = static_cast<double>(end - start) / x.size();
        runChunk(*c, x.data() + start, end - start, start / 16000.0, o, rs, out);
        if (cancel && cancel->load()) out.cancelled = true;
        start = end;
    }
    if (progress && !out.cancelled) progress(1.0);
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

Transcript transcribeFile(const std::filesystem::path& media, double from, double to, const TranscribeOptions& o,
                          const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    AudioDecoder dec(media, 16000, 1);
    if (to <= from) to = dec.lengthSamples() ? *dec.lengthSamples() / 16000.0 : from;
    const int64_t a = static_cast<int64_t>(std::llround(from * 16000)), b = static_cast<int64_t>(std::llround(to * 16000));
    std::vector<float> x(static_cast<size_t>(std::max<int64_t>(0, b - a)));
    for (int64_t pos = 0; pos < static_cast<int64_t>(x.size()); pos += 160000) {
        const int n = static_cast<int>(std::min<int64_t>(160000, static_cast<int64_t>(x.size()) - pos));
        dec.read(a + pos, n, x.data() + pos);
    }
    return transcribe(x, o, progress, cancel);
}

std::vector<std::pair<std::string, std::string>> speechLanguages() {
    std::vector<std::pair<std::string, std::string>> l;
    for (int i = 0; i <= whisper_lang_max_id(); ++i) {
        std::string name = whisper_lang_str_full(i);
        if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        l.push_back({whisper_lang_str(i), name});
    }
    return l;
}

std::string normaliseForScoring(const std::string& utf8) {
    const auto w = words(lowerAndStrip(toWide(utf8)));
    return toUtf8(expandEnglish(w));
}

double wordErrorRate(const std::string& reference, const std::string& hypothesis) {
    const auto r = words(toWide(normaliseForScoring(reference))), h = words(toWide(normaliseForScoring(hypothesis)));
    return r.empty() ? (h.empty() ? 0.0 : 1.0) : static_cast<double>(editDistance(r, h)) / r.size();
}

double charErrorRate(const std::string& reference, const std::string& hypothesis) {
    auto strip = [](const std::string& s) {
        std::wstring w = toWide(normaliseForScoring(s));
        std::vector<wchar_t> v;
        for (wchar_t ch : w)
            if (ch != L' ') v.push_back(ch);
        return v;
    };
    const auto r = strip(reference), h = strip(hypothesis);
    return r.empty() ? (h.empty() ? 0.0 : 1.0) : static_cast<double>(editDistance(r, h)) / r.size();
}

}  // namespace fa::ai
