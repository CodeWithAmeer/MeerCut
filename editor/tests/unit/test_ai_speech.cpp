// M7 checkpoint C: offline Speech to Text with whisper.cpp - accuracy against the reference transcripts of real
// recordings (LibriSpeech English, FLEURS Spanish / French / Arabic; testmedia/ai, tools/fetch_m7_testdata.py), caption
// timing against the measured speech in recordings with known pauses, silence (no captions invented), several voices,
// long recordings (chunked), clips played faster / reversed (captions follow the words), cancel, CPU against GPU, and
// the error when a model is missing. Real inference: a test reports "skipped" when a model or recording is absent.
#include "fa/ai/captions.h"
#include "fa/ai/models.h"
#include "fa/ai/speech.h"
#include "fa/media/media.h"
#include "fa/model/edit_ops.h"
#include "fa/model/keyframes.h"
#include "fa/render/audio_mix.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>

using namespace fa;

namespace {

std::filesystem::path models() { return std::filesystem::path(FA_MODELS_DIR); }
std::filesystem::path aiMedia() { return std::filesystem::path(FA_TESTMEDIA_DIR) / "ai"; }

struct ModelsEnv {
    ModelsEnv() { ai::setModelsDirectoryOverride(models()); }
    ~ModelsEnv() { ai::setModelsDirectoryOverride({}); }
};

bool installed(const char* id) {
    const ai::ModelInfo* m = ai::findModel(id);
    return m && ai::modelStatus(*m, false, models()) == ai::ModelStatus::Installed;
}

std::vector<float> load16k(const std::filesystem::path& p) {
    AudioDecoder dec(p, 16000, 1);
    std::vector<float> x(static_cast<size_t>(dec.lengthSamples().value_or(0)));
    for (size_t pos = 0; pos < x.size(); pos += 160000) {
        const int n = static_cast<int>(std::min<size_t>(160000, x.size() - pos));
        dec.read(static_cast<int64_t>(pos), n, x.data() + pos);
    }
    return x;
}

nlohmann::json readJson(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return nlohmann::json::parse(in);
}

struct Utterance {
    std::filesystem::path file;
    std::string text;
    int speaker = 0;
};

std::vector<Utterance> libriSpeech() {
    std::vector<Utterance> u;
    const auto dir = aiMedia() / "librispeech";
    if (!std::filesystem::exists(dir / "transcripts.json")) return u;
    const nlohmann::json all = readJson(dir / "transcripts.json");   // kept alive while iterating (items() refers to it)
    for (const auto& [name, v] : all.items())
        if (std::filesystem::exists(dir / name)) u.push_back({dir / name, v["text"].get<std::string>(), v["speaker"].get<int>()});
    std::sort(u.begin(), u.end(), [](const Utterance& a, const Utterance& b) { return a.file < b.file; });
    return u;
}

std::vector<Utterance> fleurs(const char* lang) {
    std::vector<Utterance> u;
    const auto dir = aiMedia() / "fleurs" / lang;
    if (!std::filesystem::exists(dir / "transcripts.json")) return u;
    const nlohmann::json all = readJson(dir / "transcripts.json");
    for (const auto& [name, v] : all.items())
        if (std::filesystem::exists(dir / name)) u.push_back({dir / name, v["raw"].get<std::string>(), 0});
    std::sort(u.begin(), u.end(), [](const Utterance& a, const Utterance& b) { return a.file < b.file; });
    return u;
}

// John F. Kennedy's inaugural address excerpt (testmedia/ai/jfk.wav, public domain): a second, different voice.
Utterance jfk() {
    return {aiMedia() / "jfk.wav",
            "And so my fellow Americans, ask not what your country can do for you, ask what you can do for your country.", 2};
}

std::string joined(const ai::Transcript& t) {
    std::string s;
    for (const auto& seg : t.segments) s += (s.empty() ? "" : " ") + seg.text;
    return s;
}

double frameRms(const std::vector<float>& x, size_t a, size_t n) {
    double s = 0;
    const size_t b = std::min(x.size(), a + n);
    for (size_t i = a; i < b; ++i) s += double(x[i]) * x[i];
    return b > a ? std::sqrt(s / (b - a)) : 0.0;
}

// Where the speech of a recording starts and ends (seconds): the first / last 20 ms frame above a tenth of the loudest.
std::pair<double, double> speechSpan(const std::vector<float>& x) {
    const size_t f = 320;
    double peak = 0;
    for (size_t a = 0; a + f <= x.size(); a += f) peak = std::max(peak, frameRms(x, a, f));
    double first = -1, last = 0;
    for (size_t a = 0; a + f <= x.size(); a += f)
        if (frameRms(x, a, f) > 0.1 * peak) {
            if (first < 0) first = a / 16000.0;
            last = (a + f) / 16000.0;
        }
    return {std::max(0.0, first), last};
}

// Recordings laid end to end with known pauses: where each one's speech is in the result.
struct Layout {
    std::vector<float> audio;
    std::vector<std::pair<double, double>> speech;   // seconds
    std::vector<std::string> text;
};

Layout concatenate(const std::vector<Utterance>& us, double lead, double gap) {
    Layout l;
    l.audio.assign(static_cast<size_t>(lead * 16000), 0.0f);
    for (const auto& u : us) {
        const auto x = load16k(u.file);
        const auto [a, b] = speechSpan(x);
        const double at = l.audio.size() / 16000.0;
        l.speech.push_back({at + a, at + b});
        l.text.push_back(u.text);
        l.audio.insert(l.audio.end(), x.begin(), x.end());
        l.audio.insert(l.audio.end(), static_cast<size_t>(gap * 16000), 0.0f);
    }
    return l;
}

void writeWav(const std::filesystem::path& p, const std::vector<float>& x) {
    std::ofstream out(p, std::ios::binary);
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(x.size() * 2);
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(16000);
    u32(32000);
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(bytes);
    for (float v : x) {
        const auto s = static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767));
        out.write(reinterpret_cast<const char*>(&s), 2);
    }
}

