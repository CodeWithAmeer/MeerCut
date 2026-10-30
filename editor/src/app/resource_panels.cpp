#include "panels.h"

#include "edit_controller.h"
#include "media_service.h"
#include "theme.h"
#include "timeline_view.h"

#include "fa/model/edit_ops.h"
#include "fa/render/renderer.h"

#include <QFileInfo>
#include <QButtonGroup>
#include <QDir>
#include <QDragEnterEvent>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QToolButton>
#include <QUrl>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <functional>

namespace fa::ui {

namespace {
constexpr int kTileW = 168;
constexpr int kTileH = 95;
constexpr int kRoleSource = Qt::UserRole + 1;
constexpr int kRoleImportTile = Qt::UserRole + 2;
constexpr int kRoleSample = Qt::UserRole + 3;   // M11: a sample video's id
enum MediaRow { kRowAll, kRowVideo, kRowAudio, kRowSamples };   // the sidebar

QString durationText(const Time& d) {
    const int64_t s = static_cast<int64_t>(d.toDouble() + 0.5);
    return s >= 3600 ? QString("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'))
                     : QString("%1:%2").arg(s / 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
}

// A library tile: the picture (painted by the callback inside the rounded frame), a duration badge and the amber
// "in use" check.
QPixmap tileFrame(const std::function<void(QPainter&, const QRectF&)>& picture, const QString& duration, bool check) {
    QPixmap pm(kTileW * 2, kTileH * 2);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r(0, 0, kTileW, kTileH);
    QPainterPath clip;
    clip.addRoundedRect(r, 6, 6);
    p.setClipPath(clip);
    p.fillRect(r, color::panelHeader);
    picture(p, r);
    p.setClipping(false);
    if (!duration.isEmpty()) {
        QFont f = p.font();
        f.setPointSizeF(8.5);
        p.setFont(f);
        const int w = p.fontMetrics().horizontalAdvance(duration) + 10;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 170));
        p.drawRoundedRect(QRectF(6, kTileH - 22, w, 16), 3, 3);
        p.setPen(QColor(240, 240, 240));
        p.drawText(QRectF(6, kTileH - 22, w, 16), Qt::AlignCenter, duration);
    }
    if (check) {   // used in the timeline / already in the project (amber check)
        p.setPen(Qt::NoPen);
        p.setBrush(color::accent);
        p.drawEllipse(QPointF(kTileW - 14, kTileH - 14), 8, 8);
        p.setPen(QPen(QColor(0x1A, 0x14, 0x08), 2));
        p.drawLine(QPointF(kTileW - 18, kTileH - 14), QPointF(kTileW - 15, kTileH - 11));
        p.drawLine(QPointF(kTileW - 15, kTileH - 11), QPointF(kTileW - 10, kTileH - 18));
    }
    return pm;
}

// The image (or the part src of it) scaled to fill r, centred.
void drawCover(QPainter& p, const QRectF& r, const QImage& img, QRect src = {}) {
    if (img.isNull()) return;
    if (src.isEmpty()) src = img.rect();
    const QSizeF s = QSizeF(src.size()).scaled(r.size(), Qt::KeepAspectRatioByExpanding);
    p.drawImage(QRectF(r.x() + (r.width() - s.width()) / 2, r.y() + (r.height() - s.height()) / 2, s.width(), s.height()),
                img, QRectF(src));
}

// The picture without letterbox bars: black rows at the top and bottom left off (when they are less than half of it).
QRect withoutBars(const QImage& img) {
    if (img.isNull()) return {};
    const QImage g = img.convertToFormat(QImage::Format_Grayscale8);
    auto black = [&g](int y) {
        const uchar* row = g.constScanLine(y);
        return std::all_of(row, row + g.width(), [](uchar v) { return v < 24; });
    };
    int top = 0, bottom = g.height() - 1;
    while (top < bottom && black(top)) ++top;
    while (bottom > top && black(bottom)) --bottom;
    const int h = bottom - top + 1;
    return h * 2 >= g.height() ? QRect(0, top, g.width(), h) : g.rect();
}
}  // namespace

// ---- resource tabs ---------------------------------------------------------------------------

ResourceTabs::ResourceTabs(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto* barW = new QWidget(this);
    barW->setObjectName("resourceBar");
    barW->setStyleSheet(QString("#resourceBar { border-bottom: 1px solid %1; }"
                                "QToolButton#resourceTab { color: %2; padding: 6px 14px 4px 14px; border-radius: 0; }"
                                "QToolButton#resourceTab:checked { color: %3; background: transparent; border-bottom: 2px solid %3; }")
                            .arg(color::border.name(), color::textDim.name(), color::accent.name()));
    bar_ = new QHBoxLayout(barW);
    bar_->setContentsMargins(6, 2, 6, 0);
    bar_->setSpacing(2);
    bar_->addStretch(1);
    stack_ = new QStackedWidget(this);
    lay->addWidget(barW);
    lay->addWidget(stack_, 1);
}

void ResourceTabs::addPage(QWidget* page, const char* iconName, const QString& text) {
    auto* b = new QToolButton(this);
    b->setObjectName("resourceTab");
    QIcon ic;
    ic.addPixmap(icon(iconName, color::textDim).pixmap(48, 48), QIcon::Normal, QIcon::Off);
    ic.addPixmap(icon(iconName, color::accent).pixmap(48, 48), QIcon::Normal, QIcon::On);
    b->setIcon(ic);
    b->setIconSize(QSize(24, 24));
    b->setText(text);
    b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    b->setCheckable(true);
    b->setAutoExclusive(true);
    b->setAccessibleName(text);
    bar_->insertWidget(static_cast<int>(buttons_.size()), b);
    buttons_.push_back(b);
    stack_->addWidget(page);
    connect(b, &QToolButton::clicked, this, [this, page] { setCurrentWidget(page); });
    if (buttons_.size() == 1) setCurrentWidget(page);
}

void ResourceTabs::setCurrentWidget(QWidget* page) {
    const int i = stack_->indexOf(page);
    if (i < 0) return;
    stack_->setCurrentIndex(i);
    buttons_[static_cast<size_t>(i)]->setChecked(true);
}

QWidget* ResourceTabs::currentWidget() const { return stack_->currentWidget(); }

// ---- media list ------------------------------------------------------------------------------

QStringList MediaList::mimeTypes() const { return {kMimeSource}; }

QMimeData* MediaList::mimeData(const QList<QListWidgetItem*>& items) const {
    if (items.isEmpty() || items.front()->data(kRoleImportTile).toBool()) return nullptr;
    if (const QString sample = items.front()->data(kRoleSample).toString(); !sample.isEmpty())
        return sampleMime ? sampleMime(sample) : nullptr;
    auto* m = new QMimeData();
    m->setData(kMimeSource, items.front()->data(kRoleSource).toString().toUtf8());
    return m;
}

MediaPanel::MediaPanel(Document* doc, EditController* edit, MediaService* media, QWidget* parent)
    : QWidget(parent), doc_(doc), edit_(edit), media_(media) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(8);
    auto* top = new QHBoxLayout();
    auto* importBtn = new QPushButton(icon("import"), "Import", this);
    importBtn->setToolTip("Import video or audio files (Ctrl+I)");
    connect(importBtn, &QPushButton::clicked, this, &MediaPanel::importRequested);
    search_ = new QLineEdit(this);
    search_->setPlaceholderText("Search media");
    search_->setClearButtonEnabled(true);
    top->addWidget(importBtn);
    top->addWidget(search_, 1);
    lay->addLayout(top);
    // sidebar (real filters) + library
    auto* body = new QHBoxLayout();
    body->setSpacing(8);
    sidebar_ = new QListWidget(this);
    sidebar_->setObjectName("mediaSidebar");
    sidebar_->setFixedWidth(150);
    sidebar_->setStyleSheet(QString("QListWidget#mediaSidebar { background: transparent; }"
                                    "QListWidget#mediaSidebar::item { padding: 6px 8px; }"));
    for (const char* n : {"Project Media", "Video", "Audio", "Sample Videos"}) sidebar_->addItem(n);
    sidebar_->setCurrentRow(0);
    body->addWidget(sidebar_);
    auto* right = new QVBoxLayout();
    right->setSpacing(6);
    auto* title = new QLabel("PROJECT MEDIA", this);
    title->setObjectName("panelTitle");
    right->addWidget(title);
    connect(sidebar_, &QListWidget::currentRowChanged, this, [this, title](int row) {
        title->setText(row == kRowVideo     ? "VIDEO"
                       : row == kRowAudio   ? "AUDIO"
                       : row == kRowSamples ? "SAMPLE VIDEOS"
                                            : "PROJECT MEDIA");
        refresh();
    });

