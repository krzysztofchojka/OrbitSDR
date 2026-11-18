#ifndef WIREMSG_H
#define WIREMSG_H
#include "../common/tetra.h"
#include "../common/pdu.h"

#include <cstdio>
#include <cstdint>

// =====================================================================
//  OrbitSDR port: Wireshark/GSMTAP output is disabled. Upstream this
//  class sent each burst as a GSMTAP packet over a UDP socket. OrbitSDR
//  never enables it (bEnableWiresharkOutput = false, so WireMsg is never
//  instantiated), so this is a portable no-op that keeps the class
//  interface intact and drops the POSIX socket dependency.
// =====================================================================

namespace Tetra {

    class WireMsg {
    public:
        WireMsg(int port = 4729);
        ~WireMsg();

        uint8_t tetraChannelToGsmChannel(const enum MacLogicalChannel channel);
        void sendMsg(const enum MacLogicalChannel tetraChannel, const struct TetraTime tetraTime, Pdu pdu);
    };

};

#endif /* WIREMSG_H */