// The captions of each recording of a layout (by midpoint), and how much caption time lies in the pauses.
struct Alignment {
    std::vector<std::vector<size_t>> perUtterance;
    double captionInPause = 0;   // seconds of captions more than 0.5 s away from any speech
    int stray = 0;               // captions whose midpoint is in no recording's span
};

Alignment align(const std::vector<std::pair<double, double>>& captions, const Layout& l) {
    Alignment a;
    a.perUtterance.resize(l.speech.size());
    for (size_t i = 0; i < captions.size(); ++i) {
        const double mid = (captions[i].first + captions[i].second) / 2;
        bool found = false;
        for (size_t k = 0; k < l.speech.size() && !found; ++k)
            if (mid >= l.speech[k].first - 0.5 && mid <= l.speech[k].second + 0.5) {
                a.perUtterance[k].push_back(i);
                found = true;
            }
        if (!found) ++a.stray;
        // caption time outside every speech span widened by 0.5 s
        const double step = 0.01;
        for (double t = captions[i].first; t < captions[i].second; t += step) {
            bool near = false;
            for (const auto& s : l.speech) near = near || (t >= s.first - 0.5 && t <= s.second + 0.5);
            if (!near) a.captionInPause += step;
        }
    }
    return a;
}

std::vector<std::pair<double, double>> spans(const ai::Transcript& t) {
    std::vector<std::pair<double, double>> s;
    for (const auto& seg : t.segments) s.push_back({seg.start, seg.end});
    return s;
}

ai::TranscribeOptions gpuOptions(const char* model = "whisper-base") {
    ai::TranscribeOptions o;
    o.model = model;
    o.accelerator = ai::Accelerator::Auto;   // the GPU when there is one (the CPU otherwise; the provider says which)
    return o;
}

}  // namespace

