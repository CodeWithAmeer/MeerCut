// Interactive multitrack timeline (custom painted).
//
// Layout: ruler on top; per track a header (name, lock, visibility/mute) and a lane. Video tracks are
// listed top-down from the highest (front-most) to Video 1, audio tracks below (Filmora layout).
// Interaction: click = select (Ctrl toggles), drag body = move (other tracks of the same kind allowed),
// drag edge = trim, ruler = scrub, Ctrl+wheel = zoom, wheel = scroll, Shift+wheel = vertical scroll.
// Drops: media from the library (adds at the drop time; sounds on the audio track under the cursor), transitions
// (onto a cut) and titles (at the drop time: titles on the video track under the cursor or above the footage,
// captions on a subtitle track). M10: effects and filters land like titles, as their own clip above the video that
// affects every track below it (Ctrl+drop onto a clip, and keying effects, apply to that clip instead); the drop
// preview shows the track the item will really land on.
// Keyframe markers (diamonds on the clip bottom edge): click = seek, drag = move in time,
// right-click = interpolation / delete. Transitions are selectable (inspector shows their settings).
// Audio clips (M6) show their volume envelope (the audio.volume keyframes; 0 dB near the top, -60 dB at the bottom)
// and fades. On the selected clip: drag the line = raise / lower the volume (every point by the same amount),
// Ctrl+click or double-click the line = add a point, drag a point = move it (Shift: level only), double-click a point
// = delete it, right-click a point = interpolation; the square handles at the top corners drag the fade lengths.
// Audio track headers have Solo.
// Every edit is executed through EditController -> Document (undoable); the view only previews
// a gesture with a ghost rectangle until the mouse is released.
#pragma once

#include "document.h"

#include <QElapsedTimer>
#include <QHash>
#include <QWidget>

