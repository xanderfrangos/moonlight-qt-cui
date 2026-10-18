#include "tsreplay.h"

#include "../../app/streaming/video/ffmpeg-renderers/pacer/pacertelemetry.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/timestamppacer.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include <chrono>
#include <random>
#include <thread>

extern "C" uint64_t LiGetMicroseconds()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

namespace {

constexpr int k_Frames = 150;

// Runs a real pacer with tracing on over a stream that exercises the
// policy's branches: arrival jitter, host timestamp noise, a run of
// unreported host latency, host repeats once latency is trusted, a host
// clock step, an invalid timestamp and a gap in frame numbers. Returns the
// trace path.
QString recordPacerTrace(const QTemporaryDir& directory)
{
    qputenv("MOONLIGHT_VRR_TRACE", directory.filePath("Moonlight.vrrtrace").toUtf8());

    constexpr uint64_t periodUs = 16667;
    PacerTelemetry telemetry;
    TimestampPacer* pacer = nullptr;
    TimestampPacer::Callbacks callbacks;
    callbacks.release = [&](AVFrame* frame) {
        const uint64_t nowUs = LiGetMicroseconds();
        pacer->notePresented(nowUs, nowUs + 500, 0, 0, true, uint32_t(frame->pts));
        av_frame_free(&frame);
    };
    callbacks.drop = [](AVFrame* frame, bool) { av_frame_free(&frame); };

    // Not the defaults, so the replay has to take them from the trace
    TimestampPacingOptions options;
    options.enabled = true;
    options.smoothing = TimestampPacingOptions::SmoothingStandard;
    options.targetPerMille = 950;
    options.minBufferMs = 1;
    options.maxBufferMs = 12;
    {
        TimestampPacer timestampPacer(options, 60, 60, false, false, &telemetry, callbacks);
        pacer = &timestampPacer;
        if (!timestampPacer.start()) {
            return QString();
        }
        // Frames leave the decoder on a planned schedule, so the stream's
        // cadence doesn't depend on how long this thread's sleeps take
        const uint64_t startUs = LiGetMicroseconds();
        std::mt19937 random(7);
        std::uniform_int_distribution<int> arrival(0, 4000);
        std::uniform_int_distribution<int> stamp(-900, 900);
        int32_t frameNumber = 0;
        for (int i = 0; i < k_Frames; i++) {
            frameNumber += i == 90 ? 2 : 1;
            AVFrame* frame = av_frame_alloc();
            frame->pts = i;
            // The host clock steps back a second at frame 60, which starts
            // a new timeline
            const uint32_t rtp = uint32_t(int64_t(frameNumber) * 1500 + stamp(random) * 9 / 100 -
                                          (i >= 60 ? 90000 : 0));
            const uint64_t dueUs = startUs + uint64_t(i) * periodUs;
            while (LiGetMicroseconds() < dueUs) {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
            PacedFrame paced(frame, frameNumber, rtp, i != 20, dueUs + uint64_t(arrival(random)));
            // Unreported latency first, then reported, then zeros the policy
            // recognizes as repeats
            paced.setHostLatencyUs(i < 10 ? 0 : (i >= 120 && i < 125) ? 0 : 3000);
            timestampPacer.submit(std::move(paced));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        timestampPacer.stop();
    }
    qunsetenv("MOONLIGHT_VRR_TRACE");
    return directory.filePath("Moonlight.tstrace");
}

TsReplay::Row frameRow(int32_t frame, uint32_t rtp, uint64_t readyUs)
{
    TsReplay::Row row;
    row.event = "scheduled";
    row.sequence = uint64_t(frame) + 1;
    row.frame = frame;
    row.rtpValid = true;
    row.rtp = rtp;
    row.hostLatencyUs = 3000;
    row.readyUs = readyUs;
    row.renderLeadUs = 1000;
    return row;
}

// A steady 60 FPS stream ready a constant 3 ms after its source time
TsReplay::Trace steadyTrace(int frames)
{
    TsReplay::Trace trace;
    trace.haveSession = true;
    trace.session.streamFps = 60;
    trace.session.displayHz = 60;
    trace.session.smoothing = TimestampPacingOptions::SmoothingLight;
    trace.session.targetPerMille = 990;
    trace.session.minBufferMs = 2;
    trace.session.maxBufferMs = 16;
    trace.session.vblankGridAvailable = true;
    for (int i = 0; i < frames; i++) {
        trace.scheduled.push_back(frameRow(i + 1, uint32_t(i) * 1500,
                                           10000000 + uint64_t(std::llround(i * 1000000.0 / 60)) + 3000));
    }
    return trace;
}

TsReplay::Settings fixedSettings(const TsReplay::Trace& trace)
{
    TsReplay::Settings settings = TsReplay::sessionSettings(trace);
    settings.renderUs = 1000;
    return settings;
}

}

class TsReplayTest : public QObject
{
    Q_OBJECT

private slots:
    void cleanup()
    {
        qunsetenv("MOONLIGHT_VRR_TRACE");
    }

    // The unchanged policy reproduces every decision the real pacer traced
    void realPacerTraceReplaysExactly()
    {
        QTemporaryDir directory;
        const QString path = recordPacerTrace(directory);
        QVERIFY(!path.isEmpty());

        TsReplay::Trace trace;
        QString error;
        QVERIFY2(TsReplay::load(path, trace, error), qPrintable(error));
        QVERIFY(trace.compressed);
        QVERIFY(trace.hashMatches());
        QVERIFY(trace.sequenceComplete);
        QVERIFY(trace.integrityValid());
        QCOMPARE(int(trace.scheduled.size()), k_Frames);

        const TsReplay::Settings session = TsReplay::sessionSettings(trace);
        QCOMPARE(session.options.smoothing, int(TimestampPacingOptions::SmoothingStandard));
        QCOMPARE(session.options.targetPerMille, 950);
        QCOMPARE(session.options.minBufferMs, 1);
        QCOMPARE(session.options.maxBufferMs, 12);
        QCOMPARE(session.streamFps, 60);
        QVERIFY(!session.fixedRefresh);

        const TsReplay::Replayed replayed = TsReplay::replayPolicy(trace.scheduled, session.options, session.streamFps);
        const TsReplay::Baseline baseline = TsReplay::compareBaseline(trace, replayed);
        QVERIFY2(baseline.exact(), baseline.haveFirst ? qPrintable(QStringLiteral("%1 at sequence %2: %3 vs %4")
                                                                     .arg(baseline.first.field)
                                                                     .arg(baseline.first.sequence)
                                                                     .arg(baseline.first.recorded)
                                                                     .arg(baseline.first.replayed))
                                                      : "no rows compared");
        QCOMPARE(baseline.compared, uint64_t(k_Frames));

        // The stream reached the branches it was built for
        int repeats = 0, unpaced = 0, restarted = 0;
        for (const TimestampPacing::Policy::Decision& decision : replayed.decisions) {
            repeats += decision.repeat;
            unpaced += !decision.paced;
            restarted += decision.restarted;
        }
        QCOMPARE(repeats, 5);
        QVERIFY(unpaced >= 6);
        QVERIFY(restarted >= 2);

        // A different policy setting must not pass as the recorded one
        TimestampPacingOptions other = session.options;
        other.smoothing = TimestampPacingOptions::SmoothingOff;
        QVERIFY(!TsReplay::compareBaseline(trace, TsReplay::replayPolicy(trace.scheduled, other, 60)).exact());
    }

    // An edited decision is found, and the edit breaks the trace's hash
    void tamperedDecisionIsReported()
    {
        QTemporaryDir directory;
        const QString path = recordPacerTrace(directory);
        QVERIFY(!path.isEmpty());
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray text;
        bool compressed = false;
        QString error;
        QVERIFY(TsReplay::Detail::expand(file.readAll(), text, compressed, error));

        QList<QByteArray> lines = text.split('\n');
        const QList<QByteArray> names = lines.first().split(',');
        const qsizetype buffer = names.indexOf("buffer_us");
        const qsizetype sequence = names.indexOf("sequence");
        QVERIFY(buffer > 0);
        int scheduled = 0;
        QByteArray editedSequence;
        for (QByteArray& line : lines) {
            QList<QByteArray> fields = line.split(',');
            if (fields.size() == names.size() && fields[2] == "scheduled" && ++scheduled == 50) {
                fields[buffer] = QByteArray::number(fields[buffer].toULongLong() + 1);
                editedSequence = fields[sequence];
                line = fields.join(',');
            }
        }
        const QString editedPath = directory.filePath("edited.tstrace.csv");
        QFile edited(editedPath);
        QVERIFY(edited.open(QIODevice::WriteOnly));
        edited.write(lines.join('\n'));
        edited.close();

        TsReplay::Trace trace;
        QVERIFY2(TsReplay::load(editedPath, trace, error), qPrintable(error));
        QVERIFY(!trace.compressed);
        QVERIFY(!trace.hashMatches());
        QVERIFY(!trace.integrityValid());
        const TsReplay::Settings session = TsReplay::sessionSettings(trace);
        const TsReplay::Baseline baseline =
            TsReplay::compareBaseline(trace, TsReplay::replayPolicy(trace.scheduled, session.options, 60));
        QVERIFY(!baseline.exact());
        QCOMPARE(baseline.mismatchedRows, uint64_t(1));
        QVERIFY(baseline.haveFirst);
        QCOMPARE(baseline.first.field, QStringLiteral("buffer_us"));
        QCOMPARE(baseline.first.sequence, editedSequence.toULongLong());
        QCOMPARE(baseline.first.replayed + 1, baseline.first.recorded);
    }

    // A steady stream on a matching grid is shown every refresh, on time,
    // with nothing to score as uneven, at every phase
    void steadyStreamIsEvenOnTheGrid()
    {
        const TsReplay::Trace trace = steadyTrace(240);
        const std::vector<bool> repeats = TsReplay::hostRepeats(trace.scheduled);
        TsReplay::ScenarioResult result = TsReplay::runScenario(trace, repeats, fixedSettings(trace));
        TsReplay::Metrics& m = result.metrics;
        QCOMPARE(m.frames, uint64_t(240 * 8));
        QCOMPARE(m.late, uint64_t(0));
        QCOMPARE(m.missedPlan, uint64_t(0));
        QCOMPARE(m.skipped, uint64_t(0));
        QCOMPARE(m.jerkOver2ms, uint64_t(0));
        QCOMPARE(m.intervalsOver2ms, uint64_t(0));
        QVERIFY(m.jerkUs.max() <= 2);
        QVERIFY(m.residualUs.max() <= 2);
        // The minimum buffer, then up to a refresh waiting for the V-blank
        QVERIFY(m.delayUs.mean() >= 2000);
        QVERIFY(m.delayUs.max() <= 2000 + 16667 + 600);
    }

    // One frame ready far too late misses its V-blank: the policy's plan is
    // late, and the presented cadence shows it
    void lateFrameIsScored()
    {
        TsReplay::Trace trace = steadyTrace(240);
        // Still ready before the next frame, as decoder output always is
        trace.scheduled[150].readyUs += 14000;
        const std::vector<bool> repeats = TsReplay::hostRepeats(trace.scheduled);
        TsReplay::Settings settings = fixedSettings(trace);
        settings.phases = 1;
        TsReplay::ScenarioResult result = TsReplay::runScenario(trace, repeats, settings);
        QVERIFY(result.outcomes[150].late);
        QVERIFY(result.outcomes[150].missedPlan);
        QVERIFY(result.metrics.late >= 1);
        QVERIFY(result.metrics.skipped >= 1);
        // The skip spans two short host steps, so it is scored, not counted
        // as a long source interval
        QVERIFY(result.metrics.jerkOver2ms >= 1);
        QCOMPARE(result.metrics.longSourceIntervals, uint64_t(0));
    }

    // VRR shows a frame no sooner than one refresh after the previous one
    void vrrRespectsTheRefreshFloor()
    {
        TsReplay::Trace trace = steadyTrace(2);
        trace.scheduled[1].readyUs = trace.scheduled[0].readyUs + 2000;
        TsReplay::Settings settings = TsReplay::sessionSettings(trace);
        settings.pacing = false;
        settings.fixedRefresh = false;
        settings.displayHz = 120;
        settings.renderUs = 0;
        const TsReplay::ScenarioResult result =
            TsReplay::runScenario(trace, TsReplay::hostRepeats(trace.scheduled), settings);
        QCOMPARE(result.outcomes[0].shownUs, trace.scheduled[0].readyUs);
        QCOMPARE(result.outcomes[1].shownUs, trace.scheduled[0].readyUs + 8333);
    }

    // A long source interval is counted, not scored as unevenness
    void longSourceIntervalsAreCountedSeparately()
    {
        TsReplay::Trace trace = steadyTrace(120);
        for (size_t i = 60; i < trace.scheduled.size(); i++) {
            trace.scheduled[i].rtp += 4500;
            trace.scheduled[i].readyUs += 50000;
        }
        TsReplay::Settings settings = fixedSettings(trace);
        settings.phases = 1;
        settings.pacing = false;
        TsReplay::ScenarioResult result =
            TsReplay::runScenario(trace, TsReplay::hostRepeats(trace.scheduled), settings);
        QCOMPARE(result.metrics.longSourceIntervals, uint64_t(1));
        QCOMPARE(result.metrics.jerkOver2ms, uint64_t(0));
        QCOMPARE(result.metrics.intervals, uint64_t(118));
    }

    void configIsValidated()
    {
        const TsReplay::Trace trace = steadyTrace(2);
        const TsReplay::Settings session = TsReplay::sessionSettings(trace);
        const auto parse = [&](const char* json, std::vector<TsReplay::Settings>& scenarios, QString& error) {
            return TsReplay::parseConfig(QJsonDocument::fromJson(json).object(), session, scenarios, error);
        };

        std::vector<TsReplay::Settings> scenarios;
        QString error;
        QVERIFY2(parse(R"({"version": 1, "scenarios": [
            {"name": "standard-95", "smoothing": "standard", "target_per_mille": 950},
            {"name": "vrr-off", "pacing": "off", "display": "vrr", "display_hz": 144, "render_us": 0}]})",
                       scenarios, error), qPrintable(error));
        QCOMPARE(int(scenarios.size()), 2);
        QCOMPARE(scenarios[0].options.smoothing, int(TimestampPacingOptions::SmoothingStandard));
        QCOMPARE(scenarios[0].options.targetPerMille, 950);
        QCOMPARE(scenarios[0].options.maxBufferMs, 16);
        QVERIFY(scenarios[0].fixedRefresh);
        QVERIFY(!scenarios[1].pacing);
        QVERIFY(!scenarios[1].fixedRefresh);
        QCOMPARE(scenarios[1].displayHz, 144.0);
        QCOMPARE(scenarios[1].renderUs, int64_t(0));

        scenarios.clear();
        QVERIFY(!parse(R"({"version": 1, "scenarios": [{"name": "x", "target_permille": 950}]})", scenarios, error));
        QVERIFY(error.contains(QStringLiteral("target_permille")));
        QVERIFY(!parse(R"({"version": 1, "scenarios": [{"name": "x", "target_per_mille": 1000}]})", scenarios, error));
        QVERIFY(!parse(R"({"version": 1, "scenarios": [{"name": "x", "min_buffer_ms": 8, "max_buffer_ms": 4}]})",
                       scenarios, error));
        QVERIFY(!parse(R"({"version": 1, "scenarios": [{"smoothing": "light"}]})", scenarios, error));
        QVERIFY(!parse(R"({"version": 2, "scenarios": [{"name": "x"}]})", scenarios, error));
    }
};

QTEST_GUILESS_MAIN(TsReplayTest)
#include "tst_tsreplay.moc"
