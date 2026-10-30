#pragma once
#include <qpa/qplatformintegration.h>
#include <qpa/qplatformnativeinterface.h>
#include <QScopedPointer>

class PbInkScreen;
class PbInkEvents;

class PbInkIntegration : public QPlatformIntegration, public QPlatformNativeInterface
{
public:
    explicit PbInkIntegration(const QStringList &paramList);
    ~PbInkIntegration() override;

    void initialize() override;
    bool hasCapability(QPlatformIntegration::Capability cap) const override;

    QPlatformWindow *createPlatformWindow(QWindow *window) const override;
    QPlatformBackingStore *createPlatformBackingStore(QWindow *window) const override;
    QAbstractEventDispatcher *createEventDispatcher() const override;

    QPlatformFontDatabase *fontDatabase() const override;
    QPlatformServices *services() const override;
    QPlatformInputContext *inputContext() const override { return m_inputContext; }
    QPlatformNativeInterface *nativeInterface() const override;

    QList<QPlatformScreen *> screens() const;
    QFunctionPointer platformFunction(const QByteArray &function) const override;

private:
    static void fullRefreshStatic();
    static void flashNextStatic();
    static void animateNextStatic(int direction);
    static void setKeepAwakeStatic(int keep);
    static int ensureNetworkStatic();
    static int connectNetworkStatic(int interactive);
    static void setRefreshStatic(int kind);
    static void setRotationStatic(int degrees);
    static int rotationStatic();
    static QImage panelImageStatic();
    static void setOrientationStatic(int orientation);
    static QObject *notifierStatic();
    static int lastDeviceOrientationStatic();

    PbInkScreen *m_screen;
    PbInkEvents *m_events = nullptr;
    QPlatformInputContext *m_inputContext = nullptr;
    QScopedPointer<QPlatformFontDatabase> m_fontDb;
    QScopedPointer<QPlatformServices> m_services;
    bool m_debug = false;
};
