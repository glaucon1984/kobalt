#include "orientation_sensor.h"
#include <QDateTime>
#include <QGuiApplication>
#include <QSensorBackend>
#include <QSensorManager>
#include <qpa/qplatformnativeinterface.h>
#include <inkview.h>

namespace kobalt::pocketbook {
namespace {
const QByteArray backendId = QByteArrayLiteral("pbink.orientation");

QObject *pluginNotifier() {
    auto *native = QGuiApplication::platformNativeInterface();
    if (auto fn = native ? native->platformFunction("pbinkNotifier") : nullptr)
        return reinterpret_cast<QObject *(*)()>(fn)();
    return nullptr;
}

// inkview's orientation codes, as KOReader decodes them: 0 upright, 2 turned clockwise,
// 3 upside down, 1 turned counter-clockwise. Qt names the edge that points up.
QOrientationReading::Orientation fromInkview(int value) {
    switch (value) {
    case 0: return QOrientationReading::TopUp;
    case 3: return QOrientationReading::TopDown;
    case 2: return QOrientationReading::LeftUp;
    case 1: return QOrientationReading::RightUp;
    default: return QOrientationReading::Undefined;
    }
}

class Backend final : public QSensorBackend {
    Q_OBJECT
public:
    explicit Backend(QSensor *sensor) : QSensorBackend(sensor) {
        setReading<QOrientationReading>(&m_reading);
        addDataRate(1, 10);
        setDescription(QStringLiteral("PocketBook gravity sensor (inkview)"));
        if (auto *notifier = pluginNotifier())
            connect(notifier, SIGNAL(deviceOrientation(int)), this, SLOT(apply(int)));
    }
    void start() override {
        m_running = true;
        // The last event the plugin saw keeps the reading consistent with what the host already
        // rotated to; inkview's GetGSensorOrientation() can lag behind the events.
        int value = -1;
        auto *native = QGuiApplication::platformNativeInterface();
        if (auto fn = native ? native->platformFunction("pbinkLastDeviceOrientation") : nullptr)
            value = reinterpret_cast<int (*)()>(fn)();
        if (value < 0)
            value = GetGSensorOrientation();
        qInfo("kobalt-pb: orientation sensor started, device code %d", value);
        apply(value);
    }
    void stop() override {
        m_running = false;
    }

public slots:
    void apply(int value) {
        if (!m_running)
            return;
        qInfo("kobalt-pb: orientation reading %d", value);
        const auto orientation = fromInkview(value);
        if (orientation == QOrientationReading::Undefined)
            return;
        m_reading.setOrientation(orientation);
        m_reading.setTimestamp(quint64(QDateTime::currentMSecsSinceEpoch()) * 1000);
        newReadingAvailable();
    }

private:
    QOrientationReading m_reading;
    bool m_running = false;
};

class Factory final : public QSensorBackendFactory {
public:
    QSensorBackend *createBackend(QSensor *sensor) override {
        return sensor->identifier() == backendId ? new Backend(sensor) : nullptr;
    }
};
}

QOrientationSensor *createOrientationSensor(QObject *parent) {
    if (!pluginNotifier())
        return nullptr;
    static Factory factory;
    if (!QSensorManager::isBackendRegistered(QOrientationSensor::type, backendId)) {
        QSensorManager::registerBackend(QOrientationSensor::type, backendId, &factory);
        QSensorManager::setDefaultBackend(QOrientationSensor::type, backendId);
    }
    auto *sensor = new QOrientationSensor(parent);
    sensor->setIdentifier(backendId);
    if (!sensor->connectToBackend()) {
        qWarning("kobalt-pb: orientation sensor backend did not connect");
        delete sensor;
        return nullptr;
    }
    return sensor;
}
}

#include "orientation_sensor.moc"
