#include "logdispatch.h"

#include <QCoreApplication>
#include <QMutex>
#include <QThreadPool>
#include <QtDebug>

#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
int failures = 0;
int submissions = 0;
std::vector<QString> messages;
std::vector<QString> emergencyMessages;
QMutex poolLock;
QMutex writerLock;
bool emitDuringStart = false;
bool emitDuringWrite = false;

void check(bool condition, const char* description)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++failures;
    }
}

void log(const QString& message, bool asynchronous)
{
    LogDispatch::dispatch(asynchronous,
        [&] {
            // Reproduce Qt's worker-start warning while the pool mutex is
            // held. Re-submitting from its message handler would hang here.
            QMutexLocker lock(&poolLock);
            ++submissions;
            if (emitDuringStart) {
                emitDuringStart = false;
                qWarning("QThread::start: Failed to set thread priority");
            }
            messages.push_back(message);
        },
        [&] {
            QMutexLocker lock(&writerLock);
            if (emitDuringWrite) {
                emitDuringWrite = false;
                qWarning("writer warning");
            }
            messages.push_back(message);
        },
        [&] { emergencyMessages.push_back(message); });
}

void messageHandler(QtMsgType, const QMessageLogContext&, const QString& message)
{
    log(message, true);
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto oldHandler = qInstallMessageHandler(messageHandler);

    emitDuringStart = true;
    log(QStringLiteral("Timestamp pacing: queue full"), true);
    check(submissions == 1, "a worker-start warning never re-enters the locked pool");
    check(messages.size() == 2 && messages[0].contains(QStringLiteral("Failed to set thread priority")) &&
          messages[1] == QStringLiteral("Timestamp pacing: queue full"),
          "both the nested warning and original message survive");
    check(emergencyMessages.empty(), "worker-start warnings reach the normal log");

    emitDuringWrite = true;
    log(QStringLiteral("synchronous message"), false);
    check(emergencyMessages.size() == 1 && emergencyMessages[0] == QStringLiteral("writer warning"),
          "a warning from the writer never re-locks the writer or enters the pool");

    // The real asynchronous worker also needs the writing guard, since its
    // thread has no dispatch scope from the submitting thread.
    QThreadPool pool;
    pool.setMaxThreadCount(1);
    pool.setThreadPriority(QThread::NormalPriority);
    pool.start([] {
        LogDispatch::write([] {
            QMutexLocker lock(&writerLock);
            qWarning("asynchronous writer warning");
        });
    });
    check(pool.waitForDone(5000), "the real logging worker completes after a nested Qt warning");
    check(emergencyMessages.size() == 2 && emergencyMessages[1] == QStringLiteral("asynchronous writer warning"),
          "the asynchronous writer uses the independent emergency sink");
    check(submissions == 1, "neither writer warning submits to the pool");

    // Restore the thread-local state even if an operation throws.
    try {
        LogDispatch::dispatch(true, [] { throw std::runtime_error("injected"); }, [] {}, [] {});
    }
    catch (const std::runtime_error&) { }
    log(QStringLiteral("after failure"), true);
    check(submissions == 2, "failed submission does not leave dispatch in a nested state");

    qInstallMessageHandler(oldHandler);
    if (failures == 0) std::puts("All log dispatch regressions passed");
    return failures == 0 ? 0 : 1;
}
