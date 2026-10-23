/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTRegExpRecord.h"
#include "AOTVector.h"
#include <wtf/HashSet.h>
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
    // A cell names its record and its flags, so the set holds the cells and nothing else.
    struct CellHash {
        static unsigned hash(RegExp*);
        static bool equal(RegExp* a, RegExp* b) { return a == b; }
        static constexpr bool safeToCompareToEmptyOrDeleted = false;
    };

    const ImageRegExp* m_records { nullptr };
    UncheckedKeyHashSet<RegExp*, CellHash> m_cells;
    Vector<RegExp*> m_cellsInOrderOfCreation;
    size_t m_numberOfCellsVisited { 0 };
    uint32_t m_lastFullVisitVersion { 0 };
};

} // namespace AOT

} // namespace JSC

#endif // ENABLE(AOT)
