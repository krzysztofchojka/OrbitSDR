#include "sds.h"

using namespace Tetra;

/**
 * @brief Constructor
 *
 */

Sds::Sds(Log * log, Report * report) : Layer(log, report)
{
    //
    // LIP is more a sub-layer of SDS than a layer, so it won't handle
    // the report creation and sending. It will only complete it with
    // informations.
    //
    // That's why it can be created here while sds is created in main
    // decoder class
    //
    m_lip = new Lip(m_log, m_report);
}

/**
 * @brief Destructor
 *
 */

Sds::~Sds()
{
    delete(m_lip);
}

/**
 * @brief SDS service entry point
 *
 */

void Sds::service(Pdu pdu, const MacLogicalChannel macLogicalChannel, TetraTime tetraTime, MacAddress macAddress)
{
    Layer::service(macLogicalChannel, tetraTime, macAddress);

    std::string txt = "";
    uint8_t pos = 0;
    uint8_t pduType = pdu.getValue(pos, 5);
    pos += 5;

    switch (pduType)                                                            // SDS subsystem
    {
    case 0b01000:
        txt = "D-STATUS";
        parseDStatus(Pdu(pdu, pos));                                            // SDS sub-entity see 14.7.1.11
        break;
    case 0b01111:
        txt = "D-SDS-DATA";
        parseDSdsData(Pdu(pdu, pos));                                           // SDS sub-entity see 14.7.1.10
        break;
    default:
        break;
    }
}


/**
 * @brief CMCE D-SDS-DATA 14.7.1.10
 *
 * WARNING: this function may generate two reports, the second one
 *          contains dump of user-defined type 4 message
 *
 */

void Sds::parseDSdsData(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "cmce_sds_parse_d_sds_data", pdu.toString().c_str());

    m_report->start("CMCE", "D-SDS-DATA", m_tetraTime, m_macAddress);

    uint32_t pos = 0;

    bool b_valid = false;

    uint8_t cpti = pdu.getValue(pos, 2);
    pos += 2;
    m_report->add("calling party type identifier", cpti);

    if (cpti == 0)                                                              // not documented
    {
        m_report->add("calling party ssi", pdu.getValue(pos, 8));
        pos += 8;
        b_valid = true;
    }
    else if (cpti == 1)                                                         // SSI
    {
        m_report->add("calling party ssi", pdu.getValue(pos, 24));
        pos += 24;
        b_valid = true;
    }
    else if (cpti == 2)                                                         // SSI + EXT
    {
        m_report->add("calling party ssi", pdu.getValue(pos, 24));
        pos += 24;

        m_report->add("calling party ext", pdu.getValue(pos, 24));
        pos += 24;
        b_valid = true;
    }

    if (!b_valid)                                                               // can't process further, return
    {
        m_report->send();
        return;
    }

    uint8_t sdti = pdu.getValue(pos, 2);                                        // short data type identifier
    pos += 2;
    m_report->add("sds type identifier", sdti);

    Pdu sdu;

    if (sdti == 0)                                                              // user-defined data 1
    {
        sdu = Pdu(pdu, pos, 16);
        pos += 16;
        m_report->add("infos", sdu);
    }
    else if (sdti == 1)                                                         // user-defined data 2
    {
        sdu = Pdu(pdu, pos, 32);
        pos += 32;
        m_report->add("infos", sdu);
    }
    else if (sdti == 2)                                                         // user-defined data 3
    {
        sdu = Pdu(pdu, pos, 64);
        pos += 64;
        m_report->add("infos", sdu);
    }
    else if (sdti == 3)                                                         // length indicator + user-defined data 4
    {
        uint16_t len = pdu.getValue(pos, 11);                                   // length indicator
        pos += 11;

        sdu = Pdu(pdu, pos, (int32_t)len);                                      // user-defined data 4
        pos += len;
        parseType4Data(sdu, len);                                               // parse type4 SDS message (message length is required to process user-defined type 4)
    }
    else
    {
        // invalid data
    }

    m_report->send();                                                           // send the decoded report

    if (sdti == 3)                                                              // dump type 4 data sdu for analysis
    {
        m_report->start("CMCE", "D-SDS-DATA", m_tetraTime, m_macAddress);
        m_report->add("hex", sdu);
        m_report->send();                                                       // send the hex dump report
    }
}

