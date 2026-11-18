// SPDX-License-Identifier: GPL-3.0-or-later
// Process-result and soft-bit helpers (semantics as mbelib-neo 2.x).
#include <mbevocoder/mbevocoder.h>

#include "internal/bits.hpp"

#include <cstdlib>

extern "C" {

void mbe_initProcessResult(mbe_process_result* result)
{
    if (!result) return;
    result->c0_errors = 0;
    result->protected_errors = 0;
    result->c4_errors = 0;
    result->total_errors = 0;
    result->flags = 0u;
}

void mbe_formatProcessResult(char* str, size_t size, const mbe_process_result* result)
{
    static const struct { unsigned flag; char marker; } markers[] = {
        {MBE_PROCESS_FLAG_ERASURE, 'E'},
        {MBE_PROCESS_FLAG_TONE, 'T'},
        {MBE_PROCESS_FLAG_REPEAT, 'R'},
        {MBE_PROCESS_FLAG_MUTE, 'M'},
    };
    if (!str || size == 0u) return;
    size_t pos = 0;
    int total = result ? result->total_errors : 0;
    if (total < 0) total = 0;
    for (int i = 0; i < total && pos + 1u < size; ++i) str[pos++] = '=';
    if (result) {
        for (const auto& m : markers) {
            if ((result->flags & m.flag) && pos + 1u < size) str[pos++] = m.marker;
        }
    }
    str[pos] = '\0';
}

mbe_soft_bit mbe_softBitFromHard(int bit, uint8_t reliability)
{
    mbe_soft_bit s;
    s.bit = static_cast<uint8_t>(bit & 1);
    s.reliability = reliability;
    return s;
}

mbe_soft_bit mbe_softBitFromLlr(int16_t llr)
{
    mbe_soft_bit s;
    s.bit = static_cast<uint8_t>(llr < 0 ? 1 : 0);
    int mag = std::abs(static_cast<int>(llr));
    s.reliability = static_cast<uint8_t>(mag > 255 ? 255 : mag);
    return s;
}

int mbe_softBitsFromHard(const char* bits, mbe_soft_bit* soft, size_t count, uint8_t reliability)
{
    if (!soft) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = mbe::bits::validate(bits, count);
    if (ret < 0) return ret;
    for (size_t i = 0; i < count; ++i) soft[i] = mbe_softBitFromHard(bits[i], reliability);
    return 0;
}

int mbe_softBitsFromLlr(const int16_t* llr, mbe_soft_bit* soft, size_t count)
{
    if (!llr || !soft) return MBE_STATUS_INVALID_ARGUMENT;
    for (size_t i = 0; i < count; ++i) soft[i] = mbe_softBitFromLlr(llr[i]);
    return 0;
}

} // extern "C"
