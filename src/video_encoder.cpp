// Hardware (VAAPI) video encoders tuned for YouTube uploads on AMD GPUs.
// The Resolve <-> FFmpeg plumbing is adapted from EdvinNilsson/ffmpeg_encoder_plugin (GPL-3.0).

#include "video_encoder.h"

#include <cstring>
#include <algorithm>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/opt.h>
#include <va/va.h>
}

using namespace IOPlugin;

namespace {

std::string ErrStr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    return av_make_error_string(buf, sizeof(buf), err);
}

// Mesa radeonsi exposes three VCN quality presets through compression_level.
bool IsRadeonSi(AVBufferRef* device) {
    if (!device) return false;
    auto* hw = reinterpret_cast<AVHWDeviceContext*>(device->data);
    if (hw->type != AV_HWDEVICE_TYPE_VAAPI) return false;
    auto* va = static_cast<AVVAAPIDeviceContext*>(hw->hwctx);
    const char* vendor = va && va->display ? vaQueryVendorString(va->display) : nullptr;
    return vendor && std::strstr(vendor, "radeonsi");
}

const VideoProfile* const kProfiles[] = {&kHevc10Profile, &kH264Profile};

}  // namespace

// Recommended: HEVC 10-bit, constant quality QP 20 (measured VMAF ~96 on this machine; no banding).
const VideoProfile IOPlugin::kHevc10Profile = {
    .uuid = {0xde, 0x89, 0x15, 0x46, 0x1f, 0x5c, 0x46, 0x84, 0xaf, 0x61, 0x4f, 0x67, 0xb9, 0xe2, 0xe6, 0xc9},
    .group = "H.265",
    .name = "YouTube HEVC 10-bit (AMD VAAPI)",
    .encoder = "hevc_vaapi",
    .key = "yta_hevc10",
    .fourCC = 'hvc1',
    .bitDepth = 10,
    .swFormat = AV_PIX_FMT_P010,
    .defaultMode = rcConstantQuality,
    .defaultQP = 20,
    .defaultBitrateKbps = 20000,
    .defaultMaxrateKbps = 30000,
    .defaultBufsizeKbps = 40000,
};

// The user's original ffmpeg preset: h264_vaapi VBR 12M / maxrate 18M / bufsize 24M.
const VideoProfile IOPlugin::kH264Profile = {
    .uuid = {0x5c, 0xa4, 0x47, 0x0f, 0xb4, 0x3a, 0x43, 0xdf, 0x93, 0x8e, 0xaf, 0x17, 0x81, 0x35, 0xee, 0xa8},
    .group = "H.264",
    .name = "YouTube H.264 8-bit (AMD VAAPI)",
    .encoder = "h264_vaapi",
    .key = "yta_h264",
    .fourCC = 'avc1',
    .bitDepth = 8,
    .swFormat = AV_PIX_FMT_NV12,
    .defaultMode = rcVariableBitrate,
    .defaultQP = 20,
    .defaultBitrateKbps = 12000,
    .defaultMaxrateKbps = 18000,
    .defaultBufsizeKbps = 24000,
};

// ---------------------------------------------------------------- settings UI

VideoSettings::VideoSettings(const VideoProfile& p)
    : mode(p.defaultMode),
      qp(p.defaultQP),
      bitrateKbps(p.defaultBitrateKbps),
      maxrateKbps(p.defaultMaxrateKbps),
      bufsizeKbps(p.defaultBufsizeKbps),
      preset(2),
      profile(&p) {}

std::string VideoSettings::Id(const char* field) const { return std::string(profile->key) + "_" + field; }

void VideoSettings::Load(IPropertyProvider* values) {
    uint8_t reset = 0;
    values->GetUINT8(Id("reset").c_str(), reset);
    if (reset) {
        *this = VideoSettings(*profile);
        return;
    }
    values->GetINT32(Id("mode").c_str(), mode);
    values->GetINT32(Id("qp").c_str(), qp);
    values->GetINT32(Id("bitrate").c_str(), bitrateKbps);
    values->GetINT32(Id("maxrate").c_str(), maxrateKbps);
    values->GetINT32(Id("bufsize").c_str(), bufsizeKbps);
    values->GetINT32(Id("preset").c_str(), preset);
    if (mode != rcConstantQuality && mode != rcVariableBitrate) mode = profile->defaultMode;
    if (preset < 0 || preset > 2) preset = 2;
}

