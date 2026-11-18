#include "mm.h"

using namespace Tetra;

/**
 * @brief MM D-OTAR sub-type - A.8.58 (EN 300 392-7)
 *
 */

void Mm::parseDOtar(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_otar", pdu.toString().c_str());

    uint32_t pos = 4;                                                           // pdu type
    uint32_t otarSubtype = pdu.getValue(pos, 4);

    // Table A.85 (downlink only)

    switch (otarSubtype)
    {
    case 0b0000:
        parseDOtarCckProvide(pdu);
        break;

    case 0b0001:
        //parseDOtarCckReject(pdu);                                             // couldn't find documentation
        break;

    case 0b0010:
        parseDOtarSckProvide(pdu);
        break;

    case 0b0011:
        parseDOtarSckReject(pdu);
        break;

    case 0b0100:
        parseDOtarGckProvide(pdu);
        break;

    case 0b0101:
        parseDOtarGckReject(pdu);
        break;

    case 0b0110:
        parseDOtarKeyAssociateDemand(pdu);
        break;

    case 0b0111:
        parseDOtarNewcell(pdu);
        break;

    case 0b1000:
        parseDOtarGskoProvide(pdu);
        break;

    case 0b1001:
        parseDOtarGskoReject(pdu);
        break;

    case 0b1010:
        parseDOtarKeyDeleteDemand(pdu);
        break;

    case 0b1011:
        parseDOtarKeyStatusDemand(pdu);
        break;

    case 0b1100:
        parseDOtarCmgGtsiProvide(pdu);
        break;

    case 0b1101:
        parseDOtarDmSckActivate(pdu);
        break;

    default:                                                                    // reserved
        break;
    }
}

/**
 * @brief MM D-OTAR CCK Provide - A.2.1 (EN 300 392-7)
 *
 */

void Mm::parseDOtarCckProvide(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_otar_cck_provide", pdu.toString().c_str());

    m_report->start("MM", "D-OTAR CCK Provide", m_tetraTime, m_macAddress);

    uint32_t pos = 8;                                                           // pdu type
    uint32_t cckProvisionFlag = pdu.getValue(pos, 1);
    pos += 1;

    if (cckProvisionFlag)
    {
        pos = parseCckInformation(pdu, pos);
    }

    m_report->send();
}

/**
 * @brief MM D-OTAR SCK Provide - A.2.7 (EN 300 392-7)
 *
 */

void Mm::parseDOtarSckProvide(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_otar_sck_provide", pdu.toString().c_str());

    m_report->start("MM", "D-OTAR SCK Provide", m_tetraTime, m_macAddress);

    uint32_t pos = 8;                                                           // pdu type
    bool acknowledgementFlag = pdu.getValue(pos, 1);
    m_report->add("Acknowledgement required", boolToString(acknowledgementFlag));
    pos += 1;

    if (acknowledgementFlag)
    {
        m_report->add("Explicit response", boolToString(pdu.getValue(pos, 1)));
    }
    else
    {
        m_report->add("Reserved", pdu.getValue(pos, 1));
    }
    pos += 1;

    m_report->add("Max response timer value", pdu.getValue(pos, 16));
    pos += 16;

    uint32_t sessionKey = pdu.getValue(pos, 1);
    pos += 1;

    if (sessionKey)                                                             // encrypted with group encryption session key
    {
        m_report->add("GSKO-VN", pdu.getValue(pos, 16));
        pos += 16;
    }
    else                                                                        // encrypted with individual encryption session key
    {
        m_report->add("Random seed for OTAR", pdu.getValue(pos, 80));
        pos += 80;
    }

    uint32_t numberOfScks = pdu.getValue(pos, 3);
    m_report->add("Number of SCKs provided", pdu.getValue(pos, 3));
    pos += 3;

    for (uint8_t cnt = 1; cnt <= numberOfScks; cnt++)
    {
        pos = parseSckKeyAndId(pdu, pos);
    }

    m_report->add("KSG number", pdu.getValue(pos, 4));
    pos += 4;

    m_report->add("OTAR Retry Interval", pdu.getValue(pos, 3));
    pos += 3;

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
    }

    m_report->send();
}

/**
 * @brief MM D-OTAR SCK Reject - A.2.9a (EN 300 392-7)
 *
 */

