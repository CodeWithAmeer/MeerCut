#include "dialogs.h"

#include "fa/ai/prepare.h"

#include "theme.h"

#include "fa/model/edit_ops.h"
#include "fa/text/title_engine.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QStandardItemModel>
#include <QRadioButton>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <optional>
#include <filesystem>

namespace fa::ui {

namespace {
std::filesystem::path toPath(const QString& s) {
    const std::string u = QDir::toNativeSeparators(s).toUtf8().toStdString();
    return std::filesystem::path(std::u8string(u.begin(), u.end()));
}
}  // namespace

// ---- render settings / preferences -------------------------------------------------------------

RenderSettings loadRenderSettings() {
    QSettings q;
    RenderSettings s;
    const QString b = q.value("render/backend", "auto").toString();
    s.backend = b == "cpu" ? BackendChoice::Cpu : b == "gpu" ? BackendChoice::Gpu : BackendChoice::Auto;
    s.hardwareDecode = q.value("render/hardwareDecode", false).toBool();
    s.renderCacheLimitGb = std::clamp(q.value("renderPreview/limitGB", 8).toInt(), 1, 1000);
    s.autosaveMinutes = std::clamp(q.value("autosave/minutes", 1).toInt(), 0, 60);
    return s;
}

void saveRenderSettings(const RenderSettings& s) {
    QSettings q;
    q.setValue("render/backend", s.backend == BackendChoice::Cpu ? "cpu" : s.backend == BackendChoice::Gpu ? "gpu" : "auto");
    q.setValue("render/hardwareDecode", s.hardwareDecode);
    q.setValue("renderPreview/limitGB", s.renderCacheLimitGb);
    q.setValue("autosave/minutes", s.autosaveMinutes);
}

QString backendLabel(BackendChoice c) {
    switch (c) {
        case BackendChoice::Cpu: return "CPU";
        case BackendChoice::Gpu: return "GPU (Direct3D 11)";
        case BackendChoice::Auto: break;
    }
    return "Auto - GPU for effects, CPU for plain frames (recommended)";
}

PreferencesDialog::PreferencesDialog(const RenderSettings& cur, const QString& previewBackend, const QString& renderCacheInfo,
                                     QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("Preferences");
    setMinimumWidth(560);
    auto* lay = new QVBoxLayout(this);
    auto* title = new QLabel("Performance", this);
    title->setStyleSheet(QString("color: %1; font-weight: 600;").arg(color::accent.name()));
    lay->addWidget(title);
    auto* form = new QFormLayout();
    backend_ = new QComboBox(this);
    backend_->setObjectName("prefBackend");
    for (BackendChoice c : {BackendChoice::Auto, BackendChoice::Gpu, BackendChoice::Cpu})
        backend_->addItem(backendLabel(c), static_cast<int>(c));
    backend_->setCurrentIndex(backend_->findData(static_cast<int>(cur.backend)));
    hwDecode_ = new QCheckBox("Hardware video decoding (D3D11VA)", this);
    hwDecode_->setObjectName("prefHwDecode");
    hwDecode_->setChecked(cur.hardwareDecode);
    form->addRow("Compositing (preview and export)", backend_);
    form->addRow("Decoding", hwDecode_);
    lay->addLayout(form);
    QString adapters;
    for (const auto& a : listGpuAdapters())
        adapters += QString("  %1 (%2 MB)%3%4%5\n").arg(QString::fromStdString(a.name)).arg(a.dedicatedVideoMemory >> 20)
                        .arg(a.software ? " - software" : "")
                        .arg(a.highPerformanceChoice ? " - used for GPU compositing" : "")
                        .arg(a.powerSavingChoice && !a.highPerformanceChoice ? " - power saving" : "");
    std::string why;
    const bool gpuOk = makeGpuBackend(&why) != nullptr;
    auto* info = new QLabel(
        "Graphics adapters:\n" + (adapters.isEmpty() ? QString("  none detected\n") : adapters) +
            (gpuOk ? QString() : "GPU compositing unavailable: " + QString::fromStdString(why) + " - the CPU is used.\n") +
            "Preview currently renders with: " + previewBackend +
            "\n\nGPU and CPU produce the same picture (within 1/255). On this kind of laptop the GPU is about 2-3x faster "
            "for transforms, masks, blend modes and transitions; plain cuts are faster on the CPU, which Auto uses for them. "
            "Hardware decoding offloads H.264 decoding to the GPU's video engine but was slower than the multi-threaded "
            "software decoder for 1080p in our measurements; it may help with high-resolution footage.",
        this);
    info->setWordWrap(true);
    info->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    lay->addWidget(info);
    auto* cacheTitle = new QLabel("Render Preview", this);
    cacheTitle->setStyleSheet(QString("color: %1; font-weight: 600;").arg(color::accent.name()));
    lay->addWidget(cacheTitle);
    auto* cacheForm = new QFormLayout();
    cacheLimit_ = new QSpinBox(this);
    cacheLimit_->setObjectName("prefRenderCacheLimit");
    cacheLimit_->setRange(1, 1000);
    cacheLimit_->setSuffix(" GB");
    cacheLimit_->setValue(cur.renderCacheLimitGb);
    cacheLimit_->setToolTip("Rendered preview frames are kept up to this size; the least recently used go first");
    cacheForm->addRow("Size limit", cacheLimit_);
    lay->addLayout(cacheForm);
    if (!renderCacheInfo.isEmpty()) {
        auto* where = new QLabel(renderCacheInfo + "\nPlayback > Clear Render Preview Files empties it.", this);
        where->setWordWrap(true);
        where->setTextInteractionFlags(Qt::TextSelectableByMouse);
        where->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
        lay->addWidget(where);
    }
    auto* recoveryTitle = new QLabel("Recovery", this);
    recoveryTitle->setStyleSheet(QString("color: %1; font-weight: 600;").arg(color::accent.name()));
    lay->addWidget(recoveryTitle);
    auto* recoveryForm = new QFormLayout();
    autosave_ = new QSpinBox(this);
    autosave_->setObjectName("prefAutosaveMinutes");
    autosave_->setRange(0, 60);
    autosave_->setSuffix(" min");
    autosave_->setSpecialValueText("Off");
    autosave_->setValue(cur.autosaveMinutes);
    autosave_->setToolTip("Unsaved changes are copied this often; after a crash MeerCut offers to recover them. "
                          "Your project file is only written when you save.");
    recoveryForm->addRow("Autosave unsaved changes every", autosave_);
    lay->addLayout(recoveryForm);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
}

RenderSettings PreferencesDialog::settings() const {
    RenderSettings s;
    s.backend = static_cast<BackendChoice>(backend_->currentData().toInt());
    s.hardwareDecode = hwDecode_->isChecked();
    s.renderCacheLimitGb = cacheLimit_->value();
    s.autosaveMinutes = autosave_->value();
    return s;
}

// ---- ExportJob -------------------------------------------------------------------------------

ExportJob::ExportJob(ProjectSnapshot snapshot, QString output, RenderOptions options, QObject* parent)
    : QObject(parent), snap_(std::move(snapshot)), output_(std::move(output)), options_(std::move(options)) {}

ExportJob::~ExportJob() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

void ExportJob::start() {
    running_ = true;
    options_.cancel = &cancel_;
    options_.sequenceId = snap_.sequenceId;
    options_.progress = [this](int64_t d, int64_t t) {
        done_ = d;
        total_ = t;
    };
    thread_ = std::thread([this] {
        bool ok = false, cancelled = false;
        QString msg;
        try {
            // M7: frames a Background Removal still lacks are analysed first (the same mattes the preview uses)
            Project project = *snap_.project;
            preparing_ = true;
            const int64_t last = options_.onlyClips.empty() ? options_.lastFrame : -1;
            ai::prepareForRender(project, snap_.sequenceId, snap_.projectFile, options_.onlyClips.empty() ? options_.firstFrame : 0,
                                 last < 0 ? -1 : last + 1, [this](const char* stage, int64_t d, int64_t t) {
                                     stage_ = stage;
                                     done_ = d;
                                     total_ = t;
                                 }, &cancel_);
            preparing_ = false;
            done_ = 0;
            total_ = 0;
            if (cancel_) throw RenderCancelled();
            report_ = renderProject(project, snap_.projectFile, toPath(output_), options_);
            ok = true;
        } catch (const RenderCancelled&) {
            cancelled = true;
            msg = "Export cancelled; the incomplete file was removed.";
        } catch (const std::exception& e) {
            msg = QString::fromUtf8(e.what());
        }
        running_ = false;
        QMetaObject::invokeMethod(this, [this, ok, cancelled, msg] { emit finished(ok, cancelled, msg); },
                                  Qt::QueuedConnection);
    });
}

// ---- ExportDialog ----------------------------------------------------------------------------

namespace {

QString audioCodecLabel(const std::string& c) {
    if (c == "aac") return "AAC";
    if (c == "libopus") return "Opus";
    if (c == "pcm_s16le") return "PCM 16-bit";
    if (c == "pcm_s24le") return "PCM 24-bit";
    if (c == "flac") return "FLAC";
    return QString::fromStdString(c);
}

bool reservedFileName(const QString& base) {
    static const QStringList names{"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
                                   "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    return names.contains(base.section('.', 0, 0).toUpper());
}

QString sizeText(double bytes) {
    if (bytes >= 1e9) return QString("%1 GB").arg(bytes / 1e9, 0, 'f', 1);
    if (bytes >= 1e6) return QString("%1 MB").arg(bytes / 1e6, 0, 'f', bytes >= 1e8 ? 0 : 1);
    return QString("%1 KB").arg(std::max(1.0, bytes / 1e3), 0, 'f', 0);
}

// The video encoder of a codec and family, or "".
std::string encoderOf(const std::string& codec, HardwareEncoding family) {
    for (const auto& s : videoEncoderSpecs())
        if (s.codec == codec && s.family == family) return s.encoder;
    return {};
}

QString sameFile(const QString& a, const QString& b) {   // empty when different
    const QFileInfo fa(a), fb(b);
    if (!fa.exists() || !fb.exists()) return {};
    return fa.canonicalFilePath().compare(fb.canonicalFilePath(), Qt::CaseInsensitive) == 0 ? fa.fileName() : QString();
}

}  // namespace

ExportDialog::ExportDialog(Document* doc, QWidget* parent)
    : QDialog(parent), doc_(doc), selection_(doc->selection()) {
    setWindowTitle("Export");
    setMinimumWidth(940);
    auto* lay = new QVBoxLayout(this);
    // two columns (output, audio, range | video) so the window fits a 1080p screen at 125 %
    auto* columns = new QHBoxLayout();
    auto* left = new QVBoxLayout();
    auto* right = new QVBoxLayout();
    columns->addLayout(left, 1);
    columns->addLayout(right, 1);
    lay->addLayout(columns);
    auto dim = [this](const QString& t) {
        auto* l = new QLabel(t, this);
        l->setWordWrap(true);
        l->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
        return l;
    };

    // ---- output
    auto* outBox = new QGroupBox("Output", this);
    auto* of = new QFormLayout(outBox);
    name_ = new QLineEdit(doc_->displayName(), outBox);
    name_->setObjectName("exportName");
    folder_ = new QLineEdit(outBox);
    folder_->setObjectName("exportFolder");
    const QString videos = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    folder_->setText(QDir::toNativeSeparators(doc_->filePath().isEmpty() ? videos : QFileInfo(doc_->filePath()).absolutePath()));
    auto* browse = new QPushButton("Browse...", outBox);
    auto* folderRow = new QHBoxLayout();
    folderRow->addWidget(folder_, 1);
    folderRow->addWidget(browse);
    format_ = new QComboBox(outBox);
    format_->setObjectName("exportFormat");
    for (const auto& f : exportFormats()) format_->addItem(QString::fromStdString(f.label), QString::fromStdString(f.id));
    fileName_ = dim("");
    fileName_->setObjectName("exportFileName");
    of->addRow("Name", name_);
    of->addRow("Save to", folderRow);
    of->addRow("Format", format_);
    of->addRow(fileName_);
    left->addWidget(outBox);

    // ---- video
    videoBox_ = new QGroupBox("Video", this);
    videoForm_ = new QFormLayout(videoBox_);
    hardware_ = new QComboBox(videoBox_);
    hardware_->setObjectName("exportHardware");
    for (HardwareEncoding h : {HardwareEncoding::Cpu, HardwareEncoding::Auto, HardwareEncoding::Nvenc, HardwareEncoding::Qsv,
                               HardwareEncoding::Amf})
        hardware_->addItem(hardwareLabel(h), QString(hardwareId(h)));
    encoderInfo_ = dim("Checking the hardware encoders...");
    encoderInfo_->setObjectName("exportEncoderInfo");
    resolution_ = new QComboBox(videoBox_);
    resolution_->setObjectName("exportResolution");
    const Sequence& seq = doc_->sequence();
    resolution_->addItem(QString("Same as the project (%1 x %2)").arg(seq.width).arg(seq.height), 0);
    resolution_->addItem("720p", 720);
    resolution_->addItem("1080p", 1080);
    resolution_->addItem("4K UHD (2160p)", 2160);
    resolution_->addItem("Custom", -1);
    customSize_ = new QWidget(videoBox_);
    auto* cs = new QHBoxLayout(customSize_);
    cs->setContentsMargins(0, 0, 0, 0);
    width_ = new QSpinBox(customSize_);
    height_ = new QSpinBox(customSize_);
    width_->setObjectName("exportWidth");
    height_->setObjectName("exportHeight");
    for (QSpinBox* s : {width_, height_}) {
        s->setRange(64, 8192);
        s->setSingleStep(2);
    }
    width_->setValue(seq.width);
    height_->setValue(seq.height);
    cs->addWidget(width_);
    cs->addWidget(new QLabel("x", customSize_));
    cs->addWidget(height_);
    cs->addStretch(1);
    frameRate_ = new QComboBox(videoBox_);
    frameRate_->setObjectName("exportFrameRate");
    quality_ = new QComboBox(videoBox_);
    quality_->setObjectName("exportQuality");
    quality_->addItem("High", 4);
    quality_->addItem("Standard", 2);
    quality_->addItem("Small file", 0);
    quality_->addItem("Bit rate", -1);
    bitrate_ = new QSpinBox(videoBox_);
    bitrate_->setObjectName("exportBitrate");
    bitrate_->setRange(200, 400000);
    bitrate_->setSingleStep(500);
    bitrate_->setSuffix(" kbit/s");
    bitrate_->setValue(seq.height >= 2160 ? 40000 : seq.height >= 1080 ? 12000 : 6000);
    bitDepth_ = new QComboBox(videoBox_);
    bitDepth_->setObjectName("exportBitDepth");
    prores_ = new QComboBox(videoBox_);
    prores_->setObjectName("exportProResProfile");
    prores_->addItem("ProRes 422 Proxy", 0);
    prores_->addItem("ProRes 422 LT", 1);
    prores_->addItem("ProRes 422", 2);
    prores_->addItem("ProRes 422 HQ", 3);
    prores_->setCurrentIndex(2);
    dither_ = new QCheckBox("Dither (smoother gradients, larger file)", videoBox_);
    dither_->setObjectName("exportGifDither");
    dither_->setChecked(true);
    speed_ = new QComboBox(videoBox_);
    speed_->setObjectName("exportSpeed");
    speed_->addItem("Balanced", "medium");
    speed_->addItem("Faster export", "veryfast");
    speed_->addItem("Smaller file (slower)", "slow");
    backend_ = new QComboBox(videoBox_);
    backend_->setObjectName("exportBackend");
    for (BackendChoice c : {BackendChoice::Auto, BackendChoice::Gpu, BackendChoice::Cpu})
        backend_->addItem(backendLabel(c), static_cast<int>(c));
    backend_->setCurrentIndex(backend_->findData(static_cast<int>(loadRenderSettings().backend)));
    videoForm_->addRow("Encoder", hardware_);
    videoForm_->addRow(encoderInfo_);
    videoForm_->addRow("Resolution", resolution_);
    videoForm_->addRow("Size", customSize_);
    videoForm_->addRow("Frame rate", frameRate_);
    videoForm_->addRow("Quality", quality_);
    videoForm_->addRow("Bit rate", bitrate_);
    videoForm_->addRow("Bit depth", bitDepth_);
    videoForm_->addRow("Profile", prores_);
    videoForm_->addRow("Colours", dither_);
    videoForm_->addRow("Encoding", speed_);
    videoForm_->addRow("Rendering", backend_);
    right->addWidget(videoBox_);
    right->addStretch(1);

    // ---- audio
    audioBox_ = new QGroupBox("Audio", this);
    auto* af = new QFormLayout(audioBox_);
    audio_ = new QCheckBox("Include audio", audioBox_);
    audio_->setObjectName("exportAudio");
    audio_->setChecked(true);
    audioCodec_ = new QComboBox(audioBox_);
    audioCodec_->setObjectName("exportAudioCodec");
    audioBitrate_ = new QComboBox(audioBox_);
    audioBitrate_->setObjectName("exportAudioBitrate");
    for (int k : {128, 192, 256, 320}) audioBitrate_->addItem(QString("%1 kbit/s").arg(k), k * 1000);
    audioBitrate_->setCurrentIndex(1);
    sampleRate_ = new QComboBox(audioBox_);
    sampleRate_->setObjectName("exportSampleRate");
    sampleRate_->addItem(QString("Same as the project (%1 Hz)").arg(seq.sampleRate), 0);
    for (int r : {44100, 48000, 96000}) sampleRate_->addItem(QString("%1 Hz").arg(r), r);
    channels_ = new QComboBox(audioBox_);
    channels_->setObjectName("exportChannels");
    channels_->addItem("Stereo", 2);
    channels_->addItem("Mono (left + right) / 2", 1);
    audioNote_ = dim("");
    af->addRow(audio_);
    af->addRow("Codec", audioCodec_);
    af->addRow("Bit rate", audioBitrate_);
    af->addRow("Sample rate", sampleRate_);
    af->addRow("Channels", channels_);
    af->addRow(audioNote_);
    left->addWidget(audioBox_);

    // ---- range
    auto* rangeBox = new QGroupBox("Range", this);
    auto* rv = new QVBoxLayout(rangeBox);
    auto* rl = new QHBoxLayout();
    rv->addLayout(rl);
    rangeWhole_ = new QRadioButton("Whole timeline", rangeBox);
    rangeInOut_ = new QRadioButton("In to Out", rangeBox);
    rangeSelected_ = new QRadioButton(QString("Selected clips (%1)").arg(selection_.size()), rangeBox);
    rangeWhole_->setObjectName("exportRangeWhole");
    rangeInOut_->setObjectName("exportRangeInOut");
    rangeSelected_->setObjectName("exportRangeSelected");
    rangeWhole_->setChecked(true);
    rangeInOut_->setEnabled(seq.markIn.has_value() || seq.markOut.has_value());
    rangeInOut_->setToolTip("Playback > Mark In (I) / Mark Out (O)");
    rangeSelected_->setEnabled(!selection_.empty());
    for (QRadioButton* r : {rangeWhole_, rangeInOut_, rangeSelected_}) rl->addWidget(r);
    rl->addStretch(1);
    selectedSpan_ = new QCheckBox("Selected clips: include everything else in their time span", rangeBox);
    selectedSpan_->setObjectName("exportSelectedSpan");
    rv->addWidget(selectedSpan_);
    auto* rangeNote = dim("Selected clips alone = those clips and their linked audio / video; titles, overlays, "
                          "adjustment layers and other audio are left out unless selected. With the option: the whole "
                          "timeline over the selection's span.");
    rv->addWidget(rangeNote);
    left->addWidget(rangeBox);
    left->addStretch(1);

    summary_ = new QLabel(this);
    summary_->setObjectName("exportSummary");
    summary_->setWordWrap(true);
    lay->addWidget(summary_);
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 1000);
    progress_->setValue(0);
    status_ = new QLabel(this);
    status_->setObjectName("exportStatus");
    status_->setWordWrap(true);
    messages_ = new QPlainTextEdit(this);
    messages_->setReadOnly(true);
    messages_->setMaximumHeight(110);
    messages_->setVisible(false);
    lay->addWidget(progress_);
    lay->addWidget(status_);
    lay->addWidget(messages_);
    auto* buttons = new QHBoxLayout();
    openFolderBtn_ = new QPushButton("Open folder", this);
    openFolderBtn_->setVisible(false);
    closeBtn_ = new QPushButton("Close", this);
    exportBtn_ = new QPushButton(icon("export", QColor(0x1A, 0x14, 0x08)), "Export", this);
    exportBtn_->setObjectName("accentButton");
    exportBtn_->setDefault(true);
    buttons->addWidget(openFolderBtn_);
    buttons->addStretch(1);
    buttons->addWidget(closeBtn_);
    buttons->addWidget(exportBtn_);
    lay->addLayout(buttons);
    poll_ = new QTimer(this);
    poll_->setInterval(100);

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, "Export folder", folder_->text());
        if (!d.isEmpty()) folder_->setText(QDir::toNativeSeparators(d));
    });
    connect(format_, &QComboBox::currentIndexChanged, this, &ExportDialog::formatChanged);
    connect(hardware_, &QComboBox::currentIndexChanged, this, &ExportDialog::refreshEncoders);
    for (QComboBox* c : {resolution_, frameRate_, quality_, bitDepth_, prores_, speed_, audioCodec_, audioBitrate_, sampleRate_, channels_})
        connect(c, &QComboBox::currentIndexChanged, this, &ExportDialog::updateSummary);
    for (QSpinBox* s : {width_, height_, bitrate_}) connect(s, &QSpinBox::valueChanged, this, &ExportDialog::updateSummary);
    for (QLineEdit* e : {name_, folder_}) connect(e, &QLineEdit::textChanged, this, &ExportDialog::updateSummary);
    for (QAbstractButton* b : std::initializer_list<QAbstractButton*>{audio_, rangeWhole_, rangeInOut_, rangeSelected_, dither_, selectedSpan_})
        connect(b, &QAbstractButton::toggled, this, &ExportDialog::updateSummary);
    connect(exportBtn_, &QPushButton::clicked, this, &ExportDialog::startExport);
    connect(closeBtn_, &QPushButton::clicked, this, &ExportDialog::reject);
    connect(openFolderBtn_, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(outputPath()).absolutePath())); });
    connect(poll_, &QTimer::timeout, this, [this] {
        if (!job_) return;
        const int64_t t = job_->total(), d = job_->done();
        if (job_->preparing()) {   // M7: Background Removal frames are analysed before the export renders
            if (t > 0) progress_->setValue(static_cast<int>(1000 * d / t));
            status_->setText(QString("%1: preparing frame %2 of %3 before the export").arg(job_->stage()).arg(d).arg(t));
            return;
        }
        if (t > 0) {
            progress_->setValue(static_cast<int>(1000 * d / t));
            const double el = (QDateTime::currentMSecsSinceEpoch() - startMs_) / 1000.0;
            const double eta = d > 0 ? el * (t - d) / d : 0;
            status_->setText(QString("Exporting frame %1 of %2 - %3 s elapsed, about %4 s left").arg(d).arg(t)
                                 .arg(el, 0, 'f', 0).arg(eta, 0, 'f', 0));
        }
    });
    formatChanged();
    // The hardware encoders are checked with real sessions (a few seconds the first time): in the background.
    probe_ = std::thread([this] {
        try {
            probeEncoders();
        } catch (...) {
        }
        QMetaObject::invokeMethod(this, [this] {
            encodersChecked_ = true;
            refreshEncoders();
        }, Qt::QueuedConnection);
    });
}

