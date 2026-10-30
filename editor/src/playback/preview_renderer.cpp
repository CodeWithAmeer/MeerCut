#include "fa/playback/preview_renderer.h"

#include "fa/render/renderer.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace fa {

namespace {

// Decodes through a DecoderPool; media that cannot be opened/decoded is replaced by a placeholder
// frame (dark red) and reported, so one offline file does not blank the whole preview.
class TolerantProvider final : public FrameProvider {
public:
    TolerantProvider(DecoderPool& pool) : pool_(pool) { placeholder_.reset(16, 9, Rgba8{90, 16, 24, 255}); }
    std::filesystem::path assetDir() const override { return pool_.assetDir(); }
    void canvas(int w, int h) override { pool_.canvas(w, h); }
    const Image* neuralFrame(const Clip& c, const MediaSource& s, int64_t a, int64_t b, double t, bool& cut) override {
        return pool_.neuralFrame(c, s, a, b, t, cut);
    }
    Result frame(const Clip& clip, const MediaSource& source, const Time& t) override {
        if (!failed_.count(source.id)) {
            try {
                return pool_.frame(clip, source, t);
            } catch (const std::exception& e) {
                failed_.insert(source.id);
                messages_.push_back(source.path + ": " + e.what());
            }
        }
        Result r;
        r.image = &placeholder_;
        r.requestedTime = t;
        return r;
    }
    std::vector<std::string> takeMessages() { return std::move(messages_); }
    void resetFailures() { failed_.clear(); }

private:
    DecoderPool& pool_;
    Image placeholder_;
    std::set<std::string> failed_;
    std::vector<std::string> messages_;
};

}  // namespace

struct PreviewRenderer::State {
    mutable std::mutex m;
    std::condition_variable cv;
    bool quit = false;
    Callback cb;
    ProjectSnapshot snap;
    int maxW = 960, maxH = 540;
    std::optional<int64_t> pending;            // display request
    int64_t prefFrom = 0, prefCount = 0;
    // cache (current version only)
    std::map<int64_t, PreviewFrame> cache;
    std::list<int64_t> lru;                    // front = most recent
    size_t cacheCap = 48;
    uint64_t rendered = 0;

    // render resources, used under renderMutex
    std::mutex renderMutex;
    std::unique_ptr<DecoderPool> pool;
    std::unique_ptr<TolerantProvider> provider;
    std::unique_ptr<RenderBackend> backend;
    BackendChoice backendChoice = BackendChoice::Auto;
    bool backendDirty = true;                  // (re)create the backend on the worker thread
    bool hardwareDecode = false;
    std::string backendName = "cpu";
    TitleRendererFactory titleFactory;
    std::unique_ptr<TitleRenderer> titles;
    bool titlesDirty = false;
    int poolW = 0, poolH = 0;
    std::filesystem::path poolFile;
    uint64_t poolVersion = 0;
    std::vector<std::string> offline;          // sticky per snapshot version
    bool compare = false;
    std::shared_ptr<const Project> strippedFrom;   // compare: the project without effects, made from this snapshot
    std::shared_ptr<const Project> stripped;
    // M5 Render Preview cache (render resources: used under renderMutex)
    std::shared_ptr<RenderCache> diskCache;
    uint64_t diskHits = 0;
    std::unique_ptr<FrameKeyer> keyer;
    std::shared_ptr<const Project> keyerProject;
    std::string keyerSequence, keyerBackend;
    std::filesystem::path keyerFile;
    int keyerW = 0, keyerH = 0;
    bool keyerHw = false;
    const FrameKeyer& keyerFor(const ProjectSnapshot& of, int w, int h, const std::string& backendUsed, bool hw) {
        if (!keyer || keyerProject != of.project || keyerSequence != of.sequenceId || keyerFile != of.projectFile ||
            keyerW != w || keyerH != h || keyerBackend != backendUsed || keyerHw != hw) {
            keyer = std::make_unique<FrameKeyer>(*of.project, *of.sequence(), of.projectFile,
                                                 FrameKeyer::Config{w, h, backendUsed, hw});
            keyerProject = of.project;
            keyerSequence = of.sequenceId;
            keyerFile = of.projectFile;
            keyerW = w;
            keyerH = h;
            keyerBackend = backendUsed;
            keyerHw = hw;
        }
        return *keyer;
    }

