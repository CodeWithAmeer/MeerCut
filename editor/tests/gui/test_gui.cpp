// GUI integration tests: drive the real MainWindow with mouse/keyboard events and check the model,
// the preview frames and the exported file. Media: Stage 1 A_red (1 kHz) / B_blue (1.5 kHz) clips with
// frame-index strips.
#include "dialogs.h"
#include "autosave.h"
#include "legacy_rename.h"
#include "library_pages.h"
#include "animation_panel.h"
#include "templates.h"
#include "fa/text/stickers.h"
#include "inspector_panel.h"
#include "preview_canvas.h"
#include "edit_controller.h"
#include "main_window.h"
#include "panels.h"
#include "playback_controller.h"
#include "theme.h"
#include "timeline_view.h"
#include "title_inspector.h"
#include "color_panel.h"
#include "color_presets.h"
#include "lut_library.h"
#include "effects_browser.h"
#include "effect_editor.h"
#include "title_library.h"
#include "speed_panel.h"
#include "speed_ramp_editor.h"
#include "render_preview_controller.h"
#include "stabilization_panel.h"
#include "tracking_panel.h"
#include "analysis_service.h"
#include "level_meter.h"
#include "model_manager.h"
#include "cutout_panel.h"
#include "speech_dialog.h"
#include "scene_dialog.h"
#include "reframe_dialog.h"
#include "upscale_panel.h"
#include "fa/ai/interpolate.h"
#include "fa/ai/prepare.h"
#include "fa/ai/upscale.h"
#include "fa/render/frame_store.h"
#include "fa/ai/reframe.h"

#include "fa/ai/inference.h"
#include "fa/ai/matting.h"
#include "fa/ai/models.h"

#include "fa/media/media.h"
#include "fa/model/edit_ops.h"
#include "fa/model/keyframes.h"
#include "fa/render/audio_dsp.h"
#include "fa/render/audio_mix.h"
#include "fa/render/renderer.h"
#include "fa/render/transitions.h"
#include "fa/model/serialize.h"
#include "fa/model/stabilize_plan.h"
#include "fa/text/title_templates.h"
#include "fa/text/title_engine.h"
#include "fa/render/effects.h"
#include "fa/model/commands.h"

#include "../support/framecode.h"

#include <QAction>
#include <QFile>
#include <QMenuBar>
#include <QSettings>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTextBrowser>
#include <QMimeData>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QLabel>
#include <QLineF>
#include <QSplitter>
#include <QUrl>
#include <QTabWidget>
#include <QToolButton>
#include <QPushButton>
#include <QStandardItemModel>
#include <QRadioButton>
#include <QGroupBox>
#include <QPlainTextEdit>
#include <QMenu>
#include <QLineEdit>
#include <QListWidget>
#include <QTimer>
#include <QContextMenuEvent>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QLockFile>
#include <QProcess>
#include <QStatusBar>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <numbers>
#include <set>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // WideCharToMultiByte: a Windows-1256 subtitle file for the encoding test
#include <crtdbg.h>

using namespace fa;
using namespace fa::ui;

namespace {

// Hard failures leave a trace in the log: Debug CRT assertions print instead of opening a dialog, and abort()
// (e.g. an exception escaping a worker thread) says so before the process ends.
struct CrashTrace {
    CrashTrace() {
#if defined(_WIN32) && defined(_DEBUG)
        for (int t : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
            _CrtSetReportMode(t, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(t, _CRTDBG_FILE_STDERR);
        }
#endif
        std::signal(SIGABRT, [](int) {
            std::fputs("FATAL: abort() (assertion or uncaught exception, possibly on a worker thread)\n", stderr);
            std::fflush(stderr);
            std::fflush(stdout);
        });
    }
} crashTrace;

QString mediaPath(const char* name) { return QDir(FA_TESTMEDIA_DIR).filePath(name); }

QString screenshotDir() {
    const QString d = qEnvironmentVariable("FA_SCREENSHOT_DIR");
    if (!d.isEmpty()) QDir().mkpath(d);
    return d;
}

void shot(QWidget* w, const QString& name) {
    const QString d = screenshotDir();
    if (d.isEmpty()) return;
    QApplication::processEvents();
    w->grab().save(QDir(d).filePath(name));
}

// Wait until the preview shows `frame` (rendered from the current project version) and return it.
QImage waitForPreview(MainWindow& w, int64_t frame, int timeoutMs = 8000) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        if (w.playback()->currentImageFrame() == frame && !w.playback()->currentImage().isNull() &&
            w.document()->playhead() == frame && w.playback()->currentImageVersion() == w.document()->version())
            return w.playback()->currentImage();
        QTest::qWait(10);
    }
    return {};
}

int stripIndex(const QImage& img) {
    if (img.isNull()) return -2;
    const QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
    Image im(rgba.width(), rgba.height());
    for (int y = 0; y < rgba.height(); ++y) std::memcpy(im.row(y), rgba.constScanLine(y), rgba.width() * 4);
    return test::readStrip(im).index;
}

// The event sequence Windows delivers for a double-click (press, release, double-click, release).
void doubleClick(QWidget* w, const QPoint& pos) {
    const QPointF local(pos);
    const QPointF global(w->mapToGlobal(pos));
    for (QEvent::Type t : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease, QEvent::MouseButtonDblClick,
                           QEvent::MouseButtonRelease}) {
        const Qt::MouseButtons buttons = t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent ev(t, local, global, Qt::LeftButton, buttons, Qt::NoModifier);
        QApplication::sendEvent(w, &ev);
    }
    QApplication::processEvents();
}

QColor centre(const QImage& img) { return img.pixelColor(img.width() / 2, img.height() / 2); }

double toneAmplitude(const std::vector<float>& x, int64_t from, int n, double f) {
    double c = 0, s = 0;
    for (int i = 0; i < n; ++i) {
        const double v = x[static_cast<size_t>((from + i) * 2)];
        c += v * std::cos(2 * std::numbers::pi * f * i / 48000);
        s += v * std::sin(2 * std::numbers::pi * f * i / 48000);
    }
    return 2 * std::hypot(c, s) / n;
}

std::filesystem::path p8(const QString& s) {
    const std::string u = QDir::toNativeSeparators(s).toUtf8().toStdString();
    return std::filesystem::path(std::u8string(u.begin(), u.end()));
}

}  // namespace

class GuiTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        applyTheme(*qApp);
        // M8: test windows never write into the user's autosave folder
        qputenv("MEERCUT_AUTOSAVE_DIR", QDir(QDir::tempPath()).filePath("meercut_gui_tests_autosave").toUtf8());
        // M9: library files are copied into a temporary library, never the user's
        qputenv("MEERCUT_LIBRARY_DIR", QDir(QDir::tempPath()).filePath("meercut_gui_tests_library").toUtf8());
    }
    void workflowImportEditSaveReopenExport();
    void playbackFollowsTheClockAndStopsExactly();
    void missingMediaIsReportedAndRelinkable();
    void exportCanBeCancelled();
    void interfaceStaysResponsive();
    void transitionsOpacityAndTrackToggles();
    void m2TransformKeyframesMasksBlend();
    void m2TransitionsBrowserAndSettings();
    void m2LayoutAndFileDrops();
    void m3TitlesBrowserInspectorPreview();
    void m3AnimationsKeyframesExport();
    void m3SubtitlesImportEditExportBurnIn();
    void m4EffectStackEditor();
    void m4ColorTab();
    void m4LutsAndHdr();
    void m4EffectsAndFiltersBrowsers();
    void m4AdjustmentLayersAndMaskedEffects();
    void m4BeforeAfterCompare();
    void m5SpeedReverseFreeze();
    void m5SpeedRamping();
    void m5MotionTracking();
    void m5Stabilization();
    void m5RenderPreview();
    void m6AudioMixing();
    void m6AudioCleanupAndDucking();
    void m6ExportWindow();
    void m7ModelManager();
    void m7BackgroundRemoval();
    void m7SpeechToText();
    void m7SceneDetection();
    void m7AutoReframe();
    void m7AiInterpolation();
    void m7VideoUpscaling();
    void m7IntegrationEveryFeature();
    void m7OlderExampleProjects();
    void m7AiToolsAreDiscoverable();
    void libraryDoubleClickAddsToTimeline();
    void startupImportStaysResponsive();
    void autosaveAndCrashRecovery();
    void longProjectStaysResponsive();
    void relativeMediaPathsResolveAgainstTheProject();
    void m9ClipboardShortcutsAndClipMenu();
    void m9AudioStickersTemplatesPages();
    void m9InspectorTabsAnimationAndStickerLook();
    void m10DeleteGap();
    void m10SoundsLandOnTheAudioTrackUnderTheMouse();
    void m10EffectsAndFiltersDropAsLayers();
    void m10EnlargedTextStaysSharp();
    void m10StickersMoveAndResizeInThePlayer();
    void m10RenamedToMeerCutWithGitHub();
    void m11SampleVideos();
    void readmeScreenshots();
};

void GuiTests::transitionsOpacityAndTrackToggles() {
    MainWindow w(MainWindow::Options{false, true});
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4"), mediaPath("B_blue_1080p30_10s.mp4")});
    QVERIFY(w.editController()->addToTimeline(ids[0], Rational(0)));
    QVERIFY(w.editController()->addToTimeline(ids[1], Rational(10)));
    TimelineView* tl = w.timelinePanel()->view();
    tl->zoomToFit();
    QApplication::processEvents();
    const std::string a = doc->sequence().tracks[0].clips[0].id;

    // Dissolve dropped onto the cut (drag & drop data as the Transitions panel produces it)
    const QRect ra = tl->clipRect(a);
    QMimeData mime;
    mime.setData(kMimeTransition, "fa.transition.dissolve");
    const QPoint cut(ra.right(), ra.center().y());
    QDragEnterEvent enter(cut, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &enter);
    QDragMoveEvent move(cut, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &move);
    QDropEvent drop(QPointF(cut), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &drop);
    const Clip* ca = edit::findClip(doc->sequence(), a);
    QVERIFY(ca->outTransition.has_value());
    QCOMPARE(ca->outTransition->duration, Rational(1));
    // at the cut the preview shows the 50/50 mix (same renderFrame as export)
    w.playback()->seek(300);
    const QImage mid = waitForPreview(w, 300);
    QVERIFY(!mid.isNull());
    QVERIFY2(centre(mid).red() > 80 && centre(mid).blue() > 80,
             qPrintable(QString("mix colour %1").arg(centre(mid).name())));
    shot(&w, "m1_05_dissolve.png");

    // opacity through the inspector's spin box (select A, type 50, Enter)
    w.playback()->seek(60);
    doc->setSelection({a});
    QApplication::processEvents();
    auto* spin = w.findChild<QDoubleSpinBox*>("prop:opacity");
    QVERIFY(spin);
    spin->setFocus();
    spin->selectAll();
    QTest::keyClicks(spin, "50");
    QTest::keyClick(spin, Qt::Key_Return);
    QTRY_COMPARE(edit::findClip(doc->sequence(), a)->opacity.constantValue(), 0.5);
    const QImage half = waitForPreview(w, 60);
    QVERIFY2(std::abs(centre(half).red() - 104) < 12, qPrintable(QString("half red %1").arg(centre(half).red())));
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);   // undo restores full opacity
    QCOMPARE(edit::findClip(doc->sequence(), a)->opacity.constantValue(), 1.0);

    // track header buttons: hide Video 1 (eye) -> black preview; mute and lock Audio 1
    const QRect lane = tl->trackLaneRect(doc->sequence().tracks[0].id);
    QTest::mouseClick(tl, Qt::LeftButton, {}, QPoint(50, lane.bottom() - 18));   // eye icon
    QVERIFY(doc->sequence().tracks[0].hidden);
    QVERIFY(centre(waitForPreview(w, 60)).red() < 10);
    const QRect alane = tl->trackLaneRect(doc->sequence().tracks[1].id);
    QTest::mouseClick(tl, Qt::LeftButton, {}, QPoint(50, alane.bottom() - 18));  // speaker icon
    QVERIFY(doc->sequence().tracks[1].muted);
    QTest::mouseClick(tl, Qt::LeftButton, {}, QPoint(22, alane.bottom() - 18));  // lock icon
    QVERIFY(doc->sequence().tracks[1].locked);
    QVERIFY(!w.editController()->moveClip(doc->sequence().tracks[1].clips[0].id, Rational(1), std::nullopt));
}

void GuiTests::interfaceStaysResponsive() {
    MainWindow w(MainWindow::Options{false, true});
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4"), mediaPath("B_blue_1080p30_10s.mp4"),
                                    mediaPath("C_testsrc2_1080p30_10s.mp4")});
    for (const auto& id : ids) QVERIFY(w.editController()->addToTimeline(id, w.document()->sequence().duration()));
    w.timelinePanel()->view()->zoomToFit();

    // layout stability: selecting a clip (taller inspector page) must not move the timeline
    w.document()->setSelection({});
    QApplication::processEvents();
    const QPoint before = w.timelinePanel()->view()->mapToGlobal(QPoint(0, 0));
    w.document()->setSelection({w.document()->sequence().tracks[0].clips[0].id});
    QApplication::processEvents();
    QCOMPARE(w.timelinePanel()->view()->mapToGlobal(QPoint(0, 0)), before);

    // seek latency for uncached frames (decode + composite at preview size, off the GUI thread)
    QElapsedTimer t;
    double worst = 0, total = 0;
    const int64_t frames[] = {700, 35, 410, 820, 5, 299, 300, 615, 150, 899};
    for (int64_t f : frames) {
        t.start();
        w.playback()->seek(f);
        QVERIFY(!waitForPreview(w, f).isNull());
        const double ms = t.nsecsElapsed() / 1e6;
        worst = std::max(worst, ms);
        total += ms;
    }
    qInfo("seek -> frame on screen: mean %.1f ms, worst %.1f ms", total / std::size(frames), worst);
    QVERIFY2(worst < 500, "seeking must show the frame within half a second");

    // timeline repaint cost (custom painted view with filmstrips and waveforms)
    TimelineView* tl = w.timelinePanel()->view();
    t.start();
    for (int i = 0; i < 20; ++i) tl->repaint();
    const double paintMs = t.nsecsElapsed() / 1e6 / 20;
    qInfo("timeline repaint: %.2f ms", paintMs);
    QVERIFY(paintMs < 40);

    // the event loop keeps running while an export renders in the background
    QTemporaryDir tmp;
    ExportDialog d(w.document(), &w);
    QElapsedTimer gap;
    double maxGap = 0;
    QTimer probe;
    probe.setInterval(10);
    gap.start();
    QObject::connect(&probe, &QTimer::timeout, [&] {
        maxGap = std::max(maxGap, gap.nsecsElapsed() / 1e6);
        gap.restart();
    });
    probe.start();
    QString err;
    QVERIFY2(d.exportTo(tmp.filePath("bg.mp4"), &err), qPrintable(err));
    probe.stop();
    qInfo("longest GUI event-loop stall during a 30 s 1080p export: %.1f ms", maxGap);
    QVERIFY(maxGap < 250);
}

void GuiTests::workflowImportEditSaveReopenExport() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 960);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    TimelineView* tl = w.timelinePanel()->view();
    const FrameRate rate = doc->sequence().frameRate;

    // 1. IMPORT
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4"), mediaPath("B_blue_1080p30_10s.mp4")});
    QCOMPARE(ids.size(), size_t(2));
    QCOMPARE(w.mediaPanel()->list()->count(), 3);   // import tile + 2 media
    QVERIFY(doc->project().findSource(ids[0])->hasAudio);

    // 2. ADD TO TIMELINE: double-click A in the media library (adds at the playhead), then B at the end
    QListWidget* list = w.mediaPanel()->list();
    // icon-mode lists lay out lazily: wait until item A is really under its rectangle
    QTRY_VERIFY(list->itemAt(list->visualItemRect(list->item(1)).center()) == list->item(1));
    const QRect itemA = list->visualItemRect(list->item(1));
    doubleClick(list->viewport(), itemA.center());
    QTRY_VERIFY2(doc->sequence().tracks[0].clips.size() == 1, qPrintable("last message: " + w.lastMessage()));
    w.playback()->toEnd();
    QVERIFY(w.editController()->addToTimeline(ids[1], Rational(10)));
    const Sequence* s = &doc->sequence();
    QCOMPARE(s->frameCount(), int64_t(600));
    const std::string a = s->tracks[0].clips[0].id, b = s->tracks[0].clips[1].id;
    QCOMPARE(edit::linkGroup(*s, a).size(), size_t(2));   // video + linked audio
    tl->zoomToFit();
    shot(&w, "m1_01_two_clips.png");

    // 3. PREVIEW: frame-accurate source frames on both sides of the cut
    for (int64_t f : {0, 150, 299, 300, 450}) {
        w.playback()->seek(f);
        const QImage img = waitForPreview(w, f);
        QVERIFY2(!img.isNull(), qPrintable(QString("no preview for frame %1").arg(f)));
        QCOMPARE(stripIndex(img), int(f < 300 ? f : f - 300));
        const QColor c = centre(img);
        if (f < 300) QVERIFY(c.red() > 150 && c.blue() < 60);
        else QVERIFY(c.blue() > 150 && c.red() < 60);
    }

    // 4. TRIM by dragging A's right edge from 10 s to 8 s (mouse on the timeline; the edge snaps to
    //    the playhead parked at 8 s, as a user would do)
    w.playback()->seek(timeToFrame(Rational(8), rate));
    QRect ra = tl->clipRect(a);
    QVERIFY(!ra.isEmpty());
    const int y = ra.center().y();
    QTest::mousePress(tl, Qt::LeftButton, {}, QPoint(ra.right() - 2, y));
    QTest::mouseMove(tl, QPoint((ra.right() + tl->xForTime(8.0)) / 2, y));
    QTest::mouseMove(tl, QPoint(tl->xForTime(8.0), y));
    QTest::mouseRelease(tl, Qt::LeftButton, {}, QPoint(tl->xForTime(8.0), y));
    s = &doc->sequence();
    QCOMPARE(edit::findClip(*s, a)->timelineEnd(), Rational(8));
    const auto aGroup = edit::linkGroup(*s, a);
    for (const auto& m : aGroup) QCOMPARE(edit::findClip(*s, m.clipId)->timelineEnd(), Rational(8));   // audio follows

    // 5. MOVE B by dragging its body left until it snaps to A's end (8 s)
    QRect rb = tl->clipRect(b);
    const QPoint grab(rb.left() + rb.width() / 3, rb.center().y());
    QTest::mousePress(tl, Qt::LeftButton, {}, grab);
    const int dx = tl->xForTime(8.0) - tl->xForTime(10.0);
    QTest::mouseMove(tl, grab + QPoint(dx / 2, 0));
    QTest::mouseMove(tl, grab + QPoint(dx + 3, 0));   // within the snap distance of 8 s
    QTest::mouseRelease(tl, Qt::LeftButton, {}, grab + QPoint(dx + 3, 0));
    s = &doc->sequence();
    QCOMPARE(edit::findClip(*s, b)->timelineStart, Rational(8));
    for (const auto& m : edit::linkGroup(*s, b)) QCOMPARE(edit::findClip(*s, m.clipId)->timelineStart, Rational(8));

    // 6. SPLIT A at 4 s with Ctrl+B (A selected by clicking it)
    w.playback()->seek(timeToFrame(Rational(4), rate));
    ra = tl->clipRect(a);
    QTest::mouseClick(tl, Qt::LeftButton, {}, QPoint(ra.left() + 20, ra.center().y()));
    QCOMPARE(doc->primarySelection(), a);
    QTest::keyClick(&w, Qt::Key_B, Qt::ControlModifier);
    s = &doc->sequence();
    QCOMPARE(s->tracks[0].clips.size(), size_t(3));
    QCOMPARE(s->tracks[1].clips.size(), size_t(3));   // linked audio split too
    shot(&w, "m1_02_split.png");

    // 7. UNDO / REDO with the keyboard
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(doc->sequence().tracks[0].clips.size(), size_t(2));
    QTest::keyClick(&w, Qt::Key_Y, Qt::ControlModifier);
    QCOMPARE(doc->sequence().tracks[0].clips.size(), size_t(3));

    // 8. RIPPLE DELETE the middle part (A 4..8 s): B and its audio close the gap -> start at 4 s
    s = &doc->sequence();
    const std::string middle = s->tracks[0].clips[1].id;
    const QRect rm = tl->clipRect(middle);
    QTest::mouseClick(tl, Qt::LeftButton, {}, rm.center());
    QCOMPARE(doc->primarySelection(), middle);
    QTest::keyClick(&w, Qt::Key_Delete, Qt::ShiftModifier);
    s = &doc->sequence();
    QCOMPARE(s->tracks[0].clips.size(), size_t(2));
    QCOMPARE(edit::findClip(*s, b)->timelineStart, Rational(4));
    for (const auto& m : edit::linkGroup(*s, b)) QCOMPARE(edit::findClip(*s, m.clipId)->timelineStart, Rational(4));

    // 9. TRIM END TO PLAYHEAD (Alt+]) on B at 6 s -> timeline is 6 s = 180 frames
    w.playback()->seek(timeToFrame(Rational(6), rate));
    const QRect rb2 = tl->clipRect(b);
    QTest::mouseClick(tl, Qt::LeftButton, {}, QPoint(rb2.left() + 10, rb2.center().y()));
    QTest::keyClick(&w, Qt::Key_BracketRight, Qt::AltModifier);
    s = &doc->sequence();
    QCOMPARE(s->frameCount(), int64_t(180));
    tl->zoomToFit();
    w.playback()->seek(150);
    QVERIFY(!waitForPreview(w, 150).isNull());
    shot(&w, "m1_03_edited.png");

    // 10. SAVE and REOPEN in a fresh window
    const QString projectFile = tmp.filePath("workflow.lcproj");
    QVERIFY(w.saveProjectTo(projectFile));
    QVERIFY(!doc->isModified());
    const std::string saved = projectToJsonString(doc->project());
    MainWindow w2(MainWindow::Options{false, true});
    w2.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w2));
    QVERIFY(w2.openProjectFile(projectFile));
    QCOMPARE(projectToJsonString(w2.document()->project()), saved);
    w2.playback()->seek(119);
    QCOMPARE(stripIndex(waitForPreview(w2, 119)), 119);   // last frame of A
    w2.playback()->seek(120);
    QCOMPARE(stripIndex(waitForPreview(w2, 120)), 0);     // first frame of B

    // 11. EXPORT from the reopened project and verify video + audio
    const QString out = tmp.filePath("workflow.mp4");
    QString err;
    QVERIFY2(w2.exportTo(out, &err), qPrintable(err));
    const MediaInfo mi = probeMedia(p8(out));
    QVERIFY(mi.video && mi.audio);
    QCOMPARE(mi.video->codec, std::string("h264"));
    QCOMPARE(mi.video->frameRate, FrameRate::fps30());
    QCOMPARE(*mi.video->frameCount, int64_t(180));
    QCOMPARE(mi.audio->codec, std::string("aac"));
    QCOMPARE(mi.audio->channels, 2);
    {
        VideoDecoder vd(p8(out));
        for (int n = 0; n < 180; ++n) {
            const VideoFrame& f = vd.frameAt(frameToTime(n, FrameRate::fps30()));
            QCOMPARE(test::readStrip(f.image).index, n < 120 ? n : n - 120);
            const Rgba8 c = f.image.at(960, 540);
            if (n < 120) QVERIFY(c.r > 150);
            else QVERIFY(c.b > 150);
        }
        AudioDecoder ad(p8(out));
        std::vector<float> pcm(288000 * 2);
        ad.read(0, 288000, pcm.data());
        QVERIFY(toneAmplitude(pcm, 192000 - 2400, 2400, 1000) > 0.1);    // A's 1 kHz before the 4 s cut
        QVERIFY(toneAmplitude(pcm, 192000 - 2400, 2400, 1500) < 0.01);
        QVERIFY(toneAmplitude(pcm, 192000 + 480, 2400, 1500) > 0.1);     // B's 1.5 kHz after it
        QVERIFY(toneAmplitude(pcm, 192000 + 480, 2400, 1000) < 0.01);
    }
    shot(&w2, "m1_04_reopened.png");
}

void GuiTests::playbackFollowsTheClockAndStopsExactly() {
    MainWindow w(MainWindow::Options{false, true});
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4")});
    QVERIFY(w.editController()->addToTimeline(ids[0], Rational(0)));
    QVERIFY(w.editController()->trimClip(w.document()->sequence().tracks[0].clips[0].id, ClipEdge::Tail, Rational(2)));
    w.playback()->seek(0);
    QVERIFY(!waitForPreview(w, 0).isNull());
    QElapsedTimer t;
    t.start();
    w.playback()->play();
    QVERIFY(w.playback()->isPlaying());
    QTest::qWait(1000);
    const int64_t f = w.document()->playhead();
    const double expected = t.elapsed() / 1000.0 * 30;
    QVERIFY2(std::abs(f - expected) < 6, qPrintable(QString("playhead %1, expected ~%2").arg(f).arg(expected)));
    // shown frame matches the clock (within a couple of frames of rendering latency)
    const int idx = stripIndex(w.playback()->currentImage());
    QVERIFY2(idx >= 0 && std::abs(idx - f) <= 3, qPrintable(QString("shown %1 vs playhead %2").arg(idx).arg(f)));
    // runs to the end of the 2 s timeline and stops on the last frame
    QTRY_VERIFY_WITH_TIMEOUT(!w.playback()->isPlaying(), 4000);
    QCOMPARE(w.document()->playhead(), int64_t(59));
    QCOMPARE(stripIndex(waitForPreview(w, 59)), 59);
    // the shortcut toggles playback
    QTest::keyClick(&w, Qt::Key_Space);
    QVERIFY(w.playback()->isPlaying());
    QTest::keyClick(&w, Qt::Key_Space);
    QVERIFY(!w.playback()->isPlaying());
}

void GuiTests::missingMediaIsReportedAndRelinkable() {
    QTemporaryDir tmp;
    const QString copy = tmp.filePath("moved.mp4");
    QVERIFY(QFile::copy(mediaPath("A_red_1080p30_10s.mp4"), copy));
    const QString proj = tmp.filePath("p.lcproj");
    std::vector<std::string> ids;
    {
        MainWindow w(MainWindow::Options{false, true});
        w.show();
        ids = w.importFiles({copy});
        QVERIFY(w.editController()->addToTimeline(ids[0], Rational(0)));
        QVERIFY(w.saveProjectTo(proj));
    }   // closing the window releases the media files (Windows locks open files)
    QVERIFY(QFile::remove(copy));
    MainWindow w2(MainWindow::Options{false, true});
    w2.show();
    QVERIFY(w2.openProjectFile(proj));
    QCOMPARE(w2.document()->missingSources().size(), size_t(1));
    QVERIFY(w2.lastMessage().contains("missing"));
    QString err;
    QVERIFY(!w2.exportTo(tmp.filePath("x.mp4"), &err));   // refused with a clear reason
    QVERIFY2(err.contains("not found"), qPrintable(err));
    QVERIFY(!QFile::exists(tmp.filePath("x.mp4")));
    // relink (same command the menu uses) and export again
    const std::string newPath = QDir::cleanPath(mediaPath("A_red_1080p30_10s.mp4")).toStdString();
    QVERIFY(w2.document()->execute(makeModifySource(ids[0], "Relink", [newPath](MediaSource& m) { m.path = newPath; })));
    QVERIFY(w2.document()->missingSources().empty());
    w2.playback()->seek(42);
    QCOMPARE(stripIndex(waitForPreview(w2, 42)), 42);
}

void GuiTests::exportCanBeCancelled() {
    MainWindow w(MainWindow::Options{false, true});
    w.show();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4")});
    QVERIFY(w.editController()->addToTimeline(ids[0], Rational(0)));
    QTemporaryDir tmp;
    const QString out = tmp.filePath("cancel.mp4");
    ExportDialog d(w.document(), &w);
    QTimer::singleShot(400, &d, [&d] { d.findChild<QPushButton*>("accentButton")->click(); });   // "Cancel" while running
    QString err;
    QVERIFY(!d.exportTo(out, &err));
    QVERIFY2(err.contains("cancel", Qt::CaseInsensitive), qPrintable(err));
    QVERIFY(!QFile::exists(out));
    QVERIFY(!QFile::exists(out + ".partial.mp4"));
}

// ---- M2: transforms, keyframes, masks, blend modes, transitions ---------------------------------

namespace {

QColor at(const QImage& img, double fx, double fy) {
    return img.pixelColor(std::clamp(int(fx * img.width()), 0, img.width() - 1),
                          std::clamp(int(fy * img.height()), 0, img.height() - 1));
}
bool isRed(const QColor& c) { return c.red() > 150 && c.blue() < 70; }
bool isBlue(const QColor& c) { return c.blue() > 150 && c.red() < 70; }
QString rgb(const QColor& c) { return QString("(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue()); }

// Mouse drag with explicit button state on every event (as Windows delivers it).
void drag(QWidget* w, QPointF from, QPointF to, Qt::KeyboardModifiers mods = Qt::NoModifier, int steps = 8) {
    auto send = [&](QEvent::Type t, QPointF p, Qt::MouseButton b, Qt::MouseButtons bs) {
        QMouseEvent ev(t, p, w->mapToGlobal(p), b, bs, mods);
        QApplication::sendEvent(w, &ev);
    };
    send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= steps; ++i) {
        send(QEvent::MouseMove, from + (to - from) * (double(i) / steps), Qt::NoButton, Qt::LeftButton);
        QApplication::processEvents();
    }
    send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();
}

void typeInto(QAbstractSpinBox* s, const QString& text) {
    s->setFocus();
    s->selectAll();
    QTest::keyClicks(s, text);
    QTest::keyClick(s, Qt::Key_Return);
    QApplication::processEvents();
}

const Clip& clipOf(Document* doc, const std::string& id) { return *edit::findClip(doc->sequence(), id); }

// Mean absolute difference per channel (0..255) between two images of the same size.
double meanAbsDiff(const QImage& a, const QImage& b) {
    const QImage x = a.convertToFormat(QImage::Format_RGBA8888), y = b.convertToFormat(QImage::Format_RGBA8888);
    double sum = 0;
    for (int r = 0; r < x.height(); ++r) {
        const uchar* p = x.constScanLine(r);
        const uchar* q = y.constScanLine(r);
        for (int i = 0; i < x.width() * 4; ++i)
            if (i % 4 != 3) sum += std::abs(int(p[i]) - int(q[i]));
    }
    return sum / (double(x.width()) * x.height() * 3);
}

}  // namespace

void GuiTests::m2TransformKeyframesMasksBlend() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 960);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    TimelineView* tl = w.timelinePanel()->view();
    PreviewCanvas* canvas = w.previewPanel()->canvas();

    // A (red) on Video 1, B (blue) above it on a new Video 2 track
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4"), mediaPath("B_blue_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    QVERIFY(ed->addTrack(TrackKind::Video));
    std::string v2;
    for (const auto& t : doc->sequence().tracks)
        if (t.name == "Video 2") v2 = t.id;
    QVERIFY(!v2.empty());
    QVERIFY2(ed->addToTimeline(ids[1], Rational(0), v2), qPrintable(w.lastMessage()));
    const std::string b = doc->sequence().findTrack(v2)->clips.at(0).id;
    tl->zoomToFit();
    w.playback()->seek(30);
    QTest::mouseClick(tl, Qt::LeftButton, {}, tl->clipRect(b).center() - QPoint(0, 12));
    QCOMPARE(doc->primarySelection(), b);
    QCOMPARE(w.inspector()->videoSubTab(), QString("basic"));

    // ---- TRANSFORM through the inspector: Scale 50 % (aspect lock on -> both axes)
    auto* scaleSpin = w.inspector()->findChild<QDoubleSpinBox*>("prop:transform.scaleX");
    QVERIFY(scaleSpin);
    typeInto(scaleSpin, "50");
    QCOMPARE(clipOf(doc, b).transform.scaleX.constantValue(), 0.5);
    QCOMPARE(clipOf(doc, b).transform.scaleY.constantValue(), 0.5);
    QImage img = waitForPreview(w, 30);
    QVERIFY2(isBlue(at(img, 0.5, 0.5)) && isRed(at(img, 0.1, 0.5)),
             qPrintable(rgb(at(img, 0.5, 0.5)) + rgb(at(img, 0.1, 0.5))));
    QVERIFY(canvas->handlesVisible());
    shot(&w, "m2_01_transform.png");

    // ---- TRANSFORM in the player: drag the picture right by a quarter of the frame
    QRectF fr = canvas->frameRect();
    const QPointF c0 = *canvas->handlePos("center");
    drag(canvas, c0, c0 + QPointF(fr.width() * 0.25, 0));
    QVERIFY2(std::abs(clipOf(doc, b).transform.positionX.constantValue() - 0.25) < 0.01,
             qPrintable(QString::number(clipOf(doc, b).transform.positionX.constantValue())));
    img = waitForPreview(w, 30);
    QVERIFY2(isBlue(at(img, 0.75, 0.5)) && isRed(at(img, 0.4, 0.5)), qPrintable(rgb(at(img, 0.75, 0.5))));
    QCOMPARE(doc->undoStack().undoLabel(), std::string("Move"));
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);   // one undo step for the whole drag
    QCOMPARE(clipOf(doc, b).transform.positionX.constantValue(), 0.0);

    // rotate with the round handle, Shift = 15 degree steps: a quarter turn clockwise
    const QPointF piv = *canvas->handlePos("center"), rh = *canvas->handlePos("rotate");
    const QPointF v = rh - piv;
    drag(canvas, rh, piv + QPointF(-v.y(), v.x()), Qt::ShiftModifier, 12);
    QCOMPARE(clipOf(doc, b).transform.rotation.constantValue(), 90.0);
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);

    // corner handle: 1.5 x the distance from the centre -> 75 %
    const QPointF br = *canvas->handlePos("br");
    drag(canvas, br, piv + (br - piv) * 1.5);
    QVERIFY2(std::abs(clipOf(doc, b).transform.scaleX.constantValue() - 0.75) < 0.01,
             qPrintable(QString::number(clipOf(doc, b).transform.scaleX.constantValue())));
    QCOMPARE(clipOf(doc, b).transform.scaleY.constantValue(), clipOf(doc, b).transform.scaleX.constantValue());
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);

    // Ctrl+drag the anchor: the anchor moves, the picture stays where it is
    const QPointF tlBefore = *canvas->handlePos("tl");
    drag(canvas, piv, piv + QPointF(fr.width() * 0.1, fr.height() * 0.05), Qt::ControlModifier);
    QVERIFY(clipOf(doc, b).transform.anchorX.constantValue() > 0.05);
    QVERIFY(QLineF(*canvas->handlePos("tl"), tlBefore).length() < 1.0);
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);

    // flip and crop through the inspector
    QTest::mouseClick(w.inspector()->findChild<QToolButton*>("flipH"), Qt::LeftButton);
    QVERIFY(clipOf(doc, b).transform.flipH);
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QVERIFY(!clipOf(doc, b).transform.flipH);
    typeInto(w.inspector()->findChild<QDoubleSpinBox*>("prop:crop.left"), "20");
    QCOMPARE(clipOf(doc, b).transform.cropLeft, 0.2);
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);

    // ---- KEYFRAMES: opacity 100 % at 0 s (diamond), 0 % at 2 s (auto-key by editing at the playhead)
    w.playback()->seek(0);
    QTest::mouseClick(w.inspector()->findChild<QToolButton*>("key:opacity"), Qt::LeftButton);
    QVERIFY(clipOf(doc, b).opacity.isAnimated());
    w.playback()->seek(60);
    typeInto(w.inspector()->findChild<QDoubleSpinBox*>("prop:opacity"), "0");
    QCOMPARE(clipOf(doc, b).opacity.keys().size(), size_t(2));
    QCOMPARE(clipOf(doc, b).opacity.value(Rational(1)), 0.5);
    w.playback()->seek(30);
    img = waitForPreview(w, 30);   // B at 50 % over A (B is 50 % scaled: centre = mix)
    QVERIFY2(at(img, 0.5, 0.5).red() > 70 && at(img, 0.5, 0.5).blue() > 70, qPrintable(rgb(at(img, 0.5, 0.5))));
    // the inspector field shows the interpolated value at the playhead
    QCOMPARE(w.inspector()->findChild<QDoubleSpinBox*>("prop:opacity")->value(), 50.0);
    // timeline marker: drag the 2 s keyframe to 3 s
    const QPoint k2 = tl->keyframePos(b, Rational(2));
    QVERIFY(!k2.isNull());
    QTest::mousePress(tl, Qt::LeftButton, {}, k2);
    QTest::mouseMove(tl, QPoint((k2.x() + tl->xForTime(3.0)) / 2, k2.y()));
    QTest::mouseMove(tl, QPoint(tl->xForTime(3.0), k2.y()));
    QTest::mouseRelease(tl, Qt::LeftButton, {}, QPoint(tl->xForTime(3.0), k2.y()));
    QVERIFY(keys::hasKeyAt(clipOf(doc, b).opacity, Rational(3)));
    QVERIFY(!keys::hasKeyAt(clipOf(doc, b).opacity, Rational(2)));
    QCOMPARE(doc->playhead(), int64_t(90));   // the playhead follows the dragged keyframe
    img = waitForPreview(w, 90);              // opacity is 0 at the moved key: only A is visible
    QVERIFY2(isRed(at(img, 0.5, 0.5)), qPrintable(rgb(at(img, 0.5, 0.5))));
    shot(&w, "m2_02_keyframes.png");
    // interpolation style Hold on the first keyframe: constant until the next keyframe
    QVERIFY(ed->setKeyframeStyle(b, Rational(0), keys::Style::Hold));
    QCOMPARE(clipOf(doc, b).opacity.value(Rational(2)), 1.0);
    // animated position through the player creates a keyframe at the playhead too
    w.playback()->seek(45);
    QTest::mouseClick(w.inspector()->findChild<QToolButton*>("key:transform.positionX"), Qt::LeftButton);
    QVERIFY(clipOf(doc, b).transform.positionX.isAnimated() && clipOf(doc, b).transform.positionY.isAnimated());
    w.playback()->seek(75);
    const QPointF cc = *canvas->handlePos("center");
    drag(canvas, cc, cc + QPointF(0, fr.height() * 0.2));
    QCOMPARE(clipOf(doc, b).transform.positionY.keys().size(), size_t(2));
    QVERIFY(std::abs(clipOf(doc, b).transform.positionY.value(Rational(75, 30)) - 0.2) < 0.01);
    QCOMPARE(clipOf(doc, b).transform.positionY.value(Rational(45, 30)), 0.0);
    // delete the opacity keyframes again (property returns to a constant)
    QVERIFY(ed->deleteKeyframes(b, Rational(0)));
    QVERIFY(ed->deleteKeyframes(b, Rational(3)));
    QVERIFY(!clipOf(doc, b).opacity.isAnimated());

    // ---- MASKS: ellipse on B at full size
    QVERIFY(ed->modifyClip(b, "test reset", [](Clip& c) { c.transform = ClipTransform{}; c.opacity.setConstant(1.0); }));
    w.playback()->seek(30);
    w.inspector()->findChild<QTabWidget*>("videoSubTabs")->setCurrentIndex(1);   // Video > Mask
    QTest::mouseClick(w.inspector()->findChild<QToolButton*>("addMaskEllipse"), Qt::LeftButton);
    QCOMPARE(clipOf(doc, b).masks.size(), size_t(1));
    QCOMPARE(doc->activeMask(), clipOf(doc, b).masks[0].id);
    QCOMPARE(w.inspector()->videoSubTab(), QString("mask"));
    img = waitForPreview(w, 30);
    QVERIFY2(isBlue(at(img, 0.5, 0.5)) && isRed(at(img, 0.15, 0.5)) && isRed(at(img, 0.3, 0.85)),
             qPrintable(rgb(at(img, 0.5, 0.5)) + rgb(at(img, 0.15, 0.5)) + rgb(at(img, 0.3, 0.85))));
    // drag the mask in the player
    const QPointF mc = *canvas->handlePos("m:center");
    drag(canvas, mc, mc + QPointF(fr.width() * 0.2, 0));
    const double expectX = 0.2 * 1920.0 / 1080.0;   // layer heights
    QVERIFY2(std::abs(clipOf(doc, b).masks[0].centerX.constantValue() - expectX) < 0.01,
             qPrintable(QString::number(clipOf(doc, b).masks[0].centerX.constantValue())));
    img = waitForPreview(w, 30);
    QVERIFY(isBlue(at(img, 0.7, 0.5)) && isRed(at(img, 0.45, 0.5)));
    // corner handle: resize
    const QPointF mbr = *canvas->handlePos("m:br"), mcen = *canvas->handlePos("m:center");
    drag(canvas, mbr, mcen + (mbr - mcen) * 1.4);
    QVERIFY(std::abs(clipOf(doc, b).masks[0].width.constantValue() - 0.7) < 0.02);
    // invert, feather through the inspector
    auto* invert = w.inspector()->findChild<QCheckBox*>("maskInvert");
    QTest::mouseClick(invert, Qt::LeftButton, {}, QPoint(8, invert->height() / 2));   // on the box itself
    QVERIFY(clipOf(doc, b).masks[0].inverted);
    img = waitForPreview(w, 30);
    QVERIFY2(isRed(at(img, 0.7, 0.5)) && isBlue(at(img, 0.15, 0.5)), qPrintable(rgb(at(img, 0.7, 0.5))));
    typeInto(w.inspector()->findChild<QDoubleSpinBox*>("prop:mask.feather"), "20");
    QVERIFY(std::abs(clipOf(doc, b).masks[0].feather.constantValue() - 20.0 / 1080.0) < 1e-6);
    shot(&w, "m2_03_mask.png");
    // custom (polygon) mask: drag one of its points
    QTest::mouseClick(w.inspector()->findChild<QToolButton*>("addMaskCustom"), Qt::LeftButton);
    QCOMPARE(clipOf(doc, b).masks.size(), size_t(2));
    const Vec2 p0 = clipOf(doc, b).masks[1].points[0];
    const QPointF hp = *canvas->handlePos("m:p0");
    drag(canvas, hp, hp + QPointF(0, -fr.height() * 0.1));
    QVERIFY(clipOf(doc, b).masks[1].points[0].y < p0.y - 0.1);
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(clipOf(doc, b).masks[1].points[0].y, p0.y);

    // ---- BLEND MODES (masks removed): Screen and Multiply of blue over red
    QVERIFY(ed->modifyClip(b, "test", [](Clip& c) { c.masks.clear(); }));
    QTest::mouseClick(tl, Qt::LeftButton, {}, tl->clipRect(b).center() - QPoint(0, 12));
    auto* blend = w.inspector()->findChild<QComboBox*>("blendMode");
    QVERIFY(blend);
    blend->setCurrentIndex(blend->findText("Screen"));
    QCOMPARE(clipOf(doc, b).blend, BlendMode::Screen);
    img = waitForPreview(w, 30);
    QVERIFY2(at(img, 0.5, 0.5).red() > 150 && at(img, 0.5, 0.5).blue() > 150, qPrintable(rgb(at(img, 0.5, 0.5))));
    shot(&w, "m2_04_blend.png");
    blend->setCurrentIndex(blend->findText("Multiply"));
    QCOMPARE(clipOf(doc, b).blend, BlendMode::Multiply);
    img = waitForPreview(w, 30);
    QVERIFY2(at(img, 0.5, 0.5).red() < 60 && at(img, 0.5, 0.5).blue() < 60, qPrintable(rgb(at(img, 0.5, 0.5))));

    // ---- SAVE / REOPEN / EXPORT: a composition using every M2 feature
    QVERIFY(ed->modifyClip(b, "test composition", [](Clip& c) {
        c.blend = BlendMode::Screen;
        c.transform.scaleX.setConstant(0.6);
        c.transform.scaleY.setConstant(0.6);
        c.transform.rotation.setConstant(12);
        c.transform.cropRight = 0.1;
        c.transform.positionX.clearKeys();
        c.transform.positionY.clearKeys();
        c.transform.positionX.setKey({Rational(0), -0.2});
        c.transform.positionX.setKey({Rational(3), 0.2});
        Mask m;
        m.id = "mask_test";
        m.shape = MaskShape::Rectangle;
        m.width.setConstant(0.6);
        m.height.setConstant(0.5);
        m.cornerRadius = 0.2;
        m.feather.setConstant(0.01);
        c.masks.push_back(m);
    }));
    const QString projectFile = tmp.filePath("m2.lcproj");
    QVERIFY(w.saveProjectTo(projectFile));
    const std::string saved = projectToJsonString(doc->project());
    MainWindow w2(MainWindow::Options{false, true});
    w2.resize(1600, 960);
    w2.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w2));
    QVERIFY(w2.openProjectFile(projectFile));
    QCOMPARE(projectToJsonString(w2.document()->project()), saved);
    const Clip& rb = clipOf(w2.document(), b);
    QCOMPARE(rb.blend, BlendMode::Screen);
    QCOMPARE(rb.transform.positionX.keys().size(), size_t(2));
    QCOMPARE(rb.masks.size(), size_t(1));
    QCOMPARE(rb.transform.cropRight, 0.1);

    // preview (rendered at monitor size) vs export (1080p H.264, scaled down): same picture
    const QString out = tmp.filePath("m2.mp4");
    QString err;
    QVERIFY2(w2.exportTo(out, &err), qPrintable(err));
    VideoDecoder vd(p8(out));
    for (int64_t f : {0, 45, 89}) {
        w2.playback()->seek(f);
        const QImage pv = waitForPreview(w2, f);
        QVERIFY(!pv.isNull());
        const VideoFrame& ef = vd.frameAt(frameToTime(f, FrameRate::fps30()));
        const QImage eq(ef.image.pixels.data(), ef.image.width, ef.image.height, ef.image.stride(), QImage::Format_RGBA8888);
        const QImage scaled = eq.scaled(pv.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const double d = meanAbsDiff(pv, scaled);
        qInfo("frame %lld: preview %dx%d vs export: mean abs difference %.2f / 255", static_cast<long long>(f), pv.width(),
              pv.height(), d);
        QVERIFY2(d < 4.0, qPrintable(QString("preview/export mismatch %1").arg(d)));
    }
    w2.playback()->seek(45);
    QTest::mouseClick(w2.timelinePanel()->view(), Qt::LeftButton, {},
                      w2.timelinePanel()->view()->clipRect(b).center() - QPoint(0, 12));
    QApplication::processEvents();
    shot(&w2, "m2_06_reopened.png");
}

void GuiTests::m2TransitionsBrowserAndSettings() {
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 960);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    TimelineView* tl = w.timelinePanel()->view();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4"), mediaPath("B_blue_1080p30_10s.mp4"),
                                    mediaPath("C_testsrc2_1080p30_10s.mp4")});
    QVERIFY(w.editController()->addToTimeline(ids[0], Rational(0)));
    QVERIFY(w.editController()->addToTimeline(ids[1], Rational(10)));
    QVERIFY(w.editController()->addToTimeline(ids[2], Rational(20)));   // textured clip C, then B again
    QVERIFY(w.editController()->addToTimeline(ids[1], Rational(30)));
    tl->zoomToFit();
    const std::string a = doc->sequence().tracks[0].clips[0].id;
    const std::string c = doc->sequence().tracks[0].clips[2].id;

    // the browser lists exactly the transitions the renderer implements
    w.resourceTabs()->setCurrentWidget(w.transitionsPanel());
    QListWidget* list = w.transitionsPanel()->list();
    QCOMPARE(list->count(), int(transitionTypes().size()));
    QStringList names;
    for (int i = 0; i < list->count(); ++i) names << list->item(i)->text();
    QCOMPARE(names, QStringList({"Dissolve", "Fade", "Wipe", "Slide", "Push", "Zoom", "Dip to White", "Flash", "Circle Open",
                                 "Circle Close", "Clock Wipe", "Barn Door", "Diamond", "Blinds", "Diagonal Wipe", "Spin",
                                 "Zoom In", "Blur", "Pixelate"}));

    // select A, double-click Wipe
    QTest::mouseClick(tl, Qt::LeftButton, {}, tl->clipRect(a).center());
    QCOMPARE(doc->primarySelection(), a);
    int wipeRow = names.indexOf("Wipe");
    QTRY_VERIFY(list->itemAt(list->visualItemRect(list->item(wipeRow)).center()) == list->item(wipeRow));
    doubleClick(list->viewport(), list->visualItemRect(list->item(wipeRow)).center());
    QVERIFY2(clipOf(doc, a).outTransition.has_value(), qPrintable(w.lastMessage()));
    QCOMPARE(clipOf(doc, a).outTransition->type, std::string("fa.transition.wipe"));
    QCOMPARE(clipOf(doc, a).outTransition->duration, Rational(1));

    // click the transition on the timeline -> its settings in the inspector
    const QRect trr = tl->transitionRect(a);
    QVERIFY(!trr.isEmpty());
    QTest::mouseClick(tl, Qt::LeftButton, {}, trr.center());
    QCOMPARE(doc->selectedTransition(), a);
    QVERIFY(doc->selection().empty());
    auto* type = w.inspector()->findChild<QComboBox*>("transitionType");
    auto* dir = w.inspector()->findChild<QComboBox*>("transitionDirection");
    auto* dur = w.inspector()->findChild<QDoubleSpinBox*>("transitionDuration");
    QVERIFY(type && dir && dur && type->isVisible());
    QCOMPARE(type->currentText(), QString("Wipe"));
    QVERIFY(dir->isEnabled());
    dir->setCurrentIndex(dir->findText("Right"));
    QCOMPARE(clipOf(doc, a).outTransition->direction, TransitionDirection::Right);
    typeInto(dur, "2");
    QCOMPARE(clipOf(doc, a).outTransition->duration, Rational(2));
    QCOMPARE(clipOf(doc, a).outTransition->type, std::string("fa.transition.wipe"));   // settings keep the type
    // halfway: B revealed from the left (the reveal travels right)
    w.playback()->seek(300);
    QImage img = waitForPreview(w, 300);
    QVERIFY2(isBlue(at(img, 0.2, 0.5)) && isRed(at(img, 0.8, 0.5)), qPrintable(rgb(at(img, 0.2, 0.5)) + rgb(at(img, 0.8, 0.5))));
    shot(&w, "m2_05_transitions.png");

    // drag Push from the browser onto the cut: replaces the type, keeps the timing
    QMimeData mime;
    mime.setData(kMimeTransition, "fa.transition.push");
    const QRect ra = tl->clipRect(a);
    const QPoint cut(ra.right(), ra.center().y());
    QDragEnterEvent enter(cut, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &enter);
    QDragMoveEvent move(cut, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &move);
    QDropEvent drop(QPointF(cut), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &drop);
    QCOMPARE(clipOf(doc, a).outTransition->type, std::string("fa.transition.push"));
    QCOMPARE(clipOf(doc, a).outTransition->duration, Rational(2));
    QCOMPARE(clipOf(doc, a).outTransition->direction, TransitionDirection::Right);
    img = waitForPreview(w, 300);
    QVERIFY2(isBlue(at(img, 0.25, 0.5)) && isRed(at(img, 0.75, 0.5)), qPrintable(rgb(at(img, 0.25, 0.5))));
    // every type renders differently (no silent fallback to a dissolve): textured C -> B at 30 s
    QVERIFY(w.editController()->addTransition(c, Rational(2)));
    std::map<std::string, QImage> looks;
    for (const auto& info : transitionTypes()) {
        TransitionRef r = *clipOf(doc, c).outTransition;
        r.type = info.id;
        QVERIFY(w.editController()->updateTransitionSettings(c, r));
        w.playback()->seek(890);
        looks[info.id] = waitForPreview(w, 890);
        QVERIFY(!looks[info.id].isNull());
    }
    for (const auto& [x, ix] : looks)
        for (const auto& [y, iy] : looks)
            if (x < y) QVERIFY2(meanAbsDiff(ix, iy) > 2.0, qPrintable(QString::fromStdString(x + " vs " + y)));
    // Delete removes the selected transition; undo restores it
    QTest::mouseClick(tl, Qt::LeftButton, {}, tl->transitionRect(a).center());
    QCOMPARE(doc->selectedTransition(), a);
    QTest::keyClick(&w, Qt::Key_Delete);
    QVERIFY(!clipOf(doc, a).outTransition.has_value());
    QCOMPARE(doc->sequence().tracks[0].clips.size(), size_t(4));   // the clips stay
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QVERIFY(clipOf(doc, a).outTransition.has_value());
}

void GuiTests::m2LayoutAndFileDrops() {
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 960);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    QApplication::processEvents();
    shot(&w, "m2_00_empty_project.png");

    // only working pages in the resource bar (no placeholders for later milestones)
    QStringList pages;
    for (auto* b : w.resourceTabs()->findChildren<QToolButton*>("resourceTab")) pages << b->text();
    QCOMPARE(pages, QStringList({"Media", "Audio", "Titles", "Transitions", "Effects", "Filters", "Stickers", "Templates"}));   // M9: Filmora's order
    // Filmora 16 default proportions: resources 46 %, player 29 %, inspector 25 %; top area 60 %
    auto* top = w.findChild<QSplitter*>("topSplitter");
    auto* vert = w.findChild<QSplitter*>("verticalSplitter");
    QVERIFY(top && vert);
    const auto ts = top->sizes();
    const double tw = ts[0] + ts[1] + ts[2];
    QVERIFY2(std::abs(ts[0] / tw - 0.46) < 0.03 && std::abs(ts[1] / tw - 0.29) < 0.03,
             qPrintable(QString("%1 %2 %3").arg(ts[0]).arg(ts[1]).arg(ts[2])));
    const auto vs = vert->sizes();
    QVERIFY2(std::abs(double(vs[0]) / (vs[0] + vs[1]) - 0.60) < 0.04, qPrintable(QString("%1 %2").arg(vs[0]).arg(vs[1])));
    // Project Info shows the sequence settings when nothing is selected
    QVERIFY(w.inspector()->findChildren<QLabel*>().size() > 6);

    // files dragged in from Explorer onto the media library are imported
    QListWidget* list = w.mediaPanel()->list();
    QMimeData files;
    files.setUrls({QUrl::fromLocalFile(mediaPath("A_red_1080p30_10s.mp4")), QUrl::fromLocalFile(mediaPath("H_tone1k_-12dB_10s.wav"))});
    const QPoint lp = list->viewport()->rect().center();
    QDragEnterEvent enter(lp, Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list->viewport(), &enter);
    QVERIFY(enter.isAccepted());
    QDropEvent drop(QPointF(lp), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list->viewport(), &drop);
    QCOMPARE(doc->project().sources.size(), size_t(2));
    // the sidebar filters by kind
    QListWidget* side = w.mediaPanel()->sidebar();
    QCOMPARE(side->item(1)->text(), QString("Video (1)"));
    QCOMPARE(side->item(2)->text(), QString("Audio (1)"));
    side->setCurrentRow(2);
    QCOMPARE(list->count(), 2);   // import tile + the WAV
    QVERIFY(list->item(1)->text().endsWith(".wav"));
    side->setCurrentRow(0);
    QCOMPARE(list->count(), 3);

    // a file dropped on the timeline is imported and placed at the drop position
    TimelineView* tl = w.timelinePanel()->view();
    tl->setPixelsPerSecond(40);
    tl->setScrollSeconds(0);
    QMimeData one;
    one.setUrls({QUrl::fromLocalFile(mediaPath("B_blue_1080p30_10s.mp4"))});
    const QPoint tp(tl->xForTime(5.0), tl->trackLaneRect(doc->sequence().tracks[0].id).center().y());
    QDragEnterEvent e2(tp, Qt::CopyAction, &one, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &e2);
    QDragMoveEvent m2(tp, Qt::CopyAction, &one, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &m2);
    QDropEvent d2(QPointF(tp), Qt::CopyAction, &one, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &d2);
    QCOMPARE(doc->project().sources.size(), size_t(3));
    QCOMPARE(doc->sequence().tracks[0].clips.size(), size_t(1));
    QCOMPARE(doc->sequence().tracks[0].clips[0].timelineStart, Rational(5));

    // full-screen player: the canvas moves to a full-screen window and back (Esc)
    auto* fsBtn = w.previewPanel()->findChild<QToolButton*>("fullscreenButton");
    QVERIFY(fsBtn);
    QTest::mouseClick(fsBtn, Qt::LeftButton);
    QVERIFY(w.previewPanel()->isFullscreen());
    QWidget* fsWin = w.previewPanel()->canvas()->window();
    QVERIFY(fsWin != &w);
    QTRY_VERIFY(fsWin->isFullScreen());
    w.playback()->seek(200);
    QVERIFY(!waitForPreview(w, 200).isNull());
    const QImage big = w.playback()->currentImage();
    QVERIFY2(big.width() >= 1280, qPrintable(QString::number(big.width())));   // renders at screen size
    QTest::keyClick(fsWin, Qt::Key_Escape);
    QTRY_VERIFY(!w.previewPanel()->isFullscreen());
    QCOMPARE(w.previewPanel()->canvas()->window(), &w);
}

namespace {

// Pixels matching a predicate inside a rectangle given in frame fractions.
int countPixels(const QImage& img, QRectF frac, const std::function<bool(const QColor&)>& pred) {
    const QImage x = img.convertToFormat(QImage::Format_RGBA8888);
    const int x0 = int(frac.left() * x.width()), x1 = int(frac.right() * x.width());
    const int y0 = int(frac.top() * x.height()), y1 = int(frac.bottom() * x.height());
    int n = 0;
    for (int y = std::max(0, y0); y < std::min(x.height(), y1); ++y)
        for (int xx = std::max(0, x0); xx < std::min(x.width(), x1); ++xx)
            if (pred(x.pixelColor(xx, y))) ++n;
    return n;
}

bool isWhite(const QColor& c) { return c.red() > 220 && c.green() > 220 && c.blue() > 220; }
bool isYellow(const QColor& c) { return c.red() > 200 && c.green() > 180 && c.blue() < 90; }

const Track* trackOfClip(Document* doc, const std::string& id) {
    const Track* tr = nullptr;
    edit::findClip(doc->sequence(), id, &tr);
    return tr;
}

}  // namespace

void GuiTests::m3TitlesBrowserInspectorPreview() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString presetFile = QDir(tmp.path()).filePath("title_presets.json");
    qputenv("MEERCUT_TITLE_PRESETS", presetFile.toUtf8());
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 960);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    TimelineView* tl = w.timelinePanel()->view();
    PreviewCanvas* canvas = w.previewPanel()->canvas();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    const std::string video = doc->sequence().tracks[0].clips[0].id;
    tl->setPixelsPerSecond(60);
    tl->setScrollSeconds(0);

    // ---- browser: the resource bar has a Titles page with categories, search and engine thumbnails
    QStringList pages;
    for (auto* b : w.resourceTabs()->findChildren<QToolButton*>("resourceTab")) pages << b->text();
    QCOMPARE(pages, QStringList({"Media", "Audio", "Titles", "Transitions", "Effects", "Filters", "Stickers", "Templates"}));   // M9: Filmora's order
    w.resourceTabs()->setCurrentWidget(w.titlesPanel());
    TitlesPanel* tp = w.titlesPanel();
    QCOMPARE(tp->list()->count(), int(text::builtinTitleTemplates().size()));
    // All + Basic, Lower Thirds, Subtitles, Openers, End Credits, (M11) Social Media, Callouts, Quotes, Promo, Gaming,
    // Elegant, and My Titles last
    QCOMPARE(tp->categories()->count(), 13);
    const int myTitles = tp->categories()->count() - 1;
    QCOMPARE(tp->categories()->item(myTitles)->text(), QString("My Titles (0)"));
    tp->categories()->setCurrentRow(2);
    QStringList lower;
    for (int i = 0; i < tp->list()->count(); ++i) lower << tp->list()->item(i)->text();
    QCOMPARE(lower, QStringList({"Name Bar", "Accent Bar", "Minimal Location", "Boxed Name", "Podcast Guest",
                                 "Social Handle", "Title and Role"}));   // M3's three, then M11's four
    tp->categories()->setCurrentRow(0);
    tp->search()->setText("neon");
    QCOMPARE(tp->list()->count(), 2);
    QCOMPARE(tp->list()->item(0)->text(), QString("Neon Glow"));
    QCOMPARE(tp->list()->item(1)->text(), QString("Neon Sign"));   // M11
    tp->search()->clear();
    // thumbnails are real renders: two different templates give different pictures
    const QImage t0 = tp->list()->item(0)->icon().pixmap(160, 90).toImage();
    const QImage t1 = tp->list()->item(4)->icon().pixmap(160, 90).toImage();
    QVERIFY(!t0.isNull() && t0.size() == t1.size());
    QVERIFY(meanAbsDiff(t0, t1) > 1.0);
    shot(&w, "m3_01_titles_browser.png");

    // ---- drag "Name Bar" onto Video 1 at 2 s: Video 1 holds footage there -> the title goes above it
    QMimeData mime;
    mime.setData(kMimeTitle, "lower.name_bar");
    const QPoint dp(tl->xForTime(2.0), tl->trackLaneRect(doc->sequence().tracks[0].id).center().y());
    QDragEnterEvent enter(dp, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &enter);
    QVERIFY(enter.isAccepted());
    QDragMoveEvent move(dp, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &move);
    QDropEvent drop(QPointF(dp), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &drop);
    const std::string title = doc->primarySelection();
    const Clip* tc = edit::findClip(doc->sequence(), title);
    QVERIFY2(tc && tc->isTitle(), qPrintable(w.lastMessage()));
    QCOMPARE(tc->timelineStart, Rational(2));
    QCOMPARE(tc->duration, Rational(5));
    QCOMPARE(tc->title->templateId, std::string("lower.name_bar"));
    QCOMPARE(trackOfClip(doc, title)->kind, TrackKind::Video);
    QVERIFY(trackOfClip(doc, title) != &doc->sequence().tracks[0]);   // above the footage

    // ---- the text inspector opens for the title
    auto* textEdit = w.inspector()->findChild<QPlainTextEdit*>("titleText");
    QVERIFY(textEdit && textEdit->isVisible());
    QCOMPARE(textEdit->toPlainText(), QString("Alex Morgan\nProduct Designer"));
    auto* sizeSpin = w.inspector()->findChild<QDoubleSpinBox*>("titleSize");
    QCOMPARE(sizeSpin->value(), 26.0);
    QCOMPARE(w.inspector()->findChild<QComboBox*>("animIn")->currentData().toString(), QString("slide_left"));

    // render at 4 s: the dark name bar sits over the red picture on the lower left
    w.playback()->seek(120);
    const QImage before = waitForPreview(w, 120);
    QVERIFY(!before.isNull());
    const QColor bar = at(before, 0.20, 0.83);
    QVERIFY2(!isRed(bar) && bar.red() < 110, qPrintable(rgb(bar)));
    QVERIFY(isRed(at(before, 0.8, 0.2)));   // the title is transparent elsewhere
    QVERIFY(countPixels(before, QRectF(0.05, 0.75, 0.35, 0.17), isWhite) > 200);

    // ---- typing edits the text (one undo step), the clip name follows the text
    textEdit->setFocus();
    textEdit->selectAll();
    QTest::keyClicks(textEdit, "Hello");
    QCOMPARE(clipOf(doc, title).title->text, std::string("Hello"));
    QCOMPARE(clipOf(doc, title).name, std::string("Hello"));
    // typography and colour change the render
    typeInto(sizeSpin, "40");
    QCOMPARE(clipOf(doc, title).title->style.size, 40.0);
    auto* colorBtn = w.inspector()->findChild<ColorButton*>("titleColor");
    QVERIFY(colorBtn);
    colorBtn->pick(0xFFE000);
    QCOMPARE(clipOf(doc, title).title->style.color, uint32_t(0xFFE000));
    auto* outline = w.inspector()->findChild<QGroupBox*>("titleOutline");
    QVERIFY(outline && !outline->isChecked());
    outline->setChecked(true);
    QVERIFY(clipOf(doc, title).title->style.outline);
    const QImage after = waitForPreview(w, 120);
    QVERIFY(meanAbsDiff(before, after) > 0.5);
    QVERIFY2(countPixels(after, QRectF(0.05, 0.7, 0.4, 0.25), isYellow) > 120,
             qPrintable(QString::number(countPixels(after, QRectF(0.05, 0.7, 0.4, 0.25), isYellow))));
    shot(&w, "m3_02_title_inspector.png");

    // ---- save as preset -> Titles > My Titles (persisted as JSON)
    emit w.inspector()->titleInspector()->savePresetRequested("Yellow name");
    QCOMPARE(w.titleLibrary()->userPresets().size(), size_t(1));
    QCOMPARE(tp->categories()->item(myTitles)->text(), QString("My Titles (1)"));
    tp->categories()->setCurrentRow(myTitles);
    QCOMPARE(tp->list()->count(), 1);
    QCOMPARE(tp->list()->item(0)->text(), QString("Yellow name"));
    {
        TitleLibrary reread(presetFile);
        QCOMPARE(reread.userPresets().size(), size_t(1));
        QCOMPARE(reread.userPresets()[0].prototype.title->style.color, uint32_t(0xFFE000));
        QCOMPARE(reread.userPresets()[0].prototype.title->text, std::string("Hello"));
    }
    tp->categories()->setCurrentRow(0);

    // ---- undo steps back through outline, colour, size, text; redo re-applies them
    for (int i = 0; i < 4; ++i) QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(clipOf(doc, title).title->text, std::string("Alex Morgan\nProduct Designer"));
    QCOMPARE(clipOf(doc, title).title->style.size, 26.0);
    QVERIFY(!clipOf(doc, title).title->style.outline);
    QCOMPARE(textEdit->toPlainText(), QString("Alex Morgan\nProduct Designer"));   // the inspector follows
    for (int i = 0; i < 4; ++i) QTest::keyClick(&w, Qt::Key_Y, Qt::ControlModifier);
    QCOMPARE(clipOf(doc, title).title->text, std::string("Hello"));
    QVERIFY(clipOf(doc, title).title->style.outline);

    // ---- direct manipulation in the player: move, then scale with a corner
    QVERIFY(canvas->handlesVisible());
    const QPointF c0 = *canvas->handlePos("center");
    const double px0 = clipOf(doc, title).transform.positionX.value(Rational(0));
    drag(canvas, c0, c0 + QPointF(100, 0));
    const double px1 = clipOf(doc, title).transform.positionX.value(Rational(0));
    QVERIFY2(std::abs((px1 - px0) - 100.0 / canvas->frameRect().width()) < 0.004, qPrintable(QString::number(px1 - px0)));
    const QPointF br = *canvas->handlePos("br");
    const QPointF c1 = *canvas->handlePos("center");
    drag(canvas, br, c1 + (br - c1) * 1.5);
    const double sc = clipOf(doc, title).transform.scaleX.value(Rational(0));
    QVERIFY2(sc > 1.3 && sc < 1.7, qPrintable(QString::number(sc)));
    const QImage moved = waitForPreview(w, 120);
    QVERIFY(meanAbsDiff(after, moved) > 0.3);
    // the box follows the rendered text: its centre lies on yellow-ish text pixels' bounding box centre
    {
        const QImage x = moved.convertToFormat(QImage::Format_RGBA8888);
        int minX = x.width(), maxX = -1;
        for (int y = 0; y < x.height(); ++y)
            for (int xx = 0; xx < x.width(); ++xx)
                if (isYellow(x.pixelColor(xx, y))) {
                    minX = std::min(minX, xx);
                    maxX = std::max(maxX, xx);
                }
        QVERIFY(maxX > minX);
        const QRectF fr = canvas->frameRect();
        const double inkCentre = fr.left() + (minX + maxX) / 2.0 * fr.width() / x.width();
        const double boxCentre = canvas->handlePos("center")->x();
        QVERIFY2(std::abs(inkCentre - boxCentre) < 12, qPrintable(QString("%1 vs %2").arg(inkCentre).arg(boxCentre)));
    }
    // clicking the title in the player selects it (from another selection)
    const QPointF titleCentre = *canvas->handlePos("center");
    doc->setSelection({video});
    QTest::mouseClick(canvas, Qt::LeftButton, {}, titleCentre.toPoint());
    QCOMPARE(doc->primarySelection(), title);

    // ---- double-click "Default Title": added at the playhead, on a free track above the footage
    w.playback()->seek(60);
    QTRY_COMPARE(doc->playhead(), int64_t(60));
    QListWidgetItem* def = nullptr;
    for (int i = 0; i < tp->list()->count(); ++i)
        if (tp->list()->item(i)->text() == "Default Title") def = tp->list()->item(i);
    QVERIFY(def);
    tp->list()->scrollToItem(def);
    doubleClick(tp->list()->viewport(), tp->list()->visualItemRect(def).center());
    const std::string title2 = doc->primarySelection();
    QVERIFY(title2 != title && clipOf(doc, title2).isTitle());
    QCOMPARE(clipOf(doc, title2).timelineStart, Rational(2));
    QVERIFY(trackOfClip(doc, title2) != trackOfClip(doc, title));   // the first title occupies its track
    QCOMPARE(clipOf(doc, title2).title->text, std::string("Text Here"));

    // ---- a caption (Edit > Add Caption at Playhead): new subtitle track; it follows the track style and line
    w.action("addCaption")->trigger();
    const std::string cap = doc->primarySelection();
    QVERIFY(cap != title2 && clipOf(doc, cap).isTitle());
    const Track* subs = trackOfClip(doc, cap);
    QCOMPARE(subs->kind, TrackKind::Subtitle);
    QVERIFY(clipOf(doc, cap).title->followTrackStyle);
    auto* line = w.inspector()->findChild<QDoubleSpinBox*>("captionLine");
    QVERIFY(line && line->isVisible());
    QCOMPARE(line->value(), 92.0);
    doc->setSelection({});   // nothing else in front: only the caption is white around the subtitle line
    QVERIFY(ed->modifyClip(title2, "hide", [](Clip& c) { c.enabled = false; }));
    // reference without captions (the test clip has its frame number burned in near the bottom)
    const std::string subsId = subs->id;
    QVERIFY(ed->setTrackFlag(subsId, "hidden", true));
    const QImage noCaps = waitForPreview(w, 60);
    QVERIFY(ed->setTrackFlag(subsId, "hidden", false));
    const QRectF low(0.3, 0.86, 0.4, 0.07), mid(0.3, 0.54, 0.4, 0.07);
    auto added = [&](const QImage& img, const QRectF& r) {   // pixels the captions changed in the band
        const QImage a = img.convertToFormat(QImage::Format_RGBA8888), b = noCaps.convertToFormat(QImage::Format_RGBA8888);
        int n = 0;
        for (int y = int(r.top() * a.height()); y < int(r.bottom() * a.height()); ++y)
            for (int x = int(r.left() * a.width()); x < int(r.right() * a.width()); ++x) {
                const QColor p = a.pixelColor(x, y), q = b.pixelColor(x, y);
                if (std::max({std::abs(p.red() - q.red()), std::abs(p.green() - q.green()), std::abs(p.blue() - q.blue())}) > 60) ++n;
            }
        return n;
    };
    QImage capImg = waitForPreview(w, 60);
    QVERIFY2(added(capImg, low) > 100, qPrintable(QString::number(added(capImg, low))));
    QVERIFY(std::abs(added(capImg, mid)) < 10);
    doc->setSelection({cap});
    typeInto(line, "60");
    QCOMPARE(trackOfClip(doc, cap)->subtitleY, 0.6);
    capImg = waitForPreview(w, 60);
    QVERIFY2(std::abs(added(capImg, low)) < 10, qPrintable(QString::number(added(capImg, low))));
    QVERIFY2(added(capImg, mid) > 100, qPrintable(QString::number(added(capImg, mid))));
    // a style change of a track-styled caption changes the track style
    typeInto(w.inspector()->findChild<QDoubleSpinBox*>("titleSize"), "30");
    QCOMPARE(trackOfClip(doc, cap)->subtitleStyle.size, 30.0);
    shot(&w, "m3_03_caption.png");

    // ---- save and reopen: titles, styles, animations and captions come back and render the same
    const QString file = QDir(tmp.path()).filePath("m3.lcproj");
    QVERIFY(w.saveProjectTo(file));
    w.playback()->seek(120);
    const QImage saved = waitForPreview(w, 120);
    MainWindow w2(MainWindow::Options{false, true});
    w2.resize(1600, 960);
    w2.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w2));
    QVERIFY(w2.openProjectFile(file));
    QVERIFY(w2.document()->sequence() == doc->sequence());
    w2.playback()->seek(120);
    const QImage reopened = waitForPreview(w2, 120);
    QVERIFY(!reopened.isNull() && reopened.size() == saved.size());
    QVERIFY2(meanAbsDiff(saved, reopened) < 0.2, qPrintable(QString::number(meanAbsDiff(saved, reopened))));
}

namespace {

struct Ink {
    int count = 0;
    double cx = 0, cy = 0;   // centroid, frame fractions
    double x0 = 1, x1 = 0;   // horizontal extent, frame fractions
};

// White text pixels inside a region (frame fractions).
Ink whiteInk(const QImage& img, QRectF frac = QRectF(0.0, 0.25, 1.0, 0.5)) {
    const QImage x = img.convertToFormat(QImage::Format_RGBA8888);
    Ink k;
    for (int y = int(frac.top() * x.height()); y < int(frac.bottom() * x.height()); ++y)
        for (int xx = int(frac.left() * x.width()); xx < int(frac.right() * x.width()); ++xx)
            if (isWhite(x.pixelColor(xx, y))) {
                ++k.count;
                k.cx += xx;
                k.cy += y;
                k.x0 = std::min(k.x0, double(xx) / x.width());
                k.x1 = std::max(k.x1, double(xx + 1) / x.width());
            }
    if (k.count) {
        k.cx /= k.count * double(x.width());
        k.cy /= k.count * double(x.height());
    }
    return k;
}

}  // namespace

void GuiTests::m3AnimationsKeyframesExport() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    qputenv("MEERCUT_TITLE_PRESETS", QDir(tmp.path()).filePath("presets.json").toUtf8());
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 960);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    w.timelinePanel()->view()->setPixelsPerSecond(90);
    w.timelinePanel()->view()->setScrollSeconds(0);
    QVERIFY(ed->addTitleFromTemplate("basic.plain", Rational(0)));   // "Your Title Here", white, centred, 5 s
    const std::string title = doc->primarySelection();
    QCOMPARE(clipOf(doc, title).duration, Rational(5));
    auto ink = [&](int64_t f) {
        w.playback()->seek(f);
        return whiteInk(waitForPreview(w, f));
    };
    const Ink full = ink(30);
    QVERIFY(full.count > 200);

    // ---- In: Type Writer (EXP-003 timing: visible = round(N t / dIn), default 0.8 s)
    TitleInspector* ti = w.inspector()->titleInspector();
    ti->tabs()->setCurrentIndex(1);
    auto* animIn = w.inspector()->findChild<QComboBox*>("animIn");
    auto* animInDur = w.inspector()->findChild<QDoubleSpinBox*>("animInDuration");
    auto* animLoop = w.inspector()->findChild<QComboBox*>("animLoop");
    auto* animOut = w.inspector()->findChild<QComboBox*>("animOut");
    auto* animOutDur = w.inspector()->findChild<QDoubleSpinBox*>("animOutDuration");
    QVERIFY(animIn && animInDur && animLoop && animOut && animOutDur && animIn->isVisible());
    animIn->setCurrentIndex(animIn->findData("typewriter"));
    QCOMPARE(clipOf(doc, title).title->in.preset, std::string("typewriter"));
    QCOMPARE(animInDur->value(), 0.8);
    QVERIFY(animIn->currentText().contains(QString::fromUtf8("\xE2\x97\x8F")));   // marked as measured in Filmora
    const Ink t0 = ink(0), t12 = ink(12), t30 = ink(30);
    qInfo("type writer 0.8 s: white px at frame 0 / 12 / 30 = %d / %d / %d", t0.count, t12.count, t30.count);
    QVERIFY(t0.count < 5);                                             // nothing typed at the first frame
    QVERIFY(t12.count > full.count / 4 && t12.count < full.count * 4 / 5);   // 8 of 15 characters
    QVERIFY(std::abs(t30.count - full.count) < full.count / 20);       // complete after 0.8 s
    QVERIFY(t12.cx < full.cx);                                         // typed from the left, in place
    // a longer In: 2 s -> half typed at 1 s
    typeInto(animInDur, "2");
    QCOMPARE(clipOf(doc, title).title->in.duration, Rational(2));
    const Ink t30b = ink(30);
    QVERIFY(t30b.count > full.count / 4 && t30b.count < full.count * 4 / 5);

    // ---- Out: Fade Out (0.5 s) ends invisible at the clip end
    animOut->setCurrentIndex(animOut->findData("fade"));
    QCOMPARE(clipOf(doc, title).title->out.preset, std::string("fade"));
    const Ink o120 = ink(120), o149 = ink(149);
    QVERIFY(std::abs(o120.count - full.count) < full.count / 20);
    QVERIFY2(o149.count < full.count / 5, qPrintable(QString::number(o149.count)));
    // In + Out longer than the clip: both shortened proportionally (shown in the inspector), undoable
    typeInto(animOutDur, "4");
    QCOMPARE(clipOf(doc, title).title->out.duration, Rational(4));
    bool shortened = false;
    for (QLabel* l : ti->findChildren<QLabel*>())
        if (l->text().contains("Shortened to 1.67 s")) shortened = true;
    QVERIFY(shortened);
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(clipOf(doc, title).title->out.duration, Rational(0));   // back to the preset's default

    // ---- Loop: Bouncing (EXP-003: 1.6 s period, up ~59 px then down ~120 px) between In and Out
    animLoop->setCurrentIndex(animLoop->findData("bounce"));
    QCOMPARE(clipOf(doc, title).title->loop.preset, std::string("bounce"));
    const Ink up = ink(75), down = ink(99);   // 0.5 s and 1.3 s into the loop
    qInfo("bouncing: text centre y %.3f at 2.5 s, %.3f at 3.3 s", up.cy, down.cy);
    QVERIFY2(down.cy - up.cy > 0.08, qPrintable(QString("%1 %2").arg(up.cy).arg(down.cy)));

    // ---- keyframes on the title's transform combine with the animations (M2 keyframe engine)
    w.playback()->seek(60);
    QTRY_COMPARE(doc->playhead(), int64_t(60));
    QVERIFY(ed->toggleKeyframe(title, "transform.positionX"));
    w.playback()->seek(120);
    QTRY_COMPARE(doc->playhead(), int64_t(120));
    QVERIFY(ed->setProperty(title, "transform.positionX", 0.2));
    QCOMPARE(clipOf(doc, title).transform.positionX.keys().size(), size_t(2));
    const Ink k60 = ink(60), k120 = ink(120);
    QVERIFY2(std::abs((k120.cx - k60.cx) - 0.2) < 0.02, qPrintable(QString("%1 -> %2").arg(k60.cx).arg(k120.cx)));
    shot(&w, "m3_04_animation.png");

    // ---- export: the animated title matches the preview frame by frame
    const QString out = tmp.filePath("m3_anim.mp4");
    QString err;
    QVERIFY2(w.exportTo(out, &err), qPrintable(err));
    VideoDecoder vd(p8(out));
    for (int64_t f : {0, 30, 75, 99, 140, 149}) {
        w.playback()->seek(f);
        const QImage pv = waitForPreview(w, f);
        QVERIFY(!pv.isNull());
        const VideoFrame& ef = vd.frameAt(frameToTime(f, FrameRate::fps30()));
        const QImage eq(ef.image.pixels.data(), ef.image.width, ef.image.height, ef.image.stride(), QImage::Format_RGBA8888);
        const QImage scaled = eq.scaled(pv.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const double d = meanAbsDiff(pv, scaled);
        const Ink pi = whiteInk(pv), xi = whiteInk(scaled);
        qInfo("frame %lld: preview vs export mean abs difference %.2f / 255; white px %d vs %d", static_cast<long long>(f), d,
              pi.count, xi.count);
        QVERIFY2(d < 4.0, qPrintable(QString("preview/export mismatch %1").arg(d)));
        // same characters at the same place (pixel counts differ a little: export is downscaled from 1080p)
        QVERIFY((pi.count == 0) == (xi.count == 0));
        if (pi.count > 50)
            QVERIFY2(std::abs(pi.x0 - xi.x0) < 0.008 && std::abs(pi.x1 - xi.x1) < 0.008 && std::abs(pi.cy - xi.cy) < 0.01,
                     qPrintable(QString("extent %1-%2 vs %3-%4").arg(pi.x0).arg(pi.x1).arg(xi.x0).arg(xi.x1)));
    }

    // ---- captions: one caption's animation applied to the whole track (one undo step)
    QVERIFY(ed->addTitleFromTemplate("subtitle.classic", Rational(6)));
    const std::string c1 = doc->primarySelection();
    QVERIFY(ed->addTitleFromTemplate("subtitle.classic", Rational(9)));
    const std::string c2 = doc->primarySelection();
    QVERIFY(trackOfClip(doc, c1) == trackOfClip(doc, c2));
    doc->setSelection({c1});
    ti->tabs()->setCurrentIndex(1);
    animIn->setCurrentIndex(animIn->findData("fade"));
    animOut->setCurrentIndex(animOut->findData("fade"));
    auto* toTrack = w.inspector()->findChild<QPushButton*>("animApplyToTrack");
    QVERIFY(toTrack && toTrack->isVisible());
    QTest::mouseClick(toTrack, Qt::LeftButton);
    QCOMPARE(clipOf(doc, c2).title->in.preset, std::string("fade"));
    QCOMPARE(clipOf(doc, c2).title->out.preset, std::string("fade"));
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QVERIFY(!clipOf(doc, c2).title->in.active());
    QCOMPARE(clipOf(doc, c1).title->in.preset, std::string("fade"));   // the source caption keeps its own
}

namespace {

void writeBytes(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}

QByteArray readBytes(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

std::vector<const Clip*> captionsOn(Document* doc, const std::string& trackId) {
    std::vector<const Clip*> v;
    if (const Track* t = doc->sequence().findTrack(trackId))
        for (const auto& c : t->clips) v.push_back(&c);
    return v;
}

// Pixels whose colour differs by more than 60 in any channel inside a band (frame fractions).
int changedPixels(const QImage& a0, const QImage& b0, QRectF band) {
    const QImage a = a0.convertToFormat(QImage::Format_RGBA8888), b = b0.convertToFormat(QImage::Format_RGBA8888);
    int n = 0;
    for (int y = int(band.top() * a.height()); y < int(band.bottom() * a.height()); ++y)
        for (int x = int(band.left() * a.width()); x < int(band.right() * a.width()); ++x) {
            const QColor p = a.pixelColor(x, y), q = b.pixelColor(x, y);
            if (std::max({std::abs(p.red() - q.red()), std::abs(p.green() - q.green()), std::abs(p.blue() - q.blue())}) > 60) ++n;
        }
    return n;
}

const char* kArabic = "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85";   // "hello world"

}  // namespace

void GuiTests::m3SubtitlesImportEditExportBurnIn() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 960);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    TimelineView* tl = w.timelinePanel()->view();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4")});
    QVERIFY(w.editController()->addToTimeline(ids[0], Rational(0)));

    // ---- SRT import: a partial tag (reported), a top caption ({\an8}), Arabic, an off-frame time (snapped)
    const QString srt = tmp.filePath("captions.srt");
    writeBytes(srt, QByteArray("1\r\n00:00:01,000 --> 00:00:02,500\r\nHello <i>world</i>\r\n\r\n"
                               "2\r\n00:00:03,010 --> 00:00:04,000\r\n{\\an8}Top caption\r\n\r\n"
                               "3\r\n00:00:05,000 --> 00:00:06,000\r\n") + QByteArray(kArabic) + "\r\n\r\n");
    QStringList warnings;
    QVERIFY(w.importSubtitlesFile(srt, 0, Rational(0), &warnings));
    QVERIFY2(warnings.join("|").contains("<i> around part of a caption"), qPrintable(warnings.join("|")));
    std::string subs;
    for (const auto& t : doc->sequence().tracks)
        if (t.kind == TrackKind::Subtitle) subs = t.id;
    QVERIFY(!subs.empty());
    auto caps = captionsOn(doc, subs);
    QCOMPARE(caps.size(), size_t(3));
    QCOMPARE(caps[0]->timelineStart, Rational(1));
    QCOMPARE(caps[0]->duration, Rational(3, 2));
    QCOMPARE(caps[0]->title->text, std::string("Hello world"));
    QCOMPARE(caps[1]->timelineStart, Rational(3));   // 3.010 s -> nearest frame at 30 fps
    QCOMPARE(caps[1]->title->style.anchor, TextAnchor::Top);
    QCOMPARE(caps[2]->title->text, std::string(kArabic));
    const std::string c0 = caps[0]->id, c1 = caps[1]->id, c2 = caps[2]->id;

    // preview: the first caption sits on the subtitle line, the {\an8} caption at the top
    QVERIFY(w.editController()->setTrackFlag(subs, "hidden", true));
    w.playback()->seek(45);
    const QImage bare45 = waitForPreview(w, 45);
    w.playback()->seek(105);
    const QImage bare105 = waitForPreview(w, 105);
    QVERIFY(w.editController()->setTrackFlag(subs, "hidden", false));
    w.playback()->seek(45);
    const QImage cap45 = waitForPreview(w, 45);
    w.playback()->seek(105);
    const QImage cap105 = waitForPreview(w, 105);
    QVERIFY(changedPixels(cap45, bare45, QRectF(0.2, 0.84, 0.6, 0.1)) > 80);
    QVERIFY(changedPixels(cap45, bare45, QRectF(0.0, 0.0, 1.0, 0.5)) == 0);
    QVERIFY(changedPixels(cap105, bare105, QRectF(0.2, 0.06, 0.6, 0.1)) > 80);
    QVERIFY(changedPixels(cap105, bare105, QRectF(0.0, 0.5, 1.0, 0.5)) == 0);

    // ---- edit: text and end time in the inspector, move a caption on the timeline
    doc->setSelection({c0});
    auto* textEdit = w.inspector()->findChild<QPlainTextEdit*>("titleText");
    QVERIFY(textEdit && textEdit->isVisible());
    textEdit->setFocus();
    textEdit->selectAll();
    QTest::keyClicks(textEdit, "Edited line");
    QCOMPARE(clipOf(doc, c0).title->text, std::string("Edited line"));
    typeInto(w.inspector()->findChild<QDoubleSpinBox*>("captionEnd"), "2");
    QCOMPARE(clipOf(doc, c0).timelineEnd(), Rational(2));
    tl->setPixelsPerSecond(100);
    tl->setScrollSeconds(0);
    QApplication::processEvents();
    const QRect r2 = tl->clipRect(c2);
    QVERIFY(!r2.isEmpty());
    drag(tl, r2.center(), r2.center() + QPoint(100, 0));
    QCOMPARE(clipOf(doc, c2).timelineStart, Rational(6));
    QCOMPARE(clipOf(doc, c2).duration, Rational(1));
    // the caption list: every caption of the track; a click selects it and moves the playhead there
    doc->setSelection({c0});
    auto* list = w.inspector()->findChild<QListWidget*>("captionList");
    QVERIFY(list && list->isVisible());
    QCOMPARE(list->count(), 3);
    QVERIFY(list->item(0)->text().contains("Edited line"));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(2)).center());
    QCOMPARE(doc->primarySelection(), c2);
    QTRY_COMPARE(doc->playhead(), int64_t(180));
    QVERIFY(!waitForPreview(w, 180).isNull());
    shot(&w, "m3_05_subtitles.png");

    // ---- export SRT and ASS: the edits are in the files and read back
    const QString outSrt = tmp.filePath("out.srt"), outAss = tmp.filePath("out.ass");
    QString err;
    QVERIFY2(w.exportSubtitlesTo(outSrt, subs, &err), qPrintable(err));
    QVERIFY2(w.exportSubtitlesTo(outAss, subs, &err), qPrintable(err));
    const QByteArray srtBytes = readBytes(outSrt), assBytes = readBytes(outAss);
    const SubtitleDoc back = parseSrt(std::string_view(srtBytes.constData(), size_t(srtBytes.size())));
    QCOMPARE(back.captions.size(), size_t(3));
    QCOMPARE(back.captions[0].text, std::string("Edited line"));
    QCOMPARE(back.captions[0].end, Rational(2));
    QCOMPARE(back.captions[1].style.value_or(back.style).anchor, TextAnchor::Top);   // {\an8} written back
    QCOMPARE(back.captions[2].start, Rational(6));
    QCOMPARE(back.captions[2].text, std::string(kArabic));
    QVERIFY(srtBytes.contains("00:00:06,000 --> 00:00:07,000"));
    const SubtitleDoc backAss = parseAss(std::string_view(assBytes.constData(), size_t(assBytes.size())));
    QCOMPARE(backAss.captions.size(), size_t(3));
    QCOMPARE(backAss.captions[2].text, std::string(kArabic));
    QVERIFY2(backAss.warnings.empty(), backAss.warnings.empty() ? "" : backAss.warnings.front().c_str());

    // ---- burn-in: the exported MP4 carries the captions (compared with the caption-free preview)
    const QString mp4 = tmp.filePath("burn.mp4");
    QVERIFY2(w.exportTo(mp4, &err), qPrintable(err));
    VideoDecoder vd(p8(mp4));
    auto exported = [&](int64_t f, const QSize& size) {
        const VideoFrame& ef = vd.frameAt(frameToTime(f, FrameRate::fps30()));
        const QImage eq(ef.image.pixels.data(), ef.image.width, ef.image.height, ef.image.stride(), QImage::Format_RGBA8888);
        return eq.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    };
    w.playback()->seek(45);
    const QImage edited45 = waitForPreview(w, 45);
    const QImage ex45 = exported(45, edited45.size()), ex105 = exported(105, cap105.size());
    QVERIFY2(meanAbsDiff(ex45, edited45) < 4.0, qPrintable(QString::number(meanAbsDiff(ex45, edited45))));
    QVERIFY(changedPixels(ex45, bare45, QRectF(0.2, 0.84, 0.6, 0.1)) > 80);   // caption burnt in
    QVERIFY(changedPixels(ex105, bare105, QRectF(0.2, 0.06, 0.6, 0.1)) > 80);
    QVERIFY(changedPixels(ex105, cap105, QRectF(0.2, 0.06, 0.6, 0.1)) * 3 <
            changedPixels(ex105, bare105, QRectF(0.2, 0.06, 0.6, 0.1)));   // ... where the preview shows it

    // ---- ASS import: unsupported override tags are reported, the text is kept
    const QString ass = tmp.filePath("styled.ass");
    writeBytes(ass, "[Script Info]\r\nScriptType: v4.00+\r\nPlayResX: 1920\r\nPlayResY: 1080\r\n\r\n"
                    "[V4+ Styles]\r\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
                    "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, "
                    "MarginL, MarginR, MarginV, Encoding\r\n"
                    "Style: Default,Arial,60,&H0000FFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,3,0,2,20,20,40,1\r\n\r\n"
                    "[Events]\r\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\r\n"
                    "Dialogue: 0,0:00:07.00,0:00:08.50,Default,,0,0,0,,{\\move(100,100,500,100)}Moving line\r\n"
                    "Dialogue: 0,0:00:08.50,0:00:09.50,Default,,0,0,0,,{\\k20}Ka{\\k30}ra{\\k25}oke\r\n");
    warnings.clear();
    const size_t tracksBefore = doc->sequence().tracks.size();
    QVERIFY(w.importSubtitlesFile(ass, 0, Rational(0), &warnings));
    const QString wj = warnings.join("|");
    QVERIFY2(wj.contains("\\move is not supported") && wj.contains("\\k is not supported (ignored) (3x)"), qPrintable(wj));
    QCOMPARE(doc->sequence().tracks.size(), tracksBefore + 1);
    const Track& assTrack = doc->sequence().tracks.back();
    QCOMPARE(assTrack.kind, TrackKind::Subtitle);
    QCOMPARE(assTrack.clips.size(), size_t(2));
    QCOMPARE(assTrack.clips[1].title->text, std::string("Karaoke"));
    QCOMPARE(assTrack.subtitleStyle.color, uint32_t(0xFFFF00));   // &H0000FFFF (BGR) = yellow
    // one undo step removes the whole import
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(doc->sequence().tracks.size(), tracksBefore);

    // ---- the import dialog: a Windows-1256 (Arabic) file is unreadable as Western text, right as Arabic
    const std::wstring arabicW = QString::fromUtf8(kArabic).toStdWString();
    const int n = WideCharToMultiByte(1256, 0, arabicW.c_str(), int(arabicW.size()), nullptr, 0, nullptr, nullptr);
    QByteArray cp1256(n, '\0');
    WideCharToMultiByte(1256, 0, arabicW.c_str(), int(arabicW.size()), cp1256.data(), n, nullptr, nullptr);
    const QByteArray legacy = "1\r\n00:00:00,500 --> 00:00:01,500\r\n" + cp1256 + "\r\n";
    SubtitleImportDialog dlg(tmp.filePath("legacy.srt"), legacy, Rational(4));
    auto* enc = dlg.findChild<QComboBox*>("subtitleEncoding");
    enc->setCurrentIndex(enc->findData(1252u));
    QVERIFY(dlg.document().captions.at(0).text != std::string(kArabic));
    enc->setCurrentIndex(enc->findData(1256u));
    QCOMPARE(dlg.document().captions.at(0).text, std::string(kArabic));
    QVERIFY(dlg.findChild<QLabel*>("subtitleSummary")->text().contains("1256"));
    dlg.findChild<QComboBox*>("subtitlePlacement")->setCurrentIndex(1);
    QCOMPARE(dlg.offset(), Rational(4));

    // ---- an SRT dropped on the timeline starts at the drop time
    tl->setPixelsPerSecond(60);
    tl->setScrollSeconds(0);
    QMimeData files;
    files.setUrls({QUrl::fromLocalFile(srt)});
    const QPoint dp(tl->xForTime(10.0), tl->trackLaneRect(doc->sequence().tracks[0].id).center().y());
    QDragEnterEvent de(dp, Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &de);
    QDropEvent dd(QPointF(dp), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &dd);
    const Track& dropped = doc->sequence().tracks.back();
    QCOMPARE(dropped.kind, TrackKind::Subtitle);
    QCOMPARE(dropped.clips.size(), size_t(3));
    QCOMPARE(dropped.clips[0].timelineStart, Rational(11));   // 1 s in the file + dropped at 10 s
    QCOMPARE(doc->project().sources.size(), size_t(1));      // not treated as media
}

namespace {

QAction* menuAction(QToolButton* b, const QString& text) {
    std::function<QAction*(QMenu*)> find = [&](QMenu* m) -> QAction* {
        for (QAction* a : m->actions()) {
            if (a->menu())
                if (QAction* x = find(a->menu())) return x;
            if (a->text() == text) return a;
        }
        return nullptr;
    };
    return b->menu() ? find(b->menu()) : nullptr;
}

// Mean squared horizontal gradient of the green channel (edge energy: a blur lowers it; the mean absolute gradient
// would not, since a blurred step keeps its total variation).
double sharpness(const QImage& img0) {
    const QImage img = img0.convertToFormat(QImage::Format_RGBA8888);
    double s = 0;
    long n = 0;
    for (int y = img.height() / 4; y < img.height() * 3 / 4; ++y)
        for (int x = 1; x < img.width(); ++x) {
            const double d = img.pixelColor(x, y).green() - img.pixelColor(x - 1, y).green();
            s += d * d;
            ++n;
        }
    return s / double(n);
}

double meanGreen(const QImage& img0) {
    const QImage img = img0.convertToFormat(QImage::Format_RGBA8888);
    double s = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) s += img.pixelColor(x, y).green();
    return s / (double(img.width()) * img.height());
}

}  // namespace

void GuiTests::m4EffectStackEditor() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 960);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({mediaPath("C_testsrc2_1080p30_10s.mp4"), mediaPath("A_red_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    QVERIFY(ed->addToTimeline(ids[1], Rational(10)));
    const std::string clip = doc->sequence().tracks[0].clips[0].id, other = doc->sequence().tracks[0].clips[1].id;
    doc->setSelection({clip});
    w.playback()->seek(60);
    const QImage plain = waitForPreview(w, 60);
    QVERIFY(!plain.isNull());

    // ---- the Effects tab lists the registry's effects; add Gaussian Blur and Color Adjust
    QTabWidget* tabs = w.inspector()->clipTabs();
    for (int i = 0; i < tabs->count(); ++i)
        if (tabs->tabText(i) == "Effects") tabs->setCurrentIndex(i);
    auto* add = w.inspector()->findChild<QToolButton*>("addEffect");
    QVERIFY(add && add->isVisible());
    QAction* blurAction = menuAction(add, "Gaussian Blur");
    QAction* colorAction = menuAction(add, "Color Adjust");
    QVERIFY(blurAction && colorAction);
    blurAction->trigger();
    QCOMPARE(clipOf(doc, clip).effects.size(), size_t(1));
    QCOMPARE(clipOf(doc, clip).effects[0].type, std::string("fa.fx.blur.gaussian"));
    QCOMPARE(clipOf(doc, clip).effects[0].id, std::string("fx1"));
    auto* radius = w.inspector()->findChild<QDoubleSpinBox*>("fxspin:fx.fx1.radius");
    QVERIFY(radius);
    QCOMPARE(radius->value(), 20.0);
    typeInto(radius, "60");
    QCOMPARE(clipOf(doc, clip).effects[0].params.at("radius").value(Time(0)), 60.0);
    const QImage blurred = waitForPreview(w, 60);
    qInfo("sharpness plain %.2f, blurred %.2f", sharpness(plain), sharpness(blurred));
    QVERIFY(sharpness(blurred) < sharpness(plain) * 0.5);

    colorAction->trigger();
    QCOMPARE(clipOf(doc, clip).effects.size(), size_t(2));
    auto* bright = w.inspector()->findChild<QDoubleSpinBox*>("fxspin:fx.fx2.brightness");
    QVERIFY(bright);
    typeInto(bright, "60");
    const QImage brighter = waitForPreview(w, 60);
    QVERIFY(meanGreen(brighter) > meanGreen(blurred) + 10);

    // ---- order: Color Adjust before the blur gives another picture; one undo step
    const QImage blurThenColor = brighter;
    QTest::mouseClick(w.inspector()->findChild<QToolButton*>("fxup:fx2"), Qt::LeftButton);
    QCOMPARE(clipOf(doc, clip).effects[0].id, std::string("fx2"));
    const QImage colorThenBlur = waitForPreview(w, 60);
    QVERIFY(meanAbsDiff(blurThenColor, colorThenBlur) > 0.3);
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(clipOf(doc, clip).effects[0].id, std::string("fx1"));

    // ---- bypass and strength
    auto* enabled = w.inspector()->findChild<QCheckBox*>("fxenabled:fx2");
    QVERIFY(enabled && enabled->isChecked());
    enabled->setChecked(false);
    QVERIFY(!clipOf(doc, clip).effects[1].enabled);
    const QImage bypassed = waitForPreview(w, 60);
    QVERIFY(meanAbsDiff(bypassed, blurred) < 0.05);
    enabled->setChecked(true);
    auto* mix = w.inspector()->findChild<QDoubleSpinBox*>("fxspin:fx.fx2.mix");
    QVERIFY(mix);
    QCOMPARE(mix->value(), 100.0);
    typeInto(mix, "50");
    QCOMPARE(clipOf(doc, clip).effects[1].mix.value(Time(0)), 0.5);
    const QImage half = waitForPreview(w, 60);
    QVERIFY(meanGreen(half) > meanGreen(blurred) + 3 && meanGreen(half) < meanGreen(brighter) - 3);

    // ---- keyframes on an effect parameter (auto-key after the first key)
    QTest::mouseClick(w.inspector()->findChild<QToolButton*>("fxkey:fx.fx2.brightness"), Qt::LeftButton);
    QVERIFY(clipOf(doc, clip).effects[1].params.at("brightness").isAnimated());
    w.playback()->seek(150);
    QTRY_COMPARE(doc->playhead(), int64_t(150));
    typeInto(w.inspector()->findChild<QDoubleSpinBox*>("fxspin:fx.fx2.brightness"), "-60");
    QCOMPARE(clipOf(doc, clip).effects[1].params.at("brightness").keys().size(), size_t(2));
    QCOMPARE(clipOf(doc, clip).effects[1].params.at("brightness").value(Rational(2)), 60.0);
    QCOMPARE(clipOf(doc, clip).effects[1].params.at("brightness").value(Rational(5)), -60.0);
    w.playback()->seek(150);
    const QImage darkened = waitForPreview(w, 150);
    QVERIFY(meanGreen(darkened) < meanGreen(half));
    shot(&w, "m4_01_effect_stack.png");

    // ---- duplicate, reset, delete, copy and paste
    QTest::mouseClick(w.inspector()->findChild<QToolButton*>("fxdup:fx1"), Qt::LeftButton);
    QCOMPARE(clipOf(doc, clip).effects.size(), size_t(3));
    QCOMPARE(clipOf(doc, clip).effects[1].id, std::string("fx3"));
    QCOMPARE(clipOf(doc, clip).effects[1].params.at("radius").value(Time(0)), 60.0);
    QTest::mouseClick(w.inspector()->findChild<QToolButton*>("fxresetall:fx3"), Qt::LeftButton);
    QCOMPARE(clipOf(doc, clip).effects[1].params.at("radius").value(Time(0)), 20.0);
    QTest::mouseClick(w.inspector()->findChild<QToolButton*>("fxdelete:fx3"), Qt::LeftButton);
    QCOMPARE(clipOf(doc, clip).effects.size(), size_t(2));
    QVERIFY(ed->copyEffects(clip));
    doc->setSelection({other});
    QVERIFY(ed->canPasteEffects());
    QTest::mouseClick(w.inspector()->findChild<QToolButton*>("pasteEffects"), Qt::LeftButton);
    QCOMPARE(clipOf(doc, other).effects.size(), size_t(2));
    QCOMPARE(clipOf(doc, other).effects[1].params.at("brightness").keys().size(), size_t(2));   // keys kept
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QVERIFY(clipOf(doc, other).effects.empty());
    QTest::keyClick(&w, Qt::Key_Y, Qt::ControlModifier);
    QCOMPARE(clipOf(doc, other).effects.size(), size_t(2));

    // ---- save and reopen; preview vs export
    const QString file = QDir(tmp.path()).filePath("m4.lcproj");
    QVERIFY(w.saveProjectTo(file));
    MainWindow w2(MainWindow::Options{false, true});
    w2.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w2));
    QVERIFY(w2.openProjectFile(file));
    QVERIFY(w2.document()->sequence() == doc->sequence());
    const QString out = tmp.filePath("m4_effects.mp4");
    QString err;
    QVERIFY2(w2.exportTo(out, &err), qPrintable(err));
    VideoDecoder vd(p8(out));
    for (int64_t f : {60, 150, 330}) {
        w2.playback()->seek(f);
        const QImage pv = waitForPreview(w2, f);
        const VideoFrame& ef = vd.frameAt(frameToTime(f, FrameRate::fps30()));
        const QImage eq(ef.image.pixels.data(), ef.image.width, ef.image.height, ef.image.stride(), QImage::Format_RGBA8888);
        const QImage scaled = eq.scaled(pv.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const double d = meanAbsDiff(pv, scaled);
        qInfo("frame %lld: preview vs export with effects %.2f / 255", static_cast<long long>(f), d);
        QVERIFY2(d < 4.0, qPrintable(QString::number(d)));
    }
}

namespace {

void selectTab(QTabWidget* tabs, const QString& text) {
    for (int i = 0; i < tabs->count(); ++i)
        if (tabs->tabText(i) == text) tabs->setCurrentIndex(i);
    QApplication::processEvents();
}

// Mean of R, G or B (channel 0, 1, 2) and the mean chroma (max - min) of an image.
double meanChannel(const QImage& img0, int ch) {
    const QImage img = img0.convertToFormat(QImage::Format_RGBA8888);
    double s = 0;
    for (int y = 0; y < img.height(); ++y) {
        const uchar* p = img.constScanLine(y);
        for (int x = 0; x < img.width(); ++x) s += p[x * 4 + ch];
    }
    return s / (double(img.width()) * img.height());
}

double meanChroma(const QImage& img0) {
    const QImage img = img0.convertToFormat(QImage::Format_RGBA8888);
    double s = 0;
    for (int y = 0; y < img.height(); ++y) {
        const uchar* p = img.constScanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            const int r = p[x * 4], g = p[x * 4 + 1], b = p[x * 4 + 2];
            s += std::max({r, g, b}) - std::min({r, g, b});
        }
    }
    return s / (double(img.width()) * img.height());
}

// Fraction of clearly blue pixels (hue near 240 deg, saturated).
double blueShare(const QImage& img0) {
    const QImage img = img0.convertToFormat(QImage::Format_RGBA8888);
    long n = 0;
    for (int y = 0; y < img.height(); ++y) {
        const uchar* p = img.constScanLine(y);
        for (int x = 0; x < img.width(); ++x)
            if (p[x * 4 + 2] > p[x * 4] + 60 && p[x * 4 + 2] > p[x * 4 + 1] + 60) ++n;
    }
    return double(n) / (double(img.width()) * img.height());
}

void dragWidget(QWidget* w, QPointF from, QPointF to, int steps = 6) {
    QTest::mousePress(w, Qt::LeftButton, {}, from.toPoint());
    for (int i = 1; i <= steps; ++i) QTest::mouseMove(w, (from + (to - from) * (double(i) / steps)).toPoint());
    QTest::mouseRelease(w, Qt::LeftButton, {}, to.toPoint());
    QApplication::processEvents();
}

const EffectRef* fxOf(Document* doc, const std::string& clip, const std::string& type) {
    return EditController::colorEffect(*edit::findClip(doc->sequence(), clip), type);
}

}  // namespace

void GuiTests::m4ColorTab() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString presetFile = tmp.filePath("color_presets.json");
    qputenv("MEERCUT_COLOR_PRESETS", presetFile.toUtf8());
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({mediaPath("C_testsrc2_1080p30_10s.mp4"), mediaPath("A_red_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    QVERIFY(ed->addToTimeline(ids[1], Rational(10)));
    const std::string clip = doc->sequence().tracks[0].clips[0].id, other = doc->sequence().tracks[0].clips[1].id;
    // a bypassed blur from the Effects tab: Color tab effects go ahead of the clip's other effects
    const std::string blur = ed->addEffect(clip, "fa.fx.blur.gaussian");
    QVERIFY(ed->setEffectEnabled(clip, blur, false));
    doc->setSelection({clip});
    w.playback()->seek(60);
    const QImage plain = waitForPreview(w, 60);
    QVERIFY(!plain.isNull());
    ColorPanel* cp = w.inspector()->colorPanel();
    QVERIFY(cp);
    selectTab(w.inspector()->clipTabs(), "Color");
    QVERIFY(cp->isVisible());

    // ---- Basic: untouched controls show neutral values; the first change adds the effect (one undo step)
    selectTab(cp->tabs(), "Basic");
    auto* sat = cp->findChild<QDoubleSpinBox*>("colorspin:fa.color.basic/saturation");
    auto* sharpAmount = cp->findChild<QDoubleSpinBox*>("colorspin:fa.fx.sharpen/amount");
    auto* sharpRadius = cp->findChild<QDoubleSpinBox*>("colorspin:fa.fx.sharpen/radius");
    QVERIFY(sat && sharpAmount && sharpRadius);
    QCOMPARE(sat->value(), 0.0);
    QCOMPARE(sharpAmount->value(), 0.0);   // neutral, not the effect's default 80 %
    QVERIFY(!cp->findChild<QCheckBox*>("colorenabled:fa.color.basic")->isEnabled());
    typeInto(sat, "-100");
    QCOMPARE(clipOf(doc, clip).effects.size(), size_t(2));
    QCOMPARE(clipOf(doc, clip).effects[0].type, std::string("fa.color.basic"));   // ahead of the blur
    QCOMPARE(clipOf(doc, clip).effects[0].params.at("saturation").value(Time(0)), -100.0);
    QVERIFY(cp->findChild<QCheckBox*>("colorenabled:fa.color.basic")->isEnabled());
    const QImage grey = waitForPreview(w, 60);
    qInfo("mean chroma plain %.1f, saturation -100 %.1f", meanChroma(plain), meanChroma(grey));
    QVERIFY(meanChroma(grey) < 2.0 && meanChroma(plain) > 40.0);
    shot(&w, "m4_02_color_basic.png");
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);   // one step: the effect is gone again
    QCOMPARE(clipOf(doc, clip).effects.size(), size_t(1));
    QCOMPARE(sat->value(), 0.0);
    QVERIFY(meanAbsDiff(waitForPreview(w, 60), plain) < 0.05);
    // sharpen: the radius alone adds a neutral (amount 0) sharpen; the amount then sharpens
    typeInto(sharpRadius, "3");
    QCOMPARE(clipOf(doc, clip).effects[0].type, std::string("fa.fx.sharpen"));
    QCOMPARE(clipOf(doc, clip).effects[0].params.at("amount").value(Time(0)), 0.0);
    QVERIFY(meanAbsDiff(waitForPreview(w, 60), plain) < 0.05);
    typeInto(sharpAmount, "200");
    const QImage sharp = waitForPreview(w, 60);
    qInfo("sharpness plain %.1f, sharpened %.1f", sharpness(plain), sharpness(sharp));
    QVERIFY(sharpness(sharp) > sharpness(plain) * 1.3);
    QTest::mouseClick(cp->findChild<QToolButton*>("colorresetall:fa.fx.sharpen"), Qt::LeftButton);
    QCOMPARE(fxOf(doc, clip, "fa.fx.sharpen")->params.at("amount").value(Time(0)), 0.0);   // reset = neutral

    // ---- HSL: the blue range desaturated; the other ranges untouched
    selectTab(cp->tabs(), "HSL");
    QTest::mouseClick(cp->findChild<QToolButton*>("hslband:blue"), Qt::LeftButton);
    QCOMPARE(cp->hslBand(), 5);
    auto* blueSat = cp->findChild<QDoubleSpinBox*>("colorspin:fa.color.hsl/blue.saturation");
    QVERIFY(blueSat && blueSat->isVisible());
    typeInto(blueSat, "-100");
    QCOMPARE(fxOf(doc, clip, "fa.color.hsl")->params.at("blue.saturation").value(Time(0)), -100.0);
    QCOMPARE(fxOf(doc, clip, "fa.color.hsl")->params.at("red.saturation").value(Time(0)), 0.0);
    const QImage noBlue = waitForPreview(w, 60);
    qInfo("blue share plain %.4f, blue saturation -100 %.4f", blueShare(plain), blueShare(noBlue));
    QVERIFY(blueShare(plain) > 0.02 && blueShare(noBlue) < blueShare(plain) * 0.2);
    QVERIFY(meanChroma(noBlue) > meanChroma(plain) * 0.6);   // other colours stay
    shot(&w, "m4_03_color_hsl.png");
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QVERIFY(fxOf(doc, clip, "fa.color.hsl") == nullptr);

    // ---- Curves: drag the master curve's middle up (adds a point on the curve, then moves it: one undo step)
    selectTab(cp->tabs(), "Curves");
    CurveEditor* tone = cp->toneCurve();
    QVERIFY(tone && tone->isVisible());
    QTest::mouseClick(cp->findChild<QToolButton*>("curvechannel:master"), Qt::LeftButton);
    dragWidget(tone, tone->toWidget({0.5, 0.5}), tone->toWidget({0.5, 0.75}));
    const EffectRef* cv = fxOf(doc, clip, "fa.color.curves");
    QVERIFY(cv);
    const auto& master = cv->curves.at("master");
    QCOMPARE(master.size(), size_t(3));
    QVERIFY(std::fabs(master[1].x - 0.5) < 0.01 && std::fabs(master[1].y - 0.75) < 0.01);
    const QImage lifted = waitForPreview(w, 60);
    qInfo("mean green plain %.1f, master curve 0.5 -> 0.75: %.1f", meanGreen(plain), meanGreen(lifted));
    QVERIFY(meanGreen(lifted) > meanGreen(plain) + 15);
    // the red channel with a preset shape; the master curve is drawn as a ghost behind it
    QTest::mouseClick(cp->findChild<QToolButton*>("curvechannel:red"), Qt::LeftButton);
    QCOMPARE(cp->toneChannel(), std::string("red"));
    auto* preset = cp->findChild<QComboBox*>("curvePreset");
    QVERIFY(preset);
    const int darken = preset->findText("Darken");
    QVERIFY(darken > 0);
    preset->setCurrentIndex(darken);
    emit preset->activated(darken);
    QApplication::processEvents();
    QCOMPARE(fxOf(doc, clip, "fa.color.curves")->curves.at("red").size(), size_t(3));
    const QImage lessRed = waitForPreview(w, 60);
    qInfo("red preset Darken on the red channel: mean R %.2f -> %.2f, G %.2f -> %.2f, B %.2f -> %.2f", meanChannel(lifted, 0),
          meanChannel(lessRed, 0), meanChannel(lifted, 1), meanChannel(lessRed, 1), meanChannel(lifted, 2), meanChannel(lessRed, 2));
    // testsrc2's red is mostly 0 or 255, which a mid-tone curve keeps: a small drop, and G / B exactly unchanged
    QVERIFY(meanChannel(lessRed, 0) < meanChannel(lifted, 0) - 1.5);
    QVERIFY(std::fabs(meanChannel(lessRed, 1) - meanChannel(lifted, 1)) < 0.01);
    QVERIFY(std::fabs(meanChannel(lessRed, 2) - meanChannel(lifted, 2)) < 0.01);
    // hue vs saturation: pull the blue hues down to grey
    QTest::mouseClick(cp->findChild<QToolButton*>("curvechannel:hueSat"), Qt::LeftButton);
    QCOMPARE(cp->hueChannel(), std::string("hueSat"));
    CurveEditor* hue = cp->hueCurve();
    for (double x : {0.5, 0.85}) {   // anchor points at aqua-ish and magenta-ish hues
        QTest::mouseClick(hue, Qt::LeftButton, {}, hue->toWidget({x, 0.5}).toPoint());
    }
    dragWidget(hue, hue->toWidget({240.0 / 360, 0.5}), hue->toWidget({240.0 / 360, 0.0}));
    QCOMPARE(fxOf(doc, clip, "fa.color.curves")->curves.at("hueSat").size(), size_t(3));
    const QImage hueSat = waitForPreview(w, 60);
    qInfo("blue share with hue-vs-sat curve %.4f", blueShare(hueSat));
    QVERIFY(blueShare(hueSat) < blueShare(lessRed) * 0.3);
    shot(&w, "m4_04_color_curves.png");
    // delete a point by double-click (tone curves keep their end points)
    QTest::mouseClick(cp->findChild<QToolButton*>("curvechannel:master"), Qt::LeftButton);
    doubleClick(tone, tone->toWidget(fxOf(doc, clip, "fa.color.curves")->curves.at("master")[1]).toPoint());
    QCOMPARE(fxOf(doc, clip, "fa.color.curves")->curves.at("master").size(), size_t(2));
    doubleClick(tone, tone->toWidget({0, 0}).toPoint());
    QCOMPARE(fxOf(doc, clip, "fa.color.curves")->curves.at("master").size(), size_t(2));
    // undo the whole curve work step by step: delete, hue drag, two anchor clicks, red preset, master drag
    for (int i = 0; i < 6; ++i) QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QVERIFY(fxOf(doc, clip, "fa.color.curves") == nullptr);

    // ---- Wheels: drag the midtones puck towards red; type a value; double-click resets the tint
    selectTab(cp->tabs(), "Wheels");
    ColorWheel* mid = cp->wheel(1);
    QVERIFY(mid && mid->isVisible());
    const QPointF c(mid->width() / 2.0, mid->height() / 2.0);
    dragWidget(mid, c, c + QPointF(32, 0));
    const EffectRef* wh = fxOf(doc, clip, "fa.color.wheels");
    QVERIFY(wh);
    const double r = wh->params.at("midtones.r").value(Time(0)), g = wh->params.at("midtones.g").value(Time(0)),
                 b = wh->params.at("midtones.b").value(Time(0));
    qInfo("midtones wheel dragged to red: r %.3f g %.3f b %.3f", r, g, b);
    QVERIFY(r > 0.3 && g < -0.1 && std::fabs(g - b) < 1e-6 && std::fabs(r + g + b) < 1e-6);
    const QImage warm = waitForPreview(w, 60);
    QVERIFY(meanChannel(warm, 0) > meanChannel(plain, 0) + 5 && meanChannel(warm, 2) < meanChannel(plain, 2));
    auto* shadowsB = cp->findChild<QDoubleSpinBox*>("colorspin:fa.color.wheels/shadows.b");
    QVERIFY(shadowsB);
    typeInto(shadowsB, "0.4");
    QCOMPARE(fxOf(doc, clip, "fa.color.wheels")->params.at("shadows.b").value(Time(0)), 0.4);
    QVERIFY(cp->wheel(0)->chroma().manhattanLength() > 0.1);   // the shadows puck follows the fields
    shot(&w, "m4_05_color_wheels.png");
    doubleClick(mid, mid->puckPosition().toPoint());
    QVERIFY(std::fabs(fxOf(doc, clip, "fa.color.wheels")->params.at("midtones.r").value(Time(0))) < 1e-9);

    // ---- custom preset: save, apply to the other clip (one undo step), persists in the preset file
    dragWidget(mid, c, c + QPointF(0, -30));   // midtones towards green
    QVERIFY(cp->saveCustomPreset("Test Look"));
    QVERIFY(QFileInfo::exists(presetFile));
    ColorPresetLibrary reloaded(presetFile);
    QCOMPARE(reloaded.presets().size(), size_t(1));
    QCOMPARE(reloaded.presets()[0].name, std::string("Test Look"));
    const std::string presetId = reloaded.presets()[0].id;
    doc->setSelection({other});
    QApplication::processEvents();
    QVERIFY(cp->applyPreset(presetId));
    const EffectRef* applied = fxOf(doc, other, "fa.color.wheels");
    QVERIFY(applied);
    QCOMPARE(applied->params.at("midtones.g").value(Time(0)), fxOf(doc, clip, "fa.color.wheels")->params.at("midtones.g").value(Time(0)));
    QVERIFY(fxOf(doc, other, "fa.fx.sharpen") != nullptr);   // every Color tab effect of the source
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QVERIFY(clipOf(doc, other).effects.empty());
    QTest::keyClick(&w, Qt::Key_Y, Qt::ControlModifier);

    // ---- save and reopen; preview vs export
    doc->setSelection({clip});
    selectTab(cp->tabs(), "Curves");
    dragWidget(tone, tone->toWidget({0.25, 0.25}), tone->toWidget({0.25, 0.15}));
    const QString file = QDir(tmp.path()).filePath("m4_color.lcproj");
    QVERIFY(w.saveProjectTo(file));
    MainWindow w2(MainWindow::Options{false, true});
    w2.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w2));
    QVERIFY(w2.openProjectFile(file));
    QVERIFY(w2.document()->sequence() == doc->sequence());
    // preview vs export with the colour effects, and (control) with every effect bypassed: the effects must not add
    // to the difference that resampling the detailed test pattern and the H.264 round trip cause on their own
    // (measured: frame 60 4.28 with / 4.45 without; frame 330 3.28 / 2.37, where the green tint pushes the saturated
    // red's blue to the gamut edge that the YUV round trip clips). Negative control: the preview with the effects
    // against the bypassed export must differ clearly.
    auto previewVsExport = [&](const QString& name, int64_t f) {
        const QString out = tmp.filePath(name);
        QString err;
        if (!QFileInfo::exists(out) && !w2.exportTo(out, &err)) return -1.0;
        VideoDecoder vd(p8(out));
        w2.playback()->seek(f);
        const QImage pv = waitForPreview(w2, f);
        const VideoFrame& ef = vd.frameAt(frameToTime(f, FrameRate::fps30()));
        const QImage eq(ef.image.pixels.data(), ef.image.width, ef.image.height, ef.image.stride(), QImage::Format_RGBA8888);
        return meanAbsDiff(pv, eq.scaled(pv.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    };
    const int64_t frames[2] = {60, 330};
    double withFx[2], without[2], mismatched[2];
    for (int i = 0; i < 2; ++i) withFx[i] = previewVsExport("m4_color.mp4", frames[i]);
    std::vector<std::pair<std::string, std::string>> enabledFx;   // (clip, effect): collected first, edits replace clips
    for (const auto& tr : w2.document()->sequence().tracks)
        for (const auto& cl : tr.clips)
            for (const auto& e : cl.effects)
                if (e.enabled) enabledFx.push_back({cl.id, e.id});
    for (const auto& [clipId, fxId] : enabledFx) QVERIFY(w2.editController()->setEffectEnabled(clipId, fxId, false));
    const int bypassed = static_cast<int>(enabledFx.size());
    for (int i = 0; i < 2; ++i) without[i] = previewVsExport("m4_color_bypassed.mp4", frames[i]);
    for (int i = 0; i < bypassed; ++i) QVERIFY(w2.document()->undo());
    QVERIFY(w2.document()->sequence() == doc->sequence());
    for (int i = 0; i < 2; ++i) mismatched[i] = previewVsExport("m4_color_bypassed.mp4", frames[i]);
    for (int i = 0; i < 2; ++i) {
        qInfo("frame %lld: preview vs export %.2f / 255 with the colour effects, %.2f bypassed; negative control %.2f",
              static_cast<long long>(frames[i]), withFx[i], without[i], mismatched[i]);
        QVERIFY(withFx[i] >= 0 && without[i] >= 0);
        QVERIFY2(withFx[i] < without[i] + 1.5, qPrintable(QString::number(withFx[i])));
        QVERIFY2(mismatched[i] > withFx[i] + 3.0, qPrintable(QString::number(mismatched[i])));
    }
    qunsetenv("MEERCUT_COLOR_PRESETS");
}

namespace {

// Mean difference between the preview of `f` and the same frame of an export (exported once per file name).
double previewVsExportOf(MainWindow& w, const QString& out, int64_t f) {
    QString err;
    if (!QFileInfo::exists(out) && !w.exportTo(out, &err)) return -1.0;
    VideoDecoder vd(p8(out));
    w.playback()->seek(f);
    const QImage pv = waitForPreview(w, f);
    const VideoFrame& ef = vd.frameAt(frameToTime(f, FrameRate::fps30()));
    const QImage eq(ef.image.pixels.data(), ef.image.width, ef.image.height, ef.image.stride(), QImage::Format_RGBA8888);
    return meanAbsDiff(pv, eq.scaled(pv.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

void chooseLut(ColorPanel* cp, const QString& name) {
    QComboBox* combo = cp->lutCombo();
    const int i = combo->findText(name, Qt::MatchStartsWith);
    QVERIFY2(i >= 0, qPrintable(name));
    combo->setCurrentIndex(i);
    emit combo->activated(i);
    QApplication::processEvents();
}

std::string lutRefOf(Document* doc, const std::string& clip) {
    const EffectRef* e = EditController::colorEffect(*edit::findClip(doc->sequence(), clip), "fa.color.lut");
    if (!e) return "<no LUT effect>";
    const auto it = e->strings.find("lut");
    return it == e->strings.end() ? std::string() : it->second;
}

}  // namespace

void GuiTests::m4LutsAndHdr() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    qputenv("MEERCUT_LUTS", tmp.filePath("luts.json").toUtf8());
    const QString projDir = tmp.filePath("project");
    QVERIFY(QDir().mkpath(projDir));
    const QString lutCopy = QDir(projDir).filePath("look.cube");
    QVERIFY(QFile::copy(QDir(FA_TESTMEDIA_DIR).filePath("luts/lut_random_3.cube"), lutCopy));
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();

    // ---- HDR source: imported with a notice, tone mapped to SDR
    QStringList errors, notices;
    const auto hdrIds = doc->importMedia({mediaPath("P_hdr_pq_1080p30_2s.mp4")}, &errors, &notices);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join("; ")));
    QCOMPARE(notices.size(), 1);
    QVERIFY2(notices[0].contains("PQ") && notices[0].contains("SDR"), qPrintable(notices[0]));
    const auto ids = w.importFiles({mediaPath("C_testsrc2_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    QVERIFY(ed->addToTimeline(hdrIds[0], Rational(10)));
    const std::string clip = doc->sequence().tracks[0].clips[0].id;
    w.playback()->seek(315);   // HDR clip: reference white on the left half -> 241, neutral
    const QImage hdrPreview = waitForPreview(w, 315);
    const QColor white = hdrPreview.pixelColor(hdrPreview.width() / 4, hdrPreview.height() / 2);
    qInfo("HDR reference white in the preview: %d %d %d", white.red(), white.green(), white.blue());
    QVERIFY(std::abs(white.green() - 241) <= 3 && std::abs(white.red() - white.blue()) <= 2);

    doc->setSelection({clip});
    w.playback()->seek(60);
    const QImage plain = waitForPreview(w, 60);
    ColorPanel* cp = w.inspector()->colorPanel();
    selectTab(w.inspector()->clipTabs(), "Color");
    selectTab(cp->tabs(), "Basic");
    QVERIFY(cp->lutCombo()->isVisible());
    QVERIFY(cp->lutCombo()->count() >= 12);   // None + 11 built-in looks, each with a thumbnail
    QVERIFY(!cp->lutCombo()->itemIcon(1).isNull());

    // ---- a built-in look (one undo step), strength 50 %
    chooseLut(cp, "Antique");   // the built-in look "sepia" (the Sepia filter is another tile)
    QCOMPARE(lutRefOf(doc, clip), std::string("builtin:sepia"));
    const QImage sepia = waitForPreview(w, 60);
    qInfo("mean chroma plain %.1f, sepia %.1f", meanChroma(plain), meanChroma(sepia));
    QVERIFY(meanChroma(sepia) < meanChroma(plain) * 0.3);
    QVERIFY(meanChannel(sepia, 0) > meanChannel(sepia, 2) + 20);   // warm brown
    auto* strength = cp->findChild<QDoubleSpinBox*>("colorspin:fa.color.lut/strength");
    QVERIFY(strength);
    QCOMPARE(strength->value(), 100.0);
    typeInto(strength, "50");
    const QImage half = waitForPreview(w, 60);
    QVERIFY(meanChroma(half) > meanChroma(sepia) + 10 && meanChroma(half) < meanChroma(plain) - 10);
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);   // strength
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);   // the look and its effect
    QCOMPARE(lutRefOf(doc, clip), std::string("<no LUT effect>"));

    // ---- import: a valid .cube joins the library (with a thumbnail) and is applied; an invalid one is refused
    const QString bad = tmp.filePath("broken.cube");
    {
        QFile f(bad);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("LUT_3D_SIZE 2\n0 0 0\n");
    }
    QVERIFY(!cp->importLut(bad));
    QVERIFY2(w.lastMessage().contains("expected 8 table entries"), qPrintable(w.lastMessage()));
    QVERIFY(cp->importLut(lutCopy));
    QCOMPARE(QString::fromStdString(lutRefOf(doc, clip)), QDir::cleanPath(lutCopy));   // unsaved project: absolute
    QVERIFY(cp->lutCombo()->findText("look") >= 0);
    QVERIFY(!ed->lutLibrary()->thumbnail(lutRefOf(doc, clip)).isNull());
    QVERIFY(cp->lutStatus()->text().contains("tetrahedral"));
    const QImage looked = waitForPreview(w, 60);
    QVERIFY(meanAbsDiff(looked, plain) > 5);
    shot(&w, "m4_06_color_lut.png");

    // ---- saved next to the project: stored relative, so project + LUT can move together
    const QString projFile = QDir(projDir).filePath("lut.lcproj");
    QVERIFY(w.saveProjectTo(projFile));
    QVERIFY(ed->setColorLut(clip, QDir::cleanPath(lutCopy).toStdString()));
    QCOMPARE(lutRefOf(doc, clip), std::string("look.cube"));
    QVERIFY(w.saveProjectTo(projFile));
    const QString moved = tmp.filePath("moved");
    QVERIFY(QDir().mkpath(moved));
    QVERIFY(QFile::copy(projFile, QDir(moved).filePath("lut.lcproj")));
    QVERIFY(QFile::copy(lutCopy, QDir(moved).filePath("look.cube")));
    MainWindow w2(MainWindow::Options{false, true});
    w2.resize(1600, 1000);
    w2.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w2));
    QVERIFY(w2.openProjectFile(QDir(moved).filePath("lut.lcproj")));
    w2.document()->setSelection({clip});
    w2.playback()->seek(60);
    QVERIFY(meanAbsDiff(waitForPreview(w2, 60), looked) < 0.5);
    QVERIFY(w2.playback()->offlineMessages().isEmpty());

    // ---- the LUT file goes missing: preview says so, the Color tab offers Relink (one undo step)
    const QString renamed = QDir(moved).filePath("renamed.cube");
    QVERIFY(QFile::rename(QDir(moved).filePath("look.cube"), renamed));
    w2.document()->setSelection({});
    w2.document()->setSelection({clip});   // re-evaluate
    ColorPanel* cp2 = w2.inspector()->colorPanel();
    selectTab(w2.inspector()->clipTabs(), "Color");
    selectTab(cp2->tabs(), "Basic");
    cp2->setClip(clip);
    QVERIFY2(cp2->lutStatus()->text().contains("file not found"), qPrintable(cp2->lutStatus()->text()));
    QVERIFY(cp2->findChild<QToolButton*>("relinkLut")->isVisible());
    // a new project version so the preview renders again instead of showing frames prepared before the rename
    QVERIFY(w2.editController()->setColorParams(clip, "fa.color.lut", {{"strength", 100.0}}));
    const QImage missing = waitForPreview(w2, 60);
    QVERIFY(meanAbsDiff(missing, plain) < 0.5);   // the LUT is skipped, not guessed
    QStringList off = w2.playback()->offlineMessages();
    QVERIFY2(off.join(" ").contains("look.cube"), qPrintable(off.join(" | ")));
    QVERIFY(cp2->relinkMissingLut(renamed));
    QVERIFY(QString::fromStdString(lutRefOf(w2.document(), clip)).endsWith("renamed.cube"));
    QVERIFY(cp2->lutStatus()->text().contains("tetrahedral"));
    QVERIFY(w2.document()->undo());
    QCOMPARE(lutRefOf(w2.document(), clip), std::string("look.cube"));
    QVERIFY(w2.document()->redo());

    // ---- export: preflight warns about the HDR source; preview and export agree
    DecoderPool pool(p8(QDir(moved).filePath("lut.lcproj")));
    bool hdrWarned = false;
    for (const auto& issue : preflight(w2.document()->project(), p8(QDir(moved).filePath("lut.lcproj")), RenderOptions{}, pool))
        if (issue.severity == ValidationIssue::Severity::Warning && issue.message.find("HDR") != std::string::npos) hdrWarned = true;
    QVERIFY(hdrWarned);
    const double d60 = previewVsExportOf(w2, tmp.filePath("lut.mp4"), 60), d315 = previewVsExportOf(w2, tmp.filePath("lut.mp4"), 315);
    // control: the same frame with the LUT bypassed (resampling the detailed pattern + H.264 alone)
    const EffectRef* lutFx = EditController::colorEffect(*edit::findClip(w2.document()->sequence(), clip), "fa.color.lut");
    QVERIFY(lutFx);
    const std::string lutFxId = lutFx->id;
    QVERIFY(w2.editController()->setEffectEnabled(clip, lutFxId, false));
    const double control = previewVsExportOf(w2, tmp.filePath("lut_bypassed.mp4"), 60);
    QVERIFY(w2.document()->undo());
    qInfo("preview vs export: LUT frame 60 %.2f / 255 (LUT bypassed: %.2f), HDR frame 315 %.2f / 255", d60, control, d315);
    QVERIFY(d60 >= 0 && control >= 0 && d60 < control + 1.0);
    QVERIFY(d315 >= 0 && d315 < 2.0);
    qunsetenv("MEERCUT_LUTS");
}

namespace {

// Drops the browser tile `key` onto `target` in the timeline (the events Qt sends during a drag). M10: onto a clip,
// with Ctrl the effect is applied to that clip; without, it becomes its own clip above the video.
void dropEffectOn(TimelineView* tl, EffectsBrowser* page, const QString& key, const QPoint& target,
                  Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QListWidgetItem* it = page->itemFor(key);
    QVERIFY2(it, qPrintable(key));
    std::unique_ptr<QMimeData> mime(page->list()->model()->mimeData({page->list()->indexFromItem(it)}));
    QVERIFY(mime && mime->hasFormat(kMimeEffect));
    QDragEnterEvent enter(target, Qt::CopyAction, mime.get(), Qt::LeftButton, mods);
    QApplication::sendEvent(tl, &enter);
    QDragMoveEvent move(target, Qt::CopyAction, mime.get(), Qt::LeftButton, mods);
    QApplication::sendEvent(tl, &move);
    QVERIFY(move.isAccepted());
    QDropEvent drop(QPointF(target), Qt::CopyAction, mime.get(), Qt::LeftButton, mods);
    QApplication::sendEvent(tl, &drop);
    QApplication::processEvents();
}

}  // namespace

void GuiTests::m4EffectsAndFiltersBrowsers() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    qputenv("MEERCUT_EFFECT_PRESETS", tmp.filePath("effect_presets.json").toUtf8());
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({mediaPath("C_testsrc2_1080p30_10s.mp4"), mediaPath("D_greenscreen_disc_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    QVERIFY(ed->addToTimeline(ids[0], Rational(10)));
    TimelineView* tl = w.timelinePanel()->view();
    tl->zoomToFit();
    QApplication::processEvents();
    const std::string a = doc->sequence().tracks[0].clips[0].id, b = doc->sequence().tracks[0].clips[1].id;

    // ---- Effects page: categories, search, favourites, hover preview
    EffectsBrowser* fxPage = w.effectsPage();
    QVERIFY(fxPage);
    w.resourceTabs()->setCurrentWidget(fxPage);
    QApplication::processEvents();
    QVERIFY(fxPage->list()->isVisible());
    // every effect of the registry's Effects section (M4's 13, M11's 17) + Adjustment Layer
    QCOMPARE(fxPage->list()->count(), 31);
    for (const char* cat : {"Blur", "Light", "Stylize", "Film", "Glitch", "Lens", "Distortion", "Keying", "Detail", "Camera", "Frame"})
        QVERIFY2(!fxPage->categories()->findItems(cat, Qt::MatchExactly).isEmpty(), cat);
    fxPage->search()->setText("blur");
    QApplication::processEvents();
    QVERIFY(fxPage->itemFor("fa.fx.blur.gaussian") && fxPage->itemFor("fa.fx.blur.directional"));
    for (int i = 0; i < fxPage->list()->count(); ++i)   // names and descriptions are searched (Glow "blurs" too)
        QVERIFY(fxPage->list()->item(i)->text().contains("Blur") || fxPage->list()->item(i)->toolTip().contains("blur"));
    QVERIFY(!fxPage->itemFor("fa.fx.pixelate"));
    fxPage->search()->clear();
    fxPage->selectCategory("Distortion");
    QApplication::processEvents();
    QCOMPARE(fxPage->list()->count(), 5);   // Wave, Bulge / Pinch, and M11's Mirror, Kaleidoscope, Swirl
    fxPage->selectCategory("All Effects");
    fxPage->setFavourite("fa.fx.glow", true);
    fxPage->selectCategory("Favourites");
    QApplication::processEvents();
    QCOMPARE(fxPage->list()->count(), 1);
    QVERIFY(fxPage->list()->item(0)->text().contains("Glow"));
    fxPage->setFavourite("fa.fx.glow", false);
    fxPage->selectCategory("All Effects");
    QApplication::processEvents();
    QListWidgetItem* wave = fxPage->itemFor("fa.fx.wave");
    QVERIFY(wave);
    const qint64 still = wave->icon().pixmap(QSize(60, 34)).cacheKey();
    const QRect waveRect = fxPage->list()->visualItemRect(wave);
    {   // the mouse rests on the tile (event filter on the list's viewport)
        QWidget* vp = fxPage->list()->viewport();
        QMouseEvent mv(QEvent::MouseMove, QPointF(waveRect.center()), QPointF(vp->mapToGlobal(waveRect.center())), Qt::NoButton,
                       Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(vp, &mv);
    }
    QTRY_VERIFY_WITH_TIMEOUT(wave->icon().pixmap(QSize(60, 34)).cacheKey() != still, 3000);   // the tile animates
    shot(&w, "m4_07_effects_browser.png");

    // ---- Ctrl+drag onto a clip (M10: a plain drop makes a layer above it): one undo step; the preview changes
    doc->setSelection({a});
    w.playback()->seek(60);
    const QImage plain = waitForPreview(w, 60);
    dropEffectOn(tl, fxPage, "fa.fx.pixelate", tl->clipRect(a).center(), Qt::ControlModifier);
    if (QTest::currentTestFailed()) return;
    QCOMPARE(clipOf(doc, a).effects.size(), size_t(1));
    QCOMPARE(clipOf(doc, a).effects[0].type, std::string("fa.fx.pixelate"));
    const QImage pixelated = waitForPreview(w, 60);
    // (block averaging itself is verified exactly in EffectsLibrary.PixelateAveragesBlocks; in the preview the
    // blocks are 24 px of the decoded frame scaled to the player, so they do not sit on the player's pixel grid)
    qInfo("pixelate: preview vs plain %.2f / 255", meanAbsDiff(pixelated, plain));
    QVERIFY(meanAbsDiff(pixelated, plain) > 3.0);
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QVERIFY(clipOf(doc, a).effects.empty());
    // double-click applies to the selected clips (both, one undo step)
    doc->setSelection({a, b});
    QVERIFY(fxPage->apply("fa.fx.blur.directional"));
    QCOMPARE(clipOf(doc, a).effects.size(), size_t(1));
    QCOMPARE(clipOf(doc, b).effects.size(), size_t(1));
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QVERIFY(clipOf(doc, a).effects.empty() && clipOf(doc, b).effects.empty());

    // ---- Filters page: a built-in look
    EffectsBrowser* fiPage = w.filtersPage();
    w.resourceTabs()->setCurrentWidget(fiPage);
    QApplication::processEvents();
    QVERIFY(fiPage->itemFor("look:teal-orange"));
    QVERIFY(fiPage->itemFor("fa.filter.bw") && fiPage->itemFor("fa.filter.sepia"));
    doc->setSelection({a});
    QVERIFY(fiPage->apply("look:bleach-bypass"));
    QCOMPARE(clipOf(doc, a).effects[0].type, std::string("fa.filter.look"));
    QCOMPARE(clipOf(doc, a).effects[0].strings.at("lut"), std::string("builtin:bleach-bypass"));
    const QImage bleached = waitForPreview(w, 60);
    QVERIFY(meanAbsDiff(bleached, plain) > 5);
    shot(&w, "m4_08_filters_browser.png");

    // ---- preset: saved from the Effects tab, offered under My Presets, keeps its settings
    QVERIFY(ed->addEffectToClips({a}, [] {
        EffectRef e = makeEffect(*findEffectDef("fa.fx.wave"), "x");
        e.params["amplitude"] = Animatable<double>(77);
        return e;
    }()));
    const std::string waveId = clipOf(doc, a).effects.back().id;
    selectTab(w.inspector()->clipTabs(), "Effects");
    QVERIFY(w.inspector()->effectEditor()->findChild<QToolButton*>(QString::fromStdString("fxsave:" + waveId)));
    QVERIFY(w.inspector()->effectEditor()->savePreset(waveId, "Big Wave"));
    w.resourceTabs()->setCurrentWidget(fxPage);
    fxPage->selectCategory("My Presets");
    QApplication::processEvents();
    QCOMPARE(fxPage->list()->count(), 1);
    QCOMPARE(fxPage->list()->item(0)->text(), QString("Big Wave"));
    const QString presetKey = fxPage->list()->item(0)->data(Qt::UserRole).toString();
    dropEffectOn(tl, fxPage, presetKey, tl->clipRect(b).center(), Qt::ControlModifier);
    if (QTest::currentTestFailed()) return;
    QCOMPARE(clipOf(doc, b).effects.size(), size_t(1));
    QCOMPARE(clipOf(doc, b).effects[0].params.at("amplitude").value(Time(0)), 77.0);

    // ---- chroma key: the green-screen clip over the test pattern
    QVERIFY(ed->addTrack(TrackKind::Video));
    std::string v2;
    for (const auto& tr : doc->sequence().tracks)
        if (tr.kind == TrackKind::Video && tr.id != doc->sequence().tracks[0].id) v2 = tr.id;
    QVERIFY(!v2.empty());
    QVERIFY(ed->addToTimeline(ids[1], Rational(0), v2));
    std::string green;
    for (const auto& tr : doc->sequence().tracks)
        if (tr.id == v2) green = tr.clips.front().id;
    w.playback()->seek(90);
    const QImage before = waitForPreview(w, 90);
    const QColor bg = before.pixelColor(before.width() * 9 / 10, before.height() / 2);   // green background area
    QVERIFY2(bg.green() > bg.red() + 80, qPrintable(bg.name()));
    fxPage->selectCategory("All Effects");
    QApplication::processEvents();
    dropEffectOn(tl, fxPage, "fa.fx.chromakey", tl->clipRect(green).center());
    if (QTest::currentTestFailed()) return;
    QVERIFY(!clipOf(doc, green).effects.empty());
    QCOMPARE(clipOf(doc, green).effects.back().type, std::string("fa.fx.chromakey"));
    const QImage keyed = waitForPreview(w, 90);
    const QColor through = keyed.pixelColor(keyed.width() * 9 / 10, keyed.height() / 2);
    qInfo("green screen pixel %s -> keyed %s", qPrintable(bg.name()), qPrintable(through.name()));
    QVERIFY(through.green() < through.red() + 60 || through.blue() > 100);   // the pattern below shows through
    shot(&w, "m4_09_chroma_key.png");
    qunsetenv("MEERCUT_EFFECT_PRESETS");
}

namespace {

// Mean chroma (max - min) and mean squared horizontal green gradient of a region given in fractions of the image.
double regionChroma(const QImage& img0, double x0, double x1) {
    const QImage img = img0.convertToFormat(QImage::Format_RGBA8888);
    double s = 0;
    long n = 0;
    for (int y = img.height() / 4; y < img.height() * 3 / 4; ++y)
        for (int x = int(img.width() * x0); x < int(img.width() * x1); ++x, ++n) {
            const QColor c = img.pixelColor(x, y);
            s += std::max({c.red(), c.green(), c.blue()}) - std::min({c.red(), c.green(), c.blue()});
        }
    return s / double(std::max(1L, n));
}

double regionSharpness(const QImage& img0, double x0, double x1, double y0, double y1) {
    const QImage img = img0.convertToFormat(QImage::Format_RGBA8888);
    double s = 0;
    long n = 0;
    for (int y = int(img.height() * y0); y < int(img.height() * y1); ++y)
        for (int x = std::max(1, int(img.width() * x0)); x < int(img.width() * x1); ++x, ++n) {
            const double d = img.pixelColor(x, y).green() - img.pixelColor(x - 1, y).green();
            s += d * d;
        }
    return s / double(std::max(1L, n));
}

}  // namespace

void GuiTests::m4AdjustmentLayersAndMaskedEffects() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    TimelineView* tl = w.timelinePanel()->view();
    PreviewCanvas* canvas = w.previewPanel()->canvas();
    const auto ids = w.importFiles({mediaPath("C_testsrc2_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    tl->zoomToFit();
    const std::string clip = doc->sequence().tracks[0].clips[0].id;
    w.playback()->seek(60);
    const QImage plain = waitForPreview(w, 60);

    // ---- adjustment layer from the Edit menu (above the footage, selected) with a filter from the Filters page
    QAction* addAdj = nullptr;
    for (QAction* a : w.findChildren<QAction*>())
        if (a->text() == "Add Adjustment Layer") addAdj = a;
    QVERIFY(addAdj);
    addAdj->trigger();
    const std::string adj = doc->primarySelection();
    const Track* adjTrack = nullptr;
    QVERIFY(edit::findClip(doc->sequence(), adj, &adjTrack) && clipOf(doc, adj).isAdjustment());
    QVERIFY(adjTrack->id != doc->sequence().tracks[0].id);   // above the footage
    QVERIFY(w.filtersPage()->apply("fa.filter.bw"));
    QCOMPARE(clipOf(doc, adj).effects[0].type, std::string("fa.filter.bw"));
    QVERIFY(regionChroma(waitForPreview(w, 60), 0, 1) < 2);   // the whole picture below is black & white
    // the adjustment layer is not "offline" in the timeline and does not capture clicks in the player
    QVERIFY(!tl->clipRect(adj).isEmpty());
    QTest::mouseClick(canvas, Qt::LeftButton, {}, canvas->frameRect().center().toPoint());
    QCOMPARE(doc->primarySelection(), clip);   // the footage under it was selected
    shot(&w, "m4_10_adjustment_layer.png");

    // ---- its mask, edited in the player: only the left half turns grey, then the mask moves right
    doc->setSelection({adj});
    QApplication::processEvents();
    const std::string mk = ed->addMask(adj, MaskShape::Rectangle);
    QVERIFY(!mk.empty());
    QVERIFY(ed->modifyClip(adj, "mask", [mk](Clip& c) {
        for (auto& m : c.masks)
            if (m.id == mk) {
                m.centerX = Animatable<double>(-0.45);   // canvas heights: left part of a 16:9 frame
                m.width = Animatable<double>(0.9);
                m.height = Animatable<double>(2.0);
            }
    }));
    QImage half = waitForPreview(w, 60);
    qInfo("adjustment mask: chroma left %.1f, right %.1f", regionChroma(half, 0.02, 0.3), regionChroma(half, 0.7, 0.98));
    QVERIFY(regionChroma(half, 0.02, 0.3) < 3 && regionChroma(half, 0.7, 0.98) > 40);
    QVERIFY(canvas->handlePos("m:center").has_value());
    QVERIFY(!canvas->handlePos("tl").has_value());   // no transform handles on an adjustment layer
    const QPointF mc = *canvas->handlePos("m:center");
    const QRectF fr = canvas->frameRect();
    drag(canvas, mc, mc + QPointF(fr.width() * 0.5, 0));
    const double movedX = clipOf(doc, adj).masks[0].centerX.constantValue();
    QVERIFY2(std::abs(movedX - (-0.45 + 0.5 * 1920.0 / 1080.0)) < 0.02, qPrintable(QString::number(movedX)));
    half = waitForPreview(w, 60);
    QVERIFY(regionChroma(half, 0.02, 0.3) > 40 && regionChroma(half, 0.7, 0.98) < 3);
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);   // one undo step for the drag
    QVERIFY(std::abs(clipOf(doc, adj).masks[0].centerX.constantValue() + 0.45) < 1e-9);

    // ---- a selective effect: a blur inside an ellipse mask of the effect, moved in the player, inverted
    QVERIFY(ed->removeMask(adj, mk));
    QVERIFY(ed->setEffectEnabled(adj, clipOf(doc, adj).effects[0].id, false));   // keep the colours
    doc->setSelection({clip});
    QApplication::processEvents();
    QVERIFY(w.effectsPage()->apply("fa.fx.blur.gaussian"));
    const std::string blur = clipOf(doc, clip).effects[0].id;
    selectTab(w.inspector()->clipTabs(), "Effects");
    auto* addEllipse = w.inspector()->effectEditor()->findChild<QToolButton*>(QString::fromStdString("fxmaskellipse:" + blur));
    QVERIFY(addEllipse);
    QTest::mouseClick(addEllipse, Qt::LeftButton);
    QCOMPARE(clipOf(doc, clip).effects[0].masks.size(), size_t(1));
    const std::string ref = blur + "/" + clipOf(doc, clip).effects[0].masks[0].id;
    QCOMPARE(doc->activeMask(), ref);
    QVERIFY(w.inspector()->effectEditor()->findChild<QCheckBox*>(QString::fromStdString("fxmaskinvert:" + ref)));
    QImage sel = waitForPreview(w, 60);
    // default ellipse: centred, half the height -> blurred in the middle, sharp at the left and right edges
    // (the outside region 8-26 % holds the red / green bar boundary: a flat bar would show no blur either way)
    const double inMid = regionSharpness(sel, 0.4, 0.6, 0.4, 0.6), atEdge = regionSharpness(sel, 0.08, 0.26, 0.4, 0.6);
    const double plainMid = regionSharpness(plain, 0.4, 0.6, 0.4, 0.6), plainEdge = regionSharpness(plain, 0.08, 0.26, 0.4, 0.6);
    QVERIFY(plainEdge > 50);
    qInfo("selective blur: middle %.1f (plain %.1f), edge %.1f (plain %.1f)", inMid, plainMid, atEdge, plainEdge);
    QVERIFY(inMid < plainMid * 0.6);
    QVERIFY(std::abs(atEdge - plainEdge) < plainEdge * 0.05 + 1);
    // the effect's mask is the one shown and moved in the player
    QVERIFY(canvas->handlePos("m:center").has_value());
    const QPointF ec = *canvas->handlePos("m:center");
    drag(canvas, ec, ec + QPointF(fr.width() * 0.25, 0));
    QVERIFY(clipOf(doc, clip).effects[0].masks[0].centerX.constantValue() > 0.3);
    QVERIFY(clipOf(doc, clip).masks.empty());   // the clip's own masks are untouched
    auto* invert = w.inspector()->effectEditor()->findChild<QCheckBox*>(QString::fromStdString("fxmaskinvert:" + ref));
    QVERIFY(invert);
    invert->setChecked(true);
    QVERIFY(clipOf(doc, clip).effects[0].masks[0].inverted);
    sel = waitForPreview(w, 60);
    QVERIFY(regionSharpness(sel, 0.08, 0.26, 0.4, 0.6) < plainEdge * 0.6);   // now the outside is blurred
    shot(&w, "m4_11_selective_effect.png");

    // ---- the Adjustment Layer tile dragged onto the timeline: a new layer at the drop time
    w.resourceTabs()->setCurrentWidget(w.effectsPage());
    QApplication::processEvents();
    QListWidgetItem* tile = w.effectsPage()->itemFor("adjustment");
    QVERIFY(tile);
    std::unique_ptr<QMimeData> mime(w.effectsPage()->list()->model()->mimeData({w.effectsPage()->list()->indexFromItem(tile)}));
    QVERIFY(mime && mime->hasFormat(kMimeAdjustment));
    const QRect lane = tl->trackLaneRect(adjTrack->id);
    const QPoint target(tl->clipRect(clip).left() + tl->clipRect(clip).width() * 3 / 4, lane.center().y());
    QDragEnterEvent enter(target, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &enter);
    QDragMoveEvent move(target, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &move);
    QDropEvent drop(QPointF(target), Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &drop);
    const Track* t2 = doc->sequence().findTrack(adjTrack->id);
    QVERIFY(t2);
    int adjustments = 0;
    for (const auto& c : t2->clips) adjustments += c.isAdjustment();
    QVERIFY2(adjustments == 2 || std::count_if(doc->sequence().tracks.begin(), doc->sequence().tracks.end(), [](const Track& t) {
                                     return std::any_of(t.clips.begin(), t.clips.end(), [](const Clip& c) { return c.isAdjustment(); });
                                 }) >= 2,
             "a second adjustment layer");

    // ---- save, reopen, export: preview and export agree (the adjustment layer's filter active again)
    QVERIFY(ed->setEffectEnabled(adj, clipOf(doc, adj).effects[0].id, true));
    const QString file = tmp.filePath("adjust.lcproj");
    QVERIFY(w.saveProjectTo(file));
    MainWindow w2(MainWindow::Options{false, true});
    w2.resize(1600, 1000);
    w2.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w2));
    QVERIFY(w2.openProjectFile(file));
    QVERIFY(w2.document()->sequence() == doc->sequence());
    const double d = previewVsExportOf(w2, tmp.filePath("adjust.mp4"), 60);
    // control: the same frame without effects (resampling the detailed pattern + H.264 alone)
    std::vector<std::pair<std::string, std::string>> fx;
    for (const auto& tr : w2.document()->sequence().tracks)
        for (const auto& c : tr.clips)
            for (const auto& e : c.effects)
                if (e.enabled) fx.push_back({c.id, e.id});
    for (const auto& [cid, eid] : fx) QVERIFY(w2.editController()->setEffectEnabled(cid, eid, false));
    const double control = previewVsExportOf(w2, tmp.filePath("adjust_bypassed.mp4"), 60);
    qInfo("preview vs export with a masked blur and adjustment layers %.2f / 255 (bypassed %.2f)", d, control);
    QVERIFY(d >= 0 && control >= 0 && d < control + 1.5);
}

// The player's before / after split: the left half without effects, colour or adjustment layers, the right half as
// it is. A view mode only: it does not edit the project.
void GuiTests::m4BeforeAfterCompare() {
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({mediaPath("C_testsrc2_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    w.timelinePanel()->view()->zoomToFit();
    const std::string clip = doc->sequence().tracks[0].clips[0].id;
    doc->setSelection({clip});
    w.playback()->seek(60);
    QVERIFY(w.filtersPage()->apply("fa.filter.bw"));
    QVERIFY(regionChroma(waitForPreview(w, 60), 0, 1) < 2);
    auto* button = w.previewPanel()->findChild<QToolButton*>("compareButton");
    QVERIFY(button && button->isCheckable() && !button->toolTip().isEmpty());
    // a frame rendered in the requested mode (the one shown before the switch does not count)
    auto shown = [&](bool compare) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 8000) {
            const QImage img = waitForPreview(w, 60);
            if (!img.isNull() && w.playback()->currentImageCompare() == compare) return img;
            QTest::qWait(10);
        }
        return QImage();
    };
    const uint64_t version = doc->version();
    QTest::mouseClick(button, Qt::LeftButton);
    QVERIFY(button->isChecked() && w.playback()->compare());
    QImage split = shown(true);
    QVERIFY(!split.isNull());
    qInfo("compare, B&W on the clip: chroma left %.1f, right %.1f", regionChroma(split, 0.02, 0.45),
          regionChroma(split, 0.55, 0.98));
    QVERIFY(regionChroma(split, 0.02, 0.45) > 40 && regionChroma(split, 0.55, 0.98) < 2);
    QCOMPARE(doc->version(), version);   // not an edit
    // the same with the filter on an adjustment layer instead (the "before" leaves adjustment layers out)
    QVERIFY(ed->setEffectEnabled(clip, clipOf(doc, clip).effects[0].id, false));
    QVERIFY(regionChroma(shown(true), 0.55, 0.98) > 40);   // nothing grey any more
    QVERIFY(ed->addAdjustmentLayer(Rational(0), Rational(10)));
    QVERIFY(w.filtersPage()->apply("fa.filter.bw"));
    QVERIFY(clipOf(doc, doc->primarySelection()).isAdjustment());
    split = shown(true);
    QVERIFY(regionChroma(split, 0.02, 0.45) > 40 && regionChroma(split, 0.55, 0.98) < 2);
    shot(&w, "m4_12_before_after.png");
    QTest::mouseClick(button, Qt::LeftButton);   // off: the whole picture with the effects
    QVERIFY(!w.playback()->compare());
    QVERIFY(regionChroma(shown(false), 0, 1) < 2);
}

// M5: Speed tab (uniform speed, duration, reverse, pitch, interpolation, reset), Freeze Frame from the Edit menu,
// hold duration, undo, save / reopen, preview vs export - on A_red, whose frame-index strip shows the source frame.
void GuiTests::m5SpeedReverseFreeze() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    w.timelinePanel()->view()->zoomToFit();
    const std::string v = doc->sequence().tracks[0].clips[0].id;
    const std::string a = doc->sequence().tracks[1].clips[0].id;   // its linked audio
    doc->setSelection({v});
    QApplication::processEvents();
    SpeedPanel* sp = w.inspector()->speedPanel();
    QVERIFY(sp);
    selectTab(w.inspector()->clipTabs(), "Speed");
    QVERIFY(sp->isVisible());
    auto* value = sp->findChild<QDoubleSpinBox*>("speedValue");
    auto* duration = sp->findChild<QLineEdit*>("speedDuration");
    auto* reverse = sp->findChild<QCheckBox*>("speedReverse");
    auto* pitch = sp->findChild<QCheckBox*>("speedPitch");
    auto* interp = sp->findChild<QComboBox*>("speedInterpolation");
    auto* reset = sp->findChild<QPushButton*>("speedReset");
    QVERIFY(value && duration && reverse && pitch && interp && reset);
    QCOMPARE(value->value(), 1.0);
    QVERIFY(pitch->isChecked());   // Filmora's default: Maintain Pitch on (EXP-004)

    // 2x: 5 s, the linked audio too; output frame 30 shows source frame 60
    value->setValue(2.0);
    QCOMPARE(clipOf(doc, v).duration, Rational(5));
    QCOMPARE(clipOf(doc, a).duration, Rational(5));
    QCOMPARE(clipOf(doc, a).speed.speed, Rational(2));
    QCOMPARE(duration->text(), QString("00:00:05:00"));
    w.playback()->seek(30);
    QCOMPARE(stripIndex(waitForPreview(w, 30)), 60);
    shot(&w, "m5_01_speed_uniform.png");
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(clipOf(doc, v).duration, Rational(10));
    QCOMPARE(value->value(), 1.0);

    // a duration typed: 20 s -> 0.5x; reverse: the first frame shows the last source frame
    duration->setText("00:00:20:00");
    QMetaObject::invokeMethod(duration, "editingFinished");
    QCOMPARE(clipOf(doc, v).speed.speed, Rational(1, 2));
    QCOMPARE(clipOf(doc, v).duration, Rational(20));
    reverse->setChecked(true);
    QVERIFY(clipOf(doc, v).speed.reverse && clipOf(doc, a).speed.reverse);
    w.playback()->seek(0);
    QCOMPARE(stripIndex(waitForPreview(w, 0)), 299);
    w.playback()->seek(2);
    QCOMPARE(stripIndex(waitForPreview(w, 2)), 298);   // 0.5x: every source frame twice, backwards
    interp->setCurrentIndex(interp->findText("Frame Blending"));
    QCOMPARE(clipOf(doc, v).speed.interpolation, FrameInterpolation::Blending);
    pitch->setChecked(false);
    QVERIFY(!clipOf(doc, a).speed.maintainPitch);
    QTest::mouseClick(reset, Qt::LeftButton);
    QCOMPARE(clipOf(doc, v).speed, ClipSpeed{});
    QCOMPARE(clipOf(doc, v).duration, Rational(10));

    // Freeze Frame at 2 s from the Edit menu: a 5 s hold of frame 60, selected; the rest moves right
    w.playback()->seek(60);
    QApplication::processEvents();
    QAction* freeze = nullptr;
    for (QAction* act : w.findChildren<QAction*>())
        if (act->text() == "Freeze Frame") freeze = act;
    QVERIFY(freeze);
    freeze->trigger();
    const std::string hold = doc->primarySelection();
    QVERIFY(clipOf(doc, hold).speed.freeze);
    QCOMPARE(clipOf(doc, hold).timelineStart, Rational(2));
    QCOMPARE(clipOf(doc, hold).duration, Rational(5));
    QCOMPARE(doc->sequence().tracks[0].clips.size(), size_t(3));
    QCOMPARE(doc->sequence().tracks[0].clips[2].timelineStart, Rational(7));
    QCOMPARE(doc->sequence().tracks[1].clips[1].timelineStart, Rational(7));   // the audio after it moved too
    w.playback()->seek(150);
    QCOMPARE(stripIndex(waitForPreview(w, 150)), 60);
    // the Speed tab shows the hold length; 2 s typed shortens it and pulls the rest back
    selectTab(w.inspector()->clipTabs(), "Speed");   // a newly selected clip opens on its first tab
    QApplication::processEvents();
    auto* holdEdit = sp->findChild<QLineEdit*>("holdDuration");
    QVERIFY(holdEdit && holdEdit->isVisible());
    QCOMPARE(holdEdit->text(), QString("00:00:05:00"));
    holdEdit->setText("2");
    QMetaObject::invokeMethod(holdEdit, "editingFinished");
    QCOMPARE(clipOf(doc, hold).duration, Rational(2));
    QCOMPARE(doc->sequence().tracks[0].clips[2].timelineStart, Rational(4));
    w.playback()->seek(120);
    QCOMPARE(stripIndex(waitForPreview(w, 120)), 60);   // after the hold: the clip resumes at frame 60
    w.playback()->seek(121);
    QCOMPARE(stripIndex(waitForPreview(w, 121)), 61);
    shot(&w, "m5_02_freeze_frame.png");

    // save, reopen, export: the speed settings survive and the export shows the same frames
    doc->setSelection({doc->sequence().tracks[0].clips[2].id});
    QApplication::processEvents();
    value->setValue(2.0);   // the part after the hold at 2x
    const QString file = tmp.filePath("speed.lcproj");
    QVERIFY(w.saveProjectTo(file));
    MainWindow w2(MainWindow::Options{false, true});
    w2.resize(1600, 1000);
    w2.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w2));
    QVERIFY(w2.openProjectFile(file));
    QVERIFY(w2.document()->sequence() == doc->sequence());
    for (int64_t f : {int64_t(80), int64_t(140)}) {
        const double d = previewVsExportOf(w2, tmp.filePath("speed.mp4"), f);
        qInfo("speed project: preview vs export at frame %lld: %.2f / 255", static_cast<long long>(f), d);
        QVERIFY(d >= 0 && d < 3.0);
    }
}

// M5: Speed Ramping - Bullet preset (Filmora's measured key points), the graph by mouse (add, drag: one undo step
// each), a key typed and deleted, a user preset saved and applied to another clip, optical-flow interpolation,
// reset, save / reopen and preview vs export.
void GuiTests::m5SpeedRamping() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    qputenv("MEERCUT_SPEED_PRESETS", tmp.filePath("speed_presets.json").toUtf8());
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4"), mediaPath("B_blue_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    QVERIFY(ed->addToTimeline(ids[1], Rational(10)));
    w.timelinePanel()->view()->zoomToFit();
    const std::string a = doc->sequence().tracks[0].clips[0].id, b = doc->sequence().tracks[0].clips[1].id;
    doc->setSelection({a});
    QApplication::processEvents();
    SpeedPanel* sp = w.inspector()->speedPanel();
    selectTab(w.inspector()->clipTabs(), "Speed");
    selectTab(sp->tabs(), "Speed Ramping");
    auto* preset = sp->findChild<QComboBox*>("rampPreset");
    RampGraph* graph = sp->rampGraph();
    auto* keySpeed = sp->findChild<QDoubleSpinBox*>("rampKeySpeed");
    auto* keyDelete = sp->findChild<QPushButton*>("rampDeleteKey");
    auto* reset = sp->findChild<QPushButton*>("rampReset");
    QVERIFY(preset && graph && keySpeed && keyDelete && reset && graph->isVisible());

    // Bullet: 107 frames; output frame 30 is in the slow-motion middle (Filmora's export: source frame 138)
    const int bi = preset->findText("Bullet");
    QVERIFY(bi >= 0);
    preset->setCurrentIndex(bi);
    emit preset->activated(bi);
    QCOMPARE(clipOf(doc, a).speed.ramp.size(), size_t(6));
    QCOMPARE(clipOf(doc, a).speed.rampPreset, std::string("bullet"));
    QCOMPARE(clipOf(doc, a).duration, Rational(107, 30));
    QCOMPARE(clipOf(doc, b).timelineStart, Rational(107, 30));   // B rippled
    w.playback()->seek(30);
    QCOMPARE(stripIndex(waitForPreview(w, 30)), 138);
    shot(&w, "m5_03_speed_ramping.png");

    // a click on the curve adds a point there (nothing changes yet), dragging it up speeds that part up
    const QPointF onCurve = graph->toWidget(0.2, 5.2);   // the first segment runs at 5.2x
    const QPointF up = graph->toWidget(0.2, 1.0);
    QTest::mousePress(graph, Qt::LeftButton, {}, onCurve.toPoint());
    QTest::mouseMove(graph, up.toPoint());
    QTest::mouseRelease(graph, Qt::LeftButton, {}, up.toPoint());
    QCOMPARE(clipOf(doc, a).speed.ramp.size(), size_t(7));
    QVERIFY2(std::abs(clipOf(doc, a).speed.ramp[1].speed - 1.0) < 0.05,
             qPrintable(QString("speed %1, graph %2 x %3").arg(clipOf(doc, a).speed.ramp[1].speed).arg(graph->width()).arg(graph->height())));
    QVERIFY(clipOf(doc, a).speed.rampPreset.empty());   // edited by hand
    QVERIFY(clipOf(doc, a).duration > Rational(107, 30));   // slower at the start: longer
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);   // one undo step for the whole gesture
    QCOMPARE(clipOf(doc, a).speed.ramp.size(), size_t(6));
    QCOMPARE(clipOf(doc, a).duration, Rational(107, 30));

    // the slow key typed to 1x, then deleted
    graph->setSelected(2);
    QApplication::processEvents();
    keySpeed->setValue(1.0);
    QVERIFY(std::abs(clipOf(doc, a).speed.ramp[2].speed - 1.0) < 1e-9);
    graph->setSelected(2);
    QApplication::processEvents();
    QTest::mouseClick(keyDelete, Qt::LeftButton);
    QCOMPARE(clipOf(doc, a).speed.ramp.size(), size_t(5));

    // saved as "My Ramp", applied to clip B
    QVERIFY(sp->saveRampPreset("My Ramp"));
    QVERIFY(QFileInfo::exists(tmp.filePath("speed_presets.json")));
    QVERIFY(preset->findText("My Presets: My Ramp") >= 0);
    doc->setSelection({b});
    QApplication::processEvents();
    const int mi = preset->findText("My Presets: My Ramp");
    preset->setCurrentIndex(mi);
    emit preset->activated(mi);
    QCOMPARE(clipOf(doc, b).speed.ramp.size(), size_t(5));
    QVERIFY(std::abs(clipOf(doc, b).duration.toDouble() - clipOf(doc, a).duration.toDouble()) < 2.0 / 30);   // same shape

    // optical-flow interpolation on the ramped clip renders (slow parts get in-between frames)
    auto* interp = sp->findChild<QComboBox*>("speedInterpolation");
    interp->setCurrentIndex(interp->findText("Optical Flow (Fast)"));
    QCOMPARE(clipOf(doc, b).speed.interpolation, FrameInterpolation::OpticalFlow);
    const int64_t bf = (clipOf(doc, b).timelineStart * Rational(30)).floor() + 40;
    w.playback()->seek(bf);
    QVERIFY(!waitForPreview(w, bf).isNull());

    // save, reopen, export
    const QString file = tmp.filePath("ramp.lcproj");
    QVERIFY(w.saveProjectTo(file));
    MainWindow w2(MainWindow::Options{false, true});
    w2.resize(1600, 1000);
    w2.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w2));
    QVERIFY(w2.openProjectFile(file));
    QVERIFY(w2.document()->sequence() == doc->sequence());
    for (int64_t f : {int64_t(30), bf}) {
        const double d = previewVsExportOf(w2, tmp.filePath("ramp.mp4"), f);
        qInfo("ramp project: preview vs export at frame %lld: %.2f / 255", static_cast<long long>(f), d);
        QVERIFY(d >= 0 && d < 3.0);
    }

    // Reset: back to normal speed
    doc->setSelection({a});
    QApplication::processEvents();
    QTest::mouseClick(reset, Qt::LeftButton);
    QVERIFY(clipOf(doc, a).speed.ramp.empty());
    QCOMPARE(clipOf(doc, a).duration, Rational(10));
    qunsetenv("MEERCUT_SPEED_PRESETS");
}

namespace {

// tools/make_m5_examples.py takes the analysed projects of the M5 GUI tests from FA_EXAMPLES_DIR (when it is set).
void keepForExamples(const QString& projectFile, const QString& name) {
    const QString dir = qEnvironmentVariable("FA_EXAMPLES_DIR");
    if (dir.isEmpty()) return;
    QDir().mkpath(dir);
    QFile::remove(QDir(dir).filePath(name));
    QFile::copy(projectFile, QDir(dir).filePath(name));
}

}  // namespace

namespace {

// EXP-013 ground truth (tools/make_motion_tests.py): the 160 px patch centre, integer-placed.
QPointF trackTruth(int n) {
    const double t = n / 30.0;
    const double cx = 960 + 140 * t, cy = 540 + 120 * std::sin(2 * std::numbers::pi * t / 5);
    return {std::round(cx - 80) + 80, std::round(cy - 80) + 80};
}

// Mean absolute horizontal + vertical step inside a rectangle (sequence pixels mapped onto the image).
double detailIn(const QImage& img, const QRectF& seqRect) {
    const QImage x = img.convertToFormat(QImage::Format_RGBA8888);
    const double k = double(x.width()) / 1920;
    const int x0 = std::max(1, int(seqRect.left() * k)), x1 = std::min(x.width() - 1, int(seqRect.right() * k));
    const int y0 = std::max(1, int(seqRect.top() * k)), y1 = std::min(x.height() - 1, int(seqRect.bottom() * k));
    double sum = 0;
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int xx = x0; xx < x1; ++xx) {
            const QColor c = x.pixelColor(xx, y), l = x.pixelColor(xx - 1, y), u = x.pixelColor(xx, y - 1);
            sum += std::abs(c.green() - l.green()) + std::abs(c.green() - u.green());
            ++n;
        }
    return n ? sum / n : 0.0;
}

}  // namespace

// Video > Tracking on the EXP-013 clip: the box is placed with the mouse on the moving checker patch (on frame 75, so
// the analysis runs forward and backward), tracked in the background with progress and Cancel; then a title follows
// the patch and a mosaic covers it. Everything survives save / reopen and the export matches the preview; replacing
// the media file marks the tracking data as stale.
void GuiTests::m5MotionTracking() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString clipFile = tmp.filePath("L_track.mp4");
    QVERIFY(QFile::copy(mediaPath("L_track_1080p30_5s.mp4"), clipFile));
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({clipFile});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    QCOMPARE(doc->sequence().width, 1920);
    w.timelinePanel()->view()->zoomToFit();
    const std::string a = doc->sequence().tracks[0].clips[0].id;
    doc->setSelection({a});
    w.playback()->seek(75);
    QVERIFY(!waitForPreview(w, 75).isNull());
    InspectorPanel* insp = w.inspector();
    insp->showVideoSubTab("tracking");
    QApplication::processEvents();
    TrackingPanel* tp = insp->trackingPanel();
    auto* enable = tp->findChild<QCheckBox*>("trackingEnable");
    auto* start = tp->findChild<QPushButton*>("trackingStart");
    auto* status = tp->findChild<QLabel*>("trackingStatus");
    auto* linkButton = tp->findChild<QPushButton*>("trackingLinkButton");
    QVERIFY(enable && start && status && linkButton);
    QVERIFY(enable->isVisible());
    QCOMPARE(insp->videoSubTab(), QString("tracking"));
    PreviewCanvas* canvas = w.previewPanel()->canvas();
    QVERIFY(!canvas->handlePos("k:tl"));   // no box until Motion Tracking is on
    const size_t undoBefore = doc->undoStack().undoCount();
    QTest::mouseClick(enable, Qt::LeftButton, {}, QPoint(9, enable->height() / 2));   // on the indicator
    QVERIFY(enable->isChecked());
    QVERIFY(doc->trackingBox().has_value());
    QVERIFY(canvas->handlePos("k:tl") && canvas->handlePos("k:br"));
    QVERIFY(!canvas->handlePos("rotate"));   // the transform handles make way for the box

    // put the box on the patch with the mouse: move it, then drag two corners (frame 75: centre 1310, 540)
    const QRectF fr = canvas->frameRect();
    auto at = [&](double x, double y) { return QPointF(fr.left() + x / 1920 * fr.width(), fr.top() + y / 1080 * fr.height()); };
    const QPointF goal = trackTruth(75);
    drag(canvas, *canvas->handlePos("k:center") + QPointF(3, 3), at(1250, 560) + QPointF(3, 3));   // grabbed inside
    QVERIFY(std::abs(doc->trackingBox()->x * 1920 - 1250) < 4);
    drag(canvas, *canvas->handlePos("k:tl"), at(goal.x() - 80, goal.y() - 80));
    drag(canvas, *canvas->handlePos("k:br"), at(goal.x() + 80, goal.y() + 80));
    {
        const auto& b = *doc->trackingBox();
        qInfo("tracking box placed at %.1f, %.1f size %.1f x %.1f px", b.x * 1920, b.y * 1080, b.w * 1920, b.h * 1080);
        QVERIFY(std::abs(b.x * 1920 - goal.x()) < 5 && std::abs(b.y * 1080 - goal.y()) < 5);
        QVERIFY(std::abs(b.w * 1920 - 160) < 8 && std::abs(b.h * 1080 - 160) < 8);
    }
    QCOMPARE(doc->undoStack().undoCount(), undoBefore);   // placing the box is not an edit
    shot(&w, "m5_04_tracking_box.png");

    // Start, then Cancel: nothing is stored
    QTest::mouseClick(start, Qt::LeftButton);
    QVERIFY(w.analysis()->busy());
    QCOMPARE(start->text(), QString("Cancel"));
    QTest::mouseClick(start, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!w.analysis()->busy(), 20000);
    QApplication::processEvents();
    QVERIFY(!clipOf(doc, a).tracking);
    QCOMPARE(start->text(), QString("Start Tracking"));

    // the whole clip, forward and backward from frame 75
    QSignalSpy progress(w.analysis(), &AnalysisService::progress);
    QElapsedTimer clock;
    clock.start();
    QVERIFY(tp->startTracking());
    QTRY_VERIFY_WITH_TIMEOUT(clipOf(doc, a).tracking.has_value(), 180000);
    const double seconds = clock.elapsed() / 1000.0;
    QVERIFY(progress.count() >= 10);
    {
        const MotionTrack& t = *clipOf(doc, a).tracking;
        QCOMPARE(t.samples.size(), size_t(150));
        QCOMPARE(t.firstFrame, int64_t(75));
        double sq = 0, worst = 0, conf = 1;
        int lost = 0;
        for (const TrackingSample& s : t.samples) {
            const QPointF g = trackTruth(static_cast<int>(s.frame));
            const double e = std::hypot(s.x * 1920 - g.x(), s.y * 1080 - g.y());
            sq += e * e;
            worst = std::max(worst, e);
            conf = std::min(conf, s.confidence);
            lost += s.lost;
        }
        const double rms = std::sqrt(sq / t.samples.size());
        qInfo("GUI tracking (960x540 analysis): %.2f s, RMS %.2f px, max %.2f px, lowest confidence %.2f, lost %d "
              "(Filmora: RMS 7.7, max 13.6 px)", seconds, rms, worst, conf, lost);
        QVERIFY(rms < 7.7);
        QVERIFY(worst < 13.6);
        QCOMPARE(lost, 0);
    }
    QVERIFY2(status->text().contains("Tracked 150 frames"), qPrintable(status->text()));
    QCOMPARE(start->text(), QString("Track Again"));
    QVERIFY(linkButton->isEnabled());
    shot(&w, "m5_05_tracking_result.png");

    // one undo step
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QVERIFY(!clipOf(doc, a).tracking);
    QTest::keyClick(&w, Qt::Key_Y, Qt::ControlModifier);
    QVERIFY(clipOf(doc, a).tracking.has_value());

    // a title 324 px below the patch follows it (its offset is kept)
    QVERIFY(ed->addTitleFromTemplate("basic.plain", Rational(0)));
    const std::string title = doc->primarySelection();
    QVERIFY(ed->modifyClip(title, "Place", [](Clip& c) { c.transform.positionY = Animatable<double>(0.3); }));
    doc->setSelection({a});
    QApplication::processEvents();
    QComboBox* link = tp->linkCombo();
    const int ti = link->findData(QString::fromStdString(title));
    QVERIFY(ti >= 0);
    link->setCurrentIndex(ti);
    QTest::mouseClick(linkButton, Qt::LeftButton);
    {
        const Clip& tc = clipOf(doc, title);
        QVERIFY(tc.transform.positionX.keys().size() >= 149);
        const Time t100 = tc.sourceTimeAt(frameToTime(100, FrameRate::fps30()));
        const QPointF g = trackTruth(100), g0 = trackTruth(0);
        const double ex = (tc.transform.positionX.value(t100) - (g.x() - g0.x()) / 1920) * 1920;
        const double ey = (tc.transform.positionY.value(t100) - 0.3 - (g.y() - g0.y()) / 1080) * 1080;
        qInfo("title follows the patch: error at frame 100 %.1f, %.1f px", ex, ey);
        QVERIFY(std::abs(ex) < 8 && std::abs(ey) < 8);
    }

    // a mosaic covers the patch: the checker detail inside goes away, the background elsewhere stays
    w.playback()->seek(100);
    const QImage before = waitForPreview(w, 100);
    const QPointF g100 = trackTruth(100);
    const QRectF on(g100.x() - 60, g100.y() - 60, 120, 120), off(200, 100, 200, 200);
    doc->setSelection({a});
    QApplication::processEvents();
    link->setCurrentIndex(link->findData(QString("mosaic")));
    QTest::mouseClick(linkButton, Qt::LeftButton);
    const std::string mosaic = doc->primarySelection();
    QVERIFY(mosaic != a && clipOf(doc, mosaic).isAdjustment());
    QCOMPARE(clipOf(doc, mosaic).effects.size(), size_t(1));
    QCOMPARE(clipOf(doc, mosaic).effects[0].type, std::string("fa.fx.pixelate"));
    {   // EXP-013 like for like: Filmora was measured on its linked Mosaic's keyframes (tools/analyze_track.py)
        const Mask& mk = clipOf(doc, mosaic).masks.at(0);
        QCOMPARE(mk.centerX.keys().size(), size_t(150));
        double sq = 0, worst = 0;
        for (size_t i = 0; i < mk.centerX.keys().size(); ++i) {
            const auto& kx = mk.centerX.keys()[i];
            const int n = static_cast<int>((kx.time * Rational(30)).floor());
            const QPointF g = trackTruth(n);
            const double e = std::hypot(kx.value * 1080 + 960 - g.x(), mk.centerY.keys()[i].value * 1080 + 540 - g.y());
            sq += e * e;
            worst = std::max(worst, e);
        }
        const double rms = std::sqrt(sq / 150);
        qInfo("EXP-013 mosaic keyframes vs ground truth: RMS %.2f px, max %.2f px (Filmora 16: 7.7 / 13.6)", rms, worst);
        QVERIFY(rms < 7.7 && worst < 13.6);
    }
    w.playback()->seek(99);
    waitForPreview(w, 99);
    w.playback()->seek(100);
    const QImage after = waitForPreview(w, 100);
    const double dOnBefore = detailIn(before, on), dOnAfter = detailIn(after, on);
    const double dOffBefore = detailIn(before, off), dOffAfter = detailIn(after, off);
    qInfo("mosaic: detail on the patch %.2f -> %.2f, background %.2f -> %.2f", dOnBefore, dOnAfter, dOffBefore, dOffAfter);
    QVERIFY(dOnAfter < 0.5 * dOnBefore);
    QVERIFY(std::abs(dOffAfter - dOffBefore) < 0.1 * dOffBefore + 0.2);
    shot(&w, "m5_06_tracking_mosaic.png");

    // save, reopen, export
    const QString file = tmp.filePath("tracking.lcproj");
    QVERIFY(w.saveProjectTo(file));
    keepForExamples(file, "tracking.lcproj");
    {
        MainWindow w2(MainWindow::Options{false, true});
        w2.resize(1600, 1000);
        w2.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w2));
        QVERIFY(w2.openProjectFile(file));
        QVERIFY(w2.document()->sequence() == doc->sequence());
        QVERIFY(clipOf(w2.document(), a).tracking == clipOf(doc, a).tracking);
        for (int64_t f : {int64_t(20), int64_t(100)}) {
            const double d = previewVsExportOf(w2, tmp.filePath("tracking.mp4"), f);
            qInfo("tracking project: preview vs export at frame %lld: %.2f / 255", static_cast<long long>(f), d);
            QVERIFY(d >= 0 && d < 3.0);
        }
    }

    // the media file changes: the tracking data is shown as stale and cannot be linked any more
    {
        QFile f(clipFile);
        QVERIFY(f.open(QIODevice::Append));
        f.write(QByteArray(64, '\0'));
    }
    doc->setSelection({a});
    QApplication::processEvents();
    tp->refresh();
    QVERIFY2(status->text().contains("media changed"), qPrintable(status->text()));
    QVERIFY(!linkButton->isEnabled());
    QCOMPARE(start->text(), QString("Track Again"));
}

namespace {

// Red marker centroid in a preview image, in 1920-wide sequence pixels (NaN when it is not visible).
QPointF redMarker(const QImage& img) {
    const QImage x = img.convertToFormat(QImage::Format_RGBA8888);
    const double kx = 1920.0 / x.width(), ky = 1080.0 / x.height();
    double sx = 0, sy = 0;
    int n = 0;
    for (int y = 0; y < x.height(); ++y) {
        const uchar* p = x.constScanLine(y);
        for (int xx = 0; xx < x.width(); ++xx)
            if (p[xx * 4] > 170 && p[xx * 4 + 1] < 80 && p[xx * 4 + 2] < 80) {
                sx += xx;
                sy += y;
                ++n;
            }
    }
    // pixel i covers [i, i + 1): scale its centre
    return n >= 20 ? QPointF((sx / n + 0.5) * kx - 0.5, (sy / n + 0.5) * ky - 0.5) : QPointF(qQNaN(), qQNaN());
}

double regionDiff(const QImage& a, const QImage& b, double x0, double x1) {
    const QRect r(int(x0 * a.width()), 0, int((x1 - x0) * a.width()), a.height());
    return meanAbsDiff(a.copy(r), b.scaled(a.size()).copy(r));
}

}  // namespace

// Video > Stabilization on the EXP-014 clip: turning it on analyses in the background (Cancel stores nothing); the
// picture is steadier in the player; Smoothness / Strength / Auto Zoom re-plan from the stored motion (one undo step
// each, no new analysis); Compare shows the shaky original on the left; save / reopen / export; a changed media file
// marks the analysis as stale; turning it off removes it.
void GuiTests::m5Stabilization() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString clipFile = tmp.filePath("M_shaky.mp4");
    QVERIFY(QFile::copy(mediaPath("M_shaky_1080p30_5s.mp4"), clipFile));
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({clipFile});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    w.timelinePanel()->view()->zoomToFit();
    const std::string a = doc->sequence().tracks[0].clips[0].id;
    doc->setSelection({a});
    QApplication::processEvents();
    InspectorPanel* insp = w.inspector();
    insp->showVideoSubTab("stabilization");
    QApplication::processEvents();
    StabilizationPanel* sp = insp->stabilizationPanel();
    auto* enable = sp->findChild<QCheckBox*>("stabEnable");
    auto* level = sp->findChild<QComboBox*>("stabLevel");
    auto* strength = sp->findChild<QSlider*>("stabStrength");
    auto* autoZoom = sp->findChild<QCheckBox*>("stabAutoZoom");
    auto* analyze = sp->findChild<QPushButton*>("stabAnalyze");
    auto* status = sp->findChild<QLabel*>("stabStatus");
    QVERIFY(enable && level && strength && autoZoom && analyze && status);
    QVERIFY(enable->isVisible() && !enable->isChecked());
    QCOMPARE(insp->videoSubTab(), QString("stabilization"));

    auto markerAt = [&](int64_t f) {
        w.playback()->seek(f);
        return redMarker(waitForPreview(w, f));
    };
    auto rmsStep = [&](int64_t from, int64_t to) {
        double sq = 0;
        int n = 0;
        QPointF prev = markerAt(from);
        for (int64_t f = from + 1; f <= to; ++f) {
            const QPointF p = markerAt(f);
            if (!qIsNaN(p.x()) && !qIsNaN(prev.x())) {
                sq += std::pow(p.x() - prev.x(), 2) + std::pow(p.y() - prev.y(), 2);
                ++n;
            }
            prev = p;
        }
        return n ? std::sqrt(sq / n) : -1.0;
    };
    const double shaky = rmsStep(0, 40);
    w.playback()->seek(17);
    const QImage original17 = waitForPreview(w, 17);

    // on: the analysis starts; Cancel stops it and stores nothing
    QTest::mouseClick(enable, Qt::LeftButton, {}, QPoint(9, enable->height() / 2));
    QVERIFY(w.analysis()->busy());
    QCOMPARE(analyze->text(), QString("Cancel"));
    QTest::mouseClick(analyze, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!w.analysis()->busy(), 20000);
    QApplication::processEvents();
    QVERIFY(!clipOf(doc, a).stabilization);
    QVERIFY(!enable->isChecked());

    // Strong chosen before the analysis is what the analysis uses
    level->setCurrentIndex(level->findText("Strong"));
    QVERIFY(!clipOf(doc, a).stabilization);
    QSignalSpy progress(w.analysis(), &AnalysisService::progress);
    QElapsedTimer clock;
    clock.start();
    QTest::mouseClick(enable, Qt::LeftButton, {}, QPoint(9, enable->height() / 2));
    QTRY_VERIFY_WITH_TIMEOUT(clipOf(doc, a).stabilization.has_value(), 180000);
    const double seconds = clock.elapsed() / 1000.0;
    QVERIFY(progress.count() >= 10);
    {
        const Stabilization& st = *clipOf(doc, a).stabilization;
        QCOMPARE(st.level, StabilizeLevel::Strong);
        QCOMPARE(st.motion.size(), size_t(150));
        QVERIFY(st.zoom > 1.0 && st.zoom <= kMaxStabilizeZoom);
        qInfo("stabilisation analysis of 150 frames: %.2f s, zoom %.3f", seconds, st.zoom);
    }
    QVERIFY(enable->isChecked());
    QVERIFY2(status->text().startsWith("Analysed 150 frames"), qPrintable(status->text()));
    const double steady = rmsStep(0, 40);
    qInfo("player, marker RMS step over frames 0-40: %.2f px shaky -> %.2f px Strong", shaky, steady);
    QVERIFY(shaky > 10);
    QVERIFY(steady < 0.2 * shaky);
    w.playback()->seek(17);
    const QImage stable17 = waitForPreview(w, 17);
    shot(&w, "m5_07_stabilization.png");

    // settings: re-planned at once from the stored motion, one undo step each
    const std::vector<CameraMotion> motion = clipOf(doc, a).stabilization->motion;
    const size_t undo0 = doc->undoStack().undoCount();
    level->setCurrentIndex(level->findText("Weak"));
    QCOMPARE(clipOf(doc, a).stabilization->level, StabilizeLevel::Weak);
    QVERIFY(!w.analysis()->busy());
    strength->setSliderDown(true);   // a drag: one step
    strength->setValue(70);
    strength->setValue(40);
    strength->setSliderDown(false);
    QCOMPARE(clipOf(doc, a).stabilization->strength, 0.4);
    QTest::mouseClick(autoZoom, Qt::LeftButton, {}, QPoint(9, autoZoom->height() / 2));
    QCOMPARE(clipOf(doc, a).stabilization->zoom, 1.0);
    QCOMPARE(doc->undoStack().undoCount(), undo0 + 3);
    QVERIFY(clipOf(doc, a).stabilization->motion == motion);   // no new analysis
    for (int i = 0; i < 3; ++i) QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(clipOf(doc, a).stabilization->level, StabilizeLevel::Strong);
    QCOMPARE(clipOf(doc, a).stabilization->strength, 1.0);
    QVERIFY(clipOf(doc, a).stabilization->autoZoom);
    QCOMPARE(level->currentText(), QString("Strong"));   // the panel follows undo
    QCOMPARE(strength->value(), 100);
    // Max Zoom: less crop - the corrected path is kept inside the frame (one undo step)
    auto* maxZoom = sp->findChild<QSpinBox*>("stabMaxZoom");
    QVERIFY(maxZoom && maxZoom->isEnabled());
    QCOMPARE(maxZoom->value(), 110);
    maxZoom->setValue(103);
    QVERIFY(clipOf(doc, a).stabilization->zoom <= 1.03 + 1e-9);
    QVERIFY(clipOf(doc, a).stabilization->limitedShare > 0);
    QVERIFY2(status->text().contains("Max Zoom"), qPrintable(status->text()));
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(clipOf(doc, a).stabilization->maxZoom, 1.10);
    QCOMPARE(maxZoom->value(), 110);

    // Compare: the left half is the shaky original, the right half the stabilised picture
    auto* compare = w.previewPanel()->findChild<QToolButton*>("compareButton");
    QVERIFY(compare);
    QTest::mouseClick(compare, Qt::LeftButton);
    QImage split;
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 8000 && split.isNull()) {
            const QImage img = waitForPreview(w, 17);
            if (!img.isNull() && w.playback()->currentImageCompare()) split = img;
            else QTest::qWait(10);
        }
    }
    QVERIFY(!split.isNull());
    const double leftVsOriginal = regionDiff(split, original17, 0.05, 0.4), leftVsStable = regionDiff(split, stable17, 0.05, 0.4);
    const double rightVsStable = regionDiff(split, stable17, 0.6, 0.95);
    qInfo("compare at frame 17: left vs original %.2f, left vs stabilised %.2f, right vs stabilised %.2f", leftVsOriginal,
          leftVsStable, rightVsStable);
    QVERIFY(leftVsOriginal < 2.0 && rightVsStable < 2.0 && leftVsStable > 8.0);
    QTest::mouseClick(compare, Qt::LeftButton);
    QVERIFY(!w.playback()->compare());

    // save, reopen (the plan is derived again on load), export
    const QString file = tmp.filePath("stab.lcproj");
    QVERIFY(w.saveProjectTo(file));
    keepForExamples(file, "stab.lcproj");
    {
        MainWindow w2(MainWindow::Options{false, true});
        w2.resize(1600, 1000);
        w2.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w2));
        QVERIFY(w2.openProjectFile(file));
        QVERIFY(w2.document()->sequence() == doc->sequence());
        // The fine noise and 1-px grid of this clip differ between the downscaled preview and the encoded export even
        // without stabilisation (measured ~6.4 /255; the 3.0 used elsewhere is for smooth test patterns), so the check
        // is relative to the same clip unstabilised. Geometry: stabilisation must move the marker by the same amount
        // in the preview and the export (the difference cancels the preview's own sampling bias); the tolerance is one
        // preview pixel, the preview's resolution.
        double stab[2], plain[2];
        QPointF previewStab[2], exportStab[2];
        const int64_t frames[2] = {17, 100};
        auto exportMarker = [&](const QString& file, int64_t f) {
            VideoDecoder vd(p8(file));
            const VideoFrame& ef = vd.frameAt(frameToTime(f, FrameRate::fps30()));
            return redMarker(QImage(ef.image.pixels.data(), ef.image.width, ef.image.height, ef.image.stride(),
                                    QImage::Format_RGBA8888));
        };
        for (int i = 0; i < 2; ++i) {
            stab[i] = previewVsExportOf(w2, tmp.filePath("stab.mp4"), frames[i]);
            previewStab[i] = redMarker(waitForPreview(w2, frames[i]));
            exportStab[i] = exportMarker(tmp.filePath("stab.mp4"), frames[i]);
        }
        QVERIFY(w2.editController()->setStabilization(a, std::nullopt));
        for (int i = 0; i < 2; ++i) {
            plain[i] = previewVsExportOf(w2, tmp.filePath("plain.mp4"), frames[i]);
            const QImage pv = waitForPreview(w2, frames[i]);
            const QPointF movedPreview = previewStab[i] - redMarker(pv);
            const QPointF movedExport = exportStab[i] - exportMarker(tmp.filePath("plain.mp4"), frames[i]);
            const double previewPixel = 1920.0 / pv.width();
            const double gap = QLineF(movedPreview, movedExport).length();
            qInfo("frame %lld: preview vs export %.2f /255 stabilised, %.2f unstabilised; stabilisation moved the marker "
                  "%.1f,%.1f px in the preview, %.1f,%.1f px in the export (gap %.2f px, preview pixel %.2f px)",
                  static_cast<long long>(frames[i]), stab[i], plain[i], movedPreview.x(), movedPreview.y(), movedExport.x(),
                  movedExport.y(), gap, previewPixel);
            QVERIFY(stab[i] >= 0 && plain[i] >= 0);
            QVERIFY(stab[i] < plain[i] + 0.5);
            QVERIFY(QLineF(QPointF(), movedExport).length() > 5);   // the frames chosen do move
            QVERIFY(gap < previewPixel);
        }
    }

    // the media file changes: stale
    {
        QFile f(clipFile);
        QVERIFY(f.open(QIODevice::Append));
        f.write(QByteArray(64, '\0'));
    }
    sp->refresh();
    QVERIFY2(status->text().contains("media changed"), qPrintable(status->text()));
    QCOMPARE(analyze->text(), QString("Analyze Again"));

    // off removes it (undoable)
    QTest::mouseClick(enable, Qt::LeftButton, {}, QPoint(9, enable->height() / 2));
    QVERIFY(!clipOf(doc, a).stabilization);
    QVERIFY(doc->undo());   // (the second window was the active one: undo directly rather than by shortcut)
    QVERIFY(clipOf(doc, a).stabilization.has_value());
    QVERIFY(enable->isChecked());
}

// Playback > Render Preview: renders the timeline in the background (progress, Cancel), marks the rendered frames
// under the ruler; the player then shows cached frames, identical to rendering them; an edit unmarks exactly the
// frames it changes; Clear Render Preview Files empties the cache; the export matches the cached preview.
void GuiTests::m5RenderPreview() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    qputenv("MEERCUT_RENDER_CACHE", tmp.filePath("cache").toUtf8());
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4"), mediaPath("C_testsrc2_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    QVERIFY(ed->addToTimeline(ids[1], Rational(10)));
    w.timelinePanel()->view()->zoomToFit();
    const std::string c = doc->sequence().tracks[0].clips[1].id;
    RenderPreviewController* rp = w.renderPreview();
    QVERIFY(rp);
    QVERIFY(rp->renderedRanges().empty());
    QVERIFY(w.timelinePanel()->view()->renderedRanges().empty());
    w.playback()->seek(400);
    QVERIFY(!waitForPreview(w, 400).isNull());
    QAction* render = w.action("renderPreview");
    QVERIFY(render && w.action("clearRenderCache"));

    // Cancel part-way: stops, keeps what was done (marked under the ruler from the start)
    render->trigger();
    QVERIFY(rp->busy());
    QCOMPARE(render->text(), QString("Cancel Render Preview"));
    QTRY_VERIFY_WITH_TIMEOUT(rp->cache().frameCount() >= 30, 60000);
    render->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!rp->busy(), 30000);
    QApplication::processEvents();
    const size_t partial = rp->cache().frameCount();
    qInfo("cancelled after %zu frames", partial);
    QVERIFY(partial >= 30 && partial < 600);
    QCOMPARE(render->text(), QString("Render Preview"));
    QTRY_VERIFY_WITH_TIMEOUT(!rp->renderedRanges().empty(), 5000);
    QCOMPARE(rp->renderedRanges().front().first, int64_t(0));
    QCOMPARE(static_cast<size_t>(rp->renderedRanges().front().second), partial);

    // the whole timeline
    QSignalSpy finished(rp, &RenderPreviewController::finished);
    QSignalSpy progress(rp, &RenderPreviewController::progress);
    QElapsedTimer clock;
    clock.start();
    render->trigger();
    QVERIFY(rp->busy());
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 300000);
    const double seconds = clock.elapsed() / 1000.0;
    QVERIFY(finished.at(0).at(0).toBool());
    QVERIFY(progress.count() >= 10);
    qInfo("Render Preview of 600 frames: %.1f s, %.0f MB on disk: %s", seconds, rp->cache().sizeBytes() / 1048576.0,
          qPrintable(finished.at(0).at(1).toString()));
    QApplication::processEvents();
    using Runs = std::vector<std::pair<int64_t, int64_t>>;
    QCOMPARE(rp->renderedRanges(), Runs({{0, 600}}));
    QCOMPARE(w.timelinePanel()->view()->renderedRanges(), rp->renderedRanges());
    shot(&w, "m5_08_render_preview.png");

    // the player shows the cached frames, identical to rendering them (reference: a separate renderer without the
    // cache, configured like the player; frame 450 was never shown, so it is not in the player's memory cache)
    PreviewRenderer& player = w.playback()->renderer();
    int mw = 0, mh = 0;
    player.maxSize(mw, mh);
    PreviewRenderer reference(mw, mh, 4);
    reference.setBackend(player.backendChoice());
    reference.setHardwareDecoding(player.hardwareDecoding());
    reference.setSnapshot(doc->snapshot());
    const PreviewFrame r450 = reference.renderNow(450);
    QCOMPARE(QString::fromStdString(reference.backendName()), QString::fromStdString(player.backendName()));
    const uint64_t hits = player.renderCacheHits();
    w.playback()->seek(450);
    const QImage cached450 = waitForPreview(w, 450);
    QVERIFY(player.renderCacheHits() > hits);
    const QImage ref450(r450.image->pixels.data(), r450.image->width, r450.image->height, r450.image->stride(),
                        QImage::Format_RGBA8888);
    QCOMPARE(meanAbsDiff(cached450, ref450), 0.0);

    // an edit to the second clip unmarks its frames only
    QVERIFY(ed->modifyClip(c, "Opacity", [](Clip& cl) { cl.opacity.setConstant(0.5); }));
    QTRY_VERIFY_WITH_TIMEOUT(rp->renderedRanges() != Runs({{0, 600}}), 5000);
    const Runs after = rp->renderedRanges();
    QCOMPARE(after.size(), size_t(1));
    QCOMPARE(after[0].first, int64_t(0));
    QVERIFY(after[0].second >= 290 && after[0].second <= 300);
    QCOMPARE(w.timelinePanel()->view()->renderedRanges(), after);
    // undo: the cached frames fit again
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(rp->renderedRanges(), Runs({{0, 600}}), 5000);

    // the export matches the cached preview (frame 100: the smooth clip the 3.0 limit is calibrated on; frame 400 of
    // the busy test pattern differs from the export by its preview downscaling alone - it was shown above to be
    // bit-identical to rendering it)
    const uint64_t hitsBefore = player.renderCacheHits();
    const double d = previewVsExportOf(w, tmp.filePath("rp.mp4"), 100);
    qInfo("cached preview vs export at frame 100: %.2f / 255", d);
    QVERIFY(player.renderCacheHits() > hitsBefore);   // shown from the cache
    QVERIFY(d >= 0 && d < 3.0);

    // Clear
    w.action("clearRenderCache")->trigger();
    QCOMPARE(rp->cache().frameCount(), size_t(0));
    QVERIFY(rp->renderedRanges().empty());
    QVERIFY(w.timelinePanel()->view()->renderedRanges().empty());
    qunsetenv("MEERCUT_RENDER_CACHE");
}

// M6 checkpoint B: the audio controls end to end - the level meter while playing, fade handles and the volume envelope
// on the timeline, the Audio inspector (volume / balance keyframes, fades, equaliser, track gain / mute / solo), the
// Solo header button, clipping, and audio crossfades with their shape list.
void GuiTests::m6AudioMixing() {
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4"), mediaPath("B_blue_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    TimelineView* tl = w.timelinePanel()->view();
    tl->zoomToFit();
    QApplication::processEvents();
    const std::string a = doc->sequence().tracks[1].clips[0].id;   // A's audio
    const std::string atrack = doc->sequence().tracks[1].id;
    doc->setSelection({a});
    QApplication::processEvents();
    selectTab(w.inspector()->clipTabs(), "Audio");

    // ---- level meter: A's tone is -18.06 dBFS
    LevelMeter* meter = w.previewPanel()->levelMeter();
    QVERIFY(meter);
    w.playback()->seek(30);
    w.playback()->play();
    QTest::qWait(700);
    w.playback()->togglePlay();
    QVERIFY2(std::abs(meter->heldDb(0) - (-18.06)) < 1.5, qPrintable(QString("held %1 dB").arg(meter->heldDb(0))));
    QVERIFY(std::abs(meter->heldDb(1) - (-18.06)) < 1.5);
    QVERIFY(!meter->clipLatched());

    // ---- fade-out handle (top right corner of the selected clip's waveform): drag to 8 s = a 2 s fade. Pressed on its
    // outer half, which lies in the clip's 6 px trim zone: the M6 real-input check pressed there and trimmed the clip.
    QRect r = tl->clipRect(a);
    const int handleY = r.y() + 16 + 4;
    const Time aEnd = clipOf(doc, a).timelineEnd();
    drag(tl, QPointF(r.right() + 1 - 4, handleY), QPointF(tl->xForTime(8.0), handleY));
    QCOMPARE(clipOf(doc, a).audio.fadeOut, Rational(2));
    QCOMPARE(clipOf(doc, a).timelineEnd(), aEnd);
    // the edge still trims from the label strip (and below the handle band); undone at once
    drag(tl, QPointF(r.right() - 1, r.y() + 8), QPointF(tl->xForTime(9.0), r.y() + 8));
    QCOMPARE(clipOf(doc, a).timelineEnd(), Rational(9));
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(clipOf(doc, a).timelineEnd(), aEnd);
    QCOMPARE(clipOf(doc, a).audio.fadeOut, Rational(2));
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);   // the whole drag is one undo step
    QCOMPARE(clipOf(doc, a).audio.fadeOut, Rational(0));
    QTest::keyClick(&w, Qt::Key_Y, Qt::ControlModifier);
    QCOMPARE(clipOf(doc, a).audio.fadeOut, Rational(2));

    // ---- volume envelope: Ctrl+click the line adds a point; drag it; double-click deletes it
    r = tl->clipRect(a);
    const double y0 = r.y() + 16 + 4.0, yBottom = r.bottom() - 1.0;
    auto yOfDb = [&](double db) { return db >= 0 ? y0 - 3.0 * db / 12 : y0 + (yBottom - y0) * db / -60; };
    QTest::mouseClick(tl, Qt::LeftButton, Qt::ControlModifier, QPoint(tl->xForTime(3.0), int(y0)));
    QCOMPARE(clipOf(doc, a).volumeDb.keys().size(), size_t(1));
    QCOMPARE(clipOf(doc, a).volumeDb.keys()[0].time, Rational(3));
    QCOMPARE(clipOf(doc, a).volumeDb.keys()[0].value, 0.0);
    drag(tl, QPointF(tl->xForTime(3.0), y0), QPointF(tl->xForTime(3.5), yOfDb(-20)));
    {
        const auto& k = clipOf(doc, a).volumeDb.keys();
        QCOMPARE(k.size(), size_t(1));
        QCOMPARE(k[0].time, Rational(7, 2));
        QVERIFY2(std::abs(k[0].value + 20) < 3.0, qPrintable(QString("point at %1 dB").arg(k[0].value)));
    }
    shot(&w, "m6_01_envelope_and_fade.png");
    const double pointDb = clipOf(doc, a).volumeDb.keys()[0].value;
    doubleClick(tl, QPoint(tl->xForTime(3.5), int(std::lround(yOfDb(pointDb)))));
    QVERIFY(!clipOf(doc, a).volumeDb.isAnimated());
    QCOMPARE(clipOf(doc, a).volumeDb.constantValue(), pointDb);   // the last point's level stays (keyframe rule)
    // dragging the line lowers the whole clip (one undo step); the clip itself does not move
    QVERIFY(ed->setClipVolume(a, 0.0));
    drag(tl, QPointF(tl->xForTime(5.0), y0), QPointF(tl->xForTime(5.0), y0 + 10));
    const double lowered = clipOf(doc, a).volumeDb.constantValue();
    QVERIFY(!clipOf(doc, a).volumeDb.isAnimated());
    QVERIFY2(std::abs(lowered - (-60.0 * 10 / (yBottom - y0))) < 1.0, qPrintable(QString("line at %1 dB").arg(lowered)));
    QCOMPARE(clipOf(doc, a).timelineStart, Rational(0));
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(clipOf(doc, a).volumeDb.constantValue(), 0.0);
    QVERIFY(!clipOf(doc, a).volumeDb.isAnimated());

    // ---- inspector: volume, its keyframes at the playhead, balance, fades, equaliser, track
    auto* volume = w.findChild<QDoubleSpinBox*>("prop:audio.volume");
    auto* balance = w.findChild<QDoubleSpinBox*>("prop:audio.balance");
    auto* volKey = w.findChild<QToolButton*>("key:audio.volume");
    auto* fadeIn = w.findChild<QDoubleSpinBox*>("audioFadeIn");
    auto* eqOn = w.findChild<QCheckBox*>("audioEqEnabled");
    auto* eqPreset = w.findChild<QComboBox*>("audioEqPreset");
    auto* eqBand3 = w.findChild<QSlider*>("audioEqBand3");
    auto* eqReset = w.findChild<QPushButton*>("audioEqReset");
    auto* trackSolo = w.findChild<QCheckBox*>("trackSolo");
    auto* trackVolume = w.findChild<QDoubleSpinBox*>("trackVolume");
    QVERIFY(volume && balance && volKey && fadeIn && eqOn && eqPreset && eqBand3 && eqReset && trackSolo && trackVolume);
    w.playback()->seek(60);   // 2 s
    QApplication::processEvents();
    volume->setValue(-6.0);
    QCOMPARE(clipOf(doc, a).volumeDb.constantValue(), -6.0);
    QTest::mouseClick(volKey, Qt::LeftButton);   // a key at the playhead (source 2 s)
    QVERIFY(clipOf(doc, a).volumeDb.keyAt(Rational(2)));
    w.playback()->seek(120);   // 4 s: auto-key
    QApplication::processEvents();
    volume->setValue(-12.0);
    QCOMPARE(clipOf(doc, a).volumeDb.keys().size(), size_t(2));
    QCOMPARE(clipOf(doc, a).volumeDb.value(Rational(3)), -9.0);
    w.playback()->seek(90);   // the inspector follows the playhead: -9 dB at 3 s
    QApplication::processEvents();
    QCOMPARE(volume->value(), -9.0);
    balance->setValue(50);
    QCOMPARE(clipOf(doc, a).audio.balance.constantValue(), 0.5);
    fadeIn->setValue(1.5);
    QCOMPARE(clipOf(doc, a).audio.fadeIn, Rational(3, 2));
    eqPreset->setCurrentIndex(eqPreset->findData(QString("bass-boost")));
    QVERIFY(clipOf(doc, a).audio.eqEnabled);
    QCOMPARE(clipOf(doc, a).audio.eqPreset, std::string("bass-boost"));
    QVERIFY(clipOf(doc, a).audio.eqGainsDb == findEqPreset("bass-boost")->gainsDb);
    eqBand3->setValue(-55);   // a band moved by hand: Custom
    QCOMPARE(clipOf(doc, a).audio.eqGainsDb[3], -5.5);
    QCOMPARE(clipOf(doc, a).audio.eqPreset, std::string());
    QCOMPARE(eqPreset->currentData().toString(), QString());
    selectTab(w.inspector()->clipTabs(), "Audio");
    shot(&w, "m6_02_audio_inspector.png");
    QTest::mouseClick(eqReset, Qt::LeftButton);
    QVERIFY(!clipOf(doc, a).audio.eqEnabled);
    trackVolume->setValue(-3.0);
    QCOMPARE(doc->sequence().findTrack(atrack)->volumeDb, -3.0);
    trackSolo->setChecked(true);
    QVERIFY(doc->sequence().findTrack(atrack)->solo);
    // the header's S button toggles it back
    const QRect lane = tl->trackLaneRect(atrack);
    QTest::mouseClick(tl, Qt::LeftButton, {}, QPoint(78, lane.bottom() - 18));
    QVERIFY(!doc->sequence().findTrack(atrack)->solo);

    // ---- clipping: +12 dB clip and +12 dB track push the -18 dBFS tone over full scale
    QVERIFY(ed->setClipVolume(a, 12.0));
    QVERIFY(ed->setTrackVolume(atrack, 12.0));
    w.playback()->seek(150);
    w.playback()->play();
    QTest::qWait(500);
    w.playback()->togglePlay();
    QVERIFY(meter->clipLatched());
    shot(w.previewPanel(), "m6_03_meter_clip.png");
    QTest::mouseClick(meter, Qt::LeftButton, {}, QPoint(meter->width() - 10, meter->height() / 2));
    QVERIFY(!meter->clipLatched());
    QVERIFY(ed->setTrackVolume(atrack, 0.0));

    // ---- audio crossfade into B: the transition page lists the crossfade shapes
    QVERIFY(ed->addToTimeline(ids[1], Rational(10)));
    QVERIFY(!ed->addTransition(a));   // a picture dissolve is refused on the audio track
    QVERIFY(ed->addAudioCrossfade(a, "equalPower"));
    QCOMPARE(clipOf(doc, a).outTransition->type, std::string("fa.audio.crossfade.equalPower"));
    doc->setSelectedTransition(a);
    QApplication::processEvents();
    auto* type = w.findChild<QComboBox*>("transitionType");
    QVERIFY(type && type->isVisible());
    QCOMPARE(type->count(), 4);
    auto* direction = w.findChild<QComboBox*>("transitionDirection");
    QVERIFY(direction && !direction->isVisible());   // picture-only settings are hidden for a crossfade
    type->setCurrentIndex(type->findData(QString("fa.audio.crossfade.exponential")));
    QCOMPARE(clipOf(doc, a).outTransition->type, std::string("fa.audio.crossfade.exponential"));
    tl->zoomToFit();
    shot(&w, "m6_04_crossfade.png");
    // and picture transitions are back in the list for a video cut
    const std::string v = doc->sequence().tracks[0].clips[0].id;
    QVERIFY(ed->addTransition(v));
    doc->setSelectedTransition(v);
    QApplication::processEvents();
    QVERIFY(type->count() > 4);
    QVERIFY(direction->isVisible());
    QVERIFY(type->findData(QString("fa.transition.dissolve")) >= 0);
}

// M6 checkpoint C: loudness measurement and normalisation, cleanup controls, Auto Ducking with a target track and the
// master limiter, all through the Audio inspector; the mix the player and export use is checked headlessly.
void GuiTests::m6AudioCleanupAndDucking() {
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4"), mediaPath("B_blue_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    QVERIFY(ed->addTrack(TrackKind::Video));
    std::string v2;
    for (const auto& t : doc->sequence().tracks)
        if (t.name == "Video 2") v2 = t.id;
    QVERIFY2(ed->addToTimeline(ids[1], Rational(0), v2), qPrintable(w.lastMessage()));
    std::vector<const Track*> audio;
    for (const auto& t : doc->sequence().tracks)
        if (t.kind == TrackKind::Audio) audio.push_back(&t);
    QCOMPARE(audio.size(), size_t(2));
    const std::string a = audio[0]->clips.at(0).id, music = audio[1]->clips.at(0).id;   // A = "voice", B = "music"
    const std::string musicTrack = audio[1]->id;
    w.timelinePanel()->view()->zoomToFit();
    doc->setSelection({a});
    QApplication::processEvents();
    selectTab(w.inspector()->clipTabs(), "Audio");

    auto* normalize = w.findChild<QCheckBox*>("audioNormalize");
    auto* target = w.findChild<QDoubleSpinBox*>("audioTargetLufs");
    auto* measure = w.findChild<QPushButton*>("audioMeasure");
    auto* measured = w.findChild<QLabel*>("audioMeasured");
    auto* denoise = w.findChild<QSlider*>("audioDenoise");
    auto* hum = w.findChild<QComboBox*>("audioHum");
    auto* highPass = w.findChild<QDoubleSpinBox*>("audioHighPass");
    auto* duckOn = w.findChild<QCheckBox*>("audioDuckEnabled");
    auto* duckTargets = w.findChild<QListWidget*>("audioDuckTargets");
    auto* duckAmount = w.findChild<QDoubleSpinBox*>("audioDuckAmount");
    auto* limiter = w.findChild<QCheckBox*>("audioLimiter");
    auto* ceiling = w.findChild<QDoubleSpinBox*>("audioLimiterCeiling");
    QVERIFY(normalize && target && measure && measured && denoise && hum && highPass && duckOn && duckTargets && duckAmount &&
            limiter && ceiling);

    // ---- loudness: Measure runs in the background; the tone is -18.06 dBFS on both channels = -18.06 LUFS
    normalize->setChecked(true);
    target->setValue(-23.0);
    QVERIFY(clipOf(doc, a).audio.cleanup.normalize);
    QCOMPARE(clipOf(doc, a).audio.cleanup.targetLufs, -23.0);
    QVERIFY(measured->text().contains("Not measured"));
    QTest::mouseClick(measure, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(clipOf(doc, a).audio.cleanup.measuredLufs.has_value(), 20000);
    const double lufs = *clipOf(doc, a).audio.cleanup.measuredLufs;
    QVERIFY2(std::abs(lufs + 18.06) < 0.3, qPrintable(QString("measured %1 LUFS").arg(lufs)));
    QTRY_VERIFY(measure->isEnabled());
    QVERIFY2(measured->text().contains("gain"), qPrintable(measured->text()));

    // ---- Auto Ducking: A lowers the music track by 20 dB
    duckOn->setChecked(true);
    QCOMPARE(duckTargets->count(), 1);   // the other audio track
    duckTargets->item(0)->setCheckState(Qt::Checked);
    duckAmount->setValue(-20.0);
    const AudioDucking d = clipOf(doc, a).audio.ducking;
    QVERIFY(d.enabled);
    QVERIFY(d.targets == std::vector<std::string>{musicTrack});
    QCOMPARE(d.amountDb, -20.0);

    // ---- cleanup controls
    hum->setCurrentIndex(hum->findData(60.0));
    highPass->setValue(80);
    denoise->setValue(40);
    const AudioCleanup k = clipOf(doc, a).audio.cleanup;
    QCOMPARE(k.humHz, 60.0);
    QCOMPARE(k.highPassHz, 80.0);
    QCOMPARE(k.denoise, 0.4);
    highPass->setValue(highPass->minimum());   // Off
    QCOMPARE(clipOf(doc, a).audio.cleanup.highPassHz, 0.0);
    denoise->setValue(0);   // the steady test tone would count as noise (documented); keep the levels checkable

    // ---- master limiter
    limiter->setChecked(true);
    ceiling->setValue(-3.0);
    QVERIFY(doc->sequence().limiter);
    QCOMPARE(doc->sequence().limiterCeilingDb, -3.0);
    // every Audio group fits the inspector's width (none forces a sideways scroll)
    for (QGroupBox* g : w.inspector()->findChildren<QGroupBox*>())
        if (g->isVisible())
            QVERIFY2(g->minimumSizeHint().width() <= g->width(), qPrintable(QString("%1 needs %2 px, has %3").arg(g->title())
                                                                            .arg(g->minimumSizeHint().width()).arg(g->width())));
    shot(&w, "m6_05_cleanup_ducking.png");

    // ---- the mix (what the player and export play): A normalised to -23 LUFS, B ducked by 20 dB
    {
        AudioMixer mixer("", 48000);
        std::vector<float> pcm(48000 * 2);
        mixer.mix(doc->project(), doc->sequence(), 3 * 48000, 48000, pcm.data());
        const double voice = toneAmplitude(pcm, 0, 4800, 1000), musicLevel = toneAmplitude(pcm, 0, 4800, 1500);
        QVERIFY2(std::abs(20 * std::log10(voice / 0.125) - (-23 - lufs)) < 0.3, qPrintable(QString("voice %1").arg(voice)));
        QVERIFY2(std::abs(20 * std::log10(musicLevel / 0.125) - (-20.0)) < 0.3, qPrintable(QString("music %1").arg(musicLevel)));
    }
    // one undo step per control: the ceiling goes back to -1 dBFS, then the limiter off
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(doc->sequence().limiterCeilingDb, -1.0);
    QVERIFY(doc->sequence().limiter);
    // the music clip's inspector lists the voice's track as a possible target of its own ducking
    doc->setSelection({music});
    QApplication::processEvents();
    QCOMPARE(duckTargets->count(), 1);
    QVERIFY(!duckOn->isChecked());
}

// M6 checkpoint E: the export window - format / codec / encoder choices, resolution and frame-rate presets, quality
// and bit rate, In / Out and selected-clip ranges, validation of the name, the check of the finished file, and that an
// existing file survives a cancelled export.
void GuiTests::m6ExportWindow() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4"), mediaPath("B_blue_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    QVERIFY(ed->addToTimeline(ids[1], Rational(10)));
    // In / Out from the Playback menu shortcuts: In at 2 s, Out after frame 3 s - 1 frame (= 3 s)
    w.playback()->seek(60);
    QApplication::processEvents();
    w.action("markIn")->trigger();
    w.playback()->seek(89);
    QApplication::processEvents();
    w.action("markOut")->trigger();
    QCOMPARE(*doc->sequence().markIn, Rational(2));
    QCOMPARE(*doc->sequence().markOut, Rational(3));
    shot(&w, "m6_06_in_out_marks.png");
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);   // one undo step each
    QVERIFY(!doc->sequence().markOut);
    QTest::keyClick(&w, Qt::Key_Y, Qt::ControlModifier);
    QVERIFY(doc->sequence().markOut);

    const std::string b = doc->sequence().tracks[0].clips.at(1).id;   // B's video
    doc->setSelection({b});
    ExportDialog d(doc, &w);
    d.show();
    QVERIFY(QTest::qWaitForWindowExposed(&d));
    QTRY_VERIFY_WITH_TIMEOUT(d.encodersChecked(), 60000);
    QVERIFY2(d.sizeHint().height() <= d.screen()->availableGeometry().height(),   // fits the screen (1080p at 125 %)
             qPrintable(QString("needs %1 px").arg(d.sizeHint().height())));
    auto* name = d.findChild<QLineEdit*>("exportName");
    auto* folder = d.findChild<QLineEdit*>("exportFolder");
    auto* format = d.findChild<QComboBox*>("exportFormat");
    auto* hardware = d.findChild<QComboBox*>("exportHardware");
    auto* resolution = d.findChild<QComboBox*>("exportResolution");
    auto* rate = d.findChild<QComboBox*>("exportFrameRate");
    auto* quality = d.findChild<QComboBox*>("exportQuality");
    auto* bitrate = d.findChild<QSpinBox*>("exportBitrate");
    auto* depth = d.findChild<QComboBox*>("exportBitDepth");
    auto* audioCodec = d.findChild<QComboBox*>("exportAudioCodec");
    auto* inOut = d.findChild<QRadioButton*>("exportRangeInOut");
    auto* selected = d.findChild<QRadioButton*>("exportRangeSelected");
    auto* summary = d.findChild<QLabel*>("exportSummary");
    auto* fileName = d.findChild<QLabel*>("exportFileName");
    QVERIFY(name && folder && format && hardware && resolution && rate && quality && bitrate && depth && audioCodec &&
            inOut && selected && summary && fileName);
    folder->setText(QDir::toNativeSeparators(tmp.path()));

    // the name: a typed extension follows the format; invalid names are refused before anything starts
    name->setText("clip.mov");
    QVERIFY(d.outputPath().endsWith("clip.mp4"));
    name->setText("bad:name");
    QVERIFY(d.problem().contains("cannot contain"));
    name->setText("CON");
    QVERIFY(!d.problem().isEmpty());
    name->setText("first");
    QVERIFY2(d.problem().isEmpty(), qPrintable(d.problem()));
    QVERIFY(summary->text().contains("Estimated size"));

    // HEVC at 720p, 29.97 fps, Standard quality, the In to Out range (2 .. 3 s): 30 frames at 29.97
    format->setCurrentIndex(format->findData(QString("mp4-hevc")));
    QVERIFY(fileName->text().endsWith("first.mp4"));
    resolution->setCurrentIndex(resolution->findData(720));
    rate->setCurrentIndex(rate->findData(QString("30000/1001")));
    quality->setCurrentIndex(quality->findData(2));
    inOut->setChecked(true);
    shot(&d, "m6_07_export_window.png");
    QString err;
    QVERIFY2(d.exportTo(tmp.filePath("first.mp4"), &err), qPrintable(err));
    QVERIFY2(d.verification().startsWith("Checked"), qPrintable(d.verification()));
    MediaInfo mi = probeMedia(p8(tmp.filePath("first.mp4")));
    QVERIFY(mi.video && mi.audio);
    QCOMPARE(mi.video->codec, std::string("hevc"));
    QCOMPARE(mi.video->width, 1280);
    QCOMPARE(mi.video->height, 720);
    QCOMPARE(mi.video->frameRate, FrameRate(Rational(30000, 1001)));
    QVERIFY(std::abs(mi.duration->toDouble() - 30 * 1.001 / 30) < 0.04);
    {   // the first exported frame is source frame 60 (2 s into A)
        VideoDecoder dec(p8(tmp.filePath("first.mp4")));
        QVERIFY(std::abs(stripIndex(QImage(dec.frameAt(Time(0)).image.pixels.data(), 1280, 720, QImage::Format_RGBA8888)) - 60) <= 1);
    }

    // 44.1 kHz mono audio over the same range: the check of the finished file confirms rate and channels
    auto* sampleRate = d.findChild<QComboBox*>("exportSampleRate");
    auto* channels = d.findChild<QComboBox*>("exportChannels");
    QVERIFY(sampleRate && channels);
    sampleRate->setCurrentIndex(sampleRate->findData(44100));
    channels->setCurrentIndex(channels->findData(1));
    QVERIFY2(d.exportTo(tmp.filePath("mono44.mp4"), &err), qPrintable(err));
    QVERIFY2(d.verification().contains("44100 Hz mono"), qPrintable(d.verification()));
    mi = probeMedia(p8(tmp.filePath("mono44.mp4")));
    QCOMPARE(mi.audio->sampleRate, 44100);
    QCOMPARE(mi.audio->channels, 1);
    sampleRate->setCurrentIndex(0);
    channels->setCurrentIndex(0);
    QVERIFY(d.findChild<QCheckBox*>("exportSelectedSpan"));

    // a bit rate instead of a quality preset, and the selected clip only (B: 10 s); WebM offers Opus only
    format->setCurrentIndex(format->findData(QString("webm-vp9")));
    QCOMPARE(audioCodec->count(), 1);
    QCOMPARE(audioCodec->currentData().toString(), QString("libopus"));
    format->setCurrentIndex(format->findData(QString("mp4-h264")));
    resolution->setCurrentIndex(resolution->findData(0));
    rate->setCurrentIndex(0);
    quality->setCurrentIndex(quality->findData(-1));
    QVERIFY(bitrate->isVisible());
    bitrate->setValue(4000);
    selected->setChecked(true);
    QVERIFY2(d.exportTo(tmp.filePath("selected.mp4"), &err), qPrintable(err));
    mi = probeMedia(p8(tmp.filePath("selected.mp4")));
    QVERIFY(std::abs(mi.duration->toDouble() - 10.0) < 0.05);
    {
        VideoDecoder dec(p8(tmp.filePath("selected.mp4")));
        const QColor c = centre(QImage(dec.frameAt(Rational(5)).image.pixels.data(), 1920, 1080, QImage::Format_RGBA8888));
        QVERIFY2(c.blue() > 150 && c.red() < 60, qPrintable(c.name()));   // B (blue), not A
    }
    // x264 records its rate control in the stream: the bit-rate mode reached the encoder (the flat test pictures need
    // far less than 4000 kbit/s, so the file size alone would not show it); High quality = constant quality, CRF 18
    auto x264Settings = [&](const QString& file) {
        QFile f(file);
        return f.open(QIODevice::ReadOnly) ? f.read(1 << 20) : QByteArray();
    };
    QVERIFY(x264Settings(tmp.filePath("selected.mp4")).contains("rc=abr"));
    QVERIFY(x264Settings(tmp.filePath("selected.mp4")).contains("bitrate=4000"));
    quality->setCurrentIndex(quality->findData(4));
    QVERIFY2(d.exportTo(tmp.filePath("selected_q.mp4"), &err), qPrintable(err));
    QVERIFY(x264Settings(tmp.filePath("selected_q.mp4")).contains("rc=crf"));
    QVERIFY(x264Settings(tmp.filePath("selected_q.mp4")).contains("crf=18.0"));

    // hardware choices: items that cannot work here are disabled and say so; a hardware export reports its encoder
    hardware->setCurrentIndex(hardware->findData(QString("auto")));
    QVERIFY(!d.findChild<QLabel*>("exportEncoderInfo")->text().isEmpty());
    const auto* model = qobject_cast<QStandardItemModel*>(hardware->model());
    QVERIFY(!model->item(hardware->findData(QString("amf")))->isEnabled());   // not in this build

    // an existing file survives a cancelled export
    {
        QFile f(tmp.filePath("keep.mp4"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("previous export");
    }
    hardware->setCurrentIndex(hardware->findData(QString("cpu")));
    d.findChild<QRadioButton*>("exportRangeWhole")->setChecked(true);
    QTimer::singleShot(1500, &d, [&d] {   // Cancel shortly after the start
        if (auto* btn = d.findChild<QPushButton*>("accentButton")) btn->click();
    });
    QVERIFY(!d.exportTo(tmp.filePath("keep.mp4"), &err));
    {
        QFile f(tmp.filePath("keep.mp4"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("previous export"));
    }
    QVERIFY(!QFileInfo::exists(tmp.filePath("keep.mp4.partial.mp4")));
    // a folder on a drive that does not exist: the export reports the error, nothing is claimed
    QString missingDrive;
    for (char c = 'Z'; c >= 'H' && missingDrive.isEmpty(); --c)
        if (!QFileInfo::exists(QString("%1:/").arg(c))) missingDrive = QString(c);
    QVERIFY(!missingDrive.isEmpty());
    QVERIFY(!d.exportTo(missingDrive + ":/meercut/out.mp4", &err));
    QVERIFY2(!err.isEmpty(), "an error message");
    // the project's own media cannot be overwritten
    folder->setText(QDir::toNativeSeparators(QFileInfo(mediaPath("A_red_1080p30_10s.mp4")).absolutePath()));
    name->setText("A_red_1080p30_10s");
    QVERIFY(d.problem().contains("used by the project"));
}

// M7 checkpoint A: AI Tools > Model Manager - the catalogue with sizes, licences and status, the devices, no download
// without the user's confirmation, and hash verification of installed models.
void GuiTests::m7ModelManager() {
    MainWindow w(MainWindow::Options{false, true});
    QVERIFY(w.action("modelManager"));
    QMenu* aiMenu = w.menuBar()->findChild<QMenu*>("aiMenu");
    QVERIFY(aiMenu);
    QVERIFY(aiMenu->actions().contains(w.action("modelManager")));
    QTemporaryDir empty;
    ai::setModelsDirectoryOverride(empty.path().toStdWString());
    {
        ModelManagerDialog d;
        d.show();
        QVERIFY(QTest::qWaitForWindowExposed(&d));
        auto* table = d.findChild<QTableWidget*>("modelTable");
        QVERIFY(table);
        const auto& cat = ai::modelCatalog();
        QCOMPARE(table->rowCount(), int(cat.size()));
        int rvm = -1;
        for (int r = 0; r < table->rowCount(); ++r) {
            QCOMPARE(table->item(r, 1)->text(), QString("Not installed"));   // Status
            QVERIFY(!table->item(r, 2)->text().isEmpty());   // download size
            QVERIFY(table->item(r, 3)->text().contains(" / "));   // code / weights licence
            if (cat[size_t(r)].id == "rvm-mobilenetv3") rvm = r;
        }
        QVERIFY(rvm >= 0);
        auto* devices = d.findChild<QLabel*>("aiDevices");
        QVERIFY(devices->text().contains("ONNX Runtime 1.24.4"));
        for (const auto& g : ai::aiCapabilities().gpus) QVERIFY(devices->text().contains(QString::fromStdString(g.name)));
        table->selectRow(rvm);
        auto* details = d.findChild<QTextBrowser*>("modelDetails");
        QVERIFY(details->toPlainText().contains("88d4531297118f595bf2fd60f6f566aec2e559393802d1f436c380f0cbbd2828"));
        QVERIFY(details->toPlainText().contains("Lin"));   // the credit
        // Download asks first (size, source, licences); "No" downloads nothing
        QString asked;
        d.confirmDownload = [&](const QString& text) {
            asked = text;
            return false;
        };
        QTest::mouseClick(d.findChild<QPushButton*>("modelDownload"), Qt::LeftButton);
        QVERIFY2(asked.contains("14 MB") && asked.contains("github.com") && asked.contains("GPL-3.0"), qPrintable(asked));
        QVERIFY(!d.busy());
        QVERIFY(d.findChild<QLabel*>("modelStatus")->text().contains("Nothing was downloaded"));
        QVERIFY(QDir(empty.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
        shot(&d, "m7_01_model_manager_empty.png");
    }
    // the installed models (FA_MODELS_DIR): Installed; Verify re-hashes the selected model
    ai::setModelsDirectoryOverride(std::filesystem::path(FA_MODELS_DIR));
    {
        ModelManagerDialog d;
        d.show();
        QVERIFY(QTest::qWaitForWindowExposed(&d));
        auto* table = d.findChild<QTableWidget*>("modelTable");
        int yunet = -1;
        for (int r = 0; r < table->rowCount(); ++r) {
            QCOMPARE(table->item(r, 1)->text(), QString("Installed"));
            if (ai::modelCatalog()[size_t(r)].id == "yunet-face") yunet = r;
        }
        table->selectRow(yunet);
        QTest::mouseClick(d.findChild<QPushButton*>("modelVerify"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!d.busy() && table->item(yunet, 1)->text().contains("verified"), 20000);
        QVERIFY(d.findChild<QLabel*>("modelStatus")->text().contains("done"));
        shot(&d, "m7_02_model_manager_installed.png");
    }
    ai::setModelsDirectoryOverride({});
}

// AI Tools > Background Removal on real footage: the Tears of Steel bridge shot (two people, source 10-12 s).
// Preview This Frame, Analyse Clip (Cancel first), a colour background, background media on a track below,
// a refinement slider with undo, and the preview of the clip with the cutout.
void GuiTests::m7BackgroundRemoval() {
    const std::filesystem::path tos = std::filesystem::path(FA_TESTMEDIA_DIR) / "ai" / "tears_of_steel_60s.mov";
    const ai::ModelInfo* rvm = ai::findModel("rvm-mobilenetv3");
    if (!std::filesystem::exists(tos) || ai::modelStatus(*rvm, false, FA_MODELS_DIR) != ai::ModelStatus::Installed)
        QSKIP("Tears of Steel or the RVM model missing");
    QTemporaryDir cacheDir;
    qputenv("MEERCUT_ANALYSIS_CACHE", QDir::toNativeSeparators(cacheDir.path()).toUtf8());
    ai::setModelsDirectoryOverride(std::filesystem::path(FA_MODELS_DIR));
    {
        MainWindow w(MainWindow::Options{false, true});
        w.resize(1600, 1000);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        Document* doc = w.document();
        EditController* ed = w.editController();
        const auto ids = w.importFiles({QString::fromStdWString(tos.wstring())});
        QCOMPARE(ids.size(), size_t(1));
        QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
        const std::string a = doc->sequence().tracks[0].clips[0].id;
        QVERIFY(ed->trimClip(a, ClipEdge::Head, Rational(10)));
        QVERIFY(ed->trimClip(a, ClipEdge::Tail, Rational(12)));
        QVERIFY(ed->moveClip(a, Rational(0), std::nullopt));
        const Clip* c = edit::findClip(doc->sequence(), a);
        QCOMPARE(c->sourceIn, Rational(10));
        QCOMPARE(c->duration, Rational(2));
        w.timelinePanel()->view()->zoomToFit();
        doc->setSelection({a});
        w.playback()->seek(24);   // source 11 s
        const QImage original = waitForPreview(w, 24);
        QVERIFY(!original.isNull());

        // the menu entry opens the panel on the selected clip
        QMenu* aiMenu = w.menuBar()->findChild<QMenu*>("aiMenu");
        QVERIFY(aiMenu && aiMenu->actions().contains(w.action("aiCutout")));
        w.action("aiCutout")->trigger();
        QApplication::processEvents();
        QCOMPARE(w.inspector()->videoSubTab(), QString("cutout"));
        CutoutPanel* cp = w.inspector()->cutoutPanel();
        auto* enable = cp->findChild<QCheckBox*>("cutoutEnable");
        auto* preview = cp->findChild<QPushButton*>("cutoutPreview");
        auto* analyse = cp->findChild<QPushButton*>("cutoutAnalyse");
        auto* status = cp->findChild<QLabel*>("cutoutStatus");
        auto* background = cp->findChild<QComboBox*>("cutoutBackground");
        auto* threshold = cp->findChild<QDoubleSpinBox*>("cutoutThreshold");
        QVERIFY(enable && preview && analyse && status && background && threshold);
        QVERIFY(enable->isVisible() && !enable->isChecked());
        QVERIFY2(status->text().contains("Not analysed"), qPrintable(status->text()));

        // Preview This Frame: the frame under the playhead analysed in the background (the editor stays responsive),
        // then shown cut out over black (nothing below)
        QTest::mouseClick(preview, Qt::LeftButton);
        QVERIFY(w.analysis()->busy());
        QTRY_VERIFY_WITH_TIMEOUT(!w.analysis()->busy(), 60000);
        QTRY_VERIFY(enable->isChecked());
        QVERIFY(edit::findClip(doc->sequence(), a)->aiCutout.enabled);
        const QImage cut = waitForPreview(w, 24);
        QVERIFY(!cut.isNull());
        auto px = [](const QImage& img, double fx, double fy) { return img.pixelColor(int(fx * img.width()), int(fy * img.height())); };
        auto lum = [](const QColor& q) { return 0.2126 * q.red() + 0.7152 * q.green() + 0.0722 * q.blue(); };
        // left person (x 390-450, y 200-330 of 1280 x 534) is kept; the far background (top left) goes
        const QColor person = px(cut, 420.0 / 1280, 265.0 / 534), personBefore = px(original, 420.0 / 1280, 265.0 / 534);
        const QColor sky = px(cut, 0.08, 0.12), skyBefore = px(original, 0.08, 0.12);
        qInfo("preview: person %s (was %s), background %s (was %s)", qPrintable(person.name()), qPrintable(personBefore.name()),
              qPrintable(sky.name()), qPrintable(skyBefore.name()));
        QVERIFY(std::abs(lum(person) - lum(personBefore)) < 25);
        QVERIFY(lum(sky) < 8 && lum(skyBefore) > 30);
        shot(&w, "m7_03_cutout_preview.png");

        // Analyse Clip: Cancel stops it; then the whole clip (48 frames)
        QTest::mouseClick(analyse, Qt::LeftButton);
        QVERIFY(w.analysis()->busy());
        QCOMPARE(analyse->text(), QString("Cancel"));
        QTest::mouseClick(analyse, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!w.analysis()->busy(), 20000);
        QTest::mouseClick(analyse, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!w.analysis()->busy(), 120000);
        QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("Analysed: all 48 frames"), 5000);
        qInfo("%s", qPrintable(status->text()));

        // a colour background (one undo step)
        const size_t undoBefore = doc->undoStack().undoCount();
        background->setCurrentIndex(1);
        QCOMPARE(doc->undoStack().undoCount(), undoBefore + 1);
        const QImage green = waitForPreview(w, 24);
        const QColor g = px(green, 0.08, 0.12);
        QVERIFY2(std::abs(g.red() - 20) < 12 && std::abs(g.green() - 120) < 12 && std::abs(g.blue() - 60) < 12, qPrintable(g.name()));
        QVERIFY(std::abs(lum(px(green, 420.0 / 1280, 265.0 / 534)) - lum(personBefore)) < 25);
        shot(&w, "m7_04_cutout_colour.png");
        QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!edit::findClip(doc->sequence(), a)->aiCutout.background);

        // background media on a track below (what Add Background Media does after its file dialog)
        const auto blue = w.importFiles({mediaPath("B_blue_1080p30_10s.mp4")});
        QVERIFY(ed->addBackgroundMedia(a, blue[0]));
        {   // a new track below the clip's (tracks are listed bottom first), over the clip's time
            int ta = -1, tb = -1;
            const auto& tracks = doc->sequence().tracks;
            for (int i = 0; i < int(tracks.size()); ++i)
                for (const auto& x : tracks[size_t(i)].clips) {
                    if (x.id == a) ta = i;
                    if (x.sourceId == blue[0]) {
                        tb = i;
                        QCOMPARE(x.timelineStart, Rational(0));
                        QCOMPARE(x.duration, Rational(2));
                    }
                }
            QVERIFY(tb >= 0 && tb < ta);
        }
        const QImage onBlue = waitForPreview(w, 24);
        const QColor b = px(onBlue, 0.20, 0.12);   // inside the blue picture (16:9 letterboxed on the 2.4:1 canvas)
        QVERIFY2(b.blue() > 180 && b.red() < 40, qPrintable(b.name()));
        shot(&w, "m7_05_cutout_background_video.png");

        // threshold: typed, one undo step; Ctrl+Z restores
        const size_t before = doc->undoStack().undoCount();
        threshold->setFocus();
        threshold->selectAll();
        QTest::keyClicks(threshold, "0.5");
        QTest::keyClick(threshold, Qt::Key_Return);
        QTRY_COMPARE(edit::findClip(doc->sequence(), a)->aiCutout.threshold, 0.5);
        QVERIFY(doc->undoStack().undoCount() <= before + 1);
        QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(edit::findClip(doc->sequence(), a)->aiCutout.threshold, 0.0);
        // switching it off shows the original again
        QTest::mouseClick(enable, Qt::LeftButton, {}, QPoint(9, enable->height() / 2));
        QVERIFY(!edit::findClip(doc->sequence(), a)->aiCutout.enabled);
        QVERIFY(std::abs(lum(px(waitForPreview(w, 24), 0.08, 0.12)) - lum(skyBefore)) < 12);
        // General objects (BiRefNet) does not fit a 4 GB GPU: the panel asks, with the CPU time, and "No" starts nothing
        AiCutout general;
        general.model = "birefnet-general-tiny";
        if (!ai::cutoutFitsGpu(general)) {
            cp->findChild<QComboBox*>("cutoutModel")->setCurrentIndex(1);
            QString asked;
            cp->confirmSlow = [&](const QString& text) {
                asked = text;
                return false;
            };
            QTest::mouseClick(preview, Qt::LeftButton);
            qInfo("%s", qPrintable(asked));
            QVERIFY2(asked.contains("CPU") && asked.contains("1 frame(s)") && asked.contains("about"), qPrintable(asked));
            QVERIFY(!w.analysis()->busy());
            cp->findChild<QComboBox*>("cutoutModel")->setCurrentIndex(0);
        }
    }
    ai::setModelsDirectoryOverride({});
    qputenv("MEERCUT_ANALYSIS_CACHE", "");
}

// AI Tools > Speech to Text on real footage with dialogue (Tears of Steel, the bridge scene): the clip's sound
// transcribed by whisper.cpp in the background, a caption corrected in the list, Add Captions (one undo step), the
// captions in the preview, after saving and reopening, burnt into the export and in the SRT export.
void GuiTests::m7SpeechToText() {
    const std::filesystem::path tos = std::filesystem::path(FA_TESTMEDIA_DIR) / "ai" / "tears_of_steel_60s.mov";
    const ai::ModelInfo* whisper = ai::findModel("whisper-base");
    if (!std::filesystem::exists(tos) || ai::modelStatus(*whisper, false, FA_MODELS_DIR) != ai::ModelStatus::Installed)
        QSKIP("Tears of Steel or whisper-base missing");
    ai::setModelsDirectoryOverride(std::filesystem::path(FA_MODELS_DIR));
    QTemporaryDir tmp;
    {
        MainWindow w(MainWindow::Options{false, true});
        w.resize(1600, 1000);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        Document* doc = w.document();
        EditController* ed = w.editController();
        const auto ids = w.importFiles({QString::fromStdWString(tos.wstring())});
        QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
        const std::string v = doc->sequence().tracks[0].clips[0].id;
        QVERIFY(ed->trimClip(v, ClipEdge::Tail, Rational(16)));   // the first 16 s: the bridge scene
        doc->setSelection({v});
        // the clip's context menu and the AI Tools menu lead to the same dialog
        QMenu* aiMenu = w.menuBar()->findChild<QMenu*>("aiMenu");
        QVERIFY(aiMenu && aiMenu->actions().contains(w.action("speechToText")));
        w.action("speechToText")->trigger();
        auto* dlg = w.findChild<SpeechToTextDialog*>("speechToText");
        QVERIFY(dlg && dlg->isVisible());
        auto* source = dlg->findChild<QComboBox*>("sttSource");
        auto* model = dlg->findChild<QComboBox*>("sttModel");
        auto* transcribe = dlg->findChild<QPushButton*>("sttTranscribe");
        auto* add = dlg->findChild<QPushButton*>("sttAdd");
        auto* list = dlg->findChild<QTableWidget*>("sttList");
        auto* status = dlg->findChild<QLabel*>("sttStatus");
        QVERIFY(source && model && transcribe && add && list && status);
        QCOMPARE(model->currentData().toString(), QString("whisper-base"));
        source->setCurrentIndex(source->findData("clip"));
        QTest::mouseClick(transcribe, Qt::LeftButton);
        QVERIFY(dlg->busy());
        QCOMPARE(transcribe->text(), QString("Cancel"));
        QApplication::processEvents();   // the window stays responsive while whisper runs
        QTRY_VERIFY_WITH_TIMEOUT(!dlg->busy(), 240000);
        qInfo("%s", qPrintable(status->text()));
        QVERIFY2(list->rowCount() >= 2, qPrintable(status->text()));
        QVERIFY(status->text().contains("language: en"));
        QVERIFY(add->isEnabled());
        for (int r = 0; r < list->rowCount(); ++r) qInfo("%s - %s %s", qPrintable(list->item(r, 0)->text()),
                                                          qPrintable(list->item(r, 1)->text()), qPrintable(list->item(r, 2)->text()));
        // a correction typed in the list is what goes on the timeline
        list->item(0, 2)->setText("Corrected first caption");
        const size_t undoBefore = doc->undoStack().undoCount();
        QTest::mouseClick(add, Qt::LeftButton);
        QCOMPARE(doc->undoStack().undoCount(), undoBefore + 1);
        std::string subs;
        for (const auto& t : doc->sequence().tracks)
            if (t.kind == TrackKind::Subtitle) subs = t.id;
        QVERIFY(!subs.empty());
        const auto caps = captionsOn(doc, subs);
        QCOMPARE(int(caps.size()), list->rowCount());
        QCOMPARE(caps[0]->title->text, std::string("Corrected first caption"));
        for (const Clip* c : caps) QVERIFY(c->timelineStart >= Rational(0) && c->timelineEnd() <= Rational(16));
        // values, not pointers: later edits (track flags) replace the clips
        const size_t capCount = caps.size();
        const Rational firstStart = caps[0]->timelineStart;
        const int64_t secondMid = ((caps[1]->timelineStart + caps[1]->timelineEnd()) * Rational(12)).floor();   // frames (24 fps)
        dlg->close();
        // one undo step removes them all; redo brings them back
        w.activateWindow();   // back to the main window, as a click on it would
        QVERIFY(QTest::qWaitForWindowActive(&w));
        QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(captionsOn(doc, subs).empty());
        QTest::keyClick(&w, Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(captionsOn(doc, subs).size(), capCount);

        // preview: the caption over the picture (compared with the subtitle track hidden)
        const int64_t f = (firstStart * Rational(24)).ceil() + 2;
        QVERIFY(ed->setTrackFlag(subs, "hidden", true));
        w.playback()->seek(f);
        const QImage bare = waitForPreview(w, f);
        QVERIFY(ed->setTrackFlag(subs, "hidden", false));
        w.playback()->seek(f);
        const QImage withCaption = waitForPreview(w, f);
        QVERIFY(changedPixels(withCaption, bare, QRectF(0.1, 0.80, 0.8, 0.18)) > 50);
        QVERIFY(changedPixels(withCaption, bare, QRectF(0.0, 0.0, 1.0, 0.5)) == 0);
        w.playback()->seek(secondMid);
        QVERIFY(!waitForPreview(w, secondMid).isNull());
        shot(&w, "m7_06_speech_captions.png");

        // save, reopen: the same captions
        const QString file = tmp.filePath("speech.lcproj");
        QVERIFY(w.saveProjectTo(file));
        MainWindow w2(MainWindow::Options{false, true});
        w2.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w2));
        QVERIFY(w2.openProjectFile(file));
        QVERIFY(w2.document()->sequence() == doc->sequence());

        // export: burnt into the video; SRT: the same text and times
        QString err;
        const QString mp4 = tmp.filePath("speech.mp4");
        QVERIFY2(w.exportTo(mp4, &err), qPrintable(err));
        {
            VideoDecoder vd(p8(mp4));
            const VideoFrame& ef = vd.frameAt(frameToTime(f, doc->sequence().frameRate));
            const QImage eq = QImage(ef.image.pixels.data(), ef.image.width, ef.image.height, ef.image.stride(),
                                     QImage::Format_RGBA8888).scaled(bare.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            QVERIFY2(changedPixels(eq, bare, QRectF(0.1, 0.80, 0.8, 0.18)) > 50, "no caption in the exported frame");
        }
        const QString srt = tmp.filePath("speech.srt");
        QVERIFY2(w.exportSubtitlesTo(srt, subs, &err), qPrintable(err));
        const QByteArray srtBytes = readBytes(srt);
        const SubtitleDoc back = parseSrt(std::string_view(srtBytes.constData(), size_t(srtBytes.size())));
        QCOMPARE(back.captions.size(), capCount);
        QCOMPARE(back.captions[0].text, std::string("Corrected first caption"));
        // SRT stores milliseconds: a frame time such as 98/24 s comes back to the millisecond
        QVERIFY(std::abs((back.captions[0].start - firstStart).toDouble()) < 0.0006);
    }
    ai::setModelsDirectoryOverride({});
}

// Scene Detection on real footage (the Sintel trailer, source frames 300-459: cuts at 334, 396, 404, 408 and a cut to
// black at 420; 408, between two blurred action shots, is the one cut of the trailer the detector misses): detect,
// review (remove the cut to black, add the missed cut by hand), split the clip with its linked audio (one undo step),
// add the scenes to the library and place one.
void GuiTests::m7SceneDetection() {
    const std::filesystem::path sintel = std::filesystem::path(FA_TESTMEDIA_DIR) / "ai" / "sintel_trailer_720p.mp4";
    if (!std::filesystem::exists(sintel)) QSKIP("testmedia/ai missing");
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    QVERIFY(w.menuBar()->findChild<QMenu*>("aiMenu")->actions().contains(w.action("sceneDetection")));
    const auto ids = w.importFiles({QString::fromStdWString(sintel.wstring())});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    const std::string v = doc->sequence().tracks[0].clips[0].id;
    QVERIFY(ed->trimClip(v, ClipEdge::Head, Rational(300, 24)));
    QVERIFY(ed->trimClip(v, ClipEdge::Tail, Rational(460, 24)));
    QVERIFY(ed->moveClip(v, Rational(0), std::nullopt));
    doc->setSelection({v});
    SceneDetectionDialog d(doc, ed, &w);
    d.show();
    QVERIFY(QTest::qWaitForWindowExposed(&d));
    auto* clipOnly = d.findChild<QCheckBox*>("sceneClipOnly");
    auto* detect = d.findChild<QPushButton*>("sceneDetect");
    auto* list = d.findChild<QListWidget*>("sceneList");
    auto* remove = d.findChild<QPushButton*>("sceneRemove");
    auto* addFrame = d.findChild<QSpinBox*>("sceneAddFrame");
    auto* add = d.findChild<QPushButton*>("sceneAdd");
    auto* split = d.findChild<QPushButton*>("sceneSplit");
    auto* library = d.findChild<QPushButton*>("sceneLibrary");
    auto* status = d.findChild<QLabel*>("sceneStatus");
    QVERIFY(clipOnly->isChecked());   // the selected clip's part of the file
    QVERIFY(status->text().contains("no AI model"));
    QTest::mouseClick(detect, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!d.busy(), 120000);
    qInfo("%s", qPrintable(status->text()));
    std::vector<int64_t> starts = d.sceneStarts();
    QString found;
    for (int64_t f : starts) found += QString::number(f) + " ";
    qInfo("scene starts: %s", qPrintable(found));
    QCOMPARE(starts.front(), int64_t(300));
    for (int64_t cut : {334, 396, 404, 420})
        QVERIFY2(std::find(starts.begin(), starts.end(), cut) != starts.end(), qPrintable(found));
    QCOMPARE(list->count(), int(starts.size()));
    QVERIFY(!list->item(1)->icon().isNull());   // thumbnails
    shot(&d, "m7_07_scene_detection.png");
    // review: remove the boundary at the cut to black (420: joins the black to the shot before), add the missed 408
    const int at420 = int(std::find(starts.begin(), starts.end(), 420) - starts.begin());
    list->setCurrentRow(at420);
    QTest::mouseClick(remove, Qt::LeftButton);
    addFrame->setValue(408);
    QTest::mouseClick(add, Qt::LeftButton);
    starts = d.sceneStarts();
    QVERIFY(std::find(starts.begin(), starts.end(), 420) == starts.end());
    QVERIFY(std::find(starts.begin(), starts.end(), 408) != starts.end());
    // split the timeline clip at every boundary: video and its linked audio cut at the same frames, one undo step
    const size_t undoBefore = doc->undoStack().undoCount();
    QTest::mouseClick(split, Qt::LeftButton);
    QCOMPARE(doc->undoStack().undoCount(), undoBefore + 1);
    std::vector<Rational> vStarts, aStarts;
    for (const auto& t : doc->sequence().tracks)
        for (const auto& c : t.clips) (t.kind == TrackKind::Video ? vStarts : aStarts).push_back(c.timelineStart);
    QCOMPARE(vStarts.size(), starts.size());
    QCOMPARE(aStarts, vStarts);   // the sound stays in sync
    for (size_t i = 0; i < starts.size(); ++i) QCOMPARE(vStarts[i], Rational(starts[i] - 300, 24));   // frame-accurate
    // Edit > Undo (the action Ctrl+Z and the toolbar trigger; with the dialog open the test window is not active, so
    // the keyboard path - covered by the M1-M6 GUI tests - is not used here)
    QVERIFY(w.action("undo")->isEnabled());
    w.action("undo")->trigger();
    QCOMPARE(doc->sequence().tracks[0].clips.size(), size_t(1));
    // the scenes into the library: one item per scene, each placing only its part
    const size_t sources = doc->project().sources.size();
    QTest::mouseClick(library, Qt::LeftButton);
    QCOMPARE(doc->project().sources.size(), sources + starts.size());
    const MediaSource& second = doc->project().sources[sources + 1];
    QVERIFY(second.name.find("Scene 2") != std::string::npos);
    QVERIFY(ed->addToTimeline(second.id, Rational(20)));
    const Clip* placed = nullptr;
    for (const auto& c : doc->sequence().tracks[0].clips)
        if (c.sourceId == second.id) placed = &c;
    QVERIFY(placed);
    QCOMPARE(placed->sourceIn, Rational(starts[1], 24));
    QCOMPARE(placed->duration, Rational(starts[2] - starts[1], 24));
    d.close();
    shot(&w, "m7_08_scene_library.png");
}

// Auto Reframe on real footage (Tears of Steel, 28-36 s: faces): 9:16, analysed in the background, applied as one undo
// step (canvas and position keyframes), the face inside the crop in the preview, saved and reopened, undone.
void GuiTests::m7AutoReframe() {
    const std::filesystem::path tos = std::filesystem::path(FA_TESTMEDIA_DIR) / "ai" / "tears_of_steel_60s.mov";
    const ai::ModelInfo* yunet = ai::findModel("yunet-face");
    if (!std::filesystem::exists(tos) || ai::modelStatus(*yunet, false, FA_MODELS_DIR) != ai::ModelStatus::Installed)
        QSKIP("Tears of Steel or YuNet missing");
    ai::setModelsDirectoryOverride(std::filesystem::path(FA_MODELS_DIR));
    QTemporaryDir tmp;
    {
        MainWindow w(MainWindow::Options{false, true});
        w.resize(1600, 1000);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        Document* doc = w.document();
        EditController* ed = w.editController();
        QVERIFY(w.menuBar()->findChild<QMenu*>("aiMenu")->actions().contains(w.action("autoReframe")));
        const auto ids = w.importFiles({QString::fromStdWString(tos.wstring())});
        QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
        const std::string v = doc->sequence().tracks[0].clips[0].id;
        QVERIFY(ed->trimClip(v, ClipEdge::Head, Rational(28)));
        QVERIFY(ed->trimClip(v, ClipEdge::Tail, Rational(36)));
        QVERIFY(ed->moveClip(v, Rational(0), std::nullopt));
        doc->setSelection({v});
        QCOMPARE(doc->sequence().width, 1280);
        AutoReframeDialog d(doc, &w);
        d.show();
        QVERIFY(QTest::qWaitForWindowExposed(&d));
        auto* aspect = d.findChild<QComboBox*>("reframeAspect");
        auto* analyse = d.findChild<QPushButton*>("reframeAnalyse");
        auto* apply = d.findChild<QPushButton*>("reframeApply");
        auto* status = d.findChild<QLabel*>("reframeStatus");
        auto* scrub = d.findChild<QSlider*>("reframeScrub");
        QCOMPARE(aspect->currentText(), QString("9:16 Vertical"));
        QTest::mouseClick(analyse, Qt::LeftButton);
        QVERIFY(d.busy());
        QTRY_VERIFY_WITH_TIMEOUT(!d.busy(), 180000);
        qInfo("%s", qPrintable(status->text()));
        QVERIFY(apply->isEnabled());
        const ai::ReframePath path = d.paths().at(v);
        QVERIFY(path.found > int(path.samples.size()) / 3);
        scrub->setValue(scrub->maximum() / 2);
        shot(&d, "m7_09_auto_reframe_dialog.png");
        const size_t undoBefore = doc->undoStack().undoCount();
        QTest::mouseClick(apply, Qt::LeftButton);
        QCOMPARE(doc->undoStack().undoCount(), undoBefore + 1);
        d.close();
        QCOMPARE(doc->sequence().width, 534);
        QCOMPARE(doc->sequence().height, 950);
        const Clip* c = edit::findClip(doc->sequence(), v);
        QVERIFY(c->transform.positionX.isAnimated() || c->transform.positionX.constantValue() != 0.0);
        // the face of a sample with a subject is inside the crop the canvas shows
        for (size_t i = 0; i < path.samples.size(); i += 5) {
            if (!path.samples[i].found) continue;
            const auto win = ai::cropWindowAt(c->transform, c->sourceIn.toDouble() + path.samples[i].t, 1280, 534, 534, 950);
            QVERIFY2(std::abs(path.samples[i].x - win.x) <= win.w / 2, qPrintable(QString("sample %1").arg(i)));
        }
        const int64_t f = 96;   // 32 s into the film: a face
        w.playback()->seek(f);
        const QImage frame = waitForPreview(w, f);
        QVERIFY(!frame.isNull());
        QVERIFY(std::abs(double(frame.width()) / frame.height() - 534.0 / 950) < 0.02);
        shot(&w, "m7_10_auto_reframe_vertical.png");
        // saved and reopened: the same keyframes
        const QString file = tmp.filePath("reframe.lcproj");
        QVERIFY(w.saveProjectTo(file));
        MainWindow w2(MainWindow::Options{false, true});
        w2.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w2));
        QVERIFY(w2.openProjectFile(file));
        QVERIFY(w2.document()->sequence() == doc->sequence());
        // undo: the 16:9 project as before
        w.activateWindow();   // back to the main window (the dialog had the focus), as a click on it would
        QVERIFY(QTest::qWaitForWindowActive(&w));
        QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(doc->sequence().width, 1280);
        QVERIFY(edit::findClip(doc->sequence(), v)->transform.isDefault());
    }
    ai::setModelsDirectoryOverride({});
}

namespace {

double psnrOf(const QImage& x, const QImage& y) {
    const QImage a = x.convertToFormat(QImage::Format_RGB32), b = y.convertToFormat(QImage::Format_RGB32);
    if (a.size() != b.size()) return 0;
    double mse = 0;
    for (int yy = 0; yy < a.height(); ++yy)
        for (int xx = 0; xx < a.width(); ++xx) {
            const QRgb p = a.pixel(xx, yy), q = b.pixel(xx, yy);
            const double dr = qRed(p) - qRed(q), dg = qGreen(p) - qGreen(q), db = qBlue(p) - qBlue(q);
            mse += (dr * dr + dg * dg + db * db) / 3;
        }
    mse /= double(a.width()) * a.height();
    return 10 * std::log10(255.0 * 255.0 / std::max(1e-9, mse));
}

QImage toQImage(const Image& im) {
    return QImage(im.pixels.data(), im.width, im.height, im.width * 4, QImage::Format_RGBA8888).copy();
}

}  // namespace

// AI Tools > AI Interpolation on real footage (the Sintel trailer, source frames 616-625: the city shot, the cut at 621
// into the sky shot): the clip slowed to 0.25x, AI Interpolation (RIFE) chosen in the Speed tab (one undo step);
// until frames are prepared the preview uses optical flow; Prepare AI Frames (mouse, background) computes the
// in-between frames, after which the preview shows exactly the prepared RIFE frame; the frames across the cut are a
// source frame, never a blend of two shots; saved and reopened; undo.
void GuiTests::m7AiInterpolation() {
    const std::filesystem::path sintel = std::filesystem::path(FA_TESTMEDIA_DIR) / "ai" / "sintel_trailer_720p.mp4";
    const ai::ModelInfo* rife = ai::findModel("rife-v4.6");
    if (!std::filesystem::exists(sintel) || ai::modelStatus(*rife, false, FA_MODELS_DIR) != ai::ModelStatus::Installed)
        QSKIP("the Sintel trailer or RIFE missing");
    QTemporaryDir tmp, cacheDir;
    qputenv("MEERCUT_ANALYSIS_CACHE", QDir::toNativeSeparators(cacheDir.path()).toUtf8());
    ai::setModelsDirectoryOverride(std::filesystem::path(FA_MODELS_DIR));
    {
        MainWindow w(MainWindow::Options{false, true});
        w.resize(1600, 1000);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        Document* doc = w.document();
        EditController* ed = w.editController();
        const auto ids = w.importFiles({QString::fromStdWString(sintel.wstring())});
        QCOMPARE(ids.size(), size_t(1));
        QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
        const std::string v = doc->sequence().tracks[0].clips[0].id;
        QVERIFY(ed->trimClip(v, ClipEdge::Head, Rational(616, 24)));
        QVERIFY(ed->trimClip(v, ClipEdge::Tail, Rational(626, 24)));
        QVERIFY(ed->moveClip(v, Rational(0), std::nullopt));
        QCOMPARE(clipOf(doc, v).sourceIn, Rational(616, 24));
        w.timelinePanel()->view()->zoomToFit();
        doc->setSelection({v});
        QApplication::processEvents();

        // the AI menu entry opens the Speed tab
        QMenu* aiMenu = w.menuBar()->findChild<QMenu*>("aiMenu");
        QVERIFY(aiMenu && aiMenu->actions().contains(w.action("aiInterpolation")));
        w.action("aiInterpolation")->trigger();
        QApplication::processEvents();
        SpeedPanel* sp = w.inspector()->speedPanel();
        QVERIFY(sp->isVisible());
        auto* value = sp->findChild<QDoubleSpinBox*>("speedValue");
        auto* interp = sp->findChild<QComboBox*>("speedInterpolation");
        auto* prepare = sp->findChild<QPushButton*>("speedPrepareAi");
        auto* note = sp->findChild<QLabel*>("speedAiNote");
        QVERIFY(value && interp && prepare && note);
        value->setValue(0.25);
        QCOMPARE(clipOf(doc, v).duration, Rational(40, 24));
        const size_t before = doc->undoStack().undoCount();
        const auto oldMode = clipOf(doc, v).speed.interpolation;
        auto* vert = w.findChild<QSplitter*>("verticalSplitter");
        const QList<int> layoutBefore = vert->sizes();
        interp->setCurrentIndex(interp->findText("AI Interpolation (RIFE)"));
        QApplication::processEvents();
        QCOMPARE(vert->sizes(), layoutBefore);   // the AI rows fit the Speed tab: the editor's layout does not move
        QCOMPARE(clipOf(doc, v).speed.interpolation, FrameInterpolation::Neural);
        QCOMPARE(doc->undoStack().undoCount(), before + 1);
        QVERIFY(prepare->isVisible());
        QVERIFY2(note->text().contains("optical flow"), qPrintable(note->text()));

        // output frame 6 = source 617.5: before preparation the preview is drawn (optical flow stands in)
        w.playback()->seek(6);
        const QImage flowed = waitForPreview(w, 6);
        QVERIFY(!flowed.isNull());

        // Prepare AI Frames (mouse): in the background, the editor stays responsive
        QString asked;
        sp->confirmSlow = [&](const QString& text) {
            asked = text;
            return true;
        };
        QTest::mouseClick(prepare, Qt::LeftButton);
        QVERIFY(w.analysis()->busy());
        QTRY_VERIFY_WITH_TIMEOUT(!w.analysis()->busy(), 120000);
        qInfo("asked: '%s'", qPrintable(asked));
        const QImage neural = waitForPreview(w, 6);
        // the stored RIFE frame of source frames 617 / 618 at t = 0.5, at the preview's size
        Image stored;
        const FrameStore store(FrameSetKey{"rife", matteMediaIdentity(sintel), ai::interpolationModelSha(), "v1"});
        QVERIFY(store.read(interpolationFrameName(617, 618, 0.5), stored));
        // scaled as the renderer scales a prepared frame to the preview's decoding size (bilinear)
        const QImage want = toQImage(resizeBilinear(stored, neural.width(), neural.height()));
        const double pn = psnrOf(neural, want), pf = psnrOf(flowed, want);
        qInfo("preview frame 6 against the prepared RIFE frame: %.1f dB (before preparation, optical flow: %.1f dB)", pn, pf);
        // the preview resamples twice (to its decoding size, then to the canvas on the GPU), so it is not pixel-equal to
        // a one-step reduction of the stored frame on this detailed shot; it must be far closer to it than the optical
        // flow frame the preview showed before (measured: 28.7 against 23.8 dB) and differ from that frame
        QVERIFY(pn > pf + 3.0);
        QVERIFY(psnrOf(neural, flowed) < 35.0);
        shot(&w, "m7_11_ai_interpolation.png");
        // across the cut (source 620 -> 621): output frame 18 (t = 0.5) is the sky shot's first frame, not a mix
        w.playback()->seek(18);
        const QImage atCut = waitForPreview(w, 18);
        Image f620, f621;
        {
            VideoDecoder dec(sintel);
            f620 = dec.frameAt(Time(620, 24)).image;
            f621 = dec.frameAt(Time(621, 24)).image;
        }
        Image mix = f620;
        for (size_t i = 0; i < mix.pixels.size(); ++i) mix.pixels[i] = static_cast<uint8_t>((f620.pixels[i] + f621.pixels[i] + 1) / 2);
        auto at = [&](const Image& im) { return psnrOf(atCut, toQImage(resizeBilinear(im, atCut.width(), atCut.height()))); };
        const double p621 = at(f621), p620 = at(f620), pmix = at(mix);
        qInfo("output frame 18 (across the cut): %.1f dB to source frame 621, %.1f dB to 620, %.1f dB to a mix of the two", p621,
              p620, pmix);
        QVERIFY(p621 > p620 + 6.0);
        QVERIFY(p621 > pmix + 3.0);

        // saved and reopened: the mode is kept and the prepared frames are used without preparing again
        const QString file = tmp.filePath("rife.lcproj");
        QVERIFY(w.saveProjectTo(file));
        {
            MainWindow w2(MainWindow::Options{false, true});
            w2.resize(1600, 1000);
            w2.show();
            QVERIFY(QTest::qWaitForWindowExposed(&w2));
            QVERIFY(w2.openProjectFile(file));
            QCOMPARE(w2.document()->sequence().tracks[0].clips[0].speed.interpolation, FrameInterpolation::Neural);
            w2.playback()->seek(6);
            QVERIFY(psnrOf(waitForPreview(w2, 6), neural) > 40.0);
        }
        // undo: the previous interpolation
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        w.action("undo")->trigger();
        QCOMPARE(clipOf(doc, v).speed.interpolation, oldMode);
    }
    ai::setModelsDirectoryOverride({});
    qputenv("MEERCUT_ANALYSIS_CACHE", "");
}

// Video > Upscale on real footage (the Sintel trailer, 1280 x 720, on a 1920 x 1080 canvas): Lanczos (not AI) at
// once; AI (Real-ESRGAN): the general model on 5 s of video asks first, with the time it takes, and "No" starts
// nothing; the anime model on half a second is prepared in the background (mouse), after which the preview shows the
// prepared frames (more detail than the plain enlargement); one undo step per setting; saved and reopened.
void GuiTests::m7VideoUpscaling() {
    const std::filesystem::path sintel = std::filesystem::path(FA_TESTMEDIA_DIR) / "ai" / "sintel_trailer_720p.mp4";
    const ai::ModelInfo* esr = ai::findModel("realesrgan");
    if (!std::filesystem::exists(sintel) || ai::modelStatus(*esr, false, FA_MODELS_DIR) != ai::ModelStatus::Installed)
        QSKIP("the Sintel trailer or Real-ESRGAN missing");
    QTemporaryDir tmp, cacheDir;
    qputenv("MEERCUT_ANALYSIS_CACHE", QDir::toNativeSeparators(cacheDir.path()).toUtf8());
    ai::setModelsDirectoryOverride(std::filesystem::path(FA_MODELS_DIR));
    {
        MainWindow w(MainWindow::Options{false, true});
        w.resize(1600, 1000);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        Document* doc = w.document();
        EditController* ed = w.editController();
        const auto ids = w.importFiles({QString::fromStdWString(sintel.wstring())});
        QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
        QVERIFY(doc->execute(makeModifySequence(doc->sequenceId(), "Canvas", [](Sequence& s) {
            s.width = 1920;
            s.height = 1080;
        })));
        const std::string v = doc->sequence().tracks[0].clips[0].id;
        QVERIFY(ed->trimClip(v, ClipEdge::Head, Rational(75, 2)));   // the desert shot (source 37.5 - 42.5 s)
        QVERIFY(ed->trimClip(v, ClipEdge::Tail, Rational(85, 2)));
        QVERIFY(ed->moveClip(v, Rational(0), std::nullopt));
        w.timelinePanel()->view()->zoomToFit();
        doc->setSelection({v});
        QApplication::processEvents();
        QMenu* aiMenu = w.menuBar()->findChild<QMenu*>("aiMenu");
        QVERIFY(aiMenu && aiMenu->actions().contains(w.action("videoUpscaling")));
        w.action("videoUpscaling")->trigger();
        QApplication::processEvents();
        QCOMPARE(w.inspector()->videoSubTab(), QString("upscale"));
        UpscalePanel* up = w.inspector()->upscalePanel();
        auto* mode = up->findChild<QComboBox*>("upscaleMode");
        auto* model = up->findChild<QComboBox*>("upscaleModel");
        auto* prepare = up->findChild<QPushButton*>("upscalePrepare");
        auto* status = up->findChild<QLabel*>("upscaleStatus");
        QVERIFY(mode && model && prepare && status);
        w.playback()->seek(6);
        const QImage plain = waitForPreview(w, 6);

        // Lanczos: at once, one undo step
        const size_t before = doc->undoStack().undoCount();
        mode->setCurrentIndex(mode->findData(static_cast<int>(UpscaleMode::Lanczos)));
        QCOMPARE(clipOf(doc, v).upscale.mode, UpscaleMode::Lanczos);
        QCOMPARE(doc->undoStack().undoCount(), before + 1);
        QVERIFY2(status->text().contains("1280 x 720 enlarged to 1920 x 1080"), qPrintable(status->text()));
        QVERIFY(!waitForPreview(w, 6).isNull());

        // AI, general model, the whole 5 s clip: asked first (minutes of work), "No" starts nothing
        mode->setCurrentIndex(mode->findData(static_cast<int>(UpscaleMode::Ai)));
        model->setCurrentIndex(model->findData("general"));
        QCOMPARE(clipOf(doc, v).upscale.model, std::string("general"));
        QVERIFY(prepare->isVisible());
        QString asked;
        up->confirmSlow = [&](const QString& text) {
            asked = text;
            return false;
        };
        QTest::mouseClick(prepare, Qt::LeftButton);
        qInfo("asked: '%s'", qPrintable(asked));
        QVERIFY2(asked.contains("120 upscaled frame(s)") && asked.contains("about"), qPrintable(asked));
        QVERIFY(!w.analysis()->busy());
        {   // the export asks too (it would prepare the same frames first); "No" writes nothing
            ExportDialog d(doc, &w);
            QTRY_VERIFY_WITH_TIMEOUT(d.encodersChecked(), 60000);
            QString exportAsked, err;
            d.confirmSlow = [&](const QString& text) {
                exportAsked = text;
                return false;
            };
            const QString refused = tmp.filePath("refused.mp4");
            QVERIFY(!d.exportTo(refused, &err));
            qInfo("export asked: '%s'", qPrintable(exportAsked));
            QVERIFY2(exportAsked.contains("120 upscaled frame(s)"), qPrintable(exportAsked));
            QVERIFY2(err.contains("not started"), qPrintable(err));
            QVERIFY(!QFileInfo::exists(refused));
        }

        // the anime model on half a second: prepared in the background, then shown
        model->setCurrentIndex(model->findData("anime"));
        QVERIFY(ed->trimClip(v, ClipEdge::Tail, Rational(1, 2)));
        asked.clear();
        up->confirmSlow = [&](const QString& text) {
            asked = text;
            return true;
        };
        QTest::mouseClick(prepare, Qt::LeftButton);
        QVERIFY(w.analysis()->busy());
        QTRY_VERIFY_WITH_TIMEOUT(!w.analysis()->busy(), 120000);
        QTRY_VERIFY_WITH_TIMEOUT(!status->text().contains("Preparing"), 5000);
        w.playback()->seek(6);
        const QImage upscaled = waitForPreview(w, 6);
        auto detail = [](const QImage& im) {
            const QImage g = im.convertToFormat(QImage::Format_Grayscale8);
            double sum = 0;
            for (int y = 1; y < g.height(); ++y) {
                const uchar* r = g.constScanLine(y);
                const uchar* r0 = g.constScanLine(y - 1);
                for (int x = 1; x < g.width(); ++x) sum += std::abs(r[x] - r[x - 1]) + std::abs(r[x] - r0[x]);
            }
            return sum / (double(g.width()) * g.height());
        };
        const Image stored = [&] {
            Image im;
            const ClipUpscale& u = clipOf(doc, v).upscale;
            const FrameStore store(FrameSetKey{"upscale", matteMediaIdentity(sintel), ai::upscaleModelSha(u.model, u.scale),
                                               u.model + "|" + std::to_string(u.scale) + "|1920x1080|" + std::to_string(u.deflicker)});
            store.read(std::to_string(900 + 6), im);
            return im;
        }();
        QVERIFY(stored.width == 1920 && stored.height == 1080);
        const QImage want = toQImage(resizeBilinear(stored, upscaled.width(), upscaled.height()));
        const double pu = psnrOf(upscaled, want), pp = psnrOf(plain, want);
        const double changed = psnrOf(upscaled, plain);
        qInfo("preview against the prepared Real-ESRGAN frame: %.1f dB (the plain preview: %.1f dB; the two previews: %.1f dB); "
              "detail: plain %.2f, upscaled %.2f", pu, pp, changed, detail(plain), detail(upscaled));
        // the preview shows the prepared frame (resampled to the panel), not the plain one: before the renderer looked
        // up frames prepared after it had missed them, the two previews were identical (pu == pp). Pixel equality at
        // full size is checked by AiUpscale.PreparedFramesInPreviewAndExport.
        QVERIFY(pu > pp + 1.5);
        QVERIFY(changed < 40.0);
        shot(&w, "m7_12_video_upscaling.png");

        // saved and reopened
        const QString file = tmp.filePath("upscale.lcproj");
        QVERIFY(w.saveProjectTo(file));
        {
            MainWindow w2(MainWindow::Options{false, true});
            w2.show();
            QVERIFY(QTest::qWaitForWindowExposed(&w2));
            QVERIFY(w2.openProjectFile(file));
            const Clip& c2 = w2.document()->sequence().tracks[0].clips[0];
            QCOMPARE(c2.upscale.mode, UpscaleMode::Ai);
            QCOMPARE(c2.upscale.model, std::string("anime"));
        }
    }
    ai::setModelsDirectoryOverride({});
    qputenv("MEERCUT_ANALYSIS_CACHE", "");
}

namespace {

// PSNR of the luma (BT.709): the export stores colour at half resolution (4:2:0), so sharp saturated edges differ in
// RGB however exact the rendering is; the luma shows whether the pictures are the same.
double lumaPsnr(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height) return 0;
    double mse = 0;
    for (size_t i = 0; i < a.pixels.size(); i += 4) {
        const double ya = 0.2126 * a.pixels[i] + 0.7152 * a.pixels[i + 1] + 0.0722 * a.pixels[i + 2];
        const double yb = 0.2126 * b.pixels[i] + 0.7152 * b.pixels[i + 1] + 0.0722 * b.pixels[i + 2];
        mse += (ya - yb) * (ya - yb);
    }
    return 10 * std::log10(255.0 * 255.0 / std::max(1e-9, mse / (a.pixels.size() / 4)));
}

double psnrImages(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height) return 0;
    double mse = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.pixels.size(); i += 4)
        for (int c = 0; c < 3; ++c, ++n) {
            const double d = double(a.pixels[i + c]) - b.pixels[i + c];
            mse += d * d;
        }
    return 10 * std::log10(255.0 * 255.0 / std::max(1e-9, mse / n));
}

}  // namespace

// M7 F integration: one project uses every M7 feature together with the earlier milestones - Background Removal
// (RVM) over a blurred, masked background clip, a dissolve into a clip slowed down with AI Interpolation (RIFE), an
// AI-upscaled low-resolution clip, a title, a caption on a subtitle track and the dialogue's audio - and is exported
// through the editor's own export window (which prepares the AI pictures first, after asking). Every checked
// exported frame equals the editor's renderer with the same prepared pictures; the AI work visibly took effect; the
// sound is there; the project saves and reopens identical; undo / redo move the AI caches' state along (nothing is
// computed again for a state that was prepared before).
void GuiTests::m7IntegrationEveryFeature() {
    const std::filesystem::path ai = std::filesystem::path(FA_TESTMEDIA_DIR) / "ai";
    for (const char* id : {"rvm-mobilenetv3", "rife-v4.6", "realesrgan"})
        if (ai::modelStatus(*ai::findModel(id), false, FA_MODELS_DIR) != ai::ModelStatus::Installed) QSKIP("an M7 model is missing");
    if (!std::filesystem::exists(ai / "sintel_trailer_720p.mp4") || !std::filesystem::exists(ai / "tears_of_steel_60s.mov"))
        QSKIP("testmedia/ai missing");
    QTemporaryDir tmp, cacheDir;
    qputenv("MEERCUT_ANALYSIS_CACHE", QDir::toNativeSeparators(cacheDir.path()).toUtf8());
    ai::setModelsDirectoryOverride(std::filesystem::path(FA_MODELS_DIR));
    // a low-resolution source (320 x 180) made from the trailer's desert shot, for the upscaling
    const std::filesystem::path low = std::filesystem::path(tmp.filePath("low_desert.mp4").toStdWString());
    {
        Project lp;
        MediaSource ms;
        ms.id = "s";
        const std::u8string u8 = (ai / "sintel_trailer_720p.mp4").u8string();
        ms.path = std::string(u8.begin(), u8.end());
        ms.duration = Rational(1253, 24);
        ms.frameRate = FrameRate(24);
        lp.sources.push_back(ms);
        Sequence q;
        q.id = "seq";
        q.width = 320;
        q.height = 180;
        q.frameRate = FrameRate(24);
        Track t{"v1", TrackKind::Video, "Video 1"};
        Clip c;
        c.id = "c";
        c.sourceId = "s";
        c.sourceIn = Rational(900, 24);
        c.duration = Rational(1);
        t.clips.push_back(c);
        q.tracks.push_back(t);
        lp.sequences.push_back(q);
        RenderOptions o;
        o.includeAudio = false;
        o.encoder.crf = 0;
        renderProject(lp, "", low, o);
    }
    {
        MainWindow w(MainWindow::Options{false, true});
        w.resize(1600, 1000);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        Document* doc = w.document();
        EditController* ed = w.editController();
        const auto ids = w.importFiles({mediaPath("C_testsrc2_1080p30_10s.mp4"), QString::fromStdWString((ai / "tears_of_steel_60s.mov").wstring()),
                                        QString::fromStdWString((ai / "sintel_trailer_720p.mp4").wstring()),
                                        QString::fromStdWString(low.wstring())});
        QCOMPARE(ids.size(), size_t(4));
        const std::string bgId = ids[0], tosId = ids[1], sintelId = ids[2], lowId = ids[3];
        // the project is written as a file and opened the way File > Open does
        Project built = doc->project();
        {
            Sequence& q = built.sequences[0];
            q.width = 1280;
            q.height = 720;
            q.frameRate = FrameRate(24);
            q.tracks.clear();
            Track v1{"v1", TrackKind::Video, "Video 1"}, v2{"v2", TrackKind::Video, "Video 2"}, v3{"v3", TrackKind::Video, "Video 3"},
                v4{"v4", TrackKind::Video, "Video 4"}, a1{"a1", TrackKind::Audio, "Audio 1"}, s1{"s1", TrackKind::Subtitle, "Subtitles 1"};
            Clip bg;   // M4: a blurred background with an elliptical mask on the effect
            bg.id = "bg";
            bg.sourceId = bgId;
            bg.duration = Rational(4);
            EffectRef blur = makeEffect(*findEffectDef("fa.fx.blur.gaussian"), "fx1");
            blur.params["radius"] = Animatable<double>(24.0);
            Mask m;
            m.id = "m1";
            m.shape = MaskShape::Ellipse;
            m.width = Animatable<double>(0.6);
            m.height = Animatable<double>(0.6);
            blur.masks.push_back(m);
            bg.effects.push_back(blur);
            v1.clips.push_back(bg);
            Clip tos;   // M7 B: Background Removal (RVM), M1: a dissolve into the next clip
            tos.id = "tos";
            tos.sourceId = tosId;
            tos.sourceIn = Rational(10);
            tos.duration = Rational(2);
            tos.aiCutout.enabled = true;
            tos.aiCutout.quality = 0;
            tos.outTransition = TransitionRef{"fa.transition.dissolve", Rational(1, 2)};
            v2.clips.push_back(tos);
            Clip sin;   // M5 + M7 E: 0.5x with AI Interpolation (RIFE)
            sin.id = "sin";
            sin.sourceId = sintelId;
            sin.sourceIn = Rational(590, 24);
            sin.timelineStart = Rational(2);
            sin.speed.speed = Rational(1, 2);
            sin.speed.interpolation = FrameInterpolation::Neural;
            sin.duration = Rational(24, 24);
            v2.clips.push_back(sin);
            Clip up;   // M7 E: AI upscaling (anime model) of the 320 x 180 clip
            up.id = "up";
            up.sourceId = lowId;
            up.timelineStart = Rational(3);
            up.duration = Rational(1);
            up.upscale.mode = UpscaleMode::Ai;
            up.upscale.model = "anime";
            v3.clips.push_back(up);
            Clip title;   // M3: a title
            title.id = "title";
            title.duration = Rational(3, 2);
            TitleData td;
            td.text = "MeerCut M7";
            td.style.size = 72;
            title.title = td;
            title.transform.positionY.setConstant(-0.3);
            v4.clips.push_back(title);
            Clip cap;   // M3: a caption (what Speech to Text writes)
            cap.id = "cap";
            cap.timelineStart = Rational(1);
            cap.duration = Rational(2);
            TitleData ct;
            ct.text = "Captions over AI pictures";
            ct.followTrackStyle = true;
            cap.title = ct;
            s1.clips.push_back(cap);
            Clip snd;   // M6: the dialogue's sound
            snd.id = "snd";
            snd.sourceId = tosId;
            snd.sourceIn = Rational(10);
            snd.duration = Rational(2);
            a1.clips.push_back(snd);
            q.tracks = {v1, v2, v3, v4, a1, s1};
        }
        const QString projectFile = tmp.filePath("integration_source.lcproj");
        {
            QFile f(projectFile);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray::fromStdString(projectToJsonString(built)));
        }
        QVERIFY(w.openProjectFile(projectFile));
        QVERIFY2(!hasErrors(validate(doc->project())), formatIssues(validate(doc->project())).c_str());
        const std::string seqId = doc->sequenceId();
        const ai::PreparationEstimate before = ai::estimatePreparation(doc->project(), seqId, "", 0, -1);
        qInfo("to prepare: %s", before.text.c_str());
        // 48 frames of the clip + 6 the dissolve shows past its end (half of 0.5 s at 24 fps)
        QVERIFY(before.mattes == 54 && before.neuralFrames > 0 && before.upscaled == 24);

        // export through the editor's export window: it asks (only when long) and prepares the AI pictures first
        const QString out = tmp.filePath("integration.mp4");
        {
            ExportDialog d(doc, &w);
            QTRY_VERIFY_WITH_TIMEOUT(d.encodersChecked(), 60000);
            d.confirmSlow = [](const QString&) { return true; };
            // a high bit rate, so that the comparison below measures preview = export and not the compression
            auto* quality = d.findChild<QComboBox*>("exportQuality");
            auto* bitrate = d.findChild<QSpinBox*>("exportBitrate");
            QVERIFY(quality && bitrate);
            quality->setCurrentIndex(quality->findData(-1));
            bitrate->setValue(80000);
            QString err;
            QVERIFY2(d.exportTo(out, &err), qPrintable(err));
        }
        QVERIFY(ai::estimatePreparation(doc->project(), seqId, "", 0, -1).frames() == 0);   // all prepared now
        const MediaInfo info = probeMedia(std::filesystem::path(out.toStdWString()));
        QVERIFY(info.video && info.audio);
        QVERIFY2(std::abs(info.duration->toDouble() - 4.0) < 0.05, qPrintable(QString::number(info.duration->toDouble())));

        // exported frames against the editor's renderer (full size, CPU) with the same prepared pictures
        const Project& proj = doc->project();
        const Sequence& seq = *proj.findSequence(seqId);
        DecoderPool pool("");
        const auto backend = makeBackend(BackendChoice::Cpu);
        auto titles = text::makeTitleEngine();
        VideoDecoder dec(std::filesystem::path(out.toStdWString()));
        Project plain = proj;   // the same project without its AI settings: the AI must visibly change the picture
        for (auto& t : plain.sequences[0].tracks)
            for (auto& c : t.clips) {
                c.aiCutout = AiCutout{};
                c.upscale = ClipUpscale{};
                if (c.speed.interpolation == FrameInterpolation::Neural) c.speed.interpolation = FrameInterpolation::Blending;
            }
        DecoderPool plainPool("");
        struct Check {
            int64_t frame;
            const char* what;
        };
        for (const Check k : {Check{12, "cutout + title"}, Check{44, "dissolve: cutout into RIFE"}, Check{55, "RIFE in-between frame"},
                              Check{84, "AI-upscaled clip"}}) {
            const Image exported = dec.frameAt(Time(k.frame, 24)).image;
            const Image preview = renderFrame(seq, k.frame, proj, pool, *backend, nullptr, nullptr, 0, 0, titles.get());
            const Image noAi = renderFrame(plain.sequences[0], k.frame, plain, plainPool, *backend, nullptr, nullptr, 0, 0, titles.get());
            const double pe = lumaPsnr(exported, preview), pa = psnrImages(preview, noAi);
            qInfo("frame %lld (%s): export vs editor %.1f dB luma (%.1f dB RGB, 4:2:0); with vs without AI %.1f dB",
                  static_cast<long long>(k.frame), k.what, pe, psnrImages(exported, preview), pa);
            QVERIFY(pe > 45.0);
            if (k.frame != 44) QVERIFY(pa < 40.0);   // the AI picture is not the plain one
        }
        QVERIFY(pool.clipsMissingMattes().empty() && pool.clipsMissingNeuralFrames().empty() && pool.clipsMissingUpscales().empty());
        {   // the masked blur on the background took effect (M4 inside the same picture)
            Project noFx = proj;
            noFx.sequences[0].tracks[0].clips[0].effects.clear();
            DecoderPool fxPool("");
            const Image with = renderFrame(seq, 12, proj, pool, *backend, nullptr, nullptr, 0, 0, titles.get());
            const Image without = renderFrame(noFx.sequences[0], 12, noFx, fxPool, *backend, nullptr, nullptr, 0, 0, titles.get());
            qInfo("frame 12 (the background shows around the cutout) with / without the masked blur: %.1f dB", psnrImages(with, without));
            QVERIFY(psnrImages(with, without) < 35.0);
        }

        // saved and reopened: the same project
        const QString file = tmp.filePath("integration.lcproj");
        QVERIFY(w.saveProjectTo(file));
        {
            MainWindow w2(MainWindow::Options{false, true});
            w2.show();
            QVERIFY(QTest::qWaitForWindowExposed(&w2));
            QVERIFY(w2.openProjectFile(file));
            QVERIFY(w2.document()->project().sequences == doc->project().sequences);
        }

        // undo / redo: the caches follow the project's state
        ClipSpeed slower = edit::findClip(doc->sequence(), "sin")->speed;
        slower.speed = Rational(1, 4);
        QVERIFY(ed->setClipSpeed("sin", slower));
        const int64_t newFrames = ai::estimatePreparation(doc->project(), seqId, "", 0, -1).neuralFrames;
        qInfo("0.25x needs %lld new in-between frames", static_cast<long long>(newFrames));
        QVERIFY(newFrames > 0);
        w.action("undo")->trigger();
        QCOMPARE(ai::estimatePreparation(doc->project(), seqId, "", 0, -1).frames(), int64_t(0));   // the prepared state
        w.action("redo")->trigger();
        QCOMPARE(ai::estimatePreparation(doc->project(), seqId, "", 0, -1).neuralFrames, newFrames);
        Project copy = doc->project();
        QVERIFY(ai::prepareForRender(copy, seqId, "", 0, -1).neuralFrames == newFrames);
        w.action("undo")->trigger();
        w.action("redo")->trigger();
        QCOMPARE(ai::estimatePreparation(doc->project(), seqId, "", 0, -1).frames(), int64_t(0));   // both states cached
    }
    ai::setModelsDirectoryOverride({});
    qputenv("MEERCUT_ANALYSIS_CACHE", "");
}

// The example projects of earlier milestones (editor/examples) still open in the M7 editor without problems and
// render their first and middle frames (old project versions are read unchanged).
void GuiTests::m7OlderExampleProjects() {
    const QDir dir(QString::fromUtf8(FA_REPO_ROOT) + "/editor/examples");
    const QStringList files = dir.entryList({"*.json"}, QDir::Files, QDir::Name);
    QVERIFY(files.size() >= 4);
    int checked = 0;
    for (const QString& f : files) {
        if (f.startsWith("m7_")) continue;   // M7's own examples are covered by their generator
        MainWindow w(MainWindow::Options{false, true});
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        const QString path = dir.filePath(f);
        QVERIFY2(w.openProjectFile(path), qPrintable(f));
        const Project& p = w.document()->project();
        QVERIFY2(!hasErrors(validate(p)), qPrintable(f + ": " + QString::fromStdString(formatIssues(validate(p)))));
        const Sequence& seq = w.document()->sequence();
        DecoderPool pool(std::filesystem::path(path.toStdWString()));
        const auto backend = makeBackend(BackendChoice::Cpu);
        auto titles = text::makeTitleEngine();
        const int64_t mid = timeToFrame(seq.duration(), seq.frameRate, Rounding::Floor) / 2;
        for (int64_t frame : {int64_t(0), mid}) {
            const Image img = renderFrame(seq, frame, p, pool, *backend, nullptr, nullptr, 0, 0, titles.get());
            QVERIFY2(img.width == seq.width && img.height == seq.height, qPrintable(f));
        }
        qInfo("%s: opens, validates and renders (%d x %d, %lld frames)", qPrintable(f), seq.width, seq.height,
              static_cast<long long>(timeToFrame(seq.duration(), seq.frameRate, Rounding::Floor)));
        ++checked;
    }
    QVERIFY(checked >= 4);
}

// Every M7 tool can be found where a user looks: the AI Tools menu (AI models first, then "Signal analysis (no AI
// model)" for Scene Detection, and the Model Manager) and the clip's right-click menu (AI Tools submenu, only the tools
// that apply: a video clip offers all six, its linked audio clip Speech to Text only); the entries lead to the panels.
void GuiTests::m7AiToolsAreDiscoverable() {
    const std::filesystem::path tos = std::filesystem::path(FA_TESTMEDIA_DIR) / "ai" / "tears_of_steel_60s.mov";
    if (!std::filesystem::exists(tos)) QSKIP("testmedia/ai missing");
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({QString::fromStdWString(tos.wstring())});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    w.timelinePanel()->view()->zoomToFit();
    QApplication::processEvents();
    const std::string v = doc->sequence().tracks[0].clips[0].id, a = doc->sequence().tracks[1].clips[0].id;

    // the menu bar's AI Tools
    QMenu* aiMenu = w.menuBar()->findChild<QMenu*>("aiMenu");
    QVERIFY(aiMenu);
    QStringList menuTexts;
    for (QAction* x : aiMenu->actions()) menuTexts << (x->isSeparator() ? "|" + x->text() : x->text());
    qInfo("AI Tools menu: %s", qPrintable(menuTexts.join(" / ")));
    for (const char* name : {"aiCutout", "speechToText", "autoReframe", "aiInterpolation", "videoUpscaling", "sceneDetection", "modelManager"})
        QVERIFY2(aiMenu->actions().contains(w.action(name)), name);
    const int analysis = menuTexts.indexOf("|Signal analysis (no AI model)");
    QVERIFY(analysis > 0 && menuTexts.indexOf(w.action("sceneDetection")->text()) > analysis);   // not listed as AI

    // the clip menu, opened the way a right-click opens it; read while it is shown, then closed
    TimelineView* tl = w.timelinePanel()->view();
    auto clipMenu = [&](const std::string& id) {
        QStringList texts;
        QTimer::singleShot(300, [&] {
            if (auto* popup = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                if (auto* sub = popup->findChild<QMenu*>("clipAiMenu"))
                    for (QAction* x : sub->actions()) texts << (x->isSeparator() ? "|" + x->text() : x->text());
                popup->close();
            }
        });
        const QPoint at = tl->clipRect(id).center();
        QContextMenuEvent ev(QContextMenuEvent::Mouse, at, tl->mapToGlobal(at));
        QApplication::sendEvent(tl, &ev);
        return texts;
    };
    const QStringList videoTools = clipMenu(v);
    qInfo("video clip > AI Tools: %s", qPrintable(videoTools.join(" / ")));
    QCOMPARE(videoTools, QStringList({"Background Removal...", "Speech to Text...", "Auto Reframe...", "AI Interpolation...",
                                      "Video Upscaling...", "|Signal analysis", "Scene Detection..."}));
    const QStringList audioTools = clipMenu(a);
    qInfo("audio clip > AI Tools: %s", qPrintable(audioTools.join(" / ")));
    QCOMPARE(audioTools, QStringList({"Speech to Text..."}));

    // the entries lead to their panels (the dialogs of the other tools are covered by their own tests)
    doc->setSelection({v});
    emit tl->aiToolRequested("aiCutout");
    QCOMPARE(w.inspector()->videoSubTab(), QString("cutout"));
    emit tl->aiToolRequested("videoUpscaling");
    QCOMPARE(w.inspector()->videoSubTab(), QString("upscale"));
    emit tl->aiToolRequested("aiInterpolation");
    QVERIFY(w.inspector()->speedPanel()->isVisible());
}

// A double-click on a media item in the library puts it on the timeline (the way the practical checks add media),
// also straight after the import while its thumbnail is still being made, and a second double-click appends.
void GuiTests::libraryDoubleClickAddsToTimeline() {
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4")});
    QCOMPARE(ids.size(), size_t(1));
    QListWidget* list = nullptr;
    for (auto* l : w.findChildren<QListWidget*>())
        if (l->accessibleName() == "Media library") list = l;
    QVERIFY(list);
    auto itemOf = [&](const std::string& id) -> QListWidgetItem* {
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->data(Qt::UserRole + 1).toString() == QString::fromStdString(id)) return list->item(i);
        return nullptr;
    };
    QListWidgetItem* it = itemOf(ids[0]);
    QVERIFY(it);
    const QPoint at = list->visualItemRect(it).center();
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, at);   // what precedes a double-click
    QTest::mouseDClick(list->viewport(), Qt::LeftButton, {}, at);
    QTRY_COMPARE(doc->sequence().tracks[0].clips.size(), size_t(1));
    QCOMPARE(doc->sequence().tracks[0].clips[0].sourceId, ids[0]);
    QTest::qWait(600);   // the thumbnail has arrived by now: the list updated in place
    it = itemOf(ids[0]);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(it).center());
    QTest::mouseDClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(it).center());
    QTRY_COMPARE(doc->sequence().tracks[0].clips.size(), size_t(2));
    QCOMPARE(doc->sequence().tracks[0].clips[1].timelineStart, Rational(10));   // appended after the first
}

// Opening the editor with a file (fa-editor.exe <file>: the window, then the import) keeps the window responsive:
// the longest event-loop stall in the first 8 s, and a double-click on the new library item adds the clip promptly.
// (The M7 practical check saw its double-clicks take effect only seconds later on a loaded laptop.)
void GuiTests::startupImportStaysResponsive() {
    const std::filesystem::path tos = std::filesystem::path(FA_TESTMEDIA_DIR) / "ai" / "tears_of_steel_60s.mov";
    if (!std::filesystem::exists(tos)) QSKIP("testmedia/ai missing");
    QElapsedTimer clock;
    clock.start();
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    const qint64 shownMs = clock.elapsed();
    qint64 last = clock.elapsed(), worst = 0, worstAt = 0;
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, [&] {
        const qint64 now = clock.elapsed();
        if (now - last > worst) {
            worst = now - last;
            worstAt = now;
        }
        last = now;
    });
    tick.start(10);
    const auto ids = w.importFiles({QString::fromStdWString(tos.wstring())});
    const qint64 importMs = clock.elapsed() - last;   // importFiles itself runs on the GUI thread
    QCOMPARE(ids.size(), size_t(1));
    QListWidget* list = nullptr;
    for (auto* l : w.findChildren<QListWidget*>())
        if (l->accessibleName() == "Media library") list = l;
    QVERIFY(list);
    QListWidgetItem* it = nullptr;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->data(Qt::UserRole + 1).toString() == QString::fromStdString(ids[0])) it = list->item(i);
    QVERIFY(it);
    QTest::qWait(300);
    // the clip (1280 x 534, 24 fps) differs from the new project (1920 x 1080, 30 fps): an interactive window asks,
    // as Filmora does, whether to match the project to it (the M7 practical check answers that question with the
    // mouse); this test window matches without asking
    const qint64 t0 = clock.elapsed();
    const QPoint at = list->visualItemRect(it).center();
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, at);   // a double-click's first press (as an OS sends it)
    QTest::mouseDClick(list->viewport(), Qt::LeftButton, {}, at);
    QTRY_VERIFY_WITH_TIMEOUT(!w.document()->sequence().tracks[0].clips.empty(), 10000);
    const qint64 addMs = clock.elapsed() - t0;
    while (clock.elapsed() < 8000) QTest::qWait(50);
    qInfo("window shown after %lld ms; importFiles took %lld ms on the GUI thread; the clip on the timeline %lld ms after "
          "the double-click; longest event-loop stall in the first 8 s: %lld ms (at %lld ms)",
          static_cast<long long>(shownMs), static_cast<long long>(importMs), static_cast<long long>(addMs),
          static_cast<long long>(worst), static_cast<long long>(worstAt));
    QCOMPARE(w.document()->sequence().width, 1280);   // matched to the media
    QVERIFY(addMs < 1000);
    QVERIFY(worst < 500);
}

// M8 autosave and crash recovery: a window with unsaved changes writes an autosaved copy; a window that ends without
// closing normally (destroyed without its close event - what a crash leaves) is offered for recovery by the next one;
// the recovered project opens as unsaved changes of its own file, with its clips and media; saving removes the copy;
// undoing back to the saved state removes it; a normal close removes it; a running window's copy is never offered;
// Discard keeps the copy in discarded/; a damaged copy is reported, not opened.
void GuiTests::autosaveAndCrashRecovery() {
    QTemporaryDir dir, work;
    qputenv("MEERCUT_AUTOSAVE_DIR", dir.path().toUtf8());
    auto countCopies = [&](const QString& sub = QString()) {
        return QDir(sub.isEmpty() ? dir.path() : dir.filePath(sub)).entryList({"*.mcproj", "*.lcproj"}, QDir::Files).size();
    };
    const QString saved = work.filePath("story.lcproj");
    {   // Edit > Preferences > Recovery: every minute by default, 0 = off
        PreferencesDialog d(RenderSettings{}, "CPU");
        auto* minutes = d.findChild<QSpinBox*>("prefAutosaveMinutes");
        QVERIFY(minutes);
        QCOMPARE(minutes->value(), 1);
        QCOMPARE(minutes->specialValueText(), QString("Off"));
        minutes->setValue(0);
        QCOMPARE(d.settings().autosaveMinutes, 0);
    }
    {   // a saved project, then more edits that were never saved; the window "crashes"
        MainWindow w(MainWindow::Options{false, true});
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        AutosaveManager* as = w.findChild<AutosaveManager*>();
        QVERIFY(as);
        QCOMPARE(as->intervalSeconds(), 0);   // off in test windows unless turned on
        const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4"), mediaPath("B_blue_1080p30_10s.mp4")});
        QVERIFY(w.editController()->addToTimeline(ids[0], Rational(0)));
        QVERIFY(w.saveProjectTo(saved));
        QVERIFY(!as->autosaveNow());   // nothing unsaved: no copy
        QCOMPARE(countCopies(), 0);
        QVERIFY(w.editController()->addToTimeline(ids[1], Rational(10)));
        QVERIFY(as->autosaveNow());
        QCOMPARE(countCopies(), 1);
        QVERIFY(!as->autosaveNow());   // unchanged since the last copy
        // undo to the saved state: the copy is not needed; redo: unsaved again
        w.action("undo")->trigger();
        QVERIFY(!w.document()->isModified());
        QCOMPARE(countCopies(), 0);
        w.action("redo")->trigger();
        QVERIFY(as->autosaveNow());
        // a second window (another running MeerCut) does not offer a running session's copy
        MainWindow other(MainWindow::Options{false, true});
        QVERIFY(other.findChild<AutosaveManager*>()->recoverableSessions().empty());
    }   // destroyed without closeEvent: like a crash, the copy stays and the lock is gone
    QCOMPARE(countCopies(), 1);
    {
        MainWindow w(MainWindow::Options{false, true});
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        AutosaveManager* as = w.findChild<AutosaveManager*>();
        const auto sessions = as->recoverableSessions();
        QCOMPARE(sessions.size(), size_t(1));
        QCOMPARE(QFileInfo(sessions[0].originalPath).fileName(), QString("story.lcproj"));
        QCOMPARE(sessions[0].displayName, QString("story"));
        QString err;
        QVERIFY2(as->recover(sessions[0], &err), qPrintable(err));
        QVERIFY(w.document()->isModified());   // unsaved changes of story.lcproj
        QCOMPARE(QFileInfo(w.document()->filePath()).fileName(), QString("story.lcproj"));
        QCOMPARE(w.document()->sequence().tracks[0].clips.size(), size_t(2));   // the unsaved second clip is back
        QVERIFY(as->recoverableSessions().empty());
        QCOMPARE(countCopies(), 1);   // now this session's copy of the recovered changes
        QVERIFY(w.saveProjectTo(saved));
        QCOMPARE(countCopies(), 0);   // saved: no copy
        QVERIFY(!w.document()->isModified());
        // a normal close removes the session's copy
        QVERIFY(w.editController()->addToTimeline(w.document()->project().sources[0].id, Rational(20)));
        QVERIFY(as->autosaveNow());
        QCOMPARE(countCopies(), 1);
        w.close();
        QCOMPARE(countCopies(), 0);
    }
    {   // Discard keeps the copy in discarded/; a damaged copy is reported and not opened
        {
            MainWindow w(MainWindow::Options{false, true});
            const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4")});
            QVERIFY(w.editController()->addToTimeline(ids[0], Rational(0)));
            QVERIFY(w.findChild<AutosaveManager*>()->autosaveNow());
        }
        MainWindow w(MainWindow::Options{false, true});
        AutosaveManager* as = w.findChild<AutosaveManager*>();
        auto sessions = as->recoverableSessions();
        QCOMPARE(sessions.size(), size_t(1));
        QCOMPARE(sessions[0].originalPath, QString());   // never saved
        QVERIFY(as->discard(sessions[0]));
        QCOMPARE(countCopies(), 0);
        QCOMPARE(countCopies("discarded"), 1);
        {
            MainWindow v(MainWindow::Options{false, true});
            const auto ids = v.importFiles({mediaPath("A_red_1080p30_10s.mp4")});
            QVERIFY(v.editController()->addToTimeline(ids[0], Rational(0)));
            QVERIFY(v.findChild<AutosaveManager*>()->autosaveNow());
        }
        sessions = as->recoverableSessions();
        QCOMPARE(sessions.size(), size_t(1));
        QFile damaged(sessions[0].projectFile);
        QVERIFY(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
        damaged.write("{\"format\": \"fa-project\", \"version\": ");   // cut off mid-file
        damaged.close();
        QString err;
        QVERIFY(!as->recover(sessions[0], &err));
        qInfo("damaged copy: %s", qPrintable(err));
        QVERIFY(!err.isEmpty());
        QCOMPARE(w.document()->sequence().tracks[0].clips.size(), size_t(0));   // the open project is untouched
    }
    {   // a real crash leaves its lock file behind, naming a process that no longer runs: still offered
        QProcess gone;
        gone.start("cmd.exe", {"/c", "exit"});
        QVERIFY(gone.waitForStarted());
        const qint64 deadPid = gone.processId();
        QVERIFY(gone.waitForFinished());
        const QString id = "20260101-120000-" + QString::number(deadPid) + "-1";
        QVERIFY(QFile::copy(saved, dir.filePath(id + ".lcproj")));   // named as Lumacut named its copies
        QFile meta(dir.filePath(id + ".json"));
        QVERIFY(meta.open(QIODevice::WriteOnly));
        meta.write(R"({"originalPath": "", "displayName": "crashed", "savedAt": "2026-01-01T12:00:00"})");
        meta.close();
        {   // a lock file exactly as QLockFile writes it, with the dead process's id in place of ours
            QLockFile mine(dir.filePath("mine.lock"));
            QVERIFY(mine.tryLock(0));
            QFile f(dir.filePath("mine.lock"));
            QVERIFY(f.open(QIODevice::ReadOnly));
            QByteArray contents = f.readAll();
            f.close();
            QCOMPARE(contents.left(contents.indexOf('\n')), QByteArray::number(QCoreApplication::applicationPid()));
            contents.replace(0, contents.indexOf('\n'), QByteArray::number(deadPid));
            QFile lock(dir.filePath(id + ".lock"));
            QVERIFY(lock.open(QIODevice::WriteOnly));
            lock.write(contents);
        }
        QVERIFY(QFileInfo::exists(dir.filePath(id + ".lock")));
        MainWindow w(MainWindow::Options{false, true});
        const auto sessions = w.findChild<AutosaveManager*>()->recoverableSessions();
        QVERIFY(std::any_of(sessions.begin(), sessions.end(), [&](const auto& s) { return s.id == id; }));
    }
    qputenv("MEERCUT_AUTOSAVE_DIR", QDir(QDir::tempPath()).filePath("meercut_gui_tests_autosave").toUtf8());
}

// Stage 3: a long project (3 video tracks and 1 audio track of 1000 clips each, 300 captions) opens, paints, scrolls,
// seeks and edits without stalling the interface. Measured times are printed (docs/STAGE3_AUDIT.md section 4).
void GuiTests::longProjectStaysResponsive() {
    QTemporaryDir dir;
    const QString file = dir.filePath("long.lcproj");
    {
        MainWindow w(MainWindow::Options{false, true});
        const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4")});
        QVERIFY(!ids.empty());
        Project p = w.document()->project();
        const std::string src = p.sources[0].id;
        Sequence& q = p.sequences[0];
        q.tracks.clear();
        for (int t = 0; t < 4; ++t) {
            Track tr{"t" + std::to_string(t), t < 3 ? TrackKind::Video : TrackKind::Audio, "T" + std::to_string(t)};
            for (int i = 0; i < 1000; ++i) {
                Clip c;
                c.id = "c" + std::to_string(t) + "_" + std::to_string(i);
                c.sourceId = src;
                c.sourceIn = Time(i % 8);
                c.timelineStart = Time(i);
                c.duration = Time(1);
                if (t < 3 && i % 10 == 0 && i + 1 < 1000)
                    c.outTransition = TransitionRef{"fa.transition.dissolve", Rational(1, 3)};
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
        q.tracks.insert(q.tracks.begin(), sub);
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray::fromStdString(projectToJsonString(p)));
    }
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 900);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QElapsedTimer t;
    t.start();
    QVERIFY(w.openProjectFile(file));
    QApplication::processEvents();
    const double openMs = t.nsecsElapsed() / 1e6;
    QCOMPARE(w.document()->sequence().tracks.size(), size_t(5));
    TimelineView* v = w.timelinePanel()->view();
    auto paintMs = [&] {
        QElapsedTimer p;
        p.start();
        v->repaint();
        return p.nsecsElapsed() / 1e6;
    };
    const double paintFirst = paintMs();   // includes first-use caches (glyphs, icons, thumbnail requests)
    double paintNormal = 0;
    for (int i = 0; i < 5; ++i) paintNormal = std::max(paintNormal, paintMs());
    const double ppsNormal = v->pixelsPerSecond();
    v->zoomToFit();   // every clip in view
    QApplication::processEvents();
    shot(w.timelinePanel(), "s3_long_project_zoomed_out.png");   // the panel only: the window shows a temp path
    paintMs();
    double paintFit = 0;
    for (int i = 0; i < 5; ++i) paintFit = std::max(paintFit, paintMs());
    v->setPixelsPerSecond(100);
    double scrollWorst = 0;
    for (int i = 0; i < 40; ++i) {
        v->setScrollSeconds(i * 25.0);
        scrollWorst = std::max(scrollWorst, paintMs());
    }
    t.start();
    w.playback()->seek(15015);   // the middle: frame 15 of the 501st clip
    QVERIFY(!waitForPreview(w, 15015).isNull());
    const double seekMs = t.nsecsElapsed() / 1e6;
    t.start();
    w.action("split")->trigger();
    QApplication::processEvents();
    const double splitMs = t.nsecsElapsed() / 1e6;
    QCOMPARE(w.document()->sequence().tracks[1].clips.size(), size_t(1001));
    t.start();
    w.action("undo")->trigger();
    QApplication::processEvents();
    const double undoMs = t.nsecsElapsed() / 1e6;
    QCOMPARE(w.document()->sequence().tracks[1].clips.size(), size_t(1000));
    qInfo("4300 clips + 300 captions: open %.0f ms, first paint %.1f ms, paint %.1f ms (%.1f px/s after opening), "
          "%.1f ms (all in view), scroll worst %.1f ms (100 px/s), seek to frame on screen %.0f ms, split %.0f ms, "
          "undo %.0f ms",
          openMs, paintFirst, paintNormal, ppsNormal, paintFit, scrollWorst, seekMs, splitMs, undoMs);
#ifdef NDEBUG
    const double slow = 1;   // the limits are for Release builds
#else
    const double slow = 8;   // unoptimised Debug build: checked for correctness, timed loosely
#endif
    QVERIFY2(openMs < 5000 * slow, "opening a 4300-clip project");
    // before the zoomed-out drawing (slivers for clips narrower than 6 px) a repaint took 120-180 ms; now about 10 ms
    QVERIFY2(paintNormal < 50 * slow && paintFit < 50 * slow && scrollWorst < 50 * slow, "timeline painting");
    QVERIFY2(seekMs < 2000 * slow, "seek in a long project");
    QVERIFY2(splitMs < 1000 * slow && undoMs < 1000 * slow, "edits in a long project");
}

// Stage 3: a project that names its media by a path relative to the project file (hand-written projects, the
// repository's examples): the timeline showed its clips as "Media offline" without filmstrip or waveform, and the
// library tile had no thumbnail, because the interface resolved the path against the current directory (the
// renderer resolved it correctly, so the project played and exported).
void GuiTests::relativeMediaPathsResolveAgainstTheProject() {
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath("media"));
    const QString media = dir.filePath("media/clip.mp4");
    QVERIFY(QFile::copy(mediaPath("A_red_1080p30_10s.mp4"), media));
    const QString project = dir.filePath("relative.lcproj");
    std::string clipId;
    {
        MainWindow w(MainWindow::Options{false, true});
        const auto ids = w.importFiles({media});
        QVERIFY(w.editController()->addToTimeline(ids[0], Rational(0)));
        clipId = w.document()->sequence().tracks[0].clips[0].id;
        QVERIFY(w.saveProjectTo(project));
    }
    QFile f(project);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QString text = QString::fromUtf8(f.readAll());
    f.close();
    const QString stored = QDir::cleanPath(media);
    QVERIFY2(text.contains(stored), qPrintable(stored));
    text.replace(stored, "media/clip.mp4");   // as a hand-written project names it
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(text.toUtf8());
    f.close();
    QVERIFY(QDir::currentPath() != QDir(dir.path()).absolutePath());   // the relative path does not exist from here

    MainWindow w(MainWindow::Options{false, true});
    w.resize(1400, 860);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QVERIFY(w.openProjectFile(project));
    const MediaSource& src = w.document()->project().sources[0];
    QCOMPARE(QString::fromStdString(src.path), QString("media/clip.mp4"));
    QCOMPARE(w.document()->mediaFilePath(src).toLower(), stored.toLower());
    QVERIFY(w.document()->missingSources().empty());
    // the timeline: the clip in the video-clip colour (offline clips are dark red), with its filmstrip
    TimelineView* v = w.timelinePanel()->view();
    v->zoomToFit();
    QImage img;
    QRect r;
    for (int i = 0; i < 40; ++i) {   // thumbnails arrive from a worker
        QTest::qWait(100);
        r = v->clipRect(clipId);
        img = QImage(v->size(), QImage::Format_ARGB32);
        v->render(&img);
        const QColor film = img.pixelColor(r.x() + r.width() / 2, r.y() + r.height() - 8);
        if (film.red() > 150 && film.green() < 60) break;   // the red clip's own picture in the filmstrip
    }
    QVERIFY(!r.isEmpty());
    const QColor label = img.pixelColor(r.right() - 20, r.y() + 5);
    qInfo("label strip %s, filmstrip %s", qPrintable(label.name()),
          qPrintable(img.pixelColor(r.x() + r.width() / 2, r.y() + r.height() - 8).name()));
    QVERIFY2(label.blue() > label.red(), "the clip is drawn as offline");
    const QColor film = img.pixelColor(r.x() + r.width() / 2, r.y() + r.height() - 8);
    QVERIFY2(film.red() > 150 && film.green() < 60, "no filmstrip");
    // importing the same file again (by its absolute path) does not add it twice
    const auto again = w.importFiles({media});
    QCOMPARE(w.document()->project().sources.size(), size_t(1));
    QCOMPARE(again.size(), size_t(1));
    QCOMPARE(again[0], src.id);
}

// M9: Filmora's clipboard and clip commands through real key events, and the clip menu (right-click) with its
// commands and colour-mark swatches.
void GuiTests::m9ClipboardShortcutsAndClipMenu() {
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1500, 900);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4"), mediaPath("B_blue_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    QVERIFY(ed->addToTimeline(ids[1], Rational(10)));
    auto countClips = [&] {
        size_t n = 0;
        for (const auto& t : doc->sequence().tracks) n += t.clips.size();
        return n;
    };
    auto clipById = [&](const std::string& id) { return *edit::findClip(doc->sequence(), id); };
    QCOMPARE(countClips(), size_t(4));
    const std::string v0 = doc->sequence().tracks[0].clips[0].id;
    std::string a0;   // its sound
    for (const auto& r : edit::linkGroup(doc->sequence(), v0))
        if (r.clipId != v0) a0 = r.clipId;
    QVERIFY(!a0.empty());
    TimelineView* tl = w.timelinePanel()->view();
    tl->setFocus();
    doc->setSelection({v0});
    QApplication::processEvents();

    // Ctrl+C, playhead to 15 s, Ctrl+V: the clip and its sound at 15 s (B is there: on the tracks above), selected;
    // Ctrl+Z undoes it
    QTest::keyClick(tl, Qt::Key_C, Qt::ControlModifier);
    QVERIFY(ed->canPaste());
    const FrameRate fps = doc->sequence().frameRate;
    auto frameAt = [&](int seconds) { return timeToFrame(Time(seconds), fps, Rounding::Nearest); };
    w.playback()->seek(frameAt(15));
    QApplication::processEvents();
    QCOMPARE(doc->playheadTime(), Time(15));
    QTest::keyClick(tl, Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(countClips(), size_t(6));
    QCOMPARE(doc->selection().size(), size_t(2));
    for (const auto& id : doc->selection()) QCOMPARE(clipById(id).timelineStart, Time(15));
    QTest::keyClick(tl, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(countClips(), size_t(4));
    // Ctrl+D duplicates right after the clip (B is there on Video 1: the copy goes to the track above)
    doc->setSelection({v0});
    QTest::keyClick(tl, Qt::Key_D, Qt::ControlModifier);
    QCOMPARE(countClips(), size_t(6));
    for (const auto& id : doc->selection()) QCOMPARE(clipById(id).timelineStart, Time(10));
    QTest::keyClick(tl, Qt::Key_Z, Qt::ControlModifier);
    // Ctrl+X cuts (the clipboard keeps it), Ctrl+Z brings it back
    doc->setSelection({v0});
    QTest::keyClick(tl, Qt::Key_X, Qt::ControlModifier);
    QCOMPARE(countClips(), size_t(2));
    QVERIFY(ed->canPaste());
    QTest::keyClick(tl, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(countClips(), size_t(4));
    // Ctrl+A selects every clip
    QTest::keyClick(tl, Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(doc->selection().size(), size_t(4));

    // single keys and chords on the selected clip
    doc->setSelection({v0});
    QApplication::processEvents();
    QTest::keyClick(tl, Qt::Key_E);   // Disable Clip
    QVERIFY(!clipById(v0).enabled);
    QTest::keyClick(tl, Qt::Key_E);
    QVERIFY(clipById(v0).enabled);
    QTest::keyClick(tl, Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);   // Mute: the clip and its sound
    QVERIFY(clipById(a0).muted);
    QTest::keyClick(tl, Qt::Key_Right, Qt::ControlModifier | Qt::AltModifier);   // Rotate 90 CW
    QCOMPARE(clipById(v0).transform.rotation.constantValue(), 90.0);
    QTest::keyClick(tl, Qt::Key_X);   // Select Clip Range: In / Out at the clip
    QCOMPARE(*doc->sequence().markIn, Time(0));
    QCOMPARE(*doc->sequence().markOut, Time(10));
    w.playback()->seek(frameAt(5));
    QApplication::processEvents();
    QTest::keyClick(tl, Qt::Key_Down);   // next edit point: the cut at 10 s
    QCOMPARE(doc->playheadTime(), Time(10));
    QTest::keyClick(tl, Qt::Key_Up);
    QCOMPARE(doc->playhead(), int64_t(0));

    // the clip menu (right-click): Filmora's commands; a colour swatch marks the clip
    QStringList items;
    QTimer::singleShot(400, [&] {
        auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!m) return;
        for (QAction* a : m->actions())
            if (!a->isSeparator()) items << a->text().section('\t', 0, 0);
        if (auto* swatch = m->findChild<QToolButton*>("colorMark6")) swatch->click();   // marks and closes
        else m->close();
    });
    tl->zoomToFit();
    QApplication::processEvents();
    const QRect r = tl->clipRect(v0);
    QVERIFY(!r.isEmpty());
    QContextMenuEvent ev(QContextMenuEvent::Mouse, r.center(), tl->mapToGlobal(r.center()));
    QApplication::sendEvent(tl, &ev);   // returns when the menu has closed
    qInfo("clip menu: %s", qPrintable(items.join(" | ")));
    for (const char* expected : {"Cut", "Copy", "Paste", "Duplicate", "Split", "Trim Start to Playhead", "Trim End to Playhead",
                                 "Ripple Delete", "Delete", "Attribute Editing", "Crop and Zoom", "Crop to Fit", "Rotate 90 CW",
                                 "Speed", "Reverse", "Detach Audio", "Audio Gain...", "Unmute", "Replace Clip...",
                                 "Disable Clip", "Export Selected Clips...", "Select Clip Range", "Rename...",
                                 "Locate in the Resource Panel", "Select All Clips with the Same Color Mark"})
        QVERIFY2(items.contains(expected), expected);
    QCOMPARE(clipById(v0).colorMark, 6);
    w.action("selectSameColor")->trigger();
    QCOMPARE(doc->selection(), std::vector<std::string>{v0});

    // Crop to Fit fills a 1920 x 1080 project with a 1280 x 534 picture; Replace Clip keeps the place
    doc->setSelection({v0});
    QVERIFY(ed->replaceClip(v0, ids[1]));
    QCOMPARE(clipById(v0).sourceId, ids[1]);
    QCOMPARE(clipById(v0).duration, Time(10));
    QCOMPARE(clipById(v0).transform.rotation.constantValue(), 90.0);   // kept
    QVERIFY(ed->cropToFit());   // same aspect as the project: nothing to crop
    QCOMPARE(clipById(v0).transform.scaleX.constantValue(), 1.0);
    // Locate in the Resource Panel shows the clip's media in the library
    w.action("locateInLibrary")->trigger();
    QCOMPARE(w.resourceTabs()->currentWidget(), static_cast<QWidget*>(w.mediaPanel()));
    QVERIFY(w.mediaPanel()->list()->currentItem());
    QCOMPARE(w.mediaPanel()->list()->currentItem()->data(Qt::UserRole + 1).toString(), QString::fromStdString(ids[1]));

    // the timeline toolbar's quick tools, as in Filmora: crop, text, speed (with its drop-down), freeze, detach, mute
    auto* bar = w.findChild<QWidget*>("timelineToolbar");
    QVERIFY(bar);
    QStringList tools;
    QToolButton* muteButton = nullptr;
    for (auto* b : bar->findChildren<QToolButton*>()) {
        if (!b->defaultAction()) continue;
        tools << b->defaultAction()->text();
        if (b->defaultAction() == w.action("mute")) muteButton = b;
    }
    qInfo("timeline toolbar: %s", qPrintable(tools.join(" | ")));
    for (const char* expected : {"Crop and Zoom", "Add Title at Playhead", "Uniform Speed...", "Freeze Frame", "Detach Audio"})
        QVERIFY2(tools.contains(expected), expected);
    auto* speedButton = bar->findChild<QToolButton*>("toolbarSpeed");
    QVERIFY(speedButton && speedButton->menu());
    QStringList speedItems;
    for (QAction* a : speedButton->menu()->actions()) speedItems << a->text();
    QVERIFY2(speedItems.contains("Normal Speed") && speedItems.contains("Slow") && speedItems.contains("Fast"),
             qPrintable(speedItems.join(", ")));
    QVERIFY(muteButton && muteButton->isVisible());
    doc->setSelection({v0});
    QApplication::processEvents();
    const bool wasMuted = clipById(v0).muted;
    QTest::mouseClick(muteButton, Qt::LeftButton);
    QCOMPARE(clipById(v0).muted, !wasMuted);
    shot(&w, "m9_main_window.png");
}

// M9: Filmora's Audio, Stickers and Templates sections - their pages, content, adding to the timeline (double-click
// actions, drops), template media slots replaced by media, and everything valid at other frame rates.
void GuiTests::m9AudioStickersTemplatesPages() {
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1500, 900);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    // the tabs, in Filmora's order
    QStringList tabs;
    for (QToolButton* b : w.resourceTabs()->findChildren<QToolButton*>("resourceTab")) tabs << b->text();
    qInfo("resource tabs: %s", qPrintable(tabs.join(", ")));
    for (const char* t : {"Media", "Audio", "Titles", "Transitions", "Effects", "Filters", "Stickers", "Templates"})
        QVERIFY2(tabs.contains(t), t);
    QVERIFY(tabs.indexOf("Audio") < tabs.indexOf("Titles") && tabs.indexOf("Stickers") < tabs.indexOf("Templates"));

    // ---- Audio: 17 music beds + 40 sound effects (M9's 6 + 19 and M11's 11 + 21), categories, preview, add (copied
    // into the library folder)
    AudioPage* audio = w.audioPage();
    QVERIFY(audio);
    QCOMPARE(audio->items().size(), size_t(57));
    w.resourceTabs()->setCurrentWidget(audio);
    audio->selectCategory("sfx:");
    QCOMPARE(audio->list()->count(), 40);
    audio->selectCategory("music:");
    QCOMPARE(audio->list()->count(), 17);
    audio->selectCategory("music:Chill");
    QCOMPARE(audio->list()->count(), 4);
    // the Preview / Add to Timeline / Favorite buttons sit under the list (until 1.0.0's README pictures they were never
    // added to the layout and lay over the search field, at the page's top left)
    QApplication::processEvents();
    for (const char* name : {"audioPreview", "audioAdd", "audioFavorite"}) {
        auto* b = audio->findChild<QPushButton*>(name);
        QVERIFY2(b && b->isVisible(), name);
        const QRect r(b->mapTo(audio, QPoint(0, 0)), b->size());
        const QRect listRect(audio->list()->mapTo(audio, QPoint(0, 0)), audio->list()->size());
        const QRect searchRect(audio->search()->mapTo(audio, QPoint(0, 0)), audio->search()->size());
        QVERIFY2(r.top() > listRect.bottom() && !r.intersects(searchRect), name);
    }
    // M11: the new groups are in the tree with at least two pieces each, and every indexed file is there
    for (const char* key : {"music:Acoustic", "music:Fun", "sfx:Everyday", "sfx:Nature", "sfx:Comedy"}) {
        audio->selectCategory(key);
        QCOMPARE(audio->currentCategory(), QString(key));
        QVERIFY2(audio->list()->count() >= 2, key);
    }
    for (const AudioItem& it : audio->items()) {   // a sound of the length the index gives it
        const QString path = QDir(assetsDir()).filePath("audio/" + it.file);
        QVERIFY2(QFileInfo(path).isFile(), qPrintable(it.id));
        const MediaInfo mi = probeMedia(std::filesystem::path(path.toStdWString()));
        QVERIFY2(mi.audio && mi.duration && std::abs(mi.duration->toDouble() - it.seconds) < 0.05, qPrintable(it.id));
    }
    audio->search()->setText("whoosh");
    audio->selectCategory("sfx:");
    QCOMPARE(audio->list()->count(), 1);
    audio->search()->clear();
    QVERIFY(audio->togglePreview("music.bright_morning"));
    QCOMPARE(audio->previewing(), QString("music.bright_morning"));
    QVERIFY(!audio->togglePreview("music.bright_morning"));   // a second click stops it
    QVERIFY(audio->previewing().isEmpty());
    const std::string a1 = audio->addToTimeline("sfx.whoosh", Time(1));
    QVERIFY(!a1.empty());
    const Track* tr = nullptr;
    const Clip* ac = edit::findClip(doc->sequence(), a1, &tr);
    QVERIFY(ac && tr->kind == TrackKind::Audio);
    QCOMPARE(ac->timelineStart, Time(1));
    const MediaSource* as = doc->project().findSource(ac->sourceId);
    QVERIFY(as);
    QVERIFY2(QString::fromStdString(as->path).contains("meercut_gui_tests_library"), as->path.c_str());

    // ---- Stickers: categories, double-click adds at the playhead at the catalogue size, the preview shows it
    StickersPage* stickers = w.stickersPage();
    QVERIFY(stickers);
    w.resourceTabs()->setCurrentWidget(stickers);
    stickers->selectCategory("all");
    QCOMPARE(stickers->list()->count(), static_cast<int>(text::stickerCatalog().size()));
    stickers->selectCategory("Emoji");
    QVERIFY(stickers->list()->count() >= 8);
    const QImage before = waitForPreview(w, 0);
    stickers->selectCategory("Shape");
    const std::string s1 = stickers->add("shape.circle", Time(0));
    QVERIFY(!s1.empty());
    const Clip* sc = edit::findClip(doc->sequence(), s1, &tr);
    QVERIFY(sc && sc->isSticker() && tr->kind == TrackKind::Video);
    QCOMPARE(sc->transform.scaleX.constantValue(), text::findSticker("shape.circle")->defaultScale);
    const QImage after = waitForPreview(w, 0);
    QVERIFY(!before.isNull() && !after.isNull());
    QVERIFY2(after != before, "the sticker shows in the player");
    // a sticker dropped on the timeline lands at the drop time
    TimelineView* tl = w.timelinePanel()->view();
    tl->zoomToFit();
    QApplication::processEvents();
    const QPoint dropAt(tl->xForTime(3.0), TimelineView::kRulerH + 20);
    QMimeData mime;
    mime.setData(kMimeSticker, "love.heart");
    QDragEnterEvent enter(dropAt, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &enter);
    QDropEvent drop(QPointF(dropAt), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tl, &drop);
    bool heart = false;
    for (const auto& t : doc->sequence().tracks)
        for (const auto& c : t.clips)
            if (c.sticker && c.sticker->id == "love.heart") {
                heart = true;
                QVERIFY(std::abs(c.timelineStart.toDouble() - 3.0) < 0.5);
            }
    QVERIFY2(heart, "the dropped sticker is on the timeline");

    // ---- Templates: thumbnails rendered by the engine, a 2 x 2 split screen stacked on four tracks, a slot replaced
    TemplatesPage* templates = w.templatesPage();
    QVERIFY(templates);
    w.resourceTabs()->setCurrentWidget(templates);
    templates->selectCategory("all");
    QCOMPARE(templates->list()->count(), static_cast<int>(templateCatalog().size()));
    QVERIFY(!templates->list()->item(0)->icon().isNull());
    templates->selectCategory("Split Screen");
    QCOMPARE(templates->list()->count(), 6);   // M9's five and M11's Picture in Picture
    const auto ids = templates->add("split.4", Time(20));
    QCOMPARE(ids.size(), size_t(4));
    std::set<std::string> trackIds;
    for (const auto& id : ids) {
        const Clip* c = edit::findClip(doc->sequence(), id, &tr);
        QVERIFY(c && c->sticker && c->sticker->id == "placeholder");
        QCOMPARE(c->transform.scaleX.constantValue(), 0.5);
        trackIds.insert(tr->id);
    }
    QCOMPARE(trackIds.size(), size_t(4));
    QVERIFY(!hasErrors(validate(doc->project())));
    const auto media = w.importFiles({mediaPath("A_red_1080p30_10s.mp4")});
    QVERIFY(w.editController()->replaceClip(ids[0], media[0]));
    const Clip* replaced = edit::findClip(doc->sequence(), ids[0]);
    QVERIFY(!replaced->sticker);
    QCOMPARE(replaced->sourceId, media[0]);
    QCOMPARE(replaced->transform.scaleX.constantValue(), 0.5);   // the cell is kept
    QVERIFY(!hasErrors(validate(doc->project())));

    // M11: 17 more templates; every layer of every template has its clips (a title or sticker the builder did not
    // find would leave its layer empty), and every category has templates
    QCOMPARE(int(templateCatalog().size()), 34);   // M9's 17 and M11's 17
    for (const auto& info : templateCatalog()) {
        std::set<std::string> layers;
        for (const auto& c : templateClips(info.id, FrameRate(30))) layers.insert(c.trackId);
        for (size_t i = 0; i < layers.size(); ++i)
            QVERIFY2(layers.count("tpl." + std::to_string(i)), qPrintable(QString::fromStdString(info.id) + ": layer " +
                                                                           QString::number(i) + " is empty"));
        QVERIFY2(!layers.empty(), info.id.c_str());
    }
    for (const auto& cat : templateCategories())
        QVERIFY2(std::any_of(templateCatalog().begin(), templateCatalog().end(),
                             [&](const TemplateInfo& t) { return t.category == cat; }),
                 cat.c_str());
    // every template is a valid composition at the usual frame rates
    for (const FrameRate& rate : {FrameRate(24000, 1001), FrameRate(25), FrameRate(30000, 1001), FrameRate(60)}) {
        for (const auto& info : templateCatalog()) {
            Project p;
            Sequence q;
            q.id = "s";
            q.frameRate = rate;
            q.tracks.push_back(Track{"v1", TrackKind::Video, "Video 1"});
            p.sequences.push_back(q);
            UndoStack st(p);
            st.execute(edit::pasteClips(p, "s", templateClips(info.id, rate), Time(0), nullptr, true));
            const auto issues = validate(p);
            QVERIFY2(!hasErrors(issues), qPrintable(QString::fromStdString(info.id + ": " + formatIssues(issues))));
        }
    }
    shot(&w, "m9_templates_page.png");
}

// M9: the inspector in Filmora's order (Video, Audio, Speed, Animation, Color), motion presets from the Animation tab
// (keyframes that render), AI Matting, and a sticker's colour and words.
void GuiTests::m9InspectorTabsAnimationAndStickerLook() {
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1500, 900);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({mediaPath("C_testsrc2_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Rational(0)));
    const std::string v = doc->sequence().tracks[0].clips[0].id;
    doc->setSelection({v});
    QApplication::processEvents();
    QTabWidget* tabs = w.inspector()->clipTabs();
    QStringList shown;
    for (int i = 0; i < tabs->count(); ++i)
        if (tabs->isTabVisible(i)) shown << tabs->tabText(i);
    qInfo("inspector tabs for a video clip: %s", qPrintable(shown.join(", ")));
    QCOMPARE(shown, QStringList({"Video", "Speed", "Animation", "Color", "Effects"}));
    auto* sub = w.inspector()->findChild<QTabWidget*>("videoSubTabs");
    QStringList subs;
    for (int i = 0; i < sub->count(); ++i) subs << sub->tabText(i);
    QVERIFY2(subs.contains("AI Matting"), qPrintable(subs.join(", ")));

    // Animation > In > Zoom In: the clip starts at half size and transparent, then rests at full size
    AnimationPanel* anim = w.inspector()->animationPanel();
    QVERIFY(anim);
    tabs->setCurrentWidget(anim);
    anim->setSlot(MotionSlot::In);
    QListWidgetItem* zoom = nullptr;
    for (int i = 0; i < anim->list()->count(); ++i)
        if (anim->list()->item(i)->text() == "Zoom In") zoom = anim->list()->item(i);
    QVERIFY(zoom);
    QTest::mouseClick(anim->list()->viewport(), Qt::LeftButton, {}, anim->list()->visualItemRect(zoom).center());
    const Clip* c = edit::findClip(doc->sequence(), v);
    QCOMPARE(c->motionIn, std::string("zoom_in"));
    QCOMPARE(c->transform.scaleX.value(Time(0)), 0.5);
    QCOMPARE(c->opacity.value(Time(0)), 0.0);
    w.playback()->seek(0);
    const QImage first = waitForPreview(w, 0);
    w.playback()->seek(60);
    const QImage later = waitForPreview(w, 60);
    QVERIFY(!first.isNull() && !later.isNull());
    auto meanLuma = [](const QImage& img) {
        double s = 0;
        for (int y = 0; y < img.height(); y += 4)
            for (int x = 0; x < img.width(); x += 4) s += qGray(img.pixel(x, y));
        return s / ((img.width() / 4) * (img.height() / 4));
    };
    qInfo("zoom in: mean luma at frame 0 %.1f, at 2 s %.1f", meanLuma(first), meanLuma(later));
    QVERIFY2(meanLuma(first) < 5.0, "the clip is transparent (black) at its first frame");
    QVERIFY(meanLuma(later) > 40.0);
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);   // one undo step
    QVERIFY(edit::findClip(doc->sequence(), v)->motionIn.empty());

    // a sticker's colour and words in Video > Basic
    const std::string s1 = ed->addSticker("badge.live", Time(1));
    QVERIFY(!s1.empty());
    QApplication::processEvents();
    auto* text = w.inspector()->findChild<QLineEdit*>("stickerText");
    QVERIFY(text && text->isEnabled());
    QCOMPARE(text->placeholderText(), QString("LIVE"));
    QVERIFY(ed->setStickerLook(s1, 0x2060FF, "ON AIR"));
    const Clip* sc = edit::findClip(doc->sequence(), s1);
    QCOMPARE(sc->sticker->color, uint32_t(0x2060FF));
    QCOMPARE(sc->sticker->text, std::string("ON AIR"));
    shot(w.inspector(), "m9_inspector_sticker.png");
}

// M10: Clipchamp's "Delete this gap". The mouse over a gap between clips (or before the first one) highlights it with
// a trash button; a real click closes it - the clips after it and their sounds move left, one undo step. Right-click
// > Delete Gap does the same. A gap that cannot close (a sound in the way of a linked sound) says why; nothing moves.
void GuiTests::m10DeleteGap() {
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1500, 900);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4"), mediaPath("B_blue_1080p30_10s.mp4")});
    QCOMPARE(ids.size(), size_t(2));
    QVERIFY(ed->addToTimeline(ids[0], Time(3)));    // starts at 3 s: a gap before it, as in Clipchamp's screenshot
    QVERIFY(ed->addToTimeline(ids[1], Time(16)));   // and a 3 s gap after it (13 - 16 s)
    TimelineView* tl = w.timelinePanel()->view();
    tl->zoomToFit();
    QApplication::processEvents();
    std::string v1, a1;
    for (const auto& t : doc->sequence().tracks) {
        if (t.kind == TrackKind::Video && v1.empty()) v1 = t.id;
        if (t.kind == TrackKind::Audio && a1.empty()) a1 = t.id;
    }
    const auto vclips = doc->sequence().findTrack(v1)->clips;
    const auto aclips = doc->sequence().findTrack(a1)->clips;
    QCOMPARE(vclips.size(), size_t(2));
    QCOMPARE(aclips.size(), size_t(2));
    const std::string a = vclips[0].id, b = vclips[1].id, bSound = aclips[1].id;
    auto start = [&](const std::string& id) { return edit::findClip(doc->sequence(), id)->timelineStart; };
    auto hover = [&](const QPoint& pt) {
        QMouseEvent mv(QEvent::MouseMove, QPointF(pt), QPointF(tl->mapToGlobal(pt)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(tl, &mv);
    };

    // the mouse over the gap between the clips: highlighted, with a trash button in its middle
    const QRect lane = tl->trackLaneRect(v1);
    hover(QPoint(tl->xForTime(13.6), lane.top() + 12));
    QVERIFY(tl->hoveredGap().has_value());
    QCOMPARE(tl->hoveredGap()->trackId, v1);
    QCOMPARE(tl->hoveredGap()->range, (TimeRange{Time(13), Time(3)}));
    const QRect button = tl->gapButtonRect();
    QVERIFY(!button.isEmpty());
    QVERIFY(button.center().x() > tl->xForTime(13.0) && button.center().x() < tl->xForTime(16.0));
    hover(button.center());   // the button lights up with its label, "Delete this gap"
    shot(w.timelinePanel(), "m10_delete_gap.png");
    QTest::mouseClick(tl, Qt::LeftButton, {}, button.center());
    QCOMPARE(start(b), Time(13));
    QCOMPARE(start(bSound), Time(13));   // the clip's sound follows it
    QCOMPARE(start(a), Time(3));         // before the gap: unchanged
    QCOMPARE(doc->undoStack().undoLabel(), std::string("Delete gap"));
    QVERIFY(!tl->hoveredGap().has_value());

    // the gap before the first clip, from the right-click menu
    bool offered = false, enabled = false;
    QTimer::singleShot(300, [&] {
        auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!m) return;
        for (QAction* act : m->actions())
            if (act->objectName() == "deleteGap") {
                offered = true;
                enabled = act->isEnabled();
                if (enabled) act->trigger();
            }
        m->close();
    });
    const QPoint lead(tl->xForTime(1.5), lane.center().y());
    QContextMenuEvent ev(QContextMenuEvent::Mouse, lead, tl->mapToGlobal(lead));
    QApplication::sendEvent(tl, &ev);   // returns when the menu has closed
    QVERIFY(offered && enabled);
    QCOMPARE(start(a), Time(0));
    QCOMPARE(start(b), Time(10));
    QVERIFY(!hasErrors(validate(doc->project())));
    QVERIFY(doc->undo());
    QVERIFY(doc->undo());
    QCOMPARE(start(a), Time(3));
    QCOMPARE(start(b), Time(16));

    // a sound in Audio 1's part of the gap: B's sound cannot move over it - the click says why and nothing moves
    const std::string pop = w.audioPage()->addToTimeline("sfx.pop", Time(14));
    QVERIFY(!pop.empty());
    const Track* popTrack = nullptr;
    QVERIFY(edit::findClip(doc->sequence(), pop, &popTrack));
    QCOMPARE(popTrack->id, a1);
    hover(QPoint(tl->xForTime(13.6), lane.top() + 12));
    QVERIFY(tl->hoveredGap().has_value());
    const QRect blocked = tl->gapButtonRect();
    QVERIFY(!blocked.isEmpty());
    hover(blocked.center());
    shot(w.timelinePanel(), "m10_delete_gap_blocked.png");
    QTest::mouseClick(tl, Qt::LeftButton, {}, blocked.center());
    qInfo("a gap that cannot close: %s", qPrintable(w.lastMessage()));
    QCOMPARE(start(b), Time(16));
    QCOMPARE(start(bSound), Time(16));
    QVERIFY2(w.lastMessage().contains("Can't delete this gap") && w.lastMessage().contains("Audio 1"), qPrintable(w.lastMessage()));
}

// M10: the user's report - a video on the timeline, "Audio track" clicked a few times, a sound dragged onto Audio 3 or 4
// went to Audio 1 instead. Sounds from the Media page, the Audio page and Explorer now land on the audio track under the
// mouse (where the drag preview shows them), and nothing else moves; a clip's sound dragged with the mouse to another
// audio track stays there, its picture following in time.
void GuiTests::m10SoundsLandOnTheAudioTrackUnderTheMouse() {
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1500, 900);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4"), mediaPath("H_tone1k_-12dB_10s.wav")});
    QCOMPARE(ids.size(), size_t(2));
    QVERIFY(ed->addToTimeline(ids[0], Time(0)));   // the video; its sound on Audio 1
    QToolButton* addAudio = nullptr;
    for (auto* b : w.timelinePanel()->findChildren<QToolButton*>())
        if (b->text() == "Audio track") addAudio = b;
    QVERIFY(addAudio);
    for (int i = 0; i < 3; ++i) QTest::mouseClick(addAudio, Qt::LeftButton);
    std::vector<std::string> audio;
    std::string v1;
    for (const auto& t : doc->sequence().tracks) {
        if (t.kind == TrackKind::Audio) audio.push_back(t.id);
        if (t.kind == TrackKind::Video && v1.empty()) v1 = t.id;
    }
    QCOMPARE(audio.size(), size_t(4));
    const std::string picture = doc->sequence().findTrack(v1)->clips.at(0).id;
    const std::string sound = doc->sequence().findTrack(audio[0])->clips.at(0).id;
    TimelineView* tl = w.timelinePanel()->view();
    tl->zoomToFit();
    QApplication::processEvents();
    auto clipsOn = [&](const std::string& track) { return doc->sequence().findTrack(track)->clips; };
    auto trackOf = [&](const std::string& clipId) {
        const Track* tr = nullptr;
        return edit::findClip(doc->sequence(), clipId, &tr) ? tr->id : std::string();
    };
    auto dropOn = [&](const QMimeData& mime, const std::string& track, double seconds, std::string* ghost) {
        const QPoint pt(tl->xForTime(seconds), tl->trackLaneRect(track).center().y());
        QDragEnterEvent enter(pt, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tl, &enter);
        QDragMoveEvent move(pt, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tl, &move);
        if (ghost) *ghost = tl->dropGhostTrack();
        const bool moveOk = move.isAccepted() && move.dropAction() == Qt::CopyAction;   // never a move (M10)
        QDropEvent drop(QPointF(pt), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tl, &drop);
        QApplication::processEvents();
        return moveOk && drop.isAccepted() && drop.dropAction() == Qt::CopyAction;
    };

    // 1. the sound from the Media page onto Audio 3 at 2 s, while Audio 1 has the video's sound there
    QMimeData fromMedia;
    fromMedia.setData(kMimeSource, QByteArray::fromStdString(ids[1]));
    std::string ghost;
    QVERIFY(dropOn(fromMedia, audio[2], 2.0, &ghost));
    QCOMPARE(ghost, audio[2]);                      // the preview was on Audio 3 ...
    QCOMPARE(clipsOn(audio[2]).size(), size_t(1));  // ... and so is the sound
    QCOMPARE(clipsOn(audio[2])[0].timelineStart, Time(2));
    QCOMPARE(clipsOn(audio[0]).size(), size_t(1));  // Audio 1: still only the video's sound
    QCOMPARE(clipOf(doc, picture).timelineStart, Time(0));   // nothing rippled

    // 2. a sound from the Audio page onto Audio 4 at 4 s (its drag carries the file and its length: a true preview)
    AudioPage* page = w.audioPage();
    w.resourceTabs()->setCurrentWidget(page);
    page->selectCategory("sfx:");
    QApplication::processEvents();
    QListWidgetItem* whoosh = nullptr;
    for (int i = 0; i < page->list()->count(); ++i)
        if (page->list()->item(i)->data(Qt::UserRole + 1).toString() == "sfx.whoosh") whoosh = page->list()->item(i);
    QVERIFY(whoosh);
    std::unique_ptr<QMimeData> fromLibrary(page->list()->model()->mimeData({page->list()->indexFromItem(whoosh)}));
    QVERIFY(fromLibrary && fromLibrary->hasFormat(kMimeLibraryAudio) && fromLibrary->hasUrls());
    QVERIFY(dropOn(*fromLibrary, audio[3], 4.0, &ghost));
    QCOMPARE(ghost, audio[3]);
    QCOMPARE(clipsOn(audio[3]).size(), size_t(1));
    QCOMPARE(clipsOn(audio[3])[0].timelineStart, Time(4));
    QVERIFY(clipsOn(audio[3])[0].duration < Time(2));

    // 3. a sound file from Explorer onto Audio 2 at 1 s
    QMimeData fromExplorer;
    fromExplorer.setUrls({QUrl::fromLocalFile(mediaPath("H_tone1k_-12dB_10s.wav"))});
    QVERIFY(dropOn(fromExplorer, audio[1], 1.0, nullptr));
    QCOMPARE(clipsOn(audio[1]).size(), size_t(1));
    QCOMPARE(clipsOn(audio[1])[0].timelineStart, Time(1));
    QCOMPARE(clipsOn(audio[0]).size(), size_t(1));
    QCOMPARE(clipOf(doc, picture).timelineStart, Time(0));
    QVERIFY(!hasErrors(validate(doc->project())));

    // 4. the video's sound dragged with the mouse from Audio 1 to Audio 4, 6 s later: it stays on Audio 4 and its
    //    picture follows on Video 1
    tl->setSnapping(false);
    const QRect sr = tl->clipRect(sound);
    QVERIFY(!sr.isEmpty());
    const QPoint from(sr.left() + sr.width() / 3, sr.center().y() + sr.height() / 4);
    const QPoint to(from.x() + tl->xForTime(6.0) - tl->xForTime(0.0), tl->trackLaneRect(audio[3]).center().y());
    QTest::mousePress(tl, Qt::LeftButton, {}, from);
    for (int i = 1; i <= 8; ++i) QTest::mouseMove(tl, from + (to - from) * i / 8);
    QTest::mouseRelease(tl, Qt::LeftButton, {}, to);
    QCOMPARE(trackOf(sound), audio[3]);
    QCOMPARE(clipOf(doc, sound).timelineStart, Time(6));
    QCOMPARE(trackOf(picture), v1);
    QCOMPARE(clipOf(doc, picture).timelineStart, Time(6));
    QVERIFY(!hasErrors(validate(doc->project())));
    shot(w.timelinePanel(), "m10_sounds_on_their_tracks.png");
}

// M10: effects and filters dropped like text - their own clip above the video, 5 s from the drop point, which changes
// everything below it while it lasts. The drag preview shows where it will land (a new track when needed). Ctrl+drop
// onto a clip applies to that clip only; a keying effect needs its clip; double-click with nothing selected adds one
// at the playhead.
void GuiTests::m10EffectsAndFiltersDropAsLayers() {
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    const auto ids = w.importFiles({mediaPath("C_testsrc2_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Time(0)));
    TimelineView* tl = w.timelinePanel()->view();
    tl->zoomToFit();
    QApplication::processEvents();
    const std::string v1 = doc->sequence().tracks[0].id;
    const std::string clip = doc->sequence().tracks[0].clips.at(0).id;
    EffectsBrowser* fx = w.effectsPage();
    w.resourceTabs()->setCurrentWidget(fx);
    QApplication::processEvents();
    auto* hint = fx->findChild<QLabel*>("effectsHint");
    QVERIFY(hint && hint->text().contains("above your video") && hint->text().contains("Ctrl"));
    w.playback()->seek(30);
    const QImage plainEarly = waitForPreview(w, 30);   // 1 s
    w.playback()->seek(90);
    const QImage plain = waitForPreview(w, 90);        // 3 s
    QVERIFY2(!plainEarly.isNull() && !plain.isNull(), "the player showed no frame");
    auto mimeOf = [](EffectsBrowser* page, const QString& key) {
        QListWidgetItem* it = page->itemFor(key);
        return std::unique_ptr<QMimeData>(it ? page->list()->model()->mimeData({page->list()->indexFromItem(it)}) : nullptr);
    };
    auto dragTo = [&](QMimeData* mime, const QPoint& pt, Qt::KeyboardModifiers mods) {
        QDragEnterEvent enter(pt, Qt::CopyAction, mime, Qt::LeftButton, mods);
        QApplication::sendEvent(tl, &enter);
        QDragMoveEvent move(pt, Qt::CopyAction, mime, Qt::LeftButton, mods);
        QApplication::sendEvent(tl, &move);
        return move.isAccepted();
    };
    auto dropAt = [&](QMimeData* mime, const QPoint& pt, Qt::KeyboardModifiers mods) {
        QDropEvent drop(QPointF(pt), Qt::CopyAction, mime, Qt::LeftButton, mods);
        QApplication::sendEvent(tl, &drop);
        QApplication::processEvents();
        return drop.isAccepted();
    };
    auto clipCount = [&] {
        size_t n = 0;
        for (const auto& t : doc->sequence().tracks) n += t.clips.size();
        return n;
    };

    // 1. Pixelate dropped onto the video at 2 s: above it, on a new track (the preview says so); the video is untouched
    const auto pix = mimeOf(fx, "fa.fx.pixelate");
    QVERIFY(pix && pix->hasFormat(kMimeEffect));
    const QPoint onClip(tl->xForTime(2.0), tl->clipRect(clip).center().y());
    QVERIFY(dragTo(pix.get(), onClip, Qt::NoModifier));
    QVERIFY(tl->dropNeedsNewTrack());
    QVERIFY(tl->dropEffectTarget().empty());
    shot(w.timelinePanel(), "m10_effect_drag_preview.png");
    QVERIFY(dropAt(pix.get(), onClip, Qt::NoModifier));
    QVERIFY(clipOf(doc, clip).effects.empty());
    const std::string layer = doc->primarySelection();
    const Track* lt = nullptr;
    const Clip* lc = edit::findClip(doc->sequence(), layer, &lt);
    QVERIFY(lc && lc->isAdjustment());
    QVERIFY(lt->kind == TrackKind::Video && lt->id != v1);
    QCOMPARE(lc->name, std::string("Pixelate"));
    QCOMPARE(lc->effects.size(), size_t(1));
    QCOMPARE(lc->effects[0].type, std::string("fa.fx.pixelate"));
    QCOMPARE(lc->timelineStart, Time(2));
    QCOMPARE(lc->duration, Time(5));
    QCOMPARE(doc->undoStack().undoLabel(), std::string("Add effect"));
    QTabWidget* tabs = w.inspector()->clipTabs();
    QCOMPARE(tabs->tabText(tabs->currentIndex()), QString("Effects"));   // its settings at hand
    const std::string layerTrack = lt->id;
    // it changes the picture while it lasts, not before
    w.playback()->seek(30);
    const QImage early = waitForPreview(w, 30);
    w.playback()->seek(90);
    const QImage during = waitForPreview(w, 90);
    QVERIFY2(!early.isNull() && !during.isNull(), "the player showed no frame");
    qInfo("pixelate layer: before it %.2f / 255 from plain, during it %.2f", meanAbsDiff(early, plainEarly), meanAbsDiff(during, plain));
    QVERIFY(meanAbsDiff(early, plainEarly) < 1.0);
    QVERIFY(meanAbsDiff(during, plain) > 3.0);

    // 2. a filter onto the free part of that track at 8 s: it lands right there
    EffectsBrowser* fi = w.filtersPage();
    w.resourceTabs()->setCurrentWidget(fi);
    QApplication::processEvents();
    const auto look = mimeOf(fi, "look:teal-orange");
    QVERIFY(look);
    const QPoint onFree(tl->xForTime(8.0), tl->trackLaneRect(layerTrack).center().y());
    QVERIFY(dragTo(look.get(), onFree, Qt::NoModifier));
    QCOMPARE(tl->dropGhostTrack(), layerTrack);
    QVERIFY(!tl->dropNeedsNewTrack());
    QVERIFY(dropAt(look.get(), onFree, Qt::NoModifier));
    const Track* ft = nullptr;
    const Clip* fc = edit::findClip(doc->sequence(), doc->primarySelection(), &ft);
    QVERIFY(fc && fc->isAdjustment());
    QCOMPARE(ft->id, layerTrack);
    QCOMPARE(fc->timelineStart, Time(8));
    QCOMPARE(fc->effects.at(0).type, std::string("fa.filter.look"));
    QCOMPARE(fc->name, std::string("Teal & Orange"));

    // 3. Ctrl+drop onto the video: applied to that clip only
    const size_t before = clipCount();
    const auto glow = mimeOf(fx, "fa.fx.glow");
    QVERIFY(glow);
    const QPoint onVideo(tl->xForTime(1.0), tl->clipRect(clip).center().y());
    QVERIFY(dragTo(glow.get(), onVideo, Qt::ControlModifier));
    QCOMPARE(tl->dropEffectTarget(), clip);
    QVERIFY(dropAt(glow.get(), onVideo, Qt::ControlModifier));
    QCOMPARE(clipOf(doc, clip).effects.size(), size_t(1));
    QCOMPARE(clipOf(doc, clip).effects[0].type, std::string("fa.fx.glow"));
    QCOMPARE(clipCount(), before);

    // 4. Chroma Key needs the clip it keys: onto the clip it applies (no Ctrl needed), over empty space it is refused
    const auto key = mimeOf(fx, "fa.fx.chromakey");
    QVERIFY(key);
    const QPoint nowhere(tl->xForTime(10.2), tl->trackLaneRect(v1).center().y());   // just after the video
    QVERIFY(!dragTo(key.get(), nowhere, Qt::NoModifier));
    QVERIFY(dragTo(key.get(), onVideo, Qt::NoModifier));
    QCOMPARE(tl->dropEffectTarget(), clip);
    QVERIFY(dropAt(key.get(), onVideo, Qt::NoModifier));
    QCOMPARE(clipOf(doc, clip).effects.size(), size_t(2));
    QCOMPARE(clipOf(doc, clip).effects[1].type, std::string("fa.fx.chromakey"));
    QCOMPARE(clipCount(), before);

    // 5. double-click with nothing selected: a layer at the playhead
    doc->setSelection({});
    w.playback()->seek(150);   // 5 s
    QVERIFY(fx->apply("fa.fx.blur.gaussian"));
    const Clip* bc = edit::findClip(doc->sequence(), doc->primarySelection());
    QVERIFY(bc && bc->isAdjustment());
    QCOMPARE(bc->timelineStart, Time(5));
    QCOMPARE(bc->effects.at(0).type, std::string("fa.fx.blur.gaussian"));
    QCOMPARE(clipCount(), before + 1);
    QVERIFY(!hasErrors(validate(doc->project())));
    shot(&w, "m10_effect_layers.png");

    // 6. saved and reopened unchanged
    QTemporaryDir tmp;
    const QString file = tmp.filePath("layers.lcproj");
    QVERIFY(w.saveProjectTo(file));
    MainWindow w2(MainWindow::Options{false, true});
    w2.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w2));
    QVERIFY(w2.openProjectFile(file));
    QVERIFY(w2.document()->sequence() == doc->sequence());
}

// M10 (user report): a text made bigger turned soft and pixelated. Made bigger the way a user does it - the Scale field
// in Video > Basic - the title in the player is 3x as tall and crisper (its soft edge share falls with the size, as for
// text drawn larger; a stretched picture kept the same share).
void GuiTests::m10EnlargedTextStaysSharp() {
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1500, 900);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    QVERIFY(ed->addTitleFromTemplate("basic.plain", Rational(0)));   // "Your Title Here", white, centred
    const std::string title = doc->primarySelection();
    auto measure = [](const QImage& img0, int* height) {
        const QImage img = img0.convertToFormat(QImage::Format_RGB32);
        int64_t soft = 0, ink = 0;
        int y0 = img.height(), y1 = -1;
        for (int y = 0; y < img.height(); ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                const int l = std::max({qRed(row[x]), qGreen(row[x]), qBlue(row[x])});
                if (l <= 24) continue;
                ++ink;
                if (l < 231) ++soft;
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
        }
        *height = y1 - y0 + 1;
        return ink ? double(soft) / double(ink) : 1.0;
    };
    w.playback()->seek(30);
    const QImage normal = waitForPreview(w, 30);
    QVERIFY2(!normal.isNull(), "the player showed no frame");
    selectTab(w.inspector()->clipTabs(), "Video");
    auto* scaleSpin = w.inspector()->findChild<QDoubleSpinBox*>("prop:transform.scaleX");
    QVERIFY(scaleSpin);
    typeInto(scaleSpin, "300");
    QCOMPARE(clipOf(doc, title).transform.scaleX.constantValue(), 3.0);
    const QImage big = waitForPreview(w, 30);
    QVERIFY2(!big.isNull(), "the player showed no frame");
    int hNormal = 0, hBig = 0;
    const double sNormal = measure(normal, &hNormal), sBig = measure(big, &hBig);
    qInfo("title in the player: 100 %% soft edge share %.3f, %d px tall; 300 %% %.3f, %d px tall", sNormal, hNormal, sBig, hBig);
    QVERIFY(std::abs(hBig - 3.0 * hNormal) <= 0.06 * 3.0 * hNormal + 3);   // really 3x as tall
    QVERIFY2(sBig < sNormal * 0.6, "the enlarged text is as crisp as text drawn that large");
    shot(w.previewPanel(), "m10_enlarged_text.png");
}

// M10 (user report): stickers and template items could only be placed by typing numbers - the player showed them no
// box. Now they have the same handles as video and titles: drag inside to move, a corner to resize (aspect kept), the
// round knob to rotate, a click in the player selects them. The box hugs the sticker's art, so a click on the
// transparent rest of its picture reaches the video below. A template's media slot and a background sticker shrunk
// to 5 % are handled the same way.
void GuiTests::m10StickersMoveAndResizeInThePlayer() {
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 1000);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    PreviewCanvas* canvas = w.previewPanel()->canvas();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4")});
    QVERIFY(ed->addToTimeline(ids[0], Time(0)));
    const std::string video = doc->sequence().tracks[0].clips.at(0).id;
    const std::string s1 = ed->addSticker("sub.actions", Time(0));   // Like / Comment / Share, above the video
    QVERIFY(!s1.empty());
    w.playback()->seek(60);   // 2 s: its pop-in is over
    QVERIFY(!waitForPreview(w, 60).isNull());
    QVERIFY(canvas->handlesVisible());
    const Time t(2);
    auto tf = [&]() -> const ClipTransform& { return clipOf(doc, s1).transform; };

    // the box hugs the three discs: a wide strip, not the sticker's square picture
    const QPointF tl = *canvas->handlePos("tl"), tr = *canvas->handlePos("tr"), bl = *canvas->handlePos("bl");
    const double boxW = QLineF(tl, tr).length(), boxH = QLineF(tl, bl).length();
    const QRectF fr = canvas->frameRect();
    const double side = tf().scaleX.value(t) * fr.height();   // the square picture on screen
    qInfo("sticker box %.0f x %.0f px, its picture %.0f x %.0f px", boxW, boxH, side, side);
    QVERIFY2(boxW > 2.5 * boxH && boxH < 0.5 * side, "the box hugs the art");
    // a click on the transparent part of the picture (above the discs) selects the video below
    const QPointF centre = *canvas->handlePos("center");
    doc->setSelection({});
    QApplication::processEvents();
    QTest::mouseClick(canvas, Qt::LeftButton, {}, (centre - QPointF(0, 0.35 * side)).toPoint());
    QCOMPARE(doc->primarySelection(), video);
    // a click on the discs selects the sticker
    QTest::mouseClick(canvas, Qt::LeftButton, {}, centre.toPoint());
    QCOMPARE(doc->primarySelection(), s1);

    // drag it: the position follows the mouse, one undo step
    const double px0 = tf().positionX.value(t), py0 = tf().positionY.value(t);
    drag(canvas, centre, centre + QPointF(120, -60));
    QVERIFY2(std::abs(tf().positionX.value(t) - px0 - 120.0 / fr.width()) < 0.004, qPrintable(QString::number(tf().positionX.value(t))));
    QVERIFY2(std::abs(tf().positionY.value(t) - py0 + 60.0 / fr.height()) < 0.004, qPrintable(QString::number(tf().positionY.value(t))));
    QCOMPARE(doc->undoStack().undoLabel(), std::string("Move"));
    // a corner: 1.5x larger, both axes
    const double sc0 = tf().scaleX.value(t);
    const QPointF br = *canvas->handlePos("br"), pivot = *canvas->handlePos("anchor");
    drag(canvas, br, pivot + (br - pivot) * 1.5);
    QVERIFY2(std::abs(tf().scaleX.value(t) / sc0 - 1.5) < 0.05, qPrintable(QString::number(tf().scaleX.value(t) / sc0)));
    QCOMPARE(tf().scaleY.value(t), tf().scaleX.value(t));
    QCOMPARE(doc->undoStack().undoLabel(), std::string("Scale"));
    // the round knob: a quarter turn clockwise
    const QPointF knob = *canvas->handlePos("rotate"), p0 = *canvas->handlePos("anchor");
    const QPointF v = knob - p0;
    drag(canvas, knob, p0 + QPointF(-v.y(), v.x()));
    QVERIFY2(std::abs(tf().rotation.value(t) - 90.0) < 2.0, qPrintable(QString::number(tf().rotation.value(t))));
    shot(w.previewPanel(), "m10_sticker_handles.png");
    QVERIFY(doc->undo());   // one step each
    QVERIFY(std::abs(tf().rotation.value(t)) < 1e-9);

    // a template's media slot: the same handles
    const auto cells = ed->addTemplate("split.2v", Time(12));
    QCOMPARE(cells.size(), size_t(2));
    doc->setSelection({cells[0]});
    w.playback()->seek(390);   // 13 s
    QVERIFY(!waitForPreview(w, 390).isNull());
    QVERIFY(canvas->handlesVisible());
    const double slotX0 = clipOf(doc, cells[0]).transform.positionX.value(Time(1));
    const QPointF sc = *canvas->handlePos("center");
    drag(canvas, sc, sc + QPointF(80, 0));
    QVERIFY(std::abs(clipOf(doc, cells[0]).transform.positionX.value(Time(1)) - slotX0 - 80.0 / fr.width()) < 0.004);

    // a background sticker shrunk to 5 % (the user's screenshot): a corner brings it back to 20 %
    const std::string bg = ed->addSticker("bg.mint", Time(20));
    QVERIFY(!bg.empty());
    QVERIFY(ed->modifyClip(bg, "Scale", [](Clip& c) {
        c.transform.scaleX.setConstant(0.05);
        c.transform.scaleY.setConstant(0.05);
    }));
    w.playback()->seek(630);   // 21 s
    QVERIFY(!waitForPreview(w, 630).isNull());
    QVERIFY(canvas->handlesVisible());
    const QPointF bbr = *canvas->handlePos("br"), bpivot = *canvas->handlePos("anchor");
    drag(canvas, bbr, bpivot + (bbr - bpivot) * 4.0);
    const double bgScale = clipOf(doc, bg).transform.scaleX.value(Time(1));
    QVERIFY2(std::abs(bgScale - 0.2) < 0.01, qPrintable(QString::number(bgScale)));
    QVERIFY(!hasErrors(validate(doc->project())));
}

// M10: Lumacut is now MeerCut (the user's decision). The window and the About box carry the new name; Help > MeerCut
// on GitHub (CodeWithAmeer) and the About box point to the author's official GitHub; projects and autosaved copies are
// .mcproj while Lumacut's .lcproj files still open; the old name's settings are taken over once, paths following the
// moved data folder (tested on settings files in a temporary folder - the editor does it with the registry).
void GuiTests::m10RenamedToMeerCutWithGitHub() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    qputenv("MEERCUT_AUTOSAVE_DIR", tmp.filePath("autosave").toUtf8());
    MainWindow w(MainWindow::Options{false, true});
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QVERIFY2(w.windowTitle().endsWith("MeerCut") && !w.windowTitle().contains("Lumacut", Qt::CaseInsensitive),
             qPrintable(w.windowTitle()));
    // Help > MeerCut on GitHub (CodeWithAmeer) - the author's official page (not opened by the test)
    QMenu* help = nullptr;
    for (QAction* a : w.menuBar()->actions())
        if (a->text().remove('&') == "Help") help = a->menu();
    QVERIFY(help);
    QAction* github = nullptr;
    for (QAction* a : help->actions())
        if (a->data().toString() == "https://github.com/CodeWithAmeer") github = a;
    QVERIFY(github);
    QCOMPARE(github->text(), QString("MeerCut on GitHub (CodeWithAmeer)"));
    QVERIFY(github->statusTip().contains("https://github.com/CodeWithAmeer"));
    QCOMPARE(w.action("about")->text(), QString("About MeerCut"));
    const QString about = w.aboutText();
    qInfo("About: %s", qPrintable(about));
    QVERIFY(about.startsWith("<b>MeerCut</b>"));
    QVERIFY(about.contains("<a href=\"https://github.com/CodeWithAmeer\">CodeWithAmeer</a>"));
    QVERIFY(!about.contains("Lumacut", Qt::CaseInsensitive));

    // projects: saved as .mcproj; a Lumacut .lcproj opens the same way (the format did not change)
    Document* doc = w.document();
    const auto ids = w.importFiles({mediaPath("A_red_1080p30_10s.mp4")});
    QVERIFY(w.editController()->addToTimeline(ids[0], Time(0)));
    const QString mc = tmp.filePath("story.mcproj"), lc = tmp.filePath("story.lcproj");
    QVERIFY(w.saveProjectTo(mc));
    QVERIFY(QFile::copy(mc, lc));
    for (const QString& f : {mc, lc}) {
        MainWindow w2(MainWindow::Options{false, true});
        QVERIFY2(w2.openProjectFile(f), qPrintable(f));
        QVERIFY(w2.document()->sequence() == doc->sequence());
    }
    // autosaved copies are .mcproj
    QVERIFY(w.editController()->addTrack(TrackKind::Video));   // an unsaved change
    auto* autosave = w.findChild<AutosaveManager*>();
    QVERIFY(autosave);
    QSignalSpy saved(autosave, &AutosaveManager::autosaved);
    QVERIFY(autosave->autosaveNow());
    QCOMPARE(saved.size(), 1);
    QVERIFY2(saved.at(0).at(0).toString().endsWith(".mcproj"), qPrintable(saved.at(0).at(0).toString()));

    // the old name's settings, taken over once; a path into the old data folder follows it to MeerCut's; a setting
    // MeerCut already has when the takeover runs (written before it, or by a takeover cut short) is MeerCut's to keep,
    // and does not stop the rest from coming over
    const QString root = tmp.filePath("local");
    QVERIFY(QDir().mkpath(root + "/MeerCut/library/audio/sfx") && QDir().mkpath(root + "/MeerCut/models"));
    {
        QFile f(root + "/MeerCut/library/audio/sfx/pop.m4a");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("pop");
    }
    QSettings from(tmp.filePath("lumacut.ini"), QSettings::IniFormat), to(tmp.filePath("meercut.ini"), QSettings::IniFormat);
    from.setValue("recentProjects", QStringList{root + "/Lumacut/library/audio/sfx/pop.m4a", "C:/Videos/holiday.mcproj"});
    from.setValue("ai/modelsDir", root + "/Lumacut/models");
    from.setValue("layout/state", QByteArray("\x01\x02\x03", 3));
    from.setValue("preview/quality", "Full");
    to.setValue("preview/quality", "Half");
    QVERIFY(adoptLegacySettings(from, to, root.toStdWString()));
    const QStringList recent = to.value("recentProjects").toStringList();
    QCOMPARE(recent.size(), 2);
    QCOMPARE(recent.at(0), root + "/MeerCut/library/audio/sfx/pop.m4a");
    QCOMPARE(recent.at(1), QString("C:/Videos/holiday.mcproj"));   // not in the old folder: unchanged
    QCOMPARE(to.value("ai/modelsDir").toString(), root + "/MeerCut/models");
    QCOMPARE(to.value("layout/state").toByteArray(), QByteArray("\x01\x02\x03", 3));
    QCOMPARE(to.value("preview/quality").toString(), QString("Half"));   // MeerCut's own
    // once only: after that a setting MeerCut no longer has does not come back from Lumacut's
    to.remove("layout/state");
    QVERIFY(!adoptLegacySettings(from, to, root.toStdWString()));
    QVERIFY(!to.contains("layout/state"));
    // a takeover whose values were not all saved (QSettings only warns) is not recorded, and the next start completes
    // it; here MeerCut's settings file is read-only
    const QString lockedFile = tmp.filePath("meercut_locked.ini"), nextFile = tmp.filePath("meercut_next.ini");
    QSettings(lockedFile, QSettings::IniFormat).setValue("preview/quality", "Half");
    QVERIFY(QFile::setPermissions(lockedFile, QFileDevice::ReadOwner | QFileDevice::ReadUser));
    {
        QSettings locked(lockedFile, QSettings::IniFormat);
        bool incomplete = false;
        QVERIFY(adoptLegacySettings(from, locked, root.toStdWString(), &incomplete));
        QVERIFY(incomplete);
    }
    QVERIFY(QFile::copy(lockedFile, nextFile));   // what was saved: MeerCut's own value only
    QVERIFY(QFile::setPermissions(nextFile, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    {
        QSettings next(nextFile, QSettings::IniFormat);
        QVERIFY(!next.contains("recentProjects"));
        bool incomplete = true;
        QVERIFY(adoptLegacySettings(from, next, root.toStdWString(), &incomplete));
        QVERIFY(!incomplete);
        QCOMPARE(next.value("recentProjects").toStringList().size(), 2);
        QCOMPARE(next.value("preview/quality").toString(), QString("Half"));
        QVERIFY(!adoptLegacySettings(from, next, root.toStdWString()));   // recorded now
    }
    QVERIFY(QFile::setPermissions(lockedFile, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    // QSettings creates the registry key it reads, so the old key is looked for without reading it: a PC that never
    // had Lumacut gets no empty Lumacut key (this only reads the registry)
    const QString absent = QString("MeerCutTestNoSuchKey%1").arg(QCoreApplication::applicationPid());
    QVERIFY(!hasRegistrySettings(absent, absent));
    QVERIFY(!QSettings(R"(HKEY_CURRENT_USER\Software)", QSettings::NativeFormat).childGroups().contains(absent));
    QVERIFY(hasRegistrySettings("Microsoft", "Windows"));
    // back to the suite's temporary folder (unset, later tests would autosave into the user's own folder)
    qputenv("MEERCUT_AUTOSAVE_DIR", QDir(QDir::tempPath()).filePath("meercut_gui_tests_autosave").toUtf8());
}

// M11: Media > Sample Videos - five clips of 20 to 24 s to practise with, all 1280x720 at 24 fps: excerpts of two
// Blender Foundation films (credited, CC BY 3.0) and MeerCut's own green screen. Double-click, the menu or a drag adds
// one (copied into the library folder once, then imported like any file; a dragged one shows where it lands), and the
// green screen keys out with Chroma Key's default settings.
void GuiTests::m11SampleVideos() {
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1500, 900);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    MediaPanel* media = w.mediaPanel();
    w.resourceTabs()->setCurrentWidget(media);
    QListWidget* side = media->sidebar();
    QCOMPARE(side->count(), 4);
    QCOMPARE(side->item(3)->text(), QString("Sample Videos (5)"));
    QCOMPARE(media->samples().size(), size_t(5));
    for (const SampleVideo& s : media->samples()) {   // the files are what the index says
        const MediaInfo mi = probeMedia(std::filesystem::path(QDir(assetsDir()).filePath("samples/" + s.file).toStdWString()));
        QVERIFY2(mi.video, qPrintable(s.id));
        QCOMPARE(mi.video->width, 1280);
        QCOMPARE(mi.video->height, 720);
        QVERIFY2(mi.video->frameRate == FrameRate::fps24(), qPrintable(s.id));
        const Time length = mi.video->duration ? *mi.video->duration : *mi.duration;
        QVERIFY2(std::abs(length.toDouble() - s.seconds) < 0.05, qPrintable(s.id));
        QVERIFY2(s.seconds >= 19.9 && s.seconds <= 30.0, qPrintable(s.id));
        const bool own = s.id == "sample.green_screen_practice";
        QCOMPARE(s.credit.isEmpty(), own);
        if (!own) {
            QVERIFY2(s.credit.contains("(c) Blender Foundation") && s.licence == "CC BY 3.0" &&
                         s.licenceUrl == "https://creativecommons.org/licenses/by/3.0/",
                     qPrintable(s.id));
            QVERIFY2(mi.audio, qPrintable(s.id));   // with the film's sound
        }
    }

    // ---- the page: the five clips (no Import tile), their credits, search, and tiles with the clips' pictures
    side->setCurrentRow(3);
    QApplication::processEvents();
    MediaList* list = media->list();
    QStringList titles;
    for (int i = 0; i < list->count(); ++i) titles << list->item(i)->text();
    QCOMPARE(titles, QStringList({"Bridge Conversation", "Sci-Fi Lab Montage", "Snowy Adventure", "Dragon and Desert",
                                  "Green Screen Practice"}));
    QVERIFY(media->samplesHint()->isVisible());
    QVERIFY(media->samplesHint()->text().contains("Tears of Steel (c) Blender Foundation | mango.blender.org"));
    QVERIFY(media->samplesHint()->text().contains("Sintel (c) Blender Foundation | durian.blender.org"));
    QVERIFY(media->samplesHint()->text().contains("https://creativecommons.org/licenses/by/3.0/"));
    QVERIFY(list->item(0)->toolTip().contains("CC BY 3.0"));
    QVERIFY(list->item(4)->toolTip().contains("MeerCut's own clip"));
    media->search()->setText("dragon");
    QCOMPARE(list->count(), 1);
    media->search()->clear();
    QCOMPARE(list->count(), 5);
    auto tilesShowPictures = [&] {
        for (int i = 0; i < list->count(); ++i) {
            const QImage img = list->item(i)->icon().pixmap(QSize(168, 95)).toImage();
            if (img.pixelColor(img.width() / 2, img.height() / 3) == color::panelHeader) return false;
        }
        return true;
    };
    QTRY_VERIFY_WITH_TIMEOUT(tilesShowPictures(), 15000);   // every clip decodes
    shot(&w, "m11_01_sample_videos.png");

    // ---- double-click: copied into the library folder, imported and added at the playhead; the first clip sets the
    // empty project to its format (these tests match without asking)
    doubleClick(list->viewport(), list->visualItemRect(list->item(0)).center());
    QApplication::processEvents();
    QCOMPARE(doc->project().sources.size(), size_t(1));
    const MediaSource bridge = doc->project().sources[0];
    QVERIFY2(QString::fromStdString(bridge.path).contains("meercut_gui_tests_library"), bridge.path.c_str());
    QVERIFY(QFileInfo(QString::fromStdString(bridge.path)).isFile());
    QCOMPARE(doc->sequence().width, 1280);
    QCOMPARE(doc->sequence().height, 720);
    QVERIFY(doc->sequence().frameRate == FrameRate::fps24());
    std::vector<std::string> video;
    for (const auto& t : doc->sequence().tracks)
        if (t.kind == TrackKind::Video) video.push_back(t.id);
    auto clipsOn = [&](const std::string& track) { return doc->sequence().findTrack(track)->clips; };
    QCOMPARE(clipsOn(video[0]).size(), size_t(1));
    QCOMPARE(clipsOn(video[0])[0].sourceId, bridge.id);
    QCOMPARE(clipsOn(video[0])[0].timelineStart, Time(0));
    QCOMPARE(clipsOn(video[0])[0].duration, frameToTime(486, FrameRate::fps24()));
    bool withSound = false;
    for (const auto& t : doc->sequence().tracks)
        if (t.kind == TrackKind::Audio)
            for (const auto& c : t.clips) withSound |= c.sourceId == bridge.id;
    QVERIFY(withSound);
    QVERIFY(list->item(0)->toolTip().contains("Already in this project"));
    QVERIFY(!list->item(3)->toolTip().contains("Already in this project"));
    // a second use takes the same media; "Add to Project Media" imports without adding a clip
    QCOMPARE(media->addSample("sample.bridge_conversation", Time(25)), bridge.id);
    QCOMPARE(doc->project().sources.size(), size_t(1));
    QCOMPARE(clipsOn(video[0]).size(), size_t(2));
    QVERIFY(!media->addSample("sample.snowy_adventure", std::nullopt, false).empty());
    QCOMPARE(doc->project().sources.size(), size_t(2));
    QCOMPARE(clipsOn(video[0]).size(), size_t(2));
    QCOMPARE(side->item(0)->text(), QString("Project Media (2)"));
    QVERIFY(media->addSample("sample.no_such_clip").empty());

    // ---- drags: the dragon onto Video 2 and the green screen onto Video 3, each previewed on its track
    QToolButton* addVideo = nullptr;
    for (auto* b : w.timelinePanel()->findChildren<QToolButton*>())
        if (b->text() == "Video track") addVideo = b;
    QVERIFY(addVideo);
    for (int i = 0; i < 2; ++i) QTest::mouseClick(addVideo, Qt::LeftButton);
    video.clear();
    for (const auto& t : doc->sequence().tracks)
        if (t.kind == TrackKind::Video) video.push_back(t.id);
    QCOMPARE(video.size(), size_t(3));
    TimelineView* tl = w.timelinePanel()->view();
    tl->zoomToFit();
    QApplication::processEvents();
    auto dragOf = [&](const QString& id) {
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->data(Qt::UserRole + 3).toString() == id)
                return std::unique_ptr<QMimeData>(list->model()->mimeData({list->indexFromItem(list->item(i))}));
        return std::unique_ptr<QMimeData>();
    };
    auto dropOn = [&](const QMimeData& mime, const std::string& track, double seconds, std::string* ghost) {
        const QPoint pt(tl->xForTime(seconds), tl->trackLaneRect(track).center().y());
        QDragEnterEvent enter(pt, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tl, &enter);
        QDragMoveEvent move(pt, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tl, &move);
        if (ghost) *ghost = tl->dropGhostTrack();
        const bool moveOk = move.isAccepted() && move.dropAction() == Qt::CopyAction;
        QDropEvent drop(QPointF(pt), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tl, &drop);
        QApplication::processEvents();
        return moveOk && drop.isAccepted() && drop.dropAction() == Qt::CopyAction;
    };
    const auto dragon = dragOf("sample.dragon_and_desert");
    QVERIFY(dragon && dragon->hasFormat(kMimeLibraryVideo) && dragon->hasUrls());
    QVERIFY(dragon->urls().front().toLocalFile().contains("meercut_gui_tests_library"));
    std::string ghost;
    QVERIFY(dropOn(*dragon, video[1], 10.0, &ghost));
    QCOMPARE(ghost, video[1]);
    QCOMPARE(clipsOn(video[1]).size(), size_t(1));
    const Clip dragonClip = clipsOn(video[1])[0];
    QVERIFY(std::abs(dragonClip.timelineStart.toDouble() - 10.0) <= 1.0 / 24 + 1e-9);   // to the frame under the mouse
    QCOMPARE(dragonClip.duration, frameToTime(563, FrameRate::fps24()));
    const int64_t at = timeToFrame(dragonClip.timelineStart + Time(5), FrameRate::fps24(), Rounding::Nearest);
    w.playback()->seek(at);
    const QImage under = waitForPreview(w, at);
    QVERIFY(!under.isNull());
    const auto green = dragOf("sample.green_screen_practice");
    QVERIFY(green && dropOn(*green, video[2], dragonClip.timelineStart.toDouble(), &ghost));
    QCOMPARE(ghost, video[2]);
    QCOMPARE(clipsOn(video[2]).size(), size_t(1));
    const std::string screen = clipsOn(video[2])[0].id;
    QCOMPARE(clipsOn(video[2])[0].timelineStart, dragonClip.timelineStart);
    QCOMPARE(doc->project().sources.size(), size_t(4));

    // ---- Chroma Key with its default settings takes the green away: the dragon shows through, the badge stays
    auto greenShare = [](const QImage& img) {
        const QImage x = img.convertToFormat(QImage::Format_RGBA8888);
        int64_t n = 0;
        for (int y = 0; y < x.height(); ++y) {
            const uchar* p = x.constScanLine(y);
            for (int i = 0; i < x.width(); ++i, p += 4) n += p[1] > p[0] + 50 && p[1] > p[2] + 50;
        }
        return double(n) / (double(x.width()) * x.height());
    };
    w.playback()->seek(at);
    const QImage unkeyed = waitForPreview(w, at);
    QVERIFY(!unkeyed.isNull());
    QVERIFY2(greenShare(unkeyed) > 0.5, qPrintable(QString::number(greenShare(unkeyed))));
    QVERIFY(ed->addEffectToClips({screen}, makeEffect(*findEffectDef("fa.fx.chromakey"), "key")));
    w.playback()->seek(at);
    const QImage keyed = waitForPreview(w, at);
    QVERIFY(!keyed.isNull());
    const double left = greenShare(keyed);
    std::printf("  green screen: %.1f %% strong green unkeyed, %.3f %% keyed (the clip under it: %.3f %%)\n",
                100 * greenShare(unkeyed), 100 * left, 100 * greenShare(under));
    QVERIFY2(left < 0.005, qPrintable(QString::number(left)));
    // where the screen was, the picture is the dragon clip's (the right 40 %: away from the badge and its sparkles)
    const QImage a = keyed.convertToFormat(QImage::Format_RGBA8888), b = under.convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(a.size(), b.size());
    double diff = 0;
    int64_t count = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = a.width() * 6 / 10; x < a.width(); ++x, ++count)
            for (int c = 0; c < 3; ++c) diff += std::abs(int(a.constScanLine(y)[x * 4 + c]) - int(b.constScanLine(y)[x * 4 + c]));
    diff /= double(count) * 3;
    std::printf("  keyed vs. the clip under it (right 40 %%): mean difference %.2f levels\n", diff);
    QVERIFY2(diff < 3.0, qPrintable(QString::number(diff)));
    // the badge's orange, 5 s in: centre (470, 371) of 1280x720, this point inside its lower left
    const QColor badge = a.pixelColor(a.width() * 367 / 1280, a.height() * 474 / 720);
    QVERIFY2(badge.red() > 200 && badge.green() > 60 && badge.green() < 140 && badge.blue() < 90, qPrintable(badge.name()));
    shot(&w, "m11_02_green_screen_keyed.png");
}

// The pictures in the README (MeerCut 1.0.0): a short film made from the sample videos - a title, transitions, an
// effect, a look, a sticker, music and Background Removal - shown on the editor's pages. They are saved when
// FA_SCREENSHOT_DIR is set; the checks make sure each page shows what its picture is meant to show.
void GuiTests::readmeScreenshots() {
    QTemporaryDir cacheDir;
    qputenv("MEERCUT_ANALYSIS_CACHE", QDir::toNativeSeparators(cacheDir.path()).toUtf8());
    ai::setModelsDirectoryOverride(std::filesystem::path(FA_MODELS_DIR));
    MainWindow w(MainWindow::Options{false, true});
    w.resize(1600, 960);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    Document* doc = w.document();
    EditController* ed = w.editController();
    MediaPanel* media = w.mediaPanel();
    QObject::connect(ed, &EditController::message, &w, [](const QString& m) { qInfo("editor says: %s", qPrintable(m)); });
    const FrameRate fps = FrameRate::fps24();
    auto frameAt = [&](double seconds) { return timeToFrame(Time::fromDouble(seconds, 1000), fps, Rounding::Nearest); };
    auto settle = [](int ms) {   // thumbnails and hover previews finish drawing
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QTest::qWait(20);
        }
    };
    auto clipsOf = [&](TrackKind kind) {
        std::vector<std::string> ids;
        for (const auto& t : doc->sequence().tracks)
            if (t.kind == kind) ids.push_back(t.id);
        return ids;
    };
    auto snap = [&](const char* name) {   // without the last status message (they come and go)
        w.statusBar()->clearMessage();
        shot(&w, name);
    };
    auto button = [&](const QString& text) {
        for (auto* b : w.timelinePanel()->findChildren<QToolButton*>())
            if (b->text() == text) return b;
        return static_cast<QToolButton*>(nullptr);
    };

    // ---- Video 1: four sample clips cut to 6-8 s, back to back (imported first in the order the media tiles show them;
    // the snowy clip opens on black)
    for (const char* id : {"sample.dragon_and_desert", "sample.bridge_conversation", "sample.scifi_lab_montage",
                           "sample.snowy_adventure"})
        QVERIFY2(!media->addSample(id, std::nullopt, false).empty(), id);
    struct Cut {
        const char* id;
        int in, out;
    };
    std::vector<std::string> v1;
    int at = 0;
    for (const Cut& c : {Cut{"sample.snowy_adventure", 2, 8}, Cut{"sample.dragon_and_desert", 3, 9},
                         Cut{"sample.bridge_conversation", 6, 14}, Cut{"sample.scifi_lab_montage", 0, 8}}) {
        QVERIFY2(!media->addSample(c.id, Time(at)).empty(), c.id);
        const std::string clip = doc->primarySelection();
        QVERIFY2(edit::findClip(doc->sequence(), clip), clip.c_str());
        if (c.in > 0) QVERIFY(ed->trimClip(clip, ClipEdge::Head, Time(at + c.in)));   // (no cut: a refused no-op)
        QVERIFY(ed->trimClip(clip, ClipEdge::Tail, Time(at + c.out)));
        if (clipOf(doc, clip).timelineStart != Time(at)) QVERIFY(ed->moveClip(clip, Time(at), std::nullopt));
        QCOMPARE(clipOf(doc, clip).sourceIn, Time(c.in));
        v1.push_back(clip);
        at += c.out - c.in;
    }
    QCOMPARE(doc->sequence().width, 1280);   // the first sample set the project to its format
    QVERIFY(ed->addTransition(v1[0], Time(1), TransitionAlignment::Centered, "fa.transition.circle_open"));
    QVERIFY(ed->addTransition(v1[1], Time(1), TransitionAlignment::Centered, "fa.transition.dissolve"));
    QVERIFY(ed->addTransition(v1[2], Time(1), TransitionAlignment::Centered, "fa.transition.blur"));

    // ---- a title, a light leak and a sticker above; music on a track of its own
    QVERIFY(ed->addTitleFromTemplate("opener.cinematic", Time(0)));
    std::string title;
    for (const auto& t : doc->sequence().tracks)
        for (const auto& c : t.clips)
            if (c.isTitle()) title = c.id;
    QVERIFY(!title.empty());
    QVERIFY(!ed->addEffectLayer(makeEffect(*findEffectDef("fa.fx.lightleak"), "leak"), "Light Leak", Time(7), Time(4)).empty());
    QVERIFY(!ed->addSticker("badge.wow", Time(23)).empty());
    QToolButton* addAudio = button("Audio track");
    QVERIFY(addAudio);
    QTest::mouseClick(addAudio, Qt::LeftButton);
    const QString music = materializeAsset("audio/music/epic_trailer.m4a");
    QVERIFY(!music.isEmpty());
    const auto musicIds = w.importFiles({music});
    QCOMPARE(musicIds.size(), size_t(1));
    QVERIFY(ed->addToTimeline(musicIds[0], Time(0), {}, clipsOf(TrackKind::Audio).back()));
    TimelineView* tl = w.timelinePanel()->view();
    tl->zoomToFit();

    // ---- 1. the editor: the title over the mountains, its inspector, the project's media
    w.resourceTabs()->setCurrentWidget(media);
    media->sidebar()->setCurrentRow(0);
    doc->setSelection({title});
    w.playback()->seek(frameAt(2.5));
    QVERIFY(!waitForPreview(w, frameAt(2.5)).isNull());
    settle(1500);
    snap("readme_01_editor.png");

    // ---- 2. titles
    w.resourceTabs()->setCurrentWidget(w.titlesPanel());
    w.titlesPanel()->categories()->setCurrentRow(4);   // Openers
    settle(800);
    snap("readme_02_titles.png");

    // ---- 3. transitions: half way through the circle opening onto the dragon clip
    w.resourceTabs()->setCurrentWidget(w.transitionsPanel());
    doc->setSelectedTransition(v1[0]);
    w.playback()->seek(frameAt(6.0));
    QVERIFY(!waitForPreview(w, frameAt(6.0)).isNull());
    settle(1200);
    snap("readme_03_transitions.png");

    // ---- 4. effects: Old Film on the bridge scene, with its settings
    EffectsBrowser* fx = w.effectsPage();
    w.resourceTabs()->setCurrentWidget(fx);
    doc->setSelection({v1[2]});
    QVERIFY(fx->apply("fa.fx.oldfilm"));
    QCOMPARE(clipOf(doc, v1[2]).effects.back().type, std::string("fa.fx.oldfilm"));
    fx->selectCategory("Film");
    selectTab(w.inspector()->clipTabs(), "Effects");
    w.playback()->seek(frameAt(16.0));
    QVERIFY(!waitForPreview(w, frameAt(16.0)).isNull());
    settle(1500);
    snap("readme_04_effects.png");

    // ---- 5. filters: a look on the dragon clip, under the light leak
    EffectsBrowser* fi = w.filtersPage();
    w.resourceTabs()->setCurrentWidget(fi);
    doc->setSelection({v1[1]});
    QVERIFY(fi->apply("look:golden-hour"));
    QCOMPARE(clipOf(doc, v1[1]).effects.back().strings.at("lut"), std::string("builtin:golden-hour"));
    fi->selectCategory("Cinematic");
    selectTab(w.inspector()->clipTabs(), "Effects");
    w.playback()->seek(frameAt(9.5));
    QVERIFY(!waitForPreview(w, frameAt(9.5)).isNull());
    settle(1500);
    snap("readme_05_filters.png");

    // ---- 6. Media > Sample Videos and 7. the Audio page
    w.resourceTabs()->setCurrentWidget(media);
    media->sidebar()->setCurrentRow(3);
    settle(2500);   // the tiles' frames are decoded in the background
    snap("readme_06_sample_videos.png");
    AudioPage* audio = w.audioPage();
    w.resourceTabs()->setCurrentWidget(audio);
    audio->selectCategory("music:");
    settle(800);
    snap("readme_07_audio.png");

    // ---- 8. AI Background Removal: the two people from the bridge, cut out over the desert clip
    const ai::ModelInfo* rvm = ai::findModel("rvm-mobilenetv3");
    if (ai::modelStatus(*rvm, false, FA_MODELS_DIR) != ai::ModelStatus::Installed) {
        qInfo("RVM is not installed: no Background Removal picture");
        return;
    }
    QToolButton* addVideo = button("Video track");
    QVERIFY(addVideo);
    QTest::mouseClick(addVideo, Qt::LeftButton);
    const std::string top = clipsOf(TrackKind::Video).back();
    const std::string bridgeSource = clipOf(doc, v1[2]).sourceId;
    QVERIFY(ed->addToTimeline(bridgeSource, Time(6), top));
    std::string person;
    for (const auto& c : doc->sequence().findTrack(top)->clips) person = c.id;
    QVERIFY(!person.empty());
    QVERIFY(ed->trimClip(person, ClipEdge::Head, Time(6 + 9)));
    QVERIFY(ed->trimClip(person, ClipEdge::Tail, Time(6 + 9 + 5)));
    QVERIFY(ed->moveClip(person, Time(7), std::nullopt));
    doc->setSelection({person});
    w.playback()->seek(frameAt(9.5));
    QVERIFY(!waitForPreview(w, frameAt(9.5)).isNull());
    w.action("aiCutout")->trigger();
    QApplication::processEvents();
    CutoutPanel* cp = w.inspector()->cutoutPanel();
    auto* preview = cp->findChild<QPushButton*>("cutoutPreview");
    auto* enable = cp->findChild<QCheckBox*>("cutoutEnable");
    QVERIFY(preview && enable);
    QTest::mouseClick(preview, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!w.analysis()->busy(), 90000);
    QTRY_VERIFY(enable->isChecked());
    const QImage cut = waitForPreview(w, frameAt(9.5));
    QVERIFY(!cut.isNull());
    w.resourceTabs()->setCurrentWidget(media);
    media->sidebar()->setCurrentRow(0);
    settle(1500);
    snap("readme_08_background_removal.png");
}

QTEST_MAIN(GuiTests)
#include "test_gui.moc"
