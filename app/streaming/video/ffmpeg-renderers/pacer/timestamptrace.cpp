#include "timestamptrace.h"

#include <Limelight.h>
#include <SDL.h>

#include <QByteArray>
#include <QFile>
#include <QFileInfo>

#include <chrono>

namespace {

// Increase when a column changes meaning or is removed. New columns are
// appended without a change.
constexpr int kSchemaVersion = 1;

constexpr char kHeader[] =
    "timestamp_trace_schema,sequence,event,event_us,"
    "frame,rtp_valid,rtp_timestamp,host_latency_us,frame_receive_us,frame_reassembled_us,"
    "decode_submit_us,decoder_output_us,decode_complete_us,decoder_queue_us,ready_us,"
    "paced,admitted,repeat,restarted,reanchored,late,policy_target_us,target_us,buffer_us,lateness_us,"
    "correction_us,host_jerk_us,host_jerk_valid,source_period_us,queue_depth,"
    "planned_release_us,vblank_grid,vblank_us,trim_us,render_lead_us,release_lead_us,extra_margin_us,"
    "wake_lead_us,scheduler_delay_us,scheduler_delay_valid,superseded,"
    "release_us,render_start_us,present_us,planned_present_us,"
    "present_start_us,display_us,refresh_period_us,grid_period_us,matched,missed,"
    "display_mode,display_mode_measured,refresh_share_per_mille,"
    "stream_fps,display_hz,smoothing,target_per_mille,min_buffer_ms,max_buffer_ms,vsync_margin_us,"
    "vblank_grid_available,present_flips_immediately,compositor_probe,measure_refresh,"
    "decode_wait_us,decode_delay_us,limit_refreshes,late_shift\n";

const char* eventName(TimestampTrace::Event event)
{
    switch (event) {
    case TimestampTrace::Event::Session: return "session";
    case TimestampTrace::Event::Scheduled: return "scheduled";
    case TimestampTrace::Event::Evicted: return "evicted";
    case TimestampTrace::Event::Superseded: return "superseded";
    case TimestampTrace::Event::Released: return "released";
    case TimestampTrace::Event::Presented: return "presented";
    case TimestampTrace::Event::Vsync: return "vsync";
    case TimestampTrace::Event::DisplayEvent: return "display_event";
    case TimestampTrace::Event::FrameDisplayed: return "frame_displayed";
    case TimestampTrace::Event::DisplayMode: return "display_mode";
    }
    return "unknown";
}

}

QString TimestampTrace::pathFor(const QString& vrrTracePath)
{
    QString base = vrrTracePath;
    QString extension = QStringLiteral(".tstrace");
    if (base.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive)) {
        base.chop(4);
        extension += QStringLiteral(".csv");
    }
    const QFileInfo info(base);
    if (!info.suffix().isEmpty() && !info.fileName().startsWith(QLatin1Char('.'))) {
        base.chop(info.suffix().size() + 1);
    }
    return base + extension;
}

std::unique_ptr<TimestampTrace> TimestampTrace::openIfRequested()
{
    // Settings sets this after SDL starts, and SDL2-compat may cache the
    // environment, so read the current process value
    const QString vrrPath = qEnvironmentVariable("MOONLIGHT_VRR_TRACE");
    if (vrrPath.isEmpty()) {
        return nullptr;
    }

    const QString path = pathFor(vrrPath);
    std::unique_ptr<TimestampTrace> trace(new TimestampTrace());
    if (!trace->m_File.open(path, kHeader, "Timestamp pacing", LiGetMicroseconds())) {
        return nullptr;
    }
    try {
        trace->m_Thread = std::thread(&TimestampTrace::run, trace.get());
    }
    catch (const std::exception& e) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Disabling timestamp pacing trace: writer thread failed: %s", e.what());
        trace->m_File.abandon();
        return nullptr;
    }
    trace->m_Accepting.store(true);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Timestamp pacing trace: %s",
                QFile::encodeName(path).constData());
    return trace;
}

TimestampTrace::~TimestampTrace()
{
    // Close admission before the writer drains, so no row lands after the
    // footer
    m_Accepting.store(false);
    m_Stopping.store(true);
    if (m_Thread.joinable()) {
        m_Thread.join();
    }

    const uint64_t dropped = m_Dropped.load();
    if (m_File.isOpen()) {
        m_File.close(QByteArrayLiteral("#timestamp_trace_footer,format_version=1,clean_shutdown=1,"
                                       "rows_allocated=") +
                     QByteArray::number(qulonglong(m_Sequence.load())) +
                     QByteArrayLiteral(",rows_enqueued=") +
                     QByteArray::number(qulonglong(m_Enqueued.load())) +
                     QByteArrayLiteral(",rows_dropped=") +
                     QByteArray::number(qulonglong(dropped)));
    }
    if (dropped != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Timestamp pacing trace dropped %llu rows to protect pacing",
                    (unsigned long long)dropped);
    }
}

