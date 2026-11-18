#include "mm.h"

using namespace Tetra;

/**
 * @brief Constructor
 *
 */

Mm::Mm(Log * log, Report * report) : Layer(log, report)
{

}

/**
 * @brief Destructor
 *
 */

Mm::~Mm()
{

}

/**
 * @brief MM service entry point - 16.9
 *
 */

void Mm::service(Pdu pdu, const MacLogicalChannel macLogicalChannel, TetraTime tetraTime, MacAddress macAddress)
{
    Layer::service(macLogicalChannel, tetraTime, macAddress);

    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - mac_channel = %s pdu = %s\n", "service_mm", macLogicalChannelName(macLogicalChannel).c_str(), pdu.toString().c_str());

    std::string txt = pdu.toString();

    uint32_t pos = 0;
    uint8_t pduType = pdu.getValue(pos, 4);

    // PDU type - 16.10.39 (EN 300 392-2) and A.8.59 (EN 300 392-7)

    switch (pduType)
    {
    case 0b0000:
        txt = "D-OTAR";
        parseDOtar(pdu);
        break;

    case 0b0001:
        txt = "D-AUTHENTICATION";
        parseDAuthentication(pdu);
        break;

    case 0b0010:
        parseDCkChangeDemand(pdu);
        break;

    case 0b0011:
        parseDDisable(pdu);
        break;

    case 0b0100:
        parseDEnable(pdu);
        break;

    case 0b0101:
        parseDLocationUpdateAccept(pdu);
        break;

    case 0b0110:
        parseDLocationUpdateCommand(pdu);
        break;

    case 0b0111:
        parseDLocationUpdateReject(pdu);
        break;

    case 0b1001:
        parseDLocationUpdateProceeding(pdu);
        break;

    case 0b1010:
        txt = "D-ATTACH/DETACH GROUP IDENTITY";
        parseDAttachDetachGroupIdentity(pdu);
        break;

    case 0b1011:
        parseDAttachDetachGroupIdentityAck(pdu);
        break;

    case 0b1100:
        parseDMmStatus(pdu);
        break;

    case 0b1111:
        parseMmPduNotSupported(pdu);
        break;

    default:                                                                    // reserved
        break;
    }

    m_log->print(LogLevel::MEDIUM, "serv_mm_sub : TN/FN/MN = %2u/%2u/%2u  %-20s  len=%3lu \n", m_tetraTime.tn, m_tetraTime.fn, m_tetraTime.mn, txt.c_str(), pdu.size());
}


/**
 * @brief MM D-CK CHANGE DEMAND - A.4.1 (EN 300 392-7)
 *
 */

void Mm::parseDCkChangeDemand(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_ck_change_demand", pdu.toString().c_str());

    m_report->start("MM", "D-CK CHANGE DEMAND", m_tetraTime, m_macAddress);

    uint32_t pos = 4;                                                           // pdu type

    m_report->add("Acknowledgement flag", pdu.getValue(pos, 1));
    pos += 1;

    m_report->add("Change of Security Class", pdu.getValue(pos, 2));
    pos += 2;

    uint32_t keyChangeType = pdu.getValue(pos, 3);
    std::string txt = valueToString("Key change type", keyChangeType);
    m_report->add("Key change type", txt);
    pos += 3;

    if (keyChangeType == 0)                                                     // SCK
    {
        uint32_t sckUse = pdu.getValue(pos, 1);
        m_report->add("SCK use", pdu.getValue(pos, 1));
        pos += 1;

        uint32_t numberOfScksChanged = pdu.getValue(pos, 4);
        m_report->add("Number of SCKs changed", pdu.getValue(pos, 4));
        pos += 4;

        if (sckUse == 1 && numberOfScksChanged == 0)                            // DMO
        {
            m_report->add("SCK subset grouping type", pdu.getValue(pos, 4));
            pos += 4;
            m_report->add("SCK subset number", pdu.getValue(pos, 5));
            pos += 5;
            m_report->add("SCK-VN", pdu.getValue(pos, 16));
            pos += 16;
        }

        if (numberOfScksChanged != 0)
        {
            for (uint8_t cnt = 1; cnt <= numberOfScksChanged; cnt++)
            {
                pos = parseSckData(pdu, pos);
            }
        }
    }

    if (keyChangeType == 1 || keyChangeType == 3)                               // CCK or Class 3 CCK and GCK activation
    {
        m_report->add("CCK-id", pdu.getValue(pos, 16));
        pos += 16;
    }

    if (keyChangeType == 2)                                                     // GCK
    {
        uint32_t numberOfGcksChanged = pdu.getValue(pos, 4);
        m_report->add("Number of GCKs changed", pdu.getValue(pos, 4));
        pos += 4;
        for (uint8_t cnt = 1; cnt <= numberOfGcksChanged; cnt++)
        {
            pos = parseGckData(pdu, pos);
        }
    }

    if (keyChangeType == 3 || keyChangeType == 4)                               // All GCKs or Class 3 CCK and GCK activation
    {
        m_report->add("GCK-VN", pdu.getValue(pos, 16));
        pos += 16;
    }

    uint32_t timeType = pdu.getValue(pos, 2);
    txt = valueToString("Time type", timeType);
    m_report->add("Time type", txt);
    pos += 2;

    if (timeType == 0)                                                          // Absolute IV
    {
        m_report->add("Slot number", pdu.getValue(pos, 2));
        pos += 2;

        m_report->add("Frame number", pdu.getValue(pos, 5));
        pos += 5;

        m_report->add("Multiframe number", pdu.getValue(pos, 6));
        pos += 6;

        m_report->add("Hyperframe number", pdu.getValue(pos, 16));
        pos += 16;
    }

    if (timeType == 1)
    {
        m_report->add("Network time", pdu.getValue(pos, 48));
        pos += 48;
    }

    m_report->send();
}


