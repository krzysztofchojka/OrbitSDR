#include "mm.h"

using namespace Tetra;

/**
 * @brief MM type 3/4 elements - E.1.1 (see E.1.3 for example)
 *
 */

uint64_t Mm::parseType34Elements(Pdu pdu, uint64_t pos)
{
    while (pdu.getValue(pos, 1))                                                // repeat for all type 3/4 elements
    {
        pos += 1;
        uint64_t elementId = pdu.getValue(pos, 4);                              // type 3/4 element identifier
        uint64_t elementType = 0;

        // 16.10.51 Table 16.89

        switch (elementId)
        {
        case 0b0000:                                                            // Reserved for future extension
            break;
        case 0b0001:                                                            // Default group attachment lifetime
            elementType = 3;                                                    // According to Table 16.11
            break;
        case 0b0010:                                                            // New registered area
            elementType = 4;                                                    // According to Table 16.11
            break;
        case 0b0011:                                                            // Security downlink
            elementType = 3;                                                    // Defined in A.7.3 (EN 300 392-7)
            break;
        case 0b0100:                                                            // Group report response
            elementType = 3;                                                    // According to Table 16.1
            break;
        case 0b0101:                                                            // Group identity location accept
            elementType = 3;                                                    // According to Table 16.11
            break;
        case 0b0110:                                                            // DM-MS address
                                                                                // Used only in DMO
            break;
        case 0b0111:                                                            // Group identity downlink
            elementType = 4;                                                    // According to Table 16.1
            break;
        case 0b1000:                                                            // Group identity uplink
            break;
        case 0b1001:                                                            // Authentication uplink
            break;
        case 0b1010:                                                            // Authentication downlink
            elementType = 3;                                                    // According to Table 16.11
            break;
        case 0b1011:                                                            // Extended capabilities
                                                                                // not used by downlink PDUs
            break;
        case 0b1100:                                                            // Group Identity Security Related Information
            elementType = 4;                                                    // According to Table 16.11
            break;
        case 0b1101:                                                            // Cell type control
            elementType = 3;                                                    // According to Table 16.12
            break;
        case 0b1110:                                                            // reserved
            break;
        case 0b1111:                                                            // Proprietary
            elementType = 3;                                                    // According to Table 16.1
            break;
        }

        std::string txt = valueToString("type 3/4 element identifier", pdu.getValue(pos, 4));
        m_report->add("Type 3/4 element identifier", txt);
        pos += 4;

        m_report->add("Length indicator", pdu.getValue(pos, 11));
        pos += 11;

        // type 4 elements may have repeated elements, whereas type 3 elements won't

        if (elementType == 4)
        {
            uint64_t numberOfElements = pdu.getValue(pos, 6);
            pos += 6;

            for (uint8_t cnt = 1; cnt <= numberOfElements; cnt++)
            {
                switch (elementId)
                {
                case 0b0010:
                    pos = parseNewRegisteredArea(pdu, pos);
                    break;
                case 0b0111:
                    pos = parseGroupIdentityDownlink(pdu, pos);
                    break;
                case 0b1100:
                    pos = parseGISRI(pdu, pos);
                    break;
                }
            }
        }

        if (elementType == 3)
        {
            switch (elementId)
            {
            case 0b0001:
                txt = valueToString("Group identity attachment lifetime", pdu.getValue(pos, 2));
                m_report->add("Default group attachment lifetime", txt);
                pos += 2;
                break;
            case 0b0011:
                pos = parseSecurityDownlink(pdu, pos);
                break;
            case 0b0100:
                pos = parseGroupReportResponse(pdu, pos);
                break;
            case 0b0101:
                pos = parseGroupIdentityLocationAccept(pdu, pos);
                break;
            case 0b1010:
                pos = parseAuthenticationDownlink(pdu, pos);
                break;
            case 0b1101:
                pos = parseCellTypeControl(pdu, pos);
                break;
            case 0b1111:
                pos = parseProprietary(pdu, pos);
                break;
            }
        }
    }
    return pos;
}

/**
 * @brief MM Address extension - 16.10.1
 *
 */

