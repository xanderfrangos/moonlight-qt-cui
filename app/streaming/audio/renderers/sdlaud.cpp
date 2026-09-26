#include "sdl.h"
#include "streaming/audio/audiostats.h"

#include <Limelight.h>

#include <cmath>

#define PI_F 3.14159265f

// Largest Opus packet moonlight-common-c delivers
#define MAX_PACKET_SIZE 1400

// Room for well over the largest jitter buffer at 5 ms per packet
#define PACKET_QUEUE_SLOTS 64

// Length of the fades used to hide discontinuities
#define FADE_MS 2

// How long Opus may extend the audio while waiting for a late packet
#define MAX_CONCEALMENT_MS 30

// Automatic buffer sizing bounds and step sizes
#define AUTO_INITIAL_MS 20
#define AUTO_MIN_MS 10
#define AUTO_MAX_MS 80
#define AUTO_CONCEALMENT_STEP_MS 5
#define AUTO_UNDERRUN_STEP_MS 10
#define AUTO_RELAX_STEP_MS 5
#define AUTO_RELAX_SECONDS 60

// A gap longer than this is the stream pausing (muting, reconnecting),
// not network jitter, so it isn't counted against the buffer size
#define STREAM_PAUSE_MS 500

SdlAudioRenderer::SdlAudioRenderer()
    : m_AudioDevice(0),
      m_DecodeCallback(nullptr),
      m_DecodeContext(nullptr),
      m_AutomaticBuffer(true),
      m_Channels(0),
      m_SampleRate(0),
      m_SamplesPerFrame(0),
      m_PeriodFrames(0),
      m_FadeFrames(0),
      m_QueueLock(SDL_CreateMutex()),
      m_PacketHead(0),
      m_PacketCount(0),
      m_State(PlaybackState::Priming),
      m_PcmFrames(0),
      m_TargetFrames(0),
      m_ConcealedFrames(0),
      m_FadeInRemaining(0),
      m_FramesSinceTrouble(0),
      m_Starved(false),
      m_StarvedFrames(0),
      m_LoggedTargetMs(0),
      m_LoggedUnderruns(0),
      m_LoggedConcealments(0),
      m_LoggedDroppedPackets(0),
      m_SubmittedPackets(0),
      m_Stats(nullptr),
      m_LastCallbackTime(0)
{
    SDL_AtomicSet(&m_TargetMs, AUTO_INITIAL_MS);
    SDL_AtomicSet(&m_Underruns, 0);
    SDL_AtomicSet(&m_Concealments, 0);
    SDL_AtomicSet(&m_DroppedPackets, 0);

    SDL_assert(!SDL_WasInit(SDL_INIT_AUDIO));

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s",
                     SDL_GetError());
        SDL_assert(SDL_WasInit(SDL_INIT_AUDIO));
    }
}

void SdlAudioRenderer::setDecodeCallback(AudioDecodeCallback callback, void* context, int bufferMs)
{
    m_DecodeCallback = callback;
    m_DecodeContext = context;
    m_AutomaticBuffer = bufferMs <= 0;
    SDL_AtomicSet(&m_TargetMs, m_AutomaticBuffer ? AUTO_INITIAL_MS : bufferMs);
}

void SdlAudioRenderer::setStatistics(AudioStats* stats)
{
    m_Stats = stats;
}

bool SdlAudioRenderer::decodesAudio()
{
    return true;
}

bool SdlAudioRenderer::prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig)
{
    SDL_AudioSpec want, have;

    m_Channels = opusConfig->channelCount;
    m_SampleRate = opusConfig->sampleRate;
    m_SamplesPerFrame = opusConfig->samplesPerFrame;
    m_FadeFrames = SDL_min(m_SampleRate * FADE_MS / 1000, m_SamplesPerFrame);
    m_TargetFrames = SDL_AtomicGet(&m_TargetMs) * m_SampleRate / 1000;
    m_LoggedTargetMs = SDL_AtomicGet(&m_TargetMs);
    if (m_Stats != nullptr) {
        m_Stats->setTargetMs((float)m_LoggedTargetMs);
    }

    SDL_zero(want);
    want.freq = opusConfig->sampleRate;
    want.format = AUDIO_F32SYS;
    want.channels = opusConfig->channelCount;
    want.callback = audioCallback;
    want.userdata = this;

    // On PulseAudio systems, setting a value too small can cause underruns for other
    // applications sharing this output device. We impose a floor of 480 samples (10 ms)
    // to mitigate this issue. Network jitter is absorbed by our own packet queue.
    want.samples = SDL_max(480, opusConfig->samplesPerFrame);

    m_PacketData.resize(PACKET_QUEUE_SLOTS * MAX_PACKET_SIZE);
    m_PacketLengths.resize(PACKET_QUEUE_SLOTS);
    m_PacketScratch.resize(MAX_PACKET_SIZE);
    m_Scratch.resize(m_SamplesPerFrame * m_Channels);

    m_AudioDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (m_AudioDevice == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to open audio device: %s",
                     SDL_GetError());
        return false;
    }

    // Each chunk we render can leave up to one decoded packet behind
    m_PeriodFrames = have.samples;
    m_Pcm.resize((m_PeriodFrames + 2 * m_SamplesPerFrame) * m_Channels);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Obtained audio buffer: %u samples (%u bytes)",
                have.samples,
                have.size);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Audio jitter buffer: %s, %d ms target",
                m_AutomaticBuffer ? "automatic" : "fixed",
                SDL_AtomicGet(&m_TargetMs));

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SDL audio driver: %s",
                SDL_GetCurrentAudioDriver());

    // Start playback
    SDL_PauseAudioDevice(m_AudioDevice, 0);

    return true;
}

