#include "wiremsg.h"

using namespace Tetra;

// OrbitSDR port: Wireshark/GSMTAP output disabled — portable no-op.
// See wiremsg.h for why.

WireMsg::WireMsg(int /*port*/) { }
WireMsg::~WireMsg() { }

uint8_t WireMsg::tetraChannelToGsmChannel(const enum MacLogicalChannel /*channel*/) { return 0; }

void WireMsg::sendMsg(const enum MacLogicalChannel /*tetraChannel*/, const struct TetraTime /*tetraTime*/, Pdu /*pdu*/) { }
