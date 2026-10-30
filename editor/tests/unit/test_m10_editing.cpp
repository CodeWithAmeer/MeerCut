// M10: Delete Gap (Clipchamp's "Delete this gap"), sounds placed on the audio track they are dropped on, and the
// placement of generated clips (titles, stickers, effect layers) that the timeline's drop preview shows.
#include "fa/model/commands.h"
#include "fa/model/edit_ops.h"
#include "fa/model/project.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <tuple>

using namespace fa;

namespace {

std::filesystem::path media(const char* name) { return std::filesystem::path(FA_TESTMEDIA_DIR) / name; }

MediaSource source(const std::string& id, const char* file, bool video, Time duration = Time(10)) {
    MediaSource s;
    s.id = id;
    s.path = media(file).string();
    s.duration = duration;
    s.hasVideo = video;
    s.hasAudio = true;
    if (video) {
        s.frameRate = FrameRate::fps30();
        s.width = 1920;
        s.height = 1080;
    }
    return s;
}

Clip clip(const std::string& id, const std::string& src, Time start, Time dur, const std::string& link = {}) {
    Clip c;
    c.id = id;
    c.name = id;
    c.sourceId = src;
    c.timelineStart = start;
    c.sourceIn = Time(0);
    c.duration = dur;
    c.linkId = link;
    return c;
}

Clip title(const std::string& id, Time start, Time dur) {
    Clip c;
    c.id = id;
    c.name = id;
    c.title = TitleData{};
    c.title->text = id;
    c.timelineStart = start;
    c.duration = dur;
    return c;
}

// Video 1: va 0-4 (L1), a 3 s gap, vb 7-11 (L2), vc 11-13 (L3, a dissolve vb -> vc).
// Audio 1: their sounds. Audio 2: music 5-9 (not linked). Video 2: a title 8-10 (not linked).
Project gapped() {
    Project p;
    p.sources = {source("A", "A_red_1080p30_10s.mp4", true), source("B", "B_blue_1080p30_10s.mp4", true),
                 source("H", "H_tone1k_-12dB_10s.wav", false)};
    Sequence s;
    s.id = "s";
    s.frameRate = FrameRate::fps30();
    s.sampleRate = 48000;
    Track v1{"v1", TrackKind::Video, "Video 1"};
    Track v2{"v2", TrackKind::Video, "Video 2"};
    Track a1{"a1", TrackKind::Audio, "Audio 1"};
    Track a2{"a2", TrackKind::Audio, "Audio 2"};
    Clip vb = clip("vb", "B", Time(7), Time(4), "L2");
    vb.outTransition = TransitionRef{"fa.transition.dissolve", Rational(1, 2)};
    v1.clips = {clip("va", "A", Time(0), Time(4), "L1"), vb, clip("vc", "A", Time(11), Time(2), "L3")};
    a1.clips = {clip("aa", "A", Time(0), Time(4), "L1"), clip("ab", "B", Time(7), Time(4), "L2"),
                clip("ac", "A", Time(11), Time(2), "L3")};
    a2.clips = {clip("music", "H", Time(5), Time(4))};
    v2.clips = {title("caption", Time(8), Time(2))};
    s.tracks = {v1, v2, a1, a2};
    p.sequences.push_back(s);
    p.nextId = 100;
    return p;
}

const Clip& at(const Project& p, const std::string& id) {
    const Clip* c = edit::findClip(p.sequences[0], id);
    if (!c) throw std::runtime_error("no clip " + id);
    return *c;
}

}  // namespace

// The gap is the empty stretch from the clip before (or 0) to the next clip; never on a clip, never after the last.
TEST(M10Gap, GapAtFindsTheEmptyStretchBetweenClips) {
    Track t{"t", TrackKind::Video, "Video 1"};
    t.clips = {clip("x", "A", Time(6), Time(3)), clip("w", "A", Time(2), Time(2)), clip("y", "A", Time(9), Time(1))};   // any order
    const auto lead = edit::gapAt(t, Time(0));
    ASSERT_TRUE(lead);
    EXPECT_EQ(*lead, (TimeRange{Time(0), Time(2)}));
    EXPECT_EQ(*edit::gapAt(t, Rational(3, 2)), (TimeRange{Time(0), Time(2)}));
    EXPECT_FALSE(edit::gapAt(t, Time(2)));   // on w
    const auto mid = edit::gapAt(t, Time(4));
    ASSERT_TRUE(mid);
    EXPECT_EQ(*mid, (TimeRange{Time(4), Time(2)}));
    EXPECT_EQ(*edit::gapAt(t, Rational(599, 100)), (TimeRange{Time(4), Time(2)}));
    EXPECT_FALSE(edit::gapAt(t, Time(6)));    // on x
    EXPECT_FALSE(edit::gapAt(t, Time(9)));    // on y (x and y touch: no gap between them)
    EXPECT_FALSE(edit::gapAt(t, Time(12)));   // after the last clip: not a gap
    EXPECT_FALSE(edit::gapAt(t, Time(-1)));
    Track empty{"e", TrackKind::Audio, "Audio 1"};
    EXPECT_FALSE(edit::gapAt(empty, Time(1)));
}

