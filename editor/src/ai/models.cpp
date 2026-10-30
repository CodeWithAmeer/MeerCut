// M7 model catalogue, model folder, integrity-checked installer (WinHTTP), SHA-256 (CNG) and a minimal zip reader.
#include "fa/ai/models.h"

#include "fa/core/app_data.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

namespace fa::ai {

namespace {

constexpr const char* kUpstreamOnly =
    "Not bundled with MeerCut. Downloaded from the upstream release only when the user asks (Model Manager), "
    "checked against the SHA-256 recorded here, stored in the user's model folder.";

const std::string kWhisperRev = "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/";
const std::string kRifeRev = "https://raw.githubusercontent.com/nihui/rife-ncnn-vulkan/a7532fc3f9f8f008cd6eecd6f2ffe2a9698e0cf7/models/rife-v4.6/";
const std::string kEsrZip = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip";
constexpr uint64_t kEsrZipBytes = 45474481;
const std::string kEsrZipSha = "abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d";

ModelInfo whisper(const char* id, const char* name, const char* purpose, const char* file, uint64_t bytes,
                  const char* sha, bool optional) {
    return {id, name, purpose, std::string("OpenAI Whisper, ggml conversion (") + file + ")", ModelRuntime::Whisper,
            "MIT (whisper.cpp)", "MIT (OpenAI Whisper weights)",
            "huggingface.co/ggerganov/whisper.cpp @ 5359861 (whisper.cpp project)",
            "Alec Radford et al.: Robust Speech Recognition via Large-Scale Weak Supervision (OpenAI, 2022); ggml "
            "conversion by the whisper.cpp project (Georgi Gerganov et al.)",
            kUpstreamOnly, optional, {{file, kWhisperRev + file, bytes, sha}}};
}

ModelFile esr(const char* name, uint64_t bytes, const char* sha) {
    return {name, kEsrZip, bytes, sha, std::string("models/") + name, kEsrZipBytes, kEsrZipSha};
}

std::vector<ModelInfo> makeCatalog() {
    std::vector<ModelInfo> c;
    c.push_back({"rvm-mobilenetv3", "Robust Video Matting (MobileNetV3)",
                 "Background Removal: people in video, temporally stable (recurrent), alpha with hair detail",
                 "v1.0.0, fp32 ONNX (opset 12)", ModelRuntime::Onnx, "GPL-3.0 (RobustVideoMatting repository)",
                 "Not stated separately; released with the GPL-3.0 repository (treated as GPL-3.0)",
                 "github.com/PeterL1n/RobustVideoMatting, release v1.0.0",
                 "Shanchuan Lin, Linjie Yang, Imran Saleemi, Soumyadip Sengupta: Robust High-Resolution Video Matting "
                 "with Temporal Guidance (WACV 2022)",
                 kUpstreamOnly, false,
                 {{"rvm_mobilenetv3_fp32.onnx",
                   "https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_mobilenetv3_fp32.onnx",
                   14975696, "88d4531297118f595bf2fd60f6f566aec2e559393802d1f436c380f0cbbd2828"}}});
    c.push_back({"birefnet-general-tiny", "BiRefNet general (Swin-T)",
                 "Background Removal: general objects and products (pictures and short clips; heavy, frame by frame)",
                 "v1, epoch 232, ONNX", ModelRuntime::Onnx, "MIT (BiRefNet repository)",
                 "MIT (released by the author with the repository)", "github.com/ZhengPeng7/BiRefNet, release v1",
                 "Peng Zheng, Dehong Gao, Deng-Ping Fan, Li Liu, Jorma Laaksonen, Wanli Ouyang, Nicu Sebe: Bilateral "
                 "Reference for High-Resolution Dichotomous Image Segmentation (CAAI AIR 2024)",
                 kUpstreamOnly, true,
                 {{"BiRefNet-general-bb_swin_v1_tiny-epoch_232.onnx",
                   "https://github.com/ZhengPeng7/BiRefNet/releases/download/v1/BiRefNet-general-bb_swin_v1_tiny-epoch_232.onnx",
                   224005088, "5600024376f572a557870a5eb0afb1e5961636bef4e1e22132025467d0f03333"}}});
    c.push_back({"yunet-face", "YuNet face detector", "Auto Reframe: finds the faces to keep in frame",
                 "2023mar, ONNX", ModelRuntime::Onnx, "MIT (OpenCV Zoo)", "MIT",
                 "huggingface.co/opencv/face_detection_yunet @ 3cc26e7 (OpenCV Zoo)",
                 "Wei Wu, Hanyang Peng, Shiqi Yu: YuNet: A Tiny Millisecond-level Face Detector (Machine Intelligence "
                 "Research, 2023)",
                 kUpstreamOnly, false,
                 {{"face_detection_yunet_2023mar.onnx",
                   "https://huggingface.co/opencv/face_detection_yunet/resolve/3cc26e7f1014a5ee5d74a42acee58bafc9d0a310/"
                   "face_detection_yunet_2023mar.onnx",
                   232589, "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4"}}});
    c.push_back(whisper("whisper-base", "Whisper base (multilingual)",
                        "Speech to Text: 99 languages with language detection; the default", "ggml-base.bin", 147951465,
                        "60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe", false));
    c.push_back(whisper("whisper-base.en", "Whisper base.en (English)", "Speech to Text: English only, more accurate "
                        "than base on English", "ggml-base.en.bin", 147964211,
                        "a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002", true));
    c.push_back(whisper("whisper-tiny", "Whisper tiny (multilingual)", "Speech to Text: fastest, least accurate",
                        "ggml-tiny.bin", 77691713, "be07e048e1e599ad46341c8d2a135645097a538221678b7acdd1b1919c6e1b21", true));
    c.push_back(whisper("whisper-small", "Whisper small (multilingual)",
                        "Speech to Text: the most accurate model offered; slower, 488 MB", "ggml-small.bin", 487601967,
                        "1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b", true));
    c.push_back({"rife-v4.6", "RIFE 4.6 (ncnn)", "AI Interpolation: neural in-between frames at any time step",
                 "RIFE v4.6, ncnn models of rife-ncnn-vulkan @ a7532fc", ModelRuntime::Ncnn,
                 "MIT (rife-ncnn-vulkan; ECCV2022-RIFE / Practical-RIFE)",
                 "MIT (ncnn conversion in the MIT-licensed rife-ncnn-vulkan repository; not stated separately)",
                 "github.com/nihui/rife-ncnn-vulkan @ a7532fc, models/rife-v4.6 (from hzwer/Practical-RIFE)",
                 "Zhewei Huang, Tianyuan Zhang, Wen Heng, Boxin Shi, Shuchang Zhou: Real-Time Intermediate Flow "
                 "Estimation for Video Frame Interpolation (ECCV 2022); ncnn port by nihui",
                 kUpstreamOnly, false,
                 {{"flownet.param", kRifeRev + "flownet.param", 16532,
                   "724569596bcd1e7b9fa50455c604777ebed99746d2ef40aa86e31b5725f1053c"},
                  {"flownet.bin", kRifeRev + "flownet.bin", 10614320,
                   "f334ed2260149ce0188a6dcf049844e8b0cdd912e01cbcfb63553157d2508958"}}});
    c.push_back({"realesrgan", "Real-ESRGAN (ncnn)",
                 "Video Upscaling: x4plus (photos, live action) and the small animevideov3 model (x2 / x3 / x4)",
                 "v0.2.5.0, ncnn models of realesrgan-ncnn-vulkan-20220424", ModelRuntime::Ncnn,
                 "BSD-3-Clause (Real-ESRGAN)", "Not stated separately; released with the BSD-3-Clause repository",
                 "github.com/xinntao/Real-ESRGAN, release v0.2.5.0 (the models of realesrgan-ncnn-vulkan-20220424-"
                 "windows.zip; its program files are not used)",
                 "Xintao Wang, Liangbin Xie, Chao Dong, Ying Shan: Real-ESRGAN: Training Real-World Blind "
                 "Super-Resolution with Pure Synthetic Data (ICCV Workshops 2021)",
                 kUpstreamOnly, false,
                 {esr("realesrgan-x4plus.param", 116029, "35330ececcea33b6c397a72548e788d5d53becee4734c50b7fada36e89f10a86"),
                  esr("realesrgan-x4plus.bin", 33424520, "713ee713b0353afaa27976f0563a64a5043bd70b9bd8936c2e26e25ebcdbcddf"),
                  esr("realesr-animevideov3-x2.param", 3173, "b88ff4f00ebf019a7fdac17fdd45a7fd3665d37509efc5baf2e4da2e24420a04"),
                  esr("realesr-animevideov3-x2.bin", 1247368, "548a36f9c3f4ab8da56cd3b13badf23968bee207b396dad14d04b830e5f2ab2d"),
                  esr("realesr-animevideov3-x3.param", 3173, "d1a5755008791d09b57e3425fc9dd0bd26b00fdf79c606210bc0e693f8230881"),
                  esr("realesr-animevideov3-x3.bin", 1247368, "548a36f9c3f4ab8da56cd3b13badf23968bee207b396dad14d04b830e5f2ab2d"),
                  esr("realesr-animevideov3-x4.param", 3077, "850a248e7c14c27e5bd8cf7265113a9441036a7db63963bb8aa5169d788a435e"),
                  esr("realesr-animevideov3-x4.bin", 1247368, "548a36f9c3f4ab8da56cd3b13badf23968bee207b396dad14d04b830e5f2ab2d")}});
    return c;
}

std::mutex g_dirMutex;
std::filesystem::path g_dirOverride;
std::atomic<uint64_t> g_networkRequests{0};

std::string u8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::filesystem::path fromU8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string hex(const unsigned char* d, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = k[d[i] >> 4];
        s[2 * i + 1] = k[d[i] & 15];
    }
    return s;
}

class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
            BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) != 0)
            throw ModelError("SHA-256 is not available (Windows CNG)");
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    void add(const void* d, size_t n) {
        const auto* p = static_cast<const unsigned char*>(d);
        while (n > 0) {
            const ULONG chunk = static_cast<ULONG>(std::min<size_t>(n, 1u << 30));
            BCryptHashData(hash_, const_cast<PUCHAR>(p), chunk, 0);
            p += chunk;
            n -= chunk;
        }
    }
    std::string finish() {
        unsigned char out[32];
        BCryptFinishHash(hash_, out, sizeof out, 0);
        return hex(out, sizeof out);
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

struct WinHttp {
    HINTERNET h = nullptr;
    explicit WinHttp(HINTERNET x) : h(x) {}
    ~WinHttp() {
        if (h) WinHttpCloseHandle(h);
    }
    WinHttp(const WinHttp&) = delete;
    WinHttp& operator=(const WinHttp&) = delete;
};

std::string lastError(const std::string& what) {
    return what + " (Windows error " + std::to_string(GetLastError()) + ")";
}

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }

}  // namespace

