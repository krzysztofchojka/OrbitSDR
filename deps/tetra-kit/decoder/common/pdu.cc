#include "pdu.h"

using namespace Tetra;

/**
 * @brief Constructor of empty PDU
 *
 */

Pdu::Pdu()
{
    m_vec.clear();
}

/**
 * @brief Constructor from another PDU, starting at specified position with specified length
 *
 * NOTES:
 *   - if length is negative, null Pdu is created
 *   - if length is 0, input pdu is copied
 *   - if length is positive, input pdu is copied with at more length bytes (may be smaller depending on remainig characters)
 *
 */

Pdu::Pdu(const Pdu & pdu, const uint32_t startPos, const int32_t length)
{
    // clear internal vector
    m_vec.clear();

    // calculate actual length after start position
    int32_t actualLen = (int32_t)pdu.m_vec.size() - (int32_t)startPos;

    // check if we have remaining data after start position and requested length is positive or null (ie. whole pdu copy)
    if ((actualLen > 0) && (length >= 0))
    {
        if ((length > 0) && (actualLen > length))
        {
            //  we have more data available than requested, limit actualLen to requested length
            actualLen = length;
        }

        m_vec.assign(pdu.m_vec.begin() + (size_t)startPos, pdu.m_vec.begin() + (size_t)startPos + (size_t)actualLen);
    }
}


/**
 * @brief Constructor from a vector data
 *
 * We use the "copy" operator with b = const vector & a
 *
 */

Pdu::Pdu(const std::vector<uint8_t> & val)
{
    m_vec = val;
}

/**
 * @brief Destructor
 *
 */

Pdu::~Pdu()
{

}

/**
 * @brief
 *
 */

void Pdu::clear()
{
    m_vec.clear();
}

/**
 * @brief
 *
 */

uint64_t Pdu::getValue(const uint64_t startPos, const uint8_t fieldLen)
{
    uint64_t val = 0;

    for (uint64_t pos = 0; (pos < fieldLen) && (pos + startPos < m_vec.size()); pos++)
    {
        val += (m_vec[pos + startPos] << (fieldLen - pos - 1));
    }

    return val;
}

/**
 * @brief
 *
 */

void Pdu::append(const uint8_t val)
{
    m_vec.push_back(val);
}

/**
 * @brief
 *
 */

void Pdu::append(const std::vector<uint8_t> vec)
{
    m_vec.insert(m_vec.end(), vec.begin(), vec.end());
}

/**
 * @brief
 *
 */

void Pdu::append(const Pdu val)
{
    append(val.m_vec);
}

/**
 * @brief
 *
 */

std::string Pdu::toString(const int len)
{
    std::string res   = "";
    std::size_t count = m_vec.size();

    if ((len > 0) && (len < (int)count))                                        // if len is 0 then print entire vector
    {
        count = (std::size_t)len;
    }

    for (std::size_t idx = 0; idx < count; idx++)
    {
        res += (char)(m_vec[idx] + '0');
    }

    return res;
}

/**
 * @brief
 *
 */

std::string Pdu::toHex()
{
    std::string txt = "";
    char buf[32] = "";

    uint32_t pos = 0;
    for (std::size_t cnt = 0; cnt < m_vec.size() / 8; cnt++)
    {
        uint8_t val = getValue(pos, 8);
        pos += 8;

        if (cnt > 0)
        {
            sprintf(buf, " %02x", val);
        }
        else
        {
            sprintf(buf, "%02x", val);
        }

        txt += buf;
    }

    return txt;
}

/**
 * @brief
 *
 */

void Pdu::print(const int len)
{
    std::cout << toString(len) << std::endl;
}

/**
 * @brief Copy vector to packed uint8_t array
 *
 */

void Pdu::toPackedUInt8(uint8_t * data)
{
    size_t idx;
    uint8_t curByte = 0;

    for (idx = 0; idx < m_vec.size(); idx++)
    {
        uint8_t bitnum = (uint8_t)(7 - (idx % 8));

        curByte |= (m_vec[idx] << bitnum);

        if((idx % 8) == 7)
        {
            *data++ = curByte;
            curByte = 0;
        }
    }

    // we have a non-modulo-8 bitcount
    if (idx % 8)
        *data++ = curByte;
}

/**
 * @brief
 *
 */

void Pdu::resize(const std::size_t len)
{
    if ((len > 0) && (len < m_vec.size()))
    {
        m_vec.resize(len);
    }
}

/**
 * @brief
 *
 */

std::size_t Pdu::size()
{
    return m_vec.size();
}

/**
 * @brief
 *
 */

uint8_t Pdu::at(const std::size_t pos)
{
    if (pos < m_vec.size())
    {
        return m_vec[pos];
    }
    else
    {
        return 0;
    }
}

/**
 * @brief
 *
 */

bool Pdu::isEmpty()
{
    return (m_vec.size() == 0);
}

/**
 * @brief Extract uint8_t vector from uint8_t vector
 *
 */

