TEMPLATE = app
TARGET = tst_timestamptrace

QT += core testlib
QT -= gui
CONFIG += console testcase c++17
CONFIG -= app_bundle
DEFINES += SDL_MAIN_HANDLED

SOURCES += \
    $$PWD/tst_timestamptrace.cpp \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/timestamptrace.cpp \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/tracefile.cpp \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/timestamppacer.cpp \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtargetwaiter.cpp \
    $$PWD/../../app/streaming/video/pacinglog.cpp
HEADERS += \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/timestamppacer.h \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/timestamppacingpolicy.h \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/timestamptrace.h \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/tracefile.h \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/vrr/tracequeue.h

INCLUDEPATH += \
    $$PWD/../../app \
    $$PWD/../../moonlight-common-c/moonlight-common-c/src

win32 {
    contains(QT_ARCH, x86_64) {
        INCLUDEPATH += $$PWD/../../libs/windows/include/x64 \
                       $$PWD/../../libs/windows/include/x64/SDL2
        LIBS += -L$$PWD/../../libs/windows/lib/x64 -lavutil -lSDL2
    }
    contains(QT_ARCH, arm64) {
        INCLUDEPATH += $$PWD/../../libs/windows/include/arm64 \
                       $$PWD/../../libs/windows/include/arm64/SDL2
        LIBS += -L$$PWD/../../libs/windows/lib/arm64 -lavutil -lSDL2
    }
}

macx {
    !disable-prebuilts {
        INCLUDEPATH += $$PWD/../../libs/mac/include \
                       $$PWD/../../libs/mac/include/SDL2
        LIBS += -L$$PWD/../../libs/mac/lib -lavutil.60 -lSDL2
    } else {
        CONFIG += link_pkgconfig
        PKGCONFIG += libavutil sdl2
    }
}

unix:!macx {
    CONFIG += link_pkgconfig
    PKGCONFIG += libavutil sdl2
}