/**
 * @brief MM D-DISABLE - A.6.1 (EN 300 392-7)
 *
 */

void Mm::parseDDisable(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_disable", pdu.toString().c_str());

    m_report->start("MM", "D-DISABLE", m_tetraTime, m_macAddress);

    uint32_t pos = 4;                                                           // pdu type

    m_report->add("Intent/Confirm", pdu.getValue(pos, 1));
    pos += 1;

    m_report->add("Disabling type", pdu.getValue(pos, 1));
    pos += 1;

    bool equipmentDisable = pdu.getValue(pos, 1);
    m_report->add("Equipment disable", pdu.getValue(pos, 1));
    pos += 1;

    if (equipmentDisable)
    {
        m_report->add("TETRA Equipment Identity", pdu.getValue(pos, 60));
        pos += 60;
    }

    bool subscriptionDisable = pdu.getValue(pos, 1);
    m_report->add("Subscription disable", pdu.getValue(pos, 1));
    pos += 1;

    if (subscriptionDisable)
    {
        pos = parseAddressExtension(pdu, pos);
        m_report->add("SSI", pdu.getValue(pos, 24));
        pos += 24;
    }

    bool oBit = pdu.getValue(pos, 1);
    pos += 1;

    if (oBit)
    {
        bool pBit = pdu.getValue(pos, 1);
        pos += 1;

        if (pBit)
        {
            pos = parseAuthenticationChallenge(pdu, pos);
        }
    }

    m_report->send();
}

/**
 * @brief MM D-ENABLE - A.6.2 (EN 300 392-7)
 *
 */

void Mm::parseDEnable(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_enable", pdu.toString().c_str());

    m_report->start("MM", "D-ENABLE", m_tetraTime, m_macAddress);

    uint32_t pos = 4;                                                           // pdu type

    m_report->add("Intent/Confirm", pdu.getValue(pos, 1));
    pos += 1;

    bool equipmentEnable = pdu.getValue(pos, 1);
    m_report->add("Equipment enable", pdu.getValue(pos, 1));
    pos += 1;

    if (equipmentEnable)
    {
        m_report->add("TETRA Equipment Identity", pdu.getValue(pos, 60));
        pos += 60;
    }

    bool subscriptionEnable = pdu.getValue(pos, 1);
    m_report->add("Subscription enable", pdu.getValue(pos, 1));
    pos += 1;

    if (subscriptionEnable)
    {
        pos = parseAddressExtension(pdu, pos);
        m_report->add("SSI", pdu.getValue(pos, 24));
        pos += 24;
    }

    bool oBit = pdu.getValue(pos, 1);
    pos += 1;

    if (oBit)
    {
        bool pBit = pdu.getValue(pos, 1);
        pos += 1;

        if (pBit)
        {
            pos = parseAuthenticationChallenge(pdu, pos);
        }
    }

    m_report->send();
}

