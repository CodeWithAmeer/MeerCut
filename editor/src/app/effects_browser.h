// Effects and Filters pages (M4): every effect of the registry (Effects page: blur, light, stylize, film, glitch,
// lens, distortion, keying, detail; Filters page: black & white, sepia and MeerCut's built-in looks), plus the
// user's effect presets ("My Presets"). Categories, search, favourites (kept in the settings), thumbnails rendered
// by applying the effect to a generated sample picture, an animated preview while the mouse rests on a tile.
// M10: dragged onto the timeline an effect becomes its own clip above the video, like text (an adjustment layer holding
// it, which affects every track below it); Ctrl+drop onto a clip applies it to that clip only. Double-click applies it
// to the selected clips, or adds it at the playhead when none is selected.
#pragma once

#include "document.h"

#include <QListWidget>
#include <QWidget>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QLabel;
class QLineEdit;
class QTimer;

namespace fa::ui {

class EditController;
class EffectPresetLibrary;

inline constexpr const char* kMimeEffect = "application/x-meercut-effect";   // one effect (clip JSON holder)
inline constexpr const char* kMimeAdjustment = "application/x-meercut-adjustment";   // a new adjustment layer

class EffectList : public QListWidget {
    Q_OBJECT
public:
    using QListWidget::QListWidget;

protected:
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
};

class EffectsBrowser : public QWidget {
    Q_OBJECT
public:
    enum class Page { Effects, Filters };
    EffectsBrowser(Page page, Document* doc, EditController* edit, EffectPresetLibrary* presets, QWidget* parent = nullptr);

    EffectList* list() const { return list_; }
    QListWidget* categories() const { return categories_; }
    QLineEdit* search() const { return search_; }
    void refresh();
    // Item helpers (the item's key: "<type>", "look:<id>" or "preset:<id>").
    QListWidgetItem* itemFor(const QString& key) const;
    EffectRef effectOf(const QString& key) const;
    bool apply(const QString& key);   // adds the effect to the selected clips (one undo step), else as a layer (M10)
    // M10: the effect as its own clip above the video at `at` (default: the playhead), like a title.
    bool addAsLayer(const QString& key, std::optional<Time> at = std::nullopt);
    bool isFavourite(const QString& key) const;
    void setFavourite(const QString& key, bool on);
    void selectCategory(const QString& name);

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    struct Entry {
        QString key, name, category, description;
        EffectRef effect;
        bool animated = false;   // time-varying: the hover preview plays time, else it fades the effect in
    };
    std::vector<Entry> entries() const;
    QIcon thumb(const Entry& e, double phase);
    void animate();

    Page page_;
    Document* doc_;
    EditController* edit_;
    EffectPresetLibrary* presets_;
    QLineEdit* search_;
    QListWidget* categories_;
    QLabel* heading_;
    EffectList* list_;
    QTimer* hoverTimer_;
    QListWidgetItem* hoverItem_ = nullptr;
    int hoverStep_ = 0;
    std::map<QString, std::vector<QIcon>> frames_;
    std::map<QString, QIcon> stills_;
    QStringList favourites_;
};

}  // namespace fa::ui
