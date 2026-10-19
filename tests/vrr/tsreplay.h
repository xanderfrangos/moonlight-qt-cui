#pragma once

// Offline replay of timestamp pacing traces (.tstrace) through
// TimestampPacing::Policy. tsreplay.cpp is the command line around it; see
// tests/vrr/README.md, "Timestamp pacing replay".
//
// The policy is pure, and every scheduled row records each input it was
// given, so the unchanged policy must reproduce every recorded decision
// exactly. That gate is what makes a replayed A/B attributable to the change
// under test rather than to the replay.
//
// The presentation model after the policy is deliberately small. It does not
// run the pacer's release thread, its learned margins, late shift, Gamescope
// frame limits or the display-mode probe, and has no GPU or compositor. Its
// scores rank policies against each other on the same capture; they are not
// predictions of measured display timing.

#include "streaming/video/ffmpeg-renderers/pacer/timestamppacingpolicy.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

namespace TsReplay {

// timestamppacer.cpp's k_MaximumHoldUs: a target this far past readiness is
// clamped to it
constexpr uint64_t MaximumHoldUs = 100000;

// Source intervals longer than this are left out of cadence scoring, as
// vrrreplay's are. It is a metric exclusion, not a finding that the host
// stalled: an idle desktop, an inactive game or a lower frame rate produces
// such intervals legitimately. They are counted separately.
constexpr double LongSourceIntervalUs = 25000;

// Presented intervals or jerk beyond this count as visible unevenness
constexpr double HitchUs = 2000;

// Render time when a frame has no recorded render lead
constexpr uint64_t DefaultRenderUs = 1000;

struct Row {
    QByteArray event;
    uint64_t sequence = 0;
    uint64_t eventUs = 0;
    int32_t frame = -1;
    bool rtpValid = false;
    uint32_t rtp = 0;
    uint32_t hostLatencyUs = 0;
    uint64_t receiveUs = 0;
    uint64_t decoderQueueUs = 0;
    uint64_t readyUs = 0;
    bool paced = false;
    bool admitted = false;
    bool repeat = false;
    bool restarted = false;
    bool reanchored = false;
    bool late = false;
    uint64_t policyTargetUs = 0;
    uint64_t targetUs = 0;
    uint64_t bufferUs = 0;
    uint64_t latenessUs = 0;
    int64_t correctionUs = 0;
    int64_t hostJerkUs = 0;
    bool hostJerkValid = false;
    uint64_t sourcePeriodUs = 0;
    uint64_t renderLeadUs = 0;
    uint64_t releaseUs = 0;
    uint64_t displayUs = 0;
    bool matched = false;
    bool missed = false;
    uint32_t displayMode = 0;
    // Released
    bool vblankGrid = false;
    uint64_t vblankUs = 0;
    uint64_t releaseLeadUs = 0;
    uint64_t gridPeriodUs = 0;
    uint32_t superseded = 0;
    bool lateShift = false;
    int32_t streamFps = 0;
    int32_t displayHz = 0;
    int32_t smoothing = 0;
    int32_t targetPerMille = 0;
    int32_t minBufferMs = 0;
    int32_t maxBufferMs = 0;
    int32_t vsyncMarginUs = 0;
    bool vblankGridAvailable = false;
    bool compositorProbe = false;
};

struct Trace {
    QString path;
    qint64 bytes = 0;
    QByteArray fileSha256;
    bool compressed = false;

    bool footerPresent = false;
    bool cleanShutdown = false;
    uint64_t rowsAllocated = 0;
    uint64_t rowsEnqueued = 0;
    uint64_t rowsDropped = 0;
    bool sizeCapped = false;
    bool writeFailed = false;
    // The footer's hash of the decoded text, and the one computed here
    QByteArray recordedSha256;
    QByteArray decodedSha256;

    uint64_t rows = 0;
    // Every sequence number from 1 to rows_allocated appears exactly once
    bool sequenceComplete = false;
    bool haveSession = false;
    Row session;
    // In sequence order, which is the order the policy saw them
    std::vector<Row> scheduled;
    std::vector<Row> presented;
    std::vector<Row> displays;
    std::vector<Row> released;
    std::vector<Row> displayModes;

    bool hashMatches() const { return !recordedSha256.isEmpty() && recordedSha256 == decodedSha256; }

    bool integrityValid() const
    {
        return footerPresent && cleanShutdown && rowsDropped == 0 && !sizeCapped && !writeFailed &&
               hashMatches() && sequenceComplete && haveSession;
    }
};

namespace Detail {

// Expands the chunk-compressed container (tracefile.h). Plain CSV passes
// through unchanged.
inline bool expand(const QByteArray& bytes, QByteArray& text, bool& compressed, QString& error)
{
    static const QByteArray magic("MLVRR1\n");
    compressed = bytes.startsWith(magic);
    if (!compressed) {
        text = bytes;
        return true;
    }
    text.clear();
    qsizetype offset = magic.size();
    while (offset < bytes.size()) {
        if (offset + 4 > bytes.size()) {
            error = QStringLiteral("truncated chunk length at byte %1").arg(offset);
            return false;
        }
        const auto* length = reinterpret_cast<const unsigned char*>(bytes.constData() + offset);
        const qsizetype chunkBytes = qsizetype(length[0]) | qsizetype(length[1]) << 8 |
                                     qsizetype(length[2]) << 16 | qsizetype(length[3]) << 24;
        offset += 4;
        if (chunkBytes < 4 || offset + chunkBytes > bytes.size()) {
            error = QStringLiteral("truncated chunk at byte %1").arg(offset);
            return false;
        }
        const QByteArray chunk = qUncompress(bytes.mid(offset, chunkBytes));
        if (chunk.isEmpty()) {
            error = QStringLiteral("chunk at byte %1 does not decompress").arg(offset);
            return false;
        }
        text += chunk;
        offset += chunkBytes;
    }
    return true;
}

class Columns
{
public:
    explicit Columns(const QList<QByteArray>& names)
    {
        for (qsizetype i = 0; i < names.size(); i++) {
            m_Index.insert(names[i], int(i));
        }
    }

    bool has(const char* name) const { return m_Index.contains(QByteArray(name)); }

    // Missing columns, from traces written before they were appended, read
    // as zero
    QByteArray field(const QList<QByteArray>& fields, const char* name) const
    {
        const int index = m_Index.value(QByteArray(name), -1);
        return index >= 0 && index < fields.size() ? fields[index] : QByteArray();
    }

private:
    QHash<QByteArray, int> m_Index;
};

struct Parser {
    const Columns& columns;
    const QList<QByteArray>& fields;
    bool ok = true;

    uint64_t u(const char* name)
    {
        const QByteArray value = columns.field(fields, name);
        if (value.isEmpty()) {
            return 0;
        }
        bool valid = false;
        const qulonglong parsed = value.toULongLong(&valid);
        ok = ok && valid;
        return uint64_t(parsed);
    }

    int64_t s(const char* name)
    {
        const QByteArray value = columns.field(fields, name);
        if (value.isEmpty()) {
            return 0;
        }
        bool valid = false;
        const qlonglong parsed = value.toLongLong(&valid);
        ok = ok && valid;
        return int64_t(parsed);
    }

