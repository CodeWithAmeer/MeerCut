#include "effects_browser.h"

#include "edit_controller.h"
#include "effect_presets.h"
#include "lut_library.h"
#include "theme.h"

#include "fa/model/edit_ops.h"
#include "fa/model/serialize.h"
#include "fa/render/effects.h"
#include "fa/render/lut.h"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>

namespace fa::ui {

namespace {

constexpr int kThumbW = 240, kThumbH = 136;   // drawn at half size (device pixel ratio 2)
constexpr int kFrames = 24;
constexpr int kRoleKey = Qt::UserRole;
constexpr int kRolePayload = Qt::UserRole + 1;

QByteArray payloadOf(const EffectRef& e, const QString& name) {
    Clip holder;   // the clip JSON format carries effects (same as copy / paste of effects)
    holder.adjustment = true;
    holder.name = name.toStdString();   // M10: the name of the layer it becomes on the timeline
    holder.effects = {e};
    return QByteArray::fromStdString(clipToJsonString(holder, -1));
}

const Image& samplePicture() {
    static const Image img = [] {
        const QImage q = LutLibrary::samplePicture(kThumbW, kThumbH);
        Image im(kThumbW, kThumbH);
        for (int y = 0; y < kThumbH; ++y) std::memcpy(im.row(y), q.constScanLine(y), static_cast<size_t>(kThumbW) * 4);
        return im;
    }();
    return img;
}

// Thumbnails are tiny (sizes are px at 1080 lines): they show each effect with stronger settings than its defaults,
// so the tile reads at a glance. What is dragged / applied keeps the real defaults.
const std::map<std::string, std::map<std::string, double>>& showcase() {
    static const std::map<std::string, std::map<std::string, double>> m = {
        {"fa.fx.vignette", {{"amount", -90}}},
        {"fa.fx.sharpen", {{"amount", 300}, {"radius", 8}}},
        {"fa.fx.blur.gaussian", {{"radius", 120}}},
        {"fa.fx.blur.directional", {{"length", 200}}},
        {"fa.fx.glow", {{"threshold", 35}, {"radius", 160}, {"intensity", 220}}},
        {"fa.fx.pixelate", {{"size", 90}}},
        {"fa.fx.grain", {{"amount", 100}, {"size", 6}}},
        {"fa.fx.rgbsplit", {{"offset", 70}}},
        {"fa.fx.chromatic", {{"amount", 100}}},
        {"fa.fx.wave", {{"amplitude", 60}, {"wavelength", 500}}},
        {"fa.fx.bulge", {{"amount", 100}, {"radius", 60}}},
        {"fa.fx.chromakey", {{"color", double(0x5E8C31)}, {"tolerance", 40}}},   // the sample's grass
    };
    return m;
}

QString settingsKey(EffectsBrowser::Page p) { return p == EffectsBrowser::Page::Effects ? "effects/favourites" : "filters/favourites"; }

}  // namespace

QStringList EffectList::mimeTypes() const { return {kMimeEffect, kMimeAdjustment}; }

QMimeData* EffectList::mimeData(const QList<QListWidgetItem*>& items) const {
    if (items.isEmpty()) return nullptr;
    auto* m = new QMimeData();
    if (items.front()->data(kRoleKey).toString() == "adjustment") m->setData(kMimeAdjustment, "5");   // seconds
    else m->setData(kMimeEffect, items.front()->data(kRolePayload).toByteArray());
    m->setText(items.front()->text());
    return m;
}

EffectsBrowser::EffectsBrowser(Page page, Document* doc, EditController* edit, EffectPresetLibrary* presets, QWidget* parent)
    : QWidget(parent), page_(page), doc_(doc), edit_(edit), presets_(presets) {
    const QString what = page == Page::Effects ? "effects" : "filters";
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(8);
    search_ = new QLineEdit(this);
    search_->setObjectName(what + "Search");
    search_->setPlaceholderText("Search " + what);
    search_->setClearButtonEnabled(true);
    lay->addWidget(search_);
    auto* body = new QHBoxLayout();
    body->setSpacing(8);
    categories_ = new QListWidget(this);
    categories_->setObjectName(what + "Categories");
    categories_->setFixedWidth(150);
    categories_->setStyleSheet(QString("QListWidget { background: transparent; } QListWidget::item { padding: 6px 8px; }"));
    body->addWidget(categories_);
    auto* right = new QVBoxLayout();
    right->setSpacing(6);
    heading_ = new QLabel(this);
    heading_->setObjectName("panelTitle");
    right->addWidget(heading_);
    list_ = new EffectList(this);
    list_->setObjectName(what + "List");
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
    list_->viewport()->installEventFilter(this);
    right->addWidget(list_, 1);
    auto* hint = new QLabel(QString("Rest the mouse on a tile to preview it. Drag it onto the timeline: the %1 goes above "
                                    "your video like text and changes everything below it for as long as it lasts. Hold "
                                    "Ctrl while dropping onto a clip to apply it to that clip only. Double-click: the "
                                    "selected clips, or the playhead. Right-click for favourites.")
                                .arg(page == Page::Effects ? "effect" : "filter"),
                            this);
    hint->setObjectName("effectsHint");
    hint->setWordWrap(true);
    hint->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    right->addWidget(hint);
    body->addLayout(right, 1);
    lay->addLayout(body, 1);
    favourites_ = QSettings().value(settingsKey(page_)).toStringList();

    hoverTimer_ = new QTimer(this);
    hoverTimer_->setInterval(60);
    connect(hoverTimer_, &QTimer::timeout, this, &EffectsBrowser::animate);
    connect(search_, &QLineEdit::textChanged, this, &EffectsBrowser::refresh);
    connect(categories_, &QListWidget::currentRowChanged, this, [this] { refresh(); });
    if (presets_) connect(presets_, &EffectPresetLibrary::changed, this, [this] {
            frames_.clear();
            refresh();
        });
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) { apply(it->data(kRoleKey).toString()); });
    connect(list_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* it = list_->itemAt(pos);
        if (!it) return;
        const QString key = it->data(kRoleKey).toString();
        QMenu m(this);
        if (key != "adjustment") {   // M10
            const EffectDef* d = findEffectDef(effectOf(key).type);
            if (!d || d->category != "Keying")   // a key needs the clip it keys
                m.addAction(icon("plus"), "Add to Timeline (above the video)", [this, key] { addAsLayer(key); });
            m.addAction(icon("effects"), "Apply to Selected Clips", [this, key] { apply(key); });
        } else {
            m.addAction(icon("plus"), "Add to Timeline", [this, key] { apply(key); });
        }
        const bool fav = isFavourite(key);
        m.addAction(fav ? "Remove from Favourites" : "Add to Favourites", [this, key, fav] { setFavourite(key, !fav); });
        if (key.startsWith("preset:") && presets_) {
            const std::string id = key.mid(7).toStdString();
            m.addSeparator();
            m.addAction("Rename Preset...", [this, id, it] {
                bool ok = false;
                const QString name = QInputDialog::getText(this, "Rename preset", "Name", QLineEdit::Normal, it->text(), &ok);
                if (ok && !presets_->rename(id, name)) emit edit_->message(presets_->lastError());
            });
            m.addAction(icon("delete"), "Delete Preset", [this, id] { presets_->remove(id); });
        }
        m.exec(list_->viewport()->mapToGlobal(pos));
    });
    refresh();
}

