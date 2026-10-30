#include "fa/render/frame_store.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

namespace fa {

namespace {

uint64_t fnv(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

std::string hex64(uint64_t v) {
    static const char* d = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i, v >>= 4) s[static_cast<size_t>(i)] = d[v & 15];
    return s;
}

}  // namespace

std::string FrameSetKey::hash() const {
    const std::string s = kind + "\n" + media + "\n" + modelSha + "\n" + settings;
    return hex64(fnv(s.data(), s.size())) + hex64(fnv(s.data(), s.size(), 0x84222325cbf29ce4ull));
}

FrameStore::FrameStore(const FrameSetKey& key, const std::filesystem::path& root) : dir_(root / key.kind / key.hash()) {}

bool FrameStore::has(const std::string& name) const {
    std::error_code ec;
    return std::filesystem::is_regular_file(dir_ / (name + ".frame"), ec) || isMarked(name);
}

bool FrameStore::isMarked(const std::string& name) const {
    std::error_code ec;
    return std::filesystem::is_regular_file(dir_ / (name + ".mark"), ec);
}

void FrameStore::mark(const std::string& name) {
    std::filesystem::create_directories(dir_);
    std::ofstream(dir_ / (name + ".mark")) << "no picture";
}

bool FrameStore::read(const std::string& name, Image& out) const {
    std::ifstream in(dir_ / (name + ".frame"), std::ios::binary);
    if (!in) return false;
    char magic[4];
    uint32_t w = 0, h = 0, n = 0;
    in.read(magic, 4);
    in.read(reinterpret_cast<char*>(&w), 4);
    in.read(reinterpret_cast<char*>(&h), 4);
    in.read(reinterpret_cast<char*>(&n), 4);
    if (!in || std::memcmp(magic, "LCFR", 4) != 0 || w == 0 || h == 0 || w > 16384 || h > 16384) return false;
    std::vector<uint8_t> comp(n);
    in.read(reinterpret_cast<char*>(comp.data()), n);
    if (!in) return false;
    out.reset(static_cast<int>(w), static_cast<int>(h));
    uLongf len = static_cast<uLongf>(out.pixels.size());
    if (uncompress(out.pixels.data(), &len, comp.data(), n) != Z_OK || len != out.pixels.size()) return false;
    // stored as left-difference rows (better compression for natural pictures): undo
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = &out.pixels[static_cast<size_t>(y) * w * 4];
        for (uint32_t x = 1; x < w; ++x)
            for (int c = 0; c < 4; ++c) row[x * 4 + c] = static_cast<uint8_t>(row[x * 4 + c] + row[(x - 1) * 4 + c]);
    }
    return true;
}

void FrameStore::write(const std::string& name, const Image& pic) {
    std::filesystem::create_directories(dir_);
    std::vector<uint8_t> diff(pic.pixels.size());
    const size_t w = static_cast<size_t>(pic.width);
    for (int y = 0; y < pic.height; ++y) {
        const uint8_t* row = &pic.pixels[static_cast<size_t>(y) * w * 4];
        uint8_t* d = &diff[static_cast<size_t>(y) * w * 4];
        for (int c = 0; c < 4; ++c) d[c] = row[c];
        for (size_t x = 1; x < w; ++x)
            for (int c = 0; c < 4; ++c) d[x * 4 + c] = static_cast<uint8_t>(row[x * 4 + c] - row[(x - 1) * 4 + c]);
    }
    std::vector<uint8_t> comp(compressBound(static_cast<uLong>(diff.size())));
    uLongf len = static_cast<uLongf>(comp.size());
    compress2(comp.data(), &len, diff.data(), static_cast<uLong>(diff.size()), 3);
    const auto target = dir_ / (name + ".frame");
    auto tmp = target;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        const uint32_t ww = static_cast<uint32_t>(pic.width), hh = static_cast<uint32_t>(pic.height), n = static_cast<uint32_t>(len);
        out.write("LCFR", 4);
        out.write(reinterpret_cast<const char*>(&ww), 4);
        out.write(reinterpret_cast<const char*>(&hh), 4);
        out.write(reinterpret_cast<const char*>(&n), 4);
        out.write(reinterpret_cast<const char*>(comp.data()), static_cast<std::streamsize>(len));
    }
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
}

void FrameStore::touch() const {
    std::error_code ec;
    if (!std::filesystem::exists(dir_, ec)) return;
    std::ofstream(dir_ / ".used", std::ios::trunc) << "used";
}

namespace {
std::mutex& identityMutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, std::string>& identities() {
    static std::map<std::string, std::string> m;
    return m;
}
}  // namespace

void setModelIdentity(const std::string& role, const std::string& sha) {
    std::lock_guard lock(identityMutex());
    identities()[role] = sha;
}

std::string modelIdentity(const std::string& role) {
    std::lock_guard lock(identityMutex());
    const auto it = identities().find(role);
    return it == identities().end() ? std::string() : it->second;
}

std::string interpolationFrameName(int64_t a, int64_t b, double t) {
    return std::to_string(a) + "_" + std::to_string(b) + "_" + std::to_string(quantiseStep(t));
}

uint64_t frameStoreBytes(const std::string& kind, const std::filesystem::path& root) {
    uint64_t n = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root / kind, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) n += it->file_size(ec);
    return n;
}

