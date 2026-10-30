// M7 checkpoint E: AI Interpolation (RIFE 4.6) and Video Upscaling (Real-ESRGAN) with the real models on real footage:
// in-between frames scored against the true middle frame (and compared with M5's Frame Blending and Optical Flow),
// arbitrary time steps, shot changes (nothing invented), CPU against GPU, determinism, the renderer and the export
// using the prepared frames (preview = export), speed ramps and reverse; upscaling scored against the original after a
// 2x reduction (with Lanczos and bilinear as the non-AI references), tiles without seams, exact size, colour and alpha,
// temporal stability with and without flicker reduction, the cache and edits.
#include "fa/ai/interpolate.h"
#include "fa/ai/models.h"
#include "fa/ai/prepare.h"
#include "fa/ai/upscale.h"
#include "fa/media/media.h"
#include "fa/model/edit_ops.h"
#include "fa/model/serialize.h"
#include "fa/model/time_map.h"
#include "fa/render/backend.h"
#include "fa/render/frame_store.h"
#include "fa/render/renderer.h"
#include "fa/vision/flow.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace fa;

namespace {

std::filesystem::path models() { return std::filesystem::path(FA_MODELS_DIR); }
std::filesystem::path sintel() { return std::filesystem::path(FA_TESTMEDIA_DIR) / "ai" / "sintel_trailer_720p.mp4"; }
std::filesystem::path tos() { return std::filesystem::path(FA_TESTMEDIA_DIR) / "ai" / "tears_of_steel_60s.mov"; }

std::filesystem::path tempDir(const char* name) {
    const auto d = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

struct AiEnv {
    std::filesystem::path cache;
    explicit AiEnv(const char* name) : cache(tempDir(name)) {
        _wputenv_s(L"MEERCUT_ANALYSIS_CACHE", cache.wstring().c_str());
        ai::setModelsDirectoryOverride(models());
    }
    ~AiEnv() {
        _wputenv_s(L"MEERCUT_ANALYSIS_CACHE", L"");
        ai::setModelsDirectoryOverride({});
        std::error_code ec;
        std::filesystem::remove_all(cache, ec);
    }
};

bool ready(const char* id) {
    const ai::ModelInfo* m = ai::findModel(id);
    return m && ai::modelStatus(*m, false, models()) == ai::ModelStatus::Installed && std::filesystem::exists(sintel());
}

std::vector<Image> frames(const std::filesystem::path& file, int64_t first, int count, int maxWidth = 0) {
    VideoDecoder::Options o;
    o.maxWidth = maxWidth;
    o.maxHeight = maxWidth ? maxWidth : 0;
    VideoDecoder dec(file, o);
    const Rational fps = dec.info().frameRate.fps();
    std::vector<Image> out;
    for (int i = 0; i < count; ++i) out.push_back(dec.frameAt(Time(first + i) / fps).image);
    return out;
}

double psnr(const Image& a, const Image& b) {
    double mse = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.pixels.size(); i += 4)
        for (int c = 0; c < 3; ++c, ++n) {
            const double d = double(a.pixels[i + c]) - b.pixels[i + c];
            mse += d * d;
        }
    return 10 * std::log10(255.0 * 255.0 / std::max(1e-9, mse / n));
}

// SSIM of the luma on 8 x 8 windows (stride 4), the usual constants.
double ssim(const Image& a, const Image& b) {
    auto luma = [](const Image& im, int x, int y) {
        const uint8_t* p = &im.pixels[(static_cast<size_t>(y) * im.width + x) * 4];
        return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
    };
    const double c1 = std::pow(0.01 * 255, 2), c2 = std::pow(0.03 * 255, 2);
    double sum = 0;
    int n = 0;
    for (int y = 0; y + 8 <= a.height; y += 4)
        for (int x = 0; x + 8 <= a.width; x += 4) {
            double ma = 0, mb = 0, va = 0, vb = 0, cov = 0;
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    ma += luma(a, x + i, y + j);
                    mb += luma(b, x + i, y + j);
                }
            ma /= 64;
            mb /= 64;
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    const double da = luma(a, x + i, y + j) - ma, db = luma(b, x + i, y + j) - mb;
                    va += da * da;
                    vb += db * db;
                    cov += da * db;
                }
            va /= 63;
            vb /= 63;
            cov /= 63;
            sum += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
            ++n;
        }
    return n ? sum / n : 0;
}

Image blend(const Image& a, const Image& b, double t) {
    Image out = a;
    for (size_t i = 0; i < out.pixels.size(); ++i) out.pixels[i] = static_cast<uint8_t>(std::lround(a.pixels[i] * (1 - t) + b.pixels[i] * t));
    return out;
}

// Area reduction by an integer factor (what a lower-resolution camera or export would have recorded).
Image reduce(const Image& src, int k) {
    Image out(src.width / k, src.height / k);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x)
            for (int c = 0; c < 4; ++c) {
                int s = 0;
                for (int j = 0; j < k; ++j)
                    for (int i = 0; i < k; ++i) s += src.pixels[(static_cast<size_t>(y * k + j) * src.width + x * k + i) * 4 + c];
                out.pixels[(static_cast<size_t>(y) * out.width + x) * 4 + c] = static_cast<uint8_t>((s + k * k / 2) / (k * k));
            }
    return out;
}

struct RemovedAtEnd {
    std::filesystem::path dir;
    ~RemovedAtEnd() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

double meanChannel(const Image& im, int c) {
    double s = 0;
    for (size_t i = 0; i < im.pixels.size(); i += 4) s += im.pixels[i + c];
    return s / (im.pixels.size() / 4);
}

}  // namespace

// A low-resolution copy of trailer frames [first, first + frames) at w x h (crf 0 = lossless), the source the upscaling
// tests enlarge again.
std::filesystem::path makeLowres(const std::filesystem::path& dir, int64_t first, int frames, int w, int h, int crf = 0);

