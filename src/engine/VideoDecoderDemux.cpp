#include "VideoDecoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

// Return the next packet from the selected video stream, discarding packets from other streams.
// A negative FFmpeg result is propagated to the caller for EOF/drain handling.
int VideoDecoder::readVideoPacket()
{
    for (;;) {
        const int result = av_read_frame(m_fmt, m_packet);
        if (result < 0) return result;
        if (m_packet->stream_index == m_stream) return result;
        av_packet_unref(m_packet);
    }
}