void ExportDialog::selectRange(Range r) {
    QRadioButton* b = r == Range::Selected ? rangeSelected_ : r == Range::InOut ? rangeInOut_ : rangeWhole_;
    if (b->isEnabled()) b->setChecked(true);
}

ExportDialog::~ExportDialog() {
    if (probe_.joinable()) probe_.join();
}

const ExportFormat* ExportDialog::format() const {
    const ExportFormat* f = findExportFormat(format_->currentData().toString().toStdString());
    return f ? f : &exportFormats().front();
}

std::pair<int, int> ExportDialog::outputSize() const {
    const Sequence& s = doc_->sequence();
    const int preset = resolution_->currentData().toInt();
    if (preset == 0) return {s.width, s.height};
    if (preset < 0) return {width_->value(), height_->value()};
    // keep the project's shape: the preset is the height; the width follows (even)
    const int w = static_cast<int>(std::lround(preset * double(s.width) / s.height / 2.0)) * 2;
    return {w, preset};
}

FrameRate ExportDialog::outputRate() const {
    const QString v = frameRate_->currentData().toString();
    if (v.isEmpty()) return doc_->sequence().frameRate;
    return FrameRate(Rational::parse(v.toStdString()));
}

std::pair<int64_t, int64_t> ExportDialog::frameRange() const {
    const Sequence& s = doc_->sequence();
    const int64_t total = s.frameCount();
    if (rangeInOut_->isChecked()) {
        const int64_t a = s.markIn ? timeToFrame(*s.markIn, s.frameRate) : 0;
        const int64_t b = s.markOut ? timeToFrame(*s.markOut, s.frameRate) - 1 : total - 1;
        return {a, std::min(b, total - 1)};
    }
    if (rangeSelected_->isChecked()) {
        std::optional<Time> a, b;
        for (const auto& id : selection_)
            if (const Clip* c = edit::findClip(s, id)) {
                a = a ? min(*a, c->timelineStart) : c->timelineStart;
                b = b ? max(*b, c->timelineEnd()) : c->timelineEnd();
            }
        if (!a) return {0, -1};
        return {timeToFrame(*a, s.frameRate, Rounding::Floor), timeToFrame(*b, s.frameRate, Rounding::Ceil) - 1};
    }
    return {0, total - 1};
}

