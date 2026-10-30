#include "panels.h"

#include "edit_controller.h"
#include "theme.h"
#include "timeline_view.h"
#include "title_library.h"

#include "fa/render/renderer.h"
#include "fa/render/text_animation.h"
#include "fa/text/title_engine.h"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVBoxLayout>

#include <cstring>

namespace fa::ui {

namespace {

constexpr int kThumbW = 320, kThumbH = 180;   // drawn at half size (device pixel ratio 2)
constexpr int kFps = 25;
constexpr int kRoleId = Qt::UserRole;
constexpr int kRoleDuration = Qt::UserRole + 1;   // seconds

// Original artwork for the thumbnails: an evening-sky gradient with a horizon and soft shapes.
Image thumbBackground(int w, int h) {
    QImage q(w, h, QImage::Format_RGBA8888);
    QPainter p(&q);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient g(0, 0, 0, h);
    g.setColorAt(0, QColor(36, 52, 84));
    g.setColorAt(0.62, QColor(118, 92, 120));
    g.setColorAt(1, QColor(52, 46, 62));
    p.fillRect(q.rect(), g);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 214, 160, 70));
    p.drawEllipse(QPointF(w * 0.72, h * 0.58), h * 0.16, h * 0.16);
    p.setBrush(QColor(20, 22, 32, 170));
    QPainterPath hills;
    hills.moveTo(0, h * 0.70);
    hills.cubicTo(w * 0.25, h * 0.58, w * 0.45, h * 0.78, w * 0.7, h * 0.66);
    hills.cubicTo(w * 0.85, h * 0.6, w * 0.95, h * 0.68, w, h * 0.66);
    hills.lineTo(w, h);
    hills.lineTo(0, h);
    hills.closeSubpath();
    p.drawPath(hills);
    p.end();
    Image img(w, h);
    for (int y = 0; y < h; ++y) std::memcpy(img.row(y), q.constScanLine(y), static_cast<size_t>(w) * 4);
    return img;
}

// Moment shown in the still thumbnail: after the In animation, before the Out animation.
double stillTime(const text::TitleTemplate& t) {
    const double d = t.duration.toDouble();
    if (t.prototype.title && t.prototype.title->loop.preset == "roll_up") return d * 0.45;
    return d * 0.5;
}

}  // namespace

struct TitlesPanel::Engines {
    text::TitleEngine titles;
    std::unique_ptr<RenderBackend> backend = makeCpuBackend();
    Image background = thumbBackground(kThumbW, kThumbH);
};

QStringList TitleList::mimeTypes() const { return {kMimeTitle}; }

QMimeData* TitleList::mimeData(const QList<QListWidgetItem*>& items) const {
    if (items.isEmpty()) return nullptr;
    auto* m = new QMimeData();
    m->setData(kMimeTitle, items.front()->data(kRoleId).toString().toUtf8());
    return m;
}

TitlesPanel::TitlesPanel(Document* doc, EditController* edit, TitleLibrary* library, QWidget* parent)
    : QWidget(parent), doc_(doc), edit_(edit), library_(library), engines_(std::make_unique<Engines>()) {
    text::ensureGuiApplication();
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(8);
    search_ = new QLineEdit(this);
    search_->setObjectName("titleSearch");
    search_->setPlaceholderText("Search titles");
    search_->setClearButtonEnabled(true);
    lay->addWidget(search_);
    auto* body = new QHBoxLayout();
    body->setSpacing(8);
    categories_ = new QListWidget(this);
    categories_->setObjectName("titleCategories");
    categories_->setFixedWidth(150);
    categories_->setStyleSheet(QString("QListWidget#titleCategories { background: transparent; }"
                                       "QListWidget#titleCategories::item { padding: 6px 8px; }"));
    categories_->addItem("All Titles");
    for (const auto& c : text::titleCategories()) categories_->addItem(QString::fromStdString(c));
    categories_->setCurrentRow(0);
    body->addWidget(categories_);
    auto* right = new QVBoxLayout();
    right->setSpacing(6);
    heading_ = new QLabel("ALL TITLES", this);
    heading_->setObjectName("panelTitle");
    right->addWidget(heading_);
    list_ = new TitleList(this);
    list_->setObjectName("titleList");
    list_->setViewMode(QListView::IconMode);
    list_->setIconSize(QSize(kThumbW / 2, kThumbH / 2));
    list_->setGridSize(QSize(kThumbW / 2 + 16, kThumbH / 2 + 32));
    list_->setMovement(QListView::Static);
    list_->setResizeMode(QListView::Adjust);
    list_->setWordWrap(false);
    list_->setTextElideMode(Qt::ElideRight);
    list_->setDragEnabled(true);
    list_->setDragDropMode(QAbstractItemView::DragOnly);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setMouseTracking(true);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setAccessibleName("Titles");
    list_->viewport()->installEventFilter(this);
    right->addWidget(list_, 1);
    auto* hint = new QLabel("Hover a title to preview it. Drag it onto the timeline, or double-click to add it at the "
                            "playhead. Save your own styles from the text inspector (My Titles).", this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    right->addWidget(hint);
    body->addLayout(right, 1);
    lay->addLayout(body, 1);

    hoverTimer_ = new QTimer(this);
    hoverTimer_->setInterval(1000 / kFps);
    connect(hoverTimer_, &QTimer::timeout, this, &TitlesPanel::animate);
    connect(search_, &QLineEdit::textChanged, this, &TitlesPanel::refresh);
    connect(categories_, &QListWidget::currentRowChanged, this, &TitlesPanel::refresh);
    connect(library_, &TitleLibrary::changed, this, &TitlesPanel::refresh);
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) { apply(it->data(kRoleId).toString()); });
    connect(list_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* it = list_->itemAt(pos);
        if (!it) return;
        const QString id = it->data(kRoleId).toString();
        QMenu m(this);
        m.addAction(icon("plus"), "Add to Timeline at Playhead", [this, id] { apply(id); });
        if (id.startsWith("user.")) {
            m.addSeparator();
            m.addAction("Rename Preset...", [this, id, it] {
                bool ok = false;
                const QString name = QInputDialog::getText(this, "Rename preset", "Name", QLineEdit::Normal, it->text(), &ok);
                if (ok && !library_->renameUserPreset(id.toStdString(), name)) emit edit_->message(library_->lastError());
            });
            m.addAction(icon("delete"), "Delete Preset", [this, id] {
                if (!library_->removeUserPreset(id.toStdString())) emit edit_->message(library_->lastError());
            });
        }
        m.exec(list_->viewport()->mapToGlobal(pos));
    });
    refresh();
}

