#include "main_window.h"

#include "autosave.h"
#include "library_pages.h"

#include "analysis_service.h"
#include "render_preview_controller.h"

#include "speed_panel.h"

#include "color_presets.h"
#include "dialogs.h"
#include "model_manager.h"
#include "cutout_panel.h"
#include "speech_dialog.h"
#include "upscale_panel.h"
#include "fa/ai/interpolate.h"
#include "scene_dialog.h"
#include "reframe_dialog.h"

#include "fa/ai/models.h"
#include "lut_library.h"
#include "effect_presets.h"
#include "effects_browser.h"
#include "edit_controller.h"
#include "inspector_panel.h"
#include "media_service.h"
#include "panels.h"
#include "playback_controller.h"
#include "theme.h"
#include "timeline_view.h"
#include "title_library.h"

#include "fa/model/edit_ops.h"
#include "fa/render/renderer.h"
#include "fa/text/title_engine.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QInputDialog>
#include <QSaveFile>
#include <QStandardPaths>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <filesystem>
#include <functional>

namespace fa::ui {

namespace {
// MeerCut projects are .mcproj; Lumacut's .lcproj open the same way (the format is the same)
const char* kProjectFilter = "MeerCut project (*.mcproj *.lcproj);;JSON project (*.json);;All files (*)";
const char* kMediaFilter =
    "Media files (*.mp4 *.mov *.m4v *.mkv *.webm *.avi *.mts *.m2ts *.wmv *.mpg *.mpeg *.mp3 *.wav *.m4a *.aac *.flac *.ogg *.opus);;"
    "Video (*.mp4 *.mov *.m4v *.mkv *.webm *.avi *.mts *.m2ts *.wmv *.mpg *.mpeg);;"
    "Audio (*.mp3 *.wav *.m4a *.aac *.flac *.ogg *.opus);;All files (*)";

std::filesystem::path toPath(const QString& s) {
    const std::string u = QDir::toNativeSeparators(s).toUtf8().toStdString();
    return std::filesystem::path(std::u8string(u.begin(), u.end()));
}
}  // namespace

MainWindow::MainWindow(Options options) : opt_(options) {
    setWindowTitle(kAppName);
    // M7: the model folder the user chose in the Model Manager (MEERCUT_MODELS, used by the tests, takes precedence)
    if (qEnvironmentVariableIsEmpty("MEERCUT_MODELS") && QSettings().contains("ai/modelsDir"))
        ai::setModelsDirectoryOverride(QSettings().value("ai/modelsDir").toString().toStdWString());
    ai::registerModelIdentities();   // M7: the renderer uses only AI pictures made by the installed models
    setWindowIcon(icon("logo"));
    doc_ = new Document(this);
    autosave_ = new AutosaveManager(doc_, this);
    // tests and automation: off unless a test turns it on (they never write into the user's autosave folder)
    autosave_->setIntervalSeconds(opt_.interactive ? loadRenderSettings().autosaveMinutes * 60 : 0);
    if (opt_.interactive) QTimer::singleShot(0, this, [this] { offerRecovery(false); });   // after the window shows
    mediaService_ = new MediaService(this);
    edit_ = new EditController(doc_, this, this);
    titleLibrary_ = new TitleLibrary({}, this);
    edit_->setTitleLibrary(titleLibrary_);
    edit_->setColorPresets(new ColorPresetLibrary({}, this));
    edit_->setLutLibrary(new LutLibrary({}, this));
    edit_->setEffectPresets(new EffectPresetLibrary({}, this));
    if (!opt_.interactive) edit_->setMatchPolicy(EditController::MatchPolicy::Always);
    playback_ = new PlaybackController(doc_, this, opt_.silentAudio ? AudioPlayer::Backend::Null : AudioPlayer::Backend::Default);
    buildActions();
    buildLayout();
    buildMenus();
    connect(edit_, &EditController::message, this, [this](const QString& m) { showMessage(m, true); });
    connect(edit_, &EditController::info, this, [this](const QString& m) { showMessage(m); });
    connect(doc_, &Document::errorOccurred, this, [this](const QString& m) { showMessage(m, true); });
    connect(playback_, &PlaybackController::statusMessage, this, [this](const QString& m) { showMessage(m); });
    connect(doc_, &Document::fileStateChanged, this, &MainWindow::updateTitle);
    connect(doc_, &Document::changed, this, &MainWindow::updateActions);
    connect(doc_, &Document::selectionChanged, this, &MainWindow::updateActions);
    connect(doc_, &Document::playheadChanged, this, &MainWindow::updateActions);
    updateTitle();
    updateActions();
    // fit the screen (e.g. 1920x1080 at 125 % scaling = 1536x864 logical pixels)
    QSize size(1600, 960);
    if (const QScreen* scr = QGuiApplication::primaryScreen()) {
        const QRect avail = scr->availableGeometry();
        size = size.boundedTo(QSize(avail.width() * 95 / 100, avail.height() * 92 / 100));
    }
    resize(size);
}

MainWindow::~MainWindow() = default;

QAction* MainWindow::action(const QString& name) const {
    auto it = actions_.find(name);
    return it == actions_.end() ? nullptr : it->second;
}

QString MainWindow::aboutText() const {
    return QString("<b>%1</b> %2<br>Made by <a href=\"%3\">%4</a> - official GitHub: <a href=\"%3\">%3</a><br>"
                   "Open-source video editor - GPL-3.0-or-later.<br>Engine: C++20, FFmpeg %5, Qt %6.<br>Licences of "
                   "the components: the licenses folder next to the program (THIRD_PARTY_NOTICES.txt).")
        .arg(QString::fromLatin1(kAppName), QCoreApplication::applicationVersion(), QString::fromLatin1(kAuthorUrl),
             QString::fromLatin1(kAuthorName), QString::fromStdString(ffmpegVersionInfo()).section(' ', 1, 1),
             QString::fromLatin1(qVersion()));
}

void MainWindow::showMessage(const QString& text, bool error) {
    lastMessage_ = text;
    statusBar()->showMessage((error ? "⚠ " : "") + text, error ? 8000 : 4000);
}

// ---- actions ---------------------------------------------------------------------------------

void MainWindow::buildActions() {
    auto add = [this](const QString& name, const QString& text, const QKeySequence& key, const char* ic,
                      std::function<void()> fn) {
        auto* a = new QAction(ic ? icon(ic) : QIcon(), text, this);
        if (!key.isEmpty()) a->setShortcut(key);
        a->setShortcutContext(Qt::WindowShortcut);
        a->setToolTip(key.isEmpty() ? text : text + "  (" + key.toString(QKeySequence::NativeText) + ")");
        connect(a, &QAction::triggered, this, [fn] { fn(); });
        addAction(a);
        actions_[name] = a;
        return a;
    };
    add("new", "New Project", QKeySequence::New, "newfile", [this] { newProject(); });
    add("open", "Open Project...", QKeySequence::Open, "open", [this] { openProject(); });
    add("save", "Save Project", QKeySequence::Save, "save", [this] { save(); });
    add("saveAs", "Save Project As...", QKeySequence("Ctrl+Shift+S"), nullptr, [this] { saveAs(); });
    add("import", "Import Media...", QKeySequence("Ctrl+I"), "import", [this] { importDialog(); });
    add("importSubtitles", "Import Subtitles (SRT / ASS)...", QKeySequence(), "captions", [this] { importSubtitlesDialog(); });
    add("exportSubtitles", "Export Subtitles (SRT / ASS)...", QKeySequence(), "captions", [this] { exportSubtitlesDialog(); });
    add("export", "Export...", QKeySequence("Ctrl+E"), "export", [this] { exportDialog(); });
    add("settings", "Project Settings...", QKeySequence(), "gear", [this] { projectSettings(); });
    add("preferences", "Preferences...", QKeySequence("Ctrl+,"), nullptr, [this] { preferences(); });
    add("exit", "Exit", QKeySequence("Alt+F4"), nullptr, [this] { close(); });
    add("recoverAutosave", "Recover Autosaved Projects...", QKeySequence(), nullptr, [this] { offerRecovery(true); });

    add("undo", "Undo", QKeySequence::Undo, "undo", [this] { doc_->undo(); });
    add("redo", "Redo", QKeySequence("Ctrl+Y"), "redo", [this] { doc_->redo(); });
    actions_["redo"]->setShortcuts({QKeySequence("Ctrl+Y"), QKeySequence("Ctrl+Shift+Z")});
    add("delete", "Delete", QKeySequence::Delete, "delete", [this] { edit_->deleteSelection(false); });
    add("rippleDelete", "Ripple Delete", QKeySequence("Shift+Del"), "ripple", [this] { edit_->deleteSelection(true); });
    add("split", "Split", QKeySequence("Ctrl+B"), "split", [this] { edit_->splitAtPlayhead(); });
    add("trimStart", "Trim Start to Playhead", QKeySequence("Alt+["), nullptr, [this] { edit_->trimToPlayhead(ClipEdge::Head); });
    add("trimEnd", "Trim End to Playhead", QKeySequence("Alt+]"), nullptr, [this] { edit_->trimToPlayhead(ClipEdge::Tail); });
    add("detach", "Detach Audio", QKeySequence("Ctrl+Alt+D"), "link", [this] { edit_->detachAudio(); });
    add("toggleClip", "Disable Clip", QKeySequence("E"), nullptr, [this] { edit_->toggleClipEnabled(); });
    // M9: the clipboard and the clip menu (Filmora's commands and keys)
    add("cut", "Cut", QKeySequence::Cut, nullptr, [this] { edit_->cutSelection(); });
    add("copy", "Copy", QKeySequence::Copy, "copy", [this] { edit_->copySelection(); });
    add("paste", "Paste", QKeySequence::Paste, "paste", [this] { edit_->paste(); });
    add("duplicate", "Duplicate", QKeySequence("Ctrl+D"), nullptr, [this] { edit_->duplicateSelection(); });
    add("selectAll", "Select All", QKeySequence::SelectAll, nullptr, [this] { edit_->selectAll(); });
    add("copyEffects", "Copy Effects", QKeySequence("Ctrl+Alt+C"), nullptr,
        [this] { edit_->copyEffects(doc_->primarySelection()); });
    add("pasteEffects", "Paste Effects", QKeySequence("Ctrl+Alt+V"), nullptr, [this] {
        for (const auto& id : doc_->selection()) edit_->pasteEffects(id);
    });
    add("cropZoom", "Crop and Zoom", QKeySequence("Alt+C"), "crop", [this] {
        if (doc_->primarySelection().empty()) showMessage("Select a video clip first", true);
        else inspector_->showCrop();
    });
    add("cropToFit", "Crop to Fit", QKeySequence("Ctrl+F"), nullptr, [this] { edit_->cropToFit(); });
    add("rotateCw", "Rotate 90 CW", QKeySequence("Ctrl+Alt+Right"), "rotateRight", [this] { edit_->rotate(90); });
    add("rotateCcw", "Rotate 90 CCW", QKeySequence("Ctrl+Alt+Left"), "rotateLeft", [this] { edit_->rotate(-90); });
    add("mute", "Mute", QKeySequence("Ctrl+Shift+M"), "mute", [this] { edit_->toggleMute(); });
    add("audioGain", "Audio Gain...", QKeySequence("G"), "volume", [this] { audioGainDialog(); });
    add("rename", "Rename...", QKeySequence("F2"), nullptr, [this] { renameClipDialog(); });
    add("selectClipRange", "Select Clip Range", QKeySequence("X"), nullptr, [this] { edit_->selectClipRange(); });
    add("replaceClip", "Replace Clip...", QKeySequence(), nullptr, [this] { replaceClipDialog(); });
    add("exportSelected", "Export Selected Clips...", QKeySequence(), "export", [this] { exportDialog(true); });
    add("locateInLibrary", "Locate in the Resource Panel", QKeySequence(), nullptr, [this] { locateInLibrary(); });
    add("selectSameColor", "Select All Clips with the Same Color Mark", QKeySequence("Alt+Shift+`"), nullptr,
        [this] { edit_->selectSameColorMark(); });
    add("prevEdit", "Previous Edit Point", QKeySequence(Qt::Key_Up), "prevKey", [this] { jumpToEditPoint(false); });
    add("nextEdit", "Next Edit Point", QKeySequence(Qt::Key_Down), "nextKey", [this] { jumpToEditPoint(true); });
    add("speedNormal", "Normal Speed", QKeySequence(), nullptr, [this] { edit_->setSpeedFactor(Rational(1)); });
    add("speedSettings", "Uniform Speed...", QKeySequence("Ctrl+R"), "speed", [this] {
        if (QTabWidget* tabs = inspector_->clipTabs()) tabs->setCurrentWidget(inspector_->speedPanel());
    });
    add("addDissolve", "Add Dissolve to Next Clip", QKeySequence(), "dissolve", [this] {
        if (doc_->primarySelection().empty()) showMessage("Select a clip first", true);
        else edit_->addTransition(doc_->primarySelection());
    });
    // M5 speed (no shortcuts: none measured in Stage 1)
    add("freezeFrame", "Freeze Frame", QKeySequence(), "freeze", [this] { edit_->freezeFrameAtPlayhead(); });
    add("reverseClip", "Reverse Clip", QKeySequence(), "reverse", [this] { edit_->toggleReverse(); });
    // no shortcuts: Filmora's own (e.g. Alt+C = Crop and Zoom) must stay free for later milestones
    add("addAdjustment", "Add Adjustment Layer", QKeySequence(), "adjustment", [this] { edit_->addAdjustmentLayer(); });
    add("addTitle", "Add Title at Playhead", QKeySequence(), "title",
        [this] { edit_->addTitleFromTemplate(text::defaultTitleTemplate().id); });
    add("addCaption", "Add Caption at Playhead", QKeySequence(), "captions",
        [this] { edit_->addTitleFromTemplate("subtitle.classic"); });
    add("addVideoTrack", "Add Video Track", QKeySequence(), "plus", [this] { edit_->addTrack(TrackKind::Video); });
    add("addAudioTrack", "Add Audio Track", QKeySequence(), "plus", [this] { edit_->addTrack(TrackKind::Audio); });

    add("play", "Play / Pause", QKeySequence(Qt::Key_Space), "play", [this] { playback_->togglePlay(); });
    add("stop", "Stop", QKeySequence("Ctrl+Space"), "stop", [this] { playback_->stop(); });
    add("prevFrame", "Previous Frame", QKeySequence(Qt::Key_Left), "prev", [this] { playback_->step(-1); });
    add("nextFrame", "Next Frame", QKeySequence(Qt::Key_Right), "next", [this] { playback_->step(1); });
    add("toStart", "Go to Start", QKeySequence(Qt::Key_Home), "start", [this] { playback_->toStart(); });
    add("toEnd", "Go to End", QKeySequence(Qt::Key_End), "end", [this] { playback_->toEnd(); });
    add("snapshot", "Snapshot...", QKeySequence("Ctrl+Alt+S"), "camera", [this] { snapshotDialog(); });
    add("markIn", "Mark In", QKeySequence("I"), nullptr, [this] { edit_->setMarkIn(); });     // M6 export range
    add("markOut", "Mark Out", QKeySequence("O"), nullptr, [this] { edit_->setMarkOut(); });
    add("clearMarks", "Clear In and Out", QKeySequence("Ctrl+Shift+X"), nullptr, [this] { edit_->clearMarks(); });
    add("renderPreview", "Render Preview", QKeySequence("Ctrl+Return"), "render", [this] {   // M5
        if (renderPreview_->busy()) renderPreview_->cancel();
        else if (renderPreview_->startForTimeline()) actions_["renderPreview"]->setText("Cancel Render Preview");
        else showMessage("Nothing to render", false);
    });
    add("clearRenderCache", "Clear Render Preview Files", QKeySequence(), nullptr, [this] {
        const double mb = renderPreview_->cache().sizeBytes() / 1048576.0;
        renderPreview_->clearCache();
        showMessage(QString("Render Preview files cleared (%1 MB)").arg(mb, 0, 'f', 0), false);
    });

    add("zoomIn", "Zoom In", QKeySequence("Ctrl+="), "zoomIn", [] {});
    add("zoomOut", "Zoom Out", QKeySequence("Ctrl+-"), "zoomOut", [] {});
    add("zoomFit", "Zoom to Fit", QKeySequence("Shift+Z"), "fit", [] {});
    auto* snapA = add("snapping", "Snapping", QKeySequence("N"), "magnet", [] {});
    snapA->setCheckable(true);
    snapA->setChecked(true);
    add("shortcuts", "Keyboard Shortcuts", QKeySequence("Ctrl+Alt+K"), nullptr, [this] { showShortcuts(); });
    add("modelManager", "Model Manager...", QKeySequence(), nullptr, [this] { modelManager(); });
    add("aiCutout", "Background Removal...", QKeySequence(), nullptr, [this] {   // M7: Video > AI Cutout of the selected clip
        inspector_->showVideoSubTab("cutout");
        if (doc_->selection().empty()) showMessage("Select a video clip on the timeline first");
    });
    add("speechToText", "Speech to Text...", QKeySequence(), nullptr, [this] { speechToText(); });
    add("sceneDetection", "Scene Detection...", QKeySequence(), nullptr, [this] { sceneDetection(); });
    actions_["sceneDetection"]->setStatusTip("Finds cuts and fades by comparing frames (signal analysis, no AI model)");
    add("autoReframe", "Auto Reframe...", QKeySequence(), nullptr, [this] { autoReframe(); });
    add("aiInterpolation", "AI Interpolation...", QKeySequence(), nullptr, [this] {   // M7: Speed tab of the clip
        if (QTabWidget* tabs = inspector_->clipTabs()) tabs->setCurrentWidget(inspector_->speedPanel());
        if (doc_->selection().empty()) showMessage("Select a video clip, slow it down, then choose AI Interpolation (RIFE)");
    });
    add("videoUpscaling", "Video Upscaling...", QKeySequence(), nullptr, [this] {   // M7: Video > Upscale
        inspector_->showVideoSubTab("upscale");
        if (doc_->selection().empty()) showMessage("Select a video clip on the timeline first");
    });
    add("about", QString("About %1").arg(kAppName), QKeySequence(), "logo", [this] {
        QMessageBox box(QMessageBox::NoIcon, QString("About %1").arg(kAppName), aboutText(), QMessageBox::Ok, this);
        box.setIconPixmap(icon("logo").pixmap(64, 64));
        box.setTextFormat(Qt::RichText);
        box.setTextInteractionFlags(Qt::TextBrowserInteraction);   // the GitHub link can be clicked
        if (auto* label = box.findChild<QLabel*>("qt_msgbox_label")) label->setOpenExternalLinks(true);
        box.exec();
    });
    // the author's official GitHub (Help menu)
    add("github", QString("%1 on GitHub (%2)").arg(QString::fromLatin1(kAppName), QString::fromLatin1(kAuthorName)), QKeySequence(), nullptr,
        [] { QDesktopServices::openUrl(QUrl(QString::fromLatin1(kAuthorUrl))); });
    actions_["github"]->setData(QString::fromLatin1(kAuthorUrl));
    actions_["github"]->setStatusTip(QString("Opens %1 - %2's official GitHub").arg(QString::fromLatin1(kAuthorUrl), QString::fromLatin1(kAppName)));
}

void MainWindow::buildMenus() {
    QMenuBar* mb = menuBar();
    QMenu* file = mb->addMenu("&File");
    for (const char* n : {"new", "open"}) file->addAction(actions_[n]);
    recentMenu_ = file->addMenu("Open Recent");
    file->addSeparator();
    file->addAction(actions_["import"]);
    file->addAction(actions_["importSubtitles"]);
    file->addSeparator();
    for (const char* n : {"save", "saveAs"}) file->addAction(actions_[n]);
    file->addSeparator();
    file->addAction(actions_["export"]);
    file->addAction(actions_["exportSubtitles"]);
    file->addAction(actions_["settings"]);
    file->addSeparator();
    file->addAction(actions_["recoverAutosave"]);
    file->addSeparator();
    file->addAction(actions_["exit"]);
    rebuildRecentMenu();

    QMenu* edit = mb->addMenu("&Edit");
    for (const char* n : {"undo", "redo"}) edit->addAction(actions_[n]);
    edit->addSeparator();
    for (const char* n : {"cut", "copy", "paste", "duplicate"}) edit->addAction(actions_[n]);
    edit->addSeparator();
    for (const char* n : {"split", "trimStart", "trimEnd"}) edit->addAction(actions_[n]);
    edit->addSeparator();
    for (const char* n : {"rippleDelete", "delete", "selectAll"}) edit->addAction(actions_[n]);
    edit->addSeparator();
    for (const char* n : {"copyEffects", "pasteEffects"}) edit->addAction(actions_[n]);
    edit->addSeparator();
    for (const char* n : {"addVideoTrack", "addAudioTrack"}) edit->addAction(actions_[n]);
    edit->addSeparator();
    edit->addAction(actions_["preferences"]);

    // M9: Filmora's Tools menu - what can be done to the selected clips
    QMenu* tools = mb->addMenu("&Tools");
    tools->setObjectName("toolsMenu");
    for (const char* n : {"cropZoom", "cropToFit", "rotateCw", "rotateCcw"}) tools->addAction(actions_[n]);
    tools->addSeparator();
    QMenu* speed = tools->addMenu(icon("speed"), "Speed");
    fillSpeedMenu(speed);
    tools->addAction(actions_["reverseClip"]);
    tools->addAction(actions_["freezeFrame"]);
    tools->addSeparator();
    QMenu* audio = tools->addMenu(icon("volume"), "Audio");
    for (const char* n : {"detach", "mute", "audioGain"}) audio->addAction(actions_[n]);
    tools->addSeparator();
    for (const char* n : {"addDissolve", "addTitle", "addCaption", "addAdjustment"}) tools->addAction(actions_[n]);
    tools->addSeparator();
    for (const char* n : {"replaceClip", "toggleClip", "rename", "selectClipRange", "locateInLibrary", "selectSameColor"})
        tools->addAction(actions_[n]);
    tools->addSeparator();
    tools->addAction(actions_["exportSelected"]);

    QMenu* view = mb->addMenu("&View");
    for (const char* n : {"zoomIn", "zoomOut", "zoomFit", "snapping"}) view->addAction(actions_[n]);

    QMenu* play = mb->addMenu("&Playback");
    for (const char* n : {"play", "stop", "prevFrame", "nextFrame", "prevEdit", "nextEdit", "toStart", "toEnd"})
        play->addAction(actions_[n]);
    play->addSeparator();
    for (const char* n : {"markIn", "markOut", "clearMarks"}) play->addAction(actions_[n]);
    play->addSeparator();
    play->addAction(actions_["snapshot"]);
    play->addSeparator();
    play->addAction(actions_["renderPreview"]);
    play->addAction(actions_["clearRenderCache"]);

    // M7 local AI: every tool runs on this PC; models are installed through the Model Manager
    QMenu* ai = mb->addMenu("&AI Tools");
    ai->setObjectName("aiMenu");
    ai->addAction(actions_["aiCutout"]);
    ai->addAction(actions_["speechToText"]);
    ai->addAction(actions_["autoReframe"]);
    ai->addAction(actions_["aiInterpolation"]);
    ai->addAction(actions_["videoUpscaling"]);
    ai->addSection("Signal analysis (no AI model)");
    ai->addAction(actions_["sceneDetection"]);
    ai->addSeparator();
    ai->addAction(actions_["modelManager"]);

    QMenu* help = mb->addMenu("&Help");
    help->addAction(actions_["shortcuts"]);
    help->addSeparator();
    help->addAction(actions_["github"]);
    help->addAction(actions_["about"]);

    // right corner: project name + accent Export button (Filmora's familiar placement)
    auto* corner = new QWidget(mb);
    auto* cl = new QHBoxLayout(corner);
    cl->setContentsMargins(0, 2, 8, 2);
    titleLabel_ = new QLabel(corner);
    titleLabel_->setStyleSheet(QString("color: %1; padding-right: 18px;").arg(color::textDim.name()));
    auto* exportBtn = new QPushButton(icon("export", QColor(0x1A, 0x14, 0x08)), "Export", corner);
    exportBtn->setObjectName("accentButton");
    exportBtn->setToolTip("Export the timeline to MP4 (Ctrl+E)");
    connect(exportBtn, &QPushButton::clicked, this, [this] { exportDialog(); });
    cl->addWidget(titleLabel_);
    cl->addWidget(exportBtn);
    mb->setCornerWidget(corner, Qt::TopRightCorner);
    auto* logo = new QLabel(mb);
    logo->setPixmap(icon("logo").pixmap(22, 22));
    logo->setContentsMargins(8, 0, 6, 0);
    mb->setCornerWidget(logo, Qt::TopLeftCorner);
}

void MainWindow::buildLayout() {
    // Only pages that work are shown (Filmora's Stock Media / Audio / Effects / Filters / Stickers / Templates
    // pages belong to later milestones).
    resources_ = new ResourceTabs(this);
    media_ = new MediaPanel(doc_, edit_, mediaService_, resources_);
    titles_ = new TitlesPanel(doc_, edit_, titleLibrary_, resources_);
    transitions_ = new TransitionsPanel(doc_, edit_, resources_);
    audioPage_ = new AudioPage(doc_, edit_, opt_.silentAudio, resources_);           // M9
    stickersPage_ = new StickersPage(doc_, edit_, resources_);
    templatesPage_ = new TemplatesPage(doc_, edit_, resources_);
    resources_->addPage(media_, "media", "Media");
    resources_->addPage(audioPage_, "music", "Audio");
    resources_->addPage(titles_, "title", "Titles");
    resources_->addPage(transitions_, "transitions", "Transitions");
    effectsPage_ = new EffectsBrowser(EffectsBrowser::Page::Effects, doc_, edit_, edit_->effectPresets(), resources_);
    filtersPage_ = new EffectsBrowser(EffectsBrowser::Page::Filters, doc_, edit_, edit_->effectPresets(), resources_);
    resources_->addPage(effectsPage_, "effects", "Effects");
    resources_->addPage(filtersPage_, "filters", "Filters");
    resources_->addPage(stickersPage_, "sticker", "Stickers");
    resources_->addPage(templatesPage_, "template", "Templates");
    connect(media_, &MediaPanel::importRequested, this, &MainWindow::importDialog);
    connect(media_, &MediaPanel::filesDropped, this, [this](const QStringList& files) { importFiles(files); });
    connect(media_, &MediaPanel::relinkRequested, this, &MainWindow::relink);

    preview_ = new PreviewPanel(doc_, edit_, playback_, this);
    connect(preview_, &PreviewPanel::snapshotRequested, this, &MainWindow::snapshotDialog);
    analysis_ = new AnalysisService(this);
    inspector_ = new InspectorPanel(doc_, edit_, analysis_, this);
    connect(inspector_, &InspectorPanel::seekRequested, playback_, &PlaybackController::seek);
    connect(inspector_->cutoutPanel(), &CutoutPanel::addBackgroundMediaRequested, this, &MainWindow::addBackgroundMedia);

    TimelineActions ta;
    ta.undo = actions_["undo"];
    ta.redo = actions_["redo"];
    ta.del = actions_["delete"];
    ta.rippleDelete = actions_["rippleDelete"];
    ta.split = actions_["split"];
    ta.snapping = actions_["snapping"];
    ta.zoomIn = actions_["zoomIn"];
    ta.zoomOut = actions_["zoomOut"];
    ta.zoomFit = actions_["zoomFit"];
    ta.renderPreview = actions_["renderPreview"];
    ta.named = &actions_;
    ta.fillSpeedMenu = [this](QMenu* m) { fillSpeedMenu(m); };
    timeline_ = new TimelinePanel(doc_, edit_, mediaService_, playback_, ta, this);
    connect(timeline_->view(), &TimelineView::videoSubTabRequested, inspector_, &InspectorPanel::showVideoSubTab);   // M5
    connect(timeline_->view(), &TimelineView::aiToolRequested, this, [this](const QString& name) {   // M7
        if (QAction* a = action(name)) a->trigger();
    });
    connect(timeline_->view(), &TimelineView::speedTabRequested, this, [this] {   // M5
        if (QTabWidget* tabs = inspector_->clipTabs()) tabs->setCurrentWidget(inspector_->speedPanel());
    });
    connect(timeline_->view(), &TimelineView::subtitleFilesDropped, this, [this](const QStringList& files, Time at) {
        for (const QString& f : files) {
            QStringList warnings;
            if (importSubtitlesFile(f, 0, at, &warnings) && !warnings.isEmpty() && opt_.interactive)
                QMessageBox::information(this, "Import Subtitles",
                                         QFileInfo(f).fileName() + " - not imported exactly:\n\n" + warnings.join("\n"));
        }
    });

    auto frame = [](QWidget* w) {
        auto* f = new QFrame();
        f->setObjectName("panel");
        auto* l = new QVBoxLayout(f);
        l->setContentsMargins(0, 0, 0, 0);
        l->addWidget(w);
        return f;
    };
    // Filmora 16 default proportions (reference screenshot): resources 46 %, player 29 %, inspector 25 %;
    // top area 60 % of the height. The user's own splitter positions are remembered.
    auto* top = new QSplitter(Qt::Horizontal);
    top->setObjectName("topSplitter");
    top->addWidget(frame(resources_));
    top->addWidget(frame(preview_));
    top->addWidget(frame(inspector_));
    top->setStretchFactor(0, 46);
    top->setStretchFactor(1, 29);
    top->setStretchFactor(2, 25);
    top->setSizes({4600, 2900, 2500});
    auto* vert = new QSplitter(Qt::Vertical);
    vert->setObjectName("verticalSplitter");
    vert->addWidget(top);
    vert->addWidget(frame(timeline_));
    vert->setStretchFactor(0, 60);
    vert->setStretchFactor(1, 40);
    vert->setSizes({6000, 4000});
    vert->setContentsMargins(6, 4, 6, 4);
    topSplit_ = top;
    vertSplit_ = vert;
    if (opt_.interactive) {
        QSettings st;
        if (st.contains("layout/top")) top->restoreState(st.value("layout/top").toByteArray());
        if (st.contains("layout/vertical")) vert->restoreState(st.value("layout/vertical").toByteArray());
    }
    setCentralWidget(vert);
    statusBar()->showMessage("Ready - import media to begin (Ctrl+I)");
    backendLabel_ = new QLabel(this);
    backendLabel_->setObjectName("backendLabel");
    backendLabel_->setStyleSheet(QString("color: %1; padding: 0 8px;").arg(color::textDim.name()));
    statusBar()->addPermanentWidget(backendLabel_);
    // M5 Render Preview: progress in the status bar, rendered frames under the timeline ruler
    renderPreview_ = new RenderPreviewController(doc_, playback_, this);
    renderProgress_ = new QProgressBar(this);
    renderProgress_->setObjectName("renderPreviewProgress");
    renderProgress_->setMaximumWidth(220);
    renderProgress_->setFormat("Render Preview %p%");
    // no taller than the status text: the status bar (and so the editor above it) keeps its height
    renderProgress_->setMaximumHeight(backendLabel_->sizeHint().height());
    renderProgress_->hide();
    statusBar()->addPermanentWidget(renderProgress_);
    connect(renderPreview_, &RenderPreviewController::progress, this, [this](qint64 done, qint64 total) {
        renderProgress_->setRange(0, static_cast<int>(std::max<qint64>(1, total)));
        renderProgress_->setValue(static_cast<int>(done));
        renderProgress_->show();
        actions_["renderPreview"]->setText("Cancel Render Preview");
    });
    connect(renderPreview_, &RenderPreviewController::finished, this, [this](bool completed, const QString& msg) {
        renderProgress_->hide();
        actions_["renderPreview"]->setText("Render Preview");
        showMessage(msg, !completed && !msg.contains("cancelled"));
    });
    connect(renderPreview_, &RenderPreviewController::rangesChanged, this,
            [this] { timeline_->view()->setRenderedRanges(renderPreview_->renderedRanges()); });
    timeline_->view()->setRenderedRanges(renderPreview_->renderedRanges());
    const RenderSettings rs = loadRenderSettings();
    playback_->setRenderSettings(rs.backend, rs.hardwareDecode);
    auto* backendPoll = new QTimer(this);   // the worker creates its backend lazily: show what it really uses
    backendPoll->setInterval(1000);
    connect(backendPoll, &QTimer::timeout, this, [this] {
        backendLabel_->setText("Preview: " + playback_->backendName());
        backendLabel_->setToolTip("Compositing backend of the preview (Edit > Preferences)");
    });
    backendPoll->start();
}

void MainWindow::updateTitle() {
    const QString name = doc_->displayName() + (doc_->isModified() ? " *" : "");
    setWindowTitle(name + " - " + kAppName);
    if (titleLabel_) titleLabel_->setText(name);
}

void MainWindow::updateActions() {
    actions_["undo"]->setEnabled(doc_->undoStack().canUndo());
    actions_["redo"]->setEnabled(doc_->undoStack().canRedo());
    actions_["undo"]->setText(doc_->undoStack().canUndo() ? "Undo " + QString::fromStdString(doc_->undoStack().undoLabel()) : "Undo");
    actions_["redo"]->setText(doc_->undoStack().canRedo() ? "Redo " + QString::fromStdString(doc_->undoStack().redoLabel()) : "Redo");
    const bool sel = !doc_->selection().empty();
    for (const char* n : {"rippleDelete", "detach", "toggleClip", "addDissolve", "cut", "copy", "duplicate", "copyEffects",
                          "pasteEffects", "cropZoom", "cropToFit", "rotateCw", "rotateCcw", "mute", "audioGain", "rename",
                          "selectClipRange", "replaceClip", "exportSelected", "locateInLibrary", "selectSameColor"})
        actions_[n]->setEnabled(sel);
    actions_["paste"]->setEnabled(edit_->canPaste());
    if (const Clip* c = edit::findClip(doc_->sequence(), doc_->primarySelection())) {
        actions_["toggleClip"]->setText(c->enabled ? "Disable Clip" : "Enable Clip");
        actions_["mute"]->setText(c->muted ? "Unmute" : "Mute");
    }
    actions_["delete"]->setEnabled(sel || !doc_->selectedTransition().empty());
    bool underPlayhead = false;
    const Time t = doc_->playheadTime();
    for (const auto& tr : doc_->sequence().tracks)
        for (const auto& c : tr.clips)
            if (c.timelineStart < t && t < c.timelineEnd()) underPlayhead = true;
    actions_["split"]->setEnabled(underPlayhead);
    actions_["export"]->setEnabled(doc_->sequence().frameCount() > 0);
}

// ---- file operations -----------------------------------------------------------------------------

void MainWindow::offerRecovery(bool fromMenu) {
    const auto sessions = autosave_->recoverableSessions();
    if (sessions.empty()) {
        if (fromMenu && opt_.interactive)
            QMessageBox::information(this, "Recover Autosaved Projects", "There is nothing to recover: every session ended normally.");
        return;
    }
    if (!opt_.interactive) return;
    if (!fromMenu && doc_->isModified()) {   // e.g. media opened from the command line: ask later, from the menu
        showMessage(QString("%1 autosaved project(s) from a session that did not end normally: File > Recover Autosaved Projects")
                        .arg(sessions.size()),
                    true);
        return;
    }
    if (fromMenu && !confirmDiscard()) return;
    const AutosaveManager::Session& s = sessions.front();
    QMessageBox box(QMessageBox::Question, "Recover project",
                    QString("MeerCut did not close normally. \"%1\"%2 has unsaved changes, autosaved at %3.\n\n"
                            "Recover them? (They open as unsaved changes; Discard keeps a copy in %4.)")
                        .arg(s.displayName)
                        .arg(s.originalPath.isEmpty() ? QString() : " (" + QDir::toNativeSeparators(s.originalPath) + ")")
                        .arg(QLocale().toString(s.savedAt, QLocale::ShortFormat))
                        .arg(QDir::toNativeSeparators(AutosaveManager::folder() + "/discarded")),
                    QMessageBox::NoButton, this);
    QPushButton* recover = box.addButton("Recover", QMessageBox::AcceptRole);
    QPushButton* discard = box.addButton("Discard", QMessageBox::DestructiveRole);
    box.addButton("Later", QMessageBox::RejectRole);
    box.setDefaultButton(recover);
    box.exec();
    if (box.clickedButton() == recover) {
        QString err;
        if (autosave_->recover(s, &err)) showMessage("Recovered " + s.displayName + " - save it to keep the changes");
        else QMessageBox::warning(this, "Recover project", "The autosaved copy cannot be opened:\n\n" + err);
    } else if (box.clickedButton() == discard) {
        autosave_->discard(s);
        showMessage("Autosaved copy moved to " + QDir::toNativeSeparators(AutosaveManager::folder() + "/discarded"));
    }
    if (sessions.size() > 1)
        showMessage(QString("%1 more autosaved project(s): File > Recover Autosaved Projects").arg(sessions.size() - 1));
}

bool MainWindow::confirmDiscard() {
    if (!doc_->isModified() || !opt_.interactive) return true;
    const auto r = QMessageBox::question(this, kAppName, "Save changes to " + doc_->displayName() + "?",
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Save) return save();
    return r == QMessageBox::Discard;
}

void MainWindow::newProject() {
    if (!confirmDiscard()) return;
    SequenceSettingsDialog d(SequenceSettings{}, "New Project", this);
    if (opt_.interactive && d.exec() != QDialog::Accepted) return;
    playback_->pause();
    doc_->newProject(d.settings());
    showMessage("New project created");
}

bool MainWindow::openProjectFile(const QString& path) {
    playback_->pause();
    QString err;
    if (!doc_->open(path, &err)) {
        showMessage("Cannot open project: " + err, true);
        if (opt_.interactive) QMessageBox::warning(this, kAppName, "Cannot open the project:\n\n" + err);
        return false;
    }
    addRecent(doc_->filePath());
    timeline_->view()->zoomToFit();
    showMessage("Opened " + QDir::toNativeSeparators(path));
    checkMissingMedia();
    return true;
}

void MainWindow::openProject() {
    if (!confirmDiscard()) return;
    const QString path = QFileDialog::getOpenFileName(this, "Open Project", QString(), kProjectFilter);
    if (!path.isEmpty()) openProjectFile(path);
}

bool MainWindow::saveProjectTo(const QString& path) {
    QString err;
    if (!doc_->save(path, &err)) {
        showMessage("Save failed: " + err, true);
        if (opt_.interactive) QMessageBox::warning(this, kAppName, "Cannot save the project:\n\n" + err);
        return false;
    }
    addRecent(doc_->filePath());
    showMessage("Saved " + QDir::toNativeSeparators(doc_->filePath()));
    return true;
}

bool MainWindow::save() { return doc_->filePath().isEmpty() ? saveAs() : saveProjectTo(doc_->filePath()); }

bool MainWindow::saveAs() {
    QString path = QFileDialog::getSaveFileName(this, "Save Project As", doc_->displayName() + ".mcproj", kProjectFilter);
    if (path.isEmpty()) return false;
    return saveProjectTo(path);
}

std::vector<std::string> MainWindow::importFiles(const QStringList& all) {
    QStringList files;   // subtitle files become captions (at their own times); the rest is media
    for (const QString& f : all) {
        const QString ext = QFileInfo(f).suffix().toLower();
        if (ext == "srt" || ext == "ass" || ext == "ssa") {
            QStringList warnings;
            if (importSubtitlesFile(f, 0, Time(0), &warnings) && !warnings.isEmpty() && opt_.interactive)
                QMessageBox::information(this, "Import Subtitles",
                                         QFileInfo(f).fileName() + " - not imported exactly:\n\n" + warnings.join("\n"));
        } else {
            files << f;
        }
    }
    if (files.isEmpty()) return {};
    QStringList errors, notices;
    const auto ids = doc_->importMedia(files, &errors, &notices);
    if (!errors.isEmpty()) {
        showMessage("Some files could not be imported: " + errors.join("; "), true);
        if (opt_.interactive) QMessageBox::warning(this, "Import", "These files could not be imported:\n\n" + errors.join("\n"));
    } else if (!notices.isEmpty()) {
        showMessage(notices.join("; "), true);   // HDR sources: say they are converted to SDR
    } else if (!ids.empty()) {
        showMessage(QString("Imported %1 file(s)").arg(files.size()));
    }
    return ids;
}

void MainWindow::importDialog() {
    QSettings s;
    const QStringList files = QFileDialog::getOpenFileNames(this, "Import Media", s.value("lastImportDir").toString(), kMediaFilter);
    if (files.isEmpty()) return;
    s.setValue("lastImportDir", QFileInfo(files.front()).absolutePath());
    importFiles(files);
    resources_->setCurrentWidget(media_);
}


// ---- subtitles -----------------------------------------------------------------------------------------

namespace {
QByteArray readAll(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "cannot read " + QDir::toNativeSeparators(path);
        return {};
    }
    return f.readAll();
}
}  // namespace

