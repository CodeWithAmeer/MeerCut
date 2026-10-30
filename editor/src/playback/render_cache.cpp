#include "fa/playback/render_cache.h"

#include "fa/model/serialize.h"
#include "fa/model/time_map.h"
#include "fa/playback/frame_codec.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <fstream>
#include <thread>
#include <tuple>

namespace fa {

namespace {

uint64_t splitmix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// Two independent 64-bit chains = a 128-bit key.
struct Hasher {
    uint64_t a = 0x243F6A8885A308D3ULL, b = 0x13198A2E03707344ULL;
    void add(uint64_t v) {
        a = splitmix(a ^ v);
        b = splitmix(b + v * 0xA0761D6478BD642FULL + (b << 23 | b >> 41));
    }
    void add(const std::string& s) {
        uint64_t f1 = 0xCBF29CE484222325ULL, f2 = 0x84222325CBF29CE4ULL;
        for (const unsigned char c : s) {
            f1 = (f1 ^ c) * 0x100000001B3ULL;
            f2 = (f2 ^ c) * 0x1000193ULL + 0x9E37ULL;
        }
        add(f1);
        add(f2 ^ s.size());
    }
    FrameKey key() const { return {a, b}; }
};

uint64_t hashOf(const std::string& s) {
    Hasher h;
    h.add(s);
    return h.a ^ splitmix(h.b);
}

bool pictureTrack(const Track& t) { return t.kind == TrackKind::Video || t.kind == TrackKind::Subtitle; }

}  // namespace

std::string FrameKey::hex() const {
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(hi), static_cast<unsigned long long>(lo));
    return buf;
}

// ---- keys ------------------------------------------------------------------------------------------

FrameKeyer::FrameKeyer(const Project& project, const Sequence& seq, const std::filesystem::path& projectFile, Config config) {
    Hasher base;
    base.add(static_cast<uint64_t>(kRenderCacheVersion));
    base.add(static_cast<uint64_t>(seq.width) << 32 | static_cast<uint32_t>(seq.height));
    base.add(static_cast<uint64_t>(seq.frameRate.fps().num()));
    base.add(static_cast<uint64_t>(seq.frameRate.fps().den()));
    base.add(static_cast<uint64_t>(config.width) << 32 | static_cast<uint32_t>(config.height));
    base.add(config.backend);
    base.add(static_cast<uint64_t>(config.hardwareDecoding));
    base_ = base.a ^ splitmix(base.b);

    auto fileIdentity = [&](const std::string& path) {   // resolved like the renderer resolves media and LUT files
        MediaSource s;
        s.path = path;
        return mediaIdentity(resolveMediaPath(s, projectFile));
    };
    const FrameRate rate = seq.frameRate;
    for (const Track& tr : seq.tracks) {
        if (!pictureTrack(tr)) continue;
        // the track's own settings that can change pixels (not its clips; lock / mute / volume do not)
        Track settings = tr;
        settings.clips.clear();
        settings.locked = false;
        settings.muted = false;
        settings.volumeDb = 0.0;
        settings.solo = false;
        Project one;
        Sequence s;
        s.id = "k";
        s.tracks.push_back(std::move(settings));
        one.sequences.push_back(std::move(s));
        trackHash_.push_back(hashOf(projectToJsonString(one, -1)));
        // A transition shows both clips around the cut (whatever its alignment, at most its duration on either side):
        // a clip's reach grows by the transitions at its ends, and each transition is a span of its own, so adding
        // or changing one only touches the frames around its cut. Clip hashes leave the transition out.
        auto framesOf = [&](const Time& t) { return timeToFrame(t, rate, Rounding::Ceil); };
        std::vector<Span> spans;
        for (size_t i = 0; i < tr.clips.size(); ++i) {
            const Clip& c = tr.clips[i];
            Clip own = c;
            own.outTransition.reset();
            own.colorMark = 0;   // M9: labels and mute do not change the picture
            own.muted = false;
            Hasher h;
            h.add(clipToJsonString(own, -1));
            if (const MediaSource* src = project.findSource(c.sourceId)) {
                h.add(fileIdentity(src->path));
                h.add(static_cast<uint64_t>(src->width) << 32 | static_cast<uint32_t>(src->height));
            }
            for (const EffectRef& e : c.effects)   // LUT files are part of the look
                for (const auto& [name, value] : e.strings)
                    if (name == "lut" && !value.empty()) h.add(fileIdentity(value));
            const Clip* prev = i > 0 ? &tr.clips[i - 1] : nullptr;
            Span sp;
            sp.from = timeToFrame(c.timelineStart, rate, Rounding::Floor) - 1 -
                      (prev && prev->outTransition ? framesOf(prev->outTransition->duration) : 0);
            sp.to = framesOf(c.timelineEnd()) + 1 + (c.outTransition ? framesOf(c.outTransition->duration) : 0);
            sp.hash = h.a ^ splitmix(h.b);
            spans.push_back(sp);
            if (c.outTransition) {
                Clip onlyTransition;
                onlyTransition.outTransition = c.outTransition;
                Hasher th;
                th.add(clipToJsonString(onlyTransition, -1));
                const int64_t cut = timeToFrame(c.timelineEnd(), rate, Rounding::Floor), d = framesOf(c.outTransition->duration);
                spans.push_back(Span{cut - d - 1, cut + d + 1, th.a ^ splitmix(th.b)});
            }
        }
        std::sort(spans.begin(), spans.end(), [](const Span& x, const Span& y) {   // a total order: canonical keys
            return std::tie(x.from, x.to, x.hash) < std::tie(y.from, y.to, y.hash);
        });
        int64_t maxTo = INT64_MIN;
        for (Span& sp : spans) sp.maxTo = maxTo = std::max(maxTo, sp.to);
        tracks_.push_back(std::move(spans));
    }
}