void TimestampTrace::record(Row row)
{
    struct ProducerGuard {
        std::atomic_uint& active;
        explicit ProducerGuard(std::atomic_uint& value) : active(value) { ++active; }
        ~ProducerGuard() { --active; }
    } producer(m_ProducersActive);

    // The sequence orders rows from different threads, which can reach the
    // file out of order, and shows where any were dropped
    row.sequence = m_Sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!m_Accepting.load() || !m_Queue.push(std::move(row))) {
        m_Dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    m_Enqueued.fetch_add(1, std::memory_order_relaxed);
}

void TimestampTrace::run()
{
    Row row;
    while (true) {
        if (m_Queue.pop(row)) {
            write(row);
            continue;
        }
        if (m_Stopping.load() && m_ProducersActive.load() == 0) {
            // A producer could have published between the empty read and
            // the active-count check
            if (m_Queue.pop(row)) {
                write(row);
                continue;
            }
            m_File.flush();
            return;
        }
        // Producers never wait for the writer, so it polls
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void TimestampTrace::write(const Row& row)
{
    QByteArray line;
    line.reserve(512);
    const auto addUnsigned = [&line](uint64_t value) {
        line.append(',');
        line.append(QByteArray::number(qulonglong(value)));
    };
    const auto addSigned = [&line](int64_t value) {
        line.append(',');
        line.append(QByteArray::number(qlonglong(value)));
    };
    const auto addBool = [&line](bool value) {
        line.append(value ? ",1" : ",0");
    };

    line.append(QByteArray::number(kSchemaVersion));
    addUnsigned(row.sequence);
    line.append(',');
    line.append(eventName(row.event));
    addUnsigned(row.eventUs);

    addSigned(row.frameNumber);
    addBool(row.rtpValid);
    addUnsigned(row.rtpTimestamp);
    addUnsigned(row.hostLatencyUs);
    addUnsigned(row.receiveUs);
    addUnsigned(row.reassembledUs);
    addUnsigned(row.decodeSubmitUs);
    addUnsigned(row.decoderOutputUs);
    addUnsigned(row.decodeCompleteUs);
    addUnsigned(row.decoderQueueUs);
    addUnsigned(row.readyUs);

    addBool(row.paced);
    addBool(row.admitted);
    addBool(row.repeat);
    addBool(row.restarted);
    addBool(row.reanchored);
    addBool(row.late);
    addUnsigned(row.policyTargetUs);
    addUnsigned(row.targetUs);
    addUnsigned(row.bufferUs);
    addUnsigned(row.latenessUs);
    addSigned(row.correctionUs);
    addSigned(row.hostJerkUs);
    addBool(row.hostJerkValid);
    addUnsigned(row.sourcePeriodUs);
    addUnsigned(row.queueDepth);

    addUnsigned(row.plannedReleaseUs);
    addBool(row.vblankGrid);
    addUnsigned(row.vblankUs);
    addSigned(row.trimUs);
    addUnsigned(row.renderLeadUs);
    addUnsigned(row.releaseLeadUs);
    addUnsigned(row.extraMarginUs);
    addUnsigned(row.wakeLeadUs);
    addUnsigned(row.schedulerDelayUs);
    addBool(row.schedulerDelayValid);
    addUnsigned(row.superseded);

    addUnsigned(row.releaseUs);
    addUnsigned(row.renderStartUs);
    addUnsigned(row.presentUs);
    addUnsigned(row.plannedPresentUs);

    addUnsigned(row.presentStartUs);
    addUnsigned(row.displayUs);
    addUnsigned(row.refreshPeriodUs);
    addUnsigned(row.gridPeriodUs);
    addBool(row.matched);
    addBool(row.missed);

    addUnsigned(row.displayMode);
    addBool(row.displayModeMeasured);
    addUnsigned(row.refreshSharePerMille);

    addSigned(row.streamFps);
    addSigned(row.displayHz);
    addSigned(row.smoothing);
    addSigned(row.targetPerMille);
    addSigned(row.minBufferMs);
    addSigned(row.maxBufferMs);
    addSigned(row.vsyncMarginUs);
    addBool(row.vblankGridAvailable);
    addBool(row.presentFlipsImmediately);
    addBool(row.compositorProbe);
    addBool(row.measureRefresh);
    addUnsigned(row.decodeWaitUs);
    addUnsigned(row.decodeDelayUs);
    addUnsigned(row.limitRefreshes);
    addBool(row.lateShift);
    line.append('\n');

    // After a write failure or at the size cap, stop queueing rows
    if (!m_File.append(line, row.eventUs)) {
        m_Accepting.store(false);
    }
}
