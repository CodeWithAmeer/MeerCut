// Dialogs: export (background render with progress/cancel), new project / project settings, subtitle import.
#pragma once

#include "document.h"

#include "fa/media/export_format.h"

#include "fa/model/subtitles.h"
#include "fa/render/renderer.h"

#include <QDialog>

#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

class QCheckBox;
class QFormLayout;
class QRadioButton;
class QGroupBox;
class QSpinBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QPlainTextEdit;
class QTimer;

namespace fa::ui {

// Performance preferences (QSettings "render/*"), used by the preview and as the export default.
struct RenderSettings {
    BackendChoice backend = BackendChoice::Auto;   // Auto: GPU for frames with compositing work
    bool hardwareDecode = false;                   // D3D11VA decoding (measured slower for 1080p H.264 here)
    int renderCacheLimitGb = 8;                    // M5 Render Preview files: size limit (least recently used go first)
    int autosaveMinutes = 1;                       // Stage 3: copy of unsaved changes for crash recovery; 0 = off
};
RenderSettings loadRenderSettings();
void saveRenderSettings(const RenderSettings& s);
QString backendLabel(BackendChoice c);

// Edit > Preferences: compositing backend and hardware decoding, with the detected graphics adapters.
class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    // renderCacheInfo: where the Render Preview files are and how much they use (shown read-only)
    PreferencesDialog(const RenderSettings& current, const QString& previewBackend, const QString& renderCacheInfo = {},
                      QWidget* parent = nullptr);
    RenderSettings settings() const;

private:
    QComboBox* backend_;
    QCheckBox* hwDecode_;
    QSpinBox* cacheLimit_;
    QSpinBox* autosave_;
};

// Runs renderProject() on a worker thread. Poll progress from the GUI thread; finished() reports
// success, cancellation or the error message.
class ExportJob : public QObject {
    Q_OBJECT
public:
    ExportJob(ProjectSnapshot snapshot, QString output, RenderOptions options, QObject* parent = nullptr);
    ~ExportJob() override;
    void start();
    void cancel() { cancel_ = true; }
    bool running() const { return running_; }
    int64_t done() const { return done_; }
    int64_t total() const { return total_; }
    bool preparing() const { return preparing_; }   // M7: preparing AI pictures before rendering
    QString stage() const { return QString::fromLatin1(stage_.load()); }
    const RenderReport& report() const { return report_; }

signals:
    void finished(bool ok, bool cancelled, const QString& message);

private:
    ProjectSnapshot snap_;
    QString output_;
    RenderOptions options_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> running_{false};
    std::atomic<int64_t> done_{0};
    std::atomic<int64_t> total_{0};
    std::atomic<bool> preparing_{false};
    std::atomic<const char*> stage_{"AI"};
    RenderReport report_;
    std::thread thread_;
};

// File > Export (M6): format, encoder (hardware when a session works, else the CPU encoder - reported), resolution
// and frame-rate presets, quality or bit rate, bit depth, audio, range (whole timeline, In to Out, selected clips),
// a size estimate, progress with Cancel, and a check of the finished file. Exports run renderProject (the renderer
// preview and fa-render use); an existing file is replaced only when the new one is complete.
class ExportDialog : public QDialog {
    Q_OBJECT
public:
    enum class Range { Whole, InOut, Selected };
    ExportDialog(Document* doc, QWidget* parent = nullptr);
    ~ExportDialog() override;
    // For tests / automation: export with the current settings to `file` (the format follows its extension when it
    // names one; an existing file is replaced once the new one is complete) and wait for the result.
    bool exportTo(const QString& file, QString* error);
    QString outputPath() const;          // folder / name + the format's extension
    RenderOptions options() const;       // what Export would render now
    QString problem() const;             // why Export cannot start now; empty = ready
    QString verification() const { return verification_; }   // the check of the last finished file
    void selectRange(Range r);           // M9: Export Selected Clips opens with "Selected clips" chosen
    bool encodersChecked() const { return encodersChecked_; }  // the background encoder check has finished
    // M7: asked before an export that must first prepare AI pictures for long (minutes on the GPU, the CPU); tests
    // answer it. Default: a message box (none in automation, which proceeds).
    std::function<bool(const QString&)> confirmSlow;

protected:
    void reject() override;

private:
    void startExport();
    void onFinished(bool ok, bool cancelled, const QString& message);
    void formatChanged();
    void refreshEncoders();
    void updateSummary();
    QString verify(const QString& file, const RenderOptions& o, const RenderReport& r) const;
    const ExportFormat* format() const;
    std::pair<int, int> outputSize() const;   // the exported picture size
    FrameRate outputRate() const;
    std::pair<int64_t, int64_t> frameRange() const;   // sequence frames of the range (inclusive); empty = first > last

