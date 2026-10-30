#include "model_manager.h"

#include "theme.h"

#include "fa/ai/inference.h"
#include "fa/ai/models.h"

#include <QDesktopServices>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace fa::ui {

namespace {

// Table columns: what matters first (status, size, licence), the long description last.
enum Column { ColModel, ColStatus, ColDownload, ColLicence, ColRuntime, ColVersion, ColPurpose, ColCount };

QString mb(uint64_t bytes) { return QString::number(bytes / 1048576.0, 'f', bytes < 10 * 1048576ull ? 1 : 0) + " MB"; }

QString deviceText() {
    const ai::AiCapabilities& c = ai::aiCapabilities();
    QStringList lines;
    lines << QString("CPU: %1 (%2 threads)").arg(QString::fromStdString(c.cpuName)).arg(c.cpuThreads);
    for (const auto& g : c.gpus) {
        QStringList how;
        if (g.dxgiIndex >= 0 && c.directMl) how << "DirectML";
        if (g.vulkanIndex >= 0) how << "Vulkan";
        lines << QString("GPU: %1 (%2, %3 MB) - %4")
                     .arg(QString::fromStdString(g.name), g.discrete ? "discrete" : "integrated")
                     .arg(g.memoryMb)
                     .arg(how.isEmpty() ? "not usable for AI" : how.join(" + "));
    }
    lines << QString("ONNX Runtime %1%2; Windows DirectML %3")
                 .arg(QString::fromStdString(c.onnxRuntimeVersion), c.directMl ? " with DirectML" : " (CPU only)",
                      c.directMlVersion.empty() ? "not found" : QString::fromStdString(c.directMlVersion));
    for (const auto& n : c.notes) lines << QString::fromStdString(n);
    lines << "Everything runs on this PC. Nothing is uploaded; models download only when you ask.";
    return lines.join("\n");
}

}  // namespace

ModelManagerDialog::ModelManagerDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Model Manager");
    setObjectName("modelManager");
    resize(1100, 640);
    auto* lay = new QVBoxLayout(this);

    devices_ = new QLabel(deviceText(), this);
    devices_->setObjectName("aiDevices");
    devices_->setWordWrap(true);
    devices_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(devices_);

    auto* folderRow = new QHBoxLayout();
    folderRow->addWidget(new QLabel("Model folder", this));
    folder_ = new QLineEdit(QString::fromStdWString(ai::modelsDirectory().wstring()), this);
    folder_->setObjectName("modelFolder");
    folder_->setReadOnly(true);
    folderRow->addWidget(folder_, 1);
    auto* openFolder = new QPushButton("Open", this);
    auto* change = new QPushButton("Change...", this);
    folderRow->addWidget(openFolder);
    folderRow->addWidget(change);
    lay->addLayout(folderRow);

    table_ = new QTableWidget(this);
    table_->setObjectName("modelTable");
    table_->setColumnCount(ColCount);
    table_->setHorizontalHeaderLabels({"Model", "Status", "Download", "Licence (code / weights)", "Runtime", "Version", "Used for"});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    const auto& cat = ai::modelCatalog();
    table_->setRowCount(static_cast<int>(cat.size()));
    for (int r = 0; r < static_cast<int>(cat.size()); ++r) {
        const ai::ModelInfo& m = cat[static_cast<size_t>(r)];
        auto set = [&](int col, const QString& text) { table_->setItem(r, col, new QTableWidgetItem(text)); };
        set(ColModel, QString::fromStdString(m.name) + (m.optional ? "  (optional)" : ""));
        set(ColStatus, "");
        set(ColDownload, mb(m.downloadBytes()));
        set(ColLicence, QString::fromStdString(m.codeLicence) + " / " + QString::fromStdString(m.weightsLicence));
        set(ColRuntime, ai::runtimeName(m.runtime));
        set(ColVersion, QString::fromStdString(m.version));
        set(ColPurpose, QString::fromStdString(m.purpose));
    }
    table_->resizeColumnsToContents();
    table_->setColumnWidth(ColStatus, 140);
    table_->setColumnWidth(ColLicence, 280);
    lay->addWidget(table_, 3);

    details_ = new QTextBrowser(this);
    details_->setObjectName("modelDetails");
    lay->addWidget(details_, 2);

    progress_ = new QProgressBar(this);
    progress_->setObjectName("modelProgress");
    progress_->setRange(0, 1000);
    progress_->setValue(0);
    lay->addWidget(progress_);
    status_ = new QLabel(this);
    status_->setObjectName("modelStatus");
    status_->setWordWrap(true);
    lay->addWidget(status_);

    auto* buttons = new QHBoxLayout();
    download_ = new QPushButton("Download...", this);
    download_->setObjectName("modelDownload");
    verify_ = new QPushButton("Verify", this);
    verify_->setObjectName("modelVerify");
    fromFolder_ = new QPushButton("Install from Folder...", this);
    fromFolder_->setObjectName("modelFromFolder");
    remove_ = new QPushButton("Remove", this);
    remove_->setObjectName("modelRemove");
    cancel_ = new QPushButton("Cancel", this);
    cancel_->setObjectName("modelCancel");
    cancel_->setEnabled(false);
    auto* close = new QPushButton("Close", this);
    for (QPushButton* b : {download_, verify_, fromFolder_, remove_, cancel_}) buttons->addWidget(b);
    buttons->addStretch(1);
    buttons->addWidget(close);
    lay->addLayout(buttons);

    timer_ = new QTimer(this);
    timer_->setInterval(100);
    connect(timer_, &QTimer::timeout, this, &ModelManagerDialog::poll);
    connect(table_, &QTableWidget::itemSelectionChanged, this, &ModelManagerDialog::selectionChanged);
    connect(download_, &QPushButton::clicked, this, &ModelManagerDialog::startDownload);
    connect(verify_, &QPushButton::clicked, this, &ModelManagerDialog::startVerify);
    connect(fromFolder_, &QPushButton::clicked, this, &ModelManagerDialog::installFromFolder);
    connect(remove_, &QPushButton::clicked, this, &ModelManagerDialog::removeSelected);
    connect(cancel_, &QPushButton::clicked, this, [this] { cancelFlag_ = true; });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(openFolder, &QPushButton::clicked, this, [] {
        std::filesystem::create_directories(ai::modelsDirectory());
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(ai::modelsDirectory().wstring())));
    });
    connect(change, &QPushButton::clicked, this, &ModelManagerDialog::chooseFolder);

    refresh(false);
    if (table_->rowCount() > 0) table_->selectRow(0);
}

