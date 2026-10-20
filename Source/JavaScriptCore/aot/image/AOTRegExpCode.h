/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTRelocation.h"
#include "YarrClassTable.h"
#include "YarrSharedRoutines.h"
#include "YarrSharedTables.h"
#include <wtf/BitSet.h>
#include <wtf/FastMalloc.h>
#include <wtf/Hasher.h>
#include <wtf/Variant.h>
#include <wtf/Vector.h>

namespace JSC::AOT {

struct PlainData {
    Vector<uint8_t> bytes;

    friend bool operator==(const PlainData&, const PlainData&) = default;
};

struct Latin1BitPlane {
    static constexpr unsigned planesPerByteTable = 8;
    static_assert(Yarr::CharacterClass::latin1TableSize == 256);

    static uint8_t preferredBit(const WTF::BitSet<256>& members) { return WTF::computeHash(members.storage()) % planesPerByteTable; }

    WTF::BitSet<256> members;
    uint8_t bit { 0 };

    friend bool operator==(const Latin1BitPlane&, const Latin1BitPlane&) = default;
};

struct Literal64 {
    uint64_t value { 0 };

    friend bool operator==(const Literal64&, const Literal64&) = default;
};

using RegExpSymbol = Variant<Yarr::SharedTableID, PlainData, Latin1BitPlane, Yarr::ClassTable, Yarr::SharedRoutine, Literal64>;

inline void add(Hasher& hasher, const PlainData& data)
{
    add(hasher, data.bytes);
}

inline void add(Hasher& hasher, const Latin1BitPlane& plane)
{
    add(hasher, plane.members.storage(), plane.bit);
}

inline void add(Hasher& hasher, const Literal64& literal)
{
    add(hasher, literal.value);
}

struct RegExpCode {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(RegExpCode);

    RelocatableCode body;
    Vector<RegExpSymbol> symbols;
    PCRelativeReach reach { PCRelativeReach::Near };
    unsigned analysisLimitHits { 0 };
    bool hasElidedFrame { false };
};

} // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