// The middle frame of real footage predicted from its neighbours (RIFE at t = 0.5), scored against the true frame and
// against M5's Frame Blending and Optical Flow (Quality) on the same pairs. Shots: slow, moderate and a fast whip pan.
TEST(AiInterpolation, QualityAgainstBlendingAndOpticalFlow) {
    AiEnv env("fa_rife_quality");
    if (!ready("rife-v4.6")) GTEST_SKIP() << "RIFE or testmedia/ai missing";
    ai::NeuralInterpolator rife(ai::Accelerator::Auto);
    struct Shot {
        const char* what;
        int64_t first;
    };
    double sumBlend = 0, sumFlow = 0, sumRife = 0;
    int n = 0;
    for (const Shot s : {Shot{"corridor (slow)", 460}, Shot{"city (moderate)", 590}, Shot{"dragon flying", 790},
                         Shot{"desert", 900}, Shot{"whip pan (fast)", 724}}) {
        const auto f = frames(sintel(), s.first, 3);
        Image flow;
        vision::interpolateFrames(f[0], f[2], 0.5, flow, vision::FlowQuality::Quality);
        const auto t0 = std::chrono::steady_clock::now();
        const Image r = rife.interpolate(f[0], f[2], 0.5);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const double pb = psnr(blend(f[0], f[2], 0.5), f[1]), pf = psnr(flow, f[1]), pr = psnr(r, f[1]);
        std::printf("%-18s blend %.2f dB, optical flow %.2f dB, RIFE %.2f dB (SSIM %.4f / %.4f / %.4f), RIFE %.0f ms\n", s.what,
                    pb, pf, pr, ssim(blend(f[0], f[2], 0.5), f[1]), ssim(flow, f[1]), ssim(r, f[1]), ms);
        sumBlend += pb;
        sumFlow += pf;
        sumRife += pr;
        ++n;
        EXPECT_EQ(r.width, f[1].width);
        EXPECT_EQ(r.height, f[1].height);
    }
    std::printf("mean over %d pairs: blend %.2f dB, optical flow %.2f dB, RIFE %.2f dB (%s)\n", n, sumBlend / n,
                sumFlow / n, sumRife / n, rife.provider().c_str());
    RecordProperty("rife_db", std::to_string(sumRife / n));
    RecordProperty("flow_db", std::to_string(sumFlow / n));
    EXPECT_GT(sumRife / n, sumBlend / n + 1.0);   // a real in-between frame, not a mix
}

// Any time step: from frames f and f + 4 of two moving shots (the city, the desert), the frames f + 1, f + 2 and f + 3
// (t = 0.25 / 0.5 / 0.75) are predicted better than a mix at the same weights; t = 0 and 1 return the end frames.
// (The dragon shot at 790 is not used here: the animation holds frames - 793 equals 794 - so the true in-between
// frame is an end frame and a mix scores well by accident.)
TEST(AiInterpolation, ArbitraryTimeSteps) {
    AiEnv env("fa_rife_steps");
    if (!ready("rife-v4.6")) GTEST_SKIP() << "RIFE or testmedia/ai missing";
    ai::NeuralInterpolator rife(ai::Accelerator::Auto);
    for (int64_t first : {590, 900}) {
        const auto f = frames(sintel(), first, 5);
        for (int k : {1, 2, 3}) {
            const double t = k / 4.0;
            Image flow;
            vision::interpolateFrames(f[0], f[4], t, flow, vision::FlowQuality::Quality);
            const double pr = psnr(rife.interpolate(f[0], f[4], t), f[static_cast<size_t>(k)]);
            const double pb = psnr(blend(f[0], f[4], t), f[static_cast<size_t>(k)]);
            std::printf("frames %lld / %lld, t = %.2f: RIFE %.2f dB, blend %.2f dB, optical flow %.2f dB\n",
                        static_cast<long long>(first), static_cast<long long>(first + 4), t, pr, pb, psnr(flow, f[static_cast<size_t>(k)]));
            EXPECT_GT(pr, pb + 1.0);
        }
        EXPECT_GT(psnr(rife.interpolate(f[0], f[4], 0.0), f[0]), 30.0);
        EXPECT_GT(psnr(rife.interpolate(f[0], f[4], 1.0), f[4]), 30.0);
    }
}

// The GPU and the CPU compute the same frame; the GPU computes it the same way twice (deterministic export).
TEST(AiInterpolation, CpuAgainstGpuAndDeterminism) {
    AiEnv env("fa_rife_devices");
    if (!ready("rife-v4.6")) GTEST_SKIP() << "RIFE or testmedia/ai missing";
    const auto f = frames(sintel(), 590, 3, 640);
    ai::NeuralInterpolator cpu(ai::Accelerator::Cpu);
    const auto t0 = std::chrono::steady_clock::now();
    const Image c = cpu.interpolate(f[0], f[2], 0.5);
    const double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_FALSE(cpu.onGpu());
    if (ai::aiCapabilities().gpus.empty() || !ai::aiCapabilities().vulkan) GTEST_SKIP() << "no Vulkan GPU";
    ai::NeuralInterpolator gpu(ai::Accelerator::Gpu);
    ASSERT_TRUE(gpu.onGpu());
    gpu.interpolate(f[0], f[2], 0.5);   // warm-up
    const auto t1 = std::chrono::steady_clock::now();
    const Image g1 = gpu.interpolate(f[0], f[2], 0.5);
    const double gpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    const Image g2 = gpu.interpolate(f[0], f[2], 0.5);
    std::printf("640 x 360: CPU %.0f ms, %s %.0f ms; CPU vs GPU %.1f dB\n", cpuMs, gpu.provider().c_str(), gpuMs, psnr(c, g1));
    EXPECT_GT(psnr(c, g1), 40.0);
    EXPECT_EQ(g1.pixels, g2.pixels);
}