ModelManagerDialog::~ModelManagerDialog() {
    cancelFlag_ = true;
    if (worker_.joinable()) worker_.join();
}

int ModelManagerDialog::selectedModel() const {
    const auto rows = table_->selectionModel()->selectedRows();
    return rows.isEmpty() ? -1 : rows.first().row();
}

void ModelManagerDialog::refresh(bool verifyHashes) {
    const auto& cat = ai::modelCatalog();
    for (int r = 0; r < static_cast<int>(cat.size()); ++r) {
        const ai::ModelStatus s = ai::modelStatus(cat[static_cast<size_t>(r)], verifyHashes);
        QString text = ai::statusName(s);
        if (s == ai::ModelStatus::Installed && verifyHashes) text += " (verified)";
        table_->item(r, ColStatus)->setText(text);
        table_->item(r, ColStatus)->setForeground(s == ai::ModelStatus::Installed ? QColor(0x7F, 0xD8, 0x8F)
                                          : s == ai::ModelStatus::Corrupt ? QColor(0xFF, 0x7A, 0x7A)
                                                                          : color::textDim);
    }
    selectionChanged();
}

void ModelManagerDialog::selectionChanged() {
    const int i = selectedModel();
    const bool idle = !busy();
    download_->setEnabled(idle && i >= 0);
    verify_->setEnabled(idle && i >= 0);
    fromFolder_->setEnabled(idle && i >= 0);
    remove_->setEnabled(idle && i >= 0);
    if (i < 0) {
        details_->clear();
        return;
    }
    const ai::ModelInfo& m = ai::modelCatalog()[static_cast<size_t>(i)];
    QString html = QString("<b>%1</b> - %2<br>").arg(QString::fromStdString(m.name).toHtmlEscaped(),
                                                      QString::fromStdString(m.purpose).toHtmlEscaped());
    html += QString("Source: %1<br>Code licence: %2<br>Weights licence: %3<br>Credit: %4<br>Distribution: %5<br>"
                    "Download %6, installed %7, folder %8<br>Files:<br>")
                .arg(QString::fromStdString(m.source).toHtmlEscaped(), QString::fromStdString(m.codeLicence).toHtmlEscaped(),
                     QString::fromStdString(m.weightsLicence).toHtmlEscaped(),
                     QString::fromStdString(m.attribution).toHtmlEscaped(),
                     QString::fromStdString(m.redistribution).toHtmlEscaped(), mb(m.downloadBytes()), mb(m.installedBytes()),
                     QString::fromStdWString(ai::modelFolder(m).wstring()).toHtmlEscaped());
    for (const auto& f : m.files)
        html += QString("&nbsp;&nbsp;%1 (%2 bytes) SHA-256 <tt>%3</tt><br>")
                    .arg(QString::fromStdString(f.path).toHtmlEscaped())
                    .arg(f.bytes)
                    .arg(QString::fromStdString(f.sha256));
    details_->setHtml(html);
}

