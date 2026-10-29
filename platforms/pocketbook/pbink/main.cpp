// Qt platform plugin for PocketBook e-readers, drawing and reading input through inkview.
#include "pbinkintegration.h"
#include <qpa/qplatformintegrationplugin.h>

class PbInkIntegrationPlugin : public QPlatformIntegrationPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QPlatformIntegrationFactoryInterface_iid FILE "pbink.json")
public:
    QPlatformIntegration *create(const QString &system, const QStringList &paramList) override
    {
        if (!system.compare(QLatin1String("pbink"), Qt::CaseInsensitive))
            return new PbInkIntegration(paramList);
        return nullptr;
    }
};

#include "main.moc"
