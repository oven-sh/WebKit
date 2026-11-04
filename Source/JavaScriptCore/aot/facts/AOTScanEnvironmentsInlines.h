/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// `function make() { var count = 0; const limit = 10; return { run }; function run(n) { count++; return n > limit; } }`: WHAT A SLOT OF A SCOPE HOLDS BEFORE ITS FIRST STORE, AND WHO CAN SEE IT.
// Nothing tests whether a slot of an environment is filled, so what it holds at first is part of its type wherever somebody may read it then. A PROOF: an environment has no door and no mark.
// Nobody reads a slot of an environment but the body that made it, under the name that the allocation gave it, and the closures over it. A closure runs once it is called, and nobody calls what has not left the body but the body itself.
// So the first value goes unseen where a store into the slot stands above (dominates) every read of the slot by the body itself and every point at which the environment leaves:
// where it, a closure over it, or what was read out of a slot that holds such a closure, is used for more than to be captured by a closure or stored into a slot of the same environment.
// The environment of each turn of a loop is another object under the same name. A store that stands above a point stands between the allocation and the point on every way: the allocation stands above the store.
// Text that is evaluated names slots by Program::evalScopes, and Link lets each of those escape: nothing is proven of them, whatever arrives.
// By dominators and first stores alone, so that a body with thousands of variables and thousands of blocks costs what it has of both, and not their product.

#pragma once

#if ENABLE(AOT)

// For the foot of AOTScan.cpp alone, which declares ScanOfBody.

