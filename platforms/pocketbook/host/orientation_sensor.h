#pragma once
#include <QOrientationSensor>

namespace kobalt::pocketbook {
// A Qt Sensors orientation backend fed by inkview's gravity sensor events (through the pbink
// plugin), so that Kobalt's automatic rotation works as on devices with a real sensor.
// Returns a sensor using that backend; nullptr if the plugin offers no orientation events.
QOrientationSensor *createOrientationSensor(QObject *parent);
}