uint64_t ModelInfo::downloadBytes() const {
    uint64_t n = 0;
    std::set<std::string> archives;
    for (const auto& f : files) {
        if (f.archiveMember.empty()) n += f.bytes;
        else if (archives.insert(f.url).second) n += f.archiveBytes;
    }
    return n;
}

uint64_t ModelInfo::installedBytes() const {
    uint64_t n = 0;
    for (const auto& f : files) n += f.bytes;
    return n;
}

const std::vector<ModelInfo>& modelCatalog() {
    static const std::vector<ModelInfo> c = makeCatalog();
    return c;
}

const ModelInfo* findModel(const std::string& id) {
    for (const auto& m : modelCatalog())
        if (m.id == id) return &m;
    return nullptr;
}

const char* runtimeName(ModelRuntime r) {
    switch (r) {
    case ModelRuntime::Onnx: return "ONNX Runtime";
    case ModelRuntime::Ncnn: return "ncnn";
    case ModelRuntime::Whisper: return "whisper.cpp";
    }
    return "?";
}

const char* statusName(ModelStatus s) {
    switch (s) {
    case ModelStatus::Missing: return "Not installed";
    case ModelStatus::Partial: return "Incomplete";
    case ModelStatus::Installed: return "Installed";
    case ModelStatus::Corrupt: return "Damaged";
    }
    return "?";
}