    list_ = new MediaList(this);
    list_->setViewMode(QListView::IconMode);
    list_->setIconSize(QSize(kTileW, kTileH));
    list_->setGridSize(QSize(kTileW + 16, kTileH + 34));
    list_->setResizeMode(QListView::Adjust);
    list_->setMovement(QListView::Static);
    list_->setWordWrap(false);
    list_->setTextElideMode(Qt::ElideMiddle);
    list_->setDragEnabled(true);
    list_->setDragDropMode(QAbstractItemView::DragOnly);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setAccessibleName("Media library");
    // files dragged in from Explorer are imported (the library's own items only drag out)
    list_->setAcceptDrops(true);
    list_->viewport()->setAcceptDrops(true);
    list_->viewport()->installEventFilter(this);
    right->addWidget(list_, 1);
    dropHint_ = new QLabel("Or drag videos or audio files here from Explorer", this);
    dropHint_->setAlignment(Qt::AlignCenter);
    dropHint_->setStyleSheet(QString("color: %1; padding: 8px;").arg(color::textDim.name()));
    right->addWidget(dropHint_);
    // M11: the sample videos and their credits (each different one once)
    QString samplesError;
    samples_ = loadSampleVideos(&samplesError);
    QStringList credits;
    for (const auto& s : samples_) {
        if (s.credit.isEmpty()) continue;
        const QString licence = s.licenceUrl.isEmpty() ? s.licence.toHtmlEscaped()
                                                       : QString("<a href=\"%1\" style=\"color: %2;\">%3</a>")
                                                             .arg(s.licenceUrl.toHtmlEscaped(), color::accent.name(),
                                                                  s.licence.toHtmlEscaped());
        const QString line = s.credit.toHtmlEscaped() + ", " + licence;
        if (!credits.contains(line)) credits << line;
    }
    samplesHint_ = new QLabel(this);
    samplesHint_->setObjectName("samplesHint");
    samplesHint_->setWordWrap(true);
    samplesHint_->setTextFormat(Qt::RichText);
    samplesHint_->setOpenExternalLinks(true);
    samplesHint_->setStyleSheet(QString("color: %1; padding: 4px 2px;").arg(color::textDim.name()));
    samplesHint_->setText(!samplesError.isEmpty()
                              ? samplesError.toHtmlEscaped()
                              : "Free clips to practise with: double-click one to add it to the timeline, or drag it there." +
                                    (credits.isEmpty() ? QString()
                                                       : "<br>" + credits.join("<br>") +
                                                             "<br>Credit these if you share a video made with them. MeerCut "
                                                             "is not affiliated with their makers."));
    samplesHint_->hide();
    right->addWidget(samplesHint_);
    list_->sampleMime = [this](const QString& id) -> QMimeData* {
        const SampleVideo* s = findSample(id);
        const QString path = s ? materializeAsset("samples/" + s->file) : QString();
        if (path.isEmpty()) return nullptr;
        auto* m = new QMimeData();
        m->setUrls({QUrl::fromLocalFile(path)});   // other programs take the file
        m->setData(kMimeLibraryVideo, QByteArray::number(s->seconds));   // the timeline shows where it lands
        return m;
    };
    body->addLayout(right, 1);
    lay->addLayout(body, 1);