    bool b(const char* name) { return u(name) != 0; }
};

inline void parseFooter(const QByteArray& line, Trace& trace)
{
    trace.footerPresent = true;
    for (const QByteArray& part : line.split(',')) {
        const qsizetype equals = part.indexOf('=');
        if (equals < 0) {
            continue;
        }
        const QByteArray key = part.left(equals);
        const QByteArray value = part.mid(equals + 1);
        if (key == "clean_shutdown") {
            trace.cleanShutdown = value == "1";
        }
        else if (key == "rows_allocated") {
            trace.rowsAllocated = value.toULongLong();
        }
        else if (key == "rows_enqueued") {
            trace.rowsEnqueued = value.toULongLong();
        }
        else if (key == "rows_dropped") {
            trace.rowsDropped = value.toULongLong();
        }
        else if (key == "size_capped") {
            trace.sizeCapped = value == "1";
        }
        else if (key == "write_failed") {
            trace.writeFailed = value == "1";
        }
        else if (key == "decoded_sha256") {
            trace.recordedSha256 = value.trimmed();
        }
    }
}

}

// Reads a .tstrace, compressed or CSV. Returns false with error set when the
// file can't be read or parsed; integrity problems the file can still be
// replayed with are reported through Trace instead.
inline bool load(const QString& path, Trace& trace, QString& error)
{
    trace = Trace();
    trace.path = path;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("cannot open %1").arg(path);
        return false;
    }
    const QByteArray bytes = file.readAll();
    trace.bytes = bytes.size();
    trace.fileSha256 = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();

    QByteArray text;
    if (!Detail::expand(bytes, text, trace.compressed, error)) {
        return false;
    }

    const qsizetype headerEnd = text.indexOf('\n');
    if (headerEnd < 0 || !text.startsWith("timestamp_trace_schema,")) {
        error = QStringLiteral("not a timestamp pacing trace (no timestamp_trace_schema header)");
        return false;
    }
    const Detail::Columns columns(text.left(headerEnd).trimmed().split(','));
    for (const char* required : { "sequence", "event", "frame", "rtp_valid", "rtp_timestamp", "host_latency_us",
                                  "decoder_queue_us", "ready_us", "policy_target_us", "buffer_us",
                                  "lateness_us", "stream_fps", "smoothing", "target_per_mille" }) {
        if (!columns.has(required)) {
            error = QStringLiteral("trace has no %1 column").arg(required);
            return false;
        }
    }

    // The footer's hash covers everything before it
    const qsizetype footerStart = text.indexOf("\n#timestamp_trace_footer");
    const qsizetype rowsEnd = footerStart >= 0 ? footerStart + 1 : text.size();
    trace.decodedSha256 = QCryptographicHash::hash(QByteArray::fromRawData(text.constData(), rowsEnd),
                                                   QCryptographicHash::Sha256).toHex();
    if (footerStart >= 0) {
        Detail::parseFooter(text.mid(footerStart + 1).trimmed(), trace);
    }

    std::vector<uint64_t> sequences;
    qsizetype start = headerEnd + 1;
    int lineNumber = 1;
    while (start < rowsEnd) {
        qsizetype end = text.indexOf('\n', start);
        if (end < 0 || end > rowsEnd) {
            end = rowsEnd;
        }
        lineNumber++;
        const QByteArray line = text.mid(start, end - start).trimmed();
        start = end + 1;
        if (line.isEmpty()) {
            continue;
        }
        const QList<QByteArray> fields = line.split(',');
        if (fields.first() != "1") {
            error = QStringLiteral("line %1: unsupported timestamp trace schema %2")
                        .arg(lineNumber).arg(QString::fromLatin1(fields.first()));
            return false;
        }

        Detail::Parser p { columns, fields };
        Row row;
        row.event = columns.field(fields, "event");
        row.sequence = p.u("sequence");
        row.eventUs = p.u("event_us");
        row.frame = int32_t(p.s("frame"));
        row.rtpValid = p.b("rtp_valid");
        row.rtp = uint32_t(p.u("rtp_timestamp"));
        row.hostLatencyUs = uint32_t(p.u("host_latency_us"));
        row.receiveUs = p.u("frame_receive_us");
        row.decoderQueueUs = p.u("decoder_queue_us");
        row.readyUs = p.u("ready_us");
        row.paced = p.b("paced");
        row.admitted = p.b("admitted");
        row.repeat = p.b("repeat");
        row.restarted = p.b("restarted");
        row.reanchored = p.b("reanchored");
        row.late = p.b("late");
        row.policyTargetUs = p.u("policy_target_us");
        row.targetUs = p.u("target_us");
        row.bufferUs = p.u("buffer_us");
        row.latenessUs = p.u("lateness_us");
        row.correctionUs = p.s("correction_us");
        row.hostJerkUs = p.s("host_jerk_us");
        row.hostJerkValid = p.b("host_jerk_valid");
        row.sourcePeriodUs = p.u("source_period_us");
        row.renderLeadUs = p.u("render_lead_us");
        row.releaseUs = p.u("release_us");
        row.displayUs = p.u("display_us");
        row.matched = p.b("matched");
        row.missed = p.b("missed");
        row.displayMode = uint32_t(p.u("display_mode"));
        row.streamFps = int32_t(p.s("stream_fps"));
        row.displayHz = int32_t(p.s("display_hz"));
        row.smoothing = int32_t(p.s("smoothing"));
        row.targetPerMille = int32_t(p.s("target_per_mille"));
        row.minBufferMs = int32_t(p.s("min_buffer_ms"));
        row.maxBufferMs = int32_t(p.s("max_buffer_ms"));
        row.vsyncMarginUs = int32_t(p.s("vsync_margin_us"));
        row.vblankGridAvailable = p.b("vblank_grid_available");
        row.compositorProbe = p.b("compositor_probe");
        row.vblankGrid = p.b("vblank_grid");
        row.vblankUs = p.u("vblank_us");
        row.releaseLeadUs = p.u("release_lead_us");
        row.gridPeriodUs = p.u("grid_period_us");
        row.superseded = uint32_t(p.u("superseded"));
        row.lateShift = p.b("late_shift");
        if (!p.ok) {
            error = QStringLiteral("line %1: malformed number").arg(lineNumber);
            return false;
        }

        trace.rows++;
        sequences.push_back(row.sequence);
        if (row.event == "session") {
            if (trace.haveSession) {
                error = QStringLiteral("line %1: a second session row").arg(lineNumber);
                return false;
            }
            trace.haveSession = true;
            trace.session = row;
        }
        else if (row.event == "scheduled") {
            trace.scheduled.push_back(row);
        }
        else if (row.event == "presented") {
            trace.presented.push_back(row);
        }
        else if (row.event == "display_event" || row.event == "frame_displayed") {
            trace.displays.push_back(row);
        }
        else if (row.event == "released") {
            trace.released.push_back(row);
        }
        else if (row.event == "display_mode") {
            trace.displayModes.push_back(row);
        }
    }