std::vector<EffectsBrowser::Entry> EffectsBrowser::entries() const {
    std::vector<Entry> v;
    const EffectSection want = page_ == Page::Effects ? EffectSection::Effects : EffectSection::Filters;
    for (const auto& d : effectDefs()) {
        if (d.section != want) continue;
        v.push_back({QString::fromStdString(d.id), QString::fromStdString(d.label), QString::fromStdString(d.category),
                     QString::fromStdString(d.description), makeEffect(d, "fx1"), d.timeVarying});
    }
    if (page_ == Page::Effects)   // not an effect but where effects for several clips live
        v.push_back({"adjustment", "Adjustment Layer", "Utility",
                     "A layer whose effects apply to every track below it, within its time range and masks. Drag it "
                     "onto the timeline, or double-click to add it at the playhead.",
                     EffectRef{}, false});
    if (page_ == Page::Filters)
        for (const auto& look : builtinLooks()) {
            EffectRef e = makeEffect(*findEffectDef("fa.filter.look"), "fx1");
            e.strings["lut"] = std::string(kBuiltinLutPrefix) + look.id;
            v.push_back({"look:" + QString::fromStdString(look.id), QString::fromStdString(look.name),
                         QString::fromStdString(look.category), QString::fromStdString(look.description), e, false});
        }
    if (presets_)
        for (const auto& p : presets_->presets()) {
            const EffectDef* d = findEffectDef(p.effect.type);
            if (!d) continue;
            const bool filter = d->section == EffectSection::Filters || p.effect.type == "fa.filter.look";
            if ((page_ == Page::Filters) != filter || d->section == EffectSection::Color) continue;
            v.push_back({"preset:" + QString::fromStdString(p.id), QString::fromStdString(p.name), "My Presets",
                         QString::fromStdString(d->label + " preset"), p.effect, d->timeVarying});
        }
    return v;
}