    connect(search_, &QLineEdit::textChanged, this, &MediaPanel::refresh);
    connect(doc_, &Document::changed, this, &MediaPanel::refresh);
    connect(media_, &MediaService::thumbnailReady, this, [this] {
        if (refreshPending_) return;   // thumbnails arrive in bursts: refresh at most every 150 ms
        refreshPending_ = true;
        QTimer::singleShot(150, this, [this] {
            refreshPending_ = false;
            refresh();
        });
    });
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) {
        if (it->data(kRoleImportTile).toBool()) emit importRequested();
        else if (const QString sample = it->data(kRoleSample).toString(); !sample.isEmpty()) addSample(sample);
        else edit_->addToTimeline(it->data(kRoleSource).toString().toStdString());
    });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        if (it->data(kRoleImportTile).toBool()) emit importRequested();
    });
    connect(list_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* it = list_->itemAt(pos);
        if (!it || it->data(kRoleImportTile).toBool()) return;
        QMenu m(this);
        if (const QString sample = it->data(kRoleSample).toString(); !sample.isEmpty()) {
            m.addAction(icon("plus"), "Add to Timeline at Playhead", [this, sample] { addSample(sample); });
            m.addAction(icon("import"), "Add to Project Media", [this, sample] { addSample(sample, std::nullopt, false); });
            m.exec(list_->viewport()->mapToGlobal(pos));
            return;
        }
        const std::string id = it->data(kRoleSource).toString().toStdString();
        m.addAction(icon("plus"), "Add to Timeline at Playhead", [this, id] { edit_->addToTimeline(id); });
        m.addAction("Relink Media...", [this, id] { emit relinkRequested(QString::fromStdString(id)); });
        m.exec(list_->viewport()->mapToGlobal(pos));
    });
    refresh();
}

