// SPDX-License-Identifier: GPL-3.0-or-later
// Small helpers for the char-per-bit arrays used throughout the mbelib API.
#pragma once

#include <mbevocoder/mbevocoder.h>

#include <cstddef>
#include <cstdint>

namespace mbe::bits {

inline int validate(const char* bits, size_t count)
{
    if (!bits) return MBE_STATUS_INVALID_ARGUMENT;
    for (size_t i = 0; i < count; ++i)
        if (bits[i] != 0 && bits[i] != 1) return MBE_STATUS_INVALID_BITS;
    return 0;
}

inline int validateSoft(const mbe_soft_bit* bits, size_t count)
{
    if (!bits) return MBE_STATUS_INVALID_ARGUMENT;
    for (size_t i = 0; i < count; ++i)
        if (bits[i].bit > 1u) return MBE_STATUS_INVALID_BITS;
    return 0;
}

// bits[high], bits[high-1], ..., bits[low] -> integer, bits[high] most significant.
inline uint32_t fromDescending(const char* bits, int high, int low)
{
    uint32_t v = 0;
    for (int i = high; i >= low; --i) v = (v << 1) | static_cast<uint32_t>(bits[i] & 1);
    return v;
}

inline void toDescending(uint32_t v, char* bits, int high, int low)
{
    for (int i = low; i <= high; ++i) { bits[i] = static_cast<char>(v & 1u); v >>= 1; }
}

// bits[first], bits[first+1], ... -> integer, bits[first] most significant.
inline uint32_t fromAscending(const char* bits, int first, int count)
{
    uint32_t v = 0;
    for (int i = 0; i < count; ++i) v = (v << 1) | static_cast<uint32_t>(bits[first + i] & 1);
    return v;
}

inline void toAscending(uint32_t v, char* bits, int first, int count)
{
    for (int i = count - 1; i >= 0; --i) { bits[first + i] = static_cast<char>(v & 1u); v >>= 1; }
}

} // namespace mbe::bits