/**
 * @brief MM D-LOCATION UPDATE ACCEPT - 16.9.2.7
 *
 */

void Mm::parseDLocationUpdateAccept(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_location_update_accept", pdu.toString().c_str());

    m_report->start("MM", "D-LOCATION UPDATE ACCEPT", m_tetraTime, m_macAddress);

    uint32_t pos = 4;                                                           // pdu type

    std::string txt = valueToString("location update accept type", pdu.getValue(pos, 3));
    m_report->add("Location update accept type", txt);
    pos += 3;

    // type 2 elements (Table E.11)

    bool oBit = pdu.getValue(pos, 1);                                           // o-bit
    pos += 1;
    if (oBit)                                                                   // there are type 2/3/4 elements
    {
        // each type 2 element has a p-bit

        bool pBit = pdu.getValue(pos, 1);                                       // p-bit for ssi element
        pos += 1;
        if (pBit)
        {
            m_report->add("SSI", pdu.getValue(pos, 24));
            pos += 24;
        }

        pBit = pdu.getValue(pos, 1);                                            // p-bit for address extension
        pos += 1;
        if (pBit)
        {
            pos = parseAddressExtension(pdu, pos);
        }

        pBit = pdu.getValue(pos, 1);                                            // p-bit for subscriber class
        pos += 1;
        if (pBit)
        {
            m_report->add("Subscriber class", pdu.getValue(pos, 16));
            pos += 16;
        }

        pBit = pdu.getValue(pos, 1);                                            // p-bit for energy saving information
        pos += 1;
        if (pBit)
        {
            pos = parseEnergySavingInformation(pdu, pos);
        }

        pBit = pdu.getValue(pos, 1);                                            // p-bit for scch information and distribution
        pos += 1;
        if (pBit)
        {
            pos = parseScchInformationAndDistribution(pdu, pos);
        }

        bool mBit = pdu.getValue(pos, 1);                                       // type 3/4 elements

        if (mBit)
        {
            pos = parseType34Elements(pdu, pos);
        }
    }

    m_report->send();
}

/**
 * @brief MM D-LOCATION UPDATE COMMAND - 16.9.2.8
 *
 */

void Mm::parseDLocationUpdateCommand(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_location_update_command", pdu.toString().c_str());

    m_report->start("MM", "D-LOCATION UPDATE COMMAND", m_tetraTime, m_macAddress);

    uint32_t pos = 4;                                                           // pdu type

    m_report->add("Group identity report request", boolToString(pdu.getValue(pos, 1)));
    pos += 1;

    bool cipherControl = pdu.getValue(pos, 1);
    pos += 1;

    if (cipherControl)
    {
        m_report->add("Ciphering", "on");
        pos = parseCipheringParameters(pdu, pos);
    }
    else
    {
        m_report->add("Ciphering", "off");
    }

    bool oBit = pdu.getValue(pos, 1);
    pos += 1;

    if (oBit)
    {
        bool pBit = pdu.getValue(pos, 1);
        pos += 1;

        if (pBit)
        {
            pos = parseAddressExtension(pdu, pos);
        }

        bool mBit = pdu.getValue(pos, 1);

        if (mBit)
        {
            pos = parseType34Elements(pdu, pos);
        }
    }

    m_report->send();
}

/**
 * @brief MM D-LOCATION UPDATE REJECT - 16.9.2.9
 *
 */

void Mm::parseDLocationUpdateReject(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_location_update_reject", pdu.toString().c_str());

    m_report->start("MM", "D-LOCATION UPDATE REJECT", m_tetraTime, m_macAddress);

    uint32_t pos = 4;                                                           // pdu type

    std::string txt = valueToString("Location update type", pdu.getValue(pos, 3));
    m_report->add("Location update type", txt);
    pos += 3;

    std::string rejectCauseTxt = valueToString("Reject cause", pdu.getValue(pos, 5));
    m_report->add("Reject cause", rejectCauseTxt);
    pos += 5;

    bool cipherControl = pdu.getValue(pos, 1);
    pos += 1;

    if (cipherControl)
    {
        m_report->add("Ciphering", "on");
        pos = parseCipheringParameters(pdu, pos);
    }
    else
    {
        m_report->add("Ciphering", "off");
    }

    bool oBit = pdu.getValue(pos, 1);
    pos += 1;

    if (oBit)
    {
        bool pBit = pdu.getValue(pos, 1);
        pos += 1;

        if (pBit)
        {
            pos = parseAddressExtension(pdu, pos);
        }

        bool mBit = pdu.getValue(pos, 1);

        if (mBit)
        {
            pos = parseType34Elements(pdu, pos);
        }
    }

    m_report->send();
}