// A clip slowed to 0.25x with AI Interpolation: preparing computes each needed in-between frame once (a second
// preparation computes none), the renderer then draws exactly the prepared frames; a pair across a shot change gets no
// invented frame (the nearer source frame is shown); without preparation the preview falls back to optical flow and
// says which clips miss frames, and the export refuses them.
TEST(AiInterpolation, PreparedFramesInTheRendererAndShotChanges) {
    AiEnv env("fa_rife_render");
    if (!ready("rife-v4.6")) GTEST_SKIP() << "RIFE or testmedia/ai missing";
    Project p;
    MediaSource s;
    s.id = "sintel";
    const std::u8string u8 = sintel().u8string();
    s.path = std::string(u8.begin(), u8.end());
    s.duration = Rational(1253, 24);
    s.frameRate = FrameRate(24);
    s.width = 1280;
    s.height = 720;
    p.sources.push_back(s);
    Sequence q;
    q.id = "seq";
    q.width = 640;
    q.height = 360;
    q.frameRate = FrameRate(24);
    Track t{"v1", TrackKind::Video, "Video 1"};
    Clip c;
    c.id = "c";
    c.sourceId = "sintel";
    c.sourceIn = Rational(616, 24);   // the city shot, the cut at 621 into the sky shot, then the sky
    c.speed.speed = Rational(1, 4);
    c.speed.interpolation = FrameInterpolation::Neural;
    c.duration = Rational(10 * 4, 24);   // 10 source frames at 0.25x
    t.clips.push_back(c);
    q.tracks.push_back(t);
    p.sequences.push_back(q);
    const auto backend = makeBackend(BackendChoice::Cpu);
    {   // not prepared: the preview still draws (optical flow) and reports the clip; the export refuses
        DecoderPool pool("");
        const Image img = renderFrame(p.sequences[0], 2, p, pool, *backend);
        EXPECT_EQ(img.width, 640);
        EXPECT_EQ(pool.clipsMissingNeuralFrames().count("c"), 1u);
        RenderOptions o;
        o.includeAudio = false;
        EXPECT_THROW(renderProject(p, "", std::filesystem::temp_directory_path() / "fa_rife_refused.mp4", o), RenderError);
    }
    const ai::NeuralPreparation prep = ai::prepareNeuralFrames(p, p.sequences[0], 0, 40, "");
    std::printf("prepared %lld of %lld in-between frames (%lld across the shot change) in %.1f s (%s)\n",
                static_cast<long long>(prep.computed), static_cast<long long>(prep.needed),
                static_cast<long long>(prep.sceneCuts), prep.seconds, prep.provider.c_str());
    EXPECT_GT(prep.computed, 0);
    EXPECT_GE(prep.sceneCuts, 1);
    EXPECT_EQ(ai::prepareNeuralFrames(p, p.sequences[0], 0, 40, "").computed, 0);   // cached
    DecoderPool pool("");
    for (int64_t f = 0; f < 40; ++f) renderFrame(p.sequences[0], f, p, pool, *backend);
    EXPECT_TRUE(pool.clipsMissingNeuralFrames().empty());
    // across the cut (source 620 -> 621, frames 16..19 of the output): no mix of the two shots - each output frame is
    // one of the two source frames
    const auto src = frames(sintel(), 620, 2, 640);
    for (int64_t f = 17; f <= 19; ++f) {
        const Image img = renderFrame(p.sequences[0], f, p, pool, *backend);
        const double a = psnr(img, resizeBilinear(src[0], img.width, img.height));
        const double b = psnr(img, resizeBilinear(src[1], img.width, img.height));
        std::printf("output %lld across the cut: %.1f dB to frame 620, %.1f dB to 621\n", static_cast<long long>(f), a, b);
        EXPECT_GT(std::max(a, b), 30.0);
    }
    // the export now succeeds and matches the preview
    RenderOptions o;
    o.includeAudio = false;
    o.encoder.crf = 0;
    const auto out = std::filesystem::temp_directory_path() / "fa_rife_export.mp4";
    std::filesystem::remove(out);
    EXPECT_EQ(renderProject(p, "", out, o).framesWritten, 40);
    Image exported;
    {
        VideoDecoder dec(out);
        exported = dec.frameAt(Time(6, 24)).image;
    }
    const Image preview = renderFrame(p.sequences[0], 6, p, pool, *backend);
    std::printf("frame 6 (an AI in-between frame): preview vs export %.1f dB\n", psnr(preview, exported));
    EXPECT_GT(psnr(preview, exported), 38.0);
    std::filesystem::remove(out);
}

// Speed ramps and reverse with AI Interpolation: preparation covers every frame the renderer needs.
TEST(AiInterpolation, RampsAndReverseArePrepared) {
    AiEnv env("fa_rife_ramp");
    if (!ready("rife-v4.6")) GTEST_SKIP() << "RIFE or testmedia/ai missing";
    Project p;
    MediaSource s;
    s.id = "sintel";
    const std::u8string u8 = sintel().u8string();
    s.path = std::string(u8.begin(), u8.end());
    s.duration = Rational(1253, 24);
    s.frameRate = FrameRate(24);
    p.sources.push_back(s);
    Sequence q;
    q.id = "seq";
    q.width = 320;
    q.height = 180;
    q.frameRate = FrameRate(24);
    Track t{"v1", TrackKind::Video, "Video 1"};
    Clip c;
    c.id = "c";
    c.sourceId = "sintel";
    c.sourceIn = Rational(790, 24);
    c.speed.speed = Rational(1, 2);
    c.speed.reverse = true;
    c.speed.interpolation = FrameInterpolation::Neural;
    c.duration = Rational(16, 24);
    t.clips.push_back(c);
    Clip r = c;
    r.id = "r";
    r.timelineStart = Rational(16, 24);
    r.speed.reverse = false;
    r.speed.ramp = {SpeedKey{Rational(790, 24), 0.5}, SpeedKey{Rational(798, 24), 2.0}};
    r.duration = durationForSource(r.speed, r.sourceIn, Rational(8, 24));
    t.clips.push_back(r);
    q.tracks.push_back(t);
    p.sequences.push_back(q);
    const int64_t end = (p.sequences[0].duration() * Rational(24)).ceil();
    const ai::NeuralPreparation prep = ai::prepareNeuralFrames(p, p.sequences[0], 0, end, "");
    std::printf("reverse 0.5x + ramp 0.5x -> 2x: %lld frames prepared of %lld needed\n", static_cast<long long>(prep.computed),
                static_cast<long long>(prep.needed));
    EXPECT_GT(prep.computed, 0);
    DecoderPool pool("");
    const auto backend = makeBackend(BackendChoice::Cpu);
    for (int64_t f = 0; f < end; ++f) renderFrame(p.sequences[0], f, p, pool, *backend);
    EXPECT_TRUE(pool.clipsMissingNeuralFrames().empty());
}