void Mm::parseDOtarSckReject(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_otar_sck_reject", pdu.toString().c_str());

    m_report->start("MM", "D-OTAR SCK Reject", m_tetraTime, m_macAddress);

    uint32_t pos = 8;                                                           // pdu type
    uint32_t numberOfScksRejected = pdu.getValue(pos, 3);
    m_report->add("Number of SCKs rejected", pdu.getValue(pos, 3));
    pos += 3;

    for (uint8_t cnt = 1; cnt <= numberOfScksRejected; cnt++)
    {
        pos = parseSckRejected(pdu, pos);
    }

    m_report->add("OTAR Retry Interval", pdu.getValue(pos, 3));
    pos += 3;

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
    }

    m_report->send();
}

/**
 * @brief MM D-OTAR GCK Provide - A.2.4 (EN 300 392-7)
 *
 */

void Mm::parseDOtarGckProvide(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_otar_gck_provide", pdu.toString().c_str());

    m_report->start("MM", "D-OTAR GCK Provide", m_tetraTime, m_macAddress);

    uint32_t pos = 8;                                                           // pdu type
    bool acknowledgementFlag = pdu.getValue(pos, 1);
    pos += 1;

    if (acknowledgementFlag)
    {
        m_report->add("Explicit response", boolToString(pdu.getValue(pos, 1)));
    }
    else
    {
        // reserved
    }
    pos += 1;

    m_report->add("Max response timer value", pdu.getValue(pos, 16));
    pos += 16;

    uint32_t sessionKey = pdu.getValue(pos, 1);
    pos += 1;

    if (sessionKey)                                                             // encrypted with group encryption session key
    {
        m_report->add("GSKO-VN", pdu.getValue(pos, 16));
        pos += 16;
    }
    else                                                                        // encrypted with individual encryption session key
    {
        m_report->add("Random seed for OTAR", pdu.getValue(pos, 80));
        pos += 80;
    }

    uint32_t numberOfGcks = pdu.getValue(pos, 3);
    m_report->add("Number of GCKs provided", pdu.getValue(pos, 3));
    pos += 3;

    for (uint8_t cnt = 1; cnt <= numberOfGcks; cnt++)
    {
        pos = parseGckKeyAndId(pdu, pos);
    }

    m_report->add("KSG number", pdu.getValue(pos, 4));
    pos += 4;

    uint8_t groupAssociation = pdu.getValue(pos, 1);
    pos += 1;

    if (groupAssociation)
    {
        m_report->add("GSSI", pdu.getValue(pos, 24));
        pos += 24;
    }
    else
    {
        m_report->add("Group association", "Associated with GCKN");
    }

    m_report->add("OTAR retry interval", pdu.getValue(pos, 3));
    pos += 3;

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
    }

    m_report->send();
}

/**
 * @brief MM D-OTAR GCK Reject - A.2.6a (EN 300 392-7)
 *
 */

void Mm::parseDOtarGckReject(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_otar_gck_reject", pdu.toString().c_str());

    m_report->start("MM", "D-OTAR GCK Reject", m_tetraTime, m_macAddress);

    uint32_t pos = 8;                                                           // pdu type
    uint32_t numberOfGcksRejected = pdu.getValue(pos, 3);
    m_report->add("Number of GCKs rejected", pdu.getValue(pos, 3));
    pos += 3;

    for (uint8_t cnt = 1; cnt <= numberOfGcksRejected; cnt++)
    {
        pos = parseGckRejected(pdu, pos);
    }

    m_report->add("OTAR Retry Interval", pdu.getValue(pos, 3));
    pos += 3;

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
    }

    m_report->send();
}

/**
 * @brief MM D-OTAR KEY ASSOCIATE demand - A.3.1 (EN 300 392-7)
 *
 */