uint64_t Mm::parseAddressExtension(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_address_extension", pdu.toString().c_str());

    m_report->add("MCC", pdu.getValue(pos, 10));
    pos += 10;
    m_report->add("MNC", pdu.getValue(pos, 14));
    pos += 14;

    return pos;
}

/**
 * @brief Authentication challenge - A.8.3
 *
 */

uint64_t Mm::parseAuthenticationChallenge(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_authentication_challenge", pdu.toString().c_str());

    m_report->add("Random challenge RAND1", pdu.getValue(pos, 80));
    pos += 80;

    m_report->add("Random seed RS", pdu.getValue(pos, 80));
    pos += 80;

    return pos;
}

/**
 * @brief MM Authentication downlink - A.7.1
 *
 */

uint64_t Mm::parseAuthenticationDownlink(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_authentication_downlink", pdu.toString().c_str());

    std::string authSuccess = pdu.getValue(pos, 1) ? "true" : "false";
    pos += 1;
    m_report->add("Authentication successful", authSuccess);

    std::string supplyTei = pdu.getValue(pos, 1) ? "true" : "false";
    pos += 1;
    m_report->add("Supply TEI", supplyTei);

    bool ckProvisionFlag = pdu.getValue(pos, 1);
    pos += 1;

    if (ckProvisionFlag)
    {
        pos = parseCkProvisioningInformation(pdu, pos);
    }
    return pos;
}

/**
 * @brief CCK information - A.8.8 (EN 300 392-7)
 *
 */

uint64_t Mm::parseCckInformation(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_cck_information", pdu.toString().c_str());

    m_report->add("CCK identifier", pdu.getValue(pos, 16));
    pos += 16;
    m_report->add("Key type flag", pdu.getValue(pos, 1));
    pos += 1;
    m_report->add("Sealed CCK", pdu.getValue(pos, 120));
    pos += 120;

    pos = parseCckLocationAreaInformation(pdu, pos);

    bool futureKeyFlag = pdu.getValue(pos, 1);
    pos += 1;

    if (futureKeyFlag)
    {
        m_report->add("Sealed CCK", pdu.getValue(pos, 120));
        pos += 120;
    }
    return pos;
}

/**
 * @brief CCK Location area information - A.8.9 (EN 300 392-7)
 *
 */

uint64_t Mm::parseCckLocationAreaInformation(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_cck_la_information", pdu.toString().c_str());

    uint8_t type = pdu.getValue(pos, 2);
    std::string txt = valueToString("Type", type);
    m_report->add("Type", txt);
    pos += 2;
    
    switch (type)
    {
        case 0b01:
            pos = parseLocationAreaList(pdu, pos);
            break;
        case 0b10:
            m_report->add("Location area bit mask", pdu.getValue(pos, 14));
            pos += 14;
            m_report->add("Location area selector", pdu.getValue(pos, 14));
            pos += 14;
            break;
        case 0b11:
            pos = parseLocationAreaRange(pdu, pos);
        default:
            break;
    }

    return pos;
}

/**
 * @brief MM Cell type control - 16.10.1a
 *
 */

uint64_t Mm::parseCellTypeControl(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_cell_type_control", pdu.toString().c_str());

    bool revertToUserApplication = pdu.getValue(pos, 1);
    m_report->add("Revert to user application setting", boolToString(revertToUserApplication));
    pos += 1;

    if (!revertToUserApplication)
    {
        uint8_t cellTypeCount = 0;
        pos = parseCellTypeListControl(pdu, pos, cellTypeCount);
        pos += 4;

        for (int i = 0; i < cellTypeCount; i++)
        {
            std::string txt = valueToString("required cell type", pdu.getValue(pos, 3));
            m_report->add("Required cell type", txt);
            pos += 3;
        }

        pos = parseCellTypeListControl(pdu, pos, cellTypeCount);
        pos += 4;

        for (int i = 0; i < cellTypeCount; i++)
        {
            std::string txt = valueToString("required cell type", pdu.getValue(pos, 3));
            m_report->add("Preferred cell type", txt);
            pos += 3;
        }
    }
    return pos;
}

