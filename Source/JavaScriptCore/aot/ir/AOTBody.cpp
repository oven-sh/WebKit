/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBody.h"

#if ENABLE(AOT)

#include "AOTThreads.h"
#include <algorithm>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(!offsetof(Wire::ValueHead, opcode) && offsetof(Wire::ValueHead, position) == sizeof(uint32_t));
static_assert(numberOfEnumerators<Opcode> <= BodyValue::opcodeMask);
static_assert(static_cast<size_t>(RemappedIDKind::Data) + 1 == numberOfRemappedIDKinds);

static bool isConstructionWrittenAsCallInBody(Opcode opcode, uint16_t flags)
{
    std::optional<unsigned> mask = FlagMask::isDoneInPlace(opcode);
    return mask && flags & *mask;
}

static bool isConstructionInBody(Opcode opcode, uint16_t flags)
{
    bool callsWithNewTarget = accessClassOf(opcode) == AccessClass::IndirectCall && OperandPosition::newTarget(opcode).has_value();
    return callsWithNewTarget || isConstructionWrittenAsCallInBody(opcode, flags);
}

WrittenPositionRole writtenPositionRoleOf(uint32_t opcodeWord, uint16_t flags)
{
    if (!(opcodeWord & Wire::WrittenPositionFollows))
        return WrittenPositionRole::None;
    if (opcodeWord & Wire::WrittenPositionIsForCallers)
        return WrittenPositionRole::StartForCallers;

    Opcode opcode = static_cast<Opcode>(opcodeWord & BodyValue::opcodeMask);
    if (isConstructionInBody(opcode, flags))
        return WrittenPositionRole::StartOfConstruction;
    if (opcode == Opcode::IteratorOpen)
        return WrittenPositionRole::WhereListIsWritten;
    return WrittenPositionRole::WhereWritten;
}

std::optional<ConstructionStart> BodyValue::constructionStart() const
{
    WrittenPositionRole role = writtenPositionRole();
    switch (role) {
    case WrittenPositionRole::None:
    case WrittenPositionRole::WhereWritten:
        return std::nullopt;
    case WrittenPositionRole::StartOfConstruction:
    case WrittenPositionRole::StartForCallers:
        return ConstructionStart {
            .reported = m_words[1],
            .start = secondPosition(),
            .isWrittenAsCall = isConstructionWrittenAsCallInBody(opcode(), flags()),
            .isForCallers = role == WrittenPositionRole::StartForCallers,
        };
    case WrittenPositionRole::WhereListIsWritten:
        return ConstructionStart { .reported = secondPosition(), .start = m_words[1], .isShownAtStart = true };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Origin BodyValue::originWithSecondPosition() const
{
    return { .position = position(), .inlineFrame = 0, .isTailCall = standsInTailPosition(), .isPartOfConstruction = isPartOfConstruction() };
}

static uint32_t unitTableEntryOf(TypeID type)
{
    return type.bits() + 1u;
}

static void composeTypesOfUnit(const IDRemapTable& placesInWireList, std::span<const TypeID> typesAsRead, std::span<uint32_t> composed)
{
    std::span<const uint32_t> places = unsafeMakeSpan(placesInWireList.entries, placesInWireList.size);
    RELEASE_ASSERT(places.size() == composed.size());
    for (size_t index = 0; index < places.size(); ++index) {
        uint32_t place = places[index];
        if (!place) {
            composed[index] = 0;
            continue;
        }
        RELEASE_ASSERT(place <= typesAsRead.size());
        composed[index] = unitTableEntryOf(typesAsRead[place - 1]);
    }
}

void UnitIDTables::initialize(const ExternalFunctionBodies* bodies, std::span<const TypeID> typesAsRead, const NumbersOfProgramRecords& numbers, const ParallelLoops& loops)
{
    constexpr size_t types = static_cast<size_t>(RemappedIDKind::Type);

    RELEASE_ASSERT(m_units.isEmpty() && m_entries.isEmpty());
    RELEASE_ASSERT(!bodies || bodies->bodyCount == numbers[RemappedIDKind::Function]);

    if (!bodies || !bodies->unitCount) {
        size_t longestList = 0;
        for (size_t kind = 0; kind < numberOfRemappedIDKinds; ++kind) {
            if (kind != types)
                longestList = std::max(longestList, numbers.sizes[kind]);
        }

        m_entries.grow(longestList + typesAsRead.size());
        std::span<uint32_t> ownNumbers = m_entries.mutableSpan().first(longestList);
        std::span<uint32_t> entriesOfTypes = m_entries.mutableSpan().subspan(longestList);
        for (size_t index = 0; index < ownNumbers.size(); ++index)
            ownNumbers[index] = static_cast<uint32_t>(index) + 1;
        for (size_t index = 0; index < entriesOfTypes.size(); ++index)
            entriesOfTypes[index] = unitTableEntryOf(typesAsRead[index]);

        UnitIDRemap unit { };
        for (size_t kind = 0; kind < numberOfRemappedIDKinds; ++kind)
            unit.tables[kind] = { ownNumbers.data(), numbers.sizes[kind] };
        unit.tables[types] = { entriesOfTypes.data(), entriesOfTypes.size() };
        m_units.reserveInitialCapacity(1);
        m_units.append(unit);
        return;
    }

    std::span<const UnitIDRemap> unitsAsWritten = unsafeMakeSpan(bodies->units, bodies->unitCount);
    m_units.reserveInitialCapacity(unitsAsWritten.size());
    m_units.append(unitsAsWritten);

    Vector<size_t> firstEntryOfUnit;
    firstEntryOfUnit.reserveInitialCapacity(unitsAsWritten.size());
    size_t numberOfEntries = 0;
    for (const UnitIDRemap& unit : unitsAsWritten) {
        firstEntryOfUnit.append(numberOfEntries);
        numberOfEntries += unit.tables[types].size;
    }

    m_entries.grow(numberOfEntries);
    std::span<uint32_t> entries = m_entries.mutableSpan();
    forEachIndexInParallel(loops, numberOfUnits(), [&](unsigned index) {
        IDRemapTable& table = m_units[index].tables[types];
        std::span<uint32_t> composed = entries.subspan(firstEntryOfUnit[index], table.size);
        composeTypesOfUnit(unitsAsWritten[index].tables[types], typesAsRead, composed);
        table.entries = composed.data();
    });
}

const UnitIDRemap& UnitIDTables::unitOf(const ExternalFunctionBodies* bodies, FunctionID function) const
{
    if (!bodies || !bodies->unitCount)
        return m_units[0];

    ASSERT(bodies->unitCount == m_units.size());
    std::span<const ExternalFunctionBody> bodiesOfFunctions = unsafeMakeSpan(bodies->bodies, bodies->bodyCount);
    RELEASE_ASSERT(function.index() < bodiesOfFunctions.size());
    return m_units[bodiesOfFunctions[function.index()].unitIndex];
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
