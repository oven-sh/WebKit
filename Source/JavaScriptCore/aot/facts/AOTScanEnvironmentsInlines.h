/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

inline bool ScanOfBody::followsEnvironmentsOfBody() const
{
    return !m_program.admitsInspector;
}

inline bool ScanOfBody::isFirstValueThatMayGoUnseen(const BodyValue& allocation, ValueID listed) const
{
    LayoutID id = allocation.as<Opcode::NewObject>().layout();
    if (!id || !m_program.at(id).isEnvironment() || !followsEnvironmentsOfBody() || !listed)
        return false;
    BodyValue definition = definitionBehindCasts(listed);
    return definition && definition.opcode() == Opcode::Undefined;
}

inline EnvironmentBeingMadeInScan* ScanOfBody::environmentBeingMade(uint32_t root)
{
    auto found = std::ranges::lower_bound(m_environmentsBeingMade, root, { }, &EnvironmentBeingMadeInScan::root);
    return found != m_environmentsBeingMade.end() && found->root == root ? &*found : nullptr;
}

inline void ScanOfBody::followEnvironment(const BodyValue& value)
{
    auto allocation = value.as<Opcode::NewObject>();
    LayoutID id = allocation.layout();
    uint32_t numberOfSlots = static_cast<uint32_t>(m_program.at(id).fields.size());
    uint32_t firstSlot = static_cast<uint32_t>(m_slotsOfEnvironments.size());
    m_slotsOfEnvironments.grow(firstSlot + numberOfSlots);
    std::span<SlotOfEnvironmentInScan> slots = m_slotsOfEnvironments.mutableSpan().subspan(firstSlot);
    std::span<const ValueID> values = allocation.valuesOperands();
    uint32_t place = 0;
    for (uint32_t field : allocation.fields()) {
        if (field < numberOfSlots)
            slots[field].firstValue = place < values.size() && isFirstValueThatMayGoUnseen(value, values[place]) ? FirstValueOfSlotInScan::IsUndefined : FirstValueOfSlotInScan::IsListed;
        ++place;
    }
    if (std::ranges::all_of(slots, [](const SlotOfEnvironmentInScan& slot) { return slot.firstValue == FirstValueOfSlotInScan::IsListed; })) {
        m_slotsOfEnvironments.shrink(firstSlot);
        return;
    }

    if (!std::exchange(m_followsSomeEnvironment, true)) {
        m_hasToDoWithEnvironment.clearAll();
        m_hasToDoWithEnvironment.ensureSize(m_roots.size());
        m_blocksAbove.fill(noDefinition, m_numberOfEdgesInto.size());
        m_isAboveBlockBeingRead.fill(false, m_numberOfEdgesInto.size());
        m_environmentsWhoseLeavingLiesBelow.fill(0, m_numberOfEdgesInto.size());
        m_blocksAboveBlockBeingRead.append(m_blockBeingRead);
        m_isAboveBlockBeingRead[m_blockBeingRead] = true;
    }
    uint32_t root = value.id().index();
    m_hasToDoWithEnvironment.quickSet(root);
    m_environmentsBeingMade.append(EnvironmentBeingMadeInScan { .root = root, .layout = id, .firstSlot = firstSlot, .numberOfSlots = numberOfSlots });
}

inline uint32_t ScanOfBody::nearestBlockAboveBothForEnvironments(uint32_t first, uint32_t second) const
{
    while (first != second) {
        if (first == noDefinition || second == noDefinition)
            return noDefinition;
        uint32_t& lower = first > second ? first : second;
        lower = m_blocksAbove[lower];
    }
    return first;
}

inline void ScanOfBody::noteEdgeForEnvironments(uint32_t from, uint32_t to, KindOfEdgeInScan kind)
{
    if (to <= from) {
        m_hasGivenUpEnvironments |= !m_isAboveBlockBeingRead[to];
        return;
    }
    uint32_t source = kind == KindOfEdgeInScan::ToHandler ? m_blocksAbove[from] : from;
    uint32_t& above = m_blocksAbove[to];
    if (!m_numberOfEdgesInto[to]) {
        above = source;
        return;
    }
    bool standsAboveSource = above != noDefinition && from == m_blockBeingRead && m_isAboveBlockBeingRead[above] && (above != from || kind != KindOfEdgeInScan::ToHandler);
    if (!standsAboveSource)
        above = nearestBlockAboveBothForEnvironments(above, source);
}

