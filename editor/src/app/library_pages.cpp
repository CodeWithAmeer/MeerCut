#include "library_pages.h"

#include "edit_controller.h"
#include "templates.h"
#include "theme.h"

#include "fa/model/edit_ops.h"
#include "fa/playback/audio_player.h"
#include "fa/render/backend.h"
#include "fa/render/renderer.h"
#include "fa/text/stickers.h"
#include "fa/text/title_engine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

namespace fa::ui {

namespace {
constexpr int kRoleId = Qt::UserRole + 1;
constexpr int kRoleKey = Qt::UserRole + 2;
}  // namespace

// ---- files ----------------------------------------------------------------------------------------------------

QString assetsDir() {
    const QString env = qEnvironmentVariable("MEERCUT_ASSETS");
    if (!env.isEmpty()) return QDir::cleanPath(env);
    return QDir(QCoreApplication::applicationDirPath()).filePath("assets");
}

QString userLibraryDir() {
    const QString env = qEnvironmentVariable("MEERCUT_LIBRARY_DIR");
    if (!env.isEmpty()) return QDir::cleanPath(env);
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/MeerCut/library";
}

QString materializeAsset(const QString& relativePath) {
    const QString from = QDir(assetsDir()).filePath(relativePath);
    const QString to = QDir(userLibraryDir()).filePath(relativePath);
    const QFileInfo src(from), dst(to);
    if (!src.exists()) return {};
    if (dst.exists() && dst.size() == src.size()) return QDir::cleanPath(dst.absoluteFilePath());
    QDir().mkpath(dst.absolutePath());
    QFile::remove(to);
    if (!QFile::copy(from, to)) return {};
    return QDir::cleanPath(QFileInfo(to).absoluteFilePath());
}

std::vector<AudioItem> loadAudioLibrary(QString* error) {
    std::vector<AudioItem> out;
    QFile f(QDir(assetsDir()).filePath("audio/library.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "The audio library is missing (" + f.fileName() + ")";
        return out;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue& v : root.value("items").toArray()) {
        const QJsonObject o = v.toObject();
        AudioItem it;
        it.id = o.value("id").toString();
        it.title = o.value("title").toString();
        it.kind = o.value("kind").toString();
        it.category = o.value("category").toString();
        it.file = o.value("file").toString();
        it.seconds = o.value("seconds").toDouble();
        it.bpm = o.value("bpm").toInt();
        if (!it.id.isEmpty() && !it.file.isEmpty()) out.push_back(it);
    }
    return out;
}

std::vector<SampleVideo> loadSampleVideos(QString* error) {
    std::vector<SampleVideo> out;
    QFile f(QDir(assetsDir()).filePath("samples/samples.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "The sample videos are missing (" + QDir::toNativeSeparators(f.fileName()) + ")";
        return out;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue& v : root.value("items").toArray()) {
        const QJsonObject o = v.toObject();
        SampleVideo s;
        s.id = o.value("id").toString();
        s.title = o.value("title").toString();
        s.file = o.value("file").toString();
        s.description = o.value("description").toString();
        s.credit = o.value("credit").toString();
        s.licence = o.value("licence").toString();
        s.licenceUrl = o.value("licenceUrl").toString();
        s.seconds = o.value("seconds").toDouble();
        s.poster = o.value("poster").toDouble();
        s.width = o.value("width").toInt();
        s.height = o.value("height").toInt();
        s.fps = o.value("fps").toDouble();
        if (!s.id.isEmpty() && !s.file.isEmpty()) out.push_back(s);
    }
    return out;
}

QStringList libraryFavorites(const QString& page) { return QSettings().value("library/favorites/" + page).toStringList(); }

void setLibraryFavorite(const QString& page, const QString& id, bool on) {
    QStringList f = libraryFavorites(page);
    f.removeAll(id);
    if (on) f << id;
    QSettings().setValue("library/favorites/" + page, f);
}

// ---- shared page layout ---------------------------------------------------------------------------------------

LibraryPageBase::LibraryPageBase(const QString& page, const QString& searchHint, QWidget* parent) : QWidget(parent), page_(page) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(8);
    search_ = new QLineEdit(this);
    search_->setObjectName(page + "Search");
    search_->setPlaceholderText(searchHint);
    search_->setClearButtonEnabled(true);
    lay->addWidget(search_);
    auto* body = new QHBoxLayout();
    body->setSpacing(8);
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(page + "Categories");
    tree_->setHeaderHidden(true);
    tree_->setFixedWidth(170);
    tree_->setIndentation(12);
    tree_->setRootIsDecorated(true);
    tree_->setStyleSheet(QString("QTreeWidget { background: transparent; border: none; }"
                                 "QTreeWidget::item { padding: 5px 4px; }"
                                 "QTreeWidget::item:selected { background: %1; color: %2; border-radius: 4px; }")
                             .arg(color::panelHeader.name(), color::accent.name()));
    body->addWidget(tree_);
    auto* right = right_ = new QVBoxLayout();
    right->setSpacing(6);
    heading_ = new QLabel(this);
    heading_->setObjectName("panelTitle");
    right->addWidget(heading_);
    list_ = new LibraryList(this);
    list_->setObjectName(page + "List");
    list_->setViewMode(QListView::IconMode);
    list_->setMovement(QListView::Static);
    list_->setResizeMode(QListView::Adjust);
    list_->setWordWrap(false);
    list_->setTextElideMode(Qt::ElideRight);
    list_->setDragEnabled(true);
    list_->setDragDropMode(QAbstractItemView::DragOnly);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setMouseTracking(true);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    right->addWidget(list_, 1);
    hint_ = new QLabel(this);
    hint_->setWordWrap(true);
    hint_->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    right->addWidget(hint_);
    body->addLayout(right, 1);
    lay->addLayout(body, 1);
    connect(search_, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* cur) {
        if (cur && !cur->data(0, kRoleKey).isValid() && cur->childCount() > 0) {   // a group: its first category
            tree_->setCurrentItem(cur->child(0));
            return;
        }
        heading_->setText(cur ? cur->text(0).toUpper() : QString());
        refresh();
    });
}

QTreeWidgetItem* LibraryPageBase::addGroup(const QString& text) {
    auto* g = new QTreeWidgetItem(tree_, {text});
    QFont f = g->font(0);
    f.setBold(true);
    g->setFont(0, f);
    g->setExpanded(true);
    return g;
}

QTreeWidgetItem* LibraryPageBase::addCategory(QTreeWidgetItem* group, const QString& text, const QString& key) {
    auto* c = new QTreeWidgetItem(group, {text});
    c->setData(0, kRoleKey, key);
    return c;
}

QString LibraryPageBase::currentCategory() const {
    const QTreeWidgetItem* it = tree_->currentItem();
    return it ? it->data(0, kRoleKey).toString() : QString("all");
}

void LibraryPageBase::selectCategory(const QString& key) {
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        if ((*it)->data(0, kRoleKey).toString() == key) {
            tree_->setCurrentItem(*it);
            return;
        }
}

bool LibraryPageBase::favorite(const QString& id) const { return libraryFavorites(page_).contains(id); }

void LibraryPageBase::toggleFavorite(const QString& id) {
    setLibraryFavorite(page_, id, !favorite(id));
    refresh();
}

// ---- Audio ----------------------------------------------------------------------------------------------------

namespace {

QString duration(double s) {
    const int t = static_cast<int>(std::lround(s));
    return QString("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QChar('0'));
}

QIcon audioCard(const AudioItem& it, bool playing, bool fav) {
    const int w = 152, h = 86;
    QPixmap pm(w * 2, h * 2);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const bool music = it.kind == "music";
    const uint hue = qHash(it.category) % 360;
    QLinearGradient g(0, 0, w, h);
    g.setColorAt(0, QColor::fromHsv(static_cast<int>(hue), music ? 110 : 70, 120));
    g.setColorAt(1, QColor::fromHsv(static_cast<int>((hue + 40) % 360), music ? 140 : 90, 60));
    QPainterPath card;
    card.addRoundedRect(QRectF(0.5, 0.5, w - 1, h - 1), 7, 7);
    p.fillPath(card, g);
    p.setPen(QPen(QColor(255, 255, 255, 60), 2));   // a stylised waveform (stable per item)
    for (int x = 10; x < w - 10; x += 4) {
        const double a = 0.25 + 0.75 * std::abs(std::sin(x * 0.21 + qHash(it.id) % 17) * std::sin(x * 0.05 + 1.3));
        p.drawLine(QPointF(x, h / 2.0 - a * h * 0.28), QPointF(x, h / 2.0 + a * h * 0.28));
    }
    icon(playing ? "pause" : "play", Qt::white).paint(&p, QRect(w / 2 - 15, h / 2 - 15, 30, 30));
    p.setPen(Qt::white);
    QFont f = p.font();
    f.setPixelSize(11);
    f.setBold(true);
    p.setFont(f);
    p.drawText(QRectF(6, h - 20, w - 12, 16), Qt::AlignRight | Qt::AlignVCenter, duration(it.seconds));
    p.drawText(QRectF(8, 4, w - 16, 16), Qt::AlignLeft | Qt::AlignVCenter, it.category + (it.bpm ? QString("  %1 BPM").arg(it.bpm) : ""));
    if (fav) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 200, 60));
        QPainterPath star;
        for (int i = 0; i < 10; ++i) {
            const double r = i % 2 ? 3.5 : 8, a = -M_PI / 2 + i * M_PI / 5;
            const QPointF q(w - 14 + r * std::cos(a), 30 + r * std::sin(a));
            if (!i) star.moveTo(q);
            else star.lineTo(q);
        }
        p.drawPath(star);
    }
    return QIcon(pm);
}

}  // namespace

