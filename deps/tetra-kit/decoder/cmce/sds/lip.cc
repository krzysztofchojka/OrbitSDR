#include "lip.h"

using namespace Tetra;

/**
 * @brief Constructor
 *
 */

Lip::Lip(Log * log, Report * report) : Layer(log, report)
{

}

/**
 * @brief Destructor
 *
 */

Lip::~Lip()
{

}

/**
 * @brief LIP protocol (stack built over SDS) - TS 100 392-18 - v1.7.3
 *
 * NOTE: Lip is more a sub-service than a service, m_report coming from SDS layer
 *       mustn't be initialized here since it copmletes SDS one
 *
 */

void Lip::service(Pdu pdu, const MacLogicalChannel macLogicalChannel, TetraTime tetraTime, MacAddress macAddress)
{
    Layer::service(macLogicalChannel, tetraTime, macAddress);

    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "cmce_sds_service_location_information_protocol", pdu.toString().c_str());

    uint32_t pos = 0;                                                           // protocol ID from SDS has been removed since LIP is a service with SDU

    uint8_t pduType = pdu.getValue(pos, 2);                                     // see 6.2
    pos += 2;

    switch (pduType)                                                            // table 6.29
    {
    case 0b00:                                                                  // short location report - 6.2.1
         m_report->add("sds-lip", "short location report");
         parseShortLocationReport(pdu);
         break;

    case 0b01:                                                                  // PDU with extension - 6.3.62 table 6.92
        m_report->add("sds-lip", "extension pdu");
        parseExtendedMessage(pdu);
        break;

    case 0b10:                                                                  // reserved
    case 0b11:
        break;
    }
}

/**
 * @brief Short location report PDU - 6.2.1
 *
 */

void Lip::parseShortLocationReport(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "cmce_sds_lip_parse_short_location_report", pdu.toString().c_str());

    static const std::size_t MIN_SIZE = 68;

    if (pdu.size() >= MIN_SIZE)
    {
        uint32_t pos = 2;                                                       // PDU type

        pos += 2;                                                               // time elapsed

        uint32_t longitude = pdu.getValue(pos, 25);
        pos += 25;
        m_report->add("longitude uint32", longitude);
        m_report->add("longitude", decodeLipLongitude(longitude));

        uint32_t latitude = pdu.getValue(pos, 24);
        pos += 24;
        m_report->add("latitude uint32", longitude);
        m_report->add("latitude", decodeLipLatitude(latitude));

        uint8_t positionError = pdu.getValue(pos, 3);
        pos += 3;
        m_report->add("position error", decodeLipPositionError(positionError));

        uint8_t horizontalVelocity = pdu.getValue(pos, 7);
        pos += 7;
        m_report->add("horizontal_velocity uint8", horizontalVelocity);
        m_report->add("horizontal_velocity", decodeLipHorizontalVelocity(horizontalVelocity));

        uint8_t directionOfTravel = pdu.getValue(pos, 4);
        pos += 4;
        m_report->add("direction of travel", decodeLipDirectionOfTravel(directionOfTravel));

        uint8_t typeOfAdditionalData = pdu.getValue(pos, 1);                    // 6.3.87 - Table 6.120
        pos += 1;

        uint8_t additionalData = pdu.getValue(pos, 8);
        pos += 8;

        if (typeOfAdditionalData == 0)                                          // reason for sending
        {
            m_report->add("reason for sending", additionalData);
        }
        else                                                                    // user-defined data
        {
            m_report->add("user-defined additional data", additionalData);
        }
    }
    else
    {
        m_report->add("invalid pdu size", (uint64_t)pdu.size());
        m_report->add("pdu minimum size", (uint64_t)MIN_SIZE);
    }

}

/**
 * @brief Parse LIP extension - see 6.3.62 table 6.92
 *
 */

