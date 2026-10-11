#pragma once
// NDI and OMT, loaded at run time (Publish.cpp): the structures of their SDKs this program uses, and their functions,
// to send (Publish.cpp) and to receive (LiveInput.cpp).
//   NDI: structures matching Processing.NDI.structs.h / Find.h / Send.h / Recv.h (SDK v5 and v6)
//   OMT: structures matching libomt.h (Open Media Transport, MIT license)

#include <QString>
#include <cstdint>

namespace ndi {
struct SendCreate {
    const char *p_ndi_name;
    const char *p_groups;
    bool clock_video, clock_audio;
};
struct VideoFrameV2 {
    int xres, yres;
    uint32_t FourCC;
    int frame_rate_N, frame_rate_D;
    float picture_aspect_ratio;
    int frame_format_type;
    int64_t timecode;
    uint8_t *p_data;
    int line_stride_in_bytes;
    const char *p_metadata;
    int64_t timestamp;
};
struct FindCreate {
    bool show_local_sources;
    const char *p_groups;
    const char *p_extra_ips;
};
struct Source {
    const char *p_ndi_name;
    const char *p_url_address;
};
struct RecvCreateV3 {
    Source source_to_connect_to;
    int color_format; // NDIlib_recv_color_format_e
    int bandwidth;    // NDIlib_recv_bandwidth_e
    bool allow_video_fields;
    const char *p_ndi_recv_name;
};
constexpr uint32_t fourcc(char a, char b, char c, char d)
{
    return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
}
constexpr uint32_t kBGRA = fourcc('B', 'G', 'R', 'A');
constexpr uint32_t kRGBA = fourcc('R', 'G', 'B', 'A'), kRGBX = fourcc('R', 'G', 'B', 'X');
constexpr int kProgressive = 1;
constexpr int kRecvRGBX_RGBA = 2, kRecvBandwidthHighest = 100;
constexpr int kFrameVideo = 1;

struct Lib {
    bool ok = false;
    QString error, path;
    bool (*initialize)() = nullptr;
    void *(*send_create)(const SendCreate *) = nullptr;
    void (*send_destroy)(void *) = nullptr;
    void (*send_video_async_v2)(void *, const VideoFrameV2 *) = nullptr;
    void (*send_video_v2)(void *, const VideoFrameV2 *) = nullptr;
    int (*send_get_no_connections)(void *, uint32_t) = nullptr;
    // Receiving (null with an older library)
    void *(*find_create_v2)(const FindCreate *) = nullptr;
    void (*find_destroy)(void *) = nullptr;
    const Source *(*find_get_current_sources)(void *, uint32_t *) = nullptr;
    void *(*recv_create_v3)(const RecvCreateV3 *) = nullptr;
    void (*recv_destroy)(void *) = nullptr;
    int (*recv_capture_v2)(void *, VideoFrameV2 *, void *, void *, uint32_t) = nullptr;
    void (*recv_free_video_v2)(void *, const VideoFrameV2 *) = nullptr;
    bool canReceive() const { return ok && find_create_v2 && find_get_current_sources && recv_create_v3 && recv_capture_v2 && recv_free_video_v2; }
};
Lib &lib(const QString &extraFolder);
} // namespace ndi

namespace omt {
struct MediaFrame {
    int Type;
    int64_t Timestamp;
    int Codec;
    int Width, Height, Stride;
    int Flags;
    int FrameRateN, FrameRateD;
    float AspectRatio;
    int ColorSpace;
    int SampleRate, Channels, SamplesPerChannel;
    void *Data;
    int DataLength;
    void *CompressedData;
    int CompressedLength;
    void *FrameMetadata;
    int FrameMetadataLength;
};
constexpr int kVideo = 2;          // OMTFrameType_Video
constexpr int kBGRA = 0x41524742;  // OMTCodec_BGRA
constexpr int kBT709 = 709;
constexpr int kPreferBGRA = 2;     // OMTPreferredVideoFormat_BGRA
constexpr int kReceiveDefault = 0; // OMTReceiveFlags_None

struct Lib {
    bool ok = false;
    QString error, path;
    void *(*send_create)(const char *, int) = nullptr;
    void (*send_destroy)(void *) = nullptr;
    int (*send)(void *, MediaFrame *) = nullptr;
    int (*send_connections)(void *) = nullptr;
    // Receiving and discovery (null with an older library)
    char **(*discovery_getaddresses)(int *) = nullptr;
    void *(*receive_create)(const char *, int, int, int) = nullptr;
    void (*receive_destroy)(void *) = nullptr;
    MediaFrame *(*receive)(void *, int, int) = nullptr;
    bool canReceive() const { return ok && discovery_getaddresses && receive_create && receive_destroy && receive; }
};
Lib &lib(const QString &extraFolder);
} // namespace omt
