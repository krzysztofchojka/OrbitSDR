#include "tetracell.h"

using namespace Tetra;

/**
 * @brief Constructor
 *
 */

TetraCell::TetraCell()
{
    m_mcc               = 0;
    m_mnc               = 0;
    m_colorCode         = 0;
    m_scramblingCode    = 0;
    m_locationArea      = 0;

    m_downlinkFrequency = 0;
    m_uplinkFrequency   = 0;

    m_cellInformationsAcquired = false;
}

/**
 * @brief Destructor
 *
 */

TetraCell::~TetraCell()
{

}

/**
 * @brief Calculate cell scrambling code
 *
 * Scrambling code - see 8.2.5
 * Tetra scrambling code - 30 bits, see 23.2.1 - Figure 141
 * for synchronisation burst, MCC = MNC = ColorCode = 0
 *
 */

void TetraCell::updateScramblingCode(const uint32_t mcc, const uint32_t mnc, const uint16_t colorCode)
{
    m_mcc = mcc;
    m_mnc = mnc;
    m_colorCode = colorCode;

    uint16_t lmcc        = m_mcc & 0x03ff;                                      // 10 MSB of MCC
    uint16_t lmnc        = m_mnc & 0x3fff;                                      // 14 MSB of MNC
    uint16_t lcolor_code = m_colorCode & 0x003f;                                // 6 MSB of ColorCode

    m_scramblingCode = lcolor_code | (lmnc << 6) | (lmcc << 20);                // 30 MSB bits
    m_scramblingCode = (m_scramblingCode << 2) | 0x0003;                        // scrambling initialized to 1 on bits 31-32 - 8.2.5.2 (54)

    m_cellInformationsAcquired = true;
}

/**
 * @brief Return scrambling code
 *
 */

uint32_t TetraCell::getScramblingCode()
{
    return m_scramblingCode;
}

/**
 * @brief Returns true if cell informations are required
 *
 */

bool TetraCell::isCellInformationsAcquired()
{
    return m_cellInformationsAcquired;
}

/**
 * @brief Get MCC
 *
 */

uint32_t TetraCell::mcc() const
{
    return m_mcc;
}

/**
 * @brief Get MNC
 *
 */

uint32_t TetraCell::mnc() const
{
    return m_mnc;
}

/**
 * @brief Get color code
 *
 */

uint32_t TetraCell::colorCode() const
{
    return m_colorCode;
}

uint32_t TetraCell::locationArea() const
{
    return m_locationArea;
}

void TetraCell::setLocationArea(uint32_t la)
{
    m_locationArea = la;
}

/**
 * @brief OrbitSDR: air interface security of the cell, from SYSINFO
 *        securityInfo < 0 keeps the last one (not every SYSINFO carries it)
 *
 */

void TetraCell::setSecurity(bool aieAvailable, int securityInfo)
{
    m_aieKnown     = true;
    m_aieAvailable = aieAvailable;
    if (securityInfo >= 0)
    {
        m_securityInfo = securityInfo;
    }
}

/**
 * @brief Get downlink frequency [Hz]
 *
 */

int32_t TetraCell::downlinkFrequency()
{
    return m_downlinkFrequency;
}

/**
 * @brief Get uplink frequency [Hz]
 *
 */

int32_t TetraCell::uplinkFrequency()
{
    return m_uplinkFrequency;
}

/**
 * @brief Set downlink and uplink frequencies [Hz]
 *
 */

void TetraCell::setFrequencies(int32_t downlinkFrequency, int32_t uplinkFrequency)
{
    m_downlinkFrequency = downlinkFrequency;
    m_uplinkFrequency = uplinkFrequency;
}
