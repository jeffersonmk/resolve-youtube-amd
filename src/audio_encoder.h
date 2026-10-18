#pragma once

#include <cstdint>
#include <vector>

#include "wrapper/plugin_api.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
}

namespace IOPlugin {

// AAC-LC stereo via FFmpeg's native encoder. Resolve on Linux has no AAC of its own.
class AacAudioEncoder final : public IPluginCodecRef {
   public:
    static const uint8_t s_UUID[16];

    AacAudioEncoder();
    ~AacAudioEncoder() override;

    static StatusCode RegisterCodecs(HostListRef* list);
    static StatusCode GetEncoderSettings(HostPropertyCollectionRef* values, HostListRef* settingsList);

   protected:
    StatusCode DoInit(HostPropertyCollectionRef* props) override;
    StatusCode DoOpen(HostBufferRef* buff) override;
    StatusCode DoProcess(HostBufferRef* buff) override;
    void DoFlush() override;

   private:
    StatusCode EncodeFrames(bool final);
    StatusCode SendFrame(AVFrame* frame);
    StatusCode Drain();

    uint32_t sampleRate{48000};
    uint32_t channels{2};
    uint32_t bitDepth{16};
    int32_t bitrateKbps{320};

    AVCodecContext* ctx{};
    AVAudioFifo* fifo{};
    AVFrame* frame{};
    AVPacket* pkt{};
    int64_t nextPts{};
    std::vector<uint8_t> cookie;
    bool flushed{};
};

}  // namespace IOPlugin