TEST(SpeechScoring, NormalisationWerAndCer) {
    EXPECT_EQ(ai::normaliseForScoring("Mr. Quilter's  MANNER, isn't it?"), "mister quilters manner isnt it");
    EXPECT_EQ(ai::normaliseForScoring("In 1984 there were 3 of them"), "in one thousand nine hundred eighty four there were three of them");
    EXPECT_EQ(ai::normaliseForScoring("«¿Qué tal?», dijo él."), "qué tal dijo él");
    // Arabic: the diacritics (harakat) and tatweel do not count
    EXPECT_EQ(ai::normaliseForScoring("\xd9\x83\xd9\x8e\xd8\xaa\xd9\x8e\xd8\xa8\xd9\x8e"), "\xd9\x83\xd8\xaa\xd8\xa8");
    EXPECT_DOUBLE_EQ(ai::wordErrorRate("the cat sat on the mat", "the cat sat on the mat"), 0.0);
    EXPECT_NEAR(ai::wordErrorRate("the cat sat on the mat", "the cat sit on mat"), 2.0 / 6, 1e-12);   // 1 sub + 1 del
    EXPECT_NEAR(ai::wordErrorRate("a b", "a b c d"), 1.0, 1e-12);                                    // 2 insertions
    EXPECT_NEAR(ai::charErrorRate("abcd", "abed"), 0.25, 1e-12);
    EXPECT_DOUBLE_EQ(ai::wordErrorRate("", ""), 0.0);
    EXPECT_DOUBLE_EQ(ai::wordErrorRate("", "hello"), 1.0);
}

TEST(AiSpeech, MissingModelIsAClearErrorAndNothingIsGuessed) {
    const auto empty = std::filesystem::temp_directory_path() / "fa_speech_nomodels";
    std::filesystem::remove_all(empty);
    std::filesystem::create_directories(empty);
    ai::setModelsDirectoryOverride(empty);
    std::vector<float> x(16000, 0.0f);
    try {
        ai::transcribe(x, gpuOptions());
        ADD_FAILURE() << "transcribed without a model";
    } catch (const ai::ModelError& e) {
        EXPECT_NE(std::string(e.what()).find("Whisper"), std::string::npos) << e.what();
    }
    ai::TranscribeOptions o = gpuOptions();
    o.model = "rvm-mobilenetv3";   // not a speech model
    EXPECT_THROW(ai::transcribe(x, o), ai::ModelError);
    ai::setModelsDirectoryOverride({});
    std::filesystem::remove_all(empty);
}

// Accuracy on LibriSpeech (read English): every recording transcribed separately, WER over the whole set.
TEST(AiSpeech, LibriSpeechWordErrorRate) {
    ModelsEnv env;
    auto us = libriSpeech();
    if (!installed("whisper-base") || us.size() < 12) GTEST_SKIP() << "whisper-base or the LibriSpeech recordings missing";
    us.resize(12);
    size_t errors = 0, words = 0;
    std::string provider;
    double audio = 0, seconds = 0;
    for (const auto& u : us) {
        const ai::Transcript t = ai::transcribeFile(u.file, 0, 0, gpuOptions());
        provider = t.provider;
        audio += t.audioSeconds;
        seconds += t.seconds;
        const std::string ref = ai::normaliseForScoring(u.text);
        const double n = static_cast<double>(std::count(ref.begin(), ref.end(), ' ') + 1);   // reference words
        const double wer = ai::wordErrorRate(u.text, joined(t));
        errors += static_cast<size_t>(std::lround(wer * n));
        words += static_cast<size_t>(n);
        EXPECT_EQ(t.language, "en") << u.file.filename();
        EXPECT_LT(wer, 0.35) << u.file.filename() << ": " << joined(t);
    }
    const double wer = static_cast<double>(errors) / words;
    std::printf("LibriSpeech %zu recordings, %.1f s of audio: WER %.3f in %.1f s (%s)\n", us.size(), audio, wer, seconds,
                provider.c_str());
    RecordProperty("wer", std::to_string(wer));
    EXPECT_LT(wer, 0.08);   // whisper-base is published at about 5 % on LibriSpeech test-clean
}

