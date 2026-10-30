#include "timeline_view.h"

#include "library_pages.h"
#include "templates.h"
#include "fa/text/stickers.h"

#include "effects_browser.h"
#include "fa/render/effects.h"
#include "fa/render/lut.h"
#include "fa/model/serialize.h"

#include "edit_controller.h"
#include "media_service.h"
#include "playback_controller.h"
#include "theme.h"
#include "title_library.h"

#include "fa/model/edit_ops.h"
#include "fa/model/keyframes.h"
#include "fa/model/serialize.h"
#include "fa/model/time_map.h"
#include "fa/render/audio_dsp.h"
#include "fa/render/text_animation.h"
#include "fa/render/transitions.h"

#include <QContextMenuEvent>
#include <QCursor>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QMenu>
#include <QHBoxLayout>
#include <QToolButton>
#include <QWidgetAction>
#include <QMimeData>
#include <QUrl>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace fa::ui {

namespace {

// Track header buttons: under the name on video/audio rows; right of the name on the short subtitle rows.
void headerButtons(int y, int h, bool compact, QRect& lock, QRect& vis) {
    const int by = compact ? y + (h - 20) / 2 : y + h - 28;
    lock = compact ? QRect(TimelineView::kHeaderW - 58, by, 20, 20) : QRect(12, by, 20, 20);
    vis = compact ? QRect(TimelineView::kHeaderW - 32, by, 20, 20) : QRect(40, by, 20, 20);
}


// Solo (M6), right of the mute button on audio rows.
QRect soloButton(int y, int h) { return QRect(68, y + h - 28, 20, 20); }

// M6 volume envelope over the waveform area (the inspector's range): -60 dB at the bottom, 0 dB 4 px below the top
// (an unchanged clip shows its line along the top, away from the middle where clips are grabbed), +12 dB at the top;
// linear in dB on each side of 0 dB.
constexpr double kEnvMinDb = -60.0;
constexpr double kEnvMaxDb = 12.0;
constexpr int kClipLabelH = 16;   // label strip of a clip (paintClip)
QRect audioContent(const QRect& clip) {
    return QRect(clip.x(), clip.y() + kClipLabelH, clip.width(), clip.height() - kClipLabelH);
}
double envY(const QRect& content, double db) {
    db = std::clamp(db, kEnvMinDb, kEnvMaxDb);
    const double y0 = content.top() + 4.0, yTop = content.top() + 1.0, yBottom = content.bottom() - 1.0;
    return db >= 0 ? y0 - (y0 - yTop) * db / kEnvMaxDb : y0 + (yBottom - y0) * db / kEnvMinDb;
}
double envDb(const QRect& content, double y) {
    const double y0 = content.top() + 4.0, yTop = content.top() + 1.0, yBottom = content.bottom() - 1.0;
    const double db = y <= y0 ? kEnvMaxDb * (y0 - y) / (y0 - yTop) : kEnvMinDb * (y - y0) / std::max(1.0, yBottom - y0);
    return std::clamp(db, kEnvMinDb, kEnvMaxDb);
}
Time timeAtSeconds(double s) { return Rational::fromDouble(s, 100000); }

// Audio crossfades show their shape; picture transitions their name.
QString transitionLabel(const std::string& type) {
    if (isAudioCrossfade(type)) {
        CrossfadeShape sh;
        if (crossfadeShapeFromId(type.substr(std::char_traits<char>::length(kAudioCrossfadePrefix)), sh))
            return QString("Crossfade: ") + QString::fromUtf8(crossfadeShapeLabel(sh));
        return "Crossfade";
    }
    const TransitionInfo* info = findTransition(type);
    return info ? QString::fromUtf8(info->label) : QString();
}
std::vector<std::pair<QString, std::string>> crossfadeChoices() {
    std::vector<std::pair<QString, std::string>> out;
    for (CrossfadeShape sh : {CrossfadeShape::ConstantGain, CrossfadeShape::EqualPower, CrossfadeShape::Exponential,
                              CrossfadeShape::Logarithmic})
        out.emplace_back(QString::fromUtf8(crossfadeShapeLabel(sh)), crossfadeShapeId(sh));
    return out;
}

constexpr int kSnapPx = 8;
constexpr int kEdgePx = 6;
constexpr int kSectionGap = 6;

// M10: every drop onto the timeline is a copy - a library item stays in its page, and Explorer must never be told
// that a file was moved (on one drive it proposes Move, and a source told "moved" may delete the original).
void acceptCopy(QDropEvent* e) {
    if (e->possibleActions() & Qt::CopyAction) {
        e->setDropAction(Qt::CopyAction);
        e->accept();
    } else {
        e->ignore();
    }
}

QString tcLabel(double seconds, const FrameRate& rate, bool frames) {
    const int64_t f = timeToFrame(Rational::fromDouble(seconds, 100000), rate, Rounding::Nearest);
    if (frames) return QString::fromStdString(formatTimecode(f, rate));
    const int64_t s = static_cast<int64_t>(std::llround(seconds));
    if (s >= 3600) return QString("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
    return QString("%1:%2").arg(s / 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
}

}  // namespace

TimelineView::TimelineView(Document* doc, EditController* edit, MediaService* media, PlaybackController* playback,
                           QWidget* parent)
    : QWidget(parent), doc_(doc), edit_(edit), media_(media), playback_(playback) {
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumHeight(kRulerH + kVideoH + kAudioH + 20);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAccessibleName("Timeline");
    connect(doc_, &Document::changed, this, [this] {
        update();
        emit viewChanged();
    });
    connect(doc_, &Document::selectionChanged, this, qOverload<>(&QWidget::update));
    connect(doc_, &Document::playheadChanged, this, [this](qint64 f) {
        // keep the playhead visible while playing
        const double t = frameToTime(f, doc_->sequence().frameRate).toDouble();
        const double vis = visibleSeconds();
        if (playback_->isPlaying() && (t > scrollT_ + vis * 0.95 || t < scrollT_)) setScrollSeconds(t - vis * 0.05);
        update();
    });
    connect(media_, &MediaService::thumbnailReady, this, qOverload<>(&QWidget::update));
    connect(media_, &MediaService::waveformReady, this, qOverload<>(&QWidget::update));
    // M10: an edit, undo, scroll or zoom can close or move the hovered gap - look again under the mouse
    connect(this, &TimelineView::viewChanged, this, [this] {
        if (!gapHover_) return;
        clearGapHover();
        if (underMouse()) updateGapHover(mapFromGlobal(QCursor::pos()));
    });
}

// ---- geometry --------------------------------------------------------------------------------

double TimelineView::contentSeconds() const {
    return std::max(60.0, doc_->sequence().duration().toDouble() + visibleSeconds() * 0.5);
}

double TimelineView::visibleSeconds() const { return std::max(1, width() - kHeaderW) / pps_; }

int TimelineView::contentHeight() const {
    int h = kRulerH + kSectionGap;
    for (const auto& t : doc_->sequence().tracks)
        h += t.kind == TrackKind::Video ? kVideoH : t.kind == TrackKind::Audio ? kAudioH : kSubtitleH;
    return h + 30;
}

int TimelineView::xForTime(double s) const { return kHeaderW + static_cast<int>(std::lround((s - scrollT_) * pps_)); }

double TimelineView::timeForX(int x) const { return scrollT_ + (x - kHeaderW) / pps_; }

void TimelineView::setRenderedRanges(std::vector<std::pair<int64_t, int64_t>> ranges) {
    if (ranges == rendered_) return;
    rendered_ = std::move(ranges);
    update(QRect(0, 0, width(), kRulerH));
}

Time TimelineView::quantize(double seconds) const {
    const FrameRate rate = doc_->sequence().frameRate;
    const int64_t f = std::max<int64_t>(0, std::llround(seconds * rate.toDouble()));
    return frameToTime(f, rate);
}

Time TimelineView::frameTimeAt(int x) const { return quantize(timeForX(x)); }

std::vector<TimelineView::Row> TimelineView::rows() const {
    std::vector<Row> out;
    const Sequence& s = doc_->sequence();
    int y = kRulerH - scrollY_;
    for (auto it = s.tracks.rbegin(); it != s.tracks.rend(); ++it)   // subtitle tracks on top (drawn above video)
        if (it->kind == TrackKind::Subtitle) {
            out.push_back({&*it, y, kSubtitleH});
            y += kSubtitleH;
        }
    for (auto it = s.tracks.rbegin(); it != s.tracks.rend(); ++it)
        if (it->kind == TrackKind::Video) {
            out.push_back({&*it, y, kVideoH});
            y += kVideoH;
        }
    y += kSectionGap;
    for (const auto& t : s.tracks)
        if (t.kind == TrackKind::Audio) {
            out.push_back({&t, y, kAudioH});
            y += kAudioH;
        }
    return out;
}

const TimelineView::Row* TimelineView::rowAt(int y) const {
    static thread_local std::vector<Row> cache;
    cache = rows();
    for (const auto& r : cache)
        if (y >= r.y && y < r.y + r.h) return &r;
    return nullptr;
}

QRect TimelineView::trackLaneRect(const std::string& trackId) const {
    for (const auto& r : rows())
        if (r.track->id == trackId) return QRect(kHeaderW, r.y, width() - kHeaderW, r.h);
    return {};
}

QRect TimelineView::clipRect(const std::string& clipId) const {
    for (const auto& r : rows())
        for (const auto& c : r.track->clips)
            if (c.id == clipId) {
                const int x0 = xForTime(c.timelineStart.toDouble()), x1 = xForTime(c.timelineEnd().toDouble());
                return QRect(x0, r.y + 3, std::max(1, x1 - x0), r.h - 6);
            }
    return {};
}

QRect TimelineView::transitionRectIn(const Row& r, size_t i) const {
    if (i >= r.track->clips.size() || !r.track->clips[i].outTransition) return {};
    std::optional<TransitionSpan> span;
    try {
        span = transitionSpan(*r.track, i);
    } catch (const ModelError&) {
        return {};
    }
    if (!span) return {};
    const int x0 = xForTime(span->range.start.toDouble()), x1 = xForTime(span->range.end().toDouble());
    return QRect(x0, r.y + 20, std::max(10, x1 - x0), r.h - 26);
}

QRect TimelineView::transitionRect(const std::string& clipId) const {
    for (const auto& r : rows())
        for (size_t i = 0; i < r.track->clips.size(); ++i)
            if (r.track->clips[i].id == clipId) return transitionRectIn(r, i);
    return {};
}

std::string TimelineView::transitionAt(const QPoint& p) const {
    if (p.x() < kHeaderW || p.y() < kRulerH) return {};
    const Row* r = rowAt(p.y());
    if (!r) return {};
    const Row row = *r;
    for (size_t i = 0; i < row.track->clips.size(); ++i)
        if (transitionRectIn(row, i).adjusted(-2, -2, 2, 2).contains(p)) return row.track->clips[i].id;
    return {};
}

QPoint TimelineView::keyframePos(const std::string& clipId, const Time& sourceTime) const {
    const Clip* c = edit::findClip(doc_->sequence(), clipId);
    const QRect r = clipRect(clipId);
    if (!c || r.isEmpty()) return {};
    return QPoint(xForTime(keys::toTimeline(*c, sourceTime).toDouble()), r.bottom() - 7);
}

std::optional<TimelineView::KeyHit> TimelineView::keyAt(const QPoint& p) const {
    if (p.x() < kHeaderW || p.y() < kRulerH) return std::nullopt;
    const Row* r = rowAt(p.y());
    if (!r) return std::nullopt;
    const Row row = *r;
    std::optional<KeyHit> best;
    int bestDx = 7;
    for (const auto& c : row.track->clips) {
        const int x0 = xForTime(c.timelineStart.toDouble()), x1 = xForTime(c.timelineEnd().toDouble());
        if (p.x() < x0 - 6 || p.x() > x1 + 6) continue;
        const int y = row.y + row.h - 4 - 7;   // == clipRect().bottom() - 7
        if (std::abs(p.y() - y) > 7) continue;
        for (const Time& t : keys::keyTimes(c)) {
            const Time tl = keys::toTimeline(c, t);
            if (tl < c.timelineStart || tl >= c.timelineEnd()) continue;
            const int dx = std::abs(p.x() - xForTime(tl.toDouble()));
            if (dx < bestDx) {
                bestDx = dx;
                best = KeyHit{c.id, t};
            }
        }
    }
    return best;
}

TimelineView::Hit TimelineView::hitTest(const QPoint& p) const {
    Hit h;
    if (p.x() < kHeaderW || p.y() < kRulerH) return h;
    const Row* r = rowAt(p.y());
    if (!r) return h;
    h.trackId = r->track->id;
    for (const auto& c : r->track->clips) {
        const int x0 = xForTime(c.timelineStart.toDouble()), x1 = xForTime(c.timelineEnd().toDouble());
        if (p.x() < x0 - 2 || p.x() > x1 + 2) continue;
        h.clipId = c.id;
        const int edge = std::min(kEdgePx, std::max(2, (x1 - x0) / 4));
        if (p.x() <= x0 + edge) h.part = Part::Head;
        else if (p.x() >= x1 - edge) h.part = Part::Tail;
        else h.part = Part::Body;
        return h;
    }
    return h;
}

std::vector<Time> TimelineView::snapPoints(const std::vector<std::string>& exclude) const {
    std::vector<Time> pts{Time(0), doc_->playheadTime()};
    for (const auto& t : doc_->sequence().tracks)
        for (const auto& c : t.clips) {
            if (std::find(exclude.begin(), exclude.end(), c.id) != exclude.end()) continue;
            pts.push_back(c.timelineStart);
            pts.push_back(c.timelineEnd());
        }
    return pts;
}

Time TimelineView::snap(Time t, const std::vector<Time>& points, bool* snapped) const {
    if (snapped) *snapped = false;
    if (!snapping_) return t;
    const double limit = kSnapPx / pps_;
    double best = limit;
    Time out = t;
    for (const auto& p : points) {
        const double d = std::fabs((p - t).toDouble());
        if (d <= best) {
            best = d;
            out = p;
            if (snapped) *snapped = true;
        }
    }
    return out;
}

void TimelineView::setPixelsPerSecond(double pps, int anchorX) {
    const double maxPps = doc_->sequence().frameRate.toDouble() * 60.0;   // 60 px per frame
    pps = std::clamp(pps, 0.05, maxPps);
    if (anchorX < kHeaderW) anchorX = xForTime(doc_->playheadTime().toDouble());
    const double anchorT = timeForX(anchorX);
    pps_ = pps;
    scrollT_ = std::max(0.0, anchorT - (anchorX - kHeaderW) / pps_);
    update();
    emit viewChanged();
}

void TimelineView::zoomBy(double factor, int anchorX) { setPixelsPerSecond(pps_ * factor, anchorX); }

void TimelineView::zoomToFit() {
    const double dur = std::max(1.0, doc_->sequence().duration().toDouble());
    pps_ = std::max(0.05, (width() - kHeaderW - 30) / (dur * 1.03));
    scrollT_ = 0;
    update();
    emit viewChanged();
}

void TimelineView::setScrollSeconds(double s) {
    scrollT_ = std::max(0.0, s);
    update();
    emit viewChanged();
}

void TimelineView::setScrollY(int y) {
    scrollY_ = std::clamp(y, 0, std::max(0, contentHeight() - height()));
    update();
    emit viewChanged();
}

QString TimelineView::sourcePath(const Clip& c) const {
    const MediaSource* s = doc_->project().findSource(c.sourceId);
    return s ? doc_->mediaFilePath(*s) : QString();   // relative paths: against the project file, not the cwd
}

bool TimelineView::sourceMissing(const Clip& c) const {
    if (c.isGenerated()) return false;   // title / adjustment layer: no media
    if (painting_)
        if (auto hit = missingInPaint_.find(c.sourceId); hit != missingInPaint_.end()) return hit->second;
    const QString p = sourcePath(c);
    bool missing = true;
    if (!p.isEmpty()) {
        if (!existsClock_.isValid()) existsClock_.start();
        const qint64 now = existsClock_.elapsed();
        auto it = existsCache_.find(p);
        if (it == existsCache_.end() || now - it->checkedMs > 2000) it = existsCache_.insert(p, {QFileInfo::exists(p), now});
        missing = !it->exists;
    }
    if (painting_) missingInPaint_[c.sourceId] = missing;
    return missing;
}

// ---- painting --------------------------------------------------------------------------------

void TimelineView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), color::window);
    missingInPaint_.clear();
    painting_ = true;
    const auto rs = rows();
    const Sequence& seq = doc_->sequence();

    for (size_t i = 0; i < rs.size(); ++i) {
        const Row& r = rs[i];
        if (r.y + r.h < kRulerH || r.y > height()) continue;
        p.fillRect(QRect(kHeaderW, r.y, width() - kHeaderW, r.h), (i % 2) ? color::trackAlt : color::track);
        p.setPen(color::border);
        p.drawLine(kHeaderW, r.y + r.h - 1, width(), r.y + r.h - 1);
        p.save();
        p.setClipRect(QRect(kHeaderW, kRulerH, width() - kHeaderW, height() - kRulerH));
        for (const auto& c : r.track->clips) {
            const int x0 = xForTime(c.timelineStart.toDouble()), x1 = xForTime(c.timelineEnd().toDouble());
            if (x1 < kHeaderW || x0 > width()) continue;
            if (mode_ == Mode::Move && c.id == pressHit_.clipId) {
                // original position dimmed; ghost drawn later
                p.setOpacity(0.35);
                paintClip(p, r, c, QRect(x0, r.y + 3, std::max(2, x1 - x0), r.h - 6));
                p.setOpacity(1.0);
                continue;
            }
            if (mode_ == Mode::Trim && c.id == pressHit_.clipId) continue;
            paintClip(p, r, c, QRect(x0, r.y + 3, std::max(2, x1 - x0), r.h - 6));
        }
        paintTransitions(p, r);
        if (r.track->kind == TrackKind::Audio)   // M6 volume envelope and fades, above the crossfades
            for (const auto& c : r.track->clips) {
                const int x0 = xForTime(c.timelineStart.toDouble()), x1 = xForTime(c.timelineEnd().toDouble());
                if (x1 < kHeaderW || x0 > width() || ((mode_ == Mode::Move || mode_ == Mode::Trim) && c.id == pressHit_.clipId))
                    continue;
                paintAudioOverlay(p, c, QRect(x0, r.y + 3, std::max(2, x1 - x0), r.h - 6));
            }
        for (const auto& c : r.track->clips) {   // keyframe markers above the transitions
            const int x0 = xForTime(c.timelineStart.toDouble()), x1 = xForTime(c.timelineEnd().toDouble());
            if (x1 < kHeaderW || x0 > width() || (mode_ == Mode::Move && c.id == pressHit_.clipId)) continue;
            paintKeyframes(p, c, QRect(x0, r.y + 3, std::max(2, x1 - x0), r.h - 6));
        }
        p.restore();
        paintHeader(p, r);
    }
    paintGapHighlight(p);   // M10

    // gesture ghosts
    p.save();
    p.setClipRect(QRect(kHeaderW, kRulerH, width() - kHeaderW, height() - kRulerH));
    if ((mode_ == Mode::Move || mode_ == Mode::Trim) && !pressHit_.clipId.empty()) {
        const Clip* c = edit::findClip(seq, pressHit_.clipId);
        for (const auto& r : rs)
            if (c && r.track->id == ghostTrack_) {
                Clip g = *c;
                if (mode_ == Mode::Trim) {
                    if (trimEdge_ == ClipEdge::Head) g.sourceIn = sourceInAfterHeadTrim(*c, ghostStart_ - c->timelineStart);
                    g.timelineStart = ghostStart_;
                    g.duration = ghostEnd_ - ghostStart_;
                } else {
                    g.timelineStart = ghostStart_;
                }
                const int x0 = xForTime(ghostStart_.toDouble()), x1 = xForTime(ghostEnd_.toDouble());
                paintClip(p, r, g, QRect(x0, r.y + 3, std::max(2, x1 - x0), r.h - 6), true);
            }
    }
    if (dropActive_ && !dropIsTransition_) {   // where the dragged item will land (M10: its real track)
        const int x0 = xForTime(dropTime_.toDouble()), x1 = xForTime((dropTime_ + dropDuration_).toDouble());
        for (const auto& r : rs)
            if (!dropGhostTrack_.empty() && r.track->id == dropGhostTrack_) {
                p.setPen(QPen(color::accent, 2, Qt::DashLine));
                p.setBrush(QColor(242, 169, 59, 50));
                p.drawRoundedRect(QRect(x0, r.y + 3, std::max(2, x1 - x0), r.h - 6), 4, 4);
            }
        if (dropNewTrack_) {   // a new video track on top: a band along the top of the highest video track
            const auto top = std::find_if(rs.begin(), rs.end(), [](const Row& r) { return r.track->kind == TrackKind::Video; });
            const int y = top != rs.end() ? top->y : kRulerH;
            const QRect band(x0, y + 2, std::max(2, x1 - x0), 24);
            p.setPen(QPen(color::accent, 2, Qt::DashLine));
            p.setBrush(QColor(242, 169, 59, 90));
            p.drawRoundedRect(band, 4, 4);
            p.setPen(QColor(0x1A, 0x14, 0x08));
            p.drawText(band.adjusted(8, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText("+ New track above", Qt::ElideRight, std::max(0, band.width() - 12)));
        }
    }
    if (dropActive_ && !dropEffectClip_.empty()) {
        p.setPen(QPen(color::accentBright, 3));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(clipRect(dropEffectClip_).adjusted(1, 1, -1, -1), 5, 5);
    }
    if (dropActive_ && dropIsTransition_ && !dropCutClip_.empty()) {
        const QRect cr = clipRect(dropCutClip_);
        p.setPen(QPen(color::transition, 3));
        p.drawLine(cr.right(), cr.top() - 2, cr.right(), cr.bottom() + 2);
    }
    if (snapLine_) {
        p.setPen(QPen(color::accentBright, 1, Qt::DashLine));
        const int x = xForTime(snapLine_->toDouble());
        p.drawLine(x, kRulerH, x, height());
    }
    p.restore();

    // header column background below the last row
    const int lastY = rs.empty() ? kRulerH : rs.back().y + rs.back().h;
    if (lastY < height()) p.fillRect(QRect(0, lastY, kHeaderW, height() - lastY), color::panel);
    if (rs.empty() || std::all_of(seq.tracks.begin(), seq.tracks.end(), [](const Track& t) { return t.clips.empty(); })) {
        p.setPen(color::textDim);
        p.drawText(QRect(kHeaderW, kRulerH, width() - kHeaderW, height() - kRulerH), Qt::AlignCenter,
                   "Drag and drop media and transitions here to create your video.");
    }

    paintRuler(p);

    // playhead
    const int px = xForTime(doc_->playheadTime().toDouble());
    if (px >= kHeaderW && px <= width()) {
        p.setPen(QPen(color::playhead, 1.5));
        p.drawLine(px, kRulerH - 6, px, height());
        QPainterPath handle;
        handle.moveTo(px - 6, 2);
        handle.lineTo(px + 6, 2);
        handle.lineTo(px + 6, kRulerH - 12);
        handle.lineTo(px, kRulerH - 5);
        handle.lineTo(px - 6, kRulerH - 12);
        handle.closeSubpath();
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(color::playhead);
        p.drawPath(handle);
        p.setRenderHint(QPainter::Antialiasing, false);
    }
    paintGapButton(p);   // M10: above everything (its label may reach over the row above)
    painting_ = false;
}

