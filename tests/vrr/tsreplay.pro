TEMPLATE = app
TARGET = tsreplay

QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle

INCLUDEPATH += $$PWD/../../app

SOURCES += $$PWD/tsreplay.cpp
HEADERS += \
    $$PWD/tsreplay.h \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/timestamppacingpolicy.h \
    $$PWD/../../app/settings/timestamppacingoptions.h
