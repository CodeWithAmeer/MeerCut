// M4 LUTs: .cube parsing and validation, tetrahedral / 1-D interpolation, the two domain conventions, caching,
// built-in looks, the LUT effect (strength, missing files) and agreement with Filmora (EXP-012) and FFmpeg's lut3d.
#include "fa/render/backend.h"
#include "fa/render/effects.h"
#include "fa/render/lut.h"
#include "fa/render/renderer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <thread>

using namespace fa;

namespace {

std::filesystem::path lutFile(const char* name) { return std::filesystem::path(FA_TESTMEDIA_DIR) / "luts" / name; }

std::shared_ptr<const Lut3D> parse(const std::string& text, std::string* err = nullptr) {
    std::string e;
    auto lut = parseCube(text, &e);
    if (err) *err = e;
    return lut;
}

std::string cube3(int n, const std::function<void(float, float, float, float&, float&, float&)>& f, const std::string& header = {}) {
    std::ostringstream s;
    s << header << "LUT_3D_SIZE " << n << "\n";
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                float x, y, z;
                f(r / float(n - 1), g / float(n - 1), b / float(n - 1), x, y, z);
                s << x << " " << y << " " << z << "\n";
            }
    return s.str();
}

std::array<float, 3> apply(const Lut3D& lut, float r, float g, float b, LutDomainMode m = LutDomainMode::Standard) {
    applyLut(lut, m, r, g, b);
    return {r, g, b};
}

// Independent tetrahedral reference: barycentric weights from the sorted fractions.
std::array<float, 3> tetraRef(const Lut3D& lut, float r, float g, float b) {
    const int n = lut.size;
    float p[3] = {r * (n - 1), g * (n - 1), b * (n - 1)};
    int i[3];
    float f[3];
    for (int c = 0; c < 3; ++c) {
        i[c] = std::min(n - 2, int(p[c]));
        f[c] = p[c] - i[c];
    }
    int order[3] = {0, 1, 2};   // axes by decreasing fraction
    std::sort(order, order + 3, [&](int a, int bb) { return f[a] > f[bb]; });
    auto at = [&](int dr, int dg, int db) { return &lut.table[((size_t(i[2] + db) * n + i[1] + dg) * n + i[0] + dr) * 3]; };
    int d[3] = {0, 0, 0};
    std::array<float, 3> out{};
    float prev = 1.0f;
    const float* c = at(0, 0, 0);
    for (int k = 0; k <= 3; ++k) {
        const float fk = k < 3 ? f[order[k]] : 0.0f;
        for (int ch = 0; ch < 3; ++ch) out[ch] += (prev - fk) * c[ch];
        if (k == 3) break;
        d[order[k]] = 1;
        c = at(d[0], d[1], d[2]);
        prev = fk;
    }
    return out;
}

}  // namespace

TEST(LutParse, ReadsTheStage1TestLuts) {
    std::string err;
    auto id = loadCube(lutFile("lut_identity_33.cube"), &err);
    ASSERT_TRUE(id) << err;
    EXPECT_EQ(id->size, 33);
    EXPECT_EQ(id->title, "identity33");
    EXPECT_TRUE(id->isIdentity());
    auto half = loadCube(lutFile("lut_domain_half_2.cube"), &err);
    ASSERT_TRUE(half) << err;
    EXPECT_FLOAT_EQ(half->domainMax[0], 0.5f);
    EXPECT_FALSE(half->isIdentity());
    auto rnd = loadCube(lutFile("lut_random_3.cube"), &err);
    ASSERT_TRUE(rnd) << err;
    EXPECT_EQ(rnd->table.size(), size_t(27 * 3));
}

