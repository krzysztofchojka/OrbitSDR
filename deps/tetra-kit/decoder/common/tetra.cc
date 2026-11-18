#include "tetra.h"

/**
 * @brief Returns MAC logical channel name
 *
 */

std::string Tetra::macLogicalChannelName(MacLogicalChannel channel)
{
    std::string ret = "";

    switch (channel)
    {
    case AACH:
        ret = "AACH";
        break;

    case BLCH:
        ret = "BLCH";
        break;

    case BNCH:
        ret = "BNCH";
        break;

    case BSCH:
        ret = "BSCH";
        break;

    case SCH_F:
        ret = "SCH_F";
        break;

    case SCH_HD:
        ret = "SCH_HD";
        break;

    case STCH:
        ret = "STCH";
        break;

    case TCH_S:
        ret = "TCH_S";
        break;

    case unknown:
        ret = "unknown";
        break;

    default:
        ret = "out of range";
        break;
    }

    return ret;
}

/**
 * @brief Convert binary value to TETRA (external subscriber) digit
 *
 */

char Tetra::getTetraDigit(const uint8_t val)
{
    const char digits[14] = "0123456789*#+";
    char res;

    if (val < 13)
    {
        res = digits[val];
    }
    else
    {
        res = '?';
    }

    return res;
}