std::vector<uint8_t> Pdu::extractVec(const uint32_t startPos, const int32_t length)
{
    std::vector<uint8_t> ret;

    if (length > 0)                                                             // valid length requested
    {
        int32_t len = (int32_t)m_vec.size() - (int32_t)startPos;                // actual remaining bytes in vector after pos

        if (len > 0)                                                            // check if actual length is valid
        {
            if (len > length)                                                   // we have more bytes than requested
            {
                len = length;                                                   // so return only the requested ones
            }

            std::copy(m_vec.begin() + startPos, m_vec.begin() + startPos + (uint32_t)len, back_inserter(ret));
        }
    }

    return ret;
}

/**
 * @brief Decode 7-bit alphabet 29.5.4.3
 *
 */

std::string Pdu::textGsm7BitDecode(const int16_t len)
{
    // NOTE: _ is a special char when we want to escape the character value
    //                   0        10         20        30         40        50        60        70        80        90        100       110      120
    //                   0123456789012345678901234567890123 4567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567
    const std::string GSM7 = "@_______________________________ !\"#{%&'()*+,-./0123456789:;<=>?_ABCDEFGHIJKLMNOPQRSTUVWXYZ______abcdefghijklmnopqrstuvwxyz_____";
    std::string res = "";

    for (int16_t idx = 0; idx < len / 7; idx++)
    {
        uint8_t chr = getValue((uint64_t)idx * 7, 7);

        char val = GSM7[chr];
        if (val == '{')
        {
            char buf[16] = "";
            sprintf(buf, "0x%02x", chr);
            res += buf;
        }
        else
        {
            res += (char)chr;
        }
    }

    return res;
}

/**
 * @brief Decode 8-bit alphabets TODO very rough function only, doesn't check alphabet input type
 *        Unknown symbols are replaced with '_' since we already have hex dump
 *
 */

std::string Pdu::textGeneric8BitDecode(const int16_t len)
{
    std::string res = "";

    for (int16_t idx = 0; idx < len / 8; idx++)
    {
        uint8_t chr = getValue((uint64_t)idx * 8, 8);
        if (isprint(chr))
        {
            res += (char)chr;
        }
        else
        {
            res += '_';
        }
    }

    return res;
}

/**
 * @brief OrbitSDR: 8-bit text in an ISO 8859 coding scheme, returned as UTF-8
 *        (upstream printed every byte above 0x7F as '_', losing e.g. Polish letters)
 *
 */

static void appendUtf8(std::string & out, uint32_t cp)
{
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

std::string Pdu::textIso8859Decode(const int16_t len, const uint8_t codingScheme)
{
    // ISO 8859-2, code points of 0xA0..0xFF
    static const uint16_t LATIN2[96] = {
        0x00A0,0x0104,0x02D8,0x0141,0x00A4,0x013D,0x015A,0x00A7,0x00A8,0x0160,0x015E,0x0164,0x0179,0x00AD,0x017D,0x017B,
        0x00B0,0x0105,0x02DB,0x0142,0x00B4,0x013E,0x015B,0x02C7,0x00B8,0x0161,0x015F,0x0165,0x017A,0x02DD,0x017E,0x017C,
        0x0154,0x00C1,0x00C2,0x0102,0x00C4,0x0139,0x0106,0x00C7,0x010C,0x00C9,0x0118,0x00CB,0x011A,0x00CD,0x00CE,0x010E,
        0x0110,0x0143,0x0147,0x00D3,0x00D4,0x0150,0x00D6,0x00D7,0x0158,0x016E,0x00DA,0x0170,0x00DC,0x00DD,0x0162,0x00DF,
        0x0155,0x00E1,0x00E2,0x0103,0x00E4,0x013A,0x0107,0x00E7,0x010D,0x00E9,0x0119,0x00EB,0x011B,0x00ED,0x00EE,0x010F,
        0x0111,0x0144,0x0148,0x00F3,0x00F4,0x0151,0x00F6,0x00F7,0x0159,0x016F,0x00FA,0x0171,0x00FC,0x00FD,0x0163,0x02D9 };

    if (codingScheme != 1 && codingScheme != 2) return textGeneric8BitDecode(len);

    std::string res;
    for (int16_t idx = 0; idx < len / 8; idx++)
    {
        const uint8_t chr = getValue((uint64_t)idx * 8, 8);
        if (chr == 0x0A || chr == 0x0D) res += ' ';                             // line breaks: one line in the UI
        else if (chr >= 0x20 && chr < 0x7F) res += (char)chr;                   // ASCII
        else if (chr >= 0xA0) appendUtf8(res, codingScheme == 2 ? LATIN2[chr - 0xA0] : chr);
        else res += '_';                                                        // C0 / C1 controls
    }
    return res;
}

/**
 * @brief NMEA location decode (NMEA 0183)
 *        ASCII-8 plain text with CR/LF endline
 *        with optional checksum separated by *
 *
 */

std::string Pdu::locationNmeaDecode(const int16_t len)
{
    std::string res = "";

    for (int16_t idx = 0; idx < len / 8; idx++)
    {
        uint8_t chr = getValue((uint64_t)idx * 8, 8);

        if ((chr != 10) && (chr != 13))                                         // skip CR/LF
        {
            res += (char)chr;
        }
    }

    return res;
}