TEST(LutParse, ValidationNamesTheProblemAndLine) {
    std::string err;
    EXPECT_FALSE(parse("", &err));
    EXPECT_NE(err.find("no LUT_3D_SIZE"), std::string::npos) << err;
    EXPECT_FALSE(parse("LUT_3D_SIZE 2\n0 0 0\n1 0 0\n", &err));
    EXPECT_NE(err.find("expected 8 table entries, found 2"), std::string::npos) << err;
    EXPECT_NE(err.find("line 2"), std::string::npos) << err;
    EXPECT_FALSE(parse("LUT_3D_SIZE 2\n0 0 0\n1 0 x\n", &err));
    EXPECT_NE(err.find("line 3"), std::string::npos) << err;
    EXPECT_FALSE(parse("LUT_3D_SIZE 1\n", &err));
    EXPECT_NE(err.find("outside 2..129"), std::string::npos) << err;
    EXPECT_FALSE(parse("LUT_3D_SIZE 300\n", &err));
    EXPECT_FALSE(parse("LUT_3D_SIZE 2.5\n", &err));
    EXPECT_FALSE(parse("LUT_3D_SIZE 2\n0 0 0\nTITLE \"late\"\n", &err));
    EXPECT_NE(err.find("keyword after the table data"), std::string::npos) << err;
    EXPECT_FALSE(parse(cube3(2, [](float r, float g, float b, float& x, float& y, float& z) { x = r, y = g, z = b; },
                             "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 0 1 1\n"), &err));
    EXPECT_NE(err.find("DOMAIN_MAX must be greater"), std::string::npos) << err;
    EXPECT_FALSE(parse("LUT_IN_VIDEO_RANGE\nLUT_3D_SIZE 2\n", &err));
    EXPECT_NE(err.find("not supported"), std::string::npos) << err;
    EXPECT_FALSE(parse("LUT_3D_SIZE 2\n0 0 nan\n", &err));
    // accepted: BOM, CRLF, comments, blank lines, '+' signs, exponents, values outside 0..1 (clamped when applied)
    auto ok = parse("\xEF\xBB\xBF# comment\r\nTITLE \"x\"\r\n\r\nLUT_3D_SIZE 2\r\n0 0 0\r\n+1 0 0\r\n0 1e0 0\r\n1 1 0\r\n"
                    "0 0 1\r\n1 0 1\r\n0 1 1\r\n1.5 1 -0.5\r\n", &err);
    ASSERT_TRUE(ok) << err;
    EXPECT_EQ(apply(*ok, 1, 1, 1), (std::array<float, 3>{1, 1, 0}));
    EXPECT_FALSE(loadCube(std::filesystem::path(FA_TESTMEDIA_DIR) / "luts" / "does_not_exist.cube", &err));
    EXPECT_EQ(err, "file not found");
}

TEST(LutApply, TetrahedralMatchesAnIndependentReferenceAndIsExactOnLinearTables) {
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(0, 1);
    // an affine colour matrix is reproduced exactly between the grid points
    auto affine = parse(cube3(5, [](float r, float g, float b, float& x, float& y, float& z) {
        x = 0.1f + 0.5f * r + 0.2f * g, y = 0.3f * r + 0.4f * b, z = 0.2f + 0.3f * g + 0.3f * b;
    }));
    ASSERT_TRUE(affine);
    for (int k = 0; k < 200; ++k) {
        const float r = u(rng), g = u(rng), b = u(rng);
        const auto o = apply(*affine, r, g, b);
        EXPECT_NEAR(o[0], 0.1f + 0.5f * r + 0.2f * g, 1e-5f);
        EXPECT_NEAR(o[1], 0.3f * r + 0.4f * b, 1e-5f);
        EXPECT_NEAR(o[2], 0.2f + 0.3f * g + 0.3f * b, 1e-5f);
    }
    std::string err;
    auto rnd = loadCube(lutFile("lut_random_3.cube"), &err);
    ASSERT_TRUE(rnd) << err;
    for (int k = 0; k < 500; ++k) {
        const float r = u(rng), g = u(rng), b = u(rng);
        const auto o = apply(*rnd, r, g, b), ref = tetraRef(*rnd, r, g, b);
        for (int c = 0; c < 3; ++c) EXPECT_NEAR(o[c], std::clamp(ref[c], 0.0f, 1.0f), 1e-5f);
    }
    // grid points return the table entries
    EXPECT_NEAR(apply(*rnd, 0.5f, 1.0f, 0.0f)[0], std::clamp(rnd->table[((0 * 3 + 2) * 3 + 1) * 3], 0.0f, 1.0f), 1e-6f);
}

