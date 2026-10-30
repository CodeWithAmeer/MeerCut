#include "fa/render/lut.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>

namespace fa {

bool Lut3D::isIdentity(float tol) const {
    for (int i = 0; i < 3; ++i)
        if (domainMin[i] != 0.0f || domainMax[i] != 1.0f || domainMin1d[i] != 0.0f || domainMax1d[i] != 1.0f) return false;
    if (size1d > 0)
        for (int i = 0; i < size1d; ++i)
            for (int c = 0; c < 3; ++c)
                if (std::fabs(table1d[static_cast<size_t>(i) * 3 + c] - float(i) / float(size1d - 1)) > tol) return false;
    if (size > 0)
        for (int b = 0; b < size; ++b)
            for (int g = 0; g < size; ++g)
                for (int r = 0; r < size; ++r) {
                    const float* t = &table[((static_cast<size_t>(b) * size + g) * size + r) * 3];
                    const float n = float(size - 1);
                    if (std::fabs(t[0] - r / n) > tol || std::fabs(t[1] - g / n) > tol || std::fabs(t[2] - b / n) > tol) return false;
                }
    return true;
}

// ---- parsing --------------------------------------------------------------------------------------------------

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Whitespace-separated floats; false when a token is not a finite number.
bool parseFloats(std::string_view s, std::vector<float>& out) {
    out.clear();
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i >= s.size()) break;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
        float v = 0;
        const char* first = s.data() + i;
        const char* last = s.data() + j;
        if (*first == '+') ++first;
        const auto [p, ec] = std::from_chars(first, last, v);
        if (ec != std::errc() || p != last || !std::isfinite(v)) return false;
        out.push_back(v);
        i = j;
    }
    return true;
}

bool fail(std::string* error, size_t line, const std::string& msg) {
    if (error) *error = (line ? "line " + std::to_string(line) + ": " : std::string()) + msg;
    return false;
}

}  // namespace