void Mm::parseDOtarKeyAssociateDemand(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_otar_key_associate_demand", pdu.toString().c_str());

    m_report->start("MM", "D-OTAR KEY ASSOCIATE demand", m_tetraTime, m_macAddress);

    uint32_t pos = 8;                                                           // pdu type
    uint32_t acknowledgementFlag = pdu.getValue(pos, 1);
    pos += 1;

    if (acknowledgementFlag)
    {
        m_report->add("Explicit response", boolToString(pdu.getValue(pos, 1)));
    }
    pos += 1;

    m_report->add("Max response timer value", pdu.getValue(pos, 16));
    pos += 16;

    uint8_t keyAssociationType = pdu.getValue(pos, 1);
    pos += 1;

    if (keyAssociationType)                                                     // gck
    {
        m_report->add("GCK select number", pdu.getValue(pos, 17));
        pos += 17;
    }
    else                                                                        // sck
    {
        m_report->add("SCK select number", pdu.getValue(pos, 6));
        pos += 6;
        m_report->add("SCK subset grouping type", pdu.getValue(pos, 4));
        pos += 4;
    }

    uint32_t numberOfGroups = pdu.getValue(pos, 5);
    m_report->add("Number of groups", pdu.getValue(pos, 5));
    pos += 5;

    for (uint8_t cnt = 1; cnt <= numberOfGroups; cnt++)
    {
        m_report->add("GSSI", pdu.getValue(pos, 24));
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
            pos = parseAddressExtension(pdu, pos);
        }
    }

    m_report->send();
}

/**
 * @brief MM D-OTAR NEWCELL - A.5.3 (EN 300 392-7)
 *
 */

void Mm::parseDOtarNewcell(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_otar_newcell", pdu.toString().c_str());

    m_report->start("MM", "D-OTAR NEWCELL", m_tetraTime, m_macAddress);

    uint32_t pos = 8;                                                           // pdu type
    m_report->add("DCK forwarding successful", boolToString(pdu.getValue(pos, 1)));
    pos += 1;

    bool cckProvisionFlag = pdu.getValue(pos, 1);
    pos += 1;

    if (cckProvisionFlag)
    {
        pos = parseCckInformation(pdu, pos);
    }

    bool mBit = pdu.getValue(pos, 1);

    if (mBit)
    {
        pos = parseType34Elements(pdu, pos);
    }

    m_report->send();
}

/**
 * @brief MM D-OTAR GSKO Provide - A.2.10 (EN 300 392-7)
 *
 */

void Mm::parseDOtarGskoProvide(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_otar_gsko_provide", pdu.toString().c_str());

    m_report->start("MM", "D-OTAR GSKO Provide", m_tetraTime, m_macAddress);

    uint32_t pos = 8;                                                           // pdu type
    m_report->add("Random seed for OTAR", pdu.getValue(pos, 80));
    pos += 80;

    m_report->add("GSKO-VN", pdu.getValue(pos, 16));
    pos += 16;

    m_report->add("Sealed GSKO", pdu.getValue(pos, 120));
    pos += 120;

    m_report->add("GSSI", pdu.getValue(pos, 24));
    pos += 24;

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
    }

    m_report->send();
}

/**
 * @brief MM D-OTAR GSKO Reject - A.2.12a (EN 300 392-7)
 *
 */

void Mm::parseDOtarGskoReject(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_otar_gsko_reject", pdu.toString().c_str());

    m_report->start("MM", "D-OTAR GSKO Reject", m_tetraTime, m_macAddress);

    uint32_t pos = 8;                                                           // pdu type
    m_report->add("OTAR reject reason", pdu.getValue(pos, 3));
    pos += 3;

    m_report->add("GSSI", pdu.getValue(pos, 24));
    pos += 24;

    m_report->add("OTAR Retry Interval", pdu.getValue(pos, 3));
    pos += 3;

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
    }

    m_report->send();
}

/**
 * @brief MM D-OTAR KEY DELETE demand - A.4a.1 (EN 300 392-7)
 *
 */

void Mm::parseDOtarKeyDeleteDemand(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_otar_key_delete_demand", pdu.toString().c_str());

    m_report->start("MM", "D-OTAR KEY DELETE demand", m_tetraTime, m_macAddress);

    uint32_t pos = 8;                                                           // pdu type
    uint32_t keyDeleteType = pdu.getValue(pos, 3);
    m_report->add("Key delete type", pdu.getValue(pos, 3));
    pos += 3;

    if (keyDeleteType == 0 or keyDeleteType == 1)
    {
        uint32_t numberOfScksDeleted = pdu.getValue(pos, 5);
        m_report->add("Number of SCKs deleted", pdu.getValue(pos, 5));
        pos += 5;

        for (uint8_t cnt = 1; cnt <= numberOfScksDeleted; cnt++)
        {
            m_report->add("SCKN", pdu.getValue(pos, 5));
            pos += 5;
        }
    }
    if (keyDeleteType == 2)
    {
        m_report->add("SCK subset grouping type", pdu.getValue(pos, 4));
        pos += 4;
        m_report->add("SCK subset number", pdu.getValue(pos, 5));
        pos += 5;
    }
    if (keyDeleteType == 3)
    {
        uint32_t numberOfGcksDeleted = pdu.getValue(pos, 4);
        m_report->add("Number of GCKs deleted", pdu.getValue(pos, 4));
        pos += 4;

        for (uint8_t cnt = 1; cnt <= numberOfGcksDeleted; cnt++)
        {
            m_report->add("GCKN", pdu.getValue(pos, 16));
            pos += 16;
        }
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
    }

    m_report->send();
}

