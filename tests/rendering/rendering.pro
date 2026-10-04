TEMPLATE = app
TARGET = tst_d3d11downscaling
QT += core qml
QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle
contains(QT_ARCH, arm64): LIBRARY_ARCH = ARM64
else: LIBRARY_ARCH = x64
INCLUDEPATH += $$PWD/../../app $$PWD/../../libs/windows/include \
               $$PWD/../../libs/windows/include/$$LIBRARY_ARCH $$PWD/../../libs/windows/include/$$LIBRARY_ARCH/SDL2
LIBS += -L$$PWD/../../libs/windows/lib/$$LIBRARY_ARCH -lSDL2 -llibplacebo -ld3d11 -ld3dcompiler -ldxgi
SOURCES += $$PWD/tst_d3d11downscaling.cpp \
           $$PWD/../../app/streaming/video/ffmpeg-renderers/d3d11downscaler.cpp \
           $$PWD/../../app/streaming/video/ffmpeg-renderers/d3d11upscaler.cpp \
           $$PWD/../../app/path.cpp
