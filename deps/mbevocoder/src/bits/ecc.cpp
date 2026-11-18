// SPDX-License-Identifier: GPL-3.0-or-later
// Golay and Hamming codes. Generator polynomials/matrices from mbelib (ISC),
// syndrome tables are computed at start-up instead of being tabulated.
#include "internal/ecc.hpp"

#include <array>

namespace mbe::ecc {

namespace {

// Parity bits contributed by each data bit, data bit 11 (MSB) first. From mbelib.
constexpr uint32_t kGolayGenerator[12] = {0x63a, 0x31d, 0x7b4, 0x3da, 0x1ed, 0x6cc,
                                          0x366, 0x1b3, 0x6e3, 0x54b, 0x49f, 0x475};

constexpr uint32_t golayParity(uint32_t data12)
{
    uint32_t ecc = 0;
    for (int i = 0; i < 12; ++i)
        if ((data12 >> (11 - i)) & 1u) ecc ^= kGolayGenerator[i];
    return ecc;
}

constexpr uint32_t golaySyndrome(uint32_t cw23)
{
    return golayParity(cw23 >> 11) ^ (cw23 & 0x7ffu);
}

constexpr int popcount(uint32_t v)
{
    int n = 0;
    while (v) { n += static_cast<int>(v & 1u); v >>= 1; }
    return n;
}

// syndrome -> 23-bit error pattern, for every pattern of weight <= 3 (exactly 2048 of them).
struct GolayTable {
    std::array<uint32_t, 2048> pattern{};
    constexpr GolayTable()
    {
        pattern[0] = 0;
        for (int a = 0; a < 23; ++a) {
            pattern[golaySyndrome(1u << a)] = 1u << a;
            for (int b = a + 1; b < 23; ++b) {
                uint32_t e2 = (1u << a) | (1u << b);
                pattern[golaySyndrome(e2)] = e2;
                for (int c = b + 1; c < 23; ++c) {
                    uint32_t e3 = e2 | (1u << c);
                    pattern[golaySyndrome(e3)] = e3;
                }
            }
        }
    }
};

const GolayTable& golayTable()
{
    static const GolayTable t{};
    return t;
}

constexpr uint32_t kHammingGenerator[2][4] = {
    {0x7f08, 0x78e4, 0x66d2, 0x55b1},   // standard (mbelib hammingGenerator)
    {0x7ac8, 0x3d64, 0x1eb2, 0x7591},   // IMBE 7100x4400 (mbelib imbe7100x4400hammingGenerator)
};
// Both generators are systematic with the parity bits in positions 3..0 (each generator
// row has exactly one bit there), so the data occupies bits 14..4 as mbelib's frame
// extraction assumes.
constexpr int kHammingDataPos[2][11] = {
    {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14},
    {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14},
};
constexpr int kHammingParityPos[2][4] = {{0, 1, 2, 3}, {0, 1, 2, 3}};

constexpr uint32_t hammingSyndrome(uint32_t cw15, int variant)
{
    uint32_t s = 0;
    for (int i = 0; i < 4; ++i)
        s |= static_cast<uint32_t>(popcount(cw15 & kHammingGenerator[variant][i]) & 1) << i;
    return s;
}

struct HammingTable {
    std::array<uint16_t, 16> correction{};   // syndrome -> single-bit error mask
    std::array<uint16_t, 2048> encode{};     // data -> codeword
    constexpr explicit HammingTable(int variant)
    {
        correction[0] = 0;
        for (int b = 0; b < 15; ++b) correction[hammingSyndrome(1u << b, variant)] = static_cast<uint16_t>(1u << b);
        for (uint32_t d = 0; d < 2048; ++d) {
            uint32_t cw = 0;
            for (int i = 0; i < 11; ++i)
                if ((d >> i) & 1u) cw |= 1u << kHammingDataPos[variant][i];
            for (uint32_t p = 0; p < 16; ++p) {
                uint32_t cand = cw;
                for (int i = 0; i < 4; ++i)
                    if ((p >> i) & 1u) cand |= 1u << kHammingParityPos[variant][i];
                if (hammingSyndrome(cand, variant) == 0) { encode[d] = static_cast<uint16_t>(cand); break; }
            }
        }
    }
};

const HammingTable& hammingTable(HammingVariant v)
{
    static const HammingTable std{0};
    static const HammingTable pv{1};
    return v == HammingVariant::Standard ? std : pv;
}

} // namespace

uint32_t golay23Encode(uint32_t data12)
{
    data12 &= 0xfffu;
    return (data12 << 11) | golayParity(data12);
}

uint32_t golay24Encode(uint32_t data12)
{
    uint32_t cw = golay23Encode(data12);
    return (cw << 1) | static_cast<uint32_t>(popcount(cw) & 1);
}

uint32_t golay23ErrorPattern(uint32_t cw23)
{
    return golayTable().pattern[golaySyndrome(cw23 & 0x7fffffu)];
}

int golay23Correct(uint32_t* cw23)
{
    uint32_t e = golay23ErrorPattern(*cw23);
    *cw23 = (*cw23 ^ e) & 0x7fffffu;
    return popcount(e);
}

int golay24Correct(uint32_t* cw24)
{
    // Correct the (23,12) part, then the overall parity bit: a valid (24,12) word has even
    // parity, so after correcting the codeword bits a mismatch means the parity bit itself
    // is in error (one codeword error plus a parity error is two errors, both fixed).
    uint32_t cw = (*cw24 >> 1) & 0x7fffffu;
    int errs = golay23Correct(&cw);
    uint32_t parity = *cw24 & 1u;
    if (((popcount(cw) + static_cast<int>(parity)) & 1) != 0) {
        parity ^= 1u;
        ++errs;
    }
    *cw24 = (cw << 1) | parity;
    return errs;
}

uint32_t hamming15Encode(uint32_t data11, HammingVariant v)
{
    return hammingTable(v).encode[data11 & 0x7ffu];
}

int hamming15Correct(uint32_t* cw15, HammingVariant v)
{
    uint32_t s = hammingSyndrome(*cw15 & 0x7fffu, v == HammingVariant::Standard ? 0 : 1);
    if (s == 0) return 0;
    *cw15 ^= hammingTable(v).correction[s];
    return 1;
}

uint32_t hamming15Data(uint32_t cw15, HammingVariant v)
{
    const int* pos = kHammingDataPos[v == HammingVariant::Standard ? 0 : 1];
    uint32_t d = 0;
    for (int i = 0; i < 11; ++i) d |= ((cw15 >> pos[i]) & 1u) << i;
    return d;
}

void pnSequence(uint32_t seed12, char* bits, unsigned count)
{
    uint32_t x = (16u * (seed12 & 0xfffu)) & 0xffffu;
    for (unsigned i = 0; i < count; ++i) {
        x = (173u * x + 13849u) & 0xffffu;
        bits[i] = static_cast<char>(x >> 15);
    }
}

uint32_t pn24(uint32_t seed12)
{
    char b[24];
    pnSequence(seed12, b, 24);
    uint32_t v = 0;
    for (char c : b) v = (v << 1) | static_cast<uint32_t>(c);
    return v;
}

} // namespace mbe::ecc
