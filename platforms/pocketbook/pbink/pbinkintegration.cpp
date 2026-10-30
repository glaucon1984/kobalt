#include "pbinkintegration.h"
#include "pbinkscreen.h"
#include "pbinkevents.h"
#include <QtFbSupport/private/qfbbackingstore_p.h>
#include <QtFbSupport/private/qfbwindow_p.h>
#include <QtFontDatabaseSupport/private/qgenericunixfontdatabase_p.h>
#include <QtServiceSupport/private/qgenericunixservices_p.h>
#include <QtEventDispatcherSupport/private/qgenericunixeventdispatcher_p.h>
#include <QtGui/private/qguiapplication_p.h>
#include <qpa/qplatforminputcontextfactory_p.h>
#include <qpa/qwindowsysteminterface.h>
#include <inkview.h> // has its own extern "C" guards

PbInkIntegration::PbInkIntegration(const QStringList &paramList)
    : m_fontDb(new QGenericUnixFontDatabase), m_services(new QGenericUnixServices)
{
    m_debug = paramList.contains(QLatin1String("debug"));
    m_screen = new PbInkScreen(paramList);
}

PbInkIntegration::~PbInkIntegration()
{
    delete m_events;
    QWindowSystemInterface::handleScreenRemoved(m_screen);
}

void PbInkIntegration::initialize()
{
    if (m_screen->initialize())
        QWindowSystemInterface::handleScreenAdded(m_screen);
    else
        qWarning("pbink: failed to initialize the screen");
    m_inputContext = QPlatformInputContextFactory::create();
    m_events = new PbInkEvents(m_screen, m_debug);
    m_events->start();
}

bool PbInkIntegration::hasCapability(QPlatformIntegration::Capability cap) const
{
    switch (cap) {
    case ThreadedPixmaps: return true;
    case WindowManagement: return false;
    default: return QPlatformIntegration::hasCapability(cap);
    }
}

QPlatformWindow *PbInkIntegration::createPlatformWindow(QWindow *window) const
{
    return new QFbWindow(window);
}

QPlatformBackingStore *PbInkIntegration::createPlatformBackingStore(QWindow *window) const
{
    return new QFbBackingStore(window);
}

QAbstractEventDispatcher *PbInkIntegration::createEventDispatcher() const
{
    return createUnixEventDispatcher();
}

QPlatformFontDatabase *PbInkIntegration::fontDatabase() const
{
    return m_fontDb.data();
}

QPlatformServices *PbInkIntegration::services() const
{
    return m_services.data();
}

QPlatformNativeInterface *PbInkIntegration::nativeInterface() const
{
    return const_cast<PbInkIntegration *>(this);
}

QList<QPlatformScreen *> PbInkIntegration::screens() const
{
    return { m_screen };
}

QFunctionPointer PbInkIntegration::platformFunction(const QByteArray &function) const
{
    if (function == "pbinkFullRefresh")
        return QFunctionPointer(fullRefreshStatic);
    if (function == "pbinkFlashNext")
        return QFunctionPointer(flashNextStatic);
    if (function == "pbinkAnimateNext")
        return QFunctionPointer(animateNextStatic);
    if (function == "pbinkSetKeepAwake")
        return QFunctionPointer(setKeepAwakeStatic);
    if (function == "pbinkEnsureNetwork")
        return QFunctionPointer(ensureNetworkStatic);
    if (function == "pbinkConnectNetwork")
        return QFunctionPointer(connectNetworkStatic);
    if (function == "pbinkSetRefresh")
        return QFunctionPointer(setRefreshStatic);
    if (function == "pbinkSetRotation")
        return QFunctionPointer(setRotationStatic);
    if (function == "pbinkRotation")
        return QFunctionPointer(rotationStatic);
    if (function == "pbinkPanelImage")
        return QFunctionPointer(panelImageStatic);
    if (function == "pbinkSetOrientation")
        return QFunctionPointer(setOrientationStatic);
    if (function == "pbinkNotifier")
        return QFunctionPointer(notifierStatic);
    if (function == "pbinkLastDeviceOrientation")
        return QFunctionPointer(lastDeviceOrientationStatic);
    return nullptr;
}

