TEMPLATE = app
TARGET = tst_logdispatch
QT = core
CONFIG += console c++17
CONFIG -= app_bundle
INCLUDEPATH += $$PWD/../../app
SOURCES += $$PWD/tst_logdispatch.cpp
HEADERS += $$PWD/../../app/logdispatch.h
