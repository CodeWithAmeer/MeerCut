// M9: the Audio, Stickers and Templates pages of the resource panel (Filmora's sections), with MeerCut's own
// content: synthesised music and sound effects (tools/make_audio_library.py), vector stickers drawn by the engine
// (fa/text/stickers.h) and templates built from titles, stickers and media placeholders (templates.h).
#pragma once

#include "document.h"

#include <QListWidget>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

namespace fa {
class AudioPlayer;
}

namespace fa::ui {

class EditController;

// ---- files shipped with MeerCut and the user's copies -----------------------------------------------------------

QString assetsDir();       // MEERCUT_ASSETS, else <program folder>/assets
QString userLibraryDir();  // MEERCUT_LIBRARY_DIR, else %LOCALAPPDATA%/MeerCut/library
// A library file as projects reference it: copied once into the user's library folder (projects then keep working
// when MeerCut is reinstalled elsewhere or updated). Empty when the asset is missing or cannot be copied.
QString materializeAsset(const QString& relativePath);

struct AudioItem {
    QString id, title, kind, category, file;   // kind: "music" or "sfx"; file relative to assets/audio
    double seconds = 0;
    int bpm = 0;
};
std::vector<AudioItem> loadAudioLibrary(QString* error = nullptr);

// M11: Media > Sample Videos - short clips to practise with (tools/make_sample_videos.py, assets/samples/samples.json).
struct SampleVideo {
    QString id, title, file, description;   // file relative to assets/samples
    QString credit, licence, licenceUrl;    // empty for MeerCut's own clips
    double seconds = 0, poster = 0;         // poster: the second its tile shows
    int width = 0, height = 0;
    double fps = 0;
};
std::vector<SampleVideo> loadSampleVideos(QString* error = nullptr);

// Favourites of a library page ("audio", "stickers", "templates"), kept in the user's settings.
QStringList libraryFavorites(const QString& page);
void setLibraryFavorite(const QString& page, const QString& id, bool on);

// A drag source whose drag data comes from the page.
class LibraryList : public QListWidget {
    Q_OBJECT
public:
    using MimeFn = std::function<QMimeData*(const QList<QListWidgetItem*>&)>;
    explicit LibraryList(QWidget* parent = nullptr) : QListWidget(parent) {}
    void setMime(QStringList types, MimeFn fn) {
        types_ = std::move(types);
        mime_ = std::move(fn);
    }

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override { return mime_ ? mime_(items) : nullptr; }
    QStringList mimeTypes() const override { return types_; }

private:
    QStringList types_;
    MimeFn mime_;
};

// Shared layout of the three pages: a category tree on the left (Mine / Favorites first), search and a card grid.
class LibraryPageBase : public QWidget {
    Q_OBJECT
public:
    LibraryPageBase(const QString& page, const QString& searchHint, QWidget* parent);
    LibraryList* list() const { return list_; }
    QTreeWidget* categories() const { return tree_; }
    QLineEdit* search() const { return search_; }
    QString currentCategory() const;   // "favorites", "all", or a category name
    void selectCategory(const QString& key);

protected:
    QTreeWidgetItem* addGroup(const QString& text);
    QTreeWidgetItem* addCategory(QTreeWidgetItem* group, const QString& text, const QString& key);
    bool favorite(const QString& id) const;
    void toggleFavorite(const QString& id);
    virtual void refresh() = 0;
    QString page_;
    QTreeWidget* tree_;
    QLineEdit* search_;
    QLabel* heading_;
    LibraryList* list_;
    QLabel* hint_;
    QVBoxLayout* right_;   // the column beside the tree: heading, list, hint (a page may add rows to it)
};

// ---- Audio ------------------------------------------------------------------------------------------------------

class AudioPage : public LibraryPageBase {
    Q_OBJECT
public:
    AudioPage(Document* doc, EditController* edit, bool silentPreview, QWidget* parent = nullptr);
    ~AudioPage() override;
    const std::vector<AudioItem>& items() const { return items_; }
    // Copies the file into the user's library, imports it and puts it on an audio track at `at` (default: the
    // playhead). Returns the new clip's id, empty on failure.
    std::string addToTimeline(const QString& id, std::optional<Time> at = std::nullopt);
    bool togglePreview(const QString& id);   // plays the item (stops a playing one)
    QString previewing() const { return previewId_; }
    void refresh() override;

private:
    const AudioItem* find(const QString& id) const;
    void stopPreview();
    Document* doc_;
    EditController* edit_;
    std::vector<AudioItem> items_;
    std::unique_ptr<AudioPlayer> player_;
    QString previewId_;
    QTimer* previewTimer_;
    QPushButton* playButton_;
    QPushButton* addButton_;
    QPushButton* favButton_;
};

// ---- Stickers ---------------------------------------------------------------------------------------------------

class StickersPage : public LibraryPageBase {
    Q_OBJECT
public:
    StickersPage(Document* doc, EditController* edit, QWidget* parent = nullptr);
    std::string add(const QString& id, std::optional<Time> at = std::nullopt);   // the new clip's id
    void refresh() override;

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    QIcon thumbnail(const QString& id, double seconds) const;
    Document* doc_;
    EditController* edit_;
    QTimer* hoverTimer_;
    QListWidgetItem* hoverItem_ = nullptr;
    int hoverStep_ = 0;
};

// ---- Templates --------------------------------------------------------------------------------------------------

class TemplatesPage : public LibraryPageBase {
    Q_OBJECT
public:
    TemplatesPage(Document* doc, EditController* edit, QWidget* parent = nullptr);
    std::vector<std::string> add(const QString& id, std::optional<Time> at = std::nullopt);   // the new clips' ids
    void refresh() override;

private:
    QIcon thumbnail(const QString& id) const;
    Document* doc_;
    EditController* edit_;
    mutable std::map<QString, QIcon> thumbs_;
};

inline constexpr const char* kMimeSticker = "application/x-meercut-sticker";     // sticker id
inline constexpr const char* kMimeTemplate = "application/x-meercut-template";   // template id
// M10: a sound from the Audio page - its length in seconds; the file itself is the drag's URL
inline constexpr const char* kMimeLibraryAudio = "application/x-meercut-library-audio";
// M11: a sample video from the Media page, the same way (it lands on the video track under the mouse)
inline constexpr const char* kMimeLibraryVideo = "application/x-meercut-library-video";

}  // namespace fa::ui