AudioPage::AudioPage(Document* doc, EditController* edit, bool silentPreview, QWidget* parent)
    : LibraryPageBase("audio", "Search music and sound effects", parent), doc_(doc), edit_(edit),
      player_(std::make_unique<AudioPlayer>(silentPreview ? AudioPlayer::Backend::Null : AudioPlayer::Backend::Default)) {
    QString err;
    items_ = loadAudioLibrary(&err);
    auto* mine = addGroup("Mine");
    addCategory(mine, "Favorites", "favorites");
    addCategory(mine, "My Music", "project");   // audio already in the project
    auto* music = addGroup("Music");
    addCategory(music, "All Music", "music:");
    QStringList cats;
    for (const auto& it : items_)
        if (it.kind == "music" && !cats.contains(it.category)) cats << it.category;
    for (const auto& c : cats) addCategory(music, c, "music:" + c);
    auto* sfx = addGroup("Sound Effect");
    addCategory(sfx, "All Sound Effects", "sfx:");
    cats.clear();
    for (const auto& it : items_)
        if (it.kind == "sfx" && !cats.contains(it.category)) cats << it.category;
    for (const auto& c : cats) addCategory(sfx, c, "sfx:" + c);
    list_->setIconSize(QSize(152, 86));
    list_->setGridSize(QSize(168, 120));
    list_->setAccessibleName("Audio library");
    auto* buttons = new QHBoxLayout();
    playButton_ = new QPushButton(icon("play"), "Preview", this);
    playButton_->setObjectName("audioPreview");
    addButton_ = new QPushButton(icon("plus"), "Add to Timeline", this);
    addButton_->setObjectName("audioAdd");
    favButton_ = new QPushButton("Favorite", this);
    favButton_->setObjectName("audioFavorite");
    for (QPushButton* b : {playButton_, addButton_, favButton_}) buttons->addWidget(b);
    buttons->addStretch(1);
    right_->insertLayout(2, buttons);   // under the list, above the hint
    hint_->setText(err.isEmpty() ? QString("MeerCut's own music and sound effects - free to use in your videos. Double-click to "
                                           "listen, drag onto the timeline or click Add to Timeline.")
                                 : err);
    previewTimer_ = new QTimer(this);
    previewTimer_->setInterval(100);
    connect(previewTimer_, &QTimer::timeout, this, [this] {
        const AudioItem* it = find(previewId_);
        if (!it || player_->position() >= static_cast<int64_t>(it->seconds * player_->sampleRate())) stopPreview();
    });
    list_->setMime({"text/uri-list", kMimeLibraryAudio}, [this](const QList<QListWidgetItem*>& sel) -> QMimeData* {
        if (sel.isEmpty()) return nullptr;
        const AudioItem* it = find(sel.front()->data(kRoleId).toString());
        if (!it) return nullptr;
        const QString path = materializeAsset("audio/" + it->file);
        if (path.isEmpty()) return nullptr;
        auto* m = new QMimeData();
        m->setUrls({QUrl::fromLocalFile(path)});   // other programs take the file
        m->setData(kMimeLibraryAudio, QByteArray::number(it->seconds));   // the timeline previews it on its track (M10)
        return m;
    });
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) { togglePreview(it->data(kRoleId).toString()); });
    auto selectedId = [this] { return list_->currentItem() ? list_->currentItem()->data(kRoleId).toString() : QString(); };
    connect(playButton_, &QPushButton::clicked, this, [this, selectedId] { togglePreview(selectedId()); });
    connect(addButton_, &QPushButton::clicked, this, [this, selectedId] { addToTimeline(selectedId()); });
    connect(favButton_, &QPushButton::clicked, this, [this, selectedId] {
        if (!selectedId().isEmpty()) toggleFavorite(selectedId());
    });
    connect(list_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* it = list_->itemAt(pos);
        if (!it) return;
        const QString id = it->data(kRoleId).toString();
        QMenu m(this);
        m.addAction(icon(previewId_ == id ? "stop" : "play"), previewId_ == id ? "Stop" : "Preview", [this, id] { togglePreview(id); });
        m.addAction(icon("plus"), "Add to Timeline at Playhead", [this, id] { addToTimeline(id); });
        m.addAction(favorite(id) ? "Remove from Favorites" : "Add to Favorites", [this, id] { toggleFavorite(id); });
        m.exec(list_->viewport()->mapToGlobal(pos));
    });
    selectCategory("music:");
}