QPixmap MediaPanel::tile(const MediaSource& src, bool used, bool missing) {
    const QString path = doc_->mediaFilePath(src);
    // duration badge: a scene item shows its own length
    const QString duration = src.range ? durationText(src.range->duration) : src.duration ? durationText(*src.duration) : QString();
    return tileFrame(
        [&](QPainter& p, const QRectF& r) {
            if (missing) {
                p.fillRect(r, QColor(90, 16, 24));
                p.setPen(color::text);
                p.drawText(r, Qt::AlignCenter, "Media offline");
            } else if (src.hasVideo) {
                // a scene item (M7): its first second; a file: a third into it (at most 1 s)
                const double t = src.range ? src.range->start.toDouble() + std::min(0.5, src.range->duration.toDouble() / 3.0)
                                 : src.duration ? std::min(1.0, src.duration->toDouble() / 3.0) : 0.0;
                drawCover(p, r, media_->thumbnail(path, t, 1.0));
            } else {   // audio-only
                icon("volume", color::waveform).paint(&p, QRect(kTileW / 2 - 20, kTileH / 2 - 20, 40, 40));
            }
        },
        duration, used);
}

// ---- M11: sample videos ----------------------------------------------------------------------------------------

QPixmap MediaPanel::sampleTile(const SampleVideo& s, bool inProject) {
    const QImage th = media_->thumbnail(QDir(assetsDir()).filePath("samples/" + s.file), s.poster, 1.0);
    return tileFrame([&](QPainter& p, const QRectF& r) { drawCover(p, r, th, withoutBars(th)); },
                     durationText(Time::fromDouble(s.seconds, 1000)), inProject);
}

const SampleVideo* MediaPanel::findSample(const QString& id) const {
    for (const auto& s : samples_)
        if (s.id == id) return &s;
    return nullptr;
}

std::string MediaPanel::sampleSourceId(const SampleVideo& s) const {
    const QString copy = QDir::cleanPath(QFileInfo(QDir(userLibraryDir()).filePath("samples/" + s.file)).absoluteFilePath());
    for (const auto& src : doc_->project().sources)
        if (doc_->mediaFilePath(src).compare(copy, Qt::CaseInsensitive) == 0) return src.id;
    return {};
}

