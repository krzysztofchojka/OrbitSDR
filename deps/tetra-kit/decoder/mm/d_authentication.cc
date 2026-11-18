#include "mm.h"

using namespace Tetra;

/**
 * @brief MM D-AUTHENTICATION - A.1 (EN 300 392-7)
 *
 */

void Mm::parseDAuthentication(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_authentication", pdu.toString().c_str());

    uint32_t pos = 0;
    uint8_t authenticationSubType = pdu.getValue(pos, 2);

    // A.8.6 (EN 300 392-7)

    switch (authenticationSubType)
    {
    case 0b00:
        parseDAuthenticationDemand(pdu);
        break;

    case 0b01:
        parseDAuthenticationResponse(pdu);
        break;

    case 0b10:
        parseDAuthenticationResult(pdu);
        break;

    case 0b11:
        parseDAuthenticationReject(pdu);
        break;

    default:                                                                    // reserved
        break;
    }

}

/**
 * @brief MM D-AUTHENTICATION demand - A.1.1 (EN 300 392-7)
 *
 */

void Mm::parseDAuthenticationDemand(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_authentication_demand", pdu.toString().c_str());

    m_report->start("MM", "D-AUTHENTICATION DEMAND", m_tetraTime, m_macAddress);

    uint32_t pos = 6;                                                           // pdu type

    m_report->add("Random challenge", pdu.getValue(pos, 80));
    pos += 80;

    m_report->add("Random seed", pdu.getValue(pos, 80));
    pos += 80;

    m_report->send();
}

/**
 * @brief MM D-AUTHENTICATION response - A.1.3 (EN 300 392-7)
 *
 */

void Mm::parseDAuthenticationResponse(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_authentication_response", pdu.toString().c_str());

    m_report->start("MM", "D-AUTHENTICATION RESPONSE", m_tetraTime, m_macAddress);

    uint32_t pos = 6;                                                           // pdu type

    m_report->add("Random seed", pdu.getValue(pos, 80));
    pos += 80;

    m_report->add("Response value", pdu.getValue(pos, 32));
    pos += 32;

    uint8_t authFlag = pdu.getValue(pos, 1);
    pos += 1;

    if (authFlag)
    {
        m_report->add("Random challenge", pdu.getValue(pos, 80));
        pos += 80;
    }

    m_report->send();
}

/**
 * @brief MM D-AUTHENTICATION result - A.1.4 (EN 300 392-7)
 *
 */

void Mm::parseDAuthenticationResult(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_authentication_result", pdu.toString().c_str());

    m_report->start("MM", "D-AUTHENTICATION RESULT", m_tetraTime, m_macAddress);

    uint32_t pos = 6;                                                           // pdu type

    m_report->add("Authentication successful", boolToString(pdu.getValue(pos, 1)));
    pos += 1;

    bool authFlag = pdu.getValue(pos, 1);
    pos += 1;

    if (authFlag)
    {
        m_report->add("Response value", pdu.getValue(pos, 32));
        pos += 32;
    }

    m_report->send();
}

/**
 * @brief MM D-AUTHENTICATION reject - A.1.2 (EN 300 392-7)
 *
 */

void Mm::parseDAuthenticationReject(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_authentication_reject", pdu.toString().c_str());

    m_report->start("MM", "D-AUTHENTICATION REJECT", m_tetraTime, m_macAddress);

    uint32_t pos = 6;                                                           // pdu type

    uint8_t authRejectReason = pdu.getValue(pos, 3);
    pos += 3;

    if (!authRejectReason)
    {
        m_report->add("Authentication reject reason", "Authentication not supported");
    }
    else
    {
        m_report->add("Authentication reject reason", authRejectReason);
    }

    m_report->send();
}
