// Linux link-time fault injection: production has no allocator hooks or test flags.
#include "AudioStream.h"
#include "VideoDecoder.h"
extern "C" {
#include <libavcodec/avcodec.h>
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
int failures = 0;
void check(bool ok, const char *why) { if (!ok) { ++failures; std::printf("FAIL decoder_allocations: %s\n", why); } }
}
extern "C" {
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
    check(VideoDecoder::softwareThreadsInUse() == initialThreads, "all video thread reservations released");
    check(VideoDecoder::softwareDecodersOpen() == initialDecoders, "all video decoders closed");
    std::printf("decoder_allocations: %d failures\n", failures);
    return failures ? 1 : 0;
}
