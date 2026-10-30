// Main-window panels: resource panel (Media, Titles, Transitions), preview monitor, inspector, timeline panel.
#pragma once

#include "document.h"
#include "library_pages.h"
#include "preview_canvas.h"

#include <QImage>
#include <QListWidget>
#include <QWidget>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

class QAction;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QMenu;
class QCheckBox;
class QScrollBar;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QToolButton;
class QTabWidget;
class QTimer;
class QGroupBox;
class QHBoxLayout;
class QVBoxLayout;

namespace fa::ui {

class EditController;
class LevelMeter;
class MediaService;
class PlaybackController;
class TimelineView;
class TitleLibrary;

// ---- timeline panel ----------------------------------------------------------------------------

struct TimelineActions {
    QAction* undo = nullptr;
    QAction* redo = nullptr;
    QAction* del = nullptr;
    QAction* rippleDelete = nullptr;
    QAction* split = nullptr;
    QAction* snapping = nullptr;
    QAction* zoomIn = nullptr;
    QAction* zoomOut = nullptr;
    QAction* zoomFit = nullptr;
    QAction* renderPreview = nullptr;   // M5
    const std::map<QString, QAction*>* named = nullptr;   // M9: every main-window command, for the clip menu
    std::function<void(QMenu*)> fillSpeedMenu;            // M9: the toolbar's Speed drop-down
};

class TimelinePanel : public QWidget {
    Q_OBJECT
public:
    TimelinePanel(Document* doc, EditController* edit, MediaService* media, PlaybackController* playback,
                  const TimelineActions& actions, QWidget* parent = nullptr);
    TimelineView* view() const { return view_; }

private:
    void syncScrollBars();
    Document* doc_;
    TimelineView* view_;
    QScrollBar* hbar_;
    QScrollBar* vbar_;
    QSlider* zoom_;
    bool syncing_ = false;
};

// ---- resource area (top left): icon-over-text page bar like Filmora's, only pages that work ----------

class ResourceTabs : public QWidget {
    Q_OBJECT
public:
    explicit ResourceTabs(QWidget* parent = nullptr);
    void addPage(QWidget* page, const char* iconName, const QString& text);
    void setCurrentWidget(QWidget* page);
    QWidget* currentWidget() const;

private:
    QHBoxLayout* bar_;
    QStackedWidget* stack_;
    std::vector<QToolButton*> buttons_;
};

// ---- media library -----------------------------------------------------------------------------

class MediaList : public QListWidget {
    Q_OBJECT
public:
    using QListWidget::QListWidget;
    std::function<QMimeData*(const QString& sampleId)> sampleMime;   // M11: a sample video's drag data (the panel's)

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
    QStringList mimeTypes() const override;
};

class MediaPanel : public QWidget {
    Q_OBJECT
public:
    MediaPanel(Document* doc, EditController* edit, MediaService* media, QWidget* parent = nullptr);
    MediaList* list() const { return list_; }
    QListWidget* sidebar() const { return sidebar_; }
    QLineEdit* search() const { return search_; }
    void refresh();
    void selectSource(const QString& sourceId);   // M9: shows every media and selects that one

