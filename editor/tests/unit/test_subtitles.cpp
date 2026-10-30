// M3: subtitle files (SRT, ASS/SSA), title/caption model, edit operations and title animation timing.
#include "fa/model/edit_ops.h"
#include "fa/model/serialize.h"
#include "fa/model/subtitles.h"
#include "fa/render/text_animation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

using namespace fa;

namespace {

bool hasWarning(const SubtitleDoc& d, const std::string& part) {
    return std::any_of(d.warnings.begin(), d.warnings.end(), [&](const std::string& w) { return w.find(part) != std::string::npos; });
}

Project emptyProject(FrameRate rate = FrameRate::fps30()) {
    Project p;
    Sequence s;
    s.id = "seq";
    s.frameRate = rate;
    Track v1;
    v1.id = "v1";
    v1.kind = TrackKind::Video;
    v1.name = "Video 1";
    s.tracks.push_back(v1);
    p.sequences.push_back(s);
    return p;
}

}  // namespace

// ---- SRT -------------------------------------------------------------------------------------------

TEST(Srt, ParsesBlocksTimesAndMultilineText) {
    const std::string srt =
        "1\r\n00:00:01,000 --> 00:00:02,500\r\nHello world\r\n\r\n"
        "2\r\n00:00:03,250 --> 00:00:05,000\r\nTwo\r\nlines\r\n\r\n"
        "3\n0:01:02.5 --> 0:01:04.75\nshort fractions, '.' separator\n";
    const SubtitleDoc d = parseSrt(srt);
    ASSERT_EQ(d.captions.size(), 3u);
    EXPECT_EQ(d.captions[0].start, Rational(1));
    EXPECT_EQ(d.captions[0].end, Rational(5, 2));
    EXPECT_EQ(d.captions[0].text, "Hello world");
    EXPECT_EQ(d.captions[1].start, Rational(3250, 1000));
    EXPECT_EQ(d.captions[1].text, "Two\nlines");
    EXPECT_EQ(d.captions[2].start, Rational(62500, 1000));   // 0:01:02.5 = 62.500 s
    EXPECT_EQ(d.captions[2].end, Rational(64750, 1000));
    EXPECT_EQ(d.encoding, "utf-8");
    EXPECT_TRUE(d.warnings.empty()) << d.warnings.front();
}

TEST(Srt, EncodingsAreDetectedAndPreserved) {
    // Arabic + accented Latin in UTF-8 with BOM, UTF-16LE with BOM and without BOM
    const std::string text = "1\n00:00:00,000 --> 00:00:01,000\n\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 caf\xc3\xa9\n";
    const SubtitleDoc a = parseSrt("\xEF\xBB\xBF" + text);
    ASSERT_EQ(a.captions.size(), 1u);
    EXPECT_EQ(a.captions[0].text, "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 caf\xc3\xa9");
    EXPECT_EQ(a.encoding, "utf-8 (BOM)");
    // UTF-16LE: encode the same text
    std::string u16 = "\xFF\xFE";
    const std::u16string w = u"1\n00:00:00,000 --> 00:00:01,000\nمرحبا café\n";
    for (char16_t c : w) {
        u16 += static_cast<char>(c & 0xFF);
        u16 += static_cast<char>(c >> 8);
    }
    const SubtitleDoc b = parseSrt(u16);
    ASSERT_EQ(b.captions.size(), 1u);
    EXPECT_EQ(b.captions[0].text, a.captions[0].text);
    EXPECT_EQ(b.encoding, "utf-16le (BOM)");
    const SubtitleDoc c = parseSrt(u16.substr(2));   // no BOM: detected by zero bytes
    ASSERT_EQ(c.captions.size(), 1u);
    EXPECT_EQ(c.captions[0].text, a.captions[0].text);
#if defined(_WIN32)
    // Windows-1256 (Arabic ANSI) when requested explicitly: 0xE3 0xD1 = meem reh
    const SubtitleDoc e = parseSrt(std::string("1\n00:00:00,000 --> 00:00:01,000\n\xE3\xD1\n"), 1256);
    ASSERT_EQ(e.captions.size(), 1u);
    EXPECT_EQ(e.captions[0].text, "\xd9\x85\xd8\xb1");
    EXPECT_EQ(e.encoding, "windows-1256");
#endif
}