StatusCode VideoSettings::Render(HostListRef* list) const {
    auto append = [&](HostUIConfigEntryRef& item) {
        if (!item.IsSuccess() || !list->Append(&item)) {
            g_Log(logLevelError, "YouTube AMD Plugin :: Failed to build settings UI");
            return false;
        }
        return true;
    };

    {
        HostUIConfigEntryRef item(Id("preset"));
        item.MakeComboBox("Encoder Preset", {"Speed", "Balanced", "Quality"}, {0, 1, 2}, preset);
        if (!append(item)) return errFail;
    }
    {
        HostUIConfigEntryRef item(Id("mode"));
        item.MakeRadioBox("Rate Control", {"Constant Quality", "Variable Bitrate"},
                          {rcConstantQuality, rcVariableBitrate}, mode);
        item.SetTriggersUpdate(true);
        if (!append(item)) return errFail;
    }
    {
        HostUIConfigEntryRef item(Id("qp"));
        const char* hint = qp <= 20 ? "(high)" : qp <= 28 ? "(medium)" : "(low)";
        item.MakeSlider("QP", hint, qp, 1, 51, profile->defaultQP);
        item.SetTriggersUpdate(true);
        item.SetHidden(mode != rcConstantQuality);
        if (!append(item)) return errFail;
    }
    {
        HostUIConfigEntryRef item(Id("bitrate"));
        item.MakeSlider("Bit Rate", "kb/s", bitrateKbps, 1000, 150000, profile->defaultBitrateKbps, 500);
        item.SetHidden(mode != rcVariableBitrate);
        if (!append(item)) return errFail;
    }
    {
        HostUIConfigEntryRef item(Id("maxrate"));
        item.MakeSlider("Max Bit Rate", "kb/s", maxrateKbps, 1000, 200000, profile->defaultMaxrateKbps, 500);
        item.SetHidden(mode != rcVariableBitrate);
        if (!append(item)) return errFail;
    }
    {
        HostUIConfigEntryRef item(Id("bufsize"));
        item.MakeSlider("Buffer Size", "kb", bufsizeKbps, 1000, 300000, profile->defaultBufsizeKbps, 500);
        item.SetHidden(mode != rcVariableBitrate);
        if (!append(item)) return errFail;
    }
    {
        HostUIConfigEntryRef item(Id("reset"));
        item.MakeButton("Reset to YouTube defaults");
        item.SetTriggersUpdate(true);
        if (!append(item)) return errFail;
    }
    return errNone;
}

// ---------------------------------------------------------------- registration

const VideoProfile* VaapiVideoEncoder::FindProfile(const uint8_t* uuid) {
    for (const VideoProfile* p : kProfiles)
        if (std::memcmp(uuid, p->uuid, 16) == 0) return p;
    return nullptr;
}

bool VaapiVideoEncoder::IsSupported(const VideoProfile& p) {
    const int oldLevel = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET);

    bool ok = false;
    AVBufferRef* device = nullptr;
    AVBufferRef* frames = nullptr;
    AVCodecContext* c = nullptr;
    const AVCodec* codec = avcodec_find_encoder_by_name(p.encoder);

    if (codec && av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) >= 0 &&
        (frames = av_hwframe_ctx_alloc(device)) && (c = avcodec_alloc_context3(codec))) {
        auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
        fc->format = AV_PIX_FMT_VAAPI;
        fc->sw_format = p.swFormat;
        fc->width = 1920;
        fc->height = 1080;
        if (av_hwframe_ctx_init(frames) >= 0) {
            c->pix_fmt = AV_PIX_FMT_VAAPI;
            c->width = 1920;
            c->height = 1080;
            c->time_base = {1, 30};
            c->hw_frames_ctx = av_buffer_ref(frames);
            ok = avcodec_open2(c, codec, nullptr) >= 0;
        }
    }

    avcodec_free_context(&c);
    av_buffer_unref(&frames);
    av_buffer_unref(&device);
    av_log_set_level(oldLevel);
    return ok;
}

