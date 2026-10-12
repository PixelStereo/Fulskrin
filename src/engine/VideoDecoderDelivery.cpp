#include "VideoDecoder.h"
#include "VideoDecoderInternal.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <cstring>
#include <utility>


// Packed planes, at the layout's offsets.
static void packPlanes(const VideoLayout &L, const AVFrame *src, uint8_t *dst)
{
    for (int p = 0; p < L.planeCount; ++p) {
        const VideoLayout::Plane &pl = L.plane[p];
        uint8_t *o = dst + pl.offset;
        if (src->linesize[p] == int(pl.rowBytes)) {
            std::memcpy(o, src->data[p], pl.size);
            continue;
        }
        for (int y = 0; y < pl.height; ++y)
            std::memcpy(o + size_t(y) * pl.rowBytes, src->data[p] + ptrdiff_t(y) * src->linesize[p], pl.rowBytes);
    }
}



// Decoded picture -> mapped upload buffer, retained AVFrame, or CPU-converted RGBA.
bool VideoDecoder::deliver(AVFrame *src, VideoFrame &f, bool mapped)
{
    if (auto L = layoutFor(src)) {
        m_stagingSize = L->totalBytes;
        f.layout = L;
        int id;
        uint8_t *ptr;
        if (takeStaging(L->totalBytes, &id, &ptr)) {
            packPlanes(*L, src, ptr);
            f.staging = id;
            return true;
        }
        // No free upload buffer: the frame is kept as it is (copied out of the hardware's memory)
        AVFrame *keep = av_frame_alloc();
        if (mapped) {
            if (av_hwframe_transfer_data(keep, m_frame, 0) < 0) {
                av_frame_free(&keep);
                f.layout.reset();
                return false;
            }
            av_frame_copy_props(keep, src);
        } else {
            av_frame_move_ref(keep, src);
        }
        f.av = keep;
        return true;
    }

    // A format the GPU path does not take: RGBA on the CPU
    const int w = src->width, h = src->height;
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(AVPixelFormat(src->format));
    const bool alpha = d && (d->flags & AV_PIX_FMT_FLAG_ALPHA);
    if (!m_rgbaLayout || m_rgbaLayout->width != w || m_rgbaLayout->height != h || m_rgbaLayout->alpha != alpha) {
        auto L = std::make_shared<VideoLayout>();
        L->mode = VideoLayout::Mode::Rgb;
        L->width = w;
        L->height = h;
        L->planeCount = 1;
        L->plane[0].width = w;
        L->plane[0].height = h;
        L->plane[0].channels = 4;
        L->plane[0].rowBytes = size_t(w) * 4;
        L->plane[0].size = L->plane[0].rowBytes * size_t(h);
        L->totalBytes = L->plane[0].size;
        for (int c = 0; c < 4; ++c) {
            L->srcPlane[c] = 0;
            L->srcChannel[c] = c;
        }
        L->alpha = alpha;
        L->description = QString::fromUtf8(d ? d->name : "?") + QStringLiteral(" · converted on the CPU");
        m_rgbaLayout = L;
    }
    m_stagingSize = m_rgbaLayout->totalBytes;
    m_sws = sws_getCachedContext(m_sws, w, h, AVPixelFormat(src->format), w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                                 nullptr, nullptr);
    if (!m_sws) return false;
    {
        // Same matrix as the GPU path would take
        float unused[9];
        QString name;
        videoDecoderYuvMatrix(src->colorspace, h, unused, &name);
        const int cs = name == QLatin1String("BT.709")      ? SWS_CS_ITU709
                       : name == QLatin1String("BT.2020")   ? SWS_CS_BT2020
                       : name == QLatin1String("FCC")       ? SWS_CS_FCC
                       : name == QLatin1String("SMPTE 240M") ? SWS_CS_SMPTE240M
                                                            : SWS_CS_ITU601;
        int *inv, *tbl, srcRange, dstRange, bri, con, sat;
        if (sws_getColorspaceDetails(m_sws, &inv, &srcRange, &tbl, &dstRange, &bri, &con, &sat) >= 0)
            sws_setColorspaceDetails(m_sws, sws_getCoefficients(cs), src->color_range == AVCOL_RANGE_JPEG ? 1 : srcRange,
                                     tbl, dstRange, bri, con, sat);
    }
    f.layout = m_rgbaLayout;
    int id;
    uint8_t *ptr = nullptr;
    if (takeStaging(m_rgbaLayout->totalBytes, &id, &ptr)) {
        f.staging = id;
    } else {
        takePooledBytes(f);
        f.bytes.resize(m_rgbaLayout->totalBytes);
        ptr = f.bytes.data();
    }
    uint8_t *dst[4] = {ptr, nullptr, nullptr, nullptr};
    int dstStride[4] = {w * 4, 0, 0, 0};
    sws_scale(m_sws, src->data, src->linesize, 0, h, dst, dstStride);
    return true;
}