void EffectsBrowser::refresh() {
    const std::vector<Entry> all = entries();
    // categories: All, Favourites, each effect category (registry order; on the Filters page the look groups' order
    // first), My Presets
    QStringList cats = {page_ == Page::Effects ? "All Effects" : "All Filters", "Favourites"};
    if (page_ == Page::Filters)
        for (const auto& c : builtinLookCategories()) {
            const QString q = QString::fromStdString(c);
            if (std::any_of(all.begin(), all.end(), [&](const Entry& e) { return e.category == q; })) cats << q;
        }
    for (const auto& e : all)
        if (!cats.contains(e.category)) cats << e.category;
    QStringList current;
    for (int i = 0; i < categories_->count(); ++i) current << categories_->item(i)->text();
    const QString selected = categories_->currentItem() ? categories_->currentItem()->text() : cats.front();
    if (current != cats) {
        QSignalBlocker b(categories_);
        categories_->clear();
        categories_->addItems(cats);
        const auto found = categories_->findItems(selected, Qt::MatchExactly);
        categories_->setCurrentRow(found.isEmpty() ? 0 : categories_->row(found.front()));
    }
    const QString cat = categories_->currentItem() ? categories_->currentItem()->text() : cats.front();
    const QString q = search_->text().trimmed();
    heading_->setText(cat.toUpper());
    hoverTimer_->stop();
    hoverItem_ = nullptr;
    list_->clear();
    for (const auto& e : all) {
        const bool inCat = cat == cats.front() || (cat == "Favourites" ? isFavourite(e.key) : e.category == cat);
        const bool match = q.isEmpty() || e.name.contains(q, Qt::CaseInsensitive) || e.category.contains(q, Qt::CaseInsensitive) ||
                           e.description.contains(q, Qt::CaseInsensitive);
        if (!inCat || !match) continue;
        auto* it = new QListWidgetItem(thumb(e, 1.0), (isFavourite(e.key) ? QString::fromUtf8("★ ") : QString()) + e.name);
        it->setData(kRoleKey, e.key);
        it->setData(kRolePayload, payloadOf(e.effect, e.name));
        it->setToolTip(e.key == "adjustment" ? e.description
                                             : e.description + "\n\nDrag onto the timeline: it goes above your video, like text, "
                                                               "and affects everything below it. Hold Ctrl while dropping onto a clip "
                                                               "to apply it to that clip only.");
        list_->addItem(it);
    }
}

QListWidgetItem* EffectsBrowser::itemFor(const QString& key) const {
    for (int i = 0; i < list_->count(); ++i)
        if (list_->item(i)->data(kRoleKey).toString() == key) return list_->item(i);
    return nullptr;
}

EffectRef EffectsBrowser::effectOf(const QString& key) const {
    for (const auto& e : entries())
        if (e.key == key) return e.effect;
    return {};
}

bool EffectsBrowser::apply(const QString& key) {
    if (key == "adjustment") return edit_->addAdjustmentLayer();
    const EffectRef e = effectOf(key);
    if (e.type.empty()) return false;
    std::vector<std::string> clips;
    for (const auto& id : doc_->selection()) {
        const Track* tr = nullptr;
        if (edit::findClip(doc_->sequence(), id, &tr) && tr->kind == TrackKind::Video) clips.push_back(id);
    }
    if (clips.empty()) {   // M10: nothing selected - like text, the effect goes on the timeline at the playhead
        if (const EffectDef* d = findEffectDef(e.type); d && d->category == "Keying") {
            emit edit_->message("Select the clip to key first (or drag the effect onto it)");
            return false;
        }
        return addAsLayer(key);
    }
    return edit_->addEffectToClips(clips, e);
}

