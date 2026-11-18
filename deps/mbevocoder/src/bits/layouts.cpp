// SPDX-License-Identifier: GPL-3.0-or-later
// On-air bit layouts <-> mbelib bitplane frames.
//
// Table provenance:
//   D-STAR dW/dX, AMBE+2 rW/rX/rY/rZ, YSF VD2 and VW interleaves: dsdcc (GPL-3.0-or-later),
//   itself derived from DSD. P25 Phase 1 iW/iX/iY/iZ: DSD (GPL-2.0-or-later, p25p1_const.h).
//   P25 Phase 2 voice codeword interleave: OP25 p25p2_vf.cc (GPL-3.0-or-later).
//   The D-STAR and AMBE+2 layouts were verified bit-exact against an AMBE-3000R (docs/bit-order.md).
#include <mbevocoder/mbevocoder.h>

#include "internal/bits.hpp"

#include <cstring>

using namespace mbe;

namespace {

struct Cell { uint8_t row, col; };

// D-STAR: on-air bit i -> ambe_fr[dW[i]][dX[i]]
constexpr uint8_t dW[72] = {
    0,0,3,2,1,1,0,0,1,1,0,0, 3,2,1,1,3,2,1,1,0,0,3,2, 0,0,3,2,1,1,0,0,1,1,0,0,
    3,2,1,1,3,2,1,1,0,0,3,2, 0,0,3,2,1,1,0,0,1,1,0,0, 3,2,1,1,3,3,2,1,0,0,3,3 };
constexpr uint8_t dX[72] = {
    10,22,11,9,10,22,11,23,8,20,9,21, 10,8,9,21,8,6,7,19,8,20,9,7, 6,18,7,5,6,18,7,19,4,16,5,17,
    6,4,5,17,4,2,3,15,4,16,5,3, 2,14,3,1,2,14,3,15,0,12,1,13, 2,0,1,13,0,12,10,11,0,12,1,13 };

// AMBE+2 (DMR, NXDN, dPMR, YSF VD1): dibit d -> bit1 at [rW][rX], bit0 at [rY][rZ]
constexpr uint8_t rW[36] = { 0,1,0,1,0,1, 0,1,0,1,0,1, 0,1,0,1,0,1, 0,1,0,1,0,2, 0,2,0,2,0,2, 0,2,0,2,0,2 };
constexpr uint8_t rX[36] = { 23,10,22,9,21,8, 20,7,19,6,18,5, 17,4,16,3,15,2, 14,1,13,0,12,10, 11,9,10,8,9,7, 8,6,7,5,6,4 };
constexpr uint8_t rY[36] = { 0,2,0,2,0,2, 0,2,0,3,0,3, 1,3,1,3,1,3, 1,3,1,3,1,3, 1,3,1,3,1,3, 1,3,1,3,1,3 };
constexpr uint8_t rZ[36] = { 5,3,4,2,3,1, 2,0,1,13,0,12, 22,11,21,10,20,9, 19,8,18,7,17,6, 16,5,15,4,14,3, 13,2,12,1,11,0 };

// P25 Phase 2: on-air bit i -> (row, col) with OP25's c0[23..0] = fr[0][23..0], c1[22..0] = fr[1], etc.
constexpr Cell p25p2[72] = {
    {0,23},{0,5},{1,10},{2,3}, {0,22},{0,4},{1,9},{2,2}, {0,21},{0,3},{1,8},{2,1}, {0,20},{0,2},{1,7},{2,0},
    {0,19},{0,1},{1,6},{3,13}, {0,18},{0,0},{1,5},{3,12}, {0,17},{1,22},{1,4},{3,11}, {0,16},{1,21},{1,3},{3,10},
    {0,15},{1,20},{1,2},{3,9}, {0,14},{1,19},{1,1},{3,8}, {0,13},{1,18},{1,0},{3,7}, {0,12},{1,17},{2,10},{3,6},
    {0,11},{1,16},{2,9},{3,5}, {0,10},{1,15},{2,8},{3,4}, {0,9},{1,14},{2,7},{3,3}, {0,8},{1,13},{2,6},{3,2},
    {0,7},{1,12},{2,5},{3,1}, {0,6},{1,11},{2,4},{3,0} };

// P25 Phase 1 IMBE: dibit d -> bit1 at [iW][iX], bit0 at [iY][iZ]
constexpr uint8_t iW[72] = { 0,2,4,1,3,5, 0,2,4,1,3,6, 0,2,4,1,3,6, 0,2,4,1,3,6, 0,2,4,1,3,6, 0,2,4,1,3,6,
                             0,2,5,1,3,6, 0,2,5,1,3,6, 0,2,5,1,3,7, 0,2,5,1,3,7, 0,2,5,1,4,7, 0,3,5,2,4,7 };
constexpr uint8_t iX[72] = { 22,20,10,20,18,0, 20,18,8,18,16,13, 18,16,6,16,14,11, 16,14,4,14,12,9, 14,12,2,12,10,7, 12,10,0,10,8,5,
                             10,8,13,8,6,3, 8,6,11,6,4,1, 6,4,9,4,2,6, 4,2,7,2,0,4, 2,0,5,0,13,2, 0,21,3,21,11,0 };
constexpr uint8_t iY[72] = { 1,3,5,0,2,4, 1,3,6,0,2,4, 1,3,6,0,2,4, 1,3,6,0,2,4, 1,3,6,0,2,4, 1,3,6,0,2,5,
                             1,3,6,0,2,5, 1,3,6,0,2,5, 1,3,6,0,2,5, 1,3,7,0,2,5, 1,4,7,0,3,5, 2,4,7,1,3,5 };
constexpr uint8_t iZ[72] = { 21,19,1,21,19,9, 19,17,14,19,17,7, 17,15,12,17,15,5, 15,13,10,15,13,3, 13,11,8,13,11,1, 11,9,6,11,9,14,
                             9,7,4,9,7,12, 7,5,2,7,5,10, 5,3,0,5,3,8, 3,1,5,3,1,6, 1,14,3,1,22,4, 22,12,1,22,20,2 };

// YSF VW: on-air bit i -> position vfr[i] in the sequential 144-bit payload
// (4 x 23-bit Golay, 3 x 15-bit Hamming, 7 unprotected, each block MSB first).
constexpr uint8_t ysfVw[144] = {
      0, 24, 48, 72, 96,120, 25,  1, 73, 49,121, 97,   2, 26, 50, 74, 98,122, 27,  3, 75, 51,123, 99,
      4, 28, 52, 76,100,124, 29,  5, 77, 53,125,101,   6, 30, 54, 78,102,126, 31,  7, 79, 55,127,103,
      8, 32, 56, 80,104,128, 33,  9, 81, 57,129,105,  10, 34, 58, 82,106,130, 35, 11, 83, 59,131,107,
     12, 36, 60, 84,108,132, 37, 13, 85, 61,133,109,  14, 38, 62, 86,110,134, 39, 15, 87, 63,135,111,
     16, 40, 64, 88,112,136, 41, 17, 89, 65,137,113,  18, 42, 66, 90,114,138, 43, 19, 91, 67,139,115,
     20, 44, 68, 92,116,140, 45, 21, 93, 69,141,117,  22, 46, 70, 94,118,142, 47, 23, 95, 71,143,119 };

// Sequential 144-bit payload position -> imbe_fr cell
Cell vwCell(int pos)
{
    if (pos < 92) return {static_cast<uint8_t>(pos / 23), static_cast<uint8_t>(22 - pos % 23)};
    if (pos < 137) return {static_cast<uint8_t>(4 + (pos - 92) / 15), static_cast<uint8_t>(14 - (pos - 92) % 15)};
    return {7, static_cast<uint8_t>(6 - (pos - 137))};
}

// YSF VD2: dibit interleave (on-air bit i -> payload position) and payload -> ambe_d order.
constexpr uint8_t vd2Interleave[104] = {
    0,26,52,78, 1,27,53,79, 2,28,54,80, 3,29,55,81, 4,30,56,82, 5,31,57,83, 6,32,58,84, 7,33,59,85,
    8,34,60,86, 9,35,61,87, 10,36,62,88, 11,37,63,89, 12,38,64,90, 13,39,65,91, 14,40,66,92, 15,41,67,93,
    16,42,68,94, 17,43,69,95, 18,44,70,96, 19,45,71,97, 20,46,72,98, 21,47,73,99, 22,48,74,100, 23,49,75,101,
    24,50,76,102, 25,51,77,103 };

// ambe_d index i -> bit position in the DVSI raw 49-bit (rate 34) word, MSB first.
constexpr uint8_t dvsi49[49] = {
    0,3,6,9,12,15,18,21,24,27,30,33,36,39,41,43,45,47, 1,4,7,10,13,16,19,22,25,28,31,34,37,40,42,44,46,48,
    2,5,8,11,14,17,20,23,26,29,32,35,38 };

// Generic 72-bit layout as a cell table.
bool ambeCells(mbe_layout layout, Cell out[72])
{
    switch (layout) {
        case MBE_LAYOUT_DSTAR:
            for (int i = 0; i < 72; ++i) out[i] = {dW[i], dX[i]};
            return true;
        case MBE_LAYOUT_DMR:
            for (int d = 0; d < 36; ++d) { out[2 * d] = {rW[d], rX[d]}; out[2 * d + 1] = {rY[d], rZ[d]}; }
            return true;
        case MBE_LAYOUT_P25P2:
            for (int i = 0; i < 72; ++i) out[i] = p25p2[i];
            return true;
        default:
            return false;
    }
}

bool imbeCells(mbe_layout layout, Cell out[144])
{
    switch (layout) {
        case MBE_LAYOUT_P25P1:
            for (int d = 0; d < 72; ++d) { out[2 * d] = {iW[d], iX[d]}; out[2 * d + 1] = {iY[d], iZ[d]}; }
            return true;
        case MBE_LAYOUT_YSF_VW:
            for (int i = 0; i < 144; ++i) out[i] = vwCell(ysfVw[i]);
            return true;
        default:
            return false;
    }
}

} // namespace

