/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineAllocator.h"

#if ENABLE(AOT)

#include <algorithm>
#include <bit>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

// Ordered from the most expensive to evict to the cheapest.
enum class RegisterFileVictim : uint8_t {
    // One of the registers that a Patch with several results has written. Nothing can store it before its Extract is reached.
    IsMemberOfTuple,
    NeedsStore,
    LiesInMemory,
    IsMadeAgain,
    IsHeldTwice,
    IsDead,
};

struct RegisterFileSearch {
    // The cheapest to evict among those that are not dead.
    Reg victim;
    RegisterFileVictim classOfVictim { RegisterFileVictim::IsMemberOfTuple };
    RegisterMask dead { 0 };
};

static ALWAYS_INLINE RegisterFileVictim classifyRegisterFileVictim(const Allocator& allocator, Reg reg, Position nextUse)
{
    const Operation* value = allocator.m_valueIn[reg.index()];
    if (nextUse == noPosition)
        return RegisterFileVictim::IsDead;
    // Its registers hold different members, not copies.
    if (!value->type().isNumeric())
        return RegisterFileVictim::IsMemberOfTuple;

    const Allocator::ValueState& state = allocator.m_values[value->index()];
    if (state.registers & ~maskOf(reg))
        return RegisterFileVictim::IsHeldTwice;
    if (allocator.m_valueFacts[value->index()].has(ValueFlag::IsRematerialized))
        return RegisterFileVictim::IsMadeAgain;
    if (allocator.spillHoldsValueIn(allocator.m_block, value) || (allocator.isInRareRegion() && state.saveInRareRegion != noSpill))
        return RegisterFileVictim::LiesInMemory;
    return RegisterFileVictim::NeedsStore;
}

// Of a victim that lies in memory. Only a Save in this block is known to have run: a path may reach the block without the Save that another block of the region has made.
static bool memoryCopyOfRegisterFileVictimIsValid(const Allocator& allocator, const Operation* value)
{
    if (allocator.spillHoldsValueIn(allocator.m_block, value))
        return true;
    SpillIndex save = allocator.m_values[value->index()].saveInRareRegion;
    ASSERT(save != noSpill);
    return allocator.m_allocation.spills()[save].begin >= allocator.m_order.headOf(allocator.m_block);
}

// Whoever takes the register without evict() loses the value.
static bool registerFileHoldsOnlyCopy(const Allocator& allocator, Reg reg)
{
    const Operation* value = allocator.m_valueIn[reg.index()];
    RegisterFileVictim classOfValue = classifyRegisterFileVictim(allocator, reg, allocator.nextUseOf(value));
    if (classOfValue == RegisterFileVictim::LiesInMemory)
        return !memoryCopyOfRegisterFileVictimIsValid(allocator, value);
    return classOfValue <= RegisterFileVictim::NeedsStore;
}

// Neither a store now nor a load later.
static ALWAYS_INLINE bool canDropFromRegisterFileForFree(const Allocator& allocator, Reg reg)
{
    return classifyRegisterFileVictim(allocator, reg, allocator.nextUseOf(allocator.m_valueIn[reg.index()])) >= RegisterFileVictim::IsMadeAgain;
}

// Every candidate is bound.
static RegisterFileSearch searchRegisterFileForVictim(const Allocator& allocator, RegisterMask candidates)
{
    RegisterFileSearch search;
    Position nextUseOfVictim = 0;
    for (; candidates; candidates &= candidates - 1) {
        Reg reg = Allocator::lowestRegisterIn(candidates);
        Position nextUse = allocator.nextUseOf(allocator.m_valueIn[reg.index()]);
        RegisterFileVictim classOfCandidate = classifyRegisterFileVictim(allocator, reg, nextUse);
        if (classOfCandidate == RegisterFileVictim::IsDead) {
            search.dead |= maskOf(reg);
            continue;
        }
        if (search.victim && (classOfCandidate < search.classOfVictim || (classOfCandidate == search.classOfVictim && nextUse <= nextUseOfVictim)))
            continue;
        search.victim = reg;
        search.classOfVictim = classOfCandidate;
        nextUseOfVictim = nextUse;
    }
    return search;
}

static const Operation* currentOperationForRegisterFileFailure(const Allocator& allocator)
{
    const Block* block = allocator.m_block;
    if (!block)
        return nullptr;
    Position first = allocator.m_order.earlyPositionOf(block, 0);
    if (allocator.m_position < first)
        return nullptr;
    unsigned index = (allocator.m_position - first) / 2;
    return index < block->size() ? block->at(index) : nullptr;
}

static bool rematerializedValueNamesFrame(const Operation* value)
{
    if (value->opcode() == FramePointer || value->opcode() == SlotBase)
        return true;
    for (const Operation* child : value->children()) {
        if (rematerializedValueNamesFrame(child))
            return true;
    }
    return false;
}

static ALWAYS_INLINE void appendRegisterFileTransfer(Allocator& allocator, StepKind kind, Operation* value, Location destination, Location source)
{
    Step step;
    step.kind = kind;
    step.width = widthOf(value->type());
    step.destination = destination;
    step.sources[0] = source;
    step.operation = value;
    allocator.appendStep(step);
}