void ExportDialog::formatChanged() {
    const ExportFormat* f = format();
    const bool video = !f->videoCodec.empty(), gif = f->videoCodec == "gif", prores = f->videoCodec == "prores";
    videoBox_->setVisible(video);
    // frame rates (GIF: smaller rates)
    {
        QSignalBlocker b(frameRate_);
        const QString keep = frameRate_->currentData().toString();
        frameRate_->clear();
        const FrameRate r = doc_->sequence().frameRate;
        frameRate_->addItem(QString("Same as the project (%1 fps)").arg(r.toDouble(), 0, 'f', r.fps().isInteger() ? 0 : 3), QString());
        const std::vector<std::pair<const char*, const char*>> rates =
            gif ? std::vector<std::pair<const char*, const char*>>{{"10", "10"}, {"12", "12"}, {"15", "15"}, {"20", "20"}, {"25", "25"}}
                : std::vector<std::pair<const char*, const char*>>{{"23.976", "24000/1001"}, {"24", "24"}, {"25", "25"},
                                                                  {"29.97", "30000/1001"}, {"30", "30"}, {"50", "50"},
                                                                  {"59.94", "60000/1001"}, {"60", "60"}};
        for (const auto& [label, value] : rates) frameRate_->addItem(QString("%1 fps").arg(label), QString(value));
        const int k = frameRate_->findData(keep);
        frameRate_->setCurrentIndex(gif ? std::max(0, frameRate_->findData(QString("15"))) : std::max(0, k));
    }
    videoForm_->setRowVisible(quality_, !gif && !prores);
    videoForm_->setRowVisible(prores_, prores);
    videoForm_->setRowVisible(dither_, gif);
    videoForm_->setRowVisible(speed_, !gif && !prores);
    // audio codecs of the container
    {
        QSignalBlocker b(audioCodec_);
        audioCodec_->clear();
        for (const auto& c : f->audioCodecs) {
            if (!audioEncoderAvailable(c)) continue;
            if (c == "flac") {
                audioCodec_->addItem("FLAC 16-bit", "flac:16");
                audioCodec_->addItem("FLAC 24-bit", "flac:24");
            } else {
                audioCodec_->addItem(audioCodecLabel(c), QString::fromStdString(c));
            }
        }
    }
    audioBox_->setVisible(!f->audioCodecs.empty());
    audio_->setEnabled(video);   // audio-only formats always carry audio
    if (!video) audio_->setChecked(true);
    refreshEncoders();
}

