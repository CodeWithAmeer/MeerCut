// Preview monitor surface with direct manipulation of the selected clip (Filmora-style):
//   * Transform editing (Video > Basic): drag inside the frame = move, corner handles = scale
//     (uniform while the aspect lock is on; edge handles appear when it is off), round handle above
//     the frame = rotate (Shift: 15 degree steps), Ctrl+drag the centre cross = anchor point (the
//     picture stays put). Shift constrains moves to one axis.
//   * Mask editing (Video > Mask, a mask active): drag inside = move the mask, corner handles =
//     resize, round handle = rotate, custom (polygon) masks: drag the points.
//   * Titles and captions (M3): the box is the text block (with its background padding); move, scale,
//     rotate and anchor work as for video. Clicking text in the player selects its title / caption.
//   * Stickers and template slots (M10): handled like video; the box hugs the sticker's art (over its whole
//     animation), so clicking the transparent part of its picture reaches the clip below.
//   * Motion tracking (M5, Video > Tracking with Motion Tracking on): the tracking box (Document::trackingBox):
//     drag inside = move, corner handles = resize. It is UI state (no undo entry). The region tracked at the
//     playhead (the clip's tracking result) is drawn as a dashed outline.
// A gesture is shown live through Document::previewEdit (renderers only, no undo entry) and committed
// as one undoable command on release. Animated properties get a keyframe at the playhead (auto-key).
#pragma once

#include "document.h"

#include "fa/render/compositing.h"
#include "fa/render/transitions.h"

#include <QImage>
#include <QPolygonF>
#include <QWidget>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace fa::text {
class TitleEngine;
}

namespace fa::ui {

class EditController;
class PlaybackController;

class PreviewCanvas : public QWidget {
    Q_OBJECT
public:
    PreviewCanvas(Document* doc, EditController* edit, PlaybackController* playback, QWidget* parent = nullptr);
    ~PreviewCanvas() override;
    void setFrame(const QImage& img, const QStringList& offline);
    void setCompare(bool on);   // draws the before / after divider
    QImage image() const { return image_; }

    // Where the sequence frame is displayed (widget coordinates).
    QRectF frameRect() const;
    // Handle positions in widget coordinates (for tests and the practical check):
    // transform: "tl" "tr" "br" "bl" "t" "r" "b" "l" "rotate" "anchor" "center";
    // mask: "m:tl" ... "m:rotate" "m:center" and "m:p0".."m:pN" (custom mask points);
    // tracking box: "k:tl" "k:tr" "k:br" "k:bl" "k:center".
    std::optional<QPointF> handlePos(const QString& name) const;
    bool handlesVisible() const;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    struct Target {   // the clip being edited at the playhead
        const Clip* clip = nullptr;
        Time t;                 // source time at the playhead
        int srcW = 0, srcH = 0;
        bool locked = false;
        // titles: "source" space is the text block (canvas px); the reference point is where the position lands
        bool title = false;
        bool caption = false;
        bool adjustment = false;   // adjustment layer: the canvas is its picture, only its masks are edited
        QRectF art{0, 0, 1, 1};    // M10: the part of the picture with something in it (fractions; stickers)
        double refX = 0, refY = 0, blockW = 0, blockH = 0, pad = 0;
        LayerAdjust adj;        // whole-title animation at the playhead
        TextStyle trackStyle;   // captions: subtitle track style and line
        double subtitleY = 0.92;
    };
    struct LayerGeo {           // sequence pixel space
        Affine2D a;             // source px -> sequence px
        QPointF corners[4];     // cropped picture: tl, tr, br, bl
        QPointF pivot;
    };
    struct MaskGeo {            // sequence pixel space
        QPolygonF outline;
        QPointF corners[4];     // mask box: tl, tr, br, bl
        QPointF center;
        std::vector<QPointF> points;   // custom mask points
    };

    std::optional<Target> target() const;
    std::optional<Target> makeTarget(const Clip& c, const Track& tr, const Time& timelineTime) const;
    Vec2 basePosition(const Clip& c, const Target& tg) const;   // captions on their subtitle line
    const Mask* targetMask(const Clip& c) const;
    LayerGeo layerGeo(const Clip& c, const Target& tg) const;
    MaskGeo maskGeo(const Clip& c, const Mask& m, const Target& tg) const;
    bool trackingMode(const Target& tg) const;   // the tracking box of the target clip is being placed
    QPointF boxToSeq(const Clip& c, const Target& tg, double fx, double fy) const;   // picture fractions -> seq px
    QPointF seqToBox(const Clip& c, const Target& tg, const QPointF& seq) const;
    std::optional<QRectF> trackedRegion(const Clip& c, const Target& tg) const;   // picture fractions at the playhead
    QPointF toWidget(const QPointF& seq) const;
    QPointF toSeq(const QPointF& widget) const;
    std::map<QString, QPointF> handles() const;   // widget coordinates
    QString handleAt(const QPointF& widgetPos, Qt::KeyboardModifiers mods = Qt::NoModifier) const;
    void updateGesture(const QPointF& widgetPos, Qt::KeyboardModifiers mods);
    void applyPreview();

    Document* doc_;
    EditController* edit_;
    PlaybackController* playback_;
    std::unique_ptr<text::TitleEngine> titleEngine_;   // GUI-thread layouts for title boxes
    QImage image_;
    QStringList offline_;
    bool compare_ = false;

    // gesture
    QString grab_;                    // handle name, "move" / "m:move", empty = none
    std::string clipId_;
    std::optional<Clip> clip0_;       // clip at the start of the gesture
    std::optional<Clip> live_;        // clip with the gesture applied (drawn while dragging)
    Target target0_;
    QPointF press_;                   // sequence coordinates
    std::function<void(Clip&)> change_;
    std::optional<Document::TrackingBox> box0_;   // tracking box at the start of the gesture
    QString label_;
    QString info_;                    // value read-out next to the cursor
    QPointF infoPos_;
};

}  // namespace fa::ui