extern "C" {

const char* mbe_layoutName(mbe_layout layout)
{
    switch (layout) {
        case MBE_LAYOUT_DSTAR: return "dstar";
        case MBE_LAYOUT_DMR: return "dmr";
        case MBE_LAYOUT_P25P2: return "p25p2";
        case MBE_LAYOUT_P25P1: return "p25p1";
        case MBE_LAYOUT_YSF_VW: return "ysf_vw";
        default: return "invalid";
    }
}

unsigned mbe_layoutBits(mbe_layout layout)
{
    switch (layout) {
        case MBE_LAYOUT_DSTAR: case MBE_LAYOUT_DMR: case MBE_LAYOUT_P25P2: return 72;
        case MBE_LAYOUT_P25P1: case MBE_LAYOUT_YSF_VW: return 144;
        default: return 0;
    }
}

int mbe_ambeFrameFromBits(mbe_layout layout, const char* b, char fr[4][24])
{
    Cell cells[72];
    if (!fr || !ambeCells(layout, cells)) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(b, 72);
    if (ret < 0) return ret;
    std::memset(fr, 0, 96);
    for (int i = 0; i < 72; ++i) fr[cells[i].row][cells[i].col] = b[i];
    return 0;
}

int mbe_ambeFrameToBits(mbe_layout layout, const char fr[4][24], char* b)
{
    Cell cells[72];
    if (!b || !ambeCells(layout, cells)) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(&fr[0][0], 96);
    if (ret < 0) return ret;
    for (int i = 0; i < 72; ++i) b[i] = fr[cells[i].row][cells[i].col];
    return 0;
}

// mbelib-neo 2.4's D-STAR DV data helpers: the D-STAR layout packed LSB first, as the GMSK
// modulator sends it (sync word not included).
int mbe_encodeDStarDVData(const char fr[4][24], unsigned char bytes9[9])
{
    char b[72];
    if (!fr || !bytes9) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = mbe_ambeFrameToBits(MBE_LAYOUT_DSTAR, fr, b);
    if (ret < 0) return ret;
    mbe_packBits(b, 72, bytes9, 0);
    return 0;
}

int mbe_decodeDStarDVData(const unsigned char bytes9[9], char fr[4][24])
{
    char b[72];
    if (!bytes9 || !fr) return MBE_STATUS_INVALID_ARGUMENT;
    mbe_unpackBits(bytes9, 72, b, 0);
    return mbe_ambeFrameFromBits(MBE_LAYOUT_DSTAR, b, fr);
}

int mbe_ambeSoftFrameFromBits(mbe_layout layout, const mbe_soft_bit* b, mbe_soft_bit fr[4][24])
{
    Cell cells[72];
    if (!fr || !ambeCells(layout, cells)) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validateSoft(b, 72);
    if (ret < 0) return ret;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 24; ++c) fr[r][c] = mbe_softBitFromHard(0, 255);
    for (int i = 0; i < 72; ++i) fr[cells[i].row][cells[i].col] = b[i];
    return 0;
}

