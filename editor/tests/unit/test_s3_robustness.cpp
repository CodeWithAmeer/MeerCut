// Stage 3 robustness: what a user, a camera or a damaged disk can hand MeerCut. Variable-frame-rate media and streams
// whose timestamps do not start at zero show the right frames; truncated, corrupted, garbage and empty files are
// decoded as far as they go or refused with an error; damaged project, subtitle and LUT files are refused cleanly
// (thousands of mutations: nothing crashes or hangs); a long project (thousands of clips) stays fast.
// Media: tools/make_s3_testmedia.ps1 (tests skip when it has not been run).
#include "fa/media/media.h"
#include "fa/model/edit_ops.h"
#include "fa/model/serialize.h"
#include "fa/model/subtitles.h"
#include "fa/render/backend.h"
#include "fa/render/effects.h"
#include "fa/render/lut.h"
#include "fa/render/renderer.h"
#include "fa/render/timeline_eval.h"

#include "../support/framecode.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <random>
#include <string>

using namespace fa;

namespace {

std::filesystem::path media(const char* name) { return std::filesystem::path(FA_TESTMEDIA_DIR) / name; }

int stripAt(VideoDecoder& dec, double seconds) {
    return test::readStrip(dec.frameAt(Time(static_cast<int64_t>(std::llround(seconds * 3000)), 3000)).image).index;
}

Project oneClipProject(const std::filesystem::path& file, Time length) {
    const MediaInfo info = probeMedia(file);
    Project p;
    MediaSource s;
    s.id = "m";
    const std::u8string u8 = file.u8string();
    s.path = std::string(u8.begin(), u8.end());
    s.duration = info.duration;
    s.frameRate = info.video->frameRate;
    s.width = info.video->width;
    s.height = info.video->height;
    s.hasAudio = info.audio.has_value();
    p.sources.push_back(s);
    Sequence q;
    q.id = "seq";
    q.width = 960;
    q.height = 540;
    q.frameRate = FrameRate(30);
    Track t{"v1", TrackKind::Video, "Video 1"};
    Clip c;
    c.id = "c";
    c.sourceId = "m";
    c.duration = length;
    t.clips.push_back(c);
    q.tracks.push_back(t);
    p.sequences.push_back(q);
    return p;
}

double seconds(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

// Variable frame rate (30 fps for 5 s, then only every second frame): the frame shown at a time is the last frame whose
// timestamp is not later - never a frame counted as if the file were constant-rate; a 30 fps timeline shows each
// dropped frame's predecessor twice.
TEST(S3Media, VariableFrameRateShowsTheFrameOfItsTimestamp) {
    const auto file = media("R_vfr_30_15_10s.mp4");
    if (!std::filesystem::exists(file)) GTEST_SKIP() << "run tools/make_s3_testmedia.ps1";
    VideoDecoder dec(file);
    EXPECT_EQ(stripAt(dec, 1.0), 30);
    EXPECT_EQ(stripAt(dec, 4.98), 149);
    EXPECT_EQ(stripAt(dec, 5.0), 150);
    EXPECT_EQ(stripAt(dec, 5.04), 150);   // 151 was dropped
    EXPECT_EQ(stripAt(dec, 6.0), 180);
    EXPECT_EQ(stripAt(dec, 9.9), 296);
    EXPECT_EQ(stripAt(dec, 2.0), 60);     // backwards: a seek
    const MediaInfo info = probeMedia(file);
    ASSERT_TRUE(info.duration);
    EXPECT_NEAR(info.duration->toDouble(), 10.0, 0.1);
    // on a 30 fps timeline
    const Project p = oneClipProject(file, Time(10));
    DecoderPool pool("");
    const auto backend = makeBackend(BackendChoice::Cpu);
    int checked = 0;
    for (int64_t f = 0; f < 300; f += 7) {
        const Image img = renderFrame(p.sequences[0], f, p, pool, *backend);
        const int want = f < 150 ? static_cast<int>(f) : static_cast<int>(f % 2 ? f - 1 : f);
        EXPECT_EQ(test::readStrip(img).index, want) << "timeline frame " << f;
        ++checked;
    }
    EXPECT_GT(checked, 40);
}

// A stream whose timestamps start at about 3.9 s (MPEG-TS with an offset): source time 0 is its first frame.
TEST(S3Media, StreamsThatDoNotStartAtZero) {
    const auto file = media("S_offset_start_10s.ts");
    if (!std::filesystem::exists(file)) GTEST_SKIP() << "run tools/make_s3_testmedia.ps1";
    VideoDecoder dec(file);
    EXPECT_EQ(stripAt(dec, 0.0), 0);
    EXPECT_EQ(stripAt(dec, 5.0), 150);
    EXPECT_EQ(stripAt(dec, 9.95), 298);
    const MediaInfo info = probeMedia(file);
    ASSERT_TRUE(info.duration);
    EXPECT_NEAR(info.duration->toDouble(), 10.0, 0.15);
}

// Damaged media: a truncated file decodes up to where it ends, a corrupted one decodes around the damage, garbage and
// empty files are refused - always with an exception the editor reports, never a crash.
TEST(S3Media, TruncatedCorruptGarbageAndEmptyFiles) {
    if (!std::filesystem::exists(media("T_truncated.mp4"))) GTEST_SKIP() << "run tools/make_s3_testmedia.ps1";
    for (const char* name : {"V_garbage.mp4", "W_empty.mp4"}) {
        EXPECT_THROW(probeMedia(media(name)), MediaError) << name;
        EXPECT_THROW(VideoDecoder{media(name)}, MediaError) << name;
    }
    int lastTruncated = -1, refused = 0;
    {
        VideoDecoder dec(media("T_truncated.mp4"));
        for (double t = 0; t < 10; t += 0.25) {
            try {
                const int i = stripAt(dec, t);
                if (i >= 0) lastTruncated = std::max(lastTruncated, i);
            } catch (const MediaError&) {
                ++refused;
            }
        }
    }
    std::printf("truncated file: frames decoded up to %d (of 299); %d requests refused\n", lastTruncated, refused);
    EXPECT_GT(lastTruncated, 100);   // what is in the file is shown
    int corruptOk = 0, corruptBad = 0;
    {
        VideoDecoder dec(media("U_corrupt_middle.mp4"));
        for (int f = 0; f < 300; ++f) {
            try {
                const int i = test::readStrip(dec.frameAt(Time(f, 30)).image).index;
                (i == f ? corruptOk : corruptBad)++;
            } catch (const MediaError&) {
                ++corruptBad;
            }
        }
    }
    std::printf("corrupted file: %d of 300 frames exact, %d damaged or refused\n", corruptOk, corruptBad);
    EXPECT_GT(corruptOk, 200);
    // exporting a clip of the truncated file either completes (the last frame held) or stops with a RenderError
    Project p = oneClipProject(media("T_truncated.mp4"), Time(10));
    RenderOptions o;
    o.includeAudio = false;
    const auto out = std::filesystem::temp_directory_path() / "fa_s3_truncated_export.mp4";
    try {
        const RenderReport r = renderProject(p, "", out, o);
        std::printf("export of the truncated clip: %lld frames written\n", static_cast<long long>(r.framesWritten));
    } catch (const RenderError& e) {
        std::printf("export of the truncated clip refused: %s\n", e.what());
    } catch (const MediaError& e) {
        std::printf("export of the truncated clip refused (media): %s\n", e.what());
    }
    std::error_code ec;
    std::filesystem::remove(out, ec);
}

// Damaged project files: thousands of mutations of a real project (truncated, bytes changed, values replaced) are
// either loaded and validated or refused with an exception; projects declaring absurd sizes are refused by validation.
TEST(S3Parsers, DamagedProjectFilesAreRefusedCleanly) {
    Project p;
    MediaSource s;
    s.id = "m";
    s.path = "clip.mp4";
    s.duration = Time(10);
    s.frameRate = FrameRate(30);
    s.width = 1920;
    s.height = 1080;
    p.sources.push_back(s);
    Sequence q;
    q.id = "seq";
    q.frameRate = FrameRate(30);
    Track v{"v1", TrackKind::Video, "Video 1"}, sub{"s1", TrackKind::Subtitle, "Subtitles 1"};
    for (int i = 0; i < 4; ++i) {
        Clip c;
        c.id = "c" + std::to_string(i);
        c.sourceId = "m";
        c.timelineStart = Time(2 * i);
        c.duration = Time(2);
        c.transform.positionX.setKey({Time(0), 0.1});
        c.transform.positionX.setKey({Time(1), -0.2});
        c.effects.push_back(makeEffect(*findEffectDef("fa.fx.blur.gaussian"), "fx" + std::to_string(i)));
        if (i < 3) c.outTransition = TransitionRef{"fa.transition.dissolve", Rational(1, 2)};
        v.clips.push_back(c);
    }
    Clip cap;
    cap.id = "cap";
    cap.duration = Time(2);
    TitleData td;
    td.text = "A caption";
    td.followTrackStyle = true;
    cap.title = td;
    sub.clips.push_back(cap);
    q.tracks = {v, sub};
    p.sequences.push_back(q);
    ASSERT_FALSE(hasErrors(validate(p))) << formatIssues(validate(p));
    const std::string good = projectToJsonString(p);
    std::mt19937 rng(20261027);
    int loaded = 0, refused = 0, truncRefused = 0;
    const char* values[] = {"1e308", "-1", "0", "\"\"", "null", "[]", "{}", "99999999999", "-99999999999", "\"1/0\"",
                            "\"-5/3\"", "true"};
    for (int k = 0; k < 3000; ++k) {
        std::string m = good;
        const int kind = k % 3;
        if (kind == 0) {   // cut off
            m.resize(std::uniform_int_distribution<size_t>(0, good.size() - 1)(rng));
        } else if (kind == 1) {   // a few bytes changed
            for (int j = 0; j < 1 + k % 5; ++j)
                m[std::uniform_int_distribution<size_t>(0, m.size() - 1)(rng)] = static_cast<char>(rng() % 256);
        } else {   // a value replaced
            const size_t colon = m.find(':', std::uniform_int_distribution<size_t>(0, m.size() - 1)(rng));
            if (colon == std::string::npos) continue;
            const size_t end = m.find_first_of(",}\n", colon);
            m.replace(colon + 1, end == std::string::npos ? 0 : end - colon - 1, std::string(" ") + values[rng() % std::size(values)]);
        }
        try {
            const Project back = projectFromJsonString(m);
            validate(back);
            ++loaded;
        } catch (const std::exception&) {
            ++refused;
            if (kind == 0) ++truncRefused;
        }
    }
    std::printf("3000 damaged projects: %d loaded (and validated), %d refused\n", loaded, refused);
    EXPECT_EQ(truncRefused, 1000);   // a cut-off file is never taken for a project
    // absurd sizes: refused by validation (nothing allocates them)
    for (int k = 0; k < 5; ++k) {
        Project bad = p;
        Sequence& b = bad.sequences[0];
        if (k == 0) b.width = 1000000;
        if (k == 1) b.height = 200000;
        if (k == 2) b.sampleRate = 10;
        if (k == 3) b.frameRate = FrameRate(Rational(5000));
        if (k == 4) b.tracks[1].clips[0].title->style.size = 1e6;
        EXPECT_TRUE(hasErrors(validate(bad))) << k;
    }
}

// Damaged subtitle (SRT / ASS) and LUT (.cube) files: thousands of mutations are parsed or refused, never a crash;
// a LUT declaring a huge size is refused before anything is allocated.
TEST(S3Parsers, DamagedSubtitleAndLutFilesAreRefusedCleanly) {
    const std::string srt = "1\r\n00:00:01,000 --> 00:00:02,500\r\nHello <i>world</i>\r\n\r\n2\r\n00:00:03,000 --> "
                            "00:00:04,000\r\nSecond line\r\nand a third\r\n\r\n3\r\n00:01:00,000 --> 00:01:02,000\r\nLast\r\n";
    const std::string ass = "[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\n\n[V4+ Styles]\nFormat: Name, "
                            "Fontname, Fontsize, PrimaryColour, Bold, Italic, Alignment, MarginV\nStyle: Default,Arial,48,"
                            "&H00FFFFFF,0,0,2,40\n\n[Events]\nFormat: Layer, Start, End, Style, Text\nDialogue: 0,0:00:01.00,"
                            "0:00:02.50,Default,{\\b1}Hello{\\b0} world\\Nsecond line\nDialogue: 0,0:00:03.00,0:00:04.00,Default,Next\n";
    std::string cube = "TITLE \"t\"\nLUT_3D_SIZE 4\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n";
    for (int i = 0; i < 64; ++i)
        cube += std::to_string((i % 4) / 3.0) + " " + std::to_string((i / 4 % 4) / 3.0) + " " + std::to_string((i / 16) / 3.0) + "\n";
    ASSERT_EQ(parseSrt(srt).captions.size(), 3u);
    std::string err;
    ASSERT_TRUE(parseCube(cube, &err)) << err;
    std::mt19937 rng(7);
    int parsed = 0, refused = 0;
    for (const std::string* base : std::initializer_list<const std::string*>{&srt, &ass, &cube})
        for (int k = 0; k < 1500; ++k) {
            std::string m = *base;
            if (k % 2) m.resize(std::uniform_int_distribution<size_t>(0, m.size())(rng));
            for (int j = 0; j < 1 + k % 6; ++j)
                if (!m.empty()) m[std::uniform_int_distribution<size_t>(0, m.size() - 1)(rng)] = static_cast<char>(rng() % 256);
            try {
                if (base == &cube) {
                    (parseCube(m, &err) ? parsed : refused)++;
                } else {
                    parseSubtitles(m, base == &srt ? "x.srt" : "x.ass");
                    ++parsed;
                }
            } catch (const std::exception&) {
                ++refused;
            }
        }
    std::printf("4500 damaged subtitle / LUT files: %d parsed, %d refused\n", parsed, refused);
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(parseCube("LUT_3D_SIZE 100000\n0 0 0\n", &err));
    EXPECT_FALSE(parseCube("LUT_1D_SIZE 2000000000\n0 0 0\n", &err));
    EXPECT_LT(seconds(t0), 0.5);
}

// A long project: 3 video tracks x 1000 one-second clips with transitions, 1000 audio clips, 300 captions. Loading,
// validating, saving, evaluating frames and editing stay fast (no quadratic cost hidden in the model or the renderer).
TEST(S3LongProject, ThousandsOfClipsStayFast) {
    const auto file = media("A_red_1080p30_10s.mp4");
    if (!std::filesystem::exists(file)) GTEST_SKIP() << "testmedia missing";
    Project p = oneClipProject(file, Time(1));
    Sequence& q = p.sequences[0];
    q.tracks.clear();
    for (int t = 0; t < 4; ++t) {
        Track tr{"t" + std::to_string(t), t < 3 ? TrackKind::Video : TrackKind::Audio, "T" + std::to_string(t)};
        for (int i = 0; i < 1000; ++i) {
            Clip c;
            c.id = "c" + std::to_string(t) + "_" + std::to_string(i);
            c.sourceId = "m";
            c.sourceIn = Time(i % 8);
            c.timelineStart = Time(i);
            c.duration = Time(1);
            if (t < 3 && i % 10 == 0 && i + 1 < 1000) c.outTransition = TransitionRef{"fa.transition.dissolve", Rational(1, 3)};
            tr.clips.push_back(c);
        }
        q.tracks.push_back(tr);
    }
    Track sub{"s", TrackKind::Subtitle, "Subtitles"};
    for (int i = 0; i < 300; ++i) {
        Clip c;
        c.id = "cap" + std::to_string(i);
        c.timelineStart = Time(3 * i);
        c.duration = Time(2);
        TitleData td;
        td.text = "Caption " + std::to_string(i);
        td.followTrackStyle = true;
        c.title = td;
        sub.clips.push_back(c);
    }
    q.tracks.push_back(sub);
    auto t0 = std::chrono::steady_clock::now();
    ASSERT_FALSE(hasErrors(validate(p))) << formatIssues(validate(p)).substr(0, 500);
    const double validateS = seconds(t0);
    t0 = std::chrono::steady_clock::now();
    const std::string json = projectToJsonString(p);
    const double saveS = seconds(t0);
    t0 = std::chrono::steady_clock::now();
    const Project back = projectFromJsonString(json);
    const double loadS = seconds(t0);
    EXPECT_EQ(back.sequences[0].tracks[0].clips.size(), 1000u);
    t0 = std::chrono::steady_clock::now();
    size_t samples = 0;
    for (int64_t f = 0; f < 30000; f += 150) samples += evaluateVideoFrame(back.sequences[0], f, &back).size();
    const double evalMs = seconds(t0) * 1000 / 200;
    // an edit in the middle, its undo
    Project e = back;
    UndoStack st(e);
    t0 = std::chrono::steady_clock::now();
    st.execute(edit::splitAt(e, "seq", Time(15005, 30)));
    EXPECT_EQ(e.sequences[0].tracks[0].clips.size(), 1001u);
    st.undo();
    EXPECT_EQ(e.sequences[0].tracks[0].clips.size(), 1000u);
    const double editS = seconds(t0);
    std::printf("4300 clips, 300 captions (%zu KB JSON): validate %.3f s, save %.3f s, load %.3f s, %.3f ms per frame "
                "evaluated (%zu samples), split + undo %.3f s\n",
                json.size() / 1024, validateS, saveS, loadS, evalMs, samples, editS);
    EXPECT_LT(validateS, 2.0);
    EXPECT_LT(saveS, 2.0);
    EXPECT_LT(loadS, 2.0);
    EXPECT_LT(evalMs, 5.0);
    EXPECT_LT(editS, 3.0);
}