bool MainWindow::addSubtitleDoc(const SubtitleDoc& sd, Time offset, const QString& name) {
    if (sd.captions.empty()) {
        showMessage(name + ": no captions found", true);
        return false;
    }
    std::vector<Caption> caps = sd.captions;
    for (auto& c : caps) {
        c.start = c.start + offset;
        c.end = c.end + offset;
    }
    std::vector<std::string> tracks;
    std::unique_ptr<Command> cmd;
    try {
        cmd = edit::addCaptions(doc_->commandTarget(), doc_->sequenceId(), std::move(caps), sd.style, sd.subtitleY, &tracks);
    } catch (const std::exception& e) {
        showMessage(name + ": " + QString::fromUtf8(e.what()), true);
        return false;
    }
    QString err;
    if (!doc_->execute(std::move(cmd), &err)) return false;
    showMessage(QString("Imported %1 caption(s) from %2 on %3 subtitle track(s)%4")
                    .arg(sd.captions.size())
                    .arg(name)
                    .arg(tracks.size())
                    .arg(sd.warnings.empty() ? QString() : QString(" - %1 note(s) on styling not imported exactly")
                                                               .arg(sd.warnings.size())));
    return true;
}

bool MainWindow::importSubtitlesFile(const QString& path, unsigned codePage, Time offset, QStringList* warnings) {
    QString err;
    const QByteArray bytes = readAll(path, &err);
    if (!err.isEmpty()) {
        showMessage(err, true);
        return false;
    }
    SubtitleDoc sd;
    try {
        sd = parseSubtitles(std::string_view(bytes.constData(), static_cast<size_t>(bytes.size())),
                            QFileInfo(path).fileName().toStdString(), codePage);
    } catch (const std::exception& e) {
        showMessage(QFileInfo(path).fileName() + ": " + QString::fromUtf8(e.what()), true);
        return false;
    }
    if (warnings)
        for (const auto& w : sd.warnings) *warnings << QString::fromStdString(w);
    return addSubtitleDoc(sd, offset, QFileInfo(path).fileName());
}