// Upscaling: Sintel frames reduced 2x and enlarged again, scored against the original - clean (an area reduction)
// and as low-resolution video usually is (the reduction encoded with H.264 at CRF 30) - for Real-ESRGAN (general
// and anime models) with Lanczos and bilinear as the non-AI references. Exact size; colours kept (the mean of each
// channel; the anime model, trained on drawn pictures, lifts live action by up to ~3 levels - measured); the time per
// frame. These are reported as measured: on clean input Real-ESRGAN does not beat Lanczos on PSNR / SSIM (it is a
// restoration model that removes what it takes for noise and compression); no ranking is asserted.
TEST(AiUpscale, QualityAgainstLanczosAndBilinear) {
    AiEnv env("fa_esrgan_quality");
    if (!ready("realesrgan")) GTEST_SKIP() << "Real-ESRGAN or testmedia/ai missing";
    const auto dir = tempDir("fa_esrgan_quality_src");
    const RemovedAtEnd cleanup{dir};
    ai::Upscaler general("general", 2, ai::Accelerator::Auto), anime("anime", 2, ai::Accelerator::Auto);
    for (int64_t at : {590, 900}) {
        const Image orig = frames(sintel(), at, 1).front();
        Image compressed;
        {
            const auto file = makeLowres(dir, at, 1, orig.width / 2, orig.height / 2, 30);
            compressed = frames(file, 0, 1).front();
        }
        for (const bool clean : {true, false}) {
            const Image small = clean ? reduce(orig, 2) : compressed;
            const Image lz = resizeLanczos(small, orig.width, orig.height), bl = resizeBilinear(small, orig.width, orig.height);
            std::printf("frame %lld, %s: bilinear %.2f dB / SSIM %.4f, Lanczos %.2f dB / %.4f\n", static_cast<long long>(at),
                        clean ? "clean 2x reduction" : "2x reduction, H.264 CRF 30", psnr(bl, orig), ssim(bl, orig), psnr(lz, orig),
                        ssim(lz, orig));
            for (ai::Upscaler* up : {&general, &anime}) {
                const bool isAnime = up == &anime;
                const auto t0 = std::chrono::steady_clock::now();
                const Image ai = up->upscale(small);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                ASSERT_EQ(ai.width, orig.width);
                ASSERT_EQ(ai.height, orig.height);
                std::printf("  Real-ESRGAN %-7s %.2f dB / SSIM %.4f, %.0f ms for 640 x 360 -> 1280 x 720 (%s, tile %d); channel "
                            "means %+.1f %+.1f %+.1f\n",
                            isAnime ? "anime" : "general", psnr(ai, orig), ssim(ai, orig), ms, up->provider().c_str(), up->tile(),
                            meanChannel(ai, 0) - meanChannel(orig, 0), meanChannel(ai, 1) - meanChannel(orig, 1),
                            meanChannel(ai, 2) - meanChannel(orig, 2));
                for (int ch = 0; ch < 3; ++ch) EXPECT_NEAR(meanChannel(ai, ch), meanChannel(orig, ch), isAnime ? 4.0 : 3.0);
                // no gross error: close to the classic filter on the same input (measured: within 2.4 dB / 0.02 SSIM below
                // it on clean input, at or above it on compressed input)
                EXPECT_GT(psnr(ai, orig), psnr(lz, orig) - 3.0);
                EXPECT_GT(ssim(ai, orig), ssim(lz, orig) - 0.03);
            }
        }
    }
}