std::string MediaPanel::addSample(const QString& id, std::optional<Time> at, bool toTimeline) {
    const SampleVideo* s = findSample(id);
    if (!s) return {};
    const QString path = materializeAsset("samples/" + s->file);
    if (path.isEmpty()) {
        emit edit_->message("The sample video " + s->file + " is missing, or could not be copied to " +
                            QDir::toNativeSeparators(userLibraryDir()));
        return {};
    }
    QStringList errors;
    const auto ids = doc_->importMedia({path}, &errors);
    if (ids.empty()) {
        emit edit_->message("Could not add the sample video: " + errors.join("; "));
        return {};
    }
    if (toTimeline && !edit_->addToTimeline(ids.front(), at)) return {};
    return ids.front();
}

void MediaPanel::refreshSamples(const QString& filter) {
    std::vector<const SampleVideo*> shown;
    for (const auto& s : samples_)
        if (filter.isEmpty() || s.title.contains(filter, Qt::CaseInsensitive) || s.description.contains(filter, Qt::CaseInsensitive))
            shown.push_back(&s);
    bool same = list_->count() == static_cast<int>(shown.size());
    for (size_t i = 0; same && i < shown.size(); ++i)
        same = list_->item(static_cast<int>(i))->data(kRoleSample).toString() == shown[i]->id;
    if (!same) {   // rebuilt only when the items change (thumbnail updates keep the selection)
        const QString selected = list_->currentItem() ? list_->currentItem()->data(kRoleSample).toString() : QString();
        list_->clear();
        for (const SampleVideo* s : shown) {
            auto* it = new QListWidgetItem(s->title);
            it->setData(kRoleSample, s->id);
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
            list_->addItem(it);
            if (s->id == selected) list_->setCurrentItem(it);
        }
    }
    for (size_t i = 0; i < shown.size(); ++i) {
        const SampleVideo& s = *shown[i];
        const bool inProject = !sampleSourceId(s).empty();
        QListWidgetItem* it = list_->item(static_cast<int>(i));
        it->setIcon(QIcon(sampleTile(s, inProject)));
        QString tip = QString("%1 - %2, %3x%4, %5 fps\n%6")
                          .arg(s.title, durationText(Time::fromDouble(s.seconds, 1000)))
                          .arg(s.width)
                          .arg(s.height)
                          .arg(s.fps, 0, 'g', 4)
                          .arg(s.description);
        tip += s.credit.isEmpty() ? QString("\n\nMeerCut's own clip - free to use in your videos.")
                                  : QString("\n\n%1 (%2). Credit it if you share a video made with it.").arg(s.credit, s.licence);
        if (inProject) tip += "\nAlready in this project (Project Media).";
        tip += "\nDouble-click to add it to the timeline, or drag it there.";
        it->setToolTip(tip);
    }
}

bool MediaPanel::eventFilter(QObject* o, QEvent* e) {
    if (o == list_->viewport()) {
        auto localFiles = [](const QMimeData* m) {
            QStringList files;
            if (m && m->hasUrls())
                for (const QUrl& u : m->urls())
                    if (u.isLocalFile()) files << u.toLocalFile();
            return files;
        };
        switch (e->type()) {
            case QEvent::DragEnter:
            case QEvent::DragMove: {
                auto* d = static_cast<QDragMoveEvent*>(e);
                if (!localFiles(d->mimeData()).isEmpty()) d->acceptProposedAction();
                else d->ignore();   // the library's own items are not re-arranged
                return true;
            }
            case QEvent::Drop: {
                auto* d = static_cast<QDropEvent*>(e);
                const QStringList files = localFiles(d->mimeData());
                if (!files.isEmpty()) {
                    d->acceptProposedAction();
                    emit filesDropped(files);
                } else {
                    d->ignore();
                }
                return true;
            }
            default: break;
        }
    }
    return QWidget::eventFilter(o, e);
}

