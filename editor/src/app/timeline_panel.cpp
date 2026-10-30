#include "panels.h"

#include "edit_controller.h"
#include "theme.h"
#include "timeline_view.h"

#include <QAction>
#include <QHBoxLayout>
#include <QMenu>
#include <QScrollBar>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace fa::ui {

namespace {
QToolButton* toolButton(QAction* a, QWidget* parent) {
    auto* b = new QToolButton(parent);
    b->setDefaultAction(a);
    b->setIconSize(QSize(20, 20));
    b->setAutoRaise(true);
    return b;
}
constexpr double kMinPps = 0.05;
}  // namespace

TimelinePanel::TimelinePanel(Document* doc, EditController* edit, MediaService* media, PlaybackController* playback,
                             const TimelineActions& a, QWidget* parent)
    : QWidget(parent), doc_(doc) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto* bar = new QWidget(this);
    bar->setObjectName("timelineToolbar");
    bar->setStyleSheet(QString("#timelineToolbar { background: %1; border-bottom: 1px solid %2; }")
                           .arg(color::panel.name(), color::border.name()));
    auto* bl = new QHBoxLayout(bar);
    bl->setContentsMargins(8, 4, 8, 4);
    bl->setSpacing(2);
    for (QAction* x : {a.undo, a.redo}) bl->addWidget(toolButton(x, bar));
    bl->addSpacing(10);
    for (QAction* x : {a.del, a.split, a.rippleDelete}) bl->addWidget(toolButton(x, bar));
    if (a.named) {   // M9: Filmora's quick tools - crop, text, speed, freeze frame, detach audio, mute
        auto named = [&](const char* n) -> QAction* {
            auto it = a.named->find(n);
            return it == a.named->end() ? nullptr : it->second;
        };
        bl->addSpacing(10);
        for (const char* n : {"cropZoom", "addTitle"})
            if (QAction* x = named(n)) bl->addWidget(toolButton(x, bar));
        if (QAction* sp = named("speedSettings")) {
            auto* b = toolButton(sp, bar);
            b->setObjectName("toolbarSpeed");
            auto* m = new QMenu(b);
            if (a.fillSpeedMenu) a.fillSpeedMenu(m);
            b->setMenu(m);
            b->setPopupMode(QToolButton::MenuButtonPopup);
            bl->addWidget(b);
        }
        for (const char* n : {"freezeFrame", "detach", "mute"})
            if (QAction* x = named(n)) bl->addWidget(toolButton(x, bar));
    }
    bl->addSpacing(10);
    bl->addWidget(toolButton(a.snapping, bar));
    if (a.renderPreview) bl->addWidget(toolButton(a.renderPreview, bar));   // M5
    auto* addV = new QToolButton(bar);
    addV->setIcon(icon("plus"));
    addV->setText("Video track");
    addV->setToolTip("Add a video track");
    addV->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connect(addV, &QToolButton::clicked, this, [edit] { edit->addTrack(TrackKind::Video); });
    auto* addA = new QToolButton(bar);
    addA->setIcon(icon("plus"));
    addA->setText("Audio track");
    addA->setToolTip("Add an audio track");
    addA->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connect(addA, &QToolButton::clicked, this, [edit] { edit->addTrack(TrackKind::Audio); });
    bl->addSpacing(10);
    bl->addWidget(addV);
    bl->addWidget(addA);
    bl->addStretch(1);
    bl->addWidget(toolButton(a.zoomOut, bar));
    zoom_ = new QSlider(Qt::Horizontal, bar);
    zoom_->setRange(0, 1000);
    zoom_->setFixedWidth(160);
    zoom_->setToolTip("Timeline zoom (Ctrl + mouse wheel)");
    bl->addWidget(zoom_);
    bl->addWidget(toolButton(a.zoomIn, bar));
    bl->addWidget(toolButton(a.zoomFit, bar));
    lay->addWidget(bar);

    auto* mid = new QHBoxLayout();
    mid->setSpacing(0);
    view_ = new TimelineView(doc, edit, media, playback, this);
    view_->setNamedActions(a.named);
    vbar_ = new QScrollBar(Qt::Vertical, this);
    mid->addWidget(view_, 1);
    mid->addWidget(vbar_);
    lay->addLayout(mid, 1);
    hbar_ = new QScrollBar(Qt::Horizontal, this);
    lay->addWidget(hbar_);

    connect(view_, &TimelineView::viewChanged, this, &TimelinePanel::syncScrollBars);
    connect(hbar_, &QScrollBar::valueChanged, this, [this](int v) {
        if (!syncing_) view_->setScrollSeconds(v / 100.0);
    });
    connect(vbar_, &QScrollBar::valueChanged, this, [this](int v) {
        if (!syncing_) view_->setScrollY(v);
    });
    connect(zoom_, &QSlider::valueChanged, this, [this, doc](int v) {
        if (syncing_) return;
        const double maxPps = doc->sequence().frameRate.toDouble() * 60.0;
        const double pps = kMinPps * std::pow(maxPps / kMinPps, v / 1000.0);
        view_->setPixelsPerSecond(pps);
    });
    connect(a.zoomIn, &QAction::triggered, view_, [this] { view_->zoomBy(1.5); });
    connect(a.zoomOut, &QAction::triggered, view_, [this] { view_->zoomBy(1 / 1.5); });
    connect(a.zoomFit, &QAction::triggered, view_, [this] { view_->zoomToFit(); });
    connect(a.snapping, &QAction::toggled, view_, [this](bool on) { view_->setSnapping(on); });
    syncScrollBars();
}

void TimelinePanel::syncScrollBars() {
    syncing_ = true;
    const double vis = view_->visibleSeconds();
    hbar_->setRange(0, static_cast<int>(std::max(0.0, view_->contentSeconds() - vis * 0.5) * 100));
    hbar_->setPageStep(static_cast<int>(vis * 100));
    hbar_->setSingleStep(std::max(1, static_cast<int>(vis * 10)));
    hbar_->setValue(static_cast<int>(view_->scrollSeconds() * 100));
    vbar_->setRange(0, std::max(0, view_->contentHeight() - view_->height()));
    vbar_->setPageStep(view_->height());
    vbar_->setValue(view_->scrollY());
    vbar_->setVisible(vbar_->maximum() > 0);
    // zoom slider position (log scale between kMinPps and 60 px per frame)
    const double maxPps = doc_->sequence().frameRate.toDouble() * 60.0;
    zoom_->setValue(static_cast<int>(std::lround(1000.0 * std::log(view_->pixelsPerSecond() / kMinPps) /
                                                  std::log(maxPps / kMinPps))));
    syncing_ = false;
}

}  // namespace fa::ui