AudioPage::~AudioPage() { stopPreview(); }

const AudioItem* AudioPage::find(const QString& id) const {
    for (const auto& it : items_)
        if (it.id == id) return &it;
    return nullptr;
}

void AudioPage::refresh() {
    const QString key = currentCategory();
    const QString needle = search_->text().trimmed();
    const QStringList favs = libraryFavorites(page_);
    list_->clear();
    if (key == "project") {   // the audio files already in the project
        for (const auto& s : doc_->project().sources) {
            if (!s.hasAudio || s.hasVideo) continue;
            const QString name = s.name.empty() ? QFileInfo(QString::fromUtf8(s.path)).fileName() : QString::fromStdString(s.name);
            if (!needle.isEmpty() && !name.contains(needle, Qt::CaseInsensitive)) continue;
            auto* li = new QListWidgetItem(icon("volume", color::waveform), name);
            li->setToolTip("Already in this project (Media page)");
            li->setFlags(Qt::ItemIsEnabled);
            list_->addItem(li);
        }
        return;
    }
    for (const auto& it : items_) {
        if (key == "favorites" ? !favs.contains(it.id) : !(it.kind + ":").startsWith(key.section(':', 0, 0) + ":")) continue;
        if (key.contains(':') && !key.section(':', 1).isEmpty() && it.category != key.section(':', 1)) continue;
        if (!needle.isEmpty() && !it.title.contains(needle, Qt::CaseInsensitive) && !it.category.contains(needle, Qt::CaseInsensitive))
            continue;
        auto* li = new QListWidgetItem(audioCard(it, previewId_ == it.id, favs.contains(it.id)), it.title);
        li->setData(kRoleId, it.id);
        li->setToolTip(QString("%1 - %2, %3").arg(it.title, it.category, duration(it.seconds)));
        list_->addItem(li);
    }
}