void MediaPanel::selectSource(const QString& sourceId) {
    if (sidebar_->count() > 0) sidebar_->setCurrentRow(0);   // "Project Media": every source
    search_->clear();
    refresh();
    for (int i = 0; i < list_->count(); ++i)
        if (list_->item(i)->data(kRoleSource).toString() == sourceId) {
            list_->setCurrentItem(list_->item(i));
            list_->scrollToItem(list_->item(i));
            return;
        }
}

void MediaPanel::refresh() {
    const auto missing = doc_->missingSources();
    const QString filter = search_->text().trimmed();
    const int kind = sidebar_->currentRow();   // a MediaRow
    int nVideo = 0, nAudio = 0;
    std::vector<const MediaSource*> shown;
    for (const auto& s : doc_->project().sources) {
        nVideo += s.hasVideo ? 1 : 0;
        nAudio += s.hasVideo ? 0 : 1;
        if ((kind == kRowVideo && !s.hasVideo) || (kind == kRowAudio && s.hasVideo)) continue;
        const QString name = s.name.empty() ? QFileInfo(QString::fromUtf8(s.path)).fileName() : QString::fromStdString(s.name);
        if (filter.isEmpty() || name.contains(filter, Qt::CaseInsensitive)) shown.push_back(&s);
    }
    const int nAll = static_cast<int>(doc_->project().sources.size());
    sidebar_->item(kRowAll)->setText(QString("Project Media (%1)").arg(nAll));
    sidebar_->item(kRowVideo)->setText(QString("Video (%1)").arg(nVideo));
    sidebar_->item(kRowAudio)->setText(QString("Audio (%1)").arg(nAudio));
    sidebar_->item(kRowSamples)->setText(QString("Sample Videos (%1)").arg(samples_.size()));
    samplesHint_->setVisible(kind == kRowSamples);
    dropHint_->setVisible(nAll == 0 && kind != kRowSamples);
    if (kind == kRowSamples) {
        refreshSamples(filter);
        return;
    }
    // Rebuild only when the set of items changes; otherwise update icons/tooltips in place so
    // selection, scrolling and in-progress clicks/drags are not disturbed by thumbnail updates.
    bool sameItems = list_->count() == static_cast<int>(shown.size()) + 1 && list_->item(0)->data(kRoleImportTile).toBool();
    for (size_t i = 0; sameItems && i < shown.size(); ++i)
        sameItems = list_->item(static_cast<int>(i) + 1)->data(kRoleSource).toString() == QString::fromStdString(shown[i]->id);
    if (!sameItems) {
        const QString selected = list_->currentItem() ? list_->currentItem()->data(kRoleSource).toString() : QString();
        list_->clear();
        auto* imp = new QListWidgetItem(icon("plus", color::textDim), "Import Media");
        imp->setData(kRoleImportTile, true);
        imp->setFlags(Qt::ItemIsEnabled);
        imp->setToolTip("Import video or audio files");
        list_->addItem(imp);
        for (const MediaSource* s : shown) {
            auto* it = new QListWidgetItem(s->name.empty() ? QFileInfo(QString::fromUtf8(s->path)).fileName()
                                                           : QString::fromStdString(s->name));
            it->setData(kRoleSource, QString::fromStdString(s->id));
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
            list_->addItem(it);
            if (it->data(kRoleSource).toString() == selected) list_->setCurrentItem(it);
        }
    }
    for (size_t i = 0; i < shown.size(); ++i) {
        const MediaSource& s = *shown[i];
        QListWidgetItem* it = list_->item(static_cast<int>(i) + 1);
        const bool isMissing = std::find(missing.begin(), missing.end(), s.id) != missing.end();
        it->setIcon(QIcon(tile(s, doc_->isSourceUsed(s.id), isMissing)));
        QString tip = QString::fromUtf8(s.path) + "\n";
        if (s.hasVideo)
            tip += QString("Video: %1x%2, %3 fps\n").arg(s.width).arg(s.height).arg(s.frameRate ? s.frameRate->toDouble() : 0.0, 0, 'f', 3);
        tip += QString("Audio: %1\n").arg(s.hasAudio ? "yes" : "none");
        if (s.duration) tip += QString("Duration: %1 s").arg(s.duration->toDouble(), 0, 'f', 3);
        if (isMissing) tip += "\nFILE NOT FOUND - right-click > Relink Media";
        it->setToolTip(tip);
    }
}