// Spanish, French and Arabic (FLEURS), language detected: detection, and the error rates whisper-base reaches.
TEST(AiSpeech, MultilingualDetectionAndAccuracy) {
    ModelsEnv env;
    if (!installed("whisper-base")) GTEST_SKIP() << "whisper-base missing";
    struct Lang {
        const char* dir;
        const char* code;
        double maxCer;
    };
    for (const Lang l : {Lang{"es_419", "es", 0.12}, Lang{"fr_fr", "fr", 0.20}, Lang{"ar_eg", "ar", 0.45}}) {
        auto us = fleurs(l.dir);
        if (us.size() < 4) {
            ADD_FAILURE() << "FLEURS " << l.dir << " recordings missing";
            continue;
        }
        us.resize(4);
        int detected = 0;
        double cer = 0, wer = 0;
        for (const auto& u : us) {
            const ai::Transcript t = ai::transcribeFile(u.file, 0, 0, gpuOptions());
            detected += t.language == l.code;
            cer += ai::charErrorRate(u.text, joined(t)) / us.size();
            wer += ai::wordErrorRate(u.text, joined(t)) / us.size();
        }
        std::printf("FLEURS %s: language detected %d/%zu, CER %.3f, WER %.3f\n", l.dir, detected, us.size(), cer, wer);
        EXPECT_GE(detected, static_cast<int>(us.size()) - 1) << l.dir;
        EXPECT_LT(cer, l.maxCer) << l.dir;
        // the language chosen by hand is used as given
        ai::TranscribeOptions o = gpuOptions();
        o.language = l.code;
        EXPECT_EQ(ai::transcribeFile(us[0].file, 0, 0, o).language, l.code);
    }
}

// Three speakers with pauses between them: each caption sits on the speech it transcribes (start within half a second
// of the measured speech), nothing is captioned in the pauses, and every speaker's words are there.
TEST(AiSpeech, CaptionTimingFollowsTheSpeechAcrossPausesAndSpeakers) {
    ModelsEnv env;
    const auto all = libriSpeech();
    if (!installed("whisper-base") || all.empty()) GTEST_SKIP() << "whisper-base or LibriSpeech missing";
    if (!std::filesystem::exists(jfk().file)) GTEST_SKIP() << "jfk.wav missing";
    // speaker 1272, Kennedy, speaker 1272 again (two different voices, alternating)
    std::vector<Utterance> us;
    for (const auto& u : all)
        if (load16k(u.file).size() < 12 * 16000 && us.size() < 2) us.push_back(u);
    ASSERT_EQ(us.size(), 2u);
    us.insert(us.begin() + 1, jfk());
    const Layout l = concatenate(us, 1.5, 3.0);
    const ai::Transcript t = ai::transcribe(l.audio, gpuOptions());
    for (const auto& seg : t.segments) std::printf("  [%6.2f - %6.2f] %s\n", seg.start, seg.end, seg.text.c_str());
    const Alignment a = align(spans(t), l);
    EXPECT_EQ(a.stray, 0);
    EXPECT_LT(a.captionInPause, 0.3);
    for (size_t k = 0; k < us.size(); ++k) {
        ASSERT_FALSE(a.perUtterance[k].empty()) << "speaker " << us[k].speaker << " not captioned";
        const auto& first = t.segments[a.perUtterance[k].front()];
        const auto& last = t.segments[a.perUtterance[k].back()];
        std::string text;
        for (size_t i : a.perUtterance[k]) text += " " + t.segments[i].text;
        const double wer = ai::wordErrorRate(l.text[k], text);
        std::printf("speaker %d: speech %.2f-%.2f s, captions %.2f-%.2f s, WER %.3f\n", us[k].speaker, l.speech[k].first,
                    l.speech[k].second, first.start, last.end, wer);
        EXPECT_NEAR(first.start, l.speech[k].first, 0.5);
        EXPECT_NEAR(last.end, l.speech[k].second, 0.8);
        EXPECT_LT(wer, 0.25);
    }
    for (size_t i = 1; i < t.segments.size(); ++i) EXPECT_GE(t.segments[i].start, t.segments[i - 1].start - 1e-9);
}

// Silence and quiet noise: no captions at all (whisper alone tends to write "Thank you." over silence).
TEST(AiSpeech, SilenceAndNoiseGiveNoCaptions) {
    ModelsEnv env;
    if (!installed("whisper-base")) GTEST_SKIP() << "whisper-base missing";
    std::vector<float> x(20 * 16000, 0.0f);
    uint32_t seed = 1;
    for (int i = 0; i < 20 * 16000; ++i) {   // then 20 s of noise at about -60 dBFS
        seed = seed * 1664525u + 1013904223u;
        x.push_back((static_cast<float>(seed >> 8) / 16777216.0f - 0.5f) * 0.0035f);
    }
    const ai::Transcript t = ai::transcribe(x, gpuOptions());
    EXPECT_TRUE(t.segments.empty()) << t.segments.size() << " segments, first: " << (t.segments.empty() ? "" : t.segments[0].text);
}

