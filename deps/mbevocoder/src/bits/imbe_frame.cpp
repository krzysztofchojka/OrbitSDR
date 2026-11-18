// SPDX-License-Identifier: GPL-3.0-or-later
// IMBE 7200x4400 and 7100x4400 frame level (mbelib-neo semantics) plus 7200 frame assembly.
#include "internal/config.h"
#include <mbevocoder/mbevocoder.h>

#include "internal/bits.hpp"
#include "internal/ecc.hpp"

#include <cstdio>
#include <cstring>

using namespace mbe;

namespace {

constexpr size_t kBits7200 = 8 * 23;
constexpr size_t kBits7100 = 7 * 24;


// The bit errors a Golay(23,12) word held: every corrected bit, parity included, which is what
// TIA-102.BABA's frame repeat and muting rules count (epsilon_0..3) and JMBE counts.
// mbe_golay2312 returns mbelib's count of corrected data bits only, which leaves a random frame
// below the repeat threshold often enough to be played as a burst up to 17 dB loud
// (docs/decoder-quality.md "IMBE: errors in the Golay parity bits"). rx is the received word,
// out the decoder's output (corrected data at [22..11]); dataOnly its own count.
int golayWordErrors(const char rx[23], const char out[23], int dataOnly)
{
#if MBEVOCODER_CHIP_MEASURED
    (void)dataOnly;
    const uint32_t cw = ecc::golay23Encode(bits::fromDescending(out, 22, 11));
    uint32_t e = cw ^ bits::fromDescending(rx, 22, 0);
    int n = 0;
    for (; e; e &= e - 1) ++n;
    return n;
#else
    (void)rx; (void)out;
    return dataOnly;
#endif
}

#ifndef MBE_SOFT_IMBE_TRUST
#define MBE_SOFT_IMBE_TRUST 2.0   // see softWordErrors; 0 counts the full distance (experiments)
#endif

// The errors a soft-decoded word reports. The soft search returns the most likely codeword,
// which can lie further from the hard decisions than a hard decoder ever corrects (3 bits of a
// Golay word, 1 of a Hamming word); counting that whole distance pushed DVSI's 10 % soft vector
// over the repeat and mute thresholds far more often than its hard decisions (517 repeats and
// 57 mutes in 1275 frames against 371 and 7). As for the AMBE C0 word (ambe_frame.cpp), the word
// is trusted when the bits it overrules are weak, their reliabilities summing to at most
// MBE_SOFT_IMBE_TRUST times the word's mean reliability, and then counts as a hard decoder's
// worst correctable word; otherwise the whole distance counts. cw: the decoded codeword's bits.
// neo-compat counts as the hard decoder does and has no use for it.
#if MBEVOCODER_CHIP_MEASURED
int softWordErrors(const mbe_soft_bit* in, const char* cw, int n, int correctable)
{
    int dist = 0;
    double overruled = 0.0, total = 0.0;
    for (int i = 0; i < n; ++i) {
        total += in[i].reliability;
        if (static_cast<int>(in[i].bit & 1u) != (cw[i] & 1)) { ++dist; overruled += in[i].reliability; }
    }
    if (dist > correctable && MBE_SOFT_IMBE_TRUST > 0.0 && total > 0.0 && overruled <= MBE_SOFT_IMBE_TRUST * total / n)
        return correctable;
    return dist;
}
#endif

// Soft Golay(23,12): the decoded word's error count under the rule above (CHIP) or the hard
// path's count (neo-compat).
int golaySoftWordErrors(const mbe_soft_bit in[23], const char rx[23], const char out[23], int dataOnly)
{
#if MBEVOCODER_CHIP_MEASURED
    (void)rx; (void)dataOnly;
    char cw[23];
    bits::toDescending(ecc::golay23Encode(bits::fromDescending(out, 22, 11)), cw, 22, 0);
    return softWordErrors(in, cw, 23, 3);
#else
    (void)in;
    return golayWordErrors(rx, out, dataOnly);
#endif
}

// ---- 7200x4400 -------------------------------------------------------------

template <typename Cell, typename Get, typename Set>
void demodulate7200(Cell fr[8][23], Get get, Set set)
{
    uint32_t seed = 0;
    for (int i = 22; i >= 11; --i) seed = (seed << 1) | get(fr[0][i]);
    char pr[114];
    ecc::pnSequence(seed, pr, 114);
    int k = 0;
    for (int i = 1; i < 4; ++i)
        for (int j = 22; j >= 0; --j) set(fr[i][j], get(fr[i][j]) ^ static_cast<unsigned>(pr[k++]));
    for (int i = 4; i < 7; ++i)
        for (int j = 14; j >= 0; --j) set(fr[i][j], get(fr[i][j]) ^ static_cast<unsigned>(pr[k++]));
}

int eccData7200(char fr[8][23], char* d, int* c4, bool fullCount = false)
{
    int errs = 0, k = 0;
    for (int i = 0; i < 4; ++i) {
        if (i > 0) {
            char gin[23], gout[23];
            for (int j = 0; j < 23; ++j) gin[j] = fr[i][j];
            const int e = mbe_golay2312(gin, gout);
            errs += fullCount ? golayWordErrors(gin, gout, e) : e;
            for (int j = 22; j > 10; --j) d[k++] = gout[j];
        } else {
            for (int j = 22; j > 10; --j) d[k++] = fr[i][j];
        }
    }
    for (int i = 4; i < 7; ++i) {
        char hin[15], hout[15];
        for (int j = 0; j < 15; ++j) hin[j] = fr[i][j];
        int he = mbe_hamming1511(hin, hout);
        errs += he;
        if (i == 4 && c4) *c4 = he;
        for (int j = 14; j >= 4; --j) d[k++] = hout[j];
    }
    for (int j = 6; j >= 0; --j) d[k++] = fr[7][j];
    return errs;
}

int eccData7200Soft(mbe_soft_bit fr[8][23], char* d, int* c4)
{
    int errs = 0, k = 0;
    for (int i = 0; i < 4; ++i) {
        if (i > 0) {
            mbe_soft_bit gin[23];
            char gout[23], rx[23];
            for (int j = 0; j < 23; ++j) { gin[j] = fr[i][j]; rx[j] = static_cast<char>(fr[i][j].bit & 1u); }
            errs += golaySoftWordErrors(gin, rx, gout, mbe_golay2312Soft(gin, gout));
            for (int j = 22; j > 10; --j) d[k++] = gout[j];
        } else {
            for (int j = 22; j > 10; --j) d[k++] = static_cast<char>(fr[i][j].bit & 1u);
        }
    }
    for (int i = 4; i < 7; ++i) {
        mbe_soft_bit hin[15];
        char hout[15];
        for (int j = 0; j < 15; ++j) hin[j] = fr[i][j];
        int he = mbe_hamming1511Soft(hin, hout);
#if MBEVOCODER_CHIP_MEASURED
        if (he > 0) he = softWordErrors(hin, hout, 15, 1);
#endif
        errs += he;
        if (i == 4 && c4) *c4 = he;
        for (int j = 14; j >= 4; --j) d[k++] = hout[j];
    }
    for (int j = 6; j >= 0; --j) d[k++] = static_cast<char>(fr[7][j].bit & 1u);
    return errs;
}

// ---- 7100x4400 -------------------------------------------------------------

int eccC0_7100(char fr[7][24])
{
    char in[23], out[23];
    for (int j = 0; j < 18; ++j) in[j] = fr[0][j + 1];
    for (int j = 18; j < 23; ++j) in[j] = 0;
    int errs = mbe_golay2312(in, out);
    for (int j = 0; j < 18; ++j) fr[0][j + 1] = out[j];
    return errs;
}

int eccC0_7100Soft(mbe_soft_bit fr[7][24])
{
    mbe_soft_bit in[23];
    char out[23];
    for (int j = 0; j < 18; ++j) in[j] = fr[0][j + 1];
    for (int j = 18; j < 23; ++j) { in[j].bit = 0; in[j].reliability = 255; }
    int errs = mbe_golay2312Soft(in, out);
    for (int j = 0; j < 18; ++j) fr[0][j + 1].bit = static_cast<uint8_t>(out[j] & 1);
    return errs;
}

template <typename Cell, typename Get, typename Set>
void demodulate7100(Cell fr[7][24], Get get, Set set)
{
    uint32_t seed = 0;
    for (int i = 18; i >= 12; --i) seed = (seed << 1) | get(fr[0][i]);
    char pr[100];
    ecc::pnSequence(seed, pr, 100);
    int k = 0;
    for (int j = 23; j >= 0; --j) set(fr[1][j], get(fr[1][j]) ^ static_cast<unsigned>(pr[k++]));
    for (int i = 2; i < 4; ++i)
        for (int j = 22; j >= 0; --j) set(fr[i][j], get(fr[i][j]) ^ static_cast<unsigned>(pr[k++]));
    for (int i = 4; i < 6; ++i)
        for (int j = 14; j >= 0; --j) set(fr[i][j], get(fr[i][j]) ^ static_cast<unsigned>(pr[k++]));
}

// Layout from mbelib: C0 = 18 bits at fr[0][18..1] (Golay 23,12 with 5 zero bits), C1 = 24-bit
// Golay at fr[1][23..0] (decoded as 23 bits at [23..1]), C2, C3 Golay 23 at [22..0], C4, C5
// Hamming at fr[4..5][14..0], C6 unprotected 23 bits at fr[6][22..0] (7 + 12 + 24 + 22 + 23 = 88).
int eccData7100(char fr[7][24], char* d, int* c4)
{
    int errs = 0, k = 0;
    char gin[23], gout[23], hin[15], hout[15];
    // C0: 7 data bits at [18..12]
    for (int j = 18; j > 11; --j) d[k++] = fr[0][j];
    // C1: 12 data bits from the 23-bit codeword at [23..1]
    for (int j = 0; j < 23; ++j) gin[j] = fr[1][j + 1];
    errs += mbe_golay2312(gin, gout);
    for (int j = 22; j > 10; --j) d[k++] = gout[j];
    // C2, C3
    for (int i = 2; i < 4; ++i) {
        for (int j = 0; j < 23; ++j) gin[j] = fr[i][j];
        errs += mbe_golay2312(gin, gout);
        for (int j = 22; j > 10; --j) d[k++] = gout[j];
    }
    // C4, C5 (ProVoice Hamming variant)
    for (int i = 4; i < 6; ++i) {
        for (int j = 0; j < 15; ++j) hin[j] = fr[i][j];
        int he = mbe_7100x4400hamming1511(hin, hout);
        errs += he;
        if (i == 4 && c4) *c4 = he;
        for (int j = 14; j > 3; --j) d[k++] = hout[j];
    }
    // C6: 23 unprotected bits (mbelib reads [22..0]; bit 22 is the status bit d[0] region)
    for (int j = 22; j >= 0; --j) d[k++] = fr[6][j];
    return errs;
}

int eccData7100Soft(mbe_soft_bit fr[7][24], char* d, int* c4)
{
    int errs = 0, k = 0;
    mbe_soft_bit gin[23], hin[15];
    char gout[23], hout[15];
    for (int j = 18; j > 11; --j) d[k++] = static_cast<char>(fr[0][j].bit & 1u);
    for (int j = 0; j < 23; ++j) gin[j] = fr[1][j + 1];
    errs += mbe_golay2312Soft(gin, gout);
    for (int j = 22; j > 10; --j) d[k++] = gout[j];
    for (int i = 2; i < 4; ++i) {
        for (int j = 0; j < 23; ++j) gin[j] = fr[i][j];
        errs += mbe_golay2312Soft(gin, gout);
        for (int j = 22; j > 10; --j) d[k++] = gout[j];
    }
    for (int i = 4; i < 6; ++i) {
        for (int j = 0; j < 15; ++j) hin[j] = fr[i][j];
        int he = mbe_7100x4400hamming1511Soft(hin, hout);
        errs += he;
        if (i == 4 && c4) *c4 = he;
        for (int j = 14; j > 3; --j) d[k++] = hout[j];
    }
    for (int j = 22; j >= 0; --j) d[k++] = static_cast<char>(fr[6][j].bit & 1u);
    return errs;
}

void fillResult(mbe_process_result* r, int c0, int prot, int c4, unsigned extra)
{
    if (!r) return;
    r->c0_errors = c0;
    r->protected_errors = prot;
    r->c4_errors = c4;
    r->total_errors = c0 + prot;
    r->flags = MBE_PROCESS_FLAG_C0_VALID | MBE_PROCESS_FLAG_C4_VALID | extra;
}

} // namespace

