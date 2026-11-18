#include "sndcp.h"

using namespace Tetra;

/**
 * @brief Constructor
 *
 */

Sndcp::Sndcp(Log * log, Report * report) : Layer(log, report)
{

}

/**
 * @brief Destructor
 *
 */

Sndcp::~Sndcp()
{

}

/**
 * @brief SNDCP service entry point - see 28.4
 *
 */

void Sndcp::service(Pdu pdu, const MacLogicalChannel macLogicalChannel, TetraTime tetraTime, MacAddress macAddress)
{
    Layer::service(macLogicalChannel, tetraTime, macAddress);

    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - mac_channel = %s pdu = %s\n", "service_sndcp", macLogicalChannelName(macLogicalChannel).c_str(), pdu.toString().c_str());

    m_report->start("SNDCP", "RAW-DATA", tetraTime, macAddress);
    m_report->add("data", pdu);
    m_report->send();
}
