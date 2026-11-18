// SPDX-License-Identifier: GPL-3.0-or-later
// AMBE 3600x2400 and 3600x2450 frame level: C0/C1 ECC, PN descrambling, parameter-bit
// extraction (mbelib-neo semantics) and the inverse frame assembly.
#include "internal/config.h"
#include <mbevocoder/mbevocoder.h>

#include "internal/bits.hpp"
#include "internal/ecc.hpp"

#include <cstdio>
#include <cstring>

using namespace mbe;

namespace {

// C0 is a Golay(24,12) word at fr[0][23..0]: bits 23..1 the (23,12) codeword, bit 0 parity.
// mbelib semantics, kept for error-count parity: the (23,12) decoder corrects the data bits
// only and passes the Golay parity bits through; when it reports no data errors but the
// 24-bit word has odd parity, the overall parity bit is flipped and one error is counted.
// The AMBE-3000R treats a C0 word the extended Golay code cannot correct (four errors, or an
// even number above: the overall parity still fails after the (23,12) correction) as a bad
// frame and repeats the previous model; mbelib's data-bit count never reaches its repeat
// threshold on such a word, so the frame decoded as garbage. Under MBEVOCODER_CHIP_MEASURED the
// count reports 4 for a word detected as uncorrectable, which is what the decoders' repeat
// rule (c0 >= 4) needs (docs/decoder-quality.md, "Bit errors").
#if MBEVOCODER_CHIP_MEASURED
int c0Uncorrectable(const char fr0[24])
{
    uint32_t cw = 0;
    for (int j = 23; j >= 0; --j) cw = (cw << 1) | static_cast<uint32_t>(fr0[j] & 1);
    return ecc::golay24Correct(&cw) >= 4;
}
#endif

int eccC0(char fr[4][24])
{
#if MBEVOCODER_CHIP_MEASURED
    const bool uncorrectable = c0Uncorrectable(fr[0]) != 0;
#endif
    char in[23], out[23];
    for (int j = 0; j < 23; ++j) in[j] = fr[0][j + 1];
    int errs = mbe_golay2312(in, out);
    for (int j = 0; j < 23; ++j) fr[0][j + 1] = out[j];
    if (errs == 0) {
        int ones = 0;
        for (int j = 0; j < 24; ++j) ones += fr[0][j] & 1;
        if (ones & 1) { fr[0][0] ^= 1; errs = 1; }
    }
#if MBEVOCODER_CHIP_MEASURED
    if (uncorrectable) errs = 4;
#endif
    return errs;
}

#ifndef MBE_SOFT_C0_TRUST
#define MBE_SOFT_C0_TRUST 2.0   // see eccC0Soft; 0 restores the hard rule (experiments)
#endif

int eccC0Soft(mbe_soft_bit fr[4][24])
{
    mbe_soft_bit in[23];
    char out[23];
    for (int j = 0; j < 23; ++j) in[j] = fr[0][j + 1];
#if MBEVOCODER_CHIP_MEASURED
    char hard[24];
    for (int j = 0; j < 24; ++j) hard[j] = static_cast<char>(fr[0][j].bit & 1u);
    const bool uncorrectable = c0Uncorrectable(hard) != 0;
#endif
    int errs = mbe_golay2312Soft(in, out);
    for (int j = 0; j < 23; ++j) fr[0][j + 1].bit = static_cast<uint8_t>(out[j] & 1);
    if (errs == 0) {
        int ones = 0;
        for (int j = 0; j < 24; ++j) ones += fr[0][j].bit & 1u;
        if (ones & 1) { fr[0][0].bit ^= 1u; errs = 1; }
    }
#if MBEVOCODER_CHIP_MEASURED
    // The hard decisions say "detected uncorrectable" (a weight-4 pattern), which repeats the
    // frame; the soft search may still have found the word. Trust it when the bits it overrules
    // are weak: their reliabilities sum to at most MBE_SOFT_C0_TRUST times the word's mean
    // reliability (scale-free: any LLR scaling). Simulated over AWGN, BPSK:
    // at 2.0 it accepts 94-97 % of these frames at 1-2 dB Eb/N0, 99.5-100 % of them right, and
    // 79 % at -1 dB, 91 % right; the hard rule repeated every one.
    if (uncorrectable) {
        uint32_t data = 0;
        for (int j = 22; j >= 11; --j) data = (data << 1) | static_cast<uint32_t>(out[j] & 1);
        const uint32_t cw = ecc::golay24Encode(data);
        double overruled = 0.0, total = 0.0;
        for (int j = 0; j < 24; ++j) {
            total += fr[0][j].reliability;
            if (static_cast<int>((cw >> j) & 1u) != hard[j]) overruled += fr[0][j].reliability;
        }
        if (MBE_SOFT_C0_TRUST > 0.0 && total > 0.0 && overruled <= MBE_SOFT_C0_TRUST * total / 24.0) {
            // the soft word stands: set its parity bit and report its distance from the hard word
            int dist = 0;
            for (int j = 0; j < 24; ++j) dist += static_cast<int>((cw >> j) & 1u) != hard[j];
            fr[0][0].bit = static_cast<uint8_t>(cw & 1u);
            errs = dist < 4 ? dist : 3;   // below the repeat threshold: the frame decodes
        } else {
            errs = 4;
        }
    }
#endif
    return errs;
}

#if MBEVOCODER_CHIP_MEASURED
// AMBE+2 error counts as TIA-102.BABA-1 5.5 counts them: every received bit, parity included,
// that differs from the corrected codeword (C0 with its extended parity bit; 4 when C0 is
// detected uncorrectable), C1 after descrambling; soft bits weighted by their reliability, in
// units of a fully reliable bit (mbelib-neo 2.5+ counts the same way). The decoder's error-rate
// mute and repeat rule read these counts; with them the standard's recursion and threshold
// (0.096) mute what the AMBE-3000R mutes (docs/decoder-quality.md, "Bit errors").
int fullC0(const char rx[24], const char fr0[24], int c0Count)
{
    if (c0Count >= 4) return 4;
    uint32_t u0 = 0;
    for (int j = 23; j >= 12; --j) u0 = (u0 << 1) | static_cast<uint32_t>(fr0[j] & 1);
    const uint32_t cw = ecc::golay24Encode(u0);
    int n = 0;
    for (int j = 0; j < 24; ++j) n += static_cast<int>((cw >> j) & 1u) != (rx[j] & 1);
    return n < 4 ? n : 3;
}

int fullC0Soft(const mbe_soft_bit rx[24], const mbe_soft_bit fr0[24], int c0Count)
{
    if (c0Count >= 4) return 4;
    uint32_t u0 = 0;
    for (int j = 23; j >= 12; --j) u0 = (u0 << 1) | static_cast<uint32_t>(fr0[j].bit & 1u);
    const uint32_t cw = ecc::golay24Encode(u0);
    int cost = 0;
    for (int j = 0; j < 24; ++j)
        if (static_cast<unsigned>((cw >> j) & 1u) != (rx[j].bit & 1u)) cost += rx[j].reliability;
    const int n = (cost + 127) / 255;
    return n < 4 ? n : 3;
}

// C1 after descrambling: the received word against the corrected data re-encoded.
uint32_t c1Codeword(const char* d)
{
    uint32_t u1 = 0;
    for (int i = 12; i < 24; ++i) u1 = (u1 << 1) | static_cast<uint32_t>(d[i] & 1);
    return ecc::golay23Encode(u1);
}

int fullC1(const char rx[23], const char* d)
{
    const uint32_t cw = c1Codeword(d);
    int n = 0;
    for (int j = 0; j < 23; ++j) n += static_cast<int>((cw >> j) & 1u) != (rx[j] & 1);
    return n;
}

int fullC1Soft(const mbe_soft_bit rx[23], const char* d)
{
    const uint32_t cw = c1Codeword(d);
    int cost = 0;
    for (int j = 0; j < 23; ++j)
        if (static_cast<unsigned>((cw >> j) & 1u) != (rx[j].bit & 1u)) cost += rx[j].reliability;
    return (cost + 127) / 255;
}
#endif

#if MBEVOCODER_CHIP_MEASURED
// D-STAR's C1 is a Golay(24,12) word whose overall parity bit sits among the unprotected cells
// (ambe_d[24], fr[2][10]), scrambled like the rest: 1 when the received bit differs from the
// corrected word's.
unsigned dstarC1ParityExpected(const char* d)
{
    const uint32_t u0 = bits::fromAscending(d, 0, 12);
    const uint32_t u1 = bits::fromAscending(d, 12, 12);
    return (ecc::golay24Encode(u1) ^ ecc::pn24(u0)) & 1u;
}
#endif

// Error counts of the frame decoders: mbelib's corrected data bits, or (MBEVOCODER_CHIP_MEASURED)
// the full weight, D-STAR's C1 parity bit included.
enum class Counts { Data, Full2450, Full2400 };

template <typename Cell, typename Get, typename Set>
void demodulate(Cell fr[4][24], Get get, Set set)
{
    uint32_t seed = 0;
    for (int i = 23; i >= 12; --i) seed = (seed << 1) | get(fr[0][i]);
    char pr[23];
    ecc::pnSequence(seed, pr, 23);
    int k = 0;
    for (int j = 22; j >= 0; --j) set(fr[1][j], get(fr[1][j]) ^ static_cast<unsigned>(pr[k++]));
}

int eccData(char fr[4][24], char* d)
{
    int k = 0;
    for (int j = 23; j > 11; --j) d[k++] = fr[0][j];
    char gin[23], gout[23];
    for (int j = 0; j < 23; ++j) gin[j] = fr[1][j];
    int errs = mbe_golay2312(gin, gout);
    for (int j = 22; j > 10; --j) d[k++] = gout[j];
    for (int j = 10; j >= 0; --j) d[k++] = fr[2][j];
    for (int j = 13; j >= 0; --j) d[k++] = fr[3][j];
    return errs;
}

int eccDataSoft(mbe_soft_bit fr[4][24], char* d)
{
    int k = 0;
    for (int j = 23; j > 11; --j) d[k++] = static_cast<char>(fr[0][j].bit & 1u);
    mbe_soft_bit gin[23];
    char gout[23];
    for (int j = 0; j < 23; ++j) gin[j] = fr[1][j];
    int errs = mbe_golay2312Soft(gin, gout);
    for (int j = 22; j > 10; --j) d[k++] = gout[j];
    for (int j = 10; j >= 0; --j) d[k++] = static_cast<char>(fr[2][j].bit & 1u);
    for (int j = 13; j >= 0; --j) d[k++] = static_cast<char>(fr[3][j].bit & 1u);
    return errs;
}

int decodeFrame(const char in[4][24], char d[49], mbe_process_result* result, Counts counts)
{
    if (result) mbe_initProcessResult(result);
    if (!d) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(&in[0][0], 96);
    if (ret < 0) return ret;
    char fr[4][24];
    std::memcpy(fr, in, sizeof fr);
    int c0 = eccC0(fr);
    const bool fullCount = counts != Counts::Data;
#if MBEVOCODER_CHIP_MEASURED
    if (fullCount) c0 = fullC0(in[0], fr[0], c0);
#else
    (void)fullCount;
#endif
    demodulate(fr, [](char c) { return static_cast<unsigned>(c & 1); }, [](char& c, unsigned v) { c = static_cast<char>(v & 1u); });
#if MBEVOCODER_CHIP_MEASURED
    char c1rx[23];
    std::memcpy(c1rx, fr[1], sizeof c1rx);
#endif
    int prot = eccData(fr, d);
#if MBEVOCODER_CHIP_MEASURED
    if (fullCount) prot = fullC1(c1rx, d);
    if (counts == Counts::Full2400) prot += static_cast<int>((static_cast<unsigned>(in[2][10] & 1) ^ dstarC1ParityExpected(d)) & 1u);
#endif
    if (result) {
        result->c0_errors = c0;
        result->protected_errors = prot;
        result->total_errors = c0 + prot;
        result->flags = MBE_PROCESS_FLAG_C0_VALID;
#if MBEVOCODER_CHIP_MEASURED
        if (c0 >= 4) result->flags |= MBE_PROCESS_FLAG_C0_UNCORRECTABLE;
#endif
    }
    return c0 + prot;
}

int decodeSoftFrame(const mbe_soft_bit in[4][24], char d[49], mbe_process_result* result, Counts counts)
{
    if (result) mbe_initProcessResult(result);
    if (!d) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validateSoft(&in[0][0], 96);
    if (ret < 0) return ret;
    mbe_soft_bit fr[4][24];
    std::memcpy(fr, in, sizeof fr);
    int c0 = eccC0Soft(fr);
    const bool fullCount = counts != Counts::Data;
#if MBEVOCODER_CHIP_MEASURED
    if (fullCount) c0 = fullC0Soft(in[0], fr[0], c0);
#else
    (void)fullCount;
#endif
    demodulate(fr, [](const mbe_soft_bit& c) { return static_cast<unsigned>(c.bit & 1u); },
               [](mbe_soft_bit& c, unsigned v) { c.bit = static_cast<uint8_t>(v & 1u); });
#if MBEVOCODER_CHIP_MEASURED
    mbe_soft_bit c1rx[23];
    std::memcpy(c1rx, fr[1], sizeof c1rx);
#endif
    int prot = eccDataSoft(fr, d);
#if MBEVOCODER_CHIP_MEASURED
    if (fullCount) prot = fullC1Soft(c1rx, d);
    if (counts == Counts::Full2400 && ((in[2][10].bit & 1u) ^ dstarC1ParityExpected(d)) != 0u)
        prot += (in[2][10].reliability + 127) / 255;
#endif
    if (result) {
        result->c0_errors = c0;
        result->protected_errors = prot;
        result->total_errors = c0 + prot;
        result->flags = MBE_PROCESS_FLAG_C0_VALID | MBE_PROCESS_FLAG_SOFT_INPUT;
#if MBEVOCODER_CHIP_MEASURED
        if (c0 >= 4) result->flags |= MBE_PROCESS_FLAG_C0_UNCORRECTABLE;
#endif
    }
    return c0 + prot;
}

// Inverse of decodeFrame. For D-STAR (c1Parity24) the 24th bit of C1 is a Golay(24,12)
// parity bit that mbelib exposes as ambe_d[24]; it is recomputed here.
int encodeFrame(const char d[49], char fr[4][24], bool c1Parity24)
{
    if (!fr) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(d, 49);
    if (ret < 0) return ret;
    std::memset(fr, 0, 4 * 24);
    uint32_t u0 = bits::fromAscending(d, 0, 12);
    uint32_t u1 = bits::fromAscending(d, 12, 12);
    uint32_t c0 = ecc::golay24Encode(u0);
    bits::toDescending(c0, fr[0], 23, 0);
    uint32_t c1 = ecc::golay23Encode(u1);
    uint32_t pn = ecc::pn24(u0);
    uint32_t c1Scrambled = c1 ^ (pn >> 1);
    bits::toDescending(c1Scrambled, fr[1], 22, 0);
    for (int j = 10; j >= 0; --j) fr[2][j] = d[24 + (10 - j)];
    for (int j = 13; j >= 0; --j) fr[3][j] = d[35 + (13 - j)];
    if (c1Parity24) {
        // fr[2][10] (= ambe_d[24]) carries the overall parity of the scrambled 24-bit C1 word,
        // i.e. golay24(u1) ^ pn24(u0) with the parity bit also scrambled.
        uint32_t c1Full = ecc::golay24Encode(u1) ^ pn;
        fr[2][10] = static_cast<char>(c1Full & 1u);
    }
    return 0;
}

void dumpData(const char* d)
{
    if (!d) return;
    for (int i = 0; i < 49; ++i) std::fprintf(stderr, "%i", d[i]);
    std::fprintf(stderr, " ");
}

void dumpFrame(const char fr[4][24])
{
    if (!fr) return;
    std::fprintf(stderr, "ambe_fr c0: ");
    for (int j = 23; j >= 0; --j) std::fprintf(stderr, "%i", fr[0][j]);
    std::fprintf(stderr, " ambe_fr c1: ");
    for (int j = 22; j >= 0; --j) std::fprintf(stderr, "%i", fr[1][j]);
    std::fprintf(stderr, " ambe_fr c2: ");
    for (int j = 10; j >= 0; --j) std::fprintf(stderr, "%i", fr[2][j]);
    std::fprintf(stderr, " ambe_fr c3: ");
    for (int j = 13; j >= 0; --j) std::fprintf(stderr, "%i", fr[3][j]);
    std::fprintf(stderr, " ");
}

} // namespace