void MainWindow::importSubtitlesDialog() {
    QSettings s;
    const QString file = QFileDialog::getOpenFileName(this, "Import Subtitles", s.value("lastSubtitleDir").toString(),
                                                      "Subtitles (*.srt *.ass *.ssa);;All files (*)");
    if (file.isEmpty()) return;
    s.setValue("lastSubtitleDir", QFileInfo(file).absolutePath());
    QString err;
    const QByteArray bytes = readAll(file, &err);
    if (!err.isEmpty()) {
        showMessage(err, true);
        return;
    }
    SubtitleImportDialog d(file, bytes, doc_->playheadTime(), this);
    if (d.exec() != QDialog::Accepted) return;
    addSubtitleDoc(d.document(), d.offset(), QFileInfo(file).fileName());
}

bool MainWindow::exportSubtitlesTo(const QString& path, std::string trackId, QString* error) {
    auto fail = [&](const QString& m) {
        if (error) *error = m;
        showMessage(m, true);
        return false;
    };
    const Sequence& seq = doc_->sequence();
    const Track* tr = trackId.empty() ? nullptr : seq.findTrack(trackId);
    if (!tr && !doc_->primarySelection().empty()) {
        const Track* t = nullptr;
        if (edit::findClip(seq, doc_->primarySelection(), &t) && t->kind == TrackKind::Subtitle) tr = t;
    }
    if (!tr)
        for (const auto& t : seq.tracks)
            if (t.kind == TrackKind::Subtitle && !tr) tr = &t;
    if (!tr || tr->kind != TrackKind::Subtitle) return fail("There are no subtitles to export");
    const std::vector<Caption> caps = captionsOfTrack(*tr);
    if (caps.empty()) return fail(QString::fromStdString(tr->name) + " has no captions");
    const QString ext = QFileInfo(path).suffix().toLower();
    const std::string text = ext == "ass" || ext == "ssa"
                                 ? writeAss(caps, tr->subtitleStyle, tr->subtitleY, seq.width, seq.height)
                                 : writeSrt(caps, tr->subtitleStyle);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(text.data(), static_cast<qint64>(text.size())) < 0 || !f.commit())
        return fail("cannot write " + QDir::toNativeSeparators(path));
    showMessage(QString("Exported %1 caption(s) from %2 to %3")
                    .arg(caps.size())
                    .arg(QString::fromStdString(tr->name))
                    .arg(QFileInfo(path).fileName()));
    return true;
}

