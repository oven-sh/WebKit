/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramStatics.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTObjectModel.h"
#include "AOTOperationData.h"
#include "AOTProgramStrings.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "FastMarkAppender.h"
#include "JSCInlines.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <wtf/Atomics.h>
#include <wtf/MathExtras.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ProgramStatics);

static uint32_t sizeFilledAtProgramEntry(const ProgramTables& tables)
{
    std::span<const StaticsLayout> layout = tables.staticsLayout();
    return layout.empty() ? 0 : layout[0].sizeFilledAtProgramEntry;
}

static std::optional<FunctionID> findFunctionVariable(const ProgramTables& tables, uint32_t offset)
{
    std::span<const FunctionVariableData> variables = tables.functionVariables();
    auto found = std::ranges::lower_bound(variables, offset, { }, &FunctionVariableData::offset);
    if (found == variables.end() || found->offset != offset)
        return std::nullopt;
    return FunctionID(found->function);
}

static bool readCreatesCell(const Instance& instance, SlotRepresentation representation, const void* slot)
{
    if (traitsOf(representation).valueKind != SlotValueKind::LiteralString)
        return false;
    uint64_t word = readSlotBits(representation, slot);
    if (!isLiteralStringWord(word))
        return false;
    uint32_t number = literalStringIndex(static_cast<uint32_t>(word));
    RELEASE_ASSERT(number < instance.tables().literalStrings().size());
    return !instance.runtime.constants.literalStrings[number];
}

void ProgramStatics::initializeStatics(uint32_t firstWord, uint32_t endWord)
{
    auto* words = reinterpret_cast<uint64_t*>(m_instance.statics());
    std::span<const StaticsRun> runs = m_tables.staticsRuns();
    for (auto run = std::ranges::lower_bound(runs, firstWord, { }, &StaticsRun::firstWord); run != runs.end() && run->firstWord < endWord; ++run)
        std::ranges::fill(std::span { words + run->firstWord, run->numberOfWords }, run->bits());
}

void ProgramStatics::initializeProgramStatics()
{
    if (m_readOnlyStaticsSize)
        OSAllocator::protect(m_instance.statics(), m_readOnlyStaticsSize, true, true);
    initializeStatics(0, sizeFilledAtProgramEntry(m_tables) / sizeof(uint64_t));
    WTF::storeStoreFence();
    m_readOnlyStaticsSize = 0;
    m_programStaticsInitialized = true;
}

JSValue ProgramStatics::readVariable(Instance& instance, SlotData slot)
{
    VM& vm = *instance.vm;
    State& state = *instance.state;
    ClosureTable& closures = state.objects->closures;
    uint32_t offset = slot.offset;
    if (slot.representation() == SlotRepresentation::DeferredClosure)
        return closures.closure(vm, FunctionID(offset));
    ProgramStatics& statics = *state.statics;
    statics.ensureProgramStaticsInitialized();
    if (JSValue value = readSlot(slot.representation(), instance.statics() + offset, statics.m_strings))
        return value;
    if (std::optional<FunctionID> function = findFunctionVariable(statics.m_tables, offset))
        return closures.closure(vm, *function);
    return { };
}

JSValue ProgramStatics::readVariableWithoutAllocating(const Instance& instance, SlotData slot)
{
    const State& state = *instance.state;
    const ClosureTable& closures = state.objects->closures;
    uint32_t offset = slot.offset;
    if (slot.representation() == SlotRepresentation::DeferredClosure)
        return closures.closureIfCreated(FunctionID(offset));
    const ProgramStatics& statics = *state.statics;
    if (!statics.m_programStaticsInitialized && offset < sizeFilledAtProgramEntry(statics.m_tables))
        return { };
    const uint8_t* address = instance.statics() + offset;
    if (readCreatesCell(instance, slot.representation(), address))
        return { };
    if (JSValue value = readSlot(slot.representation(), address, statics.m_strings))
        return value;
    if (std::optional<FunctionID> function = findFunctionVariable(statics.m_tables, offset))
        return closures.closureIfCreated(*function);
    return { };
}

