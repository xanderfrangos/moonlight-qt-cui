#pragma once

#include <Limelight.h>
#include <QtGlobal>

class AudioStats;

class IAudioRenderer
{
public:
    virtual ~IAudioRenderer() {}

    // Decodes one Opus packet into interleaved native-endian float samples, or
    // synthesizes packet loss concealment when data is null. Returns the number
    // of sample frames written, or a negative value on error.
    typedef int (*AudioDecodeCallback)(void* context, const unsigned char* data, int length,
                                       float* pcm, int frameCount);

    // Renderers that decode audio themselves queue compressed packets from
    // submitPacket() and decode them with this callback as the audio device
    // consumes them. bufferMs is the jitter buffer size, or 0 to size it
    // automatically. Called before prepareForPlayback().
    virtual void setDecodeCallback(AudioDecodeCallback, void*, int) {}

    // Where the renderer records measurements for the performance graphs.
    // Called before prepareForPlayback(). The stats outlive the renderer.
    virtual void setStatistics(AudioStats*) {}

    virtual bool decodesAudio() {
        return false;
    }

    // Return false if an unrecoverable error has occurred and the renderer must be reinitialized
    virtual bool submitPacket(const char*, int) {
        return true;
    }

    virtual bool prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig) = 0;

    virtual void* getAudioBuffer(int* size) = 0;

    // Return false if an unrecoverable error has occurred and the renderer must be reinitialized
    virtual bool submitAudio(int bytesWritten) = 0;

    virtual void remapChannels(POPUS_MULTISTREAM_CONFIGURATION) {
        // Use default channel mapping:
        // 0 - Front Left
        // 1 - Front Right
        // 2 - Center
        // 3 - LFE
        // 4 - Surround Left
        // 5 - Surround Right
    }

    enum class AudioFormat {
        Sint16NE,  // 16-bit signed integer (native endian)
        Float32NE, // 32-bit floating point (native endian)
    };
    virtual AudioFormat getAudioBufferFormat() = 0;

    int getAudioBufferSampleSize() {
        switch (getAudioBufferFormat()) {
        case IAudioRenderer::AudioFormat::Sint16NE:
            return sizeof(short);
        case IAudioRenderer::AudioFormat::Float32NE:
            return sizeof(float);
        default:
            Q_UNREACHABLE();
        }
    }
};