TEST(Srt, WholeCaptionTagsBecomeStylesPartialTagsAreReported) {
    const std::string srt =
        "1\n00:00:01,000 --> 00:00:02,000\n<i>All italic</i>\n\n"
        "2\n00:00:02,000 --> 00:00:03,000\n<font color=\"#FFFF00\"><b>Yellow bold</b></font>\n\n"
        "3\n00:00:03,000 --> 00:00:04,000\nPartly <i>italic</i> text\n\n"
        "4\n00:00:04,000 --> 00:00:05,000\n{\\an8}On top\n";
    const SubtitleDoc d = parseSrt(srt);
    ASSERT_EQ(d.captions.size(), 4u);
    ASSERT_TRUE(d.captions[0].style);
    EXPECT_TRUE(d.captions[0].style->italic);
    EXPECT_EQ(d.captions[0].text, "All italic");
    ASSERT_TRUE(d.captions[1].style);
    EXPECT_EQ(d.captions[1].style->color, 0xFFFF00u);
    EXPECT_EQ(d.captions[1].style->weight, 700);
    EXPECT_FALSE(d.captions[2].style);
    EXPECT_EQ(d.captions[2].text, "Partly italic text");
    EXPECT_TRUE(hasWarning(d, "part of a caption"));
    ASSERT_TRUE(d.captions[3].style);
    EXPECT_EQ(d.captions[3].style->anchor, TextAnchor::Top);
    ASSERT_TRUE(d.captions[3].position);
    EXPECT_LT(d.captions[3].position->y, 0.2);
    EXPECT_EQ(d.captions[3].text, "On top");
}

TEST(Srt, MalformedInputIsReportedNotSilent) {
    const std::string srt =
        "1\n00:00:01,000 --> 00:00:02,000 X1:10 X2:20 Y1:5 Y2:6\nWith coordinates\n\n"
        "2\nnot a time line\nlost\n\n"
        "3\n00:00:05,000 --> 00:00:04,000\nBackwards\n";
    const SubtitleDoc d = parseSrt(srt);
    EXPECT_EQ(d.captions.size(), 2u);
    EXPECT_TRUE(hasWarning(d, "display coordinates"));
    EXPECT_TRUE(hasWarning(d, "without a time line"));
    EXPECT_TRUE(hasWarning(d, "ends before it starts"));
}

TEST(Srt, WriteAndReadBackIsExactToTheMillisecond) {
    std::vector<Caption> caps(3);
    caps[0] = {Rational(1001, 1000), Rational(2), "First"};
    caps[1] = {Rational(1001, 30), Rational(1101, 30), "NTSC\nframe times"};   // 33.3666... s
    caps[2] = {Rational(3600 + 61), Rational(3600 + 62), "\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85"};
    const std::string out = writeSrt(caps, Track::defaultSubtitleStyle());
    EXPECT_NE(out.find("00:00:33,367 --> 00:00:36,700"), std::string::npos) << out;
    EXPECT_NE(out.find("01:01:01,000"), std::string::npos);
    const SubtitleDoc d = parseSrt(out);
    ASSERT_EQ(d.captions.size(), 3u);
    EXPECT_EQ(d.captions[0].start, Rational(1001, 1000));
    EXPECT_EQ(d.captions[1].text, "NTSC\nframe times");
    EXPECT_EQ(d.captions[2].text, caps[2].text);
    EXPECT_EQ(formatSrtTime(Rational(1, 3)), "00:00:00,333");
}

// ---- ASS / SSA -------------------------------------------------------------------------------------