TEST(LutApply, OneDimensionalAndShaperTables) {
    auto inv = parse("LUT_1D_SIZE 3\n1 1 1\n0.5 0.5 0.5\n0 0 0\n");
    ASSERT_TRUE(inv);
    const auto o = apply(*inv, 0.25f, 0.75f, 1.0f);
    EXPECT_NEAR(o[0], 0.75f, 1e-6f);
    EXPECT_NEAR(o[1], 0.25f, 1e-6f);
    EXPECT_NEAR(o[2], 0.0f, 1e-6f);
    // 1-D shaper (square) then a 3-D identity
    std::ostringstream s;
    s << "LUT_1D_SIZE 1024\nLUT_3D_SIZE 2\n";
    for (int i = 0; i < 1024; ++i) {
        const float x = i / 1023.0f;
        s << x * x << " " << x * x << " " << x * x << "\n";
    }
    for (int b = 0; b < 2; ++b)
        for (int g = 0; g < 2; ++g)
            for (int r = 0; r < 2; ++r) s << r << " " << g << " " << b << "\n";
    auto shaper = parse(s.str());
    ASSERT_TRUE(shaper);
    EXPECT_NEAR(apply(*shaper, 0.5f, 0.5f, 0.5f)[0], 0.25f, 1e-4f);
}

TEST(LutApply, DomainConventions) {
    std::string err;
    auto half = loadCube(lutFile("lut_domain_half_2.cube"), &err);   // identity table, DOMAIN_MAX 0.5
    ASSERT_TRUE(half) << err;
    // .cube specification: the input 0..0.5 spans the table -> min(1, 2x)
    EXPECT_NEAR(apply(*half, 0.2f, 0.4f, 0.8f)[0], 0.4f, 1e-6f);
    EXPECT_NEAR(apply(*half, 0.2f, 0.4f, 0.8f)[2], 1.0f, 1e-6f);
    // Filmora (EXP-012): the table output is scaled into the domain -> x / 2
    const auto f = apply(*half, 0.2f, 0.4f, 0.8f, LutDomainMode::FilmoraCompatible);
    EXPECT_NEAR(f[0], 0.1f, 1e-6f);
    EXPECT_NEAR(f[2], 0.4f, 1e-6f);
    // for the usual 0..1 domain both conventions are the same
    auto rnd = loadCube(lutFile("lut_random_3.cube"), &err);
    EXPECT_EQ(apply(*rnd, 0.3f, 0.6f, 0.9f), apply(*rnd, 0.3f, 0.6f, 0.9f, LutDomainMode::FilmoraCompatible));
}

