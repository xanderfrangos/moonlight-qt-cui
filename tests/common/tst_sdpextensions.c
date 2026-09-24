#include "Limelight-internal.h"

// The unchanged generator is compiled under this name by the production wrapper.
char* getSdpPayloadForStreamConfigBase(int rtspClientVersion, int* length);

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Line %d: %s (case %d)\n", __LINE__, #condition, cases); \
        exit(1); \
    } \
} while (0)

static int cases;

static void discardLog(const char* format, ...) {
    (void)format;
}

static void resetGeneratorState(void) {
    // The generator subtracts encryption overhead from this connection field.
    StreamConfig.packetSize = 1392;
    AudioEncryptionEnabled = false;
    EncryptionFeaturesEnabled = 0;
    HighQualitySurroundEnabled = false;
    AudioPacketDuration = 0;
}

int main(void) {
    static const struct {
        int version[4];
        bool eligible;
    } hosts[] = {
        {{3, 0, 0, 0}, false},
        {{6, 9, 999, -1}, false},
        {{7, 0, 999, -1}, false},
        {{7, 1, 349, -1}, false},
        {{7, 1, 350, -1}, true},
        {{7, 1, 431, -1}, true},
        {{7, 1, 500, 0}, false},
        {{7, 1, 500, 1}, false},
        {{7, 2, 0, -1}, true},
        {{8, 0, 0, -1}, true},
    };
    static const struct {
        int format;
        int recovery;
    } codecs[] = {
        {VIDEO_FORMAT_H264, CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC},
        {VIDEO_FORMAT_H264_HIGH8_444, CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC},
        {VIDEO_FORMAT_H265, CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC},
        {VIDEO_FORMAT_H265_MAIN10, CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC},
        {VIDEO_FORMAT_H265_REXT8_444, CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC},
        {VIDEO_FORMAT_H265_REXT10_444, CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC},
        {VIDEO_FORMAT_AV1_MAIN8, CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1},
        {VIDEO_FORMAT_AV1_MAIN10, CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1},
        {VIDEO_FORMAT_AV1_HIGH8_444, CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1},
        {VIDEO_FORMAT_AV1_HIGH10_444, CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1},
    };
    static const char attribute[] = "a=x-ss-video[0].intraRefresh:1\r\n";
    size_t host, codec;
    int caps, encrypted;
#ifdef _WIN32
    WSADATA wsaData;
    CHECK(WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
#endif

    LiInitializeStreamConfiguration(&StreamConfig);
    LiInitializeConnectionCallbacks(&ListenerCallbacks);
    ListenerCallbacks.logMessage = discardLog;
    StreamConfig.width = 1920;
    StreamConfig.height = 1080;
    StreamConfig.fps = 120;
    StreamConfig.bitrate = 20000;
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
    // Intentionally advertise all codecs, even when a fallback was negotiated.
    StreamConfig.supportedVideoFormats = VIDEO_FORMAT_MASK_H264 |
            VIDEO_FORMAT_MASK_H265 | VIDEO_FORMAT_MASK_AV1;
    RemoteAddr.ss_family = AF_INET;
    ((struct sockaddr_in*)&RemoteAddr)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    VideoPortNumber = 47998;

    for (host = 0; host < sizeof(hosts) / sizeof(hosts[0]); host++) {
        memcpy(AppVersionQuad, hosts[host].version, sizeof(AppVersionQuad));
        for (codec = 0; codec < sizeof(codecs) / sizeof(codecs[0]); codec++) {
            NegotiatedVideoFormat = codecs[codec].format;
            for (caps = 0; caps < 8; caps++) {
                VideoCallbacks.capabilities =
                        ((caps & 1) ? CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC : 0) |
                        ((caps & 2) ? CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC : 0) |
                        ((caps & 4) ? CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1 : 0);
                for (encrypted = 0; encrypted < 2; encrypted++) {
                    int baseLength = -1, actualLength = -1;
                    char* base;
                    char* actual;
                    char* added;
                    bool expected = hosts[host].eligible &&
                            (VideoCallbacks.capabilities & codecs[codec].recovery);
                    cases++;
                    StreamConfig.encryptionFlags = encrypted ? ENCFLG_ALL : 0;
                    EncryptionFeaturesSupported = encrypted ?
                            SS_ENC_CONTROL_V2 | SS_ENC_VIDEO | SS_ENC_AUDIO : 0;
                    resetGeneratorState();
                    base = getSdpPayloadForStreamConfigBase(14, &baseLength);
                    resetGeneratorState();
                    actual = getSdpPayloadForStreamConfig(14, &actualLength);
                    CHECK(base != NULL && actual != NULL);
                    if (expected) {
                        CHECK(actualLength == baseLength + sizeof(attribute) - 1);
                        CHECK(strlen(actual) == (size_t)actualLength);
                        added = strstr(actual, attribute);
                        CHECK(added != NULL);
                        CHECK(strstr(added + sizeof(attribute) - 1, attribute) == NULL);
                        CHECK(strncmp(added + sizeof(attribute) - 1,
                                      "t=0 0\r\nm=video ", sizeof("t=0 0\r\nm=video ") - 1) == 0);
                        // Removing the one requested attribute must recover the
                        // entire original SDP, including its trailing NUL.
                        memmove(added, added + sizeof(attribute) - 1,
                                (size_t)actualLength - (size_t)(added - actual) -
                                (sizeof(attribute) - 1) + 1);
                    }
                    else {
                        CHECK(actualLength == baseLength);
                    }
                    // Legacy SDP can contain binary attributes, so compare by
                    // its returned length, not strlen().
                    if (memcmp(actual, base, (size_t)baseLength + 1) != 0) {
                        fprintf(stderr, "Expected:\n%.*s\nActual:\n%.*s\n",
                                baseLength, base, baseLength, actual);
                        CHECK(false);
                    }
                    free(base);
                    free(actual);
                }
            }
        }
    }
#ifdef _WIN32
    WSACleanup();
#endif
    printf("PASS: %d SDP cases (host/version, negotiated codec, recovery, encryption)\n", cases);
    return 0;
}
