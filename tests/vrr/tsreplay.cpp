// tsreplay: replays a timestamp pacing trace (.tstrace) through
// TimestampPacing::Policy, checks that the unchanged policy reproduces every
// recorded decision, and scores scenarios on a simple presentation model.
// See tests/vrr/README.md, "Timestamp pacing replay".

#include "tsreplay.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <cstring>

namespace {

constexpr int ExitOk = 0;
constexpr int ExitError = 1;
constexpr int ExitNotExact = 2;

void printHelp()
{
    std::printf(
        "Usage: tsreplay <trace.tstrace> [options]\n"
        "\n"
        "Replays a timestamp pacing trace through the current TimestampPacing::Policy.\n"
        "The unchanged policy must reproduce every recorded decision exactly; then\n"
        "each scenario is scored on a presentation model (fixed V-blank grid or VRR).\n"
        "\n"
        "Options:\n"
        "  --require-exact-baseline  Exit 2 unless the trace is intact and the\n"
        "                            session's policy reproduces it exactly\n"
        "  --config FILE             Scenarios to score: {\"version\": 1, \"scenarios\":\n"
        "                            [{\"name\": ..., key: value, ...}]}\n"
        "  --set KEY=VALUE           Adds a \"candidate\" scenario: the session's\n"
        "                            settings with these changes (repeatable)\n"
        "  --output FILE             Writes the JSON summary\n"
        "  --compare FILE            Compares with an earlier summary of the same trace\n"
        "  --timeline FILE           Writes per-frame CSV for every scenario (phase 0)\n"
        "  --help                    Shows this\n"
        "\n"
        "Scenario keys (each scenario starts from the session's settings):\n"
        "  pacing            timestamp | off\n"
        "  smoothing         off | light | standard\n"
        "  target_per_mille  900-999\n"
        "  min_buffer_ms     0-20\n"
        "  max_buffer_ms     1-50\n"
        "  display           session | fixed | vrr\n"
        "  display_hz        refresh rate of the model's display\n"
        "  render_us         render time added to readiness, or \"recorded\" for\n"
        "                    each frame's recorded render lead (default)\n"
        "  phases            fixed-refresh grid phases averaged (default 8)\n"
        "\n"
        "Every run scores \"session-policy\" (the capture's settings) and\n"
        "\"pacing-off\" (each frame shown once rendered).\n"
        "\n"
        "Exit codes: 0 success, 1 error, 2 exact baseline required but not met.\n");
}

bool readJson(const QString& path, QJsonObject& object, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("cannot open %1").arg(path);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("%1: %2").arg(path, parseError.errorString());
        return false;
    }
    object = document.object();
    return true;
}

// --set values are numbers when they parse as one
QJsonValue settingValue(const QString& text)
{
    bool ok = false;
    const double number = text.toDouble(&ok);
    return ok ? QJsonValue(number) : QJsonValue(text);
}

bool writeTimeline(const QString& path, const TsReplay::Trace& trace,
                   const std::vector<TsReplay::ScenarioResult>& results, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    file.write("scenario,sequence,frame,rtp_valid,rtp_timestamp,ready_us,paced,target_us,buffer_us,"
               "planned_us,shown_us,late,missed_plan,skipped,delay_us\n");
    for (const TsReplay::ScenarioResult& result : results) {
        const QByteArray name = result.settings.name.toUtf8();
        for (size_t i = 0; i < trace.scheduled.size(); i++) {
            const TsReplay::Row& row = trace.scheduled[i];
            const TsReplay::Outcome& outcome = result.outcomes[i];
            const bool paced = result.settings.pacing && result.replayed.decisions[i].paced;
            const uint64_t targetUs = paced ? TsReplay::heldTargetUs(result.replayed.decisions[i], row.readyUs) : 0;
            const uint64_t bufferUs = result.settings.pacing ? result.replayed.decisions[i].bufferUs : 0;
            QByteArray line = name;
            line += ',' + QByteArray::number(qulonglong(row.sequence));
            line += ',' + QByteArray::number(row.frame);
            line += row.rtpValid ? ",1" : ",0";
            line += ',' + QByteArray::number(row.rtp);
            line += ',' + QByteArray::number(qulonglong(row.readyUs));
            line += paced ? ",1" : ",0";
            line += ',' + QByteArray::number(qulonglong(targetUs));
            line += ',' + QByteArray::number(qulonglong(bufferUs));
            line += ',' + QByteArray::number(qulonglong(outcome.plannedUs));
            line += ',' + QByteArray::number(qulonglong(outcome.shownUs));
            line += outcome.late ? ",1" : ",0";
            line += outcome.missedPlan ? ",1" : ",0";
            line += outcome.skipped ? ",1" : ",0";
            line += ',' + QByteArray::number(outcome.skipped ? 0 : qlonglong(outcome.shownUs) - qlonglong(row.readyUs));
            line += '\n';
            file.write(line);
        }
    }
    return true;
}