TEST(LutApply, AgreesWithFilmoraAndFfmpegOnTheMeasuredChart) {
    // EXP-012 per patch (tools/export_lut_patches.py), our LUT applied to the exact source colours:
    //  * Filmora's H.264 exports carry their own error (no-LUT export vs source, "baseline" ~1.38 levels); our result
    //    may add at most 0.35 levels to it (Stage 1: tetrahedral model 1.48-1.65)
    //  * FFmpeg 9's lut3d (tetrahedral) on the same PNG truncates its 8-bit output instead of rounding: its value
    //    always lies in the integer bracket [floor, ceil] of our float result (mean bias about -0.5 levels)
    const std::filesystem::path dir = std::filesystem::path(FA_REPO_ROOT) / "experiments" / "EXP-012_lut";
    struct Case {
        const char* name;
        LutDomainMode mode;
    };
    for (const Case& c : {Case{"lut_identity_33", LutDomainMode::Standard}, Case{"lut_invert_2", LutDomainMode::Standard},
                          Case{"lut_random_3", LutDomainMode::Standard}, Case{"lut_domain_half_2", LutDomainMode::FilmoraCompatible}}) {
        std::ifstream in(dir / (std::string(c.name) + ".patches.csv"));
        if (!in) GTEST_SKIP() << "Stage 1 evidence not present";
        std::string err;
        auto lut = loadCube(lutFile((std::string(c.name) + ".cube").c_str()), &err);
        ASSERT_TRUE(lut) << err;
        std::string line;
        std::getline(in, line);
        double seFilmora = 0, seBase = 0, biasFfmpeg = 0;
        int outsideBracket = 0;
        int n = 0;
        while (std::getline(in, line)) {
            std::stringstream ss(line);
            std::string cell;
            std::vector<double> v;
            while (std::getline(ss, cell, ',')) v.push_back(std::stod(cell));
            const auto f = apply(*lut, float(v[9] / 255), float(v[10] / 255), float(v[11] / 255), c.mode);
            const auto s = apply(*lut, float(v[9] / 255), float(v[10] / 255), float(v[11] / 255));
            for (int k = 0; k < 3; ++k) {
                seFilmora += std::pow(f[k] * 255 - v[3 + k], 2);
                seBase += std::pow(v[k] - v[9 + k], 2);
                const double ours = s[k] * 255.0, ff = v[6 + k];   // patch means of a flat patch: integers
                if (ff < std::floor(ours - 0.02) || ff > std::ceil(ours + 0.02)) ++outsideBracket;
                biasFfmpeg += ff - ours;
            }
            ++n;
        }
        const double rf = std::sqrt(seFilmora / (3.0 * n)), rb = std::sqrt(seBase / (3.0 * n));
        const double bias = biasFfmpeg / (3.0 * n);
        std::printf("  %-18s vs Filmora %.2f levels RMS (export baseline %.2f); FFmpeg lut3d: %d of %d outside our bracket, "
                    "bias %+.2f\n", c.name, rf, rb, outsideBracket, 3 * n, bias);
        EXPECT_LT(rf, rb + 0.35) << c.name;
        // FFmpeg ignores a DOMAIN narrower than 0..1 (its input scale is clipped to <= 1): not comparable there
        if (std::string(c.name) == "lut_domain_half_2") continue;
        EXPECT_EQ(outsideBracket, 0) << c.name;
    }
}

TEST(LutCache, SharesTablesAndReloadsChangedFiles) {
    const auto dir = std::filesystem::temp_directory_path() / "meercut_lut_test";
    std::filesystem::create_directories(dir);
    const auto file = dir / "x.cube";
    auto write = [&](float v) {
        std::ofstream o(file, std::ios::trunc);
        o << cube3(2, [v](float r, float g, float b, float& x, float& y, float& z) { x = r * v, y = g, z = b; });
    };
    write(1.0f);
    std::string err;
    auto a = cachedLut(file, &err), b = cachedLut(file, &err);
    ASSERT_TRUE(a) << err;
    EXPECT_EQ(a.get(), b.get());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    write(0.5f);
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(2));
    auto c = cachedLut(file, &err);
    ASSERT_TRUE(c) << err;
    EXPECT_NE(a.get(), c.get());
    EXPECT_NEAR(apply(*c, 1, 0, 0)[0], 0.5f, 1e-6f);
    // references: built-in looks, paths relative to the project folder, missing files
    EXPECT_TRUE(resolveLut("x.cube", dir, &err));
    EXPECT_FALSE(resolveLut("missing.cube", dir, &err));
    EXPECT_EQ(err, "file not found");
    EXPECT_FALSE(resolveLut("builtin:nope", dir, &err));
    std::filesystem::remove_all(dir);
}

TEST(LutBuiltin, LooksAreOriginalFormulasThatChangeThePicture) {
    ASSERT_GE(builtinLooks().size(), size_t(8));
    for (const auto& look : builtinLooks()) {
        auto lut = builtinLut(look.id);
        ASSERT_TRUE(lut) << look.id;
        EXPECT_EQ(lut->size, 33);
        EXPECT_FALSE(lut->isIdentity(1e-3f)) << look.id;
        for (float v : lut->table) ASSERT_TRUE(v >= 0.0f && v <= 1.0f) << look.id;
        EXPECT_EQ(builtinLut(look.id).get(), lut.get());   // baked once
    }
    const auto m = apply(*builtinLut("mono"), 0.9f, 0.2f, 0.1f);
    EXPECT_NEAR(m[0], m[1], 1e-5f);
    EXPECT_NEAR(m[1], m[2], 1e-5f);
    EXPECT_FALSE(builtinLut("does-not-exist"));
}