// ---- transitions -----------------------------------------------------------------------------

QStringList TransitionList::mimeTypes() const { return {kMimeTransition}; }

QMimeData* TransitionList::mimeData(const QList<QListWidgetItem*>& items) const {
    if (items.isEmpty()) return nullptr;
    auto* m = new QMimeData();
    m->setData(kMimeTransition, items.front()->data(Qt::UserRole).toString().toUtf8());
    return m;
}

namespace {

// Two synthetic pictures for the thumbnails (original artwork: gradients and letters).
Image samplePicture(bool b, int w, int h) {
    QImage q(w, h, QImage::Format_RGBA8888);
    QPainter p(&q);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient g(0, 0, w, h);
    g.setColorAt(0, b ? QColor(18, 92, 120) : QColor(178, 96, 28));
    g.setColorAt(1, b ? QColor(70, 190, 170) : QColor(242, 196, 92));
    p.fillRect(q.rect(), g);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 60));
    if (b) p.drawRect(QRectF(w * 0.58, h * 0.18, w * 0.3, h * 0.64));
    else p.drawEllipse(QPointF(w * 0.28, h * 0.5), h * 0.32, h * 0.32);
    QFont f;
    f.setPixelSize(h * 2 / 3);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255, 230));
    p.drawText(QRect(0, 0, w, h), Qt::AlignCenter, b ? "B" : "A");
    p.end();
    Image img(w, h);
    for (int y = 0; y < h; ++y) std::memcpy(img.row(y), q.constScanLine(y), static_cast<size_t>(w) * 4);
    return img;
}

constexpr int kThumbW = 240, kThumbH = 136, kAnimFrames = 30;

}  // namespace

QIcon TransitionsPanel::frameIcon(const QString& type, double progress) {
    static const Image a = samplePicture(false, kThumbW, kThumbH), b = samplePicture(true, kThumbW, kThumbH);
    static const std::unique_ptr<RenderBackend> backend = makeCpuBackend();
    TransitionRef t{type.toStdString(), Time(1)};
    const Image& out = renderTransitionPreview(t, progress, a, b, kThumbW, kThumbH, *backend);
    QImage q(out.pixels.data(), out.width, out.height, out.stride(), QImage::Format_RGBA8888);
    QPixmap pm = QPixmap::fromImage(q.copy());
    {
        QPainter p(&pm);   // rounded corners like the media tiles
        p.setRenderHint(QPainter::Antialiasing);
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        QPainterPath path;
        path.addRoundedRect(QRectF(0, 0, kThumbW, kThumbH), 10, 10);
        p.fillPath(path, Qt::black);
    }
    pm.setDevicePixelRatio(2.0);
    return QIcon(pm);
}

