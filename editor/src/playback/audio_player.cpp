#include "fa/playback/audio_player.h"

#include "fa/render/audio_mix.h"

#include "miniaudio_include.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace fa {

namespace {
constexpr int kChannels = 2;
constexpr int64_t kRingFrames = 1 << 15;   // 32768 frames ~ 0.68 s at 48 kHz
constexpr int kChunk = 1024;
constexpr int64_t kPrefill = 4800;         // ~100 ms queued before the device starts
}  // namespace

struct AudioPlayer::Impl {
    Backend backend;
    ma_context context{};
    bool contextOk = false;
    ma_device device{};
    bool deviceOk = false;
    int deviceRate = 0;
    std::string error;
    std::string description;

    // ring buffer (single producer: feeder thread, single consumer: device callback)
    std::vector<float> ring = std::vector<float>(static_cast<size_t>(kRingFrames) * kChannels);
    std::atomic<int64_t> writePos{0};   // frames written (monotonic)
    std::atomic<int64_t> readPos{0};    // frames read (monotonic)
    std::atomic<int64_t> underrun{0};
    std::atomic<float> vol{1.0f};
    std::atomic<bool> running{false};
    // level meter accumulators (written by the device callback, taken by the UI)
    std::atomic<float> meterPeak[kChannels]{};
    std::atomic<double> meterSumSq[kChannels]{};
    std::atomic<int64_t> meterCount{0};
    std::atomic<bool> meterClip{false};

    // feeder
    std::thread feeder;
    std::mutex m;
    std::condition_variable cv;
    bool feederQuit = false;
    ProjectSnapshot snap;
    bool snapChanged = false;
    int64_t startSample = 0;
    int rate = 48000;
    std::vector<std::string> mixErrors;

    // steady-clock fallback
    std::chrono::steady_clock::time_point t0;

    static void callback(ma_device* dev, void* out, const void*, ma_uint32 frames) {
        auto* self = static_cast<Impl*>(dev->pUserData);
        float* dst = static_cast<float*>(out);
        const int64_t r = self->readPos.load(std::memory_order_relaxed);
        const int64_t w = self->writePos.load(std::memory_order_acquire);
        const int64_t n = std::min<int64_t>(w - r, frames);
        const float v = self->vol.load(std::memory_order_relaxed);
        float pk[kChannels] = {};
        double ss[kChannels] = {};
        bool clip = false;
        for (int64_t i = 0; i < n; ++i) {
            const int64_t idx = ((r + i) & (kRingFrames - 1)) * kChannels;
            for (int c = 0; c < kChannels; ++c) {
                const float x = self->ring[idx + c];
                const float ax = std::fabs(x);
                pk[c] = std::max(pk[c], ax);
                ss[c] += static_cast<double>(x) * x;
                clip |= ax > 1.0f;
                dst[i * kChannels + c] = std::clamp(x * v, -1.0f, 1.0f);
            }
        }
        if (n > 0) {
            for (int c = 0; c < kChannels; ++c) {
                float cur = self->meterPeak[c].load(std::memory_order_relaxed);
                while (pk[c] > cur && !self->meterPeak[c].compare_exchange_weak(cur, pk[c], std::memory_order_relaxed)) {}
                self->meterSumSq[c].fetch_add(ss[c], std::memory_order_relaxed);
            }
            self->meterCount.fetch_add(n, std::memory_order_relaxed);
            if (clip) self->meterClip.store(true, std::memory_order_relaxed);
        }
        if (n < frames) {
            std::memset(dst + n * kChannels, 0, static_cast<size_t>(frames - n) * kChannels * sizeof(float));
            if (self->running.load(std::memory_order_relaxed)) self->underrun.fetch_add(frames - n);
        }
        self->readPos.store(r + n, std::memory_order_release);
        self->cvNotify();
    }
    void cvNotify() { cv.notify_one(); }

    void initContext() {
        ma_backend nullBackend = ma_backend_null;
        const ma_result res = backend == Backend::Null ? ma_context_init(&nullBackend, 1, nullptr, &context)
                                                       : ma_context_init(nullptr, 0, nullptr, &context);
        contextOk = res == MA_SUCCESS;
        if (!contextOk) error = std::string("audio context: ") + ma_result_description(res);
    }

    bool ensureDevice(int sampleRate) {
        if (deviceOk && deviceRate == sampleRate) return true;
        if (deviceOk) {
            ma_device_uninit(&device);
            deviceOk = false;
        }
        if (!contextOk) return false;
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = kChannels;
        cfg.sampleRate = static_cast<ma_uint32>(sampleRate);
        cfg.dataCallback = &Impl::callback;
        cfg.pUserData = this;
        cfg.periodSizeInMilliseconds = 10;
        const ma_result res = ma_device_init(&context, &cfg, &device);
        if (res != MA_SUCCESS) {
            error = std::string("audio device: ") + ma_result_description(res);
            return false;
        }
        deviceOk = true;
        deviceRate = sampleRate;
        description = std::string(device.playback.name) + " (" + std::to_string(sampleRate) + " Hz)";
        return true;
    }