/**
 * @brief MM Required/preferred cell type list control - 16.10.40b / 16.10.43b
 *
 */

uint64_t Mm::parseCellTypeListControl(Pdu pdu, uint64_t pos, uint8_t& cellTypeCount)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_cell_type_list_control", pdu.toString().c_str());

    uint8_t cellTypeListControl = pdu.getValue(pos, 4);
    bool ordered = false;
    pos += 4;

    if (cellTypeListControl <= 0b1000)
    {
        ordered = true;
        cellTypeCount = cellTypeListControl;
    }
    else
    {
        // Table 16.78, unordered list
        cellTypeCount = cellTypeListControl - 8;
    }

    return pos;
}

/**
 * @brief Ciphering parameters - A.8.12 (EN 300 392-7)
 *
 */

uint64_t Mm::parseCipheringParameters(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_ciphering_parameters", pdu.toString().c_str());

    m_report->add("KSG number", pdu.getValue(pos, 4));
    pos += 4;

    bool securityClass = pdu.getValue(pos, 1);

    if (securityClass)                                                          // Class 3
    {
        m_report->add("Security class", "Class 3");
        pos += 1;
        m_report->add("TM-SCK OTAR supported", boolToString(pdu.getValue(pos, 1)));
        pos += 1;
        m_report->add("SDMO and DM-SCK OTAR supported", boolToString(pdu.getValue(pos, 1)));
        pos += 1;
        m_report->add("GCK encryption/OTAR supported", boolToString(pdu.getValue(pos, 1)));
        pos += 1;
        m_report->add("Security information protocol supported", boolToString(pdu.getValue(pos, 1)));
        pos += 1;
        // reserved
        pos += 1;
    }
    else                                                                        // Class 2
    {
        m_report->add("Security class", "Class 2");
        pos += 1;
        m_report->add("SCK number", pdu.getValue(pos, 5));
        pos += 5;
    }

    return pos;
}

/**
 * @brief CK provisioning information - A.8.14 (EN 300 392-7)
 *
 */

uint64_t Mm::parseCkProvisioningInformation(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_ck_provisioning_information", pdu.toString().c_str());

    bool sckProvisionFlag = pdu.getValue(pos, 1);
    pos += 1;

    if (sckProvisionFlag)
    {
        pos = parseSckInformation(pdu, pos);
    }

    bool cckProvisionFlag = pdu.getValue(pos, 1);
    pos += 1;

    if (cckProvisionFlag)
    {
        pos = parseCckInformation(pdu, pos);
    }
    return pos;
}

/**
 * @brief MM Energy saving information - 16.10.10
 *
 */

uint64_t Mm::parseEnergySavingInformation(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_energy_saving_information", pdu.toString().c_str());

    uint8_t energySavingMode = pdu.getValue(pos, 3);

    if (energySavingMode > 0)
    {
        m_report->add("Energy saving mode", "Economy mode " + energySavingMode);
        pos += 3;
        m_report->add("Frame number", pdu.getValue(pos, 5));
        pos += 5;
        m_report->add("Multiframe number", pdu.getValue(pos, 6));
        pos += 6;
    }
    else
    {
        m_report->add("Energy saving mode", "Stay Alive");
        pos += 3;
        // The frame numbers have no meaning if the mode is "Stay Alive"
        pos += 5;
        pos += 6;
    }

    return pos;
}

/**
 * @brief MM GCK data - A.8.26
 *
 */

uint64_t Mm::parseGckData(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_gck_data", pdu.toString().c_str());

    m_report->add("GCK Number", pdu.getValue(pos, 16));
    pos += 16;

    m_report->add("GCK Version number", pdu.getValue(pos, 16));
    pos += 16;

    return pos;
}


/**
 * @brief GCK key and identifier - A.8.27 (EN 300 392-7)
 *
 */

uint64_t Mm::parseGckKeyAndId(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_gck_key_and_id", pdu.toString().c_str());

    m_report->add("GCKN", pdu.getValue(pos, 16));
    pos += 16;

    m_report->add("GCK version number", pdu.getValue(pos, 16));
    pos += 16;

    m_report->add("Sealed key (SGCK)", pdu.getValue(pos, 120));
    pos += 120;

    return pos;
}

