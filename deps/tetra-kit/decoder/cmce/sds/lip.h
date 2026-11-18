#ifndef LIP_H
#define LIP_H
#include <string>
#include <cmath>
#include "../../common/tetra.h"
#include "../../common/layer.h"
#include "../../common/log.h"
#include "../../common/report.h"
#include "../../common/utils.h"

namespace Tetra {

    class Lip : public Layer {
    public:
        Lip(Log * log, Report * report);
        ~Lip();

        void service(Pdu pdu, const MacLogicalChannel macLogicalChannel, TetraTime tetraTime, MacAddress macAddress);

    private:
        void parseShortLocationReport(Pdu pdu);
        void parseExtendedMessage(Pdu pdu);

        double decodeLipLatitude(uint32_t latitude);
        double decodeLipLongitude(uint32_t longitude);
        double decodeLipHorizontalVelocity(uint8_t val);
        std::string decodeLipDirectionOfTravel(uint8_t val);
        std::string decodeLipPositionError(uint8_t val);
    };

};

#endif /* LIP_H */