void PbInkIntegration::fullRefreshStatic()
{
    auto self = static_cast<PbInkIntegration *>(QGuiApplicationPrivate::platformIntegration());
    self->m_screen->repaintAll();
}

void PbInkIntegration::setRefreshStatic(int kind)
{
    auto self = static_cast<PbInkIntegration *>(QGuiApplicationPrivate::platformIntegration());
    self->m_screen->setDefaultRefresh(PbInkScreen::Refresh(kind));
}

void PbInkIntegration::setRotationStatic(int degrees)
{
    auto self = static_cast<PbInkIntegration *>(QGuiApplicationPrivate::platformIntegration());
    self->m_screen->setRotation(degrees);
}

int PbInkIntegration::rotationStatic()
{
    auto self = static_cast<PbInkIntegration *>(QGuiApplicationPrivate::platformIntegration());
    return self->m_screen->rotation();
}

QImage PbInkIntegration::panelImageStatic()
{
    auto self = static_cast<PbInkIntegration *>(QGuiApplicationPrivate::platformIntegration());
    return self->m_screen->panelImage();
}

void PbInkIntegration::setOrientationStatic(int orientation)
{
    auto self = static_cast<PbInkIntegration *>(QGuiApplicationPrivate::platformIntegration());
    self->m_screen->setOrientation(Qt::ScreenOrientation(orientation));
}

// A QObject with the signal deviceOrientation(int), for sensor backends built into the app.
QObject *PbInkIntegration::notifierStatic()
{
    auto self = static_cast<PbInkIntegration *>(QGuiApplicationPrivate::platformIntegration());
    return self->m_events;
}

int PbInkIntegration::lastDeviceOrientationStatic()
{
    auto self = static_cast<PbInkIntegration *>(QGuiApplicationPrivate::platformIntegration());
    return self->m_events ? self->m_events->lastOrientation() : -1;
}

// The next composed frame uses a full (flashing) e-ink update, without an extra repaint.
void PbInkIntegration::flashNextStatic()
{
    auto self = static_cast<PbInkIntegration *>(QGuiApplicationPrivate::platformIntegration());
    self->m_screen->requestFullRefresh();
}

void PbInkIntegration::animateNextStatic(int direction)
{
    auto self = static_cast<PbInkIntegration *>(QGuiApplicationPrivate::platformIntegration());
    self->m_screen->animateNext(direction);
}

// inkview suspends the device a few seconds after the last input (sleep mode 1), regardless of
// work in progress: a book opening without touches stalled until the next tap. Keep-awake turns
// that off (sleep mode 0) while the app is busy, and back on afterwards for idle reading.
void PbInkIntegration::setKeepAwakeStatic(int keep)
{
    iv_sleepmode(keep ? 0 : 1);
    qDebug("pbink: keep awake %s (inkview sleep mode %d)", keep ? "on" : "off", GetSleepmode());
}

// Bring Wi-Fi up through the firmware, as KOReader does (WiFiPower + NetConnect: the firmware
// connects to the known network, showing its own dialog if it has to). Blocks until done; the
// firmware draws over our panel meanwhile, so the screen is repainted afterwards.
// Returns 1 when connected.
int PbInkIntegration::ensureNetworkStatic()
{
    return connectNetworkStatic(1);
}

// Interactive: NetConnect, with the firmware's network dialog when no known network answers
// (sign-in, Sync now). Silent: NetConnectSilent, known networks only and no dialog (sync at
// book open and wake, where a dialog over the page would be unwelcome).
int PbInkIntegration::connectNetworkStatic(int interactive)
{
    auto self = static_cast<PbInkIntegration *>(QGuiApplicationPrivate::platformIntegration());
    if (QueryNetwork() & NET_CONNECTED)
        return 1;
    qInfo("pbink: network %d, connecting%s", QueryNetwork(), interactive ? "" : " silently");
    WiFiPower(1);
    const int result = interactive ? NetConnect(nullptr) : NetConnectSilent(nullptr);
    const bool connected = (QueryNetwork() & NET_CONNECTED) != 0;
    qInfo("pbink: %s returned %d, network %d, %s", interactive ? "NetConnect" : "NetConnectSilent", result,
          QueryNetwork(), connected ? "connected" : "not connected");
    self->m_screen->repaintAll();
    return connected ? 1 : 0;
}
