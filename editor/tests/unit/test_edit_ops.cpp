// User-level edit operations: linked A/V pairs, ripple, split, transitions, locks, atomicity.
#include "fa/model/edit_ops.h"
#include "fa/model/serialize.h"

#include <gtest/gtest.h>

using namespace fa;

namespace {

struct EditOps : ::testing::Test {
    Project p;
    UndoStack stack{p};
    const std::string seq = "seq-1";
    const FrameRate rate = FrameRate::fps30();

    void SetUp() override {
        Sequence s;
        s.id = seq;
        s.frameRate = rate;
        p.sequences.push_back(s);
        // 10 s A/V file, 4 s video-only file, 6 s audio-only file
        p.sources.push_back(MediaSource{"av", "A.mp4", Rational(10), FrameRate::fps30(), 1920, 1080, true, true});
        p.sources.push_back(MediaSource{"vid", "V.mp4", Rational(4), FrameRate::fps30(), 1920, 1080, true, false});
        p.sources.push_back(MediaSource{"aud", "M.wav", Rational(6), std::nullopt, 0, 0, false, true});
    }
    Sequence& S() { return p.sequences[0]; }
    const Clip& clip(const std::string& id) { return *edit::findClip(S(), id); }
    std::string content() const {
        Project q = p;
        q.nextId = 0;
        return projectToJsonString(q);
    }
    edit::AddedClips add(const std::string& src, Rational at, std::string track = {}) {
        edit::AddedClips ids;
        stack.execute(edit::addMedia(p, seq, src, at, std::move(track), &ids));
        return ids;
    }
};

}  // namespace

TEST_F(EditOps, AddMediaCreatesLinkedPairOnNewTracks) {
    const auto ids = add("av", Rational(0));
    ASSERT_EQ(S().tracks.size(), 2u);
    EXPECT_EQ(S().tracks[0].kind, TrackKind::Video);
    EXPECT_EQ(S().tracks[0].name, "Video 1");
    EXPECT_EQ(S().tracks[1].kind, TrackKind::Audio);
    EXPECT_EQ(S().tracks[1].name, "Audio 1");
    const Clip& v = clip(ids.videoClipId);
    const Clip& a = clip(ids.audioClipId);
    EXPECT_EQ(v.duration, Rational(10));
    EXPECT_EQ(v.timelineRange(), a.timelineRange());
    EXPECT_FALSE(v.linkId.empty());
    EXPECT_EQ(v.linkId, a.linkId);
    EXPECT_EQ(v.name, "A.mp4");
    EXPECT_EQ(edit::linkGroup(S(), v.id).size(), 2u);
    EXPECT_EQ(stack.undoLabel(), "Add to timeline");
    stack.undo();   // one undo step removes clips and the auto-created tracks
    EXPECT_TRUE(S().tracks.empty());
}

TEST_F(EditOps, AddVideoOnlyAndAudioOnly) {
    const auto v = add("vid", Rational(0));
    EXPECT_FALSE(v.videoClipId.empty());
    EXPECT_TRUE(v.audioClipId.empty());
    EXPECT_TRUE(clip(v.videoClipId).linkId.empty());
    const auto a = add("aud", Rational(0));
    EXPECT_TRUE(a.videoClipId.empty());
    const Track* t = nullptr;
    edit::findClip(S(), a.audioClipId, &t);
    EXPECT_EQ(t->kind, TrackKind::Audio);
}

TEST_F(EditOps, DurationIsFloorToFrames) {
    p.sources[1].duration = Rational(4) + Rational(1, 100);   // 4.01 s at 30 fps -> 120 frames
    const auto v = add("vid", Rational(0));
    EXPECT_EQ(clip(v.videoClipId).duration, Rational(4));
    p.sources[1].duration = Rational(1, 100);
    EXPECT_THROW(edit::addMedia(p, seq, "vid", Rational(0)), EditError);
}

TEST_F(EditOps, AddIntoOccupiedSpaceInsertsAfterAndRipples) {
    const auto first = add("av", Rational(0));
    const auto second = add("av", Rational(12));
    const auto third = add("vid", Rational(3));   // lands inside `first` -> inserted at 10 s, everything after ripples 4 s
    EXPECT_EQ(clip(third.videoClipId).timelineStart, Rational(10));
    EXPECT_EQ(clip(second.videoClipId).timelineStart, Rational(16));
    EXPECT_EQ(clip(second.audioClipId).timelineStart, Rational(16));   // audio partner stays in sync
    EXPECT_EQ(clip(first.videoClipId).timelineStart, Rational(0));
}

TEST_F(EditOps, MoveCarriesLinkedPartner) {
    const auto ids = add("av", Rational(0));
    stack.execute(edit::moveClip(p, seq, ids.videoClipId, Rational(5, 2)));
    EXPECT_EQ(clip(ids.videoClipId).timelineStart, Rational(5, 2));
    EXPECT_EQ(clip(ids.audioClipId).timelineStart, Rational(5, 2));
    stack.undo();
    EXPECT_EQ(clip(ids.audioClipId).timelineStart, Rational(0));
    // moving to a misaligned time fails atomically
    const std::string before = content();
    EXPECT_THROW(stack.execute(edit::moveClip(p, seq, ids.audioClipId, Rational(1, 60))), EditError);
    EXPECT_EQ(content(), before);
}