// Two people talking at once (the second 6 dB quieter): the louder voice is transcribed; recognition of overlapping
// speech is not claimed beyond that.
TEST(AiSpeech, OverlappingVoicesTranscribeTheDominantSpeaker) {
    ModelsEnv env;
    const auto all = libriSpeech();
    if (!installed("whisper-base") || all.size() < 20) GTEST_SKIP() << "whisper-base or LibriSpeech missing";
    if (!std::filesystem::exists(jfk().file)) GTEST_SKIP() << "jfk.wav missing";
    const Utterance* a = &all[0];   // speaker 1272, louder
    const Utterance kennedy = jfk();
    const Utterance* b = &kennedy;   // Kennedy at -6 dB over him
    std::vector<float> x = load16k(a->file);
    const std::vector<float> y = load16k(b->file);
    for (size_t i = 0; i < std::min(x.size(), y.size()); ++i) x[i] += 0.5f * y[i];
    const ai::Transcript t = ai::transcribe(x, gpuOptions());
    const double wer = ai::wordErrorRate(a->text, joined(t));
    std::printf("overlapping voices: WER against the louder speaker %.3f\n", wer);
    EXPECT_LT(wer, 0.6);
    EXPECT_FALSE(t.segments.empty());
}

// A long recording in chunks (a minute here; five by default): the cut is at a pause, no recording is lost at a
// boundary, times keep increasing and the accuracy is that of short recordings.
TEST(AiSpeech, LongRecordingsAreTranscribedInChunksWithoutLosingWords) {
    ModelsEnv env;
    auto all = libriSpeech();
    if (!installed("whisper-base") || all.size() < 20) GTEST_SKIP() << "whisper-base or LibriSpeech missing";
    all.resize(20);
    const Layout l = concatenate(all, 0.5, 0.8);
    ai::TranscribeOptions o = gpuOptions();
    o.chunkSeconds = 60;
    o.language = "en";
    std::vector<double> progress;
    const ai::Transcript t = ai::transcribe(l.audio, o, [&](double p) { progress.push_back(p); });
    ASSERT_GT(l.audio.size() / 16000.0, 150.0);
    const Alignment a = align(spans(t), l);
    for (size_t i = 0; i < t.segments.size(); ++i) {   // a caption in no recording's span: show it
        const double mid = (t.segments[i].start + t.segments[i].end) / 2;
        if (std::none_of(l.speech.begin(), l.speech.end(), [&](const auto& sp) { return mid >= sp.first - 0.5 && mid <= sp.second + 0.5; }))
            std::printf("  stray [%6.2f - %6.2f] %s\n", t.segments[i].start, t.segments[i].end, t.segments[i].text.c_str());
    }
    std::string ref, hyp;
    for (size_t k = 0; k < l.text.size(); ++k) {
        EXPECT_FALSE(a.perUtterance[k].empty()) << "recording " << k << " lost";
        ref += " " + l.text[k];
    }
    hyp = joined(t);
    const double wer = ai::wordErrorRate(ref, hyp);
    std::printf("long recording %.1f s in %.1f s (%s): %zu captions, WER %.3f\n", t.audioSeconds, t.seconds,
                t.provider.c_str(), t.segments.size(), wer);
    EXPECT_LT(wer, 0.10);
    EXPECT_EQ(a.stray, 0);
    for (size_t i = 1; i < t.segments.size(); ++i) EXPECT_GE(t.segments[i].start, t.segments[i - 1].end - 0.05);
    for (size_t i = 0; i < t.segments.size(); ++i) EXPECT_LE(static_cast<int>(t.segments[i].text.size()), o.maxCaptionChars + 16);
    ASSERT_FALSE(progress.empty());
    EXPECT_TRUE(std::is_sorted(progress.begin(), progress.end()));
    EXPECT_DOUBLE_EQ(progress.back(), 1.0);
}

