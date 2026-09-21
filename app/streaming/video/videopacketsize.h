#pragma once

#include <cstdint>

// Size of one video packet as it crosses the network, from the IP header to
// the end of the padded FEC shard, for the connection currently established.
// Every packet in the video stream is this size. Only meaningful once
// moonlight-common-c has finished negotiating the connection, which it has
// by the time the decoder is set up.
uint32_t getVideoPacketWireBytes();