    Document* doc_;
    std::vector<std::string> selection_;   // the clips selected when the window opened
    QLineEdit* name_;
    QLineEdit* folder_;
    QLabel* fileName_;
    QComboBox* format_;
    QGroupBox* videoBox_;
    QFormLayout* videoForm_;
    QComboBox* hardware_;
    QLabel* encoderInfo_;
    QComboBox* resolution_;
    QWidget* customSize_;
    QSpinBox* width_;
    QSpinBox* height_;
    QComboBox* frameRate_;
    QComboBox* quality_;
    QSpinBox* bitrate_;
    QComboBox* bitDepth_;
    QComboBox* prores_;
    QCheckBox* dither_;
    QComboBox* speed_;
    QComboBox* backend_;
    QGroupBox* audioBox_;
    QCheckBox* audio_;
    QComboBox* audioCodec_;
    QComboBox* audioBitrate_;
    QComboBox* sampleRate_;
    QComboBox* channels_;
    QLabel* audioNote_;
    QRadioButton* rangeWhole_;
    QRadioButton* rangeInOut_;
    QRadioButton* rangeSelected_;
    QCheckBox* selectedSpan_;   // selected clips: with everything else in their time span
    QLabel* summary_;
    QProgressBar* progress_;
    QLabel* status_;
    QPlainTextEdit* messages_;
    QPushButton* exportBtn_;
    QPushButton* closeBtn_;
    QPushButton* openFolderBtn_;
    QTimer* poll_;
    std::unique_ptr<ExportJob> job_;
    std::thread probe_;
    std::atomic<bool> encodersChecked_{false};
    bool automation_ = false;
    bool lastOk_ = false;
    QString lastError_;
    QString verification_;
    RenderOptions running_;
    qint64 startMs_ = 0;
};

// File > Import Subtitles: shows what an SRT / ASS / SSA file contains before it is added - caption count,
// the first captions, the encoding (changeable for files that are not UTF-8/UTF-16) and every piece of
// styling that cannot be imported exactly - and where the captions start.
class SubtitleImportDialog : public QDialog {
    Q_OBJECT
public:
    SubtitleImportDialog(const QString& file, QByteArray bytes, const Time& playhead, QWidget* parent = nullptr);
    const SubtitleDoc& document() const { return doc_; }
    unsigned codePage() const;
    Time offset() const;   // added to every caption time

private:
    void reparse();
    QString fileName_;
    QByteArray bytes_;
    Time playhead_;
    SubtitleDoc doc_;
    QComboBox* encoding_;
    QComboBox* placement_;
    QLabel* summary_;
    QPlainTextEdit* preview_;
    QPlainTextEdit* warnings_;
};

// Windows code pages offered for subtitle files without a Unicode encoding (0 = auto / system).
const std::vector<std::pair<unsigned, QString>>& subtitleCodePages();

class SequenceSettingsDialog : public QDialog {
    Q_OBJECT
public:
    SequenceSettingsDialog(const SequenceSettings& initial, const QString& title, QWidget* parent = nullptr);
    SequenceSettings settings() const;

private:
    QComboBox* resolution_;
    QComboBox* fps_;
    QComboBox* rate_;
};

}  // namespace fa::ui