double number(const QJsonObject& object, const char* path)
{
    double value = 0;
    TsReplay::metricValue(object, QString::fromLatin1(path), value);
    return value;
}

void printTable(const QJsonArray& scenarios)
{
    std::printf("\n%-24s %9s %9s %9s %9s %9s %9s %9s %9s\n", "scenario", "jerk>2ms", "jerk p99", "resid>2ms",
                "late", "missed", "skipped", "delay", "delay p95");
    std::printf("%-24s %9s %9s %9s %9s %9s %9s %9s %9s\n", "", "per mille", "ms", "per mille", "per mille",
                "per mille", "per mille", "mean ms", "ms");
    for (const QJsonValue& value : scenarios) {
        const QJsonObject scenario = value.toObject();
        std::printf("%-24s %9.2f %9.2f %9.2f %9.2f %9.2f %9.2f %9.2f %9.2f\n",
                    scenario.value(QStringLiteral("name")).toString().toUtf8().constData(),
                    number(scenario, "presented_jerk_over_2ms_per_mille"),
                    number(scenario, "presented_jerk_us/p99") / 1000.0,
                    number(scenario, "cadence_residual_over_2ms_per_mille"),
                    number(scenario, "late_per_mille"),
                    number(scenario, "missed_plan_per_mille"),
                    number(scenario, "skipped_per_mille"),
                    number(scenario, "delay_us/mean") / 1000.0,
                    number(scenario, "delay_us/p95") / 1000.0);
    }
}

}

