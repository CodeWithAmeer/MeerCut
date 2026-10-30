// CPU image buffer used between decoding, compositing and encoding.
//
// Pixel format: 8-bit RGBA, straight (non-premultiplied) alpha, gamma-encoded BT.709 RGB,
// rows tightly packed (stride = width * 4). This matches how Filmora mixes and blends
// (encoded RGB, Stage 1 EXP-001/011); a linear-light float path is planned for the GPU backend.
#pragma once

#include <cstdint>
#include <vector>

namespace fa {

struct Rgba8 {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    friend bool operator==(const Rgba8&, const Rgba8&) = default;
};

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;   // width * height * 4

    Image() = default;
    Image(int w, int h, Rgba8 fill = {}) { reset(w, h, fill); }

    void reset(int w, int h, Rgba8 fill = {}) {
        width = w;
        height = h;
        pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i] = fill.r;
            pixels[i + 1] = fill.g;
            pixels[i + 2] = fill.b;
            pixels[i + 3] = fill.a;
        }
    }
    bool empty() const { return width <= 0 || height <= 0; }
    int stride() const { return width * 4; }
    uint8_t* row(int y) { return pixels.data() + static_cast<size_t>(y) * stride(); }
    const uint8_t* row(int y) const { return pixels.data() + static_cast<size_t>(y) * stride(); }
    Rgba8 at(int x, int y) const {
        const uint8_t* p = row(y) + x * 4;
        return {p[0], p[1], p[2], p[3]};
    }
    void set(int x, int y, Rgba8 c) {
        uint8_t* p = row(y) + x * 4;
        p[0] = c.r;
        p[1] = c.g;
        p[2] = c.b;
        p[3] = c.a;
    }
};

}  // namespace fa
