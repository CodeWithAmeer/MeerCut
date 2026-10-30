// M7 checkpoint A: the local inference infrastructure - model catalogue, integrity-checked installer (no partial or
// unverified model is ever installed), zip extraction, device probing, and real inference with ONNX Runtime (CPU and
// DirectML) and ncnn (CPU and Vulkan) on the installed models (FA_MODELS_DIR; a test reports "skipped" if a model is
// not installed there, never passes without running it).
#include "fa/ai/inference.h"
#include "fa/ai/models.h"
#include "fa/media/media.h"

#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <regex>
#include <set>

using namespace fa;
using namespace fa::ai;

namespace {

std::filesystem::path models() { return std::filesystem::path(FA_MODELS_DIR); }
std::filesystem::path aiMedia(const char* name) { return std::filesystem::path(FA_TESTMEDIA_DIR) / "ai" / name; }

std::filesystem::path tempDir(const char* name) {
    const auto d = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

std::string fileUrl(const std::filesystem::path& p) {
    const std::u8string s = p.generic_u8string();
    return "file:///" + std::string(s.begin(), s.end());
}

void writeBytes(const std::filesystem::path& p, const std::vector<uint8_t>& b) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

std::vector<uint8_t> randomBytes(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> b(n);
    for (auto& x : b) x = static_cast<uint8_t>(rng() & 0xFF);
    return b;
}

// A minimal zip writer (one stored and one deflated member) to test the reader without any external tool.
std::vector<uint8_t> makeZip(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& members) {
    std::vector<uint8_t> out, cd;
    auto put16 = [](std::vector<uint8_t>& v, uint32_t x) { v.push_back(x & 255); v.push_back((x >> 8) & 255); };
    auto put32 = [&](std::vector<uint8_t>& v, uint32_t x) { put16(v, x & 0xFFFF); put16(v, x >> 16); };
    int index = 0;
    for (const auto& [name, data] : members) {
        const bool deflated = (index++ % 2) == 1;
        std::vector<uint8_t> comp = data;
        if (deflated) {
            z_stream zs{};
            deflateInit2(&zs, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            comp.resize(deflateBound(&zs, static_cast<uLong>(data.size())));
            zs.next_in = const_cast<Bytef*>(data.data());
            zs.avail_in = static_cast<uInt>(data.size());
            zs.next_out = comp.data();
            zs.avail_out = static_cast<uInt>(comp.size());
            deflate(&zs, Z_FINISH);
            comp.resize(zs.total_out);
            deflateEnd(&zs);
        }
        const uint32_t crc = crc32(0L, data.data(), static_cast<uInt>(data.size()));
        const uint32_t offset = static_cast<uint32_t>(out.size());
        put32(out, 0x04034b50); put16(out, 20); put16(out, 0); put16(out, deflated ? 8 : 0); put32(out, 0);
        put32(out, crc); put32(out, static_cast<uint32_t>(comp.size())); put32(out, static_cast<uint32_t>(data.size()));
        put16(out, static_cast<uint32_t>(name.size())); put16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), comp.begin(), comp.end());
        put32(cd, 0x02014b50); put16(cd, 20); put16(cd, 20); put16(cd, 0); put16(cd, deflated ? 8 : 0); put32(cd, 0);
        put32(cd, crc); put32(cd, static_cast<uint32_t>(comp.size())); put32(cd, static_cast<uint32_t>(data.size()));
        put16(cd, static_cast<uint32_t>(name.size())); put16(cd, 0); put16(cd, 0); put16(cd, 0); put16(cd, 0); put32(cd, 0);
        put32(cd, offset);
        cd.insert(cd.end(), name.begin(), name.end());
    }
    const uint32_t cdOffset = static_cast<uint32_t>(out.size());
    out.insert(out.end(), cd.begin(), cd.end());
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, static_cast<uint32_t>(members.size()));
    put16(out, static_cast<uint32_t>(members.size())); put32(out, static_cast<uint32_t>(cd.size())); put32(out, cdOffset);
    put16(out, 0);
    return out;
}

// RGB planes 0..1, NCHW (batch 1) or CHW.
Tensor toTensor(const Image& img, bool batch) {
    Tensor t;
    t.shape = batch ? std::vector<int64_t>{1, 3, img.height, img.width} : std::vector<int64_t>{3, img.height, img.width};
    const size_t n = static_cast<size_t>(img.width) * img.height;
    t.data.resize(3 * n);
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) t.data[c * n + i] = img.pixels[i * 4 + c] / 255.0f;
    return t;
}

