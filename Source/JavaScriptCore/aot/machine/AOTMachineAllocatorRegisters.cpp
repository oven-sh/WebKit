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

enum class RegisterFileVictim : uint8_t {
    IsMemberOfTuple,
    NeedsStore,
    LiesInMemory,
    IsMadeAgain,
    IsHeldTwice,
    IsDead,
};

struct RegisterFileSearch {
    Reg victim;
    RegisterFileVictim classOfVictim { RegisterFileVictim::IsMemberOfTuple };
    RegisterMask dead { 0 };
};

static ALWAYS_INLINE RegisterFileVictim classifyRegisterFileVictim(const Allocator& allocator, Reg reg, Position nextUse)
{
    const Operation* value = allocator.m_valueIn[reg.index()];
    if (nextUse == noPosition)
        return RegisterFileVictim::IsDead;
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

static bool memoryCopyOfRegisterFileVictimIsValid(const Allocator& allocator, const Operation* value)
{
    if (allocator.spillHoldsValueIn(allocator.m_block, value))
        return true;
    SpillIndex save = allocator.m_values[value->index()].saveInRareRegion;
    ASSERT(save != noSpill);
    return allocator.m_allocation.spills()[save].begin >= allocator.m_order.headOf(allocator.m_block);
}

static bool registerFileHoldsOnlyCopy(const Allocator& allocator, Reg reg)
{
    const Operation* value = allocator.m_valueIn[reg.index()];
    RegisterFileVictim classOfValue = classifyRegisterFileVictim(allocator, reg, allocator.nextUseOf(value));
    if (classOfValue == RegisterFileVictim::LiesInMemory)
        return !memoryCopyOfRegisterFileVictimIsValid(allocator, value);
    return classOfValue <= RegisterFileVictim::NeedsStore;
}

static ALWAYS_INLINE bool canDropFromRegisterFileForFree(const Allocator& allocator, Reg reg)
{
    static_assert(RegisterFileVictim::IsMemberOfTuple < RegisterFileVictim::IsMadeAgain && RegisterFileVictim::NeedsStore < RegisterFileVictim::IsMadeAgain && RegisterFileVictim::LiesInMemory < RegisterFileVictim::IsMadeAgain);
    static_assert(RegisterFileVictim::IsHeldTwice > RegisterFileVictim::IsMadeAgain && RegisterFileVictim::IsDead > RegisterFileVictim::IsMadeAgain);
    const Operation* value = allocator.m_valueIn[reg.index()];
    if (allocator.nextUseOf(value) == noPosition)
        return true;
    if (!value->type().isNumeric())
        return false;
    if (allocator.m_values[value->index()].registers & ~maskOf(reg))
        return true;
    return allocator.m_valueFacts[value->index()].has(ValueFlag::IsRematerialized);
}

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

static bool someChildOfRematerializedValueNamesFrame(const Operation*);

static ALWAYS_INLINE bool rematerializedValueNamesFrame(const Operation* value)
{
    if (value->opcode() == FramePointer || value->opcode() == SlotBase)
        return true;
    return value->numChildren() && someChildOfRematerializedValueNamesFrame(value);
}

static NEVER_INLINE bool someChildOfRematerializedValueNamesFrame(const Operation* value)
{
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
    if (m_numbersOfListsOfReads.isEmpty())
        m_numbersOfListsOfReads.fill(m_arena, m_order.mostOperations(), 0);
    uint32_t& numberOfList = m_numbersOfListsOfReads[value->index()];
    if (!numberOfList) {
        size_t numberOfReads = 0;
        for (uint32_t read = m_liveness.of(value).firstUse; read; read = m_liveness.use(read).next)
            ++numberOfReads;
        std::span<uint32_t> reads = m_arena.createSpan<uint32_t>(numberOfReads);
        size_t next = 0;
        for (uint32_t read = m_liveness.of(value).firstUse; read; read = m_liveness.use(read).next)
            reads[next++] = read;
        m_listsOfReads.append(m_arena, reads);
        numberOfList = m_listsOfReads.size();
    }
    std::span<const uint32_t> list = m_listsOfReads[numberOfList - 1];
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
        m_proc.fail(FailureKind::WrongValueInRegister, previous);
    unbind(reg);
    bind(reg, value);
}

Reg Allocator::takeRegisterAfterSearch(Bank bank, RegisterMask allowed, Reg preferred)
{
    RegisterMask candidates = registersThatMayBeTaken(bank, allowed);
    if (!candidates) [[unlikely]] {
        candidates = allowed & m_file.allocatableIn(bank);
        if (!candidates) {
            m_proc.fail(FailureKind::OutOfRegisters, currentOperationForRegisterFileFailure(*this));
            candidates = m_file.allocatableIn(bank);
        }
    }

    RegisterMask freeRegisters = candidates & ~m_bound;
    Reg chosen = chooseFreeRegisterWithoutNewCalleeSave(freeRegisters, preferred);
    if (!chosen) {
        RegisterFileSearch search = searchRegisterFileForVictim(*this, candidates & m_bound);
        for (RegisterMask dead = search.dead; dead; dead &= dead - 1)
            unbind(lowestRegisterIn(dead));
        chosen = chooseFreeRegisterWithoutNewCalleeSave(freeRegisters | search.dead, preferred);

        if (!chosen) {
            if (freeRegisters && (!search.victim || search.classOfVictim < RegisterFileVictim::IsMadeAgain)) {
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
            m_proc.fail(FailureKind::WrongValueInRegister, value);
    }

    bind(destination, value);
    return destination;
}

SpillIndex Allocator::markSpilled(Operation* value)
{
    if (m_isOutsideFramedPart && isStoredAtHeadsOfFramedPart(value)) {
        const ValueFacts& factsOfValue = m_valueFacts[value->index()];
        if (isInRareRegion() || hasImplicitReader(factsOfValue)) {
            m_allocation.m_needsFrameAheadOfFramedPart = true;
            m_allocation.m_needsFrameOutsideRareRegions = true;
        }
    }
    SpillIndex spill = m_allocation.m_spillOfValue[value->index()];
    if (spill != noSpill)
        return spill;

    const ValueFacts& facts = m_valueFacts[value->index()];
    if (value->opcode() == Phi && facts.definition != noPosition) {
        if (SpillIndex shared = findSpillOfDeadSource(value); shared != noSpill) {
            SpillFacts& range = m_allocation.m_spills[shared];
            range.isSharedByPhis = true;
            range.end = facts.lastUse == noPosition ? facts.definition : std::max(facts.definition, facts.lastUse);
            phisInSpill(shared).append(m_arena, value);
            m_allocation.m_spillOfValue[value->index()] = shared;
            stateOf(value).saveInRareRegion = noSpill;
            return shared;
        }
    }
    SpillFacts spillFacts;
    spillFacts.value = value;

    if (value->hasNoPosition() || facts.definition == noPosition || !value->type().isNumeric()) [[unlikely]] {
        m_proc.fail(FailureKind::WrongValueInSpillSlot, value);
        spillFacts.begin = m_position;
        spillFacts.end = m_position;
    } else {
        spillFacts.width = widthOf(value->type());
        spillFacts.begin = facts.definition;
        spillFacts.end = facts.lastUse == noPosition ? facts.definition : std::max(facts.definition, facts.lastUse);

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
