#include "ffmpeg_common.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace fa {

namespace {
constexpr int64_t kMaxForwardSamplesFactor = 2;   // decode forward up to 2 s instead of seeking
const Time kSeekPreroll(1, 5);                     // seek 200 ms early (codec priming, frame granularity)
}  // namespace

struct AudioDecoder::Impl {
    std::filesystem::path path;
    int outRate = 48000;
    int outChannels = 2;
    ff::FormatInputPtr fmt;
    ff::CodecContextPtr dec;
    AVStream* stream = nullptr;
    AudioStreamInfo info;
    Time origin;                       // container time of output sample 0
    ff::SwrPtr swr;
    int swrChannels = 0;               // channels produced by swr (1 for mono sources, else outChannels)
    ff::PacketPtr pkt = ff::makePacket();
    ff::FramePtr frame = ff::makeFrame();

    std::vector<float> buf;            // interleaved, outChannels per sample
    int64_t bufStart = 0;              // output-sample index of buf[0]
    bool positioned = false;           // bufStart known (after open/seek: from the first decoded frame)
    bool eof = false;
    bool flushSent = false;
    bool fromStreamStart = true;       // buf begins at the stream's first sample (nothing exists before it)
    int64_t lastSeekTarget = 0;
    int64_t seeks = 0;

    std::string where() const { return ff::pathUtf8(path); }
    int64_t bufEnd() const { return bufStart + static_cast<int64_t>(buf.size()) / outChannels; }

    void open(std::optional<Time> originOpt) {
        fmt = ff::openInput(path);
        const AVCodec* codec = nullptr;
        const int idx = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        if (idx < 0) throw MediaError("no audio stream in " + where());
        if (!codec) throw MediaError("no decoder for the audio stream of " + where());
        stream = fmt->streams[idx];
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
            if (static_cast<int>(i) != idx) fmt->streams[i]->discard = AVDISCARD_ALL;
        dec.reset(avcodec_alloc_context3(codec));
        if (!dec) throw MediaError("out of memory (audio decoder)");
        ff::check(avcodec_parameters_to_context(dec.get(), stream->codecpar), "audio decoder parameters for " + where());
        dec->pkt_timebase = stream->time_base;
        ff::check(avcodec_open2(dec.get(), codec, nullptr), "cannot open audio decoder for " + where());

        info.index = idx;
        info.codec = avcodec_get_name(stream->codecpar->codec_id);
        info.sampleRate = stream->codecpar->sample_rate;
        info.channels = stream->codecpar->ch_layout.nb_channels;
        info.startTime =
            stream->start_time != AV_NOPTS_VALUE ? ff::tsToTime(stream->start_time, stream->time_base) : Time(0);
        if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0)
            info.duration = ff::tsToTime(stream->duration, stream->time_base);
        if (info.sampleRate <= 0) throw MediaError("invalid audio sample rate in " + where());