void Lip::parseExtendedMessage(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "cmce_sds_lip_parse_extended_message", pdu.toString().c_str());

    uint32_t pos = 2;                                                           // pdu type

    uint8_t extension = pdu.getValue(pos, 4);
    pos += 4;

    switch (extension)                                                          // table 6.92
    {
    case 0b0000:                                                                // reserved
    case 0b0010:
    case 0b1101:
    case 0b1110:
    case 0b1111:
        m_report->add("extension", "reserved");
        break;

    case 0b0001:
        m_report->add("extension", "immediate location report request");        // TODO 6.2.16
        break;

    case 0b0011:
        m_report->add("extension", "long location report");                     // TODO 6.2.2
        break;

    case 0b0100:
        m_report->add("extension", "location report ack");                      // TODO 6.2.3
        break;

    case 0b0101:
        m_report->add("extension", "basic location parameters request/response"); // TODO 6.2.4 - 6.2.5
        break;

    case 0b0110:
        m_report->add("extension", "add/modify trigger request/response");      // TODO 6.2.8 - 6.2.9
        break;

    case 0b0111:
        m_report->add("extension", "remove trigger request/response");          // TODO 6.2.10 - 6.2.11
        break;

    case 0b1000:
        m_report->add("extension", "report trigger request/response");          // TODO 6.2.12 - 6.2.13
        break;

    case 0b1001:
        m_report->add("extension", "report basic location parameters request/response"); // TODO 6.2.4 - 6.2.5
        break;

    case 0b1010:
        m_report->add("extension", "location reporting enable/disable request/response"); // TODO 6.2.14 - 6.2.15
        break;

    case 0b1011:
        m_report->add("extension", "location reporting temporary control request/response"); // TODO 6.2.17 - 6.2.18
        break;

    case 0b1100:
        m_report->add("extension", "backlog request/response");                 // TODO 6.2.19 - 6.2.20
        break;

    default:
        break;
    }
}

/**
 * @brief Decode LIP latitude - 6.3.30
 *
 *   11001010 00011001 01011001   Latitude    = 0x0CA1959   (-37.899131)
 *
 */

double Lip::decodeLipLatitude(uint32_t latitude)
{
    return decodeIntegerTwosComplement(latitude, 24, 90.0);
}

/**
 * @brief Decode LIP longitude - 6.3.50
 *
 * 0 11001110 01101100 10011000   Longitude   = 0x0CE6C98   (145.142012)
 *
 */

double Lip::decodeLipLongitude(uint32_t longitude)
{
    return decodeIntegerTwosComplement(longitude, 25, 180.0);
}

/**
 * @brief Decode LIP horizontal velocity - 6.3.17
 *
 */

double Lip::decodeLipHorizontalVelocity(uint8_t val)
{
    double res;

    if (val == 127)
    {
        // unknown
        res = -1.0;
    }
    else
    {
        const double C = 16.0;
        const double x = 0.038;
        const double A = 13.0;
        const double B = 0.0;

        res = C * pow(1.0 + x, A - (double)val) + B;
    }

    return res;
}

/**
 * @brief Decode LIP direction on travel - 6.3.5
 *
 */

std::string Lip::decodeLipDirectionOfTravel(uint8_t val)
{
    const std::string directions[] = {
        "0 N",   "22.5 NNE",  "45 NE",  "67.5 ENE",
        "90 E",  "112.5 ESE", "135 SE", "157.5 SSE",
        "180 S", "202.5 SSW", "225 SW", "247.5 WSW",
        "270 W", "292.5 WNW", "315 NW", "337.5 NNW"
    };

    return directions[val];
}

/**
 * @brief Decode LIP position error - 6.3.63
 *
 */

std::string Lip::decodeLipPositionError(uint8_t val)
{
    const std::string posError[] = {
        "< 2 m", "< 20 m", "< 200 m", "< 2 km", "< 20 km", "<= 200 km", "> 200 km", "unknown"
    };

    return posError[val];
};