inline void ScanOfBody::enterBlockForEnvironments(uint32_t block)
{
    m_blocksOnWayUp.shrink(0);
    uint32_t nearestKept = m_blocksAbove[block];
    for (; nearestKept != noDefinition && !m_isAboveBlockBeingRead[nearestKept]; nearestKept = m_blocksAbove[nearestKept])
        m_blocksOnWayUp.append(nearestKept);
    while (!m_blocksAboveBlockBeingRead.isEmpty() && m_blocksAboveBlockBeingRead.last() != nearestKept)
        m_isAboveBlockBeingRead[m_blocksAboveBlockBeingRead.takeLast()] = false;
    m_blocksOnWayUp.reverse();
    m_blocksOnWayUp.append(block);
    for (uint32_t above : m_blocksOnWayUp) {
        m_blocksAboveBlockBeingRead.append(above);
        m_isAboveBlockBeingRead[above] = true;
    }
}

inline void ScanOfBody::noteEnvironmentLeaves(EnvironmentBeingMadeInScan& environment)
{
    if (environment.blockOfLeaving == noDefinition) {
        environment.blockOfLeaving = m_blockBeingRead;
        environment.positionOfLeaving = m_position;
        return;
    }
    if (m_isAboveBlockBeingRead[environment.blockOfLeaving])
        return;
    uint32_t above = environment.blockOfLeaving;
    do
        above = m_blocksAbove[above];
    while (above != noDefinition && !m_isAboveBlockBeingRead[above]);
    environment.isGivenUp |= above == noDefinition;
    environment.blockOfLeaving = above == noDefinition ? m_blockBeingRead : above;
    environment.positionOfLeaving = UINT32_MAX;
}

inline void ScanOfBody::noteValueStandsOverEnvironment(uint32_t value, uint32_t environment)
{
    m_hasToDoWithEnvironment.quickSet(value);
    m_valuesOverEnvironments.append(ValueOverEnvironmentInScan { value, environment });
}

inline void ScanOfBody::noteLeavingOverEnvironments(uint32_t root)
{
    if (!m_hasToDoWithEnvironment.quickGet(root))
        return;
    if (EnvironmentBeingMadeInScan* environment = environmentBeingMade(root)) {
        if (m_closureBeingMade != noDefinition)
            noteValueStandsOverEnvironment(m_closureBeingMade, root);
        else
            noteEnvironmentLeaves(*environment);
        return;
    }
    auto [first, end] = std::ranges::equal_range(m_valuesOverEnvironments, root, { }, &ValueOverEnvironmentInScan::value);
    size_t endOfValue = end - m_valuesOverEnvironments.begin();
    for (size_t index = first - m_valuesOverEnvironments.begin(); index < endOfValue; ++index) {
        uint32_t over = m_valuesOverEnvironments[index].environment;
        if (m_closureBeingMade != noDefinition) {
            noteValueStandsOverEnvironment(m_closureBeingMade, over);
            continue;
        }
        if (m_environmentStoredInto == over && !m_slotsOfEnvironments[m_slotStoredInto].isRead) {
            m_slotsOfEnvironments[m_slotStoredInto].holdsClosureOverEnvironment = true;
            continue;
        }
        noteEnvironmentLeaves(*environmentBeingMade(over));
    }
}

inline void ScanOfBody::beginOperationOverEnvironments(const BodyValue& value)
{
    Opcode opcode = value.opcode();
    uint32_t index = value.id().index();
    m_closureBeingMade = opcode == Opcode::NewClosure ? index : noDefinition;
    m_environmentStoredInto = noDefinition;

    auto slotNamedBy = [&](ValueID holder, LayoutID layout, uint32_t field) -> std::optional<uint32_t> {
        EnvironmentBeingMadeInScan* environment = holder ? environmentBeingMade(m_roots[holder.index()]) : nullptr;
        if (!environment)
            return std::nullopt;
        if (environment->layout != layout || field >= environment->numberOfSlots) {
            environment->isGivenUp = true;
            return std::nullopt;
        }
        return environment->firstSlot + field;
    };

    if (opcode == Opcode::PutEnvironmentField) {
        auto store = value.as<Opcode::PutEnvironmentField>();
        if (std::optional<uint32_t> slot = slotNamedBy(store.environmentOperand(), store.layout(), store.field())) {
            m_environmentStoredInto = m_roots[store.environmentOperand().index()];
            m_slotStoredInto = *slot;
        }
        return;
    }
    if (opcode == Opcode::GetEnvironmentField) {
        auto read = value.as<Opcode::GetEnvironmentField>();
        std::optional<uint32_t> place = slotNamedBy(read.environmentOperand(), read.layout(), read.field());
        if (!place)
            return;
        SlotOfEnvironmentInScan& slot = m_slotsOfEnvironments[*place];
        slot.isRead = true;
        slot.isSeenAsAtFirst |= slot.firstStore == noDefinition || !m_isAboveBlockBeingRead[m_blocks[slot.firstStore]];
        if (slot.holdsClosureOverEnvironment)
            noteValueStandsOverEnvironment(index, m_roots[read.environmentOperand().index()]);
        return;
    }
    if (opcode == Opcode::GetCapture) {
        if (ValueID closure = value.as<Opcode::GetCapture>().closureOperand())
            noteLeavingOverEnvironments(m_roots[closure.index()]);
    }
}