// A clip played at 1.5x (and one reversed) on the timeline: its captions are where the words play - measured against
// the speech in the timeline's own mixed audio, not against the mapping the captions were placed with.
TEST(AiSpeech, CaptionsOfRetimedAndReversedClipsFollowTheWords) {
    ModelsEnv env;
    const auto all = libriSpeech();
    if (!installed("whisper-base") || all.size() < 3) GTEST_SKIP() << "whisper-base or LibriSpeech missing";
    const Layout l = concatenate({all[0], all[1], all[2]}, 1.0, 2.5);
    const auto dir = std::filesystem::temp_directory_path() / "fa_speech_retime";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto wav = dir / "speech.wav";
    writeWav(wav, l.audio);
    Project p;
    MediaSource s;
    s.id = "speech";
    const std::u8string u8 = wav.u8string();
    s.path = std::string(u8.begin(), u8.end());
    s.hasAudio = true;
    s.duration = Rational::fromDouble(l.audio.size() / 16000.0, 16000);
    p.sources.push_back(s);
    Sequence seq;
    seq.id = "seq";
    seq.frameRate = {25, 1};
    Track a;
    a.id = "a1";
    a.kind = TrackKind::Audio;
    Clip c;
    c.id = "c1";
    c.sourceId = "speech";
    c.sourceIn = Time(0);
    c.timelineStart = Time(2);
    c.speed.speed = Rational(3, 2);
    c.duration = Rational::fromDouble(l.audio.size() / 16000.0 / 1.5, 1000);
    a.clips.push_back(c);
    seq.tracks.push_back(a);
    p.sequences.push_back(seq);
    ai::TranscribeOptions o = gpuOptions();
    o.language = "en";
    const ai::CaptionResult r = ai::captionsForClip(p, c, {}, o);
    ASSERT_GE(r.captions.size(), 3u);
    // the speech as the timeline plays it
    const int64_t end = timeToSample(c.timelineEnd(), 16000) + 16000;
    std::vector<float> mix(static_cast<size_t>(end));
    {
        AudioMixer mixer({}, 16000, 1);   // closed again before the file is removed
        for (int64_t pos = 0; pos < end; pos += 16000)
            mixer.mix(p, p.sequences[0], pos, static_cast<int>(std::min<int64_t>(16000, end - pos)), mix.data() + pos);
    }
    Layout played;
    played.audio = mix;
    // the three spans of speech in the mix: runs of loud 20 ms frames separated by more than a second of quiet
    double peak = 0;
    for (size_t f = 0; f + 320 <= mix.size(); f += 320) peak = std::max(peak, frameRms(mix, f, 320));
    double runStart = -1, lastLoud = -10;
    for (size_t f = 0; f + 320 <= mix.size(); f += 320) {
        const double t = f / 16000.0;
        if (frameRms(mix, f, 320) > 0.1 * peak) {
            if (runStart < 0 || t - lastLoud > 1.0) {
                if (runStart >= 0) played.speech.push_back({runStart, lastLoud});
                runStart = t;
            }
            lastLoud = t + 0.02;
        }
    }
    if (runStart >= 0) played.speech.push_back({runStart, lastLoud});
    ASSERT_EQ(played.speech.size(), 3u);
    played.text = l.text;
    std::vector<std::pair<double, double>> caps;
    for (const auto& cap : r.captions) caps.push_back({cap.start.toDouble(), cap.end.toDouble()});
    const Alignment al = align(caps, played);
    EXPECT_EQ(al.stray, 0);
    for (size_t k = 0; k < 3; ++k) {
        ASSERT_FALSE(al.perUtterance[k].empty());
        const double start = caps[al.perUtterance[k].front()].first;
        std::printf("1.5x: speech plays from %.2f s, its caption from %.2f s\n", played.speech[k].first, start);
        EXPECT_NEAR(start, played.speech[k].first, 0.5);
    }
    // reversed: the same captions mirrored inside the clip (the last words play first)
    Clip rev = c;
    rev.speed.reverse = true;
    const ai::CaptionResult rr = ai::captionsForClip(p, rev, {}, o);
    ASSERT_EQ(rr.captions.size(), r.captions.size());
    const double t0 = c.timelineStart.toDouble(), t1 = c.timelineEnd().toDouble();
    for (size_t i = 0; i < r.captions.size(); ++i) {
        const Caption& f = r.captions[i];
        const Caption& b = rr.captions[r.captions.size() - 1 - i];
        EXPECT_NEAR(b.start.toDouble(), t0 + t1 - f.end.toDouble(), 0.002);
        EXPECT_EQ(b.text, f.text);
    }
    std::filesystem::remove_all(dir);
}

