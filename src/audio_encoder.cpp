// AAC-LC audio for Resolve Studio on Linux, using FFmpeg's native AAC encoder.
// Lessons from Toxblh/davinci-linux-aac-codec and hexitnz/Resolve-Linux-Studio-AAC-FDK-Encoder-plugin:
// declare stereo layout + time base + AudioSpecificConfig, or Resolve's muxer guesses wrong.

#include "audio_encoder.h"

#include <algorithm>
#include <cstring>
#include <string>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

using namespace IOPlugin;

const uint8_t AacAudioEncoder::s_UUID[16] = {0xf3, 0x6d, 0xe6, 0xc3, 0xf1, 0x6d, 0x4d, 0x41,
                                             0x88, 0x0c, 0xd5, 0xa7, 0x72, 0xe0, 0xcd, 0xd3};

namespace {

const char* kBitrateId = "yta_aac_bitrate";
const std::vector<int32_t> kBitrates = {192, 256, 320};

int32_t LoadBitrate(IPropertyProvider* values) {
    int32_t kbps = 320;
    values->GetINT32(kBitrateId, kbps);
    if (std::find(kBitrates.begin(), kBitrates.end(), kbps) == kBitrates.end()) kbps = 320;
    return kbps;
}

}  // namespace

StatusCode AacAudioEncoder::RegisterCodecs(HostListRef* list) {
    if (!avcodec_find_encoder(AV_CODEC_ID_AAC)) {
        g_Log(logLevelWarn, "YouTube AMD Plugin :: FFmpeg has no AAC encoder, skipping audio codec");
        return errNone;
    }

    HostPropertyCollectionRef info;
    if (!info.IsValid()) return errAlloc;

    info.SetProperty(pIOPropUUID, propTypeUInt8, s_UUID, 16);
    const char* name = "AAC Stereo (YouTube AMD)";
    info.SetProperty(pIOPropName, propTypeString, name, static_cast<int>(strlen(name)));

    const uint32_t fourCC = 'aac ';
    info.SetProperty(pIOPropFourCC, propTypeUInt32, &fourCC, 1);
    const uint32_t media = mediaAudio;
    info.SetProperty(pIOPropMediaType, propTypeUInt32, &media, 1);
    const uint32_t dir = dirEncode;
    info.SetProperty(pIOPropCodecDirection, propTypeUInt32, &dir, 1);

    const uint32_t depth = 16;
    info.SetProperty(pIOPropBitDepth, propTypeUInt32, &depth, 1);
    info.SetProperty(pIOPropBitsPerSample, propTypeUInt32, &depth, 1);

    const uint32_t rates[] = {48000, 44100};
    info.SetProperty(pIOPropSamplingRate, propTypeUInt32, rates, 2);

    const uint32_t ch = 2;
    info.SetProperty(pIOPropNumChannels, propTypeUInt32, &ch, 1);
    const uint32_t layout = audLayoutStereo;
    info.SetProperty(pIOPropAudioChannelLayout, propTypeUInt32, &layout, 1);

    // MKV left out on purpose: Resolve's Matroska muxer writes a zero-length audio track for plugin AAC.
    static const char kContainers[] = "mp4\0mov";
    info.SetProperty(pIOPropContainerList, propTypeString, kContainers, sizeof(kContainers) - 1);

    return list->Append(&info) ? errNone : errFail;
}

StatusCode AacAudioEncoder::GetEncoderSettings(HostPropertyCollectionRef* values, HostListRef* settingsList) {
    HostUIConfigEntryRef item(kBitrateId);
    std::vector<std::string> labels;
    for (int32_t k : kBitrates) labels.push_back(std::to_string(k) + " kb/s");
    item.MakeComboBox("Bit Rate", labels, kBitrates, LoadBitrate(values));
    if (!item.IsSuccess() || !settingsList->Append(&item)) return errFail;
    return errNone;
}

AacAudioEncoder::AacAudioEncoder() = default;

