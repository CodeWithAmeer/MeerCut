// C++ port of the frame-index strip used by the Stage 1 test media (tools/make_testmedia.ps1,
// tools/framecode.py): 16 cells of 120x60 px (at 1920 wide) across the top, LSB left, white = 1.
#pragma once

#include "fa/core/image.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fa::test {

struct StripReading {
    int index = -1;       // -1 when ambiguous
    double confidence = 0;
};

inline StripReading readStrip(const Image& img) {
    const double sx = img.width / 1920.0, sy = img.height / 1080.0;
    const int y0 = static_cast<int>(10 * sy), y1 = static_cast<int>(50 * sy);
    int bits = 0;
    double conf = 1.0;
    for (int b = 0; b < 16; ++b) {
        const int x0 = static_cast<int>((b * 120 + 20) * sx), x1 = static_cast<int>((b * 120 + 100) * sx);
        double sum = 0;
        int n = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const Rgba8 c = img.at(x, y);
                sum += (c.r + c.g + c.b) / 3.0;
                ++n;
            }
        const double cell = sum / n / 255.0;
        conf = std::min(conf, std::fabs(cell - 0.5) * 2);
        if (cell > 0.5) bits |= 1 << b;
    }
    return {conf > 0.5 ? bits : -1, conf};
}

inline void drawStrip(Image& img, int index) {
    const double sx = img.width / 1920.0, sy = img.height / 1080.0;
    for (int b = 0; b < 16; ++b) {
        const uint8_t v = (index >> b) & 1 ? 235 : 16;
        for (int y = 0; y < static_cast<int>(60 * sy); ++y)
            for (int x = static_cast<int>(b * 120 * sx); x < static_cast<int>((b + 1) * 120 * sx); ++x)
                img.set(x, y, Rgba8{v, v, v, 255});
    }
}

}  // namespace fa::test
