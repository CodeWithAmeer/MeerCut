// AI Tools > Speech to Text (M7): offline transcription with whisper.cpp into captions on a subtitle track.
//   Source: the selected clip (its own sound, timed through its speed changes), the timeline from In to Out, or the
//   whole timeline (what the viewer hears). Model (installed Whisper models), language (detected or chosen), device.
//   Transcribe runs in the background (progress, Cancel); the result is listed for review and Add Captions puts it on
//   a subtitle track in one undo step - ordinary captions, edited and exported (SRT / ASS) like imported ones.
// Speech recognition makes mistakes; the captions are meant to be checked. No speaker identification, no translation.
#pragma once

#include "document.h"

#include "fa/ai/captions.h"

#include <QDialog>

#include <atomic>
#include <functional>
#include <thread>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace fa::ui {

class SpeechToTextDialog : public QDialog {
    Q_OBJECT
public:
    explicit SpeechToTextDialog(Document* doc, QWidget* parent = nullptr);
    ~SpeechToTextDialog() override;

    bool startTranscription();       // what Transcribe does (tests)
    bool busy() const { return worker_.joinable(); }
    const ai::CaptionResult& result() const { return result_; }
    // Adds the reviewed captions to the timeline (one undo step); false when there are none.
    bool addCaptions();

signals:
    void captionsAdded(int count, const QString& summary);

protected:
    void showEvent(QShowEvent* e) override;   // the model list again (models may have been installed meanwhile)

private:
    void poll();
    void refreshModels();

    Document* doc_;
    QComboBox* source_ = nullptr;
    QComboBox* model_ = nullptr;
    QComboBox* language_ = nullptr;
    QComboBox* device_ = nullptr;
    QSpinBox* maxChars_ = nullptr;
    QPushButton* transcribe_ = nullptr;
    QPushButton* add_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_ = nullptr;
    QTableWidget* list_ = nullptr;
    QTimer* timer_ = nullptr;

    std::thread worker_;
    std::atomic<bool> cancel_{false}, done_{false};
    std::atomic<int> permille_{0};
    std::string error_;
    ai::CaptionResult result_;
};

}  // namespace fa::ui