void TimelineView::paintRuler(QPainter& p) {
    const Sequence& seq = doc_->sequence();
    p.fillRect(QRect(0, 0, width(), kRulerH), color::panel);
    p.setPen(color::border);
    p.drawLine(0, kRulerH - 1, width(), kRulerH - 1);
    // corner: playhead timecode
    p.setPen(color::accent);
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.95);
    p.setFont(f);
    p.drawText(QRect(8, 0, kHeaderW - 40, kRulerH), Qt::AlignVCenter | Qt::AlignLeft,
               QString::fromStdString(formatTimecode(doc_->playhead(), seq.frameRate)));
    icon("plus", color::text).paint(&p, addTrackButtonRect());   // add track (menu)
    p.setClipRect(QRect(kHeaderW, 0, width() - kHeaderW, kRulerH));

    const double fd = seq.frameRate.frameDuration().toDouble();
    const double candidates[] = {fd, 2 * fd, 5 * fd, 10 * fd, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
    double step = candidates[std::size(candidates) - 1];
    for (double c : candidates)
        if (c * pps_ >= 90) {
            step = c;
            break;
        }
    const bool frames = step < 1.0;
    const int minor = step < 1.0 ? 5 : (step >= 60 ? 6 : 5);
    const double t0 = std::floor(scrollT_ / step) * step;
    const double t1 = scrollT_ + visibleSeconds();
    p.setPen(color::textDim);
    for (double t = t0; t <= t1 + step; t += step) {
        const int x = xForTime(t);
        p.drawLine(x, kRulerH - 12, x, kRulerH - 1);
        p.drawText(x + 4, 4, 120, kRulerH - 14, Qt::AlignLeft | Qt::AlignTop, tcLabel(t, seq.frameRate, frames));
        for (int i = 1; i < minor; ++i) {
            const int xm = xForTime(t + step * i / minor);
            p.drawLine(xm, kRulerH - 5, xm, kRulerH - 1);
        }
    }
    // In / Out marks (M6): the range between them shaded, brackets at the marks
    if (seq.markIn || seq.markOut) {
        const int xi = seq.markIn ? xForTime(seq.markIn->toDouble()) : kHeaderW;
        const int xo = seq.markOut ? xForTime(seq.markOut->toDouble()) : xForTime(seq.duration().toDouble());
        QColor band = color::accent;
        band.setAlpha(60);
        p.fillRect(QRect(xi, 0, std::max(1, xo - xi), kRulerH - 6), band);
        p.setPen(QPen(color::accent, 2));
        if (seq.markIn) {
            p.drawLine(xi, 2, xi, kRulerH - 6);
            p.drawLine(xi, 2, xi + 6, 2);
        }
        if (seq.markOut) {
            p.drawLine(xo, 2, xo, kRulerH - 6);
            p.drawLine(xo, 2, xo - 6, 2);
        }
    }
    // Render Preview: rendered frames (green bar along the bottom of the ruler)
    for (const auto& [from, to] : rendered_) {
        const int x0 = xForTime(frameToTime(from, seq.frameRate).toDouble()), x1 = xForTime(frameToTime(to, seq.frameRate).toDouble());
        if (x1 < kHeaderW || x0 > width()) continue;
        p.fillRect(QRect(x0, kRulerH - 5, std::max(1, x1 - x0), 4), QColor(76, 199, 110));
    }
    p.setClipping(false);
}

