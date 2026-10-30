// Effects (M4): registry of effect types with typed parameter schemas, evaluation of a clip's effect stack at a
// time, and the CPU reference implementation (the Direct3D 11 backend mirrors it pass for pass).
//
// Processing model (one definition for every backend):
//   * A stack runs top to bottom on a picture: a clip's decoded (or generated) picture in its own pixel space,
//     before placement and compositing; an adjustment layer's stack runs on the canvas composited so far.
//   * Pixels are float RGBA, straight alpha, gamma-encoded Rec.709 R'G'B' in 0..1 (MeerCut's working space, the
//     values Filmora's colour sliders operate on: Stage 1 EXP-006). Spatial effects filter premultiplied colour.
//     Every effect's output is clamped to 0..1. The stack's result is rounded to 8 bits once at the end.
//   * Each effect: out = in + (fx(in) - in) * mix * coverage(masks); mix is the animatable strength.
//   * Sizes (blur radius, block size, offsets) are pixels at 1080 lines of the picture's height, so an effect looks
//     the same in the preview (smaller pictures) and in the export.
#pragma once

#include "fa/core/image.h"
#include "fa/model/project.h"
#include "fa/render/compositing.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fa {

enum class ParamKind { Float, Int, Bool, Choice, Color, Angle };

struct ParamDef {
    std::string id;
    std::string label;
    ParamKind kind = ParamKind::Float;
    double min = 0, max = 1, def = 0;
    double step = 1;            // UI step
    std::string unit;           // " %", " px", "°" ...
    int decimals = 0;
    bool animatable = true;     // colours and choices are not keyframed
    std::vector<std::string> choices;   // ParamKind::Choice
    std::string group;          // UI group ("Light", "Color" ...)
    std::string tooltip;
};

struct EffectInstance;

// A curve parameter (Curves effect): tone curves map 0..1 to 0..1; periodic curves are hue curves (neutral 0.5).
struct CurveDef {
    std::string id;
    std::string label;
    bool periodic = false;
    std::vector<Vec2> defaults;
};

// Where an effect type is offered in the UI.
enum class EffectSection { Color, Effects, Filters, Hidden };

struct EffectDef {
    std::string id;             // stable, saved in projects ("fa.color.basic")
    int version = 1;
    std::string label;
    EffectSection section = EffectSection::Effects;
    std::string category;       // browser category ("Blur", "Stylize" ...)
    std::string description;
    std::vector<ParamDef> params;
    std::vector<CurveDef> curves;      // curve parameters (Curves effect)
    bool usesLut = false;              // strings["lut"]
    bool spatial = false;              // reads neighbouring pixels
    bool timeVarying = false;          // changes over time by itself (grain, waves)
    // True when the evaluated instance leaves every pixel unchanged (neutral settings); nullptr = never.
    bool (*isIdentity)(const EffectInstance&) = nullptr;
    // Parameter values (where they differ from the defaults) that make an instance leave the picture unchanged:
    // what an untouched Color-tab control means (sharpen and vignette default to a visible amount).
    std::vector<std::pair<std::string, double>> neutral;
    int paramIndex(std::string_view pid) const;
    double neutralValue(std::string_view pid) const;   // the neutral value, else the default
};

const std::vector<EffectDef>& effectDefs();
const EffectDef* findEffectDef(std::string_view id);
// A new effect instance with the type's defaults (curves: identity, LUT: none), or its neutral settings.
EffectRef makeEffect(const EffectDef& def, std::string instanceId, bool neutral = false);
// Value of a parameter at a source time (the type's default when the instance does not store it).
double effectParam(const EffectRef& e, const EffectDef& def, std::string_view pid, const Time& t);

// ---- evaluated stack ---------------------------------------------------------------------------------

struct Lut3D;   // fa/render/lut.h

// Precomputed tables of an effect at one time (shared by both backends).
struct EffectTables {
    // 1D tables of kCurveSize entries (0..1 -> value): master, red, green, blue tone curves; hue curves map
    // hue/360 -> hue offset in turns (hue vs hue) and saturation factor (hue vs sat).
    static constexpr int kCurveSize = 1024;
    std::vector<std::array<float, 4>> tone;     // y, r, g, b
    std::vector<std::array<float, 2>> hue;      // hue offset (turns), saturation factor
    std::shared_ptr<const Lut3D> lut;           // 3D LUT (LUT effect, filters)
    std::vector<float> kernel;                  // spatial weights (blurs)
};

struct EffectInstance {
    const EffectDef* def = nullptr;
    std::vector<float> values;                  // by EffectDef::params index
    float mix = 1.0f;
    std::vector<LayerMask> masks;               // in the picture's layer-height units
    std::shared_ptr<const EffectTables> tables;
    double seconds = 0;                         // clip-local time (time-varying effects)
    uint32_t seed = 0;                          // per-frame noise seed
    std::string problem;                        // non-empty: the effect cannot run (e.g. LUT file missing) -> skipped
    float value(std::string_view pid) const;
};

// The enabled effects of a stack at source time `t` (clip time for time-varying effects = t - sourceIn).
// Unknown types are reported in `problems` and skipped. LUT references (strings["lut"]) are resolved against
// `assetDir` (the project file's folder); a LUT that cannot be loaded marks the instance's `problem` (skipped).
std::vector<EffectInstance> evaluateEffects(const std::vector<EffectRef>& effects, const Time& t, double clipSeconds,
                                            std::vector<std::string>* problems = nullptr,
                                            const std::filesystem::path& assetDir = {});

// CPU reference: runs the stack on `src` (straight RGBA8) and writes the 8-bit result into `out` (same size).
void applyEffectsCpu(const Image& src, const std::vector<EffectInstance>& stack, Image& out);
// Adjustment layer on an opaque canvas (compositing.h): canvas = canvas + (fx(canvas) - canvas) * opacity * masks.
void applyAdjustmentCpu(Image& canvas, const LayerDesc& layer);
bool effectHasCpuImplementation(const std::string& type);

// True when no effect in the stack changes pixels at all (empty, all mix 0, ...): the picture is used as is.
bool effectsAreIdentity(const std::vector<EffectInstance>& stack);

}  // namespace fa
