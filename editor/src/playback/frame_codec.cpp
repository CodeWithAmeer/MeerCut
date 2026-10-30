#include "fa/playback/frame_codec.h"

#include <array>
#include <cstring>

namespace fa {

namespace {

constexpr uint8_t kOpIndex = 0x00, kOpDiff = 0x40, kOpLuma = 0x80, kOpRun = 0xC0, kOpRgb = 0xFE, kOpRgba = 0xFF;
constexpr uint8_t kMask2 = 0xC0;
constexpr std::array<uint8_t, 8> kEnd{0, 0, 0, 0, 0, 0, 0, 1};
constexpr size_t kHeader = 14;

struct Px {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    bool operator==(const Px&) const = default;
};

int slot(const Px& p) { return (p.r * 3 + p.g * 5 + p.b * 7 + p.a * 11) % 64; }

void put32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

}  // namespace

std::vector<uint8_t> encodeFrame(const Image& image) {
    std::vector<uint8_t> out;
    const size_t n = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
    out.reserve(kHeader + n * 2 + kEnd.size());
    out.insert(out.end(), {'q', 'o', 'i', 'f'});
    put32(out, static_cast<uint32_t>(image.width));
    put32(out, static_cast<uint32_t>(image.height));
    out.push_back(4);   // RGBA
    out.push_back(0);   // sRGB with linear alpha
    std::array<Px, 64> index{};
    for (auto& p : index) p = Px{0, 0, 0, 0};
    Px prev;
    int run = 0;
    const uint8_t* src = image.pixels.data();
    for (size_t i = 0; i < n; ++i, src += 4) {
        const Px px{src[0], src[1], src[2], src[3]};
        if (px == prev) {
            if (++run == 62 || i + 1 == n) {
                out.push_back(static_cast<uint8_t>(kOpRun | (run - 1)));
                run = 0;
            }
            continue;
        }
        if (run > 0) {
            out.push_back(static_cast<uint8_t>(kOpRun | (run - 1)));
            run = 0;
        }
        const int s = slot(px);
        if (index[static_cast<size_t>(s)] == px) {
            out.push_back(static_cast<uint8_t>(kOpIndex | s));
        } else {
            index[static_cast<size_t>(s)] = px;
            if (px.a == prev.a) {
                const int vr = static_cast<int8_t>(px.r - prev.r), vg = static_cast<int8_t>(px.g - prev.g),
                          vb = static_cast<int8_t>(px.b - prev.b);
                const int vgr = vr - vg, vgb = vb - vg;
                if (vr >= -2 && vr <= 1 && vg >= -2 && vg <= 1 && vb >= -2 && vb <= 1) {
                    out.push_back(static_cast<uint8_t>(kOpDiff | (vr + 2) << 4 | (vg + 2) << 2 | (vb + 2)));
                } else if (vgr >= -8 && vgr <= 7 && vg >= -32 && vg <= 31 && vgb >= -8 && vgb <= 7) {
                    out.push_back(static_cast<uint8_t>(kOpLuma | (vg + 32)));
                    out.push_back(static_cast<uint8_t>((vgr + 8) << 4 | (vgb + 8)));
                } else {
                    out.insert(out.end(), {kOpRgb, px.r, px.g, px.b});
                }
            } else {
                out.insert(out.end(), {kOpRgba, px.r, px.g, px.b, px.a});
            }
        }
        prev = px;
    }
    out.insert(out.end(), kEnd.begin(), kEnd.end());
    return out;
}

std::optional<Image> decodeFrame(const uint8_t* data, size_t size) {
    if (!data || size < kHeader + kEnd.size() || std::memcmp(data, "qoif", 4) != 0) return std::nullopt;
    const uint32_t w = get32(data + 4), h = get32(data + 8);
    if (w == 0 || h == 0 || w > 16384 || h > 16384 || data[12] != 4) return std::nullopt;
    Image img;
    img.width = static_cast<int>(w);
    img.height = static_cast<int>(h);
    const size_t n = static_cast<size_t>(w) * h;
    img.pixels.resize(n * 4);
    std::array<Px, 64> index{};
    for (auto& p : index) p = Px{0, 0, 0, 0};
    Px px;
    size_t pos = kHeader;
    const size_t end = size - kEnd.size();
    int run = 0;
    uint8_t* dst = img.pixels.data();
    for (size_t i = 0; i < n; ++i, dst += 4) {
        if (run > 0) {
            --run;
        } else {
            if (pos >= end) return std::nullopt;   // truncated
            const uint8_t b1 = data[pos++];
            if (b1 == kOpRgb) {
                if (pos + 3 > end) return std::nullopt;
                px.r = data[pos];
                px.g = data[pos + 1];
                px.b = data[pos + 2];
                pos += 3;
            } else if (b1 == kOpRgba) {
                if (pos + 4 > end) return std::nullopt;
                px = Px{data[pos], data[pos + 1], data[pos + 2], data[pos + 3]};
                pos += 4;
            } else if ((b1 & kMask2) == kOpIndex) {
                px = index[b1];
            } else if ((b1 & kMask2) == kOpDiff) {
                px.r = static_cast<uint8_t>(px.r + ((b1 >> 4) & 3) - 2);
                px.g = static_cast<uint8_t>(px.g + ((b1 >> 2) & 3) - 2);
                px.b = static_cast<uint8_t>(px.b + (b1 & 3) - 2);
            } else if ((b1 & kMask2) == kOpLuma) {
                if (pos + 1 > end) return std::nullopt;
                const uint8_t b2 = data[pos++];
                const int vg = (b1 & 0x3F) - 32;
                px.r = static_cast<uint8_t>(px.r + vg - 8 + ((b2 >> 4) & 0x0F));
                px.g = static_cast<uint8_t>(px.g + vg);
                px.b = static_cast<uint8_t>(px.b + vg - 8 + (b2 & 0x0F));
            } else {   // run
                run = b1 & 0x3F;
            }
            index[static_cast<size_t>(slot(px))] = px;
        }
        dst[0] = px.r;
        dst[1] = px.g;
        dst[2] = px.b;
        dst[3] = px.a;
    }
    if (run != 0 || pos != end || std::memcmp(data + end, kEnd.data(), kEnd.size()) != 0) return std::nullopt;
    return img;
}

}  // namespace fa
