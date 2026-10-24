/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <algorithm>
#include <array>
#include <bit>
#include <utility>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringHasherInlines.h>

namespace JSC { namespace AOT {

// A hash table from a string to its position in a list of names, which the C++ compiler fills: it is read-only data, and finding a name takes the hash that the string has already.
constexpr size_t numberOfSlotsForNames(size_t numberOfNames)
{
    return std::bit_ceil(numberOfNames + numberOfNames / 2 + 1);
}

template<size_t numberOfNames>
using BuiltinNameSlots = std::array<uint32_t, numberOfSlotsForNames(numberOfNames)>;

constexpr uint32_t hashBitsOfNameSlot(unsigned hash)
{
    return (hash >> 8) << 16;
}

constexpr uint32_t positionBitsOfNameSlot = 0xffff;

// Each run of names is hashed in a constant expression of its own, which keeps every evaluation below the compiler's limit of steps.
constexpr size_t numberOfNamesHashedAtOnce = 256;

template<const auto& names, size_t run>
consteval auto hashRunOfNames()
{
    constexpr size_t first = run * numberOfNamesHashedAtOnce;
    std::array<unsigned, std::min(numberOfNamesHashedAtOnce, std::size(names) - first)> hashes { };
    for (size_t index = 0; index < hashes.size(); ++index)
        hashes[index] = StringHasher::computeLiteralHashAndMaskTop8Bits(names[first + index]);
    return hashes;
}

template<const auto& names, size_t run>
inline constexpr auto hashesOfRunOfNames = hashRunOfNames<names, run>();

template<const auto& names, size_t... runs>
consteval BuiltinNameSlots<std::size(names)> makeBuiltinNameSlots(std::index_sequence<runs...>)
{
    static_assert(std::size(names) < positionBitsOfNameSlot);
    BuiltinNameSlots<std::size(names)> slots { };
    uint32_t position = 0;
    auto insertRun = [&](const auto& hashes) {
        for (unsigned hash : hashes) {
            size_t slot = hash & (slots.size() - 1);
            while (slots[slot])
                slot = (slot + 1) & (slots.size() - 1);
            slots[slot] = hashBitsOfNameSlot(hash) | ++position;
        }
    };
    (insertRun(hashesOfRunOfNames<names, runs>), ...);
    return slots;
}

template<const auto& names>
consteval BuiltinNameSlots<std::size(names)> makeBuiltinNameSlots()
{
    return makeBuiltinNameSlots<names>(std::make_index_sequence<(std::size(names) + numberOfNamesHashedAtOnce - 1) / numberOfNamesHashedAtOnce> { });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
