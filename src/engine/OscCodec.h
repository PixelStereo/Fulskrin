#pragma once
// OSC 1.0 messages and bundles: encoding, decoding (packets from the network: every length is checked, a malformed
// or hostile packet is refused), address patterns. No dependency beyond Qt Core (fuzz target: test/fuzz_osc.cpp).

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <vector>

namespace osc {

struct Message {
    QString address;
    QString types;     // type tags without the leading ','
    QVariantList args; // int, float (double), QString, bool, QByteArray (blob), invalid (nil / impulse)
};

QByteArray encode(const Message &m);
// Decodes a message or a bundle (flattened, nested bundles included). Returns false if malformed.
bool decode(const QByteArray &packet, std::vector<Message> &out);
// OSC 1.0 address pattern matching (* ? [abc] [!a-z] {foo,bar}), segment by segment
bool match(const QString &pattern, const QString &address);
// Name usable as one segment of an OSC address
QString safeName(const QString &name);
// The segment of each effect of a layer (names made safe; a repeated name gets _2, _3…): the same in OSC addresses,
// timeline parameters and snapshot times
QStringList uniqueSegments(const QStringList &names);

} // namespace osc
