/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTRegExpRecord.h"
#include "RememberedSlot.h"
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class RegExp;
class VM;

namespace AOT {

class CacheRegistry;

class RegExpLiterals {
    WTF_MAKE_TZONE_ALLOCATED(RegExpLiterals);
    WTF_MAKE_NONCOPYABLE(RegExpLiterals);
public:
    explicit RegExpLiterals(CacheRegistry&);

    JS_EXPORT_PRIVATE RegExp* regExpFor(VM&, RegExpRecordID, OptionSet<Yarr::Flags>);

    template<typename Visitor> void visitRoots(Visitor&);

private:
    static constexpr unsigned variantsPerRecord = 4;
    static_assert(variantsPerRecord < sizeof(ImageRegExp), "The records fill a section of less than 4 GB, so a key neither overflows nor is the empty or the deleted value.");

    static constexpr uint32_t keyOf(RegExpRecordID record, OptionSet<Yarr::Flags> flags)
    {
        uint32_t variant = (flags.contains(Yarr::Flags::Global) ? 1 : 0) | (flags.contains(Yarr::Flags::HasIndices) ? 2 : 0);
        return record.index() * variantsPerRecord + variant;
    }

    RememberedSlotSet m_writtenCells;
    UncheckedKeyHashMap<uint32_t, RegExp*, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_cells;
};

} // namespace AOT

} // namespace JSC

#endif // ENABLE(AOT)
