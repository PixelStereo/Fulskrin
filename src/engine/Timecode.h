#pragma once
// SMPTE timecode: hours, minutes, seconds and frames at a rate (24, 25, 29.97 drop frame, 30), and how it travels:
//  - LTC (linear timecode): 80-bit frames in a biphase-mark audio signal (SMPTE 12M);
//  - MTC (MIDI timecode): quarter-frame messages, 8 for a whole timecode over two frames, and full-frame SysEx;
//  - ATC (ancillary timecode): carried by the video itself (SDI, or NDI's frames), read where it comes in.
// Pure functions and small state machines, no I/O (TimecodeClock does it).

#include <QString>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

enum class TcRate { Fps24 = 0, Fps25 = 1, Fps2997Df = 2, Fps30 = 3 }; // the MTC rate codes

struct Timecode {
    int h = 0, m = 0, s = 0, f = 0;
    TcRate rate = TcRate::Fps25;

    static int nominalFps(TcRate r) { return r == TcRate::Fps24 ? 24 : r == TcRate::Fps25 ? 25 : 30; }
    static double realFps(TcRate r) { return r == TcRate::Fps2997Df ? 30000.0 / 1001.0 : nominalFps(r); }
    static bool dropFrame(TcRate r) { return r == TcRate::Fps2997Df; }
    static QString rateName(TcRate r); // "25 fps", "29.97 fps drop frame"…
    static constexpr int64_t kDayFrames30 = 24LL * 3600 * 30;
    static int64_t framesPerDay(TcRate r);

    bool valid() const;
    int64_t toFrames() const;                          // frames since 00:00:00:00 (drop frame: the numbers skipped)
    static Timecode fromFrames(int64_t n, TcRate r);   // wraps every 24 hours, negative from the end
    double toSeconds() const { return double(toFrames()) / realFps(rate); }
    static Timecode fromSeconds(double s, TcRate r);   // the frame being shown at `s`
    QString toString() const;                          // "HH:MM:SS:FF" (";" before the frames in drop frame)
    static std::optional<Timecode> parse(const QString &s, TcRate r); // "HH:MM:SS:FF" or ";" or "."
    bool operator==(const Timecode &o) const { return h == o.h && m == o.m && s == o.s && f == o.f && rate == o.rate; }
    bool operator!=(const Timecode &o) const { return !(*this == o); }
};

// What to send next: `step` frames after the last one sent, unless the clock is elsewhere (a jump, another rate): then
// the clock's. Sent timecode runs on evenly while the clock's own reading jitters.
class TcSequence
{
public:
    std::optional<Timecode> next(const std::optional<Timecode> &clock, int step = 1);
    bool jumped() const { return m_jumped; } // the last next() did not follow on
    void reset() { m_last.reset(); }

private:
    std::optional<Timecode> m_last;
    bool m_jumped = false;
};

// A clock at an instant, as the outputs send it: a frame count with its fraction (offset included)
struct TcReading {
    double frames = 0;    // since 00:00:00:00, real frames (drop frame: the numbers skipped are not counted)
    TcRate rate = TcRate::Fps25;
    bool running = false; // advancing (else held: a pause, a source that stopped coming)
    bool forward = true;  // a ping-pong coming back runs backwards
    Timecode timecode() const;
};

namespace ltc {

// The 80 bits of a frame, bit 0 first (as sent): BCD digits, flags, user bits at 0, the sync word, the polarity bit
// set so that each frame holds an even number of zeros
std::array<uint8_t, 80> encodeFrame(const Timecode &tc);
// Back to a timecode (nullopt: not a frame, or digits out of range). The rate: the drop-frame flag, else the hint.
std::optional<Timecode> decodeFrame(const std::array<uint8_t, 80> &bits, TcRate hint);

// Biphase mark audio: a transition at the start of each bit, one more in its middle for a 1
class Encoder
{
public:
    explicit Encoder(int sampleRate, float amplitude = 0.5f) : m_rate(sampleRate), m_amp(amplitude) {}
    void frame(const Timecode &tc, std::vector<float> &out); // appends one frame's samples (1/fps s)

private:
    int m_rate;
    float m_amp, m_level = 1.0f;
    double m_phase = 0; // fraction of a sample carried over (29.97 fps)
};

// A clock's timecode as LTC audio, frame after frame: running forwards, frames that follow on evenly (TcSequence);
// held or backwards, the frame it shows, again and again (receivers locate on it); silence while it shows none
class Generator
{
public:
    explicit Generator(int sampleRate) : m_rate(sampleRate), m_enc(sampleRate) {}
    // `frames` samples on each of `channels` (interleaved); clock: called once per LTC frame
    void render(float *out, int frames, int channels, const std::function<std::optional<TcReading>()> &clock);

private:
    int m_rate;
    Encoder m_enc;
    TcSequence m_seq;
    std::vector<float> m_buf;
    size_t m_pos = 0;
};

class Decoder
{
public:
    explicit Decoder(int sampleRate);
    void push(const float *samples, size_t n); // mono
    std::optional<Timecode> take();            // the last frame read since the last take
    double fps() const;                        // measured from the bit period
    void setRateHint(TcRate r) { m_hint = r; }

private:
    void edge(double length);
    void bit(uint8_t b);
    int m_rate;
    double m_period;   // samples per bit (adaptive)
    double m_count = 0;
    bool m_high = false, m_half = false;
    std::array<uint8_t, 80> m_bits{};
    int m_have = 0;    // bits received (up to 80)
    TcRate m_hint = TcRate::Fps25;
    std::optional<Timecode> m_last;
};

} // namespace ltc

namespace mtc {

// Quarter frame `piece` (0 to 7) of a timecode: the data byte after 0xF1
uint8_t quarterFrame(const Timecode &tc, int piece);
// Full frame SysEx: F0 7F 7F 01 01 hh mm ss ff F7
std::vector<uint8_t> fullFrame(const Timecode &tc);

class Decoder
{
public:
    // A MIDI message (status byte first). True when it changed the timecode.
    bool push(const uint8_t *msg, size_t len);
    std::optional<Timecode> current() const { return m_current; }

private:
    std::array<int, 8> m_pieces{};
    int m_received = 0; // bits: the pieces received since piece 0
    std::optional<Timecode> m_current;
};

} // namespace mtc