int mbe_imbeFrameFromBits(mbe_layout layout, const char* b, char fr[8][23])
{
    Cell cells[144];
    if (!fr || !imbeCells(layout, cells)) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(b, 144);
    if (ret < 0) return ret;
    std::memset(fr, 0, 8 * 23);
    for (int i = 0; i < 144; ++i) fr[cells[i].row][cells[i].col] = b[i];
    return 0;
}

int mbe_imbeFrameToBits(mbe_layout layout, const char fr[8][23], char* b)
{
    Cell cells[144];
    if (!b || !imbeCells(layout, cells)) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(&fr[0][0], 8 * 23);
    if (ret < 0) return ret;
    for (int i = 0; i < 144; ++i) b[i] = fr[cells[i].row][cells[i].col];
    return 0;
}

int mbe_imbeSoftFrameFromBits(mbe_layout layout, const mbe_soft_bit* b, mbe_soft_bit fr[8][23])
{
    Cell cells[144];
    if (!fr || !imbeCells(layout, cells)) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validateSoft(b, 144);
    if (ret < 0) return ret;
    for (int r = 0; r < 8; ++r) for (int c = 0; c < 23; ++c) fr[r][c] = mbe_softBitFromHard(0, 255);
    for (int i = 0; i < 144; ++i) fr[cells[i].row][cells[i].col] = b[i];
    return 0;
}