extern "C" {

void mbe_dumpImbe4400Data(const char* d)
{
    if (!d) return;
    for (int i = 0; i < 88; ++i) std::fprintf(stderr, "%i", d[i]);
    std::fprintf(stderr, " ");
}
void mbe_dumpImbe7200x4400Data(const char* d) { mbe_dumpImbe4400Data(d); }
void mbe_dumpImbe7100x4400Data(const char* d) { mbe_dumpImbe4400Data(d); }

void mbe_dumpImbe7200x4400Frame(const char fr[8][23])
{
    if (!fr) return;
    for (int i = 0; i < 8; ++i) {
        std::fprintf(stderr, "imbe_fr c%d: ", i);
        int hi = i < 4 ? 22 : (i < 7 ? 14 : 6);
        for (int j = hi; j >= 0; --j) std::fprintf(stderr, "%i", fr[i][j]);
        std::fprintf(stderr, " ");
    }
}

void mbe_dumpImbe7100x4400Frame(const char fr[7][24])
{
    if (!fr) return;
    for (int i = 0; i < 7; ++i) {
        std::fprintf(stderr, "imbe_fr c%d: ", i);
        for (int j = 23; j >= 0; --j) std::fprintf(stderr, "%i", fr[i][j]);
        std::fprintf(stderr, " ");
    }
}

int mbe_eccImbe7200x4400C0(char fr[8][23])
{
    int ret = bits::validate(&fr[0][0], kBits7200);
    if (ret < 0) return ret;
    char in[23], out[23];
    for (int j = 0; j < 23; ++j) in[j] = fr[0][j];
    int errs = mbe_golay2312(in, out);
    for (int j = 0; j < 23; ++j) fr[0][j] = out[j];
    return errs;
}

int mbe_eccImbe7200x4400Data(char fr[8][23], char* d)
{
    if (!d) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(&fr[0][0], kBits7200);
    return ret < 0 ? ret : eccData7200(fr, d, nullptr);
}

int mbe_demodulateImbe7200x4400Data(char fr[8][23])
{
    int ret = bits::validate(&fr[0][0], kBits7200);
    if (ret < 0) return ret;
    demodulate7200(fr, [](char c) { return static_cast<unsigned>(c & 1); }, [](char& c, unsigned v) { c = static_cast<char>(v & 1u); });
    return 0;
}

int mbe_decodeImbe7200x4400Frame(const char in[8][23], char d[88], mbe_process_result* result)
{
    if (result) mbe_initProcessResult(result);
    if (!d) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(&in[0][0], kBits7200);
    if (ret < 0) return ret;
    char fr[8][23];
    std::memcpy(fr, in, sizeof fr);
    int c0 = golayWordErrors(in[0], fr[0], mbe_eccImbe7200x4400C0(fr));
    mbe_demodulateImbe7200x4400Data(fr);
    int c4 = 0;
    int prot = eccData7200(fr, d, &c4, true);
    fillResult(result, c0, prot, c4, 0);
    return c0 + prot;
}

int mbe_decodeImbe7200x4400SoftFrame(const mbe_soft_bit in[8][23], char d[88], mbe_process_result* result)
{
    if (result) mbe_initProcessResult(result);
    if (!d) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validateSoft(&in[0][0], kBits7200);
    if (ret < 0) return ret;
    mbe_soft_bit fr[8][23];
    std::memcpy(fr, in, sizeof fr);
    mbe_soft_bit gin[23];
    char gout[23];
    char rx[23];
    for (int j = 0; j < 23; ++j) { gin[j] = fr[0][j]; rx[j] = static_cast<char>(fr[0][j].bit & 1u); }
    int c0 = golaySoftWordErrors(gin, rx, gout, mbe_golay2312Soft(gin, gout));
    for (int j = 0; j < 23; ++j) fr[0][j].bit = static_cast<uint8_t>(gout[j] & 1);
    demodulate7200(fr, [](const mbe_soft_bit& c) { return static_cast<unsigned>(c.bit & 1u); },
                   [](mbe_soft_bit& c, unsigned v) { c.bit = static_cast<uint8_t>(v & 1u); });
    int c4 = 0;
    int prot = eccData7200Soft(fr, d, &c4);
    fillResult(result, c0, prot, c4, MBE_PROCESS_FLAG_SOFT_INPUT);
    return c0 + prot;
}

int mbe_encodeImbe7200x4400Frame(const char d[88], char fr[8][23])
{
    if (!fr) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(d, 88);
    if (ret < 0) return ret;
    std::memset(fr, 0, kBits7200);
    for (int i = 0; i < 4; ++i)
        bits::toDescending(ecc::golay23Encode(bits::fromAscending(d, 12 * i, 12)), fr[i], 22, 0);
    for (int i = 0; i < 3; ++i)
        bits::toDescending(ecc::hamming15Encode(bits::fromAscending(d, 48 + 11 * i, 11), ecc::HammingVariant::Standard), fr[4 + i], 14, 0);
    for (int j = 6; j >= 0; --j) fr[7][j] = d[81 + (6 - j)];
    // Scrambling is its own inverse.
    demodulate7200(fr, [](char c) { return static_cast<unsigned>(c & 1); }, [](char& c, unsigned v) { c = static_cast<char>(v & 1u); });
    return 0;
}

int mbe_eccImbe7100x4400C0(char fr[7][24])
{
    int ret = bits::validate(&fr[0][0], kBits7100);
    return ret < 0 ? ret : eccC0_7100(fr);
}

int mbe_eccImbe7100x4400Data(char fr[7][24], char* d)
{
    if (!d) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(&fr[0][0], kBits7100);
    return ret < 0 ? ret : eccData7100(fr, d, nullptr);
}

int mbe_demodulateImbe7100x4400Data(char fr[7][24])
{
    int ret = bits::validate(&fr[0][0], kBits7100);
    if (ret < 0) return ret;
    demodulate7100(fr, [](char c) { return static_cast<unsigned>(c & 1); }, [](char& c, unsigned v) { c = static_cast<char>(v & 1u); });
    return 0;
}

int mbe_convertImbe7100to7200(char* d)
{
    int ret = bits::validate(d, 88);
    if (ret < 0) return ret;
    static const unsigned b0Idx[] = {1u, 2u, 3u, 4u, 5u, 6u, 86u, 87u};
    int b0 = 0;
    for (unsigned idx : b0Idx) b0 = (b0 << 1) | (d[idx] & 1);
    float w0 = (4.0f * 3.14159265358979f) / (static_cast<float>(b0) + 39.5f);
    int L = static_cast<int>(0.9254 * static_cast<int>((3.14159265358979 / w0) + 0.25));
    int K = L < 37 ? static_cast<int>(static_cast<float>(L + 2) / 3.0f) : 12;
    char tmp[88];
    tmp[87] = d[0];
    tmp[48 + K] = d[42];
    tmp[49 + K] = d[43];
    int k = 44, j = 48;
    for (int i = 0; i < K; ++i) tmp[j++] = d[k++];
    j = 0; k = 1;
    while (j < 87) {
        tmp[j] = d[k];
        if (++j == 48) j += K + 2;
        if (++k == 42) k += K + 2;
    }
    std::memcpy(d, tmp, 88);
    return 0;
}

int mbe_decodeImbe7100x4400Frame(const char in[7][24], char d[88], mbe_process_result* result)
{
    if (result) mbe_initProcessResult(result);
    if (!d) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(&in[0][0], kBits7100);
    if (ret < 0) return ret;
    char fr[7][24];
    std::memcpy(fr, in, sizeof fr);
    int c0 = eccC0_7100(fr);
    demodulate7100(fr, [](char c) { return static_cast<unsigned>(c & 1); }, [](char& c, unsigned v) { c = static_cast<char>(v & 1u); });
    int c4 = 0;
    int prot = eccData7100(fr, d, &c4);
    mbe_convertImbe7100to7200(d);
    fillResult(result, c0, prot, c4, MBE_PROCESS_FLAG_PROVOICE);
    return c0 + prot;
}

int mbe_decodeImbe7100x4400SoftFrame(const mbe_soft_bit in[7][24], char d[88], mbe_process_result* result)
{
    if (result) mbe_initProcessResult(result);
    if (!d) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validateSoft(&in[0][0], kBits7100);
    if (ret < 0) return ret;
    mbe_soft_bit fr[7][24];
    std::memcpy(fr, in, sizeof fr);
    int c0 = eccC0_7100Soft(fr);
    demodulate7100(fr, [](const mbe_soft_bit& c) { return static_cast<unsigned>(c.bit & 1u); },
                   [](mbe_soft_bit& c, unsigned v) { c.bit = static_cast<uint8_t>(v & 1u); });
    int c4 = 0;
    int prot = eccData7100Soft(fr, d, &c4);
    mbe_convertImbe7100to7200(d);
    fillResult(result, c0, prot, c4, MBE_PROCESS_FLAG_SOFT_INPUT | MBE_PROCESS_FLAG_PROVOICE);
    return c0 + prot;
}

} // extern "C"