double regionMean(const Tensor& alpha, int w, int h, double x0, double y0, double x1, double y1) {
    double s = 0;
    int n = 0;
    for (int y = int(y0 * h); y < int(y1 * h); ++y)
        for (int x = int(x0 * w); x < int(x1 * w); ++x, ++n) s += alpha.data[static_cast<size_t>(y) * w + x];
    return n ? s / n : 0;
}

Tensor runRvm(OnnxModel& m, const Image& img, float ratio) {
    Tensor src = toTensor(img, true), r{{1, 1, 1, 1}, {0.f}}, ds{{1}, {ratio}};
    const auto out = m.run({{"src", &src}, {"r1i", &r}, {"r2i", &r}, {"r3i", &r}, {"r4i", &r}, {"downsample_ratio", &ds}},
                           {"pha"});
    return out[0];
}

}  // namespace

TEST(AiModels, CatalogIsConsistent) {
    const std::regex hex64("^[0-9a-f]{64}$");
    std::set<std::string> ids;
    for (const ModelInfo& m : modelCatalog()) {
        EXPECT_TRUE(ids.insert(m.id).second) << m.id;
        EXPECT_FALSE(m.name.empty() || m.purpose.empty() || m.version.empty() || m.source.empty()) << m.id;
        EXPECT_FALSE(m.codeLicence.empty() || m.weightsLicence.empty() || m.attribution.empty() || m.redistribution.empty())
            << m.id << ": code and weight licences are recorded separately";
        ASSERT_FALSE(m.files.empty()) << m.id;
        for (const ModelFile& f : m.files) {
            EXPECT_EQ(f.url.rfind("https://", 0), 0u) << f.url;
            EXPECT_GT(f.bytes, 0u) << f.path;
            EXPECT_TRUE(std::regex_match(f.sha256, hex64)) << f.path;
            if (!f.archiveMember.empty()) {
                EXPECT_TRUE(std::regex_match(f.archiveSha256, hex64)) << f.path;
                EXPECT_GT(f.archiveBytes, f.bytes) << f.path;
            }
            EXPECT_EQ(f.url.find("/main/"), std::string::npos) << f.url << ": URLs are pinned to a release or commit";
        }
        EXPECT_EQ(findModel(m.id), &m);
    }
    // the Real-ESRGAN models are four pairs from one 45 MB archive, downloaded once
    const ModelInfo* esr = findModel("realesrgan");
    ASSERT_TRUE(esr);
    EXPECT_EQ(esr->downloadBytes(), 45474481u);
    EXPECT_EQ(findModel("nonexistent"), nullptr);
}