void MainWindow::exportSubtitlesDialog() {
    const Sequence& seq = doc_->sequence();
    std::vector<const Track*> subs;
    for (const auto& t : seq.tracks)
        if (t.kind == TrackKind::Subtitle && !t.clips.empty()) subs.push_back(&t);
    if (subs.empty()) {
        showMessage("There are no subtitles to export", true);
        if (opt_.interactive) QMessageBox::information(this, "Export Subtitles", "The timeline has no captions to export.");
        return;
    }
    std::string trackId;
    const Track* sel = nullptr;
    if (!doc_->primarySelection().empty()) edit::findClip(seq, doc_->primarySelection(), &sel);
    if (sel && sel->kind == TrackKind::Subtitle) trackId = sel->id;
    else if (subs.size() == 1) trackId = subs.front()->id;
    else {
        QStringList names;
        for (const Track* t : subs) names << QString::fromStdString(t->name);
        bool ok = false;
        const QString pick = QInputDialog::getItem(this, "Export Subtitles", "Subtitle track", names, 0, false, &ok);
        if (!ok) return;
        trackId = subs[static_cast<size_t>(names.indexOf(pick))]->id;
    }
    QSettings s;
    const QString base = QFileInfo(doc_->displayName()).completeBaseName();
    const QString dir = s.value("lastSubtitleDir", QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).toString();
    QString selected;
    const QString file = QFileDialog::getSaveFileName(this, "Export Subtitles", QDir(dir).filePath(base + ".srt"),
                                                      "SubRip (*.srt);;Advanced SubStation Alpha (*.ass)", &selected);
    if (file.isEmpty()) return;
    s.setValue("lastSubtitleDir", QFileInfo(file).absolutePath());
    exportSubtitlesTo(file, trackId);
}