/**
 * @brief GCK rejected - A.8.28b (EN 300 392-7)
 *
 */

uint64_t Mm::parseGckRejected(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_gck_rejected", pdu.toString().c_str());

    std::string txt = valueToString("OTAR reject reason", pdu.getValue(pos, 3));
    m_report->add("OTAR reject reason", txt);
    pos += 3;

    uint8_t groupAssociation = pdu.getValue(pos, 1);
    pos += 1;

    if (groupAssociation)                                                       // Associated with specific GSSI
    {
        m_report->add("GSSI", pdu.getValue(pos, 24));
        pos += 24;
    }
    else
    {
        m_report->add("GCKN", pdu.getValue(pos, 16));
        pos += 16;
    }

    return pos;
}

/**
 * @brief MM Group identity attachment - 16.10.19
 *
 */

uint64_t Mm::parseGroupIdentityAttachment(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_group_identity_attachment", pdu.toString().c_str());

    std::string txt = valueToString("Group identity attachment lifetime", pdu.getValue(pos, 2));
    m_report->add("Group identity attachment lifetime", txt);
    pos += 2;

    uint8_t classOfUsage = pdu.getValue(pos, 3) + 1;
    m_report->add("Class of usage", classOfUsage);
    pos += 3;

    return pos;
}

/**
 * @brief MM Group identity downlink - 16.10.22
 *
 */

uint64_t Mm::parseGroupIdentityDownlink(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_group_identity_downlink", pdu.toString().c_str());

    bool attachDetachType = pdu.getValue(pos, 1);
    pos += 1;

    if (attachDetachType)                                                       // detachment
    {
        std::string txt = valueToString("Group identity detachment downlink", pdu.getValue(pos, 2));
        m_report->add("Group identity detachment downlink", txt);
        pos += 2;
    }
    else                                                                        // attachment
    {
        pos = parseGroupIdentityAttachment(pdu, pos);
    }

    uint8_t groupIdentityAddressType = pdu.getValue(pos, 2);
    pos += 2;

    // 16.10.15
    switch (groupIdentityAddressType)
    {
        case 0b00:
            m_report->add("GSSI", pdu.getValue(pos, 24));
            pos += 24;
            break;
        case 0b01:
            m_report->add("GSSI", pdu.getValue(pos, 24));
            pos += 24;
            pos = parseAddressExtension(pdu, pos);
            break;
        case 0b10:
            m_report->add("(V)GSSI", pdu.getValue(pos, 24));
            pos += 24;
            break;
        case 0b11:
            m_report->add("GSSI", pdu.getValue(pos, 24));
            pos += 24;
            pos = parseAddressExtension(pdu, pos);
            m_report->add("(V)GSSI", pdu.getValue(pos, 24));
            pos += 24;
            break;
    }

    return pos;
}

/**
 * @brief MM Group identity location accept - 16.10.23
 *
 */

uint64_t Mm::parseGroupIdentityLocationAccept(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_group_identity_location_accept", pdu.toString().c_str());

    // 16.10.12 Group identity accept/reject
    m_report->add("All attachment/detachments accepted", boolToString(!pdu.getValue(pos, 1)));
    pos += 1;

    // reserved
    pos += 1;

    bool oBit = pdu.getValue(pos, 1);
    pos += 1;

    // Group Identity Downlink is type 4 element
    if (oBit)
    {
        pos = parseType34Elements(pdu, pos);
    }

    return pos;
}

/**
 * @brief Group Identity Security Related Information - A.8.31a (EN 300 392-7)
 *
 */

