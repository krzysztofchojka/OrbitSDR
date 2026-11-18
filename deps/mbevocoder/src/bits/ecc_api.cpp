// SPDX-License-Identifier: GPL-3.0-or-later
// Public ECC entry points. The char-array functions keep mbelib-neo's exact semantics
// (LSB at index 0, error count over the data bits only for Golay); the integer
// functions are new and count every corrected bit.
#include <mbevocoder/mbevocoder.h>

#include "internal/bits.hpp"
#include "internal/ecc.hpp"

#include <array>
#include <bitset>
#include <cstring>

using namespace mbe;

namespace {

// The soft decoders score every codeword against the received bits: the cost of a candidate is
// the sum of the reliabilities of the bits where it differs from the hard decisions. Per call the
// costs are tabulated per byte of the difference word, so a candidate costs three lookups rather
// than a loop over its bits; the codewords are tabulated once. Same candidates, same order, same
// tie-breaks as scoring bit by bit.
void byteCosts(const mbe_soft_bit* in, int n, int (*t)[256], int bytes)
{
    for (int b = 0; b < bytes; ++b) {
        t[b][0] = 0;
        for (int v = 1; v < 256; ++v) {
            int low = 0;
            while (!((v >> low) & 1)) ++low;
            const int idx = 8 * b + low;
            t[b][v] = t[b][v & (v - 1)] + (idx < n ? in[idx].reliability : 0);
        }
    }
}

uint32_t word(const char* bits, int n)
{
    uint32_t w = 0;
    for (int i = 0; i < n; ++i) w |= static_cast<uint32_t>(bits[i] & 1) << i;
    return w;
}

uint32_t softWord(const mbe_soft_bit* in, int n)
{
    uint32_t w = 0;
    for (int i = 0; i < n; ++i) w |= static_cast<uint32_t>(in[i].bit & 1u) << i;
    return w;
}

int ones(uint32_t x) { return static_cast<int>(std::bitset<32>(x).count()); }

const std::array<uint32_t, 4096>& golayCodewords()
{
    static const std::array<uint32_t, 4096> t = [] {
        std::array<uint32_t, 4096> a{};
        for (uint32_t d = 0; d < 4096; ++d) a[d] = ecc::golay23Encode(d);
        return a;
    }();
    return t;
}

const std::array<uint16_t, 2048>& hammingCodewords(ecc::HammingVariant v)
{
    static const auto make = [](ecc::HammingVariant var) {
        std::array<uint16_t, 2048> a{};
        for (uint32_t d = 0; d < 2048; ++d) a[d] = static_cast<uint16_t>(ecc::hamming15Encode(d, var));
        return a;
    };
    static const std::array<uint16_t, 2048> standard = make(ecc::HammingVariant::Standard), imbe7100 = make(ecc::HammingVariant::Imbe7100);
    return v == ecc::HammingVariant::Standard ? standard : imbe7100;
}

bool candidateIsBetter(bool haveBest, int score, int bestScore, bool matchesHard, bool bestMatchesHard, int diffs, int bestDiffs)
{
    if (!haveBest || score < bestScore) return true;
    if (score != bestScore) return false;
    if (matchesHard != bestMatchesHard) return matchesHard;
    return diffs < bestDiffs;
}

int hammingSoftCommon(const mbe_soft_bit* in, char* out, ecc::HammingVariant v)
{
    if (!out) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validateSoft(in, 15);
    if (ret < 0) return ret;
    char hardIn[15], hardOut[15];
    for (int i = 0; i < 15; ++i) hardIn[i] = static_cast<char>(in[i].bit & 1u);
    if (v == ecc::HammingVariant::Standard) mbe_hamming1511(hardIn, hardOut);
    else mbe_7100x4400hamming1511(hardIn, hardOut);

    int cost[2][256];
    byteCosts(in, 15, cost, 2);
    const uint32_t rx = softWord(in, 15), hard = word(hardOut, 15);
    const auto& codewords = hammingCodewords(v);
    uint32_t best = 0;
    int bestScore = 0x3fffffff, bestDiffs = 0x3fffffff;
    bool haveBest = false;
    for (uint32_t data = 0; data < 2048; ++data) {
        const uint32_t cand = codewords[data], x = cand ^ rx;
        const int score = cost[0][x & 0xffu] + cost[1][x >> 8];
        const bool matchesHard = cand == hard;
        const bool bestMatchesHard = haveBest && best == hard;
        if (candidateIsBetter(haveBest, score, bestScore, matchesHard, bestMatchesHard, ones(x), bestDiffs)) {
            best = cand; bestScore = score; bestDiffs = ones(x); haveBest = true;
        }
    }
    bits::toDescending(best, out, 14, 0);
    return bestDiffs;
}

int hammingHardCommon(const char* in, char* out, ecc::HammingVariant v)
{
    if (!out) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(in, 15);
    if (ret < 0) return ret;
    uint32_t cw = bits::fromDescending(in, 14, 0);
    int errs = ecc::hamming15Correct(&cw, v);
    bits::toDescending(cw, out, 14, 0);
    return errs;
}

} // namespace

