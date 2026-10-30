// Render Preview cache (M5): rendered preview frames kept on disk and shown instead of rendering again.
//
// Content keys: a frame's key covers everything its pixels depend on - every clip on screen at that time (all of its
// settings: transform, effects, masks, speed, tracking, stabilisation, titles...) with the identity of its media file
// (path, size, modification time) and of the LUT files its effects use, the clips' tracks (order, kind, visibility,
// caption style), the sequence (size, rate), the frame number, the preview size, the compositing backend, hardware
// decoding and the renderer version. Any edit changes the keys of exactly the frames it can affect, so a stale frame
// is never shown; the old files age out of the size limit (least recently used first).
//
// Safety: a frame is written to a temporary file and renamed into place, so a crash never leaves a half-written frame
// under a real key; a file that does not decode completely is treated as missing and deleted. Export never reads this
// cache (renderProject has no access to it).
#pragma once

#include "fa/core/image.h"
#include "fa/model/project.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fa {

// bump when rendering changes in a way keys do not capture (2: M10, enlarged titles and stickers drawn sharp)
inline constexpr int kRenderCacheVersion = 2;

struct FrameKey {
    uint64_t hi = 0, lo = 0;
    friend bool operator==(const FrameKey&, const FrameKey&) = default;
    friend auto operator<=>(const FrameKey&, const FrameKey&) = default;
    std::string hex() const;
};

// Keys of the frames of one sequence for one preview configuration (built once per project snapshot).
class FrameKeyer {
public:
    struct Config {
        int width = 0, height = 0;   // preview canvas
        std::string backend;         // compositing backend in use (outputs may differ by a level between backends)
        bool hardwareDecoding = false;
    };
    FrameKeyer(const Project& project, const Sequence& sequence, const std::filesystem::path& projectFile, Config config);
    FrameKey key(int64_t frame) const;

private:
    struct Span {          // a clip's (or a transition's) reach on its track and its hash
        int64_t from = 0;  // first frame it can affect
        int64_t to = 0;    // one past the last
        uint64_t hash = 0;
        int64_t maxTo = 0; // the largest `to` of this and all earlier spans (stops the backward search)
    };
    std::vector<std::vector<Span>> tracks_;   // picture tracks in compositing order, spans sorted by `from`
    std::vector<uint64_t> trackHash_;
    uint64_t base_ = 0;
};

class RenderCache {
public:
    explicit RenderCache(std::filesystem::path dir, uint64_t limitBytes = 8ull << 30);
    const std::filesystem::path& dir() const { return dir_; }
    void setLimit(uint64_t bytes);   // evicts at once if needed
    uint64_t limit() const;

    bool contains(const FrameKey& key) const;
    std::optional<Image> load(const FrameKey& key);   // nullopt if missing or unreadable (then deleted)
    bool store(const FrameKey& key, const Image& image);
    void clear();
    uint64_t sizeBytes() const;
    size_t frameCount() const;
    uint64_t changeCount() const;   // increases whenever frames are added or removed (timeline indicator refresh)

private:
    struct Entry {
        uint64_t bytes = 0;
        uint64_t lastUse = 0;
    };
    std::filesystem::path fileOf(const FrameKey& key) const;
    void evictLocked();
    void removeLocked(const FrameKey& key);

    std::filesystem::path dir_;
    mutable std::mutex m_;
    std::map<FrameKey, Entry> index_;
    uint64_t total_ = 0;
    uint64_t limit_ = 0;
    uint64_t clock_ = 0;
    uint64_t changes_ = 0;
};

}  // namespace fa