void TimelineView::paintHeader(QPainter& p, const Row& r) {
    const QRect hr(0, r.y, kHeaderW, r.h);
    if (r.y + r.h < kRulerH) return;
    p.save();
    p.setClipRect(QRect(0, kRulerH, kHeaderW, height() - kRulerH));
    p.fillRect(hr, color::panel);
    p.setPen(color::border);
    p.drawLine(0, r.y + r.h - 1, kHeaderW, r.y + r.h - 1);
    p.drawLine(kHeaderW - 1, r.y, kHeaderW - 1, r.y + r.h);
    const bool compact = r.track->kind == TrackKind::Subtitle;
    p.setPen(color::text);
    const QRect nameR = compact ? QRect(12, r.y, kHeaderW - 76, r.h) : QRect(12, r.y + 6, kHeaderW - 20, 18);
    p.drawText(nameR, Qt::AlignLeft | Qt::AlignVCenter,
               p.fontMetrics().elidedText(QString::fromUtf8(r.track->name.empty() ? r.track->id : r.track->name),
                                          Qt::ElideRight, nameR.width()));
    // icons: lock, then eye (video) / speaker (audio)
    const bool video = r.track->kind != TrackKind::Audio;   // video and subtitle tracks: visibility toggle
    const QColor on = color::text, off = color::playhead;
    QRect lockR, visR;
    headerButtons(r.y, r.h, compact, lockR, visR);
    icon(r.track->locked ? "lock" : "unlock", r.track->locked ? color::accent : color::textDim).paint(&p, lockR);
    if (video) icon(r.track->hidden ? "eyeOff" : "eye", r.track->hidden ? off : on).paint(&p, visR);
    else icon(r.track->muted ? "mute" : "volume", r.track->muted ? off : on).paint(&p, visR);
    if (!video) {   // M6 solo
        const QRect sr = soloButton(r.y, r.h);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(r.track->solo ? color::accent : color::textDim, 1));
        p.setBrush(r.track->solo ? QBrush(color::accent) : QBrush(Qt::NoBrush));
        p.drawRoundedRect(QRectF(sr).adjusted(2.5, 2.5, -2.5, -2.5), 3, 3);
        QFont sf = p.font();
        sf.setBold(true);
        sf.setPointSizeF(sf.pointSizeF() * 0.8);
        p.setFont(sf);
        p.setPen(r.track->solo ? QColor(20, 20, 24) : color::textDim);
        p.drawText(sr, Qt::AlignCenter, "S");
    }
    p.restore();
}