    bool isCached(int64_t f) const { return cache.count(f) != 0; }
    void touch(int64_t f) {
        lru.remove(f);
        lru.push_front(f);
    }
    void insert(const PreviewFrame& fr) {
        cache[fr.index] = fr;
        touch(fr.index);
        while (cache.size() > cacheCap && !lru.empty()) {
            cache.erase(lru.back());
            lru.pop_back();
        }
    }
    void invalidate() {
        cache.clear();
        lru.clear();
    }
    std::optional<int64_t> nextPrefetch() const {
        for (int64_t f = prefFrom; f < prefFrom + prefCount; ++f)
            if (!isCached(f)) return f;
        return std::nullopt;
    }
};

PreviewRenderer::PreviewRenderer(int maxWidth, int maxHeight, size_t cacheFrames) : s_(std::make_unique<State>()) {
    s_->maxW = maxWidth;
    s_->maxH = maxHeight;
    s_->cacheCap = std::max<size_t>(cacheFrames, 4);
    worker_ = std::thread([this] { run(); });
}

PreviewRenderer::~PreviewRenderer() {
    {
        std::lock_guard lk(s_->m);
        s_->quit = true;
    }
    s_->cv.notify_all();
    if (worker_.joinable()) worker_.join();
}

void PreviewRenderer::setCallback(Callback cb) {
    std::lock_guard lk(s_->m);
    s_->cb = std::move(cb);
}

void PreviewRenderer::setSnapshot(ProjectSnapshot snapshot) {
    {
        std::lock_guard lk(s_->m);
        if (snapshot.version != s_->snap.version || snapshot.project != s_->snap.project) s_->invalidate();
        s_->snap = std::move(snapshot);
    }
    s_->cv.notify_all();
}

void PreviewRenderer::setCompare(bool on) {
    {
        std::lock_guard lk(s_->m);
        if (s_->compare == on) return;
        s_->compare = on;
        s_->invalidate();
    }
    s_->cv.notify_all();
}

void PreviewRenderer::setBackend(BackendChoice choice) {
    {
        std::lock_guard lk(s_->m);
        if (choice == s_->backendChoice && s_->backend) return;
        s_->backendChoice = choice;
        s_->backendDirty = true;
        s_->invalidate();
    }
    s_->cv.notify_all();
}

void PreviewRenderer::setTitleRendererFactory(TitleRendererFactory factory) {
    {
        std::lock_guard lk(s_->m);
        s_->titleFactory = std::move(factory);
        s_->titlesDirty = true;
        s_->invalidate();
    }
    s_->cv.notify_all();
}

void PreviewRenderer::setHardwareDecoding(bool on) {
    {
        std::lock_guard rl(s_->renderMutex);   // decoders are recreated with the new setting
        std::lock_guard lk(s_->m);
        if (on == s_->hardwareDecode) return;
        s_->hardwareDecode = on;
        s_->pool.reset();
        s_->provider.reset();
        s_->invalidate();
    }
    s_->cv.notify_all();
}

std::string PreviewRenderer::backendName() const {
    std::lock_guard lk(s_->m);
    return s_->backendName;
}

BackendChoice PreviewRenderer::backendChoice() const {
    std::lock_guard lk(s_->m);
    return s_->backendChoice;
}

bool PreviewRenderer::hardwareDecoding() const {
    std::lock_guard lk(s_->m);
    return s_->hardwareDecode;
}

void PreviewRenderer::maxSize(int& maxWidth, int& maxHeight) const {
    std::lock_guard lk(s_->m);
    maxWidth = s_->maxW;
    maxHeight = s_->maxH;
}

void PreviewRenderer::setRenderCache(std::shared_ptr<RenderCache> cache) {
    {
        std::lock_guard rl(s_->renderMutex);
        std::lock_guard lk(s_->m);
        s_->diskCache = std::move(cache);
        s_->invalidate();
    }
    s_->cv.notify_all();
}

uint64_t PreviewRenderer::renderCacheHits() const {
    std::lock_guard lk(s_->m);
    return s_->diskHits;
}

std::optional<FrameKey> PreviewRenderer::frameKey(int64_t frame) {
    std::lock_guard rl(s_->renderMutex);
    ProjectSnapshot snap;
    int w = 0, h = 0;
    std::string backend;
    bool hw = false;
    {
        std::lock_guard lk(s_->m);
        snap = s_->snap;
        if (!snap.sequence()) return std::nullopt;
        canvasSize(*snap.sequence(), s_->maxW, s_->maxH, w, h);
        backend = s_->backendName;
        hw = s_->hardwareDecode;
    }
    return s_->keyerFor(snap, w, h, backend, hw).key(frame);
}

