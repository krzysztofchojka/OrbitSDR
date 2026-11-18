#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>

// =====================================================================
//  TETRA speech: the ACELP decoder of ETSI EN 300 395-2.
//
//  The codec itself is not in the repository. scripts/tetracodec.sh
//  downloads ETSI's reference source, checks it and builds its decoder
//  part into .cache/ (ENABLE_TETRA_CODEC). Without it, speechAvailable()
//  is false and nothing here is compiled in.
//
//  One TCH/S time slot (432 bits) carries two 30 ms speech frames:
//    de-interleave -> channel decoding (RCPC + CRC, gives a bad frame
//    flag) -> 2 x 137 bits -> ACELP synthesis -> 2 x 240 samples, 8 kHz.
//  A bad frame is not dropped: the decoder conceals it from the last
//  good one, as a radio does.
//
//  The reference code keeps its whole state (synthesis memories, the
//  channel decoder's first-pass flag) in globals, so one process can run
//  one speech stream at a time. SpeechCodec serialises access; the caller
//  (tetra::Core) makes sure only one channel / time slot owns it.
// =====================================================================

#ifdef ENABLE_TETRA_CODEC
extern "C" {
int16_t Desinterleaving_Speech(int16_t Input_Frame[], int16_t Output_frame[]);
int16_t Channel_Decoding(short first_pass, int16_t Frame_Stealing, int16_t Input_Frame[], int16_t Output_Frame[]);
void    Init_Decod_Tetra(void);
void    Bits2prm_Tetra(int16_t* bits, int16_t prm[]);
void    Decod_Tetra(int16_t parm[], int16_t synth[]);
void    Post_Process(int16_t signal[], int16_t lg);
}
#endif

namespace tetra {

inline bool speechAvailable() {
#ifdef ENABLE_TETRA_CODEC
    return true;
#else
    return false;
#endif
}

class SpeechCodec {
public:
    static constexpr int SLOT_BITS = 432;      // one TCH/S time slot
    static constexpr int SLOT_SAMPLES = 480;   // 2 x 240 samples at 8 kHz (60 ms)

    // soft[i]: +127 for a 0 bit, -127 for a 1 bit (the reference format).
    // restart: a new stream begins (resets the synthesis memories).
    // Returns how many of the slot's two speech frames passed the CRC (0..2),
    // or -1 when this build has no codec.
    static int decodeSlot(const int16_t soft[SLOT_BITS], int16_t pcm[SLOT_SAMPLES], bool restart) {
#ifdef ENABLE_TETRA_CODEC
        std::lock_guard<std::mutex> l(mutex());
        static bool firstPass = true;
        static bool initialised = false;
        if (restart || !initialised) { Init_Decod_Tetra(); firstPass = true; initialised = true; }

        int16_t interleaved[SLOT_BITS], coded[SLOT_BITS], reordered[286];
        std::memcpy(interleaved, soft, sizeof(interleaved));
        Desinterleaving_Speech(interleaved, coded);
        const int16_t bfi = Channel_Decoding(firstPass ? 1 : 0, 0, coded, reordered);
        firstPass = false;

        int16_t serial[138], parm[25];
        for (int f = 0; f < 2; f++) {
            serial[0] = bfi;                               // the bad frame flag covers both frames
            std::memcpy(serial + 1, reordered + 137 * f, 137 * sizeof(int16_t));
            Bits2prm_Tetra(serial, parm);
            Decod_Tetra(parm, pcm + 240 * f);
            Post_Process(pcm + 240 * f, 240);
        }
        return bfi ? 0 : 2;
#else
        (void)soft; (void)restart;
        std::memset(pcm, 0, SLOT_SAMPLES * sizeof(int16_t));
        return -1;
#endif
    }

private:
    static std::mutex& mutex() { static std::mutex m; return m; }
};

} // namespace tetra
