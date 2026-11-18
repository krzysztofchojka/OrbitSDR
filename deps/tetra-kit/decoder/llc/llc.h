#ifndef LLC_H
#define LLC_H
#include "../common/tetra.h"
#include "../common/layer.h"
#include "../common/log.h"
#include "../common/report.h"
#include "../mle/mle.h"

namespace Tetra {

    class Llc : public Layer {
    public:
        Llc(Log * log, Report * report, Mle * mle);
        ~Llc();

        void service(Pdu pdu, const MacLogicalChannel macLogicalChannel, TetraTime tetraTime, MacAddress macAddress);

    private:
        Mle * m_mle;
    };

};

#endif /* LLC_H */