namespace {
const char* kAss =
    "[Script Info]\r\nScriptType: v4.00+\r\nPlayResX: 1920\r\nPlayResY: 1080\r\nWrapStyle: 0\r\n\r\n"
    "[V4+ Styles]\r\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, "
    "Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, "
    "MarginV, Encoding\r\n"
    "Style: Default,Arial,60,&H0000FFFF,&H000000FF,&H00000000,&H80000000,-1,0,0,0,100,100,0,0,1,3,2,2,40,40,54,1\r\n"
    "Style: Sign,Georgia,40,&H00FFFFFF,&H000000FF,&H00FF0000,&H00000000,0,-1,0,0,100,100,0,0,3,6,0,8,10,10,30,1\r\n\r\n"
    "[Events]\r\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\r\n"
    "Dialogue: 0,0:00:01.00,0:00:03.50,Default,,0,0,0,,Line one\\NLine two, with a comma\r\n"
    "Dialogue: 0,0:00:04.00,0:00:05.00,Sign,,0,0,0,,{\\pos(960,200)\\fad(250,500)}A sign\r\n"
    "Dialogue: 0,0:00:06.00,0:00:07.00,Default,,0,0,0,,{\\i1\\c&H0000FF&}Red italic{\\i0}\r\n"
    "Dialogue: 0,0:00:08.00,0:00:09.00,Default,,0,0,0,,Mid{\\b1}line{\\b0} bold and {\\k20}karaoke\r\n"
    "Comment: 0,0:00:10.00,0:00:11.00,Default,,0,0,0,,not shown\r\n"
    "Dialogue: 1,0:00:12.00,0:00:13.00,Default,,0,0,0,,{\\move(0,0,100,100)\\blur3}Moving\r\n";
}  // namespace

TEST(Ass, StylesEventsAndSupportedOverrides) {
    const SubtitleDoc d = parseAss(kAss);
    ASSERT_EQ(d.captions.size(), 5u);
    // document style = Default (most used): Arial, yellow (&H0000FFFF = BGR 00FFFF), bold, outline 3 px, shadow 2 px
    EXPECT_EQ(d.style.font, "Arial");
    EXPECT_EQ(d.style.color, 0xFFFF00u);
    EXPECT_EQ(d.style.weight, 700);
    EXPECT_TRUE(d.style.outline);
    EXPECT_NEAR(d.style.outlineWidth, 3.0, 1e-9);
    EXPECT_TRUE(d.style.shadow);
    EXPECT_NEAR(d.style.shadowOpacity, 1.0 - 128.0 / 255.0, 1e-9);   // BackColour alpha &H80
    EXPECT_EQ(d.style.anchor, TextAnchor::Bottom);
    EXPECT_NEAR(d.subtitleY, 1.0 - 54.0 / 1080.0, 1e-9);              // MarginV 54 at PlayResY 1080
    EXPECT_NEAR(d.style.wrapWidth, (1920.0 - 80) / 1920.0, 1e-9);
    // text: \N line break, commas inside the text field
    EXPECT_EQ(d.captions[0].text, "Line one\nLine two, with a comma");
    EXPECT_FALSE(d.captions[0].style);
    // Sign: other style (box), \pos and \fad
    ASSERT_TRUE(d.captions[1].style);
    EXPECT_EQ(d.captions[1].style->font, "Georgia");
    EXPECT_TRUE(d.captions[1].style->italic);
    EXPECT_TRUE(d.captions[1].style->background);              // BorderStyle 3
    EXPECT_EQ(d.captions[1].style->backgroundColor, 0x0000FFu);   // outline colour &H00FF0000 = blue box
    ASSERT_TRUE(d.captions[1].position);
    EXPECT_NEAR(d.captions[1].position->x, 0.5, 1e-9);
    EXPECT_NEAR(d.captions[1].position->y, 200.0 / 1080.0, 1e-9);
    EXPECT_EQ(d.captions[1].fadeIn, Rational(1, 4));
    EXPECT_EQ(d.captions[1].fadeOut, Rational(1, 2));
    // whole-line overrides; the trailing {\i0} changes nothing
    ASSERT_TRUE(d.captions[2].style);
    EXPECT_TRUE(d.captions[2].style->italic);
    EXPECT_EQ(d.captions[2].style->color, 0xFF0000u);
    EXPECT_EQ(d.captions[2].text, "Red italic");
    // mid-line changes and unsupported tags are reported, text is kept
    EXPECT_EQ(d.captions[3].text, "Midline bold and karaoke");
    EXPECT_TRUE(hasWarning(d, "middle of a line"));
    EXPECT_EQ(d.captions[4].text, "Moving");
    EXPECT_TRUE(hasWarning(d, "\\move is not supported"));
    EXPECT_TRUE(hasWarning(d, "\\blur is not supported"));
    EXPECT_TRUE(hasWarning(d, "layers are not supported"));
    EXPECT_TRUE(hasWarning(d, "Comment events skipped"));
}