bool AudioPage::togglePreview(const QString& id) {
    const bool same = previewId_ == id;
    stopPreview();
    if (same || id.isEmpty()) return false;
    const AudioItem* it = find(id);
    if (!it) return false;
    // the item as a one-clip project, played by its own player (the timeline's playback is not touched)
    auto p = std::make_shared<Project>();
    MediaSource s;
    s.id = "preview";
    const QString path = QDir(assetsDir()).filePath("audio/" + it->file);
    const QByteArray u8 = QDir::cleanPath(path).toUtf8();
    s.path = std::string(u8.constData(), static_cast<size_t>(u8.size()));
    s.duration = Rational::fromDouble(it->seconds, 1000);
    s.hasAudio = true;
    p->sources.push_back(s);
    Sequence q;
    q.id = "s";
    q.frameRate = FrameRate::fps30();
    Track a{"a", TrackKind::Audio, "Audio"};
    Clip c;
    c.id = "c";
    c.sourceId = "preview";
    c.duration = frameToTime(timeToFrame(*s.duration, q.frameRate, Rounding::Floor), q.frameRate);
    a.clips.push_back(c);
    q.tracks.push_back(a);
    p->sequences.push_back(q);
    ProjectSnapshot snap;
    snap.project = p;
    snap.sequenceId = "s";
    snap.version = 1;
    player_->start(snap, 0);
    previewId_ = id;
    previewTimer_->start();
    playButton_->setText("Stop");
    playButton_->setIcon(icon("stop"));
    refresh();
    return true;
}

