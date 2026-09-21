// This file reads sockaddr_storage, so it needs the socket headers. On Windows
// those have to come before anything that pulls in windows.h, which is why
// this lives on its own rather than in ffmpeg.cpp.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#include "videopacketsize.h"

#include <Limelight.h>

// The packet size and encryption actually in use are only settled once
// moonlight-common-c has negotiated with the host: it caps the packet size for
// remote streaming and turns video encryption on or off to match the host.
// None of this is exposed through its public API, so read the negotiated state
// straight from its globals, declared in its Limelight-internal.h. It's linked
// in statically, so a rename there fails the link rather than quietly skewing
// the graph.
extern "C" {
extern STREAM_CONFIGURATION StreamConfig;
extern uint32_t EncryptionFeaturesEnabled;
extern struct sockaddr_storage RemoteAddr;
}

namespace {

// Mirrors of moonlight-common-c's Video.h and Limelight-internal.h
const uint32_t k_MaxRtpHeaderSize = 16;       // MAX_RTP_HEADER_SIZE
const uint32_t k_EncVideoHeaderSize = 32;     // sizeof(ENC_VIDEO_HEADER)
const uint32_t k_EncVideoFeatureFlag = 0x02;  // SS_ENC_VIDEO

const uint32_t k_UdpHeaderSize = 8;
const uint32_t k_Ipv4HeaderSize = 20;
const uint32_t k_Ipv6HeaderSize = 40;

}

uint32_t getVideoPacketWireBytes()
{
    // The host pads every shard of a block to the full packet size so FEC can
    // run over them, then adds the RTP and NV video headers, then the
    // encryption header if video is encrypted. See VideoStream.c, which sizes
    // its receive buffer the same way.
    uint32_t bytes = (uint32_t)StreamConfig.packetSize + k_MaxRtpHeaderSize;
    if (EncryptionFeaturesEnabled & k_EncVideoFeatureFlag) {
        bytes += k_EncVideoHeaderSize;
    }

    // NAT64 addresses count as IPv6, which is right: the client's end of that
    // path carries IPv6 headers.
    bytes += k_UdpHeaderSize +
             (RemoteAddr.ss_family == AF_INET6 ? k_Ipv6HeaderSize : k_Ipv4HeaderSize);

    return bytes;
}
