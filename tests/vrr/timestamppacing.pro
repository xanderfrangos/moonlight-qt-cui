TEMPLATE = app
TARGET = tst_timestamppacing
CONFIG += console c++17
CONFIG -= qt app_bundle
INCLUDEPATH += $$PWD/../../app
SOURCES += $$PWD/tst_timestamppacing.cpp
HEADERS += \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/timestamppacingpolicy.h \
    $$PWD/../../app/settings/timestamppacingoptions.h