/**
 * @brief CMCE D-STATUS 14.7.1.11
 *
 */

void Sds::parseDStatus(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "cmce_sds_parse_d_status", pdu.toString().c_str());

    m_report->start("CMCE", "D-STATUS", m_tetraTime, m_macAddress);

    uint32_t pos = 0;

    uint8_t cpti = pdu.getValue(pos, 2);
    pos += 2;
    m_report->add("calling party type identifier", cpti);

    if (cpti == 1)                                                              // SSI
    {
        m_report->add("calling party ssi", pdu.getValue(pos, 24));
        pos += 24;
    }
    else if (cpti == 2)                                                         // SSI + EXT
    {
        m_report->add("calling party ssi", pdu.getValue(pos, 24));
        pos += 24;

        m_report->add("calling party ext", pdu.getValue(pos, 24));
        pos += 24;
    }

    m_report->add("pre-coded status", pdu.getValue(pos, 16));
    pos += 16;

    uint8_t oFlag = pdu.getValue(pos, 1);                                       // type 3 flag
    pos += 1;

    if (oFlag)                                                                  // there is type3 fields
    {
        uint8_t digitsCount = pdu.getValue(pos, 8);
        pos += 8;

        std::string extNumber = "";
        for (int idx = 0; idx < digitsCount; idx++)
        {
            extNumber += getTetraDigit((uint8_t)pdu.getValue(pos, 4));
            pos += 4;
        }

        if ((digitsCount % 2) != 0)                                             // check if we have a dummy digit
        {
            pos += 4;
        }
        m_report->add("external suscriber number", extNumber);
    }

    m_report->send();
}

/**
 * @brief SDS user-defined type 4 data - see 29.4, 29.5, Annex E, I, J
 *   Annex J - J.1 Protocol identifier information element
 *
 *   Protocol identifier 0b00000000 to 0b01111111 see clause 29.5 + Annex J.1 - Protocol specific definitions (shouldn't use SDS-TL) (text, location, OTAR...) see table 29.21
 *   Protocol identifier 0b10000000 to 0b11111111 see clause 29.4 + Annex J.1 - Other protocols with SDS-TL (NOTE some protocol may note use SDS-TL)
 *                       0b11000000 to 0b11111110
 *
 *   TODO check maximum length for user-defined data 4 is 2047 bits including protocol identifier 14.8.52
 */

