#include "mle.h"

using namespace Tetra;

/**
 * @brief 18.5.17 Neighbour cell information for CA
 *
*/

uint64_t Mle::parseBsServiceDetails(Pdu pdu, uint64_t pos, std::vector<std::tuple<std::string, uint64_t>> & infos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mle_parse_bs_service_details", pdu.toString().c_str());

    infos.push_back(std::make_tuple("Registration mandatory", pdu.getValue(pos, 1)));
    pos += 1;
    infos.push_back(std::make_tuple("De-registration requested", pdu.getValue(pos, 1)));
    pos += 1;
    infos.push_back(std::make_tuple("Priority cell", pdu.getValue(pos, 1)));
    pos += 1;
    infos.push_back(std::make_tuple("Minimum mode service supported", pdu.getValue(pos, 1)));
    pos += 1;
    infos.push_back(std::make_tuple("Migration supported", pdu.getValue(pos, 1)));
    pos += 1;
    infos.push_back(std::make_tuple("System wide services supported", pdu.getValue(pos, 1)));
    pos += 1;
    infos.push_back(std::make_tuple("TETRA voice service supported", pdu.getValue(pos, 1)));
    pos += 1;
    infos.push_back(std::make_tuple("Circuit mode data service supported", pdu.getValue(pos, 1)));
    pos += 1;
    infos.push_back(std::make_tuple("Reserved", pdu.getValue(pos, 1)));
    pos += 1;
    infos.push_back(std::make_tuple("SNDCP service available", pdu.getValue(pos, 1)));
    pos += 1;
    infos.push_back(std::make_tuple("Air interface encryption service available", pdu.getValue(pos, 1)));
    pos += 1;
    infos.push_back(std::make_tuple("Advanced link supported", pdu.getValue(pos, 1)));
    pos += 1;

    return pos;
}

/**
 * @brief MLE Cell re-select parameters - 18.5.4
 *
 */

uint64_t Mle::parseCellReselectParameters(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mle_parse_cell_reselect_parameters", pdu.toString().c_str());

    uint8_t thresholdDb = pdu.getValue(pos, 4) * 2;
    m_report->add("SLOW_RESELECT_THRESHOLD_ABOVE_FAST", thresholdDb);
    pos += 4;

    thresholdDb = pdu.getValue(pos, 4) * 2;
    m_report->add("FAST_RESELECT_THRESHOLD", thresholdDb);
    pos += 4;

    thresholdDb = pdu.getValue(pos, 4) * 2;
    m_report->add("SLOW_RESELECT HYSTERESIS", thresholdDb);
    pos += 4;

    thresholdDb = pdu.getValue(pos, 4) * 2;
    m_report->add("FAST_RESELECT_HYSTERESIS", thresholdDb);
    pos += 4;

    return pos;
}

/**
 * @brief MLE Main carrier number extension - 18.5.11
 *
 */

uint64_t Mle::parseMainCarrierNumberExtension(Pdu pdu, uint64_t pos, std::vector<std::tuple<std::string, uint64_t>> & infos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mle_parse_main_carrier_number_extension", pdu.toString().c_str());

    uint64_t freqBand = pdu.getValue(pos, 4) * 100;
    infos.push_back(std::make_tuple("Frequency band", freqBand));
    pos += 4;

    infos.push_back(std::make_tuple("Offset", pdu.getValue(pos, 2)));
    pos += 2;

    infos.push_back(std::make_tuple("Duplex spacing", pdu.getValue(pos, 3)));
    pos += 3;

    infos.push_back(std::make_tuple("Reverse operation", pdu.getValue(pos, 1)));
    pos += 1;

    return pos;
}

uint64_t Mle::parseNeighbourCellBroadcast(Pdu pdu, uint64_t pos, std::vector<std::tuple<std::string, uint64_t>> & infos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mle_parse_neighbour_cell_broadcast", pdu.toString().c_str());

    infos.push_back(std::make_tuple("D-NWRK-BROADCAST broadcast supported", pdu.getValue(pos, 1)));
    pos += 1;
    infos.push_back(std::make_tuple("D-NWRK-BROADCAST enquiry supported", pdu.getValue(pos, 1)));
    pos += 1;

    return pos;
}


/**
 * @brief Parse neighbour cell information 18.5.17 and return actual data length read
 *        to increase flux position. This function used by mle_process_d_nwrk_broadcast
 *
 */