void ProgramStatics::writeVariable(Instance& instance, SlotData slot, JSValue value)
{
    SlotRepresentation representation = slot.representation();
    RELEASE_ASSERT(representation != SlotRepresentation::DeferredClosure);
    ProgramStatics& statics = *instance.state->statics;
    statics.ensureProgramStaticsInitialized();
    uint8_t* address = instance.statics() + slot.offset;
    if (!value) {
        RELEASE_ASSERT(traitsOf(representation).hasAbsent);
        clearSlot(representation, address);
        return;
    }
    bool didWrite = tryWriteSlot(statics.m_strings, representation, address, value);
    RELEASE_ASSERT(didWrite);
    if (traitsOf(representation).holdsCell)
        statics.didStoreRootInStatics(slot.offset);
}

template<typename Appender>
ALWAYS_INLINE void ProgramStatics::visitRegion(Appender& appender, size_t begin, size_t end)
{
    const uint8_t* statics = m_instance.statics();
    std::span<const StaticsRange> roots = m_tables.staticsRoots();
    begin = std::max<size_t>(begin, m_readOnlyStaticsSize);
    auto first = std::ranges::partition_point(roots, [&](const StaticsRange& range) {
        return range.end() <= begin;
    });
    for (const StaticsRange& range : std::span { first, roots.end() }) {
        if (range.offset >= end)
            break;
        size_t endInRange = std::min<size_t>(range.end(), end);
        for (size_t offset = std::max<size_t>(range.offset, begin); offset < endInRange; offset += sizeof(EncodedJSValue))
            appender.appendUnbarriered(JSValue::decode(*reinterpret_cast<const EncodedJSValue*>(statics + offset)));
    }
}

template<typename Visitor>
void ProgramStatics::visitRoots(Visitor& visitor)
{
    visitor.appendUnbarriered(JSValue::decode(m_instance.runtime.exceptions.thrownValue));
    bool isDeferred = m_slots.tryDeferFullVisit(visitor, [](SlotVisitor& visitor, void* owner, size_t begin, size_t end) {
        appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
            static_cast<ProgramStatics*>(owner)->visitRegion(appender, begin, end);
        });
    }, this);
    if (isDeferred)
        return;
    appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
        m_slots.forEachRegionToVisit(visitor, [&](size_t begin, size_t end) ALWAYS_INLINE_LAMBDA {
            visitRegion(appender, begin, end);
        });
    });
}

template void ProgramStatics::visitRoots(AbstractSlotVisitor&);
template void ProgramStatics::visitRoots(SlotVisitor&);

ProgramStatics::ProgramStatics(State& state)
    : m_instance(state.instance)
    , m_tables(state.tables)
    , m_strings(*state.strings)
    , m_slots({ state.instance.statics(), state.tables.instanceSizes.sizeOfStatics }, InstanceLayout::cardShift, { state.instance.cards(), RememberedSlotSet::numberOfCardsFor(state.tables.instanceSizes.sizeOfStatics, InstanceLayout::cardShift) })
{
    uint8_t* statics = m_instance.statics();
    uint32_t filled = sizeFilledAtProgramEntry(m_tables);
    initializeStatics(filled / sizeof(uint64_t), std::numeric_limits<uint32_t>::max());
    // Compiled code may load from anywhere in the statics ahead of the branch that needs the value, so only stores are caught until the program is entered.
    m_readOnlyStaticsSize = static_cast<uint32_t>(WTF::roundDownToMultipleOf(pageSize(), static_cast<size_t>(filled)));
    if (m_readOnlyStaticsSize)
        OSAllocator::protect(statics, m_readOnlyStaticsSize, true, false);
    state.caches->add(CacheKind::StrongRoots, "statics"_s, *this, RootVolatility::GreyedByExecution);
    for (const StaticsRange& range : m_tables.staticsClearedEachCollection()) {
        RELEASE_ASSERT(range.offset >= filled);
        state.caches->addClearedEachCollection("statics cleared each collection"_s, { statics + range.offset, range.size });
    }
}

ProgramStatics::~ProgramStatics()
{
    if (m_readOnlyStaticsSize)
        OSAllocator::protect(m_instance.statics(), m_readOnlyStaticsSize, true, true);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