void Sds::parseType4Data(Pdu pdu, const uint16_t len)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - len = %u pdu = %s\n", "cmce_sds_parse_type4_data", len, pdu.toString().c_str());

    if ((pdu.size() < len) || (len > 2047))
    {
        m_report->add("type4", "invalid pdu");
        m_report->add("type4 declared len", len);
        m_report->add("type4 actual len", (uint64_t)pdu.size());
        return;                                                                 // invalid PDU
    }

    uint32_t pos = 0;
    Pdu sdu;

    uint8_t protocolId = pdu.getValue(pos, 8);
    pos += 8;
    m_report->add("protocol id", protocolId);                                   // Note that report has been opened and will be send parseDSdsData function

    if (protocolId <= 0b01111111)                                               // Non SDS-TL protocols - see Table 29.21
    {
        switch (protocolId)
        {
        case 0b00000000:
            m_report->add("protocol info", "reserved");
            break;

        case 0b00000001:
            m_report->add("protocol info", "OTAK");                             // 29.5.1
            break;

        case 0b00000010:
            m_report->add("protocol info", "simple text messaging");            // 29.5.2
            parseSimpleTextMessaging(pdu, len);
            break;

        case 0b00000011:
            m_report->add("protocol info", "simple location system");           // 29.5.5
            parseSimpleLocationSystem(pdu, len);
            break;

        case 0b00000100:
            m_report->add("protocol info", "wireless datagram protocol");       // 29.5.8
            break;

        case 0b00000101:
            m_report->add("protocol info", "wireless control message protocol"); // 29.5.8
            break;

        case 0b00000110:
            m_report->add("protocol info", "M-DMO");                            // EN 300 396-10 [26] - 29.5.1
            break;

        case 0b00000111:
            m_report->add("protocol info", "pin authentification");             // 29.5.1
            break;

        case 0b00001000:
            m_report->add("protocol info", "end-to-end encrypted message");     // 29.5.1
            break;

        case 0b00001001:
            m_report->add("protocol info", "simple immediate text messaging");  // 29.5.2 - same message than "simple text messaging". Only the handling by the MS differs
            parseSimpleTextMessaging(pdu, len);
            break;

        case 0b00001010:
            m_report->add("protocol info", "location information protocol");    // 29.5.12 - TS 100 392-18 v1.7.2 - LIP service
            sdu = Pdu(pdu, pos, (int32_t)len - (int32_t)pos);
            m_lip->service(sdu, m_macLogicalChannel, m_tetraTime, m_macAddress);
            break;

        case 0b00001011:
            m_report->add("protocol info", "net assist protocol");              // 29.5.13
            break;

        case 0b00001100:
            m_report->add("protocol info", "concatenated sds message");         // TODO 29.5.14 - make the UDH protocol handling to reconsrtuct full message
            break;

        case 0b00001101:
            m_report->add("protocol info", "DOTAM");                            // TS 100 392-18-3 [48] - 29.5.1
            break;

        default:
            if (protocolId <= 0b00111111)
            {
                m_report->add("protocol info", "reserved for future standard definition");
            }
            else if (protocolId <= 0b00111110)
            {
                m_report->add("protocol info", "available for user application definition"); // Annex J
            }
            else                                                                // = 0b01111111
            {
                m_report->add("protocol info", "reserved for extension");       // TODO this value indicates that the next 8 bits is the protocol identifier and the 16 bits
                                                                                // replaces the 8 bits of the procol identifier in this PDU using this extension method
            }
            break;
        }
    }    // end of non SDS-TL protocols
    else                                                                        // SDS-TL (sub) protocols - see Table 29.21
    {
        // Annex J.1 - Table SDS-TL
        // protocol values in the scope of norm: 0b11000000 to 0b1111110
        //
        // Note: protocol id is not use here and will be handled by SDS TL service so the sdu is the full pdu

        uint8_t messageType = pdu.getValue(pos, 4);
        pos += 4;
        m_report->add("message type", messageType);

        switch (messageType)                                                    // 29.4.3.8 - Table 29.20
        {
        case 0b0000:                                                            // SDS-TRANSFER - 29.4.2.4 for user data type-4 informations
            m_report->add("sds-pdu", "SDS-TRANSFER");
            parseSubDTransfer(pdu, len);
            break;

        case 0b0001:
            m_report->add("sds-pdu", "SDS-REPORT");                             // TODO don't process SDS message ?
            break;

        case 0b0010:
            m_report->add("sds-pdu", "SDS-ACK");                                // TODO don't process SDS message ?
            break;

        default:                                                                // TODO don't process SDS message ?
            if (messageType <= 0b0111)
            {
                m_report->add("sds-pdu", "reserved for additional message types");
            }
            else                                                                // > 0b1000 29.4.3.8 - Table 29.20
            {
                m_report->add("protocol info", "defined by application");
            }

            break;
        }
    } // end of SDS-TL protocols
}

/**
 * @brief Parser sub protocol SDS-TRANFER
 *
 */