// M11: 32 more looks, in the Filters page's groups; every look is its own (no two bake to nearly the same table)
TEST(LutBuiltin, M11LooksAreGroupedAndDistinct) {
    EXPECT_EQ(builtinLooks().size(), 43u);
    const auto& cats = builtinLookCategories();
    std::set<std::string> ids, names;
    std::map<std::string, int> perCategory;
    for (const auto& l : builtinLooks()) {
        EXPECT_TRUE(ids.insert(l.id).second) << l.id;
        EXPECT_TRUE(names.insert(l.name).second) << l.name;
        EXPECT_FALSE(l.description.empty()) << l.id;
        EXPECT_NE(std::find(cats.begin(), cats.end(), l.category), cats.end()) << l.id << ": " << l.category;
        ++perCategory[l.category];
    }
    for (const auto& c : cats) EXPECT_GE(perCategory[c], 4) << c;   // every group is worth opening
    const auto& looks = builtinLooks();
    for (size_t i = 0; i < looks.size(); ++i)
        for (size_t j = i + 1; j < looks.size(); ++j) {   // at least 2.5 levels apart on average over the whole cube
            const auto a = builtinLut(looks[i].id), b = builtinLut(looks[j].id);
            double sum = 0;
            for (size_t k = 0; k < a->table.size(); ++k) sum += std::fabs(a->table[k] - b->table[k]);
            EXPECT_GT(sum / double(a->table.size()), 0.01) << looks[i].id << " vs " << looks[j].id;
        }
    for (const char* id : {"noir", "silver", "high-key"}) {   // neutral black and white
        const auto v = apply(*builtinLut(id), 0.8f, 0.3f, 0.1f);
        EXPECT_NEAR(v[0], v[1], 1e-5f) << id;
        EXPECT_NEAR(v[1], v[2], 1e-5f) << id;
    }
    const auto warmTone = apply(*builtinLut("warm-bw"), 0.5f, 0.5f, 0.5f), coolTone = apply(*builtinLut("selenium"), 0.5f, 0.5f, 0.5f);
    EXPECT_GT(warmTone[0], warmTone[2] + 0.03f);
    EXPECT_GT(coolTone[2], coolTone[0] + 0.03f);
}

// ---- the LUT effect ------------------------------------------------------------------------------------------

namespace {
EffectRef lutEffect(const std::string& ref, double strength = 100, double domain = 0) {
    EffectRef e = makeEffect(*findEffectDef("fa.color.lut"), "fx1");
    e.params["strength"] = Animatable<double>(strength);
    e.params["domain"] = Animatable<double>(domain);
    e.strings["lut"] = ref;
    return e;
}

Image runStack(const Image& src, const std::vector<EffectRef>& stack, const std::filesystem::path& dir = {},
               std::vector<std::string>* problems = nullptr) {
    const auto inst = evaluateEffects(stack, Time(0), 0.0, problems, dir);
    Image out;
    applyEffectsCpu(src, inst, out);
    return out;
}

class SolidLutProvider final : public FrameProvider {
public:
    explicit SolidLutProvider(std::filesystem::path dir) : dir_(std::move(dir)) {}
    Result frame(const Clip&, const MediaSource&, const Time& t) override {
        img_.reset(64, 36, Rgba8{200, 100, 50, 255});
        Result r;
        r.image = &img_;
        r.requestedTime = r.frameTime = t;
        return r;
    }
    std::filesystem::path assetDir() const override { return dir_; }

private:
    std::filesystem::path dir_;
    Image img_;
};
}  // namespace

