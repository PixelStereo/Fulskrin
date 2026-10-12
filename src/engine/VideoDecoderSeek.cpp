#include "VideoDecoder.h"

#include <algorithm>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

// Seek to the nearest preceding keyframe and reset the codec's delayed-frame state.
void VideoDecoder::doSeek(double t)
{
    int64_t ts = int64_t((t + m_startTime) / m_timeBase);
    if (av_seek_frame(m_fmt, m_stream, ts, AVSEEK_FLAG_BACKWARD) < 0)
        av_seek_frame(m_fmt, -1, int64_t(t * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
    if (m_codec) avcodec_flush_buffers(m_codec);
    m_draining = false;
}


void VideoDecoder::seek(double c)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        ++m_generation;
        m_seekTarget = std::max(0.0, c);
        m_seekPending = true;
        m_needFirst = true;
        m_eof = false;
        for (VideoFrame &f : m_queue) recycleLocked(std::move(f));
        m_queue.clear();
    }
    m_cv.notify_all();
}