/**
 * @brief MM D-OTAR KEY STATUS demand - A.4b.1 (EN 300 392-7)
 *
 */

void Mm::parseDOtarKeyStatusDemand(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_otar_key_status_demand", pdu.toString().c_str());

    m_report->start("MM", "D-OTAR KEY STATUS demand", m_tetraTime, m_macAddress);

    uint32_t pos = 8;                                                           // pdu type
    bool acknowledgementFlag = pdu.getValue(pos, 1);
    pos += 1;

    if (acknowledgementFlag)
    {
        m_report->add("Explicit response", boolToString(pdu.getValue(pos, 1)));
    }
    pos += 1;

    m_report->add("Max response timer value", pdu.getValue(pos, 16));
    pos += 16;

    uint32_t keyStatusType = pdu.getValue(pos, 3);
    m_report->add("Key status type", pdu.getValue(pos, 3));
    pos += 3;

    if (keyStatusType == 0)
    {
        m_report->add("SCKN", pdu.getValue(pos, 5));
        pos += 5;
    }
    if (keyStatusType == 1)
    {
        m_report->add("SCK subset grouping type", pdu.getValue(pos, 4));
        pos += 4;
        m_report->add("SCK subset number", pdu.getValue(pos, 5));
        pos += 5;
    }
    if (keyStatusType == 3)
    {
        m_report->add("GCKN", pdu.getValue(pos, 16));
        pos += 16;
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
    }

    m_report->send();
}

/**
 * @brief MM D-OTAR CMG GTSI PROVIDE - A.5.4 (EN 300 392-7)
 *
 */

void Mm::parseDOtarCmgGtsiProvide(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_otar_cmg_gtsi_provide", pdu.toString().c_str());

    m_report->start("MM", "D-OTAR CMG GTSI PROVIDE", m_tetraTime, m_macAddress);

    uint32_t pos = 8;
    m_report->add("GSSI", pdu.getValue(pos, 24));
    pos += 24;

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
    }

    m_report->send();
}

/**
 * @brief MM D-DM-SCK ACTIVATE DEMAND - A.5.3 (EN 300 392-7)
 *
 */

void Mm::parseDOtarDmSckActivate(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_d_otar_dm_sck_activate", pdu.toString().c_str());

    m_report->start("MM", "D-DM-SCK ACTIVATE DEMAND", m_tetraTime, m_macAddress);

    uint32_t pos = 8;                                                           // pdu type
    m_report->add("Acknowledgement required", boolToString(pdu.getValue(pos, 1)));
    pos += 1;

    uint32_t numberOfScksChanged = pdu.getValue(pos, 4);
    m_report->add("Number of SCKs changed", pdu.getValue(pos, 4));
    pos += 4;

    if (numberOfScksChanged == 0)
    {
        m_report->add("SCK subset grouping type", pdu.getValue(pos, 4));
        pos += 4;
        m_report->add("SCK subset number", pdu.getValue(pos, 5));
        pos += 5;
        m_report->add("SCK-VN", pdu.getValue(pos, 16));
        pos += 16;
    }
    else
    {
        for (uint8_t cnt = 1; cnt <= numberOfScksChanged; cnt++)
        {
            pos = parseSckData(pdu, pos);
        }
    }

    uint8_t timeType = pdu.getValue(pos, 2);
    std::string txt = valueToString("Time type", timeType);
    m_report->add("Time type", txt);
    pos += 2;

    if (timeType == 0)                                                          // absolute IV
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
    if (timeType == 1)                                                          // network time
    {
        m_report->add("Network time", pdu.getValue(pos, 48));
        pos += 48;
    }

    pos = parseAddressExtension(pdu, pos);

    m_report->send();
}