void PreviewRenderer::setMaxSize(int maxWidth, int maxHeight) {
    {
        std::lock_guard lk(s_->m);
        if (maxWidth == s_->maxW && maxHeight == s_->maxH) return;
        s_->maxW = std::max(16, maxWidth);
        s_->maxH = std::max(16, maxHeight);
        s_->invalidate();
    }
    s_->cv.notify_all();
}

void PreviewRenderer::canvasSize(const Sequence& seq, int maxWidth, int maxHeight, int& w, int& h) {
    const double s = std::min({double(maxWidth) / seq.width, double(maxHeight) / seq.height, 1.0});
    w = std::max(2, static_cast<int>(seq.width * s) & ~1);
    h = std::max(2, static_cast<int>(seq.height * s) & ~1);
}

void PreviewRenderer::request(int64_t frame) {
    PreviewFrame hit;
    Callback cb;
    {
        std::lock_guard lk(s_->m);
        auto it = s_->cache.find(frame);
        if (it != s_->cache.end()) {
            s_->touch(frame);
            hit = it->second;
            cb = s_->cb;
            s_->pending.reset();
        } else {
            s_->pending = frame;
        }
    }
    if (cb) cb(hit);
    else s_->cv.notify_all();
}

void PreviewRenderer::setPrefetch(int64_t from, int64_t count) {
    {
        std::lock_guard lk(s_->m);
        s_->prefFrom = from;
        s_->prefCount = std::max<int64_t>(0, count);
    }
    s_->cv.notify_all();
}

void PreviewRenderer::clearPrefetch() { setPrefetch(0, 0); }

std::optional<PreviewFrame> PreviewRenderer::cached(int64_t frame) const {
    std::lock_guard lk(s_->m);
    auto it = s_->cache.find(frame);
    if (it == s_->cache.end()) return std::nullopt;
    return it->second;
}

uint64_t PreviewRenderer::framesRendered() const {
    std::lock_guard lk(s_->m);
    return s_->rendered;
}

PreviewFrame PreviewRenderer::renderOne(const ProjectSnapshot& snap, int64_t frame, int w, int h) {
    std::lock_guard rl(s_->renderMutex);
    PreviewFrame out;
    out.index = frame;
    out.version = snap.version;
    {
        std::lock_guard lk(s_->m);
        out.compare = s_->compare;
    }
    const Sequence* seq = snap.sequence();
    if (!seq) {
        out.image = std::make_shared<Image>(std::max(w, 2), std::max(h, 2), Rgba8{0, 0, 0, 255});
        return out;
    }
    if (s_->backendDirty || !s_->backend) {   // created here: the D3D11 device is used by this thread only
        BackendChoice choice;
        {
            std::lock_guard lk(s_->m);
            choice = s_->backendChoice;
            s_->backendDirty = false;
        }
        std::string used;
        s_->backend = makeBackend(choice, &used);
        std::lock_guard lk(s_->m);
        s_->backendName = used;
    }
    if (!s_->pool || s_->poolW != w || s_->poolH != h || s_->poolFile != snap.projectFile) {
        VideoDecoder::Options opt;
        opt.maxWidth = w;
        opt.maxHeight = h;
        opt.threads = 4;   // bounded memory per decoder (8 GB machines); still real-time at 1080p
        opt.convertThreads = 4;
        {
            std::lock_guard lk(s_->m);
            opt.hardware = s_->hardwareDecode;
        }
        s_->pool = std::make_unique<DecoderPool>(snap.projectFile, 6, opt);
        s_->provider = std::make_unique<TolerantProvider>(*s_->pool);
        s_->poolW = w;
        s_->poolH = h;
        s_->poolFile = snap.projectFile;
    }
    if (s_->poolVersion != snap.version) {   // after an edit (e.g. relink) retry media that failed before
        s_->provider->resetFailures();
        s_->offline.clear();
        s_->poolVersion = snap.version;
    }
    {
        TitleRendererFactory make;
        bool dirty = false;
        {
            std::lock_guard lk(s_->m);
            dirty = s_->titlesDirty;
            s_->titlesDirty = false;
            make = s_->titleFactory;
        }
        if (dirty) s_->titles = make ? make() : nullptr;
    }
    if (s_->diskCache && !out.compare) {   // M5 Render Preview: a frame rendered earlier with the same content
        std::string backend;
        bool hw = false;
        {
            std::lock_guard lk(s_->m);
            backend = s_->backendName;
            hw = s_->hardwareDecode;
        }
        if (auto cached = s_->diskCache->load(s_->keyerFor(snap, w, h, backend, hw).key(frame))) {
            out.image = std::make_shared<Image>(std::move(*cached));
            out.fromRenderCache = true;
            out.offline = s_->offline;
            std::lock_guard lk(s_->m);
            ++s_->diskHits;
            return out;
        }
    }
    std::vector<std::string> problems;
    const Image& img = renderFrame(*seq, frame, *snap.project, *s_->provider, *s_->backend, nullptr, nullptr, w, h,
                                   s_->titles.get(), &problems);
    auto image = std::make_shared<Image>(img);
    if (out.compare) {   // left half: the same frame without effects, colour grading, adjustment layers or stabilisation
        if (s_->strippedFrom != snap.project || !s_->stripped) {
            auto p = std::make_shared<Project>(*snap.project);
            for (auto& sq : p->sequences)
                for (auto& tr : sq.tracks) {
                    std::erase_if(tr.clips, [](const Clip& c) { return c.isAdjustment(); });
                    for (auto& c : tr.clips) {
                        c.effects.clear();
                        c.stabilization.reset();
                    }
                }
            s_->stripped = std::move(p);
            s_->strippedFrom = snap.project;
        }
        if (const Sequence* before = s_->stripped->findSequence(snap.sequenceId)) {
            const Image& b = renderFrame(*before, frame, *s_->stripped, *s_->provider, *s_->backend, nullptr, nullptr, w, h,
                                         s_->titles.get());
            const int half = image->width / 2;
            if (b.width == image->width && b.height == image->height)
                for (int y = 0; y < image->height; ++y) std::memcpy(image->row(y), b.row(y), static_cast<size_t>(half) * 4);
        }
    }
    out.image = std::move(image);
    for (auto& msg : s_->provider->takeMessages()) s_->offline.push_back(std::move(msg));
    for (auto& msg : problems)   // e.g. a LUT file that is missing: the effect is skipped
        if (std::find(s_->offline.begin(), s_->offline.end(), msg) == s_->offline.end()) s_->offline.push_back(std::move(msg));
    out.offline = s_->offline;
    return out;
}