void ExportDialog::refreshEncoders() {
    const ExportFormat* f = format();
    if (f->videoCodec.empty()) {
        updateSummary();
        return;
    }
    HardwareEncoding pref = HardwareEncoding::Cpu;
    hardwareFromId(hardware_->currentData().toString().toStdString(), pref);
    // bit depths that some allowed encoder can make here (checked sessions; before the check: what the format lists)
    {
        QSignalBlocker b(bitDepth_);
        const int keep = bitDepth_->currentData().toInt();
        bitDepth_->clear();
        const std::vector<int> depths = encodersChecked_ ? availableBitDepths(*f, pref) : f->bitDepths;
        for (int d : depths) bitDepth_->addItem(d == 10 ? (f->videoCodec == "prores" ? "10-bit 4:2:2" : "10-bit") : "8-bit", d);
        bitDepth_->setCurrentIndex(std::max(0, bitDepth_->findData(keep)));
        videoForm_->setRowVisible(bitDepth_, depths.size() > 1 || (depths.size() == 1 && depths.front() == 10));
    }
    // which hardware encoders of this codec work here
    QStringList works, fails;
    for (HardwareEncoding h : {HardwareEncoding::Nvenc, HardwareEncoding::Qsv, HardwareEncoding::Amf}) {
        const std::string e = encoderOf(f->videoCodec, h);
        const int item = hardware_->findData(QString(hardwareId(h)));
        if (e.empty()) {
            if (auto* m = qobject_cast<QStandardItemModel*>(hardware_->model())) m->item(item)->setEnabled(false);
            continue;
        }
        if (!encodersChecked_) continue;
        const bool ok = encoderWorks(e, 8) || encoderWorks(e, 10);
        (ok ? works : fails) << QString::fromStdString(e);
        if (auto* m = qobject_cast<QStandardItemModel*>(hardware_->model())) {
            m->item(item)->setEnabled(ok);
            m->item(item)->setText(QString(hardwareLabel(h)) + (ok ? "" : " - not available here"));
        }
    }
    const std::string cpu = encoderOf(f->videoCodec, HardwareEncoding::Cpu);
    if (!encodersChecked_) {
        encoderInfo_->setText("Checking the hardware encoders...");
    } else if (pref == HardwareEncoding::Cpu) {
        encoderInfo_->setText(QString("CPU encoder %1.%2").arg(QString::fromStdString(cpu))
                                  .arg(works.isEmpty() ? QString() : " Hardware here: " + works.join(", ") + "."));
    } else {
        encoderInfo_->setText(QString("Hardware that works here: %1; if it cannot be used for this export, %2 (CPU) is "
                                      "used and the export says so.")
                                  .arg(works.isEmpty() ? QString("none") : works.join(", "))
                                  .arg(QString::fromStdString(cpu)));
    }
    updateSummary();
}

