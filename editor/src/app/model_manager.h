// M7 AI Tools > Model Manager: the AI devices of this PC, the model catalogue with each model's size, version,
// licences, source and status, and downloading (only after the user confirms), verifying, installing from separately
// obtained files and removing models. Long operations run on a worker thread with progress and Cancel.
#pragma once

#include <QDialog>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTextBrowser;
class QTimer;

namespace fa::ui {

class ModelManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit ModelManagerDialog(QWidget* parent = nullptr);
    ~ModelManagerDialog() override;

    void refresh(bool verifyHashes = false);   // re-read every model's status
    bool busy() const { return worker_.joinable(); }
    // Tests: the confirmation before a download (default: a QMessageBox). Return true to download.
    std::function<bool(const QString& text)> confirmDownload;

private:
    void selectionChanged();
    int selectedModel() const;   // index into the catalogue, or -1
    void startDownload();
    void startVerify();
    void installFromFolder();
    void removeSelected();
    void chooseFolder();
    void run(const QString& what, std::function<std::string()> job);   // job returns "" on success, else an error
    void poll();

    QLabel* devices_ = nullptr;
    QLineEdit* folder_ = nullptr;
    QTableWidget* table_ = nullptr;
    QTextBrowser* details_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* download_ = nullptr;
    QPushButton* verify_ = nullptr;
    QPushButton* fromFolder_ = nullptr;
    QPushButton* remove_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QTimer* timer_ = nullptr;

    std::thread worker_;
    std::atomic<bool> cancelFlag_{false};
    std::atomic<bool> done_{false};
    std::atomic<uint64_t> doneBytes_{0}, totalBytes_{0};
    std::string result_;   // written by the worker before done_ is set
    QString running_;
};

}  // namespace fa::ui
