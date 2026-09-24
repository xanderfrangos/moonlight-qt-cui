# Build the same source list (including the parent-owned SDP wrapper) as the
# application, so this test also catches submodule integration changes.
include(../../moonlight-common-c/moonlight-common-c.pro)
TEMPLATE = app
TARGET = tst_sdpextensions
CONFIG -= staticlib app_bundle
CONFIG += console testcase
SOURCES += $$PWD/tst_sdpextensions.c

win32 {
    equals(QT_ARCH, i386): LIBS += -L$$PWD/../../libs/windows/lib/x86
    equals(QT_ARCH, x86_64): LIBS += -L$$PWD/../../libs/windows/lib/x64
    equals(QT_ARCH, arm64): LIBS += -L$$PWD/../../libs/windows/lib/arm64
    LIBS += -llibssl -llibcrypto ws2_32.lib winmm.lib advapi32.lib user32.lib
}
macx {
    LIBS += -L$$PWD/../../libs/mac/lib -lssl.3 -lcrypto.3
}
unix:!macx: LIBS += -lpthread
