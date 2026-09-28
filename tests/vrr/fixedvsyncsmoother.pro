TEMPLATE = app
TARGET = tst_fixedvsyncsmoother
CONFIG += console c++17
CONFIG -= qt app_bundle
SOURCES += $$PWD/tst_fixedvsyncsmoother.cpp
HEADERS += $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/fixedvsyncsmoother.h
HEADERS += $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/refreshclock.h