namespace JSC { namespace AOT { namespace Typed {

// A debugger stands at any operation and calls whatever it finds in the scope.
inline bool ScanOfBody::followsEnvironmentsOfBody() const
{
    return !m_program.admitsInspector;
}

// E1. A read is a bare load. Where the slot is written to have a dead zone, whoever reads it empty gets the mark, and the check behind the read throws: `undefined` comes out of it only where it was stored.
inline TypeID ScanOfBody::typeOfEmptySlotOfEnvironment(LayoutID id, uint32_t field) const
{
    TypeID written = m_program.at(id).fields[field].declared.written();
    if (written && (m_program.at(written).primitives & Primitive::Absent))
        return TypeTable::absent;
    return typeOfPrimitives(Primitive::Undefined | Primitive::Absent);
}

// E3. `var count = 0`: the allocation lists undefined, as the language has it, and the store stands behind it.
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
        // Whatever leads here is of no matter: nothing in front of the first allocation names an environment that is followed.
        m_blocksAboveBlockBeingRead.append(m_blockBeingRead);
        m_isAboveBlockBeingRead[m_blockBeingRead] = true;
    }
    uint32_t root = value.id().index();
    m_hasToDoWithEnvironment.quickSet(root);
    m_environmentsBeingMade.append(EnvironmentBeingMadeInScan { .root = root, .layout = id, .firstSlot = firstSlot, .numberOfSlots = numberOfSlots });
}

// ---- Which blocks stand above which, from the first allocation on. noDefinition: none that is known.
// Blocks stand in reverse postorder, so one pass over the edges that lead forward finds it. An edge that was read before the first allocation is known by its count alone: nothing is said to stand above its target.

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

// In front of carryFillsOver(), which counts the edge.
inline void ScanOfBody::noteEdgeForEnvironments(uint32_t from, uint32_t to, KindOfEdgeInScan kind)
{
    if (to <= from) {
        // It takes nothing from what stands above its head where the head stands above its source. JavaScript writes no other loop.
        m_hasGivenUpEnvironments |= !m_isAboveBlockBeingRead[to];
        return;
    }
    // Whatever throws may do so in front of everything else in its block.
    uint32_t source = kind == KindOfEdgeInScan::ToHandler ? m_blocksAbove[from] : from;
    m_blocksAbove[to] = m_numberOfEdgesInto[to] ? nearestBlockAboveBothForEnvironments(m_blocksAbove[to], source) : source;
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

// ---- What the body does with them

inline void ScanOfBody::noteEnvironmentLeaves(EnvironmentBeingMadeInScan& environment)
{
    if (environment.blockOfLeaving == noDefinition) {
        environment.blockOfLeaving = m_blockBeingRead;
        environment.positionOfLeaving = m_position;
        return;
    }
    // The point that is kept stands above this one. In one block the earlier one is kept.
    if (m_isAboveBlockBeingRead[environment.blockOfLeaving])
        return;
    // It only ever goes up, so all the steps of an environment together are no more than the blocks between its allocation and where it first left.
    uint32_t above = environment.blockOfLeaving;
    do
        above = m_blocksAbove[above];
    while (above != noDefinition && !m_isAboveBlockBeingRead[above]);
    environment.isGivenUp |= above == noDefinition;
    environment.blockOfLeaving = above == noDefinition ? m_blockBeingRead : above;
    // Both points come behind the whole of it.
    environment.positionOfLeaving = UINT32_MAX;
}

inline void ScanOfBody::noteValueStandsOverEnvironment(uint32_t value, uint32_t environment)
{
    m_hasToDoWithEnvironment.quickSet(value);
    m_valuesOverEnvironments.append(ValueOverEnvironmentInScan { value, environment });
}

// From noteLeaving(): the value is used for more than to be looked at.
inline void ScanOfBody::noteLeavingOverEnvironments(uint32_t root)
{
    if (!m_hasToDoWithEnvironment.quickGet(root))
        return;
    if (EnvironmentBeingMadeInScan* environment = environmentBeingMade(root)) {
        if (m_closureBeingMade != noDefinition)
            noteValueStandsOverEnvironment(m_closureBeingMade, root);
        else {
            noteEnvironmentLeaves(*environment);
            // A function that the body enters itself gets the scope as what it captures.
            noteEntryOverEnvironment(root, m_functionCalledByBinding ? m_functionCalledByBinding.index() : SummaryOrderOfScope::anyone);
        }
        return;
    }
    auto [first, end] = std::ranges::equal_range(m_valuesOverEnvironments, root, { }, &ValueOverEnvironmentInScan::value);
    size_t endOfValue = end - m_valuesOverEnvironments.begin();
    for (size_t index = first - m_valuesOverEnvironments.begin(); index < endOfValue; ++index) {
        uint32_t over = m_valuesOverEnvironments[index].environment;
        // `const f = () => count; const g = () => f()`: whoever has g has f.
        if (m_closureBeingMade != noDefinition) {
            noteValueStandsOverEnvironment(m_closureBeingMade, over);
            noteHoldOverEnvironment(over, thingOverEnvironment(m_closureBeingMade), thingOverEnvironment(root));
            continue;
        }
        // Whoever finds it there has the environment already. A read that stands in front of the store comes behind it in the next turn of a loop, and nothing was noted of what it gave.
        if (m_environmentStoredInto == over && !m_slotsOfEnvironments[m_slotStoredInto].isRead) {
            m_slotsOfEnvironments[m_slotStoredInto].holdsClosureOverEnvironment = true;
            noteHoldOverEnvironment(over, SummaryOrderOfScope::slotBit | (m_slotStoredInto - environmentBeingMade(over)->firstSlot), thingOverEnvironment(root));
            continue;
        }
        noteEnvironmentLeaves(*environmentBeingMade(over));
        noteEntryOverEnvironment(over, m_environmentComesOutUnderAnotherName ? SummaryOrderOfScope::anyone : thingOverEnvironment(root));
    }
}

// In front of the operands.
inline void ScanOfBody::beginOperationOverEnvironments(const BodyValue& value)
{
    Opcode opcode = value.opcode();
    uint32_t index = value.id().index();
    m_closureBeingMade = opcode == Opcode::NewClosure ? index : noDefinition;
    m_environmentStoredInto = noDefinition;
    m_functionCalledByBinding = opcode == Opcode::CallDirect ? value.as<Opcode::CallDirect>().function() : FunctionID();
    m_environmentComesOutUnderAnotherName = opcode == Opcode::GetCapture;

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
    // The environment comes out under another name.
    if (opcode == Opcode::GetCapture) {
        if (ValueID closure = value.as<Opcode::GetCapture>().closureOperand())
            noteLeavingOverEnvironments(m_roots[closure.index()]);
    }
}

// Behind the operands: what is stored may let another environment leave, and does so in front of the store.
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
    m_functionCalledByBinding = { };
    m_environmentComesOutUnderAnotherName = false;
}

// From finish(), in front of removeRepeatedEdges().
inline void ScanOfBody::letFirstValuesOfSlotsArrive()
{
    uint32_t number = 0;
    for (const EnvironmentBeingMadeInScan& environment : m_environmentsBeingMade) {
        ++number;
        bool hasLeft = environment.blockOfLeaving != noDefinition;
        bool isGivenUp = m_hasGivenUpEnvironments || environment.isGivenUp;
        if (hasLeft && !isGivenUp) {
            // Every store into it stands at or below its allocation.
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
        for (uint32_t field = 0; field < environment.numberOfSlots; ++field) {
            const SlotOfEnvironmentInScan& slot = m_slotsOfEnvironments[environment.firstSlot + field];
            if (slot.firstValue == FirstValueOfSlotInScan::IsListed)
                continue;
            bool goesUnseen = !isGivenUp && !slot.isSeenAsAtFirst && (!hasLeft || (slot.firstStore != noDefinition && standsAboveLeaving(slot.firstStore)));
            // Also a slot into which the body never stores: the head of `for (const x of probe(() => x))` makes a scope of the layout of the turns, and Link has to hear of every place that makes one.
            if (slot.firstValue == FirstValueOfSlotInScan::IsEmpty && typeOfEmptySlotOfEnvironment(environment.layout, field) == TypeTable::absent)
                m_slotsLeftOpen.append(SlotLeftOpenInScan { environment.root, field, goesUnseen, isGivenUp || (!goesUnseen && slot.firstStore == noDefinition) });
            if (!goesUnseen)
                addEdge(constant(slot.firstValue == FirstValueOfSlotInScan::IsUndefined ? TypeTable::undefined : typeOfEmptySlotOfEnvironment(environment.layout, field)), fieldNode(environment.layout, field));
        }
    }
}

// ---- From where each closure over a scope may be entered: SummaryOrderOfScope

inline uint32_t ScanOfBody::thingOverEnvironment(uint32_t root) const
{
    BodyValue definition = definitionOf(root);
    if (definition && definition.opcode() == Opcode::NewClosure && definition.as<Opcode::NewClosure>().function())
        return definition.as<Opcode::NewClosure>().function().index();
    if (definition && definition.opcode() == Opcode::GetEnvironmentField)
        return SummaryOrderOfScope::slotBit | definition.as<Opcode::GetEnvironmentField>().field();
    return SummaryOrderOfScope::anyone;
}

inline void ScanOfBody::noteEntryOverEnvironment(uint32_t environment, uint32_t thing)
{
    m_entriesOverEnvironments.append(EntryOverEnvironmentInScan { environment, thing, m_blockBeingRead, m_position });
}

inline void ScanOfBody::noteHoldOverEnvironment(uint32_t environment, uint32_t holder, uint32_t held)
{
    m_holdsOverEnvironments.append(HoldOverEnvironmentInScan { environment, holder, held });
}

// From finish(), behind letFirstValuesOfSlotsArrive().
inline void ScanOfBody::listOrdersOfScopes()
{
    if (m_slotsLeftOpen.isEmpty())
        return;
    // The order of dominators. A block stands behind the block right above it, so one pass upward counts what stands below each block and one pass downward hands out the numbers.
    // A block above which nothing is known begins a row of its own: nothing outside of the row stands above it.
    bool someSlotOfBodyIsOpen = std::ranges::any_of(m_slotsLeftOpen, [](const SlotLeftOpenInScan& slot) { return !slot.goesUnseen && !slot.isGivenUp; });
    uint32_t numberOfBlocks = someSlotOfBodyIsOpen ? static_cast<uint32_t>(m_blocksAbove.size()) : 0;
    Vector<uint32_t> numberBelow;
    numberBelow.fill(1, numberOfBlocks);
    for (uint32_t block = numberOfBlocks; block--;) {
        if (m_blocksAbove[block] != noDefinition)
            numberBelow[m_blocksAbove[block]] += numberBelow[block];
    }
    Vector<uint32_t> numberOf;
    numberOf.fill(0, numberOfBlocks);
    Vector<uint32_t> nextBelow;
    nextBelow.fill(0, numberOfBlocks);
    uint32_t nextRow = 0;
    for (uint32_t block = 0; block < numberOfBlocks; ++block) {
        uint32_t& next = m_blocksAbove[block] != noDefinition ? nextBelow[m_blocksAbove[block]] : nextRow;
        numberOf[block] = next;
        next += numberBelow[block];
        nextBelow[block] = numberOf[block] + 1;
    }

    // In the order of the environments, which is that of m_slotsLeftOpen.
    for (size_t place = 0; place < m_slotsLeftOpen.size();) {
        uint32_t root = m_slotsLeftOpen[place].environment;
        const EnvironmentBeingMadeInScan& environment = *environmentBeingMade(root);
        SummaryOrderOfScope order;
        order.layout = environment.layout;
        bool someSlotIsOpen = false;
        for (; place < m_slotsLeftOpen.size() && m_slotsLeftOpen[place].environment == root; ++place) {
            uint32_t field = m_slotsLeftOpen[place].field;
            if (m_slotsLeftOpen[place].isGivenUp) {
                order.isGivenUp = true;
                continue;
            }
            if (m_slotsLeftOpen[place].goesUnseen) {
                order.stores.append(SummaryOrderOfScope::Store { field, 0, SummaryOrderOfScope::aboveAll, 0 });
                continue;
            }
            someSlotIsOpen = true;
            uint32_t store = m_slotsOfEnvironments[environment.firstSlot + field].firstStore;
            uint32_t block = m_blocks[store];
            order.stores.append(SummaryOrderOfScope::Store { field, numberOf[block], numberOf[block] + numberBelow[block] - 1, store });
        }
        for (const EntryOverEnvironmentInScan& entry : m_entriesOverEnvironments) {
            if (someSlotIsOpen && entry.environment == root)
                order.entries.append(SummaryOrderOfScope::Entry { entry.thing, numberOf[entry.block], entry.position });
        }
        for (const HoldOverEnvironmentInScan& hold : m_holdsOverEnvironments) {
            if (someSlotIsOpen && hold.environment == root)
                order.holds.append(SummaryOrderOfScope::Hold { hold.holder, hold.held });
        }
        m_summary.ordersOfScopes.append(WTF::move(order));
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
