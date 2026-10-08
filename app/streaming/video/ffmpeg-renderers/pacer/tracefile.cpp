#include "tracefile.h"

#include <SDL.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cstring>

namespace {

// Each compressed chunk covers only a few seconds, limiting crash loss while
// still turning repeated timestamps and controller state into very small,
// infrequent physical writes.
constexpr int kChunkBytes = 256 * 1024;
// Always preserve at least an hour, including the maximum supported 480 FPS
// stream cadence. The physical cap takes effect only after that duration, so
// an unusually incompressible trace remains complete rather than silently
// trading away replay fidelity. Chunk compression keeps normal captures far
// below this limit.
constexpr uint64_t kMinimumDurationUs = 60ULL * 60ULL * 1000000ULL;
constexpr uint64_t kMaximumBytes = 512ULL * 1024ULL * 1024ULL;
constexpr char kMagic[] = "MLVRR1\n";

#ifdef _WIN32
bool isUncPath(const QString& path)
{
    return path.startsWith(QStringLiteral("\\\\")) || path.startsWith(QStringLiteral("//"));
}
#endif

}

TraceFile::~TraceFile()
{
    abandon();
}

void TraceFile::abandon()
{
    if (m_File != nullptr) {
        std::fclose(m_File);
        m_File = nullptr;
    }
    m_Chunk.clear();
}

bool TraceFile::open(const QString& path, const char* header, const char* label, uint64_t startUs)
{
    m_Label = label;

#ifdef _WIN32
    // A buffered stdio stream still flushes synchronously when its buffer
    // fills. Keep diagnostic I/O off the time-critical worker's network path.
    if (isUncPath(path)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s trace must use a local path; refusing network trace: %s",
                    label, qPrintable(path));
        return false;
    }
#endif

    // The launcher owns one path for the application lifetime, while each
    // reconnect creates a new trace. Preserve the completed connection before
    // reusing that path so launchers and their latest-trace links still name
    // the current capture. A failed archive must never fall through to truncate.
    QFile previous(path);
    if (previous.exists()) {
        const QFileInfo info(previous);
        const QString extension = info.suffix().isEmpty() ? QString() :
            QStringLiteral(".") + info.suffix();
        QString archivePath;
        for (unsigned connection = 1; ; ++connection) {
            archivePath = info.absoluteDir().filePath(
                info.completeBaseName() +
                QStringLiteral("-connection-%1").arg(connection) + extension);
            if (!QFileInfo::exists(archivePath)) break;
        }
        if (!previous.rename(archivePath)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to preserve previous %s trace; tracing disabled: %s",
                        label, qPrintable(path));
            return false;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "%s trace: preserved earlier connection at %s",
                    label, QFile::encodeName(archivePath).constData());
    }

#ifdef _WIN32
    // Use the checked CRT variant on Windows so enabling diagnostics does not
    // introduce a deprecation warning in the normal application build.
    if (_wfopen_s(&m_File, reinterpret_cast<const wchar_t*>(path.utf16()), L"wb") != 0) {
        m_File = nullptr;
    }
#else
    m_File = std::fopen(QFile::encodeName(path).constData(), "wb");
#endif
    if (m_File == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to open %s trace file: %s", label, qPrintable(path));
        return false;
    }

    // A .csv suffix explicitly requests the directly readable compatibility
    // format. The recommended compressed format compresses independent chunks
    // and typically reduces a full session by an order of magnitude.
    m_Compressed = !path.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive);

    // Amortize local diagnostic writes instead of flushing on every frame.
    // fclose() commits the CSV tail; compressed chunks flush independently.
    std::setvbuf(m_File, nullptr, _IOFBF, 1024 * 1024);

    m_BytesWritten = 0;
    m_Chunk.clear();
    m_DecodedHash.reset();
    m_SizeCapped = false;
    m_WriteFailed = false;
    const size_t headerBytes = std::strlen(header);
    if (m_Compressed) {
        const size_t magicBytes = sizeof(kMagic) - 1;
        if (std::fwrite(kMagic, 1, magicBytes, m_File) != magicBytes) {
            std::fclose(m_File);
            m_File = nullptr;
            return false;
        }
        m_BytesWritten = magicBytes;
        m_Chunk.append(header, qsizetype(headerBytes));
    }
    else {
        if (std::fwrite(header, 1, headerBytes, m_File) != headerBytes) {
            std::fclose(m_File);
            m_File = nullptr;
            return false;
        }
        m_BytesWritten = headerBytes;
    }
    // Qt 6.2 (Ubuntu 22.04 AppImage builds) has no QByteArrayView overload
    m_DecodedHash.addData(QByteArray::fromRawData(header, qsizetype(headerBytes)));

    m_StartUs = startUs;
    m_LatestUs = startUs;
    return true;
}

