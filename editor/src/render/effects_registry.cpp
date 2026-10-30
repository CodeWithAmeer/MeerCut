// Effect types (schemas) and evaluation of an effect stack at a time.
#include "fa/render/effects.h"

#include "fa/render/curves.h"
#include "fa/render/lut.h"

#include <map>
#include <mutex>
#include <sstream>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fa {

namespace {

ParamDef slider(std::string id, std::string label, std::string group, double min = -100, double max = 100, double def = 0,
                std::string unit = "", int decimals = 0, std::string tooltip = {}) {
    ParamDef p;
    p.id = std::move(id);
    p.label = std::move(label);
    p.kind = ParamKind::Float;
    p.min = min;
    p.max = max;
    p.def = def;
    p.step = decimals > 0 ? std::pow(10.0, -decimals) : 1.0;
    p.unit = std::move(unit);
    p.decimals = decimals;
    p.group = std::move(group);
    p.tooltip = std::move(tooltip);
    return p;
}

ParamDef choiceParam(std::string id, std::string label, std::string group, std::vector<std::string> choices, int def = 0) {
    ParamDef p;
    p.id = std::move(id);
    p.label = std::move(label);
    p.kind = ParamKind::Choice;
    p.min = 0;
    p.max = static_cast<double>(choices.size()) - 1;
    p.def = def;
    p.choices = std::move(choices);
    p.animatable = false;
    p.group = std::move(group);
    return p;
}

ParamDef switchParam(std::string id, std::string label, std::string group, bool def = false, std::string tooltip = {}) {
    ParamDef p;
    p.id = std::move(id);
    p.label = std::move(label);
    p.kind = ParamKind::Bool;
    p.min = 0;
    p.max = 1;
    p.def = def ? 1 : 0;
    p.animatable = false;
    p.group = std::move(group);
    p.tooltip = std::move(tooltip);
    return p;
}

ParamDef colorParam(std::string id, std::string label, std::string group, uint32_t rgb) {
    ParamDef p;
    p.id = std::move(id);
    p.label = std::move(label);
    p.kind = ParamKind::Color;
    p.min = 0;
    p.max = 16777215;
    p.def = rgb;
    p.animatable = false;
    p.group = std::move(group);
    return p;
}

bool allZero(const EffectInstance& e) {
    for (float v : e.values)
        if (v != 0.0f) return false;
    return true;
}

std::vector<EffectDef> buildDefs() {
    std::vector<EffectDef> v;
    {
        EffectDef d;
        d.id = "fa.color.basic";
        d.label = "Color Adjust";
        d.section = EffectSection::Color;
        d.category = "Color";
        d.description = "White balance, light and colour. Exposure, brightness, contrast, highlights, temperature and tint "
                        "follow measured response curves.";
        d.params = {
            slider("temperature", "Temperature", "White Balance", -100, 100, 0, "", 0, "Warmer (+) or cooler (-)"),
            slider("tint", "Tint", "White Balance", -100, 100, 0, "", 0, "Magenta (+) or green (-)"),
            slider("exposure", "Exposure", "Light"),
            slider("brightness", "Brightness", "Light"),
            slider("contrast", "Contrast", "Light"),
            slider("highlights", "Highlights", "Light"),
            slider("shadows", "Shadows", "Light"),
            slider("whites", "Whites", "Light"),
            slider("blacks", "Blacks", "Light"),
            slider("gamma", "Gamma", "Light", -100, 100, 0, "", 0, "Mid-tone brightness (+ brighter), black and white fixed"),
            slider("saturation", "Saturation", "Color"),
            slider("vibrance", "Vibrance", "Color", -100, 100, 0, "", 0, "Saturation of the less saturated colours"),
        };
        d.isIdentity = allZero;
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.color.hsl";
        d.label = "HSL";
        d.section = EffectSection::Color;
        d.category = "Color";
        d.description = "Hue, saturation and luminance of 8 colour ranges with smooth transitions (band centres and shapes "
                        "fitted to measurements).";
        const char* bands[8][2] = {{"red", "Red"},   {"orange", "Orange"}, {"yellow", "Yellow"}, {"green", "Green"},
                                   {"aqua", "Aqua"}, {"blue", "Blue"},     {"purple", "Purple"}, {"magenta", "Magenta"}};
        for (const auto& b : bands) {
            d.params.push_back(slider(std::string(b[0]) + ".hue", "Hue", b[1]));
            d.params.push_back(slider(std::string(b[0]) + ".saturation", "Saturation", b[1]));
            d.params.push_back(slider(std::string(b[0]) + ".luminance", "Luminance", b[1]));
        }
        d.isIdentity = allZero;
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.color.curves";
        d.label = "Curves";
        d.section = EffectSection::Color;
        d.category = "Color";
        d.description = "Tone curves (master, red, green, blue) and hue curves (hue vs hue, hue vs saturation), smooth "
                        "through the points.";
        d.curves = {{"master", "Master", false, {{0, 0}, {1, 1}}}, {"red", "Red", false, {{0, 0}, {1, 1}}},
                    {"green", "Green", false, {{0, 0}, {1, 1}}},   {"blue", "Blue", false, {{0, 0}, {1, 1}}},
                    {"hueHue", "Hue vs Hue", true, {}},            {"hueSat", "Hue vs Saturation", true, {}}};
        d.isIdentity = [](const EffectInstance& e) { return !e.tables; };
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.color.wheels";
        d.label = "Color Wheels";
        d.section = EffectSection::Color;
        d.category = "Color";
        d.description = "Shadows, midtones and highlights tint and brightness (tonal ranges fitted to measurements).";
        for (const auto& [id, label] : {std::pair{"shadows", "Shadows"}, {"midtones", "Midtones"}, {"highlights", "Highlights"}}) {
            d.params.push_back(slider(std::string(id) + ".r", "Red", label, -1, 1, 0, "", 2));
            d.params.push_back(slider(std::string(id) + ".g", "Green", label, -1, 1, 0, "", 2));
            d.params.push_back(slider(std::string(id) + ".b", "Blue", label, -1, 1, 0, "", 2));
            d.params.push_back(slider(std::string(id) + ".lum", "Luminance", label, -1, 1, 0, "", 2));
        }
        d.isIdentity = allZero;
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.fx.vignette";
        d.label = "Vignette";
        d.section = EffectSection::Effects;
        d.category = "Stylize";
        d.description = "Darkens (or lightens) the edges of the picture.";
        d.params = {slider("amount", "Amount", "Vignette", -100, 100, -50, "", 0, "Negative darkens, positive lightens the edges"),
                    slider("size", "Size", "Vignette", 0, 100, 50, " %"),
                    slider("roundness", "Roundness", "Vignette", -100, 100, 0, "", 0, "-100 follows the frame, +100 is a circle"),
                    slider("feather", "Feather", "Vignette", 0, 100, 50, " %")};
        d.isIdentity = [](const EffectInstance& e) { return e.value("amount") == 0.0f; };
        d.neutral = {{"amount", 0.0}};
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.fx.sharpen";
        d.label = "Sharpen";
        d.section = EffectSection::Effects;
        d.category = "Detail";
        d.description = "Unsharp mask: enhances edges.";
        d.params = {slider("amount", "Amount", "Sharpen", 0, 300, 80, " %"),
                    slider("radius", "Radius", "Sharpen", 0.5, 20, 2, " px", 1, "Edge width in pixels at 1080p (Gaussian sigma)")};
        d.spatial = true;
        d.isIdentity = [](const EffectInstance& e) { return e.value("amount") <= 0.0f; };
        d.neutral = {{"amount", 0.0}};
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.color.lut";
        d.label = "LUT";
        d.section = EffectSection::Color;
        d.category = "Color";
        d.description = "3-D / 1-D colour lookup table (.cube) or a built-in look, with tetrahedral interpolation.";
        ParamDef domain;
        domain.id = "domain";
        domain.label = "Domain";
        domain.kind = ParamKind::Choice;
        domain.min = 0;
        domain.max = 1;
        domain.def = 0;
        domain.animatable = false;
        domain.choices = {"Standard (.cube)", "Output scale"};
        domain.tooltip = "How DOMAIN_MIN/MAX other than 0..1 are read: as the .cube specification says (input range), or "
                         "as an output scale, as some editors read them. Same result for ordinary LUTs.";
        d.params = {slider("strength", "Strength", "LUT", 0, 100, 100, " %"), domain};
        d.usesLut = true;
        d.isIdentity = [](const EffectInstance& e) { return !e.tables || !e.tables->lut || e.value("strength") <= 0.0f; };
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.fx.blur.directional";
        d.label = "Directional Blur";
        d.category = "Blur";
        d.description = "Motion-style blur along one direction.";
        d.params = {slider("angle", "Angle", "Blur", -180, 180, 0, "°"),
                    slider("length", "Length", "Blur", 0, 200, 30, " px", 0, "Blur length in pixels at 1080p")};
        d.spatial = true;
        d.isIdentity = [](const EffectInstance& e) { return e.value("length") <= 0.0f; };
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.fx.glow";
        d.label = "Glow";
        d.category = "Light";
        d.description = "Bright areas bloom: the parts above the threshold are blurred and screened back on.";
        d.params = {slider("threshold", "Threshold", "Glow", 0, 100, 60, " %"),
                    slider("radius", "Radius", "Glow", 1, 200, 30, " px", 0, "Glow spread in pixels at 1080p"),
                    slider("intensity", "Intensity", "Glow", 0, 300, 100, " %")};
        d.spatial = true;
        d.isIdentity = [](const EffectInstance& e) { return e.value("intensity") <= 0.0f; };
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.fx.pixelate";
        d.label = "Pixelate";
        d.category = "Stylize";
        d.description = "Mosaic of square blocks (each the average of its pixels).";
        d.params = {slider("size", "Block Size", "Pixelate", 2, 200, 24, " px", 0, "Block size in pixels at 1080p")};
        d.spatial = true;
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.fx.grain";
        d.label = "Film Grain";
        d.category = "Film";
        d.description = "Animated film grain, strongest in the mid-tones.";
        d.params = {slider("amount", "Amount", "Grain", 0, 100, 35, " %"),
                    slider("size", "Size", "Grain", 1, 8, 1.5, " px", 1, "Grain size in pixels at 1080p"),
                    switchParam("color", "Colour grain", "Grain", false, "Separate grain per colour channel")};
        d.timeVarying = true;
        d.isIdentity = [](const EffectInstance& e) { return e.value("amount") <= 0.0f; };
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.fx.rgbsplit";
        d.label = "RGB Split";
        d.category = "Glitch";
        d.description = "Shifts the red and blue channels apart.";
        d.params = {slider("offset", "Offset", "RGB Split", 0, 100, 12, " px", 0, "Shift in pixels at 1080p"),
                    slider("angle", "Angle", "RGB Split", -180, 180, 0, "°")};
        d.spatial = true;
        d.isIdentity = [](const EffectInstance& e) { return e.value("offset") <= 0.0f; };
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.fx.edge";
        d.label = "Edge Detect";
        d.category = "Stylize";
        d.description = "Outlines from the picture's edges (Sobel): white edges, a pencil sketch or coloured edges.";
        d.params = {choiceParam("mode", "Style", "Edges", {"Edges", "Pencil Sketch", "Coloured Edges"}),
                    slider("strength", "Strength", "Edges", 0, 300, 100, " %")};
        d.spatial = true;
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.fx.chromatic";
        d.label = "Chromatic Aberration";
        d.category = "Lens";
        d.description = "Lens colour fringing: red and blue scaled apart from the centre.";
        d.params = {slider("amount", "Amount", "Lens", 0, 100, 30, " %")};
        d.spatial = true;
        d.isIdentity = [](const EffectInstance& e) { return e.value("amount") <= 0.0f; };
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.fx.wave";
        d.label = "Wave";
        d.category = "Distortion";
        d.description = "Animated sine-wave displacement.";
        d.params = {slider("amplitude", "Amplitude", "Wave", 0, 200, 12, " px", 0, "Displacement in pixels at 1080p"),
                    slider("wavelength", "Wavelength", "Wave", 10, 1000, 160, " px"),
                    slider("speed", "Speed", "Wave", -10, 10, 1, " Hz", 1, "Wave cycles per second"),
                    choiceParam("direction", "Direction", "Wave", {"Horizontal", "Vertical"})};
        d.spatial = true;
        d.timeVarying = true;
        d.isIdentity = [](const EffectInstance& e) { return e.value("amplitude") <= 0.0f; };
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.fx.bulge";
        d.label = "Bulge / Pinch";
        d.category = "Distortion";
        d.description = "Magnifies (bulge) or shrinks (pinch) a round area.";
        d.params = {slider("amount", "Amount", "Bulge", -100, 100, 50, "", 0, "Positive bulges, negative pinches"),
                    slider("radius", "Radius", "Bulge", 1, 100, 40, " %", 0, "Radius in % of the picture height"),
                    slider("centerX", "Center X", "Bulge", -100, 100, 0, " %"),
                    slider("centerY", "Center Y", "Bulge", -100, 100, 0, " %")};
        d.spatial = true;
        d.isIdentity = [](const EffectInstance& e) { return e.value("amount") == 0.0f; };
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.fx.chromakey";
        d.label = "Chroma Key";
        d.category = "Keying";
        d.description = "Removes a background colour (green screen), with tolerance, pedestal, spill suppression and "
                        "edge controls.";
        d.params = {colorParam("color", "Key Colour", "Chroma Key", 0x00B140),
                    slider("tolerance", "Tolerance", "Chroma Key", 0, 100, 20, "", 0, "Widens the removed colour range"),
                    slider("pedestal", "Pedestal", "Chroma Key", 0, 100, 30, "", 0,
                           "Clip level: higher removes more of the colours near the key"),
                    slider("spill", "Spill Suppression", "Chroma Key", 0, 100, 0, "", 0, "Neutralises key-coloured fringes"),
                    slider("feather", "Edge Feather", "Chroma Key", 0, 100, 10, "", 0, "Softens the matte edge"),
                    slider("thickness", "Edge Thickness", "Chroma Key", -10, 10, -2, "", 0,
                           "Negative shrinks the kept area, positive grows it"),
                    switchParam("alphaView", "Show Matte", "Chroma Key", false, "Shows the matte (white = kept)")};
        d.spatial = true;
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.filter.bw";
        d.label = "Black & White";
        d.section = EffectSection::Filters;
        d.category = "Black & White";
        d.description = "Monochrome from a mix of the red, green and blue channels.";
        d.params = {slider("red", "Red", "Mix", -200, 300, 21, " %"), slider("green", "Green", "Mix", -200, 300, 72, " %"),
                    slider("blue", "Blue", "Mix", -200, 300, 7, " %")};
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.filter.sepia";
        d.label = "Sepia";
        d.section = EffectSection::Filters;
        d.category = "Vintage";
        d.description = "Monochrome tinted with a tone colour in the mid-tones.";
        d.params = {colorParam("tone", "Tone", "Sepia", 0xA87B4D), slider("strength", "Tint", "Sepia", 0, 200, 100, " %")};
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.filter.look";
        d.label = "Look";
        d.section = EffectSection::Hidden;   // added from the Filters page with a look chosen
        d.category = "Looks";
        d.description = "One of MeerCut's built-in looks (a 3-D LUT).";
        d.params = {slider("strength", "Strength", "Look", 0, 100, 100, " %")};
        d.usesLut = true;
        d.isIdentity = [](const EffectInstance& e) { return !e.tables || !e.tables->lut || e.value("strength") <= 0.0f; };
        v.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.id = "fa.fx.blur.gaussian";
        d.label = "Gaussian Blur";
        d.section = EffectSection::Effects;
        d.category = "Blur";
        d.description = "Smooth blur of the whole picture.";
        d.params = {slider("radius", "Radius", "Blur", 0, 200, 20, " px", 1, "Blur extent in pixels at 1080p (3 sigma)")};
        d.spatial = true;
        d.isIdentity = [](const EffectInstance& e) { return e.value("radius") <= 0.0f; };
        v.push_back(std::move(d));
    }
    // ---- M11: more effects and a filter (MeerCut designs; effects_cpu.cpp and the GPU passes draw them alike)
    auto add = [&](const char* id, const char* label, const char* category, const char* description,
                   std::vector<ParamDef> params, bool spatial, bool timeVarying,
                   bool (*identity)(const EffectInstance&) = nullptr) {
        EffectDef d;
        d.id = id;
        d.label = label;
        d.category = category;
        d.description = description;
        d.params = std::move(params);
        d.spatial = spatial;
        d.timeVarying = timeVarying;
        d.isIdentity = identity;
        v.push_back(std::move(d));
        return &v.back();   // valid until the next push_back
    };
    auto center = [](const char* group) {
        return std::vector<ParamDef>{slider("centerX", "Center X", group, -100, 100, 0, " %"),
                                     slider("centerY", "Center Y", group, -100, 100, 0, " %")};
    };
    auto join = [](std::vector<ParamDef> a, const std::vector<ParamDef>& b) {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    };
    add("fa.fx.mirror", "Mirror", "Distortion", "One half of the picture reflected onto the other (or a quarter onto all four).",
        {choiceParam("mode", "Mirror", "Mirror",
                     {"Left onto right", "Right onto left", "Top onto bottom", "Bottom onto top", "Four corners"})},
        true, false);
    add("fa.fx.kaleidoscope", "Kaleidoscope", "Distortion", "Mirrored wedges around the centre, like a kaleidoscope.",
        {slider("segments", "Segments", "Kaleidoscope", 2, 16, 6), slider("angle", "Angle", "Kaleidoscope", -180, 180, 0, "°"),
         slider("zoom", "Zoom", "Kaleidoscope", 50, 200, 100, " %")},
        true, false);
    add("fa.fx.swirl", "Swirl", "Distortion", "Twists a round area around its centre (positive turns clockwise).",
        join({slider("angle", "Angle", "Swirl", -720, 720, 180, "°"),
              slider("radius", "Radius", "Swirl", 1, 100, 50, " %", 0, "Radius in % of the picture height")},
             center("Swirl")),
        true, false, [](const EffectInstance& e) { return e.value("angle") == 0.0f; });
    add("fa.fx.fisheye", "Fisheye", "Lens", "Lens distortion: positive bulges the middle like a fisheye lens, negative "
                                            "pinches it. The corners stay in place.",
        {slider("amount", "Amount", "Fisheye", -100, 100, 50)}, true, false,
        [](const EffectInstance& e) { return e.value("amount") == 0.0f; });
    add("fa.fx.blur.zoom", "Zoom Blur", "Blur", "Streaks towards a centre, as if the camera zoomed during the exposure.",
        join({slider("amount", "Amount", "Zoom Blur", 0, 100, 25, " %", 0, "Streak length in % of the distance to the centre")},
             center("Zoom Blur")),
        true, false, [](const EffectInstance& e) { return e.value("amount") <= 0.0f; });
    add("fa.fx.tiltshift", "Tilt-Shift", "Blur", "A sharp band with blur above and below: the miniature-model look.",
        {slider("position", "Focus Position", "Tilt-Shift", -100, 100, 0, " %"),
         slider("width", "Focus Width", "Tilt-Shift", 0, 100, 30, " %", 0, "Height of the sharp band in % of the picture"),
         slider("blur", "Blur", "Tilt-Shift", 0, 200, 40, " px", 0, "Blur in pixels at 1080p"),
         slider("feather", "Feather", "Tilt-Shift", 0, 100, 40, " %")},
        true, false, [](const EffectInstance& e) { return e.value("blur") <= 0.0f; });
    add("fa.fx.shake", "Camera Shake", "Camera", "A hand-held wobble: the picture moves, turns and is enlarged a little "
                                                 "so its edges stay outside the frame.",
        {slider("amount", "Amount", "Shake", 0, 100, 20, " px", 0, "Movement in pixels at 1080p"),
         slider("speed", "Speed", "Shake", 0.1, 10, 2, " Hz", 1), slider("rotation", "Rotation", "Shake", 0, 10, 1, "°", 1),
         slider("zoom", "Zoom", "Shake", 0, 30, 6, " %", 0, "Enlarges the picture so its moving edges stay outside")},
        true, true, [](const EffectInstance& e) {
            return e.value("amount") <= 0.0f && e.value("rotation") <= 0.0f && e.value("zoom") <= 0.0f;
        });
    add("fa.fx.glitch", "Glitch", "Glitch", "Bands of the picture slip sideways with split colours, changing several "
                                            "times a second.",
        {slider("amount", "Amount", "Glitch", 0, 100, 50, " %"),
         slider("speed", "Speed", "Glitch", 1, 30, 8, " /s", 0, "How many times a second the glitch changes"),
         slider("bands", "Bands", "Glitch", 2, 40, 12, "", 0, "Number of horizontal bands that can slip")},
        true, true, [](const EffectInstance& e) { return e.value("amount") <= 0.0f; });
    add("fa.fx.vhs", "VHS", "Glitch", "Old video tape: colour bleed, scan lines, noise and a rolling tracking band.",
        {slider("amount", "Colour Bleed", "VHS", 0, 100, 50, " %"), slider("scanlines", "Scan Lines", "VHS", 0, 100, 40, " %"),
         slider("noise", "Noise", "VHS", 0, 100, 30, " %"),
         slider("tracking", "Tracking", "VHS", 0, 100, 40, " %", 0, "A band of distortion rolling down the picture")},
        true, true, [](const EffectInstance& e) {
            return e.value("amount") <= 0.0f && e.value("scanlines") <= 0.0f && e.value("noise") <= 0.0f &&
                   e.value("tracking") <= 0.0f;
        });
    add("fa.fx.oldfilm", "Old Film", "Film", "Faded brown film with flicker, scratches, dust and dark corners.",
        {slider("amount", "Age", "Old Film", 0, 100, 70, " %", 0, "How faded and brown the picture becomes"),
         slider("scratches", "Scratches", "Old Film", 0, 100, 40, " %"), slider("dust", "Dust", "Old Film", 0, 100, 30, " %"),
         slider("flicker", "Flicker", "Old Film", 0, 100, 30, " %"), slider("vignette", "Vignette", "Old Film", 0, 100, 50, " %")},
        false, true, [](const EffectInstance& e) {
            return e.value("amount") <= 0.0f && e.value("scratches") <= 0.0f && e.value("dust") <= 0.0f &&
                   e.value("flicker") <= 0.0f && e.value("vignette") <= 0.0f;
        });
    add("fa.fx.lightleak", "Light Leak", "Film", "Warm light drifting in from the edges, as from a leaky film camera.",
        {slider("amount", "Amount", "Light Leak", 0, 100, 60, " %"), colorParam("color", "Colour", "Light Leak", 0xFF7A2F),
         slider("speed", "Speed", "Light Leak", 0, 2, 0.3, "", 2, "How fast the light drifts"),
         slider("size", "Size", "Light Leak", 10, 150, 70, " %")},
        false, true, [](const EffectInstance& e) { return e.value("amount") <= 0.0f; });
    add("fa.fx.halftone", "Halftone", "Stylize", "Printed-dot look: dots that grow in the dark parts of the picture.",
        {slider("size", "Dot Size", "Halftone", 4, 80, 12, " px", 0, "Dot spacing in pixels at 1080p"),
         slider("angle", "Angle", "Halftone", -90, 90, 45, "°"),
         choiceParam("style", "Style", "Halftone", {"Black on White", "Colour Dots", "Newsprint"})},
        false, false);
    add("fa.fx.posterize", "Posterize", "Stylize", "Fewer tones per colour channel: flat, poster-like colours.",
        {slider("levels", "Levels", "Posterize", 2, 16, 5, "", 0, "Tones per colour channel")}, false, false);
    add("fa.fx.emboss", "Emboss", "Stylize", "A raised relief lit from one side.",
        {slider("strength", "Strength", "Emboss", 0, 300, 100, " %"), slider("angle", "Light Angle", "Emboss", -180, 180, 135, "°"),
         choiceParam("style", "Style", "Emboss", {"Grey Relief", "Colour Relief"})},
        true, false);
    add("fa.fx.invert", "Invert", "Stylize", "Negative: every colour turned into its opposite.",
        {slider("amount", "Amount", "Invert", 0, 100, 100, " %")}, false, false,
        [](const EffectInstance& e) { return e.value("amount") <= 0.0f; });
    add("fa.fx.letterbox", "Letterbox", "Frame", "Bars that crop the picture to a cinema, square or vertical frame.",
        {choiceParam("ratio", "Aspect Ratio", "Letterbox",
                     {"2.39:1 (Cinema)", "2:1", "1.85:1", "4:3", "1:1 (Square)", "4:5 (Portrait)", "9:16 (Vertical)"}),
         colorParam("color", "Bar Colour", "Letterbox", 0x000000), slider("opacity", "Opacity", "Letterbox", 0, 100, 100, " %")},
        false, false, [](const EffectInstance& e) { return e.value("opacity") <= 0.0f; });
    add("fa.fx.spotlight", "Spotlight", "Light", "Darkens everything outside a soft round light.",
        join(center("Spotlight"), {slider("size", "Size", "Spotlight", 5, 150, 45, " %", 0, "Radius in % of the picture height"),
                                   slider("feather", "Feather", "Spotlight", 0, 100, 50, " %"),
                                   slider("darkness", "Darkness", "Spotlight", 0, 100, 75, " %")}),
        false, false, [](const EffectInstance& e) { return e.value("darkness") <= 0.0f; });
    EffectDef* duotone = add("fa.filter.duotone", "Duotone", "Creative",
                             "Two colours only: the shadows in one, the highlights in the other.",
                             {colorParam("shadows", "Shadows", "Duotone", 0x1B2A6B),
                              colorParam("highlights", "Highlights", "Duotone", 0xFFB36B),
                              slider("strength", "Strength", "Duotone", 0, 100, 100, " %")},
                             false, false, [](const EffectInstance& e) { return e.value("strength") <= 0.0f; });
    duotone->section = EffectSection::Filters;
    return v;
}

// Curve tables of a Curves effect (cached by the curve points; nullptr when every curve is neutral).
std::shared_ptr<const EffectTables> curveTablesFor(const EffectDef& def, const EffectRef& e) {
    std::ostringstream key;
    key.precision(17);
    bool neutral = true;
    std::vector<std::vector<Vec2>> pts;
    for (const auto& c : def.curves) {
        const auto it = e.curves.find(c.id);
        std::vector<Vec2> p = normalizeCurve(it == e.curves.end() ? c.defaults : it->second);
        const bool identity = c.periodic ? std::all_of(p.begin(), p.end(), [](const Vec2& q) { return std::fabs(q.y - 0.5) < 1e-9; })
                                         : (p.size() == 2 && p[0].x == 0 && p[0].y == 0 && p[1].x == 1 && p[1].y == 1) ||
                                               p.empty();
        neutral = neutral && identity;
        key << c.id << ':';
        for (const auto& q : p) key << q.x << ',' << q.y << ';';
        pts.push_back(std::move(p));
    }
    if (neutral) return nullptr;
    static std::mutex m;
    static std::map<std::string, std::shared_ptr<const EffectTables>> cache;
    std::lock_guard lk(m);
    if (auto it = cache.find(key.str()); it != cache.end()) return it->second;
    auto t = std::make_shared<EffectTables>();
    const int n = EffectTables::kCurveSize;
    std::vector<std::vector<float>> tab;
    for (size_t i = 0; i < def.curves.size(); ++i) tab.push_back(curveTable(pts[i], n, def.curves[i].periodic));
    t->tone.resize(static_cast<size_t>(n));
    t->hue.resize(static_cast<size_t>(n));
    for (size_t i = 0; i < static_cast<size_t>(n); ++i) {
        t->tone[i] = {tab[0][i], tab[1][i], tab[2][i], tab[3][i]};
        t->hue[i] = {tab[4][i] - 0.5f, tab[5][i] * 2.0f};   // hue offset in turns (+-0.5), saturation factor 0..2
    }
    if (cache.size() > 64) cache.clear();
    cache[key.str()] = t;
    return t;
}

uint32_t hash32(uint32_t x) {   // integer hash (also used by the GPU noise)
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

}  // namespace

int EffectDef::paramIndex(std::string_view pid) const {
    for (size_t i = 0; i < params.size(); ++i)
        if (params[i].id == pid) return static_cast<int>(i);
    return -1;
}

const std::vector<EffectDef>& effectDefs() {
    static const std::vector<EffectDef> defs = buildDefs();
    return defs;
}

const EffectDef* findEffectDef(std::string_view id) {
    for (const auto& d : effectDefs())
        if (d.id == id) return &d;
    return nullptr;
}

double EffectDef::neutralValue(std::string_view pid) const {
    for (const auto& [name, v] : neutral)
        if (name == pid) return v;
    const int i = paramIndex(pid);
    return i < 0 ? 0.0 : params[static_cast<size_t>(i)].def;
}

EffectRef makeEffect(const EffectDef& def, std::string instanceId, bool neutral) {
    EffectRef e;
    e.id = std::move(instanceId);
    e.type = def.id;
    e.version = def.version;
    for (const auto& p : def.params) e.params[p.id] = Animatable<double>(neutral ? def.neutralValue(p.id) : p.def);
    for (const auto& c : def.curves) e.curves[c.id] = c.defaults;
    return e;
}

double effectParam(const EffectRef& e, const EffectDef& def, std::string_view pid, const Time& t) {
    const int i = def.paramIndex(pid);
    if (i < 0) return 0.0;
    const ParamDef& p = def.params[static_cast<size_t>(i)];
    const auto it = e.params.find(p.id);
    const double v = it == e.params.end() ? p.def : it->second.value(t);
    return std::clamp(v, p.min, p.max);
}

float EffectInstance::value(std::string_view pid) const {
    const int i = def ? def->paramIndex(pid) : -1;
    return i >= 0 && static_cast<size_t>(i) < values.size() ? values[static_cast<size_t>(i)] : 0.0f;
}

std::vector<EffectInstance> evaluateEffects(const std::vector<EffectRef>& effects, const Time& t, double clipSeconds,
                                            std::vector<std::string>* problems, const std::filesystem::path& assetDir) {
    std::vector<EffectInstance> out;
    for (const EffectRef& e : effects) {
        if (!e.enabled) continue;
        const EffectDef* def = findEffectDef(e.type);
        if (!def) {
            if (problems) problems->push_back("effect '" + e.type + "' is not available in this version");
            continue;
        }
        EffectInstance in;
        in.def = def;
        in.mix = static_cast<float>(std::clamp(e.mix.value(t), 0.0, 1.0));
        if (in.mix <= 0.0f) continue;
        for (const auto& p : def->params) in.values.push_back(static_cast<float>(effectParam(e, *def, p.id, t)));
        for (const Mask& m : e.masks) {
            if (!m.enabled) continue;
            LayerMask lm;
            lm.shape = m.shape;
            lm.mode = m.mode;
            lm.inverted = m.inverted;
            lm.cx = m.centerX.value(t);
            lm.cy = m.centerY.value(t);
            lm.w = m.width.value(t);
            lm.h = m.height.value(t);
            lm.rotationDeg = m.rotation.value(t);
            lm.feather = std::max(0.0, m.feather.value(t));
            lm.opacity = std::clamp(m.opacity.value(t), 0.0, 1.0);
            lm.cornerRadius = m.cornerRadius;
            lm.points = m.points;
            in.masks.push_back(std::move(lm));
        }
        if (!def->curves.empty()) in.tables = curveTablesFor(*def, e);
        if (def->usesLut) {
            const auto it = e.strings.find("lut");
            if (it != e.strings.end() && !it->second.empty()) {
                std::string err;
                if (auto lut = resolveLut(it->second, assetDir, &err)) {
                    auto tables = std::make_shared<EffectTables>();
                    tables->lut = std::move(lut);
                    in.tables = std::move(tables);
                } else {
                    in.problem = "LUT \"" + it->second + "\": " + err;   // skipped; reported by the renderer
                }
            }
        }
        in.seconds = clipSeconds;
        uint32_t h = 2166136261u;
        for (char ch : e.id) h = (h ^ static_cast<uint8_t>(ch)) * 16777619u;
        in.seed = hash32(h ^ static_cast<uint32_t>(std::llround(clipSeconds * 1000.0)));
        out.push_back(std::move(in));
    }
    return out;
}

bool effectsAreIdentity(const std::vector<EffectInstance>& stack) {
    for (const auto& e : stack) {
        if (e.mix <= 0.0f || !e.problem.empty()) continue;
        if (!e.def->isIdentity || !e.def->isIdentity(e)) return false;
    }
    return true;
}

}  // namespace fa
