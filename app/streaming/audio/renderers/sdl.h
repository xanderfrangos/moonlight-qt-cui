#pragma once

#include "renderer.h"
#include "SDL_compat.h"

#include <vector>

class AudioStats;

class SdlAudioRenderer : public IAudioRenderer
{
public:
    SdlAudioRenderer();

    virtual ~SdlAudioRenderer();

    virtual void setDecodeCallback(AudioDecodeCallback callback, void* context, int bufferMs);

    virtual void setStatistics(AudioStats* stats);

    virtual bool decodesAudio();

    virtual bool submitPacket(const char* data, int length);

    virtual bool prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig);

    virtual void* getAudioBuffer(int* size);

    virtual bool submitAudio(int bytesWritten);

    virtual AudioFormat getAudioBufferFormat();

private:
    enum class PlaybackState {
        Priming,
        Playing,
    };

    static void SDLCALL audioCallback(void* userdata, Uint8* stream, int len);

    void renderChunk(float* out, int frames);

    int queuedFrames();

    bool popPacket(int* length);

    int decodeInto(float* pcm, int length);

    void dropPacketWithCrossfade(int length);

    void applyFadeIn(int frames);

    void adjustAutomaticTarget(int deltaMs);

    void logStatistics(bool final);

    SDL_AudioDeviceID m_AudioDevice;
    AudioDecodeCallback m_DecodeCallback;
    void* m_DecodeContext;
    bool m_AutomaticBuffer;

    int m_Channels;
    int m_SampleRate;
    int m_SamplesPerFrame;
    int m_PeriodFrames;
    int m_FadeFrames;

    // Compressed packets waiting to be decoded, guarded by m_QueueLock.
    // A zero length marks a packet lost on the network.
    SDL_mutex* m_QueueLock;
    std::vector<unsigned char> m_PacketData;
    std::vector<int> m_PacketLengths;
    int m_PacketHead;
    int m_PacketCount;

    // Only touched by the audio device thread after playback starts
    PlaybackState m_State;
    std::vector<float> m_Pcm;
    int m_PcmFrames;
    std::vector<float> m_Scratch;
    std::vector<unsigned char> m_PacketScratch;
    int m_TargetFrames;
    int m_ConcealedFrames;
    int m_FadeInRemaining;
    Uint64 m_FramesSinceTrouble;
    bool m_Starved;
    int m_StarvedFrames;

    // Written by the audio device thread and logged by the submitting thread
    SDL_atomic_t m_TargetMs;
    SDL_atomic_t m_Underruns;
    SDL_atomic_t m_Concealments;
    SDL_atomic_t m_DroppedPackets;
    int m_LoggedTargetMs;
    int m_LoggedUnderruns;
    int m_LoggedConcealments;
    int m_LoggedDroppedPackets;
    int m_SubmittedPackets;

    // Performance graph measurements, recorded on the audio device thread
    AudioStats* m_Stats;
    Uint64 m_LastCallbackTime;
};