// The timeline from In to Out (what the viewer hears, all audio tracks mixed): two recordings on two audio tracks, the
// second muted, the range starting inside the first pause - captions at timeline times, only the audible track's words.
TEST(AiSpeech, TimelineRangeTranscribesTheMixAtTimelineTimes) {
    ModelsEnv env;
    const auto all = libriSpeech();
    if (!installed("whisper-base") || all.size() < 3 || !std::filesystem::exists(jfk().file)) GTEST_SKIP() << "media missing";
    const Layout l = concatenate({all[0], all[1]}, 1.0, 2.0);
    const auto dir = std::filesystem::temp_directory_path() / "fa_speech_range";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    writeWav(dir / "a.wav", l.audio);
    Project p;
    for (const char* id : {"a", "k"}) {
        MediaSource s;
        s.id = id;
        const std::u8string u8 = (std::string(id) == "a" ? dir / "a.wav" : jfk().file).u8string();
        s.path = std::string(u8.begin(), u8.end());
        s.hasVideo = false;
        s.hasAudio = true;
        s.duration = Rational::fromDouble(std::string(id) == "a" ? l.audio.size() / 16000.0 : 11.0, 1000);
        p.sources.push_back(s);
    }
    Sequence seq;
    seq.id = "seq";
    seq.frameRate = FrameRate(25);
    Track ta{"a1", TrackKind::Audio, "Audio 1"}, tk{"a2", TrackKind::Audio, "Audio 2"};
    Clip c;
    c.id = "ca";
    c.sourceId = "a";
    c.timelineStart = Time(3);
    c.duration = Rational::fromDouble(l.audio.size() / 16000.0, 25);
    ta.clips.push_back(c);
    Clip k;
    k.id = "ck";
    k.sourceId = "k";
    k.timelineStart = Time(3);
    k.duration = Time(10);
    tk.clips.push_back(k);
    tk.muted = true;   // what the viewer does not hear is not captioned
    seq.tracks.push_back(ta);
    seq.tracks.push_back(tk);
    p.sequences.push_back(seq);
    ai::TranscribeOptions o = gpuOptions();
    o.language = "en";
    const Time from = Rational(2), to = c.timelineEnd();
    const ai::CaptionResult r = ai::captionsForRange(p, p.sequences[0], from, to, {}, o);
    ASSERT_FALSE(r.captions.empty());
    std::string text;
    for (const auto& cap : r.captions) {
        std::printf("  [%6.2f - %6.2f] %s\n", cap.start.toDouble(), cap.end.toDouble(), cap.text.c_str());
        EXPECT_GE(cap.start, from);
        EXPECT_LE(cap.end, to);
        text += " " + cap.text;
    }
    // the first words start where the first recording's speech plays on the timeline
    EXPECT_NEAR(r.captions.front().start.toDouble(), 3.0 + l.speech[0].first, 0.5);
    EXPECT_LT(ai::wordErrorRate(l.text[0] + " " + l.text[1], text), 0.2);
    EXPECT_EQ(text.find("Americans"), std::string::npos);   // the muted track
    std::filesystem::remove_all(dir);
}

