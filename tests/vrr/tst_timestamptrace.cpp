#include "../../app/streaming/video/ffmpeg-renderers/pacer/pacertelemetry.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/timestamppacer.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/timestamptrace.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

#include <chrono>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

extern "C" uint64_t LiGetMicroseconds()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

namespace {

// Expands a chunk-compressed trace as scripts/decode-vrr-trace.py does
QByteArray decode(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray bytes = file.readAll();
    if (!bytes.startsWith("MLVRR1\n")) {
        return {};
    }
    QByteArray text;
    qsizetype offset = 7;
    while (offset + 4 <= bytes.size()) {
        const auto* length = reinterpret_cast<const unsigned char*>(bytes.constData() + offset);
        const qsizetype compressed = qsizetype(length[0]) | qsizetype(length[1]) << 8 |
                                     qsizetype(length[2]) << 16 | qsizetype(length[3]) << 24;
        offset += 4;
        if (offset + compressed > bytes.size()) {
            return {};
        }
        text += qUncompress(bytes.mid(offset, compressed));
        offset += compressed;
    }
    return offset == bytes.size() ? text : QByteArray();
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

TimestampTrace::Row frameRow(TimestampTrace::Event event, int32_t frame)
{
    TimestampTrace::Row row;
    row.event = event;
    row.eventUs = LiGetMicroseconds();
    row.frameNumber = frame;
    row.rtpValid = true;
    row.rtpTimestamp = uint32_t(frame) * 1500;
    row.targetUs = row.eventUs + 8000;
    row.trimUs = -250;
    return row;
}

}

class TimestampTraceTest : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        qunsetenv("MOONLIGHT_VRR_TRACE");
    }

    void cleanup()
    {
        qunsetenv("MOONLIGHT_VRR_TRACE");
    }

    void pathBesideVrrTrace()
    {
        QCOMPARE(TimestampTrace::pathFor("C:/captures/Moonlight.vrrtrace"),
                 QString("C:/captures/Moonlight.tstrace"));
        QCOMPARE(TimestampTrace::pathFor("C:/a.b/trace"), QString("C:/a.b/trace.tstrace"));
        QCOMPARE(TimestampTrace::pathFor("trace.vrrtrace.csv"), QString("trace.tstrace.csv"));
        QCOMPARE(TimestampTrace::pathFor("trace.CSV"), QString("trace.tstrace.csv"));
    }

    void disabledWithoutVrrTrace()
    {
        QVERIFY(TimestampTrace::openIfRequested() == nullptr);
    }

    void recordsEveryThreadsRows()
    {
        QTemporaryDir directory;
        const QString vrrPath = directory.filePath("Moonlight.vrrtrace");
        qputenv("MOONLIGHT_VRR_TRACE", vrrPath.toUtf8());

        constexpr int threads = 4;
        constexpr int rowsPerThread = 2000;
        {
            auto trace = TimestampTrace::openIfRequested();
            QVERIFY(trace != nullptr);
            std::vector<std::thread> producers;
            for (int t = 0; t < threads; t++) {
                producers.emplace_back([&trace, t] {
                    for (int i = 0; i < rowsPerThread; i++) {
                        trace->record(frameRow(t % 2 ? TimestampTrace::Event::Released
                                                     : TimestampTrace::Event::Scheduled,
                                               t * rowsPerThread + i));
                    }
                });
            }
            for (auto& producer : producers) {
                producer.join();
            }
        }

        // Only its own file, beside where the VRR trace would be
        QVERIFY(!QFile::exists(vrrPath));
        const QByteArray text = decode(directory.filePath("Moonlight.tstrace"));
        QVERIFY(!text.isEmpty());
        QList<QByteArray> lines = text.split('\n');
        QVERIFY(lines.last().isEmpty());
        lines.removeLast();

        const QByteArray header = lines.first();
        QVERIFY(header.startsWith("timestamp_trace_schema,sequence,event,event_us,frame,"));
        const qsizetype columns = header.count(',') + 1;

        const QByteArray footer = lines.last();
        QVERIFY(footer.startsWith("#timestamp_trace_footer,format_version=1,clean_shutdown=1,"));
        QVERIFY(footer.contains(",size_capped=0,write_failed=0,"));
        // Contended producers may give up on a slot rather than wait, as VRR
        // Pacing Mode's do. Every row is either written or counted as dropped.
        const auto footerCount = [&footer](const QByteArray& name) {
            const qsizetype start = footer.indexOf("," + name + "=") + name.size() + 2;
            return footer.mid(start, footer.indexOf(',', start) - start).toLongLong();
        };
        const qlonglong enqueued = footerCount("rows_enqueued");
        const qlonglong dropped = footerCount("rows_dropped");
        QCOMPARE(footerCount("rows_allocated"), qlonglong(threads * rowsPerThread));
        QCOMPARE(enqueued + dropped, qlonglong(threads * rowsPerThread));
        QVERIFY(dropped < threads * rowsPerThread / 100);
        QCOMPARE(qlonglong(lines.size()), enqueued + 2);
        // The hash covers everything before the footer
        const QByteArray body = text.left(text.size() - footer.size() - 1);
        QVERIFY(footer.endsWith("decoded_sha256=" +
                                QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex()));

        QSet<qulonglong> sequences;
        QSet<int> frames;
        int scheduled = 0;
        for (qsizetype i = 1; i + 1 < lines.size(); i++) {
            const QList<QByteArray> fields = lines[i].split(',');
            QCOMPARE(fields.size(), columns);
            QCOMPARE(fields[0], QByteArray("1"));
            sequences.insert(fields[1].toULongLong());
            QVERIFY(fields[2] == "scheduled" || fields[2] == "released");
            scheduled += fields[2] == "scheduled";
            frames.insert(fields[4].toInt());
        }
        QCOMPARE(qlonglong(sequences.size()), enqueued);
        QCOMPARE(qlonglong(frames.size()), enqueued);
        for (qulonglong sequence : sequences) {
            QVERIFY(sequence >= 1 && sequence <= qulonglong(threads * rowsPerThread));
        }
        QVERIFY(scheduled > 0 && scheduled < enqueued);

        // Signed columns keep their sign
        const QList<QByteArray> names = header.split(',');
        const QList<QByteArray> first = lines[1].split(',');
        QCOMPARE(first[names.indexOf("trim_us")], QByteArray("-250"));
    }

    void preservesEarlierConnection()
    {
        QTemporaryDir directory;
        qputenv("MOONLIGHT_VRR_TRACE", directory.filePath("Moonlight.vrrtrace").toUtf8());

        for (int connection = 0; connection < 2; connection++) {
            auto trace = TimestampTrace::openIfRequested();
            QVERIFY(trace != nullptr);
            trace->record(frameRow(TimestampTrace::Event::Scheduled, 100 + connection));
        }

        const QByteArray first = decode(directory.filePath("Moonlight-connection-1.tstrace"));
        const QByteArray latest = decode(directory.filePath("Moonlight.tstrace"));
        QVERIFY(first.contains(",scheduled,") && first.contains(",100,"));
        QVERIFY(latest.contains(",scheduled,") && latest.contains(",101,"));
        QVERIFY(!latest.contains(",100,"));
    }

    void csvPathWritesPlainCsv()
    {
        QTemporaryDir directory;
        qputenv("MOONLIGHT_VRR_TRACE", directory.filePath("Moonlight.vrrtrace.csv").toUtf8());
        {
            auto trace = TimestampTrace::openIfRequested();
            QVERIFY(trace != nullptr);
            TimestampTrace::Row row;
            row.event = TimestampTrace::Event::Vsync;
            row.eventUs = 12345;
            trace->record(row);
        }
        const QByteArray text = readAll(directory.filePath("Moonlight.tstrace.csv"));
        QVERIFY(text.startsWith("timestamp_trace_schema,"));
        QVERIFY(text.contains("\n1,1,vsync,12345,-1,"));
        QVERIFY(text.contains("#timestamp_trace_footer,format_version=1,clean_shutdown=1,rows_allocated=1,"));
    }

    // A real pacer at 60 FPS on a 60 Hz V-blank grid. Every frame must be
    // traced as scheduled and then released, superseded or evicted, and
    // V-sync rows recorded after stop() must still land in the trace.
    void pacerTracesEveryFrame()
    {
        QTemporaryDir directory;
        qputenv("MOONLIGHT_VRR_TRACE", directory.filePath("Moonlight.vrrtrace").toUtf8());

        constexpr int frames = 60;
        constexpr uint64_t periodUs = 16667;
        PacerTelemetry telemetry;
        std::atomic_int released { 0 };
        std::atomic_int dropped { 0 };
        TimestampPacer* pacer = nullptr;
        TimestampPacer::Callbacks callbacks;
        callbacks.release = [&](AVFrame* frame) {
            released++;
            const uint64_t nowUs = LiGetMicroseconds();
            pacer->notePresented(nowUs, nowUs + 500, 0, 0, true, uint32_t(frame->pts));
            av_frame_free(&frame);
        };
        callbacks.drop = [&](AVFrame* frame, bool) {
            dropped++;
            av_frame_free(&frame);
        };

        TimestampPacingOptions options;
        options.enabled = true;
        {
            TimestampPacer timestampPacer(options, 60, 60, true, false, &telemetry, callbacks);
            pacer = &timestampPacer;
            QVERIFY(timestampPacer.start());

            std::atomic_bool stopVsync { false };
            std::thread vsync([&] {
                uint64_t nextUs = LiGetMicroseconds();
                while (!stopVsync.load()) {
                    nextUs += periodUs;
                    while (LiGetMicroseconds() < nextUs) {
                        std::this_thread::sleep_for(std::chrono::microseconds(500));
                    }
                    timestampPacer.onVsync(nextUs);
                }
            });

            for (int i = 0; i < frames; i++) {
                AVFrame* frame = av_frame_alloc();
                frame->pts = int64_t(i) * 1500;
                PacedFrame paced(frame, i + 1, uint32_t(i * 1500), true, LiGetMicroseconds());
                paced.setHostLatencyUs(3000);
                timestampPacer.submit(std::move(paced));
                std::this_thread::sleep_for(std::chrono::microseconds(periodUs));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            timestampPacer.stop();
            // The owner stops V-sync after the pacer, as Pacer does
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            stopVsync.store(true);
            vsync.join();
        }
        QCOMPARE(released.load() + dropped.load(), frames);

        const QByteArray text = decode(directory.filePath("Moonlight.tstrace"));
        QVERIFY(!text.isEmpty());
        QList<QByteArray> lines = text.split('\n');
        lines.removeLast();
        const QList<QByteArray> names = lines.first().split(',');
        QVERIFY(lines.last().contains(",rows_dropped=0,"));
        const qsizetype vblankGrid = names.indexOf("vblank_grid");
        const qsizetype vblankUs = names.indexOf("vblank_us");
        const qsizetype streamFps = names.indexOf("stream_fps");

        QMap<QByteArray, int> counts;
        uint64_t lastVsyncUs = 0;
        int onGrid = 0;
        for (qsizetype i = 1; i + 1 < lines.size(); i++) {
            const QList<QByteArray> fields = lines[i].split(',');
            QCOMPARE(fields.size(), names.size());
            counts[fields[2]]++;
            if (fields[2] == "session") {
                QCOMPARE(fields[streamFps], QByteArray("60"));
            }
            if (fields[2] == "released" && fields[vblankGrid] == "1") {
                QVERIFY(fields[vblankUs].toULongLong() != 0);
                onGrid++;
            }
            if (fields[2] == "vsync") {
                lastVsyncUs = std::max(lastVsyncUs, fields[3].toULongLong());
            }
        }
        QCOMPARE(counts["session"], 1);
        QCOMPARE(counts["scheduled"], frames);
        QCOMPARE(counts["released"], released.load());
        QCOMPARE(counts["superseded"] + counts["evicted"], dropped.load());
        QCOMPARE(counts["presented"], released.load());
        QVERIFY(onGrid > frames / 2);
        QVERIFY(counts["vsync"] > frames);
        QVERIFY(lastVsyncUs != 0);
    }

    // The decode delay, driven by renders that begin around it as the pacer
    // holds them, given a decode that takes decodeUs and short contention
    // waits. Returns the delay at each second.
    static std::vector<uint64_t> runDecodeDelay(TimestampPacer& pacer, uint64_t& nowUs, int seconds,
                                                uint64_t decodeUs, int64_t startOffsetUs)
    {
        // Held until the delay, renders begin at it or a little after, so
        // only the decay can lower an estimate that is too high
        static const int64_t k_Jitter[] = { 0, 50, 100, 200, 300 };
        std::vector<uint64_t> perSecond;
        for (int frame = 0; frame < seconds * 60; frame++) {
            nowUs += 16667;
            const int64_t start = std::max<int64_t>(0, int64_t(pacer.decodeDelayUs()) + startOffsetUs +
                                                           k_Jitter[frame % 5]);
            const uint64_t startedAfterUs = uint64_t(start);
            uint64_t waitUs = decodeUs > startedAfterUs ? decodeUs - startedAfterUs : 0;
            waitUs += frame % 7 == 0 ? 400 : 20;
            const uint64_t outputUs = nowUs - 10000;
            pacer.notePresented(outputUs + startedAfterUs, outputUs + startedAfterUs + 1000, outputUs, waitUs,
                                false, 0);
            if (frame % 60 == 59) {
                perSecond.push_back(pacer.decodeDelayUs());
            }
        }
        return perSecond;
    }

    // An estimate that ends up too high falls until renders begin just
    // before their decode finishes and settles there, and renders held for
    // other reasons don't lower it
    void decodeDelaySettles()
    {
        PacerTelemetry telemetry;
        TimestampPacingOptions options;
        options.enabled = true;
        TimestampPacer pacer(options, 60, 60, true, false, &telemetry, TimestampPacer::Callbacks());
        uint64_t nowUs = 1000000;

        // One slow stretch (8 ms decodes) raises it
        for (int frame = 0; frame < 60; frame++) {
            nowUs += 16667;
            const uint64_t outputUs = nowUs - 10000;
            pacer.notePresented(outputUs + 100, outputUs + 9000, outputUs, 8000, false, 0);
        }
        QVERIFY(pacer.decodeDelayUs() > 7500);

        // Renders begun well after it (FIFO back-pressure) leave it alone
        const uint64_t raisedUs = pacer.decodeDelayUs();
        runDecodeDelay(pacer, nowUs, 20, 4000, 4000);
        QCOMPARE(pacer.decodeDelayUs(), raisedUs);

        // Held by it with 4 ms decodes, it falls and settles where the
        // median render (begun 100 us after it) waits about 0.15 ms
        const std::vector<uint64_t> perSecond = runDecodeDelay(pacer, nowUs, 180, 4000, 0);
        const auto last = perSecond.end() - 60;
        const uint64_t lowUs = *std::min_element(last, perSecond.end());
        const uint64_t highUs = *std::max_element(last, perSecond.end());
        qInfo("decode delay over the last minute: %llu-%llu us", (unsigned long long)lowUs,
              (unsigned long long)highUs);
        QVERIFY(lowUs >= 3500);
        QVERIFY(highUs <= 3900);
        QVERIFY(highUs - lowUs <= 150);
    }

    // A real pacer, in real time, against a modelled Gamescope under the
    // frame limit: FIFO at 90 Hz, showing a frame at the first refresh at
    // least 4 ms after its present returned and after the frame before it,
    // and VAAPI decodes finishing 4.8 ms after decoder output (as on a
    // Deck). Host frames come 15.4-18.2 ms apart (55-65 FPS), so their phase
    // against the refresh keeps moving and some are always planned for a
    // V-blank their decode can't make. Those miss, but handing frames over
    // earlier couldn't help them, so the margin must stay down.
    void marginIgnoresDecodeBoundMisses()
    {
        constexpr uint64_t refreshUs = 11111;
        constexpr uint64_t latchUs = 4000;
        constexpr uint64_t decodeUs = 4800;
        constexpr uint64_t renderUs = 1500;
        constexpr int seconds = 12;
#ifdef Q_OS_WIN
        // Moonlight runs with SDL's 1 ms timer resolution; without it the
        // pacer's waits overshoot by up to a scheduler tick
        timeBeginPeriod(1);
        const auto restoreTimer = qScopeGuard([] { timeEndPeriod(1); });
#endif

        // Sleeps most of the way so the pacer's own thread isn't crowded out
        const auto spinUntil = [](uint64_t atUs) {
            while (LiGetMicroseconds() + 2000 < atUs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            while (LiGetMicroseconds() < atUs) {
                std::this_thread::yield();
            }
        };

        PacerTelemetry telemetry;
        TimestampPacer* pacer = nullptr;
        std::mutex fifoLock;
        std::deque<uint64_t> fifo;
        std::atomic_int presented { 0 };
        TimestampPacer::Callbacks callbacks;
        callbacks.release = [&](AVFrame* frame) {
            const uint64_t startUs = LiGetMicroseconds();
            const uint64_t outputUs = uint64_t(frame->pts);
            const uint64_t doneUs = outputUs + decodeUs;
            const uint64_t waitUs = doneUs > startUs ? doneUs - startUs : 20;
            const uint64_t presentUs = std::max(startUs, doneUs) + renderUs;
            av_frame_free(&frame);
            pacer->notePresented(startUs, presentUs, outputUs, waitUs, false, 0);
            std::lock_guard<std::mutex> lock(fifoLock);
            fifo.push_back(presentUs);
            presented++;
        };
        callbacks.drop = [](AVFrame* frame, bool) {
            av_frame_free(&frame);
        };

        TimestampPacingOptions options;
        options.enabled = true;
        options.vsyncMarginUs = TimestampPacingOptions::GamescopeVsyncMarginUs;
        TimestampPacer timestampPacer(options, 90, 90, true, false, &telemetry, callbacks);
        pacer = &timestampPacer;
        timestampPacer.setDisplayModeProbe([] {
            return TimestampPacer::CompositorState { TimestampPacer::DisplayMode::FrameLimited, 90 };
        });
        QVERIFY(timestampPacer.start());

        std::atomic_bool stopDisplay { false };
        std::atomic_int shown { 0 };
        std::thread display([&] {
            uint64_t vblankUs = LiGetMicroseconds();
            while (!stopDisplay.load()) {
                vblankUs += refreshUs;
                spinUntil(vblankUs);
                bool show = false;
                {
                    std::lock_guard<std::mutex> lock(fifoLock);
                    if (!fifo.empty() && fifo.front() + latchUs <= vblankUs) {
                        fifo.pop_front();
                        show = true;
                    }
                }
                if (show) {
                    timestampPacer.onDisplayEvent(vblankUs, refreshUs);
                    shown++;
                }
            }
        });

        std::mt19937 random(7);
        std::uniform_int_distribution<uint64_t> period(15385, 18182);
        const uint64_t startUs = LiGetMicroseconds();
        uint64_t sendUs = startUs;
        uint64_t maxMarginUs = 0;
        uint64_t nextCheckUs = startUs + 2000000;
        int frames = 0;
        while (sendUs < startUs + uint64_t(seconds) * 1000000) {
            sendUs += period(random);
            spinUntil(sendUs);
            // The decode is timed from when the frame actually came out
            const uint64_t outputUs = LiGetMicroseconds();
            AVFrame* frame = av_frame_alloc();
            frame->pts = int64_t(outputUs);
            PacedFrame paced(frame, ++frames, uint32_t((sendUs - startUs) * 9 / 100), true, outputUs);
            paced.setHostLatencyUs(3000);
            timestampPacer.submit(std::move(paced));
            if (sendUs >= nextCheckUs) {
                // After the decode delay is learned
                maxMarginUs = std::max(maxMarginUs, timestampPacer.extraMarginUs());
                nextCheckUs += 250000;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        timestampPacer.stop();
        stopDisplay.store(true);
        display.join();

        qInfo("%d frames, %d presented, %d shown, extra margin at most %llu us, decode delay %llu us", frames,
              presented.load(), shown.load(), (unsigned long long)maxMarginUs,
              (unsigned long long)timestampPacer.decodeDelayUs());
        QVERIFY(presented.load() >= frames * 97 / 100);
        QVERIFY(maxMarginUs <= 1000);
    }

    void refusesUnwritableDestination()
    {
        QTemporaryDir directory;
        qputenv("MOONLIGHT_VRR_TRACE", directory.filePath("missing/Moonlight.vrrtrace").toUtf8());
        QVERIFY(TimestampTrace::openIfRequested() == nullptr);
    }
};

QTEST_GUILESS_MAIN(TimestampTraceTest)
#include "tst_timestamptrace.moc"