void MainWindow::exportDialog(bool selectedClips) {
    playback_->pause();
    ExportDialog d(doc_, this);
    if (selectedClips) d.selectRange(ExportDialog::Range::Selected);
    d.exec();
}

void MainWindow::fillSpeedMenu(QMenu* m) {
    m->addAction(actions_["speedSettings"]);
    m->addAction(actions_["speedNormal"]);
    QMenu* slow = m->addMenu("Slow");
    for (const auto& [label, r] : {std::pair{"0.5x", Rational(1, 2)}, {"0.25x", Rational(1, 4)}, {"0.1x", Rational(1, 10)}})
        slow->addAction(label, [this, r] { edit_->setSpeedFactor(r); });
    QMenu* fast = m->addMenu("Fast");
    for (const auto& [label, r] : {std::pair{"2x", Rational(2)}, {"4x", Rational(4)}, {"8x", Rational(8)}})
        fast->addAction(label, [this, r] { edit_->setSpeedFactor(r); });
    m->addAction(actions_["freezeFrame"]);
}

void MainWindow::renameClipDialog() {
    const Clip* c = edit::findClip(doc_->sequence(), doc_->primarySelection());
    if (!c) {
        showMessage("Select a clip first", true);
        return;
    }
    const std::string id = c->id;
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Rename", "Clip name:", QLineEdit::Normal, QString::fromStdString(c->name), &ok);
    if (ok && !name.trimmed().isEmpty()) edit_->renameClip(id, name.trimmed());
}

