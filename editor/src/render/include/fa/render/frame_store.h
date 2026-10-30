// M7 AI Interpolation and Video Upscaling: pictures made by a model before an export or Render Preview (fa_ai) and
// read by the renderer, kept in the analysis cache outside the project like the Background Removal mattes:
//   <analysis cache>/<kind>/<set hash>/<name>.frame      (RGBA, zlib)
// A set belongs to (media identity, model SHA-256, settings), so timeline edits never invalidate it. A name may also
// carry a marker instead of a picture (an interpolation across a shot change: nothing is invented there).
#pragma once

#include "fa/core/image.h"
#include "fa/model/project.h"
#include "fa/render/matte_cache.h"

#include <filesystem>
#include <string>
#include <utility>

namespace fa {

struct FrameSetKey {
    std::string kind;      // "rife" or "upscale"
    std::string media;     // matteMediaIdentity()
    std::string modelSha;  // the model's weights (empty for non-AI pictures)
    std::string settings;  // what else changes the pictures (scale, size, deflicker ...)
    std::string hash() const;
};

class FrameStore {
public:
    explicit FrameStore(const FrameSetKey& key, const std::filesystem::path& root = analysisCacheDir());
    const std::filesystem::path& folder() const { return dir_; }
    bool has(const std::string& name) const;
    bool read(const std::string& name, Image& out) const;
    void write(const std::string& name, const Image& picture);   // atomic
    bool isMarked(const std::string& name) const;                // a marker instead of a picture
    void mark(const std::string& name);
    void touch() const;

private:
    std::filesystem::path dir_;
};

// The identity (weights SHA-256) of the model behind prepared pictures of a role ("rife", "upscale:<model>:<scale>",
// "cutout:<model id>" for Background Removal clips that do not record their model),
// registered by fa_ai (ai::registerModelIdentities); empty = unknown (prepared pictures are not used).
void setModelIdentity(const std::string& role, const std::string& sha);
std::string modelIdentity(const std::string& role);

// The name of an in-between frame: source frames a and b and the time step quantised to 1/256.
std::string interpolationFrameName(int64_t a, int64_t b, double t);
inline int quantiseStep(double t) { return static_cast<int>(t * 256.0 + 0.5); }

// Total bytes of one kind's sets and least-recently-used eviction (never `keep`).
uint64_t frameStoreBytes(const std::string& kind, const std::filesystem::path& root = analysisCacheDir());
void trimFrameStores(const std::string& kind, uint64_t limitBytes, const std::filesystem::path& keep = {},
                     const std::filesystem::path& root = analysisCacheDir());

// Bilinear resize (frames read from a store at another size than the decoder delivered, e.g. preview decoding).
Image resizeBilinear(const Image& src, int width, int height);
// Lanczos-3 resize (separable, the kernel widened when shrinking; colours filtered premultiplied by alpha so that
// transparent pixels do not bleed): the non-AI upscaling mode and the final size step of the AI one.
Image resizeLanczos(const Image& src, int width, int height);

// The size an upscaled clip is prepared at: the size it is shown at on a canvasW x canvasH canvas (fitted, times its
// largest transform scale), but never more than the source times the chosen factor; {0, 0} when that is not larger
// than the source (nothing to upscale).
std::pair<int, int> upscaleTarget(const Clip& clip, int sourceW, int sourceH, int canvasW, int canvasH);

}  // namespace fa