bool TraceFile::append(const QByteArray& line, uint64_t coveredUs)
{
    if (m_File == nullptr || !accepting()) {
        return false;
    }
    m_LatestUs = std::max(m_LatestUs, coveredUs);

    if (m_Compressed) {
        m_DecodedHash.addData(line);
        m_Chunk.append(line);
        if (m_Chunk.size() >= kChunkBytes) {
            flushChunk(true);
        }
        return accepting();
    }

    const size_t bytesWritten = std::fwrite(
        line.constData(), 1, static_cast<size_t>(line.size()), m_File);
    m_BytesWritten += bytesWritten;
    if (bytesWritten != static_cast<size_t>(line.size())) {
        m_WriteFailed = true;
    }
    else {
        m_DecodedHash.addData(line);
        if (m_BytesWritten >= kMaximumBytes && minimumDurationCaptured()) {
            m_SizeCapped = true;
        }
    }
    return accepting();
}

void TraceFile::flush()
{
    if (m_Compressed) {
        flushChunk(true);
    }
}

void TraceFile::flushChunk(bool enforceSizeCap)
{
    if (m_Chunk.isEmpty() || m_File == nullptr) {
        return;
    }

    const QByteArray compressed = qCompress(m_Chunk, 6);
    const uint32_t compressedBytes = static_cast<uint32_t>(compressed.size());
    const uint64_t recordBytes = sizeof(compressedBytes) + compressedBytes;

    const unsigned char lengthBytes[4] = {
        static_cast<unsigned char>(compressedBytes & 0xff),
        static_cast<unsigned char>((compressedBytes >> 8) & 0xff),
        static_cast<unsigned char>((compressedBytes >> 16) & 0xff),
        static_cast<unsigned char>((compressedBytes >> 24) & 0xff),
    };
    const size_t lengthWritten = std::fwrite(
        lengthBytes, 1, sizeof(lengthBytes), m_File);
    const size_t payloadWritten = std::fwrite(
        compressed.constData(), 1, static_cast<size_t>(compressed.size()),
        m_File);
    if (lengthWritten != sizeof(lengthBytes) ||
        payloadWritten != static_cast<size_t>(compressed.size())) {
        m_WriteFailed = true;
    }
    else {
        m_BytesWritten += recordBytes;
        // A completed chunk is independently recoverable after a crash. This
        // is a low-frequency write performed only by the background thread.
        if (std::fflush(m_File) != 0) {
            m_WriteFailed = true;
        }
        else if (enforceSizeCap &&
                 m_BytesWritten >= kMaximumBytes &&
                 minimumDurationCaptured()) {
            m_SizeCapped = true;
        }
    }
    m_Chunk.clear();
}

bool TraceFile::minimumDurationCaptured() const
{
    return m_LatestUs >= m_StartUs &&
        m_LatestUs - m_StartUs >= kMinimumDurationUs;
}

void TraceFile::close(const QByteArray& footer)
{
    if (m_File != nullptr) {
        if (!m_Compressed && std::fflush(m_File) != 0) {
            m_WriteFailed = true;
        }
        const QByteArray line = footer +
            QByteArrayLiteral(",size_capped=") +
            QByteArray::number(m_SizeCapped ? 1 : 0) +
            QByteArrayLiteral(",write_failed=") +
            QByteArray::number(m_WriteFailed ? 1 : 0) +
            QByteArrayLiteral(",decoded_sha256=") +
            m_DecodedHash.result().toHex() +
            QByteArrayLiteral("\n");
        if (m_Compressed) {
            m_Chunk.append(line);
            // The footer is metadata, not a captured row. Crossing the size
            // threshold by these few bytes did not truncate the capture.
            flushChunk(false);
        }
        else {
            const size_t footerBytes = static_cast<size_t>(line.size());
            if (std::fwrite(line.constData(), 1, footerBytes, m_File) != footerBytes ||
                    std::fflush(m_File) != 0) {
                m_WriteFailed = true;
            }
        }
    }

    if (m_SizeCapped) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s trace was capped at 512 MiB after preserving at least one hour",
                    m_Label);
        m_SizeCapped = false;
    }
    if (m_WriteFailed) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s trace reported a write failure", m_Label);
    }

    if (m_File != nullptr) {
        if (std::fclose(m_File) != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "%s trace close reported a write failure", m_Label);
        }
        m_File = nullptr;
    }
    m_Chunk.clear();
    m_WriteFailed = false;
}