void AudioPage::stopPreview() {
    if (player_) player_->stop();
    if (previewTimer_) previewTimer_->stop();
    const bool was = !previewId_.isEmpty();
    previewId_.clear();
    if (playButton_) {
        playButton_->setText("Preview");
        playButton_->setIcon(icon("play"));
    }
    if (was) refresh();
}

std::string AudioPage::addToTimeline(const QString& id, std::optional<Time> at) {
    const AudioItem* it = find(id);
    if (!it) {
        emit edit_->message("Choose a sound first");
        return {};
    }
    const QString path = materializeAsset("audio/" + it->file);
    if (path.isEmpty()) {
        emit edit_->message("The sound file is missing from the audio library: " + it->file);
        return {};
    }
    QStringList errors, notices;
    const auto ids = doc_->importMedia({path}, &errors, &notices);
    if (ids.empty()) {
        emit edit_->message("Could not add the sound: " + errors.join("; "));
        return {};
    }
    if (!edit_->addToTimeline(ids.front(), at)) return {};
    emit edit_->info(it->title + " added to the timeline");
    return doc_->primarySelection();
}

// ---- Stickers -------------------------------------------------------------------------------------------------

StickersPage::StickersPage(Document* doc, EditController* edit, QWidget* parent)
    : LibraryPageBase("stickers", "Search all stickers", parent), doc_(doc), edit_(edit) {
    text::ensureGuiApplication();
    auto* mine = addGroup("Mine");
    addCategory(mine, "Favorites", "favorites");
    auto* all = addGroup("Stickers");
    addCategory(all, "All Stickers", "all");
    for (const auto& c : text::stickerCategories()) addCategory(all, QString::fromStdString(c), QString::fromStdString(c));
    list_->setIconSize(QSize(96, 96));
    list_->setGridSize(QSize(112, 124));
    list_->setAccessibleName("Stickers");
    list_->viewport()->installEventFilter(this);
    list_->setMime({kMimeSticker}, [](const QList<QListWidgetItem*>& sel) -> QMimeData* {
        if (sel.isEmpty()) return nullptr;
        auto* m = new QMimeData();
        m->setData(kMimeSticker, sel.front()->data(kRoleId).toString().toUtf8());
        return m;
    });
    hint_->setText("MeerCut's own animated stickers. Hover to play, drag onto the timeline or double-click to add at the "
                   "playhead; resize and move them in the player.");
    hoverTimer_ = new QTimer(this);
    hoverTimer_->setInterval(1000 / 15);
    connect(hoverTimer_, &QTimer::timeout, this, [this] {
        if (!hoverItem_) return;
        ++hoverStep_;
        hoverItem_->setIcon(thumbnail(hoverItem_->data(kRoleId).toString(), std::fmod(hoverStep_ / 15.0, 3.0)));
    });
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) { add(it->data(kRoleId).toString()); });
    connect(list_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* it = list_->itemAt(pos);
        if (!it) return;
        const QString id = it->data(kRoleId).toString();
        QMenu m(this);
        m.addAction(icon("plus"), "Add to Timeline at Playhead", [this, id] { add(id); });
        m.addAction(favorite(id) ? "Remove from Favorites" : "Add to Favorites", [this, id] { toggleFavorite(id); });
        m.exec(list_->viewport()->mapToGlobal(pos));
    });
    selectCategory("Trending");
}

