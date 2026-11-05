/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineAllocator.h"

#if ENABLE(AOT)

#include "AOTMachineCallsOnWays.h"
#include <algorithm>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

static constexpr uint16_t flagsOfOperationWithLeavesOfItsOwn = Allocator::bitOf(ValueFlag::WorksInMemory) | Allocator::bitOf(ValueFlag::GoesByOverflow);

static constexpr bool allocatedOperationMayTakeScratchRegisters(Opcode opcode)
{
    return opcode == Switch || opcode == PopCount;
}

enum TraitOfAllocatedOpcode : uint8_t {
    AllocatedOpcodeGoesTheLongWay = 1 << 0,
    AllocatedOpcodeMayJumpUnconditionally = 1 << 1,
    AllocatedOpcodeIsLoad = 1 << 2,
    AllocatedOpcodeIsFusedIntoAddresses = 1 << 3,
    AllocatedOpcodeOverwritesFirstChild = 1 << 4,
    AllocatedOpcodeIsCommutative = 1 << 5,
    AllocatedOpcodeChangesWidthOfInteger = 1 << 6,
    AllocatedOpcodeIsMemoryAccess = 1 << 7,
};

struct TraitsOfAllocatedOpcodes {
    uint8_t of[numberOfOpcodes] { };
};

static constexpr TraitsOfAllocatedOpcodes traitsOfAllocatedOpcodes = [] {
    TraitsOfAllocatedOpcodes traits;
    for (unsigned index = 0; index < numberOfOpcodes; ++index) {
        Opcode opcode = static_cast<Opcode>(index);
        unsigned bits = 0;
        if (opcode == Nop || opcode == ArgumentReg || opcode == Extract || opcode == Phi || opcode == Upsilon || opcode == Patch || opcode == Check || opcode == Jump || opcode == Branch
            || hasNoPosition(opcode) || allocatedOperationMayTakeScratchRegisters(opcode))
            bits |= AllocatedOpcodeGoesTheLongWay;
        if (opcode == Jump || opcode == Branch)
            bits |= AllocatedOpcodeMayJumpUnconditionally;
        if (isLoad(opcode))
            bits |= AllocatedOpcodeIsLoad;
        if (opcode == Add || opcode == Shl || opcode == ZExt32 || opcode == SExt32)
            bits |= AllocatedOpcodeIsFusedIntoAddresses;
        if (resultPrefersRegisterOfFirstChild() && resultOverwritesFirstChild(opcode))
            bits |= AllocatedOpcodeOverwritesFirstChild;
        if (isCommutative(opcode))
            bits |= AllocatedOpcodeIsCommutative;
        if (changesWidthOfInteger(opcode))
            bits |= AllocatedOpcodeChangesWidthOfInteger;
        if (isMemoryAccess(opcode))
            bits |= AllocatedOpcodeIsMemoryAccess;
        traits.of[index] = static_cast<uint8_t>(bits);
    }
    return traits;
}();

static_assert([] {
    for (unsigned index = 0; index < numberOfOpcodes; ++index) {
        Opcode opcode = static_cast<Opcode>(index);
        if ((opcode == Patch || opcode == Check || namesSymbol(opcode)) && !(traitsOfAllocatedOpcodes.of[index] & AllocatedOpcodeGoesTheLongWay))
            return false;
    }
    return true;
}());
static_assert(Operation::maxInlineChildren <= maxLeaves);

enum class KindOfAllocatedOpcode : uint8_t {
    GoesTheLongWay,
    IsPatchOrCheck,
    IsArgumentRegister,
    HasAnyLeaves,
    HasOneChild,
    HasOneChildAndChangesItsWidth,
    HasTwoChildren,
    HasTwoChildrenAndOverwritesFirst,
};

struct KindsOfAllocatedOpcodes {
    KindOfAllocatedOpcode of[numberOfOpcodes] { };
};

static constexpr KindsOfAllocatedOpcodes kindsOfAllocatedOpcodes = [] {
    KindsOfAllocatedOpcodes kinds;
    for (unsigned index = 0; index < numberOfOpcodes; ++index) {
        Opcode opcode = static_cast<Opcode>(index);
        unsigned traits = traitsOfAllocatedOpcodes.of[index];
        unsigned numberOfChildren = descriptionOf(opcode).numberOfChildren;
        KindOfAllocatedOpcode kind = KindOfAllocatedOpcode::HasAnyLeaves;
        if (opcode == Patch || opcode == Check)
            kind = KindOfAllocatedOpcode::IsPatchOrCheck;
        else if (opcode == ArgumentReg)
            kind = KindOfAllocatedOpcode::IsArgumentRegister;
        else if (traits & AllocatedOpcodeGoesTheLongWay)
            kind = KindOfAllocatedOpcode::GoesTheLongWay;
        else if (numberOfChildren == 1 && !(traits & AllocatedOpcodeOverwritesFirstChild))
            kind = (traits & AllocatedOpcodeChangesWidthOfInteger) ? KindOfAllocatedOpcode::HasOneChildAndChangesItsWidth : KindOfAllocatedOpcode::HasOneChild;
        else if (numberOfChildren == 2 && !(traits & AllocatedOpcodeChangesWidthOfInteger))
            kind = (traits & AllocatedOpcodeOverwritesFirstChild) ? KindOfAllocatedOpcode::HasTwoChildrenAndOverwritesFirst : KindOfAllocatedOpcode::HasTwoChildren;
        kinds.of[index] = kind;
    }
    return kinds;
}();

static constexpr unsigned kindOfAllocatedOperationHasNoResult = 1 << 8;

static ALWAYS_INLINE Operation* const* inlineChildrenOfAllocatedOperation(const Operation* operation)
{
    ASSERT(!operation->as<PatchOperation>() && !namesSymbol(operation->opcode()));
    return operation->inlineChildren();
}

static ALWAYS_INLINE unsigned numberOfInlineChildrenOfAllocatedOperation(const Operation* operation)
{
    ASSERT(!operation->as<PatchOperation>() && !namesSymbol(operation->opcode()));
    return operation->numberOfInlineChildren();
}

static ALWAYS_INLINE Reg hintIfAmong(Reg hint, RegisterMask candidates)
{
    return hint && (candidates & maskOf(hint)) ? hint : Reg();
}

static bool allocatedOperationHasFusedLoad(const Liveness& liveness, const Operation* operation)
{
    for (const Operation* child : operation->children()) {
        if (liveness.isFused(child) && (isLoad(child->opcode()) || allocatedOperationHasFusedLoad(liveness, child)))
            return true;
    }
    return false;
}

struct LeavesOfAllocatedOperation {
    ALWAYS_INLINE void append(const Operation* user, unsigned childIndex, Operation* leaf)
    {
        RELEASE_ASSERT(size < maxLeaves);
        leaves[size] = leaf;
        users[size] = user;
        childIndexes[size] = childIndex;
        ++size;
    }

    Operation* leaves[maxLeaves];
    const Operation* users[maxLeaves];
    unsigned childIndexes[maxLeaves];
    unsigned size;
    bool hasFusedLoad;
};

static void listLeavesOfOperationFusedIntoAllocatedOperation(const Liveness&, const ValueFacts*, const Operation* fused, const Operation* selectAbove, LeavesOfAllocatedOperation&);

static ALWAYS_INLINE void listLeavesAmongChildrenOfAllocatedOperation(const Liveness& liveness, const ValueFacts* facts, const Operation* operation, const Operation* selectAbove, LeavesOfAllocatedOperation& leaves)
{
    std::span<Operation* const> children = operation->children();
    Operation* const* firstChild = children.data();
    for (unsigned index = 0, numberOfChildren = static_cast<unsigned>(children.size()); index < numberOfChildren; ++index) {
        Operation* child = firstChild[index];
        if (facts[child->index()].has(ValueFlag::IsFused)) {
            listLeavesOfOperationFusedIntoAllocatedOperation(liveness, facts, child, operation->opcode() == Select ? operation : nullptr, leaves);
            continue;
        }
        if (selectAbove) [[unlikely]] {
            if (Operation* arm = armOfSelectThatExtends(selectAbove, child))
                child = arm;
        }
        leaves.append(operation, index, child);
    }
}

static NEVER_INLINE void listLeavesOfOperationFusedIntoAllocatedOperation(const Liveness& liveness, const ValueFacts* facts, const Operation* fused, const Operation* selectAbove, LeavesOfAllocatedOperation& leaves)
{
    unsigned traits = traitsOfAllocatedOpcodes.of[fused->opcode()];
    leaves.hasFusedLoad |= !!(traits & AllocatedOpcodeIsLoad);
    if ((facts[fused->index()].flags & flagsOfOperationWithLeavesOfItsOwn) || ((traits & AllocatedOpcodeMayJumpUnconditionally) && fused->isUnconditionalJump())) [[unlikely]] {
        leaves.hasFusedLoad |= allocatedOperationHasFusedLoad(liveness, fused);
        forEachLeafBelow(liveness, fused, [&](const Operation* user, unsigned childIndex, Operation* leaf) {
            leaves.append(user, childIndex, leaf);
        }, selectAbove);
        return;
    }
    listLeavesAmongChildrenOfAllocatedOperation(liveness, facts, fused, selectAbove, leaves);
}

static bool allocatedLeafNamesFrame(const Operation* leafWithoutPosition)
{
    const Operation* base = leafWithoutPosition->opcode() == Add ? leafWithoutPosition->child(0) : leafWithoutPosition;
    return base->opcode() == FramePointer || base->opcode() == SlotBase;
}

