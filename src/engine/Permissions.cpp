#include "Permissions.h"

#include <QCoreApplication>
#include <QDebug>
#include <QtGlobal>
#ifdef FULSKRIN_HAS_CAMERA
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QPermissions>
#endif
#endif

#ifdef Q_OS_MACOS
void requestLocalNetworkAccess(); // Permissions.mm
#endif

void requestSystemPermissions()
{
#if defined(FULSKRIN_HAS_CAMERA) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (QCoreApplication::instance()) {
        QCameraPermission camera;
        const Qt::PermissionStatus status = qApp->checkPermission(camera);
        qInfo("Camera permission at startup: %s",
              status == Qt::PermissionStatus::Granted ? "granted"
              : status == Qt::PermissionStatus::Denied ? "denied" : "not asked yet");
        if (status == Qt::PermissionStatus::Undetermined)
            qApp->requestPermission(camera, qApp, [](const QPermission &p) {
                qInfo("Camera permission answer: %s",
                      p.status() == Qt::PermissionStatus::Granted ? "granted" : "not granted");
            });
    }
#endif
#ifdef Q_OS_MACOS
    requestLocalNetworkAccess();
#endif
}