TEST(AiModels, Sha256KnownVectors) {
    EXPECT_EQ(sha256Bytes("", 0), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256Bytes("abc", 3), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const auto dir = tempDir("fa_ai_sha");
    writeBytes(dir / "a.bin", {'a', 'b', 'c'});
    EXPECT_EQ(sha256File(dir / "a.bin"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::filesystem::remove_all(dir);
}

TEST(AiModels, InstallVerifiesEveryFileAndLeavesNothingBehindOnFailure) {
    const auto src = tempDir("fa_ai_src"), root = tempDir("fa_ai_models");
    const auto bytes = randomBytes(300000, 7), bytes2 = randomBytes(1000, 8);
    writeBytes(src / "weights.bin", bytes);
    writeBytes(src / "config.txt", bytes2);
    ModelInfo m{"test-model", "Test model", "tests", "1", ModelRuntime::Onnx, "MIT", "MIT", "test", "test", "test", false,
                {{"weights.bin", fileUrl(src / "weights.bin"), bytes.size(), sha256Bytes(bytes.data(), bytes.size())},
                 {"sub/config.txt", fileUrl(src / "config.txt"), bytes2.size(), sha256Bytes(bytes2.data(), bytes2.size())}}};
    EXPECT_EQ(modelStatus(m, true, root), ModelStatus::Missing);
    EXPECT_THROW(requireModel(m, root), ModelError);
    uint64_t last = 0, total = 0;
    installModel(m, {[&](uint64_t d, uint64_t t) { last = d; total = t; }, nullptr}, root);
    EXPECT_EQ(total, bytes.size() + bytes2.size());
    EXPECT_EQ(last, total);
    EXPECT_EQ(modelStatus(m, true, root), ModelStatus::Installed);
    EXPECT_NO_THROW(requireModel(m, root));
    EXPECT_FALSE(std::filesystem::exists(root / "test-model.download"));
    // one flipped byte: the size check alone cannot see it, the hash check does (why the Model Manager verifies)
    {
        std::fstream f(modelFilePath(m, 0, root), std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(1234);
        const char x = static_cast<char>(bytes[1234] ^ 0x5A);
        f.write(&x, 1);
    }
    EXPECT_EQ(modelStatus(m, false, root), ModelStatus::Installed);
    EXPECT_EQ(modelStatus(m, true, root), ModelStatus::Corrupt);
    // a missing file: incomplete (by size); with hashes the damaged first file makes it damaged
    std::filesystem::remove(modelFilePath(m, 1, root));
    EXPECT_EQ(modelStatus(m, false, root), ModelStatus::Partial);
    EXPECT_EQ(modelStatus(m, true, root), ModelStatus::Corrupt);
    removeModel(m, root);
    EXPECT_EQ(modelStatus(m, false, root), ModelStatus::Missing);
    // negative controls: a wrong hash, a wrong size, a cancelled download - none leaves a model or a staging folder
    ModelInfo wrongHash = m;
    wrongHash.files[1].sha256[0] = wrongHash.files[1].sha256[0] == 'a' ? 'b' : 'a';
    EXPECT_THROW(installModel(wrongHash, {}, root), ModelError);
    EXPECT_EQ(modelStatus(m, false, root), ModelStatus::Missing);
    ModelInfo wrongSize = m;
    wrongSize.files[0].bytes += 1;
    EXPECT_THROW(installModel(wrongSize, {}, root), ModelError);
    std::atomic<bool> cancel{true};
    EXPECT_THROW(installModel(m, {nullptr, &cancel}, root), ModelError);
    EXPECT_EQ(modelStatus(m, false, root), ModelStatus::Missing);
    EXPECT_FALSE(std::filesystem::exists(root / "test-model.download"));
    // only https:// (or file:// for tests) is ever fetched
    ModelInfo http = m;
    http.files[0].url = "http://example.invalid/weights.bin";
    EXPECT_THROW(installModel(http, {}, root), ModelError);
    std::filesystem::remove_all(src);
    std::filesystem::remove_all(root);
}

TEST(AiModels, ArchiveMembersAreExtractedAndChecked) {
    const auto dir = tempDir("fa_ai_zip"), root = tempDir("fa_ai_zip_models");
    const auto a = randomBytes(5000, 1);
    std::vector<uint8_t> b(70000, 'x');   // compressible: stored deflated
    writeBytes(dir / "pack.zip", makeZip({{"models/a.param", a}, {"models/b.bin", b}}));
    EXPECT_EQ(readZipMember(dir / "pack.zip", "models/a.param"), a);
    EXPECT_EQ(readZipMember(dir / "pack.zip", "models/b.bin"), b);
    EXPECT_THROW(readZipMember(dir / "pack.zip", "models/missing.bin"), ModelError);
    const auto zipSha = sha256File(dir / "pack.zip");
    const uint64_t zipBytes = std::filesystem::file_size(dir / "pack.zip");
    ModelInfo m{"zip-model", "Zip model", "tests", "1", ModelRuntime::Ncnn, "MIT", "MIT", "test", "test", "test", false,
                {{"a.param", fileUrl(dir / "pack.zip"), a.size(), sha256Bytes(a.data(), a.size()), "models/a.param", zipBytes, zipSha},
                 {"b.bin", fileUrl(dir / "pack.zip"), b.size(), sha256Bytes(b.data(), b.size()), "models/b.bin", zipBytes, zipSha}}};
    EXPECT_EQ(m.downloadBytes(), zipBytes);   // one archive, downloaded once
    installModel(m, {}, root);
    EXPECT_EQ(modelStatus(m, true, root), ModelStatus::Installed);
    EXPECT_FALSE(std::filesystem::exists(modelFolder(m, root) / "archive0.zip"));   // the archive is not kept
    ModelInfo badArchive = m;
    removeModel(m, root);
    for (auto& f : badArchive.files) f.archiveSha256[3] = f.archiveSha256[3] == '0' ? '1' : '0';
    EXPECT_THROW(installModel(badArchive, {}, root), ModelError);
    EXPECT_EQ(modelStatus(m, false, root), ModelStatus::Missing);
    // the real Real-ESRGAN release archive, when it is on disk: its members have the recorded hashes
    const auto real = models() / "_downloads" / "realesrgan-ncnn-vulkan-20220424-windows.zip";
    if (std::filesystem::exists(real)) {
        const ModelInfo* esr = findModel("realesrgan");
        for (const ModelFile& f : esr->files) {
            const auto data = readZipMember(real, f.archiveMember);
            EXPECT_EQ(data.size(), f.bytes) << f.path;
            EXPECT_EQ(sha256Bytes(data.data(), data.size()), f.sha256) << f.path;
        }
    }
    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(root);
}

TEST(AiModels, InstalledModelsMatchTheCatalogHashes) {
    if (!std::filesystem::exists(models())) GTEST_SKIP() << "FA_MODELS_DIR " << models() << " does not exist";
    for (const ModelInfo& m : modelCatalog())
        EXPECT_EQ(modelStatus(m, true, models()), ModelStatus::Installed) << m.id << " in " << models();
}

TEST(AiDevices, ProbeIsConsistent) {
    const AiCapabilities& c = aiCapabilities();
    EXPECT_GT(c.cpuThreads, 0);
    EXPECT_FALSE(c.cpuName.empty());
    EXPECT_EQ(c.onnxRuntimeVersion, "1.24.4");
    EXPECT_NE(std::find(c.onnxProviders.begin(), c.onnxProviders.end(), "CPUExecutionProvider"), c.onnxProviders.end());
    for (const AiGpu& g : c.gpus) {
        EXPECT_FALSE(g.name.empty());
        EXPECT_TRUE(g.dxgiIndex >= 0 || g.vulkanIndex >= 0) << g.name;
    }
    if (c.directMl) EXPECT_GE(preferredDirectMlGpu(), 0);
    if (c.vulkan) EXPECT_GE(preferredVulkanGpu(), 0);
    const std::string text = describeCapabilities(c);
    EXPECT_NE(text.find("ONNX Runtime 1.24.4"), std::string::npos);
    std::printf("%s", text.c_str());
    Accelerator a;
    EXPECT_TRUE(acceleratorFromId("gpu", a));
    EXPECT_EQ(a, Accelerator::Gpu);
    EXPECT_FALSE(acceleratorFromId("tpu", a));
}

// Real inference: Robust Video Matting on a Tears of Steel frame with two people, on the CPU and through DirectML.
TEST(AiOnnx, RvmFindsPeopleOnCpuAndDirectMlAndNothingInAPatternWithoutPeople) {
    const ModelInfo* m = findModel("rvm-mobilenetv3");
    if (modelStatus(*m, false, models()) != ModelStatus::Installed) GTEST_SKIP() << "RVM not installed in " << models();
    if (!std::filesystem::exists(aiMedia("tears_of_steel_60s.mov"))) GTEST_SKIP() << "testmedia/ai not fetched";
    Image frame;
    {
        VideoDecoder dec(aiMedia("tears_of_steel_60s.mov"));
        frame = dec.frameAt(Time(10)).image;
    }
    ASSERT_EQ(frame.width, 1280);
    OnnxModel cpu(modelFilePath(*m, 0, models()), Accelerator::Cpu);
    EXPECT_FALSE(cpu.onGpu());
    const Tensor a = runRvm(cpu, frame, 0.375f);
    ASSERT_EQ(a.shape, (std::vector<int64_t>{1, 1, frame.height, frame.width}));
    const int w = frame.width, h = frame.height;
    const double left = regionMean(a, w, h, 0.30, 0.38, 0.36, 0.62), right = regionMean(a, w, h, 0.60, 0.38, 0.68, 0.62);
    const double sky = regionMean(a, w, h, 0.02, 0.05, 0.20, 0.30), buildings = regionMean(a, w, h, 0.82, 0.05, 0.98, 0.30);
    std::printf("RVM CPU: person L %.3f R %.3f, background %.3f %.3f\n", left, right, sky, buildings);
    EXPECT_GT(left, 0.8);
    EXPECT_GT(right, 0.8);
    EXPECT_LT(sky, 0.05);
    EXPECT_LT(buildings, 0.05);
    // negative control: the testsrc2 pattern (no person) - the model must not invent a foreground
    Image pattern;
    {
        VideoDecoder dec(std::filesystem::path(FA_TESTMEDIA_DIR) / "C_testsrc2_1080p30_10s.mp4");
        pattern = dec.frameAt(Time(3)).image;
    }
    OnnxModel fresh(modelFilePath(*m, 0, models()), Accelerator::Cpu);   // a fresh session (no recurrent history)
    const Tensor nb = runRvm(fresh, pattern, 0.25f);
    const double patternMean = regionMean(nb, pattern.width, pattern.height, 0, 0, 1, 1);
    std::printf("RVM CPU on the test pattern: mean alpha %.4f\n", patternMean);
    EXPECT_LT(patternMean, 0.05);
    // DirectML (Windows' DirectML.dll) gives the same matte
    if (!aiCapabilities().directMl) GTEST_SKIP() << "no DirectML on this PC";
    OnnxModel gpu(modelFilePath(*m, 0, models()), Accelerator::Gpu);
    ASSERT_TRUE(gpu.onGpu()) << gpu.fallbackReason();
    EXPECT_NE(gpu.provider().find("DirectML"), std::string::npos);
    const Tensor b = runRvm(gpu, frame, 0.375f);
    ASSERT_EQ(b.shape, a.shape);
    double maxDiff = 0, sumDiff = 0;
    for (size_t i = 0; i < a.data.size(); ++i) {
        const double d = std::fabs(a.data[i] - b.data[i]);
        maxDiff = std::max(maxDiff, d);
        sumDiff += d;
    }
    std::printf("RVM DirectML (%s) vs CPU: mean |d| %.5f, max %.4f\n", gpu.provider().c_str(), sumDiff / a.data.size(), maxDiff);
    EXPECT_LT(sumDiff / a.data.size(), 0.005);
    EXPECT_LT(maxDiff, 0.25);
}

// Real inference with ncnn: Real-ESRGAN animevideov3 x2 on the CPU and on Vulkan - same output size and content.
TEST(AiNcnn, RealEsrganRunsOnCpuAndVulkanWithTheSameResult) {
    const ModelInfo* m = findModel("realesrgan");
    if (modelStatus(*m, false, models()) != ModelStatus::Installed) GTEST_SKIP() << "Real-ESRGAN not installed";
    // a 96 x 64 crop of a real picture (Tears of Steel, the lab scene), or the test pattern without testmedia/ai
    Image src(96, 64);
    {
        const bool real = std::filesystem::exists(aiMedia("tears_of_steel_60s.mov"));
        VideoDecoder dec(real ? aiMedia("tears_of_steel_60s.mov")
                              : std::filesystem::path(FA_TESTMEDIA_DIR) / "C_testsrc2_1080p30_10s.mp4");
        const Image& f = dec.frameAt(Time(real ? 30 : 3)).image;
        for (int y = 0; y < src.height; ++y)
            std::memcpy(&src.pixels[static_cast<size_t>(y) * src.width * 4],
                        &f.pixels[(static_cast<size_t>(f.height / 3 + y) * f.width + f.width / 2) * 4], src.width * 4);
    }
    const Tensor in = toTensor(src, false);
    const auto param = modelFolder(*m, models()) / "realesr-animevideov3-x2.param";
    const auto bin = modelFolder(*m, models()) / "realesr-animevideov3-x2.bin";
    NcnnModel cpu(param, bin, Accelerator::Cpu);
    const Tensor a = cpu.run({{"data", &in}}, {"output"})[0];
    ASSERT_EQ(a.shape, (std::vector<int64_t>{3, 128, 192}));
    // the enlargement keeps the picture: per-channel means within 2 % of the input's
    for (int c = 0; c < 3; ++c) {
        double si = 0, so = 0;
        for (size_t i = 0; i < 96 * 64; ++i) si += in.data[c * 96 * 64 + i];
        for (size_t i = 0; i < 192 * 128; ++i) so += a.data[c * 192 * 128 + i];
        EXPECT_NEAR(so / (192 * 128), si / (96 * 64), 0.02) << "channel " << c;
    }
    if (!aiCapabilities().vulkan) GTEST_SKIP() << "no Vulkan device on this PC";
    NcnnModel gpu(param, bin, Accelerator::Gpu);
    ASSERT_TRUE(gpu.onGpu()) << gpu.fallbackReason();
    EXPECT_GT(gpu.gpuMemoryBudgetMb(), 0u);
    const Tensor b = gpu.run({{"data", &in}}, {"output"})[0];
    ASSERT_EQ(b.shape, a.shape);
    double mse = 0;
    for (size_t i = 0; i < a.data.size(); ++i) mse += (a.data[i] - b.data[i]) * (a.data[i] - b.data[i]);
    const double psnr = 10 * std::log10(1.0 / std::max(1e-12, mse / a.data.size()));
    std::printf("Real-ESRGAN Vulkan (%s) vs CPU: PSNR %.1f dB\n", gpu.provider().c_str(), psnr);
    EXPECT_GT(psnr, 40.0);   // fp16 on the GPU vs fp32 on the CPU
}