std::filesystem::path modelsDirectory() {
    {
        std::lock_guard<std::mutex> lock(g_dirMutex);
        if (!g_dirOverride.empty()) return g_dirOverride;
    }
    if (const wchar_t* e = _wgetenv(L"MEERCUT_MODELS"); e && *e) return std::filesystem::path(e);
    if (const auto d = appDataFolder("models"); !d.empty()) return d;   // %LOCALAPPDATA%\MeerCut\models
    return std::filesystem::temp_directory_path() / "MeerCut" / "models";
}

void setModelsDirectoryOverride(const std::filesystem::path& dir) {
    std::lock_guard<std::mutex> lock(g_dirMutex);
    g_dirOverride = dir;
}

std::filesystem::path modelFolder(const ModelInfo& m, const std::filesystem::path& root) { return root / m.id; }

std::filesystem::path modelFilePath(const ModelInfo& m, size_t file, const std::filesystem::path& root) {
    return modelFolder(m, root) / std::filesystem::path(m.files.at(file).path);
}

ModelStatus modelStatus(const ModelInfo& m, bool verifyHashes, const std::filesystem::path& root) {
    size_t present = 0;
    bool bad = false;
    for (size_t i = 0; i < m.files.size(); ++i) {
        const auto p = modelFilePath(m, i, root);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(p, ec)) continue;
        ++present;
        if (std::filesystem::file_size(p, ec) != m.files[i].bytes) bad = true;
        else if (verifyHashes && sha256File(p) != m.files[i].sha256) bad = true;
    }
    if (bad) return ModelStatus::Corrupt;
    if (present == 0) return ModelStatus::Missing;
    return present == m.files.size() ? ModelStatus::Installed : ModelStatus::Partial;
}

