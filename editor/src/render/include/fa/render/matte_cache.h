// M7 Background Removal: per-source-frame alpha mattes made by an AI matting model (fa_ai/matting.h) and read by the
// renderer, and the refinement applied when a frame is composited.
//
// A matte set belongs to (source media identity, model id + the model file's SHA-256, analysis quality): timeline
// edits (trims, splits, moves, speed changes) never invalidate it, because mattes are stored per *source* frame.
// Frames are zlib-compressed 8-bit alpha at the source resolution, one file per frame, in
//   <analysis cache>/mattes/<set hash>/<frame>.alpha
// The cache has a size limit; whole sets are evicted least recently used first.
#pragma once

#include "fa/core/image.h"
#include "fa/model/project.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fa {

struct AlphaImage {
    int width = 0, height = 0;
    std::vector<uint8_t> a;   // width * height, 0 = background, 255 = subject
    bool empty() const { return a.empty(); }
};

// The analysis cache folder: MEERCUT_ANALYSIS_CACHE if set, else %LOCALAPPDATA%\MeerCut\analysis.
std::filesystem::path analysisCacheDir();
// Identity of a media file for cache keys: absolute path, size, modification time and a hash of its first 64 KB.
std::string matteMediaIdentity(const std::filesystem::path& file);

struct MatteSetKey {
    std::string media;        // matteMediaIdentity()
    std::string model;        // model id, e.g. "rvm-mobilenetv3"
    std::string modelSha;     // SHA-256 of the model file (a different model version = different mattes)
    int quality = 0;          // analysis quality (the matting resolution)
    std::string hash() const; // hex, the set's folder name
};

class MatteStore {
public:
    explicit MatteStore(const MatteSetKey& key, const std::filesystem::path& root = analysisCacheDir());
    const std::filesystem::path& folder() const { return dir_; }
    bool has(int64_t frame) const;          // a matte (analysed or a single-frame preview) the renderer can use
    bool hasAnalysed(int64_t frame) const;  // analysed with the clip's motion context (not a preview)
    bool read(int64_t frame, AlphaImage& out) const;
    void write(int64_t frame, const AlphaImage& matte);          // atomic (temporary file + rename)
    void writePreview(int64_t frame, const AlphaImage& matte);   // marked: the clip analysis recomputes it
    int64_t count(int64_t first, int64_t last) const;     // analysed frames in [first, last] (previews not counted)
    void touch() const;                                  // mark as recently used (eviction order)

private:
    std::filesystem::path dir_;
};

// The size the analysis cache may use: MEERCUT_ANALYSIS_CACHE_MB if set, else 8 GB.
uint64_t analysisCacheLimitBytes();
// Total size of the matte cache and eviction of least recently used sets beyond `limitBytes` (never `keep`).
uint64_t matteCacheBytes(const std::filesystem::path& root = analysisCacheDir());
void trimMatteCache(uint64_t limitBytes, const std::filesystem::path& keep = {},
                    const std::filesystem::path& root = analysisCacheDir());

// The matte set a clip's cutout uses (its model's file hash comes from the caller: fa_render does not know models).
MatteSetKey cutoutSetKey(const std::filesystem::path& media, const AiCutout& cutout, const std::string& modelSha);

// Refines a matte with the clip's settings and writes it into the picture's alpha (straight alpha, multiplied with
// any alpha the picture had): threshold, edge shift (shrink / grow), feather, flicker smoothing against the previous
// and next frames' mattes (only where they differ little, so movement does not ghost), and spill cleanup (edge colours
// decontaminated from the local background colour). `prev` / `next` may be null. Deterministic.
void applyCutout(Image& picture, const AlphaImage& matte, const AlphaImage* prev, const AlphaImage* next,
                 const AiCutout& settings);
// The refined matte alone (what applyCutout writes into alpha), for tests and the inspector's matte view.
AlphaImage refineMatte(const AlphaImage& matte, const AlphaImage* prev, const AlphaImage* next, const AiCutout& settings);

}  // namespace fa