uint64_t Mm::parseGISRI(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_gisri", pdu.toString().c_str());

    uint64_t numberOfGroups = pdu.getValue(pos, 5);
    m_report->add("Number of groups", pdu.getValue(pos, 5));
    pos += 5;

    for (uint8_t gisri_cnt = 1; gisri_cnt <= numberOfGroups; gisri_cnt++)
    {
        m_report->add("GSSI", pdu.getValue(pos, 24));
        pos += 24;
    }

    bool gckAssociation = pdu.getValue(pos, 1);
    pos += 1;

    if (gckAssociation)                                         // GCK association information provided
    {
        m_report->add("GCK select number", pdu.getValue(pos, 17));
        pos += 17;
    }

    bool sckAssociation = pdu.getValue(pos, 1);
    pos += 1;

    if (sckAssociation)                                         // SCK association information provided
    {
        m_report->add("SCK subset grouping type", pdu.getValue(pos, 4));
        pos += 4;
        m_report->add("SCK subset number", pdu.getValue(pos, 5));
        pos += 5;
    }
    return pos;
}


/**
 * @brief MM Group report response - 16.10.27a
 *
 */

uint64_t Mm::parseGroupReportResponse(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_group_report_response", pdu.toString().c_str());

    m_report->add("group report response", pdu.getValue(pos, 1));
    pos += 1;

    return pos;
}

/**
 * @brief Location area information - A.8.45 (EN 300 392-7)
 *
 */

uint64_t Mm::parseLocationAreaList(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_location_area_list", pdu.toString().c_str());

    uint8_t numberOfLAs = pdu.getValue(pos, 4);
    m_report->add("Number of location areas", pdu.getValue(pos, 4));
    pos += 4;

    for (int i = 0; i < numberOfLAs; i++)
    {
        m_report->add("Location area", pdu.getValue(pos, 14));
        pos += 14;
    }

    return pos;
}

/**
 * @brief Location area range - A.8.46 (EN 300 392-7)
 *
 */

uint64_t Mm::parseLocationAreaRange(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_location_area_range", pdu.toString().c_str());

    m_report->add("LLAV", pdu.getValue(pos, 14));
    pos += 14;

    m_report->add("HLAV", pdu.getValue(pos, 14));
    pos += 14;

    return pos;
}

/**
 * @brief MM New registered area - 16.10.40
 *
 */

uint64_t Mm::parseNewRegisteredArea(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_new_registered_area", pdu.toString().c_str());

    std::string txt = valueToString("LA timer", pdu.getValue(pos, 3));
    m_report->add("LA timer", txt);
    pos += 3;

    m_report->add("LA", pdu.getValue(pos, 14));
    pos += 14;

    bool oBit = pdu.getValue(pos, 1);
    pos += 1;

    if (oBit)
    {
        bool pBit = pdu.getValue(pos, 1);
        pos += 1;

        if (pBit)
        {
            m_report->add("LACC", pdu.getValue(pos, 10));
            pos += 10;
        }

        pBit = pdu.getValue(pos, 1);
        pos += 1;

        if (pBit)
        {
            m_report->add("LANC", pdu.getValue(pos, 14));
            pos += 14;
        }
    }

    return pos;
}


/**
 * @brief Proprietary - 16.10.41 / Annex H.1
 *
 */
uint64_t Mm::parseProprietary(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "parse_proprietary", pdu.toString().c_str());

    uint32_t proprietaryElementOwner = pdu.getValue(pos, 8);
    m_report->add("Proprietary element owner", pdu.getValue(pos, 8));
    pos += 8;

    if (proprietaryElementOwner == 0)
    {
        // proprietary element, cannot be parsed
        /*
        m_report->add("proprietary element owner extension", pdu.getValue(pos, 1));
        pos += 1;
        */
    }

    return pos;
}

/**
 * @brief MM SCCH information and distribution on 18th frame - 16.10.46
 *
 */

uint64_t Mm::parseScchInformationAndDistribution(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_scch_information_and_distribution", pdu.toString().c_str());

    uint8_t scchInformation = pdu.getValue(pos, 4);

    if (scchInformation < 0b1100)
    {
        m_report->add("SCCH information", "MS SCCH allocation " + scchInformation);
    }
    pos += 4;

    uint8_t timeSlot = pdu.getValue(pos, 2) + 1;
    m_report->add("Distribution on 18th frame", "Time slot " + timeSlot);
    pos += 2;

    return pos;
}

/**
 * @brief MM SCK data - A.8.67
 *
 */