    // Rows from different threads can reach the file out of order. Scheduled
    // rows all come from the decoder thread, after the policy decided, so
    // their sequence is the policy's order.
    std::sort(sequences.begin(), sequences.end());
    trace.sequenceComplete = trace.footerPresent && !sequences.empty() && sequences.size() == trace.rowsAllocated;
    for (size_t i = 0; trace.sequenceComplete && i < sequences.size(); i++) {
        trace.sequenceComplete = sequences[i] == i + 1;
    }
    const auto bySequence = [](const Row& a, const Row& b) { return a.sequence < b.sequence; };
    std::sort(trace.scheduled.begin(), trace.scheduled.end(), bySequence);
    std::sort(trace.presented.begin(), trace.presented.end(), bySequence);
    std::sort(trace.displays.begin(), trace.displays.end(), bySequence);
    std::sort(trace.released.begin(), trace.released.end(), bySequence);
    std::sort(trace.displayModes.begin(), trace.displayModes.end(), bySequence);

    if (!trace.haveSession) {
        error = QStringLiteral("trace has no session row");
        return false;
    }
    return true;
}

// One way of pacing and showing a capture
struct Settings {
    QString name;
    // Off shows each frame as soon as it is ready
    bool pacing = true;
    TimestampPacingOptions options;
    int streamFps = 60;
    // A fixed V-blank grid at displayHz, or VRR no faster than displayHz
    bool fixedRefresh = true;
    double displayHz = 60;
    // Negative: each frame's recorded render lead
    int64_t renderUs = -1;
    // Fixed refresh only: grid phases averaged, since the real phase isn't
    // known to the model
    int phases = 8;
    // The render lead given to the policy; negative: each frame's recorded
    // one, as the pacer gives it
    int64_t bufferLeadUs = -1;
};

// The settings the capture ran with
inline Settings sessionSettings(const Trace& trace)
{
    Settings settings;
    settings.name = QStringLiteral("session-policy");
    settings.options.enabled = true;
    settings.options.smoothing = trace.session.smoothing;
    settings.options.targetPerMille = trace.session.targetPerMille;
    settings.options.minBufferMs = trace.session.minBufferMs;
    settings.options.maxBufferMs = trace.session.maxBufferMs;
    if (trace.session.vsyncMarginUs != 0) {
        settings.options.vsyncMarginUs = trace.session.vsyncMarginUs;
    }
    settings.options = settings.options.resolved();
    settings.streamFps = trace.session.streamFps;
    settings.displayHz = trace.session.displayHz;
    settings.fixedRefresh = trace.session.vblankGridAvailable && trace.session.displayHz > 0;
    return settings;
}

inline QString smoothingName(int smoothing)
{
    switch (smoothing) {
    case TimestampPacingOptions::SmoothingOff: return QStringLiteral("off");
    case TimestampPacingOptions::SmoothingLight: return QStringLiteral("light");
    case TimestampPacingOptions::SmoothingStandard: return QStringLiteral("standard");
    }
    return QString::number(smoothing);
}

// Applies one scenario key. Config files and --set share these keys.
inline bool applySetting(Settings& settings, const Settings& session, const QString& key,
                         const QJsonValue& value, QString& error)
{
    const auto integer = [&](int minimum, int maximum, int& out) {
        double number = 0;
        if (value.isDouble()) {
            number = value.toDouble();
        }
        else if (value.isString()) {
            bool ok = false;
            number = value.toString().toDouble(&ok);
            if (!ok) {
                error = QStringLiteral("%1 must be a number").arg(key);
                return false;
            }
        }
        else {
            error = QStringLiteral("%1 must be a number").arg(key);
            return false;
        }
        if (number != std::floor(number) || number < minimum || number > maximum) {
            error = QStringLiteral("%1 must be an integer from %2 to %3").arg(key).arg(minimum).arg(maximum);
            return false;
        }
        out = int(number);
        return true;
    };
    const QString text = value.isString() ? value.toString() : QString();

    if (key == QLatin1String("pacing")) {
        if (text == QLatin1String("timestamp")) {
            settings.pacing = true;
        }
        else if (text == QLatin1String("off")) {
            settings.pacing = false;
        }
        else {
            error = QStringLiteral("pacing must be \"timestamp\" or \"off\"");
            return false;
        }
        return true;
    }
    if (key == QLatin1String("smoothing")) {
        if (value.isString() && text == QLatin1String("off")) {
            settings.options.smoothing = TimestampPacingOptions::SmoothingOff;
            return true;
        }
        if (value.isString() && text == QLatin1String("light")) {
            settings.options.smoothing = TimestampPacingOptions::SmoothingLight;
            return true;
        }
        if (value.isString() && text == QLatin1String("standard")) {
            settings.options.smoothing = TimestampPacingOptions::SmoothingStandard;
            return true;
        }
        return integer(TimestampPacingOptions::SmoothingOff, TimestampPacingOptions::SmoothingStandard,
                       settings.options.smoothing);
    }
    // The ranges are the ones TimestampPacingOptions::resolved() allows, so
    // a scenario never silently runs with a different value than it names
    if (key == QLatin1String("target_per_mille")) {
        return integer(900, 999, settings.options.targetPerMille);
    }
    if (key == QLatin1String("min_buffer_ms")) {
        return integer(0, 20, settings.options.minBufferMs);
    }
    if (key == QLatin1String("max_buffer_ms")) {
        return integer(1, 50, settings.options.maxBufferMs);
    }
    if (key == QLatin1String("display")) {
        if (text == QLatin1String("session")) {
            settings.fixedRefresh = session.fixedRefresh;
            settings.displayHz = session.displayHz;
        }
        else if (text == QLatin1String("fixed")) {
            settings.fixedRefresh = true;
        }
        else if (text == QLatin1String("vrr")) {
            settings.fixedRefresh = false;
        }
        else {
            error = QStringLiteral("display must be \"session\", \"fixed\" or \"vrr\"");
            return false;
        }
        return true;
    }
    if (key == QLatin1String("display_hz")) {
        int hz = 0;
        if (!integer(1, 1000, hz)) {
            return false;
        }
        settings.displayHz = hz;
        return true;
    }
    if (key == QLatin1String("render_us")) {
        if (value.isString() && text == QLatin1String("recorded")) {
            settings.renderUs = -1;
            return true;
        }
        int renderUs = 0;
        if (!integer(0, 100000, renderUs)) {
            return false;
        }
        settings.renderUs = renderUs;
        return true;
    }
    if (key == QLatin1String("buffer_lead_us")) {
        if (value.isString() && text == QLatin1String("recorded")) {
            settings.bufferLeadUs = -1;
            return true;
        }
        int leadUs = 0;
        if (!integer(0, 20000, leadUs)) {
            return false;
        }
        settings.bufferLeadUs = leadUs;
        return true;
    }
    if (key == QLatin1String("phases")) {
        return integer(1, 64, settings.phases);
    }
    error = QStringLiteral("unknown scenario key \"%1\"").arg(key);
    return false;
}