// Tiles: a picture upscaled in small (48-pixel) tiles against the same picture in one tile. Without overlap the tile
// borders show (the control); with the default overlap they differ from one tile no more than the rest of the picture
// does (what remains everywhere is the GPU's half-precision arithmetic, which depends on the tile size). Alpha is kept.
TEST(AiUpscale, TilesWithoutSeamsAndAlpha) {
    AiEnv env("fa_esrgan_tiles");
    if (!ready("realesrgan")) GTEST_SKIP() << "Real-ESRGAN or testmedia/ai missing";
    Image small = reduce(frames(sintel(), 590, 1).front(), 4);   // 320 x 180
    for (int y = 0; y < small.height; ++y)   // an alpha ramp from left to right
        for (int x = 0; x < small.width; ++x) small.pixels[(static_cast<size_t>(y) * small.width + x) * 4 + 3] = static_cast<uint8_t>(x * 255 / (small.width - 1));
    struct Seams {
        double borderMean = 0, borderMax = 0, restMean = 0, restMax = 0;
    };
    auto seams = [](const Image& a, const Image& b) {   // tile borders at every 96th output pixel (2x of 48)
        Seams r;
        double sb = 0, sr = 0;
        long nb = 0, nr = 0;
        for (int y = 0; y < a.height; ++y)
            for (int x = 0; x < a.width; ++x) {
                const bool border = x % 96 <= 1 || x % 96 >= 94 || y % 96 <= 1 || y % 96 >= 94;
                for (int c = 0; c < 3; ++c) {
                    const size_t i = (static_cast<size_t>(y) * a.width + x) * 4 + c;
                    const double d = std::abs(double(a.pixels[i]) - b.pixels[i]);
                    if (border) {
                        sb += d;
                        ++nb;
                        r.borderMax = std::max(r.borderMax, d);
                    } else {
                        sr += d;
                        ++nr;
                        r.restMax = std::max(r.restMax, d);
                    }
                }
            }
        r.borderMean = sb / nb;
        r.restMean = sr / nr;
        return r;
    };
    Image a;
    for (const char* model : {"anime", "general"}) {
        ai::Upscaler whole(model, 2, ai::Accelerator::Auto, 512, 0), noOverlap(model, 2, ai::Accelerator::Auto, 48, 0),
            tiled(model, 2, ai::Accelerator::Auto, 48);
        const Image b = whole.upscale(small);
        a = tiled.upscale(small);
        const Seams s0 = seams(noOverlap.upscale(small), b), s = seams(a, b);
        std::printf("%s, 48-pixel tiles against one tile: no overlap - borders mean %.2f / max %.0f, elsewhere %.2f / %.0f; "
                    "overlap %d - borders %.2f / %.0f, elsewhere %.2f / %.0f (%.1f dB)\n",
                    model, s0.borderMean, s0.borderMax, s0.restMean, s0.restMax, tiled.pad(), s.borderMean, s.borderMax,
                    s.restMean, s.restMax, psnr(a, b));
        EXPECT_GT(s0.borderMean, 2 * s.borderMean);              // the control: without overlap the borders show
        EXPECT_LE(s.borderMean, 1.5 * s.restMean + 0.05);         // with it they are like the rest of the picture
        EXPECT_LE(s.borderMax, s.restMax + 4);
        EXPECT_GT(psnr(a, b), 45.0);
    }
    const Image alpha = resizeLanczos(small, a.width, a.height);
    for (int x : {0, a.width / 2, a.width - 1}) EXPECT_NEAR(a.pixels[(static_cast<size_t>(a.height / 2) * a.width + x) * 4 + 3],
                                                            alpha.pixels[(static_cast<size_t>(a.height / 2) * a.width + x) * 4 + 3], 2);
}

std::filesystem::path makeLowres(const std::filesystem::path& dir, int64_t first, int frames, int w, int h, int crf) {
    const auto out = dir / ("low_" + std::to_string(first) + "_" + std::to_string(w) + "_" + std::to_string(crf) + ".mp4");
    Project p;
    MediaSource s;
    s.id = "s";
    const std::u8string u8 = sintel().u8string();
    s.path = std::string(u8.begin(), u8.end());
    s.duration = Rational(1253, 24);
    s.frameRate = FrameRate(24);
    p.sources.push_back(s);
    Sequence q;
    q.id = "seq";
    q.width = w;
    q.height = h;
    q.frameRate = FrameRate(24);
    Track t{"v1", TrackKind::Video, "Video 1"};
    Clip c;
    c.id = "c";
    c.sourceId = "s";
    c.sourceIn = Rational(first, 24);
    c.duration = Rational(frames, 24);
    t.clips.push_back(c);
    q.tracks.push_back(t);
    p.sequences.push_back(q);
    RenderOptions o;
    o.includeAudio = false;
    o.encoder.crf = crf;
    renderProject(p, "", out, o);
    return out;
}

// Temporal stability: in a slow shot, consecutive upscaled frames should change only where the source changes. The
// flicker (mean change of the result where the source is still) of the prepared frames without and with the flicker
// reduction, and of Lanczos (non-AI) as the reference.
TEST(AiUpscale, FlickerOfStaticAreas) {
    AiEnv env("fa_esrgan_flicker");
    if (!ready("realesrgan")) GTEST_SKIP() << "Real-ESRGAN or testmedia/ai missing";
    const auto dir = tempDir("fa_esrgan_flicker_src");
    const RemovedAtEnd cleanup{dir};
    const auto low = makeLowres(dir, 450, 10, 320, 180);   // the dark corridor: slow camera
    std::vector<Image> src = frames(low, 0, 10);
    auto flicker = [&](const std::vector<Image>& out) {
        double sum = 0;
        int n = 0;
        for (size_t k = 1; k < out.size(); ++k)
            for (int y = 0; y < out[k].height; ++y)
                for (int x = 0; x < out[k].width; ++x) {
                    const size_t si = (static_cast<size_t>(y * src[k].height / out[k].height) * src[k].width + x * src[k].width / out[k].width) * 4;
                    int d = 0;
                    for (int c = 0; c < 3; ++c) d = std::max(d, std::abs(src[k].pixels[si + c] - src[k - 1].pixels[si + c]));
                    if (d > 1) continue;   // only where the source is still
                    const size_t oi = (static_cast<size_t>(y) * out[k].width + x) * 4;
                    for (int c = 0; c < 3; ++c, ++n) sum += std::abs(out[k].pixels[oi + c] - out[k - 1].pixels[oi + c]);
                }
        return n ? sum / n : 0.0;
    };
    // a 320 x 180 clip on a 640 x 360 canvas, AI 2x, prepared with the flicker reduction off and at its default
    double result[2] = {0, 0};
    for (int k = 0; k < 2; ++k) {
        Project p;
        MediaSource s;
        s.id = "low";
        const std::u8string u8 = low.u8string();
        s.path = std::string(u8.begin(), u8.end());
        s.duration = Rational(10, 24);
        s.frameRate = FrameRate(24);
        s.width = 320;
        s.height = 180;
        p.sources.push_back(s);
        Sequence q;
        q.id = "seq";
        q.width = 640;
        q.height = 360;
        q.frameRate = FrameRate(24);
        Track t{"v1", TrackKind::Video, "Video 1"};
        Clip c;
        c.id = "c";
        c.sourceId = "low";
        c.duration = Rational(10, 24);
        c.upscale.mode = UpscaleMode::Ai;
        c.upscale.model = "general";
        c.upscale.deflicker = k == 0 ? 0.0 : 0.5;
        t.clips.push_back(c);
        q.tracks.push_back(t);
        p.sequences.push_back(q);
        ai::prepareUpscales(p, p.sequences[0], 0, 10, "");
        const FrameStore store(FrameSetKey{"upscale", matteMediaIdentity(low), ai::upscaleModelSha("general", 2),
                                           std::string("general|2|640x360|") + std::to_string(c.upscale.deflicker)});
        std::vector<Image> out(10);
        for (int f = 0; f < 10; ++f) ASSERT_TRUE(store.read(std::to_string(f), out[static_cast<size_t>(f)])) << "frame " << f;
        result[k] = flicker(out);
    }
    std::vector<Image> lz;
    for (const Image& f : src) lz.push_back(resizeLanczos(f, 640, 360));
    std::printf("static-area change per frame: Lanczos %.3f levels, Real-ESRGAN %.3f without flicker reduction, %.3f with it\n",
                flicker(lz), result[0], result[1]);
    RecordProperty("flicker_ai", std::to_string(result[0]));
    RecordProperty("flicker_ai_reduced", std::to_string(result[1]));
    EXPECT_LT(result[1], result[0]);   // the reduction works where the picture is still
}