// Deleting the gap moves the clips after it and their linked sounds left by its length - one undo step; the clips
// before it, the music and the title (not linked) stay; the dissolve between the moved clips survives.
TEST(M10Gap, DeleteGapMovesTheClipsAfterItWithTheirLinkedPartners) {
    Project p = gapped();
    const Project before = p;
    EXPECT_EQ(edit::gapProblem(p.sequences[0], "v1", Time(5)), "");
    UndoStack st(p);
    st.execute(edit::deleteGap(p, "s", "v1", Time(5)));
    EXPECT_EQ(st.undoLabel(), "Delete gap");
    EXPECT_EQ(at(p, "va").timelineStart, Time(0));
    EXPECT_EQ(at(p, "vb").timelineStart, Time(4));
    EXPECT_EQ(at(p, "vc").timelineStart, Time(8));
    EXPECT_EQ(at(p, "ab").timelineStart, Time(4));   // the linked sounds follow
    EXPECT_EQ(at(p, "ac").timelineStart, Time(8));
    EXPECT_EQ(at(p, "aa").timelineStart, Time(0));
    EXPECT_EQ(at(p, "music").timelineStart, Time(5));     // other tracks: untouched
    EXPECT_EQ(at(p, "caption").timelineStart, Time(8));
    ASSERT_TRUE(at(p, "vb").outTransition);              // vb and vc still meet
    EXPECT_FALSE(hasErrors(validate(p)));
    st.undo();
    EXPECT_EQ(p.sequences, before.sequences);
    st.redo();
    EXPECT_EQ(at(p, "vb").timelineStart, Time(4));
    // nothing to delete on a clip or after the last one
    EXPECT_EQ(edit::gapProblem(p.sequences[0], "v1", Time(1)), "There is no gap here");
    EXPECT_THROW(edit::deleteGap(p, "s", "v1", Time(20)), EditError);
}

// A leading gap (Clipchamp's screenshot: the video starts later than 0) and gaps between sounds.
TEST(M10Gap, LeadingGapAndAGapOnAnAudioTrack) {
    Project p = gapped();
    auto& s = p.sequences[0];
    for (auto& t : s.tracks)
        for (auto& c : t.clips) c.timelineStart += Time(3);   // everything 3 s later
    UndoStack st(p);
    st.execute(edit::deleteGap(p, "s", "v1", Time(1)));
    EXPECT_EQ(at(p, "va").timelineStart, Time(0));
    EXPECT_EQ(at(p, "aa").timelineStart, Time(0));
    EXPECT_EQ(at(p, "vb").timelineStart, Time(7));
    EXPECT_EQ(at(p, "music").timelineStart, Time(8));   // not linked: stays
    // Audio 2: the music now starts at 8 - deleting its leading gap moves it to 0 (nothing is linked to it)
    st.execute(edit::deleteGap(p, "s", "a2", Time(2)));
    EXPECT_EQ(at(p, "music").timelineStart, Time(0));
    EXPECT_FALSE(hasErrors(validate(p)));
}

// Refused - with a reason for the user - when a linked sound would run into a clip that stays, when a track that
// would change is locked, or when a linked clip would start before 0. Nothing changes.
TEST(M10Gap, RefusedWithAReasonAndNothingChanges) {
    {
        Project p = gapped();
        Clip pop = clip("Pop", "H", Rational(9, 2), Rational(3, 2));   // a sound in Audio 1's gap: 4.5-6
        p.sequences[0].tracks[2].clips.insert(p.sequences[0].tracks[2].clips.begin() + 1, pop);
        const std::string why = edit::gapProblem(p.sequences[0], "v1", Time(5));
        EXPECT_NE(why.find("Can't delete this gap"), std::string::npos) << why;
        EXPECT_NE(why.find("'ab'"), std::string::npos) << why;
        EXPECT_NE(why.find("'Pop'"), std::string::npos) << why;
        EXPECT_NE(why.find("Audio 1"), std::string::npos) << why;
        const Project before = p;
        EXPECT_THROW(edit::deleteGap(p, "s", "v1", Time(5)), EditError);
        EXPECT_EQ(p, before);
    }
    {
        Project p = gapped();
        p.sequences[0].tracks[2].locked = true;   // Audio 1: the linked sounds cannot move
        const std::string why = edit::gapProblem(p.sequences[0], "v1", Time(5));
        EXPECT_NE(why.find("Audio 1 is locked"), std::string::npos) << why;
        p.sequences[0].tracks[2].locked = false;
        p.sequences[0].tracks[0].locked = true;   // the gap's own track
        EXPECT_EQ(edit::gapProblem(p.sequences[0], "v1", Time(5)), "Can't delete this gap: Video 1 is locked");
        EXPECT_THROW(edit::deleteGap(p, "s", "v1", Time(5)), EditError);
    }
    {
        Project p = gapped();
        // vb's sound starts 6 s before its picture (on Audio 2 for room): moving it left by 3 s passes 0
        auto& a1 = p.sequences[0].tracks[2].clips;
        a1.erase(a1.begin() + 1);
        p.sequences[0].tracks[3].clips = {clip("early", "B", Time(1), Time(3), "L2")};
        const std::string why = edit::gapProblem(p.sequences[0], "v1", Time(5));
        EXPECT_NE(why.find("would start before 0"), std::string::npos) << why;
    }
}