void MainWindow::audioGainDialog() {
    std::vector<std::string> ids;   // the selected clips' sound: audio clips, or the audio linked to a video clip
    for (const auto& sid : doc_->selection())
        for (const auto& r : edit::linkGroup(doc_->sequence(), sid)) {
            const Track* tr = nullptr;
            if (edit::findClip(doc_->sequence(), r.clipId, &tr) && tr->kind == TrackKind::Audio &&
                std::find(ids.begin(), ids.end(), r.clipId) == ids.end())
                ids.push_back(r.clipId);
        }
    if (ids.empty()) {
        showMessage("Select a clip with sound first", true);
        return;
    }
    const Clip* first = edit::findClip(doc_->sequence(), ids.front());
    bool ok = false;
    const double db = QInputDialog::getDouble(this, "Audio Gain", "Gain (dB):", first->volumeDb.constantValue(), -60.0, 20.0, 1,
                                              &ok, Qt::WindowFlags(), 0.5);
    if (!ok) return;
    for (const auto& id : ids) edit_->setClipVolume(id, db);
}

void MainWindow::replaceClipDialog() {
    const std::string clipId = doc_->primarySelection();
    const Track* tr = nullptr;
    const Clip* c = edit::findClip(doc_->sequence(), clipId, &tr);
    if (!c) {
        showMessage("Select a clip first", true);
        return;
    }
    QStringList names;
    std::vector<std::string> ids;
    for (const auto& s : doc_->project().sources) {
        if (s.id == c->sourceId) continue;
        if (tr->kind == TrackKind::Video ? !s.hasVideo : !s.hasAudio) continue;
        names << (s.name.empty() ? QFileInfo(QString::fromUtf8(s.path)).fileName() : QString::fromStdString(s.name));
        ids.push_back(s.id);
    }
    names << "Import a file...";
    bool ok = false;
    const QString pick = QInputDialog::getItem(this, "Replace Clip", "Replace with:", names, 0, false, &ok);
    if (!ok) return;
    std::string sourceId;
    const int i = static_cast<int>(names.indexOf(pick));
    if (i >= 0 && i < static_cast<int>(ids.size())) {
        sourceId = ids[static_cast<size_t>(i)];
    } else {
        const QString file = QFileDialog::getOpenFileName(this, "Replace Clip", QString(), kMediaFilter);
        if (file.isEmpty()) return;
        const auto imported = importFiles({file});
        if (imported.empty()) return;
        sourceId = imported.front();
    }
    edit_->replaceClip(clipId, sourceId);
}