// The renderer and the export use the prepared upscaled frames (Lanczos needs no preparation); the cache is reused,
// and a trim needs no new upscaling; the shown size is what the clip needs on the canvas.
TEST(AiUpscale, PreparedFramesInPreviewAndExport) {
    AiEnv env("fa_esrgan_render");
    if (!ready("realesrgan")) GTEST_SKIP() << "Real-ESRGAN or testmedia/ai missing";
    // a 640 x 360 source made from the trailer (lossless), shown on a 1280 x 720 canvas
    const auto dir = tempDir("fa_esrgan_src");
    const RemovedAtEnd cleanup{dir};
    const auto lowres = makeLowres(dir, 450, 12, 640, 360);
    Project p;
    MediaSource s;
    s.id = "low";
    const std::u8string u8 = lowres.u8string();
    s.path = std::string(u8.begin(), u8.end());
    s.duration = Rational(12, 24);
    s.frameRate = FrameRate(24);
    s.width = 640;
    s.height = 360;
    p.sources.push_back(s);
    Sequence q;
    q.id = "seq";
    q.width = 1280;
    q.height = 720;
    q.frameRate = FrameRate(24);
    Track t{"v1", TrackKind::Video, "Video 1"};
    Clip c;
    c.id = "c";
    c.sourceId = "low";
    c.duration = Rational(12, 24);
    c.upscale.mode = UpscaleMode::Ai;
    c.upscale.model = "anime";
    c.upscale.scale = 2;
    t.clips.push_back(c);
    q.tracks.push_back(t);
    p.sequences.push_back(q);
    EXPECT_EQ(upscaleTarget(c, 640, 360, 1280, 720), std::make_pair(1280, 720));
    EXPECT_EQ(upscaleTarget(c, 1920, 1080, 1280, 720), std::make_pair(0, 0));   // shown smaller: nothing to do
    const auto backend = makeBackend(BackendChoice::Cpu);
    DecoderPool early("");   // an editor's pool that drew the frame before it was prepared
    const Image before = renderFrame(p.sequences[0], 3, p, early, *backend);
    EXPECT_EQ(early.clipsMissingUpscales().count("c"), 1u);   // the preview shows the plain frame and says so
    const ai::UpscalePreparation prep = ai::prepareUpscales(p, p.sequences[0], 0, 12, "");
    std::printf("upscaled %lld frames in %.1f s (%s)\n", static_cast<long long>(prep.computed), prep.seconds, prep.provider.c_str());
    EXPECT_EQ(prep.computed, 12);
    EXPECT_EQ(ai::prepareUpscales(p, p.sequences[0], 0, 12, "").computed, 0);
    DecoderPool pool("");
    const Image up = renderFrame(p.sequences[0], 5, p, pool, *backend);
    EXPECT_TRUE(pool.clipsMissingUpscales().empty());
    {   // the pool that missed frame 3 draws it now, without an edit (it looks a missing frame up again)
        const Image after = renderFrame(p.sequences[0], 3, p, early, *backend);
        EXPECT_GT(psnr(after, renderFrame(p.sequences[0], 3, p, pool, *backend)), 60.0);
        EXPECT_LT(psnr(after, before), 45.0);
    }
    // sharper than the plain enlargement: more detail (mean gradient) than the same frame drawn without upscaling
    Project plain = p;
    plain.sequences[0].tracks[0].clips[0].upscale = ClipUpscale{};
    DecoderPool pool2("");
    const Image base = renderFrame(plain.sequences[0], 5, plain, pool2, *backend);
    auto detail = [](const Image& im) {
        double s = 0;
        for (int y = 1; y < im.height; ++y)
            for (int x = 1; x < im.width; ++x) {
                const size_t i = (static_cast<size_t>(y) * im.width + x) * 4;
                s += std::abs(im.pixels[i + 1] - im.pixels[i - 4 + 1]) + std::abs(im.pixels[i + 1] - im.pixels[i - static_cast<size_t>(im.width) * 4 + 1]);
            }
        return s / (im.width * im.height);
    };
    std::printf("detail: plain %.2f, upscaled %.2f\n", detail(base), detail(up));
    EXPECT_GT(detail(up), detail(base));
    // Lanczos: no preparation, nothing missing
    Project lz = p;
    lz.sequences[0].tracks[0].clips[0].upscale.mode = UpscaleMode::Lanczos;
    DecoderPool pool3("");
    renderFrame(lz.sequences[0], 5, lz, pool3, *backend);
    EXPECT_TRUE(pool3.clipsMissingUpscales().empty());
    // export = preview; a trim reuses the cache
    RenderOptions o;
    o.includeAudio = false;
    o.encoder.crf = 0;
    const auto out = dir / "up.mp4";
    EXPECT_EQ(renderProject(p, "", out, o).framesWritten, 12);
    Image exported;
    {
        VideoDecoder dec(out);
        exported = dec.frameAt(Time(5, 24)).image;
    }
    std::printf("upscaled frame: preview vs export %.1f dB\n", psnr(up, exported));
    EXPECT_GT(psnr(up, exported), 38.0);
    Project e = p;
    UndoStack st(e);
    st.execute(edit::trimClip(e, "seq", "c", ClipEdge::Head, Rational(3, 24)));
    EXPECT_EQ(ai::prepareUpscales(e, e.sequences[0], 0, 12, "").computed, 0);
}