// A configuration file: {"version": 1, "scenarios": [{"name": ..., key: value, ...}]}.
// Each scenario starts from the session's settings.
inline bool parseConfig(const QJsonObject& root, const Settings& session, std::vector<Settings>& scenarios,
                        QString& error)
{
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (it.key() != QLatin1String("version") && it.key() != QLatin1String("scenarios") &&
                it.key() != QLatin1String("description")) {
            error = QStringLiteral("unknown config key \"%1\"").arg(it.key());
            return false;
        }
    }
    if (root.value(QStringLiteral("version")).toInt() != 1) {
        error = QStringLiteral("config version must be 1");
        return false;
    }
    const QJsonValue list = root.value(QStringLiteral("scenarios"));
    if (!list.isArray() || list.toArray().isEmpty()) {
        error = QStringLiteral("config needs a non-empty scenarios array");
        return false;
    }
    for (const QJsonValue& entry : list.toArray()) {
        if (!entry.isObject()) {
            error = QStringLiteral("each scenario must be an object");
            return false;
        }
        const QJsonObject object = entry.toObject();
        Settings settings = session;
        settings.name = object.value(QStringLiteral("name")).toString();
        if (settings.name.isEmpty()) {
            error = QStringLiteral("each scenario needs a name");
            return false;
        }
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (it.key() == QLatin1String("name") || it.key() == QLatin1String("description")) {
                continue;
            }
            if (!applySetting(settings, session, it.key(), it.value(), error)) {
                error = QStringLiteral("scenario \"%1\": %2").arg(settings.name, error);
                return false;
            }
        }
        if (settings.options.maxBufferMs < settings.options.minBufferMs) {
            error = QStringLiteral("scenario \"%1\": max_buffer_ms is below min_buffer_ms").arg(settings.name);
            return false;
        }
        settings.options = settings.options.resolved();
        scenarios.push_back(settings);
    }
    return true;
}

struct Replayed {
    std::vector<TimestampPacing::Policy::Decision> decisions;
    // The policy's source period after each frame, as the trace records it
    std::vector<double> sourcePeriodUs;
};

// The policy is given each frame's recorded render lead, as the pacer gives
// it, unless bufferLeadUs replaces it (zero: size the buffer from readiness
// alone, as the policy did before 2026-10-18)
inline Replayed replayPolicy(const std::vector<Row>& scheduled, const TimestampPacingOptions& options,
                             int streamFps, int64_t bufferLeadUs = -1)
{
    Replayed replayed;
    replayed.decisions.reserve(scheduled.size());
    replayed.sourcePeriodUs.reserve(scheduled.size());
    TimestampPacing::Policy policy;
    policy.configure(options, streamFps);
    for (const Row& row : scheduled) {
        const uint64_t leadUs = bufferLeadUs >= 0 ? uint64_t(bufferLeadUs) : row.renderLeadUs;
        replayed.decisions.push_back(policy.schedule(row.rtpValid, row.frame, row.rtp, row.readyUs,
                                                     row.hostLatencyUs, row.decoderQueueUs, leadUs));
        replayed.sourcePeriodUs.push_back(policy.sourcePeriodUs());
    }
    return replayed;
}

// The target the pacer kept after its hold limit
inline uint64_t heldTargetUs(const TimestampPacing::Policy::Decision& decision, uint64_t readyUs)
{
    return decision.paced ? std::min(decision.targetUs, readyUs + MaximumHoldUs) : 0;
}

struct Mismatch {
    uint64_t sequence = 0;
    int32_t frame = -1;
    QString field;
    qint64 recorded = 0;
    qint64 replayed = 0;
};

struct Baseline {
    uint64_t compared = 0;
    uint64_t mismatchedRows = 0;
    std::map<QString, uint64_t> fieldMismatches;
    bool haveFirst = false;
    Mismatch first;

    bool exact() const { return compared != 0 && mismatchedRows == 0; }
};

// Compares every recorded decision with the replayed one, field by field,
// rounding as the trace writer does
inline Baseline compareBaseline(const Trace& trace, const Replayed& replayed)
{
    Baseline baseline;
    for (size_t i = 0; i < trace.scheduled.size() && i < replayed.decisions.size(); i++) {
        const Row& row = trace.scheduled[i];
        const TimestampPacing::Policy::Decision& d = replayed.decisions[i];
        bool rowMismatched = false;
        const auto check = [&](const char* field, qint64 recorded, qint64 replayedValue) {
            if (recorded == replayedValue) {
                return;
            }
            const QString name = QString::fromLatin1(field);
            baseline.fieldMismatches[name]++;
            rowMismatched = true;
            if (!baseline.haveFirst) {
                baseline.haveFirst = true;
                baseline.first = { row.sequence, row.frame, name, recorded, replayedValue };
            }
        };
        check("paced", row.paced, d.paced);
        check("admitted", row.admitted, d.admitted);
        check("repeat", row.repeat, d.repeat);
        check("restarted", row.restarted, d.restarted);
        check("reanchored", row.reanchored, d.reanchored);
        check("policy_target_us", qint64(row.policyTargetUs), qint64(d.targetUs));
        check("target_us", qint64(row.targetUs), qint64(heldTargetUs(d, row.readyUs)));
        check("buffer_us", qint64(row.bufferUs), qint64(d.bufferUs));
        // The pacer's late flag, from the target and its recorded render lead
        check("late", row.late, d.paced && heldTargetUs(d, row.readyUs) < row.readyUs + row.renderLeadUs);
        check("lateness_us", qint64(row.latenessUs), qint64(d.latenessUs));
        check("correction_us", row.correctionUs, qint64(std::llround(d.correctionUs)));
        check("host_jerk_us", row.hostJerkUs, qint64(std::llround(d.hostJerkUs)));
        check("host_jerk_valid", row.hostJerkValid, d.hostJerkValid);
        check("source_period_us", qint64(row.sourcePeriodUs), qint64(uint64_t(replayed.sourcePeriodUs[i])));
        baseline.compared++;
        if (rowMismatched) {
            baseline.mismatchedRows++;
        }
    }
    if (trace.scheduled.size() != replayed.decisions.size()) {
        baseline.mismatchedRows++;
    }
    return baseline;
}

// Nearest-rank percentiles over collected samples
class Distribution
{
public:
    void add(double value) { m_Values.push_back(value); }
    void merge(const Distribution& other) { m_Values.insert(m_Values.end(), other.m_Values.begin(), other.m_Values.end()); }
    size_t count() const { return m_Values.size(); }

    double mean() const
    {
        if (m_Values.empty()) {
            return 0;
        }
        double sum = 0;
        for (double value : m_Values) {
            sum += value;
        }
        return sum / double(m_Values.size());
    }

    double percentile(double p)
    {
        if (m_Values.empty()) {
            return 0;
        }
        sort();
        const size_t rank = size_t(std::ceil(p / 100.0 * double(m_Values.size())));
        return m_Values[std::min(m_Values.size(), std::max<size_t>(1, rank)) - 1];
    }

    double max()
    {
        return m_Values.empty() ? 0 : percentile(100);
    }

    QJsonObject json()
    {
        QJsonObject object;
        object[QStringLiteral("count")] = qint64(count());
        object[QStringLiteral("mean")] = std::round(mean());
        object[QStringLiteral("p50")] = std::round(percentile(50));
        object[QStringLiteral("p95")] = std::round(percentile(95));
        object[QStringLiteral("p99")] = std::round(percentile(99));
        object[QStringLiteral("p99_9")] = std::round(percentile(99.9));
        object[QStringLiteral("max")] = std::round(max());
        return object;
    }

private:
    void sort()
    {
        if (!m_Sorted) {
            std::sort(m_Values.begin(), m_Values.end());
            m_Sorted = true;
        }
    }

    std::vector<double> m_Values;
    bool m_Sorted = false;
};

