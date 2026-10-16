// Linux link-time fault injection: production has no allocator hooks or test flags.
#include "AudioStream.h"
#include "VideoDecoder.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>
#include <cerrno>
#include <cstdio>

namespace {
enum Site { Codec, Parameters, Frame, Packet, SiteCount };
std::atomic<int> calls[SiteCount]{}, failAt[SiteCount]{};
bool fail(Site site) { const int n = ++calls[site]; return n == failAt[site].load(); }
void reset() { for (int i = 0; i < SiteCount; ++i) { calls[i] = 0; failAt[i] = 0; } }
std::atomic<int> seekMode{0}, seekCalls{0}, flushCalls{0}; // 1: both fail, 2: stream seek fails
int failures = 0;
void check(bool ok, const char *why) { if (!ok) { ++failures; std::printf("FAIL decoder_allocations: %s\n", why); } }
}
extern "C" {
int __real_av_seek_frame(AVFormatContext *, int, int64_t, int);
void __real_avcodec_flush_buffers(AVCodecContext *);
int __wrap_av_seek_frame(AVFormatContext *fmt, int stream, int64_t ts, int flags)
{
    ++seekCalls;
    if (seekMode == 1 || (seekMode == 2 && stream >= 0)) return AVERROR(EIO);
    return __real_av_seek_frame(fmt, stream, ts, flags);
}
void __wrap_avcodec_flush_buffers(AVCodecContext *codec)
{ ++flushCalls; __real_avcodec_flush_buffers(codec); }
AVCodecContext *__real_avcodec_alloc_context3(const AVCodec *);
AVFrame *__real_av_frame_alloc();
AVPacket *__real_av_packet_alloc();
int __real_avcodec_parameters_to_context(AVCodecContext *, const AVCodecParameters *);
AVCodecContext *__wrap_avcodec_alloc_context3(const AVCodec *codec)
{ return fail(Codec) ? nullptr : __real_avcodec_alloc_context3(codec); }
AVFrame *__wrap_av_frame_alloc()
{ return fail(Frame) ? nullptr : __real_av_frame_alloc(); }
AVPacket *__wrap_av_packet_alloc()
{ return fail(Packet) ? nullptr : __real_av_packet_alloc(); }
int __wrap_avcodec_parameters_to_context(AVCodecContext *codec, const AVCodecParameters *params)
{ return fail(Parameters) ? AVERROR(ENOMEM) : __real_avcodec_parameters_to_context(codec, params); }
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    VideoDecoder::setHardwareDecoding(false);
    const QString video = QString(TEST_DIR) + "/media/h264.mp4";
    const QString audio = QString(TEST_DIR) + "/media/tone.wav";
    const int initialThreads = VideoDecoder::softwareThreadsInUse();
    const int initialDecoders = VideoDecoder::softwareDecodersOpen();
    for (Site site : {Codec, Parameters, Frame, Packet}) {
        // Every failed initialization must clean up and leave the instance reusable.
        {
            reset(); failAt[site] = 1;
            VideoDecoder decoder;
            QString error;
            check(!decoder.open(video, &error), "video open rejects injected allocation failure");
            check(!error.isEmpty(), "video failure has an error message");
            check(VideoDecoder::softwareThreadsInUse() == initialThreads, "failed open releases thread reservation");
            reset();
            check(decoder.open(video, &error), "video reopens after failed initialization");
            decoder.close();
        }
        {
            reset(); failAt[site] = 1;
            AudioStream stream;
            QString error;
            check(!stream.open(audio, 48000, &error), "audio open rejects injected allocation failure");
            check(!error.isEmpty(), "audio failure has an error message");
            reset();
            check(stream.open(audio, 48000, &error), "audio reopens after failed initialization");
            stream.close();
        }
    }
    {
        reset(); failAt[Frame] = 2; // software transfer frame, after the primary frame
        VideoDecoder decoder;
        QString error;
        check(!decoder.open(video, &error), "video checks the second frame allocation");
    }
    {
        reset(); failAt[Frame] = 3; // two open-time frames, then first delivery without a PBO
        VideoDecoder decoder;
        QString error;
        check(decoder.open(video, &error), "delivery test opens normally");
        QElapsedTimer timer; timer.start();
        while (decoder.metrics().decodedFrames == 0 && timer.elapsed() < 2000) QThread::msleep(5);
        check(calls[Frame] >= 3, "delivery allocation failure was exercised");
        check(decoder.metrics().decodedFrames > 0, "decoder recovers after one undelivered frame");
        decoder.close();
    }
    // Forward and reverse: failures must not flush state, fabricate EOF, or retry in a hot loop.
    for (const int direction : {1, -1}) {
        reset();
        VideoDecoder decoder;
        QString error;
        check(decoder.open(video, &error), "seek test opens video");
        Timeline tl; tl.mode = Timeline::Once; tl.dir = direction;
        tl.origin = direction > 0 ? 0.0 : decoder.duration();
        decoder.setTimeline(tl);
        seekMode = 1; seekCalls = 0; flushCalls = 0;
        decoder.seek(0.2);
        QElapsedTimer timer; timer.start();
        while (decoder.metrics().lastAvError == 0 && timer.elapsed() < 2000) QThread::msleep(5);
        check(decoder.metrics().lastAvError == AVERROR(EIO), "failed video seek is reported");
        QThread::msleep(50);
        check(seekCalls == 2, "failed video seek attempts fallback once then waits");
        check(flushCalls == 0, "failed video seek does not flush codec");
        check(!decoder.finished(), "failed seek is not clean EOF");
        const auto decoded = decoder.metrics().decodedFrames;
        seekMode = 2;
        decoder.seek(0.0);
        timer.restart();
        while (decoder.metrics().decodedFrames == decoded && timer.elapsed() < 2000) QThread::msleep(5);
        check(decoder.metrics().decodedFrames > decoded, "video resumes through successful fallback");
        check(flushCalls > 0, "successful fallback flushes codec");
        decoder.close();
        seekMode = 0;
    }
    for (const int direction : {1, -1}) {
        reset();
        AudioStream stream;
        QString error;
        check(stream.open(audio, 48000, &error), "seek test opens audio");
        seekMode = 1; seekCalls = 0; flushCalls = 0;
        Timeline tl; tl.duration = stream.duration(); tl.mode = Timeline::Once; tl.dir = direction;
        tl.origin = direction > 0 ? 0.0 : stream.duration();
        float output[512]{};
        stream.setTransport(0.2, true, 1.0, tl, 1, 1.0f);
        stream.mix(output, 256, 0);
        QElapsedTimer timer; timer.start();
        while (stream.lastSeekError() == 0 && timer.elapsed() < 2000) QThread::msleep(5);
        check(stream.lastSeekError() == AVERROR(EIO), "failed audio seek is reported");
        QThread::msleep(50);
        check(seekCalls == 2, "failed audio seek attempts fallback once then waits");
        check(flushCalls == 0, "failed audio seek does not flush codec");
        seekMode = 2;
        stream.setTransport(0.0, true, 1.0, tl, 2, 1.0f);
        stream.mix(output, 256, 0);
        timer.restart();
        while (flushCalls == 0 && timer.elapsed() < 2000) QThread::msleep(5);
        check(flushCalls > 0, "audio retries on a new transport seek");
        // close joins the worker before inspecting its final published status.
        stream.close();
        check(stream.lastSeekError() == 0, "successful audio fallback clears the seek error");
        seekMode = 0;
    }
    check(VideoDecoder::softwareThreadsInUse() == initialThreads, "all video thread reservations released");
    check(VideoDecoder::softwareDecodersOpen() == initialDecoders, "all video decoders closed");
    std::printf("decoder_allocations: %d failures\n", failures);
    return failures ? 1 : 0;
}
