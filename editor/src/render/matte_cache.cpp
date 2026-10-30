#include "fa/render/matte_cache.h"

#include "fa/core/app_data.h"

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>

namespace fa {

namespace {

uint64_t fnv(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

std::string hex64(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::filesystem::path frameFile(const std::filesystem::path& dir, int64_t frame) {
    return dir / (std::to_string(frame) + ".alpha");
}

// Sliding-window minimum (erode) or maximum (dilate) over 2r+1 samples along a line (O(n), monotonic deque).
void slidingExtreme(const float* in, float* out, int n, int stride, int r, bool maximum) {
    std::deque<int> q;
    auto better = [&](float a, float b) { return maximum ? a >= b : a <= b; };
    int next = 0;
    for (int i = 0; i < n; ++i) {
        const int hi = std::min(n - 1, i + r);
        for (; next <= hi; ++next) {
            while (!q.empty() && better(in[next * stride], in[q.back() * stride])) q.pop_back();
            q.push_back(next);
        }
        while (q.front() < i - r) q.pop_front();
        out[i * stride] = in[q.front() * stride];
    }
}

// Grey-level dilation / erosion with a disc of radius r (the edge moves by r in every direction, not further along
// the diagonals as with a square): the disc is the union of horizontal segments, row dy of half-width
// floor(sqrt(r^2 - dy^2)); each half-width is one pass of row extremes, used for +dy and -dy. O(r w h).
void morph(std::vector<float>& a, int w, int h, int r, bool grow) {
    if (r <= 0) return;
    std::vector<float> out(a.size(), grow ? 0.0f : 1.0f), rows(a.size());
    for (int dy = 0; dy <= r; ++dy) {
        const int hw = static_cast<int>(std::floor(std::sqrt(static_cast<double>(r) * r - static_cast<double>(dy) * dy) + 1e-9));
        for (int y = 0; y < h; ++y)
            slidingExtreme(&a[static_cast<size_t>(y) * w], &rows[static_cast<size_t>(y) * w], w, 1, hw, grow);
        for (int sign : {-1, 1}) {
            if (dy == 0 && sign > 0) break;
            for (int y = 0; y < h; ++y) {
                const int sy = std::clamp(y + sign * dy, 0, h - 1);
                const float* src = &rows[static_cast<size_t>(sy) * w];
                float* dst = &out[static_cast<size_t>(y) * w];
                for (int x = 0; x < w; ++x) dst[x] = grow ? std::max(dst[x], src[x]) : std::min(dst[x], src[x]);
            }
        }
    }
    a.swap(out);
}

// Box blur of radius r along rows then columns (running sums); three passes approximate a Gaussian.
void boxBlur(std::vector<float>& a, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<float> tmp(a.size());
    const float inv = 1.0f / (2 * r + 1);
    for (int y = 0; y < h; ++y) {
        const float* in = &a[static_cast<size_t>(y) * w];
        float* out = &tmp[static_cast<size_t>(y) * w];
        double s = 0;
        for (int i = -r; i <= r; ++i) s += in[std::clamp(i, 0, w - 1)];
        for (int x = 0; x < w; ++x) {
            out[x] = static_cast<float>(s * inv);
            s += in[std::min(w - 1, x + r + 1)] - in[std::max(0, x - r)];
        }
    }
    for (int x = 0; x < w; ++x) {
        double s = 0;
        for (int i = -r; i <= r; ++i) s += tmp[static_cast<size_t>(std::clamp(i, 0, h - 1)) * w + x];
        for (int y = 0; y < h; ++y) {
            a[static_cast<size_t>(y) * w + x] = static_cast<float>(s * inv);
            s += tmp[static_cast<size_t>(std::min(h - 1, y + r + 1)) * w + x] - tmp[static_cast<size_t>(std::max(0, y - r)) * w + x];
        }
    }
}

void gaussian(std::vector<float>& a, int w, int h, double sigma) {
    if (sigma < 0.3) return;
    // three box passes with radius from sigma (variance of a box of width n is (n^2 - 1) / 12)
    const int r = std::max(1, static_cast<int>(std::lround((std::sqrt(4.0 * sigma * sigma + 1.0) - 1.0) / 2.0)));
    for (int pass = 0; pass < 3; ++pass) boxBlur(a, w, h, r);
}

double sample(const std::vector<float>& a, int w, int h, double x, double y) {
    x = std::clamp(x, 0.0, w - 1.0);
    y = std::clamp(y, 0.0, h - 1.0);
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const int x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
    const double fx = x - x0, fy = y - y0;
    const auto at = [&](int xx, int yy) { return a[static_cast<size_t>(yy) * w + xx]; };
    return (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
}

}  // namespace

std::filesystem::path analysisCacheDir() {
    if (const wchar_t* e = _wgetenv(L"MEERCUT_ANALYSIS_CACHE"); e && *e) return std::filesystem::path(e);
    if (const auto d = appDataFolder("analysis"); !d.empty()) return d;   // %LOCALAPPDATA%\MeerCut\analysis
    return std::filesystem::temp_directory_path() / "MeerCut" / "analysis";
}

std::string matteMediaIdentity(const std::filesystem::path& file) {
    std::error_code ec;
    const auto abs = std::filesystem::absolute(file, ec);
    const auto size = std::filesystem::file_size(file, ec);
    const auto mtime = std::filesystem::last_write_time(file, ec).time_since_epoch().count();
    uint64_t head = 0;
    if (std::ifstream in(file, std::ios::binary); in) {
        std::vector<char> buf(65536);
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        head = fnv(buf.data(), static_cast<size_t>(in.gcount()));
    }
    const std::u8string a8 = abs.generic_u8string();
    return std::string(a8.begin(), a8.end()) + "|" + std::to_string(size) + "|" + std::to_string(mtime) + "|" + hex64(head);
}

std::string MatteSetKey::hash() const {
    const std::string s = media + "\n" + model + "\n" + modelSha + "\n" + std::to_string(quality);
    return hex64(fnv(s.data(), s.size())) + hex64(fnv(s.data(), s.size(), 0x84222325cbf29ce4ull));
}

MatteStore::MatteStore(const MatteSetKey& key, const std::filesystem::path& root) : dir_(root / "mattes" / key.hash()) {}

bool MatteStore::has(int64_t frame) const {
    std::error_code ec;
    return std::filesystem::is_regular_file(frameFile(dir_, frame), ec);
}

bool MatteStore::hasAnalysed(int64_t frame) const {
    std::error_code ec;
    return has(frame) && !std::filesystem::exists(dir_ / (std::to_string(frame) + ".preview"), ec);
}

void MatteStore::writePreview(int64_t frame, const AlphaImage& matte) {
    write(frame, matte);
    std::ofstream(dir_ / (std::to_string(frame) + ".preview")) << "single-frame preview";
}

bool MatteStore::read(int64_t frame, AlphaImage& out) const {
    std::ifstream in(frameFile(dir_, frame), std::ios::binary);
    if (!in) return false;
    char magic[4];
    uint32_t w = 0, h = 0, n = 0;
    in.read(magic, 4);
    in.read(reinterpret_cast<char*>(&w), 4);
    in.read(reinterpret_cast<char*>(&h), 4);
    in.read(reinterpret_cast<char*>(&n), 4);
    if (!in || std::memcmp(magic, "LCMT", 4) != 0 || w == 0 || h == 0 || w > 16384 || h > 16384) return false;
    std::vector<uint8_t> comp(n);
    in.read(reinterpret_cast<char*>(comp.data()), n);
    if (!in) return false;
    out.width = static_cast<int>(w);
    out.height = static_cast<int>(h);
    out.a.resize(static_cast<size_t>(w) * h);
    uLongf len = static_cast<uLongf>(out.a.size());
    return uncompress(out.a.data(), &len, comp.data(), n) == Z_OK && len == out.a.size();
}

void MatteStore::write(int64_t frame, const AlphaImage& m) {
    std::filesystem::create_directories(dir_);
    std::vector<uint8_t> comp(compressBound(static_cast<uLong>(m.a.size())));
    uLongf len = static_cast<uLongf>(comp.size());
    compress2(comp.data(), &len, m.a.data(), static_cast<uLong>(m.a.size()), 6);
    const auto target = frameFile(dir_, frame);
    auto tmp = target;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        const uint32_t w = static_cast<uint32_t>(m.width), h = static_cast<uint32_t>(m.height), n = static_cast<uint32_t>(len);
        out.write("LCMT", 4);
        out.write(reinterpret_cast<const char*>(&w), 4);
        out.write(reinterpret_cast<const char*>(&h), 4);
        out.write(reinterpret_cast<const char*>(&n), 4);
        out.write(reinterpret_cast<const char*>(comp.data()), static_cast<std::streamsize>(len));
    }
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    std::filesystem::remove(dir_ / (std::to_string(frame) + ".preview"), ec);   // an analysed frame replaces a preview
}

int64_t MatteStore::count(int64_t first, int64_t last) const {
    int64_t n = 0;
    for (int64_t f = first; f <= last; ++f) n += hasAnalysed(f) ? 1 : 0;
    return n;
}

void MatteStore::touch() const {
    std::error_code ec;
    if (!std::filesystem::exists(dir_, ec)) return;
    std::ofstream(dir_ / ".used", std::ios::trunc) << "used";
}

uint64_t analysisCacheLimitBytes() {
    if (const wchar_t* e = _wgetenv(L"MEERCUT_ANALYSIS_CACHE_MB"); e && *e)   // megabytes (fractions allowed)
        return static_cast<uint64_t>(std::max(0.0, std::wcstod(e, nullptr)) * 1048576.0);
    return 8ull << 30;
}

uint64_t matteCacheBytes(const std::filesystem::path& root) {
    uint64_t n = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root / "mattes", ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) n += it->file_size(ec);
    return n;
}

void trimMatteCache(uint64_t limitBytes, const std::filesystem::path& keep, const std::filesystem::path& root) {
    struct Set {
        std::filesystem::path dir;
        std::filesystem::file_time_type used;
        uint64_t bytes = 0;
    };
    std::vector<Set> sets;
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(root / "mattes", ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        Set s{it->path(), std::filesystem::last_write_time(it->path(), ec)};
        if (std::filesystem::exists(it->path() / ".used", ec)) s.used = std::filesystem::last_write_time(it->path() / ".used", ec);
        for (auto f = std::filesystem::directory_iterator(it->path(), ec); !ec && f != std::filesystem::directory_iterator();
             f.increment(ec))
            if (f->is_regular_file(ec)) s.bytes += f->file_size(ec);
        total += s.bytes;
        sets.push_back(s);
    }
    std::sort(sets.begin(), sets.end(), [](const Set& a, const Set& b) { return a.used < b.used; });
    for (const Set& s : sets) {
        if (total <= limitBytes) break;
        if (!keep.empty() && std::filesystem::equivalent(s.dir, keep, ec)) continue;
        std::filesystem::remove_all(s.dir, ec);
        total -= std::min(total, s.bytes);
    }
}

MatteSetKey cutoutSetKey(const std::filesystem::path& media, const AiCutout& cutout, const std::string& modelSha) {
    return {matteMediaIdentity(media), cutout.model, modelSha, cutout.quality};
}

AlphaImage refineMatte(const AlphaImage& matte, const AlphaImage* prev, const AlphaImage* next, const AiCutout& s) {
    const int w = matte.width, h = matte.height;
    const size_t n = static_cast<size_t>(w) * h;
    std::vector<float> a(n);
    for (size_t i = 0; i < n; ++i) a[i] = matte.a[i] / 255.0f;
    // flicker smoothing: pull towards the neighbours' average only where all three agree closely (static edges that
    // shimmer); a pixel that really changes (movement) is left alone, so nothing ghosts
    const bool usePrev = prev && prev->width == w && prev->height == h;
    const bool useNext = next && next->width == w && next->height == h;
    if (s.smoothing > 0 && (usePrev || useNext)) {
        constexpr float tau = 0.25f;
        for (size_t i = 0; i < n; ++i) {
            float sum = 0, cnt = 0, worst = 0;
            if (usePrev) {
                const float p = prev->a[i] / 255.0f;
                sum += p;
                ++cnt;
                worst = std::max(worst, std::fabs(p - a[i]));
            }
            if (useNext) {
                const float q = next->a[i] / 255.0f;
                sum += q;
                ++cnt;
                worst = std::max(worst, std::fabs(q - a[i]));
            }
            if (worst >= tau) continue;
            const float wgt = static_cast<float>(s.smoothing) * (2.0f / 3.0f) * (1.0f - worst / tau);
            a[i] = (1 - wgt) * a[i] + wgt * (sum / cnt);
        }
    }
    if (s.threshold > 0) {
        const float t = static_cast<float>(std::clamp(s.threshold, 0.0, 0.95));
        for (float& v : a) v = std::clamp((v - t) / (1 - t), 0.0f, 1.0f);
    }
    const int r = static_cast<int>(std::lround(std::fabs(s.edge)));
    if (r > 0) morph(a, w, h, r, s.edge > 0);
    gaussian(a, w, h, s.feather);
    AlphaImage out{w, h, std::vector<uint8_t>(n)};
    for (size_t i = 0; i < n; ++i) out.a[i] = static_cast<uint8_t>(std::lround(std::clamp(a[i], 0.0f, 1.0f) * 255.0f));
    return out;
}

void applyCutout(Image& pic, const AlphaImage& matte, const AlphaImage* prev, const AlphaImage* next, const AiCutout& s) {
    if (matte.empty() || pic.width <= 0) return;
    const AlphaImage ref = refineMatte(matte, prev, next, s);
    const int w = pic.width, h = pic.height;
    const size_t n = static_cast<size_t>(w) * h;
    // the matte at the picture's size (the preview may decode smaller pictures)
    std::vector<float> alpha(n);
    if (ref.width == w && ref.height == h) {
        for (size_t i = 0; i < n; ++i) alpha[i] = ref.a[i] / 255.0f;
    } else {
        std::vector<float> src(ref.a.size());
        for (size_t i = 0; i < src.size(); ++i) src[i] = ref.a[i] / 255.0f;
        const double sx = static_cast<double>(ref.width) / w, sy = static_cast<double>(ref.height) / h;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                alpha[static_cast<size_t>(y) * w + x] =
                    static_cast<float>(sample(src, ref.width, ref.height, (x + 0.5) * sx - 0.5, (y + 0.5) * sy - 0.5));
    }
    // spill cleanup: estimate the local background colour B from pixels weighted by (1 - alpha) at 1/8 size, then
    // recover the subject colour F = (I - (1 - a) B) / a on semi-transparent edge pixels
    std::vector<float> bgR, bgG, bgB;
    int lw = 0, lh = 0;
    if (s.spill > 0) {
        lw = std::max(1, w / 8);
        lh = std::max(1, h / 8);
        std::vector<float> sr(static_cast<size_t>(lw) * lh), sg(sr.size()), sb(sr.size()), sw(sr.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                const size_t j = static_cast<size_t>(std::min(lh - 1, y / 8)) * lw + std::min(lw - 1, x / 8);
                const float wb = 1.0f - alpha[i];
                const uint8_t* p = &pic.pixels[i * 4];
                sr[j] += wb * p[0];
                sg[j] += wb * p[1];
                sb[j] += wb * p[2];
                sw[j] += wb;
            }
        for (auto* v : {&sr, &sg, &sb, &sw}) gaussian(*v, lw, lh, 2.0);
        bgR.resize(sr.size());
        bgG.resize(sr.size());
        bgB.resize(sr.size());
        for (size_t j = 0; j < sr.size(); ++j) {
            const float d = std::max(sw[j], 1e-3f);
            bgR[j] = sr[j] / d;
            bgG[j] = sg[j] / d;
            bgB[j] = sb[j] / d;
        }
    }
    const bool fill = s.background.has_value();
    const Rgba8 bg = fill ? *s.background : Rgba8{};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            uint8_t* p = &pic.pixels[i * 4];
            const float a = alpha[i];
            float c[3] = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
            if (s.spill > 0 && a > 0.02f && a < 0.98f) {
                const double lx = (x + 0.5) / 8.0 - 0.5, ly = (y + 0.5) / 8.0 - 0.5;
                const float b[3] = {static_cast<float>(sample(bgR, lw, lh, lx, ly)), static_cast<float>(sample(bgG, lw, lh, lx, ly)),
                                    static_cast<float>(sample(bgB, lw, lh, lx, ly))};
                for (int k = 0; k < 3; ++k) {
                    const float f = std::clamp((c[k] - (1 - a) * b[k]) / a, 0.0f, 255.0f);
                    c[k] += static_cast<float>(s.spill) * (f - c[k]);
                }
            }
            if (fill) {
                const float bc[3] = {static_cast<float>(bg.r), static_cast<float>(bg.g), static_cast<float>(bg.b)};
                for (int k = 0; k < 3; ++k) p[k] = static_cast<uint8_t>(std::lround(c[k] * a + bc[k] * (1 - a)));
                // the picture keeps its own alpha (a still-transparent source stays so)
            } else {
                for (int k = 0; k < 3; ++k) p[k] = static_cast<uint8_t>(std::lround(c[k]));
                p[3] = static_cast<uint8_t>(std::lround(p[3] * a));
            }
        }
}

}  // namespace fa