std::shared_ptr<const Lut3D> parseCube(std::string_view text, std::string* error) {
    auto lut = std::make_shared<Lut3D>();
    bool haveDomain = false, have1dRange = false, have3dRange = false;
    std::array<float, 3> dmin{0, 0, 0}, dmax{1, 1, 1};
    std::vector<float> nums, data;
    size_t lineNo = 0, firstDataLine = 0;
    bool inData = false;
    if (text.size() >= 3 && std::memcmp(text.data(), "\xEF\xBB\xBF", 3) == 0) text.remove_prefix(3);   // UTF-8 BOM
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        const std::string_view raw = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        ++lineNo;
        const std::string_view line = trim(raw);
        if (line.empty() || line.front() == '#') continue;
        const char c0 = line.front();
        if ((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z')) {
            if (inData) return fail(error, lineNo, "keyword after the table data"), nullptr;
            const size_t sp = line.find_first_of(" \t");
            const std::string key(line.substr(0, sp));
            const std::string_view rest = sp == std::string_view::npos ? std::string_view() : trim(line.substr(sp));
            if (key == "TITLE") {
                std::string t(rest);
                if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);
                lut->title = t;
                continue;
            }
            if (!parseFloats(rest, nums)) return fail(error, lineNo, "bad number in " + key), nullptr;
            if (key == "LUT_3D_SIZE" || key == "LUT_1D_SIZE") {
                if (nums.size() != 1 || nums[0] != std::floor(nums[0])) return fail(error, lineNo, key + " needs one integer"), nullptr;
                const int n = static_cast<int>(nums[0]);
                if (key == "LUT_3D_SIZE") {
                    if (n < 2 || n > 129) return fail(error, lineNo, "LUT_3D_SIZE " + std::to_string(n) + " is outside 2..129"), nullptr;
                    lut->size = n;
                } else {
                    if (n < 2 || n > 65536) return fail(error, lineNo, "LUT_1D_SIZE " + std::to_string(n) + " is outside 2..65536"), nullptr;
                    lut->size1d = n;
                }
            } else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
                if (nums.size() != 3) return fail(error, lineNo, key + " needs three numbers"), nullptr;
                (key == "DOMAIN_MIN" ? dmin : dmax) = {nums[0], nums[1], nums[2]};
                haveDomain = true;
            } else if (key == "LUT_1D_INPUT_RANGE" || key == "LUT_3D_INPUT_RANGE") {
                if (nums.size() != 2) return fail(error, lineNo, key + " needs two numbers"), nullptr;
                auto& mn = key == "LUT_1D_INPUT_RANGE" ? lut->domainMin1d : lut->domainMin;
                auto& mx = key == "LUT_1D_INPUT_RANGE" ? lut->domainMax1d : lut->domainMax;
                mn = {nums[0], nums[0], nums[0]};
                mx = {nums[1], nums[1], nums[1]};
                (key == "LUT_1D_INPUT_RANGE" ? have1dRange : have3dRange) = true;
            } else if (key == "LUT_IN_VIDEO_RANGE" || key == "LUT_OUT_VIDEO_RANGE") {
                return fail(error, lineNo, key + " (video-range LUTs) is not supported"), nullptr;
            }
            // other keywords (e.g. LUT_3D_INPUT_SHAPER notes from some exporters) carry no data we use: ignored
            continue;
        }
        if (!parseFloats(line, nums) || nums.size() != 3) return fail(error, lineNo, "expected three numbers (R G B)"), nullptr;
        if (!inData) firstDataLine = lineNo;
        inData = true;
        data.insert(data.end(), nums.begin(), nums.end());
    }
    if (lut->size == 0 && lut->size1d == 0) return fail(error, 0, "no LUT_3D_SIZE or LUT_1D_SIZE"), nullptr;
    const size_t n1 = static_cast<size_t>(lut->size1d), n3 = static_cast<size_t>(lut->size) * lut->size * lut->size;
    if (data.size() != (n1 + n3) * 3)
        return fail(error, firstDataLine, "expected " + std::to_string(n1 + n3) + " table entries, found " + std::to_string(data.size() / 3)),
               nullptr;
    lut->table1d.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n1 * 3));
    lut->table.assign(data.begin() + static_cast<std::ptrdiff_t>(n1 * 3), data.end());
    if (haveDomain) {   // DOMAIN_* is the input range of the first table
        if (lut->size1d > 0 && !have1dRange) {
            lut->domainMin1d = dmin;
            lut->domainMax1d = dmax;
        } else if (!have3dRange) {
            lut->domainMin = dmin;
            lut->domainMax = dmax;
        }
    }
    for (int c = 0; c < 3; ++c)
        if (!(lut->domainMax[c] > lut->domainMin[c]) || !(lut->domainMax1d[c] > lut->domainMin1d[c]))
            return fail(error, 0, "DOMAIN_MAX must be greater than DOMAIN_MIN"), nullptr;
    return lut;
}

std::shared_ptr<const Lut3D> loadCube(const std::filesystem::path& file, std::string* error) {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(file, ec);
    if (ec) return fail(error, 0, "file not found"), nullptr;
    if (bytes > 64ull * 1024 * 1024) return fail(error, 0, "file is larger than 64 MB"), nullptr;
    std::ifstream in(file, std::ios::binary);
    if (!in) return fail(error, 0, "cannot read the file"), nullptr;
    std::string text(static_cast<size_t>(bytes), '\0');
    in.read(text.data(), static_cast<std::streamsize>(bytes));
    return parseCube(text, error);
}