// Frames the host re-sent without a new capture, by the policy's own rule.
// It depends only on host latency, so every scenario shares it.
inline std::vector<bool> hostRepeats(const std::vector<Row>& scheduled)
{
    std::vector<bool> repeats(scheduled.size());
    TimestampPacing::RepeatDetector detector;
    for (size_t i = 0; i < scheduled.size(); i++) {
        repeats[i] = scheduled[i].rtpValid && detector.isRepeat(scheduled[i].hostLatencyUs);
    }
    return repeats;
}

// The host's interval between two frames, or zero when they can't be compared
inline double sourceIntervalUs(const Row& from, const Row& to)
{
    if (!from.rtpValid || !to.rtpValid) {
        return 0;
    }
    const int32_t ticks = int32_t(to.rtp - from.rtp);
    return ticks > 0 ? ticks * 100.0 / 9.0 : 0;
}

enum class Span : uint8_t {
    // Every host step between the two frames is comparable and short
    Scored,
    // One of them is longer than LongSourceIntervalUs
    Long,
    // A step without valid timestamps, a host repeat, or frames out of order
    Uncomparable,
};

// Whether the presented interval between two shown frames can be scored
// against the host's. Judged step by step over the frames between them, so a
// frame the model skipped doesn't turn two short source steps into one long
// one and drop the resulting jerk from the score.
inline Span spanBetween(const std::vector<Row>& scheduled, const std::vector<bool>& repeats, size_t from, size_t to,
                        double& sourceUs)
{
    sourceUs = 0;
    if (to <= from) {
        return Span::Uncomparable;
    }
    Span span = Span::Scored;
    for (size_t i = from + 1; i <= to; i++) {
        const double stepUs = repeats[i] || repeats[i - 1] ? 0 : sourceIntervalUs(scheduled[i - 1], scheduled[i]);
        if (stepUs <= 0) {
            return Span::Uncomparable;
        }
        if (stepUs > LongSourceIntervalUs) {
            span = Span::Long;
        }
    }
    sourceUs = sourceIntervalUs(scheduled[from], scheduled[to]);
    return sourceUs > 0 ? span : Span::Uncomparable;
}

struct Outcome {
    // The V-blank or time the frame was planned for; zero when unpaced
    uint64_t plannedUs = 0;
    uint64_t shownUs = 0;
    // Not ready, with its render time, by its target: the pacer's own late
    // flag, which the trace records
    bool late = false;
    // Not ready by the V-blank or time it was planned for, so shown later
    bool missedPlan = false;
    // Replaced before it was shown
    bool skipped = false;
};

// Shows the frames of one scenario, at one grid phase on fixed refresh.
//
// Fixed refresh: a paced frame takes the V-blank its target falls on, through
// the policy's PhaseLock as the pacer uses it; ready too late for it (ready
// plus render time), it takes the first V-blank after. A frame due no later
// than the one before it replaces it, as a mailbox swapchain would.
// VRR: a frame is shown at its target, or once rendered if later, and no
// sooner than one refresh at displayHz after the previous frame.
// Unpaced frames are shown once rendered.
inline std::vector<Outcome> present(const std::vector<Row>& scheduled, const Replayed* replayed,
                                    const Settings& settings, double phaseUs, uint64_t* phaseWraps = nullptr)
{
    std::vector<Outcome> outcomes(scheduled.size());
    const double periodUs = settings.displayHz > 0 ? 1000000.0 / settings.displayHz : 0;
    const bool grid = settings.fixedRefresh && periodUs > 0;
    const auto atOrAfter = [&](double timeUs) {
        return uint64_t(std::llround(phaseUs + std::ceil((timeUs - phaseUs) / periodUs) * periodUs));
    };
    TimestampPacing::PhaseLock lock;
    lock.reset();
    size_t previous = SIZE_MAX;
    for (size_t i = 0; i < scheduled.size(); i++) {
        const Row& row = scheduled[i];
        Outcome& outcome = outcomes[i];
        const bool paced = replayed != nullptr && replayed->decisions[i].paced;
        const uint64_t targetUs = paced ? heldTargetUs(replayed->decisions[i], row.readyUs) : 0;
        const uint64_t renderUs = settings.renderUs >= 0 ? uint64_t(settings.renderUs)
                                  : row.renderLeadUs != 0 ? row.renderLeadUs : DefaultRenderUs;
        const uint64_t dueUs = row.readyUs + renderUs;

        if (grid) {
            uint64_t vblankUs;
            if (paced) {
                vblankUs = atOrAfter(double(targetUs) + lock.trimUs());
                const double trimBeforeUs = lock.trimUs();
                lock.update(targetUs, vblankUs, periodUs, replayed->sourcePeriodUs[i]);
                // A trim that wraps moves every following frame a refresh
                if (phaseWraps != nullptr && std::fabs(lock.trimUs() - trimBeforeUs) > periodUs / 2) {
                    (*phaseWraps)++;
                }
                outcome.plannedUs = vblankUs;
                outcome.late = dueUs > targetUs;
                outcome.missedPlan = dueUs > vblankUs;
                if (outcome.missedPlan) {
                    vblankUs = atOrAfter(double(dueUs));
                }
            }
            else {
                vblankUs = atOrAfter(double(dueUs));
            }
            if (previous != SIZE_MAX && vblankUs <= outcomes[previous].shownUs) {
                outcomes[previous].skipped = true;
                vblankUs = outcomes[previous].shownUs;
            }
            outcome.shownUs = vblankUs;
        }
        else {
            uint64_t showUs = dueUs;
            if (paced) {
                outcome.plannedUs = targetUs;
                outcome.late = dueUs > targetUs;
                outcome.missedPlan = outcome.late;
                showUs = std::max(targetUs, dueUs);
            }
            if (previous != SIZE_MAX && periodUs > 0) {
                showUs = std::max(showUs, uint64_t(std::llround(double(outcomes[previous].shownUs) + periodUs)));
            }
            outcome.shownUs = showUs;
        }
        previous = i;
    }
    return outcomes;
}

struct Metrics {
    uint64_t frames = 0;
    uint64_t paced = 0;
    uint64_t late = 0;
    uint64_t missedPlan = 0;
    uint64_t skipped = 0;
    uint64_t shown = 0;
    uint64_t restarted = 0;
    uint64_t reanchored = 0;
    uint64_t admitted = 0;
    // Presented intervals between consecutive shown frames that were scored,
    // and those left out
    uint64_t intervals = 0;
    uint64_t intervalsOver2ms = 0;
    uint64_t longSourceIntervals = 0;
    uint64_t uncomparableIntervals = 0;
    uint64_t jerkOver2ms = 0;
    // Phase lock trim wraps, summed over grid phases
    uint64_t phaseWraps = 0;
    int phases = 1;
    // Shown minus ready
    Distribution delayUs;
    Distribution bufferUs;
    // Change in presented interval from one scored pair to the next
    Distribution jerkUs;
    // Presented interval minus the host's interval for the same frames
    Distribution residualUs;
};

