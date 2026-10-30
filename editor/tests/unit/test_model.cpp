#include "fa/model/commands.h"
#include "fa/model/serialize.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace fa;

namespace {

Project twoClipProject(FrameRate rate = FrameRate::fps30()) {
    Project p;
    p.name = "two clips";
    MediaSource a{"src-a", "media/A.mp4", Rational(10), FrameRate::fps30(), 1920, 1080, true, true};
    MediaSource b{"src-b", "media/B.mp4", Rational(10), FrameRate::fps30(), 1920, 1080, true, true};
    p.sources = {a, b};
    Sequence s;
    s.id = "seq-1";
    s.name = "Main";
    s.frameRate = rate;
    Track v;
    v.id = "v1";
    v.kind = TrackKind::Video;
    Clip ca;
    ca.id = "clip-a";
    ca.sourceId = "src-a";
    ca.timelineStart = Rational(0);
    ca.sourceIn = Rational(2);
    ca.duration = Rational(2);
    Clip cb = ca;
    cb.id = "clip-b";
    cb.sourceId = "src-b";
    cb.timelineStart = Rational(2);
    cb.sourceIn = Rational(5);
    v.clips = {ca, cb};
    s.tracks.push_back(v);
    p.sequences.push_back(s);
    p.nextId = 10;
    return p;
}

}  // namespace

TEST(Model, ValidProjectHasNoIssues) {
    const Project p = twoClipProject();
    const auto issues = validate(p);
    EXPECT_TRUE(issues.empty()) << formatIssues(issues);
    EXPECT_EQ(p.sequences[0].duration(), Rational(4));
    EXPECT_EQ(p.sequences[0].frameCount(), 120);
}

TEST(Model, ClipAtUsesHalfOpenRanges) {
    const Project p = twoClipProject();
    const Track& t = p.sequences[0].tracks[0];
    EXPECT_EQ(t.clipAt(Rational(0))->id, "clip-a");
    EXPECT_EQ(t.clipAt(Rational(59, 30))->id, "clip-a");   // last frame of A
    EXPECT_EQ(t.clipAt(Rational(2))->id, "clip-b");        // the cut belongs to B
    EXPECT_EQ(t.clipAt(Rational(4)), nullptr);
}

TEST(Model, ValidationFindsStructuralErrors) {
    Project p = twoClipProject();
    auto& clips = p.sequences[0].tracks[0].clips;
    clips[1].timelineStart = Rational(1);                    // overlaps A [0, 2)
    clips[1].duration = Rational(1, 60);                     // half a frame at 30 fps
    clips[1].sourceId = "missing";                           // dangling reference
    const auto issues = validate(p);
    EXPECT_TRUE(hasErrors(issues));
    const std::string text = formatIssues(issues);
    EXPECT_NE(text.find("overlaps"), std::string::npos) << text;
    EXPECT_NE(text.find("whole number of frames"), std::string::npos) << text;
    EXPECT_NE(text.find("unknown source"), std::string::npos) << text;
}

TEST(Model, ValidationChecksSourceLengthAndIds) {
    Project p = twoClipProject();
    p.sources[1].duration = Rational(6);   // B needs source [5, 7)
    p.sequences[0].tracks[0].clips[0].id = "clip-b";
    const std::string text = formatIssues(validate(p));
    EXPECT_NE(text.find("exceeds source duration"), std::string::npos) << text;
    EXPECT_NE(text.find("duplicate id"), std::string::npos) << text;
}

TEST(Serialization, RoundTripPreservesEverything) {
    Project p = twoClipProject(FrameRate::ntsc30());
    auto& clips = p.sequences[0].tracks[0].clips;
    clips[1].timelineStart = frameToTime(60, FrameRate::ntsc30());
    clips[1].duration = frameToTime(45, FrameRate::ntsc30());
    clips[0].duration = frameToTime(60, FrameRate::ntsc30());
    clips[0].sourceIn = Rational(1001, 30000) * Rational(7);
    Keyframe<double> k0, k1;
    k0.time = Rational(2);
    k0.value = 0.25;
    k0.interp = Interpolation::Bezier;
    k0.tangent = TangentMode::Manual;
    k0.outSlope = 0.0625;
    k0.outHandle = 0.3;
    k1.time = Rational(3);
    k1.value = 1.0 / 3.0;
    k1.tangent = TangentMode::Auto;
    clips[0].opacity.setKey(k0);
    clips[0].opacity.setKey(k1);
    clips[0].outTransition = TransitionRef{"fa.transition.dissolve", Rational(1), TransitionAlignment::EndsAtCut};
    EffectRef fx;
    fx.id = "fx1";   // effect instance ids are part of the model since format v4
    fx.type = "fa.color.basic";
    fx.params["brightness"] = Animatable<double>(0.1);
    clips[1].effects.push_back(fx);
    p.sequences[0].tracks.push_back(Track{"a1", TrackKind::Audio, "Music", false, true, {}});

    const std::string json = projectToJsonString(p);
    const Project q = projectFromJsonString(json);
    EXPECT_EQ(p, q);
    EXPECT_EQ(projectToJsonString(q), json);   // stable output
    EXPECT_NE(json.find("\"30000/1001\""), std::string::npos);
}

TEST(Serialization, SaveLoadFile) {
    const Project p = twoClipProject();
    const auto file = std::filesystem::temp_directory_path() / "fa_model_roundtrip.json";
    saveProject(p, file);
    EXPECT_EQ(loadProject(file), p);
    std::filesystem::remove(file);
    EXPECT_THROW(loadProject(file), ModelError);
}

TEST(Serialization, ErrorsNameTheField) {
    auto expectError = [](const std::string& text, const std::string& fragment) {
        try {
            projectFromJsonString(text);
            ADD_FAILURE() << "no error for " << text;
        } catch (const ModelError& e) {
            EXPECT_NE(std::string(e.what()).find(fragment), std::string::npos) << e.what();
        }
    };
    expectError("{not json", "not valid JSON");
    expectError(R"({"format":"other","version":1})", "$.format");
    expectError(R"({"format":"fa-project","version":99,"sources":[],"sequences":[]})", "unsupported project version");
    expectError(R"({"format":"fa-project","version":1,"sources":[{"id":"s"}],"sequences":[]})", "$.sources[0].path");
    expectError(R"({"format":"fa-project","version":1,"sources":[],"sequences":[{"id":"q","width":2,"height":2,
                  "frameRate":"0/1","tracks":[]}]})", "$.sequences[0].frameRate");
    expectError(R"({"format":"fa-project","version":1,"sources":[],"sequences":[{"id":"q","width":2,"height":2,
                  "frameRate":"30","tracks":[{"id":"t","kind":"video","clips":[{"id":"c","source":"s","start":"1/x",
                  "in":"0","duration":"1"}]}]}]})", "$.sequences[0].tracks[0].clips[0].start");
}

TEST(Serialization, RelativeMediaPathsResolveAgainstProjectFile) {
    MediaSource s;
    s.path = "media/A.mp4";
    const auto resolved = resolveMediaPath(s, std::filesystem::path("C:/work/proj/edit.json"));
    EXPECT_EQ(resolved, std::filesystem::path("C:/work/proj/media/A.mp4"));
}