SdlAudioRenderer::~SdlAudioRenderer()
{
    if (m_AudioDevice != 0) {
        // Stop playback. This waits for any running audio callback to finish.
        SDL_PauseAudioDevice(m_AudioDevice, 1);
        SDL_CloseAudioDevice(m_AudioDevice);

        logStatistics(true);
    }

    SDL_DestroyMutex(m_QueueLock);

    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    SDL_assert(!SDL_WasInit(SDL_INIT_AUDIO));
}

void* SdlAudioRenderer::getAudioBuffer(int*)
{
    // Unused because we decode packets ourselves
    return nullptr;
}

bool SdlAudioRenderer::submitAudio(int)
{
    // Unused because we decode packets ourselves
    return true;
}

bool SdlAudioRenderer::submitPacket(const char* data, int length)
{
    // Our device may enter a permanent error status upon removal, so we need
    // to recreate the audio device to pick up the new default audio device.
    if (SDL_GetAudioDeviceStatus(m_AudioDevice) == SDL_AUDIO_STOPPED) {
        return false;
    }

    // Conceal anything we can't hold rather than dropping it from the timeline
    if (data == nullptr || length > MAX_PACKET_SIZE) {
        length = 0;
    }

    SDL_LockMutex(m_QueueLock);

    if (m_PacketCount == PACKET_QUEUE_SLOTS) {
        // The device has stopped consuming audio, so discard the oldest
        // packet to keep latency bounded
        m_PacketHead = (m_PacketHead + 1) % PACKET_QUEUE_SLOTS;
        m_PacketCount--;
        SDL_AtomicIncRef(&m_DroppedPackets);
    }

    int slot = (m_PacketHead + m_PacketCount) % PACKET_QUEUE_SLOTS;
    if (length > 0) {
        SDL_memcpy(&m_PacketData[slot * MAX_PACKET_SIZE], data, length);
    }
    m_PacketLengths[slot] = length;
    m_PacketCount++;

    SDL_UnlockMutex(m_QueueLock);

    // Report buffer changes about once per second
    if (++m_SubmittedPackets % (m_SampleRate / m_SamplesPerFrame) == 0) {
        logStatistics(false);
    }

    return true;
}

IAudioRenderer::AudioFormat SdlAudioRenderer::getAudioBufferFormat()
{
    return AudioFormat::Float32NE;
}

void SDLCALL SdlAudioRenderer::audioCallback(void* userdata, Uint8* stream, int len)
{
    SdlAudioRenderer* me = static_cast<SdlAudioRenderer*>(userdata);
    float* out = reinterpret_cast<float*>(stream);
    int frames = len / (int)(sizeof(float) * me->m_Channels);
    int requestedFrames = frames;

    while (frames > 0) {
        int chunk = SDL_min(frames, me->m_PeriodFrames);
        me->renderChunk(out, chunk);
        out += chunk * me->m_Channels;
        frames -= chunk;
    }

    if (me->m_Stats != nullptr) {
        Uint64 now = SDL_GetPerformanceCounter();
        float intervalMs = me->m_LastCallbackTime != 0
                ? (float)((now - me->m_LastCallbackTime) * 1000.0 / SDL_GetPerformanceFrequency())
                : 0;
        me->m_LastCallbackTime = now;

        float framesPerMs = me->m_SampleRate / 1000.0f;
        me->m_Stats->recordDeviceRequest(intervalMs,
                                         requestedFrames / framesPerMs,
                                         (me->queuedFrames() + me->m_PcmFrames) / framesPerMs);
    }
}

