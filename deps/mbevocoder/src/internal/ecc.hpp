// SPDX-License-Identifier: GPL-3.0-or-later
// Internal error-correction primitives. Codeword values are right-aligned integers:
//   Golay(23,12): data in bits 22..11, parity in bits 10..0
//   Golay(24,12): Golay(23,12) codeword << 1 | overall even-parity bit
//   Hamming(15,11): data in bits 14..4, parity in bits 3..0, with the standard IMBE generator
//                   or the IMBE 7100x4400 (ProVoice) generator
// Generator constants originate from mbelib (ISC licence).
#pragma once

#include <cstdint>

namespace mbe::ecc {

uint32_t golay23Encode(uint32_t data12);
uint32_t golay24Encode(uint32_t data12);

// Returns the 23-bit error pattern for a received codeword (perfect code: always ≤3 bits).
uint32_t golay23ErrorPattern(uint32_t codeword23);

// Decode in place: *codeword becomes the corrected 23-bit codeword. Returns the number of
// corrected bits (0..3) counted over the whole codeword.
int golay23Correct(uint32_t* codeword23);

// Decode a 24-bit codeword: corrects up to 3 errors in the 23-bit part, then flips the overall
// parity bit if the result has odd parity and counts it. A return of 4 is a weight-4 error
// pattern, which the extended code detects but cannot correct (the word is a guess).
int golay24Correct(uint32_t* codeword24);

enum class HammingVariant { Standard, Imbe7100 };

uint32_t hamming15Encode(uint32_t data11, HammingVariant v);
// Corrects a single error in place; returns 0 or 1.
int hamming15Correct(uint32_t* codeword15, HammingVariant v);
// Extracts the 11 data bits from a (corrected) codeword.
uint32_t hamming15Data(uint32_t codeword15, HammingVariant v);

// Pseudo-random modulation sequence used by AMBE/AMBE+2/IMBE: x[n+1] = (173 x[n] + 13849) mod 65536,
// x[0] = 16 * seed; output bit n (n >= 1) is x[n] >> 15. Fills bits[0..count) with x[1..count] >> 15.
void pnSequence(uint32_t seed12, char* bits, unsigned count);
// First 24 bits of the sequence as an integer, bit 23 = x[1] >> 15 (matches OP25's pr_n table).
uint32_t pn24(uint32_t seed12);

} // namespace mbe::ecc
