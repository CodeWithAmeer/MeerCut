// M7 checkpoint F: guarantees across the AI features - no model operation uses the network, the frames a clip shows
// through its transitions are analysed, GPU memory is measured rather than assumed, and missing, damaged or failing
// models give clear errors instead of wrong pictures.
#include "fa/ai/faces.h"
#include "fa/ai/interpolate.h"
#include "fa/ai/matting.h"
#include "fa/ai/models.h"
#include "fa/ai/prepare.h"
#include "fa/ai/speech.h"
#include "fa/ai/upscale.h"
#include "fa/media/media.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace fa;

namespace {

std::filesystem::path models() { return std::filesystem::path(FA_MODELS_DIR); }
std::filesystem::path testmedia() { return std::filesystem::path(FA_TESTMEDIA_DIR); }

std::filesystem::path tempDir(const char* name) {
    const auto d = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

bool installed(const char* id) {
    const ai::ModelInfo* m = ai::findModel(id);
    return m && ai::modelStatus(*m, false, models()) == ai::ModelStatus::Installed;
}

struct ModelsDir {
    explicit ModelsDir(const std::filesystem::path& d) { ai::setModelsDirectoryOverride(d); }
    ~ModelsDir() { ai::setModelsDirectoryOverride({}); }
};

Image frameOf(const std::filesystem::path& file, double seconds, int maxWidth = 0) {
    VideoDecoder::Options o;
    o.maxWidth = maxWidth;
    o.maxHeight = maxWidth;
    VideoDecoder dec(file, o);
    return dec.frameAt(Time(static_cast<int64_t>(seconds * 1000), 1000)).image;
}

}  // namespace

// Once the models are installed nothing goes to the network: the device probe, Background Removal (RVM), face
// detection (YuNet), Speech to Text (whisper), AI Interpolation (RIFE) and Video Upscaling (Real-ESRGAN) all run
// without the downloader - the only code in MeerCut that can reach the network - being called.
TEST(AiOffline, NoModelOperationUsesTheNetwork) {
    for (const char* id : {"rvm-mobilenetv3", "yunet-face", "whisper-tiny", "rife-v4.6", "realesrgan"})
        if (!installed(id)) GTEST_SKIP() << id << " is not installed";
    const auto tos = testmedia() / "ai" / "tears_of_steel_60s.mov";
    if (!std::filesystem::exists(tos)) GTEST_SKIP() << "testmedia/ai missing";
    const ModelsDir dir(models());
    const uint64_t before = ai::networkRequests();
    std::printf("%s", ai::describeCapabilities(ai::aiCapabilities()).c_str());
    const Image frame = frameOf(tos, 30.0, 640), next = frameOf(tos, 30.0 + 1.0 / 24, 640);
    AiCutout cut;
    cut.enabled = true;
    cut.quality = 0;
    EXPECT_EQ(ai::matteForPicture(frame, cut).width, frame.width);
    EXPECT_FALSE(ai::FaceDetector().detect(frameOf(tos, 10.0)).empty());   // the two people of AiFaces
    ai::TranscribeOptions so;
    so.model = "whisper-tiny";
    so.language = "en";
    EXPECT_FALSE(ai::transcribeFile(tos, 29.0, 34.0, so).segments.empty());
    EXPECT_EQ(ai::NeuralInterpolator().interpolate(frame, next, 0.5).width, frame.width);
    EXPECT_EQ(ai::Upscaler("anime", 2).upscale(frame).width, frame.width * 2);
    EXPECT_EQ(ai::networkRequests(), before);
}

// A Background Removal clip followed by a dissolve shows its own frames past its end (the dissolve plays it on into its
// handle), and the clip after it starts before its own start: the frames analysed include them (before, the export
// refused such a project: "no matte for this frame").
TEST(AiCutoutRange, TransitionHandleFramesAreIncluded) {
    MediaSource src;
    src.id = "m";
    src.duration = Rational(60);
    src.frameRate = FrameRate(24);
    Track t{"v1", TrackKind::Video, "Video 1"};
    Clip a;
    a.id = "a";
    a.sourceId = "m";
    a.sourceIn = Rational(10);
    a.duration = Rational(2);
    a.outTransition = TransitionRef{"fa.transition.dissolve", Rational(1, 2)};   // centred: 0.25 s each side of the cut
    Clip b;
    b.id = "b";
    b.sourceId = "m";
    b.sourceIn = Rational(30);
    b.timelineStart = Rational(2);
    b.duration = Rational(2);
    t.clips = {a, b};
    EXPECT_EQ(ai::clipSourceFrames(t.clips[0], src), std::make_pair(int64_t(240), int64_t(287)));   // the clip alone
    EXPECT_EQ(ai::clipSourceFrames(t.clips[0], src, &t), std::make_pair(int64_t(240), int64_t(293)));   // + 6 after
    EXPECT_EQ(ai::clipSourceFrames(t.clips[1], src, &t), std::make_pair(int64_t(714), int64_t(767)));   // + 6 before
    // a reversed clip: its handle after the cut lies before its source range
    t.clips[0].speed.reverse = true;
    EXPECT_EQ(ai::clipSourceFrames(t.clips[0], src, &t), std::make_pair(int64_t(234), int64_t(287)));
}

// GPU memory: what the operating system lets this process use now is measured (it shrinks while other programs use the
// GPU) and is never more than a discrete card has; BiRefNet's GPU check uses it.
TEST(AiMemory, AvailableGpuMemoryIsMeasured) {
    const auto& caps = ai::aiCapabilities();
    for (size_t i = 0; i < caps.gpus.size(); ++i) {
        const auto& g = caps.gpus[i];
        const int64_t free = ai::gpuMemoryAvailableMb(static_cast<int>(i));
        std::printf("%s: %llu MB, %lld MB available now\n", g.name.c_str(), static_cast<unsigned long long>(g.memoryMb),
                    static_cast<long long>(free));
        if (g.dxgiIndex < 0) continue;
        EXPECT_GE(free, 0);
        if (g.discrete) EXPECT_LE(free, static_cast<int64_t>(g.memoryMb));
    }
    EXPECT_EQ(ai::gpuMemoryAvailableMb(-1), -1);
    AiCutout general;
    general.model = "birefnet-general-tiny";
    const int g = ai::preferredDirectMlGpu();
    if (g >= 0 && ai::gpuMemoryAvailableMb(g) >= 0 && ai::gpuMemoryAvailableMb(g) <= 4500) EXPECT_FALSE(ai::cutoutFitsGpu(general));
}

// Missing, damaged and failing models: a clear error that names the Model Manager, never a picture made from garbage.
// A weight file damaged without changing its size is found by its checksum.
TEST(AiModelsNegative, MissingDamagedAndFailingModelsGiveClearErrors) {
    if (!installed("realesrgan") || !installed("rife-v4.6")) GTEST_SKIP() << "Real-ESRGAN / RIFE not installed";
    const auto root = tempDir("fa_ai_negative_models");
    {   // missing
        const ModelsDir dir(root);
        try {
            ai::Upscaler up("anime", 2);
            FAIL() << "no error without the model";
        } catch (const ai::ModelError& e) {
            EXPECT_NE(std::string(e.what()).find("Model Manager"), std::string::npos) << e.what();
        }
        EXPECT_THROW(ai::NeuralInterpolator(), ai::ModelError);
    }
    // damaged: a copy of Real-ESRGAN with one byte of the anime weights changed (same size)
    const ai::ModelInfo& esr = *ai::findModel("realesrgan");
    std::filesystem::create_directories(ai::modelFolder(esr, root));
    for (size_t i = 0; i < esr.files.size(); ++i)
        std::filesystem::copy_file(ai::modelFilePath(esr, i, models()), ai::modelFilePath(esr, i, root));
    {
        const ModelsDir dir(root);
        EXPECT_NO_THROW(ai::Upscaler("anime", 2));   // intact: accepted (and recorded as verified)
        std::fstream f(ai::modelFilePath(esr, 3, root), std::ios::in | std::ios::out | std::ios::binary);   // x2 .bin
        f.seekp(100000);
        f.put('\x5a');
        f.close();
        try {
            ai::Upscaler up("anime", 2);
            FAIL() << "damaged weights accepted";
        } catch (const ai::ModelError& e) {
            EXPECT_NE(std::string(e.what()).find("checksum"), std::string::npos) << e.what();
        }
    }
    // failing inference: frames of different sizes
    {
        const ModelsDir dir(models());
        Image a(64, 64), b(32, 32);
        EXPECT_THROW(ai::NeuralInterpolator().interpolate(a, b, 0.5), ai::ModelError);
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