namespace {

// M7 F: a file of the right size can still be damaged (weights that load and compute garbage). Each file's SHA-256 is
// checked on its first use and the result kept in `.verified` in the model's folder (name, size, modification time):
// later runs hash again only a file that changed. Not a security boundary - it catches damage, not tampering.
std::mutex g_verifyMutex;

std::map<std::string, std::string> readVerified(const std::filesystem::path& folder) {
    std::map<std::string, std::string> out;   // file name -> "size mtime sha"
    std::ifstream in(folder / ".verified");
    std::string line;
    while (std::getline(in, line))
        if (const auto tab = line.find('\t'); tab != std::string::npos) out[line.substr(0, tab)] = line.substr(tab + 1);
    return out;
}

}  // namespace

void requireModel(const ModelInfo& m, const std::filesystem::path& root) {
    for (size_t i = 0; i < m.files.size(); ++i) {
        const auto p = modelFilePath(m, i, root);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(p, ec))
            throw ModelError("The model \"" + m.name + "\" is not installed (AI Tools > Model Manager).");
        if (std::filesystem::file_size(p, ec) != m.files[i].bytes)
            throw ModelError("The model file " + u8(p) + " is damaged (wrong size); reinstall \"" + m.name +
                             "\" in the Model Manager.");
    }
    std::lock_guard lock(g_verifyMutex);
    const auto folder = modelFolder(m, root);
    auto verified = readVerified(folder);
    bool changed = false;
    for (size_t i = 0; i < m.files.size(); ++i) {
        const auto p = modelFilePath(m, i, root);
        std::error_code ec;
        const std::string stamp = std::to_string(m.files[i].bytes) + " " +
                                  std::to_string(std::filesystem::last_write_time(p, ec).time_since_epoch().count()) + " " +
                                  m.files[i].sha256;
        const std::string name = u8(p.filename());
        if (verified[name] == stamp) continue;
        if (sha256File(p) != m.files[i].sha256)
            throw ModelError("The model file " + u8(p) + " is damaged (its checksum does not match the release); reinstall \"" +
                             m.name + "\" in the Model Manager.");
        verified[name] = stamp;
        changed = true;
    }
    if (changed) {   // best effort: a read-only folder only means hashing again next time
        std::ofstream out(folder / ".verified", std::ios::trunc);
        for (const auto& [name, stamp] : verified)
            if (!stamp.empty()) out << name << '\t' << stamp << '\n';
    }
}