extern "C" {

int mbe_checkGolayBlock(long int* block)
{
    if (!block) return MBE_STATUS_INVALID_ARGUMENT;
    uint32_t cw = static_cast<uint32_t>(*block) & 0x7fffffu;
    uint32_t e = ecc::golay23ErrorPattern(cw);
    // mbelib semantics: the block is replaced by the corrected 12 data bits.
    *block = static_cast<long>(((cw ^ e) >> 11) & 0xfffu);
    return 0;
}

int mbe_golay2312(const char* in, char* out)
{
    if (!out) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(in, 23);
    if (ret < 0) return ret;
    uint32_t cw = bits::fromDescending(in, 22, 0);
    uint32_t e = ecc::golay23ErrorPattern(cw);
    uint32_t data = ((cw ^ e) >> 11) & 0xfffu;
    // Corrected data bits, parity bits copied through uncorrected (as mbelib does).
    bits::toDescending(data, out, 22, 11);
    for (int i = 10; i >= 0; --i) out[i] = in[i];
    int errs = 0;
    for (int i = 22; i >= 11; --i) if (out[i] != in[i]) ++errs;
    return errs;
}

int mbe_golay2312Soft(const mbe_soft_bit* in, char* out)
{
    if (!out) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validateSoft(in, 23);
    if (ret < 0) return ret;
    char hardIn[23], hardOut[23];
    for (int i = 0; i < 23; ++i) hardIn[i] = static_cast<char>(in[i].bit & 1u);
    mbe_golay2312(hardIn, hardOut);

    int cost[3][256];
    byteCosts(in, 23, cost, 3);
    constexpr uint32_t kData = 0x7ff800u;   // bits 22..11: the data bits
    const uint32_t rx = softWord(in, 23), hard = word(hardOut, 23);
    const auto& codewords = golayCodewords();
    uint32_t best = 0;
    int bestScore = 0x3fffffff, bestDiffs = 0x3fffffff;
    bool haveBest = false;
    for (uint32_t data = 0; data < 4096; ++data) {
        const uint32_t cand = codewords[data], x = cand ^ rx;
        const int score = cost[0][x & 0xffu] + cost[1][(x >> 8) & 0xffu] + cost[2][x >> 16];
        const int diffs = ones(x & kData);
        const bool matchesHard = ((cand ^ hard) & kData) == 0;
        const bool bestMatchesHard = haveBest && ((best ^ hard) & kData) == 0;
        if (candidateIsBetter(haveBest, score, bestScore, matchesHard, bestMatchesHard, diffs, bestDiffs)) {
            best = cand; bestScore = score; bestDiffs = diffs; haveBest = true;
        }
    }
    bits::toDescending(best, out, 22, 0);
    for (int i = 10; i >= 0; --i) out[i] = hardIn[i];
    return bestDiffs;
}

int mbe_hamming1511(const char* in, char* out) { return hammingHardCommon(in, out, ecc::HammingVariant::Standard); }
int mbe_hamming1511Soft(const mbe_soft_bit* in, char* out) { return hammingSoftCommon(in, out, ecc::HammingVariant::Standard); }
int mbe_7100x4400hamming1511(const char* in, char* out) { return hammingHardCommon(in, out, ecc::HammingVariant::Imbe7100); }
int mbe_7100x4400hamming1511Soft(const mbe_soft_bit* in, char* out) { return hammingSoftCommon(in, out, ecc::HammingVariant::Imbe7100); }

uint32_t mbe_golay2312Encode(uint32_t data12) { return ecc::golay23Encode(data12); }
uint32_t mbe_golay2412Encode(uint32_t data12) { return ecc::golay24Encode(data12); }

int mbe_golay2312Decode(uint32_t* cw)
{
    if (!cw) return MBE_STATUS_INVALID_ARGUMENT;
    return ecc::golay23Correct(cw);
}

int mbe_golay2412Decode(uint32_t* cw)
{
    if (!cw) return MBE_STATUS_INVALID_ARGUMENT;
    return ecc::golay24Correct(cw);
}

uint32_t mbe_hamming1511Encode(uint32_t data11) { return ecc::hamming15Encode(data11, ecc::HammingVariant::Standard); }
uint32_t mbe_7100x4400hamming1511Encode(uint32_t data11) { return ecc::hamming15Encode(data11, ecc::HammingVariant::Imbe7100); }

int mbe_hamming1511Decode(uint32_t* cw, uint32_t* data)
{
    if (!cw) return MBE_STATUS_INVALID_ARGUMENT;
    int errs = ecc::hamming15Correct(cw, ecc::HammingVariant::Standard);
    if (data) *data = ecc::hamming15Data(*cw, ecc::HammingVariant::Standard);
    return errs;
}

int mbe_7100x4400hamming1511Decode(uint32_t* cw, uint32_t* data)
{
    if (!cw) return MBE_STATUS_INVALID_ARGUMENT;
    int errs = ecc::hamming15Correct(cw, ecc::HammingVariant::Imbe7100);
    if (data) *data = ecc::hamming15Data(*cw, ecc::HammingVariant::Imbe7100);
    return errs;
}

void mbe_pnSequence(uint32_t seed12, char* bits, unsigned count)
{
    if (bits) ecc::pnSequence(seed12, bits, count);
}

uint32_t mbe_pn24(uint32_t seed12) { return ecc::pn24(seed12); }

} // extern "C"
