/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineChecker.h"

#if ENABLE(AOT)

#include "AOTSavedFrame.h"
#include <algorithm>
#include <bit>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

class AllocationChecker {
    WTF_MAKE_NONCOPYABLE(AllocationChecker);
public:
    explicit AllocationChecker(Procedure&);
    bool run();

private:
    using ValueID = uint32_t;
    using Place = uint32_t;

    static constexpr ValueID nothing = 0;
    static constexpr ValueID isPointerOfReference = 1u << 31;
    static constexpr Place numberOfRegisters = 64;

    struct ValueRecord {
        Position definition { 0 };
        Width width { Width64 };
        bool isFrameAddress { false };
    };
    struct Entry {
        Place place { 0 };
        ValueID value { nothing };
    };
    struct Head {
        std::span<Entry> entries;
        bool isSet { false };
        bool isDirty { false };
    };
    struct Interval {
        Position lower { 0 };
        Position upper { 0 };
    };
    struct WriteOutsideRange {
        SpillIndex spill { noSpill };
        Interval at;
    };

    bool fail(FailureKind kind, const Operation* operation)
    {
        m_proc.fail(kind, operation);
        m_failed = true;
        return false;
    }

    void numberValues();
    Position definitionOf(const Operation*) const;
    ValueID idOf(const Operation*) const;
    ValueID narrowed(ValueID, Width) const;

    static RegisterMask registerMaskOf(Location);
    static bool isOutgoingWord(Location, ValueRep::OffsetType offsetFromSP);
    static bool isHeldInPinnedRegister(Reg, const Operation*);
    static bool namesStackSlot(const Operation*, unsigned indexOfStackSlot);
    static bool mayLackStep(const Operation*);
    bool findPlace(Location, Place&) const;
    ValueID valueIn(Reg, const Operation* claimed) const;
    void set(Place, ValueID);
    void killRegisters(RegisterMask);
    void killSecondScratchGPR();
    void killOutgoingWords();
    bool forgetWhatSuspensionLoses(const PatchOperation*);
    void clearState();

    bool isInRange(const SpillFacts& facts) const { return facts.begin <= m_now.upper && m_now.lower <= facts.end && !(facts.isInGap(m_now.lower) && facts.isInGap(m_now.upper)); }
    bool isPhiOfOnlySuccessor(const Operation*) const;
    bool beginsBehindExtractsAlone(const SpillFacts&) const;
    bool noteSpillRead(SpillIndex, const Operation* reader);
    bool noteSpillWrite(SpillIndex, Width, const Operation* writer);

    void checkBlock(Block*);
    bool enter(Block*);
    bool runSteps(uint32_t begin, uint32_t end, bool isOnEdge);
    void findNextExecute(uint32_t begin, uint32_t end);
    bool matchNextOperation(const Operation*, Position& early);
    bool transfer(const Step&);
    bool execute(const Step&);
    bool executeOperation(const Step&);
    bool executePatch(const Step&, const PatchOperation*);
    bool checkSources(const Step&, RegisterMask& sources);
    bool checkLeaf(const Operation* reader, const Operation* user, unsigned childIndex, const Operation* leaf, Location, unsigned& numberOfMemoryOperands);
    bool checkOperand(const PatchOperation*, unsigned child, Location, RegisterMask deadBehind);
    bool checkHolds(Location, const Operation* value, const Operation* reader);
    unsigned numberOfFusedLoadsBelow(const Operation*) const;
    bool storeBehindDefinition(const Operation*);
    bool checkBranchOnOverflow(const Step&);
    void flowAlong(Block* from, unsigned successorIndex);

    Procedure& m_proc;
    Arena& m_arena;
    const Order& m_order;
    const Liveness& m_liveness;
    const Allocation& m_allocation;
    std::span<const SpillFacts> m_spills;
    const RegisterFile m_file;
    RegisterMask m_pinned;
    RegisterMask m_allocatable;
    RegisterMask m_scratchGPRs { 0 };
    RegisterMask m_scratch { 0 };
    Place m_firstSpillPlace { 0 };

    ValueID m_firstIncomingValue { 0 };
    ArenaVector<ValueID> m_firstMember;
    ArenaVector<ValueRecord> m_values;

    ArenaVector<Head> m_heads;
    ArenaVector<ValueID> m_state;
    ArenaVector<ValueID> m_onEdge;
    BitSpan m_isTouched;
    ArenaVector<Place> m_touched;
    ArenaVector<ValueID> m_incoming;

    Block* m_block { nullptr };
    Position m_head { 0 };
    Interval m_now;
    Interval m_behind;
    uint32_t m_nextSunk { 0 };
    uint32_t m_nextOperation { 0 };
    ArenaVector<WriteOutsideRange> m_writesOutsideRange;
    bool m_wroteSpillBehindLastExecute { false };
    const Step* m_stepThatLeftFlags { nullptr };
    bool m_needsAnotherRound { false };
    bool m_failed { false };
};

