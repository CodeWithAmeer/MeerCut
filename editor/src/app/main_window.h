// Main editor window: Filmora-style four-pane workspace (resource panel, player, inspector, timeline).
#pragma once

#include "document.h"

#include "fa/model/subtitles.h"

#include <QMainWindow>

#include <map>
#include <memory>
#include <string>
#include <vector>

class QAction;
class QLabel;
class QProgressBar;
class QMenu;
class QSplitter;

namespace fa::ui {

class AnalysisService;
class AutosaveManager;
class RenderPreviewController;
class EditController;
class InspectorPanel;
class MediaPanel;
class MediaService;
class PlaybackController;
class PreviewPanel;
class ResourceTabs;
class TimelinePanel;
class TitleLibrary;
class TitlesPanel;
class TransitionsPanel;
class EffectsBrowser;
class AudioPage;
class StickersPage;
class TemplatesPage;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    struct Options {
        bool interactive = true;     // false: no modal message boxes (tests/automation); errors -> status bar
        bool silentAudio = false;    // true: null audio backend (tests on machines without speakers)
    };
    explicit MainWindow(Options options);
    explicit MainWindow() : MainWindow(Options{}) {}
    ~MainWindow() override;

    // Non-interactive entry points (also used by the menus after their file dialogs).
    bool openProjectFile(const QString& path);
    bool saveProjectTo(const QString& path);
    std::vector<std::string> importFiles(const QStringList& files);
    bool exportTo(const QString& file, QString* error);
    // Subtitles (M3). Import adds the captions on subtitle tracks (one undo step), `offset` added to their
    // times; `warnings` receives what was not imported exactly. Export writes one subtitle track as SRT or
    // ASS (by extension); default track: the selected caption's, else the first subtitle track.
    bool importSubtitlesFile(const QString& path, unsigned codePage = 0, Time offset = Time(0),
                             QStringList* warnings = nullptr);
    bool exportSubtitlesTo(const QString& path, std::string trackId = {}, QString* error = nullptr);
    bool saveSnapshot(const QString& pngPath);

    Document* document() const { return doc_; }
    EditController* editController() const { return edit_; }
    PlaybackController* playback() const { return playback_; }
    TimelinePanel* timelinePanel() const { return timeline_; }
    MediaPanel* mediaPanel() const { return media_; }
    PreviewPanel* previewPanel() const { return preview_; }
    InspectorPanel* inspector() const { return inspector_; }
    AnalysisService* analysis() const { return analysis_; }
    RenderPreviewController* renderPreview() const { return renderPreview_; }
    TransitionsPanel* transitionsPanel() const { return transitions_; }
    EffectsBrowser* effectsPage() const { return effectsPage_; }
    EffectsBrowser* filtersPage() const { return filtersPage_; }
    TitlesPanel* titlesPanel() const { return titles_; }
    TitleLibrary* titleLibrary() const { return titleLibrary_; }
    ResourceTabs* resourceTabs() const { return resources_; }
    AudioPage* audioPage() const { return audioPage_; }          // M9
    StickersPage* stickersPage() const { return stickersPage_; }
    TemplatesPage* templatesPage() const { return templatesPage_; }
    QAction* action(const QString& name) const;
    QString lastMessage() const { return lastMessage_; }

    void showMessage(const QString& text, bool error = false);   // status bar (error: a warning)
    QString aboutText() const;   // the About box: version, author and GitHub, licence, components

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void buildActions();
    void buildMenus();
    void buildLayout();
    void updateTitle();
    void updateActions();
    bool confirmDiscard();
    // M8: autosaved copies of sessions that did not end normally - offered one at a time, newest first
    void offerRecovery(bool fromMenu);
    void newProject();
    void openProject();
    bool save();
    bool saveAs();
    void importDialog();
    void importSubtitlesDialog();
    void exportSubtitlesDialog();
    bool addSubtitleDoc(const SubtitleDoc& doc, Time offset, const QString& name);
    void exportDialog(bool selectedClips = false);
    // M9: the clip menu's dialogs and helpers
    void fillSpeedMenu(QMenu* menu);
    void renameClipDialog();
    void audioGainDialog();
    void replaceClipDialog();
    void locateInLibrary();
    void jumpToEditPoint(bool next);
    void projectSettings();
    void preferences();
    void modelManager();
    void speechToText();                              // M7: AI Tools > Speech to Text
    void sceneDetection();                            // M7: AI Tools > Scene Detection (signal analysis)
    void autoReframe();                               // M7: AI Tools > Auto Reframe
    void addBackgroundMedia(const QString& clipId);   // M7: Video > AI Cutout > Add Background Media
    void snapshotDialog();
    void relink(const QString& sourceId);
    void checkMissingMedia();
    void addRecent(const QString& path);
    void rebuildRecentMenu();
    void showShortcuts();

    Options opt_;
    Document* doc_;
    AutosaveManager* autosave_ = nullptr;   // M8 autosave / crash recovery
    MediaService* mediaService_;
    EditController* edit_;
    PlaybackController* playback_;
    TimelinePanel* timeline_ = nullptr;
    MediaPanel* media_ = nullptr;
    TransitionsPanel* transitions_ = nullptr;
    EffectsBrowser* effectsPage_ = nullptr;
    EffectsBrowser* filtersPage_ = nullptr;
    TitlesPanel* titles_ = nullptr;
    TitleLibrary* titleLibrary_ = nullptr;
    PreviewPanel* preview_ = nullptr;
    InspectorPanel* inspector_ = nullptr;
    AnalysisService* analysis_ = nullptr;   // M5: tracking / stabilisation analysis in the background
    RenderPreviewController* renderPreview_ = nullptr;   // M5: Render Preview cache
    QProgressBar* renderProgress_ = nullptr;
    ResourceTabs* resources_ = nullptr;
    QSplitter* topSplit_ = nullptr;
    QSplitter* vertSplit_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* backendLabel_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    std::map<QString, QAction*> actions_;
    AudioPage* audioPage_ = nullptr;
    StickersPage* stickersPage_ = nullptr;
    TemplatesPage* templatesPage_ = nullptr;
    QString lastMessage_;
};

}  // namespace fa::ui
