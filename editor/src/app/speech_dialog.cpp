#include "speech_dialog.h"

#include "model_manager.h"

#include "fa/ai/models.h"
#include "fa/model/edit_ops.h"
#include "fa/model/subtitles.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace fa::ui {

namespace {

QString clock(const Time& t) {
    const double s = t.toDouble();
    return QString("%1:%2.%3")
        .arg(static_cast<int>(s) / 60)
        .arg(static_cast<int>(s) % 60, 2, 10, QChar('0'))
        .arg(static_cast<int>((s - std::floor(s)) * 10));
}

}  // namespace

SpeechToTextDialog::SpeechToTextDialog(Document* doc, QWidget* parent) : QDialog(parent), doc_(doc) {
    setWindowTitle("Speech to Text");
    setObjectName("speechToText");
    resize(820, 620);
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    source_ = new QComboBox(this);
    source_->setObjectName("sttSource");
    source_->addItem("Selected clip", "clip");
    source_->addItem("Timeline from In to Out", "inout");
    source_->addItem("Whole timeline", "timeline");
    source_->setToolTip("Selected clip: its own sound at normal speed, the captions placed where the words play. "
                        "Timeline: what the viewer hears (all audio tracks mixed).");
    form->addRow("Transcribe", source_);
    model_ = new QComboBox(this);
    model_->setObjectName("sttModel");
    auto* modelRow = new QHBoxLayout();
    modelRow->addWidget(model_, 1);
    auto* manage = new QPushButton("Model Manager...", this);
    manage->setObjectName("sttModelManager");
    manage->setToolTip("Install or remove speech models (downloads only after you confirm)");
    modelRow->addWidget(manage);
    form->addRow("Model", modelRow);
    language_ = new QComboBox(this);
    language_->setObjectName("sttLanguage");
    language_->addItem("Detect automatically", "auto");
    for (const auto& [code, name] : ai::speechLanguages())
        language_->addItem(QString::fromStdString(name) + " (" + QString::fromStdString(code) + ")", QString::fromStdString(code));
    form->addRow("Language", language_);
    device_ = new QComboBox(this);
    device_->setObjectName("sttDevice");
    device_->addItem("Auto (GPU when available)", "auto");
    device_->addItem("CPU", "cpu");
    device_->addItem("GPU", "gpu");
    form->addRow("Run on", device_);
    maxChars_ = new QSpinBox(this);
    maxChars_->setObjectName("sttMaxChars");
    maxChars_->setRange(20, 200);
    maxChars_->setValue(84);
    maxChars_->setSuffix(" characters");
    maxChars_->setToolTip("Longer sentences are split at word boundaries into captions of at most this length");
    form->addRow("Caption length", maxChars_);
    lay->addLayout(form);
    auto* row = new QHBoxLayout();
    transcribe_ = new QPushButton("Transcribe", this);
    transcribe_->setObjectName("sttTranscribe");
    progress_ = new QProgressBar(this);
    progress_->setObjectName("sttProgress");
    progress_->setRange(0, 1000);
    row->addWidget(transcribe_);
    row->addWidget(progress_, 1);
    lay->addLayout(row);
    status_ = new QLabel("Runs on this PC; nothing is uploaded. Recognition makes mistakes: check the captions.", this);
    status_->setObjectName("sttStatus");
    status_->setWordWrap(true);
    lay->addWidget(status_);
    list_ = new QTableWidget(this);
    list_->setObjectName("sttList");
    list_->setColumnCount(3);
    list_->setHorizontalHeaderLabels({"Start", "End", "Text"});
    list_->horizontalHeader()->setStretchLastSection(true);
    list_->verticalHeader()->setVisible(false);
    lay->addWidget(list_, 1);
    auto* buttons = new QHBoxLayout();
    add_ = new QPushButton("Add Captions", this);
    add_->setObjectName("sttAdd");
    add_->setEnabled(false);
    auto* close = new QPushButton("Close", this);
    buttons->addStretch(1);
    buttons->addWidget(add_);
    buttons->addWidget(close);
    lay->addLayout(buttons);
    timer_ = new QTimer(this);
    timer_->setInterval(100);
    connect(timer_, &QTimer::timeout, this, &SpeechToTextDialog::poll);
    connect(transcribe_, &QPushButton::clicked, this, [this] {
        if (busy()) cancel_ = true;
        else startTranscription();
    });
    connect(add_, &QPushButton::clicked, this, [this] { addCaptions(); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(manage, &QPushButton::clicked, this, [this] {
        ModelManagerDialog d(this);
        d.exec();
        if (!busy()) refreshModels();
    });
    refreshModels();
}

SpeechToTextDialog::~SpeechToTextDialog() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

void SpeechToTextDialog::showEvent(QShowEvent* e) {
    QDialog::showEvent(e);
    if (!busy()) refreshModels();
}

void SpeechToTextDialog::refreshModels() {
    const QString current = model_->currentData().toString();
    model_->clear();
    for (const auto& m : ai::modelCatalog()) {
        if (m.runtime != ai::ModelRuntime::Whisper) continue;
        const bool ok = ai::modelStatus(m, false) == ai::ModelStatus::Installed;
        model_->addItem(QString::fromStdString(m.name) + (ok ? "" : "  - not installed (Model Manager)"), QString::fromStdString(m.id));
        if (!ok) model_->setItemData(model_->count() - 1, 0, Qt::UserRole - 1);   // disabled
    }
    int pick = -1;
    for (int i = 0; i < model_->count(); ++i)
        if (model_->itemData(i, Qt::UserRole - 1).isNull() && (pick < 0 || model_->itemData(i).toString() == current))
            pick = i;
    if (pick >= 0) model_->setCurrentIndex(pick);
}

bool SpeechToTextDialog::startTranscription() {
    if (busy()) return false;
    const ai::ModelInfo* m = ai::findModel(model_->currentData().toString().toStdString());
    if (!m || ai::modelStatus(*m, false) != ai::ModelStatus::Installed) {
        status_->setText("Install a Whisper model first: AI Tools > Model Manager.");
        return false;
    }
    ai::TranscribeOptions o;
    o.model = m->id;
    o.language = language_->currentData().toString().toStdString();
    ai::acceleratorFromId(device_->currentData().toString().toStdString(), o.accelerator);
    o.maxCaptionChars = maxChars_->value();
    const ProjectSnapshot snap = doc_->snapshot();
    const QString src = source_->currentData().toString();
    const Sequence* seq = snap.sequence();
    if (!seq) return false;
    using Progress = std::function<void(double)>;
    std::function<ai::CaptionResult(const Progress&, const std::atomic<bool>*)> job;
    if (src == "clip") {
        const Clip* clip = nullptr;
        for (const auto& id : doc_->selection())
            if ((clip = edit::findClip(*seq, id)) && !clip->isGenerated()) break;
        if (!clip || clip->isGenerated()) {
            status_->setText("Select a video or audio clip with sound on the timeline.");
            return false;
        }
        job = [snap, c = *clip, o](const Progress& p, const std::atomic<bool>* cancel) {
            return ai::captionsForClip(*snap.project, c, snap.projectFile, o, p, cancel);
        };
    } else {
        Time from(0), to = seq->duration();
        if (src == "inout") {
            if (!seq->markIn && !seq->markOut) {
                status_->setText("Set In / Out marks first (Playback > Mark In / Mark Out).");
                return false;
            }
            from = seq->markIn.value_or(Time(0));
            to = seq->markOut.value_or(seq->duration());
        }
        job = [snap, s = *seq, from, to, o](const Progress& p, const std::atomic<bool>* cancel) {
            return ai::captionsForRange(*snap.project, s, from, to, snap.projectFile, o, p, cancel);
        };
    }
    cancel_ = false;
    done_ = false;
    permille_ = 0;
    error_.clear();
    result_ = {};
    list_->setRowCount(0);
    add_->setEnabled(false);
    transcribe_->setText("Cancel");
    status_->setText("Transcribing...");
    worker_ = std::thread([this, job] {
        try {
            // cancel: whisper.cpp checks the flag between its decoder windows
            result_ = job([this](double f) { permille_ = static_cast<int>(f * 1000); }, &cancel_);
        } catch (const std::exception& e) {
            error_ = e.what();
        }
        done_ = true;
    });
    timer_->start();
    return true;
}

void SpeechToTextDialog::poll() {
    progress_->setValue(permille_);
    if (!done_) return;
    timer_->stop();
    worker_.join();
    transcribe_->setText("Transcribe");
    if (!error_.empty() || result_.transcript.cancelled) {
        status_->setText(error_.empty() ? "Cancelled; nothing was added." : QString::fromStdString("Failed: " + error_));
        progress_->setValue(0);
        result_ = {};
        return;
    }
    const ai::Transcript& t = result_.transcript;
    status_->setText(QString("%1 captions from %2 s of audio in %3 s (%4), language: %5%6. Check them, then Add Captions.")
                         .arg(result_.captions.size())
                         .arg(t.audioSeconds, 0, 'f', 1)
                         .arg(t.seconds, 0, 'f', 1)
                         .arg(QString::fromStdString(t.provider))
                         .arg(QString::fromStdString(t.language))
                         .arg(t.fallback.empty() ? QString() : " - " + QString::fromStdString(t.fallback)));
    list_->setRowCount(static_cast<int>(result_.captions.size()));
    for (int i = 0; i < static_cast<int>(result_.captions.size()); ++i) {
        const Caption& c = result_.captions[static_cast<size_t>(i)];
        list_->setItem(i, 0, new QTableWidgetItem(clock(c.start)));
        list_->setItem(i, 1, new QTableWidgetItem(clock(c.end)));
        list_->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(c.text)));
    }
    add_->setEnabled(!result_.captions.empty());
}

