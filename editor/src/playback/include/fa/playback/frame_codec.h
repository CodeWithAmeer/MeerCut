// Lossless RGBA frame codec for the Render Preview cache (M5): an own implementation of the QOI image format
// ("Quite OK Image", public specification by Dominic Szablewski, qoiformat.org). Byte-oriented, no dependencies;
// a 960x540 preview frame encodes / decodes in a few milliseconds, and a cached frame is bit-identical to the
// rendered one.
#pragma once

#include "fa/core/image.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace fa {

std::vector<uint8_t> encodeFrame(const Image& image);
// nullopt for anything that is not a complete, well-formed 4-channel QOI image (truncated or corrupt files).
std::optional<Image> decodeFrame(const uint8_t* data, size_t size);

}  // namespace fa
