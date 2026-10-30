// M9: clipboard (copy / paste / duplicate as model operations), colour marks and clip mute.
#include "fa/model/commands.h"
#include "fa/model/edit_ops.h"
#include "fa/model/serialize.h"
#include "fa/render/audio_mix.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace fa;

namespace {

constexpr int kRate = 48000;

std::filesystem::path media(const char* name) { return std::filesystem::path(FA_TESTMEDIA_DIR) / name; }

MediaSource source(const std::string& id, const std::filesystem::path& file, bool video) {
    MediaSource s;
    s.id = id;
    s.path = file.string();
    s.duration = Rational(10);
    s.hasVideo = video;
    s.hasAudio = true;
    if (video) {
        s.frameRate = FrameRate::fps30();
        s.width = 1920;
        s.height = 1080;
    }
    return s;
}

Clip clip(const std::string& id, const std::string& src, Time start, Time in, Time dur, const std::string& link = {}) {
    Clip c;
    c.id = id;
    c.sourceId = src;
    c.timelineStart = start;
    c.sourceIn = in;
    c.duration = dur;
    c.linkId = link;
    return c;
}

// Video 1 / Audio 1: A (0-4 s, linked) then B (4-8 s, linked) with a dissolve from A into B.
Project base() {
    Project p;
    p.sources = {source("A", media("A_red_1080p30_10s.mp4"), true), source("B", media("B_blue_1080p30_10s.mp4"), true),
                 source("H", media("H_tone1k_-12dB_10s.wav"), false)};
    Sequence s;
    s.id = "s";
    s.frameRate = FrameRate::fps30();
    s.sampleRate = kRate;
    Track v1{"v1", TrackKind::Video, "Video 1"};
    Track a1{"a1", TrackKind::Audio, "Audio 1"};
    Clip va = clip("va", "A", Time(0), Time(0), Time(4), "L1");
    va.outTransition = TransitionRef{"fa.transition.dissolve", Rational(1, 2)};
    EffectRef fx;
    fx.id = "fx1";
    fx.type = "fa.fx.blur.gaussian";
    va.effects.push_back(fx);
    va.colorMark = 4;
    v1.clips = {va, clip("vb", "B", Time(4), Time(0), Time(4), "L2")};
    a1.clips = {clip("aa", "A", Time(0), Time(0), Time(4), "L1"), clip("ab", "B", Time(4), Time(0), Time(4), "L2")};
    s.tracks = {v1, a1};
    p.sequences.push_back(s);
    p.nextId = 100;
    return p;
}

const Clip* find(const Project& p, const std::string& id, const Track** tr = nullptr) {
    return edit::findClip(p.sequences[0], id, tr);
}

}  // namespace

// Copy takes the selected clip with its A/V partner; the clipboard text round-trips every clip property.
TEST(M9Clipboard, CopyTakesLinkPartnersAndRoundTripsThroughTheClipboardText) {
    const Project p = base();
    const auto copied = edit::copyClips(p.sequences[0], {"va"});
    ASSERT_EQ(copied.size(), 2u);
    std::set<std::string> ids{copied[0].clip.id, copied[1].clip.id};
    EXPECT_EQ(ids, (std::set<std::string>{"va", "aa"}));
    const std::string text = edit::clipboardToJson(copied);
    const auto back = edit::clipboardFromJson(text);
    ASSERT_EQ(back.size(), 2u);
    for (size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(back[i].clip, copied[i].clip);
        EXPECT_EQ(back[i].trackId, copied[i].trackId);
        EXPECT_EQ(back[i].kind, copied[i].kind);
    }
    EXPECT_THROW(edit::clipboardFromJson("{\"format\": \"something else\"}"), ModelError);
    EXPECT_THROW(edit::clipboardFromJson("not json"), ModelError);
}

// Paste at a free place: the same tracks, new ids, one new link shared by the pair, offsets kept, the transition into
// a clip that was not copied dropped, effects and the colour mark kept; one undo step.
TEST(M9Clipboard, PasteKeepsTracksAndLinksWithNewIds) {
    Project p = base();
    const Project before = p;
    const auto copied = edit::copyClips(p.sequences[0], {"va"});
    std::vector<std::string> ids;
    UndoStack st(p);
    st.execute(edit::pasteClips(p, "s", copied, Time(12), &ids));
    ASSERT_EQ(ids.size(), 2u);
    const Track* tv = nullptr;
    const Track* ta = nullptr;
    const Clip* v = nullptr;
    const Clip* a = nullptr;
    for (const auto& id : ids) {
        const Track* tr = nullptr;
        const Clip* c = find(p, id, &tr);
        ASSERT_TRUE(c);
        if (tr->kind == TrackKind::Video) v = c, tv = tr;
        else a = c, ta = tr;
    }
    ASSERT_TRUE(v && a);
    EXPECT_EQ(tv->id, "v1");
    EXPECT_EQ(ta->id, "a1");
    EXPECT_EQ(v->timelineStart, Time(12));
    EXPECT_EQ(a->timelineStart, Time(12));
    EXPECT_EQ(v->duration, Time(4));
    EXPECT_FALSE(v->linkId.empty());
    EXPECT_EQ(v->linkId, a->linkId);
    EXPECT_NE(v->linkId, "L1");
    EXPECT_NE(v->id, "va");
    EXPECT_FALSE(v->outTransition);   // its partner B was not copied
    ASSERT_EQ(v->effects.size(), 1u);
    EXPECT_EQ(v->effects[0].type, "fa.fx.blur.gaussian");
    EXPECT_EQ(v->colorMark, 4);
    EXPECT_FALSE(hasErrors(validate(p)));
    st.undo();
    EXPECT_EQ(p.sequences[0], before.sequences[0]);
}