void MainWindow::locateInLibrary() {
    const Clip* c = edit::findClip(doc_->sequence(), doc_->primarySelection());
    if (!c || c->sourceId.empty()) {
        showMessage("Select a media clip first", true);
        return;
    }
    resources_->setCurrentWidget(media_);
    media_->selectSource(QString::fromStdString(c->sourceId));
}

// Up / Down: the playhead to the previous / next clip start or end on any track (Filmora: edit points)
void MainWindow::jumpToEditPoint(bool next) {
    const Time now = doc_->playheadTime();
    std::optional<Time> best;
    for (const auto& tr : doc_->sequence().tracks)
        for (const auto& c : tr.clips)
            for (const Time& t : {c.timelineStart, c.timelineEnd()}) {
                if (next ? t <= now : t >= now) continue;
                if (!best || (next ? t < *best : t > *best)) best = t;
            }
    if (!best) best = next ? doc_->sequence().duration() : Time(0);
    playback_->seek(timeToFrame(*best, doc_->sequence().frameRate, Rounding::Nearest));
}

bool MainWindow::exportTo(const QString& file, QString* error) {
    playback_->pause();
    ExportDialog d(doc_, this);
    return d.exportTo(file, error);
}

void MainWindow::addBackgroundMedia(const QString& clipId) {
    // videos only: still pictures are not yet a kind of clip in MeerCut
    const QString file = QFileDialog::getOpenFileName(this, "Background Video", QString(),
                                                      "Videos (*.mp4 *.mov *.mkv *.webm *.avi *.m4v)");
    if (file.isEmpty()) return;
    const auto ids = importFiles({file});
    if (ids.empty()) return;
    if (edit_->addBackgroundMedia(clipId.toStdString(), ids.front())) showMessage("Background added below the clip");
}

void MainWindow::modelManager() {
    ModelManagerDialog d(this);
    d.exec();
}

// Scene Detection and Auto Reframe: modal, their analysis runs on a worker thread (the window stays responsive).
void MainWindow::sceneDetection() {
    SceneDetectionDialog d(doc_, edit_, this);
    d.exec();
}

void MainWindow::autoReframe() {
    AutoReframeDialog d(doc_, this);
    d.exec();
}

