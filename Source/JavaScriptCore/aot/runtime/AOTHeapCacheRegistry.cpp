/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHeapCacheRegistry.h"

#if ENABLE(AOT)

#include "AOTRuntimeValidation.h"
#include "AbstractSlotVisitorInlines.h"
#include "SlotVisitorInlines.h"
#include <algorithm>
#include <wtf/DataLog.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CacheRegistry);

CacheRegistry::CacheRegistry() = default;
CacheRegistry::~CacheRegistry() = default;

static bool isAllZero(std::span<const uint8_t> bytes)
{
    uint8_t combinedBits = 0;
    for (uint8_t byte : bytes)
        combinedBits |= byte;
    return !combinedBits;
}

// Reading a zero-filled page that was never written does not commit it. Writing does.
static void zeroNonzeroPieces(std::span<uint8_t> bytes)
{
    constexpr size_t pieceSize = 4 * KB;
    while (!bytes.empty()) {
        auto piece = bytes.first(std::min(bytes.size(), pieceSize));
        if (!isAllZero(piece))
            zeroSpan(piece);
        bytes = bytes.subspan(piece.size());
    }
}

[[noreturn]] static void crashOnUnrecordedWrite(VM& vm, ASCIILiteral name)
{
    dataLogLn("AOT: cache \"", name, "\" was written without a record");
    crashOnValidationFailure(vm, { ValidationFailureKind::CacheEntryDropped, { }, { }, { } });
}

void CacheRegistry::append(Entry&& entry)
{
    Locker locker { m_lock };
    m_entries.append(WTF::move(entry));
}

void CacheRegistry::addStrongRoots(ASCIILiteral name, std::span<JSCell* const> cellsOrNull, RootVolatility volatility)
{
    Entry entry {
        .kind = EntryKind::StrongRoots,
        .volatility = volatility,
        .name = name,
        .owner = const_cast<JSCell**>(cellsOrNull.data()),
        .size = cellsOrNull.size(),
    };
    auto visitCells = [](Entry& entry, auto& visitor) {
        for (JSCell* cell : unsafeMakeSpan(static_cast<JSCell* const*>(entry.owner), entry.size)) {
            if (cell)
                visitor.appendUnbarriered(cell);
        }
    };
    entry.visitWithSlotVisitor = visitCells;
    entry.visitWithAbstractSlotVisitor = visitCells;
    append(WTF::move(entry));
}

void CacheRegistry::addClearedEachCollection(ASCIILiteral name, std::span<uint8_t> zeroedByClear)
{
    Entry entry {
        .kind = EntryKind::ClearedEachCollection,
        .name = name,
        .owner = zeroedByClear.data(),
        .size = zeroedByClear.size(),
    };
    entry.clear = [](Entry& entry) {
        zeroNonzeroPieces(unsafeMakeSpan(static_cast<uint8_t*>(entry.owner), entry.size));
    };
    append(WTF::move(entry));
}

void CacheRegistry::addClearedEachCollection(ASCIILiteral name, std::span<uint8_t> zeroedByClear, RememberedSlotSet& writtenRegions)
{
    Entry entry {
        .kind = EntryKind::ClearedEachCollection,
        .name = name,
        .owner = zeroedByClear.data(),
        .size = zeroedByClear.size(),
        .writtenRegions = &writtenRegions,
    };
    entry.clear = [](Entry& entry) {
        auto bytes = unsafeMakeSpan(static_cast<uint8_t*>(entry.owner), entry.size);
        entry.writtenRegions->forEachRegionToClear([&](size_t begin, size_t end) {
            zeroSpan(bytes.subspan(begin, end - begin));
        });
    };
    append(WTF::move(entry));
}

template<typename Visitor>
void CacheRegistry::visitRoots(Visitor& visitor, RootVolatility volatility)
{
    Locker locker { m_lock };
    for (Entry& entry : m_entries) {
        if (entry.kind != EntryKind::StrongRoots || entry.volatility != volatility)
            continue;

        if constexpr (std::is_same_v<Visitor, SlotVisitor>)
            entry.visitWithSlotVisitor(entry, visitor);
        else
            entry.visitWithAbstractSlotVisitor(entry, visitor);
    }

    // The GC verifier marks on its own and leaves the state of the real collection alone.
    if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
        if (volatility == RootVolatility::GreyedByExecution)
            m_hasVisitedExecutionRoots = true;
    }
}

template void CacheRegistry::visitRoots<SlotVisitor>(SlotVisitor&, RootVolatility);
template void CacheRegistry::visitRoots<AbstractSlotVisitor>(AbstractSlotVisitor&, RootVolatility);

void CacheRegistry::finalize(VM& vm, CollectionScope scope, CacheValidation validation)
{
    bool shouldValidate = validation == CacheValidation::On;
    for (Entry& entry : m_entries) {
        switch (entry.kind) {
        case EntryKind::StrongRoots:
        case EntryKind::Observer:
            break;
        case EntryKind::WeakByKey:
            entry.removeDeadEntries(entry, vm, scope);
            // An Eden collection kills young cells only, and a store of a young cell is recorded. So the regions it skipped have no dead entry.
            if (shouldValidate && scope == CollectionScope::Eden && entry.removeDeadEntries(entry, vm, CollectionScope::Full))
                crashOnUnrecordedWrite(vm, entry.name);
            break;
        case EntryKind::ClearedEachCollection:
            entry.clear(entry);
            if (shouldValidate && entry.size && !isAllZero(unsafeMakeSpan(static_cast<const uint8_t*>(entry.owner), entry.size)))
                crashOnUnrecordedWrite(vm, entry.name);
            break;
        }
    }

    for (Entry& entry : m_entries) {
        if (entry.kind == EntryKind::Observer)
            entry.didFinishCollection(entry, vm, scope);
    }

    m_hasVisitedExecutionRoots = false;
}

void CacheRegistry::willAnalyzeHeap(VM& vm)
{
    for (Entry& entry : m_entries) {
        if (entry.willAnalyzeHeap)
            entry.willAnalyzeHeap(entry, vm);
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
