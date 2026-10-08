TEMPLATE = app
TARGET = tst_presentationlateness
CONFIG += console c++17
CONFIG -= qt app_bundle
SOURCES += $$PWD/tst_presentationlateness.cpp
HEADERS += $$PWD/../../app/streaming/video/presentationlateness.h