TEST(Ass, LegacySsaAlignmentAndColours) {
    const std::string ssa =
        "[Script Info]\nScriptType: v4.00\nPlayResY: 288\n\n[V4 Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, TertiaryColour, BackColour, Bold, Italic, "
        "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, AlphaLevel, Encoding\n"
        "Style: Default,Tahoma,24,16777215,65535,0,0,0,0,1,2,0,6,10,10,20,0,0\n\n"
        "[Events]\nFormat: Marked, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: Marked=0,0:00:00.50,0:00:01.50,Default,,0000,0000,0000,,Top centre\n";
    const SubtitleDoc d = parseAss(ssa);
    ASSERT_EQ(d.captions.size(), 1u);
    EXPECT_EQ(d.style.font, "Tahoma");
    EXPECT_EQ(d.style.color, 0xFFFFFFu);        // decimal BGR 16777215 = white
    EXPECT_EQ(d.style.anchor, TextAnchor::Top);  // SSA alignment 6 = top centre
    EXPECT_EQ(d.style.align, TextAlign::Center);
    EXPECT_EQ(d.captions[0].start, Rational(1, 2));
    // size: Fontsize 24 at PlayResY 288 -> em ~ 24 * 0.87 * 1080/288 px at 1080p
    EXPECT_NEAR(d.style.size * kTitlePxPerSizeAt1080, 24 * 0.87 * 1080.0 / 288.0, 1e-6);
}

TEST(Ass, WriteThenReadKeepsCaptionsAndStyle) {
    TextStyle doc = Track::defaultSubtitleStyle();
    doc.font = "Segoe UI";
    doc.color = 0xFFEE00;
    doc.background = true;
    doc.backgroundColor = 0x102030;
    std::vector<Caption> caps(2);
    caps[0] = {Rational(1), Rational(2), "Hello\nthere"};
    caps[1] = {Rational(3), Rational(4), "Placed"};
    caps[1].position = Vec2{0.25, 0.3};
    caps[1].fadeIn = Rational(1, 5);
    caps[1].fadeOut = Rational(3, 10);
    TextStyle red = doc;
    red.color = 0xFF0000;
    red.italic = true;
    caps[1].style = red;
    const std::string out = writeAss(caps, doc, 0.9, 1920, 1080);
    const SubtitleDoc d = parseAss(out);
    EXPECT_EQ(d.encoding, "utf-8 (BOM)");
    ASSERT_EQ(d.captions.size(), 2u);
    EXPECT_EQ(d.captions[0].text, "Hello\nthere");
    EXPECT_EQ(d.captions[0].start, Rational(1));
    EXPECT_EQ(d.style.color, 0xFFEE00u);
    EXPECT_TRUE(d.style.background);
    EXPECT_EQ(d.style.backgroundColor, 0x102030u);
    EXPECT_NEAR(d.style.size, doc.size, 0.01);
    EXPECT_NEAR(d.subtitleY, 0.9, 0.002);
    ASSERT_TRUE(d.captions[1].style);
    EXPECT_EQ(d.captions[1].style->color, 0xFF0000u);
    EXPECT_TRUE(d.captions[1].style->italic);
    ASSERT_TRUE(d.captions[1].position);
    EXPECT_NEAR(d.captions[1].position->x, 0.25, 1e-3);
    EXPECT_EQ(d.captions[1].fadeIn, Rational(1, 5));
    EXPECT_EQ(d.captions[1].fadeOut, Rational(3, 10));
    EXPECT_TRUE(d.warnings.empty()) << d.warnings.front();
}

// ---- model + edit operations ----------------------------------------------------------------------