AllocationChecker::AllocationChecker(Procedure& proc)
    : m_proc(proc)
    , m_arena(proc.arena())
    , m_order(proc.order())
    , m_liveness(proc.liveness())
    , m_allocation(proc.allocation())
    , m_spills(proc.allocation().spills())
    , m_file(registerFileFor(proc.traits()))
    , m_pinned(pinnedRegisters())
    , m_allocatable(m_file.allocatableIn(Bank::GP) | m_file.allocatableIn(Bank::FP))
{
    m_scratchGPRs = maskOf(Reg(scratchGPR));
    if constexpr (secondScratchGPR != InvalidGPRReg)
        m_scratchGPRs |= maskOf(Reg(secondScratchGPR));
    m_scratch = m_scratchGPRs | maskOf(Reg(scratchFPR));

    m_firstSpillPlace = numberOfRegisters + static_cast<Place>((proc.callArgAreaSizeInBytes() + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    Place numberOfPlaces = m_firstSpillPlace + static_cast<Place>(m_spills.size());
    m_state.fill(m_arena, numberOfPlaces, nothing);
    m_onEdge.fill(m_arena, numberOfPlaces, nothing);
    m_isTouched = BitSpan::create(m_arena, numberOfPlaces);
    m_incoming.fill(m_arena, proc.numberOfOperations(), nothing);
    m_heads.grow(m_arena, proc.numberOfBlocks());
    numberValues();
}

Position AllocationChecker::definitionOf(const Operation* operation) const
{
    if (operation->hasNoPosition())
        return 0;
    if (m_liveness.isSunk(operation))
        return m_liveness.of(operation).definition;
    if (!m_order.of(operation->owner()).isReached())
        return noPosition;
    return operation->opcode() == Phi ? m_order.headOf(operation->owner()) : m_order.latePositionOf(operation);
}

void AllocationChecker::numberValues()
{
    uint32_t numberOfOperations = m_proc.numberOfOperations();
    m_firstIncomingValue = numberOfOperations + 1;
    ValueID numberOfValues = m_firstIncomingValue + numberOfRegisters;
    m_firstMember.fill(m_arena, numberOfOperations, nothing);
    for (uint32_t index = 0; index < numberOfOperations; ++index) {
        Type type = m_proc.operationAt(index)->type();
        if (!type.isTuple())
            continue;
        m_firstMember[index] = numberOfValues;
        numberOfValues += m_proc.resultCount(type);
    }

    m_values.grow(m_arena, numberOfValues);
    for (uint32_t index = 0; index < numberOfOperations; ++index) {
        const Operation* operation = m_proc.operationAt(index);
        Type type = operation->type();
        Position definition = definitionOf(operation);
        m_values[index + 1].definition = definition;
        const Operation* base = operation->opcode() == Add && operation->hasNoPosition() ? operation->child(0) : operation;
        m_values[index + 1].isFrameAddress = base->opcode() == FramePointer || base->opcode() == SlotBase;
        if (type.isNumeric())
            m_values[idOf(operation)].width = widthOf(type);
        if (!type.isTuple())
            continue;
        for (unsigned member = 0; member < m_proc.resultCount(type); ++member)
            m_values[m_firstMember[index] + member] = { definition, widthOf(m_proc.resultType(type, member)) };
    }
}

AllocationChecker::ValueID AllocationChecker::idOf(const Operation* value) const
{
    if (value->opcode() == Extract) {
        const Operation* tuple = value->child(0);
        ValueID first = m_firstMember[tuple->index()];
        if (!first || value->extractedIndex() >= m_proc.resultCount(tuple->type()))
            return nothing;
        return first + value->extractedIndex();
    }
    if (value->opcode() == ArgumentReg && !value->hasNoPosition())
        return m_firstIncomingValue + value->argumentReg().index();
    return value->index() + 1;
}

AllocationChecker::ValueID AllocationChecker::narrowed(ValueID value, Width width) const
{
    Width needed = value & isPointerOfReference ? Width64 : m_values[value].width;
    return bytesForWidth(width) >= bytesForWidth(needed) ? value : nothing;
}

RegisterMask AllocationChecker::registerMaskOf(Location location)
{
    if (!location.isRegister() || location.reg().index() >= numberOfRegisters)
        return 0;
    return maskOf(location.reg());
}

bool AllocationChecker::isOutgoingWord(Location location, ValueRep::OffsetType offsetFromSP)
{
    return location.kind() == Location::Outgoing && offsetFromSP >= 0 && location.offsetFromSP() == static_cast<uint32_t>(offsetFromSP);
}

bool AllocationChecker::isHeldInPinnedRegister(Reg reg, const Operation* value)
{
    if (value->opcode() == ArgumentReg)
        return value->argumentReg() == reg;
    if (!value->isConstant())
        return false;
    std::optional<PinnedConstant> pinned = findPinnedConstant(static_cast<uint64_t>(value->constantBits()));
    return pinned && !pinned->offset && Reg(pinned->reg) == reg;
}

bool AllocationChecker::namesStackSlot(const Operation* value, unsigned indexOfStackSlot)
{
    if (value->opcode() == Add && value->hasNoPosition())
        value = value->child(0);
    return value->opcode() == SlotBase && value->slot()->index() == indexOfStackSlot;
}

bool AllocationChecker::mayLackStep(const Operation* operation)
{
    return operation->opcode() == Nop || operation->opcode() == ArgumentReg || operation->opcode() == Extract;
}

bool AllocationChecker::findPlace(Location location, Place& place) const
{
    switch (location.kind()) {
    case Location::Register:
        place = location.reg().index();
        return place < numberOfRegisters;
    case Location::Spill:
        place = m_firstSpillPlace + location.spillIndex();
        return location.spillIndex() < m_spills.size();
    case Location::Outgoing:
        place = numberOfRegisters + static_cast<Place>(location.offsetFromSP() / sizeof(uint64_t));
        return !(location.offsetFromSP() % sizeof(uint64_t)) && place < m_firstSpillPlace;
    case Location::None:
    case Location::SlotBytes:
    case Location::InstanceWord:
    case Location::Immediate:
        return false;
    }
    return false;
}

AllocationChecker::ValueID AllocationChecker::valueIn(Reg reg, const Operation* claimed) const
{
    if (!(m_pinned & maskOf(reg)))
        return m_state[reg.index()];
    return claimed && isHeldInPinnedRegister(reg, claimed) ? idOf(claimed) : nothing;
}

void AllocationChecker::set(Place place, ValueID value)
{
    m_state[place] = value;
    if (!value || m_isTouched.get(place))
        return;
    m_isTouched.set(place);
    m_touched.append(m_arena, place);
}

void AllocationChecker::killRegisters(RegisterMask mask)
{
    forEachRegister(mask, [&](Reg reg) {
        m_state[reg.index()] = nothing;
    });
}

void AllocationChecker::killSecondScratchGPR()
{
    if constexpr (secondScratchGPR != InvalidGPRReg)
        m_state[Reg(secondScratchGPR).index()] = nothing;
}

void AllocationChecker::killOutgoingWords()
{
    for (Place place = numberOfRegisters; place < m_firstSpillPlace; ++place)
        m_state[place] = nothing;
}

bool AllocationChecker::forgetWhatSuspensionLoses(const PatchOperation* suspension)
{
    std::span<PatchOperation* const> suspensions = m_allocation.suspensions();
    auto listed = std::partition_point(suspensions.begin(), suspensions.end(), [&](const PatchOperation* candidate) {
        return m_order.latePositionOf(candidate) < m_order.latePositionOf(suspension);
    });
    if (listed == suspensions.end() || *listed != suspension)
        return fail(FailureKind::SpillSlotLacksItsValue, suspension);
    const BitSpan& spillsHeld = m_allocation.spillsHeldAt(static_cast<uint32_t>(listed - suspensions.begin()));

    RegisterMask kept = 0;
    if constexpr (SavedFrame::preservedRegister != InvalidGPRReg) {
        Reg preserved(SavedFrame::preservedRegister);
        if (!m_values[m_state[preserved.index()] & ~isPointerOfReference].isFrameAddress)
            kept = maskOf(preserved);
    }
    killRegisters(~kept);
    for (Place place : m_touched) {
        if (place >= m_firstSpillPlace && !spillsHeld.get(place - m_firstSpillPlace))
            m_state[place] = nothing;
    }
    return true;
}

void AllocationChecker::clearState()
{
    for (Place place : m_touched) {
        m_state[place] = nothing;
        m_isTouched.clear(place);
    }
    m_touched.clear();
}

bool AllocationChecker::isPhiOfOnlySuccessor(const Operation* value) const
{
    return value && value->opcode() == Phi && m_block->numSuccessors() == 1 && value->owner() == m_block->successor(0).block();
}

bool AllocationChecker::beginsBehindExtractsAlone(const SpillFacts& facts) const
{
    const Operation* extract = facts.value;
    if (!extract || extract->opcode() != Extract || extract->owner() != m_block || !m_liveness.standsInItsBlock(extract))
        return false;
    if (facts.begin != m_order.latePositionOf(extract) || extract->indexInBlock() < m_nextOperation || m_nextSunk < m_liveness.sunkInto(m_block).size())
        return false;
    for (uint32_t index = m_nextOperation; index < extract->indexInBlock(); ++index) {
        const Operation* between = m_block->at(index);
        if (m_liveness.standsInItsBlock(between) && between->opcode() != Extract)
            return false;
    }
    return true;
}

bool AllocationChecker::noteSpillRead(SpillIndex spill, const Operation* reader)
{
    const SpillFacts& facts = m_spills[spill];
    if (!isInRange(facts))
        return fail(FailureKind::SpillSlotLacksItsValue, reader);
    for (const WriteOutsideRange& written : m_writesOutsideRange) {
        const SpillFacts& other = m_spills[written.spill];
        bool mayShareItsPlace = other.liesApartFrom(facts);
        bool heldItsValueAtTheWrite = facts.begin <= written.at.upper && written.at.lower <= facts.end;
        if (mayShareItsPlace && heldItsValueAtTheWrite)
            return fail(FailureKind::SpillSlotLacksItsValue, reader);
    }
    return true;
}

bool AllocationChecker::noteSpillWrite(SpillIndex spill, Width width, const Operation* writer)
{
    const SpillFacts& facts = m_spills[spill];
    if (bytesForWidth(width) > bytesForWidth(facts.width))
        return fail(FailureKind::SpillSlotLacksItsValue, writer);
    m_wroteSpillBehindLastExecute = true;
    if (isInRange(facts))
        return true;
    bool beginsAtNextOperation = facts.begin == m_now.upper + 1;
    if (!beginsAtNextOperation && !beginsBehindExtractsAlone(facts) && !isPhiOfOnlySuccessor(facts.value))
        return fail(FailureKind::SpillSlotLacksItsValue, writer);
    m_writesOutsideRange.append(m_arena, { spill, m_now });
    return true;
}

bool AllocationChecker::checkHolds(Location location, const Operation* value, const Operation* reader)
{
    FailureKind kind = location.isMemory() ? FailureKind::SpillSlotLacksItsValue : FailureKind::RegisterLacksItsValue;
    Place place;
    if (!findPlace(location, place))
        return fail(kind, reader);
    if (location.isRegister()) {
        Reg reg = location.reg();
        if (m_pinned & maskOf(reg))
            return isHeldInPinnedRegister(reg, value) || fail(kind, reader);
        if (!value->type().isNumeric() || bankOf(reg) != bankOf(value->type()))
            return fail(kind, reader);
    } else if (location.isSpill() && !noteSpillRead(location.spillIndex(), reader))
        return false;
    ValueID id = idOf(value);
    return (id && m_state[place] == id) || fail(kind, reader);
}

unsigned AllocationChecker::numberOfFusedLoadsBelow(const Operation* operation) const
{
    unsigned count = 0;
    for (Operation* child : operation->children()) {
        if (m_liveness.isFused(child))
            count += isLoad(child->opcode()) + numberOfFusedLoadsBelow(child);
    }
    return count;
}

bool AllocationChecker::checkLeaf(const Operation* reader, const Operation* user, unsigned childIndex, const Operation* leaf, Location location, unsigned& numberOfMemoryOperands)
{
    switch (location.kind()) {
    case Location::None:
    case Location::Outgoing:
        return fail(FailureKind::RegisterLacksItsValue, reader);
    case Location::Register:
        if (registerMaskOf(location) & m_scratch)
            return fail(FailureKind::RegisterLacksItsValue, reader);
        return checkHolds(location, leaf, reader);
    case Location::Spill:
        ++numberOfMemoryOperands;
        if (isLeafOfAddress(m_liveness, user, childIndex) || !acceptsMemoryOperand(*user, childIndex))
            return fail(FailureKind::SpillSlotLacksItsValue, reader);
        return checkHolds(location, leaf, reader);
    case Location::InstanceWord: {
        ++numberOfMemoryOperands;
        if (!leaf->isConstant() || !acceptsMemoryOperand(*user, childIndex))
            return fail(FailureKind::RegisterLacksItsValue, reader);
        std::optional<PinnedConstant> pinned = findPinnedConstant(static_cast<uint64_t>(leaf->constantBits()));
        if (!pinned || pinned->reg != instanceGPR || !pinned->offset || *pinned->offset != location.offsetInInstance())
            return fail(FailureKind::RegisterLacksItsValue, reader);
        return true;
    }
    case Location::SlotBytes:
        if (!namesStackSlot(leaf, location.indexOfStackSlot()))
            return fail(FailureKind::RegisterLacksItsValue, reader);
        [[fallthrough]];
    case Location::Immediate:
        if (!leaf->hasNoPosition() || !standsInInstruction(m_liveness, user, childIndex, leaf))
            return fail(FailureKind::RegisterLacksItsValue, reader);
        return true;
    }
    return fail(FailureKind::RegisterLacksItsValue, reader);
}

bool AllocationChecker::checkSources(const Step& step, RegisterMask& sources)
{
    const Operation* operation = step.operation;
    const Operation* root = operation;
    bool rootIsTheOnlyLeaf = false;
    if (operation->opcode() == Check) {
        root = operation->child(0);
        rootIsTheOnlyLeaf = !m_liveness.isFused(root);
    }

    unsigned numberOfLeaves = 0;
    unsigned numberOfMemoryOperands = rootIsTheOnlyLeaf ? 0 : numberOfFusedLoadsBelow(root);
    if (m_liveness.worksInMemory(root))
        numberOfMemoryOperands = 1;
    bool isSound = true;
    auto checkNextLeaf = [&](const Operation* user, unsigned childIndex, const Operation* leaf) {
        if (!isSound)
            return;
        if (numberOfLeaves == maxLeaves) {
            isSound = fail(FailureKind::RegisterLacksItsValue, operation);
            return;
        }
        Location location = step.sources[numberOfLeaves++];
        sources |= registerMaskOf(location);
        isSound = checkLeaf(operation, user, childIndex, leaf, location, numberOfMemoryOperands);
    };
    if (rootIsTheOnlyLeaf)
        checkNextLeaf(operation, 0, root);
    else
        forEachLeaf(m_liveness, root, checkNextLeaf);
    if (!isSound)
        return false;
    return numberOfMemoryOperands <= 1 || fail(FailureKind::SpillSlotLacksItsValue, operation);
}

bool AllocationChecker::storeBehindDefinition(const Operation* value)
{
    SpillIndex spill = m_allocation.spillOf(value);
    Reg reg = m_allocation.registerAtDefinitionOf(value);
    if (spill == noSpill || !reg.isSet() || m_allocation.isWrittenByEdges(value))
        return true;
    if (spill < m_spills.size() && (m_spills[spill].isStoredAtHeadOfFramedPart || m_spills[spill].isReadByNoStep))
        return true;
    if (spill >= m_spills.size() || reg.index() >= numberOfRegisters || m_spills[spill].value != value || !value->type().isNumeric())
        return fail(FailureKind::SpillSlotLacksItsValue, value);
    if (!noteSpillWrite(spill, widthOf(value->type()), value))
        return false;
    ValueID id = idOf(value);
    if (!id || m_state[reg.index()] != id)
        return fail(FailureKind::SpillSlotLacksItsValue, value);
    set(m_firstSpillPlace + spill, id);
    killSecondScratchGPR();
    return true;
}

bool AllocationChecker::checkBranchOnOverflow(const Step& step)
{
    const Operation* branch = step.operation;
    SumTestedByOverflow test = sumTestedByOverflowOf(m_liveness, branch);
    const Step* stepOfSum = m_stepThatLeftFlags;
    if (!stepOfSum || stepOfSum->operation != test.sum)
        return fail(FailureKind::RegisterLacksItsValue, branch);
    const Operation* operand = step.other;
    if (!operand)
        return true;

    unsigned index = operand == test.left() ? 0 : 1;
    const Operation* otherOperand = index ? test.left() : test.right();
    if ((index && (operand != test.right() || test.subtracts())) || operand == otherOperand || operand->type() != Int32 || test.isWidenedWhereTestFails)
        return fail(FailureKind::RegisterLacksItsValue, branch);
    Location taken = stepOfSum->sources[index];
    Location other = stepOfSum->sources[1 - index];
    if (!taken.isRegister() || taken != stepOfSum->destination)
        return fail(FailureKind::RegisterLacksItsValue, branch);

    ValueID held = m_state[taken.reg().index()];
    bool holdsLowHalfOfSum = held == idOf(test.sum);
    if (!holdsLowHalfOfSum && held && held <= m_proc.numberOfOperations()) {
        const Operation* holder = m_proc.operationAt(held - 1);
        holdsLowHalfOfSum = holder->opcode() == Trunc && holder->child(0) == test.sum;
    }
    if (!holdsLowHalfOfSum)
        return fail(FailureKind::RegisterLacksItsValue, branch);
    if (other.isRegister())
        return checkHolds(other, otherOperand, branch);
    return other.isImmediate() || fail(FailureKind::RegisterLacksItsValue, branch);
}

bool AllocationChecker::executeOperation(const Step& step)
{
    const Operation* operation = step.operation;
    if (operation->opcode() == Extract) {
        if (step.destination.isSpill()) {
            if (m_allocation.registerAtDefinitionOf(operation).isSet() || step.destination.spillIndex() != m_allocation.spillOf(operation))
                return fail(FailureKind::SpillSlotLacksItsValue, operation);
            m_now = m_behind;
        }
        if (!checkHolds(step.destination, operation, operation))
            return false;
        m_now = m_behind;
        return storeBehindDefinition(operation);
    }
    if (operation->opcode() == ArgumentReg) {
        if (!checkHolds(Location::reg(operation->argumentReg()), operation, operation))
            return false;
        if (!operation->type().isNumeric() || !(registerMaskOf(step.destination) & m_file.allocatableIn(bankOf(operation->type()))))
            return fail(FailureKind::RegisterLacksItsValue, operation);
        set(step.destination.reg().index(), idOf(operation));
        m_now = m_behind;
        return storeBehindDefinition(operation);
    }

    RegisterMask sources = 0;
    if (!checkSources(step, sources))
        return false;
    bool goesByOverflow = m_liveness.goesByOverflow(operation);
    if (goesByOverflow && operation->opcode() == Branch && !checkBranchOnOverflow(step))
        return false;
    if (goesByOverflow && operation->opcode() != Branch)
        m_stepThatLeftFlags = &step;
    if (operation->opcode() == Return && operation->numChildren() == 1) {
        Reg expected = bankOf(operation->child(0)->type()) == Bank::GP ? Reg(GPRInfo::returnValueGPR) : Reg(FPRInfo::returnValueFPR);
        if (sources != maskOf(expected))
            return fail(FailureKind::RegisterLacksItsValue, operation);
    }

    RegisterMask scratch = 0;
    if (step.patchAssignment != UINT32_MAX) {
        if (step.patchAssignment >= m_allocation.numberOfPatchAssignments())
            return fail(FailureKind::RegisterLacksItsValue, operation);
        const PatchAssignment& assignment = m_allocation.patchAssignment(step.patchAssignment);
        scratch = assignment.gpScratch;
        if (assignment.fpScratch)
            return fail(FailureKind::RegisterLacksItsValue, operation);
    }
    if (static_cast<unsigned>(std::popcount(scratch)) != numberOfScratchGPRsOf(m_proc.traits(), *operation)
        || (scratch & ~m_file.allocatableIn(Bank::GP))
        || (scratch & (sources | registerMaskOf(step.destination))))
        return fail(FailureKind::RegisterLacksItsValue, operation);

    m_now = m_behind;
    killRegisters(m_scratch | scratch);
    Type type = operation->type();
    if (type == Void)
        return true;
    if (!type.isNumeric() || !(registerMaskOf(step.destination) & m_file.allocatableIn(bankOf(type))))
        return fail(FailureKind::RegisterLacksItsValue, operation);
    set(step.destination.reg().index(), idOf(operation));
    return storeBehindDefinition(operation);
}

bool AllocationChecker::checkOperand(const PatchOperation* patch, unsigned child, Location location, RegisterMask deadBehind)
{
    ValueRep constraint = patch->onlyKeepsChildrenAlive ? ValueRep(ValueRep::ColdAny) : patch->constraintOf(child);
    const Operation* value = patch->operands()[child];
    RegisterMask mask = registerMaskOf(location);
    bool isMet = false;
    switch (constraint.kind()) {
    case ValueRep::WarmAny:
    case ValueRep::ColdAny:
        isMet = true;
        break;
    case ValueRep::LateColdAny:
        isMet = !(mask & deadBehind);
        break;
    case ValueRep::SomeRegister:
        isMet = !!mask;
        break;
    case ValueRep::SomeLateRegister:
        isMet = mask && !(mask & deadBehind);
        break;
    case ValueRep::Register:
        isMet = mask == maskOf(constraint.reg());
        break;
    case ValueRep::StackArgument:
        isMet = isOutgoingWord(location, constraint.offsetFromSP());
        break;
    case ValueRep::SomeEarlyRegister:
    case ValueRep::Stack:
    case ValueRep::Constant:
        break;
    }
    if (!constraint.isReg() && (mask & (m_scratch | patch->earlyClobberedMask())))
        isMet = false;
    if (!isMet)
        return fail(FailureKind::RegisterLacksItsValue, patch);
    if (location.isImmediate()) {
        bool isNeverRead = patch->onlyKeepsChildrenAlive && value->hasNoPosition();
        return (constraint.isAny() && (value->isConstant() || isNeverRead)) || fail(FailureKind::RegisterLacksItsValue, patch);
    }
    return checkHolds(location, value, patch);
}

bool AllocationChecker::executePatch(const Step& step, const PatchOperation* patch)
{
    if (step.patchAssignment >= m_allocation.numberOfPatchAssignments())
        return fail(FailureKind::RegisterLacksItsValue, patch);
    const PatchAssignment& assignment = m_allocation.patchAssignment(step.patchAssignment);
    bool isCheck = patch->opcode() == Check;
    bool runsCodeOfGenerator = !isCheck && !patch->onlyKeepsChildrenAlive;
    unsigned numberOfResults = m_proc.resultCount(patch->type());
    unsigned firstOperand = isCheck ? 1 : 0;
    if (patch->numberOfOperands() < firstOperand || assignment.locations.size() != numberOfResults + patch->numberOfOperands() - firstOperand)
        return fail(FailureKind::RegisterLacksItsValue, patch);
    std::span<const Location> locationsOfResults = assignment.locations.first(numberOfResults);
    std::span<const Location> locationsOfOperands = assignment.locations.subspan(numberOfResults);

    RegisterMask early = patch->earlyClobberedMask();
    RegisterMask late = patch->lateClobberedMask();
    RegisterMask scratch = assignment.gpScratch | assignment.fpScratch;
    RegisterMask results = 0;
    for (Location location : locationsOfResults) {
        RegisterMask mask = registerMaskOf(location);
        if (mask & results)
            return fail(FailureKind::RegisterLacksItsValue, patch);
        results |= mask;
    }

    for (unsigned child = firstOperand; child < patch->numberOfOperands(); ++child) {
        Reg reg = patch->registerToBringOperandTo(child);
        if (reg && registerMaskOf(locationsOfOperands[child - firstOperand]) != maskOf(reg))
            late |= maskOf(reg) & ~results;
    }

    RegisterMask operands = 0;
    if (isCheck && !checkSources(step, operands))
        return false;
    RegisterMask deadBehind = late | scratch | results;
    if (patch->isSuspension()) {
        RegisterMask kept = m_pinned;
        if constexpr (SavedFrame::preservedRegister != InvalidGPRReg)
            kept |= maskOf(Reg(SavedFrame::preservedRegister));
        deadBehind |= ~kept;
    }
    for (unsigned child = firstOperand; child < patch->numberOfOperands(); ++child) {
        Location location = locationsOfOperands[child - firstOperand];
        if (!checkOperand(patch, child, location, deadBehind))
            return false;
        operands |= registerMaskOf(location);
    }

    if (std::popcount(assignment.gpScratch) != patch->numGPScratchRegisters
        || std::popcount(assignment.fpScratch) != patch->numFPScratchRegisters
        || (assignment.gpScratch & ~m_file.allocatableIn(Bank::GP))
        || (assignment.fpScratch & ~m_file.allocatableIn(Bank::FP))
        || (scratch & (operands | results | early | late)))
        return fail(FailureKind::RegisterLacksItsValue, patch);

    for (unsigned index = 0; index < numberOfResults; ++index) {
        Location location = locationsOfResults[index];
        RegisterMask mask = registerMaskOf(location);
        ValueRep constraint = index < patch->resultConstraints.size() ? patch->resultConstraints[index] : ValueRep(ValueRep::SomeRegister);
        bool isMet = false;
        switch (constraint.kind()) {
        case ValueRep::SomeRegister:
            isMet = (mask & m_allocatable) && !(mask & late);
            break;
        case ValueRep::SomeEarlyRegister:
            isMet = (mask & m_allocatable) && !(mask & (late | operands));
            break;
        case ValueRep::Register:
            isMet = mask == maskOf(constraint.reg());
            break;
        case ValueRep::StackArgument:
            isMet = isOutgoingWord(location, constraint.offsetFromSP());
            break;
        case ValueRep::WarmAny:
        case ValueRep::ColdAny:
        case ValueRep::LateColdAny:
        case ValueRep::SomeLateRegister:
        case ValueRep::Stack:
        case ValueRep::Constant:
            break;
        }
        if (mask && ((mask & (m_pinned | m_scratch)) || bankOf(location.reg()) != bankOf(m_proc.resultType(patch->type(), index))))
            isMet = false;
        if (!isMet)
            return fail(FailureKind::RegisterLacksItsValue, patch);
    }
    if (runsCodeOfGenerator && (results & ~assignment.usedRegisters))
        return fail(FailureKind::RegisterLacksItsValue, patch);

    m_now = m_behind;
    killRegisters(m_scratch);
    if (runsCodeOfGenerator) {
        killRegisters(scratch | early | late | ~assignment.usedRegisters);
        killOutgoingWords();
        if (patch->isSuspension() && !forgetWhatSuspensionLoses(patch))
            return false;
    }

    for (unsigned index = 0; index < numberOfResults; ++index) {
        Place place;
        if (!findPlace(locationsOfResults[index], place))
            return fail(FailureKind::RegisterLacksItsValue, patch);
        set(place, patch->type().isTuple() ? m_firstMember[patch->index()] + index : idOf(patch));
    }
    if (numberOfResults == 1 && !patch->type().isTuple() && locationsOfResults[0].isRegister())
        return storeBehindDefinition(patch);
    return true;
}

bool AllocationChecker::matchNextOperation(const Operation* operation, Position& early)
{
    std::span<Operation* const> sunk = m_liveness.sunkInto(m_block);
    if (m_nextSunk < sunk.size()) {
        if (sunk[m_nextSunk++] != operation)
            return fail(FailureKind::RegisterLacksItsValue, operation);
        early = m_head;
        return true;
    }
    while (m_nextOperation < m_block->size()) {
        const Operation* expected = m_block->at(m_nextOperation++);
        bool stands = m_liveness.standsInItsBlock(expected);
        if (expected == operation) {
            early = m_order.earlyPositionOf(m_block, m_nextOperation - 1);
            return stands || fail(FailureKind::RegisterLacksItsValue, operation);
        }
        if (stands && !mayLackStep(expected))
            return fail(FailureKind::RegisterLacksItsValue, expected);
    }
    return fail(FailureKind::RegisterLacksItsValue, operation);
}

bool AllocationChecker::execute(const Step& step)
{
    const Operation* operation = step.operation;
    if (!operation)
        return fail(FailureKind::RegisterLacksItsValue, nullptr);
    if (operation->opcode() == Phi || operation->opcode() == Upsilon)
        return true;
    if (m_stepThatLeftFlags && operation->opcode() != Trunc && operation != m_block->last())
        return fail(FailureKind::RegisterLacksItsValue, operation);
    Position early;
    if (!matchNextOperation(operation, early))
        return false;
    bool isSunk = m_liveness.isSunk(operation);
    m_now = { early, isSunk ? early + 1 : early };
    m_behind = { isSunk ? early : early + 1, early + 1 };
    m_wroteSpillBehindLastExecute = false;
    const PatchOperation* patch = operation->as<PatchOperation>();
    bool isSound = patch ? executePatch(step, patch) : executeOperation(step);
    m_now = m_behind;
    return isSound;
}

bool AllocationChecker::transfer(const Step& step)
{
    RegisterMask toRegister = registerMaskOf(step.destination);
    RegisterMask fromRegister = registerMaskOf(step.sources[0]);
    Place place;
    if (m_stepThatLeftFlags && step.kind != StepKind::Execute && step.kind != StepKind::Move && step.kind != StepKind::Reload && step.kind != StepKind::Save)
        return fail(FailureKind::RegisterLacksItsValue, step.operation);
    switch (step.kind) {
    case StepKind::Execute:
        return execute(step);
    case StepKind::Move:
        if (!fromRegister || !(toRegister & (m_allocatable | m_scratch)) || bankOf(step.sources[0].reg()) != bankOf(step.destination.reg()))
            return fail(FailureKind::RegisterLacksItsValue, step.operation);
        set(step.destination.reg().index(), narrowed(valueIn(step.sources[0].reg(), step.operation), step.width));
        return true;
    case StepKind::Reload: {
        if (!(toRegister & (m_allocatable | m_scratch)) || fromRegister || !findPlace(step.sources[0], place))
            return fail(FailureKind::RegisterLacksItsValue, step.operation);
        if (step.sources[0].isSpill() && !noteSpillRead(step.sources[0].spillIndex(), step.operation))
            return false;
        ValueID value = narrowed(m_state[place], step.width);
        killSecondScratchGPR();
        set(step.destination.reg().index(), value);
        return true;
    }
    case StepKind::Save: {
        if (!fromRegister || toRegister || !findPlace(step.destination, place))
            return fail(FailureKind::SpillSlotLacksItsValue, step.operation);
        if (step.destination.isSpill() && !noteSpillWrite(step.destination.spillIndex(), step.width, step.operation))
            return false;
        set(place, narrowed(valueIn(step.sources[0].reg(), step.operation), step.width));
        killSecondScratchGPR();
        return true;
    }
    case StepKind::Materialize:
        if (!step.operation || !step.operation->hasNoPosition() || !step.operation->type().isNumeric()
            || !(toRegister & (m_allocatable | m_scratch)) || bankOf(step.destination.reg()) != bankOf(step.operation->type()))
            return fail(FailureKind::RegisterLacksItsValue, step.operation);
        killRegisters(m_scratchGPRs);
        set(step.destination.reg().index(), idOf(step.operation));
        return true;
    case StepKind::Swap: {
        if (!(toRegister & m_allocatable) || !(fromRegister & m_allocatable) || bankOf(step.sources[0].reg()) != bankOf(step.destination.reg()))
            return fail(FailureKind::RegisterLacksItsValue, step.operation);
        ValueID endsInSource = narrowed(m_state[step.destination.reg().index()], step.width);
        ValueID endsInDestination = narrowed(m_state[step.sources[0].reg().index()], step.width);
        set(step.destination.reg().index(), endsInDestination);
        set(step.sources[0].reg().index(), endsInSource);
        killRegisters(m_scratch);
        return true;
    }
    case StepKind::ClearSpill: {
        if (!step.destination.isSpill() || !findPlace(step.destination, place))
            return fail(FailureKind::SpillSlotLacksItsValue, step.operation);
        const SpillFacts& facts = m_spills[step.destination.spillIndex()];
        bool standsRightBehindLastRead = m_now.lower == facts.end + 1 && facts.begin <= m_now.upper && !m_wroteSpillBehindLastExecute;
        if (!isInRange(facts) && !standsRightBehindLastRead)
            return fail(FailureKind::SpillSlotLacksItsValue, step.operation);
        set(place, nothing);
        killSecondScratchGPR();
        return true;
    }
    case StepKind::KeepPointer: {
        if (!step.operation || !step.destination.isSpill() || !findPlace(step.destination, place))
            return fail(FailureKind::SpillSlotLacksItsValue, step.operation);
        const SpillFacts& facts = m_spills[step.destination.spillIndex()];
        if (!facts.isPointerBesideNarrowReference || facts.value != step.operation)
            return fail(FailureKind::SpillSlotLacksItsValue, step.operation);
        if (!fromRegister || (fromRegister & m_scratch))
            return fail(FailureKind::RegisterLacksItsValue, step.operation);
        if (!checkHolds(step.sources[0], step.operation, step.operation) || !noteSpillWrite(step.destination.spillIndex(), Width64, step.operation))
            return false;
        killRegisters(m_scratchGPRs);
        set(place, idOf(step.operation) | isPointerOfReference);
        return true;
    }
    }
    return fail(FailureKind::RegisterLacksItsValue, step.operation);
}

void AllocationChecker::findNextExecute(uint32_t begin, uint32_t end)
{
    for (uint32_t index = begin; index < end; ++index) {
        const Step& step = m_allocation.step(index);
        if (step.kind != StepKind::Execute || !step.operation)
            continue;
        bool standsAtHead = step.operation->owner() != m_block || m_liveness.isSunk(step.operation);
        m_now.upper = standsAtHead ? m_head + 1 : m_order.earlyPositionOf(step.operation);
        return;
    }
    m_now.upper = m_order.earlyPositionOf(m_block, m_block->size() - 1);
}

bool AllocationChecker::runSteps(uint32_t begin, uint32_t end, bool isOnEdge)
{
    findNextExecute(begin, end);
    for (uint32_t index = begin; index < end; ++index) {
        const Step& step = m_allocation.step(index);
        bool isExecute = step.kind == StepKind::Execute;
        if (isExecute && isOnEdge)
            return fail(FailureKind::RegisterLacksItsValue, step.operation);
        if (!transfer(step))
            return false;
        if (isExecute)
            findNextExecute(index + 1, end);
    }
    return true;
}

bool AllocationChecker::enter(Block* block)
{
    m_block = block;
    m_head = m_order.headOf(block);
    m_now = { m_head, m_head + 1 };
    m_nextSunk = 0;
    m_nextOperation = 0;
    m_writesOutsideRange.clear();
    m_wroteSpillBehindLastExecute = false;
    m_stepThatLeftFlags = nullptr;

    clearState();
    for (const Entry& entry : m_heads[block->index()].entries)
        set(entry.place, entry.value);
    if (block->isEntry()) {
        for (Operation* operation : block->operations()) {
            if (operation->opcode() == ArgumentReg && !(maskOf(operation->argumentReg()) & (m_pinned | m_scratch)))
                set(operation->argumentReg().index(), idOf(operation));
        }
    }

    for (const RegisterBinding& binding : m_allocation.registersAtHeadOf(block)) {
        if (!binding.value || binding.reg.index() >= numberOfRegisters || !(maskOf(binding.reg) & m_allocatable))
            return fail(FailureKind::RegisterLacksItsValue, binding.value);
        ValueID id = idOf(binding.value);
        if (!id)
            return fail(FailureKind::RegisterLacksItsValue, binding.value);
        if (m_state[binding.reg.index()] != id) {
            SpillIndex spill = binding.value->opcode() == Phi ? m_allocation.spillOf(binding.value) : noSpill;
            if (spill >= m_spills.size() || !m_spills[spill].isSharedByPhis)
                return fail(FailureKind::RegisterLacksItsValue, binding.value);
        }
    }

    for (Operation* phi : block->phis()) {
        if (m_liveness.isDropped(phi) || !m_allocation.isSpilled(phi))
            continue;
        if (m_allocation.registerAtDefinitionOf(phi).isSet() && !m_allocation.isWrittenByEdges(phi)) {
            if (!storeBehindDefinition(phi))
                return false;
            continue;
        }
        SpillIndex spill = m_allocation.spillOf(phi);
        if (spill >= m_spills.size() || m_state[m_firstSpillPlace + spill] != idOf(phi))
            return fail(FailureKind::SpillSlotLacksItsValue, phi);
    }
    m_now.lower = m_head;
    return true;
}

void AllocationChecker::flowAlong(Block* from, unsigned successorIndex)
{
    Block* target = from->successor(successorIndex).block();
    const BlockPlace& placeOfTarget = m_order.of(target);
    if (!placeOfTarget.isReached())
        return;
    Position head = placeOfTarget.head;

    RegisterMask killed = 0;
    Position definitionOfLostResults = noPosition;
    if (from->last()->opcode() == Patch) {
        killed = static_cast<const PatchOperation*>(from->last())->lateClobberedOnSuccessor(successorIndex);
        if (successorIndex)
            definitionOfLostResults = m_values[from->last()->index() + 1].definition;
    }
    if (target->isLandingPad())
        killed = ~RegisterMask { 0 };
    auto runsInFrame = [&](const Block* block) {
        const BlockPlace& place = m_order.of(block);
        return m_allocation.isInFramedPart(block) || (place.isRare() && m_allocation.needsFrameIn(place.rareRegion));
    };
    bool leavesFramedPart = m_allocation.hasFramedPart() && !m_allocation.needsFrameAheadOfFramedPart() && runsInFrame(from) && (!runsInFrame(target) || m_allocation.isHeadOfFramedPart(target));
    if (leavesFramedPart)
        killed |= m_file.calleeSaved;

    for (Place place : m_touched) {
        ValueID value = m_state[place];
        if (!value)
            continue;
        Position definition = m_values[value & ~isPointerOfReference].definition;
        if (definition >= head || definition == definitionOfLostResults)
            continue;
        if (leavesFramedPart && place >= numberOfRegisters)
            continue;
        if (place < numberOfRegisters) {
            if (killed >> place & 1)
                continue;
        } else if (place < m_firstSpillPlace) {
            if (target->isLandingPad())
                continue;
        } else {
            const SpillFacts& facts = m_spills[place - m_firstSpillPlace];
            if (facts.begin > head + 1 || facts.end < head || facts.isInGap(head))
                continue;
        }
        m_onEdge[place] = value;
    }

    if (from->last()->opcode() == Branch && m_liveness.goesByOverflow(from->last())) {
        const Operation* operandTakenBack = m_allocation.step(m_allocation.stepsOf(from).terminator).other;
        if (operandTakenBack && successorIndex != sumTestedByOverflowOf(m_liveness, from->last()).successorThatPasses)
            m_onEdge[m_stepThatLeftFlags->destination.reg().index()] = idOf(operandTakenBack);
    }

    for (Operation* upsilon : from->upsilons()) {
        Operation* phi = upsilon->phi();
        if (phi && phi->owner() == target)
            m_incoming[phi->index()] = idOf(upsilon->child(0));
    }
    for (const RegisterBinding& binding : m_allocation.registersAtHeadOf(target)) {
        if (!binding.value || binding.value->opcode() != Phi || binding.value->owner() != target || binding.reg.index() >= numberOfRegisters)
            continue;
        Place place = binding.reg.index();
        ValueID incoming = m_incoming[binding.value->index()];
        if (incoming && m_state[place] == incoming && !(killed >> place & 1))
            m_onEdge[place] = idOf(binding.value);
    }
    for (Operation* phi : target->phis()) {
        SpillIndex spill = m_allocation.spillOf(phi);
        if (m_liveness.isDropped(phi) || spill >= m_spills.size() || (m_allocation.registerAtDefinitionOf(phi).isSet() && !m_allocation.isWrittenByEdges(phi)))
            continue;
        Place place = m_firstSpillPlace + spill;
        ValueID incoming = m_incoming[phi->index()];
        m_onEdge[place] = incoming && m_state[place] == incoming ? idOf(phi) : nothing;
    }
    for (Operation* upsilon : from->upsilons()) {
        if (Operation* phi = upsilon->phi())
            m_incoming[phi->index()] = nothing;
    }

    Head& headOfTarget = m_heads[target->index()];
    bool hasChanged = false;
    if (!headOfTarget.isSet) {
        size_t count = 0;
        for (Place place : m_touched)
            count += !!m_onEdge[place];
        headOfTarget.entries = m_arena.createSpan<Entry>(count);
        size_t next = 0;
        for (Place place : m_touched) {
            if (m_onEdge[place])
                headOfTarget.entries[next++] = { place, m_onEdge[place] };
        }
        headOfTarget.isSet = true;
        hasChanged = true;
    } else {
        size_t kept = 0;
        for (const Entry& entry : headOfTarget.entries) {
            if (m_onEdge[entry.place] == entry.value)
                headOfTarget.entries[kept++] = entry;
        }
        hasChanged = kept != headOfTarget.entries.size();
        headOfTarget.entries = headOfTarget.entries.first(kept);
    }
    for (Place place : m_touched)
        m_onEdge[place] = nothing;

    if (!hasChanged)
        return;
    headOfTarget.isDirty = true;
    if (placeOfTarget.orderIndex <= m_order.of(from).orderIndex)
        m_needsAnotherRound = true;
}

void AllocationChecker::checkBlock(Block* block)
{
    const BlockSteps& range = m_allocation.stepsOf(block);
    size_t numberOfSteps = m_allocation.steps().size();
    bool hasEdgeMoves = range.beginOfEdgeMoves != range.endOfEdgeMoves;
    if (block->isEmpty() || range.begin > range.end || range.end > numberOfSteps
        || range.beginOfEdgeMoves > range.endOfEdgeMoves || range.endOfEdgeMoves > numberOfSteps
        || range.terminator >= numberOfSteps || (hasEdgeMoves && block->numSuccessors() != 1)) {
        fail(FailureKind::RegisterLacksItsValue, block->isEmpty() ? nullptr : block->last());
        return;
    }
    const Step& terminator = m_allocation.step(range.terminator);
    if (terminator.kind != StepKind::Execute || terminator.operation != block->last()) {
        fail(FailureKind::RegisterLacksItsValue, block->last());
        return;
    }

    if (!enter(block))
        return;
    if (m_allocation.isHeadOfFramedPart(block)) {
        uint32_t begin = m_allocation.headOfFramedPartAbove(block).beginOfSteps;
        uint32_t end = m_allocation.headOfFramedPartAbove(block).endOfSteps;
        if (begin > end || end > numberOfSteps) {
            fail(FailureKind::SpillSlotLacksItsValue, block->last());
            return;
        }
        if (!runSteps(begin, end, true))
            return;
    }
    if (!runSteps(range.begin, range.end, false) || !runSteps(range.beginOfEdgeMoves, range.endOfEdgeMoves, true) || !execute(terminator))
        return;
    for (unsigned index = 0; index < block->numSuccessors(); ++index)
        flowAlong(block, index);
}

bool AllocationChecker::run()
{
    for (const Entrypoint& entrypoint : m_proc.entrypoints()) {
        Head& head = m_heads[entrypoint.block->index()];
        head.isSet = true;
        head.isDirty = true;
    }
    do {
        m_needsAnotherRound = false;
        for (Block* block : m_order.blocksInOrder()) {
            Head& head = m_heads[block->index()];
            if (!head.isDirty)
                continue;
            head.isDirty = false;
            checkBlock(block);
            if (m_failed)
                return false;
        }
    } while (m_needsAnotherRound);
    return true;
}

bool checkAllocation(Procedure& proc)
{
    AllocationChecker checker(proc);
    return checker.run();
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
