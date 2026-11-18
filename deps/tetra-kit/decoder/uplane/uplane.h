#ifndef UPLANE_H
#define UPLANE_H
#include "../common/tetra.h"
#include "../common/layer.h"
#include "../common/log.h"
#include "../common/report.h"
#include "../common/utils.h"

namespace Tetra {

    class UPlane : public Layer {
    public:
        UPlane(Log * log, Report * report);
        ~UPlane();

        void service(Pdu pdu, const MacLogicalChannel macLogicalChannel, TetraTime tetraTime, MacAddress macAddress, MacState macState, uint8_t encryptionMode);
    };

};

#endif /* UPLANE_H */