TEST(TitleModel, SerializationRoundTrip) {
    Project p = emptyProject();
    Clip c;
    c.id = "t1";
    c.duration = Rational(5);
    TitleData t;
    t.text = "Line 1\n\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a \xf0\x9f\x91\x8b";
    t.style.font = "Arial";
    t.style.size = 64;
    t.style.weight = 800;
    t.style.italic = true;
    t.style.color = 0x12ABEF;
    t.style.gradient = true;
    t.style.outline = true;
    t.style.shadow = true;
    t.style.shadowAngle = 45;
    t.style.glow = true;
    t.style.background = true;
    t.style.align = TextAlign::Right;
    t.style.wrapWidth = 0.5;
    t.in = {"typewriter", Rational(4, 5)};
    t.loop = {"bounce", Time(0)};
    t.out = {"shake_fade", Rational(21, 25)};
    t.templateId = "basic.default";
    c.title = t;
    c.transform.positionY.setConstant(0.3633);
    p.sequences[0].tracks[0].clips.push_back(c);
    Track sub;
    sub.id = "s1";
    sub.kind = TrackKind::Subtitle;
    sub.subtitleStyle.color = 0xFFFF00;
    sub.subtitleY = 0.88;
    Clip cap;
    cap.id = "c1";
    cap.duration = Rational(2);
    TitleData ct;
    ct.text = "Caption";
    ct.followTrackStyle = true;
    cap.title = ct;
    sub.clips.push_back(cap);
    p.sequences[0].tracks.push_back(sub);
    const std::string json = projectToJsonString(p);
    EXPECT_EQ(json.find("\"source\""), json.find("\"source\""));   // titles write no source
    const Project back = projectFromJsonString(json);
    EXPECT_EQ(back, p);
    EXPECT_TRUE(validate(back).empty()) << formatIssues(validate(back));
}

TEST(TitleModel, ValidationRules) {
    Project p = emptyProject();
    Track a;
    a.id = "a1";
    a.kind = TrackKind::Audio;
    Clip t;
    t.id = "t";
    t.duration = Rational(1);
    t.title = TitleData{};
    a.clips.push_back(t);
    p.sequences[0].tracks.push_back(a);
    EXPECT_TRUE(hasErrors(validate(p)));   // titles do not belong on audio tracks
    p.sequences[0].tracks.pop_back();
    Track s;
    s.id = "s1";
    s.kind = TrackKind::Subtitle;
    Clip m;
    m.id = "m";
    m.sourceId = "src";
    m.duration = Rational(1);
    s.clips.push_back(m);
    p.sequences[0].tracks.push_back(s);
    EXPECT_TRUE(hasErrors(validate(p)));   // media clips do not belong on subtitle tracks
}

TEST(TitleEdits, AddTitleGoesAboveVideoAndUndoes) {
    Project p = emptyProject();
    Clip proto;
    proto.title = TitleData{};
    std::string id;
    auto cmd = edit::addTitle(p, "seq", proto, Rational(2), edit::kDefaultTitleDuration, {}, &id);
    UndoStack st(p);
    st.execute(std::move(cmd));
    const Sequence& s = p.sequences[0];
    ASSERT_EQ(s.tracks.size(), 2u);   // a new video track above Video 1
    const Track* tr = nullptr;
    const Clip* c = edit::findClip(s, id, &tr);
    ASSERT_TRUE(c);
    EXPECT_EQ(tr->kind, TrackKind::Video);
    EXPECT_NE(tr->id, "v1");
    EXPECT_EQ(c->timelineStart, Rational(2));
    EXPECT_EQ(c->duration, Rational(5));   // EXP-003 default 5 s
    EXPECT_EQ(c->name, "Text Here");
    const std::string titleTrack = tr->id;   // (pointers into the model do not survive later edits)
    // a second title at the same time goes to yet another track; one at another time reuses Video 2
    std::string id2, id3;
    st.execute(edit::addTitle(p, "seq", proto, Rational(3), Rational(1), {}, &id2));
    st.execute(edit::addTitle(p, "seq", proto, Rational(10), Rational(1), {}, &id3));
    EXPECT_EQ(p.sequences[0].tracks.size(), 3u);
    const Track *t2 = nullptr, *t3 = nullptr;
    edit::findClip(p.sequences[0], id2, &t2);
    edit::findClip(p.sequences[0], id3, &t3);
    EXPECT_NE(t2->id, titleTrack);
    EXPECT_EQ(t3->id, titleTrack);
    st.undo();
    st.undo();
    st.undo();
    EXPECT_EQ(p.sequences[0].tracks.size(), 1u);
}

