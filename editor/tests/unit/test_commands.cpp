#include "fa/model/commands.h"
#include "fa/model/serialize.h"

#include <gtest/gtest.h>

using namespace fa;

namespace {

struct Fixture : ::testing::Test {
    Project p;
    UndoStack stack{p};
    std::string seq = "seq-1";
    std::string v1;

    void SetUp() override {
        p.name = "edit test";
        Sequence s;
        s.id = seq;
        s.frameRate = FrameRate::fps30();
        p.sequences.push_back(s);
        stack.execute(makeAddSource(p, MediaSource{"src-a", "A.mp4", Rational(10), FrameRate::fps30(), 1920, 1080, true, true}));
        stack.execute(makeAddSource(p, MediaSource{"src-b", "B.mp4", Rational(10), FrameRate::fps30(), 1920, 1080, true, true}));
        auto add = makeAddTrack(p, seq, TrackKind::Video);
        stack.execute(std::move(add));
        v1 = p.sequences[0].tracks[0].id;
        stack.clear();
        stack.setClean();
    }

    Clip clip(const std::string& src, Rational start, Rational in, Rational dur) {
        Clip c;
        c.sourceId = src;
        c.timelineStart = start;
        c.sourceIn = in;
        c.duration = dur;
        return c;
    }
    const std::vector<Clip>& clips() { return p.sequences[0].tracks[0].clips; }
    // Document content. The id counter is excluded on purpose: it only ever increases (ids are never
    // reused, even after undo or a rejected edit), so it is not part of what undo restores.
    std::string snapshot() const {
        Project q = p;
        q.nextId = 0;
        return projectToJsonString(q);
    }
};

}  // namespace

TEST_F(Fixture, InsertUndoRedoRestoresExactState) {
    const std::string s0 = snapshot();
    stack.execute(makeInsertClip(p, seq, v1, clip("src-a", Rational(0), Rational(2), Rational(2))));
    const std::string s1 = snapshot();
    stack.execute(makeInsertClip(p, seq, v1, clip("src-b", Rational(2), Rational(5), Rational(2))));
    const std::string s2 = snapshot();
    ASSERT_EQ(clips().size(), 2u);
    EXPECT_FALSE(stack.isClean());

    stack.undo();
    EXPECT_EQ(snapshot(), s1);
    stack.undo();
    EXPECT_EQ(snapshot(), s0);
    EXPECT_TRUE(stack.isClean());
    EXPECT_FALSE(stack.canUndo());
    stack.redo();
    EXPECT_EQ(snapshot(), s1);
    stack.redo();
    EXPECT_EQ(snapshot(), s2);   // identical ids on redo
    EXPECT_FALSE(stack.canRedo());
    EXPECT_THROW(stack.redo(), EditError);
}

TEST_F(Fixture, NewCommandClearsRedoHistory) {
    stack.execute(makeInsertClip(p, seq, v1, clip("src-a", Rational(0), Rational(0), Rational(1))));
    stack.undo();
    EXPECT_TRUE(stack.canRedo());
    stack.execute(makeInsertClip(p, seq, v1, clip("src-b", Rational(0), Rational(0), Rational(1))));
    EXPECT_FALSE(stack.canRedo());
    EXPECT_EQ(clips()[0].sourceId, "src-b");
}

TEST_F(Fixture, RejectedEditsLeaveProjectUntouched) {
    stack.execute(makeInsertClip(p, seq, v1, clip("src-a", Rational(0), Rational(0), Rational(2))));
    const std::string before = snapshot();
    const size_t undoCount = stack.undoCount();
    // overlap
    EXPECT_THROW(stack.execute(makeInsertClip(p, seq, v1, clip("src-b", Rational(1), Rational(0), Rational(2)))), EditError);
    // not frame aligned (1/60 s at 30 fps)
    EXPECT_THROW(stack.execute(makeInsertClip(p, seq, v1, clip("src-b", Rational(121, 60), Rational(0), Rational(1)))),
                 EditError);
    // beyond end of media
    EXPECT_THROW(stack.execute(makeInsertClip(p, seq, v1, clip("src-b", Rational(5), Rational(9), Rational(2)))), EditError);
    // unknown clip
    EXPECT_THROW(stack.execute(makeRemoveClip(seq, "nope", false)), EditError);
    EXPECT_EQ(snapshot(), before);
    EXPECT_EQ(stack.undoCount(), undoCount);
}

TEST_F(Fixture, SplitKeepsPictureAndUndoes) {
    stack.execute(makeInsertClip(p, seq, v1, clip("src-a", Rational(1), Rational(3), Rational(4))));
    const std::string before = snapshot();
    const std::string id = clips()[0].id;
    stack.execute(makeSplitClip(p, seq, id, Rational(2)));
    ASSERT_EQ(clips().size(), 2u);
    const Clip& l = clips()[0];
    const Clip& r = clips()[1];
    EXPECT_EQ(l.id, id);
    EXPECT_EQ(l.timelineRange(), (TimeRange{Rational(1), Rational(1)}));
    EXPECT_EQ(r.timelineRange(), (TimeRange{Rational(2), Rational(3)}));
    EXPECT_EQ(r.sourceIn, Rational(4));
    // Same picture at every instant: source time at t is unchanged by the split.
    for (int f = 30; f < 150; ++f) {
        const Time t = frameToTime(f, FrameRate::fps30());
        const Clip* c = p.sequences[0].tracks[0].clipAt(t);
        ASSERT_NE(c, nullptr);
        EXPECT_EQ(c->sourceTimeAt(t), Rational(3) + (t - Rational(1)));
    }
    EXPECT_THROW(stack.execute(makeSplitClip(p, seq, id, Rational(1))), EditError);   // at the edge
    stack.undo();
    EXPECT_EQ(snapshot(), before);
}