// Scores one presentation of a scenario into metrics, which sums over phases
inline void score(const std::vector<Row>& scheduled, const std::vector<bool>& repeats, const Replayed* replayed,
                  const std::vector<Outcome>& outcomes, Metrics& metrics)
{
    size_t previous = SIZE_MAX;
    double previousIntervalUs = 0;
    bool havePreviousInterval = false;
    for (size_t i = 0; i < scheduled.size(); i++) {
        const Row& row = scheduled[i];
        const Outcome& outcome = outcomes[i];
        metrics.frames++;
        if (replayed != nullptr) {
            const TimestampPacing::Policy::Decision& decision = replayed->decisions[i];
            metrics.bufferUs.add(double(decision.bufferUs));
            metrics.restarted += decision.restarted;
            metrics.reanchored += decision.reanchored;
            metrics.admitted += decision.admitted;
            if (decision.paced) {
                metrics.paced++;
                metrics.late += outcome.late;
                metrics.missedPlan += outcome.missedPlan;
            }
        }
        if (outcome.skipped) {
            metrics.skipped++;
            continue;
        }
        metrics.shown++;
        metrics.delayUs.add(double(outcome.shownUs) - double(row.readyUs));

        if (previous != SIZE_MAX) {
            double sourceUs = 0;
            const Span span = spanBetween(scheduled, repeats, previous, i, sourceUs);
            const double presentedUs = double(outcome.shownUs) - double(outcomes[previous].shownUs);
            if (span == Span::Scored) {
                const double residualUs = std::fabs(presentedUs - sourceUs);
                metrics.residualUs.add(residualUs);
                metrics.intervals++;
                metrics.intervalsOver2ms += residualUs > HitchUs;
                if (havePreviousInterval) {
                    const double jerkUs = std::fabs(presentedUs - previousIntervalUs);
                    metrics.jerkUs.add(jerkUs);
                    metrics.jerkOver2ms += jerkUs > HitchUs;
                }
                previousIntervalUs = presentedUs;
                havePreviousInterval = true;
            }
            else {
                if (span == Span::Long) {
                    metrics.longSourceIntervals++;
                }
                else {
                    metrics.uncomparableIntervals++;
                }
                havePreviousInterval = false;
            }
        }
        previous = i;
    }
}

inline double perMilleExact(uint64_t count, uint64_t total)
{
    return total != 0 ? std::round(100000.0 * double(count) / double(total)) / 100.0 : 0;
}

struct ScenarioResult {
    Settings settings;
    Metrics metrics;
    // Phase zero, for --timeline
    Replayed replayed;
    std::vector<Outcome> outcomes;
};

inline ScenarioResult runScenario(const Trace& trace, const std::vector<bool>& repeats, const Settings& settings)
{
    ScenarioResult result;
    result.settings = settings;
    if (settings.pacing) {
        result.replayed = replayPolicy(trace.scheduled, settings.options, settings.streamFps, settings.bufferLeadUs);
    }
    const Replayed* replayed = settings.pacing ? &result.replayed : nullptr;
    const bool grid = settings.fixedRefresh && settings.displayHz > 0;
    const int phases = grid ? std::max(1, settings.phases) : 1;
    result.metrics.phases = phases;
    const double periodUs = settings.displayHz > 0 ? 1000000.0 / settings.displayHz : 0;
    for (int phase = 0; phase < phases; phase++) {
        std::vector<Outcome> outcomes =
            present(trace.scheduled, replayed, settings, periodUs * phase / phases, &result.metrics.phaseWraps);
        score(trace.scheduled, repeats, replayed, outcomes, result.metrics);
        if (phase == 0) {
            result.outcomes = std::move(outcomes);
        }
    }
    return result;
}

inline QJsonObject settingsJson(const Settings& settings)
{
    QJsonObject object;
    object[QStringLiteral("pacing")] = settings.pacing ? QStringLiteral("timestamp") : QStringLiteral("off");
    if (settings.pacing) {
        object[QStringLiteral("smoothing")] = smoothingName(settings.options.smoothing);
        object[QStringLiteral("target_per_mille")] = settings.options.targetPerMille;
        object[QStringLiteral("min_buffer_ms")] = settings.options.minBufferMs;
        object[QStringLiteral("max_buffer_ms")] = settings.options.maxBufferMs;
    }
    object[QStringLiteral("stream_fps")] = settings.streamFps;
    object[QStringLiteral("display")] = settings.fixedRefresh ? QStringLiteral("fixed") : QStringLiteral("vrr");
    object[QStringLiteral("display_hz")] = settings.displayHz;
    object[QStringLiteral("render_us")] = settings.renderUs >= 0 ? QJsonValue(qint64(settings.renderUs))
                                                                  : QJsonValue(QStringLiteral("recorded"));
    object[QStringLiteral("phases")] = settings.fixedRefresh && settings.displayHz > 0 ? settings.phases : 1;
    if (settings.pacing) {
        object[QStringLiteral("buffer_lead_us")] = settings.bufferLeadUs >= 0 ? QJsonValue(qint64(settings.bufferLeadUs))
                                                                             : QJsonValue(QStringLiteral("recorded"));
    }
    return object;
}

inline QJsonObject metricsJson(Metrics& m)
{
    QJsonObject object;
    object[QStringLiteral("frames")] = qint64(m.frames);
    object[QStringLiteral("paced")] = qint64(m.paced);
    object[QStringLiteral("shown")] = qint64(m.shown);
    object[QStringLiteral("late_per_mille")] = perMilleExact(m.late, m.paced);
    object[QStringLiteral("missed_plan_per_mille")] = perMilleExact(m.missedPlan, m.paced);
    object[QStringLiteral("skipped_per_mille")] = perMilleExact(m.skipped, m.frames);
    object[QStringLiteral("restarted")] = qint64(m.restarted);
    object[QStringLiteral("reanchored")] = qint64(m.reanchored);
    object[QStringLiteral("admitted")] = qint64(m.admitted);
    object[QStringLiteral("delay_us")] = m.delayUs.json();
    object[QStringLiteral("buffer_us")] = m.bufferUs.json();
    object[QStringLiteral("presented_jerk_us")] = m.jerkUs.json();
    object[QStringLiteral("presented_jerk_over_2ms_per_mille")] = perMilleExact(m.jerkOver2ms, m.jerkUs.count());
    object[QStringLiteral("cadence_residual_us")] = m.residualUs.json();
    object[QStringLiteral("cadence_residual_over_2ms_per_mille")] = perMilleExact(m.intervalsOver2ms, m.intervals);
    object[QStringLiteral("scored_intervals")] = qint64(m.intervals);
    object[QStringLiteral("long_source_intervals")] = qint64(m.longSourceIntervals);
    object[QStringLiteral("uncomparable_intervals")] = qint64(m.uncomparableIntervals);
    object[QStringLiteral("phase_wraps_per_phase")] = std::round(100.0 * double(m.phaseWraps) / std::max(1, m.phases)) / 100.0;
    return object;
}