TitlesPanel::~TitlesPanel() = default;

QIcon TitlesPanel::frameIcon(const QString& templateId, double seconds) {
    const text::TitleTemplate* t = library_->find(templateId.toStdString());
    if (!t) return {};
    const Vec2 base = t->caption ? Vec2{0.0, 0.92 - 0.5} : Vec2{};
    const Time local = Rational::fromDouble(seconds, kFps);
    const Image& out = renderTitlePreview(t->prototype, local, t->duration, engines_->background, kThumbW, kThumbH,
                                          *engines_->backend, engines_->titles, base);
    QImage q(out.pixels.data(), out.width, out.height, out.stride(), QImage::Format_RGBA8888);
    QPixmap pm = QPixmap::fromImage(q.copy());
    {
        QPainter p(&pm);   // rounded corners like the other browsers
        p.setRenderHint(QPainter::Antialiasing);
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        QPainterPath path;
        path.addRoundedRect(QRectF(0, 0, kThumbW, kThumbH), 10, 10);
        p.fillPath(path, Qt::black);
    }
    pm.setDevicePixelRatio(2.0);
    return QIcon(pm);
}

void TitlesPanel::refresh() {
    hoverTimer_->stop();
    hoverItem_ = nullptr;
    const int row = categories_->currentRow();
    const QString category = row <= 0 ? QString() : categories_->item(row)->text().section(" (", 0, 0);
    heading_->setText(category.isEmpty() ? "ALL TITLES" : category.toUpper());
    const QString filter = search_->text().trimmed();
    const auto all = library_->all();
    // counts per category
    std::map<std::string, int> counts;
    for (const auto* t : all) ++counts[t->category];
    categories_->item(0)->setText(QString("All Titles (%1)").arg(all.size()));
    for (int i = 1; i < categories_->count(); ++i) {
        const std::string c = categories_->item(i)->text().section(" (", 0, 0).toStdString();
        categories_->item(i)->setText(QString("%1 (%2)").arg(QString::fromStdString(c)).arg(counts[c]));
    }
    list_->clear();
    for (const auto* t : all) {
        const QString name = QString::fromStdString(t->name);
        if (!category.isEmpty() && QString::fromStdString(t->category) != category) continue;
        if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive) &&
            !QString::fromStdString(t->description).contains(filter, Qt::CaseInsensitive) &&
            !QString::fromStdString(t->category).contains(filter, Qt::CaseInsensitive))
            continue;
        const QString id = QString::fromStdString(t->id);
        auto* it = new QListWidgetItem(frameIcon(id, stillTime(*t)), name);
        it->setData(kRoleId, id);
        it->setData(kRoleDuration, t->duration.toDouble());
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        QString tip = QString::fromStdString(t->description);
        auto anim = [](const char* slot, TextAnimStage stage, const TextAnimation& a) {
            if (!a.active()) return QString();
            const TextAnimPreset* p = findTextAnimPreset(stage, a.preset);
            return QString("\n%1: %2").arg(slot).arg(p ? QString::fromUtf8(p->label) : QString::fromStdString(a.preset));
        };
        const TitleData& td = *t->prototype.title;
        tip += anim("In", TextAnimStage::In, td.in) + anim("Loop", TextAnimStage::Loop, td.loop) +
               anim("Out", TextAnimStage::Out, td.out);
        tip += t->caption ? "\n\nAdds a caption on a subtitle track." : "\n\nAdds a title clip above the footage.";
        tip += "\nDrag it onto the timeline, or double-click to add it at the playhead.";
        it->setToolTip(tip);
        list_->addItem(it);
    }
}

bool TitlesPanel::apply(const QString& templateId) { return edit_->addTitleFromTemplate(templateId.toStdString()); }

bool TitlesPanel::eventFilter(QObject* o, QEvent* e) {
    if (o == list_->viewport()) {
        auto restore = [this] {
            if (!hoverItem_) return;
            if (const auto* t = library_->find(hoverItem_->data(kRoleId).toString().toStdString()))
                hoverItem_->setIcon(frameIcon(hoverItem_->data(kRoleId).toString(), stillTime(*t)));
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

void TitlesPanel::animate() {
    if (!hoverItem_) return;
    // play the whole clip, hold the last frame for half a second, restart
    const int frames = std::max(1, static_cast<int>(hoverItem_->data(kRoleDuration).toDouble() * kFps));
    const int step = hoverStep_++ % (frames + kFps / 2);
    hoverItem_->setIcon(frameIcon(hoverItem_->data(kRoleId).toString(), double(std::min(step, frames - 1)) / kFps));
}

}  // namespace fa::ui