TEST_F(EditOps, MoveToAnotherVideoTrack) {
    const auto ids = add("av", Rational(0));
    stack.execute(makeAddTrack(p, seq, TrackKind::Video, std::nullopt, "Video 2"));
    const std::string v2 = S().tracks.back().id;
    stack.execute(edit::moveClip(p, seq, ids.videoClipId, Rational(1), v2));
    const Track* t = nullptr;
    edit::findClip(S(), ids.videoClipId, &t);
    EXPECT_EQ(t->id, v2);
    EXPECT_EQ(clip(ids.audioClipId).timelineStart, Rational(1));
}

TEST_F(EditOps, TrimAppliesSameDeltaToPartner) {
    const auto ids = add("av", Rational(0));
    stack.execute(edit::trimClip(p, seq, ids.audioClipId, ClipEdge::Head, Rational(2)));
    for (const auto& id : {ids.videoClipId, ids.audioClipId}) {
        EXPECT_EQ(clip(id).timelineStart, Rational(2));
        EXPECT_EQ(clip(id).sourceIn, Rational(2));
        EXPECT_EQ(clip(id).duration, Rational(8));
    }
    stack.execute(edit::trimClip(p, seq, ids.videoClipId, ClipEdge::Tail, Rational(7)));
    EXPECT_EQ(clip(ids.audioClipId).timelineEnd(), Rational(7));
    // cannot extend past the media
    EXPECT_THROW(edit::trimClip(p, seq, ids.videoClipId, ClipEdge::Tail, Rational(11)), EditError);
}

TEST_F(EditOps, SplitLinkedPairGivesTwoPairs) {
    const auto ids = add("av", Rational(0));
    stack.execute(edit::splitAt(p, seq, Rational(4), {ids.videoClipId}));
    ASSERT_EQ(S().tracks[0].clips.size(), 2u);
    ASSERT_EQ(S().tracks[1].clips.size(), 2u);
    const Clip& vl = S().tracks[0].clips[0];
    const Clip& vr = S().tracks[0].clips[1];
    const Clip& al = S().tracks[1].clips[0];
    const Clip& ar = S().tracks[1].clips[1];
    EXPECT_EQ(vl.linkId, al.linkId);
    EXPECT_EQ(vr.linkId, ar.linkId);
    EXPECT_NE(vl.linkId, vr.linkId);
    EXPECT_EQ(vr.sourceIn, Rational(4));
    EXPECT_EQ(ar.sourceIn, Rational(4));
    // moving the right video half moves only the right audio half
    stack.execute(edit::moveClip(p, seq, vr.id, Rational(12)));
    EXPECT_EQ(S().tracks[1].clips[1].timelineStart, Rational(12));
    EXPECT_EQ(S().tracks[1].clips[0].timelineStart, Rational(0));
    EXPECT_EQ(stack.undoCount(), 3u);
}

TEST_F(EditOps, SplitAtPlayheadWithoutSelection) {
    add("av", Rational(0));
    add("aud", Rational(0));   // second audio track? no: lands on Audio 1 -> occupied -> inserted at 10 s
    EXPECT_THROW(edit::splitAt(p, seq, Rational(10)), EditError);   // 10 s is a boundary everywhere
    stack.execute(edit::splitAt(p, seq, Rational(5)));
    EXPECT_EQ(S().tracks[0].clips.size(), 2u);
}

TEST_F(EditOps, RippleDeleteKeepsLinkedClipsInSync) {
    const auto a = add("av", Rational(0));
    const auto b = add("av", Rational(10));
    const auto c = add("vid", Rational(20));   // video only, after b
    stack.execute(edit::deleteClips(p, seq, {a.videoClipId}, /*ripple=*/true));
    EXPECT_EQ(clip(b.videoClipId).timelineStart, Rational(0));
    EXPECT_EQ(clip(b.audioClipId).timelineStart, Rational(0));
    EXPECT_EQ(clip(c.videoClipId).timelineStart, Rational(10));
    EXPECT_EQ(edit::findClip(S(), a.audioClipId), nullptr);   // partner deleted too
    stack.undo();
    EXPECT_EQ(clip(b.videoClipId).timelineStart, Rational(10));
    // plain delete leaves a gap
    stack.execute(edit::deleteClips(p, seq, {a.audioClipId}, false));
    EXPECT_EQ(clip(b.videoClipId).timelineStart, Rational(10));
    EXPECT_EQ(edit::findClip(S(), a.videoClipId), nullptr);
}

