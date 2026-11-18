///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016 Edouard Griffiths, F4EXB.                                  //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#ifndef DMR_H_
#define DMR_H_

#include <string>
#include "fec.h"
#include "export.h"

#define DMR_TYPES_COUNT 12
#define DMR_SLOT_TYPE_PARITY_LEN 12

#define DMR_TS_LEN 288              //!< Length of 30ms frame in bits
#define DMR_CACH_LEN 24
#define DMR_SYNC_LEN 48

#define DMR_EMB_PART_LEN 8
#define DMR_ES_LEN 32               //!< Embedded signaling length
#define DMR_SLOT_TYPE_PART_LEN 10

#define DMR_VOCODER_FRAME_LEN 72    //!< Size in bits of Vocoder frame (20ms)
#define DMR_VOX_PART_LEN 108
#define DMR_DATA_PART_LEN 98

#define DMR_VOX_SUPERFRAME_LEN 6    //!< Count of frames in Superframe
#define IN_DIBITS(x) ((x)/2)        //!< Convert size to dibit units count
#define IN_BYTES(x) ((x)/8)
#define DMR_BP_KEYS_COUNT 255   //!< Basic Privacy Keys count

typedef enum
{
    SingleLC_FirstCSBK,      // 0
    FirstLC,                 // 1
    LastLC_CSBK,             // 2
    ContLC_CSBK              // 3
} DMRLcss;

namespace DSDcc
{

class DSDDecoder;

class DSDCC_API DSDDMR
{
public:
    typedef enum
    {
        DSDDMRBurstNone,
        DSDDMRBaseStation,       //!< BS sourced voice or data
        DSDDMRMobileStation,     //!< MS sourced voice or data
        DSDDMRMobileStationRC,   //!< MS sourced standalone RC
        DSDDMRDirectSlot1,       //!< TDMA direct mode time slot 1 voice or data
        DSDDMRDirectSlot2        //!< TDMA direct mode time slot 2 voice or data
    } DSDDMRBurstType;

    typedef enum
    {
        DSDDMRSlot1,             // 0
        DSDDMRSlot2,             // 1
        DSDDMRSlotUndefined      // 2
    } DSDDMRSlot;

    typedef enum
    {
        DSDDMRDataPIHeader,         // 0
        DSDDMRDataVoiceLCHeader,    // 1
        DSDDMRDataTerminatorWithLC, // 2
        DSDDMRDataCSBK,             // 3
        DSDDMRDataMBCHeader,        // 4
        DSDDMRDataMBCContinuation,  // 5
        DSDDMRDataDataHeader,       // 6
        DSDDMRDataRate_1_2_Data,    // 7
        DSDDMRDataRate_3_4_Data,    // 8
        DSDDMRDataIdle,             // 9
        DSDDMRDataRate_1,           // 10
        DSDDMRDataUnifiedSingleBlock, // 11
        DSDDMRDataReserved,
        DSDDMRDataUnknown
    } DSDDMRDataTYpe;

    explicit DSDDMR(DSDDecoder *dsdDecoder);
    ~DSDDMR();

    void initData();
    void initVoice();
    void processData();
    void processVoice();
    void processSyncOrSkip();

    void initVoiceMS();
    void processVoiceMS();
    void processSkipMS();
    void initDataMS();
    void processDataMS();

    const char *getSlot0Text() const;
    const char *getSlot1Text() const;
    unsigned char getColorCode() const;

    // ---- OrbitSDR additions (see dmr_ext.cpp) ----
    // What one time slot carries: the addresses and options of the current call (from the
    // voice LC header, the embedded LC of the voice superframes and the terminator), the
    // privacy (PI) header of an encrypted call, talker alias, GPS, the last control block.
    struct SlotInfo
    {
        unsigned int seq = 0;          //!< bumped on every update (the reader polls it)
        bool haveLC = false;
        unsigned int src = 0, dst = 0;
        bool group = true;
        int flco = -1, fid = -1;
        int svcOpts = 0;
        bool emergency = false, privacy = false, broadcast = false, ovcm = false;
        int priority = 0;
        bool havePI = false;           //!< PI header: the call is encrypted
        int algId = 0, keyId = 0, piFid = 0;
        unsigned int mi = 0;
        char alias[64] = {0};          //!< talker alias (UTF-8)
        unsigned char aliasRaw[4][7] = {{0}};
        unsigned int aliasMask = 0;
        bool haveGps = false;
        double lat = 0.0, lon = 0.0;
        int posErr = 0;
        int lastDataType = -1;         //!< slot type of the last data burst (DSDDMRDataTYpe)
        bool callEnded = false;        //!< terminator seen
        int csbkOpcode = -1, csbkFid = -1;
        unsigned int csbkCount = 0;
        unsigned int csbkSrc = 0, csbkDst = 0;
        unsigned int dataSrc = 0, dataDst = 0; //!< data header (SMS, GPS reports ...)
        unsigned int lcOk = 0, lcBad = 0;
        // OrbitSDR: the last data header (CRC checked), bumped dataSeq on each one
        unsigned int dataSeq = 0;
        int dataDpf = -1;              //!< data packet format: 0 UDT, 1 response, 2 unconfirmed, 3 confirmed, 13-15 short data / proprietary
        int dataSap = -1;              //!< service access point: 0 UDT, 2/3 compressed TCP/UDP, 4 IP, 5 ARP, 9 proprietary, 10 short data
        int dataBlocks = 0;            //!< blocks that follow the header
        int dataUdtFormat = -1;        //!< UDT: the format of the appended blocks
        bool dataGroup = false;
        // OrbitSDR: the last text message (SMS) reassembled and CRC checked, bumped textSeq
        unsigned int textSeq = 0;
        unsigned int textSrc = 0, textDst = 0;
        bool textGroup = false;
        char textKind[16] = {0};       //!< "UDT 7-bit", "UTF-16BE/IP", "TMS" ...
        char text[256] = {0};
        unsigned int crcOk = 0;        //!< bursts whose CRC / Reed-Solomon checked: the signal is real
    };
    const SlotInfo& getSlotInfo(int slot) const { return m_slotInfo[slot & 1]; }
    unsigned char getSlotTypeColorCode() const { return m_colorCode; }
    unsigned int getSyncCount() const { return m_syncCount; }

private:
    struct DMRAddresses
    {
        DMRAddresses() :
            m_group(false),
            m_target(0),
            m_source(0)
        {
        }

