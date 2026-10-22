QT = core testlib
CONFIG += console testcase c++11 link_pkgconfig
CONFIG -= app_bundle
PKGCONFIG += sdl2
TARGET = mousebridge-test
SOURCES += test.cpp ../../app/streaming/input/mousebridge.cpp
HEADERS += ../../app/streaming/input/mousebridge.h
QMAKE_LFLAGS += -Wl,--wrap=ioctl