uint64_t Mm::parseSckData(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_sck_data", pdu.toString().c_str());

    m_report->add("SCK Number", pdu.getValue(pos, 5));
    pos += 5;

    m_report->add("SCK Version number", pdu.getValue(pos, 16));
    pos += 16;

    return pos;
}

/**
 * @brief SCK information - A.8.68 (EN 300 392-7)
 *
 */

uint64_t Mm::parseSckInformation(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_sck_information", pdu.toString().c_str());

    bool sessionKey = pdu.getValue(pos, 1);
    pos += 1;

    if (sessionKey)                                                     // session key for group
    {
        m_report->add("GSKO-VN", pdu.getValue(pos, 16));
        pos += 16;
    }
    else                                                                // session key for individual
    {
        m_report->add("Random seed for OTAR", pdu.getValue(pos, 80));
        pos += 80;
    }

    m_report->add("SCK number", pdu.getValue(pos, 5));
    pos += 5;
    m_report->add("SCK version number", pdu.getValue(pos, 16));
    pos += 16;
    m_report->add("Sealed SCK", pdu.getValue(pos, 120));
    pos += 120;

    bool futureKeyFlag = pdu.getValue(pos, 1);
    pos += 1;

    if (futureKeyFlag)
    {
        m_report->add("SCK number", pdu.getValue(pos, 5));
        pos += 5;
        m_report->add("SCK version number", pdu.getValue(pos, 16));
        pos += 16;
        m_report->add("Sealed SCK", pdu.getValue(pos, 120));
        pos += 120;
    }
    return pos;
}

/**
 * @brief SCK key and identifier - A.8.69 (EN 300 392-7)
 *
 */

uint64_t Mm::parseSckKeyAndId(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_sck_key_and_id", pdu.toString().c_str());

    m_report->add("SCKN", pdu.getValue(pos, 5));
    pos += 5;

    m_report->add("SCK version number", pdu.getValue(pos, 16));
    pos += 16;

    bool sckUse = pdu.getValue(pos, 1);
    pos += 1;

    if (sckUse)
    {
        m_report->add("SCK use", "DMO");
    }
    else
    {
        m_report->add("SCK use", "TMO");
    }

    pos += 1;                                                           // reserved

    m_report->add("Sealed key (SSCK)", pdu.getValue(pos, 120));
    pos += 120;

    return pos;
}

/**
 * @brief SCK rejected - A.8.72b (EN 300 392-7)
 *
 */

uint64_t Mm::parseSckRejected(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_sck_rejected", pdu.toString().c_str());

    std::string txt = valueToString("OTAR reject reason", pdu.getValue(pos, 3));
    m_report->add("OTAR reject reason", txt);
    pos += 3;

    m_report->add("SCKN", pdu.getValue(pos, 5));
    pos += 5;

    return pos;
}

/**
 * @brief Security downlink - A.7.3 (EN 300 392-7)
 *
 */

uint64_t Mm::parseSecurityDownlink(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mm_parse_security_downlink", pdu.toString().c_str());

    m_report->add("Authentication successful", boolToString(pdu.getValue(pos, 1)));
    pos += 1;

    m_report->add("Supply TEI", boolToString(pdu.getValue(pos, 1)));
    pos += 1;

    m_report->add("Model number information requested", boolToString(pdu.getValue(pos, 1)));
    pos += 1;

    m_report->add("HW SW version number information requested", boolToString(pdu.getValue(pos, 1)));
    pos += 1;

    m_report->add("AI algorithm information requested", boolToString(pdu.getValue(pos, 1)));
    pos += 1;

    // reserved
    pos += 1;

    return pos;
}

/**
 * @brief Find value in map
 *
 */

std::string Mm::getMapValue(std::map<uint32_t, std::string> informationElement, uint32_t val)
{
    // get the default base value
    std::string res = Layer::getMapValue(informationElement, val);

    // map value was not found, so return reserved keyword instead
    if (strEqualsU(res, "not found"))
    {
        res = "reserved";
    }

    return res;
}

/**
 * @brief Convert MM elements value to string
 *
 */