std::string sha256File(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw ModelError("cannot read " + u8(file));
    Sha256 h;
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (in.gcount() > 0) h.add(buf.data(), static_cast<size_t>(in.gcount()));
    }
    return h.finish();
}

std::string sha256Bytes(const void* data, size_t size) {
    Sha256 h;
    h.add(data, size);
    return h.finish();
}

uint64_t networkRequests() { return g_networkRequests.load(); }

void downloadFile(const std::string& url, const std::filesystem::path& target, uint64_t expectedBytes,
                  const std::function<void(uint64_t)>& onBytes, const std::atomic<bool>* cancel) {
    std::filesystem::create_directories(target.parent_path());
    auto fail = [&](const std::string& why) {
        std::error_code ec;
        std::filesystem::remove(target, ec);
        throw ModelError(why);
    };
    if (url.rfind("file:///", 0) == 0) {   // tests: a local file stands for the server
        const std::filesystem::path src = fromU8(url.substr(8));
        std::ifstream in(src, std::ios::binary);
        std::ofstream out(target, std::ios::binary);
        if (!in || !out) fail("cannot copy " + u8(src));
        std::vector<char> buf(1 << 16);
        uint64_t done = 0;
        while (in) {
            if (cancel && cancel->load()) {
                out.close();
                fail("download cancelled");
            }
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            out.write(buf.data(), in.gcount());
            done += static_cast<uint64_t>(in.gcount());
            if (onBytes) onBytes(done);
        }
        out.close();
        if (expectedBytes && done != expectedBytes)
            fail(url + ": " + std::to_string(done) + " bytes, expected " + std::to_string(expectedBytes));
        return;
    }
    if (url.rfind("https://", 0) != 0) fail("only https:// downloads are allowed: " + url);
    const std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4095;
    wchar_t extra[4096] = {};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 4095;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) fail(lastError("invalid URL"));
    ++g_networkRequests;
    WinHttp session(WinHttpOpen(L"MeerCut-ModelManager/0.7", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) fail(lastError("cannot start WinHTTP"));
    WinHttpSetTimeouts(session.h, 30000, 30000, 60000, 120000);
    WinHttp connect(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!connect.h) fail(lastError("cannot connect to the model server"));
    const std::wstring object = std::wstring(path) + extra;
    WinHttp request(WinHttpOpenRequest(connect.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.h) fail(lastError("cannot create the request"));
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        fail(lastError("the download of " + url + " failed"));
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) fail(url + ": HTTP status " + std::to_string(status));
    std::ofstream out(target, std::ios::binary);
    if (!out) fail("cannot write " + u8(target));
    std::vector<char> buf(1 << 16);
    uint64_t done = 0;
    for (;;) {
        if (cancel && cancel->load()) {
            out.close();
            fail("download cancelled");
        }
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
            out.close();
            fail(lastError("the download of " + url + " was interrupted"));
        }
        if (got == 0) break;
        out.write(buf.data(), got);
        done += got;
        if (onBytes) onBytes(done);
    }
    out.close();
    if (!out) fail("cannot write " + u8(target));
    if (expectedBytes && done != expectedBytes)
        fail(url + ": received " + std::to_string(done) + " bytes, expected " + std::to_string(expectedBytes));
}