QString ExportDialog::outputPath() const {
    QString n = name_->text().trimmed();
    if (n.isEmpty()) n = "export";
    // a typed media extension is replaced by the format's
    for (const auto& f : exportFormats()) {
        const QString ext = QString::fromStdString(f.extension);
        if (n.endsWith(ext, Qt::CaseInsensitive)) {
            n.chop(ext.size());
            break;
        }
    }
    return QDir(folder_->text().trimmed()).filePath(n + QString::fromStdString(format()->extension));
}

RenderOptions ExportDialog::options() const {
    RenderOptions o;
    const ExportFormat* f = format();
    o.format = f->id;
    hardwareFromId(hardware_->currentData().toString().toStdString(), o.hardware);
    o.encoder.preset = speed_->currentData().toString().toStdString();
    const int q = quality_->currentData().toInt();
    if (q < 0) o.encoder.bitrate = static_cast<int64_t>(bitrate_->value()) * 1000;
    else o.qualityLevel = q;
    o.encoder.bitDepth = bitDepth_->count() > 0 ? bitDepth_->currentData().toInt() : 8;
    o.encoder.proresProfile = prores_->currentData().toInt();
    o.encoder.dither = dither_->isChecked();
    const auto [w, h] = outputSize();
    const Sequence& s = doc_->sequence();
    if (w != s.width || h != s.height) {
        o.outputWidth = w;
        o.outputHeight = h;
    }
    if (!frameRate_->currentData().toString().isEmpty()) o.outputRate = outputRate();
    o.includeAudio = !f->audioCodecs.empty() && (audio_->isChecked() || f->videoCodec.empty());
    const QString ac = audioCodec_->currentData().toString();
    o.audioCodec = ac.section(':', 0, 0).toStdString();
    if (ac.startsWith("flac:")) o.encoder.audioBits = ac.section(':', 1).toInt();
    o.audioBitrate = audioBitrate_->currentData().toInt();
    o.audioSampleRate = sampleRate_->currentData().toInt();
    o.audioChannels = channels_->currentData().toInt();
    o.backend = static_cast<BackendChoice>(backend_->currentData().toInt());
    o.hardwareDecode = loadRenderSettings().hardwareDecode;
    o.titleRenderer = [] { return text::makeTitleEngine(); };
    if (rangeSelected_->isChecked() && !selectedSpan_->isChecked()) {
        o.onlyClips = selection_;
    } else {
        const auto [a, b] = frameRange();
        o.firstFrame = a;
        o.lastFrame = b;
    }
    return o;
}

QString ExportDialog::problem() const {
    const ExportFormat* f = format();
    const Sequence& s = doc_->sequence();
    if (s.frameCount() <= 0) return "The timeline is empty - add clips before exporting.";
    const QString name = name_->text().trimmed();
    if (name.isEmpty()) return "Give the file a name.";
    for (QChar c : name)
        if (QString("<>:\"/\\|?*").contains(c) || c.unicode() < 32) return QString("The name cannot contain %1").arg(c);
    if (name.endsWith('.') || reservedFileName(name)) return "Windows does not allow that file name.";
    const QString folder = folder_->text().trimmed();
    if (folder.isEmpty() || !QDir::isAbsolutePath(folder)) return "Choose a folder to save to.";
    const QString out = outputPath();
    if (out.size() >= 260) return "The path is too long for Windows (260 characters).";
    if (!sameFile(out, doc_->filePath()).isEmpty()) return "That is the project file itself.";
    for (const auto& src : doc_->project().sources)
        if (const QString clash = sameFile(out, doc_->mediaFilePath(src)); !clash.isEmpty())
            return "That file is used by the project (" + clash + "): pick another name.";
    if (rangeInOut_->isChecked() && !s.markIn && !s.markOut) return "Set In / Out marks first (Playback > Mark In / Mark Out).";
    if (rangeSelected_->isChecked() && frameRange().first > frameRange().second) return "None of the selected clips is on the timeline.";
    if (const auto r = frameRange(); r.first > r.second) return "The range is empty.";
    if (!f->videoCodec.empty()) {
        const auto [w, h] = outputSize();
        if (w % 2 || h % 2 || w < 64 || h < 64 || w > 8192 || h > 8192) return "The size must be even and 64 .. 8192 pixels.";
        if (bitDepth_->count() == 0) return QString("No encoder here can make %1.").arg(QString::fromStdString(f->label));
    }
    const int rate = sampleRate_->currentData().toInt() > 0 ? sampleRate_->currentData().toInt() : s.sampleRate;
    if (audioCodec_->currentData().toString() == "libopus" && rate != 48000 && (audio_->isChecked() || f->videoCodec.empty()))
        return "Opus needs 48 kHz audio: choose 48000 Hz.";
    return {};
}

