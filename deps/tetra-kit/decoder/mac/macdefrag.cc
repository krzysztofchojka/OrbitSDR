#include "macdefrag.h"

using namespace Tetra;

static const int DEBUG_VAL = 3;                                                 // start debug informations at this level

/**
 * @brief Defragmenter constructor
 *
 */

MacDefrag::MacDefrag(int debug_level)
{
    g_debug_level = debug_level;
    m_sdu.clear();

    m_fragmentsCount = 0;
    b_stopped        = true;
}

/**
 * @brief Defragmenter destructor
 *
 */

MacDefrag::~MacDefrag()
{
    m_sdu.clear();
}

/**
 * @brief Start defragmenter, flush previous data if already in use
 *        and report informations
 *
 * NOTE: total fragmented length is unknown
 *
 */

void MacDefrag::start(const MacAddress address, const TetraTime timeSlot)
{
    if (m_sdu.size() > 0u)
    {
        if (g_debug_level >= DEBUG_VAL)
        {
            TK_PRINTF("  * DEFRAG FAILED   : invalid %d fragments received for SSI = %u: %u recovered for address %u\n",
                   m_fragmentsCount,
                   macAddress.ssi,
                   (uint32_t)m_sdu.size(),
                   macAddress.ssi);
        }
    }

    macAddress     = address;                                                   // at this point, the defragmenter MAC address contains encryption mode
    startTime      = timeSlot;
    m_fragmentsCount = 0;

    if (g_debug_level >= DEBUG_VAL)
    {
        TK_PRINTF("  * DEFRAG START    : SSI = %u - TN/FN/MN = %02u/%02u/%02u\n",
               macAddress.ssi,
               startTime.tn,
               startTime.fn,
               startTime.mn);
    }

    m_sdu.clear();                                                              // clear the buffer

    b_stopped = false;
}

/**
 * @brief Append data to defragmenter
 *
 */

void MacDefrag::append(Pdu sdu, const MacAddress address)
{
    if (b_stopped)                                                              // we can't append if in stopped mode
    {
        if (g_debug_level >= DEBUG_VAL)
        {
            TK_PRINTF("  * DEFRAG APPEND   : FAILED SSI = %u\n", address.ssi);
        }
    }
    else if (address.ssi != macAddress.ssi)                                     // check mac addresses
    {
        stop();                                                                 // stop defragmenter

        if (g_debug_level >= DEBUG_VAL)
        {
            TK_PRINTF("  * DEFRAG APPEND   : FAILED appending SSI = %u while fragment SSI = %u\n", macAddress.ssi, address.ssi);
        }
    }
    else
    {
        m_sdu.append(sdu);
        m_fragmentsCount++;

        if (g_debug_level >= DEBUG_VAL)
        {
            std::size_t sduLen = sdu.size();
            TK_PRINTF("  * DEFRAG APPEND   : SSI = %u - TN/FN/MN = %02u/%02u/%02u - fragment %d - length = sdu %u / m_sdu %u - encr = %u\n",
                   macAddress.ssi,
                   startTime.tn,
                   startTime.fn,
                   startTime.mn,
                   m_fragmentsCount,
                   (uint32_t)sduLen,
                   (uint32_t)m_sdu.size(),
                   macAddress.encryptionMode
                );
        }
    }
}

/**
 * @brief Check SDU validity and return it
 *
 */

Pdu MacDefrag::getSdu(uint8_t * encryptionMode, uint8_t * usageMarker)
{
    Pdu ret;

    if (b_stopped)
    {
        if (g_debug_level >= DEBUG_VAL)
        {
            TK_PRINTF("  * DEFRAG END      : FAILED SSI = %u - TN/FN/MN = %02u/%02u/%02u - fragment %d - length = %u - encr = %u\n",
                   macAddress.ssi,
                   startTime.tn,
                   startTime.fn,
                   startTime.mn,
                   m_fragmentsCount,
                   (uint32_t)m_sdu.size(),
                   macAddress.encryptionMode
                );
        }
    }
    else
    {
        // FIXME add check
        *encryptionMode = macAddress.encryptionMode;
        *usageMarker    = macAddress.usageMarker;
        ret = m_sdu;
    }

    return ret;
}

/**
 * @brief Stop defragmenter
 *
 */

void MacDefrag::stop()
{
    // clean stop
    b_stopped = true;
    m_fragmentsCount = 0;
    m_sdu.clear();
}