std::string Mm::valueToString(std::string key, uint32_t val)
{
    if (strEqualsU(key, "group identity attachment lifetime"))
    {
        // 16.10.16 Table 16.48

        std::map<uint32_t, std::string> gIdAttachmentLifetime
        {
            {0b00, "Attachment not needed"},
            {0b01, "Attachment for next ITSI attach required"},
            {0b10, "Attachment not allowed for next ITSI attach"},
            {0b11, "Attachment for next location update required"}
        };

        return getMapValue(gIdAttachmentLifetime, val);
    }
    if (strEqualsU(key, "group identity detachment downlink"))
    {
        // 16.10.20 Table 16.52

        std::map<uint32_t, std::string> gIdDetachmentDownlink
        {
            {0b00, "Unknown group identity"},
            {0b01, "Temporary 1 detachment"},
            {0b10, "Temporary 2 detachment"},
            {0b11, "Permanent detachment"}
        };

        return getMapValue(gIdDetachmentDownlink, val);
    }
    if (strEqualsU(key, "key change type"))
    {
        // A.8.39

        std::map<uint32_t, std::string> keyChangeType
        {
            {0b000, "SCK"},
            {0b001, "CCK"},
            {0b010, "GCK"},
            {0b011, "Class 3 CCK and GCK activation"},
            {0b100, "All GCKs"},
            {0b101, "No cipher key"}
        };

        return getMapValue(keyChangeType, val);
    }
    if (strEqualsU(key, "LA timer"))
    {
        // 16.10.33 Table 16.63

        std::map<uint32_t, std::string> laTimer
        {
            {0b000, "10 min"},
            {0b001, "30 min"},
            {0b010, "1 hour"},
            {0b011, "2 hours"},
            {0b100, "4 hours"},
            {0b101, "8 hours"},
            {0b110, "24 hours"},
            {0b111, "no timing"}
        };

        return getMapValue(laTimer, val);
    }
    if (strEqualsU(key, "location update accept type"))
    {
        // 16.10.35a

        std::map<uint32_t, std::string> locationUpdateAcceptType
        {
            {0b000, "Roaming location updating"},
            {0b001, "Temporary registration"},
            {0b010, "Periodic location updating"},
            {0b011, "ITSI attach"},
            {0b100, "Call restoration roaming location updating"},
            {0b101, "Migrating or call restoration migrating location updating"},
            {0b110, "Demand location updating"},
            {0b111, "Disabled MS updating"}
        };

        return getMapValue(locationUpdateAcceptType, val);
    }
    if (strEqualsU(key, "location update type"))
    {
        // 16.10.35

        std::map<uint32_t, std::string> locationUpdateType
        {
            {0b000, "Roaming location updating"},
            {0b001, "Migrating location updating"},
            {0b010, "Periodic location updating"},
            {0b011, "ITSI attach"},
            {0b100, "Call restoration roaming location updating"},
            {0b101, "Call restoration migrating location updating"},
            {0b110, "Demand location updating"},
            {0b111, "Disabled MS updating"}
        };

        return getMapValue(locationUpdateType, val);
    }
    if (strEqualsU(key, "otar reject reason"))
    {
        // A.8.57b

        std::map<uint32_t, std::string> otarRejectReason
        {
            {0b000, "Key not available"},
            {0b001, "Invalid key number"},
            {0b010, "Invalid address"},
            {0b011, "KSG number not supported"}
        };

        return getMapValue(otarRejectReason, val);
    }
    if (strEqualsU(key, "pdu type"))
    {
        // 16.10.39

        std::map<uint32_t, std::string> pduType
        {
            {0b0000, "D-OTAR"},
            {0b0001, "D-AUTHENTICATION"},
            {0b0010, "D-CK CHANGE DEMAND"},
            {0b0011, "D-DISABLE"},
            {0b0100, "D-ENABLE"},
            {0b0101, "D-LOCATION UPDATE ACCEPT"},
            {0b0110, "D-LOCATION UPDATE COMMAND"},
            {0b0111, "D-LOCATION UPDATE REJECT"},
            {0b1001, "D-LOCATION UPDATE PROCEEDING"},
            {0b1010, "D-ATTACH/DETACH GROUP IDENTITY"},
            {0b1011, "D-ATTACH/DETACH GROUP IDENTITY ACK"},
            {0b1100, "D-MM STATUS"},
            {0b1111, "MM PDU/FUNCTION NOT SUPPORTED"}
        };

        return getMapValue(pduType, val);
    }
    if (strEqualsU(key, "reject cause"))
    {
        // 16.10.42

        std::map<uint32_t, std::string> rejectCause
        {
            {0b00001, "ITSI/ATSI unknown (system rejection)"},
            {0b00010, "Illegal MS (system rejection)"},
            {0b00011, "LA not allowed (LA rejection)"},
            {0b00100, "LA unknown (LA rejection)"},
            {0b00101, "Network failure (cell rejection)"},
            {0b00110, "Congestion (cell rejection)"},
            {0b00111, "Forward registration failure (cell rejection)"},
            {0b01000, "Service not subscribed (LA rejection)"},
            {0b01001, "Mandatory element error (system rejection)"},
            {0b01010, "Message consistency error (system rejection)"},
            {0b01011, "Roaming not supported (LA rejection)"},
            {0b01100, "Migration not supported (LA rejection)"},
            {0b01101, "No cipher KSG (cell rejection)"},
            {0b01110, "Identified cipher KSG not supported (cell rejection)"},
            {0b01111, "Requested cipher key type not available (cell rejection)"},
            {0b10000, "Identified cipher key not available (cell rejection)"},
            {0b10010, "Ciphering required (cell rejection)"},
            {0b10011, "Authentication failure"}
        };

        return getMapValue(rejectCause, val);
    }
    if (strEqualsU(key, "required cell type"))
    {
        // 16.10.43a

        std::map<uint32_t, std::string> requiredCellType
        {
            {0b000, "CA cell"},
            {0b001, "DA cell"}
        };

        return getMapValue(requiredCellType, val);
    }
    if (strEqualsU(key, "result of dual watch request"))
    {
        // 16.10.43d

        std::map<uint32_t, std::string> resultOfDwRequest
        {
            {0b000, "Request rejected for undefined reason"},
            {0b001, "Dual watch not supported"},
            {0b010, "Request accepted"}
        };

        return getMapValue(resultOfDwRequest, val);
    }
    if (strEqualsU(key, "time type"))
    {
        // A.8.86

        std::map<uint32_t, std::string> timeType
        {
            {0b00, "Absolute IV"},
            {0b01, "Network time"},
            {0b10, "Immediate, first slot of first frame of next multiframe"},
            {0b11, "Currently in use"}
        };

        return getMapValue(timeType, val);
    }
    if (strEqualsU(key, "type"))
    {
        // A.8.9

        std::map<uint32_t, std::string> type
        {
            {0b00, "All location areas"},
            {0b01, "List is provided"},
            {0b10, "LA-id mask is provided"},
            {0b11, "Range of LA-ids is provided"}
        };

        return getMapValue(type, val);
    }
    if (strEqualsU(key, "type 3/4 element identifier"))
    {
        // 16.10.51 Table 16.89

        std::map<uint32_t, std::string> type34ElementIdentifier
        {
            {0b0000, "Reserved for future extension"},
            {0b0001, "Default group attachment lifetime"},
            {0b0010, "New registered area"},
            {0b0011, "Group identity location demand"},
            {0b0100, "Group report response"},
            {0b0101, "Group identity location accept"},
            {0b0110, "DM-MS address"},
            {0b0111, "Group identity downlink"},
            {0b1000, "Group identity uplink"},
            {0b1001, "Authentication uplink"},
            {0b1010, "Authentication downlink"},
            {0b1011, "Extended capabilities"},
            {0b1100, "Group Identity Security Related Information"},
            {0b1101, "Reserved for any future specified Type 3/4 element"},
            {0b1110, "Reserved for any future specified Type 3/4 element"},
            {0b1111, "Proprietary"}
        };

        return getMapValue(type34ElementIdentifier, val);
    }
    return "unknown";
}