// Subtitle import inserts all captions of a track with one command (one track snapshot for undo).
TEST(TitleEdits, BulkInsertChecksOverlapsAndUndoesInOneStep) {
    Project p = emptyProject();
    auto caption = [](int startFrame, int frames) {
        Clip c;
        c.title = TitleData{};
        c.timelineStart = Rational(startFrame, 30);
        c.duration = Rational(frames, 30);
        return c;
    };
    std::vector<Clip> many;
    for (int i = 0; i < 500; ++i) many.push_back(caption(i * 10, 9));
    UndoStack st(p);
    st.execute(makeAddTrack(p, "seq", TrackKind::Subtitle, std::nullopt, "Subtitles 1"));
    const std::string sub = p.sequences[0].tracks.back().id;
    st.execute(makeInsertClips(p, "seq", sub, many));
    const Track& t = *p.sequences[0].findTrack(sub);
    ASSERT_EQ(t.clips.size(), 500u);
    EXPECT_TRUE(std::is_sorted(t.clips.begin(), t.clips.end(),
                               [](const Clip& a, const Clip& b) { return a.timelineStart < b.timelineStart; }));
    std::set<std::string> ids;
    for (const auto& c : t.clips) ids.insert(c.id);
    EXPECT_EQ(ids.size(), 500u);   // ids allocated
    EXPECT_TRUE(validate(p).empty()) << formatIssues(validate(p));
    // overlapping with an existing caption, or with each other: rejected, nothing changes
    EXPECT_THROW(st.execute(makeInsertClips(p, "seq", sub, {caption(5, 3)})), EditError);
    EXPECT_THROW(st.execute(makeInsertClips(p, "seq", sub, {caption(6000, 10), caption(6005, 10)})), EditError);
    EXPECT_EQ(p.sequences[0].findTrack(sub)->clips.size(), 500u);
    // media clips are refused on a subtitle track
    Clip media;
    media.sourceId = "nope";
    media.duration = Rational(1);
    EXPECT_THROW(st.execute(makeInsertClips(p, "seq", sub, {media})), EditError);
    st.undo();   // the whole bulk insert in one step
    EXPECT_TRUE(p.sequences[0].findTrack(sub)->clips.empty());
    st.redo();
    EXPECT_EQ(p.sequences[0].findTrack(sub)->clips.size(), 500u);
}

TEST(TitleEdits, CaptionsAreFrameSnappedAndOverlapsGetASecondTrack) {
    Project p = emptyProject(FrameRate::ntsc30());
    std::vector<Caption> caps(3);
    caps[0] = {Rational(1), Rational(2), "one"};
    caps[1] = {Rational(3, 2), Rational(5, 2), "overlaps one"};
    caps[2] = {Rational(3), Rational(3), "zero length"};
    caps[2].position = Vec2{0.5, 0.1};
    std::vector<std::string> tracks;
    UndoStack st(p);
    st.execute(edit::addCaptions(p, "seq", caps, Track::defaultSubtitleStyle(), 0.9, &tracks));
    ASSERT_EQ(tracks.size(), 2u);
    const Sequence& s = p.sequences[0];
    const Track* first = s.findTrack(tracks[0]);
    ASSERT_EQ(first->kind, TrackKind::Subtitle);
    ASSERT_EQ(first->clips.size(), 2u);
    for (const auto& c : first->clips) {
        EXPECT_TRUE(isFrameAligned(c.timelineStart, s.frameRate));
        EXPECT_TRUE(isFrameAligned(c.duration, s.frameRate));
        EXPECT_GT(c.duration, Time(0));
    }
    EXPECT_EQ(first->clips[1].duration, s.frameRate.frameDuration());   // zero-length caption: one frame
    EXPECT_NEAR(first->clips[1].transform.positionY.constantValue(), -0.4, 1e-12);
    EXPECT_TRUE(first->clips[0].title->followTrackStyle);
    EXPECT_NEAR(first->subtitleY, 0.9, 1e-12);
    EXPECT_TRUE(validate(p).empty()) << formatIssues(validate(p));
    // export -> import keeps the text and (millisecond-rounded) times
    const auto back = parseSrt(writeSrt(captionsOfTrack(*first), first->subtitleStyle));
    ASSERT_EQ(back.captions.size(), 2u);
    EXPECT_EQ(back.captions[0].text, "one");
    st.undo();
    EXPECT_EQ(p.sequences[0].tracks.size(), 1u);
}