/**
 * @brief MM D-LOCATION UPDATE PROCEEDING - 16.9.2.10
 *
 */

void Mm::parseDLocationUpdateProceeding(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_location_update_proceeding", pdu.toString().c_str());

    m_report->start("MM", "D-LOCATION UPDATE PROCEEDING", m_tetraTime, m_macAddress);

    uint32_t pos = 4;                                                           // pdu type

    m_report->add("SSI", pdu.getValue(pos, 24));
    pos += 24;

    pos = parseAddressExtension(pdu, pos);

    m_report->send();
}


/**
 * @brief MM D-ATTACH/DETACH GROUP IDENTITY - 16.9.2.1
 *
 */

void Mm::parseDAttachDetachGroupIdentity(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_attach_detach_group_identity", pdu.toString().c_str());

    m_report->start("MM", "D-ATTACH/DETACH GROUP IDENTITY", m_tetraTime, m_macAddress);

    uint32_t pos = 4;                                                           // pdu type

    m_report->add("Group identity report request", boolToString(pdu.getValue(pos, 1)));
    pos += 1;

    m_report->add("Group identity acknowledgement requested", boolToString(pdu.getValue(pos, 1)));
    pos += 1;

    bool groupIdAttachMode = pdu.getValue(pos, 1);
    pos += 1;

    if (groupIdAttachMode)
    {
        m_report->add("Group identity attach/detach mode", "Detach all and attach");
    }
    else
    {
        m_report->add("Group identity attach/detach mode", "Amendment");
    }

    bool oBit = pdu.getValue(pos, 1);                                           // o-bit
    pos += 1;

    if (oBit)                                                                   // there are type 2/3/4 elements
    {
        bool mBit = pdu.getValue(pos, 1);                                       // type 3/4 elements

        if (mBit)
        {
            parseType34Elements(pdu, pos);
        }
    }

    m_report->send();
}

/**
 * @brief MM D-ATTACH/DETACH GROUP IDENTITY ACKNOWLEDGEMENT - 16.9.2.2
 *
 */

void Mm::parseDAttachDetachGroupIdentityAck(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_attach_detach_group_identity_ack", pdu.toString().c_str());

    m_report->start("MM", "D-ATTACH/DETACH GROUP IDENTITY ACKNOWLEDGEMENT", m_tetraTime, m_macAddress);

    uint32_t pos = 4;                                                           // pdu type

    // 16.10.12 Group identity accept/reject
    m_report->add("All attachment/detachments accepted", boolToString(!pdu.getValue(pos, 1)));
    pos += 1;

    // reserved
    pos += 1;

    bool oBit = pdu.getValue(pos, 1);                                           // o-bit
    pos += 1;

    if (oBit)                                                                   // there are type 2/3/4 elements
    {
        bool mBit = pdu.getValue(pos, 1);                                       // type 3/4 elements

        if (mBit)
        {
            parseType34Elements(pdu, pos);
        }
    }

    m_report->send();
}

/**
 * @brief MM PDU/FUNCTION NOT SUPPORTED - 16.9.4.1
 *
 */

void Mm::parseMmPduNotSupported(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_mm_pdu_function_not_supported", pdu.toString().c_str());

    m_report->start("MM", "MM PDU/FUNCTION NOT SUPPORTED", m_tetraTime, m_macAddress);

    uint32_t pos = 4;

    std::string txt = valueToString("PDU type", pdu.getValue(pos, 4));
    m_report->add("Not-supported PDU type", txt);
    pos += 4;

    bool oBit = pdu.getValue(pos, 1);
    pos += 1;

    /* variable data
    if (oBit)
    {
        bool pBit = pdu.getValue(pos, 1);
        pos += 1;

        if (pBit)
        {
            m_report->add("not-supported sub pdu type", pdu.getValue(pos, 4));
            pos += 4;
        }

        pBit = pdu.getValue(pos, 1);
        pos += 1;

        if (pBit)
        {
            m_report->add("Length of the copied PDU", pdu.getValue(pos, 8));
            pos += 8;
        }
    }
    */

    m_report->send();
}