std::vector<uint8_t> readZipMember(const std::filesystem::path& zip, const std::string& member) {
    std::ifstream in(zip, std::ios::binary | std::ios::ate);
    if (!in) throw ModelError("cannot read " + u8(zip));
    const auto fileSize = static_cast<uint64_t>(in.tellg());
    const uint64_t tail = std::min<uint64_t>(fileSize, 65557);
    std::vector<uint8_t> end(static_cast<size_t>(tail));
    in.seekg(static_cast<std::streamoff>(fileSize - tail));
    in.read(reinterpret_cast<char*>(end.data()), static_cast<std::streamsize>(tail));
    int64_t eocd = -1;
    for (int64_t i = static_cast<int64_t>(tail) - 22; i >= 0; --i)
        if (rd32(&end[static_cast<size_t>(i)]) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd < 0) throw ModelError(u8(zip) + " is not a zip archive");
    const uint32_t cdSize = rd32(&end[static_cast<size_t>(eocd) + 12]), cdOffset = rd32(&end[static_cast<size_t>(eocd) + 16]);
    std::vector<uint8_t> cd(cdSize);
    in.seekg(cdOffset);
    in.read(reinterpret_cast<char*>(cd.data()), cdSize);
    for (size_t p = 0; p + 46 <= cd.size();) {
        if (rd32(&cd[p]) != 0x02014b50) break;
        const uint16_t method = rd16(&cd[p + 10]);
        const uint32_t crc = rd32(&cd[p + 16]), csize = rd32(&cd[p + 20]), usize = rd32(&cd[p + 24]);
        const uint16_t nlen = rd16(&cd[p + 28]), xlen = rd16(&cd[p + 30]), clen = rd16(&cd[p + 32]);
        const uint32_t local = rd32(&cd[p + 42]);
        const std::string name(reinterpret_cast<const char*>(&cd[p + 46]), nlen);
        p += 46u + nlen + xlen + clen;
        if (name != member) continue;
        uint8_t lh[30];
        in.seekg(local);
        in.read(reinterpret_cast<char*>(lh), 30);
        if (rd32(lh) != 0x04034b50) throw ModelError("damaged zip entry " + member);
        in.seekg(static_cast<std::streamoff>(local) + 30 + rd16(&lh[26]) + rd16(&lh[28]));
        std::vector<uint8_t> comp(csize), data(usize);
        in.read(reinterpret_cast<char*>(comp.data()), csize);
        if (method == 0) {
            data = comp;
        } else if (method == 8) {
            z_stream zs{};
            if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) throw ModelError("zlib error");
            zs.next_in = comp.data();
            zs.avail_in = csize;
            zs.next_out = data.data();
            zs.avail_out = usize;
            const int r = inflate(&zs, Z_FINISH);
            inflateEnd(&zs);
            if (r != Z_STREAM_END) throw ModelError("cannot unpack " + member);
        } else {
            throw ModelError("unsupported zip compression in " + member);
        }
        if (crc32(0L, data.data(), static_cast<uInt>(data.size())) != crc) throw ModelError("CRC mismatch in " + member);
        return data;
    }
    throw ModelError(member + " is not in " + u8(zip));
}