    // M11: Sample Videos (the sidebar's fourth entry). A sample is copied into the user's library folder and imported
    // like any file (once: a second use takes the same media); `toTimeline` then adds it at `at` (default: the
    // playhead). Returns the media's id, empty on failure (the status bar says why).
    const std::vector<SampleVideo>& samples() const { return samples_; }
    std::string addSample(const QString& id, std::optional<Time> at = std::nullopt, bool toTimeline = true);
    QLabel* samplesHint() const { return samplesHint_; }

signals:
    void importRequested();
    void relinkRequested(const QString& sourceId);
    void filesDropped(const QStringList& files);   // dragged in from Explorer

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    QPixmap tile(const MediaSource& src, bool used, bool missing);
    QPixmap sampleTile(const SampleVideo& s, bool inProject);
    void refreshSamples(const QString& filter);
    const SampleVideo* findSample(const QString& id) const;
    std::string sampleSourceId(const SampleVideo& s) const;   // the project's media for this sample, if imported
    Document* doc_;
    EditController* edit_;
    MediaService* media_;
    MediaList* list_;
    QListWidget* sidebar_;
    QLineEdit* search_;
    QLabel* dropHint_;
    QLabel* samplesHint_;
    std::vector<SampleVideo> samples_;
    bool refreshPending_ = false;
};

class TransitionList : public QListWidget {
    Q_OBJECT
public:
    using QListWidget::QListWidget;

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
    QStringList mimeTypes() const override;
};

// Transition browser: every transition the renderer implements, with thumbnails rendered by the
// engine's own compositor (hovering an item plays it). Drag onto a cut, or double-click to add it
// after the selected clip / change the selected transition.
class TransitionsPanel : public QWidget {
    Q_OBJECT
public:
    TransitionsPanel(Document* doc, EditController* edit, QWidget* parent = nullptr);
    TransitionList* list() const { return list_; }
    bool apply(const QString& type);   // double-click action

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    QIcon frameIcon(const QString& type, double progress);
    void animate();
    Document* doc_;
    EditController* edit_;
    TransitionList* list_;
    QTimer* hoverTimer_;
    QListWidgetItem* hoverItem_ = nullptr;
    int hoverStep_ = 0;
    std::map<QString, std::vector<QIcon>> frames_;
};

// ---- titles (M3) -------------------------------------------------------------------------------

class TitleList : public QListWidget {
    Q_OBJECT
public:
    using QListWidget::QListWidget;

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
    QStringList mimeTypes() const override;
};

// Titles browser: MeerCut's own templates and the user's presets ("My Titles") by category, with search.
// Thumbnails are rendered by the real text engine and compositor (hovering an item plays its animation).
// Drag a title onto the timeline, or double-click to add it at the playhead.
class TitlesPanel : public QWidget {
    Q_OBJECT
public:
    TitlesPanel(Document* doc, EditController* edit, TitleLibrary* library, QWidget* parent = nullptr);
    ~TitlesPanel() override;
    TitleList* list() const { return list_; }
    QListWidget* categories() const { return categories_; }
    QLineEdit* search() const { return search_; }
    bool apply(const QString& templateId);   // double-click action: add at the playhead
    void refresh();

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    QIcon frameIcon(const QString& templateId, double seconds);
    void animate();
    Document* doc_;
    EditController* edit_;
    TitleLibrary* library_;
    QListWidget* categories_;
    QLineEdit* search_;
    QLabel* heading_;
    TitleList* list_;
    QTimer* hoverTimer_;
    QListWidgetItem* hoverItem_ = nullptr;
    int hoverStep_ = 0;
    struct Engines;
    std::unique_ptr<Engines> engines_;
};

// ---- preview -----------------------------------------------------------------------------------

class PreviewPanel : public QWidget {
    Q_OBJECT
public:
    PreviewPanel(Document* doc, EditController* edit, PlaybackController* playback, QWidget* parent = nullptr);
    PreviewCanvas* canvas() const { return canvas_; }
    QToolButton* playButton() const { return play_; }
    LevelMeter* levelMeter() const { return meter_; }   // M6
    void toggleFullscreen();   // the player alone on the whole screen (Esc / double-click to leave)
    bool isFullscreen() const { return fullscreen_ != nullptr; }

signals:
    void snapshotRequested();

protected:
    void resizeEvent(QResizeEvent*) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void updateTime();
    void updatePreviewSize();
    Document* doc_;
    PlaybackController* playback_;
    PreviewCanvas* canvas_;
    QWidget* fullscreen_ = nullptr;
    QVBoxLayout* layout_ = nullptr;
    QSlider* scrub_;
    QLabel* time_;
    QToolButton* play_;
    QComboBox* quality_;
    LevelMeter* meter_ = nullptr;
    QTimer* meterPoll_ = nullptr;
    bool scrubbing_ = false;
};

}  // namespace fa::ui
