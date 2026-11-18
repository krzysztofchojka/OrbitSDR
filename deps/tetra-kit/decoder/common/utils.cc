#include "utils.h"

/**
 * @brief Extract uint8_t vector from uint8_t vector
 *
 */

std::vector<uint8_t> Tetra::vectorExtract(std::vector<uint8_t> source, const uint32_t pos, const int32_t length)
{
    std::vector<uint8_t> ret;

    if (length > 0)                                                             // check if invalid length requested
    {
        int32_t len = (int32_t)source.size() - (int32_t)pos;                    // actual remaining bytes in vector after pos

        if (len > 0)                                                            // check if actual length is valid
        {
            if (len > length)                                                   // we have more bytes than requested
            {
                len = length;                                                   // so return only the requested ones
            }

            std::copy(source.begin() + pos, source.begin() + pos + (uint32_t)len, back_inserter(ret));
        }
    }

    return ret;
}

/**
 * @brief Concatenate two uint8_t vectors
 *
 */

std::vector<uint8_t> Tetra::vectorAppend(std::vector<uint8_t> vec1, std::vector<uint8_t> vec2)
{
    std::vector<uint8_t> ret(vec1);

    ret.insert(ret.end(), vec2.begin(), vec2.end());

    return ret;
}

/**
 * @brief Convert binary vector to text string 0/1
 *
 */

std::string Tetra::vectorToString(const std::vector<uint8_t> data, const int len)
{
    std::string res = "";

    std::size_t count = data.size();

    if (len < (int)count)
    {
        count = (std::size_t)len;
    }

    for (std::size_t idx = 0; idx < count; idx++)
    {
        res += (char)(data[idx] + '0');
    }

    return res;
}

/**
 * @brief Convert bool to string (true/false)
 *
 */

std::string Tetra::boolToString(bool value)
{
    return value ? "true" : "false";
}

/**
 * @brief Varidiac function to return string
 *
 */

std::string Tetra::formatStr(const char * fmt, ...)
{
    const std::size_t BUF_LEN = 8192;
    char buf[BUF_LEN];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, BUF_LEN - 1, fmt, args);
    va_end(args);

    return std::string(buf);
}

/**
 * @brief Decode floating point value coded as 2's complement integer
 *
 */

double Tetra::decodeIntegerTwosComplement(uint32_t data, uint8_t nBits, double mult)
{
    double res;

    uint32_t val = data;

    if (val & (1 << (nBits - 1)))                                               // negative value, take the 2's complement
    {
        val = ~val;                                                             // flip bits
        val += 1;                                                               // add one
        val &= (0xFFFFFFFF >> (32 - nBits));                                    // mask bits

        res = val * (-mult) / (double)(1 << (nBits - 1));
    }
    else                                                                        // positive value
    {
        res = val * mult / (double)(1 << (nBits - 1));
    }

    return res;
}

/**
 * @brief Test if two string (converted as uppercase) are identical
 *
 */

bool Tetra::strEqualsU(const std::string txt1, const std::string txt2)
{
    std::string txt1Uc = txt1;
    std::string txt2Uc = txt2;

    auto up = [](unsigned char c) -> char { return (char)std::toupper(c); };    // std::ptr_fun removed in C++17 (OrbitSDR port)
    std::transform(txt1Uc.begin(), txt1Uc.end(), txt1Uc.begin(), up);
    std::transform(txt2Uc.begin(), txt2Uc.end(), txt2Uc.begin(), up);

    return (txt1Uc == txt2Uc);
}