void Sds::parseSubDTransfer(Pdu pdu, const uint16_t len)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - len = %u pdu = %s\n", "cmce_sds_parse_sub_d_transfer", len, pdu.toString().c_str());

    uint32_t pos = 0;

    uint8_t protocolId = pdu.getValue(pos, 8);                                  // protocol id
    pos += 8;

    pos += 4;                                                                   // message type = SDS-TRANSFER
    pos += 2;                                                                   // delivery report request
    pos += 1;                                                                   // service selection / short form report


    uint8_t serviceForwardControl = pdu.getValue(pos, 1);
    pos += 1;

    m_report->add("message reference", pdu.getValue(pos, 8));
    pos += 8;

    uint8_t digitsCount = 0;
    std::string extNumber = "";

    if (serviceForwardControl)                                                  // service forward control required
    {
        m_report->add("validity period", pdu.getValue(pos, 5));
        pos += 5;

        uint8_t forwardAddressType = pdu.getValue(pos, 3);
        pos += 3;
        m_report->add("forward address type", forwardAddressType);              // see 29.4.3.5

        switch (forwardAddressType)
        {
        case  0b000:                                                            // SNA shouldn't be used (outside of scope of downlink receiver since it is reserved to MS -> SwMI direction)
            m_report->add("forward address ssi", pdu.getValue(pos, 8));
            pos += 8;
            break;

        case 0b001:                                                             // SSI
            m_report->add("forward address ssi", pdu.getValue(pos, 24));
            pos += 24;
            break;

        case 0b010:                                                             // TSI
            m_report->add("forward address ssi", pdu.getValue(pos, 24));
            pos += 24;

            m_report->add("forward address ext", pdu.getValue(pos, 24));
            pos += 24;
            break;

        case 0b011:                                                             // external subscriber number - CMCE type 3 block - 14.8.20
            digitsCount = pdu.getValue(pos, 8);
            pos += 8;

            extNumber = "";
            for (int idx = 0; idx < digitsCount; idx++)
            {
                extNumber += Tetra::getTetraDigit((uint8_t)pdu.getValue(pos, 4));
                pos += 4;
            }

            if ((digitsCount % 2) != 0)                                         // check if we have a dummy digit
            {
                pos += 4;
            }
            m_report->add("forward address external number", extNumber);
            break;

        case 0b111:                                                             // no forward address present
            m_report->add("forward address", "none");
            break;

        default:                                                                // reserved
            break;
        }
    }

    int32_t sduLength = (int32_t)len - (int32_t)pos;
    Pdu sdu = Pdu(pdu, pos, sduLength);

    switch (protocolId)                                                         // table 29.21
    {
    case 0b10000010:                                                            // 29.5.3
        m_report->add("protocol info", "text messaging (SDS-TL)");
        parseTextMessagingWithSdsTl(sdu);                                       // "infos" block will be filled in function
        break;

    case 0b10000011:                                                            // 29.5.6
        m_report->add("protocol info", "location system (SDS-TL)");
        parseLocationSystemWithSdsTl(sdu);                                      // "infos" block will be filled in function
        break;

    case 0b10000100:                                                            // Wireless Datagram Protocol WAP - 29.5.8
        m_report->add("protocol info", "WAP (SDS-TL)");
        m_report->add("infos", sdu);                                            // remaining part of pdu is sdu since we may be managed by SDS-TL
        break;

    case 0b10000101:                                                            // Wireless Control Message Protocol WCMP - 29.5.8
        m_report->add("protocol info", "WCMP (SDS-TL)");
        m_report->add("infos", sdu);                                            // remaining part of pdu is sdu since we may be managed by SDS-TL
        break;

    case 0b10000110:                                                            // Managed DMO M-DMO - 29.5.1
        m_report->add("protocol info", "M-DMO (SDS-TL)");
        m_report->add("infos", sdu);                                            // remaining part of pdu is sdu since we may be managed by SDS-TL
        break;

    case 0b10001000:                                                            // end-to-end encrypted message
        m_report->add("protocol info", "end-to-end encrypted message (SDS-TL)");
        m_report->add("infos", sdu);                                            // remaining part of pdu is sdu since we may be managed by SDS-TL
        break;

    case 0b10001001:                                                            // 29.5.3
        m_report->add("protocol info", "immediate text messaging (SDS-TL)");
        parseTextMessagingWithSdsTl(sdu);                                       // same as text messaging, only the handling by MS differs
        break;

    case 0b10001010:                                                            // TODO UDH - 29.5.9
        m_report->add("protocol info", "message with user-data header");
        m_report->add("infos", sdu);                                            // remaining part of pdu is sdu since we may be managed by SDS-TL
        break;

    case 0b10001100:                                                            // TODO 29.5.14
        m_report->add("protocol info", "concatenated sds message (SDS-TL)");
        m_report->add("infos", sdu);                                            // remaining part of pdu is sdu since we may be managed by SDS-TL
        break;

    default:
        m_report->add("protocol info", "reserved/user-defined");                // if reserved or user-defined, try a generic text read
        parseTextMessagingWithSdsTl(sdu);                                       // "infos" block will be filled in function
        //parseLocationSystemWithSdsTl(sdu);                        // "infos" block will be filled in function
        break;
    }

}

