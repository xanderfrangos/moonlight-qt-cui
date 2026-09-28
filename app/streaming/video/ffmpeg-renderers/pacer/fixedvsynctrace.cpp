#include "fixedvsynctrace.h"

#include "SDL_compat.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>

#include <chrono>

std::unique_ptr<FixedVsyncTrace> FixedVsyncTrace::create(int mode, int sourceFps, int displayHz)
{
    const QString base = qEnvironmentVariable("MOONLIGHT_VRR_TRACE");
    // Keep network I/O away from frame delivery, like the VRR tracer
    if (base.isEmpty() || base.startsWith(QStringLiteral("//")) ||
            base.startsWith(QStringLiteral("\\\\"))) {
        return {};
    }

    static std::atomic<uint64_t> sequence{0};
    const QString path = base + QStringLiteral(".vsync-%1-%2-%3.csv")
        .arg(QCoreApplication::applicationPid())
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(sequence.fetch_add(1));
    return std::unique_ptr<FixedVsyncTrace>(new FixedVsyncTrace(path, mode, sourceFps, displayHz));
}

FixedVsyncTrace::FixedVsyncTrace(const QString& path, int mode, int sourceFps, int displayHz)
    : m_Thread([this, path, mode, sourceFps, displayHz] {
          write(path, mode, sourceFps, displayHz);
      })
{
}

FixedVsyncTrace::~FixedVsyncTrace()
{
    // The Pacer's V-sync thread and the decoder have stopped submitting.
    m_Stop.store(true);
    m_Thread.join();
}

void FixedVsyncTrace::record(Row row)
{
    if (!m_Accept.load(std::memory_order_relaxed) || !m_Queue.push(std::move(row))) {
        m_Dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void FixedVsyncTrace::write(const QString& path, int mode, int sourceFps, int displayHz)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        m_Accept.store(false);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Fixed V-Sync trace open failed: %s", qPrintable(path));
        return;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Fixed V-Sync trace v1: %s", qPrintable(path));

    QByteArray batch = QByteArray("# fixed_vsync_trace_version=1; vsync_mode=") +
        QByteArray::number(mode) + " (0=default 1=mailbox 2=smooth); stream_fps=" +
        QByteArray::number(sourceFps) + "; display_hz=" + QByteArray::number(displayHz) +
        "; times=LiGetMicroseconds; rtp=90 kHz host stamp\n"
        "# tick: a=sent b=skipped c=queue_before d=queue_after e=missed_slot f=late\n"
        "# admit: a=host_us b=smoothed_host_us c=transit_us d=desired_offset_us e=resynced f=rephased\n"
        "event,time_us,rtp,arrival_us,due_us,slot_us,a,b,c,d,e,f,offset_us,display_period_us\n";
    bool healthy = true;
    bool truncated = false;
    for (;;) {
        const bool stopping = m_Stop.load();
        Row row;
        while (m_Queue.pop(row)) {
            batch += row.event;
            const auto append = [&batch](auto value) {
                batch += ',';
                batch += QByteArray::number(value);
            };
            append(qulonglong(row.timeUs)); append(qlonglong(row.rtp));
            append(qulonglong(row.arrivalUs)); append(qulonglong(row.dueUs));
            append(qulonglong(row.slotUs));
            append(qlonglong(row.a)); append(qlonglong(row.b)); append(qlonglong(row.c));
            append(qlonglong(row.d)); append(qlonglong(row.e)); append(qlonglong(row.f));
            append(qlonglong(row.offsetUs)); append(qulonglong(row.displayPeriodUs));
            batch += '\n';
            if (batch.size() >= 65536) {
                break;
            }
        }
        if (!batch.isEmpty()) {
            healthy = file.write(batch) == batch.size();
            batch.clear();
        }
        else if (stopping) {
            break;
        }
        if (!healthy || file.pos() >= 256 * 1024 * 1024) {
            truncated = true;
            m_Accept.store(false);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Fixed V-Sync trace stopped: write failure or 256 MiB cap: %s",
                        qPrintable(path));
            break;
        }
        if (!stopping) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    const QByteArray footer = QByteArray("# closed=1; truncated=") + (truncated ? "1" : "0") +
        "; dropped_rows=" + QByteArray::number(qulonglong(m_Dropped.load())) + '\n';
    if (!(healthy && file.write(footer) == footer.size() && file.flush())) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Fixed V-Sync trace write failed: %s", qPrintable(path));
    }
}
