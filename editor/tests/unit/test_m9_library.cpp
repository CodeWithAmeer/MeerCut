// M9: sticker clips in the model (saved, validated, on video tracks only) and pasting template layers in order.
#include "fa/model/commands.h"
#include "fa/model/edit_ops.h"
#include "fa/model/serialize.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace fa;

namespace {

Project oneTrack(int videoTracks = 1) {
    Project p;
    Sequence s;
    s.id = "s";
    s.frameRate = FrameRate::fps30();
    for (int i = 0; i < videoTracks; ++i) s.tracks.push_back(Track{"v" + std::to_string(i + 1), TrackKind::Video, "Video"});
    s.tracks.push_back(Track{"a1", TrackKind::Audio, "Audio 1"});
    p.sequences.push_back(s);
    p.nextId = 100;
    return p;
}

Clip sticker(const std::string& id, const std::string& stickerId, Time start, Time dur) {
    Clip c;
    c.id = id;
    c.sticker = StickerData{stickerId, 0x112233, "Hi"};
    c.timelineStart = start;
    c.duration = dur;
    return c;
}

}  // namespace

TEST(M9Sticker, SavedValidatedAndOnVideoTracksOnly) {
    Project p = oneTrack();
    p.sequences[0].tracks[0].clips.push_back(sticker("st", "emoji.smile", Time(0), Time(3)));
    ASSERT_FALSE(hasErrors(validate(p))) << formatIssues(validate(p));
    const Project back = projectFromJsonString(projectToJsonString(p));
    const Clip& c = back.sequences[0].tracks[0].clips[0];
    ASSERT_TRUE(c.isSticker());
    EXPECT_TRUE(c.isGenerated());
    EXPECT_EQ(*c.sticker, (StickerData{"emoji.smile", 0x112233, "Hi"}));
    EXPECT_TRUE(c.sourceId.empty());
    // refused: on an audio track, with media, without an id, with a colour beyond 24 bits
    Project bad = oneTrack();
    bad.sequences[0].tracks[1].clips.push_back(sticker("st", "emoji.smile", Time(0), Time(3)));
    EXPECT_TRUE(hasErrors(validate(bad)));
    bad = p;
    bad.sequences[0].tracks[0].clips[0].sourceId = "m";
    EXPECT_TRUE(hasErrors(validate(bad)));
    bad = p;
    bad.sequences[0].tracks[0].clips[0].sticker->id.clear();
    EXPECT_TRUE(hasErrors(validate(bad)));
    bad = p;
    bad.sequences[0].tracks[0].clips[0].sticker->color = 0x1000000;
    EXPECT_TRUE(hasErrors(validate(bad)));
    // the editing commands keep stickers off audio tracks
    Project q = oneTrack();
    EXPECT_THROW(makeInsertClip(q, "s", "a1", sticker("", "emoji.smile", Time(0), Time(1)))->apply(q), EditError);
    // a sticker is a generated clip: it can be any length and placed like a title
    std::string id;
    UndoStack st(q);
    Clip proto;
    proto.sticker = StickerData{"love.heart", 0, {}};
    st.execute(edit::addGenerated(q, "s", proto, Time(1), Time(2), {}, &id));
    const Track* tr = nullptr;
    const Clip* added = edit::findClip(q.sequences[0], id, &tr);
    ASSERT_TRUE(added);
    EXPECT_EQ(tr->kind, TrackKind::Video);
    EXPECT_EQ(added->name, "Sticker");
}

// Template layers keep their order: each goes above the previous one even where a lower track is free for it.
TEST(M9Clipboard, StackInOrderKeepsLayersAbove) {
    Project p = oneTrack();
    Clip footage;
    footage.id = "f";
    footage.sticker = StickerData{"bg.solid", 0, {}};
    footage.duration = Time(3);
    p.sequences[0].tracks[0].clips.push_back(footage);   // Video 1 is taken 0-3 s
    std::vector<edit::CopiedClip> layers = {
        {sticker("", "bg.night", Time(0), Time(5)), "tpl.0", TrackKind::Video},
        {sticker("", "emoji.smile", Time(4), Time(1)), "tpl.1", TrackKind::Video},   // Video 1 is free at 4-5 s
    };
    for (bool stacked : {false, true}) {
        Project q = p;
        std::vector<std::string> ids;
        UndoStack st(q);
        st.execute(edit::pasteClips(q, "s", layers, Time(0), &ids, stacked));
        ASSERT_EQ(ids.size(), 2u);
        auto ordinal = [&](const std::string& id) {
            int k = 0;
            for (const auto& t : q.sequences[0].tracks) {
                if (t.kind != TrackKind::Video) continue;
                for (const auto& c : t.clips)
                    if (c.id == id) return k;
                ++k;
            }
            return -1;
        };
        if (stacked) EXPECT_GT(ordinal(ids[1]), ordinal(ids[0]));   // the smile stays above the background
        else EXPECT_LT(ordinal(ids[1]), ordinal(ids[0]));           // without it the lower free track is taken
        EXPECT_FALSE(hasErrors(validate(q)));
    }
}