uint32_t Mle::parseNeighbourCellInformation(Pdu data, uint32_t posStart, std::vector<std::tuple<std::string, uint64_t>> & infos)
{
    uint32_t pos = posStart;

    infos.push_back(std::make_tuple("Cell identifier CA", data.getValue(pos, 5)));
    pos += 5;

    infos.push_back(std::make_tuple("Cell reselection types supported", data.getValue(pos, 2)));
    pos += 2;

    infos.push_back(std::make_tuple("Neighbour cell synchronized", data.getValue(pos, 1)));
    pos += 1;

    infos.push_back(std::make_tuple("Cell load CA", data.getValue(pos, 2)));
    pos += 2;

    infos.push_back(std::make_tuple("Main carrier number", data.getValue(pos, 12)));
    pos += 12;

    bool oFlag = data.getValue(pos, 1);                                         // option flag
    pos += 1;
    if (oFlag)                                                                  // there is type2 fields
    {
        bool pFlag = data.getValue(pos, 1);
        pos += 1;
        if (pFlag)
        {
            pos = parseMainCarrierNumberExtension(data, pos, infos);
        }

        pFlag = data.getValue(pos, 1);
        pos += 1;
        if (pFlag)
        {
            infos.push_back(std::make_tuple("MCC", data.getValue(pos, 10)));
            pos += 10;
        }

        pFlag = data.getValue(pos, 1);
        pos += 1;
        if (pFlag)
        {
            infos.push_back(std::make_tuple("MNC", data.getValue(pos, 14)));
            pos += 14;
        }

        pFlag = data.getValue(pos, 1);
        pos += 1;
        if (pFlag)
        {
            infos.push_back(std::make_tuple("LA", data.getValue(pos, 14)));
            pos += 14;
        }

        pFlag = data.getValue(pos, 1);
        pos += 1;
        if (pFlag)
        {
            // 18.5.13
            uint64_t maxTxPower = 15 + (data.getValue(pos, 3) - 1) * 5;
            infos.push_back(std::make_tuple("Maximum MS transmit power", maxTxPower));
            pos += 3;
        }

        pFlag = data.getValue(pos, 1);
        pos += 1;
        if (pFlag)
        {
            int minRxLevel = data.getValue(pos, 4) * 5 - 125;
            uint64_t minRxUnsigned = minRxLevel * -1;
            infos.push_back(std::make_tuple("Minimum RX access level", minRxUnsigned));
            pos += 4;
        }

        pFlag = data.getValue(pos, 1);
        pos += 1;
        if (pFlag)
        {
            infos.push_back(std::make_tuple("Subscriber class", data.getValue(pos, 16)));
            pos += 16;
        }

        pFlag = data.getValue(pos, 1);
        pos += 1;
        if (pFlag)
        {
            pos = parseBsServiceDetails(data, pos, infos);
        }

        pFlag = data.getValue(pos, 1);
        pos += 1;
        if (pFlag)
        {
            pos = parseTimeshareOrSecurity(data, pos, infos);
        }

        pFlag = data.getValue(pos, 1);
        pos += 1;
        if (pFlag)
        {
            infos.push_back(std::make_tuple("TDMA frame offset", data.getValue(pos, 6)));
            pos += 6;
        }
    }

    return pos;
}

/**
 * @brief 18.5.24 TETRA network time
 *
*/

uint64_t Mle::parseTetraNetworkTime(Pdu pdu, uint64_t pos)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mle_parse_tetra_network_time", pdu.toString().c_str());

    uint32_t utctime = pdu.getValue(pos, 24) * 2;
    pos += 24;

    uint8_t sign = pdu.getValue(pos, 1);
    pos += 1;

    uint8_t looffset = pdu.getValue(pos, 6);
    pos += 6;

    uint32_t year = pdu.getValue(pos, 6);
    pos += 6;

    pos += 11;                                                          // reserved

    if ( (utctime < 0xf142ff) && (looffset < 0x39) && (year < 0x3f) )   // check if values are not reserved or invalid
    {
        int offsetsec = looffset * (sign ? -15 : 15) * 60;              // calc offset in seconds

        time_t rawtime =  utctime + offsetsec;                          // 1.1.1970 00:00:00
        struct tm * timeinfo;
        timeinfo = localtime(&rawtime);
        timeinfo->tm_year += (30 + year);                               // Tetra time starts at year 2000

        char buf[sizeof("2000-01-01T00:00:00Z")];
        strftime(buf, sizeof(buf), "%FT%TZ", timeinfo);                 // encode time as ISO 8601 string
        m_report->add("TETRA network time", buf);
    }

    return pos;
}

/**
 * @brief 18.5.25 Timeshare cell information or security parameters
 *
*/

uint64_t Mle::parseTimeshareOrSecurity(Pdu pdu, uint64_t pos, std::vector<std::tuple<std::string, uint64_t>> & elements)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mle_parse_timeshare_or_security", pdu.toString().c_str());

    uint8_t discMode = pdu.getValue(pos, 2);
    elements.push_back(std::make_tuple("Discontinuous mode", pdu.getValue(pos, 2)));
    pos += 2;

    if (discMode == 0)
    {
        pos = parseSecurityParameters(pdu, pos, elements);
    }
    else
    {
        elements.push_back(std::make_tuple("Reserved frames per two multiframes", pdu.getValue(pos, 3)));
        pos += 3;
    }

    return pos;
}

/**
 * @brief A.8.77a Security parameters
 *
*/

uint64_t Mle::parseSecurityParameters(Pdu pdu, uint64_t pos, std::vector<std::tuple<std::string, uint64_t>> & elements)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "mle_parse_security_parameters", pdu.toString().c_str());

    elements.push_back(std::make_tuple("Authentication required", pdu.getValue(pos, 1)));
    pos += 1;

    elements.push_back(std::make_tuple("Security Class 1 supported", pdu.getValue(pos, 1)));
    pos += 1;

    elements.push_back(std::make_tuple("Security Class 2 or 3 support", pdu.getValue(pos, 1)));
    pos += 1;

    return pos;
}