// Shot changes are found by Scene Detection (checkpoint D) over the clip's source range: on the Sintel trailer
// (source frames 600-679, hand-checked cuts at 621, 658 and 672) slowed to 0.5x, exactly the three pairs across a cut
// get no invented frame, every other pair a RIFE frame.
TEST(AiInterpolation, ShotChangesFoundBySceneDetection) {
    AiEnv env("fa_rife_cuts");
    if (!ready("rife-v4.6")) GTEST_SKIP() << "RIFE or testmedia/ai missing";
    Project p;
    MediaSource s;
    s.id = "sintel";
    const std::u8string u8 = sintel().u8string();
    s.path = std::string(u8.begin(), u8.end());
    s.duration = Rational(1253, 24);
    s.frameRate = FrameRate(24);
    s.width = 1280;
    s.height = 720;
    p.sources.push_back(s);
    Sequence q;
    q.id = "seq";
    q.width = 640;
    q.height = 360;
    q.frameRate = FrameRate(24);
    Track t{"v1", TrackKind::Video, "Video 1"};
    Clip c;
    c.id = "c";
    c.sourceId = "sintel";
    c.sourceIn = Rational(600, 24);
    c.speed.speed = Rational(1, 2);
    c.speed.interpolation = FrameInterpolation::Neural;
    c.duration = Rational(80 * 2, 24);
    t.clips.push_back(c);
    q.tracks.push_back(t);
    p.sequences.push_back(q);
    const ai::NeuralPreparation prep = ai::prepareNeuralFrames(p, p.sequences[0], 0, 160, "");
    std::printf("%lld pairs: %lld RIFE frames, %lld across a shot change, %.1f s (%s)\n", static_cast<long long>(prep.needed),
                static_cast<long long>(prep.computed), static_cast<long long>(prep.sceneCuts), prep.seconds, prep.provider.c_str());
    const FrameStore store(FrameSetKey{"rife", matteMediaIdentity(sintel()), ai::interpolationModelSha(), "v1"});
    std::vector<int64_t> marked;
    for (int64_t a = 600; a < 679; ++a) {
        const std::string name = interpolationFrameName(a, a + 1, 0.5);
        if (store.isMarked(name)) marked.push_back(a + 1);
        else EXPECT_TRUE(store.has(name)) << a;
    }
    EXPECT_EQ(marked, (std::vector<int64_t>{621, 658, 672}));
    EXPECT_EQ(prep.sceneCuts, 3);
}

// The estimate before long work counts exactly what preparation will compute (nothing once prepared); a damaged
// cached frame is computed again (and drawn); a replaced source file gets a new set (nothing stale is shown); the
// whole clip on the CPU would be announced as CPU work.
TEST(AiUpscale, EstimateDamagedCacheAndReplacedSource) {
    AiEnv env("fa_esrgan_estimate");
    if (!ready("realesrgan")) GTEST_SKIP() << "Real-ESRGAN or testmedia/ai missing";
    const auto dir = tempDir("fa_esrgan_estimate_src");
    const RemovedAtEnd cleanup{dir};
    const auto made = makeLowres(dir, 900, 6, 320, 180);
    const auto media = dir / "clip.mp4";
    std::filesystem::rename(made, media);
    Project p;
    MediaSource s;
    s.id = "low";
    const std::u8string u8 = media.u8string();
    s.path = std::string(u8.begin(), u8.end());
    s.duration = Rational(6, 24);
    s.frameRate = FrameRate(24);
    s.width = 320;
    s.height = 180;
    p.sources.push_back(s);
    Sequence q;
    q.id = "seq";
    q.width = 640;
    q.height = 360;
    q.frameRate = FrameRate(24);
    Track t{"v1", TrackKind::Video, "Video 1"};
    Clip c;
    c.id = "c";
    c.sourceId = "low";
    c.duration = Rational(6, 24);
    c.upscale.mode = UpscaleMode::Ai;
    c.upscale.model = "anime";
    t.clips.push_back(c);
    q.tracks.push_back(t);
    p.sequences.push_back(q);
    const ai::PreparationEstimate e0 = ai::estimatePreparation(p, "seq", "", 0, 6);
    std::printf("estimate: %s\n", e0.text.c_str());
    EXPECT_EQ(e0.upscaled, 6);
    EXPECT_GT(e0.seconds, 0.0);
    EXPECT_EQ(e0.frames(), 6);
    EXPECT_EQ(ai::prepareForRender(p, "seq", "", 0, 6).upscaled, 6);
    EXPECT_EQ(ai::estimatePreparation(p, "seq", "", 0, 6).frames(), 0);
    EXPECT_EQ(ai::prepareForRender(p, "seq", "", 0, 6).upscaled, 0);
    // a damaged cached frame: computed again, then drawn
    const FrameStore store(FrameSetKey{"upscale", matteMediaIdentity(media), ai::upscaleModelSha("anime", 2),
                                       std::string("anime|2|640x360|") + std::to_string(0.5)});
    ASSERT_TRUE(std::filesystem::exists(store.folder() / "3.frame"));
    std::ofstream(store.folder() / "3.frame", std::ios::binary | std::ios::trunc) << "LCFR damaged";
    EXPECT_EQ(ai::prepareForRender(p, "seq", "", 0, 6).upscaled, 1);
    {
        DecoderPool pool("");
        const auto backend = makeBackend(BackendChoice::Cpu);
        renderFrame(p.sequences[0], 3, p, pool, *backend);
        EXPECT_TRUE(pool.clipsMissingUpscales().empty());
    }
    // the source file replaced by other footage under the same name: a new set, all six computed again
    const auto other = makeLowres(dir, 590, 6, 320, 180);
    std::filesystem::remove(media);
    std::filesystem::rename(other, media);
    EXPECT_EQ(ai::estimatePreparation(p, "seq", "", 0, 6).upscaled, 6);
    {   // nothing of the old footage is drawn: the renderer reports the frames as missing
        DecoderPool pool("");
        const auto backend = makeBackend(BackendChoice::Cpu);
        renderFrame(p.sequences[0], 2, p, pool, *backend);
        EXPECT_EQ(pool.clipsMissingUpscales().count("c"), 1u);
    }
    EXPECT_EQ(ai::prepareForRender(p, "seq", "", 0, 6).upscaled, 6);
    // long work is announced: the general model at 4x over a minute of this clip is not started without asking
    EXPECT_FALSE(ai::needsConfirmation(ai::estimatePreparation(p, "seq", "", 0, 6)));
    ai::PreparationEstimate big;
    big.upscaled = 1440;
    big.seconds = 1440 * ai::upscaleSecondsPerFrame("general", 4, 0.92, false);
    big.cpu = true;
    std::printf("a minute of 1280 x 720 with the general model on the CPU: %s\n", ai::durationText(big.seconds).c_str());
    EXPECT_TRUE(ai::needsConfirmation(big));
}