static ALWAYS_INLINE bool allocatedLocationLiesInFrame(Location location)
{
    switch (location.kind()) {
    case Location::Spill:
    case Location::SlotBytes:
    case Location::Outgoing:
        return true;
    case Location::None:
    case Location::Register:
    case Location::InstanceWord:
    case Location::Immediate:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static NEVER_INLINE SpillIndex keepPointerBesideAllocatedReferenceThatNeedsIt(Allocator& allocator, Operation* reference, SpillIndex spill)
{
    const ValueFacts& facts = allocator.m_liveness.of(reference);
    if (facts.has(ValueFlag::IsDropped) || allocator.pointerWaitsForFramedPart(reference))
        return spill;

    Reg reg = allocator.ensureInRegister(reference, allocator.m_file.allocatableIn(Bank::GP));
    if (!reg) {
        allocator.m_proc.fail(FailureKind::OutOfRegisters, reference);
        return spill;
    }

    if (spill == noSpill) {
        SpillFacts pointer;
        pointer.value = reference;
        pointer.begin = facts.definition;
        pointer.end = facts.lastUse;
        pointer.width = Width64;
        pointer.isPointerBesideNarrowReference = true;
        spill = allocator.addSpill(pointer);
    }

    Step& step = allocator.appendStep(StepKind::KeepPointer, reference);
    step.width = Width64;
    step.destination = Location::spill(spill);
    step.sources[0] = Location::reg(reg);
    allocator.noteFrameUse();
    return spill;
}

static ALWAYS_INLINE SpillIndex keepPointerBesideAllocatedReference(Allocator& allocator, Operation* reference, SpillIndex spill = noSpill)
{
    if (!allocator.m_valueFacts[reference->index()].has(ValueFlag::NeedsPointerBesideIt)) [[likely]]
        return spill;
    return keepPointerBesideAllocatedReferenceThatNeedsIt(allocator, reference, spill);
}

static NEVER_INLINE void bindArgumentRegistersOfEntry(Allocator& allocator, Block* entry)
{
    allocator.m_lastDefinitionOfArgumentRegister = 0;
    for (Operation* argument : entry->operations()) {
        if (argument->opcode() != ArgumentReg || !allocator.m_liveness.standsInItsBlock(argument))
            continue;
        allocator.m_lastDefinitionOfArgumentRegister = std::max(allocator.m_lastDefinitionOfArgumentRegister, allocator.m_liveness.of(argument).definition);
        Reg reg = argument->argumentReg();
        if (!(allocator.m_file.allocatableIn(bankOf(reg)) & maskOf(reg))) {
            allocator.m_proc.fail(FailureKind::MalformedProcedure, argument);
            return;
        }
        if (allocator.m_valueIn[reg.index()] == argument)
            continue;
        if (allocator.m_valueIn[reg.index()])
            allocator.unbind(reg);
        allocator.bind(reg, argument);
    }
}

static RegisterMask argumentRegistersNotYetRead(const Allocator& allocator)
{
    RegisterMask registers = 0;
    forEachRegister(allocator.m_bound, [&](Reg reg) {
        const Operation* value = allocator.m_valueIn[reg.index()];
        if (value->isArgumentReg(reg) && allocator.m_liveness.of(value).definition > allocator.m_position + 1)
            registers |= maskOf(reg);
    });
    return registers;
}

static ALWAYS_INLINE RegisterMask registersReservedAtAllocatedOperation(const Allocator& allocator)
{
    bool someArgumentRegisterMayWait = allocator.m_position + 1 < allocator.m_lastDefinitionOfArgumentRegister && allocator.m_block->isEntry();
    return (someArgumentRegisterMayWait ? argumentRegistersNotYetRead(allocator) : 0) | allocator.m_registersReadOnWayToFailingSuccessor;
}

template<typename Functor>
static ALWAYS_INLINE void forEachIndexOfListOfAllocator(uint32_t count, const Functor& functor)
{
    uint32_t index = 0;
    for (uint32_t endOfFours = count & ~3u; index < endOfFours; index += 4) {
        functor(index);
        functor(index + 1);
        functor(index + 2);
        functor(index + 3);
    }
    for (; index < count; ++index)
        functor(index);
}

Allocation* allocateRegisters(Procedure& proc)
{
    Allocation* allocation = proc.arena().create<Allocation>();
    Allocator allocator(proc, *allocation);
    allocator.run();
    return allocation;
}

Allocator::Allocator(Procedure& proc, Allocation& allocation)
    : m_proc(proc)
    , m_arena(proc.arena())
    , m_order(proc.order())
    , m_liveness(proc.liveness())
    , m_allocation(allocation)
    , m_file(registerFileFor(proc.traits()))
    , m_valueFacts(proc.liveness().valueFacts())
    , m_uses(proc.liveness().uses())
{
    constexpr unsigned numberOfRegisters = Reg::maxIndex() + 1;
    std::fill_n(m_valueIn, numberOfRegisters, nullptr);
    std::fill_n(m_rematerializedValueKeptIn, numberOfRegisters, nullptr);
    std::fill_n(m_rematerializedValueIsKeptUntil, numberOfRegisters, 0);
    uint64_t smallestPinnedConstant = UINT64_MAX;
    for (uint64_t bits : bitsOfPinnedConstants())
        smallestPinnedConstant = std::min(smallestPinnedConstant, bits);
    m_smallestPinnedConstant = smallestPinnedConstant;

    Arena& arena = proc.arena();
    uint32_t numberOfOperations = proc.numberOfOperations();
    uint32_t numberOfBlocks = m_order.mostBlocks();
    uint32_t mostOperations = m_order.mostOperations();

    m_values.growWithoutInitializing(arena, numberOfOperations);
    ValueState* states = m_values.begin();
    const ValueFacts* facts = m_valueFacts;
    forEachIndexOfListOfAllocator(numberOfOperations, [&](uint32_t index) ALWAYS_INLINE_LAMBDA {
        new (NotNull, states + index) ValueState { 0, facts[index].firstUse, noSpill };
    });
    m_heads.grow(arena, numberOfBlocks);
    m_ends.grow(arena, numberOfBlocks);
    m_hints.fill(arena, numberOfOperations, Reg());
    m_hasPassedOnHintsOfPhis = BitSpan::create(arena, numberOfBlocks);

    allocation.m_steps.reserve(arena, numberOfOperations);
    allocation.m_blocks.growWithoutInitializing(arena, numberOfBlocks);
    BlockSteps* stepsOfBlocks = allocation.m_blocks.begin();
    forEachIndexOfListOfAllocator(numberOfBlocks, [&](uint32_t index) ALWAYS_INLINE_LAMBDA {
        new (NotNull, stepsOfBlocks + index) BlockSteps;
    });
    allocation.m_patchAssignments.reserve(arena, numberOfOperations / 4 + 4);
    allocation.m_registersAtHead.grow(arena, numberOfBlocks);
    allocation.m_spillOfValue.fill(arena, mostOperations, noSpill);
    allocation.m_registerAtDefinition.fill(arena, mostOperations, Reg());
    allocation.m_regionNeedsFrame = BitSpan::create(arena, m_order.rareRegions().size());
}

void Allocator::run()
{
    findFramedPart();
    const std::optional<Failure>& failure = m_proc.failure();
    for (Block* block : m_order.blocksInOrder()) {
        allocateBlock(block);
        if (failure) [[unlikely]]
            return;
    }
    Order::placeBlocksSplitLate(m_proc);

    findSpillsHeldAtSuspensions();
    for (const SpillFacts& spill : m_allocation.m_spills) {
        const Operation* value = spill.value;
        if (spill.isSaveInRareRegion || spill.isPointerBesideNarrowReference || m_liveness.isSunk(value))
            continue;
        const Block* blockOfStore = value->endsBlock() ? value->owner()->successor(0).block() : value->owner();
        standIn(blockOfStore);
        if (!leavesValuesAcrossCallsToFramedPart())
            noteFrameUse();
    }

    const Step* allSteps = m_allocation.m_steps.begin();
    auto stepNeedsFrame = [&](const Step& step) ALWAYS_INLINE_LAMBDA {
        if (allocatedLocationLiesInFrame(step.destination))
            return true;
        if (step.kind != StepKind::Execute) {
            if (allocatedLocationLiesInFrame(step.sources[0]))
                return true;
            return step.kind == StepKind::Materialize && allocatedLeafNamesFrame(step.operation);
        }
        for (Location source : step.sources) {
            if (!source)
                break;
            if (allocatedLocationLiesInFrame(source))
                return true;
        }
        const PatchOperation* patch = step.operation->as<PatchOperation>();
        if (patch && (patch->needsFrameRecord() || !patch->runsWithoutFrame()))
            return true;
        if (step.patchAssignment == UINT32_MAX)
            return false;
        for (Location location : m_allocation.patchAssignment(step.patchAssignment).locations) {
            if (allocatedLocationLiesInFrame(location))
                return true;
        }
        return false;
    };
    auto stepsNeedFrame = [&](uint32_t begin, uint32_t end) {
        for (uint32_t index = begin; index < end; ++index) {
            if (stepNeedsFrame(allSteps[index]))
                return true;
        }
        return false;
    };
    const bool hasFramedPart = m_allocation.hasFramedPart();
    auto everyBlockIsKnownToNeedFrame = [&] {
        if (!m_allocation.needsFrameOutsideRareRegions() || (hasFramedPart && !m_allocation.needsFrameAheadOfFramedPart()))
            return false;
        uint32_t numberOfRareRegions = static_cast<uint32_t>(m_order.rareRegions().size());
        for (uint32_t region = 0; region < numberOfRareRegions; ++region) {
            if (!m_allocation.needsFrameIn(region))
                return false;
        }
        return true;
    };
    if (!everyBlockIsKnownToNeedFrame()) {
        const BlockPlace* places = m_order.places();
        const BlockSteps* stepsOfBlocks = m_allocation.stepsOfBlocks();
        for (Block* block : m_order.blocksInOrder()) {
            uint32_t rareRegion = places[block->index()].rareRegion;
            bool isKnownToNeedFrame;
            if (rareRegion != noRareRegion)
                isKnownToNeedFrame = m_allocation.needsFrameIn(rareRegion);
            else if (hasFramedPart && !m_allocation.isInFramedPart(block))
                isKnownToNeedFrame = m_allocation.needsFrameAheadOfFramedPart();
            else
                isKnownToNeedFrame = m_allocation.needsFrameOutsideRareRegions();
            if (isKnownToNeedFrame)
                continue;
            const BlockSteps& steps = stepsOfBlocks[block->index()];
            bool storesWhatSank = std::ranges::any_of(m_liveness.sunkInto(block), [&](const Operation* sunk) {
                return m_allocation.isSpilled(sunk);
            });
            if (storesWhatSank || stepsNeedFrame(steps.begin, steps.end) || stepsNeedFrame(steps.beginOfEdgeMoves, steps.endOfEdgeMoves) || stepsNeedFrame(steps.terminator, steps.terminator + 1)) {
                standIn(block);
                noteFrameUse();
            }
        }
    }

    m_rareRegion = noRareRegion;
    m_isAheadOfFramedPart = false;
    if (m_allocation.usedCalleeSaves())
        noteFrameUse();
    settleFramedPart();
    findGapsAheadOfLandingPads();
}

ALWAYS_INLINE void Allocator::allocateBlock(Block* block)
{
    Allocation& allocation = m_allocation;
    const BlockPlace& place = m_order.places()[block->index()];
    BlockSteps& steps = allocation.m_blocks[block->index()];
    const Position head = place.head;
    const unsigned size = block->size();
    bool standsInFramedPart = false;
    bool standsAheadOfFramedPart = false;
    if (allocation.hasFramedPart()) [[unlikely]] {
        standsInFramedPart = allocation.isInFramedPart(block);
        standsAheadOfFramedPart = !standsInFramedPart;
    }

    m_block = block;
    m_position = head;
    m_headOfBlock = head;
    m_endOfBlock = head + 2 * (size + 1);
    bool everyReadBehindBlockCounts = !place.isRare();
    Position beginOfLoopAroundBlock = 0;
    if (place.loop != noLoop) {
        const Loop& loop = m_order.loops().data()[place.loop];
        beginOfLoopAroundBlock = loop.begin;
        everyReadBehindBlockCounts = everyReadBehindBlockCounts && head < m_liveness.of(loop, m_order).endOfLastExitingBlock;
    }
    m_everyReadBehindBlockCounts = everyReadBehindBlockCounts;
    m_beginOfLoopAroundBlock = beginOfLoopAroundBlock;
    m_endOfWhatWaysLeadToFromBlock = m_liveness.endOfWhatWaysLeadToFrom(block);
    m_isAheadOfFramedPart = standsAheadOfFramedPart;
    m_operandTakenBackByBranch = nullptr;
    m_registersReadOnWayToFailingSuccessor = 0;
    steps.begin = allocation.m_steps.size();
    if (size == 1 && passThroughBlockThatOnlyJumps(block))
        return;
    enterBlock(block);
    const std::optional<Failure>& failure = m_proc.failure();
    if (failure) [[unlikely]]
        return;
    ASSERT(m_rareRegion == place.rareRegion);
    if (standsAheadOfFramedPart && place.entersRareRegion && (m_bound & m_file.calleeSaved)) [[unlikely]]
        allocation.m_needsFrameAheadOfFramedPart = true;

    auto allocate = [&](Operation* operation) ALWAYS_INLINE_LAMBDA {
        allocateByKindOfOpcode<true>(operation);
    };

    if (m_somePatchWithResultHasEndedBlock && place.predecessors.size() == 1 && place.predecessors.data()[0]->last()->opcode() == Patch) [[unlikely]] {
        finishResultsOfPatchThatEndedPredecessor(*this, block);
        if (failure) [[unlikely]]
            return;
    }
    if (standsInFramedPart && allocation.headOfFramedPartAbove(block).block == block) [[unlikely]] {
        enterFramedPart();
        if (failure) [[unlikely]]
            return;
    }
    if (!block->phis().empty()) {
        for (Operation* phi : block->phis())
            keepPointerBesideAllocatedReference(*this, phi);
        if (failure) [[unlikely]]
            return;
    }
    if (block->isEntry()) [[unlikely]] {
        bindArgumentRegistersOfEntry(*this, block);
        if (failure) [[unlikely]]
            return;
    }

    std::span<Operation* const> sunk = m_liveness.sunkInto(block);
    if (!sunk.empty()) [[unlikely]] {
        m_position = head - 2;
        for (Operation* operation : sunk) {
            allocate(operation);
            if (failure)
                return;
        }
        m_position = head;
        for (Operation* operation : sunk)
            passUsesOf(operation, head + 1);
    }

    constexpr uint16_t absent = bitOf(ValueFlag::IsDropped) | bitOf(ValueFlag::IsFused) | bitOf(ValueFlag::IsSunk);
    const ValueFacts* facts = m_valueFacts;
    Operation* const* operations = block->operations().data();
    const unsigned indexOfLast = size - 1;
    Position position = head;
    for (unsigned index = 0; index < indexOfLast; ++index) {
        position += 2;
        Operation* operation = operations[index];
        if (facts[operation->index()].flags & absent)
            continue;
        m_position = position;
        allocate(operation);
        if (failure) [[unlikely]]
            return;
    }

    Operation* last = operations[indexOfLast];
    m_position = head + 2 * size;
    const Opcode opcodeOfLast = last->opcode();
    if (opcodeOfLast == Jump) {
        uint32_t indexOfStep = allocation.m_steps.size();
        appendStep(StepKind::Execute, last);
        steps.terminator = indexOfStep;
        steps.end = indexOfStep;
    } else if (opcodeOfLast == Branch) {
        allocateBranch(last);
        if (failure) [[unlikely]]
            return;
        steps.terminator = allocation.m_steps.size() - 1;
        steps.end = steps.terminator;
    } else if (PatchOperation* patch = last->as<PatchOperation>()) {
        allocatePatch(patch);
        if (failure) [[unlikely]]
            return;
        RELEASE_ASSERT(allocation.m_steps.size() > steps.begin && allocation.m_steps.last().kind == StepKind::Execute && allocation.m_steps.last().operation == patch);
        steps.terminator = allocation.m_steps.size() - 1;
        steps.end = steps.terminator;
        m_somePatchWithResultHasEndedBlock |= patch->type() != Void;
    } else {
        allocateTerminator(block, last);
        if (failure) [[unlikely]]
            return;
    }
    leaveBlock(block);
}

static constexpr uint16_t flagsOfAllocatedValueThatAsksForMoreThanRegister = Allocator::bitOf(ValueFlag::LivesAcrossCall) | Allocator::bitOf(ValueFlag::LivesIntoLandingPad) | Allocator::bitOf(ValueFlag::LivesAcrossSuspension);

NEVER_INLINE void Allocator::allocateArgumentRegister(Operation* operation)
{
    if (!m_block->isEntry()) {
        m_proc.fail(FailureKind::MalformedProcedure, operation);
        return;
    }
    Reg reg = operation->argumentReg();
    Operation* lastReader = m_valueIn[reg.index()];
    if (!lastReader || !lastReader->isArgumentReg(reg) || m_allocation.registerAtDefinitionOf(lastReader)) {
        m_proc.fail(FailureKind::RegisterLacksItsValue, operation);
        return;
    }

    Location location;
    if (lastReader == operation) {
        const ValueFacts& facts = m_valueFacts[operation->index()];
        unbindAll(operation);
        if (!(facts.flags & flagsOfAllocatedValueThatAsksForMoreThanRegister) && (maskOf(reg) & m_file.allocatableIn(bankOf(operation->type())) & ~m_file.calleeSaved)) [[likely]] {
            ASSERT(registerFileIsConsistent());
            ASSERT(!m_valueIn[reg.index()]);
            m_allocation.m_registerAtDefinition[operation->index()] = reg;
            if (facts.lastUse != noPosition)
                bind(reg, operation);
            location = Location::reg(reg);
        } else
            location = defineResult(operation, maskOf(reg), 0, reg);
    } else
        location = defineResult(operation, m_file.allocatableIn(bankOf(reg)), registersReservedAtAllocatedOperation(*this), Reg());
    if (m_proc.failure())
        return;
    if (lastReader != operation) {
        Step& move = appendStep(StepKind::Move, lastReader);
        move.destination = location;
        move.sources[0] = Location::reg(reg);
    }
    appendStep(StepKind::Execute, operation).destination = location;
    keepPointerBesideAllocatedReference(*this, operation);
}

static NEVER_INLINE void allocateExtractBehindItsPatch(Allocator& allocator, Operation* operation)
{
    Reg reg = allocator.m_allocation.registerAtDefinitionOf(operation);
    if (!reg && allocator.m_allocation.isSpilled(operation)) {
        allocator.passUse(operation->child(0), allocator.m_position);
        Step& step = allocator.appendStep(StepKind::Execute, operation);
        step.destination = Location::spill(allocator.m_allocation.spillOf(operation));
        step.sources[0] = step.destination;
        return;
    }
    if (!reg || !(allocator.stateOf(operation).registers & maskOf(reg))) {
        allocator.m_proc.fail(FailureKind::RegisterLacksItsValue, operation);
        return;
    }
    allocator.passUse(operation->child(0), allocator.m_position);
    Step& step = allocator.appendStep(StepKind::Execute, operation);
    step.destination = Location::reg(reg);
    step.sources[0] = Location::reg(reg);
}

struct PlacesTakenByAllocatedLeaves {
    RegisterMask locked;
    bool hasMemoryOperand;
    const Operation* operationNotSearchedForFusedLoads;
};

static ALWAYS_INLINE bool takeMemoryOperandForAllocatedLeaf(const Allocator& allocator, const Operation* user, unsigned childIndex, PlacesTakenByAllocatedLeaves& taken)
{
    if (taken.hasMemoryOperand)
        return false;
    const Liveness& liveness = allocator.m_liveness;
    if ((traitsOfAllocatedOpcodes.of[user->opcode()] & (AllocatedOpcodeIsMemoryAccess | AllocatedOpcodeIsFusedIntoAddresses)) && isLeafOfAddress(liveness, user, childIndex))
        return false;
    ASSERT(!isLeafOfAddress(liveness, user, childIndex));
    if (!acceptsMemoryOperand(*user, childIndex))
        return false;
    const ValueFacts* facts = allocator.m_valueFacts;
    for (const Operation* sibling : user->children()) {
        if (facts[sibling->index()].has(ValueFlag::IsFused) && (traitsOfAllocatedOpcodes.of[sibling->opcode()] & AllocatedOpcodeIsLoad))
            return false;
    }
    if (isX86_64()) {
        if (const Operation* operation = taken.operationNotSearchedForFusedLoads) {
            taken.operationNotSearchedForFusedLoads = nullptr;
            if (allocatedOperationHasFusedLoad(liveness, operation)) {
                taken.hasMemoryOperand = true;
                return false;
            }
        }
    }
    taken.hasMemoryOperand = true;
    return true;
}

static NEVER_INLINE Location placeAllocatedLeafInRegisterOrMemory(Allocator& allocator, const Operation* user, unsigned childIndex, Operation* leaf, PlacesTakenByAllocatedLeaves& taken)
{
    Bank bank = bankOf(leaf->type());
    RegisterMask allocatable = allocator.m_file.allocatableIn(bank);
    Reg reg = allocator.anyRegisterOf(leaf, allocatable);
    if (!reg) {
        RegisterMask candidates = allocatable & ~taken.locked;
        if (!leaf->hasNoPosition()) {
            if (!allocator.m_allocation.isSpilled(leaf) && allocator.stateOf(leaf).saveInRareRegion == noSpill) {
                allocator.m_proc.fail(FailureKind::RegisterLacksItsValue, user);
                return Location();
            }
            allocator.noteFrameUse();
            if ((allocator.m_valueFacts[leaf->index()].lastUse <= allocator.m_position || !allocator.hasFreeRegister(bank, candidates)) && takeMemoryOperandForAllocatedLeaf(allocator, user, childIndex, taken))
                return allocator.memoryLocationOf(leaf);
        }
        reg = allocator.ensureInRegister(leaf, candidates, hintIfAmong(allocator.hintOf(leaf), candidates));
        if (!reg) {
            allocator.m_proc.fail(FailureKind::OutOfRegisters, user);
            return Location();
        }
    }
    taken.locked |= maskOf(reg);
    return Location::reg(reg);
}

static NEVER_INLINE Location placeAllocatedLeafThatStandsInInstruction(Allocator& allocator, const Operation* leaf)
{
    if (allocatedLeafNamesFrame(leaf))
        allocator.noteFrameUse();
    return leaf->opcode() == SlotBase ? Location::slotBytes(leaf->slot()->index()) : Location::immediate();
}

static NEVER_INLINE Location placeAllocatedLeafWithoutPosition(Allocator& allocator, const Operation* user, unsigned childIndex, Operation* leaf, PlacesTakenByAllocatedLeaves& taken)
{
    ASSERT(leaf->hasNoPosition());
    const Opcode opcodeOfLeaf = leaf->opcode();
    if (opcodeOfLeaf == ArgumentReg)
        return Location::reg(leaf->argumentReg());

    bool isImmediate = (traitsOfAllocatedOpcodes.of[user->opcode()] & AllocatedOpcodeIsFusedIntoAddresses) ? standsInInstruction(allocator.m_liveness, user, childIndex, leaf) : acceptsImmediate(*user, childIndex, *leaf);
    ASSERT(isImmediate == standsInInstruction(allocator.m_liveness, user, childIndex, leaf));
    if (opcodeOfLeaf == Const64 || opcodeOfLeaf == ConstDouble) {
        uint64_t bits = static_cast<uint64_t>(leaf->constantBits());
        ASSERT(bits >= allocator.m_smallestPinnedConstant || !findPinnedConstant(bits));
        if (bits >= allocator.m_smallestPinnedConstant) {
            if (std::optional<PinnedConstant> found = findPinnedConstant(bits)) {
                const PinnedConstant pinned = *found;
                if (!isImmediate || pinned.replacesImmediates) {
                    if (!pinned.offset) {
                        if (opcodeOfLeaf == Const64)
                            return Location::reg(pinned.reg);
                    } else {
                        int32_t offset = *pinned.offset;
                        if (takeMemoryOperandForAllocatedLeaf(allocator, user, childIndex, taken))
                            return Location::instanceWord(offset);
                    }
                }
            }
        }
    }
    if (isImmediate)
        return placeAllocatedLeafThatStandsInInstruction(allocator, leaf);
    return placeAllocatedLeafInRegisterOrMemory(allocator, user, childIndex, leaf, taken);
}

static ALWAYS_INLINE Location placeLeafOfAllocatedOperation(Allocator& allocator, const Operation* user, unsigned childIndex, Operation* leaf, RegisterMask& locked, bool& hasMemoryOperand)
{
    const bool leafHasNoPosition = leaf->hasNoPosition();
    if (!leafHasNoPosition) {
        if (RegisterMask holding = allocator.stateOf(leaf).registers & allocator.m_file.allocatableIn(bankOf(leaf->type()))) [[likely]] {
            Reg reg = Allocator::lowestRegisterIn(holding);
            locked |= maskOf(reg);
            return Location::reg(reg);
        }
    } else if (leaf->opcode() == ArgumentReg)
        return Location::reg(leaf->argumentReg());
    PlacesTakenByAllocatedLeaves taken { locked, hasMemoryOperand, nullptr };
    Location location = leafHasNoPosition ? placeAllocatedLeafWithoutPosition(allocator, user, childIndex, leaf, taken) : placeAllocatedLeafInRegisterOrMemory(allocator, user, childIndex, leaf, taken);
    locked = taken.locked;
    hasMemoryOperand = taken.hasMemoryOperand;
    return location;
}

NEVER_INLINE void Allocator::passReadOfOwnerOfInnerPointer(const Operation* innerPointer, Position position)
{
    if (!innerPointer->numChildren())
        return;
    Operation* owner = isLoad(innerPointer->opcode()) ? innerPointer->lastChild() : innerPointer->child(0);
    if (m_liveness.isFused(owner)) {
        passUsesOf(owner, position);
        return;
    }
    passUse(owner, position);
}

ALWAYS_INLINE Location Allocator::defineResultWithFacts(Operation* operation, const ValueFacts& facts, RegisterMask allowed, RegisterMask avoided, Reg preferred)
{
    Bank bank = bankOf(operation->type());
    RegisterMask candidates = allowed & ~avoided & m_file.allocatableIn(bank);
    if (!candidates) [[unlikely]] {
        m_proc.fail(FailureKind::OutOfRegisters, operation);
        return Location();
    }
    if (!preferred)
        preferred = hintOf(operation);

    bool isSpilledAtOnce = facts.flags & (bitOf(ValueFlag::LivesIntoLandingPad) | bitOf(ValueFlag::LivesAcrossSuspension));
    Reg reg;
    if (facts.has(ValueFlag::LivesAcrossCall) && !leavesValuesAcrossCallsToFramedPart() && callStandsOnWayFromDefinitionToRead(*this, operation)) {
        RegisterMask calleeSaved = calleeSavesWorthTakingFor(operation, bank, m_block) & calleeSavesThatMayBeTaken();
        RegisterMask calleeSavedCandidates = candidates & calleeSaved;
        if (calleeSavedCandidates && hasFreeRegister(bank, calleeSavedCandidates))
            reg = takeRegister(bank, calleeSavedCandidates, hintIfAmong(preferred, calleeSavedCandidates));
        else if (calleeSavedCandidates || !calleeSaved || !hasFreeRegister(bank, calleeSaved)) {
            isSpilledAtOnce = true;
        }
    }
    if (!reg)
        reg = takeRegister(bank, candidates, hintIfAmong(preferred, candidates));
    if (!reg) [[unlikely]] {
        m_proc.fail(FailureKind::OutOfRegisters, operation);
        return Location();
    }

    m_allocation.m_registerAtDefinition[operation->index()] = reg;
    if (m_file.calleeSaved & maskOf(reg)) {
        noteCalleeSaveUse(reg);
        noteFrameUse();
    }
    if (facts.lastUse == noPosition)
        return Location::reg(reg);

    bind(reg, operation);
    if (isSpilledAtOnce) {
        markSpilled(operation);
        noteFrameUse();
    }
    return Location::reg(reg);
}

struct PlacedLeavesOfAllocatedOperation {
    static constexpr unsigned capacity = maxLeaves + Operation::maxInlineChildren;

    Location sources[capacity];
    Operation* leaves[capacity];
    unsigned size;
    PlacesTakenByAllocatedLeaves taken;
};

template<typename Functor>
static ALWAYS_INLINE Location placeAllocatedLeafOutsideRegisters(Allocator& allocator, const Operation* user, unsigned childIndex, Operation* leaf, bool targetAloneSaysWhatStandsInInstruction, PlacesTakenByAllocatedLeaves& taken, const Functor& noteFusedLoadsNotCounted)
{
    if (!leaf->hasNoPosition()) {
        noteFusedLoadsNotCounted();
        return placeAllocatedLeafInRegisterOrMemory(allocator, user, childIndex, leaf, taken);
    }
    const Opcode opcodeOfLeaf = leaf->opcode();
    if (opcodeOfLeaf == ArgumentReg)
        return Location::reg(leaf->argumentReg());
    const bool mayBePinnedConstant = (opcodeOfLeaf == Const64 || opcodeOfLeaf == ConstDouble) && static_cast<uint64_t>(leaf->constantBits()) >= allocator.m_smallestPinnedConstant;
    if (targetAloneSaysWhatStandsInInstruction && !mayBePinnedConstant) {
        ASSERT(acceptsImmediate(*user, childIndex, *leaf) == standsInInstruction(allocator.m_liveness, user, childIndex, leaf));
        if (!acceptsImmediate(*user, childIndex, *leaf))
            return placeAllocatedLeafInRegisterOrMemory(allocator, user, childIndex, leaf, taken);
        if (isConstant(opcodeOfLeaf))
            return Location::immediate();
        return placeAllocatedLeafThatStandsInInstruction(allocator, leaf);
    }
    noteFusedLoadsNotCounted();
    return placeAllocatedLeafWithoutPosition(allocator, user, childIndex, leaf, taken);
}

static ALWAYS_INLINE void placeNextLeafOfAllocatedOperation(Allocator& allocator, const Operation* user, unsigned childIndex, Operation* leaf, bool targetAloneSaysWhatStandsInInstruction, PlacedLeavesOfAllocatedOperation& placed)
{
    RELEASE_ASSERT(placed.size < maxLeaves);
    Location source;
    if (RegisterMask holding = leaf->hasNoPosition() ? 0 : allocator.stateOf(leaf).registers) [[likely]] {
        Reg reg = Allocator::lowestRegisterIn(holding);
        placed.taken.locked |= maskOf(reg);
        source = Location::reg(reg);
    } else
        source = placeAllocatedLeafOutsideRegisters(allocator, user, childIndex, leaf, targetAloneSaysWhatStandsInInstruction, placed.taken, [] { });
    placed.leaves[placed.size] = leaf;
    placed.sources[placed.size] = source;
    ++placed.size;
}

static NEVER_INLINE void placeLeavesBelowOperationFusedIntoAllocatedOperation(Allocator& allocator, const Operation* fused, const Operation* selectAbove, PlacedLeavesOfAllocatedOperation& placed)
{
    const Liveness& liveness = allocator.m_liveness;
    const ValueFacts* facts = allocator.m_valueFacts;
    const unsigned traits = traitsOfAllocatedOpcodes.of[fused->opcode()];
    if (isX86_64() && (traits & AllocatedOpcodeIsLoad))
        placed.taken.hasMemoryOperand = true;
    if ((facts[fused->index()].flags & flagsOfOperationWithLeavesOfItsOwn) || ((traits & AllocatedOpcodeMayJumpUnconditionally) && fused->isUnconditionalJump())) [[unlikely]] {
        if (isX86_64() && allocatedOperationHasFusedLoad(liveness, fused))
            placed.taken.hasMemoryOperand = true;
        forEachLeafBelow(liveness, fused, [&](const Operation* user, unsigned childIndex, Operation* leaf) {
            placeNextLeafOfAllocatedOperation(allocator, user, childIndex, leaf, false, placed);
        }, selectAbove);
        return;
    }
    const bool targetAloneSaysWhatStandsInInstruction = !(traits & AllocatedOpcodeIsFusedIntoAddresses);
    std::span<Operation* const> children = fused->children();
    Operation* const* firstChild = children.data();
    for (unsigned index = 0, numberOfChildren = static_cast<unsigned>(children.size()); index < numberOfChildren; ++index) {
        Operation* child = firstChild[index];
        if (facts[child->index()].has(ValueFlag::IsFused)) {
            placeLeavesBelowOperationFusedIntoAllocatedOperation(allocator, child, fused->opcode() == Select ? fused : nullptr, placed);
            continue;
        }
        if (selectAbove) [[unlikely]] {
            if (Operation* arm = armOfSelectThatExtends(selectAbove, child))
                child = arm;
        }
        placeNextLeafOfAllocatedOperation(allocator, fused, index, child, targetAloneSaysWhatStandsInInstruction, placed);
    }
}

static ALWAYS_INLINE bool placeLeavesOfAllocatedOperation(Allocator& allocator, const Operation* operation, const Operation* user, Operation* const* children, unsigned numberOfChildren, unsigned firstChildThatTargetAloneJudges, unsigned& fusedChildren, PlacedLeavesOfAllocatedOperation& placed)
{
    const ValueFacts* const facts = allocator.m_valueFacts;
    const std::optional<Failure>& failure = allocator.m_proc.failure();
    unsigned size = 0;
    for (unsigned index = 0; index < numberOfChildren; ++index) {
        Operation* leaf = children[index];
        const Operation* userOfLeaf = user;
        unsigned childIndex = index;
        bool targetAloneSaysWhatStandsInInstruction = index >= firstChildThatTargetAloneJudges;
        const uint16_t flags = facts[leaf->index()].flags;
        if (flags & Allocator::bitOf(ValueFlag::IsFused)) [[unlikely]] {
            const Operation* fused = leaf;
            fusedChildren |= 1u << index;
            if (firstChildThatTargetAloneJudges)
                firstChildThatTargetAloneJudges = numberOfChildren;
            const unsigned traitsOfFused = traitsOfAllocatedOpcodes.of[fused->opcode()];
            const Operation* selectAbove = user->opcode() == Select ? user : nullptr;
            Operation* onlyChild = nullptr;
            if (!selectAbove && !(traitsOfFused & (AllocatedOpcodeGoesTheLongWay | AllocatedOpcodeMayJumpUnconditionally)) && !(flags & flagsOfOperationWithLeavesOfItsOwn) && numberOfInlineChildrenOfAllocatedOperation(fused) == 1)
                onlyChild = inlineChildrenOfAllocatedOperation(fused)[0];
            if (!onlyChild || facts[onlyChild->index()].has(ValueFlag::IsFused)) {
                placed.size = size;
                placed.taken.operationNotSearchedForFusedLoads = operation;
                placeLeavesBelowOperationFusedIntoAllocatedOperation(allocator, fused, selectAbove, placed);
                if (failure) [[unlikely]]
                    return false;
                size = placed.size;
                continue;
            }
            if (isX86_64() && (traitsOfFused & AllocatedOpcodeIsLoad))
                placed.taken.hasMemoryOperand = true;
            leaf = onlyChild;
            userOfLeaf = fused;
            childIndex = 0;
            targetAloneSaysWhatStandsInInstruction = !(traitsOfFused & AllocatedOpcodeIsFusedIntoAddresses);
        }

        placed.leaves[size] = leaf;
        if (RegisterMask holding = leaf->hasNoPosition() ? 0 : allocator.stateOf(leaf).registers) [[likely]] {
            ASSERT(!(holding & ~allocator.m_file.allocatableIn(bankOf(leaf->type()))));
            Reg reg = Allocator::lowestRegisterIn(holding);
            placed.taken.locked |= maskOf(reg);
            placed.sources[size++] = Location::reg(reg);
            continue;
        }
        placed.sources[size++] = placeAllocatedLeafOutsideRegisters(allocator, userOfLeaf, childIndex, leaf, targetAloneSaysWhatStandsInInstruction, placed.taken, [&]() ALWAYS_INLINE_LAMBDA {
            if (!isX86_64())
                return;
            const Operation* notSearched = nullptr;
            for (unsigned later = index + 1; later < numberOfChildren; ++later) {
                if (facts[children[later]->index()].has(ValueFlag::IsFused))
                    notSearched = operation;
            }
            placed.taken.operationNotSearchedForFusedLoads = notSearched;
        });
        if (failure) [[unlikely]]
            return false;
    }
    RELEASE_ASSERT(size <= maxLeaves);
    placed.size = size;
    return true;
}

ALWAYS_INLINE bool Allocator::defineResultBehindLeaves(Operation* operation, unsigned traits, unsigned fusedChildren, RegisterMask operandRegisters, Location& firstSource, Location secondSource, Position position, Location& destination)
{
    const Type type = operation->type();
    if (type == Void)
        return true;

    const ValueFacts* const factsOfValues = m_valueFacts;
    const ValueFacts& facts = factsOfValues[operation->index()];
    bool overwritesFirstChild = false;
    bool maySwapChildren = false;
    RegisterMask bound;
    RegisterMask avoided;
    RegisterMask preferred = 0;
    if (!(traits & (AllocatedOpcodeOverwritesFirstChild | AllocatedOpcodeChangesWidthOfInteger))) {
        bound = m_bound;
        avoided = operandRegisters & bound;
        if (Reg hint = hintOf(operation)) {
            preferred = maskOf(hint) & ~bound;
            if (!preferred && (operandRegisters & maskOf(hint))) {
                Operation* held = m_valueIn[hint.index()];
                if (held && factsOfValues[held->index()].has(ValueFlag::IsRematerialized) && !isKeptThroughLoopAround(hint, held, position)) {
                    unbind(hint);
                    bound = m_bound;
                    avoided = operandRegisters & bound;
                    preferred = maskOf(hint);
                }
            }
        }
        if (!preferred && firstSource.isRegister())
            preferred = maskOf(firstSource.reg()) & ~bound;
    } else {
        const bool firstSourceIsRegister = firstSource.isRegister();
        const RegisterMask registerOfFirstSource = firstSourceIsRegister ? maskOf(firstSource.reg()) : 0;
        bool isFreeInRegisterOfFirstChild = false;
        if (firstSourceIsRegister && !(fusedChildren & 1)) {
            overwritesFirstChild = (traits & AllocatedOpcodeOverwritesFirstChild) && !(fusedChildren && addsScaledIndex(m_liveness, operation));
            maySwapChildren = overwritesFirstChild && (traits & AllocatedOpcodeIsCommutative) && !(fusedChildren & 2) && secondSource.isRegister();
            isFreeInRegisterOfFirstChild = (traits & AllocatedOpcodeChangesWidthOfInteger) && isFreeInRegisterOfChild(m_liveness, operation);
            if (overwritesFirstChild || isFreeInRegisterOfFirstChild) {
                Operation* first = m_valueIn[firstSource.reg().index()];
                if (first && factsOfValues[first->index()].has(ValueFlag::IsRematerialized) && !isKeptThroughLoopAround(firstSource.reg(), first, position))
                    unbind(firstSource.reg());
            }
        }

        bound = m_bound;
        avoided = operandRegisters & bound;
        if (overwritesFirstChild && !maySwapChildren)
            avoided |= operandRegisters & ~registerOfFirstSource;

        if (isFreeInRegisterOfFirstChild)
            preferred = registerOfFirstSource & ~bound;
        if (!preferred) {
            if (Reg hint = hintOf(operation))
                preferred = maskOf(hint) & ~bound;
        }
        if (!preferred)
            preferred = registerOfFirstSource & ~bound;
        if (!preferred && maySwapChildren)
            preferred = maskOf(secondSource.reg()) & ~bound;
    }

    const RegisterMask allocatable = m_file.allocatableIn(bankOf(type));
    RegisterMask freeRegisters = allocatable & ~avoided & ~bound;
    if (m_isAheadOfFramedPart || isInRareRegion()) [[unlikely]]
        freeRegisters &= ~m_file.calleeSaved | calleeSavesThatMayBeTaken();
    RegisterMask chosen = preferred & freeRegisters;
    if (!chosen) {
        RegisterMask addingNothingToFrame = freeRegisters & (~m_file.calleeSaved | m_allocation.usedCalleeSaves());
        if (RegisterMask savedByNoCallee = addingNothingToFrame & ~m_file.calleeSaved)
            addingNothingToFrame = savedByNoCallee;
        chosen = addingNothingToFrame & (~addingNothingToFrame + 1);
    }
    if (!chosen || (facts.flags & flagsOfAllocatedValueThatAsksForMoreThanRegister)) {
        destination = defineResult(operation, allocatable, avoided, preferred ? lowestRegisterIn(preferred) : Reg());
        if (m_proc.failure()) [[unlikely]]
            return false;
    } else {
        Reg reg = lowestRegisterIn(chosen);
        ASSERT(!m_valueIn[reg.index()]);
        m_allocation.m_registerAtDefinition[operation->index()] = reg;
        if (chosen & m_file.calleeSaved) [[unlikely]] {
            noteCalleeSaveUse(reg);
            noteFrameUse();
        }
        if (facts.lastUse != noPosition) {
            m_valueIn[reg.index()] = operation;
            m_bound = bound | chosen;
            stateOf(operation).registers |= chosen;
        }
        destination = Location::reg(reg);
    }

    if (overwritesFirstChild) {
        const Opcode opcode = operation->opcode();
        bool hasThreeAddressForm = type.isInt() && ((opcode == Add && (maySwapChildren || secondSource.isImmediate())) || (opcode == Mul && secondSource.isImmediate()));
        if (!hasThreeAddressForm && destination != firstSource && !(maySwapChildren && destination == secondSource)) {
            Step& move = appendStep(StepKind::Move, inlineChildrenOfAllocatedOperation(operation)[0]);
            move.destination = destination;
            move.sources[0] = firstSource;
            firstSource = destination;
        }
    }
    return true;
}

NEVER_INLINE void Allocator::allocateOperationWithAnyLeaves(Operation* operation)
{
    const Opcode opcode = operation->opcode();
    ASSERT(!operation->hasNoPosition() && opcode != Phi && opcode != Upsilon && opcode != Patch && opcode != Check);
    const unsigned traits = traitsOfAllocatedOpcodes.of[opcode];
    if (traits & AllocatedOpcodeGoesTheLongWay) [[unlikely]]
        return allocateOperationTheLongWay(operation);
    const ValueFacts& facts = m_valueFacts[operation->index()];
    const Position position = m_position;
    if (facts.flags & flagsOfOperationWithLeavesOfItsOwn) [[unlikely]]
        return allocateOperationTheLongWay(operation);
    if (position + 1 < m_lastDefinitionOfArgumentRegister) [[unlikely]]
        return allocateOperationTheLongWay(operation);
    if (m_registersReadOnWayToFailingSuccessor) [[unlikely]]
        return allocateOperationTheLongWay(operation);

    PlacedLeavesOfAllocatedOperation placed;
    placed.taken.locked = 0;
    placed.taken.hasMemoryOperand = false;
    const unsigned numberOfChildren = numberOfInlineChildrenOfAllocatedOperation(operation);
    unsigned fusedChildren = 0;
    if (!placeLeavesOfAllocatedOperation(*this, operation, operation, inlineChildrenOfAllocatedOperation(operation), numberOfChildren, (traits & AllocatedOpcodeIsFusedIntoAddresses) ? numberOfChildren - 1 : 0, fusedChildren, placed))
        return;
    for (unsigned index = 0, numberOfLeaves = placed.size; index < numberOfLeaves; ++index)
        passUseReachedByWalk(placed.leaves[index], position);

    Location destination;
    if (!defineResultBehindLeaves(operation, traits, fusedChildren, placed.taken.locked, placed.sources[0], placed.sources[1], position, destination))
        return;
    appendExecute(operation, destination, placed.sources);

    if (destination && facts.has(ValueFlag::NeedsPointerBesideIt)) [[unlikely]]
        keepPointerBesideAllocatedReferenceThatNeedsIt(*this, operation, noSpill);
}

template<unsigned numberOfLeaves, unsigned traitsOfKind>
ALWAYS_INLINE bool Allocator::allocateWithChildrenForLeaves(Operation* operation, const Operation* user, bool userIsFusedLoad)
{
    static_assert(numberOfLeaves == 1 || numberOfLeaves == 2);
    ASSERT(numberOfInlineChildrenOfAllocatedOperation(user) == numberOfLeaves);
    const ValueFacts* const factsOfValues = m_valueFacts;
    ValueState* const states = m_values.begin();
    Operation* const* const children = inlineChildrenOfAllocatedOperation(user);
    Operation* leaves[numberOfLeaves];
    uint32_t indexes[numberOfLeaves];
    unsigned flagsOfLeaves = 0;
    for (unsigned index = 0; index < numberOfLeaves; ++index) {
        leaves[index] = children[index];
        indexes[index] = leaves[index]->index();
        flagsOfLeaves |= factsOfValues[indexes[index]].flags;
    }
    if (flagsOfLeaves & bitOf(ValueFlag::IsFused)) [[unlikely]]
        return false;

    const Position position = m_position;
    RegisterMask locked = 0;
    bool hasMemoryOperand = isX86_64() && userIsFusedLoad;
    Location sources[numberOfLeaves];
    auto place = [&](unsigned index) ALWAYS_INLINE_LAMBDA {
        Operation* leaf = leaves[index];
        if (RegisterMask holding = leaf->hasNoPosition() ? 0 : states[indexes[index]].registers) [[likely]] {
            ASSERT(!(holding & ~m_file.allocatableIn(bankOf(leaf->type()))));
            Reg reg = lowestRegisterIn(holding);
            locked |= maskOf(reg);
            sources[index] = Location::reg(reg);
            return true;
        }
        PlacesTakenByAllocatedLeaves taken { locked, hasMemoryOperand, nullptr };
        sources[index] = placeAllocatedLeafOutsideRegisters(*this, user, index, leaf, true, taken, [] { });
        locked = taken.locked;
        hasMemoryOperand = taken.hasMemoryOperand;
        return !m_proc.failure();
    };
    if (!place(0)) [[unlikely]]
        return true;
    if constexpr (numberOfLeaves > 1) {
        if (!place(1)) [[unlikely]]
            return true;
    }

    passUseReachedByWalk(leaves[0], states[indexes[0]], factsOfValues[indexes[0]], position);
    if constexpr (numberOfLeaves > 1)
        passUseReachedByWalk(leaves[1], states[indexes[1]], factsOfValues[indexes[1]], position);

    const Location secondSource = numberOfLeaves > 1 ? sources[numberOfLeaves - 1] : Location();
    Location destination;
    if constexpr (!(traitsOfKind & kindOfAllocatedOperationHasNoResult)) {
        unsigned traits = traitsOfKind;
        if constexpr (numberOfLeaves > 1 && (traitsOfKind & AllocatedOpcodeOverwritesFirstChild))
            traits |= traitsOfAllocatedOpcodes.of[operation->opcode()] & AllocatedOpcodeIsCommutative;
        if (!defineResultBehindLeaves(operation, traits, 0, locked, sources[0], secondSource, position, destination))
            return true;
    }
    appendExecute(operation, destination, sources[0], secondSource);

    if constexpr (!(traitsOfKind & kindOfAllocatedOperationHasNoResult)) {
        if (destination && factsOfValues[operation->index()].has(ValueFlag::NeedsPointerBesideIt)) [[unlikely]]
            keepPointerBesideAllocatedReferenceThatNeedsIt(*this, operation, noSpill);
    }
    return true;
}

template<unsigned numberOfChildren, unsigned traitsOfKind>
NEVER_INLINE void Allocator::allocateOperationOfKind(Operation* operation)
{
    ASSERT((traitsOfAllocatedOpcodes.of[operation->opcode()] & (AllocatedOpcodeGoesTheLongWay | AllocatedOpcodeOverwritesFirstChild | AllocatedOpcodeChangesWidthOfInteger)) == traitsOfKind);
    if (m_valueFacts[operation->index()].flags & flagsOfOperationWithLeavesOfItsOwn) [[unlikely]]
        return allocateOperationWithAnyLeaves(operation);
    if (m_position + 1 < m_lastDefinitionOfArgumentRegister) [[unlikely]]
        return allocateOperationWithAnyLeaves(operation);
    if (m_registersReadOnWayToFailingSuccessor) [[unlikely]]
        return allocateOperationWithAnyLeaves(operation);
    if (numberOfInlineChildrenOfAllocatedOperation(operation) != numberOfChildren) [[unlikely]]
        return allocateOperationWithAnyLeaves(operation);
    if (!allocateWithChildrenForLeaves<numberOfChildren, traitsOfKind>(operation, operation, false)) [[unlikely]]
        return allocateOperationWithAnyLeaves(operation);
}

template<bool mayBePatch>
ALWAYS_INLINE void Allocator::allocateByKindOfOpcode(Operation* operation)
{
    ASSERT(!!operation->as<PatchOperation>() == (kindsOfAllocatedOpcodes.of[operation->opcode()] == KindOfAllocatedOpcode::IsPatchOrCheck));
    switch (kindsOfAllocatedOpcodes.of[operation->opcode()]) {
    case KindOfAllocatedOpcode::IsPatchOrCheck:
        if constexpr (mayBePatch)
            return allocatePatch(static_cast<PatchOperation*>(operation));
        return allocateOperationTheLongWay(operation);
    case KindOfAllocatedOpcode::GoesTheLongWay:
        return allocateOperationTheLongWay(operation);
    case KindOfAllocatedOpcode::IsArgumentRegister:
        return allocateArgumentRegister(operation);
    case KindOfAllocatedOpcode::HasAnyLeaves:
        return allocateOperationWithAnyLeaves(operation);
    case KindOfAllocatedOpcode::HasOneChild:
        return allocateOperationOfKind<1, 0>(operation);
    case KindOfAllocatedOpcode::HasOneChildAndChangesItsWidth:
        return allocateOperationOfKind<1, AllocatedOpcodeChangesWidthOfInteger>(operation);
    case KindOfAllocatedOpcode::HasTwoChildren:
        return allocateOperationOfKind<2, 0>(operation);
    case KindOfAllocatedOpcode::HasTwoChildrenAndOverwritesFirst:
        return allocateOperationOfKind<2, AllocatedOpcodeOverwritesFirstChild>(operation);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Allocator::allocateOperation(Operation* operation)
{
    ASSERT(!operation->hasNoPosition() && operation->opcode() != Phi && operation->opcode() != Upsilon && operation->opcode() != Patch && operation->opcode() != Check);
    allocateByKindOfOpcode<false>(operation);
}

NEVER_INLINE void Allocator::allocateBranch(Operation* branch)
{
    ASSERT(branch->opcode() == Branch && branch->numChildren() == 1 && branch->type() == Void && branch == m_block->last());
    const ValueFacts* const factsOfValues = m_valueFacts;
    ASSERT(m_position + 1 >= m_lastDefinitionOfArgumentRegister);
    if (factsOfValues[branch->index()].flags & flagsOfOperationWithLeavesOfItsOwn) [[unlikely]]
        return allocateOperationTheLongWay(branch);
    if (m_registersReadOnWayToFailingSuccessor) [[unlikely]]
        return allocateOperationTheLongWay(branch);
    if (branch->owner()->numSuccessors() == 1) [[unlikely]]
        return allocateBranchWithAnyLeaves(branch);

    const Operation* user = branch;
    unsigned numberOfLeaves = 1;
    bool userIsFusedLoad = false;
    const Operation* predicate = inlineChildrenOfAllocatedOperation(branch)[0];
    const uint16_t flagsOfPredicate = factsOfValues[predicate->index()].flags;
    if (flagsOfPredicate & bitOf(ValueFlag::IsFused)) {
        const unsigned traitsOfPredicate = traitsOfAllocatedOpcodes.of[predicate->opcode()];
        if ((traitsOfPredicate & (AllocatedOpcodeGoesTheLongWay | AllocatedOpcodeMayJumpUnconditionally | AllocatedOpcodeIsFusedIntoAddresses)) || (flagsOfPredicate & flagsOfOperationWithLeavesOfItsOwn)) [[unlikely]]
            return allocateBranchWithAnyLeaves(branch);
        user = predicate;
        numberOfLeaves = numberOfInlineChildrenOfAllocatedOperation(predicate);
        userIsFusedLoad = traitsOfPredicate & AllocatedOpcodeIsLoad;
    }
    if (numberOfLeaves == 2) {
        if (allocateWithChildrenForLeaves<2, kindOfAllocatedOperationHasNoResult>(branch, user, userIsFusedLoad)) [[likely]]
            return;
    } else if (numberOfLeaves == 1) {
        if (allocateWithChildrenForLeaves<1, kindOfAllocatedOperationHasNoResult>(branch, user, userIsFusedLoad)) [[likely]]
            return;
    }
    allocateBranchWithAnyLeaves(branch);
}

NEVER_INLINE void Allocator::allocateBranchWithAnyLeaves(Operation* branch)
{
    const ValueFacts* const factsOfValues = m_valueFacts;
    const Position position = m_position;
    PlacedLeavesOfAllocatedOperation placed;
    if (branch->owner()->numSuccessors() != 1) [[likely]] {
        placed.taken.locked = 0;
        placed.taken.hasMemoryOperand = false;
        const Operation* user = branch;
        Operation* const* children = inlineChildrenOfAllocatedOperation(branch);
        unsigned numberOfChildren = 1;
        unsigned firstChildThatTargetAloneJudges = 0;
        const Operation* predicate = children[0];
        const uint16_t flagsOfPredicate = factsOfValues[predicate->index()].flags;
        if (flagsOfPredicate & bitOf(ValueFlag::IsFused)) {
            const unsigned traitsOfPredicate = traitsOfAllocatedOpcodes.of[predicate->opcode()];
            if (!(traitsOfPredicate & (AllocatedOpcodeGoesTheLongWay | AllocatedOpcodeMayJumpUnconditionally)) && !(flagsOfPredicate & flagsOfOperationWithLeavesOfItsOwn)) [[likely]] {
                user = predicate;
                children = inlineChildrenOfAllocatedOperation(predicate);
                numberOfChildren = numberOfInlineChildrenOfAllocatedOperation(predicate);
                if (traitsOfPredicate & AllocatedOpcodeIsFusedIntoAddresses)
                    firstChildThatTargetAloneJudges = numberOfChildren;
                if (isX86_64() && (traitsOfPredicate & AllocatedOpcodeIsLoad))
                    placed.taken.hasMemoryOperand = true;
            }
        }
        unsigned fusedChildren = 0;
        if (!placeLeavesOfAllocatedOperation(*this, branch, user, children, numberOfChildren, firstChildThatTargetAloneJudges, fusedChildren, placed))
            return;
        for (unsigned index = 0, numberOfLeaves = placed.size; index < numberOfLeaves; ++index)
            passUseReachedByWalk(placed.leaves[index], position);
    }
    appendExecute(branch, Location(), placed.sources);
}

NEVER_INLINE void Allocator::allocateOperationTheLongWay(Operation* operation)
{
    Opcode opcode = operation->opcode();
    if (opcode == Nop)
        return;
    if (opcode == ArgumentReg) {
        allocateArgumentRegister(operation);
        return;
    }
    if (opcode == Extract) {
        allocateExtractBehindItsPatch(*this, operation);
        return;
    }
    RegisterMask reserved = registersReservedAtAllocatedOperation(*this);

    const ValueFacts* factsOfValues = m_valueFacts;
    const ValueFacts& facts = factsOfValues[operation->index()];
    bool goesByOverflow = facts.has(ValueFlag::GoesByOverflow);
    LeavesOfAllocatedOperation leaves;
    leaves.size = 0;
    leaves.hasFusedLoad = false;
    bool hasMemoryOperand;
    if (facts.flags & flagsOfOperationWithLeavesOfItsOwn) {
        hasMemoryOperand = isX86_64() && (facts.has(ValueFlag::WorksInMemory) || (goesByOverflow && opcode == Branch) || allocatedOperationHasFusedLoad(m_liveness, operation));
        forEachLeaf(m_liveness, operation, [&](const Operation* user, unsigned childIndex, Operation* leaf) {
            leaves.append(user, childIndex, leaf);
        });
    } else {
        if (!operation->isUnconditionalJump())
            listLeavesAmongChildrenOfAllocatedOperation(m_liveness, factsOfValues, operation, nullptr, leaves);
        hasMemoryOperand = isX86_64() && leaves.hasFusedLoad;
    }

    Location sources[maxLeaves];
    const unsigned numberOfLeaves = leaves.size;
    RegisterMask locked = reserved;
    for (unsigned index = 0; index < numberOfLeaves; ++index)
        sources[index] = placeLeafOfAllocatedOperation(*this, leaves.users[index], leaves.childIndexes[index], leaves.leaves[index], locked, hasMemoryOperand);
    if (m_proc.failure()) [[unlikely]]
        return;
    const Position position = m_position;
    for (unsigned index = 0; index < numberOfLeaves; ++index)
        passUseReachedByWalk(leaves.leaves[index], position);

    RegisterMask operandRegisters = locked;
    uint32_t scratchAssignment = UINT32_MAX;
    RegisterMask scratch = 0;
    ASSERT(allocatedOperationMayTakeScratchRegisters(opcode) || !numberOfScratchGPRsOf(m_proc.traits(), *operation));
    if (allocatedOperationMayTakeScratchRegisters(opcode)) {
        if (unsigned numberOfScratchRegisters = numberOfScratchGPRsOf(m_proc.traits(), *operation)) {
            scratch = takeScratchRegisters(Bank::GP, numberOfScratchRegisters, locked);
            if (m_proc.failure())
                return;
            PatchAssignment assignment;
            assignment.gpScratch = scratch;
            scratchAssignment = m_allocation.m_patchAssignments.size();
            m_allocation.m_patchAssignments.append(m_arena, assignment);
        }
    }

    Location destination;
    if (operation->type() != Void) {
        auto isFused = [&](const Operation* child) ALWAYS_INLINE_LAMBDA {
            return factsOfValues[child->index()].has(ValueFlag::IsFused);
        };
        bool overwritesFirstChild = resultPrefersRegisterOfFirstChild() && resultOverwritesFirstChild(opcode)
            && !isFused(operation->child(0)) && sources[0].isRegister() && !addsScaledIndex(m_liveness, operation);
        bool maySwapChildren = overwritesFirstChild && isCommutative(opcode) && !isFused(operation->child(1)) && sources[1].isRegister();
        bool isFreeInRegisterOfFirstChild = isFreeInRegisterOfChild(m_liveness, operation) && !isFused(operation->child(0)) && sources[0].isRegister();
        if (overwritesFirstChild || isFreeInRegisterOfFirstChild) {
            Operation* first = m_valueIn[sources[0].reg().index()];
            if (first && factsOfValues[first->index()].has(ValueFlag::IsRematerialized) && !isKeptThroughLoopAround(sources[0].reg(), first, m_position))
                unbind(sources[0].reg());
        }

        OperandTakenBack operandTakenBack;
        if (goesByOverflow) {
            operandTakenBack = findOperandThatSumMayOverwrite(operation, std::span<const Location>(sources, numberOfLeaves));
            if (operandTakenBack.value)
                unbind(operandTakenBack.reg);
        }

        RegisterMask avoided = reserved | (operandRegisters & m_bound) | scratch;
        if (overwritesFirstChild && !maySwapChildren)
            avoided |= operandRegisters & ~maskOf(sources[0].reg());

        Reg preferred = operandTakenBack.reg;
        if (!preferred && isFreeInRegisterOfFirstChild)
            preferred = hintIfAmong(sources[0].reg(), ~m_bound);
        if (!preferred)
            preferred = hintIfAmong(hintOf(operation), ~m_bound);
        if (!preferred && sources[0].isRegister())
            preferred = hintIfAmong(sources[0].reg(), ~m_bound);
        if (!preferred && maySwapChildren)
            preferred = hintIfAmong(sources[1].reg(), ~m_bound);

        destination = defineResultWithFacts(operation, facts, m_file.allocatableIn(bankOf(operation->type())), avoided, preferred);
        if (m_proc.failure()) [[unlikely]]
            return;
        if (operandTakenBack.value) {
            if (destination == Location::reg(operandTakenBack.reg))
                noteOperandTakenBack(operation, operandTakenBack, sources[sources[0] == destination ? 1 : 0]);
            else
                bind(operandTakenBack.reg, operandTakenBack.value);
        }

        bool hasThreeAddressForm = goesByOverflow || (operation->type().isInt()
            && ((opcode == Add && (maySwapChildren || sources[1].isImmediate())) || (opcode == Mul && sources[1].isImmediate())));
        if (overwritesFirstChild && !hasThreeAddressForm && destination != sources[0] && !(maySwapChildren && destination == sources[1])) {
            Step& move = appendStep(StepKind::Move, operation->child(0));
            move.destination = destination;
            move.sources[0] = sources[0];
            sources[0] = destination;
        }
    }

    Step step;
    step.kind = StepKind::Execute;
    step.patchAssignment = scratchAssignment;
    step.destination = destination;
    std::copy_n(sources, maxLeaves, step.sources);
    step.operation = operation;
    if (goesByOverflow && opcode == Branch)
        step.other = m_operandTakenBackByBranch;
    appendStep(step);

    if (destination)
        keepPointerBesideAllocatedReference(*this, operation);
}

void Allocator::allocateTerminator(Block* block, Operation* operation)
{
    Opcode opcode = operation->opcode();
    if (opcode == Jump) {
        appendStep(StepKind::Execute, operation);
    } else if (opcode == Return && operation->numChildren()) {
        if (operation->numChildren() > 1) {
            m_proc.fail(FailureKind::MalformedProcedure, operation);
            return;
        }
        Operation* value = operation->child(0);
        Reg target = bankOf(value->type()) == Bank::GP ? Reg(GPRInfo::returnValueGPR) : Reg(FPRInfo::returnValueFPR);
        if (Operation* held = m_valueIn[target.index()]; held && held != value)
            unbind(target);
        if (ensureInRegister(value, maskOf(target), target) != target) {
            m_proc.fail(FailureKind::OutOfRegisters, operation);
            return;
        }
        passUseReachedByWalk(value, m_position);
        appendStep(StepKind::Execute, operation).sources[0] = Location::reg(target);
    } else
        allocateOperation(operation);
    if (m_proc.failure()) [[unlikely]]
        return;

    BlockSteps& steps = m_allocation.m_blocks[block->index()];
    steps.terminator = m_allocation.m_steps.size() - 1;
    steps.end = steps.terminator;
}

Location Allocator::placeLeaf(const Operation* user, unsigned childIndex, Operation* leaf, RegisterMask& locked, bool& hasMemoryOperand)
{
    return placeLeafOfAllocatedOperation(*this, user, childIndex, leaf, locked, hasMemoryOperand);
}

static NEVER_INLINE void passUsesOfLeavesBelowAllocatedOperation(Allocator& allocator, const Operation* below, const Operation* selectAbove, Position position)
{
    forEachLeafBelow(allocator.m_liveness, below, [&](const Operation*, unsigned, Operation* leaf) {
        allocator.passUse(leaf, position);
    }, selectAbove);
}

void Allocator::passUsesOf(const Operation* operation, Position position)
{
    position = std::min(position, m_position + 1);
    forEachLeafInParts(m_liveness, operation, [&](unsigned, Operation* leaf) ALWAYS_INLINE_LAMBDA {
        passUseReachedByWalk(leaf, position);
    }, [&](const Operation* below, const Operation* selectAbove) ALWAYS_INLINE_LAMBDA {
        passUsesOfLeavesBelowAllocatedOperation(*this, below, selectAbove, position);
    });
}

void Allocator::passUse(Operation* value, Position position)
{
    passUseReachedByWalk(value, std::min(position, m_position + 1));
}

Location Allocator::defineResult(Operation* operation, RegisterMask allowed, RegisterMask avoided, Reg preferred)
{
    return defineResultWithFacts(operation, m_valueFacts[operation->index()], allowed, avoided, preferred);
}

void Allocator::enterFramedPart()
{
    for (ReferenceWhosePointerWaits& waiting : m_referencesWhosePointersWaitForFramedPart) {
        Operation* reference = waiting.reference;
        if (stateOf(reference).registers || (m_allocation.isSpilled(reference) && isLiveBehind(reference, m_position) && !isDeadOnEveryPathFrom(reference, m_block, m_headOfBlock)))
            waiting.pointer = keepPointerBesideAllocatedReference(*this, reference, waiting.pointer);
    }

    forEachRegister(m_bound & ~m_file.calleeSaved, [&](Reg reg) {
        Operation* value = m_valueIn[reg.index()];
        if (!value || !value->type().isNumeric() || (stateOf(value).registers & m_file.calleeSaved))
            return;
        const ValueFacts& facts = m_liveness.of(value);
        if (!facts.has(ValueFlag::LivesAcrossCall) || facts.has(ValueFlag::IsRematerialized))
            return;
        if (!callStandsOnWayFromHeadToRead(*this, value))
            return;
        Bank bank = bankOf(reg);
        RegisterMask freeRegisters = calleeSavesWorthTakingFor(value, bank, m_block) & ~m_bound;
        if (!freeRegisters)
            return;
        Reg target = takeRegister(bank, freeRegisters, hintIfAmong(hintOf(value), freeRegisters));
        Step& move = appendStep(StepKind::Move, value);
        move.destination = Location::reg(target);
        move.sources[0] = Location::reg(reg);
        unbindAll(value);
        bind(target, value);
    });
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