TEST_F(Fixture, RippleDeleteShiftsFollowingClips) {
    stack.execute(makeInsertClip(p, seq, v1, clip("src-a", Rational(0), Rational(0), Rational(2))));
    stack.execute(makeInsertClip(p, seq, v1, clip("src-b", Rational(2), Rational(0), Rational(3))));
    stack.execute(makeInsertClip(p, seq, v1, clip("src-a", Rational(6), Rational(5), Rational(1))));
    const std::string before = snapshot();
    const std::string first = clips()[0].id;
    stack.execute(makeRemoveClip(seq, first, /*ripple=*/true));
    ASSERT_EQ(clips().size(), 2u);
    EXPECT_EQ(clips()[0].timelineStart, Rational(0));
    EXPECT_EQ(clips()[1].timelineStart, Rational(4));
    EXPECT_EQ(stack.undoLabel(), "Ripple delete");
    stack.undo();
    EXPECT_EQ(snapshot(), before);

    stack.execute(makeRemoveClip(seq, first, /*ripple=*/false));
    EXPECT_EQ(clips()[0].timelineStart, Rational(2));
}

TEST_F(Fixture, TrimHeadMovesSourceInAndTailChangesDuration) {
    stack.execute(makeInsertClip(p, seq, v1, clip("src-a", Rational(1), Rational(3), Rational(4))));
    const std::string id = clips()[0].id;
    stack.execute(makeTrimClip(seq, id, ClipEdge::Head, Rational(2)));
    EXPECT_EQ(clips()[0].timelineStart, Rational(2));
    EXPECT_EQ(clips()[0].sourceIn, Rational(4));
    EXPECT_EQ(clips()[0].duration, Rational(3));
    stack.execute(makeTrimClip(seq, id, ClipEdge::Tail, Rational(3)));
    EXPECT_EQ(clips()[0].duration, Rational(1));
    // head before media start: sourceIn would become negative
    EXPECT_THROW(stack.execute(makeTrimClip(seq, id, ClipEdge::Head, Rational(-3))), EditError);
    // tail past media end (source 4 + 7 > 10)
    EXPECT_THROW(stack.execute(makeTrimClip(seq, id, ClipEdge::Tail, Rational(9))), EditError);
    stack.undo();
    stack.undo();
    EXPECT_EQ(clips()[0].timelineRange(), (TimeRange{Rational(1), Rational(4)}));
    EXPECT_EQ(clips()[0].sourceIn, Rational(3));
}

TEST_F(Fixture, MoveBetweenTracksAndUndo) {
    stack.execute(makeAddTrack(p, seq, TrackKind::Video));
    const std::string v2 = p.sequences[0].tracks[1].id;
    stack.execute(makeInsertClip(p, seq, v1, clip("src-a", Rational(0), Rational(0), Rational(2))));
    const std::string id = clips()[0].id;
    const std::string before = snapshot();
    stack.execute(makeMoveClip(seq, id, Rational(5), v2));
    EXPECT_TRUE(p.sequences[0].tracks[0].clips.empty());
    ASSERT_EQ(p.sequences[0].tracks[1].clips.size(), 1u);
    EXPECT_EQ(p.sequences[0].tracks[1].clips[0].timelineStart, Rational(5));
    stack.undo();
    EXPECT_EQ(snapshot(), before);
}

TEST_F(Fixture, ModifyClipKeyframesUndoable) {
    stack.execute(makeInsertClip(p, seq, v1, clip("src-a", Rational(0), Rational(0), Rational(2))));
    const std::string id = clips()[0].id;
    const std::string before = snapshot();
    stack.execute(makeModifyClip(seq, id, "Fade in", [](Clip& c) {
        Keyframe<double> a, b;
        a.time = Rational(0);
        a.value = 0.0;
        b.time = Rational(1);
        b.value = 1.0;
        c.opacity.setKey(a);
        c.opacity.setKey(b);
    }));
    EXPECT_TRUE(clips()[0].opacity.isAnimated());
    EXPECT_EQ(stack.undoLabel(), "Fade in");
    EXPECT_THROW(stack.execute(makeModifyClip(seq, id, "bad", [](Clip& c) { c.duration = Rational(1); })), EditError);
    stack.undo();
    EXPECT_EQ(snapshot(), before);
}

TEST_F(Fixture, TrackAddRemoveUndo) {
    const std::string before = snapshot();
    stack.execute(makeAddTrack(p, seq, TrackKind::Audio, 0, "Music"));
    EXPECT_EQ(p.sequences[0].tracks[0].kind, TrackKind::Audio);
    stack.execute(makeRemoveTrack(seq, p.sequences[0].tracks[0].id));
    stack.undo();
    stack.undo();
    EXPECT_EQ(snapshot(), before);
}

TEST_F(Fixture, UndoLimitDropsOldest) {
    UndoStack small(p, 3);
    for (int i = 0; i < 5; ++i)
        small.execute(makeInsertClip(p, seq, v1, clip("src-a", Rational(i), Rational(0), Rational(1))));
    EXPECT_EQ(small.undoCount(), 3u);
    small.undo();
    small.undo();
    small.undo();
    EXPECT_EQ(clips().size(), 2u);
    EXPECT_FALSE(small.canUndo());
}
