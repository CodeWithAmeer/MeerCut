// M9 motion presets (Animation tab): keyframes in the right window, around the clip's own values, replaceable and
// removable, saved with the project.
#include "fa/model/motion_presets.h"
#include "fa/model/serialize.h"

#include <gtest/gtest.h>

using namespace fa;

namespace {

Clip videoClip(Time start = Time(10), Time sourceIn = Time(2), Time dur = Time(4)) {
    Clip c;
    c.id = "c";
    c.sourceId = "m";
    c.timelineStart = start;
    c.sourceIn = sourceIn;
    c.duration = dur;
    return c;
}

double at(const Animatable<double>& a, double sourceSeconds) { return a.value(sourceSeconds); }

}  // namespace

TEST(M9Motion, InAndOutPresetsWorkInTheirWindowsAroundTheRestingValues) {
    Clip c = videoClip();
    c.transform.scaleX.setConstant(0.8);
    c.transform.scaleY.setConstant(0.8);
    c.transform.positionX.setConstant(0.1);
    const double len = motionLength(c.duration).toDouble();
    EXPECT_NEAR(len, 0.6, 1e-9);
    EXPECT_NEAR(motionLength(Time(1)).toDouble(), 1.0 / 3, 1e-9);   // at most a third of a short clip
    applyMotionPreset(c, MotionSlot::In, "zoom_in");
    EXPECT_EQ(c.motionIn, "zoom_in");
    // keys are in source time: the clip plays source 2..6 s
    EXPECT_NEAR(at(c.transform.scaleX, 2.0), 0.4, 1e-9);        // half the resting 0.8 at the start
    EXPECT_NEAR(at(c.transform.scaleX, 2.0 + len), 0.8, 1e-9);  // resting at the end of the window
    EXPECT_NEAR(at(c.transform.scaleX, 4.0), 0.8, 1e-9);        // and after it
    EXPECT_NEAR(at(c.transform.scaleY, 2.0), 0.4, 1e-9);
    EXPECT_NEAR(at(c.opacity, 2.0), 0.0, 1e-9);
    applyMotionPreset(c, MotionSlot::Out, "slide_left_out");
    EXPECT_EQ(c.motionOut, "slide_left_out");
    EXPECT_NEAR(at(c.transform.positionX, 6.0 - len), 0.1, 1e-9);   // from its own position...
    EXPECT_NEAR(at(c.transform.positionX, 6.0), 0.1 - 1.0, 1e-9);   // ...one frame width to the left
    EXPECT_NEAR(at(c.transform.positionX, 3.0), 0.1, 1e-9);         // untouched in the middle
    // replacing the In preset removes its keys first
    applyMotionPreset(c, MotionSlot::In, "fade_in");
    EXPECT_EQ(c.motionIn, "fade_in");
    EXPECT_NEAR(at(c.transform.scaleX, 2.0), 0.8, 1e-9);
    EXPECT_NEAR(at(c.opacity, 2.0), 0.0, 1e-9);
    EXPECT_NEAR(at(c.opacity, 2.0 + len), 1.0, 1e-9);
    // a Combo preset replaced over the whole clip leaves the Out slide (another property) alone
    applyMotionPreset(c, MotionSlot::Combo, "push_in");
    applyMotionPreset(c, MotionSlot::Combo, "pull_out");
    EXPECT_NEAR(at(c.transform.positionX, 6.0), 0.1 - 1.0, 1e-9);
    applyMotionPreset(c, MotionSlot::Combo, "");
    // None removes it
    applyMotionPreset(c, MotionSlot::In, "");
    EXPECT_TRUE(c.motionIn.empty());
    EXPECT_NEAR(at(c.opacity, 2.0), 1.0, 1e-9);
    EXPECT_THROW(applyMotionPreset(c, MotionSlot::In, "no_such_preset"), ModelError);
}

TEST(M9Motion, ComboCoversTheWholeClipAndEverythingIsSaved) {
    Clip c = videoClip();
    applyMotionPreset(c, MotionSlot::Combo, "ken_burns");
    EXPECT_NEAR(at(c.transform.scaleX, 2.0), 1.0, 1e-9);
    EXPECT_NEAR(at(c.transform.scaleX, 6.0), 1.18, 1e-9);
    applyMotionPreset(c, MotionSlot::Combo, "pulse");
    EXPECT_EQ(c.motionCombo, "pulse");
    EXPECT_GE(c.transform.scaleX.keys().size(), 8u);   // a beat every second over 4 s
    for (const auto& p : motionPresets()) {             // every preset applies in its slot
        Clip d = videoClip();
        EXPECT_NO_THROW(applyMotionPreset(d, p.slot, p.id)) << p.id;
        EXPECT_TRUE(d.transform.positionX.isAnimated() || d.transform.positionY.isAnimated() || d.transform.scaleX.isAnimated() ||
                    d.opacity.isAnimated() ||
                    d.transform.rotation.isAnimated())
            << p.id;
    }
    // saved with the project
    Project proj;
    MediaSource s;
    s.id = "m";
    s.path = "x.mp4";
    s.duration = Time(20);
    s.hasVideo = true;
    s.frameRate = FrameRate::fps30();
    proj.sources = {s};
    Sequence q;
    q.id = "s";
    Track v{"v", TrackKind::Video, "Video 1"};
    applyMotionPreset(c, MotionSlot::In, "boom");
    v.clips = {c};
    q.tracks = {v};
    proj.sequences = {q};
    const Project back = projectFromJsonString(projectToJsonString(proj));
    const Clip& b = back.sequences[0].tracks[0].clips[0];
    EXPECT_EQ(b.motionIn, "boom");
    EXPECT_EQ(b.motionCombo, "pulse");
    EXPECT_EQ(b.transform.scaleX, c.transform.scaleX);
}
