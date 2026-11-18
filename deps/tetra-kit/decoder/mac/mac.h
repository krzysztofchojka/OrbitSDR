#ifndef MAC_H
#define MAC_H
#include "../common/tetra.h"
#include "../common/tetracell.h"
#include "../common/layer.h"
#include "../common/log.h"
#include "../common/report.h"
#include "../common/utils.h"
#include "../llc/llc.h"
#include "../mle/mle.h"
#include "../uplane/uplane.h"
#include "../wiremsg/wiremsg.h"
#include "viterbi.h"
#include "macdefrag.h"

namespace Tetra {

    /**
     * @brief MAC layers class
     *
     */

    class Mac : public Layer {
    public:
        Mac(Log * log, Report * report, TetraCell * tetraCell, UPlane * uPlane, Llc * llc, Mle * mle, WireMsg * wMsg, bool bRemoveFillBits);
        ~Mac();

        void incrementTn();
        TetraTime getTime();

        void serviceLowerMac(std::vector<uint8_t> data, int burst_type);
        std::string burstName(int val);

    private:
        TetraCell * m_tetraCell;                                                ///< Tetra cell informations

        Llc    * m_llc;                                                         ///< LLC layer
        Mle    * m_mle;                                                         ///< MLE layer
        UPlane * m_uPlane;                                                      ///< U-Plane layer
        WireMsg * m_wireMsg;                                                    ///< Wireshark output

        MacDefrag * m_macDefrag;                                                ///< MAC defragmenter

        MacState   m_macState;                                                  ///< Current MAC state (from ACCESS-ASSIGN PDU)
        MacAddress m_macAddress;                                                ///< Current MAc address (from MAC-RESOURCE PDU)
        uint8_t m_usageMarkerEncryptionMode[64];                                ///< Usage marker encryption mode for U-Plane (MAC TRAFFIC)

        uint8_t m_secondSlotStolenFlag;                                         ///< 1 if second slot is stolen
        bool m_bRemoveFillBits;                                                 ///< Remove filling bits flags
        Pdu removeFillBits(const Pdu pdu);
        int32_t decodeLength(uint32_t val);

        // decoding functions per clause 8
        ViterbiCodec * m_viterbiCodec1614;                                      ///< Viterbi codec
        std::vector<uint8_t> descramble(std::vector<uint8_t> data, const int len, const uint32_t scramblingCode);
        std::vector<uint8_t> deinterleave(std::vector<uint8_t> data, const uint32_t K, const uint32_t a);
        std::vector<uint8_t> depuncture23(std::vector<uint8_t> data, const uint32_t len);
        std::vector<uint8_t> viterbiDecode1614(std::vector<uint8_t> data);
        std::vector<uint8_t> reedMuller3014Decode(std::vector<uint8_t> data);
        int checkCrc16Ccitt(std::vector<uint8_t> data, const int len);

        void serviceUpperMac(const Pdu data, MacLogicalChannel macLogicalChannel);

        Pdu  pduProcessSync(const Pdu pdu);                                                                                               // process SYNC
        void pduProcessAach(const Pdu data);                                                                                              // process ACCESS-ASSIGN - no SDU
        Pdu  pduProcessResource(const Pdu pdu, MacLogicalChannel macLogicalChannel, bool * fragmentedPacketFlag, int32_t * pduSizeInMac); // process MAC-RESOURCE
        Pdu  pduProcessSysinfo(const Pdu pdu, int32_t * pduSizeInMac);                                                                    // process SYSINFO
        void pduProcessMacFrag(const Pdu pdu);                                                                                            // process MAC-FRAG
        Pdu  pduProcessMacEnd(const Pdu pdu);                                                                                             // process MAC-END
        Pdu  pduProcessDBlock(const Pdu pdu, int32_t * pduSizeInMac);                                                                     // process MAC-D-BLCK
        void pduProcessAccessDefine(const Pdu pdu, int32_t * pduSizeInMac);                                                               // process ACCESS-DEFINE - no SDU
    };

};

#endif /* MAC_H */