StatusCode VaapiVideoEncoder::RegisterCodecs(HostListRef* list) {
    for (const VideoProfile* p : kProfiles) {
        if (!IsSupported(*p)) {
            g_Log(logLevelWarn, "YouTube AMD Plugin :: '%s' is not available on this GPU/driver, skipping", p->name);
            continue;
        }

        HostPropertyCollectionRef info;
        if (!info.IsValid()) return errAlloc;

        info.SetProperty(pIOPropUUID, propTypeUInt8, p->uuid, 16);
        info.SetProperty(pIOPropGroup, propTypeString, p->group, static_cast<int>(strlen(p->group)));
        info.SetProperty(pIOPropName, propTypeString, p->name, static_cast<int>(strlen(p->name)));
        info.SetProperty(pIOPropFourCC, propTypeUInt32, &p->fourCC, 1);

        const uint32_t media = mediaVideo;
        info.SetProperty(pIOPropMediaType, propTypeUInt32, &media, 1);
        const uint32_t dir = dirEncode;
        info.SetProperty(pIOPropCodecDirection, propTypeUInt32, &dir, 1);

        const uint32_t color = clrNV12;
        info.SetProperty(pIOPropColorModel, propTypeUInt32, &color, 1);
        const uint8_t sub = 2;
        info.SetProperty(pIOPropHSubsampling, propTypeUInt8, &sub, 1);
        info.SetProperty(pIOPropVSubsampling, propTypeUInt8, &sub, 1);

        const uint8_t dataRange[] = {0, 1};
        info.SetProperty(pIOPropDataRange, propTypeUInt8, dataRange, sizeof(dataRange));

        const uint32_t depth = p->bitDepth;
        info.SetProperty(pIOPropBitDepth, propTypeUInt32, &depth, 1);
        info.SetProperty(pIOPropBitsPerSample, propTypeUInt32, &depth, 1);

        const uint32_t reorder = 0;
        info.SetProperty(pIOPropTemporalReordering, propTypeUInt32, &reorder, 1);
        const uint8_t fields = fieldProgressive | fieldTop | fieldBottom;
        info.SetProperty(pIOPropFieldOrder, propTypeUInt8, &fields, 1);
        const uint8_t threadSafe = 1;
        info.SetProperty(pIOPropThreadSafe, propTypeUInt8, &threadSafe, 1);
        const uint8_t hwAcc = 1;
        info.SetProperty(pIOPropHWAcc, propTypeUInt8, &hwAcc, 1);

        static const char kContainers[] = "mp4\0mov";
        info.SetProperty(pIOPropContainerList, propTypeString, kContainers, sizeof(kContainers) - 1);

        if (!list->Append(&info)) return errFail;
        g_Log(logLevelInfo, "YouTube AMD Plugin :: registered '%s'", p->name);
    }
    return errNone;
}

StatusCode VaapiVideoEncoder::GetEncoderSettings(const VideoProfile& profile, HostPropertyCollectionRef* values,
                                                 HostListRef* settingsList) {
    VideoSettings settings(profile);
    settings.Load(values);
    return settings.Render(settingsList);
}

// ---------------------------------------------------------------- encoding

VaapiVideoEncoder::VaapiVideoEncoder(const VideoProfile& p) : profile(p) {}

VaapiVideoEncoder::~VaapiVideoEncoder() {
    avcodec_free_context(&ctx);
    av_buffer_unref(&hwDevice);
    av_packet_free(&pkt);
    av_frame_free(&swFrame);
}

StatusCode VaapiVideoEncoder::DoInit(HostPropertyCollectionRef*) { return errNone; }

