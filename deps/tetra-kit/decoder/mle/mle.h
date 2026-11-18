#ifndef MLE_H
#define MLE_H
#include <sstream>
#include "../common/tetra.h"
#include "../common/layer.h"
#include "../common/log.h"
#include "../common/report.h"
#include "../common/utils.h"
#include "../cmce/cmce.h"
#include "../mm/mm.h"
#include "../sndcp/sndcp.h"

namespace Tetra {

    class Mle : public Layer {
    public:
        Mle(Log * log, Report * report, Layer * cmce, Layer * mm, Layer * sndcp);
        ~Mle();

        void service(Pdu pdu, const MacLogicalChannel macLogicalChannel, TetraTime tetraTime, MacAddress macAddress);

    private:
        Layer * m_cmce;
        Layer * m_mm;
        Layer * m_sndcp;

        void serviceMleSubsystem(Pdu pdu, MacLogicalChannel macLogicalChannel);
        void processDNwrkBroadcast(Pdu pdu);
        void processDNwrkBroadcastExtension(Pdu pdu);

        uint64_t parseBsServiceDetails(Pdu pdu, uint64_t pos, std::vector<std::tuple<std::string, uint64_t>> & infos);
        uint64_t parseCellReselectParameters(Pdu pdu, uint64_t pos);
        uint64_t parseMainCarrierNumberExtension(Pdu pdu, uint64_t pos, std::vector<std::tuple<std::string, uint64_t>> & infos);
        uint64_t parseNeighbourCellBroadcast(Pdu pdu, uint64_t pos, std::vector<std::tuple<std::string, uint64_t>> & infos);
        uint32_t parseNeighbourCellInformation(Pdu data, uint32_t posStart, std::vector<std::tuple<std::string, uint64_t>> & infos);
        uint64_t parseTetraNetworkTime(Pdu pdu, uint64_t pos);
        uint64_t parseTimeshareOrSecurity(Pdu pdu, uint64_t pos, std::vector<std::tuple<std::string, uint64_t>> & elements);
        uint64_t parseSecurityParameters(Pdu pdu, uint64_t pos, std::vector<std::tuple<std::string, uint64_t>> & elements);
    };

};

#endif /* MLE_H */