int SdlAudioRenderer::queuedFrames()
{
    SDL_LockMutex(m_QueueLock);
    int frames = m_PacketCount * m_SamplesPerFrame;
    SDL_UnlockMutex(m_QueueLock);
    return frames;
}

bool SdlAudioRenderer::popPacket(int* length)
{
    SDL_LockMutex(m_QueueLock);

    if (m_PacketCount == 0) {
        SDL_UnlockMutex(m_QueueLock);
        return false;
    }

    *length = m_PacketLengths[m_PacketHead];
    if (*length > 0) {
        SDL_memcpy(m_PacketScratch.data(), &m_PacketData[m_PacketHead * MAX_PACKET_SIZE], *length);
    }
    m_PacketHead = (m_PacketHead + 1) % PACKET_QUEUE_SLOTS;
    m_PacketCount--;

    SDL_UnlockMutex(m_QueueLock);
    return true;
}

int SdlAudioRenderer::decodeInto(float* pcm, int length)
{
    // A zero length asks Opus to conceal a missing packet
    return m_DecodeCallback(m_DecodeContext,
                            length > 0 ? m_PacketScratch.data() : nullptr,
                            length,
                            pcm,
                            m_SamplesPerFrame);
}

void SdlAudioRenderer::renderChunk(float* out, int frames)
{
    if (m_State == PlaybackState::Priming) {
        // Stay silent until the queue holds the target plus this chunk
        if (m_DecodeCallback == nullptr || queuedFrames() + m_PcmFrames < m_TargetFrames + frames) {
            SDL_memset(out, 0, frames * m_Channels * sizeof(float));
            if (m_Starved) {
                // Capped so a very long pause can't overflow
                m_StarvedFrames = SDL_min(m_StarvedFrames + frames, m_SampleRate * STREAM_PAUSE_MS / 1000);
            }
            return;
        }

        if (m_Starved && m_StarvedFrames < m_SampleRate * STREAM_PAUSE_MS / 1000) {
            SDL_AtomicIncRef(&m_Underruns);
            if (m_Stats != nullptr) {
                m_Stats->recordUnderrun();
            }
            adjustAutomaticTarget(AUTO_UNDERRUN_STEP_MS);
        }

        m_Starved = false;
        m_State = PlaybackState::Playing;
        m_ConcealedFrames = 0;
        m_FadeInRemaining = m_FadeFrames;
    }

    // Decode a fade's worth past this chunk, so that if the audio runs out
    // there is still something left to fade out instead of cutting off
    int neededFrames = frames + m_FadeFrames;
    int maxConcealedFrames = m_SampleRate * MAX_CONCEALMENT_MS / 1000;
    while (m_PcmFrames < neededFrames) {
        float* dest = &m_Pcm[m_PcmFrames * m_Channels];
        int length;

        if (popPacket(&length)) {
            int decoded = decodeInto(dest, length);
            if (decoded > 0) {
                m_PcmFrames += decoded;
            }

            // A late packet arrived after we concealed its absence
            if (length > 0 && m_ConcealedFrames > 0) {
                SDL_AtomicIncRef(&m_Concealments);
                if (m_Stats != nullptr) {
                    m_Stats->recordConcealment();
                }
                adjustAutomaticTarget(AUTO_CONCEALMENT_STEP_MS);
                m_ConcealedFrames = 0;
            }
        }
        else if (m_ConcealedFrames < maxConcealedFrames) {
            // The next packet is late. Let Opus extend the audio rather than
            // letting the device run dry and click.
            int decoded = decodeInto(dest, 0);
            if (decoded > 0) {
                m_PcmFrames += decoded;
                m_ConcealedFrames += decoded;
            }
            else {
                m_ConcealedFrames = maxConcealedFrames;
            }
        }
        else {
            break;
        }
    }

    if (m_PcmFrames < neededFrames) {
        // Underrun. Fade out what we have and refill the buffer before resuming.
        int available = SDL_min(m_PcmFrames, frames);
        applyFadeIn(available);

        int fade = SDL_min(m_FadeFrames, available);
        float* tail = &m_Pcm[(available - fade) * m_Channels];
        for (int i = 0; i < fade; i++) {
            float gain = 0.5f + 0.5f * cosf(PI_F * (i + 0.5f) / fade);
            for (int c = 0; c < m_Channels; c++) {
                tail[i * m_Channels + c] *= gain;
            }
        }

        SDL_memcpy(out, m_Pcm.data(), available * m_Channels * sizeof(float));
        SDL_memset(&out[available * m_Channels], 0, (frames - available) * m_Channels * sizeof(float));

        m_PcmFrames = 0;
        m_State = PlaybackState::Priming;
        m_Starved = true;
        m_StarvedFrames = frames - available;
        m_FramesSinceTrouble = 0;
        return;
    }

    // If a burst of late packets or clock drift has built up well past the
    // target, drop whole packets to bring latency back down
    int remaining = queuedFrames() + m_PcmFrames - frames;
    if (remaining > 2 * m_TargetFrames + m_SamplesPerFrame) {
        int length;
        while (remaining - m_SamplesPerFrame >= m_TargetFrames && popPacket(&length)) {
            dropPacketWithCrossfade(length);
            remaining -= m_SamplesPerFrame;
            SDL_AtomicIncRef(&m_DroppedPackets);
        }
    }

    applyFadeIn(frames);
    SDL_memcpy(out, m_Pcm.data(), frames * m_Channels * sizeof(float));
    m_PcmFrames -= frames;
    SDL_memmove(m_Pcm.data(), &m_Pcm[frames * m_Channels], m_PcmFrames * m_Channels * sizeof(float));

    // Shrink an automatic buffer after a long stretch without trouble
    m_FramesSinceTrouble += frames;
    if (m_FramesSinceTrouble >= (Uint64)m_SampleRate * AUTO_RELAX_SECONDS) {
        adjustAutomaticTarget(-AUTO_RELAX_STEP_MS);
    }
}

