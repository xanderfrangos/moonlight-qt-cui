#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>

#include <cstdint>
#include <cstdio>

// The file behind a pacing trace (MOONLIGHT_VRR_TRACE), shared by VRR Pacing
// Mode and timestamp pacing. Each owns its rows, queue and writer thread; this
// owns everything from a formatted line onwards. Only the writer thread may
// call append() and flush(), and close() only after it has stopped.
//
// A path ending in .csv is written as plain CSV. Any other path gets the
// chunk-compressed format: "MLVRR1\n", then independently compressed chunks
// (scripts/decode-vrr-trace.py expands them). Both end with a footer line
// that counts rows and carries the SHA-256 of the decoded text.
class TraceFile
{
public:
    TraceFile() = default;
    TraceFile(const TraceFile&) = delete;
    TraceFile& operator=(const TraceFile&) = delete;
    ~TraceFile();

    // Opens a new trace at path, after renaming a file already there (an
    // earlier connection under the same launcher path) to
    // <base>-connection-N.<suffix>. label names the trace in log messages.
    // Refuses network paths on Windows. startUs begins the hour that is
    // always kept before the size cap can apply.
    bool open(const QString& path, const char* header, const char* label, uint64_t startUs);
    bool isOpen() const { return m_File != nullptr; }

    // Writes one formatted line, ending in '\n'. coveredUs is the newest
    // capture time it contains, which decides when the size cap may apply.
    // Returns false once the file accepts no more lines: a write failed or
    // the cap was reached.
    bool append(const QByteArray& line, uint64_t coveredUs);

    // Writes the pending compressed chunk
    void flush();

    // Writes footer, followed by the size cap, write failure and decoded
    // hash fields, and closes the file. footer starts with '#' and has no
    // trailing newline.
    void close(const QByteArray& footer);

    // Closes a file that never received rows, without a footer
    void abandon();

    bool sizeCapped() const { return m_SizeCapped; }
    bool writeFailed() const { return m_WriteFailed; }

private:
    void flushChunk(bool enforceSizeCap);
    bool minimumDurationCaptured() const;
    bool accepting() const { return !m_WriteFailed && !m_SizeCapped; }

    const char* m_Label = "";
    std::FILE* m_File = nullptr;
    bool m_Compressed = false;
    QByteArray m_Chunk;
    QCryptographicHash m_DecodedHash { QCryptographicHash::Sha256 };
    uint64_t m_BytesWritten = 0;
    uint64_t m_StartUs = 0;
    uint64_t m_LatestUs = 0;
    bool m_SizeCapped = false;
    bool m_WriteFailed = false;
};