std::shared_ptr<const Lut3D> cachedLut(const std::filesystem::path& file, std::string* error) {
    struct Entry {
        uintmax_t size;
        std::filesystem::file_time_type time;
        std::shared_ptr<const Lut3D> lut;
    };
    static std::mutex m;
    static std::map<std::filesystem::path, Entry> cache;
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec) return fail(error, 0, "file not found"), nullptr;
    const auto time = std::filesystem::last_write_time(file, ec);
    {
        std::lock_guard lk(m);
        if (const auto it = cache.find(file); it != cache.end() && it->second.size == size && it->second.time == time) return it->second.lut;
    }
    auto lut = loadCube(file, error);
    if (!lut) return nullptr;
    std::lock_guard lk(m);
    if (cache.size() > 32) cache.clear();
    cache[file] = Entry{size, time, lut};
    return lut;
}

// ---- applying -------------------------------------------------------------------------------------------------

void applyLut(const Lut3D& lut, LutDomainMode mode, float& r, float& g, float& b) {
    float v[3] = {r, g, b};
    const bool filmora = mode == LutDomainMode::FilmoraCompatible;
    if (lut.size1d > 0) {
        const int n = lut.size1d;
        for (int c = 0; c < 3; ++c) {
            float x = v[c];
            if (!filmora) x = (x - lut.domainMin1d[c]) / (lut.domainMax1d[c] - lut.domainMin1d[c]);
            const float pos = std::clamp(x, 0.0f, 1.0f) * float(n - 1);
            const int i = std::min(n - 2, static_cast<int>(pos));
            const float f = pos - float(i);
            const float a = lut.table1d[static_cast<size_t>(i) * 3 + c], bb = lut.table1d[static_cast<size_t>(i + 1) * 3 + c];
            v[c] = a + (bb - a) * f;
            if (filmora) v[c] = lut.domainMin1d[c] + v[c] * (lut.domainMax1d[c] - lut.domainMin1d[c]);
        }
    }
    if (lut.size > 0) {
        const int n = lut.size;
        float p[3];
        int i[3];
        float f[3];
        for (int c = 0; c < 3; ++c) {
            float x = v[c];
            if (!filmora) x = (x - lut.domainMin[c]) / (lut.domainMax[c] - lut.domainMin[c]);
            p[c] = std::clamp(x, 0.0f, 1.0f) * float(n - 1);
            i[c] = std::min(n - 2, static_cast<int>(p[c]));
            f[c] = p[c] - float(i[c]);
        }
        auto at = [&](int dr, int dg, int db) {
            return &lut.table[((static_cast<size_t>(i[2] + db) * n + (i[1] + dg)) * n + (i[0] + dr)) * 3];
        };
        const float *c000 = at(0, 0, 0), *c111 = at(1, 1, 1);
        const float fr = f[0], fg = f[1], fb = f[2];
        // tetrahedral: the cube is split into 6 tetrahedra along its main diagonal (same split as FFmpeg's lut3d)
        const float* cA;
        const float* cB;
        float w0, wA, wB, w1;
        if (fr > fg) {
            if (fg > fb) { cA = at(1, 0, 0); cB = at(1, 1, 0); w0 = 1 - fr; wA = fr - fg; wB = fg - fb; w1 = fb; }
            else if (fr > fb) { cA = at(1, 0, 0); cB = at(1, 0, 1); w0 = 1 - fr; wA = fr - fb; wB = fb - fg; w1 = fg; }
            else { cA = at(0, 0, 1); cB = at(1, 0, 1); w0 = 1 - fb; wA = fb - fr; wB = fr - fg; w1 = fg; }
        } else {
            if (fb > fg) { cA = at(0, 0, 1); cB = at(0, 1, 1); w0 = 1 - fb; wA = fb - fg; wB = fg - fr; w1 = fr; }
            else if (fb > fr) { cA = at(0, 1, 0); cB = at(0, 1, 1); w0 = 1 - fg; wA = fg - fb; wB = fb - fr; w1 = fr; }
            else { cA = at(0, 1, 0); cB = at(1, 1, 0); w0 = 1 - fg; wA = fg - fr; wB = fr - fb; w1 = fb; }
        }
        for (int c = 0; c < 3; ++c) {
            v[c] = w0 * c000[c] + wA * cA[c] + wB * cB[c] + w1 * c111[c];
            if (filmora) v[c] = lut.domainMin[c] + v[c] * (lut.domainMax[c] - lut.domainMin[c]);
        }
    }
    r = std::clamp(v[0], 0.0f, 1.0f);
    g = std::clamp(v[1], 0.0f, 1.0f);
    b = std::clamp(v[2], 0.0f, 1.0f);
}