// The host's cadence over the capture, independent of any scenario
inline QJsonObject sourceJson(const Trace& trace, const std::vector<bool>& repeats)
{
    Distribution intervals;
    uint64_t rtpValid = 0, repeatCount = 0, frameGaps = 0, longIntervals = 0;
    for (size_t i = 0; i < trace.scheduled.size(); i++) {
        const Row& row = trace.scheduled[i];
        rtpValid += row.rtpValid;
        repeatCount += repeats[i];
        if (i == 0) {
            continue;
        }
        const Row& previous = trace.scheduled[i - 1];
        if (row.frame > previous.frame + 1) {
            frameGaps++;
        }
        if (row.frame == previous.frame + 1 && !repeats[i] && !repeats[i - 1]) {
            const double intervalUs = sourceIntervalUs(previous, row);
            if (intervalUs > 0) {
                intervals.add(intervalUs);
                longIntervals += intervalUs > LongSourceIntervalUs;
            }
        }
    }
    QJsonObject object;
    object[QStringLiteral("frames")] = qint64(trace.scheduled.size());
    object[QStringLiteral("rtp_valid")] = qint64(rtpValid);
    object[QStringLiteral("host_repeats")] = qint64(repeatCount);
    object[QStringLiteral("frame_number_gaps")] = qint64(frameGaps);
    object[QStringLiteral("interval_us")] = intervals.json();
    object[QStringLiteral("intervals_over_25ms")] = qint64(longIntervals);
    if (!trace.scheduled.empty()) {
        object[QStringLiteral("duration_s")] =
            std::round(double(trace.scheduled.back().readyUs - trace.scheduled.front().readyUs) / 10000.0) / 100.0;
    }
    return object;
}

// What the capture itself shows: the pacer's own late flags, and display
// times the renderer reported, matched to frames through their release time.
// OS display reports, not optical measurements.
inline QJsonObject recordedJson(const Trace& trace, const std::vector<bool>& repeats)
{
    uint64_t paced = 0, late = 0;
    QHash<uint64_t, size_t> scheduledByRtp;
    for (size_t i = 0; i < trace.scheduled.size(); i++) {
        const Row& row = trace.scheduled[i];
        paced += row.paced;
        late += row.paced && row.late;
        if (row.rtpValid) {
            scheduledByRtp.insert(row.rtp, i);
        }
    }
    QHash<uint64_t, uint32_t> rtpByRelease;
    for (const Row& row : trace.presented) {
        if (row.releaseUs != 0 && row.rtpValid) {
            rtpByRelease.insert(row.releaseUs, row.rtp);
        }
    }

    struct Shown {
        uint64_t displayUs;
        size_t index;
    };
    std::vector<Shown> shown;
    uint64_t reports = 0, unmatched = 0, missed = 0;
    for (const Row& row : trace.displays) {
        if (!row.matched || row.displayUs == 0) {
            continue;
        }
        reports++;
        missed += row.missed;
        const auto rtp = rtpByRelease.constFind(row.releaseUs);
        const auto index = rtp != rtpByRelease.constEnd() ? scheduledByRtp.constFind(*rtp) : scheduledByRtp.constEnd();
        if (index == scheduledByRtp.constEnd()) {
            unmatched++;
            continue;
        }
        shown.push_back({ row.displayUs, *index });
    }
    std::sort(shown.begin(), shown.end(), [](const Shown& a, const Shown& b) { return a.displayUs < b.displayUs; });

    Metrics m;
    Distribution delayUs;
    double previousIntervalUs = 0;
    bool havePreviousInterval = false;
    for (size_t i = 0; i < shown.size(); i++) {
        const Row& row = trace.scheduled[shown[i].index];
        delayUs.add(double(shown[i].displayUs) - double(row.readyUs));
        if (i == 0) {
            continue;
        }
        double sourceUs = 0;
        const Span span = spanBetween(trace.scheduled, repeats, shown[i - 1].index, shown[i].index, sourceUs);
        const double presentedUs = double(shown[i].displayUs) - double(shown[i - 1].displayUs);
        if (span == Span::Scored) {
            const double residualUs = std::fabs(presentedUs - sourceUs);
            m.residualUs.add(residualUs);
            m.intervals++;
            m.intervalsOver2ms += residualUs > HitchUs;
            if (havePreviousInterval) {
                const double jerkUs = std::fabs(presentedUs - previousIntervalUs);
                m.jerkUs.add(jerkUs);
                m.jerkOver2ms += jerkUs > HitchUs;
            }
            previousIntervalUs = presentedUs;
            havePreviousInterval = true;
        }
        else {
            havePreviousInterval = false;
        }
    }

    QJsonObject object;
    object[QStringLiteral("late_per_mille")] = perMilleExact(late, paced);
    object[QStringLiteral("display_reports")] = qint64(reports);
    object[QStringLiteral("display_reports_matched_to_frames")] = qint64(shown.size());
    object[QStringLiteral("display_reports_unmatched")] = qint64(unmatched);
    object[QStringLiteral("display_reports_missed")] = qint64(missed);
    if (!shown.empty()) {
        object[QStringLiteral("display_delay_us")] = delayUs.json();
        object[QStringLiteral("presented_jerk_us")] = m.jerkUs.json();
        object[QStringLiteral("presented_jerk_over_2ms_per_mille")] = perMilleExact(m.jerkOver2ms, m.jerkUs.count());
        object[QStringLiteral("cadence_residual_us")] = m.residualUs.json();
        object[QStringLiteral("cadence_residual_over_2ms_per_mille")] = perMilleExact(m.intervalsOver2ms, m.intervals);
    }
    return object;
}

// The late shift (TimestampPacing::LateShift) replayed from the trace's
// releases, display reports and display mode changes, in the order the pacer
// handled them. Each release's prediction is rebuilt from the frame's
// readiness, its recorded release lead and the V-blank it was planned for,
// less the refresh the shift itself added.
struct LateShiftReplay {
    bool applicable = false;
    size_t confirmedMissesNeeded = 0;
    uint64_t releases = 0;
    uint64_t mismatched = 0;
    uint64_t firstMismatchSequence = 0;
    uint64_t shiftedReleases = 0;
    uint64_t recordedShiftedReleases = 0;
    uint64_t entries = 0;
    uint64_t recordedEntries = 0;
    // Seconds after the first frame was ready
    std::vector<double> entryTimesS;
    std::vector<double> recordedEntryTimesS;

    bool matchesRecorded() const { return applicable && releases != 0 && mismatched == 0; }
};