void installModel(const ModelInfo& m, const InstallProgress& p, const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    const auto staging = root / (m.id + ".download");
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging);
    const uint64_t total = m.downloadBytes();
    uint64_t base = 0;
    auto report = [&](uint64_t n) {
        if (p.progress) p.progress(std::min(total, base + n), total);
    };
    try {
        std::map<std::string, std::filesystem::path> archives;   // url -> downloaded archive
        for (size_t i = 0; i < m.files.size(); ++i) {
            const ModelFile& f = m.files[i];
            const auto dest = staging / std::filesystem::path(f.path);
            std::filesystem::create_directories(dest.parent_path());
            if (f.archiveMember.empty()) {
                downloadFile(f.url, dest, f.bytes, report, p.cancel);
                base += f.bytes;
            } else {
                auto it = archives.find(f.url);
                if (it == archives.end()) {
                    const auto zip = staging / ("archive" + std::to_string(archives.size()) + ".zip");
                    downloadFile(f.url, zip, f.archiveBytes, report, p.cancel);
                    base += f.archiveBytes;
                    if (sha256File(zip) != f.archiveSha256)
                        throw ModelError("the archive " + f.url + " does not have the expected SHA-256");
                    it = archives.emplace(f.url, zip).first;
                }
                const auto bytes = readZipMember(it->second, f.archiveMember);
                std::ofstream out(dest, std::ios::binary);
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!out) throw ModelError("cannot write " + u8(dest));
            }
            if (std::filesystem::file_size(dest) != f.bytes || sha256File(dest) != f.sha256)
                throw ModelError("\"" + m.name + "\": " + f.path + " does not match the SHA-256 recorded for it - the "
                                 "download was not installed");
        }
        for (const auto& [url, zip] : archives) std::filesystem::remove(zip);
        const auto folder = modelFolder(m, root);
        std::filesystem::remove_all(folder, ec);
        std::filesystem::rename(staging, folder);
    } catch (...) {
        std::filesystem::remove_all(staging, ec);
        throw;
    }
}

void installModelFromFolder(const ModelInfo& m, const std::filesystem::path& folder, const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    const auto staging = root / (m.id + ".download");
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging);
    try {
        for (const ModelFile& f : m.files) {
            const auto dest = staging / std::filesystem::path(f.path);
            std::filesystem::create_directories(dest.parent_path());
            const auto direct = folder / std::filesystem::path(f.path).filename();
            if (std::filesystem::is_regular_file(direct, ec) && std::filesystem::file_size(direct, ec) == f.bytes) {
                std::filesystem::copy_file(direct, dest, std::filesystem::copy_options::overwrite_existing);
            } else if (!f.archiveMember.empty()) {
                const auto archiveName = fromU8(f.url.substr(f.url.find_last_of('/') + 1));
                const auto archive = folder / archiveName;
                if (!std::filesystem::is_regular_file(archive, ec))
                    throw ModelError("\"" + m.name + "\": neither " + u8(direct.filename()) + " nor " +
                                     u8(archiveName) + " is in " + u8(folder));
                if (sha256File(archive) != f.archiveSha256)
                    throw ModelError(u8(archive) + " is not the archive recorded for \"" + m.name + "\"");
                const auto bytes = readZipMember(archive, f.archiveMember);
                std::ofstream out(dest, std::ios::binary);
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            } else {
                throw ModelError("\"" + m.name + "\": " + u8(direct.filename()) + " (" + std::to_string(f.bytes) +
                                 " bytes) is not in " + u8(folder));
            }
            if (std::filesystem::file_size(dest) != f.bytes || sha256File(dest) != f.sha256)
                throw ModelError("\"" + m.name + "\": " + f.path + " does not match the SHA-256 recorded for it - "
                                 "nothing was installed");
        }
        const auto target = modelFolder(m, root);
        std::filesystem::remove_all(target, ec);
        std::filesystem::rename(staging, target);
    } catch (...) {
        std::filesystem::remove_all(staging, ec);
        throw;
    }
}

void removeModel(const ModelInfo& m, const std::filesystem::path& root) {
    std::error_code ec;
    std::filesystem::remove_all(modelFolder(m, root), ec);
}

}  // namespace fa::ai
