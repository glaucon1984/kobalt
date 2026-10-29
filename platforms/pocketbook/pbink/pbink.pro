# Static Qt platform plugin "pbink": PocketBook inkview framebuffer + events.
TEMPLATE = lib
TARGET = pbink
CONFIG += static plugin c++14 warn_on
QT += core-private gui-private fb_support-private eventdispatcher_support-private \
     service_support-private fontdatabase_support-private

DEFINES += QT_NO_FOREACH

isEmpty(PB_SYSROOT): PB_SYSROOT = /work/sdk/SDK-B288/usr/arm-obreey-linux-gnueabi/sysroot
INCLUDEPATH += $$PB_SYSROOT/usr/local/include
LIBS += -L$$PB_SYSROOT/usr/local/lib -linkview

HEADERS += pbinkintegration.h pbinkscreen.h pbinkevents.h
SOURCES += main.cpp pbinkintegration.cpp pbinkscreen.cpp pbinkevents.cpp
OTHER_FILES += pbink.json