StatusCode VaapiVideoEncoder::DoOpen(HostBufferRef* buff) {
    commonProps.Load(buff);
    VideoSettings settings(profile);
    settings.Load(buff);

    int16_t matrix{}, primaries{}, transfer{};
    uint8_t range{};
    if (!buff->GetINT16(pIOColorMatrix, matrix) || !buff->GetINT16(pIOPropColorPrimaries, primaries) ||
        !buff->GetINT16(pIOTransferCharacteristics, transfer) || !buff->GetUINT8(pIOPropDataRange, range)) {
        return errNoParam;
    }

    const uint8_t multiPass = 0;
    buff->SetProperty(pIOPropMultiPass, propTypeUInt8, &multiPass, 1);

    width = static_cast<int>(commonProps.GetWidth());
    height = static_cast<int>(commonProps.GetHeight());
    const int fpsNum = static_cast<int>(commonProps.GetFrameRateNum());
    const int fpsDen = static_cast<int>(commonProps.GetFrameRateDen());

    const AVCodec* codec = avcodec_find_encoder_by_name(profile.encoder);
    if (!codec) {
        g_Log(logLevelError, "YouTube AMD Plugin :: FFmpeg encoder '%s' not found", profile.encoder);
        return errNoCodec;
    }

    int err = av_hwdevice_ctx_create(&hwDevice, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
    if (err < 0) {
        g_Log(logLevelError, "YouTube AMD Plugin :: Cannot open VAAPI device: %s", ErrStr(err).c_str());
        return errUnsupported;
    }

    ctx = avcodec_alloc_context3(codec);
    if (!ctx) return errAlloc;

    ctx->pix_fmt = AV_PIX_FMT_VAAPI;
    ctx->width = width;
    ctx->height = height;
    ctx->time_base = {fpsDen, fpsNum};
    ctx->framerate = {fpsNum, fpsDen};
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ctx->colorspace = static_cast<AVColorSpace>(matrix);
    ctx->color_primaries = static_cast<AVColorPrimaries>(primaries);
    ctx->color_trc = static_cast<AVColorTransferCharacteristic>(transfer);
    ctx->color_range = range == 1 ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;

    if (settings.mode == rcConstantQuality) {
        av_opt_set(ctx->priv_data, "rc_mode", "CQP", 0);
        ctx->global_quality = settings.qp;
    } else {
        av_opt_set(ctx->priv_data, "rc_mode", "VBR", 0);
        ctx->bit_rate = int64_t(settings.bitrateKbps) * 1000;
        ctx->rc_max_rate = int64_t(std::max(settings.maxrateKbps, settings.bitrateKbps)) * 1000;
        ctx->rc_buffer_size = settings.bufsizeKbps * 1000;
    }

    if (IsRadeonSi(hwDevice)) {
        // bit0 = valid, bits1-2 = preset (speed/balanced/quality), bit3 = pre-encode, bit4 = VBAQ
        ctx->compression_level = settings.preset << 1 | 1 << 3 | 1 << 4 | 1;
    }

    AVBufferRef* frames = av_hwframe_ctx_alloc(hwDevice);
    if (!frames) return errAlloc;
    auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
    fc->format = AV_PIX_FMT_VAAPI;
    fc->sw_format = profile.swFormat;
    fc->width = width;
    fc->height = height;
    fc->initial_pool_size = 20;
    if ((err = av_hwframe_ctx_init(frames)) < 0) {
        g_Log(logLevelError, "YouTube AMD Plugin :: Cannot init VAAPI frames: %s", ErrStr(err).c_str());
        av_buffer_unref(&frames);
        return errUnsupported;
    }
    ctx->hw_frames_ctx = frames;  // ownership moves to the codec context

    if ((err = avcodec_open2(ctx, codec, nullptr)) < 0) {
        const std::string msg = std::string("Could not open ") + profile.encoder + ": " + ErrStr(err);
        g_Log(logLevelError, "YouTube AMD Plugin :: %s", msg.c_str());
        buff->SetProperty(pIOCustomErrorString, propTypeString, msg.c_str(), static_cast<int>(msg.size()));
        return errNoCodec;
    }

    g_Log(logLevelInfo, "YouTube AMD Plugin :: %s %dx%d @ %d/%d, %s, preset %d", profile.encoder, width, height,
          fpsNum, fpsDen,
          settings.mode == rcConstantQuality
              ? ("QP " + std::to_string(settings.qp)).c_str()
              : ("VBR " + std::to_string(settings.bitrateKbps) + "/" + std::to_string(settings.maxrateKbps) +
                 "/" + std::to_string(settings.bufsizeKbps) + " kb/s")
                    .c_str(),
          settings.preset);

    pkt = av_packet_alloc();
    swFrame = av_frame_alloc();
    if (!pkt || !swFrame) return errAlloc;
    swFrame->format = profile.swFormat;
    swFrame->width = width;
    swFrame->height = height;
    av_image_fill_linesizes(swFrame->linesize, profile.swFormat, width);

    buff->SetProperty(pIOPropMagicCookie, propTypeUInt8, ctx->extradata, ctx->extradata_size);
    const uint32_t cookieType = profile.fourCC == 'avc1' ? 'avcC' : 0;
    buff->SetProperty(pIOPropMagicCookieType, propTypeUInt32, &cookieType, 1);

    const uint32_t reorder = ctx->has_b_frames;
    buff->SetProperty(pIOPropTemporalReordering, propTypeUInt32, &reorder, 1);
    return errNone;
}

StatusCode VaapiVideoEncoder::DoProcess(HostBufferRef* buff) {
    int ret;
    if (buff == nullptr || !buff->IsValid()) {
        if (flushed) return errNone;
        flushed = true;
        ret = avcodec_send_frame(ctx, nullptr);
    } else {
        char* data = nullptr;
        size_t size = 0;
        if (!buff->LockBuffer(&data, &size) || !data || size == 0) {
            g_Log(logLevelError, "YouTube AMD Plugin :: Empty video buffer");
            return errFail;
        }
        int64_t pts = 0;
        if (!buff->GetINT64(pIOPropPTS, pts)) {
            buff->UnlockBuffer();
            return errNoParam;
        }
        if (av_image_fill_pointers(swFrame->data, profile.swFormat, height, reinterpret_cast<uint8_t*>(data),
                                   swFrame->linesize) < 0) {
            buff->UnlockBuffer();
            return errFail;
        }

        AVFrame* hw = av_frame_alloc();
        if (!hw) {
            buff->UnlockBuffer();
            return errAlloc;
        }
        int err = av_hwframe_get_buffer(ctx->hw_frames_ctx, hw, 0);
        if (err >= 0) err = av_hwframe_transfer_data(hw, swFrame, 0);
        buff->UnlockBuffer();
        if (err < 0) {
            g_Log(logLevelError, "YouTube AMD Plugin :: GPU upload failed: %s", ErrStr(err).c_str());
            av_frame_free(&hw);
            return errFail;
        }
        hw->pts = pts;
        ret = avcodec_send_frame(ctx, hw);
        av_frame_free(&hw);
    }

    if (ret < 0 && ret != AVERROR_EOF) {
        g_Log(logLevelError, "YouTube AMD Plugin :: Encode failed: %s", ErrStr(ret).c_str());
        return errFail;
    }
    return Drain();
}

StatusCode VaapiVideoEncoder::Drain() {
    while (true) {
        const int ret = avcodec_receive_packet(ctx, pkt);
        if (ret == AVERROR(EAGAIN)) return errMoreData;
        if (ret == AVERROR_EOF) return errNone;
        if (ret < 0) {
            g_Log(logLevelError, "YouTube AMD Plugin :: Read packet failed: %s", ErrStr(ret).c_str());
            return errFail;
        }

        HostBufferRef out(false);
        char* dst = nullptr;
        size_t dstSize = 0;
        if (!out.IsValid() || !out.Resize(pkt->size) || !out.LockBuffer(&dst, &dstSize)) {
            av_packet_unref(pkt);
            return errAlloc;
        }
        std::memcpy(dst, pkt->data, pkt->size);
        out.SetProperty(pIOPropPTS, propTypeInt64, &pkt->pts, 1);
        out.SetProperty(pIOPropDTS, propTypeInt64, &pkt->dts, 1);
        const uint8_t key = (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
        out.SetProperty(pIOPropIsKeyFrame, propTypeUInt8, &key, 1);
        av_packet_unref(pkt);

        m_pCallback->SendOutput(&out);
    }
}

void VaapiVideoEncoder::DoFlush() { DoProcess(nullptr); }
