#include "VideoDecoder.h"
#include "Hap.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <mutex>
#include <utility>

// HAP packet decoding stays specialized so compressed texture blocks can be passed to the GPU.
int VideoDecoder::decodeHap(VideoFrame &f, double skipBefore)
{
    for (;;) {
        const int r = readVideoPacket();
        if (r < 0) {
            if (r == AVERROR_EOF) return 0;
            markDecodeError(r, true);
            return -1;
        }
        int64_t ts = m_packet->pts != AV_NOPTS_VALUE ? m_packet->pts : m_packet->dts;
        const double local = ts != AV_NOPTS_VALUE ? ts * m_timeBase - m_startTime : m_lastPts + 1.0 / m_fps;
        m_lastPts = local;
        f.pts = local;
        if (local < skipBefore - 0.25 / m_fps) { // not needed: not even decompressed
            av_packet_unref(m_packet);
            return 2;
        }
        hap::Frame hf;
        QString err;
        bool ok = hap::parse(m_packet->data, size_t(m_packet->size), &hf, &err);
        std::shared_ptr<const VideoLayout> L;
        if (ok) L = hapLayout(hf.count, int(hf.tex[0].format), hf.count > 1 ? int(hf.tex[1].format) : 0);
        if (ok && L)
            for (int i = 0; i < hf.count; ++i)
                if (hf.tex[i].outSize != hap::textureBytes(hf.tex[i].format, m_width, m_height)) ok = false;
        if (!ok || !L) {
            av_packet_unref(m_packet);
            return 2; // a damaged frame: skipped
        }
        m_stagingSize = L->totalBytes;
        f.layout = L;
        int id;
        uint8_t *dst = nullptr;
        if (takeStaging(L->totalBytes, &id, &dst)) {
            f.staging = id;
        } else {
            takePooledBytes(f);
            f.bytes.resize(L->totalBytes);
            dst = f.bytes.data();
        }
        for (int i = 0; i < hf.count && ok; ++i) {
            const VideoLayout::Plane &p = L->plane[i];
            if (p.texels != VideoLayout::Texels::U8) {
                ok = hap::decompress(hf.tex[i], dst + p.offset, p.size);
            } else { // the GPU does not sample this format: its blocks decoded here
                m_hapScratch.resize(hf.tex[i].outSize);
                ok = hap::decompress(hf.tex[i], m_hapScratch.data(), m_hapScratch.size());
                if (ok) hap::decodeBlocks(hf.tex[i].format, m_hapScratch.data(), m_width, m_height, dst + p.offset, p.rowBytes);
            }
        }
        av_packet_unref(m_packet);
        if (!ok) {
            recycle(std::move(f));
            return 2;
        }
        return 1;
    }
}

