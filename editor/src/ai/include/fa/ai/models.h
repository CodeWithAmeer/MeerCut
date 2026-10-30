// M7 local AI: the model catalogue (what can be installed, from where, under which licences), the model folder and the
// integrity-checked installer. Models are never bundled with MeerCut and never downloaded without the user asking:
// the editor's Model Manager calls installModel() after the user confirms (name, size, source, licence shown). Once
// installed, every model runs offline - nothing in fa_ai other than installModel() touches the network.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fa::ai {

struct ModelError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// One file of an installed model. When `archiveMember` is set, `url` names a zip archive and the file is that member.
struct ModelFile {
    std::string path;            // relative to the model's folder, e.g. "flownet.bin"
    std::string url;             // https:// (or file:// in tests)
    uint64_t bytes = 0;          // size of the installed file
    std::string sha256;          // of the installed file (lower-case hex)
    std::string archiveMember;   // e.g. "models/realesrgan-x4plus.bin"
    uint64_t archiveBytes = 0;   // size of the archive at `url` (when archiveMember is set)
    std::string archiveSha256;   // sha256 of the archive (when archiveMember is set)
};

enum class ModelRuntime { Onnx, Ncnn, Whisper };

struct ModelInfo {
    std::string id;              // stable id stored in projects, e.g. "rvm-mobilenetv3"
    std::string name;            // UI name
    std::string purpose;         // what MeerCut uses it for
    std::string version;         // upstream release / commit
    ModelRuntime runtime = ModelRuntime::Onnx;
    std::string codeLicence;     // licence of the upstream code
    std::string weightsLicence;  // licence of the weights, as stated (or "not stated separately ...")
    std::string source;          // upstream project and release
    std::string attribution;     // paper / authors to credit
    std::string redistribution;  // what MeerCut does with it (downloaded from upstream on request, not bundled)
    bool optional = false;       // large or alternative models; the features work without them
    std::vector<ModelFile> files;
    uint64_t downloadBytes() const;   // what installing it downloads
    uint64_t installedBytes() const;  // disk space once installed
};

const std::vector<ModelInfo>& modelCatalog();
const ModelInfo* findModel(const std::string& id);
const char* runtimeName(ModelRuntime r);   // "ONNX Runtime", "ncnn", "whisper.cpp"

// The model folder: MEERCUT_MODELS if set, else %LOCALAPPDATA%\MeerCut\models. Each model lives in <folder>/<id>/.
std::filesystem::path modelsDirectory();
void setModelsDirectoryOverride(const std::filesystem::path& dir);   // tests and the Model Manager's "change folder"
std::filesystem::path modelFolder(const ModelInfo& m, const std::filesystem::path& root = modelsDirectory());
std::filesystem::path modelFilePath(const ModelInfo& m, size_t file, const std::filesystem::path& root = modelsDirectory());

enum class ModelStatus { Missing, Partial, Installed, Corrupt };
const char* statusName(ModelStatus s);
// Missing = no file; Partial = some files; Installed = all files with the right sizes (and, when verify is set, the
// right SHA-256); Corrupt = a file has the wrong size or hash. Hashing large models takes a moment, so the Model
// Manager checks sizes on display and verifies hashes on request and after every install.
ModelStatus modelStatus(const ModelInfo& m, bool verifyHashes, const std::filesystem::path& root = modelsDirectory());
// Throws ModelError naming the model and the missing / corrupt file unless the model is installed (sizes checked).
void requireModel(const ModelInfo& m, const std::filesystem::path& root = modelsDirectory());

struct InstallProgress {
    std::function<void(uint64_t done, uint64_t total)> progress;   // bytes downloaded of downloadBytes()
    const std::atomic<bool>* cancel = nullptr;
};
// Downloads every file (archives once, then extracts their members), checks sizes and SHA-256 of what was downloaded
// and of every installed file, and only then moves them into the model's folder. A failed, cancelled or mismatched
// download leaves no partial model behind (temporary files are removed) and throws ModelError.
void installModel(const ModelInfo& m, const InstallProgress& p = {}, const std::filesystem::path& root = modelsDirectory());
// Offline installation of separately obtained files: `folder` holds the model's files (by file name) or, for archive
// members, the release archive itself. Every file is checked against the catalogue's SHA-256 before it is copied;
// a missing or different file throws ModelError naming it and installs nothing.
void installModelFromFolder(const ModelInfo& m, const std::filesystem::path& folder,
                            const std::filesystem::path& root = modelsDirectory());
void removeModel(const ModelInfo& m, const std::filesystem::path& root = modelsDirectory());

// Lower-case hex SHA-256 of a file / of bytes (Windows CNG).
std::string sha256File(const std::filesystem::path& file);
std::string sha256Bytes(const void* data, size_t size);

// Download `url` to `target` (https via WinHTTP, file:// by copying); throws ModelError, removes `target` on failure.
// This is the only code in MeerCut that uses the network (the Model Manager's Install); networkRequests() counts the
// https downloads it started in this process (tests: no model operation ever reaches it).
void downloadFile(const std::string& url, const std::filesystem::path& target, uint64_t expectedBytes,
                  const std::function<void(uint64_t)>& onBytes, const std::atomic<bool>* cancel);

uint64_t networkRequests();

// Read one member of a zip archive (stored or deflated).
std::vector<uint8_t> readZipMember(const std::filesystem::path& zip, const std::string& member);

}  // namespace fa::ai