// M10: a sound dropped on Audio 3 goes on Audio 3 - also while Audio 1 is busy there (before, it went to Audio 1:
// inserted after the clip there, with everything rippled right).
TEST(M10Media, ASoundGoesOnTheAudioTrackItIsDroppedOn) {
    Project p = gapped();
    auto& s = p.sequences[0];
    s.tracks.push_back(Track{"a3", TrackKind::Audio, "Audio 3"});
    const Project before = p;
    UndoStack st(p);
    edit::AddedClips added;
    st.execute(edit::addMedia(p, "s", "H", Time(2), {}, &added, "a3"));
    const Track* tr = nullptr;
    const Clip* c = edit::findClip(p.sequences[0], added.audioClipId, &tr);
    ASSERT_TRUE(c);
    EXPECT_EQ(tr->id, "a3");
    EXPECT_EQ(c->timelineStart, Time(2));
    EXPECT_TRUE(added.videoClipId.empty());
    EXPECT_EQ(at(p, "va").timelineStart, Time(0));   // nothing rippled
    EXPECT_EQ(at(p, "vb").timelineStart, Time(7));
    st.undo();
    EXPECT_EQ(p.sequences, before.sequences);
    // without a track: the old default (the first audio track), which is busy at 2 s - inserted after its clip
    st.execute(edit::addMedia(p, "s", "H", Time(2), {}, &added));
    edit::findClip(p.sequences[0], added.audioClipId, &tr);
    EXPECT_EQ(tr->id, "a1");
    st.undo();
    EXPECT_THROW(edit::addMedia(p, "s", "H", Time(2), {}, &added, "v1"), EditError);   // not an audio track
    // a video with sound: its sound on the requested audio track, linked to the picture
    st.execute(edit::addMedia(p, "s", "B", Time(20), {}, &added, "a3"));
    const Clip* v = edit::findClip(p.sequences[0], added.videoClipId);
    const Clip* a = edit::findClip(p.sequences[0], added.audioClipId, &tr);
    ASSERT_TRUE(v && a);
    EXPECT_EQ(tr->id, "a3");
    EXPECT_EQ(v->linkId, a->linkId);
}

// The drop preview's track (generatedTarget) is where addGenerated really puts a title, sticker or effect layer:
// the track under the mouse if it is free there, else the lowest free video track above it, else a new one on top.
TEST(M10Generated, ThePreviewTrackIsWhereTheClipLands) {
    Project p = gapped();
    p.sequences[0].tracks.insert(p.sequences[0].tracks.begin() + 2, Track{"v3", TrackKind::Video, "Video 3"});
    const Sequence& s = p.sequences[0];
    EXPECT_EQ(edit::generatedTarget(s, "v1", {Time(1), Time(5)}), "v2");   // on va: the track above is free there
    EXPECT_EQ(edit::generatedTarget(s, "v1", {Time(7), Time(5)}), "v3");   // Video 2 has the title at 8-10
    EXPECT_EQ(edit::generatedTarget(s, "v2", {Time(1), Time(5)}), "v2");   // free where it was dropped
    EXPECT_EQ(edit::generatedTarget(s, "", {Time(0), Time(1)}), "v2");     // no track: above Video 1
    p.sequences[0].tracks[2].locked = true;                                 // Video 3 locked: a new track
    EXPECT_EQ(edit::generatedTarget(p.sequences[0], "v1", {Time(7), Time(5)}), "");
    p.sequences[0].tracks[2].locked = false;
    for (const auto& [requested, start, expected] : {std::tuple<std::string, Time, std::string>{"v1", Time(1), "v2"},
                                                     {"v1", Time(7), "v3"}, {"v2", Time(1), "v2"}}) {
        Project q = p;
        UndoStack st(q);
        Clip proto;
        proto.adjustment = true;
        EffectRef fx;
        fx.id = "fx1";
        fx.type = "fa.fx.pixelate";
        proto.effects = {fx};
        std::string id;
        st.execute(edit::addGenerated(q, "s", proto, start, Time(5), requested, &id));
        const Track* tr = nullptr;
        ASSERT_TRUE(edit::findClip(q.sequences[0], id, &tr));
        EXPECT_EQ(tr->id, expected) << requested;
        EXPECT_EQ(st.undoLabel(), "Add effect");   // an effect dropped as a layer
    }
    {   // nothing free above: a new video track on top
        Project q = p;
        q.sequences[0].tracks[2].locked = true;
        UndoStack st(q);
        Clip proto;
        proto.adjustment = true;
        std::string id;
        st.execute(edit::addGenerated(q, "s", proto, Time(7), Time(5), "v1", &id));
        const Track* tr = nullptr;
        ASSERT_TRUE(edit::findClip(q.sequences[0], id, &tr));
        EXPECT_EQ(tr->name, "Video 4");
        EXPECT_EQ(st.undoLabel(), "Add adjustment layer");   // no effect yet
    }
}
