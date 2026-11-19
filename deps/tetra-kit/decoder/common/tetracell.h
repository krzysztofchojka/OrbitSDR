#ifndef TETRACELL_H
#define TETRACELL_H
#include <cstdint>
#include "tetra.h"

/**
 * @defgroup tetra_common TETRA cell informations
 *
 * @{
 *
 */

namespace Tetra {

    /**
     * @brief Tetra sub-system synchronisation - ch. 7
     *
     */

    class TetraCell {
    public:
        TetraCell();
        ~TetraCell();

        uint32_t getScramblingCode();
        void updateScramblingCode(const uint32_t mcc, const uint32_t mnc, const uint16_t colorCode);
        bool isCellInformationsAcquired();
        void setFrequencies(int32_t downlinkFrequency, int32_t uplinkFrequency);
        void setLocationArea(uint32_t la);

        // OrbitSDR: air interface security of the cell, from SYSINFO (EN 300 392-7 A.8.77)
        void setSecurity(bool aieAvailable, int securityInfo);
        bool aieKnown() const     { return m_aieKnown; }
        bool aieAvailable() const { return m_aieAvailable; }
        int  securityInfo() const { return m_securityInfo; }       ///< 8-bit Security information element, -1 = not broadcast yet

        uint32_t mcc() const;
        uint32_t mnc() const;
        uint32_t colorCode() const;
        uint32_t locationArea() const;
        int32_t  downlinkFrequency();
        int32_t  uplinkFrequency();

    private:
        uint32_t m_mcc;                                                         ///< Cell MCC
        uint32_t m_mnc;                                                         ///< Cell MNC
        uint16_t m_colorCode;                                                   ///< Cell Color code
        uint32_t m_scramblingCode;                                              ///< Cell Scrambling code
        uint32_t m_locationArea;

        bool m_aieKnown     = false;                                            ///< OrbitSDR: D-MLE-SYSINFO seen
        bool m_aieAvailable = false;                                            ///< OrbitSDR: "Air interface encryption service" (BS service details)
        int  m_securityInfo = -1;                                               ///< OrbitSDR: Security information (extended services broadcast)

        int32_t m_downlinkFrequency;                                            ///< Downlink frequency [Hz]
        int32_t m_uplinkFrequency;                                              ///< Uplink frequency [Hz]
        bool    m_cellInformationsAcquired;
    };

};

/** @} */

#endif /* TETRACELL_H */