extern "C" {

void mbe_dumpAmbe2400Data(const char* d) { dumpData(d); }
void mbe_dumpAmbe2450Data(const char* d) { dumpData(d); }
void mbe_dumpAmbe3600x2400Frame(const char fr[4][24]) { dumpFrame(fr); }
void mbe_dumpAmbe3600x2450Frame(const char fr[4][24]) { dumpFrame(fr); }

int mbe_eccAmbe3600x2400C0(char fr[4][24])
{
    int ret = bits::validate(&fr[0][0], 96);
    return ret < 0 ? ret : eccC0(fr);
}
int mbe_eccAmbe3600x2450C0(char fr[4][24])
{
#if MBEVOCODER_CHIP_MEASURED
    // the full-weight count (mbelib-neo 2.6), parity bits included
    int ret = bits::validate(&fr[0][0], 96);
    if (ret < 0) return ret;
    char rx[24];
    std::memcpy(rx, fr[0], sizeof rx);
    return fullC0(rx, fr[0], eccC0(fr));
#else
    return mbe_eccAmbe3600x2400C0(fr);
#endif
}

int mbe_eccAmbe3600x2400Data(char fr[4][24], char* d)
{
    if (!d) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(&fr[0][0], 96);
    return ret < 0 ? ret : eccData(fr, d);
}
int mbe_eccAmbe3600x2450Data(char fr[4][24], char* d)
{
#if MBEVOCODER_CHIP_MEASURED
    // the full-weight C1 count (mbelib-neo 2.6): the descrambled word against the corrected one
    int ret = mbe_eccAmbe3600x2400Data(fr, d);
    return ret < 0 ? ret : fullC1(fr[1], d);
#else
    return mbe_eccAmbe3600x2400Data(fr, d);
#endif
}

int mbe_demodulateAmbe3600x2400Data(char fr[4][24])
{
    int ret = bits::validate(&fr[0][0], 96);
    if (ret < 0) return ret;
    demodulate(fr, [](char c) { return static_cast<unsigned>(c & 1); }, [](char& c, unsigned v) { c = static_cast<char>(v & 1u); });
    return 0;
}
int mbe_demodulateAmbe3600x2450Data(char fr[4][24]) { return mbe_demodulateAmbe3600x2400Data(fr); }

int mbe_decodeAmbe3600x2400Frame(const char fr[4][24], char d[49], mbe_process_result* r) { return decodeFrame(fr, d, r, Counts::Full2400); }
int mbe_decodeAmbe3600x2450Frame(const char fr[4][24], char d[49], mbe_process_result* r) { return decodeFrame(fr, d, r, Counts::Full2450); }
int mbe_decodeAmbe3600x2400SoftFrame(const mbe_soft_bit fr[4][24], char d[49], mbe_process_result* r) { return decodeSoftFrame(fr, d, r, Counts::Full2400); }
int mbe_decodeAmbe3600x2450SoftFrame(const mbe_soft_bit fr[4][24], char d[49], mbe_process_result* r) { return decodeSoftFrame(fr, d, r, Counts::Full2450); }

int mbe_encodeAmbe3600x2400Frame(const char d[49], char fr[4][24]) { return encodeFrame(d, fr, true); }
int mbe_encodeAmbe3600x2450Frame(const char d[49], char fr[4][24]) { return encodeFrame(d, fr, false); }

} // extern "C"