void ExportDialog::updateSummary() {
    const ExportFormat* f = format();
    fileName_->setText("Saves as " + QDir::toNativeSeparators(outputPath()));
    videoForm_->setRowVisible(customSize_, resolution_->currentData().toInt() < 0);
    videoForm_->setRowVisible(bitrate_, quality_->currentData().toInt() < 0 && quality_->isVisibleTo(videoBox_));
    const QString ac = audioCodec_->currentData().toString();
    const bool lossy = ac == "aac" || ac == "libopus";
    audioCodec_->setEnabled(audio_->isChecked());
    audioBitrate_->setEnabled(audio_->isChecked() && lossy);
    sampleRate_->setEnabled(audio_->isChecked());
    channels_->setEnabled(audio_->isChecked());
    selectedSpan_->setEnabled(rangeSelected_->isChecked());
    audioNote_->setText(QString("Mixed at the chosen rate (the project's is %1 Hz).%2").arg(doc_->sequence().sampleRate)
                            .arg(ac == "libopus" && f->muxer == "mp4" ? " Opus in MP4 plays in browsers and VLC, not in every player." : ""));
    const auto [a, b] = frameRange();
    const Sequence& s = doc_->sequence();
    const QString why = problem();
    exportBtn_->setEnabled(why.isEmpty() || (job_ && job_->running()));
    if (a > b) {
        summary_->setText(why);
        return;
    }
    const double seconds = (frameToTime(b + 1, s.frameRate) - frameToTime(a, s.frameRate)).toDouble();
    const FrameRate r = f->videoCodec.empty() ? s.frameRate : outputRate();
    const int64_t frames = static_cast<int64_t>(std::ceil(seconds * r.toDouble() - 1e-9));
    const auto [w, h] = outputSize();
    // a rough size estimate: bit rate x duration, or typical bits per pixel for the quality preset
    double videoBits = 0;
    if (!f->videoCodec.empty()) {
        const double pixels = double(w) * h * r.toDouble();
        if (f->videoCodec == "prores") {   // Apple's published ProRes 422 target rates at 1920 x 1080, 29.97 fps
            static const double mbps[] = {45, 102, 147, 220};
            videoBits = mbps[std::clamp(prores_->currentData().toInt(), 0, 3)] * 1e6 * pixels / (1920.0 * 1080 * 29.97) * seconds;
        } else if (f->videoCodec == "gif") {
            videoBits = pixels * seconds * 0.8;
        } else if (quality_->currentData().toInt() < 0) {
            videoBits = bitrate_->value() * 1000.0 * seconds;
        } else {
            static const double bpp[] = {0.04, 0.055, 0.07, 0.095, 0.12};   // H.264 at the quality levels 0 .. 4
            const double codecFactor = f->videoCodec == "h264" ? 1.0 : f->videoCodec == "vp9" ? 0.7 : 0.6;
            videoBits = pixels * seconds * bpp[std::clamp(quality_->currentData().toInt(), 0, 4)] * codecFactor;
        }
    }
    double audioBits = 0;
    if (!f->audioCodecs.empty() && (audio_->isChecked() || f->videoCodec.empty())) {
        const int rate = (sampleRate_->currentData().toInt() > 0 ? sampleRate_->currentData().toInt() : s.sampleRate) *
                         channels_->currentData().toInt() / 2;
        audioBits = lossy ? audioBitrate_->currentData().toInt() * seconds
                    : ac == "pcm_s24le" || ac == "flac:24" ? rate * 2 * 24.0 * seconds * (ac.startsWith("flac") ? 0.6 : 1.0)
                                                           : rate * 2 * 16.0 * seconds * (ac.startsWith("flac") ? 0.6 : 1.0);
    }
    QString text = QString("%1 - %2 (%3 s)").arg(QString::fromStdString(formatTimecode(a, s.frameRate)))
                       .arg(QString::fromStdString(formatTimecode(b + 1, s.frameRate))).arg(seconds, 0, 'f', 2);
    if (!f->videoCodec.empty())
        text += QString(", %1 x %2, %3 fps, %4 frames").arg(w).arg(h).arg(r.toDouble(), 0, 'f', r.fps().isInteger() ? 0 : 3).arg(frames);
    text += QString(". Estimated size: about %1 (rough - it depends on the picture).").arg(sizeText((videoBits + audioBits) / 8));
    if (!why.isEmpty()) text += "\n" + why;
    summary_->setText(text);
}

void ExportDialog::startExport() {
    if (job_ && job_->running()) {   // the button acts as Cancel while exporting
        job_->cancel();
        status_->setText("Cancelling...");
        return;
    }
    if (const QString why = problem(); !why.isEmpty()) {
        lastOk_ = false;
        lastError_ = why;
        status_->setText(why);
        return;
    }
    const QString out = outputPath();
    if (!QDir().mkpath(QFileInfo(out).absolutePath())) {
        lastError_ = "Cannot create the folder " + QFileInfo(out).absolutePath();
        status_->setText(lastError_);
        return;
    }
    if (QFileInfo::exists(out) && !automation_ &&
        QMessageBox::question(this, "Export", QFileInfo(out).fileName() +
                                                  " already exists. Replace it? (It is kept until the new file is complete.)") != QMessageBox::Yes)
        return;
    // preflight: refuse with the full list of problems instead of failing half-way
    const ProjectSnapshot snap = doc_->snapshot();
    RenderOptions opt = options();
    opt.sequenceId = snap.sequenceId;
    DecoderPool pool(snap.projectFile);
    const auto issues = preflight(*snap.project, snap.projectFile, opt, pool);
    messages_->clear();
    for (const auto& i : issues)
        messages_->appendPlainText(QString(i.severity == ValidationIssue::Severity::Error ? "Error: " : "Warning: ") +
                                   QString::fromUtf8(i.where + ": " + i.message));
    messages_->setVisible(!issues.empty());
    if (hasErrors(issues)) {
        lastOk_ = false;
        lastError_ = QString::fromUtf8(formatIssues(issues));
        status_->setText("The project cannot be exported - see the problems above.");
        return;
    }
    // M7: AI pictures the export must prepare first (Background Removal, AI Interpolation, Video Upscaling): long
    // work - possibly hours on a CPU - only after a yes, with what it costs
    {
        const int64_t first = opt.onlyClips.empty() ? opt.firstFrame : 0;
        const int64_t end = opt.onlyClips.empty() && opt.lastFrame >= 0 ? opt.lastFrame + 1 : -1;
        const ai::PreparationEstimate est = ai::estimatePreparation(*snap.project, snap.sequenceId, snap.projectFile, first, end);
        if (ai::needsConfirmation(est)) {
            const QString text = QString("Before the export, the AI tools prepare %1.%2 Export now?")
                                     .arg(QString::fromStdString(est.text))
                                     .arg(est.cpu ? " There is no usable GPU for part of it, so this PC's processor does that work." : "");
            const bool yes = confirmSlow ? confirmSlow(text)
                             : automation_ ? true
                                           : QMessageBox::question(this, "Export", text) == QMessageBox::Yes;
            if (!yes) {
                lastOk_ = false;
                lastError_ = "Export not started: " + QString::fromStdString(est.text);
                status_->setText(lastError_);
                return;
            }
        }
    }
    running_ = opt;
    verification_.clear();
    job_ = std::make_unique<ExportJob>(snap, out, opt);
    connect(job_.get(), &ExportJob::finished, this, &ExportDialog::onFinished);
    startMs_ = QDateTime::currentMSecsSinceEpoch();
    progress_->setValue(0);
    openFolderBtn_->setVisible(false);
    exportBtn_->setText("Cancel");
    for (QWidget* w : std::initializer_list<QWidget*>{name_, folder_, format_, videoBox_, audioBox_}) w->setEnabled(false);
    poll_->start();
    job_->start();
}

