// M4 checkpoint E: the effects library (blur, light, stylize, film, glitch, lens, distortion, keying, filters).
// Each effect is checked for what it is meant to do on constructed pictures; the chroma key against Filmora's
// measured key (Stage 1 EXP-005).
#include "fa/render/effects.h"
#include "fa/render/lut.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace fa;

namespace {

EffectRef fx(const std::string& type, std::map<std::string, double> values = {}) {
    const EffectDef* def = findEffectDef(type);
    EXPECT_NE(def, nullptr) << type;
    EffectRef e = makeEffect(*def, "fx1");
    for (const auto& [k, v] : values) e.params[k] = Animatable<double>(v);
    return e;
}

Image apply(const Image& src, const EffectRef& e, double seconds = 0.0) {
    std::vector<std::string> problems;
    const auto inst = evaluateEffects({e}, Time(0), seconds, &problems);
    EXPECT_TRUE(problems.empty());
    Image out;
    applyEffectsCpu(src, inst, out);
    return out;
}

double meanChannel(const Image& img, int c) {
    double s = 0;
    for (size_t i = static_cast<size_t>(c); i < img.pixels.size(); i += 4) s += img.pixels[i];
    return s / (img.pixels.size() / 4.0);
}

// 1080-line picture with a vertical step (left dark, right light) or a horizontal step
Image step(bool vertical, int w = 200, int h = 1080) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool light = vertical ? x >= w / 2 : y >= h / 2;
            img.set(x, y, light ? Rgba8{200, 200, 200, 255} : Rgba8{40, 40, 40, 255});
        }
    return img;
}

}  // namespace

TEST(EffectsLibrary, SchemasAndCatalogue) {
    for (const char* id : {"fa.fx.blur.directional", "fa.fx.glow", "fa.fx.pixelate", "fa.fx.grain", "fa.fx.rgbsplit", "fa.fx.edge",
                           "fa.fx.chromatic", "fa.fx.wave", "fa.fx.bulge", "fa.fx.chromakey", "fa.filter.bw", "fa.filter.sepia",
                           "fa.filter.look"}) {
        const EffectDef* d = findEffectDef(id);
        ASSERT_NE(d, nullptr) << id;
        EXPECT_TRUE(effectHasCpuImplementation(id)) << id;
        EXPECT_FALSE(d->category.empty()) << id;
        EXPECT_FALSE(d->description.empty()) << id;
    }
}

TEST(EffectsLibrary, DirectionalBlurBlursAlongItsAngleOnly) {
    const Image v = step(true), h = step(false);
    const Image hv = apply(v, fx("fa.fx.blur.directional", {{"angle", 0}, {"length", 40}}));
    EXPECT_GT(hv.at(98, 540).g, 60);    // a vertical edge is smeared horizontally
    EXPECT_LT(hv.at(101, 540).g, 180);
    EXPECT_EQ(hv.at(10, 540).g, 40);
    const Image hh = apply(h, fx("fa.fx.blur.directional", {{"angle", 0}, {"length", 40}}));
    EXPECT_EQ(hh.at(100, 539).g, 40);   // ... but a horizontal edge is untouched
    EXPECT_EQ(hh.at(100, 540).g, 200);
    const Image vh = apply(h, fx("fa.fx.blur.directional", {{"angle", 90}, {"length", 40}}));
    EXPECT_GT(vh.at(100, 535).g, 60);
    EXPECT_NEAR(meanChannel(hv, 1), meanChannel(v, 1), 0.6);   // energy kept
}

TEST(EffectsLibrary, GlowBloomsOnlyBrightAreas) {
    Image img(200, 1080, Rgba8{30, 30, 30, 255});
    for (int y = 520; y < 560; ++y)
        for (int x = 80; x < 120; ++x) img.set(x, y, Rgba8{255, 255, 255, 255});
    const Image g = apply(img, fx("fa.fx.glow", {{"threshold", 60}, {"radius", 40}, {"intensity", 150}}));
    EXPECT_GT(g.at(70, 540).g, 45);   // light spills around the bright square
    EXPECT_EQ(g.at(5, 100).g, 30);    // far away nothing changes
    const Image dark = apply(Image(64, 64, Rgba8{120, 120, 120, 255}), fx("fa.fx.glow", {{"threshold", 60}}));
    EXPECT_EQ(dark.at(32, 32).g, 120);   // below the threshold: no glow
}

