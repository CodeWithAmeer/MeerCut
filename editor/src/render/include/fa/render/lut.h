// Colour lookup tables (M4): Adobe / Resolve ".cube" files with a 3-D table (LUT_3D_SIZE 2..129), a 1-D table
// (LUT_1D_SIZE 2..65536) or both (the 1-D "shaper" runs first), TITLE, DOMAIN_MIN / DOMAIN_MAX (or Resolve's
// LUT_1D_INPUT_RANGE / LUT_3D_INPUT_RANGE) and '#' comments. Tables apply to gamma-encoded R'G'B' 0..1 (the effect
// working space); the result is clamped to 0..1 like every effect.
//   interpolation: tetrahedral for the 3-D table (Filmora uses it: Stage 1 EXP-012, 1.51 levels vs trilinear 7.88),
//                  linear for the 1-D table
//   domain:        Standard = the .cube specification: the input range DOMAIN_MIN..DOMAIN_MAX is mapped onto the
//                  table (inputs outside it are clamped). FilmoraCompatible = what Filmora 16 does (EXP-012): the
//                  table is looked up with the input as is and its output is scaled into DOMAIN_MIN..DOMAIN_MAX (an
//                  identity table with DOMAIN_MAX 0.5 gives x / 2). The two agree for the usual 0..1 domain.
#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fa {

enum class LutDomainMode { Standard = 0, FilmoraCompatible = 1 };

struct Lut3D {
    std::string title;
    int size = 0;                  // 3-D entries per axis (0: no 3-D table)
    std::vector<float> table;      // size^3 RGB triplets, red fastest, then green, then blue
    std::array<float, 3> domainMin{0, 0, 0}, domainMax{1, 1, 1};
    int size1d = 0;                // 1-D entries (0: no 1-D table)
    std::vector<float> table1d;    // size1d RGB triplets
    std::array<float, 3> domainMin1d{0, 0, 0}, domainMax1d{1, 1, 1};

    bool isIdentity(float tolerance = 1e-5f) const;
};

// Parses .cube text. On failure returns nullptr and sets `error` ("line 12: ..."); `name` prefixes messages.
std::shared_ptr<const Lut3D> parseCube(std::string_view text, std::string* error);
// Reads and parses a file (at most 64 MB).
std::shared_ptr<const Lut3D> loadCube(const std::filesystem::path& file, std::string* error);
// loadCube cached by path, size and modification time: every effect and both backends share one table.
std::shared_ptr<const Lut3D> cachedLut(const std::filesystem::path& file, std::string* error);

// One pixel, 0..1 in and out.
void applyLut(const Lut3D& lut, LutDomainMode mode, float& r, float& g, float& b);

// Built-in looks (MeerCut's own, generated from formulas, 33^3): "builtin:<id>" references.
struct BuiltinLook {
    std::string id;      // "teal-orange"
    std::string name;    // "Teal & Orange"
    std::string description;
    std::string category;   // the Filters page group: one of builtinLookCategories()
};
// The look groups in the order the Filters page lists them.
const std::vector<std::string>& builtinLookCategories();
const std::vector<BuiltinLook>& builtinLooks();
std::shared_ptr<const Lut3D> builtinLut(std::string_view id);   // nullptr for an unknown id
constexpr std::string_view kBuiltinLutPrefix = "builtin:";

// Resolves an effect's LUT reference: "builtin:<id>", an absolute path, or a path relative to `baseDir` (the
// project file's folder). Returns nullptr and sets `error` when it cannot be loaded.
std::shared_ptr<const Lut3D> resolveLut(const std::string& reference, const std::filesystem::path& baseDir, std::string* error);

}  // namespace fa