void trimFrameStores(const std::string& kind, uint64_t limitBytes, const std::filesystem::path& keep,
                     const std::filesystem::path& root) {
    struct Set {
        std::filesystem::path dir;
        std::filesystem::file_time_type used;
        uint64_t bytes = 0;
    };
    std::vector<Set> sets;
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(root / kind, ec); !ec && it != std::filesystem::directory_iterator();
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

Image resizeBilinear(const Image& src, int w, int h) {
    if (src.width == w && src.height == h) return src;
    Image out(w, h);
    const double sx = static_cast<double>(src.width) / w, sy = static_cast<double>(src.height) / h;
    for (int y = 0; y < h; ++y) {
        const double fy = std::clamp((y + 0.5) * sy - 0.5, 0.0, src.height - 1.0);
        const int y0 = static_cast<int>(fy), y1 = std::min(src.height - 1, y0 + 1);
        const double ty = fy - y0;
        for (int x = 0; x < w; ++x) {
            const double fx = std::clamp((x + 0.5) * sx - 0.5, 0.0, src.width - 1.0);
            const int x0 = static_cast<int>(fx), x1 = std::min(src.width - 1, x0 + 1);
            const double tx = fx - x0;
            for (int c = 0; c < 4; ++c) {
                auto at = [&](int xx, int yy) { return src.pixels[(static_cast<size_t>(yy) * src.width + xx) * 4 + c]; };
                const double v = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
                out.pixels[(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<uint8_t>(v + 0.5);
            }
        }
    }
    return out;
}

namespace {

// One separable Lanczos-3 pass along rows (horizontal) or columns, on premultiplied float RGBA.
void lanczosPass(const std::vector<float>& in, int inW, int inH, std::vector<float>& out, int outW, int outH, bool horizontal) {
    const int inLen = horizontal ? inW : inH, outLen = horizontal ? outW : outH;
    const double scale = static_cast<double>(outLen) / inLen;
    const double support = 3.0 * std::max(1.0, 1.0 / scale);   // widened when shrinking (anti-aliasing)
    const double fscale = std::min(1.0, scale);
    auto lanczos = [](double x) {
        x = std::abs(x);
        if (x < 1e-9) return 1.0;
        if (x >= 3.0) return 0.0;
        const double px = 3.14159265358979323846 * x;
        return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
    };
    struct Tap {
        int first = 0;
        std::vector<double> w;
    };
    std::vector<Tap> taps(static_cast<size_t>(outLen));
    for (int o = 0; o < outLen; ++o) {
        const double centre = (o + 0.5) / scale - 0.5;
        Tap& t = taps[static_cast<size_t>(o)];
        t.first = static_cast<int>(std::floor(centre - support)) + 1;
        const int last = static_cast<int>(std::floor(centre + support));
        double sum = 0;
        for (int i = t.first; i <= last; ++i) sum += t.w.emplace_back(lanczos((i - centre) * fscale));
        for (double& w : t.w) w /= sum;
    }
    out.assign(static_cast<size_t>(outW) * outH * 4, 0.0f);
    const int lines = horizontal ? inH : inW;
    for (int l = 0; l < lines; ++l)
        for (int o = 0; o < outLen; ++o) {
            const Tap& t = taps[static_cast<size_t>(o)];
            double acc[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < t.w.size(); ++k) {
                const int i = std::clamp(t.first + static_cast<int>(k), 0, inLen - 1);
                const size_t src = horizontal ? (static_cast<size_t>(l) * inW + i) * 4 : (static_cast<size_t>(i) * inW + l) * 4;
                for (int c = 0; c < 4; ++c) acc[c] += t.w[k] * in[src + c];
            }
            const size_t dst = horizontal ? (static_cast<size_t>(l) * outW + o) * 4 : (static_cast<size_t>(o) * outW + l) * 4;
            for (int c = 0; c < 4; ++c) out[dst + c] = static_cast<float>(acc[c]);
        }
}

}  // namespace

Image resizeLanczos(const Image& src, int w, int h) {
    if (src.width == w && src.height == h) return src;
    std::vector<float> a(src.pixels.size()), b, c;
    for (size_t i = 0; i < src.pixels.size(); i += 4) {
        const float alpha = src.pixels[i + 3] / 255.0f;
        for (int k = 0; k < 3; ++k) a[i + k] = src.pixels[i + k] * alpha;
        a[i + 3] = alpha;
    }
    lanczosPass(a, src.width, src.height, b, w, src.height, true);
    lanczosPass(b, w, src.height, c, w, h, false);
    Image out(w, h);
    for (size_t i = 0; i < out.pixels.size(); i += 4) {
        const float alpha = std::clamp(c[i + 3], 0.0f, 1.0f);
        for (int k = 0; k < 3; ++k)
            out.pixels[i + k] = static_cast<uint8_t>(std::clamp(alpha > 1e-4f ? c[i + k] / alpha : 0.0f, 0.0f, 255.0f) + 0.5f);
        out.pixels[i + 3] = static_cast<uint8_t>(alpha * 255.0f + 0.5f);
    }
    return out;
}

std::pair<int, int> upscaleTarget(const Clip& clip, int sw, int sh, int cw, int ch) {
    if (sw <= 0 || sh <= 0 || cw <= 0 || ch <= 0) return {0, 0};
    // shown size at scale 1: fitted into the canvas
    const double fit = std::min(static_cast<double>(cw) / sw, static_cast<double>(ch) / sh);
    double zoom = 1.0;
    for (const auto* a : {&clip.transform.scaleX, &clip.transform.scaleY}) {
        zoom = std::max(zoom, a->constantValue());
        for (const auto& k : a->keys()) zoom = std::max(zoom, k.value);
    }
    const double factor = std::min(fit * zoom, static_cast<double>(std::max(1, clip.upscale.scale)));
    if (factor <= 1.0 + 1e-6) return {0, 0};
    auto even = [](double x) { return std::max(2, static_cast<int>(std::lround(x / 2)) * 2); };
    return {even(sw * factor), even(sh * factor)};
}

}  // namespace fa