TEST_F(EditOps, RippleDeleteRefusedWhenItWouldBreakSync) {
    const auto a = add("vid", Rational(0));   // video only, 0-4
    const auto b = add("av", Rational(4));    // V 4-14, A 4-14
    Clip blocker;                             // audio-only clip on Audio 1 at 0-4 blocks the audio partner's ripple
    blocker.sourceId = "aud";
    blocker.timelineStart = Rational(0);
    blocker.sourceIn = Rational(0);
    blocker.duration = Rational(4);
    stack.execute(makeInsertClip(p, seq, S().tracks[1].id, blocker));
    ASSERT_EQ(clip(b.audioClipId).timelineStart, Rational(4));
    const std::string before = content();
    EXPECT_THROW(stack.execute(edit::deleteClips(p, seq, {a.videoClipId}, true)), EditError);
    EXPECT_EQ(content(), before);
}

TEST_F(EditOps, TransitionsAreValidatedAndDroppedWhenBroken) {
    const auto a = add("vid", Rational(0));
    const auto b = add("vid", Rational(4));
    EXPECT_EQ(edit::maxTransitionDuration(S(), a.videoClipId, TransitionAlignment::Centered), Rational(8));
    EXPECT_EQ(edit::maxTransitionDuration(S(), b.videoClipId, TransitionAlignment::Centered), std::nullopt);
    stack.execute(edit::setTransition(p, seq, a.videoClipId,
                                      TransitionRef{"fa.transition.dissolve", Rational(1), TransitionAlignment::Centered}));
    EXPECT_TRUE(clip(a.videoClipId).outTransition.has_value());
    EXPECT_THROW(edit::setTransition(p, seq, b.videoClipId,
                                     TransitionRef{"fa.transition.dissolve", Rational(1), TransitionAlignment::Centered}),
                 EditError);   // no following clip
    // moving b away breaks adjacency -> transition removed in the same undo step
    stack.execute(edit::moveClip(p, seq, b.videoClipId, Rational(6)));
    EXPECT_FALSE(clip(a.videoClipId).outTransition.has_value());
    stack.undo();
    EXPECT_TRUE(clip(a.videoClipId).outTransition.has_value());
}

TEST_F(EditOps, LockedTracksRefuseEdits) {
    const auto ids = add("av", Rational(0));
    stack.execute(makeModifyTrack(seq, S().tracks[1].id, "Lock", [](Track& t) { t.locked = true; }));
    EXPECT_THROW(edit::moveClip(p, seq, ids.videoClipId, Rational(1)), EditError);   // partner is locked
    EXPECT_THROW(edit::deleteClips(p, seq, {ids.videoClipId}, false), EditError);
    stack.execute(edit::unlink(p, seq, ids.videoClipId));
    stack.execute(edit::moveClip(p, seq, ids.videoClipId, Rational(1)));
    EXPECT_EQ(clip(ids.audioClipId).timelineStart, Rational(0));
}

TEST_F(EditOps, CompositeFailureRevertsEarlierSteps) {
    add("vid", Rational(0));
    const std::string before = content();
    std::vector<std::unique_ptr<Command>> cmds;
    cmds.push_back(makeAddTrack(p, seq, TrackKind::Video));
    cmds.push_back(makeRemoveClip(seq, "does-not-exist", false));
    EXPECT_THROW(stack.execute(makeComposite("broken", std::move(cmds))), EditError);
    EXPECT_EQ(content(), before);
}

TEST_F(EditOps, ShiftAndModifyCommands) {
    const auto a = add("vid", Rational(0));
    stack.execute(makeShiftClips(seq, S().tracks[0].id, Rational(0), Rational(2)));
    EXPECT_EQ(clip(a.videoClipId).timelineStart, Rational(2));
    EXPECT_THROW(stack.execute(makeShiftClips(seq, S().tracks[0].id, Rational(0), Rational(-3))), EditError);
    stack.execute(makeModifyTrack(seq, S().tracks[0].id, "Mute", [](Track& t) { t.volumeDb = -6; }));
    EXPECT_THROW(stack.execute(makeModifyTrack(seq, S().tracks[0].id, "x", [](Track& t) { t.clips.clear(); })), EditError);
    stack.execute(makeModifySource("vid", "Relink", [](MediaSource& s) { s.path = "other.mp4"; }));
    EXPECT_EQ(p.findSource("vid")->path, "other.mp4");
    stack.undo();
    EXPECT_EQ(p.findSource("vid")->path, "V.mp4");
    // the clip starts at 2 s = 59.94 frames at 29.97 fps -> not frame-aligned -> refused
    EXPECT_THROW(stack.execute(makeModifySequence(seq, "fps", [](Sequence& s) { s.frameRate = FrameRate::ntsc30(); })),
                 EditError);
    stack.execute(makeModifySequence(seq, "fps", [](Sequence& s) { s.frameRate = FrameRate::fps25(); }));
    EXPECT_EQ(S().frameRate, FrameRate::fps25());
}

TEST_F(EditOps, LinkAndVolumeRoundTripThroughJson) {
    const auto ids = add("av", Rational(0));
    stack.execute(makeModifyClip(seq, ids.audioClipId, "Volume", [](Clip& c) { c.volumeDb.setConstant(-6.5); }));
    stack.execute(makeModifyTrack(seq, S().tracks[1].id, "Lock", [](Track& t) {
        t.locked = true;
        t.volumeDb = 3;
    }));
    const Project q = projectFromJsonString(projectToJsonString(p));
    EXPECT_EQ(q, p);
}
