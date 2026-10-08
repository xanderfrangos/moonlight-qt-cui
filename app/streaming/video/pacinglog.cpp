#include "pacinglog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QString>

#include <SDL.h>

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <chrono>
#include <thread>
#include <vector>

namespace PacingLog {

std::atomic<bool> g_Active{false};

namespace {

// Bounds memory if the writer falls behind: at 240 FPS this is several
// seconds of every event type.
constexpr size_t k_MaxPending = 65536;

std::mutex s_Lock;
std::condition_variable s_Wake;
std::vector<Record> s_Pending;
uint64_t s_Overflowed = 0;
bool s_Stopping = false;
std::thread s_Writer;
QFile* s_File = nullptr;

const char* eventName(Event event)
{
    switch (event) {
    case Event::Decoded: return "decoded";
    case Event::Presented: return "presented";
    case Event::Dropped: return "dropped";
    case Event::Vsync: return "vsync";
    case Event::Scheduled: return "scheduled";
    }
    return "unknown";
}

const char* reasonName(DropReason reason)
{
    switch (reason) {
    case DropReason::NotDropped: return "";
    case DropReason::VsyncCatchUp: return "vsync_catch_up";
    case DropReason::RenderCatchUp: return "render_catch_up";
    case DropReason::PacingQueueFull: return "pacing_queue_full";
    case DropReason::RenderQueueFull: return "render_queue_full";
    case DropReason::TimestampSuperseded: return "timestamp_superseded";
    case DropReason::TimestampQueueFull: return "timestamp_queue_full";
    }
    return "unknown";
}

// Empty for zero, so a column that doesn't apply to an event is blank
void appendTime(QByteArray& line, uint64_t us)
{
    line += ',';
    if (us != 0) {
        line += QByteArray::number((qulonglong)us);
    }
}

void writeRecords(const std::vector<Record>& records)
{
    QByteArray text;
    text.reserve((int)records.size() * 128);
    for (const Record& r : records) {
        text += eventName(r.event);
        appendTime(text, r.eventUs);
        text += ',';
        if (r.frameNumber >= 0) {
            text += QByteArray::number(r.frameNumber);
        }
        text += ',';
        if (r.rtpValid) {
            text += QByteArray::number(r.rtpTimestamp);
        }
        text += ',';
        if (r.event == Event::Decoded) {
            text += QByteArray::number(r.frameType);
            text += ',';
            text += QByteArray::number(r.bytes);
            text += ',';
            text += QByteArray::number(r.hostLatencyUs);
        }
        else {
            text += ",,";
        }
        appendTime(text, r.receiveUs);
        appendTime(text, r.reassembledUs);
        appendTime(text, r.presentationUs);
        appendTime(text, r.decodeSubmitUs);
        appendTime(text, r.decoderOutputUs);
        appendTime(text, r.renderBeginUs);
        text += ',';
        if (r.event == Event::Presented || r.event == Event::Dropped) {
            text += QByteArray::number(r.queueDepth);
        }
        text += ',';
        text += reasonName(r.reason);
        appendTime(text, r.targetUs);
        text += ',';
        if (r.event == Event::Scheduled) {
            text += QByteArray::number((qulonglong)r.bufferUs);
            text += ',';
            text += QByteArray::number((qulonglong)r.latenessUs);
            text += ',';
            text += r.repeat ? '1' : '0';
        }
        else {
            text += ",,";
        }
        text += '\n';
    }
    s_File->write(text);
}

void writerThread()
{
    std::vector<Record> batch;
    batch.reserve(k_MaxPending);
    for (;;) {
        bool stopping;
        uint64_t overflowed;
        {
            std::unique_lock<std::mutex> lock(s_Lock);
            s_Wake.wait_for(lock, std::chrono::milliseconds(250), [] { return s_Stopping; });
            // The swap hands the producers the already-reserved buffer
            batch.clear();
            batch.swap(s_Pending);
            stopping = s_Stopping;
            overflowed = s_Overflowed;
            s_Overflowed = 0;
        }

        writeRecords(batch);
        if (overflowed != 0) {
            s_File->write(QByteArray("# writer fell behind; discarded ") +
                          QByteArray::number((qulonglong)overflowed) + " records\n");
        }
        if (stopping) {
            return;
        }
    }
}

}

void start(const QString& description)
{
    stop();

    const QByteArray setting = qgetenv("MOONLIGHT_PACING_LOG");
    if (setting.isEmpty() || setting == "0") {
        return;
    }

    const QString path = QDir(QDir::tempPath()).filePath(
        QStringLiteral("moonlight-pacing-%1.csv")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))));
    auto* file = new QFile(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Pacing log: failed to open %s", qPrintable(path));
        delete file;
        return;
    }

    file->write("# Moonlight pacing log v2 (temporary diagnostics)\n");
    file->write("# Times are client monotonic microseconds (LiGetMicroseconds). rtp is the host's raw 90 kHz timestamp.\n");
    file->write("# " + description.toUtf8() + "\n");
    file->write("kind,event_us,frame,rtp,frame_type,bytes,host_latency_us,receive_us,reassembled_us,"
                "presentation_us,decode_submit_us,decoder_output_us,render_begin_us,queue_depth,reason,"
                "target_us,buffer_us,lateness_us,repeat\n");

    s_File = file;
    s_Pending.clear();
    s_Pending.reserve(k_MaxPending);
    s_Overflowed = 0;
    s_Stopping = false;
    s_Writer = std::thread(writerThread);
    g_Active.store(true, std::memory_order_release);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Pacing log: writing %s", qPrintable(path));
}

void stop()
{
    if (!s_Writer.joinable()) {
        return;
    }

    g_Active.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        s_Stopping = true;
    }
    s_Wake.notify_one();
    s_Writer.join();

    s_File->close();
    delete s_File;
    s_File = nullptr;
}

void record(const Record& record)
{
    if (!active()) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_Lock);
    if (s_Stopping) {
        return;
    }
    if (s_Pending.size() >= k_MaxPending) {
        s_Overflowed++;
        return;
    }
    s_Pending.push_back(record);
}

}