void TimelineView::paintClip(QPainter& p, const Row& r, const Clip& c, const QRect& rect, bool ghost) {
    const bool video = r.track->kind == TrackKind::Video;
    const bool selected = std::find(doc_->selection().begin(), doc_->selection().end(), c.id) != doc_->selection().end();
    bool partnerSelected = false;
    if (!selected && !c.linkId.empty())
        for (const auto& id : doc_->selection())
            if (const Clip* s = edit::findClip(doc_->sequence(), id); s && s->linkId == c.linkId) partnerSelected = true;
    const bool missing = sourceMissing(c);
    QColor base = video ? color::videoClip : color::audioClip;
    QColor dark = video ? color::videoClipDark : color::audioClipDark;
    const bool caption = r.track->kind == TrackKind::Subtitle;
    if (c.isTitle()) {   // titles: violet; captions: teal
        base = caption ? QColor(38, 140, 140) : QColor(122, 82, 214);
        dark = caption ? QColor(22, 78, 80) : QColor(58, 40, 104);
    }
    if (c.isAdjustment()) {   // adjustment layers: slate green
        base = QColor(64, 128, 112);
        dark = QColor(34, 70, 62);
    }
    if (c.isSticker()) {   // M9: stickers rose; a template's media slot grey
        const bool slot = c.sticker->id == "placeholder";
        base = slot ? QColor(104, 110, 122) : QColor(190, 82, 146);
        dark = slot ? QColor(58, 62, 70) : QColor(96, 40, 74);
    }
    if (c.colorMark > 0 && c.colorMark <= kColorMarkCount) {   // M9: the clip takes its colour mark
        base = color::colorMarks[c.colorMark - 1];
        dark = base.darker(185);
    }
    if (missing) {
        base = QColor(150, 40, 48);
        dark = QColor(90, 20, 26);
    }
    if (!c.enabled) {
        base = base.darker(180);
        dark = dark.darker(160);
    }
    if (rect.width() < 6 && !ghost) {
        // zoomed far out (thousands of clips in view): a sliver in the clip's colour, no rounded clip path, filmstrip or
        // label (a 4300-clip timeline repainted in 120-180 ms before, which stalled playback)
        p.fillRect(rect, selected ? color::accent : partnerSelected ? color::accent.darker(150) : base);
        return;
    }
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath shape;
    shape.addRoundedRect(rect, 5, 5);
    p.setClipPath(shape, Qt::IntersectClip);
    p.fillRect(rect, dark);
    const int labelH = 16;
    const QRect content(rect.x(), rect.y() + labelH, rect.width(), rect.height() - labelH);
    const QString path = sourcePath(c);

    if (c.isAdjustment()) {   // what the layer does: its effects
        QStringList names;
        for (const auto& e : c.effects) {
            const EffectDef* d = findEffectDef(e.type);
            if (!d) continue;
            QString name = QString::fromStdString(d->label);
            if (const auto lut = e.strings.find("lut"); e.type == "fa.filter.look" && lut != e.strings.end())   // M10: its name
                for (const auto& look : builtinLooks())
                    if (lut->second == std::string(kBuiltinLutPrefix) + look.id) name = QString::fromStdString(look.name);
            names << name + (e.enabled ? "" : " (off)");
        }
        const QString text = names.isEmpty() ? QString("no effects yet - add them in the Effects tab") : names.join(", ");
        p.setPen(QColor(226, 240, 236));
        const int tx = std::max(content.x() + 8, kHeaderW + 6);
        const char* layerIcon = "adjustment";   // M10: an effect / filter layer shows its page's icon
        if (c.effects.size() == 1)
            if (const EffectDef* d = findEffectDef(c.effects.front().type))
                layerIcon = d->section == EffectSection::Filters || c.effects.front().type == "fa.filter.look" ? "filters"
                            : d->section == EffectSection::Effects                                          ? "effects"
                                                                                                           : "adjustment";
        icon(layerIcon, QColor(226, 240, 236)).paint(&p, QRect(tx, content.center().y() - 9, 18, 18));
        const QRect textRect(tx + 24, content.y(), std::max(0, rect.right() - tx - 30), content.height());
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(text, Qt::ElideRight, textRect.width()));
    } else if (c.isTitle()) {   // title text preview instead of a filmstrip
        const QString text = QString::fromUtf8(c.title->text).replace(QLatin1Char('\n'), QStringLiteral(" / "));
        QFont tf = font();
        tf.setPointSizeF(tf.pointSizeF() * (caption ? 0.9 : 1.05));
        tf.setBold(!caption);
        p.setFont(tf);
        p.setPen(QColor(236, 232, 250));
        const int tx = std::max(content.x() + 8, kHeaderW + 6);
        const QRect tr(tx, caption ? rect.y() : content.y(), std::max(0, rect.right() - tx - 6), caption ? rect.height() : content.height());
        if (!caption) icon("title", QColor(236, 232, 250)).paint(&p, QRect(tx, content.center().y() - 9, 18, 18));
        const QRect textRect = caption ? tr : tr.adjusted(24, 0, 0, 0);
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(text, Qt::ElideRight, textRect.width()));
        // animation spans under the label strip: In (from the start), Loop (dotted), Out (to the end)
        const TitleData& td = *c.title;
        if (td.in.active() || td.loop.active() || td.out.active()) {
            const TitleAnimWindows win = titleAnimWindows(td, c.duration);
            const int y = caption ? rect.y() + 2 : content.y() + 1;
            const double x0 = rect.x(), k = rect.width() / std::max(1e-9, c.duration.toDouble());
            const int xIn = int(x0 + win.inEnd.toDouble() * k), xOut = int(x0 + win.outStart.toDouble() * k);
            const QColor mark(255, 214, 120, 210);
            if (td.in.active() && xIn > rect.x()) p.fillRect(QRect(rect.x(), y, xIn - rect.x(), 4), mark);
            if (td.out.active() && xOut < rect.right()) p.fillRect(QRect(xOut, y, rect.right() - xOut + 1, 4), mark);
            if (td.loop.active() && xOut > xIn) {
                p.setPen(QPen(mark, 2, Qt::DotLine));
                p.drawLine(xIn, y + 2, xOut, y + 2);
            }
        }
    } else if (c.isSticker()) {   // M9: the sticker's own picture at the start of the clip (the name is in the label)
        static std::map<std::string, QImage> thumbs;
        auto it = thumbs.find(c.sticker->id);
        if (it == thumbs.end()) {
            const text::StickerInfo* info = text::findSticker(c.sticker->id);
            const bool full = info && info->fullFrame;
            it = thumbs.emplace(c.sticker->id, text::stickerImage(StickerData{c.sticker->id, 0, {}}, 1.2, 5.0, full ? 128 : 96, full ? 72 : 96)).first;
        }
        const QImage& img = it->second;
        const int th = std::max(8, content.height() - 4);
        const int tw = std::max(8, th * img.width() / std::max(1, img.height()));
        const int x0 = std::max(content.x() + 4, kHeaderW + 2);
        p.drawImage(QRect(x0, content.y() + 2, tw, th), img);
    } else if (video && !missing) {   // filmstrip
        const int tileH = content.height();
        const int tileW = std::max(16, tileH * 16 / 9);
        const double tileSecs = tileW / pps_;
        const double bucket = std::max(0.25, std::pow(2.0, std::ceil(std::log2(std::max(0.25, tileSecs)))));
        const int startX = std::max(rect.x(), kHeaderW - tileW);
        for (int x = startX - ((startX - rect.x()) % tileW); x < std::min(rect.right(), width()); x += tileW) {
            const double tl = timeForX(x);
            const double src = c.speed == ClipSpeed{} ? c.sourceIn.toDouble() + (tl - c.timelineStart.toDouble())
                                                      : c.sourceTimeAt(Time::fromDouble(tl, 1000)).toDouble();
            const QImage th = media_->thumbnail(path, std::max(0.0, src), bucket);
            const QRect tr(x, content.y(), tileW, tileH);
            if (!th.isNull()) p.drawImage(tr, th);
            else p.fillRect(tr.adjusted(1, 1, -1, -1), base.darker(130));
        }
    } else if (!video) {   // waveform
        p.fillRect(content, base.darker(140));
        const std::vector<float>* peaks = missing ? nullptr : media_->waveform(path);
        if (peaks && !peaks->empty()) {
            const bool automated = c.volumeDb.isAnimated();   // M6: the waveform follows the volume envelope
            const double constGain = std::pow(10.0, (c.volumeDb.constantValue() + r.track->volumeDb) / 20.0);
            const int mid = content.center().y();
            const int half = content.height() / 2 - 2;
            p.setPen(color::waveform);
            const int xs = std::max(content.left(), kHeaderW), xe = std::min(content.right(), width());
            QVector<QLine> lines;   // one drawLines call: per-column drawLine dominated the timeline repaint
            lines.reserve(std::max(0, xe - xs + 1));
            const bool plain = c.speed == ClipSpeed{};
            auto srcAt = [&](double tl) {   // source seconds at timeline seconds (M5 speed map)
                return plain ? c.sourceIn.toDouble() + (tl - c.timelineStart.toDouble())
                             : c.sourceTimeAt(Time::fromDouble(tl, 1000)).toDouble();
            };
            for (int x = xs; x <= xe; ++x) {
                if (c.speed.freeze) break;   // a freeze frame is silent
                double s0 = srcAt(timeForX(x)), s1 = srcAt(timeForX(x + 1));
                if (s1 < s0) std::swap(s0, s1);   // reversed
                const int i0 = std::max(0, static_cast<int>(s0 * MediaService::kPeaksPerSecond));
                const int i1 = std::max(i0 + 1, static_cast<int>(s1 * MediaService::kPeaksPerSecond));
                float pk = 0;
                for (int i = i0; i < i1 && i < static_cast<int>(peaks->size()); ++i) pk = std::max(pk, (*peaks)[i]);
                const double gain = automated ? std::pow(10.0, (c.volumeDb.value((s0 + s1) / 2) + r.track->volumeDb) / 20.0)
                                              : constGain;
                // dB scale (-48 dBFS .. 0 dBFS) like most editors: quiet material stays visible
                const double db = 20.0 * std::log10(std::max(1e-6, pk * gain));
                const int a = static_cast<int>(std::clamp(1.0 + db / 48.0, 0.0, 1.0) * half);
                if (a > 0) lines.append(QLine(x, mid - a, x, mid + a));
            }
            p.drawLines(lines);
        }
    }
    // label strip
    if (caption) {
        p.setClipping(false);
        if (ghost) p.setPen(QPen(color::accentBright, 2, Qt::DashLine));
        else if (selected) p.setPen(QPen(color::accent, 2));
        else p.setPen(QPen(base.lighter(130), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
        p.restore();
        return;
    }
    p.fillRect(QRect(rect.x(), rect.y(), rect.width(), labelH), base);
    p.setPen(QColor(245, 247, 250));
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    p.setFont(f);
    QString name = c.name.empty() ? QFileInfo(path).fileName() : QString::fromUtf8(c.name);
    if (missing) name = "Media offline - " + name;
    if (!c.enabled) name += "  (disabled)";
    if (!c.linkId.empty()) name = QString::fromUtf8("\xE2\x9B\x93 ") + name;   // chain mark for linked clips
    const int textX = std::max(rect.x() + 6, kHeaderW + 4);
    int textRight = rect.right() - 4;
    // M5 badges on the right of the label strip: speed, tracking, stabilisation (right to left, while there is room)
    QStringList badges;
    if (isRetimed(c.speed)) {
        const ClipSpeed& sp = c.speed;
        if (sp.freeze) badges << "Freeze";
        else if (sp.ramp.size() >= 2) {
            const RampPreset* pre = findRampPreset(sp.rampPreset);
            badges << (pre ? "Ramp: " + QString::fromStdString(pre->name) : QString("Ramp"));
        } else {
            badges << (sp.reverse ? "Reverse " : "") + QString::number(sp.speed.toDouble(), 'f', 2) + "x";
        }
    }
    if (c.muted) badges << "Muted";   // M9
    if (c.tracking) badges << "Tracked";
    if (c.stabilization) badges << "Stabilized";
    for (const QString& badge : badges) {
        const int bw = p.fontMetrics().horizontalAdvance(badge) + 10;
        const QRect br(textRight - bw + 1, rect.y() + 2, bw, labelH - 4);
        if (br.left() <= textX + 24) break;
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 110));
        p.drawRoundedRect(br, 4, 4);
        p.setPen(QColor(255, 214, 120));
        p.drawText(br, Qt::AlignCenter, badge);
        p.restore();
        textRight = br.left() - 4;
    }
    p.drawText(QRect(textX, rect.y(), std::max(0, textRight - textX), labelH), Qt::AlignVCenter | Qt::AlignLeft,
               p.fontMetrics().elidedText(name, Qt::ElideRight, std::max(0, textRight - textX)));
    p.setClipping(false);
    // border
    if (ghost) p.setPen(QPen(color::accentBright, 2, Qt::DashLine));
    else if (selected) p.setPen(QPen(color::accent, 2));
    else if (partnerSelected) p.setPen(QPen(color::accent.darker(150), 1.5));
    else p.setPen(QPen(base.lighter(130), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
    p.restore();
}

void TimelineView::paintTransitions(QPainter& p, const Row& r) {
    for (size_t i = 0; i < r.track->clips.size(); ++i) {
        const Clip& c = r.track->clips[i];
        if (!c.outTransition) continue;
        const QRect tr = transitionRectIn(r, i);
        if (tr.isEmpty() || tr.right() < kHeaderW || tr.left() > width()) continue;
        const bool selected = doc_->selectedTransition() == c.id;
        const double spanW = c.outTransition->duration.toDouble() * pps_;
        if (spanW < 6) {   // zoomed far out: a plain marker (see paintClip)
            p.fillRect(QRect(tr.x(), tr.y(), 3, tr.height()), selected ? color::accent : color::transition);
            continue;
        }
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        QColor fill = color::transition;
        fill.setAlpha(selected ? 170 : 110);
        p.setBrush(fill);
        p.setPen(selected ? QPen(color::accent, 2) : QPen(color::transition, 1.5));
        p.drawRoundedRect(tr, 4, 4);
        const int s = std::min(20, tr.height() - 4);
        const QString label = transitionLabel(c.outTransition->type);
        if (!label.isEmpty() && tr.width() > 70) {   // icon + name
            p.setPen(QColor(255, 255, 255));
            QFont f = font();
            f.setPointSizeF(f.pointSizeF() * 0.8);
            p.setFont(f);
            icon("dissolve", QColor(255, 255, 255)).paint(&p, QRect(tr.x() + 4, tr.center().y() - s / 2, s, s));
            p.drawText(tr.adjusted(s + 8, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText(label, Qt::ElideRight, tr.width() - s - 12));
        } else {
            icon("dissolve", QColor(255, 255, 255)).paint(&p, QRect(tr.center().x() - s / 2, tr.center().y() - s / 2, s, s));
        }
        p.restore();
    }
}

void TimelineView::paintKeyframes(QPainter& p, const Clip& c, const QRect& rect) {
    if (rect.width() < 6) return;   // M9: a sliver (zoomed far out) has no room for markers
    const std::vector<Time> times = keys::keyTimes(c);
    if (times.empty()) return;
    const bool selected = std::find(doc_->selection().begin(), doc_->selection().end(), c.id) != doc_->selection().end();
    const Time now = doc_->playheadTime();
    const int y = rect.bottom() - 7;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    for (const Time& st : times) {
        Time tl = keys::toTimeline(c, st);
        if (keyDrag_ && keyDrag_->clipId == c.id && keyDrag_->sourceTime == st) tl = keyGhost_;
        if (tl < c.timelineStart || tl >= c.timelineEnd()) continue;
        const double x = xForTime(tl.toDouble());
        const double r = tl == now ? 6.0 : 5.0;
        const QPointF pts[] = {{x, y - r}, {x + r, double(y)}, {x, y + r}, {x - r, double(y)}};
        p.setPen(QPen(QColor(20, 20, 24), 1.2));
        p.setBrush(selected ? color::accent : QColor(225, 228, 235));
        p.drawPolygon(pts, 4);
    }
    p.restore();
}

// ---- M6 audio overlay: volume envelope and fades ------------------------------------------------

int TimelineView::fadeHandleX(const Clip& c, bool fadeIn) const {
    const int x0 = xForTime(c.timelineStart.toDouble()), x1 = xForTime(c.timelineEnd().toDouble());
    if (fadeIn) return std::min(x1 - 1, x0 + std::max(7, static_cast<int>(std::lround(c.audio.fadeIn.toDouble() * pps_))));
    return std::max(x0 + 1, x1 - std::max(7, static_cast<int>(std::lround(c.audio.fadeOut.toDouble() * pps_))));
}

TimelineView::AudioHit TimelineView::audioHitAt(const QPoint& p) const {
    AudioHit h;
    if (p.x() < kHeaderW || p.y() < kRulerH) return h;
    const Row* r = rowAt(p.y());
    if (!r || r->track->kind != TrackKind::Audio) return h;
    const Row row = *r;
    const auto& sel = doc_->selection();
    for (const auto& c : row.track->clips) {
        // only the selected clip's envelope and fades are handles: a click on another clip selects / moves it
        if (c.speed.freeze || std::find(sel.begin(), sel.end(), c.id) == sel.end()) continue;
        const int x0 = xForTime(c.timelineStart.toDouble()), x1 = xForTime(c.timelineEnd().toDouble());
        if (p.x() < x0 || p.x() > x1 || x1 - x0 < 16) continue;
        const QRect content = audioContent(QRect(x0, row.y + 3, x1 - x0, row.h - 6));
        if (p.y() < content.top() - 1 || p.y() > content.bottom() + 1) return h;
        h.clipId = c.id;
        const bool trimZone = p.x() <= x0 + kEdgePx || p.x() >= x1 - kEdgePx;
        // fade handles (top corners of the waveform) win over the trim zone they overlap: without a fade the square sits
        // 7 px inside the edge, so its outer half lies in the 6 px trim zone (pressing there trimmed the clip in the M6
        // real-input check); the edge still trims from the label strip and below this band
        if (std::abs(p.y() - (content.top() + 4)) <= 5) {
            if (std::abs(p.x() - fadeHandleX(c, true)) <= 5) {
                h.part = AudioPart::FadeIn;
                return h;
            }
            if (std::abs(p.x() - fadeHandleX(c, false)) <= 5) {
                h.part = AudioPart::FadeOut;
                return h;
            }
        }
        for (const auto& k : c.volumeDb.keys()) {   // envelope points
            const Time tl = keys::toTimeline(c, k.time);
            if (tl < c.timelineStart || tl > c.timelineEnd()) continue;
            if (std::abs(p.x() - xForTime(tl.toDouble())) <= 5 && std::abs(p.y() - envY(content, k.value)) <= 5) {
                h.part = AudioPart::EnvPoint;
                h.keyTime = k.time;
                return h;
            }
        }
        const double db = c.volumeDb.value(keys::toSource(c, timeAtSeconds(timeForX(p.x()))));
        if (!trimZone && std::abs(p.y() - envY(content, db)) <= 3) {
            h.part = AudioPart::EnvLine;
            return h;
        }
        h.clipId.clear();
        return h;
    }
    return h;
}

void TimelineView::paintAudioOverlay(QPainter& p, const Clip& c, const QRect& rect) {
    if (c.speed.freeze || rect.width() < 6) return;
    const QRect content = audioContent(rect);
    const bool selected = std::find(doc_->selection().begin(), doc_->selection().end(), c.id) != doc_->selection().end();
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath shape;
    shape.addRoundedRect(rect, 5, 5);
    p.setClipPath(shape, Qt::IntersectClip);
    const double x0 = rect.x(), x1 = rect.x() + rect.width();
    const double top = content.top(), bottom = content.bottom() + 1;
    // fades: shade what they take away, and a line along the gain (linear in amplitude)
    const QColor shade(0, 0, 0, 120), fadeLine(235, 238, 245, 210);
    if (c.audio.fadeIn > Time(0)) {
        const double xi = x0 + c.audio.fadeIn.toDouble() * pps_;
        const QPointF tri[] = {{x0, top}, {xi, top}, {x0, bottom}};
        p.setPen(Qt::NoPen);
        p.setBrush(shade);
        p.drawPolygon(tri, 3);
        p.setPen(QPen(fadeLine, 1.2));
        p.drawLine(QPointF(x0, bottom), QPointF(xi, top));
    }
    if (c.audio.fadeOut > Time(0)) {
        const double xo = x1 - c.audio.fadeOut.toDouble() * pps_;
        const QPointF tri[] = {{xo, top}, {x1, top}, {x1, bottom}};
        p.setPen(Qt::NoPen);
        p.setBrush(shade);
        p.drawPolygon(tri, 3);
        p.setPen(QPen(fadeLine, 1.2));
        p.drawLine(QPointF(xo, top), QPointF(x1, bottom));
    }
    // volume envelope (source-time keys through the clip's speed map)
    QColor env(255, 214, 120);
    if (!selected) env.setAlpha(170);
    const int xs = std::max(rect.left(), kHeaderW), xe = std::min(rect.right(), width());
    std::vector<QPointF> line;
    if (!c.volumeDb.isAnimated()) {
        const double y = envY(content, c.volumeDb.constantValue());
        line = {QPointF(xs, y), QPointF(xe, y)};
    } else {
        for (int x = xs; x <= xe; x += 2)
            line.emplace_back(x, envY(content, c.volumeDb.value(keys::toSource(c, timeAtSeconds(timeForX(x))))));
    }
    p.setPen(QPen(env, selected ? 1.6 : 1.2));
    p.setBrush(Qt::NoBrush);
    if (line.size() >= 2) p.drawPolyline(line.data(), static_cast<int>(line.size()));
    for (const auto& k : c.volumeDb.keys()) {
        const Time tl = keys::toTimeline(c, k.time);
        if (tl < c.timelineStart || tl > c.timelineEnd()) continue;
        p.setPen(QPen(QColor(20, 20, 24), 1));
        p.setBrush(env);
        p.drawEllipse(QPointF(xForTime(tl.toDouble()), envY(content, k.value)), 3.5, 3.5);
    }
    // fade handles (the selected clip)
    if (selected && rect.width() >= 16)
        for (bool in : {true, false}) {
            p.setPen(QPen(QColor(20, 20, 24), 1));
            p.setBrush(QColor(235, 238, 245));
            p.drawRect(QRectF(fadeHandleX(c, in) - 3.5, top + 0.5, 7, 7));
        }
    p.restore();
}

void TimelineView::audioDragTo(QMouseEvent* e) {
    const Clip* c = edit::findClip(doc_->sequence(), audioDrag_.clipId);
    if (!c) return;
    const QPoint tip = e->globalPosition().toPoint();
    if (mode_ == Mode::FadeDrag) {
        const Time t = frameTimeAt(e->pos().x());
        Time in = c->audio.fadeIn, out = c->audio.fadeOut;
        const bool head = audioDrag_.part == AudioPart::FadeIn;
        if (head) in = std::clamp(t - c->timelineStart, Time(0), c->duration - out);
        else out = std::clamp(c->timelineEnd() - t, Time(0), c->duration - in);
        if (in != c->audio.fadeIn || out != c->audio.fadeOut)
            edit_->setClipFades(c->id, in, out, "fade#" + std::to_string(gesture_));
        QToolTip::showText(tip, QString("Fade %1: %2 s").arg(head ? "in" : "out").arg((head ? in : out).toDouble(), 0, 'f', 2), this);
        return;
    }
    const QRect rc = clipRect(c->id);
    if (rc.isEmpty()) return;
    const QRect content = audioContent(rc);
    const double db = std::round(envDb(content, e->pos().y()) * 10) / 10;   // 0.1 dB steps
    const std::string key = "env#" + std::to_string(gesture_);
    if (audioDrag_.part == AudioPart::EnvPoint) {
        const Time frame = doc_->sequence().frameRate.frameDuration();
        const Time from = audioDrag_.keyTime;
        Time tl = keys::toTimeline(*c, from);
        if (!(e->modifiers() & Qt::ShiftModifier)) {   // Shift: level only
            const Time cur = tl;
            tl = std::clamp(frameTimeAt(e->pos().x()), c->timelineStart, c->timelineEnd() - frame);
            for (const auto& k : c->volumeDb.keys()) {   // stay between the neighbouring points
                if (k.time == from) continue;
                const Time o = keys::toTimeline(*c, k.time);
                if (o < cur) tl = max(tl, o + frame);
                else tl = min(tl, o - frame);
            }
        }
        Time to = keys::toSource(*c, tl);
        if (to != from && c->volumeDb.keyAt(to)) to = from;   // another point is there
        // merged drag steps are applied to the state before the drag: move from where the point was then
        const Time origin = envOrigin_;
        edit_->modifyClip(c->id, "Volume point", [origin, to, db](Clip& x) {
            if (to != origin) keys::moveKey(x.volumeDb, origin, to);
            if (auto* k = x.volumeDb.keyAt(to)) k->value = db;
        }, key);
        audioDrag_.keyTime = to;
        QToolTip::showText(tip, QString("%1 dB").arg(db, 0, 'f', 1), this);
        return;
    }
    // the line: raise / lower the whole volume (every point by the same amount)
    if (std::abs(e->pos().y() - pressPos_.y()) < 1) return;
    const double delta = db - std::round(envDb(content, pressPos_.y()) * 10) / 10;
    const std::vector<Keyframe<double>> orig = envOrigKeys_;
    const double c0 = envOrigConstant_;
    edit_->modifyClip(c->id, "Volume", [orig, c0, delta](Clip& x) {
        if (orig.empty()) x.volumeDb.setConstant(std::clamp(c0 + delta, kEnvMinDb, kEnvMaxDb));
        for (const auto& k : orig)
            if (auto* kk = x.volumeDb.keyAt(k.time)) kk->value = std::clamp(k.value + delta, kEnvMinDb, kEnvMaxDb);
    }, key);
    QToolTip::showText(tip, orig.empty() ? QString("%1 dB").arg(std::clamp(c0 + delta, kEnvMinDb, kEnvMaxDb), 0, 'f', 1)
                                         : QString("%1%2 dB").arg(delta >= 0 ? "+" : "").arg(delta, 0, 'f', 1), this);
}

// ---- mouse -----------------------------------------------------------------------------------

void TimelineView::seekToX(int x) {
    const FrameRate rate = doc_->sequence().frameRate;
    playback_->seek(timeToFrame(frameTimeAt(x), rate));
}

void TimelineView::mousePressEvent(QMouseEvent* e) {
    pressPos_ = e->pos();
    pressHit_ = {};
    snapLine_.reset();
    if (e->button() != Qt::LeftButton) return;
    if (addTrackButtonRect().contains(e->pos())) {
        QMenu menu(this);
        menu.addAction(icon("plus"), "Add Video Track", [this] { edit_->addTrack(TrackKind::Video); });
        menu.addAction(icon("plus"), "Add Audio Track", [this] { edit_->addTrack(TrackKind::Audio); });
        menu.exec(mapToGlobal(addTrackButtonRect().bottomLeft()));
        return;
    }
    if (e->pos().y() < kRulerH && e->pos().x() >= kHeaderW) {
        mode_ = Mode::Scrub;
        seekToX(e->pos().x());
        return;
    }
    if (e->pos().x() < kHeaderW) {   // track header buttons
        if (const Row* r = rowAt(e->pos().y())) {
            QRect lockR, visR;
            headerButtons(r->y, r->h, r->track->kind == TrackKind::Subtitle, lockR, visR);
            const std::string id = r->track->id;
            if (lockR.contains(e->pos())) edit_->setTrackFlag(id, "locked", !r->track->locked);
            else if (visR.contains(e->pos())) {
                if (r->track->kind != TrackKind::Audio) edit_->setTrackFlag(id, "hidden", !r->track->hidden);
                else edit_->setTrackFlag(id, "muted", !r->track->muted);
            } else if (r->track->kind == TrackKind::Audio && soloButton(r->y, r->h).contains(e->pos())) {
                edit_->setTrackFlag(id, "solo", !r->track->solo);
            }
        }
        return;
    }
    if (gapHover_ && gapButtonRect().contains(e->pos())) {   // M10: Delete this gap
        const GapHover g = *gapHover_;
        const QString why = gapProblem_;
        clearGapHover();
        if (!why.isEmpty()) emit edit_->message(why);
        else edit_->deleteGap(g.trackId, g.range.start);
        return;
    }
    clearGapHover();   // no highlight during a gesture
    if (const AudioHit ah = audioHitAt(e->pos()); !ah.clipId.empty()) {   // M6 fades and volume envelope
        doc_->setSelection({ah.clipId});
        const Track* atr = nullptr;
        const Clip* c = edit::findClip(doc_->sequence(), ah.clipId, &atr);
        if (!c || !atr || atr->locked) {
            update();
            return;
        }
        audioDrag_ = ah;
        ++gesture_;
        if (ah.part == AudioPart::EnvLine && (e->modifiers() & Qt::ControlModifier)) {   // Ctrl+click: add a point
            const Time src = keys::toSource(*c, std::clamp(frameTimeAt(e->pos().x()), c->timelineStart, c->timelineEnd()));
            if (!c->volumeDb.keyAt(src))
                edit_->modifyClip(c->id, "Add volume point", [src](Clip& x) { keys::toggleKey(x.volumeDb, src); });
            audioDrag_.part = AudioPart::EnvPoint;
            audioDrag_.keyTime = src;
        }
        envOrigin_ = audioDrag_.keyTime;
        if (audioDrag_.part == AudioPart::EnvLine) {
            envOrigKeys_ = c->volumeDb.keys();
            envOrigConstant_ = c->volumeDb.constantValue();
        }
        mode_ = ah.part == AudioPart::FadeIn || ah.part == AudioPart::FadeOut ? Mode::FadeDrag : Mode::EnvelopeDrag;
        update();
        return;
    }
    if (const auto k = keyAt(e->pos())) {   // keyframe marker: select the clip, seek, maybe drag
        doc_->setSelection({k->clipId});
        const Track* ktr = nullptr;
        const Clip* c = edit::findClip(doc_->sequence(), k->clipId, &ktr);
        keyGhost_ = keys::toTimeline(*c, k->sourceTime);
        playback_->seek(timeToFrame(keyGhost_, doc_->sequence().frameRate));
        if (ktr && !ktr->locked) {
            keyDrag_ = k;
            mode_ = Mode::KeyDrag;
        }
        update();
        return;
    }
    if (const std::string tid = transitionAt(e->pos()); !tid.empty()) {
        doc_->setSelectedTransition(tid);
        mode_ = Mode::None;
        update();
        return;
    }
    const Hit h = hitTest(e->pos());
    if (h.clipId.empty()) {
        doc_->setSelection({});
        mode_ = Mode::None;
        return;
    }
    if (e->modifiers() & Qt::ControlModifier) {
        auto sel = doc_->selection();
        auto it = std::find(sel.begin(), sel.end(), h.clipId);
        if (it != sel.end()) sel.erase(it);
        else sel.push_back(h.clipId);
        doc_->setSelection(sel);
        return;
    }
    doc_->setSelection({h.clipId});
    const Track* tr = doc_->sequence().findTrack(h.trackId);
    if (tr && tr->locked) return;
    const Clip* c = edit::findClip(doc_->sequence(), h.clipId);
    pressHit_ = h;
    origStart_ = c->timelineStart;
    origEnd_ = c->timelineEnd();
    ghostStart_ = origStart_;
    ghostEnd_ = origEnd_;
    ghostTrack_ = h.trackId;
    std::vector<std::string> group;
    for (const auto& m : edit::linkGroup(doc_->sequence(), h.clipId)) group.push_back(m.clipId);
    dragSnaps_ = snapPoints(group);
    if (h.part == Part::Head || h.part == Part::Tail) {
        mode_ = Mode::Trim;
        trimEdge_ = h.part == Part::Head ? ClipEdge::Head : ClipEdge::Tail;
        const FrameRate rate = doc_->sequence().frameRate;
        const Time oneFrame = rate.frameDuration();
        Time prevEnd(0), nextStart(1000000);
        for (const auto& o : tr->clips) {
            if (o.id == c->id) continue;
            if (o.timelineEnd() <= c->timelineStart) prevEnd = max(prevEnd, o.timelineEnd());
            if (o.timelineStart >= c->timelineEnd()) nextStart = min(nextStart, o.timelineStart);
        }
        const MediaSource* src = doc_->project().findSource(c->sourceId);
        if (trimEdge_ == ClipEdge::Head) {
            // titles have no media to run out of: their head extends freely (source time may go negative)
            trimMin_ = c->isGenerated() ? prevEnd : max(prevEnd, max(Time(0), c->timelineStart - c->sourceIn));
            trimMax_ = c->timelineEnd() - oneFrame;
        } else {
            trimMin_ = c->timelineStart + oneFrame;
            trimMax_ = nextStart;
            if (src && src->duration) trimMax_ = min(trimMax_, c->timelineStart + (*src->duration - c->sourceIn));
        }
    } else {
        mode_ = Mode::PendingMove;
    }
    update();
}

void TimelineView::mouseMoveEvent(QMouseEvent* e) {
    if (mode_ == Mode::None) {
        const Hit h = hitTest(e->pos());
        const Track* tr = h.trackId.empty() ? nullptr : doc_->sequence().findTrack(h.trackId);
        const bool locked = tr && tr->locked;
        const AudioHit ah = locked ? AudioHit{} : audioHitAt(e->pos());
        updateGapHover(e->pos());   // M10
        if (gapButtonHot_) setCursor(gapProblem_.isEmpty() ? Qt::PointingHandCursor : Qt::ForbiddenCursor);
        else if (ah.part == AudioPart::FadeIn || ah.part == AudioPart::FadeOut) setCursor(Qt::SizeHorCursor);
        else if (ah.part == AudioPart::EnvPoint) setCursor(Qt::SizeAllCursor);
        else if (ah.part == AudioPart::EnvLine) setCursor(Qt::SizeVerCursor);
        else if (keyAt(e->pos()) || !transitionAt(e->pos()).empty()) setCursor(Qt::PointingHandCursor);
        else if (!locked && (h.part == Part::Head || h.part == Part::Tail)) setCursor(Qt::SizeHorCursor);
        else if (e->pos().y() < kRulerH && e->pos().x() >= kHeaderW) setCursor(Qt::IBeamCursor);
        else if (addTrackButtonRect().contains(e->pos())) setCursor(Qt::PointingHandCursor);
        else unsetCursor();
        return;
    }
    if (mode_ == Mode::Scrub) {
        seekToX(e->pos().x());
        return;
    }
    if (mode_ == Mode::FadeDrag || mode_ == Mode::EnvelopeDrag) {
        audioDragTo(e);
        return;
    }
    if (mode_ == Mode::KeyDrag) {
        if ((e->pos() - pressPos_).manhattanLength() < 3 || !keyDrag_) return;
        const Clip* c = edit::findClip(doc_->sequence(), keyDrag_->clipId);
        if (!c) return;
        const FrameRate rate = doc_->sequence().frameRate;
        keyGhost_ = std::clamp(frameTimeAt(e->pos().x()), c->timelineStart, c->timelineEnd() - rate.frameDuration());
        setCursor(Qt::SizeHorCursor);
        playback_->seek(timeToFrame(keyGhost_, rate));   // the monitor follows the keyframe
        update();
        return;
    }
    if (mode_ == Mode::PendingMove) {
        if ((e->pos() - pressPos_).manhattanLength() < 4) return;
        mode_ = Mode::Move;
        setCursor(Qt::ClosedHandCursor);
    }
    const double dx = (e->pos().x() - pressPos_.x()) / pps_;
    if (mode_ == Mode::Move) {
        const Time dur = origEnd_ - origStart_;
        Time start = quantize(std::max(0.0, origStart_.toDouble() + dx));
        bool s1 = false, s2 = false;
        const Time snappedStart = snap(start, dragSnaps_, &s1);
        const Time snappedEnd = snap(start + dur, dragSnaps_, &s2);
        snapLine_.reset();
        if (s1) {
            start = snappedStart;
            snapLine_ = snappedStart;
        } else if (s2 && snappedEnd - dur >= Time(0)) {
            start = snappedEnd - dur;
            snapLine_ = snappedEnd;
        }
        ghostStart_ = start;
        ghostEnd_ = start + dur;
        // track change: only to a track of the same kind
        if (const Row* r = rowAt(e->pos().y())) {
            const Track* orig = doc_->sequence().findTrack(pressHit_.trackId);
            if (orig && r->track->kind == orig->kind && !r->track->locked) ghostTrack_ = r->track->id;
        }
    } else if (mode_ == Mode::Trim) {
        const Time edge0 = trimEdge_ == ClipEdge::Head ? origStart_ : origEnd_;
        Time t = quantize(edge0.toDouble() + dx);
        bool s = false;
        const Time snapped = snap(t, dragSnaps_, &s);
        snapLine_.reset();
        if (s) {
            t = snapped;
            snapLine_ = snapped;
        }
        t = std::clamp(t, trimMin_, trimMax_);
        if (trimEdge_ == ClipEdge::Head) ghostStart_ = t;
        else ghostEnd_ = t;
    }
    update();
}

void TimelineView::mouseReleaseEvent(QMouseEvent* e) {
    const Mode m = mode_;
    mode_ = Mode::None;
    snapLine_.reset();
    unsetCursor();
    const std::optional<KeyHit> key = keyDrag_;
    keyDrag_.reset();
    if (m == Mode::FadeDrag || m == Mode::EnvelopeDrag) {
        audioDrag_ = {};
        envOrigKeys_.clear();
        QToolTip::hideText();
        update();
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    if (m == Mode::KeyDrag && key) {
        if (const Clip* c = edit::findClip(doc_->sequence(), key->clipId)) {
            const Time to = keys::toSource(*c, keyGhost_);
            if (to != key->sourceTime) edit_->moveKeyframes(key->clipId, key->sourceTime, to);
        }
    } else if (m == Mode::Move) {
        const bool trackChanged = ghostTrack_ != pressHit_.trackId;
        if (ghostStart_ != origStart_ || trackChanged)
            edit_->moveClip(pressHit_.clipId, ghostStart_, trackChanged ? std::optional<std::string>(ghostTrack_) : std::nullopt);
    } else if (m == Mode::Trim) {
        const Time edge = trimEdge_ == ClipEdge::Head ? ghostStart_ : ghostEnd_;
        const Time orig = trimEdge_ == ClipEdge::Head ? origStart_ : origEnd_;
        if (edge != orig) edit_->trimClip(pressHit_.clipId, trimEdge_, edge);
    }
    pressHit_ = {};
    update();
}

void TimelineView::mouseDoubleClickEvent(QMouseEvent* e) {
    if (const AudioHit ah = audioHitAt(e->pos()); ah.part == AudioPart::EnvPoint || ah.part == AudioPart::EnvLine) {
        const Clip* c = edit::findClip(doc_->sequence(), ah.clipId);
        if (!c) return;
        const Time t = ah.part == AudioPart::EnvPoint
                           ? ah.keyTime
                           : keys::toSource(*c, std::clamp(frameTimeAt(e->pos().x()), c->timelineStart, c->timelineEnd()));
        const bool remove = ah.part == AudioPart::EnvPoint;
        if (remove || !c->volumeDb.keyAt(t))
            edit_->modifyClip(c->id, remove ? "Delete volume point" : "Add volume point",
                              [t](Clip& x) { keys::toggleKey(x.volumeDb, t); });
        return;
    }
    const Hit h = hitTest(e->pos());
    if (!h.clipId.empty()) {   // jump to the clip start
        const Clip* c = edit::findClip(doc_->sequence(), h.clipId);
        playback_->seek(timeToFrame(c->timelineStart, doc_->sequence().frameRate));
    }
}

void TimelineView::wheelEvent(QWheelEvent* e) {
    const int dy = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
    if (e->modifiers() & Qt::ControlModifier) {
        zoomBy(dy > 0 ? 1.25 : 0.8, static_cast<int>(e->position().x()));
    } else if (e->modifiers() & Qt::ShiftModifier) {
        setScrollY(scrollY_ - dy / 2);
    } else {
        setScrollSeconds(scrollT_ - dy / 120.0 * visibleSeconds() * 0.1);
    }
    e->accept();
}

void TimelineView::keyframeMenu(QMenu& menu, const KeyHit& k) {
    const Clip* c = edit::findClip(doc_->sequence(), k.clipId);
    if (!c) return;
    Clip copy = *c;
    std::optional<keys::Style> cur;
    for (const auto& prop : keys::properties(copy))
        if (const auto st = keys::styleAt(*prop.value, k.sourceTime)) {
            cur = st;
            break;
        }
    menu.addSection("Keyframe interpolation");
    for (keys::Style st : {keys::Style::Linear, keys::Style::Hold, keys::Style::Ease, keys::Style::Smooth}) {
        QAction* a = menu.addAction(keys::styleLabel(st), [this, k, st] { edit_->setKeyframeStyle(k.clipId, k.sourceTime, st); });
        a->setCheckable(true);
        a->setChecked(cur == st);
    }
    menu.addSeparator();
    menu.addAction(icon("delete"), "Delete Keyframe", [this, k] { edit_->deleteKeyframes(k.clipId, k.sourceTime); });
}

QAction* TimelineView::namedAction(const char* name) const {
    if (!named_) return nullptr;
    const auto it = named_->find(QString::fromLatin1(name));
    return it == named_->end() ? nullptr : it->second;
}

void TimelineView::contextMenuEvent(QContextMenuEvent* e) {
    QMenu menu(this);
    if (const AudioHit ah = audioHitAt(e->pos()); ah.part == AudioPart::EnvPoint) {   // M6 volume point
        doc_->setSelection({ah.clipId});
        const Clip* c = edit::findClip(doc_->sequence(), ah.clipId);
        if (!c) return;
        const std::string id = ah.clipId;
        const Time t = ah.keyTime;
        const auto cur = keys::styleAt(c->volumeDb, t);
        menu.addSection("Volume point");
        for (keys::Style st : {keys::Style::Linear, keys::Style::Hold, keys::Style::Ease, keys::Style::Smooth}) {
            QAction* a = menu.addAction(keys::styleLabel(st), [this, id, t, st] {
                edit_->modifyClip(id, "Volume point interpolation", [t, st](Clip& x) { keys::setStyle(x.volumeDb, t, st); });
            });
            a->setCheckable(true);
            a->setChecked(cur == st);
        }
        menu.addSeparator();
        menu.addAction(icon("delete"), "Delete Point", [this, id, t] {
            edit_->modifyClip(id, "Delete volume point", [t](Clip& x) {
                if (x.volumeDb.keyAt(t)) keys::toggleKey(x.volumeDb, t);
            });
        });
        menu.exec(e->globalPos());
        return;
    }
    if (const auto k = keyAt(e->pos())) {
        doc_->setSelection({k->clipId});
        keyframeMenu(menu, *k);
        menu.exec(e->globalPos());
        return;
    }
    if (const std::string tid = transitionAt(e->pos()); !tid.empty()) {
        doc_->setSelectedTransition(tid);
        const Clip* c = edit::findClip(doc_->sequence(), tid);
        const bool crossfade = c && c->outTransition && isAudioCrossfade(c->outTransition->type);
        QMenu* types = menu.addMenu(icon("transitions"), crossfade ? "Crossfade Shape" : "Transition Type");
        std::vector<std::pair<QString, std::string>> choices;
        if (crossfade)
            for (const auto& [label, shape] : crossfadeChoices()) choices.emplace_back(label, kAudioCrossfadePrefix + shape);
        else
            for (const auto& info : transitionTypes()) choices.emplace_back(QString::fromUtf8(info.label), info.id);
        for (const auto& choice : choices) {
            const std::string type = choice.second;
            QAction* a = types->addAction(choice.first, [this, tid, type] {
                const Clip* cc = edit::findClip(doc_->sequence(), tid);
                if (!cc || !cc->outTransition) return;
                TransitionRef r = *cc->outTransition;
                r.type = type;
                edit_->updateTransitionSettings(tid, r);
            });
            a->setCheckable(true);
            a->setChecked(c && c->outTransition && c->outTransition->type == type);
        }
        menu.addAction(icon("delete"), crossfade ? "Remove Crossfade" : "Remove Transition",
                       [this, tid] { edit_->removeTransition(tid); });
        menu.exec(e->globalPos());
        return;
    }
    const Hit h = hitTest(e->pos());
    auto addNamed = [this](QMenu* m, std::initializer_list<const char*> names) {
        for (const char* n : names)
            if (QAction* a = namedAction(n)) m->addAction(a);
    };
    if (!h.clipId.empty()) {
        if (std::find(doc_->selection().begin(), doc_->selection().end(), h.clipId) == doc_->selection().end())
            doc_->setSelection({h.clipId});
        const Clip* c = edit::findClip(doc_->sequence(), h.clipId);
        const std::string id = h.clipId;
        const Track* ctr = nullptr;
        edit::findClip(doc_->sequence(), id, &ctr);
        const bool audioClip = ctr && ctr->kind == TrackKind::Audio;
        const bool videoTrack = ctr && ctr->kind == TrackKind::Video;
        const MediaSource* src = doc_->project().findSource(c->sourceId);
        const bool hasSound = audioClip || !c->linkId.empty();
        menu.setObjectName("clipMenu");
        // M9: Filmora's clip menu - clipboard, cutting, attributes, picture, speed, sound, clip, range, name, marks
        addNamed(&menu, {"cut", "copy", "paste", "duplicate"});
        menu.addSeparator();
        addNamed(&menu, {"split", "trimStart", "trimEnd", "rippleDelete", "delete"});
        menu.addSeparator();
        QMenu* attr = menu.addMenu("Attribute Editing");
        addNamed(attr, {"copyEffects", "pasteEffects"});
        if (videoTrack && !c->isAdjustment()) addNamed(&menu, {"cropZoom", "cropToFit", "rotateCw"});
        if (!c->isGenerated()) {   // M5 speed
            menu.addSeparator();
            QMenu* speed = menu.addMenu(icon("speed"), "Speed");
            addNamed(speed, {"speedSettings", "speedNormal"});
            QMenu* slow = speed->addMenu("Slow");
            for (const auto& [label, r] : {std::pair{"0.5x", Rational(1, 2)}, {"0.25x", Rational(1, 4)}, {"0.1x", Rational(1, 10)}})
                slow->addAction(label, [this, r] { edit_->setSpeedFactor(r); });
            QMenu* fast = speed->addMenu("Fast");
            for (const auto& [label, r] : {std::pair{"2x", Rational(2)}, {"4x", Rational(4)}, {"8x", Rational(8)}})
                fast->addAction(label, [this, r] { edit_->setSpeedFactor(r); });
            speed->addAction("Speed Ramping...", [this] { emit speedTabRequested(); });
            if (videoTrack && !c->speed.freeze)
                speed->addAction(icon("freeze"), "Freeze Frame at Playhead", [this, id] { edit_->freezeFrameAtPlayhead(id); });
            if (isRetimed(c->speed) && !c->speed.freeze) speed->addAction("Reset Speed", [this, id] { edit_->resetClipSpeed(id); });
            if (!c->speed.freeze) {
                QAction* rev = menu.addAction(icon("reverse"), "Reverse", [this, id] { edit_->toggleReverse(id); });
                rev->setCheckable(true);
                rev->setChecked(c->speed.reverse);
            }
        }
        if (hasSound) {   // the clip's sound
            menu.addSeparator();
            if (!c->linkId.empty()) addNamed(&menu, {"detach"});
            if (src && src->hasAudio && !c->speed.freeze) addNamed(&menu, {"speechToText"});
            addNamed(&menu, {"audioGain", "mute"});
        }
        if (audioClip) {   // M6 volume points and fades
            menu.addAction("Add / Remove Volume Point at Playhead", [this, id] { edit_->toggleKeyframe(id, "audio.volume"); });
            if (c->audio.fadeIn > Time(0) || c->audio.fadeOut > Time(0))
                menu.addAction("Remove Fades", [this, id] { edit_->setClipFades(id, Time(0), Time(0)); });
        }
        menu.addSeparator();
        if (c->outTransition) {
            menu.addAction(audioClip ? "Remove Crossfade" : "Remove Transition", [this, id] { edit_->removeTransition(id); });
        } else if (edit::maxTransitionDuration(doc_->sequence(), id, TransitionAlignment::Centered)) {
            if (audioClip) {   // M6
                QMenu* xf = menu.addMenu(icon("dissolve"), "Add Audio Crossfade to Next Clip");
                for (const auto& choice : crossfadeChoices()) {
                    const std::string shape = choice.second;
                    xf->addAction(choice.first, [this, id, shape] {
                        if (edit_->addAudioCrossfade(id, shape)) doc_->setSelectedTransition(id);
                    });
                }
            } else {
                menu.addAction(icon("dissolve"), "Add Dissolve to Next Clip", [this, id] { edit_->addTransition(id); });
            }
        }
        if (!c->isAdjustment() && ctr && ctr->kind != TrackKind::Subtitle) addNamed(&menu, {"replaceClip"});
        addNamed(&menu, {"toggleClip", "exportSelected"});
        if (!c->isGenerated()) {   // M7: the AI tools for this clip (the same actions as the AI Tools menu)
            menu.addSeparator();
            QMenu* ai = menu.addMenu("AI Tools");
            ai->setObjectName("clipAiMenu");
            if (videoTrack)
                ai->addAction(c->aiCutout.enabled ? "Background Removal... (on)" : "Background Removal...",
                              [this] { emit aiToolRequested("aiCutout"); });
            if (src && src->hasAudio && !c->speed.freeze)
                ai->addAction("Speech to Text...", [this] { emit aiToolRequested("speechToText"); });
            if (videoTrack) {
                ai->addAction("Auto Reframe...", [this] { emit aiToolRequested("autoReframe"); });
                ai->addAction("AI Interpolation...", [this] { emit aiToolRequested("aiInterpolation"); });
                ai->addAction(c->upscale.mode != UpscaleMode::Off ? "Video Upscaling... (on)" : "Video Upscaling...",
                              [this] { emit aiToolRequested("videoUpscaling"); });
                ai->addSection("Signal analysis");
                ai->addAction("Scene Detection...", [this] { emit aiToolRequested("sceneDetection"); });
                menu.addAction(c->tracking ? "Motion Tracking... (tracked)" : "Motion Tracking...",
                               [this] { emit videoSubTabRequested("tracking"); });
                menu.addAction(c->stabilization ? "Stabilization... (on)" : "Stabilization...",
                               [this] { emit videoSubTabRequested("stabilization"); });
            }
        }
        menu.addSeparator();
        addNamed(&menu, {"selectClipRange"});
        menu.addSeparator();
        addNamed(&menu, {"rename"});
        menu.addSeparator();
        if (!c->sourceId.empty()) addNamed(&menu, {"locateInLibrary"});
        addNamed(&menu, {"selectSameColor"});
        // the colour marks: a row of swatches (the first removes the mark)
        auto* marks = new QWidgetAction(&menu);
        auto* row = new QWidget(&menu);
        row->setObjectName("colorMarkRow");
        auto* hl = new QHBoxLayout(row);
        hl->setContentsMargins(22, 4, 12, 6);
        hl->setSpacing(5);
        for (int k = 0; k <= kColorMarkCount; ++k) {
            auto* b = new QToolButton(row);
            b->setObjectName(QString("colorMark%1").arg(k));
            b->setFixedSize(16, 16);
            b->setToolTip(k ? QString("Color mark %1").arg(k) : QString("No color mark"));
            const QColor col = k ? color::colorMarks[k - 1] : color::panelHeader;
            b->setStyleSheet(QString("QToolButton { background: %1; border: %2; border-radius: 2px; color: %3; }")
                                 .arg(col.name(), c->colorMark == k ? "2px solid #FFFFFF" : "1px solid #4A505A",
                                      color::textDim.name()));
            if (!k) b->setText(QString::fromUtf8("\xE2\x88\x85"));   // empty set sign: no mark
            QMenu* m = &menu;
            connect(b, &QToolButton::clicked, m, [this, k, m] {
                edit_->setColorMark(k);
                m->close();
            });
            hl->addWidget(b);
        }
        hl->addStretch(1);
        marks->setDefaultWidget(row);
        menu.addAction(marks);
    } else {
        if (const auto g = gapUnder(e->pos())) {   // M10: Delete Gap (also for gaps too narrow for the button)
            const std::string gapTrack = g->trackId;
            const Time gapTime = g->range.start;
            QAction* del = menu.addAction(icon("delete"), "Delete Gap", [this, gapTrack, gapTime] { edit_->deleteGap(gapTrack, gapTime); });
            del->setObjectName("deleteGap");
            if (const QString why = edit_->gapProblem(gapTrack, gapTime); !why.isEmpty()) {
                del->setEnabled(false);
                del->setToolTip(why);
                menu.setToolTipsVisible(true);
            }
            menu.addSeparator();
        }
        addNamed(&menu, {"paste", "selectAll"});
        menu.addSeparator();
        menu.addAction(icon("plus"), "Add Video Track", [this] { edit_->addTrack(TrackKind::Video); });
        menu.addAction(icon("plus"), "Add Audio Track", [this] { edit_->addTrack(TrackKind::Audio); });
    }
    menu.exec(e->globalPos());
}

void TimelineView::leaveEvent(QEvent*) {
    if (mode_ == Mode::None) unsetCursor();
    clearGapHover();
}

// ---- M10: Delete this gap -------------------------------------------------------------------------------------

std::optional<TimelineView::GapHover> TimelineView::gapUnder(const QPoint& pos) const {
    if (pos.x() < kHeaderW || pos.x() >= width() || pos.y() < kRulerH) return std::nullopt;
    const Row* r = rowAt(pos.y());
    if (!r) return std::nullopt;
    const Row row = *r;   // rowAt's storage is reused by the next call (hitTest)
    if (row.track->kind == TrackKind::Subtitle || row.track->locked) return std::nullopt;   // captions keep their pauses
    if (pos.y() < row.y + 3 || pos.y() >= row.y + row.h - 3) return std::nullopt;   // the clips' band of the lane
    if (!hitTest(pos).clipId.empty()) return std::nullopt;
    const auto gap = edit::gapAt(*row.track, Rational::fromDouble(timeForX(pos.x()), 10000));
    if (!gap) return std::nullopt;
    if (xForTime(gap->end().toDouble()) - xForTime(gap->start.toDouble()) < 6) return std::nullopt;   // too thin to see
    return GapHover{row.track->id, *gap};
}

QRect TimelineView::gapButtonRect() const {
    if (!gapHover_) return {};
    for (const auto& r : rows()) {
        if (r.track->id != gapHover_->trackId) continue;
        // centred in the visible part of the gap
        const int x0 = std::max(kHeaderW, xForTime(gapHover_->range.start.toDouble()));
        const int x1 = std::min(width(), xForTime(gapHover_->range.end().toDouble()));
        const int d = std::min(28, r.h - 14);
        if (x1 - x0 < d + 8) return {};
        const QPoint c((x0 + x1) / 2, r.y + r.h / 2);
        return QRect(c.x() - d / 2, c.y() - d / 2, d, d);
    }
    return {};
}

void TimelineView::updateGapHover(const QPoint& pos) {
    const std::optional<GapHover> g = mode_ == Mode::None && !dropActive_ ? gapUnder(pos) : std::nullopt;
    const bool changed = g.has_value() != gapHover_.has_value() ||
                         (g && (g->trackId != gapHover_->trackId || !(g->range == gapHover_->range)));
    if (changed) {
        gapHover_ = g;
        gapProblem_ = g ? edit_->gapProblem(g->trackId, g->range.start) : QString();
    }
    const bool hot = gapHover_ && gapButtonRect().contains(pos);
    if (changed || hot != gapButtonHot_) {
        gapButtonHot_ = hot;
        update();
    }
}

void TimelineView::clearGapHover() {
    if (!gapHover_ && !gapButtonHot_) return;
    gapHover_.reset();
    gapProblem_.clear();
    gapButtonHot_ = false;
    update();
}

void TimelineView::paintGapHighlight(QPainter& p) {
    if (!gapHover_) return;
    const QRect lane = trackLaneRect(gapHover_->trackId);
    if (lane.isEmpty()) return;
    const int x0 = std::max(kHeaderW, xForTime(gapHover_->range.start.toDouble()));
    const int x1 = std::min(width(), xForTime(gapHover_->range.end().toDouble()));
    if (x1 - x0 < 3) return;
    const QRectF r(x0 + 1.5, lane.y() + 3.5, x1 - x0 - 3, lane.height() - 7);
    p.save();
    p.setClipRect(QRect(kHeaderW, kRulerH, width() - kHeaderW, height() - kRulerH));
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath shape;
    shape.addRoundedRect(r, 5, 5);
    p.fillPath(shape, QColor(255, 255, 255, 12));
    p.save();
    p.setClipPath(shape, Qt::IntersectClip);   // diagonal hatching, as Clipchamp marks a gap
    p.setPen(QPen(QColor(255, 255, 255, 36), 1.5));
    for (double x = r.left() - r.height(); x < r.right(); x += 9) p.drawLine(QPointF(x, r.bottom()), QPointF(x + r.height(), r.top()));
    p.restore();
    p.setPen(QPen(QColor(255, 255, 255, 110), 1, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawPath(shape);
    p.restore();
}

void TimelineView::paintGapButton(QPainter& p) {   // drawn last: its label may reach over the row above
    if (!gapHover_) return;
    const QRect lane = trackLaneRect(gapHover_->trackId);
    if (lane.isEmpty()) return;
    const bool possible = gapProblem_.isEmpty();
    const QRect b = gapButtonRect();
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QString label;
    QPoint anchor;   // the label's bottom centre
    if (!b.isEmpty()) {
        p.setPen(QPen(QColor(255, 255, 255, possible ? 70 : 35), 1));
        p.setBrush(gapButtonHot_ && possible ? color::accent : QColor(0x2A, 0x2E, 0x36));
        p.drawEllipse(QRectF(b).adjusted(0.5, 0.5, -0.5, -0.5));
        const QColor ic = !possible ? color::textDim : gapButtonHot_ ? QColor(0x1A, 0x14, 0x08) : QColor(235, 238, 245);
        icon("delete", ic).paint(&p, b.adjusted(6, 6, -6, -6));
        if (gapButtonHot_) label = possible ? QString("Delete this gap") : gapProblem_;
        anchor = QPoint(b.center().x(), b.top() - 6);
    } else {   // too narrow for the button: the menu has it
        label = possible ? QString("Right-click to delete this gap") : gapProblem_;
        const int x0 = std::max(kHeaderW, xForTime(gapHover_->range.start.toDouble()));
        const int x1 = std::min(width(), xForTime(gapHover_->range.end().toDouble()));
        anchor = QPoint((x0 + x1) / 2, lane.y() - 2);
    }
    if (!label.isEmpty()) {
        const QFontMetrics fm(font());
        const int w = std::min(fm.horizontalAdvance(label) + 22, width() - kHeaderW - 8);
        const int h = fm.height() + 10;
        QRect bubble(anchor.x() - w / 2, anchor.y() - h, w, h);
        if (bubble.top() < 2) bubble.moveTop(b.isEmpty() ? lane.y() + 2 : b.bottom() + 6);   // no room above: below
        bubble.moveLeft(std::clamp(bubble.left(), kHeaderW + 4, std::max(kHeaderW + 4, width() - w - 4)));
        p.setPen(QPen(QColor(255, 255, 255, 45), 1));
        p.setBrush(QColor(0x1C, 0x1F, 0x25, 245));
        p.drawRoundedRect(QRectF(bubble).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        p.setPen(possible ? QColor(240, 242, 246) : QColor(0xF0, 0xB4, 0x6E));
        p.drawText(bubble, Qt::AlignCenter, fm.elidedText(label, Qt::ElideRight, bubble.width() - 12));
    }
    p.restore();
}

void TimelineView::resizeEvent(QResizeEvent*) {
    setScrollY(scrollY_);
    emit viewChanged();
}

// ---- drag & drop -------------------------------------------------------------------------------

namespace {
// Files from Explorer (MeerCut's own drags of library files carry their own format next to the URL)
bool explorerFiles(const QMimeData* m) {
    return m->hasUrls() && !m->hasFormat(kMimeSource) && !m->hasFormat(kMimeLibraryAudio) && !m->hasFormat(kMimeLibraryVideo);
}
}  // namespace

void TimelineView::dragEnterEvent(QDragEnterEvent* e) {
    clearGapHover();
    const QMimeData* m = e->mimeData();
    if (explorerFiles(m)) {
        acceptCopy(e);
        dropActive_ = false;
        return;
    }
    if (m->hasFormat(kMimeSource) || m->hasFormat(kMimeTransition) || m->hasFormat(kMimeTitle) || m->hasFormat(kMimeEffect) ||
        m->hasFormat(kMimeAdjustment) || m->hasFormat(kMimeSticker) || m->hasFormat(kMimeTemplate) ||
        m->hasFormat(kMimeLibraryAudio) || m->hasFormat(kMimeLibraryVideo)) {
        acceptCopy(e);
        dropActive_ = true;
    }
}

void TimelineView::placeGeneratedGhost() {
    const Sequence& seq = doc_->sequence();
    const FrameRate rate = seq.frameRate;
    const Time dur = frameToTime(std::max<int64_t>(1, timeToFrame(dropDuration_, rate, Rounding::Nearest)), rate);   // as addGenerated
    dropGhostTrack_ = edit::generatedTarget(seq, dropTrack_, TimeRange{dropTime_, dur});
    dropNewTrack_ = dropGhostTrack_.empty();
}

bool TimelineView::updateDropTarget(const QMimeData* mime, const QPoint& pos, Qt::KeyboardModifiers mods) {
    const Sequence& seq = doc_->sequence();
    dropEffectClip_.clear();
    dropAdjustment_ = false;
    dropSticker_.clear();
    dropTemplate_.clear();
    dropReplace_.clear();
    dropGhostTrack_.clear();
    dropNewTrack_ = false;
    dropEffectLayer_ = false;
    dropLibraryFile_.clear();
    if (mime->hasFormat(kMimeSticker) || mime->hasFormat(kMimeTemplate)) {   // M9: a sticker / template at the mouse
        dropIsTransition_ = false;
        dropSource_.clear();
        dropTitle_.clear();
        const bool sticker = mime->hasFormat(kMimeSticker);
        if (sticker) {
            dropSticker_ = mime->data(kMimeSticker).toStdString();
            const text::StickerInfo* info = text::findSticker(dropSticker_);
            if (!info) return false;
            dropDuration_ = Time::fromDouble(info->defaultSeconds, 1000);
        } else {
            dropTemplate_ = mime->data(kMimeTemplate).toStdString();
            const TemplateInfo* info = findTemplate(dropTemplate_);
            if (!info) return false;
            dropDuration_ = Time::fromDouble(info->seconds, 1000);
        }
        dropTime_ = snap(frameTimeAt(pos.x()), snapPoints({}));
        dropTrack_.clear();
        if (const Row* r = rowAt(pos.y()); sticker && r && r->track->kind == TrackKind::Video) dropTrack_ = r->track->id;
        if (sticker) placeGeneratedGhost();
        update();
        return true;
    }
    if (mime->hasFormat(kMimeLibraryAudio) || mime->hasFormat(kMimeLibraryVideo)) {
        // M10: a sound from the Audio page, on the audio track under the mouse; M11: a sample video from the Media page,
        // on the video track under the mouse (else Video 1)
        const bool video = mime->hasFormat(kMimeLibraryVideo);
        const TrackKind kind = video ? TrackKind::Video : TrackKind::Audio;
        dropIsTransition_ = false;
        dropSource_.clear();
        dropTitle_.clear();
        const QList<QUrl> urls = mime->urls();
        if (urls.isEmpty() || !urls.front().isLocalFile()) return false;
        dropLibraryFile_ = urls.front().toLocalFile();
        bool ok = false;
        const double seconds = QString::fromUtf8(mime->data(video ? kMimeLibraryVideo : kMimeLibraryAudio)).toDouble(&ok);
        const FrameRate rate = seq.frameRate;
        const Time length = Time::fromDouble(ok && seconds > 0 ? seconds : 1.0, 1000);
        dropDuration_ = frameToTime(std::max<int64_t>(1, timeToFrame(length, rate, Rounding::Floor)), rate);
        dropTime_ = snap(frameTimeAt(pos.x()), snapPoints({}));
        dropTrack_.clear();
        if (const Row* r = rowAt(pos.y()); r && r->track->kind == kind) dropTrack_ = r->track->id;
        else
            for (const auto& t2 : seq.tracks)
                if (t2.kind == kind && dropTrack_.empty()) dropTrack_ = t2.id;
        dropGhostTrack_ = dropTrack_;
        update();
        return true;
    }
    if (mime->hasFormat(kMimeSource)) {   // M9: a video dropped on a template's media slot replaces it
        const Hit h = hitTest(pos);
        const Clip* hc = h.clipId.empty() ? nullptr : edit::findClip(seq, h.clipId);
        const MediaSource* s0 = doc_->project().findSource(mime->data(kMimeSource).toStdString());
        if (hc && hc->sticker && hc->sticker->id == "placeholder" && s0 && s0->hasVideo) {
            dropIsTransition_ = false;
            dropReplace_ = h.clipId;
            dropSource_ = s0->id;
            dropEffectClip_ = h.clipId;   // highlighted like an effect target
            dropTrack_.clear();
            update();
            return true;
        }
    }
    if (mime->hasFormat(kMimeAdjustment)) {   // a new adjustment layer at the mouse (a video track, else above)
        dropIsTransition_ = false;
        dropSource_.clear();
        dropTitle_.clear();
        dropAdjustment_ = true;
        dropDuration_ = Time(5);
        dropTime_ = snap(frameTimeAt(pos.x()), snapPoints({}));
        dropTrack_.clear();
        if (const Row* r = rowAt(pos.y()); r && r->track->kind == TrackKind::Video) dropTrack_ = r->track->id;
        placeGeneratedGhost();
        update();
        return true;
    }
    if (mime->hasFormat(kMimeEffect)) {
        // M10: an effect or filter lands like a title - its own clip above the video, which affects every track below
        // it. Ctrl+drop onto a clip applies it to that clip only; a keying effect always needs the clip it keys.
        dropIsTransition_ = false;
        dropSource_.clear();
        dropTitle_.clear();
        dropTrack_.clear();
        const QByteArray payload = mime->data(kMimeEffect);
        if (payload != dropEffectPayload_) {   // parsed once per drag
            dropEffectPayload_ = payload;
            try {
                dropEffectHolder_ = clipFromJsonString(payload.toStdString());
            } catch (const std::exception&) {
                dropEffectHolder_ = Clip{};
            }
        }
        if (dropEffectHolder_.effects.size() != 1) return false;
        const EffectDef* def = findEffectDef(dropEffectHolder_.effects.front().type);
        if (!def) return false;
        const bool keying = def->category == "Keying";
        const Hit h = hitTest(pos);
        const Track* tr = h.clipId.empty() ? nullptr : seq.findTrack(h.trackId);
        if (tr && tr->kind == TrackKind::Video && (keying || (mods & Qt::ControlModifier))) {
            dropEffectClip_ = h.clipId;
            update();
            return true;
        }
        if (keying) {
            update();
            return false;
        }
        dropEffectLayer_ = true;
        dropDuration_ = Time(5);
        dropTime_ = snap(frameTimeAt(pos.x()), snapPoints({}));
        if (const Row* r = rowAt(pos.y()); r && r->track->kind == TrackKind::Video) dropTrack_ = r->track->id;
        placeGeneratedGhost();
        update();
        return true;
    }
    dropIsTransition_ = mime->hasFormat(kMimeTransition);
    if (dropIsTransition_) {
        dropCutClip_.clear();
        if (const Row* r = rowAt(pos.y()); r && r->track->kind == TrackKind::Video)
            dropCutClip_ = edit_->cutBefore(r->track->id, Rational::fromDouble(timeForX(pos.x()), 10000),
                                            Rational::fromDouble(20.0 / pps_, 10000));
        update();
        return !dropCutClip_.empty();
    }
    dropTitle_.clear();
    if (mime->hasFormat(kMimeTitle)) {
        dropSource_.clear();
        dropTitle_ = mime->data(kMimeTitle).toStdString();
        const text::TitleTemplate* tt = edit_->titleLibrary() ? edit_->titleLibrary()->find(dropTitle_)
                                                              : text::findTitleTemplate(dropTitle_);
        if (!tt) return false;
        dropDuration_ = tt->duration;
        dropTime_ = snap(frameTimeAt(pos.x()), snapPoints({}));
        dropTrack_.clear();
        const TrackKind want = tt->caption ? TrackKind::Subtitle : TrackKind::Video;
        if (const Row* r = rowAt(pos.y()); r && r->track->kind == want) dropTrack_ = r->track->id;
        else if (tt->caption)
            for (const auto& t2 : seq.tracks)
                if (t2.kind == TrackKind::Subtitle && dropTrack_.empty()) dropTrack_ = t2.id;
        if (tt->caption) dropGhostTrack_ = dropTrack_;
        else placeGeneratedGhost();
        update();
        return true;
    }
    dropSource_ = mime->data(kMimeSource).toStdString();
    const MediaSource* src = doc_->project().findSource(dropSource_);
    if (!src || !src->duration) return false;
    dropDuration_ = edit::fullClipDuration(*src, seq.frameRate);
    Time t = frameTimeAt(pos.x());
    t = snap(t, snapPoints({}));
    dropTime_ = t;
    dropTrack_.clear();
    const Row* r = rowAt(pos.y());
    if (src->hasVideo) {
        if (r && r->track->kind == TrackKind::Video) dropTrack_ = r->track->id;
        else
            for (auto it = seq.tracks.rbegin(); it != seq.tracks.rend(); ++it)
                if (it->kind == TrackKind::Video) dropTrack_ = it->id;
    } else {
        if (r && r->track->kind == TrackKind::Audio) dropTrack_ = r->track->id;
        else
            for (const auto& t2 : seq.tracks)
                if (t2.kind == TrackKind::Audio && dropTrack_.empty()) dropTrack_ = t2.id;
    }
    dropGhostTrack_ = dropTrack_;
    update();
    return true;
}

void TimelineView::dragMoveEvent(QDragMoveEvent* e) {
    const QMimeData* m = e->mimeData();
    if (explorerFiles(m)) {
        acceptCopy(e);
        return;
    }
    if (updateDropTarget(m, e->position().toPoint(), e->modifiers())) acceptCopy(e);
    else e->ignore();
}

void TimelineView::dragLeaveEvent(QDragLeaveEvent*) {
    dropActive_ = false;
    dropCutClip_.clear();
    dropEffectClip_.clear();
    dropGhostTrack_.clear();
    dropNewTrack_ = false;
    update();
}

void TimelineView::dropEvent(QDropEvent* e) {
    if (explorerFiles(e->mimeData())) {
        // files from Explorer: import them, then place them one after another at the drop time
        QStringList files, subtitles;
        for (const QUrl& u : e->mimeData()->urls()) {
            if (!u.isLocalFile()) continue;
            const QString ext = QFileInfo(u.toLocalFile()).suffix().toLower();
            (ext == "srt" || ext == "ass" || ext == "ssa" ? subtitles : files) << u.toLocalFile();
        }
        if (!subtitles.isEmpty()) emit subtitleFilesDropped(subtitles, snap(frameTimeAt(e->position().toPoint().x()), snapPoints({})));
        if (files.isEmpty()) {
            acceptCopy(e);
            update();
            return;
        }
        QStringList errors, notices;
        const auto ids = doc_->importMedia(files, &errors, &notices);
        if (!errors.isEmpty()) emit edit_->message("Some files could not be imported: " + errors.join("; "));
        else if (!notices.isEmpty()) emit edit_->message(notices.join("; "));
        Time at = snap(frameTimeAt(e->position().toPoint().x()), snapPoints({}));
        const Row* r = rowAt(e->position().toPoint().y());
        const std::string videoTrack = r && r->track->kind == TrackKind::Video ? r->track->id : std::string();
        const std::string audioTrack = r && r->track->kind == TrackKind::Audio ? r->track->id : std::string();   // M10
        for (const auto& id : ids) {
            const MediaSource* src = doc_->project().findSource(id);
            if (!src || !src->duration) continue;
            const Time length = edit::fullClipDuration(*src, doc_->sequence().frameRate);
            // a sound goes on the audio track it was dropped on (M10: it went to Audio 1 before)
            if (edit_->addToTimeline(id, at, videoTrack, src->hasVideo ? std::string() : audioTrack)) at = at + length;
        }
        acceptCopy(e);
        update();
        return;
    }
    const bool ok = updateDropTarget(e->mimeData(), e->position().toPoint(), e->modifiers());   // final position
    dropActive_ = false;
    dropGhostTrack_.clear();
    dropNewTrack_ = false;
    if (!ok) {
        e->ignore();
        update();
        return;
    }
    if (dropAdjustment_) {
        dropAdjustment_ = false;
        edit_->addAdjustmentLayer(dropTime_, dropDuration_, dropTrack_);
        acceptCopy(e);
        update();
        return;
    }
    if (!dropLibraryFile_.isEmpty()) {   // M10: a sound from the Audio page; M11: a sample video
        const QString path = dropLibraryFile_;
        dropLibraryFile_.clear();
        const bool video = e->mimeData()->hasFormat(kMimeLibraryVideo);
        QStringList errors, notices;
        const auto ids = doc_->importMedia({path}, &errors, &notices);
        if (ids.empty()) emit edit_->message((video ? "Could not add the video: " : "Could not add the sound: ") + errors.join("; "));
        else if (video) edit_->addToTimeline(ids.front(), dropTime_, dropTrack_);   // its sound: the paired audio track
        else edit_->addToTimeline(ids.front(), dropTime_, {}, dropTrack_);
        acceptCopy(e);
        update();
        return;
    }
    if (dropEffectLayer_) {   // M10: the effect as its own clip above the video
        dropEffectLayer_ = false;
        if (dropEffectHolder_.effects.size() == 1)
            edit_->addEffectLayer(dropEffectHolder_.effects.front(), dropEffectHolder_.name, dropTime_, dropDuration_, dropTrack_);
        acceptCopy(e);
        update();
        return;
    }
    if (!dropReplace_.empty()) {   // M9: media onto a template's slot
        const std::string target = dropReplace_, source = dropSource_;
        dropReplace_.clear();
        dropEffectClip_.clear();
        if (edit_->replaceClip(target, source)) doc_->setSelection({target});
        acceptCopy(e);
        update();
        return;
    }
    if (!dropSticker_.empty() || !dropTemplate_.empty()) {   // M9
        const std::string sticker = dropSticker_, tpl = dropTemplate_;
        dropSticker_.clear();
        dropTemplate_.clear();
        if (!sticker.empty()) edit_->addSticker(sticker, dropTime_, dropTrack_);
        else edit_->addTemplate(tpl, dropTime_);
        acceptCopy(e);
        update();
        return;
    }
    if (!dropEffectClip_.empty()) {
        try {
            const Clip holder = clipFromJsonString(e->mimeData()->data(kMimeEffect).toStdString());
            if (holder.effects.size() == 1 && edit_->addEffectToClips({dropEffectClip_}, holder.effects.front()))
                doc_->setSelection({dropEffectClip_});
        } catch (const std::exception& ex) {
            emit edit_->message(QString::fromUtf8(ex.what()));
        }
        dropEffectClip_.clear();
        acceptCopy(e);
        update();
        return;
    }
    if (dropIsTransition_) {
        std::string type = e->mimeData()->data(kMimeTransition).toStdString();
        if (!findTransition(type)) type = "fa.transition.dissolve";
        if (!dropCutClip_.empty()) {
            const Clip* c = edit::findClip(doc_->sequence(), dropCutClip_);
            bool added = false;
            if (c && c->outTransition) {   // replace the type, keep timing and settings
                TransitionRef r = *c->outTransition;
                r.type = type;
                added = edit_->updateTransitionSettings(dropCutClip_, r);
            } else {
                added = edit_->addTransition(dropCutClip_, std::nullopt, TransitionAlignment::Centered, type);
            }
            if (added) doc_->setSelectedTransition(dropCutClip_);
        }
        dropCutClip_.clear();
    } else if (!dropTitle_.empty()) {
        edit_->addTitleFromTemplate(dropTitle_, dropTime_, dropTrack_);
        dropTitle_.clear();
    } else if (!dropSource_.empty()) {
        const Track* tr = doc_->sequence().findTrack(dropTrack_);
        const std::string videoTrack = tr && tr->kind == TrackKind::Video ? dropTrack_ : std::string();
        // M10: a sound goes on the audio track it was dropped on
        const MediaSource* src = doc_->project().findSource(dropSource_);
        const std::string audioTrack = tr && tr->kind == TrackKind::Audio && src && !src->hasVideo ? dropTrack_ : std::string();
        edit_->addToTimeline(dropSource_, dropTime_, videoTrack, audioTrack);
    }
    acceptCopy(e);
    update();
}

}  // namespace fa::ui