/**
 * @brief Parse simple text messaging - see 29.5.2
 *
 */

void Sds::parseSimpleTextMessaging(Pdu pdu, const uint16_t len)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - len = %u pdu = %s\n", "cmce_sds_parse_simple_text_messaging", len, pdu.toString().c_str());

    uint32_t pos = 8;                                                           // protocol id

    uint8_t timestamp_used = pdu.getValue(pos, 1);
    pos += 1;                                                                   // fill bit - (should be 0) - see 29.5.2.3
                                                                                // and if in SDS-TL this is the timestamp present bit - see 29.5.3.3
    uint8_t textCodingScheme = pdu.getValue(pos, 7);
    pos += 7;
    m_report->add("text coding scheme", textCodingScheme);

    if(timestamp_used)
    {
        uint32_t timestamp = pdu.getValue(pos, 24);                             // TODO decode timestamp
        pos += 24;
        m_report->add("timestamp", timestamp);
    }

    std::string txt = "";
    int32_t sduLength = (int32_t)len - (int32_t)pos;
    Pdu sdu = Pdu(pdu, pos, sduLength);

    if (textCodingScheme == 0b0000000)                                          // GSM 7-bit alphabet - see 29.5.4.3
    {
        txt = sdu.textGsm7BitDecode(sduLength);
        m_report->add("infos", txt);
    }
    else if (textCodingScheme <= 0b0011001)                                     // 8 bit alphabets
    {
        txt = sdu.textIso8859Decode(sduLength, textCodingScheme);
        m_report->add("infos", txt);
    }
    else                                                                        // try generic 8 bits alphabet since we already have the full hexadecimal SDU
    {
        txt = sdu.textIso8859Decode(sduLength, textCodingScheme);
        m_report->add("infos", txt);
    }
}

/**
 * @brief Parse text messaging with SDS-TL - see 29.5.3
 *
 */