AacAudioEncoder::~AacAudioEncoder() {
    avcodec_free_context(&ctx);
    if (fifo) av_audio_fifo_free(fifo);
    av_frame_free(&frame);
    av_packet_free(&pkt);
}

StatusCode AacAudioEncoder::DoInit(HostPropertyCollectionRef* props) {
    props->GetUINT32(pIOPropSamplingRate, sampleRate);
    props->GetUINT32(pIOPropNumChannels, channels);
    props->GetUINT32(pIOPropBitDepth, bitDepth);

    if (channels != 2 || bitDepth != 16 || (sampleRate != 48000 && sampleRate != 44100)) {
        g_Log(logLevelError, "YouTube AMD Plugin :: AAC needs 2 ch / 16-bit / 44.1 or 48 kHz, got %u ch / %u-bit / %u Hz",
              channels, bitDepth, sampleRate);
        return errFail;
    }
    return errNone;
}

StatusCode AacAudioEncoder::DoOpen(HostBufferRef* buff) {
    bitrateKbps = LoadBitrate(buff);

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) return errNoCodec;

    ctx = avcodec_alloc_context3(codec);
    if (!ctx) return errAlloc;
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    ctx->sample_rate = static_cast<int>(sampleRate);
    ctx->time_base = {1, static_cast<int>(sampleRate)};
    ctx->bit_rate = int64_t(bitrateKbps) * 1000;
    ctx->profile = AV_PROFILE_AAC_LOW;
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;  // AudioSpecificConfig goes to extradata, not ADTS headers
    av_channel_layout_default(&ctx->ch_layout, static_cast<int>(channels));

    if (int err = avcodec_open2(ctx, codec, nullptr); err < 0) {
        g_Log(logLevelError, "YouTube AMD Plugin :: Could not open AAC encoder (%d)", err);
        return errNoCodec;
    }

    fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, static_cast<int>(channels), ctx->frame_size * 4);
    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!fifo || !frame || !pkt) return errAlloc;

    const uint64_t bitRate = uint64_t(bitrateKbps) * 1000;
    buff->SetProperty(pIOPropBitRate, propTypeUInt64, &bitRate, 1);

    const uint32_t timeBase[2] = {1, sampleRate};
    buff->SetProperty(pIOPropTimeBase, propTypeUInt32, timeBase, 2);

    const int delay = std::max(ctx->initial_padding, 0);

    cookie.assign(ctx->extradata, ctx->extradata + ctx->extradata_size);
    if (!cookie.empty()) {
        buff->SetProperty(pIOPropMagicCookie, propTypeUInt8, cookie.data(), static_cast<int>(cookie.size()));
        const uint32_t cookieType = 'esds';
        buff->SetProperty(pIOPropMagicCookieType, propTypeUInt32, &cookieType, 1);
    }

    g_Log(logLevelInfo, "YouTube AMD Plugin :: AAC-LC %u Hz stereo %d kb/s, frame %d, priming %d", sampleRate,
          bitrateKbps, ctx->frame_size, delay);
    return errNone;
}

StatusCode AacAudioEncoder::DoProcess(HostBufferRef* buff) {
    if (!ctx) return errFail;

    if (buff == nullptr || !buff->IsValid()) {
        if (flushed) return errNone;
        flushed = true;
        return EncodeFrames(true);
    }

    char* data = nullptr;
    size_t size = 0;
    if (!buff->LockBuffer(&data, &size)) return errFail;
    if (!data || size == 0) {
        buff->UnlockBuffer();
        return errNone;
    }

    // Resolve sends interleaved signed 16-bit; the AAC encoder wants planar float.
    const int samples = static_cast<int>(size / (channels * sizeof(int16_t)));
    const auto* src = reinterpret_cast<const int16_t*>(data);
    std::vector<float> left(samples), right(samples);
    for (int i = 0; i < samples; ++i) {
        left[i] = src[2 * i] / 32768.0f;
        right[i] = src[2 * i + 1] / 32768.0f;
    }
    buff->UnlockBuffer();

    void* planes[2] = {left.data(), right.data()};
    if (av_audio_fifo_write(fifo, planes, samples) < samples) return errAlloc;

    return EncodeFrames(false);
}