void SdlAudioRenderer::dropPacketWithCrossfade(int length)
{
    // Decode the packet so Opus's state stays continuous, then blend the end
    // of it into the audio we already have. The next packet follows on from
    // the end of this one, so the join is seamless.
    int decoded = decodeInto(m_Scratch.data(), length);
    if (decoded <= 0) {
        return;
    }

    int fade = SDL_min(SDL_min(m_FadeFrames, decoded), m_PcmFrames);
    float* tail = &m_Pcm[(m_PcmFrames - fade) * m_Channels];
    const float* next = &m_Scratch[(decoded - fade) * m_Channels];
    for (int i = 0; i < fade; i++) {
        float weight = 0.5f - 0.5f * cosf(PI_F * (i + 0.5f) / fade);
        for (int c = 0; c < m_Channels; c++) {
            float& sample = tail[i * m_Channels + c];
            sample = sample * (1.0f - weight) + next[i * m_Channels + c] * weight;
        }
    }
}

void SdlAudioRenderer::applyFadeIn(int frames)
{
    int count = SDL_min(frames, m_FadeInRemaining);
    int start = m_FadeFrames - m_FadeInRemaining;
    for (int i = 0; i < count; i++) {
        float gain = 0.5f - 0.5f * cosf(PI_F * (start + i + 0.5f) / m_FadeFrames);
        for (int c = 0; c < m_Channels; c++) {
            m_Pcm[i * m_Channels + c] *= gain;
        }
    }
    m_FadeInRemaining -= count;
}

void SdlAudioRenderer::adjustAutomaticTarget(int deltaMs)
{
    m_FramesSinceTrouble = 0;

    if (!m_AutomaticBuffer) {
        return;
    }

    int targetMs = SDL_max(AUTO_MIN_MS, SDL_min(SDL_AtomicGet(&m_TargetMs) + deltaMs, AUTO_MAX_MS));
    m_TargetFrames = targetMs * m_SampleRate / 1000;
    SDL_AtomicSet(&m_TargetMs, targetMs);
    if (m_Stats != nullptr) {
        m_Stats->setTargetMs((float)targetMs);
    }
}

void SdlAudioRenderer::logStatistics(bool final)
{
    int targetMs = SDL_AtomicGet(&m_TargetMs);
    int underruns = SDL_AtomicGet(&m_Underruns);
    int concealments = SDL_AtomicGet(&m_Concealments);
    int droppedPackets = SDL_AtomicGet(&m_DroppedPackets);

    if (!final &&
            targetMs == m_LoggedTargetMs &&
            underruns == m_LoggedUnderruns &&
            concealments == m_LoggedConcealments &&
            droppedPackets == m_LoggedDroppedPackets) {
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Audio buffer%s: %d ms target, %d underruns, %d late packets concealed, %d packets dropped to reduce latency",
                final ? " totals" : "",
                targetMs,
                underruns,
                concealments,
                droppedPackets);

    m_LoggedTargetMs = targetMs;
    m_LoggedUnderruns = underruns;
    m_LoggedConcealments = concealments;
    m_LoggedDroppedPackets = droppedPackets;
}