void ExportDialog::onFinished(bool ok, bool cancelled, const QString& message) {
    poll_->stop();
    lastOk_ = ok;
    lastError_ = message;
    exportBtn_->setText("Export");
    for (QWidget* w : std::initializer_list<QWidget*>{name_, folder_, format_, videoBox_, audioBox_}) w->setEnabled(true);
    updateSummary();
    if (ok) {
        progress_->setValue(1000);
        const auto& r = job_->report();
        verification_ = verify(outputPath(), running_, r);
        status_->setText(QString("Exported %1 in %2 s with %3%4 (compositing: %5).\n%6")
                             .arg(QDir::toNativeSeparators(outputPath()))
                             .arg(r.seconds, 0, 'f', 1)
                             .arg(r.videoEncoder.empty() ? QString::fromStdString(r.audioEncoder) : QString::fromStdString(r.videoEncoder))
                             .arg(r.hardwareEncoder ? " (hardware)" : "")
                             .arg(QString::fromStdString(r.backend.empty() ? std::string("none") : r.backend))
                             .arg(verification_));
        messages_->clear();
        for (const auto& w : r.warnings) messages_->appendPlainText("Note: " + QString::fromStdString(w));
        if (!r.encoderNote.empty() && running_.hardware != HardwareEncoding::Cpu)
            messages_->appendPlainText("Encoder: " + QString::fromStdString(r.encoderNote));
        messages_->setVisible(!messages_->toPlainText().isEmpty());
        if (verification_.startsWith("Check failed")) {
            lastOk_ = false;
            lastError_ = verification_;
        }
        openFolderBtn_->setVisible(true);
    } else {
        progress_->setValue(0);
        status_->setText(cancelled ? message + " An existing file of that name was left as it was." : "Export failed.");
        if (!cancelled) {
            messages_->appendPlainText("Error: " + message);
            messages_->setVisible(true);
        }
    }
}

// The finished file, opened again: the streams the format promises, size, frame rate, duration, and the first and last
// frames decode.
QString ExportDialog::verify(const QString& file, const RenderOptions& o, const RenderReport& r) const {
    try {
        const std::filesystem::path p = toPath(file);
        const MediaInfo mi = probeMedia(p);
        const ExportFormat* f = findExportFormat(r.format);
        if (!f) return "Check failed: unknown format";
        QStringList facts, problems;
        const Sequence& s = doc_->sequence();
        const FrameRate rate = o.outputRate ? *o.outputRate : s.frameRate;
        double expected = 0;
        if (!f->videoCodec.empty()) {
            expected = r.framesWritten / rate.toDouble();
            if (!mi.video) {
                problems << "no video stream";
            } else {
                const auto& v = *mi.video;
                if (v.codec != f->videoCodec) problems << QString("video is %1, not %2").arg(QString::fromStdString(v.codec), QString::fromStdString(f->videoCodec));
                const int w = o.outputWidth > 0 ? o.outputWidth : s.width, h = o.outputHeight > 0 ? o.outputHeight : s.height;
                if (v.width != w || v.height != h) problems << QString("picture is %1 x %2, not %3 x %4").arg(v.width).arg(v.height).arg(w).arg(h);
                if (f->videoCodec != "gif" && v.frameRate != rate) problems << "frame rate " + QString::fromStdString(v.frameRate.toString());
                facts << QString("%1 %2 x %3 %4 fps").arg(QString::fromStdString(v.codec).toUpper()).arg(v.width).arg(v.height)
                              .arg(v.frameRate.toDouble(), 0, 'f', v.frameRate.fps().isInteger() ? 0 : 3);
                VideoDecoder dec(p);
                (void)dec.frameAt(Time(0));
                (void)dec.frameAt(frameToTime(std::max<int64_t>(0, r.framesWritten - 1), rate));
            }
        } else {
            expected = double(r.audioSamples) / (o.audioSampleRate > 0 ? o.audioSampleRate : s.sampleRate);
        }
        if (o.includeAudio && !f->audioCodecs.empty()) {
            const int wantRate = o.audioSampleRate > 0 ? o.audioSampleRate : s.sampleRate;
            if (!mi.audio) {
                problems << "no audio stream";
            } else {
                if (mi.audio->sampleRate != wantRate) problems << QString("audio at %1 Hz, not %2").arg(mi.audio->sampleRate).arg(wantRate);
                if (mi.audio->channels != o.audioChannels) problems << QString("%1 audio channels, not %2").arg(mi.audio->channels).arg(o.audioChannels);
                facts << QString("%1 %2 Hz %3").arg(QString::fromStdString(mi.audio->codec).toUpper()).arg(mi.audio->sampleRate)
                             .arg(mi.audio->channels == 1 ? "mono" : "stereo");
            }
        }
        if (mi.duration && std::fabs(mi.duration->toDouble() - expected) > 1.0 / rate.toDouble() + 0.05)
            problems << QString("lasts %1 s, expected %2 s").arg(mi.duration->toDouble(), 0, 'f', 3).arg(expected, 0, 'f', 3);
        facts << QString("%1 s").arg(mi.duration ? mi.duration->toDouble() : expected, 0, 'f', 3);
        return problems.isEmpty() ? "Checked: " + facts.join(", ") + "; the first and last frames decode."
                                  : "Check failed: " + problems.join("; ");
    } catch (const std::exception& e) {
        return "Check failed: " + QString::fromUtf8(e.what());
    }
}