TEST(EffectsLibrary, PixelateAveragesBlocks) {
    Image img(64, 1080);
    for (int y = 0; y < 1080; ++y)
        for (int x = 0; x < 64; ++x) img.set(x, y, Rgba8{uint8_t(x * 4), uint8_t(y % 256), 0, 255});
    const Image p = apply(img, fx("fa.fx.pixelate", {{"size", 16}}));
    for (int bx = 0; bx < 4; ++bx) {
        const Rgba8 a = p.at(bx * 16, 0), b = p.at(bx * 16 + 15, 15);
        EXPECT_EQ(a, b);
        EXPECT_NEAR(a.r, bx * 64 + 30, 1);   // mean of x*4 over the block
    }
}

TEST(EffectsLibrary, GrainIsAnimatedDeterministicAndMeanNeutral) {
    const Image grey(256, 256, Rgba8{128, 128, 128, 255});
    const EffectRef e = fx("fa.fx.grain", {{"amount", 60}});
    const Image a = apply(grey, e, 1.0), b = apply(grey, e, 1.0), c = apply(grey, e, 1.0333);
    EXPECT_EQ(a.pixels, b.pixels);   // same frame, same grain
    EXPECT_NE(a.pixels, c.pixels);   // next frame, new grain
    EXPECT_NEAR(meanChannel(a, 1), 128, 1.0);
    int differ = 0;
    for (int i = 0; i < 1000; ++i) {
        const Rgba8 p = a.at(i % 256, i / 256);
        differ += p.g != 128;
        EXPECT_EQ(p.r, p.g);   // monochrome grain
    }
    EXPECT_GT(differ, 600);
    const Image col = apply(grey, fx("fa.fx.grain", {{"amount", 60}, {"color", 1}}), 1.0);
    int chroma = 0;
    for (int i = 0; i < 1000; ++i) chroma += col.at(i % 256, i / 256).r != col.at(i % 256, i / 256).g;
    EXPECT_GT(chroma, 500);
}

TEST(EffectsLibrary, RgbSplitShiftsRedAndBlue) {
    Image img(400, 1080, Rgba8{0, 0, 0, 255});
    for (int y = 0; y < 1080; ++y) img.set(200, y, Rgba8{255, 255, 255, 255});   // a white vertical line
    const Image s = apply(img, fx("fa.fx.rgbsplit", {{"offset", 20}, {"angle", 0}}));
    EXPECT_EQ(s.at(220, 540), (Rgba8{255, 0, 0, 255}));   // red comes from 20 px to the left
    EXPECT_EQ(s.at(180, 540), (Rgba8{0, 0, 255, 255}));   // blue from 20 px to the right
    EXPECT_EQ(s.at(200, 540), (Rgba8{0, 255, 0, 255}));
}

TEST(EffectsLibrary, EdgeDetectModes) {
    const Image flat(64, 64, Rgba8{90, 120, 60, 255});
    EXPECT_EQ(apply(flat, fx("fa.fx.edge")).at(32, 32), (Rgba8{0, 0, 0, 255}));
    EXPECT_EQ(apply(flat, fx("fa.fx.edge", {{"mode", 1}})).at(32, 32), (Rgba8{255, 255, 255, 255}));
    const Image e = apply(step(true), fx("fa.fx.edge"));
    EXPECT_NEAR(e.at(100, 540).g, 160, 2);   // the edge's brightness is the step height (40 -> 200)
    EXPECT_EQ(e.at(20, 540).g, 0);
}

TEST(EffectsLibrary, ChromaticAberrationGrowsTowardsTheEdges) {
    Image img(400, 400, Rgba8{0, 0, 0, 255});
    for (int y = 0; y < 400; ++y) {
        img.set(200, y, Rgba8{255, 255, 255, 255});
        img.set(380, y, Rgba8{255, 255, 255, 255});
    }
    const Image c = apply(img, fx("fa.fx.chromatic", {{"amount", 100}}));
    const Rgba8 mid = c.at(200, 200);   // centre: (almost) no fringe
    EXPECT_GE(std::min({mid.r, mid.g, mid.b}), 250);
    // near the border (180 px out, 2 % scale) the line's red copy lands ~4 px further out, the blue one ~4 px in
    EXPECT_EQ(c.at(380, 200).g, 255);
    EXPECT_GT(c.at(384, 200).r, 150);
    EXPECT_EQ(c.at(384, 200).g, 0);
    EXPECT_GT(c.at(376, 200).b, 100);
    EXPECT_EQ(c.at(376, 200).r, 0);
}