inline LateShiftReplay replayLateShift(const Trace& trace, size_t confirmedMissesNeeded)
{
    LateShiftReplay result;
    result.confirmedMissesNeeded = confirmedMissesNeeded;
    // Only Gamescope's display-mode probe enables the shift
    result.applicable = trace.session.compositorProbe && !trace.released.empty();
    if (!result.applicable) {
        return result;
    }
    QHash<qint64, uint64_t> readyByFrame;
    for (const Row& row : trace.scheduled) {
        readyByFrame.insert(qint64(row.frame) << 32 | row.rtp, row.readyUs);
    }

    // Merge the three event kinds by sequence
    std::vector<const Row*> events;
    events.reserve(trace.released.size() + trace.displays.size() + trace.displayModes.size());
    for (const std::vector<Row>* rows : { &trace.released, &trace.displays, &trace.displayModes }) {
        for (const Row& row : *rows) {
            events.push_back(&row);
        }
    }
    std::sort(events.begin(), events.end(), [](const Row* a, const Row* b) { return a->sequence < b->sequence; });

    const uint64_t startUs = trace.scheduled.empty() ? 0 : trace.scheduled.front().readyUs;
    const auto secondsAfterStart = [startUs](uint64_t atUs) {
        return std::round((double(atUs) - double(startUs)) / 100000.0) / 10.0;
    };
    // TimestampPacer::DisplayMode::FrameLimited
    constexpr uint32_t frameLimited = 4;
    TimestampPacing::LateShift shift(confirmedMissesNeeded);
    bool recordedShift = false;
    for (const Row* row : events) {
        if (row->event == "display_mode") {
            // Every mode change and refresh change starts the grid over
            shift.reset();
            recordedShift = false;
            continue;
        }
        if (row->event == "display_event") {
            if (row->matched) {
                for (uint32_t i = 0; i < row->superseded; i++) {
                    shift.noteOutcome(true, row->eventUs);
                }
                shift.noteOutcome(row->missed, row->eventUs);
            }
            continue;
        }
        if (row->event != "released") {
            continue;
        }
        const uint64_t readyUs = readyByFrame.value(qint64(row->frame) << 32 | row->rtp, 0);
        if (row->vblankGrid && row->paced && row->displayMode != frameLimited && readyUs != 0 && row->vblankUs != 0) {
            const uint64_t shiftUs = recordedShift ? row->gridPeriodUs : 0;
            const uint64_t assignedUs = row->vblankUs > shiftUs ? row->vblankUs - shiftUs : 0;
            const bool before = shift.shifted();
            shift.notePlanned(readyUs + row->releaseLeadUs > assignedUs, row->eventUs);
            if (!before && shift.shifted()) {
                result.entries++;
                result.entryTimesS.push_back(secondsAfterStart(row->eventUs));
            }
        }
        result.releases++;
        result.shiftedReleases += shift.shifted();
        result.recordedShiftedReleases += row->lateShift;
        if (!recordedShift && row->lateShift) {
            result.recordedEntries++;
            result.recordedEntryTimesS.push_back(secondsAfterStart(row->eventUs));
        }
        if (shift.shifted() != row->lateShift) {
            if (result.mismatched++ == 0) {
                result.firstMismatchSequence = row->sequence;
            }
        }
        recordedShift = row->lateShift;
    }
    return result;
}

inline QJsonObject lateShiftJson(const LateShiftReplay& replay)
{
    QJsonObject object;
    object[QStringLiteral("confirmed_misses_needed")] = qint64(replay.confirmedMissesNeeded);
    object[QStringLiteral("matches_recorded")] = replay.matchesRecorded();
    object[QStringLiteral("releases")] = qint64(replay.releases);
    object[QStringLiteral("mismatched_releases")] = qint64(replay.mismatched);
    if (replay.mismatched != 0) {
        object[QStringLiteral("first_mismatch_sequence")] = qint64(replay.firstMismatchSequence);
    }
    object[QStringLiteral("shifted_releases")] = qint64(replay.shiftedReleases);
    object[QStringLiteral("entries")] = qint64(replay.entries);
    QJsonArray times;
    for (double atS : replay.entryTimesS) {
        times.append(atS);
    }
    object[QStringLiteral("entry_times_s")] = times;
    return object;
}

inline QJsonObject captureJson(const Trace& trace)
{
    QJsonObject footer;
    footer[QStringLiteral("present")] = trace.footerPresent;
    footer[QStringLiteral("clean_shutdown")] = trace.cleanShutdown;
    footer[QStringLiteral("rows_allocated")] = qint64(trace.rowsAllocated);
    footer[QStringLiteral("rows_enqueued")] = qint64(trace.rowsEnqueued);
    footer[QStringLiteral("rows_dropped")] = qint64(trace.rowsDropped);
    footer[QStringLiteral("size_capped")] = trace.sizeCapped;
    footer[QStringLiteral("write_failed")] = trace.writeFailed;

    QJsonObject object;
    object[QStringLiteral("path")] = trace.path;
    object[QStringLiteral("bytes")] = trace.bytes;
    object[QStringLiteral("sha256")] = QString::fromLatin1(trace.fileSha256);
    object[QStringLiteral("compressed")] = trace.compressed;
    object[QStringLiteral("rows")] = qint64(trace.rows);
    object[QStringLiteral("scheduled_rows")] = qint64(trace.scheduled.size());
    object[QStringLiteral("footer")] = footer;
    object[QStringLiteral("decoded_sha256_matches")] = trace.hashMatches();
    object[QStringLiteral("sequence_complete")] = trace.sequenceComplete;
    object[QStringLiteral("integrity_valid")] = trace.integrityValid();
    return object;
}

inline QJsonObject sessionJson(const Trace& trace)
{
    const Row& s = trace.session;
    QJsonObject object;
    object[QStringLiteral("stream_fps")] = s.streamFps;
    object[QStringLiteral("display_hz")] = s.displayHz;
    object[QStringLiteral("smoothing")] = smoothingName(s.smoothing);
    object[QStringLiteral("target_per_mille")] = s.targetPerMille;
    object[QStringLiteral("min_buffer_ms")] = s.minBufferMs;
    object[QStringLiteral("max_buffer_ms")] = s.maxBufferMs;
    object[QStringLiteral("vsync_margin_us")] = s.vsyncMarginUs;
    object[QStringLiteral("vblank_grid_available")] = s.vblankGridAvailable;
    return object;
}

inline QJsonObject baselineJson(const Baseline& baseline)
{
    QJsonObject object;
    object[QStringLiteral("baseline_exact")] = baseline.exact();
    object[QStringLiteral("compared_rows")] = qint64(baseline.compared);
    object[QStringLiteral("mismatched_rows")] = qint64(baseline.mismatchedRows);
    QJsonObject fields;
    for (const auto& field : baseline.fieldMismatches) {
        fields[field.first] = qint64(field.second);
    }
    object[QStringLiteral("field_mismatches")] = fields;
    if (baseline.haveFirst) {
        QJsonObject first;
        first[QStringLiteral("sequence")] = qint64(baseline.first.sequence);
        first[QStringLiteral("frame")] = baseline.first.frame;
        first[QStringLiteral("field")] = baseline.first.field;
        first[QStringLiteral("recorded")] = baseline.first.recorded;
        first[QStringLiteral("replayed")] = baseline.first.replayed;
        object[QStringLiteral("first_mismatch")] = first;
    }
    return object;
}

// The headline numbers --compare reports, as paths into a scenario object
inline const QStringList& comparedMetrics()
{
    static const QStringList metrics {
        QStringLiteral("presented_jerk_over_2ms_per_mille"),
        QStringLiteral("presented_jerk_us/p99"),
        QStringLiteral("cadence_residual_over_2ms_per_mille"),
        QStringLiteral("cadence_residual_us/p99"),
        QStringLiteral("late_per_mille"),
        QStringLiteral("missed_plan_per_mille"),
        QStringLiteral("skipped_per_mille"),
        QStringLiteral("delay_us/mean"),
        QStringLiteral("delay_us/p95"),
        QStringLiteral("delay_us/p99"),
        QStringLiteral("buffer_us/mean"),
        QStringLiteral("phase_wraps_per_phase"),
    };
    return metrics;
}

inline bool metricValue(const QJsonObject& scenario, const QString& path, double& value)
{
    const QStringList parts = path.split(QLatin1Char('/'));
    QJsonValue current = scenario.value(QStringLiteral("metrics"));
    for (const QString& part : parts) {
        if (!current.isObject()) {
            return false;
        }
        current = current.toObject().value(part);
    }
    if (!current.isDouble()) {
        return false;
    }
    value = current.toDouble();
    return true;
}

}