void ModelManagerDialog::run(const QString& what, std::function<std::string()> job) {
    if (busy()) return;
    running_ = what;
    cancelFlag_ = false;
    done_ = false;
    doneBytes_ = 0;
    totalBytes_ = 0;
    status_->setText(what + "...");
    progress_->setValue(0);
    cancel_->setEnabled(true);
    worker_ = std::thread([this, job = std::move(job)] {
        try {
            result_ = job();
        } catch (const std::exception& e) {
            result_ = e.what();
        }
        done_ = true;
    });
    selectionChanged();
    timer_->start();
}

void ModelManagerDialog::poll() {
    if (totalBytes_ > 0) progress_->setValue(static_cast<int>(1000.0 * doneBytes_ / totalBytes_));
    if (totalBytes_ > 0)
        status_->setText(QString("%1: %2 of %3").arg(running_, mb(doneBytes_), mb(totalBytes_)));
    if (!done_) return;
    timer_->stop();
    worker_.join();
    cancel_->setEnabled(false);
    const QString r = QString::fromStdString(result_);
    status_->setText(r.isEmpty() ? running_ + ": done." : running_ + " failed: " + r);
    progress_->setValue(r.isEmpty() ? 1000 : 0);
    refresh(running_.startsWith("Verif"));
}

void ModelManagerDialog::startDownload() {
    const int i = selectedModel();
    if (i < 0) return;
    const ai::ModelInfo& m = ai::modelCatalog()[static_cast<size_t>(i)];
    const QString url = QString::fromStdString(m.files.front().url);
    const QString text = QString("Download \"%1\"?\n\n%2 from %3\nVersion: %4\nCode licence: %5\nWeights licence: %6\n\n"
                                 "It is saved in %7 and checked against the SHA-256 recorded in MeerCut before it is "
                                 "installed. Afterwards it runs without any connection.")
                             .arg(QString::fromStdString(m.name), mb(m.downloadBytes()), QUrl(url).host(),
                                  QString::fromStdString(m.version), QString::fromStdString(m.codeLicence),
                                  QString::fromStdString(m.weightsLicence),
                                  QString::fromStdWString(ai::modelFolder(m).wstring()));
    const bool ok = confirmDownload ? confirmDownload(text)
                                    : QMessageBox::question(this, "Download Model", text) == QMessageBox::Yes;
    if (!ok) {
        status_->setText("Nothing was downloaded.");
        return;
    }
    totalBytes_ = m.downloadBytes();
    run("Downloading " + QString::fromStdString(m.name), [this, &m] {
        ai::installModel(m, {[this](uint64_t d, uint64_t t) {
                                 doneBytes_ = d;
                                 totalBytes_ = t;
                             },
                             &cancelFlag_});
        return std::string();
    });
}

void ModelManagerDialog::startVerify() {
    const int i = selectedModel();
    if (i < 0) return;
    const ai::ModelInfo& m = ai::modelCatalog()[static_cast<size_t>(i)];
    run("Verifying " + QString::fromStdString(m.name), [&m] {
        const ai::ModelStatus s = ai::modelStatus(m, true);
        if (s == ai::ModelStatus::Installed) return std::string();
        return std::string(ai::statusName(s));
    });
}

void ModelManagerDialog::installFromFolder() {
    const int i = selectedModel();
    if (i < 0) return;
    const ai::ModelInfo& m = ai::modelCatalog()[static_cast<size_t>(i)];
    const QString dir = QFileDialog::getExistingDirectory(this, "Folder with the files of " + QString::fromStdString(m.name));
    if (dir.isEmpty()) return;
    const std::filesystem::path folder = dir.toStdWString();
    run("Installing " + QString::fromStdString(m.name) + " from a folder", [&m, folder] {
        ai::installModelFromFolder(m, folder);
        return std::string();
    });
}

void ModelManagerDialog::removeSelected() {
    const int i = selectedModel();
    if (i < 0) return;
    const ai::ModelInfo& m = ai::modelCatalog()[static_cast<size_t>(i)];
    if (QMessageBox::question(this, "Remove Model", "Remove \"" + QString::fromStdString(m.name) + "\" from this PC?") !=
        QMessageBox::Yes)
        return;
    ai::removeModel(m);
    status_->setText("Removed " + QString::fromStdString(m.name) + ".");
    refresh(false);
}

void ModelManagerDialog::chooseFolder() {
    const QString dir = QFileDialog::getExistingDirectory(this, "Model folder", folder_->text());
    if (dir.isEmpty()) return;
    QSettings().setValue("ai/modelsDir", dir);
    ai::setModelsDirectoryOverride(dir.toStdWString());
    folder_->setText(dir);
    refresh(false);
}

}  // namespace fa::ui