TEST(EffectsLibrary, WaveDisplacesAndMoves) {
    Image img(400, 1080, Rgba8{0, 0, 0, 255});
    for (int y = 0; y < 1080; ++y)
        for (int x = 198; x < 202; ++x) img.set(x, y, Rgba8{255, 255, 255, 255});
    const EffectRef e = fx("fa.fx.wave", {{"amplitude", 40}, {"wavelength", 400}, {"speed", 1}});
    auto lineX = [](const Image& im, int y) {
        double s = 0, n = 0;
        for (int x = 0; x < im.width; ++x) s += x * im.at(x, y).g, n += im.at(x, y).g;
        return s / n + 0.5;
    };
    const Image a = apply(img, e, 0.0);
    EXPECT_NEAR(lineX(a, 100) - 200, -40 * std::sin(2 * 3.14159265 * 100.5 / 400), 1.5);   // samples from x + offset
    const Image b = apply(img, e, 0.25);   // a quarter cycle later the phase moved
    EXPECT_GT(std::fabs(lineX(b, 100) - lineX(a, 100)), 10);
}

TEST(EffectsLibrary, BulgeMagnifiesInsideItsRadiusOnly) {
    Image img(1080, 1080, Rgba8{0, 0, 0, 255});
    for (int y = 530; y < 550; ++y)
        for (int x = 530; x < 550; ++x) img.set(x, y, Rgba8{255, 255, 255, 255});   // a small centred square
    img.set(10, 10, Rgba8{255, 0, 0, 255});
    const Image b = apply(img, fx("fa.fx.bulge", {{"amount", 100}, {"radius", 30}}));
    EXPECT_GT(b.at(551, 540).g, 128);   // the square grew
    EXPECT_EQ(b.at(10, 10), (Rgba8{255, 0, 0, 255}));
    const Image p = apply(img, fx("fa.fx.bulge", {{"amount", -100}, {"radius", 30}}));
    EXPECT_LT(p.at(548, 540).g, 128);   // pinch shrinks it
}

TEST(EffectsLibrary, BlackAndWhiteAndSepia) {
    const Image c(4, 4, Rgba8{200, 100, 50, 255});
    const Rgba8 bw = apply(c, fx("fa.filter.bw")).at(1, 1);
    EXPECT_EQ(bw.r, bw.g);
    EXPECT_NEAR(bw.g, 0.2126 * 200 + 0.7152 * 100 + 0.0722 * 50, 1.5);   // default = Rec.709 luma
    const Rgba8 red = apply(c, fx("fa.filter.bw", {{"red", 100}, {"green", 0}, {"blue", 0}})).at(1, 1);
    EXPECT_EQ(red.g, 200);
    const Rgba8 s = apply(Image(4, 4, Rgba8{128, 128, 128, 255}), fx("fa.filter.sepia")).at(1, 1);
    EXPECT_GT(s.r, s.g);
    EXPECT_GT(s.g, s.b);   // warm brown mid-tones
    EXPECT_EQ(apply(Image(4, 4, Rgba8{255, 255, 255, 255}), fx("fa.filter.sepia")).at(1, 1), (Rgba8{255, 255, 255, 255}));
}

TEST(EffectsLibrary, LookFilterUsesTheBuiltInLut) {
    EffectRef look = fx("fa.filter.look");
    look.strings["lut"] = "builtin:mono";
    const Rgba8 m = apply(Image(4, 4, Rgba8{200, 60, 30, 255}), look).at(1, 1);
    EXPECT_EQ(m.r, m.g);
    EXPECT_EQ(m.g, m.b);
}

// ---- chroma key against Filmora's measured key (EXP-005) -----------------------------------------------------