// ---- built-in looks -------------------------------------------------------------------------------------------
// MeerCut's own looks: simple, documented formulas on gamma-encoded R'G'B', baked into 33^3 tables.

namespace {

using Fn = std::function<void(float&, float&, float&)>;

float luma709(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
float sCurve(float x, float k) {   // contrast around mid grey, k > 1 steeper
    const float a = std::pow(std::clamp(x, 0.0f, 1.0f), k), b = std::pow(1.0f - std::clamp(x, 0.0f, 1.0f), k);
    return a / (a + b);
}
void saturate(float& r, float& g, float& b, float s) {
    const float y = luma709(r, g, b);
    r = y + (r - y) * s;
    g = y + (g - y) * s;
    b = y + (b - y) * s;
}
// M11 helpers
void contrast(float& r, float& g, float& b, float k) { r = sCurve(r, k), g = sCurve(g, k), b = sCurve(b, k); }
void levels(float& r, float& g, float& b, float black, float white) {   // output range black .. white
    r = black + r * (white - black), g = black + g * (white - black), b = black + b * (white - black);
}
void lift(float& r, float& g, float& b, float k) {   // brighter mid-tones, white and black stay: 1 - (1 - x)^k
    r = 1 - std::pow(1 - std::clamp(r, 0.0f, 1.0f), k);
    g = 1 - std::pow(1 - std::clamp(g, 0.0f, 1.0f), k);
    b = 1 - std::pow(1 - std::clamp(b, 0.0f, 1.0f), k);
}
void split(float& r, float& g, float& b, float sr, float sg, float sb, float hr, float hg, float hb) {   // split toning
    const float y = std::clamp(luma709(r, g, b), 0.0f, 1.0f), sh = (1 - y) * (1 - y), hi = y * y;
    r += sr * sh + hr * hi, g += sg * sh + hg * hi, b += sb * sh + hb * hi;
}
void monoTone(float& r, float& g, float& b, float tr, float tg, float tb, float offset = 0) {   // tinted black and white
    const float y = luma709(r, g, b);
    r = y * tr + offset, g = y * tg + offset, b = y * tb + offset;
}
void hueRotate(float& r, float& g, float& b, float degrees) {   // about the grey axis (Rodrigues' rotation)
    const float a = degrees * 3.14159265f / 180.0f, c = std::cos(a), k = (1 - c) / 3, q = std::sin(a) * 0.57735027f;
    const float nr = r * (c + k) + g * (k - q) + b * (k + q), ng = r * (k + q) + g * (c + k) + b * (k - q),
                nb = r * (k - q) + g * (k + q) + b * (c + k);
    r = nr, g = ng, b = nb;
}

struct LookDef {
    BuiltinLook info;
    Fn fn;
};

const std::vector<LookDef>& lookDefs() {
    static const std::vector<LookDef> v = {
        {{"teal-orange", "Teal & Orange", "Cool teal shadows, warm highlights and skin, gentle contrast", "Cinematic"},
         [](float& r, float& g, float& b) {
             const float y = luma709(r, g, b);
             const float sh = (1 - y) * (1 - y), hi = y * y;
             r += -0.05f * sh + 0.07f * hi;
             g += 0.02f * sh + 0.02f * hi;
             b += 0.06f * sh - 0.06f * hi;
             r = sCurve(r, 1.12f), g = sCurve(g, 1.12f), b = sCurve(b, 1.12f);
             saturate(r, g, b, 1.08f);
         }},
        {{"warm", "Warm", "Warmer white balance", "Basic"},
         [](float& r, float& g, float& b) { r *= 1.07f, g *= 1.01f, b *= 0.90f; }},
        {{"cool", "Cool", "Cooler white balance", "Basic"},
         [](float& r, float& g, float& b) { r *= 0.92f, g *= 0.99f, b *= 1.08f; }},
        {{"bleach-bypass", "Bleach Bypass", "Low saturation, high contrast, silvery highlights", "Cinematic"},
         [](float& r, float& g, float& b) {
             saturate(r, g, b, 0.45f);
             r = sCurve(r, 1.45f), g = sCurve(g, 1.45f), b = sCurve(b, 1.45f);
         }},
        {{"faded-film", "Faded Film", "Lifted blacks, softened whites, muted warm colour", "Vintage"},
         [](float& r, float& g, float& b) {
             saturate(r, g, b, 0.8f);
             r = 0.07f + r * 0.88f, g = 0.06f + g * 0.87f, b = 0.05f + b * 0.84f;
         }},
        {{"mono", "Mono", "Black and white (Rec.709 luma) with a slight contrast boost", "Black & White"},
         [](float& r, float& g, float& b) { r = g = b = sCurve(luma709(r, g, b), 1.1f); }},
        {{"sepia", "Antique", "Warm brown monochrome (the Sepia filter is the adjustable version)", "Vintage"},
         [](float& r, float& g, float& b) {
             const float y = luma709(r, g, b);
             r = y * 1.07f + 0.03f, g = y * 0.89f + 0.02f, b = y * 0.70f;
         }},
        {{"vintage", "Vintage", "Cross-processed colour: yellow highlights, blue-green shadows", "Vintage"},
         [](float& r, float& g, float& b) {
             r = sCurve(r, 1.25f);
             g = 0.03f + 0.95f * g;
             b = 0.12f + 0.70f * b;
             saturate(r, g, b, 0.9f);
         }},
        {{"high-contrast", "High Contrast", "Strong S-curve", "Basic"},
         [](float& r, float& g, float& b) { r = sCurve(r, 1.5f), g = sCurve(g, 1.5f), b = sCurve(b, 1.5f); }},
        {{"night", "Night", "Day-for-night: dark, cool and desaturated", "Cinematic"},
         [](float& r, float& g, float& b) {
             saturate(r, g, b, 0.5f);
             r *= 0.55f, g *= 0.65f, b = b * 0.80f + 0.03f;
         }},
        {{"vivid", "Vivid", "Richer colour and a little contrast", "Basic"},
         [](float& r, float& g, float& b) {
             saturate(r, g, b, 1.3f);
             r = sCurve(r, 1.1f), g = sCurve(g, 1.1f), b = sCurve(b, 1.1f);
         }},
        // ---- M11 (MeerCut designs) ----
        {{"soft", "Soft", "Gentle contrast with slightly lifted shadows", "Basic"},
         [](float& r, float& g, float& b) {
             contrast(r, g, b, 0.85f);
             levels(r, g, b, 0.03f, 0.98f);
             saturate(r, g, b, 0.95f);
         }},
        {{"bright", "Bright", "Brighter mid-tones; white and black stay", "Basic"},
         [](float& r, float& g, float& b) {
             lift(r, g, b, 1.35f);
             saturate(r, g, b, 1.05f);
         }},
        {{"punchy", "Punchy", "Strong contrast and rich colour", "Basic"},
         [](float& r, float& g, float& b) {
             contrast(r, g, b, 1.3f);
             saturate(r, g, b, 1.25f);
         }},
        {{"blockbuster", "Blockbuster", "Strong teal and orange with deep blacks", "Cinematic"},
         [](float& r, float& g, float& b) {
             split(r, g, b, -0.08f, 0.02f, 0.10f, 0.10f, 0.03f, -0.08f);
             contrast(r, g, b, 1.3f);
             saturate(r, g, b, 1.1f);
         }},
        {{"moody-blue", "Moody Blue", "Cool, quiet colour with softened highlights", "Cinematic"},
         [](float& r, float& g, float& b) {
             saturate(r, g, b, 0.75f);
             split(r, g, b, -0.02f, 0.0f, 0.06f, -0.02f, 0.0f, 0.02f);
             r *= 0.92f, g *= 0.92f, b *= 0.92f;
             contrast(r, g, b, 1.15f);
         }},
        {{"matte", "Matte", "Lifted blacks, lowered whites and muted colour", "Cinematic"},
         [](float& r, float& g, float& b) {
             contrast(r, g, b, 0.9f);
             levels(r, g, b, 0.08f, 0.92f);
             saturate(r, g, b, 0.85f);
         }},
        {{"golden-hour", "Golden Hour", "Warm, glowing highlights of the hour before sunset", "Cinematic"},
         [](float& r, float& g, float& b) {
             split(r, g, b, 0.0f, 0.0f, 0.0f, 0.12f, 0.06f, -0.06f);
             contrast(r, g, b, 1.05f);
             saturate(r, g, b, 1.1f);
         }},
        {{"dramatic", "Dramatic", "Muted colour, hard contrast and cool shadows", "Cinematic"},
         [](float& r, float& g, float& b) {
             saturate(r, g, b, 0.6f);
             contrast(r, g, b, 1.45f);
             split(r, g, b, -0.03f, 0.0f, 0.04f, 0.02f, 0.01f, 0.0f);
         }},
        {{"instant-film", "Instant Film", "The soft, faded colour of an instant photo print", "Vintage"},
         [](float& r, float& g, float& b) {
             r = 0.05f + r * 0.92f, g = 0.06f + g * 0.90f, b = 0.09f + b * 0.80f;
             split(r, g, b, -0.02f, 0.03f, 0.04f, 0.04f, 0.02f, -0.03f);
             saturate(r, g, b, 0.85f);
         }},
        {{"retro-70s", "Retro 70s", "Orange-brown cast, low contrast, faded blues", "Vintage"},
         [](float& r, float& g, float& b) {
             r *= 1.06f, g *= 0.98f, b *= 0.82f;
             saturate(r, g, b, 0.8f);
             contrast(r, g, b, 0.9f);
             levels(r, g, b, 0.04f, 0.95f);
         }},
        {{"expired-film", "Expired Film", "Green shadows, magenta highlights and faded tones", "Vintage"},
         [](float& r, float& g, float& b) {
             split(r, g, b, -0.02f, 0.06f, 0.0f, 0.08f, -0.02f, 0.06f);
             levels(r, g, b, 0.07f, 0.93f);
             saturate(r, g, b, 0.8f);
         }},
        {{"warm-slide", "Warm Slide", "Rich colour slide film: deep blues, warm reds, crisp contrast", "Vintage"},
         [](float& r, float& g, float& b) {
             contrast(r, g, b, 1.25f);
             saturate(r, g, b, 1.3f);
             r *= 1.04f, b *= 0.95f;
         }},
        {{"dreamy", "Dreamy", "Bright, soft and gently pink", "Mood"},
         [](float& r, float& g, float& b) {
             lift(r, g, b, 1.25f);
             contrast(r, g, b, 0.85f);
             saturate(r, g, b, 0.9f);
             split(r, g, b, 0.02f, 0.0f, 0.04f, 0.04f, 0.02f, 0.03f);
         }},
        {{"pastel", "Pastel", "Light, chalky, gentle colours", "Mood"},
         [](float& r, float& g, float& b) {
             saturate(r, g, b, 0.6f);
             levels(r, g, b, 0.12f, 1.0f);
             contrast(r, g, b, 0.85f);
         }},
        {{"sunset", "Sunset", "Orange highlights over purple shadows", "Mood"},
         [](float& r, float& g, float& b) {
             split(r, g, b, 0.02f, -0.03f, 0.06f, 0.14f, 0.04f, -0.06f);
             saturate(r, g, b, 1.1f);
         }},
        {{"emerald", "Emerald", "Deep, lush greens", "Mood"},
         [](float& r, float& g, float& b) {
             r *= 0.94f, g *= 1.06f, b *= 0.97f;
             saturate(r, g, b, 1.15f);
             contrast(r, g, b, 1.1f);
         }},
        {{"rose", "Rose", "A soft pink glow", "Mood"},
         [](float& r, float& g, float& b) {
             split(r, g, b, 0.04f, -0.01f, 0.03f, 0.06f, 0.0f, 0.03f);
             saturate(r, g, b, 0.95f);
             levels(r, g, b, 0.03f, 1.0f);
         }},
        {{"lavender", "Lavender", "Cool violet tint, light and calm", "Mood"},
         [](float& r, float& g, float& b) {
             r *= 1.02f, g *= 0.95f, b *= 1.08f;
             saturate(r, g, b, 0.85f);
             levels(r, g, b, 0.04f, 1.0f);
         }},
        {{"arctic", "Arctic", "Icy blue and bright", "Mood"},
         [](float& r, float& g, float& b) {
             r *= 0.92f, b *= 1.10f;
             lift(r, g, b, 1.2f);
             saturate(r, g, b, 0.8f);
         }},
        {{"spring", "Spring", "Fresh greens and light, clear colour", "Seasons"},
         [](float& r, float& g, float& b) {
             g *= 1.04f;
             lift(r, g, b, 1.15f);
             saturate(r, g, b, 1.15f);
             split(r, g, b, 0.0f, 0.02f, 0.01f, 0.03f, 0.03f, -0.01f);
         }},
        {{"summer", "Summer", "Warm, saturated sunshine colour", "Seasons"},
         [](float& r, float& g, float& b) {
             r *= 1.05f, g *= 1.02f, b *= 0.92f;
             saturate(r, g, b, 1.2f);
             contrast(r, g, b, 1.1f);
         }},
        {{"autumn", "Autumn", "Greens turned golden, warm reds and oranges", "Seasons"},
         [](float& r, float& g, float& b) {
             hueRotate(r, g, b, -12.0f);
             r *= 1.08f, g *= 0.98f, b *= 0.85f;
             saturate(r, g, b, 1.15f);
         }},
        {{"winter", "Winter", "Cold, pale and bright", "Seasons"},
         [](float& r, float& g, float& b) {
             r *= 0.94f, g *= 0.99f, b *= 1.08f;
             saturate(r, g, b, 0.65f);
             lift(r, g, b, 1.1f);
         }},
        {{"noir", "Noir", "Hard black and white with deep blacks", "Black & White"},
         [](float& r, float& g, float& b) {
             r = g = b = sCurve(luma709(r, g, b), 1.7f);
             levels(r, g, b, -0.02f, 1.0f);
         }},
        {{"silver", "Silver", "Soft, silvery black and white", "Black & White"},
         [](float& r, float& g, float& b) {
             r = g = b = sCurve(luma709(r, g, b), 0.9f);
             levels(r, g, b, 0.06f, 0.96f);
         }},
        {{"selenium", "Selenium", "Black and white with a cool blue-grey tone", "Black & White"},
         [](float& r, float& g, float& b) { monoTone(r, g, b, 0.95f, 0.98f, 1.06f, 0.01f); }},
        {{"warm-bw", "Warm Tone", "Black and white with a warm paper tone", "Black & White"},
         [](float& r, float& g, float& b) { monoTone(r, g, b, 1.05f, 1.0f, 0.9f, 0.01f); }},
        {{"high-key", "High Key", "Bright, airy black and white", "Black & White"},
         [](float& r, float& g, float& b) {
             r = g = b = luma709(r, g, b);
             lift(r, g, b, 1.6f);
             contrast(r, g, b, 0.9f);
         }},
        {{"cyberpunk", "Cyberpunk", "Magenta shadows and cyan highlights", "Creative"},
         [](float& r, float& g, float& b) {
             split(r, g, b, 0.08f, -0.04f, 0.12f, -0.06f, 0.06f, 0.08f);
             saturate(r, g, b, 1.25f);
             contrast(r, g, b, 1.15f);
         }},
        {{"infrared", "Infrared", "False colour: foliage turns pale pink", "Creative"},
         [](float& r, float& g, float& b) {
             const float nr = 0.2f * r + 0.9f * g, ng = 0.4f * r + 0.55f * g, nb = 0.8f * b + 0.15f * g;
             r = nr, g = ng, b = nb;
         }},
        {{"digital-green", "Digital Green", "A cold green computer-screen cast", "Creative"},
         [](float& r, float& g, float& b) {
             r = 0.6f * r, g = g * 1.05f + 0.03f, b = 0.55f * b + 0.03f;
             contrast(r, g, b, 1.2f);
         }},
        {{"pop", "Pop", "Bold, saturated poster colour", "Creative"},
         [](float& r, float& g, float& b) {
             saturate(r, g, b, 1.6f);
             contrast(r, g, b, 1.2f);
         }},
    };
    return v;
}

std::shared_ptr<const Lut3D> bake(const std::string& title, const Fn& fn, int n = 33) {
    auto lut = std::make_shared<Lut3D>();
    lut->title = title;
    lut->size = n;
    lut->table.resize(static_cast<size_t>(n) * n * n * 3);
    size_t k = 0;
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                float x = r / float(n - 1), y = g / float(n - 1), z = b / float(n - 1);
                fn(x, y, z);
                lut->table[k++] = std::clamp(x, 0.0f, 1.0f);
                lut->table[k++] = std::clamp(y, 0.0f, 1.0f);
                lut->table[k++] = std::clamp(z, 0.0f, 1.0f);
            }
    return lut;
}

}  // namespace

