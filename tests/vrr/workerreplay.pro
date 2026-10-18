include(pacingworker.pro)
TARGET = vrrworkerreplay
DEFINES += VRR_WORKER_REPLAY
# VRR_WORKER_REPLAY adds members to VrrPacingWorker. The other projects in
# this directory compile the same worker source without it, so keep these
# objects apart or a test can link an object built for the other layout.
CONFIG(debug, debug|release): OBJECTS_DIR = debug/workerreplay
else: OBJECTS_DIR = release/workerreplay
SOURCES -= $$PWD/tst_vrrpacingworker.cpp
SOURCES += $$PWD/vrrworkerreplay.cpp $$PWD/vrrreplayconfig.cpp
