#include "Permissions.h"

#include <QCoreApplication>
#include <QtGlobal>
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QPermissions>
#endif

#ifdef Q_OS_MACOS
void requestLocalNetworkAccess(); // Permissions.mm
#endif

void requestSystemPermissions()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (QCoreApplication::instance()) {
        // The microphone: asked too, so the app appears in Privacy › Microphone (no audio is captured yet)
        QMicrophonePermission microphone;
        if (qApp->checkPermission(microphone) == Qt::PermissionStatus::Undetermined)
            qApp->requestPermission(microphone, qApp, [](const QPermission &) {});
    }
#endif
#if defined(PINOL_HAS_CAMERA) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (QCoreApplication::instance()) {
        QCameraPermission camera;
        if (qApp->checkPermission(camera) == Qt::PermissionStatus::Undetermined)
            qApp->requestPermission(camera, qApp, [](const QPermission &) {});
    }
#endif
#ifdef Q_OS_MACOS
    requestLocalNetworkAccess();
#endif
}