QIcon StickersPage::thumbnail(const QString& id, double seconds) const {
    const text::StickerInfo* info = text::findSticker(id.toStdString());
    const int s = 192;
    QPixmap pm(s, s);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath card;
    card.addRoundedRect(QRectF(0, 0, s / 2.0, s / 2.0), 6, 6);
    p.fillPath(card, color::panelHeader.lighter(115));
    if (info) {
        const int w = info->fullFrame ? s : s, h = info->fullFrame ? s * 9 / 16 : s;
        const QImage img = text::stickerImage(StickerData{info->id, 0, {}}, seconds, 5.0, w, h);
        p.drawImage(QRectF((s / 2.0 - w / 2.0) / 2.0 + (s / 2.0 - w / 2.0) / 2.0, (s / 2.0 - h / 2.0) / 2.0 + (s / 2.0 - h / 2.0) / 2.0,
                           w / 2.0, h / 2.0),
                    img);
    }
    return QIcon(pm);
}

void StickersPage::refresh() {
    hoverItem_ = nullptr;
    hoverTimer_->stop();
    const QString key = currentCategory();
    const QString needle = search_->text().trimmed();
    const QStringList favs = libraryFavorites(page_);
    list_->clear();
    for (const auto& info : text::stickerCatalog()) {
        const QString id = QString::fromStdString(info.id), name = QString::fromStdString(info.name);
        const QString cat = QString::fromStdString(info.category);
        if (key == "favorites" ? !favs.contains(id) : (key != "all" && cat != key)) continue;
        if (!needle.isEmpty() && !name.contains(needle, Qt::CaseInsensitive) && !cat.contains(needle, Qt::CaseInsensitive)) continue;
        auto* li = new QListWidgetItem(thumbnail(id, info.animated ? 1.2 : 0.0), name);
        li->setData(kRoleId, id);
        li->setToolTip(name + " (" + cat + (info.animated ? ", animated" : "") + ")");
        list_->addItem(li);
    }
}

bool StickersPage::eventFilter(QObject* o, QEvent* e) {
    if (o == list_->viewport()) {
        if (e->type() == QEvent::MouseMove) {
            QListWidgetItem* it = list_->itemAt(static_cast<QMouseEvent*>(e)->position().toPoint());
            if (it != hoverItem_) {
                if (hoverItem_) hoverItem_->setIcon(thumbnail(hoverItem_->data(kRoleId).toString(), 1.2));
                hoverItem_ = it;
                hoverStep_ = 0;
                if (it) hoverTimer_->start();
                else hoverTimer_->stop();
            }
        } else if (e->type() == QEvent::Leave && hoverItem_) {
            hoverItem_->setIcon(thumbnail(hoverItem_->data(kRoleId).toString(), 1.2));
            hoverItem_ = nullptr;
            hoverTimer_->stop();
        }
    }
    return QWidget::eventFilter(o, e);
}

std::string StickersPage::add(const QString& id, std::optional<Time> at) { return edit_->addSticker(id.toStdString(), at); }

// ---- Templates ------------------------------------------------------------------------------------------------

namespace {
struct NoMedia final : FrameProvider {   // templates hold generated pictures only
    Result frame(const Clip&, const MediaSource&, const Time&) override { throw RenderError("templates have no media"); }
};
}  // namespace