int main(int argc, char** argv)
{
    QString tracePath, configPath, outputPath, comparePath, timelinePath;
    QStringList sets;
    bool requireExact = false;
    for (int i = 1; i < argc; i++) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        const auto next = [&](QString& out) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "tsreplay: %s needs a value\n", argv[i]);
                return false;
            }
            out = QString::fromLocal8Bit(argv[++i]);
            return true;
        };
        if (arg == QLatin1String("--help") || arg == QLatin1String("-h")) {
            printHelp();
            return ExitOk;
        }
        if (arg == QLatin1String("--require-exact-baseline")) {
            requireExact = true;
        }
        else if (arg == QLatin1String("--config")) {
            if (!next(configPath)) return ExitError;
        }
        else if (arg == QLatin1String("--output")) {
            if (!next(outputPath)) return ExitError;
        }
        else if (arg == QLatin1String("--compare")) {
            if (!next(comparePath)) return ExitError;
        }
        else if (arg == QLatin1String("--timeline")) {
            if (!next(timelinePath)) return ExitError;
        }
        else if (arg == QLatin1String("--set")) {
            QString value;
            if (!next(value)) return ExitError;
            sets.push_back(value);
        }
        else if (arg.startsWith(QLatin1String("--"))) {
            std::fprintf(stderr, "tsreplay: unknown option %s (see --help)\n", argv[i]);
            return ExitError;
        }
        else if (tracePath.isEmpty()) {
            tracePath = arg;
        }
        else {
            std::fprintf(stderr, "tsreplay: more than one trace given\n");
            return ExitError;
        }
    }
    if (tracePath.isEmpty()) {
        printHelp();
        return ExitError;
    }

    QString error;
    TsReplay::Trace trace;
    if (!TsReplay::load(tracePath, trace, error)) {
        std::fprintf(stderr, "tsreplay: %s\n", error.toUtf8().constData());
        return ExitError;
    }

    const TsReplay::Settings session = TsReplay::sessionSettings(trace);
    std::vector<TsReplay::Settings> scenarios { session };
    TsReplay::Settings off = session;
    off.name = QStringLiteral("pacing-off");
    off.pacing = false;
    scenarios.push_back(off);
    if (!configPath.isEmpty()) {
        QJsonObject config;
        if (!readJson(configPath, config, error) ||
                !TsReplay::parseConfig(config, session, scenarios, error)) {
            std::fprintf(stderr, "tsreplay: %s\n", error.toUtf8().constData());
            return ExitError;
        }
    }
    if (!sets.isEmpty()) {
        TsReplay::Settings candidate = session;
        candidate.name = QStringLiteral("candidate");
        for (const QString& set : sets) {
            const qsizetype equals = set.indexOf(QLatin1Char('='));
            if (equals <= 0 ||
                    !TsReplay::applySetting(candidate, session, set.left(equals),
                                            settingValue(set.mid(equals + 1)), error)) {
                std::fprintf(stderr, "tsreplay: --set %s: %s\n", set.toUtf8().constData(),
                             equals <= 0 ? "expected KEY=VALUE" : error.toUtf8().constData());
                return ExitError;
            }
        }
        if (candidate.options.maxBufferMs < candidate.options.minBufferMs) {
            std::fprintf(stderr, "tsreplay: --set: max_buffer_ms is below min_buffer_ms\n");
            return ExitError;
        }
        candidate.options = candidate.options.resolved();
        scenarios.push_back(candidate);
    }
    QSet<QString> names;
    for (const TsReplay::Settings& settings : scenarios) {
        if (names.contains(settings.name)) {
            std::fprintf(stderr, "tsreplay: scenario name \"%s\" is used twice\n", settings.name.toUtf8().constData());
            return ExitError;
        }
        names.insert(settings.name);
    }

    const std::vector<bool> repeats = TsReplay::hostRepeats(trace.scheduled);
    std::vector<TsReplay::ScenarioResult> results;
    results.reserve(scenarios.size());
    for (const TsReplay::Settings& settings : scenarios) {
        results.push_back(TsReplay::runScenario(trace, repeats, settings));
    }
    // The session-policy scenario replays the capture's own settings, so its
    // decisions are the ones the exact baseline checks
    const TsReplay::Baseline baseline = TsReplay::compareBaseline(trace, results.front().replayed);
    const bool exact = trace.integrityValid() && baseline.exact();

    QJsonObject summary;
    summary[QStringLiteral("tool")] = QStringLiteral("tsreplay");
    summary[QStringLiteral("summary_version")] = 1;
    summary[QStringLiteral("capture")] = TsReplay::captureJson(trace);
    summary[QStringLiteral("session")] = TsReplay::sessionJson(trace);
    QJsonObject fidelity = TsReplay::baselineJson(baseline);
    fidelity[QStringLiteral("strict_ab_valid")] = exact;
    summary[QStringLiteral("fidelity")] = fidelity;
    summary[QStringLiteral("source")] = TsReplay::sourceJson(trace, repeats);
    summary[QStringLiteral("recorded")] = TsReplay::recordedJson(trace, repeats);
    QJsonObject model;
    model[QStringLiteral("long_source_interval_us")] = TsReplay::LongSourceIntervalUs;
    model[QStringLiteral("hitch_us")] = TsReplay::HitchUs;
    model[QStringLiteral("note")] = QStringLiteral(
        "Policy replay is exact; presentation is a model without the pacer's release thread, "
        "learned margins, late shift, frame limits, GPU or compositor. Long source intervals are "
        "excluded from cadence scores by design, not classified as stalls.");
    summary[QStringLiteral("model")] = model;

    QJsonArray scenarioArray;
    for (TsReplay::ScenarioResult& result : results) {
        QJsonObject scenario;
        scenario[QStringLiteral("name")] = result.settings.name;
        scenario[QStringLiteral("settings")] = TsReplay::settingsJson(result.settings);
        scenario[QStringLiteral("metrics")] = TsReplay::metricsJson(result.metrics);
        scenarioArray.append(scenario);
    }
    summary[QStringLiteral("scenarios")] = scenarioArray;

    std::printf("Trace: %s (%lld bytes, sha256 %s)\n", tracePath.toUtf8().constData(),
                (long long)trace.bytes, trace.fileSha256.constData());
    std::printf("Session: %d FPS on %d Hz, %s, smoothing %s, %d per mille, buffer %d-%d ms\n",
                trace.session.streamFps, trace.session.displayHz,
                session.fixedRefresh ? "V-blank grid" : "no V-blank grid",
                TsReplay::smoothingName(trace.session.smoothing).toUtf8().constData(),
                trace.session.targetPerMille, trace.session.minBufferMs, trace.session.maxBufferMs);
    std::printf("Integrity: %s (footer %s, clean shutdown %s, dropped %llu, hash %s, sequence %s)\n",
                trace.integrityValid() ? "valid" : "NOT VALID",
                trace.footerPresent ? "present" : "missing", trace.cleanShutdown ? "yes" : "no",
                (unsigned long long)trace.rowsDropped, trace.hashMatches() ? "matches" : "differs",
                trace.sequenceComplete ? "complete" : "incomplete");
    if (baseline.exact()) {
        std::printf("Baseline: exact, %llu scheduled rows reproduced\n", (unsigned long long)baseline.compared);
    }
    else {
        std::printf("Baseline: NOT EXACT, %llu of %llu scheduled rows differ", (unsigned long long)baseline.mismatchedRows,
                    (unsigned long long)baseline.compared);
        if (baseline.haveFirst) {
            std::printf("; first at sequence %llu, frame %d: %s recorded %lld, replayed %lld",
                        (unsigned long long)baseline.first.sequence, baseline.first.frame,
                        baseline.first.field.toUtf8().constData(), (long long)baseline.first.recorded,
                        (long long)baseline.first.replayed);
        }
        std::printf("\n          Results are exploratory: this policy is not the one the capture ran.\n");
    }
    printTable(scenarioArray);

    if (!comparePath.isEmpty()) {
        QJsonObject earlier;
        if (!readJson(comparePath, earlier, error)) {
            std::fprintf(stderr, "tsreplay: %s\n", error.toUtf8().constData());
            return ExitError;
        }
        const QString earlierSha = earlier.value(QStringLiteral("capture")).toObject()
                                       .value(QStringLiteral("sha256")).toString();
        if (earlierSha != QString::fromLatin1(trace.fileSha256)) {
            std::fprintf(stderr, "tsreplay: %s summarizes a different trace (sha256 %s)\n",
                         comparePath.toUtf8().constData(), earlierSha.toUtf8().constData());
            return ExitError;
        }
        QHash<QString, QJsonObject> earlierScenarios;
        for (const QJsonValue& value : earlier.value(QStringLiteral("scenarios")).toArray()) {
            earlierScenarios.insert(value.toObject().value(QStringLiteral("name")).toString(), value.toObject());
        }
        QJsonObject comparison;
        std::printf("\nCompared with %s\n", comparePath.toUtf8().constData());
        for (const QJsonValue& value : scenarioArray) {
            const QJsonObject now = value.toObject();
            const QString name = now.value(QStringLiteral("name")).toString();
            const auto before = earlierScenarios.constFind(name);
            if (before == earlierScenarios.constEnd()) {
                std::printf("  %s: not in the earlier summary; compare it with this run's session-policy\n",
                            name.toUtf8().constData());
                continue;
            }
            if (before->value(QStringLiteral("settings")) != now.value(QStringLiteral("settings"))) {
                std::printf("  %s: settings differ from the earlier run; deltas still reported\n",
                            name.toUtf8().constData());
            }
            QJsonObject deltas;
            std::printf("  %s\n", name.toUtf8().constData());
            for (const QString& metric : TsReplay::comparedMetrics()) {
                double a = 0, b = 0;
                if (!TsReplay::metricValue(*before, metric, a) || !TsReplay::metricValue(now, metric, b)) {
                    continue;
                }
                QJsonObject delta;
                delta[QStringLiteral("baseline")] = a;
                delta[QStringLiteral("candidate")] = b;
                delta[QStringLiteral("delta")] = b - a;
                deltas[metric] = delta;
                std::printf("    %-38s %12.2f -> %12.2f  (%+.2f)\n", metric.toUtf8().constData(), a, b, b - a);
            }
            comparison[name] = deltas;
        }
        QJsonObject compare;
        compare[QStringLiteral("baseline_path")] = comparePath;
        compare[QStringLiteral("baseline_strict_ab_valid")] =
            earlier.value(QStringLiteral("fidelity")).toObject().value(QStringLiteral("strict_ab_valid"));
        compare[QStringLiteral("scenarios")] = comparison;
        summary[QStringLiteral("compare")] = compare;
    }

    if (!outputPath.isEmpty()) {
        QFile file(outputPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                file.write(QJsonDocument(summary).toJson(QJsonDocument::Indented)) < 0) {
            std::fprintf(stderr, "tsreplay: cannot write %s\n", outputPath.toUtf8().constData());
            return ExitError;
        }
    }
    if (!timelinePath.isEmpty() && !writeTimeline(timelinePath, trace, results, error)) {
        std::fprintf(stderr, "tsreplay: %s\n", error.toUtf8().constData());
        return ExitError;
    }


    if (requireExact && !exact) {
        std::fprintf(stderr, "tsreplay: exact baseline required but not met\n");
        return ExitNotExact;
    }
    return ExitOk;
}
