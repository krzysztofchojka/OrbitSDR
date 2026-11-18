#ifndef CMCE_H
#define CMCE_H
#include <sstream>
#include "../common/tetra.h"
#include "../common/layer.h"
#include "../common/log.h"
#include "../common/report.h"
#include "../common/utils.h"
#include "sds/sds.h"

namespace Tetra {

    class Cmce : public Layer {
    public:
        Cmce(Log * log, Report * report, Sds * sds);
        ~Cmce();

        void service(Pdu pdu, const MacLogicalChannel macLogicalChannel, TetraTime tetraTime, MacAddress macAddress);

    private:
        Sds * m_sds;

        void parseDAlert(Pdu pdu);
        void parseDCallProceeding(Pdu pdu);
        void parseDCallRestore(Pdu pdu);
        void parseDConnect(Pdu pdu);
        void parseDConnectAck(Pdu pdu);
        void parseDDisconnect(Pdu pdu);
        void parseDInfo(Pdu pdu);
        void parseDRelease(Pdu pdu);
        void parseDSetup(Pdu pdu);
        void parseDTxCeased(Pdu pdu);
        void parseDTxContinue(Pdu pdu);
        void parseDTxGranted(Pdu pdu);
        void parseDTxInterrupt(Pdu pdu);
        void parseDTxWait(Pdu pdu);
    };

};

#endif /* CMCE_H */
