// Refresh hooks of the reader, mapped onto the pbink platform plugin: a "clean" page refresh
// becomes a full (flashing) e-ink update of the next frame; everything else stays with the
// plugin's per-frame partial update.
#include "platform/api/kobo_refresh.h"
#include <QGuiApplication>
#include <QWidget>
#include <qpa/qplatformnativeinterface.h>

namespace kobalt::platform {
namespace {
void flashNext() {
    auto *native = QGuiApplication::platformNativeInterface();
    if (!native)
        return;
    if (auto fn = native->platformFunction("pbinkFlashNext"))
        reinterpret_cast<void (*)()>(fn)();
}
}
// Kobalt asks for a flashing refresh by setting "reader_full_flash" on its window (page counts,
// chapter starts, menus); the host consumes that property and calls this.
void flashNextFrame() {
    flashNext();
}
// Kobalt sets "reader_animation_direction" on the reader's top-level window for a turn it wants
// animated; the host consumes it and the plugin sweeps the next frame in.
void animateNextFrame(int direction) {
    auto *native = QGuiApplication::platformNativeInterface();
    if (!native)
        return;
    if (auto fn = native->platformFunction("pbinkAnimateNext"))
        reinterpret_cast<void (*)(int)>(fn)(direction);
}
void textRefresh(QWidget *) {}
void pageRefresh(QWidget *widget, bool clean, bool) {
    if (!widget)
        return;
    if (clean) {
        flashNext();
        widget->update();
    }
}
void setReaderRefreshWindow(QWidget *) {}
}
// nickel4::configureLoopback comes from src/publication/loopback.cpp, as on Kobo.
