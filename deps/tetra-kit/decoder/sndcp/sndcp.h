#ifndef SNDCP_H
#define SNDCP_H
#include <sstream>
#include "../common/tetra.h"
#include "../common/layer.h"
#include "../common/log.h"
#include "../common/report.h"
#include "../common/utils.h"

namespace Tetra {

    class Sndcp : public Layer {
    public:
        Sndcp(Log * log, Report * report);
        ~Sndcp();

        void service(Pdu pdu, const MacLogicalChannel macLogicalChannel, TetraTime tetraTime, MacAddress macAddress);
    };

};

#endif /* SNDCP_H */