// Project format v9: AI Interpolation and Video Upscaling settings are saved and loaded; a v8 file (without them) loads
// unchanged; impossible settings are refused by validation. No model needed.
TEST(AiEnhanceProject, FormatV9RoundTripOlderVersionsAndValidation) {
    auto make = [] {
        Project p;
        MediaSource s;
        s.id = "m";
        s.path = "clip.mp4";
        s.duration = Rational(10);
        s.frameRate = FrameRate(24);
        s.width = 640;
        s.height = 360;
        p.sources.push_back(s);
        Sequence q;
        q.id = "seq";
        q.width = 1280;
        q.height = 720;
        q.frameRate = FrameRate(24);
        Track t{"v1", TrackKind::Video, "Video 1"};
        Clip c;
        c.id = "c";
        c.sourceId = "m";
        c.duration = Rational(4);
        t.clips.push_back(c);
        q.tracks.push_back(t);
        p.sequences.push_back(q);
        return p;
    };
    Project p = make();
    Clip& c = p.sequences[0].tracks[0].clips[0];
    c.speed.speed = Rational(1, 4);
    c.duration = Rational(16);
    c.speed.interpolation = FrameInterpolation::Neural;
    c.upscale.mode = UpscaleMode::Ai;
    c.upscale.model = "anime";
    c.upscale.scale = 3;
    c.upscale.deflicker = 0.25;
    EXPECT_FALSE(hasErrors(validate(p))) << formatIssues(validate(p));
    const std::string json = projectToJsonString(p);
    EXPECT_NE(json.find("\"version\": " + std::to_string(Project::kFormatVersion)), std::string::npos);
    EXPECT_GE(Project::kFormatVersion, 9);
    EXPECT_NE(json.find("\"neural\""), std::string::npos);
    const Project back = projectFromJsonString(json);
    const Clip& b = back.sequences[0].tracks[0].clips[0];
    EXPECT_EQ(b.speed.interpolation, FrameInterpolation::Neural);
    EXPECT_EQ(b.upscale, c.upscale);
    // Lanczos alone (no model fields that matter) round-trips too
    Project lz = make();
    lz.sequences[0].tracks[0].clips[0].upscale.mode = UpscaleMode::Lanczos;
    EXPECT_EQ(projectFromJsonString(projectToJsonString(lz)).sequences[0].tracks[0].clips[0].upscale.mode, UpscaleMode::Lanczos);
    // a v8 file: no "upscale", interpolation as before - loads with both off
    std::string v8 = projectToJsonString(make());
    const std::string current = "\"version\": " + std::to_string(Project::kFormatVersion);
    v8.replace(v8.find(current), current.size(), "\"version\": 8");
    EXPECT_EQ(v8.find("upscale"), std::string::npos);
    const Project old = projectFromJsonString(v8);
    EXPECT_TRUE(old.sequences[0].tracks[0].clips[0].upscale.isDefault());
    EXPECT_EQ(old.sequences[0].tracks[0].clips[0].speed.interpolation, FrameInterpolation::Sampling);
    // refused: an unknown model, a factor of 5, 3x with the general model, flicker reduction above 1, an unknown mode
    for (int k = 0; k < 4; ++k) {
        Project bad = p;
        ClipUpscale& u = bad.sequences[0].tracks[0].clips[0].upscale;
        if (k == 0) u.model = "esrgan-ultra";
        if (k == 1) u.scale = 5;
        if (k == 2) u.model = "general";
        if (k == 3) u.deflicker = 1.5;
        EXPECT_TRUE(hasErrors(validate(bad))) << k;
    }
    std::string badMode = json;
    badMode.replace(badMode.find("\"mode\": \"ai\""), 12, "\"mode\": \"xx\"");
    EXPECT_THROW(projectFromJsonString(badMode), std::exception);
}
