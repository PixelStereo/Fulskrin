#include "Permissions.h"

#include <QCoreApplication>
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
        if (qApp->checkPermission(camera) == Qt::PermissionStatus::Undetermined)
            qApp->requestPermission(camera, qApp, [](const QPermission &) {});
    }
#endif
#ifdef Q_OS_MACOS
    requestLocalNetworkAccess();
#endif
}