#if ASSERT_ENABLED
bool Allocator::registerFileIsConsistent() const
{
    if (m_bound & ~(m_file.allocatableIn(Bank::GP) | m_file.allocatableIn(Bank::FP)))
        return false;
    for (unsigned index = 0; index <= Reg::maxIndex(); ++index) {
        RegisterMask bit = RegisterMask { 1 } << index;
        const Operation* value = m_valueIn[index];
        if (!value != !(m_bound & bit))
            return false;
        if (!value)
            continue;
        RegisterMask registers = m_values[value->index()].registers;
        if (!(registers & bit))
            return false;
        for (; registers; registers &= registers - 1) {
            if (m_valueIn[std::countr_zero(registers)] != value)
                return false;
        }
    }
    return true;
}
#endif

uint32_t Allocator::findReadFromInList(const Operation* value, Position position) const
{
    if (m_listsOfReads.isEmpty())
        m_listsOfReads.fill(m_arena, m_order.mostOperations(), std::span<const uint32_t> { });
    std::span<const uint32_t>& list = m_listsOfReads[value->index()];
    if (list.empty()) {
        size_t numberOfReads = 0;
        for (uint32_t read = m_liveness.of(value).firstUse; read; read = m_liveness.use(read).next)
            ++numberOfReads;
        std::span<uint32_t> reads = m_arena.createSpan<uint32_t>(numberOfReads);
        size_t next = 0;
        for (uint32_t read = m_liveness.of(value).firstUse; read; read = m_liveness.use(read).next)
            reads[next++] = read;
        list = reads;
    }
    auto found = std::ranges::lower_bound(list, position, { }, [&](uint32_t read) {
        return m_liveness.use(read).position;
    });
    return found == list.end() ? 0 : *found;
}

void Allocator::bindRegisterThatIsBound(Reg reg, Operation* value)
{
    Operation* previous = m_valueIn[reg.index()];
    ASSERT(previous);
    if (previous == value)
        return;
    if (isLiveBehind(previous, m_position) && registerFileHoldsOnlyCopy(*this, reg))
        m_proc.fail(FailureKind::RegisterLacksItsValue, previous);
    unbind(reg);
    bind(reg, value);
}

Reg Allocator::takeRegisterAfterSearch(Bank bank, RegisterMask allowed, Reg preferred)
{
    RegisterMask candidates = registersThatMayBeTaken(bank, allowed);
    if (!candidates) [[unlikely]] {
        // The constraints of one operation in a rare region have locked everything that the region may take. The function then saves one register more.
        candidates = allowed & m_file.allocatableIn(bank);
        if (!candidates) {
            // The walk goes on until the failure is seen, and needs a register until then.
            m_proc.fail(FailureKind::OutOfRegisters, currentOperationForRegisterFileFailure(*this));
            candidates = m_file.allocatableIn(bank);
        }
    }

    RegisterMask freeRegisters = candidates & ~m_bound;
    Reg chosen = chooseFreeRegisterWithoutNewCalleeSave(freeRegisters, preferred);
    if (!chosen) {
        // passUse() leaves a value bound whose range was extended to the end of a loop, and it stays bound behind that end. One search frees all that have piled up.
        RegisterFileSearch search = searchRegisterFileForVictim(*this, candidates & m_bound);
        for (RegisterMask dead = search.dead; dead; dead &= dead - 1)
            unbind(lowestRegisterIn(dead));
        chosen = chooseFreeRegisterWithoutNewCalleeSave(freeRegisters | search.dead, preferred);

        if (!chosen) {
            // A callee-saved register that is taken for the first time costs a store, a load and perhaps the only frame of the function. Making a constant again costs less.
            if (freeRegisters && (!search.victim || search.classOfVictim < RegisterFileVictim::IsMadeAgain)) {
                // The saves of a function that saves its frame are a run that ends in x24, so the highest register adds the fewest pairs.
                if (isARM64() && m_file.calleeSavedClobberedAtSuspensions)
                    chosen = Reg::fromIndex(static_cast<unsigned>(63 - std::countl_zero(freeRegisters)));
                else
                    chosen = lowestRegisterIn(freeRegisters);
            } else {
                chosen = search.victim;
                evict(chosen);
            }
        }
    }

    ASSERT(chosen && !m_valueIn[chosen.index()]);
    // A scratch register is written and never bound.
    noteCalleeSaveUse(chosen);
    return chosen;
}

bool Allocator::canDropSomeRegisterForFree(RegisterMask candidates) const
{
    for (; candidates; candidates &= candidates - 1) {
        if (canDropFromRegisterFileForFree(*this, lowestRegisterIn(candidates)))
            return true;
    }
    return false;
}

Reg Allocator::chooseVictim(Bank bank, RegisterMask allowed) const
{
    RegisterFileSearch search = searchRegisterFileForVictim(*this, allowed & m_bound & m_file.allocatableIn(bank));
    return search.dead ? lowestRegisterIn(search.dead) : search.victim;
}