bool SpeechToTextDialog::addCaptions() {
    if (result_.captions.empty()) return false;
    // the text as reviewed in the list (corrections typed there are kept)
    std::vector<Caption> caps = result_.captions;
    for (int i = 0; i < list_->rowCount() && i < static_cast<int>(caps.size()); ++i)
        if (QTableWidgetItem* it = list_->item(i, 2)) caps[static_cast<size_t>(i)].text = it->text().toStdString();
    std::vector<std::string> tracks;
    QString err;
    std::unique_ptr<Command> cmd;
    try {
        cmd = edit::addCaptions(doc_->commandTarget(), doc_->sequenceId(), std::move(caps), Track::defaultSubtitleStyle(), Track().subtitleY,
                                &tracks);
    } catch (const std::exception& e) {
        status_->setText(QString::fromUtf8(e.what()));
        return false;
    }
    if (!doc_->execute(std::move(cmd), &err)) {
        status_->setText(err);
        return false;
    }
    const QString summary = QString("Added %1 captions (speech to text, %2)")
                                .arg(result_.captions.size())
                                .arg(QString::fromStdString(result_.transcript.language));
    status_->setText(summary + ". Edit them on the subtitle track like any caption.");
    add_->setEnabled(false);
    emit captionsAdded(static_cast<int>(result_.captions.size()), summary);
    return true;
}

}  // namespace fa::ui