        if (originOpt) {
            origin = *originOpt;
        } else {
            const int vi = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            const AVStream* vs = vi >= 0 ? fmt->streams[vi] : nullptr;
            origin = (vs && vs->start_time != AV_NOPTS_VALUE) ? ff::tsToTime(vs->start_time, vs->time_base)
                                                              : info.startTime;
        }
        positioned = false;   // the first decoded frame sets the position
        fromStreamStart = true;
    }

    void setupSwr(const AVFrame* f) {
        AVChannelLayout inLayout{};
        if (f->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC && f->ch_layout.nb_channels > 0)
            ff::check(av_channel_layout_copy(&inLayout, &f->ch_layout), "channel layout");
        else
            av_channel_layout_default(&inLayout, f->ch_layout.nb_channels > 0 ? f->ch_layout.nb_channels : 1);
        // mono sources stay mono (copied to every output channel at unity); others are converted to stereo and, for a
        // mono output, folded down as (L + R) / 2 so a centred sound keeps its level (swresample's default -3 dB
        // matrix would make it 3 dB louder)
        swrChannels = inLayout.nb_channels == 1 ? 1 : std::max(outChannels, 2);
        AVChannelLayout outLayout{};
        av_channel_layout_default(&outLayout, swrChannels);
        SwrContext* raw = nullptr;
        const int r = swr_alloc_set_opts2(&raw, &outLayout, AV_SAMPLE_FMT_FLT, outRate, &inLayout,
                                          static_cast<AVSampleFormat>(f->format), f->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&inLayout);
        av_channel_layout_uninit(&outLayout);
        ff::check(r, "audio converter setup for " + where());
        swr.reset(raw);
        ff::check(swr_init(swr.get()), "audio converter init for " + where());
    }

    // Append converted samples of one decoded frame (or the swr tail when f == nullptr).
    void append(const AVFrame* f) {
        if (!swr) return;
        const int inSamples = f ? f->nb_samples : 0;
        const int maxOut = swr_get_out_samples(swr.get(), inSamples);
        if (maxOut <= 0) return;
        std::vector<float> tmp(static_cast<size_t>(maxOut) * swrChannels);
        uint8_t* outPtr[1] = {reinterpret_cast<uint8_t*>(tmp.data())};
        const int got = swr_convert(swr.get(), outPtr, maxOut, f ? const_cast<const uint8_t**>(f->extended_data) : nullptr,
                                    inSamples);
        ff::check(got, "audio conversion in " + where());
        const size_t old = buf.size();
        buf.resize(old + static_cast<size_t>(got) * outChannels);
        float* dst = buf.data() + old;
        if (swrChannels == 1) {
            for (int i = 0; i < got; ++i)
                for (int c = 0; c < outChannels; ++c) dst[i * outChannels + c] = tmp[i];   // mono: unity on all channels
        } else if (outChannels == 1) {
            for (int i = 0; i < got; ++i) dst[i] = 0.5f * (tmp[static_cast<size_t>(i) * 2] + tmp[static_cast<size_t>(i) * 2 + 1]);
        } else {
            std::memcpy(dst, tmp.data(), static_cast<size_t>(got) * outChannels * sizeof(float));
        }
    }

    // Decode one more frame into buf. Returns false at end of stream.
    bool decodeMore() {
        for (;;) {
            const int r = avcodec_receive_frame(dec.get(), frame.get());
            if (r == 0) {
                if (!swr || frame->sample_rate != swrRate || frame->format != swrFormat ||
                    frame->ch_layout.nb_channels != swrInChannels) {
                    if (swr) append(nullptr);   // drain the old converter
                    setupSwr(frame.get());
                    swrRate = frame->sample_rate;
                    swrFormat = frame->format;
                    swrInChannels = frame->ch_layout.nb_channels;
                }
                if (!positioned) {
                    int64_t ts = frame->best_effort_timestamp;
                    if (ts == AV_NOPTS_VALUE) ts = frame->pts;
                    const Time t = (ts != AV_NOPTS_VALUE ? ff::tsToTime(ts, stream->time_base) : info.startTime) - origin;
                    bufStart = (t * Rational(outRate)).round();
                    buf.clear();
                    positioned = true;
                }
                append(frame.get());
                av_frame_unref(frame.get());
                return true;
            }
            if (r == AVERROR_EOF) {
                append(nullptr);   // flush resampler tail
                eof = true;
                return false;
            }
            if (r != AVERROR(EAGAIN)) ff::check(r, "audio decode error in " + where());
            if (flushSent) {
                eof = true;
                return false;
            }
            const int rr = av_read_frame(fmt.get(), pkt.get());
            if (rr == AVERROR_EOF) {
                ff::check(avcodec_send_packet(dec.get(), nullptr), "flushing audio decoder of " + where());
                flushSent = true;
                continue;
            }
            ff::check(rr, "read error in " + where());
            if (pkt->stream_index == stream->index) {
                const int sr = avcodec_send_packet(dec.get(), pkt.get());
                av_packet_unref(pkt.get());
                if (sr < 0 && sr != AVERROR(EAGAIN) && sr != AVERROR_INVALIDDATA)
                    ff::check(sr, "cannot send audio packet in " + where());
            } else {
                av_packet_unref(pkt.get());
            }
        }
    }

    void seekTo(int64_t sample) {
        const Time t = max(Time(0), Rational(sample, outRate) - kSeekPreroll) + origin;
        const int64_t ts = (t / ff::toRational(stream->time_base)).floor();
        int r = av_seek_frame(fmt.get(), stream->index, ts, AVSEEK_FLAG_BACKWARD);
        if (r < 0) r = av_seek_frame(fmt.get(), stream->index, ts, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
        ff::check(r, "audio seek failed in " + where());
        avcodec_flush_buffers(dec.get());
        lastSeekTarget = sample;
        fromStreamStart = t <= origin;
        swr.reset();
        swrRate = swrFormat = swrInChannels = -1;
        buf.clear();
        positioned = false;
        eof = flushSent = false;
        ++seeks;
    }

    void read(int64_t start, int count, float* out) {
        std::fill(out, out + static_cast<size_t>(count) * outChannels, 0.0f);
        if (count <= 0) return;
        const int64_t end = start + count;
        if (end <= 0) return;   // entirely before the stream
        const int64_t maxForward = kMaxForwardSamplesFactor * outRate;
        const bool pastEnd = positioned && eof && start >= bufEnd();
        const bool covered = positioned && (start >= bufStart || fromStreamStart) && start <= bufEnd() + maxForward;
        const bool fresh = !positioned && seeks == 0 && start <= maxForward;   // just opened: decode from the top
        if (!pastEnd && !covered && !fresh) seekTo(std::max<int64_t>(start, 0));
        while ((!positioned || bufEnd() < end) && !eof) decodeMore();
        if (positioned && lastSeekTarget > 0 && bufStart > std::max<int64_t>(start, 0)) {
            seekTo(0);   // the seek landed after `start` (coarse index): decode from the beginning instead
            while ((!positioned || bufEnd() < end) && !eof) decodeMore();
        }
        if (!positioned) return;
        const int64_t from = std::max(start, bufStart);
        const int64_t to = std::min(end, bufEnd());
        if (to > from)
            std::memcpy(out + (from - start) * outChannels, buf.data() + (from - bufStart) * outChannels,
                        static_cast<size_t>(to - from) * outChannels * sizeof(float));
        // keep 0.5 s of history (small backward reads stay seek-free) and drop the rest
        const int64_t keepFrom = std::min(std::max(bufStart, start - outRate / 2), bufEnd());
        if (keepFrom > bufStart) {
            buf.erase(buf.begin(), buf.begin() + (keepFrom - bufStart) * outChannels);
            bufStart = keepFrom;
            fromStreamStart = false;
        }
    }

    int swrRate = -1, swrFormat = -1, swrInChannels = -1;
};

AudioDecoder::AudioDecoder(const std::filesystem::path& file, int sampleRate, int channels, std::optional<Time> origin)
    : impl_(std::make_unique<Impl>()) {
    if (sampleRate <= 0 || channels <= 0) throw MediaError("invalid audio output format");
    impl_->path = file;
    impl_->outRate = sampleRate;
    impl_->outChannels = channels;
    impl_->open(origin);
}

AudioDecoder::~AudioDecoder() = default;
const AudioStreamInfo& AudioDecoder::info() const { return impl_->info; }
int AudioDecoder::sampleRate() const { return impl_->outRate; }
int AudioDecoder::channels() const { return impl_->outChannels; }
int64_t AudioDecoder::seekCount() const { return impl_->seeks; }

std::optional<int64_t> AudioDecoder::lengthSamples() const {
    if (!impl_->info.duration) return std::nullopt;
    const Time end = impl_->info.startTime + *impl_->info.duration - impl_->origin;
    return (end * Rational(impl_->outRate)).round();
}

void AudioDecoder::read(int64_t start, int count, float* out) { impl_->read(start, count, out); }

}  // namespace fa
