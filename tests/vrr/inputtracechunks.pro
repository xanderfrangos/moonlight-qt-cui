include(inputpercentileintegration.pro)
TARGET = tst_inputtracechunks
SOURCES -= $$PWD/tst_inputpercentileintegration.cpp
SOURCES += $$PWD/tst_inputtracechunks.cpp
# MSVC's __FILE__ is relative to the build directory, and CI runs the tests
# from the repository root
DEFINES += INPUT_TRACE_FIXTURE=\\\"$$PWD/fixtures/buffer-drain-20261017.json.qz\\\"