// Where the original track is taken, the clips go up to a free track of the same kind (created when needed), and the
// A/V pair stays in sync.
TEST(M9Clipboard, PasteOntoAnOccupiedPlaceUsesTheNextFreeTrack) {
    Project p = base();
    const auto copied = edit::copyClips(p.sequences[0], {"vb"});
    std::vector<std::string> ids;
    UndoStack st(p);
    st.execute(edit::pasteClips(p, "s", copied, Time(1), &ids));   // 1-5 s overlaps A and B on both tracks
    ASSERT_EQ(ids.size(), 2u);
    const Sequence& s = p.sequences[0];
    ASSERT_EQ(s.tracks.size(), 4u);
    for (const auto& id : ids) {
        const Track* tr = nullptr;
        const Clip* c = edit::findClip(s, id, &tr);
        ASSERT_TRUE(c);
        EXPECT_TRUE(tr->id != "v1" && tr->id != "a1") << tr->id;
        EXPECT_EQ(c->timelineStart, Time(1));
    }
    EXPECT_FALSE(hasErrors(validate(p)));
    // a second paste at the same place takes yet another pair of tracks
    st.execute(edit::pasteClips(p, "s", copied, Time(1), &ids));
    EXPECT_EQ(p.sequences[0].tracks.size(), 6u);
}

// Several clips keep their distances; a pasted title needs no media; media missing from the project is refused.
TEST(M9Clipboard, GroupsTitlesAndMissingMedia) {
    Project p = base();
    auto copied = edit::copyClips(p.sequences[0], {"va", "vb"});
    ASSERT_EQ(copied.size(), 4u);
    std::vector<std::string> ids;
    UndoStack st(p);
    st.execute(edit::pasteClips(p, "s", copied, Time(20), &ids));
    std::vector<Time> starts;
    for (const auto& id : ids) starts.push_back(find(p, id)->timelineStart);
    std::sort(starts.begin(), starts.end());
    EXPECT_EQ(starts, (std::vector<Time>{Time(20), Time(20), Time(24), Time(24)}));
    // the dissolve between the two copied clips is kept
    int dissolves = 0;
    for (const auto& id : ids) dissolves += find(p, id)->outTransition ? 1 : 0;
    EXPECT_EQ(dissolves, 1);

    Clip title;
    title.title = TitleData{};
    title.title->text = "Hello";
    title.duration = Time(2);
    title.timelineStart = Time(3);
    title.id = "t";
    st.execute(edit::pasteClips(p, "s", {{title, "v1", TrackKind::Video}}, Time(30), &ids));
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(find(p, ids[0])->title->text, "Hello");

    Clip foreign = clip("x", "not-in-this-project", Time(0), Time(0), Time(1));
    EXPECT_THROW(edit::pasteClips(p, "s", {{foreign, "v1", TrackKind::Video}}, Time(40)), EditError);
}

// Colour marks and mute are saved (format 10), out-of-range marks are refused.
TEST(M9Clip, ColorMarkAndMuteAreSavedAndValidated) {
    Project p = base();
    EXPECT_GE(Project::kFormatVersion, 10);
    p.sequences[0].tracks[1].clips[0].muted = true;
    const Project back = projectFromJsonString(projectToJsonString(p));
    EXPECT_EQ(back.sequences[0].tracks[0].clips[0].colorMark, 4);
    EXPECT_TRUE(back.sequences[0].tracks[1].clips[0].muted);
    EXPECT_FALSE(back.sequences[0].tracks[1].clips[1].muted);
    p.sequences[0].tracks[0].clips[0].colorMark = kColorMarkCount + 1;
    EXPECT_TRUE(hasErrors(validate(p)));
    p.sequences[0].tracks[0].clips[0].colorMark = -1;
    EXPECT_TRUE(hasErrors(validate(p)));
}

// A muted clip is silent in the mix (and an unmuted one is not).
TEST(M9Clip, MutedClipIsSilent) {
    Project p;
    p.sources = {source("H", media("H_tone1k_-12dB_10s.wav"), false)};
    Sequence s;
    s.id = "s";
    s.frameRate = FrameRate::fps30();
    s.sampleRate = kRate;
    Track a1{"a1", TrackKind::Audio, "Audio 1"};
    a1.clips = {clip("h", "H", Time(0), Time(0), Time(4))};
    s.tracks = {a1};
    p.sequences.push_back(s);
    auto rms = [&](const Project& q) {
        AudioMixer m("", kRate);
        std::vector<float> out(static_cast<size_t>(kRate) * 2);
        m.mix(q, q.sequences[0], kRate, kRate, out.data());
        double e = 0;
        for (float v : out) e += double(v) * v;
        return std::sqrt(e / out.size());
    };
    EXPECT_GT(rms(p), 0.01);   // the tone is audible (measured 0.022)
    p.sequences[0].tracks[0].clips[0].muted = true;
    EXPECT_EQ(rms(p), 0.0);
}