int mbe_ysfVd2ToAmbe2450Data(const char b[104], char d[49])
{
    if (!d) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(b, 104);
    if (ret < 0) return ret;
    char raw[104];
    for (int i = 0; i < 104; ++i) raw[vd2Interleave[i]] = b[i];
    int disagreements = 0;
    for (int i = 0; i < 27; ++i) {
        int ones = raw[3 * i] + raw[3 * i + 1] + raw[3 * i + 2];
        d[i] = static_cast<char>(ones > 1 ? 1 : 0);
        if (ones == 1 || ones == 2) ++disagreements;
    }
    for (int i = 81; i < 103; ++i) d[i - 81 + 27] = raw[i];
    return disagreements;
}

int mbe_ambe2450DataToYsfVd2(const char d[49], char b[104])
{
    if (!b) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(d, 49);
    if (ret < 0) return ret;
    char raw[104];
    for (int i = 0; i < 27; ++i) raw[3 * i] = raw[3 * i + 1] = raw[3 * i + 2] = d[i];
    for (int i = 81; i < 103; ++i) raw[i] = d[i - 81 + 27];
    raw[103] = 0;
    for (int i = 0; i < 104; ++i) b[i] = raw[vd2Interleave[i]];
    return 0;
}

void mbe_packBits(const char* b, unsigned count, uint8_t* bytes, int msbFirst)
{
    if (!b || !bytes) return;
    std::memset(bytes, 0, (count + 7) / 8);
    for (unsigned i = 0; i < count; ++i) {
        if (!(b[i] & 1)) continue;
        bytes[i / 8] |= static_cast<uint8_t>(1u << (msbFirst ? 7 - (i % 8) : i % 8));
    }
}

void mbe_unpackBits(const uint8_t* bytes, unsigned count, char* b, int msbFirst)
{
    if (!b || !bytes) return;
    for (unsigned i = 0; i < count; ++i)
        b[i] = static_cast<char>((bytes[i / 8] >> (msbFirst ? 7 - (i % 8) : i % 8)) & 1u);
}

int mbe_ambe2450DataToDvsi(const char d[49], uint8_t bytes7[7])
{
    if (!bytes7) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(d, 49);
    if (ret < 0) return ret;
    char raw[56] = {0};
    for (int i = 0; i < 49; ++i) raw[dvsi49[i]] = d[i];
    mbe_packBits(raw, 56, bytes7, 1);
    return 0;
}

int mbe_dvsiToAmbe2450Data(const uint8_t bytes7[7], char d[49])
{
    if (!bytes7 || !d) return MBE_STATUS_INVALID_ARGUMENT;
    char raw[56];
    mbe_unpackBits(bytes7, 56, raw, 1);
    for (int i = 0; i < 49; ++i) d[i] = raw[dvsi49[i]];
    return 0;
}

} // extern "C"
