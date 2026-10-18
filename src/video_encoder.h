#pragma once

#include <memory>
#include <string>

#include "wrapper/plugin_api.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

namespace IOPlugin {

enum RateControl : int32_t { rcConstantQuality = 0, rcVariableBitrate = 1 };

// One entry in Resolve's codec list. Everything a preset needs is here.
struct VideoProfile {
    uint8_t uuid[16];
    const char* group;       // Resolve groups codecs by this ("H.265", "H.264")
    const char* name;        // what the user sees in Deliver
    const char* encoder;     // ffmpeg encoder name
    const char* key;         // prefix for saved UI values
    uint32_t fourCC;
    int bitDepth;
    AVPixelFormat swFormat;  // what Resolve hands us / what we upload to the GPU
    int32_t defaultMode;
    int32_t defaultQP;
    int32_t defaultBitrateKbps;
    int32_t defaultMaxrateKbps;
    int32_t defaultBufsizeKbps;
};

extern const VideoProfile kHevc10Profile;
extern const VideoProfile kH264Profile;

class VideoSettings {
   public:
    explicit VideoSettings(const VideoProfile& profile);
    void Load(IPropertyProvider* values);
    StatusCode Render(HostListRef* list) const;

    int32_t mode;
    int32_t qp;
    int32_t bitrateKbps;
    int32_t maxrateKbps;
    int32_t bufsizeKbps;
    int32_t preset;

   private:
    std::string Id(const char* field) const;
    const VideoProfile* profile;
};

class VaapiVideoEncoder final : public IPluginCodecRef {
   public:
    explicit VaapiVideoEncoder(const VideoProfile& profile);
    ~VaapiVideoEncoder() override;

    static StatusCode RegisterCodecs(HostListRef* list);
    static StatusCode GetEncoderSettings(const VideoProfile& profile, HostPropertyCollectionRef* values,
                                         HostListRef* settingsList);
    static const VideoProfile* FindProfile(const uint8_t* uuid);

   protected:
    StatusCode DoInit(HostPropertyCollectionRef* props) override;
    StatusCode DoOpen(HostBufferRef* buff) override;
    StatusCode DoProcess(HostBufferRef* buff) override;
    void DoFlush() override;

   private:
    static bool IsSupported(const VideoProfile& profile);
    StatusCode Drain();

    const VideoProfile& profile;
    HostCodecConfigCommon commonProps{};
    int width{};
    int height{};
    AVCodecContext* ctx{};
    AVBufferRef* hwDevice{};
    AVPacket* pkt{};
    AVFrame* swFrame{};
    bool flushed{};
};

}  // namespace IOPlugin