bool ExportDialog::exportTo(const QString& file, QString* error) {
    const QFileInfo fi(file);
    folder_->setText(QDir::toNativeSeparators(fi.absolutePath()));
    name_->setText(fi.completeBaseName());
    if (const ExportFormat* f = exportFormatForPath(fi.fileName().toStdString()); f && f->extension != format()->extension)
        format_->setCurrentIndex(format_->findData(QString::fromStdString(f->id)));
    lastOk_ = false;
    lastError_.clear();
    automation_ = true;   // no questions: an existing file is replaced once the new one is complete
    startExport();
    automation_ = false;
    if (job_ && job_->running()) {
        QEventLoop loop;
        connect(job_.get(), &ExportJob::finished, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (!lastOk_ && error) *error = lastError_;
    return lastOk_;
}

void ExportDialog::reject() {
    if (job_ && job_->running()) {
        if (QMessageBox::question(this, "Export", "An export is running. Cancel it?") != QMessageBox::Yes) return;
        job_->cancel();
        QEventLoop loop;
        connect(job_.get(), &ExportJob::finished, &loop, &QEventLoop::quit);
        if (job_->running()) loop.exec();
    }
    QDialog::reject();
}

// ---- SequenceSettingsDialog --------------------------------------------------------------------

SequenceSettingsDialog::SequenceSettingsDialog(const SequenceSettings& in, const QString& title, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(title);
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    resolution_ = new QComboBox(this);
    const QList<QSize> sizes = {{3840, 2160}, {2560, 1440}, {1920, 1080}, {1280, 720}, {1080, 1920}, {1080, 1080}, {720, 1280}};
    for (const QSize& s : sizes) {
        const QString label = QString("%1 x %2  (%3)").arg(s.width()).arg(s.height())
                                  .arg(s.width() > s.height() ? "16:9" : s.width() == s.height() ? "1:1" : "9:16");
        resolution_->addItem(label, s);
    }
    int ri = resolution_->findData(QSize(in.width, in.height));
    if (ri < 0) {
        resolution_->addItem(QString("%1 x %2").arg(in.width).arg(in.height), QSize(in.width, in.height));
        ri = resolution_->count() - 1;
    }
    resolution_->setCurrentIndex(ri);
    fps_ = new QComboBox(this);
    const QList<QPair<QString, QPair<int, int>>> rates = {{"23.976", {24000, 1001}}, {"24", {24, 1}}, {"25", {25, 1}},
                                                          {"29.97", {30000, 1001}}, {"30", {30, 1}}, {"50", {50, 1}},
                                                          {"59.94", {60000, 1001}}, {"60", {60, 1}}};
    for (const auto& r : rates) fps_->addItem(r.first + " fps", QPoint(r.second.first, r.second.second));
    int fi = fps_->findData(QPoint(static_cast<int>(in.frameRate.fps().num()), static_cast<int>(in.frameRate.fps().den())));
    fps_->setCurrentIndex(fi < 0 ? 4 : fi);
    rate_ = new QComboBox(this);
    rate_->addItem("48000 Hz", 48000);
    rate_->addItem("44100 Hz", 44100);
    rate_->setCurrentIndex(in.sampleRate == 44100 ? 1 : 0);
    form->addRow("Resolution", resolution_);
    form->addRow("Frame rate", fps_);
    form->addRow("Audio sample rate", rate_);
    lay->addLayout(form);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
}

SequenceSettings SequenceSettingsDialog::settings() const {
    SequenceSettings s;
    const QSize sz = resolution_->currentData().toSize();
    s.width = sz.width();
    s.height = sz.height();
    const QPoint r = fps_->currentData().toPoint();
    s.frameRate = FrameRate(r.x(), r.y());
    s.sampleRate = rate_->currentData().toInt();
    return s;
}

// ---- subtitle import -----------------------------------------------------------------------------

const std::vector<std::pair<unsigned, QString>>& subtitleCodePages() {
    static const std::vector<std::pair<unsigned, QString>> pages = {
        {0, "Automatic (Unicode, else the Windows code page)"},
        {1252, "Western European (Windows-1252)"},
        {1250, "Central European (Windows-1250)"},
        {1251, "Cyrillic (Windows-1251)"},
        {1253, "Greek (Windows-1253)"},
        {1254, "Turkish (Windows-1254)"},
        {1255, "Hebrew (Windows-1255)"},
        {1256, "Arabic (Windows-1256)"},
        {1257, "Baltic (Windows-1257)"},
        {1258, "Vietnamese (Windows-1258)"},
        {874, "Thai (Windows-874)"},
        {932, "Japanese (Shift-JIS)"},
        {936, "Simplified Chinese (GBK)"},
        {949, "Korean (EUC-KR)"},
        {950, "Traditional Chinese (Big5)"},
    };
    return pages;
}

SubtitleImportDialog::SubtitleImportDialog(const QString& file, QByteArray bytes, const Time& playhead, QWidget* parent)
    : QDialog(parent), fileName_(file), bytes_(std::move(bytes)), playhead_(playhead) {
    setWindowTitle("Import Subtitles");
    resize(620, 560);
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    auto* name = new QLabel(QFileInfo(file).fileName(), this);
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow("File", name);
    encoding_ = new QComboBox(this);
    encoding_->setObjectName("subtitleEncoding");
    for (const auto& [cp, label] : subtitleCodePages()) encoding_->addItem(label, cp);
    encoding_->setToolTip("UTF-8 and UTF-16 files are always read as Unicode. Choose the code page of older files "
                          "whose accented or non-Latin letters look wrong in the preview.");
    form->addRow("Text encoding", encoding_);
    placement_ = new QComboBox(this);
    placement_->setObjectName("subtitlePlacement");
    placement_->addItem("At the times in the file (from the start of the timeline)");
    placement_->addItem(QString("From the playhead (%1 s)").arg(playhead.toDouble(), 0, 'f', 3));
    form->addRow("Place captions", placement_);
    lay->addLayout(form);
    summary_ = new QLabel(this);
    summary_->setObjectName("subtitleSummary");
    summary_->setWordWrap(true);
    lay->addWidget(summary_);
    preview_ = new QPlainTextEdit(this);
    preview_->setObjectName("subtitlePreview");
    preview_->setReadOnly(true);
    lay->addWidget(preview_, 3);
    lay->addWidget(new QLabel("Not imported exactly (MeerCut keeps the text and timing):", this));
    warnings_ = new QPlainTextEdit(this);
    warnings_->setObjectName("subtitleWarnings");
    warnings_->setReadOnly(true);
    lay->addWidget(warnings_, 2);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText("Import");
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
    connect(encoding_, &QComboBox::currentIndexChanged, this, &SubtitleImportDialog::reparse);
    reparse();
}

unsigned SubtitleImportDialog::codePage() const { return encoding_->currentData().toUInt(); }

Time SubtitleImportDialog::offset() const { return placement_->currentIndex() == 1 ? playhead_ : Time(0); }

void SubtitleImportDialog::reparse() {
    try {
        doc_ = parseSubtitles(std::string_view(bytes_.constData(), static_cast<size_t>(bytes_.size())),
                              QFileInfo(fileName_).fileName().toStdString(), codePage());
    } catch (const std::exception& e) {
        doc_ = SubtitleDoc{};
        doc_.warnings.push_back(e.what());
    }
    summary_->setText(QString("%1 caption(s), text read as %2.")
                          .arg(doc_.captions.size())
                          .arg(QString::fromStdString(doc_.encoding.empty() ? std::string("?") : doc_.encoding)));
    QString text;
    const size_t shown = std::min<size_t>(doc_.captions.size(), 200);
    for (size_t i = 0; i < shown; ++i) {
        const Caption& c = doc_.captions[i];
        text += QString("%1 --> %2\n%3\n\n")
                    .arg(QString::fromStdString(formatSrtTime(c.start)))
                    .arg(QString::fromStdString(formatSrtTime(c.end)))
                    .arg(QString::fromStdString(c.text));
    }
    if (doc_.captions.size() > shown) text += QString("... and %1 more").arg(doc_.captions.size() - shown);
    preview_->setPlainText(text);
    QStringList w;
    for (const auto& s : doc_.warnings) w << QString::fromStdString(s);
    warnings_->setPlainText(w.isEmpty() ? QString("Nothing - everything in the file is supported.") : w.join("\n"));
    if (auto* ok = findChild<QDialogButtonBox*>()) ok->button(QDialogButtonBox::Ok)->setEnabled(!doc_.captions.empty());
}

}  // namespace fa::ui