// Modeless: editing goes on while a long recording is transcribed (the dialog works on a snapshot).
void MainWindow::speechToText() {
    auto* d = findChild<SpeechToTextDialog*>("speechToText");
    if (!d) {
        d = new SpeechToTextDialog(doc_, this);
        connect(d, &SpeechToTextDialog::captionsAdded, this, [this](int, const QString& s) { showMessage(s); });
    }
    d->show();
    d->raise();
    d->activateWindow();
}

void MainWindow::preferences() {
    RenderCache& cache = renderPreview_->cache();
    const QString cacheInfo = QString("Location: %1\nIn use: %2 frames, %3 MB")
                                  .arg(QString::fromStdWString(cache.dir().wstring()))
                                  .arg(cache.frameCount())
                                  .arg(cache.sizeBytes() / 1048576.0, 0, 'f', 0);
    PreferencesDialog d(loadRenderSettings(), playback_->backendName(), cacheInfo, this);
    if (d.exec() != QDialog::Accepted) return;
    const RenderSettings s = d.settings();
    saveRenderSettings(s);
    playback_->setRenderSettings(s.backend, s.hardwareDecode);
    if (qEnvironmentVariableIsEmpty("MEERCUT_RENDER_CACHE_MB")) cache.setLimit(static_cast<uint64_t>(s.renderCacheLimitGb) << 30);
    if (opt_.interactive) autosave_->setIntervalSeconds(s.autosaveMinutes * 60);
    showMessage("Preferences saved");
}

void MainWindow::projectSettings() {
    const Sequence& s = doc_->sequence();
    SequenceSettings cur{s.width, s.height, s.frameRate, s.sampleRate};
    SequenceSettingsDialog d(cur, "Project Settings", this);
    if (d.exec() != QDialog::Accepted) return;
    const SequenceSettings n = d.settings();
    doc_->execute(makeModifySequence(doc_->sequenceId(), "Project settings", [n](Sequence& q) {
        q.width = n.width;
        q.height = n.height;
        q.frameRate = n.frameRate;
        q.sampleRate = n.sampleRate;
    }));
}

bool MainWindow::saveSnapshot(const QString& pngPath) {
    // full-resolution render of the playhead frame with the export pipeline
    try {
        const ProjectSnapshot snap = doc_->snapshot();
        DecoderPool pool(snap.projectFile);
        auto backend = makeCpuBackend();
        auto titles = text::makeTitleEngine();
        const Image& img = renderFrame(doc_->sequence(), doc_->playhead(), *snap.project, pool, *backend, nullptr,
                                       nullptr, 0, 0, titles.get());
        QImage q(img.pixels.data(), img.width, img.height, img.stride(), QImage::Format_RGBA8888);
        if (!q.save(pngPath, "PNG")) throw std::runtime_error("cannot write " + pngPath.toStdString());
        showMessage("Snapshot saved: " + QDir::toNativeSeparators(pngPath));
        return true;
    } catch (const std::exception& e) {
        showMessage(QString("Snapshot failed: ") + e.what(), true);
        return false;
    }
}

void MainWindow::snapshotDialog() {
    const QString def = doc_->displayName() + "_" + QString::fromStdString(formatTimecode(doc_->playhead(), doc_->sequence().frameRate)).replace(':', '-') + ".png";
    const QString path = QFileDialog::getSaveFileName(this, "Save Snapshot", def, "PNG image (*.png)");
    if (!path.isEmpty()) saveSnapshot(path);
}

void MainWindow::relink(const QString& sourceId) {
    const MediaSource* src = doc_->project().findSource(sourceId.toStdString());
    if (!src) return;
    const QString old = doc_->mediaFilePath(*src);
    const QString path = QFileDialog::getOpenFileName(this, "Locate " + QFileInfo(old).fileName(), QFileInfo(old).absolutePath(), kMediaFilter);
    if (path.isEmpty()) return;
    try {
        const MediaInfo mi = probeMedia(toPath(path));
        if ((src->hasVideo && !mi.video) || (src->hasAudio && !mi.audio))
            throw std::runtime_error("the file does not have the same kind of streams (video/audio)");
        const std::string p = QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toUtf8().toStdString();
        doc_->execute(makeModifySource(sourceId.toStdString(), "Relink media", [p](MediaSource& s) { s.path = p; }));
    } catch (const std::exception& e) {
        showMessage(QString("Relink failed: ") + e.what(), true);
    }
}

void MainWindow::checkMissingMedia() {
    const auto missing = doc_->missingSources();
    if (missing.empty()) return;
    QStringList names;
    for (const auto& id : missing) names << QString::fromUtf8(doc_->project().findSource(id)->path);
    showMessage(QString("%1 media file(s) are missing - shown in red; right-click them in the library to relink").arg(missing.size()), true);
    if (opt_.interactive)
        QMessageBox::warning(this, "Missing media",
                             "These media files were not found. Their clips are shown as offline and the project "
                             "cannot be exported until they are relinked (right-click the item in the Media panel "
                             "> Relink Media):\n\n" + names.join("\n"));
}

void MainWindow::addRecent(const QString& path) {
    QSettings s;
    QStringList r = s.value("recentProjects").toStringList();
    r.removeAll(path);
    r.prepend(path);
    while (r.size() > 8) r.removeLast();
    s.setValue("recentProjects", r);
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu() {
    if (!recentMenu_) return;
    recentMenu_->clear();
    const QStringList r = QSettings().value("recentProjects").toStringList();
    for (const QString& p : r)
        recentMenu_->addAction(QDir::toNativeSeparators(p), this, [this, p] {
            if (confirmDiscard()) openProjectFile(p);
        });
    recentMenu_->setEnabled(!r.isEmpty());
}

void MainWindow::showShortcuts() {
    static const std::vector<std::pair<const char*, std::vector<const char*>>> groups = {
        {"File", {"new", "open", "save", "saveAs", "import", "export", "preferences"}},
        {"Edit", {"undo", "redo", "cut", "copy", "paste", "duplicate", "delete", "rippleDelete", "selectAll", "split",
                  "trimStart", "trimEnd", "copyEffects", "pasteEffects"}},
        {"Clip", {"cropZoom", "cropToFit", "rotateCw", "rotateCcw", "speedSettings", "detach", "mute", "audioGain",
                  "toggleClip", "rename", "selectClipRange", "selectSameColor"}},
        {"Playback", {"play", "stop", "prevFrame", "nextFrame", "prevEdit", "nextEdit", "toStart", "toEnd", "markIn", "markOut",
                      "clearMarks", "snapshot", "renderPreview"}},
        {"View", {"zoomIn", "zoomOut", "zoomFit", "snapping", "shortcuts"}},
    };
    QString html = "<table cellspacing=4>";
    for (const auto& [group, names] : groups) {
        html += QString("<tr><td colspan=2><br><b style='color:%1'>%2</b></td></tr>").arg(color::accent.name(), group);
        for (const char* n : names) {
            QAction* a = actions_[n];
            QStringList keys;
            for (const auto& k : a->shortcuts()) keys << k.toString(QKeySequence::NativeText);
            if (keys.isEmpty()) continue;
            html += QString("<tr><td>%1</td><td><b>%2</b></td></tr>").arg(a->text().remove('&').remove("..."), keys.join(", "));
        }
    }
    html += "</table><p>Mouse: drag clips to move, drag clip edges to trim, click the ruler to seek, Ctrl+wheel to zoom. "
            "Right-click a clip for every command.</p>";
    QMessageBox::information(this, "Keyboard Shortcuts", html);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (!confirmDiscard()) {
        e->ignore();
        return;
    }
    playback_->pause();
    if (opt_.interactive) {
        QSettings st;
        st.setValue("layout/top", topSplit_->saveState());
        st.setValue("layout/vertical", vertSplit_->saveState());
    }
    autosave_->endSession();   // a normal end: the autosaved copy is not needed
    e->accept();
}

}  // namespace fa::ui