void Allocator::evict(Reg reg)
{
    Operation* value = m_valueIn[reg.index()];
    if (!value)
        return;

    switch (classifyRegisterFileVictim(*this, reg, nextUseOf(value))) {
    case RegisterFileVictim::IsMemberOfTuple:
        m_proc.fail(FailureKind::OutOfRegisters, value);
        break;
    case RegisterFileVictim::NeedsStore:
        if (isInRareRegion() && isDefinedOutsideRareRegion(value))
            saveInRareRegion(value, reg);
        else
            markSpilled(value);
        break;
    case RegisterFileVictim::LiesInMemory:
        if (memoryCopyOfRegisterFileVictimIsValid(*this, value))
            break;
        // A step behind the operation that ends the block belongs to no block.
        if (m_allocation.stepsOf(m_block).terminator != UINT32_MAX) {
            markSpilled(value);
            break;
        }
        appendRegisterFileTransfer(*this, StepKind::Save, value, Location::spill(stateOf(value).saveInRareRegion), Location::reg(reg));
        noteFrameUse();
        break;
    case RegisterFileVictim::IsMadeAgain:
    case RegisterFileVictim::IsHeldTwice:
    case RegisterFileVictim::IsDead:
        break;
    }
    unbind(reg);
}

Reg Allocator::bringIntoRegister(Operation* value, RegisterMask allowed, Reg preferred)
{
    ASSERT(!(allowed & ~m_file.allocatableIn(bankOf(value->type()))) && !(stateOf(value).registers & allowed));

    // It lies outside `allowed`, so taking a register leaves it alone.
    Reg source = anyRegisterOf(value, ~RegisterMask { 0 });
    Reg destination = takeRegister(bankOf(value->type()), allowed, preferred);

    if (source)
        appendRegisterFileTransfer(*this, StepKind::Move, value, Location::reg(destination), Location::reg(source));
    else if (m_valueFacts[value->index()].has(ValueFlag::IsRematerialized)) {
        appendRegisterFileTransfer(*this, StepKind::Materialize, value, Location::reg(destination), Location::immediate());
        if (rematerializedValueNamesFrame(value))
            noteFrameUse();
    } else {
        Location memory = memoryLocationOf(value);
        if (memory.isMemory()) {
            appendRegisterFileTransfer(*this, StepKind::Reload, value, Location::reg(destination), memory);
            noteFrameUse();
        } else
            m_proc.fail(FailureKind::RegisterLacksItsValue, value);
    }

    bind(destination, value);
    return destination;
}

SpillIndex Allocator::markSpilled(Operation* value)
{
    // A rare region that asks for it counts on the spill from here on, on ways that have passed no Save of the region too. So the value is stored behind its definition.
    if (m_isAheadOfFramedPart && isInRareRegion() && isStoredAtHeadsOfFramedPart(value)) {
        m_allocation.m_needsFrameAheadOfFramedPart = true;
        m_allocation.m_needsFrameOutsideRareRegions = true;
    }
    SpillIndex spill = m_allocation.m_spillOfValue[value->index()];
    if (spill != noSpill)
        return spill;

    const ValueFacts& facts = m_valueFacts[value->index()];
    SpillFacts spillFacts;
    spillFacts.value = value;

    if (value->hasNoPosition() || facts.definition == noPosition || !value->type().isNumeric()) [[unlikely]] {
        // Nothing would ever store it. The walk goes on until the failure is seen.
        m_proc.fail(FailureKind::SpillSlotLacksItsValue, value);
        spillFacts.begin = m_position;
        spillFacts.end = m_position;
    } else {
        spillFacts.width = widthOf(value->type());
        spillFacts.begin = facts.definition;
        spillFacts.end = facts.lastUse == noPosition ? facts.definition : std::max(facts.definition, facts.lastUse);

        // A Phi without a register is written by the moves of its incoming edges, at the ends of its predecessors.
        // Its place is taken from the first of them to the last: a spill that those moves read must not share it, since the moves are ordered by spill and not by place.
        if (value->opcode() == Phi && !m_allocation.m_registerAtDefinition[value->index()]) {
            for (const Block* predecessor : m_order.of(value->owner()).predecessors) {
                Position positionOfMoves = m_order.earlyPositionOf(predecessor, predecessor->size() - 1);
                spillFacts.begin = std::min(spillFacts.begin, positionOfMoves);
                spillFacts.end = std::max(spillFacts.end, positionOfMoves);
            }
        }
    }

    spill = addSpill(spillFacts);
    m_allocation.m_spillOfValue[value->index()] = spill;
    // The spill holds the value on every path through the function, a Save only behind the Save. memoryLocationOf() asks for the save first, and must answer with the spill from here on.
    stateOf(value).saveInRareRegion = noSpill;
    return spill;
}

SpillIndex Allocator::addSpill(const SpillFacts& facts)
{
    ASSERT(facts.value);
    ASSERT(facts.begin != noPosition && facts.begin <= facts.end);
    ASSERT(facts.width == Width32 || facts.width == Width64);
    SpillIndex index = m_allocation.m_spills.size();
    m_allocation.m_spills.append(m_arena, facts);
    return index;
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
