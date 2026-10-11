#pragma once
// Live inputs: a layer's source that comes in as it is made — a camera (Qt Multimedia), an NDI or OMT source on the
// network, a Syphon server (macOS). Each one by a URI, stored in the project:
//   input:camera:<device id>   input:ndi:<source name>   input:omt:<address>   input:syphon:<server name>
//   input:test:<r>,<g>,<b>     (a solid color, made here: the tests)
// Frames arrive on threads of their own; the render thread takes the newest one (update) into the layer's texture.

#include "Gl.h"

#include <QSize>
#include <QString>
#include <memory>
#include <vector>

struct LiveInputInfo {
    QString uri;
    QString name; // as shown
    QString kind; // "Camera", "NDI", "OMT", "Syphon"
};

// What can be received now (cameras plugged in, NDI and OMT sources announced on the network, Syphon servers). The
// network ones take a moment to be found: listing again a little later finds more. libraryFolder: where NDI / OMT
// may be (as for publishing)
std::vector<LiveInputInfo> listLiveInputs(const QString &libraryFolder = QString());
inline bool isLiveInputUri(const QString &s) { return s.startsWith(QLatin1String("input:")); }
// As a (virtual) path, dragged from the Media Bin like a file: /Fulskrin/Inputs/<the URI, percent-encoded>
QString liveInputPath(const QString &uri);
QString liveInputUriOfPath(const QString &path); // empty: not one
QString liveInputName(const QString &uri); // its name, from its URI ("FaceTime HD Camera", "MACHINE (Source)")
QString liveInputKind(const QString &uri);

class LiveInput
{
public:
    virtual ~LiveInput() = default;
    // Render thread, context current: the newest picture into `tex` (RGBA); true when a new one came
    virtual bool update(Texture2D &tex) = 0;
    virtual QString status() const { return {}; } // why there is no picture (empty: fine)
};

// Started at once (its thread, its connection); null with `err` when it cannot be
std::unique_ptr<LiveInput> createLiveInput(const QString &uri, const QString &libraryFolder, QString *err);