TEST(TitleEdits, AddCaptionAtPlayhead) {
    Project p = emptyProject();
    UndoStack st(p);
    std::string id;
    st.execute(edit::addCaption(p, "seq", "Hi", Rational(4), Rational(2), {}, &id));
    const Track* tr = nullptr;
    const Clip* c = edit::findClip(p.sequences[0], id, &tr);
    ASSERT_TRUE(c);
    EXPECT_EQ(tr->kind, TrackKind::Subtitle);
    // the next caption is shortened so captions on one track never overlap
    std::string id2;
    st.execute(edit::addCaption(p, "seq", "Before", Rational(3), Rational(3), tr->id, &id2));
    EXPECT_EQ(edit::findClip(p.sequences[0], id2)->duration, Rational(1));
    EXPECT_THROW(edit::addCaption(p, "seq", "Inside", Rational(5), Rational(1), tr->id), EditError);
}

// ---- animation timing --------------------------------------------------------------------------------

TEST(TitleAnimation, TypeWriterMatchesFilmoraExp003) {
    // EXP-003: "AbgQ Hxy 123" (12 characters incl. spaces), 25 fps: visible = round(12 * n / 20), n = 0..20
    TitleData t;
    t.text = "AbgQ Hxy 123";
    t.in = {"typewriter", Time(0)};   // default 0.8 s
    TextUnitCounts counts;
    counts.chars = 12;
    for (int n = 0; n <= 24; ++n) {
        const auto s = evaluateTitleAnimation(t, Rational(n, 25), Rational(5), counts);
        const int expected = std::min(12, static_cast<int>(std::floor(12.0 * n / 20.0 + 0.5)));
        int visible = 0;
        for (const auto& u : s.units) visible += u.opacity > 0.5;
        if (n < 20) {
            ASSERT_EQ(s.units.size(), 12u) << n;
            EXPECT_EQ(visible, expected) << "frame " << n;
        } else {
            EXPECT_TRUE(s.atRest()) << "frame " << n;   // static afterwards (Filmora: static until the clip end)
        }
    }
}

TEST(TitleAnimation, InLoopOutWindowsAndScaling) {
    TitleData t;
    t.in = {"fade", Rational(1)};
    t.out = {"fade", Rational(1)};
    t.loop = {"pulse", Rational(1)};
    const TextUnitCounts counts;
    EXPECT_DOUBLE_EQ(evaluateTitleAnimation(t, Rational(1, 2), Rational(5), counts).opacity, 0.5);   // In
    const auto mid = evaluateTitleAnimation(t, Rational(5, 2), Rational(5), counts);                 // Loop
    EXPECT_DOUBLE_EQ(mid.opacity, 1.0);
    EXPECT_NEAR(mid.scale, 1.08, 1e-9);   // pulse: phase 0.5 of the 1 s period after the In
    EXPECT_DOUBLE_EQ(evaluateTitleAnimation(t, Rational(9, 2), Rational(5), counts).opacity, 0.5);   // Out
    // clip shorter than In + Out: both scaled to fit (In 0.75 s and Out 0.75 s of a 1.5 s clip)
    const auto w = titleAnimWindows(t, Rational(3, 2));
    EXPECT_EQ(w.inEnd, Rational(3, 4));
    EXPECT_EQ(w.outStart, Rational(3, 4));
}