const std::vector<std::string>& builtinLookCategories() {
    static const std::vector<std::string> c = {"Basic", "Cinematic", "Vintage", "Mood", "Seasons", "Black & White", "Creative"};
    return c;
}

const std::vector<BuiltinLook>& builtinLooks() {
    static const std::vector<BuiltinLook> v = [] {
        std::vector<BuiltinLook> out;
        for (const auto& d : lookDefs()) out.push_back(d.info);
        return out;
    }();
    return v;
}

std::shared_ptr<const Lut3D> builtinLut(std::string_view id) {
    static std::mutex m;
    static std::map<std::string, std::shared_ptr<const Lut3D>, std::less<>> baked;
    std::lock_guard lk(m);
    if (const auto it = baked.find(id); it != baked.end()) return it->second;
    for (const auto& d : lookDefs())
        if (d.info.id == id) return baked[d.info.id] = bake(d.info.name, d.fn);
    return nullptr;
}

std::shared_ptr<const Lut3D> resolveLut(const std::string& reference, const std::filesystem::path& baseDir, std::string* error) {
    if (reference.rfind(kBuiltinLutPrefix, 0) == 0) {
        auto lut = builtinLut(std::string_view(reference).substr(kBuiltinLutPrefix.size()));
        if (!lut && error) *error = "unknown built-in look " + reference;
        return lut;
    }
    std::filesystem::path p(std::u8string(reference.begin(), reference.end()));   // UTF-8 like media paths
    if (p.is_relative() && !baseDir.empty()) p = baseDir / p;
    return cachedLut(p.lexically_normal(), error);
}

}  // namespace fa
