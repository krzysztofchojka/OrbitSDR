#ifndef SDS_H
#define SDS_H
#include <string>
#include "../../common/tetra.h"
#include "../../common/layer.h"
#include "../../common/log.h"
#include "../../common/report.h"
#include "../../common/utils.h"
#include "lip.h"

namespace Tetra {

    class Sds : public Layer {
    public:
        Sds(Log * log, Report * report);
        ~Sds();

        void service(Pdu pdu, const MacLogicalChannel macLogicalChannel, TetraTime tetraTime, MacAddress macAddress);

    private:
        Lip * m_lip;

        void parseDSdsData(Pdu pdu);
        void parseDStatus(Pdu pdu);
        void parseType4Data(Pdu pdu, const uint16_t len);
        void parseSubDTransfer(Pdu pdu, const uint16_t len);
        void parseSimpleTextMessaging(Pdu pdu, const uint16_t len);
        void parseTextMessagingWithSdsTl(Pdu pdu);
        void parseSimpleLocationSystem(Pdu pdu, const uint16_t len);
        void parseLocationSystemWithSdsTl(Pdu pdu);
        void parseLip(Pdu pdu);
    };

};

#endif /* SDS_H */