TEST(ChromaKey, AlphaMatchesFilmorasMeasuredKey) {
    std::ifstream in(std::filesystem::path(FA_REPO_ROOT) / "experiments" / "EXP-005_chromakey" / "chroma_patches.csv");
    if (!in) GTEST_SKIP() << "Stage 1 evidence not present";
    std::string line;
    std::getline(in, line);
    struct Variant {
        int column;
        double tolerance, pedestal, maxRms;
    };
    // the per-pixel settings (feather / thickness act on edges, measured on patch interiors)
    const std::map<std::string, Variant> variants = {{"default", {4, 20, 30, 0.04}},
                                                     {"tolerance 0", {5, 0, 30, 0.04}},
                                                     {"tolerance 100", {6, 100, 30, 0.06}},
                                                     {"pedestal 0", {7, 20, 0, 0.04}}};
    std::vector<std::vector<std::string>> rows;
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string cell;
        std::vector<std::string> v;
        while (std::getline(ss, cell, ',')) v.push_back(cell);
        rows.push_back(v);
    }
    ASSERT_EQ(rows.size(), size_t(32));
    for (const auto& [name, var] : variants) {
        double se = 0;
        for (const auto& r : rows) {
            Image px(96, 96, Rgba8{uint8_t(std::stoi(r[1])), uint8_t(std::stoi(r[2])), uint8_t(std::stoi(r[3])), 255});
            const Image k = apply(px, fx("fa.fx.chromakey", {{"tolerance", var.tolerance}, {"pedestal", var.pedestal}}));
            const double a = k.at(48, 48).a / 255.0, m = std::stod(r[static_cast<size_t>(var.column)]);
            se += (a - m) * (a - m);
        }
        const double rms = std::sqrt(se / rows.size());
        std::printf("  chroma key %-14s alpha RMS %.3f vs Filmora (32 patches)\n", name.c_str(), rms);
        EXPECT_LT(rms, var.maxRms) << name;
    }
}

TEST(ChromaKey, SpillFeatherThicknessAndMatteView) {
    // spill: a green-tinted grey that stays opaque loses its green cast
    const Rgba8 tinted{120, 150, 120, 255};
    const Rgba8 noSpill = apply(Image(32, 32, tinted), fx("fa.fx.chromakey", {{"spill", 0}})).at(16, 16);
    const Rgba8 spilled = apply(Image(32, 32, tinted), fx("fa.fx.chromakey", {{"spill", 100}})).at(16, 16);
    EXPECT_GT(noSpill.g, noSpill.r + 20);
    EXPECT_LT(std::abs(int(spilled.g) - int(spilled.r)), std::abs(int(noSpill.g) - int(noSpill.r)) / 2);
    // a red square on green: hard matte without feather, soft with it; thickness erodes the kept area
    Image img(200, 1080, Rgba8{0, 177, 64, 255});
    for (int y = 440; y < 640; ++y)
        for (int x = 50; x < 150; ++x) img.set(x, y, Rgba8{220, 30, 30, 255});
    const Image hard = apply(img, fx("fa.fx.chromakey", {{"feather", 0}, {"thickness", 0}}));
    EXPECT_EQ(hard.at(100, 540).a, 255);
    EXPECT_EQ(hard.at(20, 540).a, 0);
    EXPECT_EQ(hard.at(150, 540).a, 0);
    EXPECT_EQ(hard.at(149, 540).a, 255);
    const Image soft = apply(img, fx("fa.fx.chromakey", {{"feather", 100}, {"thickness", 0}}));
    EXPECT_GT(soft.at(152, 540).a, 0);
    EXPECT_LT(soft.at(147, 540).a, 255);
    const Image eroded = apply(img, fx("fa.fx.chromakey", {{"feather", 0}, {"thickness", -10}}));
    EXPECT_EQ(eroded.at(147, 540).a, 0);   // 5 px eroded
    EXPECT_EQ(eroded.at(140, 540).a, 255);
    const Image view = apply(img, fx("fa.fx.chromakey", {{"alphaView", 1}, {"feather", 0}, {"thickness", 0}}));
    EXPECT_EQ(view.at(100, 540), (Rgba8{255, 255, 255, 255}));
    EXPECT_EQ(view.at(20, 540), (Rgba8{0, 0, 0, 255}));
}
