// Compile the unchanged submodule generator under a private name, then expose
// its normal entry point with this client's optional negotiation extensions.
// Keep this file in the parent repository. Do not also compile SdpGenerator.c
// separately or apply this rename to other common-c translation units.
#define getSdpPayloadForStreamConfig getSdpPayloadForStreamConfigBase
#include "moonlight-common-c/src/SdpGenerator.c"
#undef getSdpPayloadForStreamConfig

#include <limits.h>

char* getSdpPayloadForStreamConfig(int rtspClientVersion, int* length) {
    static const char attribute[] = "a=x-ss-video[0].intraRefresh:1\r\n";
    const size_t attributeLength = sizeof(attribute) - 1;
    char* payload = getSdpPayloadForStreamConfigBase(rtspClientVersion, length);
    char* tail;
    char* expandedPayload;
    size_t insertionOffset;

    // Match VideoDepacketizer's frame-type marker support and use the actual
    // negotiated codec, including fallback from the client's preferred codec.
    // This requests host support; it does not prove that its encoder enables it.
    if (payload == NULL || !IS_SUNSHINE() || !APP_VERSION_AT_LEAST(7, 1, 350) ||
            !isReferenceFrameInvalidationSupportedByDecoder()) {
        return payload;
    }

    // Keep the extension in the generator's attribute block before its tail.
    // RTSP will calculate Content-length and encrypt the resulting payload as
    // usual. No RTSP transport or public common-c API changes are necessary.
    tail = strstr(payload, "\r\nt=0 0\r\nm=video ");
    if (tail == NULL || *length < 0 || attributeLength > (size_t)(INT_MAX - *length)) {
        free(payload);
        return NULL;
    }

    insertionOffset = (size_t)(tail + 2 - payload);
    expandedPayload = realloc(payload, (size_t)*length + attributeLength + 1);
    if (expandedPayload == NULL) {
        free(payload);
        return NULL;
    }

    memmove(expandedPayload + insertionOffset + attributeLength,
            expandedPayload + insertionOffset,
            (size_t)*length - insertionOffset + 1);
    memcpy(expandedPayload + insertionOffset, attribute, attributeLength);
    *length += (int)attributeLength;
    return expandedPayload;
}