class QMenu;
class QMimeData;

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fa::ui {

class EditController;
class MediaService;
class PlaybackController;

inline constexpr const char* kMimeSource = "application/x-meercut-source";
inline constexpr const char* kMimeTransition = "application/x-meercut-transition";
inline constexpr const char* kMimeTitle = "application/x-meercut-title";   // title template id

class TimelineView : public QWidget {
    Q_OBJECT
public:
    TimelineView(Document* doc, EditController* edit, MediaService* media, PlaybackController* playback,
                 QWidget* parent = nullptr);

    // M9: the main window's commands by name; the clip menu shows them (same keys, same enabled state)
    void setNamedActions(const std::map<QString, QAction*>* actions) { named_ = actions; }

    static constexpr int kHeaderW = 150;
    static constexpr int kRulerH = 30;
    static constexpr int kVideoH = 74;
    static constexpr int kAudioH = 54;
    static constexpr int kSubtitleH = 44;

    double pixelsPerSecond() const { return pps_; }
    double scrollSeconds() const { return scrollT_; }
    int scrollY() const { return scrollY_; }
    double contentSeconds() const;   // scrollable length
    int contentHeight() const;
    double visibleSeconds() const;

    void setPixelsPerSecond(double pps, int anchorX = -1);
    void zoomBy(double factor, int anchorX = -1);
    void zoomToFit();
    void setScrollSeconds(double s);
    void setScrollY(int y);
    bool snapping() const { return snapping_; }
    void setSnapping(bool on) { snapping_ = on; }

    // Geometry helpers (also used by GUI tests).
    int xForTime(double seconds) const;
    // M5 Render Preview: runs of rendered frames [from, to), drawn as a green bar under the ruler.
    void setRenderedRanges(std::vector<std::pair<int64_t, int64_t>> ranges);
    const std::vector<std::pair<int64_t, int64_t>>& renderedRanges() const { return rendered_; }
    double timeForX(int x) const;
    QRect clipRect(const std::string& clipId) const;      // in widget coordinates (empty if not visible)
    QRect trackLaneRect(const std::string& trackId) const;
    QRect transitionRect(const std::string& clipId) const;   // transition owned by clipId (empty if none)
    // Centre of the keyframe marker of a clip at a source time (keyframe markers sit on the clip's bottom edge).
    QPoint keyframePos(const std::string& clipId, const Time& sourceTime) const;

    // M10 "Delete this gap" (Clipchamp): the empty stretch of a track under the mouse is highlighted with a trash
    // button in its middle; clicking it closes the gap (edit::deleteGap). Right-click in a gap: Delete Gap.
    struct GapHover {
        std::string trackId;
        TimeRange range;
    };
    const std::optional<GapHover>& hoveredGap() const { return gapHover_; }
    QRect gapButtonRect() const;   // empty when no gap is hovered or it is too narrow for the button
    // While something is dragged over the timeline (M10, also for GUI tests): the track it will land on, or a new
    // video track on top; the clip an effect will be applied to instead (Ctrl+drop, or a keying effect).
    const std::string& dropGhostTrack() const { return dropGhostTrack_; }
    bool dropNeedsNewTrack() const { return dropNewTrack_; }
    const std::string& dropEffectTarget() const { return dropEffectClip_; }

signals:
    void speedTabRequested();   // M5: "Speed Settings..." in the clip menu
    void videoSubTabRequested(const QString& name);   // M5: "Motion Tracking..." / "Stabilization..." in the clip menu
    void aiToolRequested(const QString& actionName);  // M7: the clip menu's AI Tools (main window action names)
    void viewChanged();   // zoom / scroll / content size changed
    void subtitleFilesDropped(const QStringList& files, Time at);   // .srt / .ass / .ssa dropped at a time

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    const std::map<QString, QAction*>* named_ = nullptr;
    QAction* namedAction(const char* name) const;
    struct Row {
        const Track* track = nullptr;
        int y = 0;
        int h = 0;
    };
    enum class Part { None, Body, Head, Tail };
    struct Hit {
        std::string clipId;
        std::string trackId;
        Part part = Part::None;
    };
    enum class Mode { None, Scrub, PendingMove, Move, Trim, KeyDrag, FadeDrag, EnvelopeDrag };
    struct KeyHit {
        std::string clipId;
        Time sourceTime;
    };

    // M6 audio overlay: what is under the mouse on an audio clip's waveform.
    enum class AudioPart { None, FadeIn, FadeOut, EnvPoint, EnvLine };
    struct AudioHit {
        std::string clipId;
        AudioPart part = AudioPart::None;
        Time keyTime;   // EnvPoint: the volume key (source time)
    };
    AudioHit audioHitAt(const QPoint& p) const;
    int fadeHandleX(const Clip& c, bool fadeIn) const;
    void paintAudioOverlay(QPainter& p, const Clip& c, const QRect& rect);
    void audioDragTo(QMouseEvent* e);   // FadeDrag / EnvelopeDrag

    std::vector<Row> rows() const;
    const Row* rowAt(int y) const;
    Hit hitTest(const QPoint& p) const;
    std::optional<KeyHit> keyAt(const QPoint& p) const;
    std::string transitionAt(const QPoint& p) const;
    Time frameTimeAt(int x) const;           // frame-aligned timeline time under x (>= 0)
    std::vector<Time> snapPoints(const std::vector<std::string>& exclude) const;
    // Snap t to the nearest point within the snap distance; returns the snapped time or t.
    Time snap(Time t, const std::vector<Time>& points, bool* snapped = nullptr) const;
    Time quantize(double seconds) const;
    void seekToX(int x);
    // false = cannot drop here. Ctrl: an effect dropped on a clip applies to that clip (M10).
    bool updateDropTarget(const QMimeData* mime, const QPoint& pos, Qt::KeyboardModifiers mods = {});
    void placeGeneratedGhost();   // M10: dropGhostTrack_ / dropNewTrack_ for a title, sticker or layer being dropped
    // M10 gaps
    std::optional<GapHover> gapUnder(const QPoint& pos) const;
    void updateGapHover(const QPoint& pos);
    void clearGapHover();
    void paintGapHighlight(QPainter& p);
    void paintGapButton(QPainter& p);
    QString sourcePath(const Clip& c) const;
    bool sourceMissing(const Clip& c) const;
    // File existence per path, re-checked at most every 2 s: painting stays off the disk (a check through the
    // OneDrive / network file system can take milliseconds per clip, every repaint).
    struct ExistsCheck {
        bool exists = false;
        qint64 checkedMs = 0;
    };
    mutable QHash<QString, ExistsCheck> existsCache_;
    mutable QElapsedTimer existsClock_;
    // M9: during a repaint each source is checked once, not once per clip (thousands of clips in view)
    mutable std::unordered_map<std::string, bool> missingInPaint_;
    bool painting_ = false;

    void paintRuler(QPainter& p);
    QRect addTrackButtonRect() const { return QRect(kHeaderW - 28, 5, 20, 20); }
    void paintHeader(QPainter& p, const Row& r);
    void paintClip(QPainter& p, const Row& r, const Clip& c, const QRect& rect, bool ghost = false);
    void paintTransitions(QPainter& p, const Row& r);
    void paintKeyframes(QPainter& p, const Clip& c, const QRect& rect);
    QRect transitionRectIn(const Row& r, size_t clipIndex) const;
    void keyframeMenu(QMenu& menu, const KeyHit& k);

    Document* doc_;
    EditController* edit_;
    MediaService* media_;
    PlaybackController* playback_;

    double pps_ = 40.0;
    std::vector<std::pair<int64_t, int64_t>> rendered_;   // Render Preview (M5)
    double scrollT_ = 0.0;
    int scrollY_ = 0;
    bool snapping_ = true;

    // gesture state
    Mode mode_ = Mode::None;
    QPoint pressPos_;
    Hit pressHit_;
    Time origStart_, origEnd_;
    Time ghostStart_, ghostEnd_;
    std::string ghostTrack_;
    ClipEdge trimEdge_ = ClipEdge::Head;
    Time trimMin_, trimMax_;
    std::vector<Time> dragSnaps_;
    std::optional<Time> snapLine_;
    std::optional<KeyHit> keyDrag_;   // keyframe being dragged
    Time keyGhost_;                   // its timeline time while dragging
    AudioHit audioDrag_;              // fade handle / envelope point or line being dragged (M6)
    std::vector<Keyframe<double>> envOrigKeys_;   // the volume keys when an envelope line drag began
    double envOrigConstant_ = 0.0;
    Time envOrigin_;                  // where the dragged point was when the drag began
    int gesture_ = 0;                 // numbers drags: each one is one undo step

    // drag & drop state
    bool dropActive_ = false;
    bool dropIsTransition_ = false;
    std::string dropSource_;
    std::string dropTitle_;   // title template id
    std::string dropTrack_;
    Time dropTime_;
    Time dropDuration_;
    std::string dropCutClip_;
    std::string dropEffectClip_;   // clip an effect from the Effects / Filters page would land on
    bool dropAdjustment_ = false;  // a new adjustment layer is being dragged in
    std::string dropSticker_;      // M9: a sticker / template being dragged in
    std::string dropTemplate_;
    std::string dropReplace_;      // M9: a template's media slot that the dragged media will replace
    std::string dropGhostTrack_;   // M10: where the dragged item will land ...
    bool dropNewTrack_ = false;    // ... or on a new video track on top
    bool dropEffectLayer_ = false; // M10: an effect / filter dropped as its own clip above the video
    QByteArray dropEffectPayload_; // the dragged effect, parsed once per drag
    Clip dropEffectHolder_;
    QString dropLibraryFile_;      // M10: a sound from the Audio page (its file), dropped on the audio track under the
                                   // mouse; M11: a sample video from the Media page, on the video track

    // M10: Delete this gap
    std::optional<GapHover> gapHover_;
    QString gapProblem_;           // why the hovered gap cannot be deleted ("" = it can)
    bool gapButtonHot_ = false;
};

}  // namespace fa::ui
