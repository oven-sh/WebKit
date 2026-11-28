/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "CollectionScope.h"
#include "JSString.h"
#include "MarkedBlockCage.h"
#include <span>
#include <type_traits>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct State;

struct CaseConversions {
    enum class LetterCase : uint8_t { Lower, Upper };

    static constexpr size_t maxLengthOfRememberedString = 64;
    static constexpr uint32_t multiplier = 0x9e3779b1u;
    static constexpr unsigned shift = 25;
    static constexpr size_t ways = 4;
    static constexpr unsigned setShift = 3;
    static constexpr size_t indexOf(uint32_t source, LetterCase letterCase, size_t way = 0) { return (source * multiplier >> shift) << setShift | way << 1 | static_cast<size_t>(letterCase); }
    static constexpr size_t numberOfSets = size_t { 1 } << (32 - shift);
    static constexpr size_t numberOfEntries = numberOfSets << setShift;
    static_assert(2 * ways == 1u << setShift);

    static CaseConversions& ensure(State&);

    ALWAYS_INLINE JSString* tryFind(const JSString* source, LetterCase letterCase) const
    {
        uintptr_t distance = std::bit_cast<uintptr_t>(source) - cageBase;
        if (distance >= MarkedBlockCage::maxSize)
            return nullptr;
        uint32_t encoded = static_cast<uint32_t>(distance >> MarkedBlockCage::shift);
        for (size_t way = 0; way < ways; ++way) {
            uint64_t entry = entries[indexOf(encoded, letterCase, way)];
            if (entry >> 32 == encoded)
                return uncheckedDowncast<JSString>(MarkedBlockCage::decode(cageBase, static_cast<uint32_t>(entry)));
        }
        return nullptr;
    }
    void remember(const JSString* source, LetterCase, const JSString* converted);
    JSString* convertedString(VM&, std::span<const Latin1Character>);
    size_t removeDeadEntries(VM&, CollectionScope);

    static constexpr size_t numberOfConvertedStrings = 256;

    uint64_t entries[numberOfEntries];
    uint64_t setsWithEntries[numberOfSets / 64];
    uint64_t convertedStrings[numberOfConvertedStrings];
    uintptr_t cageBase;
};
static_assert(std::is_trivially_default_constructible_v<CaseConversions> && !OBJECT_OFFSETOF(CaseConversions, entries));

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