inline void ScanOfBody::noteStoreIntoEnvironment()
{
    if (m_environmentStoredInto == noDefinition)
        return;
    SlotOfEnvironmentInScan& slot = m_slotsOfEnvironments[m_slotStoredInto];
    if (slot.firstStore == noDefinition)
        slot.firstStore = m_position;
}

inline void ScanOfBody::endOperationsOverEnvironments()
{
    m_closureBeingMade = noDefinition;
    m_environmentStoredInto = noDefinition;
}

inline void ScanOfBody::letFirstValuesOfSlotsArrive()
{
    uint32_t number = 0;
    for (const EnvironmentBeingMadeInScan& environment : m_environmentsBeingMade) {
        ++number;
        bool hasLeft = environment.blockOfLeaving != noDefinition;
        bool isGivenUp = m_hasGivenUpEnvironments || environment.isGivenUp;
        if (hasLeft && !isGivenUp) {
            uint32_t blockOfAllocation = m_blocks[environment.root];
            uint32_t above = environment.blockOfLeaving;
            for (; above != noDefinition && above != blockOfAllocation; above = m_blocksAbove[above])
                m_environmentsWhoseLeavingLiesBelow[above] = number;
            isGivenUp = above == noDefinition;
            m_environmentsWhoseLeavingLiesBelow[blockOfAllocation] = number;
        }
        auto standsAboveLeaving = [&](uint32_t store) {
            uint32_t block = m_blocks[store];
            return m_environmentsWhoseLeavingLiesBelow[block] == number && (block != environment.blockOfLeaving || store < environment.positionOfLeaving);
        };
        SummaryScopeStoredFirst storedFirst { ValueID(environment.root), environment.blockOfLeaving, environment.positionOfLeaving, 0, { } };
        for (uint32_t field = 0; field < environment.numberOfSlots; ++field) {
            const SlotOfEnvironmentInScan& slot = m_slotsOfEnvironments[environment.firstSlot + field];
            if (slot.firstValue == FirstValueOfSlotInScan::IsListed)
                continue;
            bool goesUnseen = !isGivenUp && !slot.isSeenAsAtFirst && (!hasLeft || (slot.firstStore != noDefinition && standsAboveLeaving(slot.firstStore)));
            if (goesUnseen)
                storedFirst.fields.append(field);
            else
                addEdge(constant(slot.firstValue == FirstValueOfSlotInScan::IsUndefined ? TypeTable::undefined : typeOfEmptySlotOfEnvironment(environment.layout, field)), fieldNode(environment.layout, field));
        }
        if (!storedFirst.fields.isEmpty())
            m_summary.scopesStoredFirst.append(WTF::move(storedFirst));
    }

    Vector<SummaryScopeStoredFirst>& scopes = m_summary.scopesStoredFirst;
    if (scopes.size() < 2)
        return;
    Vector<uint32_t, 8> inOrderOfLeaving;
    for (uint32_t scope = 0; scope < scopes.size(); ++scope)
        inOrderOfLeaving.append(scope);
    std::ranges::sort(inOrderOfLeaving, { }, [&](uint32_t scope) {
        return std::tuple { scopes[scope].blockOfLeaving, scopes[scope].positionOfLeaving, scope };
    });
    for (uint32_t place = 0; place < scopes.size(); ++place)
        scopes[place].scopeAtThisPlaceInOrderOfLeaving = inOrderOfLeaving[place];
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
