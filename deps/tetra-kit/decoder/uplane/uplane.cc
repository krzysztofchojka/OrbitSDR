#include "uplane.h"

using namespace Tetra;

/**
 * @brief Constructor
 *
 */

UPlane::UPlane(Log * log, Report * report) : Layer(log, report)
{

}

/**
 * @brief Destructor
 *
 */

UPlane::~UPlane()
{

}

/**
 * @brief User-Plane traffic handling
 *
 */

void UPlane::service(Pdu pdu, const MacLogicalChannel macLogicalChannel, TetraTime tetraTime, MacAddress macAddress, MacState macState, uint8_t encryptionMode)
{
    Layer::service(macLogicalChannel, tetraTime, macAddress);

    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - mac_channel = %s pdu = %s encr = %u\n", "service_u_plane", macLogicalChannelName(macLogicalChannel).c_str(), pdu.toString().c_str(), encryptionMode);

    if (macLogicalChannel == TCH_S)                                             // speech frame
    {
        m_report->startUPlane("UPLANE", "TCH_S", tetraTime, macAddress);

        const std::size_t MIN_SIZE = 432;
        const int FRAME_SIZE = 690;

        if (pdu.size() >= MIN_SIZE)
        {

            uint16_t speechFrame[FRAME_SIZE] = {0};

            for (int idx = 0; idx < 6; idx++)
            {
                speechFrame[115 * idx] = 0x6b21 + (uint16_t)idx;                // FIXME to check (uint16_t)
            }

            for (int idx = 0; idx < 114; idx++)
            {
                speechFrame[1 + idx]  = pdu.at(idx) ? -127 : 127;
            }

            for (int idx = 0; idx < 114; idx++)
            {
                speechFrame[116 + idx] = pdu.at(114 + idx) ? -127 : 127;
            }

            for (int idx = 0; idx < 114; idx++)
            {
                speechFrame[231 + idx] = pdu.at(228 + idx) ? -127 : 127;
            }

            for (int idx = 0; idx < 90; idx++)
            {
                speechFrame[346 + idx] = pdu.at(342 + idx) ? -127 : 127;
            }

            // speech frame will be converted in base64 string

            m_report->add("downlink usage marker", macState.downlinkUsageMarker);                 // current usage marker
            m_report->add("encryption mode",  encryptionMode);                                    // current encryption mode
            m_report->addCompressed("frame", (const unsigned char *)speechFrame, 2 * FRAME_SIZE); // actual binary frame 1380 bytes
        }
        else
        {
            m_report->add("invalid pdu size", (uint64_t)pdu.size());
            m_report->add("pdu minimum size", (uint64_t)MIN_SIZE);
        }

        m_report->send();
    }

}
