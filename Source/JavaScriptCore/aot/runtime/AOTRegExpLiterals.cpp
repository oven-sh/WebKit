/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpLiterals.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTRegExpRecords.h"
#include "AbstractSlotVisitorInlines.h"
#include "RegExp.h"
#include "SlotVisitorInlines.h"
#include "VM.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RegExpLiterals);

template<typename Visitor>
void RegExpLiterals::visitRoots(Visitor& visitor)
{
    m_writtenCells.forEachRegionToVisit(visitor, [&](size_t, size_t) {
        for (RegExp* regExp : m_cells.values())
            visitor.appendUnbarriered(regExp);
    });
}

RegExpLiterals::RegExpLiterals(CacheRegistry& registry)
{
    registry.add(CacheKind::StrongRoots, "regexp literals"_s, *this, RootVolatility::GreyedByExecution);
}

RegExp* RegExpLiterals::regExpFor(VM& vm, RegExpRecordID id, OptionSet<Yarr::Flags> flags)
{
    uint32_t key = keyOf(id, flags);
    if (RegExp* regExp = m_cells.get(key))
        return regExp;

    const ImageRegExp& record = RegExpRecords(*vm.aotState().image).at(id);
    RegExp* regExp = RegExp::createFromImage(vm, regExpPatternOf(vm, record), flags, record);
    m_cells.set(key, regExp);
    m_writtenCells.recordAll();
    return regExp;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