TemplatesPage::TemplatesPage(Document* doc, EditController* edit, QWidget* parent)
    : LibraryPageBase("templates", "Search all templates", parent), doc_(doc), edit_(edit) {
    text::ensureGuiApplication();
    auto* mine = addGroup("Mine");
    addCategory(mine, "Favorites", "favorites");
    auto* all = addGroup("Templates");
    addCategory(all, "All Templates", "all");
    addCategory(all, "Trending", "trending");
    for (const auto& c : templateCategories())
        if (c != "Trending") addCategory(all, QString::fromStdString(c), QString::fromStdString(c));
    list_->setIconSize(QSize(160, 90));
    list_->setGridSize(QSize(176, 126));
    list_->setAccessibleName("Templates");
    list_->setMime({kMimeTemplate}, [](const QList<QListWidgetItem*>& sel) -> QMimeData* {
        if (sel.isEmpty()) return nullptr;
        auto* m = new QMimeData();
        m->setData(kMimeTemplate, sel.front()->data(kRoleId).toString().toUtf8());
        return m;
    });
    hint_->setText("Ready-made intros, end screens, countdowns and split screens. Double-click or drag to add; right-click a "
                   "grey media slot > Replace Clip (or drop a video on it) to put your own media in.");
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) { add(it->data(kRoleId).toString()); });
    connect(list_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* it = list_->itemAt(pos);
        if (!it) return;
        const QString id = it->data(kRoleId).toString();
        QMenu m(this);
        m.addAction(icon("plus"), "Add to Timeline at Playhead", [this, id] { add(id); });
        m.addAction(favorite(id) ? "Remove from Favorites" : "Add to Favorites", [this, id] { toggleFavorite(id); });
        m.exec(list_->viewport()->mapToGlobal(pos));
    });
    selectCategory("trending");
}

QIcon TemplatesPage::thumbnail(const QString& id) const {
    if (auto it = thumbs_.find(id); it != thumbs_.end()) return it->second;
    const TemplateInfo* info = findTemplate(id.toStdString());
    QImage frame(320, 180, QImage::Format_RGBA8888);
    frame.fill(color::panelHeader);
    if (info) {
        try {   // the template rendered by the engine at 60 % of its length
            Project p;
            Sequence q;
            q.id = "s";
            q.frameRate = FrameRate::fps30();
            q.tracks.push_back(Track{"v1", TrackKind::Video, "Video 1"});
            p.sequences.push_back(q);
            UndoStack st(p);
            st.execute(edit::pasteClips(p, "s", templateClips(info->id, q.frameRate), Time(0), nullptr, true));
            static thread_local auto titles = text::makeTitleEngine();
            static thread_local auto backend = makeCpuBackend();
            NoMedia media;
            const int64_t at = static_cast<int64_t>(info->seconds * 0.6 * 30);
            const Image& img = renderFrame(p.sequences[0], at, p, media, *backend, nullptr, nullptr, 320, 180, titles.get());
            frame = QImage(img.pixels.data(), img.width, img.height, img.stride(), QImage::Format_RGBA8888).copy();
        } catch (const std::exception&) {
        }
    }
    QPixmap pm = QPixmap::fromImage(frame);
    pm.setDevicePixelRatio(2.0);
    QIcon ic(pm);
    thumbs_[id] = ic;
    return ic;
}

void TemplatesPage::refresh() {
    const QString key = currentCategory();
    const QString needle = search_->text().trimmed();
    const QStringList favs = libraryFavorites(page_);
    list_->clear();
    for (const auto& info : templateCatalog()) {
        const QString id = QString::fromStdString(info.id), name = QString::fromStdString(info.name);
        const QString cat = QString::fromStdString(info.category);
        if (key == "favorites" ? !favs.contains(id) : key == "trending" ? !info.trending : (key != "all" && cat != key)) continue;
        if (!needle.isEmpty() && !name.contains(needle, Qt::CaseInsensitive) && !cat.contains(needle, Qt::CaseInsensitive)) continue;
        auto* li = new QListWidgetItem(thumbnail(id), name);
        li->setData(kRoleId, id);
        li->setToolTip(QString::fromStdString(info.description) + QString(" (%1 s)").arg(info.seconds));
        list_->addItem(li);
    }
}

std::vector<std::string> TemplatesPage::add(const QString& id, std::optional<Time> at) {
    return edit_->addTemplate(id.toStdString(), at);
}

}  // namespace fa::ui
