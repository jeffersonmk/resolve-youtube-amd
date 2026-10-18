// Resolve YouTube AMD — DaVinci Resolve Studio encoder plugin (Linux, AMD VAAPI).

#include "plugin.h"

#include <cstring>

#include "audio_encoder.h"
#include "video_encoder.h"

using namespace IOPlugin;

static const uint8_t kPluginUUID[16] = {0x87, 0x71, 0x8b, 0x12, 0x1a, 0x60, 0x42, 0x2e,
                                        0xb4, 0xa3, 0x3e, 0x4d, 0x33, 0xcf, 0x62, 0x67};

StatusCode g_HandleGetInfo(HostPropertyCollectionRef* props) {
    StatusCode err = props->SetProperty(pIOPropUUID, propTypeUInt8, kPluginUUID, 16);
    if (err == errNone) {
        const char* name = "YouTube AMD Plugin";
        err = props->SetProperty(pIOPropName, propTypeString, name, static_cast<int>(strlen(name)));
    }
    return err;
}

StatusCode g_HandleCreateObj(unsigned char* uuid, ObjectRef* obj) {
    if (const VideoProfile* profile = VaapiVideoEncoder::FindProfile(uuid)) {
        *obj = new VaapiVideoEncoder(*profile);
        return errNone;
    }
    if (memcmp(uuid, AacAudioEncoder::s_UUID, 16) == 0) {
        *obj = new AacAudioEncoder();
        return errNone;
    }
    return errUnsupported;
}

StatusCode g_HandlePluginStart() { return errNone; }

StatusCode g_HandlePluginTerminate() { return errNone; }

StatusCode g_ListCodecs(HostListRef* list) {
    if (StatusCode err = VaapiVideoEncoder::RegisterCodecs(list); err != errNone) return err;
    if (GetHostAPI()->version >= 0x00000001) return AacAudioEncoder::RegisterCodecs(list);
    return errNone;
}

StatusCode g_ListContainers(HostListRef*) { return errNone; }

StatusCode g_GetEncoderSettings(unsigned char* uuid, HostPropertyCollectionRef* values, HostListRef* settingsList) {
    if (const VideoProfile* profile = VaapiVideoEncoder::FindProfile(uuid))
        return VaapiVideoEncoder::GetEncoderSettings(*profile, values, settingsList);
    if (memcmp(uuid, AacAudioEncoder::s_UUID, 16) == 0)
        return AacAudioEncoder::GetEncoderSettings(values, settingsList);
    return errNoCodec;
}