TransitionsPanel::TransitionsPanel(Document* doc, EditController* edit, QWidget* parent)
    : QWidget(parent), doc_(doc), edit_(edit) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    auto* title = new QLabel("BASIC", this);
    title->setObjectName("panelTitle");
    lay->addWidget(title);
    list_ = new TransitionList(this);
    list_->setObjectName("transitionList");
    list_->setViewMode(QListView::IconMode);
    list_->setIconSize(QSize(kThumbW / 2, kThumbH / 2));
    list_->setGridSize(QSize(kThumbW / 2 + 16, kThumbH / 2 + 32));
    list_->setMovement(QListView::Static);
    list_->setResizeMode(QListView::Adjust);
    list_->setDragEnabled(true);
    list_->setDragDropMode(QAbstractItemView::DragOnly);
    list_->setMouseTracking(true);
    list_->viewport()->installEventFilter(this);
    for (const auto& info : transitionTypes()) {
        const QString id = QString::fromUtf8(info.id);
        // thumbnail at the moment where the effect is most recognisable (the dips and the flash are a plain colour
        // mid-way)
        const double p = id.endsWith("dip_to_black") || id.endsWith("dip_to_white") ? 0.35
                         : id.endsWith("flash")                                        ? 0.3
                         : id.endsWith("zoom") || id.endsWith("zoom_in")               ? 0.45
                                                                                       : 0.5;
        auto* it = new QListWidgetItem(frameIcon(id, p), QString::fromUtf8(info.label));
        it->setData(Qt::UserRole, id);
        it->setData(Qt::UserRole + 1, p);
        it->setToolTip(QString::fromUtf8(info.description) +
                       "\n\nDrag it onto a cut between two clips, or double-click to add it after the selected clip "
                       "(or to change the selected transition).");
        list_->addItem(it);
    }
    lay->addWidget(list_, 1);
    auto* hint = new QLabel("Hover a transition to preview it. Drag it onto the cut between two clips on a video "
                            "track, or select a clip and double-click.", this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    lay->addWidget(hint);
    hoverTimer_ = new QTimer(this);
    hoverTimer_->setInterval(40);
    connect(hoverTimer_, &QTimer::timeout, this, &TransitionsPanel::animate);
    connect(list_, &QListWidget::itemActivated, this,
            [this](QListWidgetItem* it) { apply(it->data(Qt::UserRole).toString()); });
}

bool TransitionsPanel::apply(const QString& type) {
    const std::string t = type.toStdString();
    if (const std::string tid = doc_->selectedTransition(); !tid.empty()) {   // change the selected transition
        if (const Clip* c = edit::findClip(doc_->sequence(), tid); c && c->outTransition) {
            TransitionRef r = *c->outTransition;
            r.type = t;
            return edit_->updateTransitionSettings(tid, r);
        }
    }
    const std::string id = doc_->primarySelection();
    if (id.empty()) {
        emit edit_->message("Select the clip before the cut first");
        return false;
    }
    if (const Clip* c = edit::findClip(doc_->sequence(), id); c && c->outTransition) {   // replace, keep timing
        TransitionRef r = *c->outTransition;
        r.type = t;
        return edit_->updateTransitionSettings(id, r);
    }
    return edit_->addTransition(id, std::nullopt, TransitionAlignment::Centered, t);
}

bool TransitionsPanel::eventFilter(QObject* o, QEvent* e) {
    if (o == list_->viewport()) {
        auto restore = [this] {
            if (hoverItem_)
                hoverItem_->setIcon(frameIcon(hoverItem_->data(Qt::UserRole).toString(),
                                              hoverItem_->data(Qt::UserRole + 1).toDouble()));
        };
        if (e->type() == QEvent::MouseMove) {
            QListWidgetItem* it = list_->itemAt(static_cast<QMouseEvent*>(e)->position().toPoint());
            if (it != hoverItem_) {
                restore();
                hoverItem_ = it;
                hoverStep_ = 0;
                if (it) hoverTimer_->start();
                else hoverTimer_->stop();
            }
        } else if (e->type() == QEvent::Leave) {
            hoverTimer_->stop();
            restore();
            hoverItem_ = nullptr;
        }
    }
    return QWidget::eventFilter(o, e);
}

void TransitionsPanel::animate() {
    if (!hoverItem_) return;
    const QString id = hoverItem_->data(Qt::UserRole).toString();
    auto& frames = frames_[id];
    if (frames.empty())
        for (int i = 0; i <= kAnimFrames; ++i) frames.push_back(frameIcon(id, double(i) / kAnimFrames));
    // play forward, hold the end for a moment, restart
    const int cycle = kAnimFrames + 12;
    const int step = hoverStep_++ % cycle;
    hoverItem_->setIcon(frames[std::min(step, kAnimFrames)]);
}

}  // namespace fa::ui
