#pragma once
// Blackmagic DeckLink (SDI / HDMI cards, UltraStudio): the output ports this machine has, the video modes each one
// plays, and a viewport's picture sent to one of them (createDeckLinkPublisher, Publish.h).
//
// The SDK's headers are in the source tree (third_party/decklink, Boost license); the driver (Desktop Video) is loaded
// at run time: without it there is simply no port. The ports are looked up again on each call (a card plugged in,
// an UltraStudio connected, show up).

#include "Timecode.h"

#include <QSize>
#include <QString>
#include <functional>
#include <memory>
#include <vector>

namespace decklink {

struct Port {
    QString id;   // persistent: its persistent ID (else its topological ID, else its name)
    QString name; // "DeckLink Duo (1)"
};

struct Mode {
    QString key;  // its display mode code, four characters ("Hp25", "4k60"…)
    QString name; // "1080p25", "2160p59.94"…
    int width = 0, height = 0;
    double fps = 0; // frames (an interlaced mode: its frames, two fields each)
    bool interlaced = false;
};

bool compiledIn();                                 // built with the DeckLink SDK
QString driverVersion();                           // "12.4" (empty: no driver)
std::vector<Port> outputPorts();                   // the ports that play out (none without a driver or a card)
std::vector<Mode> outputModes(const QString &port); // the modes it plays (empty: no such port)
std::vector<Port> inputPorts();                    // the ports that take a signal in
// The mode for a picture of `size` at `fps` among `modes`: the same size, progressive, the closest rate (else 1080p)
const Mode *bestMode(const std::vector<Mode> &modes, QSize size, double fps);

// BGRA rows (top to bottom) to 8-bit 4:2:2 Y'CbCr ('2vuy': Cb Y0 Cr Y1), BT.709 (BT.601 below 720 lines), video range
void bgraToUyvy(const uint8_t *bgra, int stride, int width, int height, uint8_t *out, int outStride);

// The timecode an input carries (ATC: RP188, else VITC), in whatever mode comes in
class TimecodeInput
{
public:
    using Callback = std::function<void(const Timecode &)>; // each frame's (the card's thread)
    TimecodeInput();
    ~TimecodeInput();
    bool open(const QString &port, Callback cb, QString *err); // empty: the first input
    QString status() const;                                    // the port and the mode coming in

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace decklink