TEST(TitleAnimation, BouncingAndFadeOutShakeMatchExp003) {
    // Bouncing: period 1.6 s; top ~59 px up at 0.56 s, bottom ~120 px down at 1.12 s after the loop start
    TitleData b;
    b.loop = {"bounce", Time(0)};
    const TextUnitCounts counts;
    EXPECT_NEAR(evaluateTitleAnimation(b, Rational(56, 100), Rational(10), counts).dy, -59.0, 1e-6);
    EXPECT_NEAR(evaluateTitleAnimation(b, Rational(112, 100), Rational(10), counts).dy, 120.0, 1e-6);
    EXPECT_NEAR(evaluateTitleAnimation(b, Rational(56, 100) + Rational(8, 5), Rational(10), counts).dy, -59.0, 1e-6);
    double lowest = 0, highest = 0;
    for (int f = 0; f < 40; ++f) {
        const double dy = evaluateTitleAnimation(b, Rational(f, 25), Rational(10), counts).dy;
        lowest = std::min(lowest, dy);
        highest = std::max(highest, dy);
    }
    EXPECT_NEAR(lowest, -59.0, 3.0);
    EXPECT_NEAR(highest, 120.0, 3.0);
    // Fade Out Shake on a 125-frame (25 fps) clip: shake from frame 104, fade over the last 6 frames
    TitleData o;
    o.out = {"shake_fade", Time(0)};   // 0.84 s
    const Time D(5);
    EXPECT_TRUE(evaluateTitleAnimation(o, Rational(103, 25), D, counts).atRest());
    const auto shake = evaluateTitleAnimation(o, Rational(110, 25), D, counts);
    EXPECT_DOUBLE_EQ(shake.opacity, 1.0);
    EXPECT_LE(std::abs(shake.dx), 3.0);
    EXPECT_LE(std::abs(shake.dy), 1.4);
    EXPECT_GT(std::abs(shake.dx) + std::abs(shake.dy), 0.0);
    EXPECT_NEAR(evaluateTitleAnimation(o, Rational(121, 25), D, counts).opacity, 0.667, 0.02);
    EXPECT_LT(evaluateTitleAnimation(o, Rational(124, 25), D, counts).opacity, 0.2);
}

TEST(TitleAnimation, DeterministicAcrossFrameRates) {
    // the same instant gives the same state whichever frame rate samples it
    TitleData t;
    t.text = "Hello there";
    t.in = {"char_rise", Rational(1)};
    t.loop = {"shake", Time(0)};
    TextUnitCounts counts;
    counts.chars = 11;
    const Time at = Rational(1, 2);   // frame 15 at 30 fps, frame 12 at 24 fps, 30 at 60 fps
    const auto a = evaluateTitleAnimation(t, frameToTime(15, FrameRate::fps30()), Rational(4), counts);
    const auto b = evaluateTitleAnimation(t, frameToTime(12, FrameRate(24, 1)), Rational(4), counts);
    const auto c = evaluateTitleAnimation(t, at, Rational(4), counts);
    ASSERT_EQ(a.units.size(), 11u);
    EXPECT_EQ(a.units, b.units);
    EXPECT_EQ(a.units, c.units);
    // word and line units use their own counts
    TitleData w;
    w.in = {"word_reveal", Rational(1)};
    counts.words = 4;
    EXPECT_EQ(evaluateTitleAnimation(w, Rational(1, 2), Rational(3), counts).units.size(), 4u);
    EXPECT_EQ(evaluateTitleAnimation(w, Rational(1, 2), Rational(3), counts).unit, TextAnimUnit::Word);
}

TEST(TitleAnimation, EveryPresetHasAWorkingEvaluation) {
    TextUnitCounts counts;
    counts.chars = 8;
    counts.words = 2;
    counts.lines = 1;
    counts.blockHeight = 100;
    for (const auto& p : textAnimPresets()) {
        TitleData t;
        TextAnimation slot{p.id, Time(0)};
        if (p.stage == TextAnimStage::In) t.in = slot;
        else if (p.stage == TextAnimStage::Out) t.out = slot;
        else t.loop = slot;
        const Time D(10);
        std::vector<Time> probes;
        if (p.stage == TextAnimStage::In) probes = {Rational(p.defaultMs, 4000)};
        else if (p.stage == TextAnimStage::Out) probes = {D - Rational(p.defaultMs, 4000)};
        else probes = {Rational(13, 10), Rational(17, 10), Rational(21, 10)};   // loops: a few phases
        bool moves = false;
        for (const Time& probe : probes) moves = moves || !evaluateTitleAnimation(t, probe, D, counts).atRest();
        EXPECT_TRUE(moves) << p.label << " does nothing inside its window";
    }
}
