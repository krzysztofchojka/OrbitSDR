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

#ifndef DSDCC_DSD_MBE_H_
#define DSDCC_DSD_MBE_H_

#include "dsd_filters.h"
#include "export.h"

namespace DSDcc
{

class DSDDecoder;
struct DSDmbelibParms;

class DSDCC_API DSDMBEDecoder
{
public:
    explicit DSDMBEDecoder(DSDDecoder *dsdDecoder);
    ~DSDMBEDecoder();

    void initMbeParms();
    void processFrame(char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]);
    //! quality: the bit errors the protocol's own code corrected in the frame (-1: not known)
    void processData(char imbe_data[88], char ambe_data[49], int quality = -1);

    // OrbitSDR: the voice gate. A sync pattern matched by chance in noise starts the decoding of
    // frames that are noise: played, they crackle. Every frame's FEC error count tells: on noise
    // the Golay words of the AMBE frames always need 4 or more corrections, on a received signal
    // mostly none. The frames wait (up to GATE_PENDING of them) until two of them have at most
    // GATE_GOOD errors, then they are all played; noise never opens the gate. It closes after
    // GATE_CLOSE noise-like frames in a row, or when no frame came for GATE_GAP samples.
    bool gateOpen() const { return m_gateOpen; }
    unsigned long long lastGoodSample() const { return m_gateLastGood; }
    void resetGate();

    short *getAudio(int& nbSamples)
    {
        nbSamples = m_audio_out_nb_samples;
        return m_audio_out_buf;
    }

    void resetAudio()
    {
        m_audio_out_nb_samples = 0;
        m_audio_out_buf_p = m_audio_out_buf;
    }

    void setAudioGain(float aout_gain) { m_aout_gain = aout_gain; }
    void setAutoGain(bool auto_gain) { m_auto_gain = auto_gain; }
    void setVolume(float volume) { m_volume = volume; }
    void setStereo(bool stereo) { m_stereo = stereo; }
    void setChannels(unsigned char channels) { m_channels = channels % 4; }
    void setUpsamplingFactor(int upsample) { m_upsample = upsample; }
    int getUpsamplingFactor() const { return m_upsample; }
    void useHP(bool useHP) { m_upsamplingFilter.useHP(useHP); }

private:
    static const int GATE_PENDING = 4;
    static const int GATE_GOOD = 2;      //!< errors in a frame that noise never has
    static const int GATE_BAD = 4;       //!< errors noise always has
    static const int GATE_CLOSE = 6;     //!< noise-like frames in a row that close the gate (120 ms)
    static const unsigned long long GATE_GAP = 48000 / 4;   //!< a quarter of a second without frames
    void gateFrame(int quality);
    bool m_gateOpen;
    int m_gateBadRun;
    int m_gatePendingCount;
    int m_gatePendingGood;
    unsigned long long m_gateLastFrame;
    unsigned long long m_gateLastGood;
    float m_gatePending[GATE_PENDING][160];
    bool m_gatePendingIsGood[GATE_PENDING];

    void processAudio();
    void upsample(int upsampling, float invalue);

    DSDDecoder *m_dsdDecoder;
    char imbe_d[88];
    char ambe_d[49];
    float m_upsamplerLastValue;

    DSDmbelibParms *m_mbelibParms;
    int m_errs;
    int m_errs2;
    char m_err_str[64];

    float m_audio_out_temp_buf[160];   //!< output of decoder
    float *m_audio_out_temp_buf_p;

    float m_audio_out_float_buf[1120]; //!< output of upsampler - 1 frame of 160 samples upampled up to 7 times
    float *m_audio_out_float_buf_p;

    float m_aout_max_buf[200];
    float *m_aout_max_buf_p;
    int m_aout_max_buf_idx;

    short m_audio_out_buf[2*48000];    //!< final result - 1s of L+R S16LE samples
    short *m_audio_out_buf_p;
    int   m_audio_out_nb_samples;
    int   m_audio_out_buf_size;
    int   m_audio_out_idx;
    int   m_audio_out_idx2;

    float m_aout_gain;
    float m_volume;
    bool m_auto_gain;
    int m_upsample;            //!< upsampling factor
    bool m_stereo;             //!< double each audio sample to produce L+R channels
    unsigned char m_channels;  //!< when in stereo output to none (0) or only left (1), right (2) or both (3) channels

    DSDMBEAudioInterpolatorFilter m_upsamplingFilter;
};

}

#endif /* DSDCC_DSD_MBE_H_ */