// Cancel stops within a moment and keeps what was done.
TEST(AiSpeech, CancelStopsAndKeepsWhatWasDone) {
    ModelsEnv env;
    auto all = libriSpeech();
    if (!installed("whisper-base") || all.size() < 12) GTEST_SKIP() << "whisper-base or LibriSpeech missing";
    all.resize(12);
    const Layout l = concatenate(all, 0.5, 0.5);
    std::atomic<bool> cancel{false};
    ai::TranscribeOptions o = gpuOptions();
    o.chunkSeconds = 30;
    const auto t0 = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point asked;
    const ai::Transcript t = ai::transcribe(l.audio, o, [&](double p) {
        if (p > 0.2 && !cancel) {
            asked = std::chrono::steady_clock::now();
            cancel = true;
        }
    }, &cancel);
    const double after = std::chrono::duration<double>(std::chrono::steady_clock::now() - asked).count();
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("cancel: stopped %.2f s after the request (%.1f s in all, %zu captions kept)\n", after, total, t.segments.size());
    EXPECT_TRUE(t.cancelled);
    EXPECT_LT(after, 10.0);
    ASSERT_FALSE(t.segments.empty());
    EXPECT_LT(t.segments.back().end, l.audio.size() / 16000.0 * 0.8);
}

// The GPU (Vulkan) and the CPU produce the same words and times. whisper-tiny: in a Debug build ggml's CPU code is
// unoptimised (minutes for base), and the claim - both devices run the same model to the same result - is the same.
TEST(AiSpeech, CpuAgainstGpu) {
    ModelsEnv env;
    const auto all = libriSpeech();
    if (!installed("whisper-tiny") || all.empty()) GTEST_SKIP() << "whisper-tiny or LibriSpeech missing";
    const auto& caps = ai::aiCapabilities();
    if (caps.gpus.empty() || !caps.vulkan) GTEST_SKIP() << "no Vulkan GPU for the comparison";
    ai::TranscribeOptions cpu = gpuOptions("whisper-tiny"), gpu = gpuOptions("whisper-tiny");
    cpu.accelerator = ai::Accelerator::Cpu;
    gpu.accelerator = ai::Accelerator::Gpu;
    cpu.language = gpu.language = "en";
    const ai::Transcript a = ai::transcribeFile(all[0].file, 0, 0, cpu);
    const ai::Transcript b = ai::transcribeFile(all[0].file, 0, 0, gpu);
    std::printf("CPU %.2f s (%s), GPU %.2f s (%s) for %.1f s of audio\n", a.seconds, a.provider.c_str(), b.seconds,
                b.provider.c_str(), a.audioSeconds);
    EXPECT_NE(a.provider.find("CPU"), std::string::npos);
    EXPECT_NE(b.provider.find("Vulkan"), std::string::npos);
    EXPECT_LT(ai::wordErrorRate(joined(a), joined(b)), 0.1);
    ASSERT_EQ(a.segments.empty(), b.segments.empty());
    EXPECT_NEAR(a.segments.front().start, b.segments.front().start, 0.3);
}

// Captions from speech are ordinary captions: one undo step puts them on a subtitle track, snapped to frames.
TEST(AiSpeech, CaptionsGoOnASubtitleTrackInOneUndoStep) {
    Project p;
    Sequence seq;
    seq.id = "seq";
    seq.frameRate = {25, 1};
    p.sequences.push_back(seq);
    std::vector<Caption> caps(2);
    caps[0].start = Rational::fromDouble(1.013, 1000);
    caps[0].end = Rational::fromDouble(2.5, 1000);
    caps[0].text = "Hello there.";
    caps[1].start = Rational::fromDouble(3.0, 1000);
    caps[1].end = Rational::fromDouble(4.27, 1000);
    caps[1].text = "¿Qué tal?";
    std::vector<std::string> tracks;
    auto cmd = edit::addCaptions(p, "seq", caps, Track::defaultSubtitleStyle(), Track().subtitleY, &tracks);
    cmd->apply(p);
    ASSERT_EQ(tracks.size(), 1u);
    const Track* tr = nullptr;
    for (const auto& t : p.sequences[0].tracks)
        if (t.id == tracks[0]) tr = &t;
    ASSERT_TRUE(tr);
    ASSERT_EQ(tr->clips.size(), 2u);
    EXPECT_EQ(tr->clips[0].timelineStart, Time(1));   // 1.013 s snapped to frame 25
    ASSERT_TRUE(tr->clips[1].title);
    EXPECT_EQ(tr->clips[1].title->text, "¿Qué tal?");
    cmd->revert(p);
    EXPECT_TRUE(p.sequences[0].tracks.empty());
}
