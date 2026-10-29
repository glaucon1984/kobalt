# Spike: a minimal Qt Widgets app running on a PocketBook through the pbink platform plugin.
TEMPLATE = app
TARGET = pbqt-hello
CONFIG += c++14 warn_on
QT += widgets core-private gui-private fb_support-private eventdispatcher_support-private \
     service_support-private fontdatabase_support-private

isEmpty(PB_SYSROOT): PB_SYSROOT = /work/sdk/SDK-B288/usr/arm-obreey-linux-gnueabi/sysroot
INCLUDEPATH += ../../pbink $$PB_SYSROOT/usr/local/include
LIBS += -L../../pbink -lpbink -L$$PB_SYSROOT/usr/local/lib -linkview
PRE_TARGETDEPS += ../../pbink/libpbink.a

QMAKE_LFLAGS += -static-libstdc++ -static-libgcc -Wl,-z,noexecstack

SOURCES += main.cpp