bool EffectsBrowser::addAsLayer(const QString& key, std::optional<Time> at) {
    for (const auto& entry : entries()) {
        if (entry.key != key || entry.effect.type.empty()) continue;
        if (const EffectDef* d = findEffectDef(entry.effect.type); d && d->category == "Keying") {
            emit edit_->message("A key works on its own clip: drag it onto the clip to key");
            return false;
        }
        return !edit_->addEffectLayer(entry.effect, entry.name.toStdString(), at).empty();
    }
    return false;
}

bool EffectsBrowser::isFavourite(const QString& key) const { return favourites_.contains(key); }

void EffectsBrowser::setFavourite(const QString& key, bool on) {
    favourites_.removeAll(key);
    if (on) favourites_ << key;
    QSettings().setValue(settingsKey(page_), favourites_);
    refresh();
}

void EffectsBrowser::selectCategory(const QString& name) {
    const auto found = categories_->findItems(name, Qt::MatchExactly);
    if (!found.isEmpty()) categories_->setCurrentItem(found.front());
}

QIcon EffectsBrowser::thumb(const Entry& entry, double phase) {
    const bool still = phase >= 1.0;
    if (still)
        if (const auto it = stills_.find(entry.key); it != stills_.end()) return it->second;
    if (entry.key == "adjustment") {   // the sample with an adjustment band over its middle
        QPixmap pm = QPixmap::fromImage(LutLibrary::samplePicture(kThumbW, kThumbH));
        QPainter p(&pm);
        p.fillRect(QRect(0, kThumbH / 3, kThumbW, kThumbH / 3), QColor(64, 128, 112, 170));
        icon("adjustment", Qt::white).paint(&p, QRect(kThumbW / 2 - 18, kThumbH / 2 - 18, 36, 36));
        p.end();
        pm.setDevicePixelRatio(2.0);
        return QIcon(pm);
    }
    EffectRef e = entry.effect;
    if (!entry.key.startsWith("preset:"))
        if (const auto it = showcase().find(e.type); it != showcase().end())
            for (const auto& [k, v] : it->second) e.params[k] = Animatable<double>(v);
    double seconds = 0.5;
    if (entry.animated) seconds = phase * 2.0;   // a 2-second loop of the effect's own motion
    else e.mix = Animatable<double>(std::clamp(phase, 0.0, 1.0));   // the effect fades in over the picture
    const auto inst = evaluateEffects({e}, Time(0), seconds);
    Image out;
    applyEffectsCpu(samplePicture(), inst, out);
    QImage q(out.pixels.data(), out.width, out.height, out.stride(), QImage::Format_RGBA8888);
    QPixmap pm(kThumbW, kThumbH);
    {
        QPainter p(&pm);   // transparency (keying) shows as a checkerboard
        for (int y = 0; y < kThumbH; y += 12)
            for (int x = 0; x < kThumbW; x += 12) p.fillRect(x, y, 12, 12, ((x + y) / 12) % 2 ? QColor(70, 70, 76) : QColor(110, 110, 118));
        p.drawImage(0, 0, q);
        p.setRenderHint(QPainter::Antialiasing);
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        QPainterPath path;
        path.addRoundedRect(QRectF(0, 0, kThumbW, kThumbH), 10, 10);
        p.fillPath(path, Qt::black);
    }
    pm.setDevicePixelRatio(2.0);
    QIcon icon(pm);
    if (still) stills_[entry.key] = icon;
    return icon;
}

bool EffectsBrowser::eventFilter(QObject* o, QEvent* e) {
    if (o == list_->viewport()) {
        auto restore = [this] {
            if (!hoverItem_) return;
            for (const auto& en : entries())
                if (en.key == hoverItem_->data(kRoleKey).toString()) hoverItem_->setIcon(thumb(en, 1.0));
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

void EffectsBrowser::animate() {
    if (!hoverItem_) return;
    const QString key = hoverItem_->data(kRoleKey).toString();
    auto& frames = frames_[key];
    if (frames.empty()) {
        for (const auto& en : entries())
            if (en.key == key)
                for (int i = 0; i <= kFrames; ++i) frames.push_back(thumb(en, double(i) / kFrames));
        if (frames.empty()) return;
    }
    // play the frames, hold the end for a moment, restart
    const int cycle = static_cast<int>(frames.size()) + 10;
    const int step = hoverStep_++ % cycle;
    hoverItem_->setIcon(frames[static_cast<size_t>(std::min(step, static_cast<int>(frames.size()) - 1))]);
}

}  // namespace fa::ui