TEST(LutEffect, StrengthReferencesAndMissingFiles) {
    const Image src(8, 8, Rgba8{40, 120, 200, 255});
    const std::string invert = lutFile("lut_invert_2.cube").string();
    // full strength inverts, 50 % lands half way, 0 % and an empty reference leave the picture alone
    EXPECT_EQ(runStack(src, {lutEffect(invert)}).at(3, 3), (Rgba8{215, 135, 55, 255}));
    const Rgba8 half = runStack(src, {lutEffect(invert, 50)}).at(3, 3);
    EXPECT_NEAR(half.r, (40 + 215) / 2.0, 1);
    EXPECT_NEAR(half.b, (200 + 55) / 2.0, 1);
    EXPECT_TRUE(effectsAreIdentity(evaluateEffects({lutEffect(invert, 0)}, Time(0), 0.0)));
    std::vector<std::string> problems;
    EXPECT_TRUE(effectsAreIdentity(evaluateEffects({lutEffect("")}, Time(0), 0.0, &problems)));
    EXPECT_TRUE(problems.empty());
    // a missing file: the effect is skipped (picture unchanged) and the problem names the reference
    const auto inst = evaluateEffects({lutEffect("C:/nowhere/look.cube")}, Time(0), 0.0, &problems);
    ASSERT_EQ(inst.size(), size_t(1));
    EXPECT_NE(inst[0].problem.find("look.cube"), std::string::npos) << inst[0].problem;
    EXPECT_NE(inst[0].problem.find("file not found"), std::string::npos) << inst[0].problem;
    EXPECT_EQ(runStack(src, {lutEffect("C:/nowhere/look.cube")}).at(3, 3), src.at(3, 3));
    // a reference relative to the project folder; a built-in look
    EXPECT_EQ(runStack(src, {lutEffect("lut_invert_2.cube")}, lutFile("x").parent_path()).at(3, 3), (Rgba8{215, 135, 55, 255}));
    EXPECT_NE(runStack(src, {lutEffect("builtin:sepia")}).at(3, 3), src.at(3, 3));
    // a malformed file reports the parser's message
    const auto bad = std::filesystem::temp_directory_path() / "meercut_bad.cube";
    {
        std::ofstream o(bad);
        o << "LUT_3D_SIZE 2\n0 0 0\n";
    }
    const auto badInst = evaluateEffects({lutEffect(bad.string())}, Time(0), 0.0);
    EXPECT_NE(badInst[0].problem.find("expected 8 table entries"), std::string::npos) << badInst[0].problem;
    std::filesystem::remove(bad);
}

TEST(LutEffect, RendererResolvesAgainstTheProjectFolderAndReportsMissingLuts) {
    Project p;
    MediaSource src;
    src.id = "src";
    src.path = "solid.mp4";
    src.duration = Rational(10);
    src.width = 64;
    src.height = 36;
    p.sources.push_back(src);
    Sequence s;
    s.id = "seq";
    s.width = 64;
    s.height = 36;
    Track v1{"v1", TrackKind::Video, "Video 1"};
    Clip c;
    c.id = "c";
    c.sourceId = "src";
    c.duration = Rational(10);
    c.effects.push_back(lutEffect("lut_invert_2.cube"));   // relative: next to the project file
    v1.clips.push_back(c);
    s.tracks = {v1};
    p.sequences.push_back(s);
    auto be = makeCpuBackend();
    std::vector<std::string> problems;
    SolidLutProvider inLutFolder(lutFile("x").parent_path());
    EXPECT_EQ(renderFrame(p.sequences[0], 0, p, inLutFolder, *be, nullptr, nullptr, 0, 0, nullptr, &problems).at(10, 10),
              (Rgba8{55, 155, 205, 255}));
    EXPECT_TRUE(problems.empty());
    SolidLutProvider elsewhere(std::filesystem::temp_directory_path());
    EXPECT_EQ(renderFrame(p.sequences[0], 0, p, elsewhere, *be, nullptr, nullptr, 0, 0, nullptr, &problems).at(10, 10),
              (Rgba8{200, 100, 50, 255}));
    ASSERT_FALSE(problems.empty());
    EXPECT_NE(problems[0].find("lut_invert_2.cube"), std::string::npos) << problems[0];
}