void Sds::parseTextMessagingWithSdsTl(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "cmce_sds_parse_text_messaging_with_sds_tl", pdu.toString().c_str());

    // Table 28.29 - 29.5.3.3
    uint16_t len = pdu.size();
    uint32_t pos = 0;

    uint8_t timestampFlag = pdu.getValue(pos, 1);                               // timestamp flag
    pos += 1;

    uint8_t textCodingScheme = pdu.getValue(pos, 7);
    pos += 7;
    m_report->add("text coding scheme", textCodingScheme);

    if (timestampFlag)
    {
        uint32_t timestamp = pdu.getValue(pos, 24);
        pos += 24;
        m_report->add("timestamp", timestamp);
    }

    std::string txt = "";
    int32_t sduLength = (int32_t)len - (int32_t)pos;
    Pdu sdu = Pdu(pdu, pos, sduLength);

    if (textCodingScheme == 0b0000000)                                          // GSM 7-bit alphabet - see 29.5.4.3
    {
        txt = sdu.textGsm7BitDecode(sduLength);
        m_report->add("infos", txt);
    }
    else if (textCodingScheme <= 0b0011001)                                     // 8 bit alphabets
    {
        txt = sdu.textIso8859Decode(sduLength, textCodingScheme);
        m_report->add("infos", txt);
    }
    else                                                                        // try generic 8 bits alphabet since we already have the full hexadecimal SDU
    {
        txt = sdu.textIso8859Decode(sduLength, textCodingScheme);
        m_report->add("infos", txt);
    }
}

/**
 * @brief Parse SDS simple location system - see 29.5.5
 *
 */

void Sds::parseSimpleLocationSystem(Pdu pdu, const uint16_t len)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "cmce_sds_parse_simple_location_system", pdu.toString().c_str());

    uint32_t pos = 8;                                                           // protocol id

    uint8_t locationSystemCoding = pdu.getValue(pos, 8);
    pos += 8;
    m_report->add("location coding system", locationSystemCoding);

    // remaining bits are len - 8 - 8 since len is size of pdu
    std::string txt = "";
    int32_t sduLength = (int32_t)len - (int32_t)pos;
    Pdu sdu = Pdu(pdu, pos, sduLength);

    switch (locationSystemCoding)
    {
    case 0b00000000:                                                            // NMEA 0183 - see Annex L
        txt = sdu.locationNmeaDecode(sduLength);
        m_report->add("infos", txt);
        break;

    case 0b00000001:                                                            // TODO RTCM RC-104 - see Annex L
        //txt = location_rtcm_decode(Pdu(pdu, pos, len - pos), len - pos);
        m_report->add("infos", sdu);
        break;

    case 0b10000000:                                                            // TODO Proprietary. Notes from SQ5BPF: some proprietary system seen in the wild in Spain, Italy and France some speculate it's either from DAMM or SEPURA
        m_report->add("infos", sdu);
        break;

    default:
        m_report->add("infos", sdu);
        break;
    }
}

/**
 * @brief Parse SDS location system with SDS-TL - see 29.5.6
 *
 */

void Sds::parseLocationSystemWithSdsTl(Pdu pdu)
{
    m_log->print(LogLevel::HIGH, "DEBUG ::%-44s - pdu = %s\n", "cmce_sds_parse_location_system_with_sds_tl", pdu.toString().c_str());

    uint16_t len = pdu.size();
    uint32_t pos = 0;

    uint8_t locationSystemCoding = pdu.getValue(pos, 8);
    pos += 8;
    m_report->add("location coding system", locationSystemCoding);

    std::string txt = "";
    int32_t sduLength = (int32_t)len - (int32_t)pos;
    Pdu sdu = Pdu(pdu, pos);

    switch (locationSystemCoding)
    {
    case 0b00000000:                                                            // NMEA 0183 - see Annex L
        txt = sdu.locationNmeaDecode(sduLength);
        m_report->add("infos", txt);
        break;

    case 0b00000001:                                                            // TODO RTCM RC-104 - see Annex L
        //txt = location_rtcm_decode(Pdu(pdu, pos, len - pos), len - pos);
        m_report->add("infos", sdu);
        break;

    case 0b10000000:                                                            // TODO Proprietary. Notes from SQ5BPF: some proprietary system seen in the wild in Spain, Itlay and France some speculate it's either from DAMM or SEPURA
        m_report->add("infos", sdu);
        break;

    default:
        m_report->add("infos", sdu);
        break;
    }
}