PreviewFrame PreviewRenderer::renderNow(int64_t frame) {
    ProjectSnapshot snap;
    int w = 0, h = 0;
    {
        std::lock_guard lk(s_->m);
        snap = s_->snap;
        if (const Sequence* seq = snap.sequence()) canvasSize(*seq, s_->maxW, s_->maxH, w, h);
    }
    PreviewFrame f = renderOne(snap, frame, w, h);
    std::lock_guard lk(s_->m);
    if (f.version == s_->snap.version && f.compare == s_->compare) s_->insert(f);
    return f;
}

void PreviewRenderer::run() {
    for (;;) {
        ProjectSnapshot snap;
        int64_t frame = 0;
        bool display = false;
        int w = 0, h = 0;
        {
            std::unique_lock lk(s_->m);
            s_->cv.wait(lk, [&] {
                if (s_->quit) return true;
                if (!s_->snap.sequence()) return false;
                if (s_->pending && !s_->isCached(*s_->pending)) return true;
                return s_->nextPrefetch().has_value();
            });
            if (s_->quit) {
                // the text engine holds Qt font engines, which must be released on the thread that made them
                s_->titles.reset();
                return;
            }
            if (s_->pending && !s_->isCached(*s_->pending)) {
                frame = *s_->pending;
                display = true;
            } else {
                frame = *s_->nextPrefetch();
            }
            snap = s_->snap;
            canvasSize(*snap.sequence(), s_->maxW, s_->maxH, w, h);
        }
        PreviewFrame f;
        try {
            f = renderOne(snap, frame, w, h);
        } catch (const std::exception& e) {
            f.index = frame;
            f.version = snap.version;
            f.image = std::make_shared<Image>(w, h, Rgba8{40, 0, 0, 255});
            f.offline.push_back(e.what());
        }
        Callback cb;
        {
            std::lock_guard lk(s_->m);
            ++s_->rendered;
            if (f.version != s_->snap.version) continue;   // stale: an edit happened meanwhile
            if (f.compare != s_->compare) continue;        // stale: compare was switched meanwhile
            s_->insert(f);
            if (display && s_->pending == frame) {
                s_->pending.reset();
                cb = s_->cb;
            }
        }
        if (cb) cb(f);
    }
}

}  // namespace fa