FrameKey FrameKeyer::key(int64_t frame) const {
    Hasher h;
    h.add(base_);
    h.add(static_cast<uint64_t>(frame));
    for (size_t t = 0; t < tracks_.size(); ++t) {
        const auto& spans = tracks_[t];
        // spans whose reach contains the frame: scan back from the last one starting at or before it, until no
        // earlier span reaches this far (spans are sorted by start; maxTo is the running maximum of their ends)
        auto it = std::upper_bound(spans.begin(), spans.end(), frame, [](int64_t f, const Span& s) { return f < s.from; });
        bool any = false;
        for (auto j = it; j != spans.begin();) {
            --j;
            if (j->maxTo <= frame) break;
            if (j->to <= frame) continue;
            if (!any) {
                h.add(static_cast<uint64_t>(t) << 32 | 0x7A11u);
                h.add(trackHash_[t]);
                any = true;
            }
            h.add(j->hash);
        }
    }
    return h.key();
}

// ---- storage ---------------------------------------------------------------------------------------

RenderCache::RenderCache(std::filesystem::path dir, uint64_t limitBytes) : dir_(std::move(dir)), limit_(limitBytes) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    // existing frames: least recently written first; leftovers of interrupted writes are removed
    std::vector<std::pair<std::filesystem::file_time_type, std::pair<FrameKey, uint64_t>>> found;
    for (const auto& e : std::filesystem::directory_iterator(dir_, ec)) {
        const auto& p = e.path();
        const std::string name = p.filename().string();
        if (name.find(".tmp") != std::string::npos) {
            std::filesystem::remove(p, ec);
            continue;
        }
        if (p.extension() != ".qoi" || p.stem().string().size() != 32) continue;
        FrameKey k;
        try {
            k.hi = std::stoull(p.stem().string().substr(0, 16), nullptr, 16);
            k.lo = std::stoull(p.stem().string().substr(16), nullptr, 16);
        } catch (const std::exception&) {
            continue;
        }
        found.push_back({e.last_write_time(ec), {k, static_cast<uint64_t>(e.file_size(ec))}});
    }
    std::sort(found.begin(), found.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    std::lock_guard lk(m_);
    for (const auto& [t, kv] : found) {
        index_[kv.first] = Entry{kv.second, ++clock_};
        total_ += kv.second;
    }
    evictLocked();
}

std::filesystem::path RenderCache::fileOf(const FrameKey& key) const { return dir_ / (key.hex() + ".qoi"); }

void RenderCache::setLimit(uint64_t bytes) {
    std::lock_guard lk(m_);
    limit_ = bytes;
    evictLocked();
}

uint64_t RenderCache::limit() const {
    std::lock_guard lk(m_);
    return limit_;
}

bool RenderCache::contains(const FrameKey& key) const {
    std::lock_guard lk(m_);
    return index_.count(key) != 0;
}

std::optional<Image> RenderCache::load(const FrameKey& key) {
    {
        std::lock_guard lk(m_);
        if (!index_.count(key)) return std::nullopt;
    }
    std::vector<uint8_t> bytes;
    {
        std::ifstream in(fileOf(key), std::ios::binary);
        if (in) bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::optional<Image> img = decodeFrame(bytes.data(), bytes.size());
    std::lock_guard lk(m_);
    if (!img) {   // unreadable: forget it
        removeLocked(key);
        ++changes_;
        return std::nullopt;
    }
    if (auto it = index_.find(key); it != index_.end()) it->second.lastUse = ++clock_;
    return img;
}

bool RenderCache::store(const FrameKey& key, const Image& image) {
    const std::vector<uint8_t> bytes = encodeFrame(image);
    const std::filesystem::path file = fileOf(key);
    std::filesystem::path tmp = file;
    tmp += ".tmp" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100000);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::lock_guard lk(m_);
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);   // atomic replace: readers see the old file or the complete new one
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    auto& e = index_[key];
    total_ -= e.bytes;
    e.bytes = bytes.size();
    e.lastUse = ++clock_;
    total_ += e.bytes;
    ++changes_;
    evictLocked();
    return true;
}

void RenderCache::removeLocked(const FrameKey& key) {
    auto it = index_.find(key);
    if (it == index_.end()) return;
    std::error_code ec;
    std::filesystem::remove(fileOf(key), ec);
    total_ -= it->second.bytes;
    index_.erase(it);
}

void RenderCache::evictLocked() {
    if (total_ <= limit_) return;
    std::vector<std::pair<uint64_t, FrameKey>> byAge;
    byAge.reserve(index_.size());
    for (const auto& [k, e] : index_) byAge.push_back({e.lastUse, k});
    std::sort(byAge.begin(), byAge.end());
    for (const auto& [age, k] : byAge) {
        if (total_ <= limit_) break;
        removeLocked(k);
        ++changes_;
    }
}

void RenderCache::clear() {
    std::lock_guard lk(m_);
    std::error_code ec;
    for (const auto& [k, e] : index_) std::filesystem::remove(fileOf(k), ec);
    index_.clear();
    total_ = 0;
    ++changes_;
}

uint64_t RenderCache::sizeBytes() const {
    std::lock_guard lk(m_);
    return total_;
}

size_t RenderCache::frameCount() const {
    std::lock_guard lk(m_);
    return index_.size();
}

uint64_t RenderCache::changeCount() const {
    std::lock_guard lk(m_);
    return changes_;
}

}  // namespace fa