    void feederLoop() {
        std::unique_ptr<AudioMixer> mixer;
        std::filesystem::path mixerFile;
        std::vector<float> chunk(static_cast<size_t>(kChunk) * kChannels);
        ProjectSnapshot local;
        int64_t mixPos = 0;
        {
            std::lock_guard lk(m);
            local = snap;
            mixPos = startSample;
        }
        for (;;) {
            {
                std::unique_lock lk(m);
                cv.wait_for(lk, std::chrono::milliseconds(5), [&] {
                    return feederQuit || snapChanged ||
                           kRingFrames - (writePos.load() - readPos.load()) >= kChunk;
                });
                if (feederQuit) return;
                if (snapChanged) {
                    local = snap;
                    snapChanged = false;
                }
            }
            if (kRingFrames - (writePos.load() - readPos.load()) < kChunk) continue;
            const Sequence* seq = local.sequence();
            if (!mixer || mixerFile != local.projectFile) {
                mixer = std::make_unique<AudioMixer>(local.projectFile, rate, kChannels);
                mixerFile = local.projectFile;
            }
            if (seq) {
                try {
                    mixer->mix(*local.project, *seq, mixPos, kChunk, chunk.data(), /*tolerateErrors=*/true);
                } catch (...) {
                    std::fill(chunk.begin(), chunk.end(), 0.0f);
                }
                if (!mixer->errors().empty()) {
                    std::lock_guard lk(m);
                    mixErrors = mixer->errors();
                }
            } else {
                std::fill(chunk.begin(), chunk.end(), 0.0f);
            }
            const int64_t w = writePos.load(std::memory_order_relaxed);
            for (int i = 0; i < kChunk; ++i) {
                const int64_t idx = ((w + i) & (kRingFrames - 1)) * kChannels;
                for (int c = 0; c < kChannels; ++c) ring[idx + c] = chunk[static_cast<size_t>(i) * kChannels + c];
            }
            writePos.store(w + kChunk, std::memory_order_release);
            mixPos += kChunk;
        }
    }

    void stopFeeder() {
        {
            std::lock_guard lk(m);
            feederQuit = true;
        }
        cv.notify_all();
        if (feeder.joinable()) feeder.join();
        feederQuit = false;
    }
};

AudioPlayer::AudioPlayer(Backend backend) : impl_(std::make_unique<Impl>()) {
    impl_->backend = backend;
    impl_->initContext();
}

AudioPlayer::~AudioPlayer() {
    stop();
    if (impl_->deviceOk) ma_device_uninit(&impl_->device);
    if (impl_->contextOk) ma_context_uninit(&impl_->context);
}

bool AudioPlayer::deviceAvailable() const { return impl_->contextOk; }
std::string AudioPlayer::deviceDescription() const { return impl_->description; }
std::string AudioPlayer::lastError() const { return impl_->error; }
bool AudioPlayer::playing() const { return impl_->running.load(); }
int AudioPlayer::sampleRate() const { return impl_->rate; }
void AudioPlayer::setVolume(float v) { impl_->vol.store(std::clamp(v, 0.0f, 1.0f)); }
float AudioPlayer::volume() const { return impl_->vol.load(); }
int64_t AudioPlayer::underrunSamples() const { return impl_->underrun.load(); }

AudioPlayer::Levels AudioPlayer::takeLevels() {
    Impl& d = *impl_;
    Levels out;
    out.samples = d.meterCount.exchange(0);
    for (int c = 0; c < kChannels; ++c) {
        out.peak[c] = d.meterPeak[c].exchange(0.0f);
        const double ss = d.meterSumSq[c].exchange(0.0);
        out.rms[c] = out.samples > 0 ? static_cast<float>(std::sqrt(ss / static_cast<double>(out.samples))) : 0.0f;
    }
    out.clipped = d.meterClip.exchange(false);
    return out;
}

std::vector<std::string> AudioPlayer::mediaErrors() const {
    std::lock_guard lk(impl_->m);
    return impl_->mixErrors;
}

void AudioPlayer::start(const ProjectSnapshot& snapshot, int64_t startSample) {
    stop();
    Impl& d = *impl_;
    const Sequence* seq = snapshot.sequence();
    d.rate = seq ? seq->sampleRate : 48000;
    d.readPos = 0;
    d.writePos = 0;
    d.underrun = 0;
    {
        std::lock_guard lk(d.m);
        d.snap = snapshot;
        d.snapChanged = false;
        d.startSample = startSample;
        d.mixErrors.clear();
    }
    const bool haveDevice = d.ensureDevice(d.rate);
    if (haveDevice) {
        d.feeder = std::thread([&d] { d.feederLoop(); });
        // prefill ~100 ms so the device does not start with an underrun
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (d.writePos.load() < kPrefill && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        d.running = true;
        const ma_result res = ma_device_start(&d.device);
        if (res != MA_SUCCESS) {   // fall back to the steady clock (silent playback, still in sync)
            d.error = std::string("audio start: ") + ma_result_description(res);
            d.stopFeeder();
            ma_device_uninit(&d.device);
            d.deviceOk = false;
        }
    } else {
        d.running = true;   // steady-clock fallback (no sound)
    }
    d.t0 = std::chrono::steady_clock::now();
}

void AudioPlayer::stop() {
    Impl& d = *impl_;
    if (!d.running.exchange(false)) return;
    if (d.deviceOk) ma_device_stop(&d.device);
    d.stopFeeder();
}

void AudioPlayer::updateSnapshot(const ProjectSnapshot& snapshot) {
    {
        std::lock_guard lk(impl_->m);
        impl_->snap = snapshot;
        impl_->snapChanged = true;
    }
    impl_->cv.notify_all();
}

int64_t AudioPlayer::position() const {
    const Impl& d = *impl_;
    if (d.deviceOk && d.feeder.joinable()) {
        // samples taken by the device, minus one device period still on its way to the speaker
        const int64_t latency = d.device.playback.internalPeriodSizeInFrames;
        return d.startSample + std::max<int64_t>(0, d.readPos.load() - latency);
    }
    if (!d.running) return d.startSample;
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - d.t0).count();
    return d.startSample + static_cast<int64_t>(secs * d.rate);
}

}  // namespace fa