        bool         m_group;
        unsigned int m_target;
        unsigned int m_source;
    };

    void processDataFirstHalf(unsigned int shiftBack);  //!< Because sync is in the middle of a frame you need to process the first half first: CACH to end of SYNC
    void processVoiceFirstHalf(unsigned int shiftBack); //!< Because sync is in the middle of a frame you need to process the first half first: CACH to end of SYNC
    void decodeCACH(unsigned char *cachBits);
    void processSlotTypePDU();
    bool processEMB();
    bool processVoiceEmbeddedSignalling(int& voiceEmbSig_dibitsIndex, unsigned char *voiceEmbSigRawBits, bool& voiceEmbSig_OK, DMRAddresses& addresses);
    void processVoiceDibit(unsigned char dibit);
    void processDataDibit(unsigned char dibit);
    void storeSymbolDV(unsigned char *mbeFrame, int dibitindex, unsigned char dibit, bool invertDibit = false);
    static void textVoiceEmbeddedSignalling(DMRAddresses& addresses, char *slotText);

    void processVoiceFirstHalfMS();
    void processDataFirstHalfMS();

    void BasicPrivacyXOR(unsigned char *dibit, int pos);

    // ---- OrbitSDR additions ----
    void handleLC(const unsigned char *lc, int slot, int origin); //!< origin 0 header, 1 embedded, 2 terminator
    bool embeddedLC(const unsigned char *rawBits, DMRAddresses& addresses);
    void processDataBurst();
    void updateAlias(SlotInfo& si);
    SlotInfo m_slotInfo[2];
    // OrbitSDR: reassembly of a data transmission (data header + rate 1/2 blocks) per slot
    struct DataAssembly
    {
        bool udt = false, packet = false, confirmed = false, group = false;
        int format = 0, padNibbles = 0, padOctets = 0, sap = 0;
        int blocksExpected = 0, blocksReceived = 0, nextSerial = -1, dataBytes = 0;
        unsigned int src = 0, dst = 0;
        unsigned char data[160 * 12];
    };
    DataAssembly m_dataAsm[2];
    void parseDataHeader(const unsigned char *h, int slot);
    void parseUdtBlock(const unsigned char *b, int slot);
    void parsePacketBlock(const unsigned char *b, int slot);
    void publishPacketText(int slot);
    void setText(int slot, unsigned int src, unsigned int dst, bool group, const char *kind, const std::string& text);
    unsigned char m_dataBits[196];
    bool m_slotTypeOK = false;
    unsigned int m_syncCount = 0;

    DSDDecoder *m_dsdDecoder;
    int  m_symbolIndex;                   //!< current symbol index in non HD sequence
    int  m_cachSymbolIndex;               //!< count of symbols since last positive CACH identification
    DSDDMRBurstType m_burstType;
    DSDDMRSlot m_slot;
    bool m_continuation;
    bool m_cachOK;
    unsigned char m_lcss;
    unsigned char m_colorCode;
    DSDDMRDataTYpe m_dataType;
    char *m_slotText;
    unsigned char m_slotTypePDU_dibits[10];
    unsigned char m_cachBits[24];
    unsigned char m_emb_dibits[8];
    unsigned char m_voiceEmbSig_dibits[16];
    unsigned char m_voice1EmbSigRawBits[16*8];
    int           m_voice1EmbSig_dibitsIndex;
    bool          m_voice1EmbSig_OK;
    DMRAddresses  m_slot1Addresses;
    unsigned char m_voice2EmbSigRawBits[16*8];
    int           m_voice2EmbSig_dibitsIndex;
    bool          m_voice2EmbSig_OK;
    DMRAddresses  m_slot2Addresses;
    unsigned char m_syncDibits[24];
    unsigned int m_voice1FrameCount; //!< current frame count in voice superframe: [0..5] else no superframe on going
    unsigned int m_voice2FrameCount; //!< current frame count in voice superframe: [0..5] else no superframe on going
    unsigned char m_mbeDVFrame[9];

    Hamming_7_4 m_hamming_7_4;
    Golay_20_8 m_golay_20_8;
    QR_16_7_6 m_qr_16_7_6;
    Hamming_16_11_4 m_hamming_16_11_4;

    const int *w, *x, *y, *z;

    static const int m_cachInterleave[24];
    static const int m_embSigInterleave[128];
    static const char *m_slotTypeText[DMR_TYPES_COUNT];

    static const int rW[36];
    static const int rX[36];
    static const int rY[36];
    static const int rZ[36];
    static const unsigned short BasicPrivacyKeys[DMR_BP_KEYS_COUNT];
};

} // namespace DSDcc



#endif /* DMR_H_ */