StatusCode AacAudioEncoder::EncodeFrames(bool final) {
    const int frameSize = ctx->frame_size;
    while (av_audio_fifo_size(fifo) >= frameSize || (final && av_audio_fifo_size(fifo) > 0)) {
        const int n = std::min(frameSize, av_audio_fifo_size(fifo));
        av_frame_unref(frame);
        frame->nb_samples = n;  // a shorter last frame is allowed (AV_CODEC_CAP_SMALL_LAST_FRAME)
        frame->format = ctx->sample_fmt;
        frame->sample_rate = ctx->sample_rate;
        av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
        if (av_frame_get_buffer(frame, 0) < 0) return errAlloc;
        if (av_audio_fifo_read(fifo, reinterpret_cast<void**>(frame->data), n) < n) return errFail;
        frame->pts = nextPts;
        nextPts += n;
        if (StatusCode s = SendFrame(frame); s != errNone) return s;
    }
    return final ? SendFrame(nullptr) : errNone;
}

StatusCode AacAudioEncoder::SendFrame(AVFrame* f) {
    const int ret = avcodec_send_frame(ctx, f);
    if (ret < 0 && ret != AVERROR_EOF) {
        g_Log(logLevelError, "YouTube AMD Plugin :: AAC encode failed (%d)", ret);
        return errFail;
    }
    return Drain();
}

StatusCode AacAudioEncoder::Drain() {
    while (true) {
        const int ret = avcodec_receive_packet(ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return errNone;
        if (ret < 0) return errFail;

        HostBufferRef out(false);
        char* dst = nullptr;
        size_t dstSize = 0;
        if (!out.IsValid() || !out.Resize(pkt->size) || !out.LockBuffer(&dst, &dstSize)) {
            av_packet_unref(pkt);
            return errAlloc;
        }
        std::memcpy(dst, pkt->data, pkt->size);
        out.UnlockBuffer();

        // Keep FFmpeg's timestamps as they are: the first packet starts at -1024 (encoder priming).
        // Resolve's MP4 muxer turns that into an edit list, so players skip the priming and audio stays
        // in sync with the picture (measured: 0.04 ms). Shifting to 0 instead made audio 21 ms late.
        const int64_t pts = pkt->pts;
        const int64_t duration = pkt->duration;
        const uint32_t timeBase[2] = {1, sampleRate};
        const uint64_t bitRate = uint64_t(bitrateKbps) * 1000;
        const uint32_t layout = audLayoutStereo;
        const uint8_t key = 1;
        out.SetProperty(pIOPropPTS, propTypeInt64, &pts, 1);
        out.SetProperty(pIOPropDuration, propTypeInt64, &duration, 1);
        out.SetProperty(pIOPropTimeBase, propTypeUInt32, timeBase, 2);
        out.SetProperty(pIOPropSamplingRate, propTypeUInt32, &sampleRate, 1);
        out.SetProperty(pIOPropNumChannels, propTypeUInt32, &channels, 1);
        out.SetProperty(pIOPropAudioChannelLayout, propTypeUInt32, &layout, 1);
        out.SetProperty(pIOPropBitRate, propTypeUInt64, &bitRate, 1);
        out.SetProperty(pIOPropIsKeyFrame, propTypeUInt8, &key, 1);
        if (!cookie.empty()) {
            out.SetProperty(pIOPropMagicCookie, propTypeUInt8, cookie.data(), static_cast<int>(cookie.size()));
            const uint32_t cookieType = 'esds';
            out.SetProperty(pIOPropMagicCookieType, propTypeUInt32, &cookieType, 1);
        }
        av_packet_unref(pkt);

        m_pCallback->SendOutput(&out);
    }
}

void AacAudioEncoder::DoFlush() { DoProcess(nullptr); }
