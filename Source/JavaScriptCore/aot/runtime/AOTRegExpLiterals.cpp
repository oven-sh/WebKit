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
#include <bit>
#include <wtf/HashFunctions.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RegExpLiterals);

namespace {

struct Literal {
    const ImageRegExp* record;
    OptionSet<Yarr::Flags> flags;
};

struct LiteralTranslator {
    static unsigned hash(const Literal& literal)
    {
        constexpr unsigned bitsAboveFlags = 64 - 8 * sizeof(literal.flags);
        return intHash(static_cast<uint64_t>(std::bit_cast<uintptr_t>(literal.record)) ^ static_cast<uint64_t>(literal.flags.toRaw()) << bitsAboveFlags);
    }

    static bool equal(RegExp* regExp, const Literal& literal)
    {
        return regExp->imageRecord() == literal.record && regExp->flags() == literal.flags;
    }
};

} // anonymous namespace

unsigned RegExpLiterals::CellHash::hash(RegExp* regExp)
{
    return LiteralTranslator::hash({ regExp->imageRecord(), regExp->flags() });
}

template<typename Visitor>
void RegExpLiterals::visitRoots(Visitor& visitor)
{
    // A cell lasts as long as the program. One that a collection has marked is old, and only the next full collection has to mark it again.
    size_t first = 0;
    if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
        if (visitor.collectionScope() == CollectionScope::Full && m_lastFullVisitVersion != visitor.markingVersion())
            m_lastFullVisitVersion = visitor.markingVersion();
        else
            first = m_numberOfCellsVisited;
        m_numberOfCellsVisited = m_cellsInOrderOfCreation.size();
    }
    for (RegExp* regExp : m_cellsInOrderOfCreation.span().subspan(first))
        visitor.appendUnbarriered(regExp);
}

RegExpLiterals::RegExpLiterals(CacheRegistry& registry)
{
    registry.add(CacheKind::StrongRoots, "regexp literals"_s, *this, RootVolatility::GreyedByExecution);
}

RegExp* RegExpLiterals::regExpFor(VM& vm, RegExpRecordID id, OptionSet<Yarr::Flags> flags)
{
    if (!m_records) [[unlikely]]
        m_records = &RegExpRecords(*vm.aotState().image).at(id) - id.index();
    const ImageRegExp& record = m_records[id.index()];
    if (auto known = m_cells.find<LiteralTranslator>(Literal { &record, flags }); known != m_cells.end())
        return *known;

    RegExp* regExp = RegExp::createFromImage(vm, regExpPatternOf(vm, record), flags, record);
    m_cells.add(regExp);
    m_cellsInOrderOfCreation.append(regExp);
    return regExp;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
