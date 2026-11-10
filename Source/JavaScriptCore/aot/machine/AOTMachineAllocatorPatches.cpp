/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineAllocator.h"

#if ENABLE(AOT)

#include "AOTMachineCallsOnWays.h"
#include <algorithm>
#include <bit>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

static unsigned firstPlacedChildOfPatch(const PatchOperation* patch)
{
    return patch->opcode() == Check ? 1 : 0;
}

static ALWAYS_INLINE ValueRep::Kind placedKindOfPatchOperandUnder(ValueRep::Kind kind, bool onlyKeepsChildrenAlive, bool isCheck)
{
    if (onlyKeepsChildrenAlive || (kind == ValueRep::WarmAny && isCheck))
        return ValueRep::ColdAny;
    return kind;
}

static ValueRep::Kind placedKindOfPatchOperand(const PatchOperation* patch, unsigned child)
{
    return placedKindOfPatchOperandUnder(patch->constraintOf(child).kind(), patch->onlyKeepsChildrenAlive, patch->opcode() == Check);
}

static bool patchOperandMayLieAnywhere(ValueRep::Kind kind)
{
    return kind == ValueRep::WarmAny || kind == ValueRep::ColdAny || kind == ValueRep::LateColdAny;
}

static bool patchOperandIsReadLate(ValueRep::Kind kind)
{
    return kind == ValueRep::LateColdAny || kind == ValueRep::SomeLateRegister;
}

static ValueRep placedConstraintOfPatchResult(const PatchOperation* patch, unsigned index)
{
    if (index < patch->resultConstraints.size())
        return patch->resultConstraints[index];
    return ValueRep::SomeRegister;
}

static RegisterMask registersOfFixedPatchResults(const PatchOperation* patch)
{
    RegisterMask registers = 0;
    for (const ValueRep& constraint : patch->resultConstraints.span()) {
        if (constraint.isReg())
            registers |= maskOf(constraint.reg());
    }
    return registers;
}

[[maybe_unused]] static Position earlyPositionOfAllocatedPatch(const Allocator& allocator, const PatchOperation* patch)
{
    if (allocator.m_liveness.isSunk(patch))
        return allocator.position();
    return allocator.m_order.earlyPositionOf(patch);
}

static bool patchStackWordLiesInOutgoingArea(const Procedure& procedure, const ValueRep& constraint)
{
    ValueRep::OffsetType offset = constraint.offsetFromSP();
    return offset >= 0 && static_cast<uint64_t>(offset) + sizeof(uint64_t) <= procedure.callArgAreaSizeInBytes();
}

static Reg hintOfPatchResultAmong(Reg hint, RegisterMask allowed)
{
    return hint && (maskOf(hint) & allowed) ? hint : Reg();
}

static Reg takeRegisterForPatchResult(Allocator& allocator, Bank bank, RegisterMask allowed, const Operation* value)
{
    if (!allowed)
        return Reg();
    const ValueFacts* facts = value ? &allocator.m_liveness.of(value) : nullptr;
    Reg hint = value ? allocator.hintOf(value) : Reg();
    RegisterMask keptByCallees = facts && facts->has(ValueFlag::LivesAcrossCall) ? allowed & allocator.calleeSavesWorthTakingFor(value, bank, allocator.m_block) : 0;
    if (facts && facts->has(ValueFlag::LivesAcrossCall) && allocator.hasFreeRegister(bank, keptByCallees) && callStandsOnWayFromDefinitionToRead(allocator, value))
        return allocator.takeRegister(bank, keptByCallees, hintOfPatchResultAmong(hint, keptByCallees));
    return allocator.takeRegister(bank, allowed, hintOfPatchResultAmong(hint, allowed));
}

static void noteRegisterTakenForPatch(Allocator& allocator, Reg reg)
{
    if (maskOf(reg) & allocator.m_file.calleeSaved)
        allocator.noteCalleeSaveUse(reg);
}

static ALWAYS_INLINE void appendMoveForPatchAllocation(Allocator& allocator, StepKind kind, Operation* value, Location destination, Location source)
{
    Step step;
    step.kind = kind;
    step.width = widthOf(value->type());
    step.destination = destination;
    step.sources[0] = source;
    step.operation = value;
    allocator.appendStep(step);
}

static ALWAYS_INLINE bool spillHoldsValueWherePatchStands(const Allocator& allocator, const Operation* value)
{
    bool holdsValue = allocator.m_allocation.isSpilled(value) && (!allocator.m_isOutsideFramedPart || allocator.spillHoldsValueIn(allocator.m_block, value));
    ASSERT(holdsValue == allocator.spillHoldsValueIn(allocator.m_block, value));
    return holdsValue;
}

static bool liesInMemoryForPatchAllocation(Allocator& allocator, const Operation* value)
{
    if (allocator.m_allocation.isSpilled(value) && !allocator.stateOf(value).registers)
        return true;
    return spillHoldsValueWherePatchStands(allocator, value) || allocator.stateOf(value).saveInRareRegion != noSpill;
}

static Location memoryHomeOfPatchOperand(Allocator& allocator, Operation* value)
{
    if (spillHoldsValueWherePatchStands(allocator, value)) {
        allocator.noteFrameUse();
        return allocator.placeToReadIn(allocator.m_allocation.spillOf(value));
    }

    if (!liesInMemoryForPatchAllocation(allocator, value)) {
        if (allocator.isInRareRegion() && allocator.isDefinedOutsideRareRegion(value))
            allocator.saveInRareRegion(value, allocator.anyRegisterOf(value, allocator.stateOf(value).registers));
        else
            allocator.markSpilled(value);
    } else if (allocator.stateOf(value).registers) {
        SpillIndex save = allocator.stateOf(value).saveInRareRegion;
        if (allocator.m_allocation.spills()[save].begin < allocator.m_order.headOf(allocator.m_block))
            appendMoveForPatchAllocation(allocator, StepKind::Save, value, Location::spill(save), Location::reg(allocator.anyRegisterOf(value, allocator.stateOf(value).registers)));
    }
    allocator.noteFrameUse();
    return allocator.memoryLocationOf(value);
}

static ALWAYS_INLINE Location memoryHomeOfPatchOperandInLine(Allocator& allocator, Operation* value)
{
    if (spillHoldsValueWherePatchStands(allocator, value)) [[likely]] {
        allocator.noteFrameUse();
        return allocator.placeToReadIn(allocator.m_allocation.spillOf(value));
    }
    return memoryHomeOfPatchOperand(allocator, value);
}

static ALWAYS_INLINE bool patchOperandTakesRegister(ValueRep::Kind placedKind, const Operation* value, bool onlyKeepsChildrenAlive)
{
    if (!patchOperandMayLieAnywhere(placedKind))
        return true;
    return value->hasNoPosition() && !value->isConstant() && !onlyKeepsChildrenAlive;
}

static unsigned numberOfRegistersPatchTakesBesideOperand(const Procedure& procedure, const PatchOperation* patch, Bank bank, bool operandIsReadLate)
{
    unsigned number = bank == Bank::GP ? patch->numGPScratchRegisters : patch->numFPScratchRegisters;
    unsigned numberOfResults = procedure.resultCount(patch->type());
    for (unsigned index = 0; index < numberOfResults; ++index) {
        if (bankOf(procedure.resultType(patch->type(), index)) != bank)
            continue;
        ValueRep::Kind kind = placedConstraintOfPatchResult(patch, index).kind();
        bool avoidsLateOperands = kind != ValueRep::Register && (kind != ValueRep::StackArgument || patch->type().isTuple());
        if (kind == ValueRep::SomeEarlyRegister || (operandIsReadLate && avoidsLateOperands))
            ++number;
    }
    return number;
}

static bool patchKeepsRegistersBesideOperand(const Allocator& allocator, const PatchOperation* patch, Bank bank, RegisterMask locked, unsigned numberStillToLock, bool operandIsReadLate)
{
    unsigned numberTaken = numberStillToLock + numberOfRegistersPatchTakesBesideOperand(allocator.m_proc, patch, bank, operandIsReadLate);
    if (!numberTaken)
        return true;
    RegisterMask registers = allocator.m_file.allocatableIn(bank) & (~allocator.m_file.calleeSaved | allocator.calleeSavesThatMayBeTaken());
    RegisterMask unavailable = locked | patch->earlyClobberedMask() | patch->lateClobberedMask() | registersOfFixedPatchResults(patch);
    return static_cast<unsigned>(std::popcount(registers & ~unavailable)) >= numberTaken;
}

static ALWAYS_INLINE void bindFreeRegisterNotedForPatch(Allocator& allocator, Reg reg, Operation* value, Allocator::ValueState& state)
{
    RegisterMask bit = maskOf(reg);
    ASSERT(!allocator.m_valueIn[reg.index()] && !(allocator.m_bound & bit));
    ASSERT(&allocator.stateOf(value) == &state);
    ASSERT(!(bit & allocator.m_file.calleeSaved & ~allocator.m_allocation.usedCalleeSaves()));
    allocator.m_valueIn[reg.index()] = value;
    allocator.m_bound |= bit;
    state.registers |= bit;
}

static NEVER_INLINE RegisterMask moveValueOutOfRegistersVacatedForPatch(Allocator& allocator, Reg reg, Operation* value, RegisterMask held, RegisterMask unavailable, bool spillHoldsValue)
{
    Bank bank = bankOf(reg);
    RegisterMask allowed = allocator.m_file.allocatableIn(bank) & ~unavailable;
    Allocator::ValueState& state = allocator.stateOf(value);
    ASSERT(held && held == state.registers);
    bool staysReachable = value->hasNoPosition() || spillHoldsValue || state.saveInRareRegion != noSpill;
    ASSERT(staysReachable == (value->hasNoPosition() || liesInMemoryForPatchAllocation(allocator, value)));
    Reg target;
    RegisterMask registersThatStayTaken = 0;
    if (!staysReachable && allowed) {
        allowed &= ~allocator.m_file.calleeSaved | allocator.calleeSavesWorthTakingFor(value, bank, allocator.m_block);
        if (allowed) {
            const RegisterMask candidates = allocator.registersThatMayBeTaken(bank, allowed);
            const RegisterMask freeRegisters = candidates & ~allocator.m_bound;
            if (freeRegisters || allocator.canDropSomeRegisterForFree(candidates)) {
                const Reg preferred = hintOfPatchResultAmong(allocator.hintOf(value), allowed);
                target = allocator.chooseFreeRegisterWithoutNewCalleeSave(freeRegisters, preferred);
                if (target)
                    allocator.noteCalleeSaveUse(target);
                else
                    target = allocator.takeRegisterAfterSearch(bank, allowed, preferred);
            } else
                registersThatStayTaken = candidates;
        }
    }
    if (!target) {
        allocator.evict(reg);
        return registersThatStayTaken;
    }

    appendMoveForPatchAllocation(allocator, StepKind::Move, value, Location::reg(target), Location::reg(reg));
    bindFreeRegisterNotedForPatch(allocator, target, value, state);
    for (RegisterMask holders = held; holders; holders &= holders - 1)
        allocator.m_valueIn[std::countr_zero(holders)] = nullptr;
    allocator.m_bound &= ~held;
    state.registers &= ~held;
    return 0;
}

static ALWAYS_INLINE void vacateRegistersForPatchInLine(Allocator& allocator, RegisterMask registers, Position behind, RegisterMask unavailable)
{
    unavailable |= registers;
    for (RegisterMask remaining = registers & allocator.m_bound; remaining; remaining &= remaining - 1) {
        Reg reg = Allocator::lowestRegisterIn(remaining);
        Operation* value = allocator.m_valueIn[reg.index()];
        if (!value)
            continue;
        RegisterMask held = allocator.stateOf(value).registers;
        if (!allocator.isLiveBehind(value, behind) || (held & ~registers)) {
            allocator.unbind(reg);
            continue;
        }
        const bool isNumeric = value->type().isNumeric();
        if (isNumeric && value->hasNoPosition() && allocator.m_valueFacts[value->index()].has(ValueFlag::IsRematerialized)) {
            allocator.unbind(reg);
            continue;
        }
        const bool spillHoldsValue = spillHoldsValueWherePatchStands(allocator, value);
        if (isNumeric && spillHoldsValue) {
            allocator.unbind(reg);
            continue;
        }
        unavailable |= moveValueOutOfRegistersVacatedForPatch(allocator, reg, value, held, unavailable, spillHoldsValue);
    }
}

static NEVER_INLINE void vacateRegistersForPatch(Allocator& allocator, RegisterMask registers, Position behind, RegisterMask unavailable)
{
    vacateRegistersForPatchInLine(allocator, registers, behind, unavailable);
}

template<typename Functor>
static void forEachExtractBehindPatch(const Allocator& allocator, const PatchOperation* patch, const Functor& functor)
{
    const Block* block = patch->owner();
    unsigned index = patch->indexInBlock() + 1;
    if (patch->endsBlock()) {
        if (!block->numSuccessors())
            return;
        block = block->successor(0).block();
        index = 0;
    }
    for (; index < block->size(); ++index) {
        Operation* operation = block->at(index);
        if (!allocator.m_liveness.standsInItsBlock(operation))
            continue;
        if (operation->opcode() != Extract || operation->child(0) != patch)
            return;
        functor(operation);
    }
}

static Operation* allocatedValueOfPatchResult(const Allocator& allocator, PatchOperation* patch, unsigned index)
{
    if (!patch->type().isTuple())
        return patch;
    Operation* value = nullptr;
    forEachExtractBehindPatch(allocator, patch, [&](Operation* extract) {
        if (!value && extract->extractedIndex() == index)
            value = extract;
    });
    return value;
}

static bool extractsStandBehindPatch(const Allocator& allocator, PatchOperation* patch)
{
    const Liveness& liveness = allocator.m_liveness;
    unsigned numberOfReads = 0;
    for (uint32_t use = liveness.of(patch).firstUse; use; use = liveness.use(use).next)
        ++numberOfReads;

    unsigned numberFound = 0;
    bool eachMemberIsExtractedOnce = true;
    forEachExtractBehindPatch(allocator, patch, [&](Operation* extract) {
        ++numberFound;
        eachMemberIsExtractedOnce &= allocatedValueOfPatchResult(allocator, patch, extract->extractedIndex()) == extract;
    });
    if (numberOfReads > numberFound || !eachMemberIsExtractedOnce)
        return false;
    if (!numberFound || !patch->endsBlock())
        return true;
    const Block* successor = patch->owner()->successor(0).block();
    return allocator.m_order.of(successor).predecessors.size() == 1 && liveness.sunkInto(successor).empty();
}

static ALWAYS_INLINE bool someSuccessorIsNoLandingPad(const Block* block)
{
    for (const Edge& edge : block->successors()) {
        if (!edge.block()->isLandingPad())
            return true;
    }
    return false;
}

static bool registersMatterBehindPatch(const PatchOperation* patch)
{
    return !patch->endsBlock() || someSuccessorIsNoLandingPad(patch->owner());
}

static bool isReadOnlyByFixedPatchOperands(const Allocator& allocator, const PatchOperation* patch, const Operation* value, Position late)
{
    if (patch->opcode() == Check || (registersMatterBehindPatch(patch) && allocator.isLiveBehind(value, late)))
        return false;
    std::span<Operation* const> operands = patch->operands();
    for (unsigned child = 0; child < operands.size(); ++child) {
        if (operands[child] != value)
            continue;
        ValueRep::Kind kind = placedKindOfPatchOperand(patch, child);
        if (kind != ValueRep::Register && kind != ValueRep::StackArgument)
            return false;
    }
    return true;
}

static ALWAYS_INLINE Location sourceOfFixedPatchOperand(Allocator& allocator, Operation* value, RegisterMask wanted)
{
    if (RegisterMask held = allocator.stateOf(value).registers)
        return Location::reg(allocator.anyRegisterOf(value, (held & ~wanted) ? (held & ~wanted) : held));
    if (value->hasNoPosition())
        return Location::immediate();
    allocator.noteFrameUse();
    if (spillHoldsValueWherePatchStands(allocator, value)) [[likely]]
        return allocator.placeToReadIn(allocator.m_allocation.spillOf(value));
    return allocator.memoryLocationOf(value);
}

static bool patchResultBorrowsRegister(const Allocator& allocator, const Operation* value, Location definedIn)
{
    return value->opcode() == Extract && definedIn.kind() == Location::Outgoing && allocator.m_liveness.of(value).lastUse != noPosition && !allocator.m_allocation.registerAtDefinitionOf(value);
}

static void appendStepsBehindPatchResult(Allocator& allocator, Operation* value, Location definedIn, Reg borrowed = Reg())
{
    const ValueFacts& facts = allocator.m_liveness.of(value);
    if (facts.lastUse == noPosition)
        return;
    const bool isMember = value->opcode() == Extract;

    if (definedIn.kind() == Location::Outgoing && !allocator.stateOf(value).registers) {
        Bank bank = bankOf(value->type());
        Reg reg = borrowed;
        if (!reg)
            reg = isMember ? allocator.m_allocation.registerAtDefinitionOf(value) : takeRegisterForPatchResult(allocator, bank, allocator.m_file.allocatableIn(bank), value);
        if (allocator.m_bound & maskOf(reg))
            allocator.evict(reg);
        appendMoveForPatchAllocation(allocator, StepKind::Reload, value, Location::reg(reg), definedIn);
        allocator.bind(reg, value);
        SpillIndex spill = allocator.markSpilled(value);
        if (!isMember || borrowed) {
            appendMoveForPatchAllocation(allocator, StepKind::Save, value, Location::spill(spill), Location::reg(reg));
            allocator.m_spillsWrittenInBlocks.append(allocator.m_arena, Allocator::SpillWrittenInBlock { spill, allocator.m_block });
        }
        allocator.noteFrameUse();
    }

    if (facts.has(ValueFlag::NeedsPointerBesideIt) && !allocator.pointerWaitsForFramedPart(value)) {
        Reg reg = allocator.ensureInRegister(value, allocator.m_file.allocatableIn(Bank::GP));
        Position begin = isMember ? allocator.m_liveness.of(value->child(0)).definition : facts.definition;
        SpillIndex pointer = allocator.addSpill(SpillFacts { .value = value, .begin = begin, .end = facts.lastUse, .width = Width64, .isPointerBesideNarrowReference = true });
        Step& step = allocator.appendStep(StepKind::KeepPointer, value);
        step.width = Width64;
        step.destination = Location::spill(pointer);
        step.sources[0] = Location::reg(reg);
        allocator.noteFrameUse();
    }

    if (borrowed)
        allocator.unbind(borrowed);
}

static NEVER_INLINE void appendStepsBehindMembersOfPatchResult(Allocator& allocator, PatchOperation* patch, std::span<const Location> locationsOfResults)
{
    Reg registerToBorrow[numberOfBanks];
    bool someMemberBorrows = false;
    for (unsigned index = 0; index < locationsOfResults.size(); ++index) {
        if (locationsOfResults[index].kind() != Location::Outgoing)
            continue;
        Operation* value = allocatedValueOfPatchResult(allocator, patch, index);
        if (!value || value == patch || allocator.m_liveness.of(value).lastUse == noPosition)
            continue;
        if (Reg reg = allocator.m_allocation.registerAtDefinitionOf(value))
            registerToBorrow[static_cast<unsigned>(bankOf(value->type()))] = reg;
        else
            someMemberBorrows = true;
    }

    for (unsigned index = 0; someMemberBorrows && index < locationsOfResults.size(); ++index) {
        Operation* value = allocatedValueOfPatchResult(allocator, patch, index);
        if (value && patchResultBorrowsRegister(allocator, value, locationsOfResults[index]))
            appendStepsBehindPatchResult(allocator, value, locationsOfResults[index], registerToBorrow[static_cast<unsigned>(bankOf(value->type()))]);
    }
    for (unsigned index = 0; index < locationsOfResults.size(); ++index) {
        Operation* value = allocatedValueOfPatchResult(allocator, patch, index);
        if (value && !patchResultBorrowsRegister(allocator, value, locationsOfResults[index]))
            appendStepsBehindPatchResult(allocator, value, locationsOfResults[index]);
    }
}

static ALWAYS_INLINE void appendStepsBehindPatchResults(Allocator& allocator, PatchOperation* patch, std::span<const Location> locationsOfResults)
{
    if (patch->type().isTuple()) {
        appendStepsBehindMembersOfPatchResult(allocator, patch, locationsOfResults);
        return;
    }
    if (locationsOfResults.empty())
        return;
    const ValueFacts& facts = allocator.m_liveness.of(patch);
    if (facts.lastUse != noPosition && (locationsOfResults[0].kind() == Location::Outgoing || facts.has(ValueFlag::NeedsPointerBesideIt)))
        appendStepsBehindPatchResult(allocator, patch, locationsOfResults[0]);
}

static NEVER_INLINE void finishResultsOfPatchThatEndedBlock(Allocator& allocator, PatchOperation* patch, const Block* predecessor)
{
    unsigned numberOfResults = allocator.m_proc.resultCount(patch->type());
    uint32_t terminator = allocator.m_allocation.stepsOf(predecessor).terminator;
    if (!numberOfResults || terminator == UINT32_MAX)
        return;

    std::span<const Location> locations = allocator.m_allocation.patchAssignment(allocator.m_allocation.step(terminator).patchAssignment).locations;
    appendStepsBehindPatchResults(allocator, patch, locations.first(numberOfResults));
}

void finishResultsOfPatchThatEndedPredecessor(Allocator& allocator, Block* block)
{
    std::span<Block* const> predecessors = allocator.m_order.of(block).predecessors;
    if (predecessors.size() != 1)
        return;
    Block* predecessor = predecessors[0];
    Operation* last = predecessor->last();
    if (last->opcode() != Patch || last->type() == Void)
        return;
    if (!last->endsBlock() || predecessor->successor(0).block() != block)
        return;
    PatchOperation* patch = static_cast<PatchOperation*>(last);
    if (!patch->type().isTuple()) {
        const ValueFacts& facts = allocator.m_valueFacts[patch->index()];
        if (facts.lastUse == noPosition || (!facts.has(ValueFlag::NeedsPointerBesideIt) && placedConstraintOfPatchResult(patch, 0).kind() != ValueRep::StackArgument))
            return;
    }
    finishResultsOfPatchThatEndedBlock(allocator, patch, predecessor);
}

static NEVER_INLINE void keepValuesHandedOnBehindPatchInMemory(Allocator& allocator, const Block* block, Position late)
{
    for (Operation* upsilon : block->upsilons()) {
        if (allocator.m_liveness.isDropped(upsilon) || allocator.m_liveness.isDropped(upsilon->phi()))
            continue;
        Operation* handedOn = upsilon->child(0);
        if (!handedOn->hasNoPosition() && allocator.stateOf(handedOn).registers && !allocator.isLiveBehind(handedOn, late))
            memoryHomeOfPatchOperand(allocator, handedOn);
    }
}

static NEVER_INLINE unsigned placeLeavesOfCheckPredicate(Allocator& allocator, PatchOperation* check, Location* sources, RegisterMask& locked)
{
    unsigned numberOfSources = 0;
    bool hasMemoryOperand = false;
    auto placeLeafOfPredicate = [&](const Operation* user, unsigned childIndex, Operation* leaf) {
        if (numberOfSources == maxLeaves) {
            allocator.m_proc.fail(FailureKind::MalformedProcedure, check);
            return;
        }
        sources[numberOfSources++] = allocator.placeLeaf(user, childIndex, leaf, locked, hasMemoryOperand);
    };
    Operation* predicate = check->child(0);
    if (allocator.m_liveness.isFused(predicate))
        forEachLeaf(allocator.m_liveness, predicate, placeLeafOfPredicate);
    else
        placeLeafOfPredicate(check, 0, predicate);
    return numberOfSources;
}

static void placeOperandOfPatchUnder(Allocator& allocator, PatchOperation* patch, ValueRep::Kind placedKind, Operation* value, Location& location, RegisterMask& locked)
{
    bool needsRegister = false;
    bool isWarm = false;
    bool isLate = false;
    switch (placedKind) {
    case ValueRep::WarmAny:
        isWarm = true;
        break;
    case ValueRep::ColdAny:
        break;
    case ValueRep::LateColdAny:
        isLate = true;
        break;
    case ValueRep::SomeRegister:
        needsRegister = true;
        break;
    case ValueRep::SomeLateRegister:
        needsRegister = true;
        isLate = true;
        break;
    case ValueRep::Register:
    case ValueRep::StackArgument:
        return;
    case ValueRep::SomeEarlyRegister:
    case ValueRep::Stack:
    case ValueRep::Constant:
        allocator.m_proc.fail(FailureKind::MalformedProcedure, patch);
        return;
    }

    if (value->hasNoPosition()) {
        if (!needsRegister && (value->isConstant() || patch->onlyKeepsChildrenAlive)) {
            location = Location::immediate();
            return;
        }
        if (value->opcode() == ArgumentReg) {
            location = Location::reg(value->argumentReg());
            return;
        }
        needsRegister = true;
    }

    Bank bank = bankOf(value->type());
    const RegisterMask allocatableInBank = allocator.m_file.allocatableIn(bank);
    RegisterMask forbidden = patch->earlyClobberedMask();
    if (isLate)
        forbidden |= patch->lateClobberedMask() | registersOfFixedPatchResults(patch);

    if (Reg reg = allocator.anyRegisterOf(value, allocatableInBank & ~forbidden)) {
        bool isLockedAlready = !isLate && (locked & maskOf(reg));
        bool isRootInKeptRegister = isLate && patch->isSuspension();
        if (needsRegister || isLockedAlready || isRootInKeptRegister || patchKeepsRegistersBesideOperand(allocator, patch, bank, locked | maskOf(reg), 0, isLate)) {
            location = Location::reg(reg);
            locked |= maskOf(reg);
            return;
        }
        location = memoryHomeOfPatchOperand(allocator, value);
        return;
    }

    RegisterMask allowed = allocatableInBank & ~forbidden & ~locked;
    bool isLoaded = needsRegister || (isWarm && allowed && allocator.hasFreeRegister(bank, allowed) && patchKeepsRegistersBesideOperand(allocator, patch, bank, locked, 1, isLate));
    if (!isLoaded) {
        location = memoryHomeOfPatchOperand(allocator, value);
        return;
    }

    Reg reg = allowed ? allocator.ensureInRegister(value, allowed, hintOfPatchResultAmong(allocator.hintOf(value), allowed)) : Reg();
    if (!reg) {
        allocator.m_proc.fail(FailureKind::OutOfRegisters, patch);
        return;
    }
    noteRegisterTakenForPatch(allocator, reg);
    location = Location::reg(reg);
    locked |= maskOf(reg);
}

static ALWAYS_INLINE RegisterMask takeScratchRegistersInLine(Allocator& allocator, Bank bank, unsigned count, RegisterMask& locked)
{
    RegisterMask taken = 0;
    while (count--) {
        RegisterMask allowed = allocator.m_file.allocatableIn(bank) & ~locked;
        Reg reg = allowed ? allocator.takeRegister(bank, allowed) : Reg();
        if (!reg) {
            allocator.m_proc.fail(FailureKind::OutOfRegisters);
            return taken;
        }
        noteRegisterTakenForPatch(allocator, reg);
        taken |= maskOf(reg);
        locked |= maskOf(reg);
    }
    return taken;
}

struct RegistersPatchTakesBesideOperands {
    RegisterMask gpScratch { 0 };
    RegisterMask fpScratch { 0 };
    RegisterMask earlyResults { 0 };
};

static NEVER_INLINE RegistersPatchTakesBesideOperands takeRegistersOfPatchBesideOperands(Allocator& allocator, PatchOperation* patch, std::span<Location> locationsOfResults, RegisterMask strictlyLocked)
{
    RegistersPatchTakesBesideOperands taken;
    const Type type = patch->type();
    unsigned numberNeeded[numberOfBanks] { patch->numGPScratchRegisters, patch->numFPScratchRegisters };
    bool earlyResultsOfBank[numberOfBanks] { };
    for (unsigned index = 0; index < locationsOfResults.size(); ++index) {
        if (placedConstraintOfPatchResult(patch, index).kind() == ValueRep::SomeEarlyRegister) {
            unsigned bank = static_cast<unsigned>(bankOf(allocator.m_proc.resultType(type, index)));
            ++numberNeeded[bank];
            earlyResultsOfBank[bank] = true;
        }
    }
    for (Bank bank : { Bank::GP, Bank::FP }) {
        RegisterMask available = allocator.m_file.allocatableIn(bank) & ~strictlyLocked;
        if (static_cast<unsigned>(std::popcount(available)) < numberNeeded[static_cast<unsigned>(bank)] || (earlyResultsOfBank[static_cast<unsigned>(bank)] && !(available & patch->registersOfResults()))) {
            allocator.m_proc.fail(FailureKind::OutOfRegisters, patch);
            return taken;
        }
    }

    taken.gpScratch = takeScratchRegistersInLine(allocator, Bank::GP, patch->numGPScratchRegisters, strictlyLocked);
    taken.fpScratch = takeScratchRegistersInLine(allocator, Bank::FP, patch->numFPScratchRegisters, strictlyLocked);
    for (unsigned index = 0; index < locationsOfResults.size(); ++index) {
        if (placedConstraintOfPatchResult(patch, index).kind() != ValueRep::SomeEarlyRegister)
            continue;
        Bank bank = bankOf(allocator.m_proc.resultType(type, index));
        Reg reg = takeRegisterForPatchResult(allocator, bank, allocator.m_file.allocatableIn(bank) & ~strictlyLocked & patch->registersOfResults(), allocatedValueOfPatchResult(allocator, patch, index));
        if (!reg) {
            allocator.m_proc.fail(FailureKind::OutOfRegisters, patch);
            return taken;
        }
        strictlyLocked |= maskOf(reg);
        taken.earlyResults |= maskOf(reg);
        locationsOfResults[index] = Location::reg(reg);
    }
    return taken;
}

static NEVER_INLINE void placeResultsOfPatch(Allocator& allocator, PatchOperation* patch, ArenaVector<Reg>& registersAtDefinitions, RegisterMask fixedResults, std::span<Location> locationsOfResults, RegisterMask locked)
{
    const Type type = patch->type();
    const std::span<const ValueRep> constraints = patch->resultConstraints.span();
    RegisterMask taken = locked | fixedResults;
    RegisterMask given = 0;
    RegisterMask loadedBehindPatch = 0;
    for (unsigned index = 0; index < locationsOfResults.size(); ++index) {
        const ValueRep::Kind kind = index < constraints.size() ? constraints[index].kind() : ValueRep::SomeRegister;
        Bank bank = bankOf(allocator.m_proc.resultType(type, index));
        Operation* value = type.isTuple() ? allocatedValueOfPatchResult(allocator, patch, index) : patch;
        const ValueFacts* facts = value ? &allocator.m_liveness.of(value) : nullptr;
        bool isRead = facts && facts->lastUse != noPosition;

        RegisterMask available = allocator.m_file.allocatableIn(bank) & ~taken;
        Reg reg;
        switch (kind) {
        case ValueRep::Register:
            reg = constraints[index].reg();
            break;
        case ValueRep::SomeRegister:
            reg = takeRegisterForPatchResult(allocator, bank, available & ~patch->lateClobberedMask() & patch->registersOfResults(), value);
            break;
        case ValueRep::SomeEarlyRegister:
            if (locationsOfResults[index].isRegister())
                reg = locationsOfResults[index].reg();
            break;
        case ValueRep::StackArgument: {
            const ValueRep& constraint = constraints[index];
            if (!patchStackWordLiesInOutgoingArea(allocator.m_proc, constraint)) {
                allocator.m_proc.fail(FailureKind::MalformedProcedure, patch);
                return;
            }
            locationsOfResults[index] = Location::outgoing(static_cast<uint32_t>(constraint.offsetFromSP()));
            allocator.noteFrameUse();
            if (!isRead || value == patch)
                continue;
            RegisterMask clobberedLate = available & patch->lateClobberedMask();
            reg = takeRegisterForPatchResult(allocator, bank, clobberedLate ? clobberedLate : available, value);
            if (!reg && (loadedBehindPatch & allocator.m_file.allocatableIn(bank))) {
                allocator.stateOf(value).nextUse = facts->firstUse;
                continue;
            }
            if (reg)
                loadedBehindPatch |= maskOf(reg);
            break;
        }
        case ValueRep::WarmAny:
        case ValueRep::ColdAny:
        case ValueRep::LateColdAny:
        case ValueRep::SomeLateRegister:
        case ValueRep::Stack:
        case ValueRep::Constant:
            allocator.m_proc.fail(FailureKind::MalformedProcedure, patch);
            return;
        }
        if (!reg || (given & maskOf(reg))) {
            allocator.m_proc.fail(FailureKind::OutOfRegisters, patch);
            return;
        }

        given |= maskOf(reg);
        taken |= maskOf(reg);
        noteRegisterTakenForPatch(allocator, reg);
        if (value) {
            registersAtDefinitions[value->index()] = reg;
            allocator.stateOf(value).nextUse = facts->firstUse;
        }
        if (kind == ValueRep::StackArgument)
            continue;

        locationsOfResults[index] = Location::reg(reg);
        if (!isRead)
            continue;
        if (allocator.m_bound & maskOf(reg))
            allocator.evict(reg);
        allocator.bind(reg, value);
        bool isLostAtNextCall = facts->has(ValueFlag::LivesAcrossCall) && !allocator.leavesValuesAcrossCallsToFramedPart() && !(maskOf(reg) & allocator.m_file.calleeSaved) && !allocator.hasFreeRegister(bank, allocator.m_file.calleeSavedIn(bank) & ~taken) && callStandsOnWayFromDefinitionToRead(allocator, value);
        if (facts->has(ValueFlag::LivesIntoLandingPad) || facts->has(ValueFlag::LivesAcrossSuspension) || isLostAtNextCall)
            allocator.markSpilled(value);
    }
}

static ALWAYS_INLINE Reg takeRegisterForPatchResultWithFacts(Allocator& allocator, Bank bank, RegisterMask allowed, const Operation* value, const ValueFacts& facts)
{
    if (!allowed || facts.has(ValueFlag::LivesAcrossCall))
        return takeRegisterForPatchResult(allocator, bank, allowed, value);
    return allocator.takeRegister(bank, allowed, hintOfPatchResultAmong(allocator.hintOf(value), allowed));
}

static ALWAYS_INLINE void bringOperandOfPatchToNamedRegister(Allocator& allocator, PatchOperation* patch, Operation* value, Reg reg, RegisterMask wanted, ArenaVector<Step>& steps)
{
    const Position early = allocator.m_position;

    Operation* holder = allocator.m_valueIn[reg.index()];
    if (holder && holder != value && !isReadOnlyByFixedPatchOperands(allocator, patch, holder, early + 1)) {
        RegisterMask unavailable = wanted | patch->earlyClobberedMask() | patch->lateClobberedMask() | registersOfFixedPatchResults(patch);
        vacateRegistersForPatch(allocator, maskOf(reg), std::max<Position>(early, 1) - 1, unavailable);
    }

    Allocator::ValueState& state = allocator.stateOf(value);
    if (!(state.registers & maskOf(reg))) {
        Location source = sourceOfFixedPatchOperand(allocator, value, wanted);
        if (!source) [[unlikely]] {
            allocator.m_pendingMoves.clear();
            allocator.m_pendingMoves.append(allocator.m_arena, PendingMove { value, Location::reg(reg), source, widthOf(value->type()) });
            resolveParallelMoves(allocator.m_arena, allocator.m_pendingMoves.span(), steps);
        } else {
            StepKind kind = source.isRegister() ? StepKind::Move : source.isImmediate() ? StepKind::Materialize : StepKind::Reload;
            appendMoveForPatchAllocation(allocator, kind, value, Location::reg(reg), source);
        }
    }

    holder = allocator.m_valueIn[reg.index()];
    if (holder == value)
        return;
    if (holder)
        allocator.unbind(reg);
    if (!value->hasNoPosition())
        bindFreeRegisterNotedForPatch(allocator, reg, value, state);
}

static NEVER_INLINE void bringOperandsOfPatchToNamedRegisters(Allocator& allocator, PatchOperation* patch, Operation* const* wantedIn, RegisterMask wanted, const uint8_t* registersToFill, unsigned numberOfRegistersToFill, ArenaVector<Step>& steps)
{
    ASSERT(numberOfRegistersToFill > 1);
    const Position early = allocator.m_position;

    RegisterMask registersToClear = 0;
    for (unsigned place = 0; place < numberOfRegistersToFill; ++place) {
        Reg reg = Reg::fromIndex(registersToFill[place]);
        Operation* holder = allocator.m_valueIn[reg.index()];
        if (holder && holder != wantedIn[reg.index()] && !isReadOnlyByFixedPatchOperands(allocator, patch, holder, early + 1))
            registersToClear |= maskOf(reg);
    }
    if (registersToClear) {
        RegisterMask unavailable = wanted | patch->earlyClobberedMask() | patch->lateClobberedMask() | registersOfFixedPatchResults(patch);
        vacateRegistersForPatch(allocator, registersToClear, std::max<Position>(early, 1) - 1, unavailable);
    }

    RegisterMask filledFromRegisters = 0;
    RegisterMask filledFromElsewhere = 0;
    RegisterMask registersRead = 0;
    alignas(Location) std::byte storageOfSources[64 * sizeof(Location)];
    Location* const sourceInto = reinterpret_cast<Location*>(storageOfSources);
    bool movesNeedResolver = false;
    for (unsigned place = 0; place < numberOfRegistersToFill; ++place) {
        const unsigned index = registersToFill[place];
        const RegisterMask mask = RegisterMask { 1 } << index;
        Operation* value = wantedIn[index];
        if (allocator.stateOf(value).registers & mask)
            continue;

        Location source = sourceOfFixedPatchOperand(allocator, value, wanted);
        if (source.isRegister()) {
            RegisterMask maskOfSource = maskOf(source.reg());
            movesNeedResolver |= !!(registersRead & maskOfSource);
            registersRead |= maskOfSource;
            filledFromRegisters |= mask;
        } else {
            movesNeedResolver |= !source;
            for (RegisterMask earlier = filledFromElsewhere; earlier; earlier &= earlier - 1) {
                unsigned indexOfEarlier = static_cast<unsigned>(std::countr_zero(earlier));
                movesNeedResolver |= sourceInto[indexOfEarlier] == source && (!source.isImmediate() || wantedIn[indexOfEarlier] == value);
            }
            filledFromElsewhere |= mask;
        }
        sourceInto[index] = source;
    }

    const RegisterMask filled = filledFromRegisters | filledFromElsewhere;
    if (!movesNeedResolver && !(filled & registersRead)) {
        for (RegisterMask remaining = filledFromRegisters; remaining; remaining &= remaining - 1) {
            unsigned index = static_cast<unsigned>(std::countr_zero(remaining));
            appendMoveForPatchAllocation(allocator, StepKind::Move, wantedIn[index], Location::reg(Reg::fromIndex(index)), sourceInto[index]);
        }
        for (RegisterMask remaining = filledFromElsewhere; remaining; remaining &= remaining - 1) {
            unsigned index = static_cast<unsigned>(std::countr_zero(remaining));
            appendMoveForPatchAllocation(allocator, sourceInto[index].isImmediate() ? StepKind::Materialize : StepKind::Reload, wantedIn[index], Location::reg(Reg::fromIndex(index)), sourceInto[index]);
        }
    } else {
        uint8_t movesOutOfRegisters[64];
        uint8_t movesFromElsewhere[64];
        unsigned numberOutOfRegisters = 0;
        unsigned numberFromElsewhere = 0;
        if (!movesNeedResolver) {
            for (RegisterMask ready = filledFromRegisters & ~registersRead; ready; ready &= ready - 1)
                movesOutOfRegisters[numberOutOfRegisters++] = static_cast<uint8_t>(std::countr_zero(ready));
            for (RegisterMask ready = filledFromElsewhere & ~registersRead; ready; ready &= ready - 1)
                movesFromElsewhere[numberFromElsewhere++] = static_cast<uint8_t>(std::countr_zero(ready));
            for (unsigned next = 0; next < numberOutOfRegisters; ++next) {
                Reg freed = sourceInto[movesOutOfRegisters[next]].reg();
                if (filledFromRegisters & maskOf(freed))
                    movesOutOfRegisters[numberOutOfRegisters++] = static_cast<uint8_t>(freed.index());
                else if (filledFromElsewhere & maskOf(freed))
                    movesFromElsewhere[numberFromElsewhere++] = static_cast<uint8_t>(freed.index());
            }
        }

        if (movesNeedResolver || numberOutOfRegisters + numberFromElsewhere != static_cast<unsigned>(std::popcount(filled))) {
            allocator.m_pendingMoves.clear();
            for (RegisterMask remaining = filled; remaining; remaining &= remaining - 1) {
                Reg reg = Allocator::lowestRegisterIn(remaining);
                Operation* value = wantedIn[reg.index()];
                allocator.m_pendingMoves.append(allocator.m_arena, PendingMove { value, Location::reg(reg), sourceInto[reg.index()], widthOf(value->type()) });
            }
            resolveParallelMoves(allocator.m_arena, allocator.m_pendingMoves.span(), steps);
        } else {
            for (unsigned next = 0; next < numberOutOfRegisters; ++next) {
                unsigned index = movesOutOfRegisters[next];
                appendMoveForPatchAllocation(allocator, StepKind::Move, wantedIn[index], Location::reg(Reg::fromIndex(index)), sourceInto[index]);
            }
            for (unsigned next = 0; next < numberFromElsewhere; ++next) {
                unsigned index = movesFromElsewhere[next];
                appendMoveForPatchAllocation(allocator, sourceInto[index].isImmediate() ? StepKind::Materialize : StepKind::Reload, wantedIn[index], Location::reg(Reg::fromIndex(index)), sourceInto[index]);
            }
        }
    }

    for (unsigned place = 0; place < numberOfRegistersToFill; ++place) {
        Reg reg = Reg::fromIndex(registersToFill[place]);
        Operation* value = wantedIn[reg.index()];
        Operation* holder = allocator.m_valueIn[reg.index()];
        if (holder == value)
            continue;
        if (holder)
            allocator.unbind(reg);
        if (!value->hasNoPosition())
            bindFreeRegisterNotedForPatch(allocator, reg, value, allocator.stateOf(value));
    }
}

static NEVER_INLINE bool childrenKeptAliveArePlacedAsConstrained(const PatchOperation* patch)
{
    std::span<Operation* const> operands = patch->operands();
    for (unsigned child = 0; child < operands.size(); ++child) {
        if (patch->constraintOf(child).kind() != ValueRep::ColdAny || (operands[child]->hasNoPosition() && !operands[child]->isConstant()))
            return false;
    }
    return true;
}

void Allocator::allocatePatch(PatchOperation* patch)
{
    if (m_isOutsideFramedPart && isInRareRegion() && patch->effects.mayCollect) [[unlikely]]
        keepPointersThatWait(endOfWhatWaysLeadToFromSafepointsOfRareRegion());

    auto allocatePlainPatch = [&]<bool patchEndsBlock, bool someOperandMayBeBrought>() ALWAYS_INLINE_LAMBDA -> bool {
        ASSERT(someOperandMayBeBrought || patch->m_registersToBringOperandsTo.isEmpty());
        const uint16_t bits = patch->m_bits;
        const Type type = patch->type();
        const unsigned numberOfResults = patch->resultConstraints.size();
        if (patch->opcode() != Patch || type.isTuple() || numberOfResults != (type.kind() == Void ? 0u : 1u)
            || (bits & PatchOperation::IsSuspension)) [[unlikely]]
            return false;
        if (patch->onlyKeepsChildrenAlive && !childrenKeptAliveArePlacedAsConstrained(patch)) [[unlikely]]
            return false;

        enum : unsigned {
            SomeOperandTakesRegister = 1 << 0,
            SomeOperandMayLieAnywhere = 1 << 1,
            SomeOperandIsPlacedInItsTurn = 1 << 2,
            SomeOperandStaysInItsSpill = 1 << 3,
            RegistersAreLostBehindPatch = 1 << 4,
            PatchTakesRegistersBesideOperands = 1 << 5,
        };
        unsigned traitsOfPatch = 0;
        if (patch->numGPScratchRegisters || patch->numFPScratchRegisters)
            traitsOfPatch = PatchTakesRegistersBesideOperands;

        const RegisterMask allocatable = m_file.allocatableIn(Bank::GP) | m_file.allocatableIn(Bank::FP);
        ValueRep::Kind kindOfResult = ValueRep::SomeRegister;
        RegisterMask fixedResults = 0;
        if (numberOfResults) {
            const ValueRep& constraint = patch->resultConstraints[0];
            kindOfResult = constraint.kind();
            if (kindOfResult == ValueRep::Register) {
                fixedResults = maskOf(constraint.reg());
                if (fixedResults & ~allocatable) [[unlikely]]
                    return false;
            } else if (kindOfResult == ValueRep::SomeEarlyRegister)
                traitsOfPatch |= PatchTakesRegistersBesideOperands;
            else if (kindOfResult != ValueRep::SomeRegister) [[unlikely]]
                return false;
        }

        ASSERT(m_position == earlyPositionOfAllocatedPatch(*this, patch));

        constexpr uint16_t bitsThatSpeakOfFrame = PatchOperation::RunsWithoutFrame | PatchOperation::FollowsFrameSetup | PatchOperation::NeedsFrameRecord;
        if (!(bits & bitsThatSpeakOfFrame)) {
            noteFrameUse();
            m_allocation.m_holdsPatchThatCountsOnStackCheck = true;
        } else if ((bits & bitsThatSpeakOfFrame) != PatchOperation::RunsWithoutFrame) {
            if ((bits & PatchOperation::NeedsFrameRecord) || !(bits & PatchOperation::RunsWithoutFrame))
                noteFrameUse();
            if (bits & PatchOperation::FollowsFrameSetup)
                m_allocation.m_holdsPatchThatFollowsFrameSetup = true;
            if (bits & PatchOperation::NeedsFrameRecord)
                m_allocation.m_holdsPatchThatCountsOnFrameRecord = true;
            m_allocation.m_holdsPatchThatCountsOnStackCheck = true;
        } else if (patchEndsBlock && !patch->owner()->numSuccessors() && !(bits & PatchOperation::LeavesByReturning))
            m_allocation.m_holdsPatchThatCountsOnStackCheck = true;

        Location* locations = nullptr;
        if (const unsigned numberOfLocations = numberOfResults + patch->m_operands.size())
            locations = m_arena.allocateArray<Location>(numberOfLocations);

        RegisterMask registersNamedBesideWanted = 0;
        RegisterMask writtenForOperands = 0;
        Operation* wantedIn[64];
        RegisterMask wanted = 0;
        uint8_t registersToFill[64];
        unsigned numberOfRegistersToFill = 0;
        RegisterMask registersOfOperandsThatStay = 0;
        RegisterMask writtenForOperandsToldElsewhere = 0;
        if (const unsigned numberOfChildren = patch->m_operands.size()) {
            ASSERT(patch->m_constraints.size() == numberOfChildren);
            Operation* const* const children = patch->m_operands.begin();
            const ValueRep* const constraints = patch->m_constraints.begin();
            const unsigned endOfOperandsBroughtToRegisters = someOperandMayBeBrought ? std::min<unsigned>(patch->m_registersToBringOperandsTo.size(), numberOfChildren) : 0;
            const Reg* const registersToBringOperandsTo = patch->m_registersToBringOperandsTo.begin();
            Location* const placesOfOperands = locations + numberOfResults;
            const RegisterMask earlyClobbered = patch->m_earlyClobbered;
            for (unsigned child = 0; child < numberOfChildren; ++child) {
                const ValueRep& constraint = constraints[child];
                const ValueRep::Kind kind = constraint.kind();
                Operation* value = children[child];
                const Reg registerToBringTo = child < endOfOperandsBroughtToRegisters ? registersToBringOperandsTo[child] : Reg();
                if (kind == ValueRep::Register) {
                    const Reg reg = constraint.reg();
                    const RegisterMask mask = maskOf(reg);
                    if (registerToBringTo) [[unlikely]]
                        return false;
                    placesOfOperands[child] = Location::reg(reg);
                    if (value->hasNoPosition() && value->isArgumentReg(reg)) {
                        registersNamedBesideWanted |= mask;
                        continue;
                    }
                    if (wanted & mask) {
                        if (wantedIn[reg.index()] != value) [[unlikely]]
                            return false;
                        continue;
                    }
                    if (!(mask & allocatable)) [[unlikely]]
                        return false;
                    wantedIn[reg.index()] = value;
                    wanted |= mask;
                    if (m_valueIn[reg.index()] != value)
                        registersToFill[numberOfRegistersToFill++] = static_cast<uint8_t>(reg.index());
                    continue;
                }

                const RegisterMask registersOfValue = stateOf(value).registers;
                RegisterMask maskToBringTo = 0;
                if (registerToBringTo) {
                    maskToBringTo = maskOf(registerToBringTo);
                    registersNamedBesideWanted |= maskToBringTo;
                    if (!(registersOfValue & maskToBringTo))
                        writtenForOperands |= maskToBringTo;
                }

                const RegisterMask held = registersOfValue & ~earlyClobbered;
                ASSERT(!(held & ~m_file.allocatableIn(bankOf(value->type()))));
                Location& place = placesOfOperands[child];
                auto letOperandStayInRegister = [&]() ALWAYS_INLINE_LAMBDA {
                    const Reg reg = lowestRegisterIn(held);
                    place = Location::reg(reg);
                    registersOfOperandsThatStay |= maskOf(reg);
                    if (maskOf(reg) != maskToBringTo)
                        writtenForOperandsToldElsewhere |= maskToBringTo;
                };
                if (patchOperandMayLieAnywhere(kind)) {
                    if (value->hasNoPosition()) {
                        if (value->isConstant()) {
                            traitsOfPatch |= SomeOperandMayLieAnywhere;
                            place = Location::immediate();
                            writtenForOperandsToldElsewhere |= maskToBringTo;
                        } else
                            traitsOfPatch |= SomeOperandTakesRegister | SomeOperandIsPlacedInItsTurn;
                        continue;
                    }
                    traitsOfPatch |= SomeOperandMayLieAnywhere;
                    if (held && kind != ValueRep::LateColdAny)
                        letOperandStayInRegister();
                    else if (!held && kind == ValueRep::ColdAny && spillHoldsValueWherePatchStands(*this, value)) {
                        traitsOfPatch |= SomeOperandStaysInItsSpill;
                        place = placeToReadIn(m_allocation.spillOf(value));
                        writtenForOperandsToldElsewhere |= maskToBringTo;
                    } else
                        traitsOfPatch |= SomeOperandIsPlacedInItsTurn;
                    continue;
                }
                traitsOfPatch |= SomeOperandTakesRegister;
                if (kind == ValueRep::SomeRegister && held && !value->hasNoPosition())
                    letOperandStayInRegister();
                else if (kind == ValueRep::SomeRegister || kind == ValueRep::SomeLateRegister)
                    traitsOfPatch |= SomeOperandIsPlacedInItsTurn;
                else [[unlikely]]
                    return false;
            }
        }

        RegisterMask written = patch->m_earlyClobbered | patch->m_lateClobbered;
        if (patchEndsBlock && !patch->m_successorClobbers.isEmpty()) {
            const unsigned numberOfSuccessors = patch->owner()->numSuccessors();
            for (const auto& clobber : patch->m_successorClobbers) {
                if (clobber.successorIndex < numberOfSuccessors)
                    written |= clobber.mask;
            }
        }
        const RegisterMask registersNamedByPatch = fixedResults | wanted | registersNamedBesideWanted;
        if ((written | registersNamedByPatch) & m_file.calleeSaved) [[unlikely]] {
            written &= ~m_file.calleeSavedClobberedAtSuspensions;
            written |= registersNamedByPatch;
            written &= m_file.calleeSaved;
            if (written) {
                forEachRegister(written, [&](Reg reg) {
                    noteCalleeSaveUse(reg);
                });
                if (isInRareRegion() && (written & ~m_calleeSavesTakenOutsideRareRegions))
                    m_allocation.m_needsFrameOutsideRareRegions = true;
            }
        }

        if (patchEndsBlock) {
            if (!someSuccessorIsNoLandingPad(patch->owner()))
                traitsOfPatch |= RegistersAreLostBehindPatch;
            else if (!patch->owner()->upsilons().empty()) {
                keepValuesHandedOnBehindPatchInMemory(*this, patch->owner(), m_position + 1);
                if (m_proc.failure()) [[unlikely]]
                    return true;
                traitsOfPatch |= SomeOperandIsPlacedInItsTurn;
            }
        }

        if (numberOfRegistersToFill) {
            if (numberOfRegistersToFill == 1)
                bringOperandOfPatchToNamedRegister(*this, patch, wantedIn[registersToFill[0]], Reg::fromIndex(registersToFill[0]), wanted, m_allocation.m_steps);
            else
                bringOperandsOfPatchToNamedRegisters(*this, patch, wantedIn, wanted, registersToFill, numberOfRegistersToFill, m_allocation.m_steps);
            if (m_proc.failure()) [[unlikely]]
                return true;
        }
        RegisterMask locked = wanted;

        RegisterMask heldLate = 0;
        if (const unsigned numberOfChildren = patch->m_operands.size()) {
            Operation* const* const children = patch->m_operands.begin();

            if (someOperandMayBeBrought && patch->bringsOperandsToTheirRegisters()) {
                if (numberOfRegistersToFill) {
                    const unsigned endOfOperandsBroughtToRegisters = std::min<unsigned>(patch->m_registersToBringOperandsTo.size(), numberOfChildren);
                    const Reg* const registersToBringOperandsTo = patch->m_registersToBringOperandsTo.begin();
                    writtenForOperands = 0;
                    for (unsigned child = 0; child < endOfOperandsBroughtToRegisters; ++child) {
                        Reg reg = registersToBringOperandsTo[child];
                        if (reg && !(stateOf(children[child]).registers & maskOf(reg)))
                            writtenForOperands |= maskOf(reg);
                    }
                }
                patch->m_lateClobbered |= writtenForOperands & ~fixedResults;
            }

            if (traitsOfPatch & (SomeOperandTakesRegister | SomeOperandMayLieAnywhere)) {
                const bool takesNothingBesideOperands = !(traitsOfPatch & PatchTakesRegistersBesideOperands);

                bool operandsStayWhereTheyLie = !(traitsOfPatch & SomeOperandIsPlacedInItsTurn) && !numberOfRegistersToFill;
                if (operandsStayWhereTheyLie && !takesNothingBesideOperands && (traitsOfPatch & SomeOperandMayLieAnywhere)) {
                    const RegisterMask registersLeft = (~m_file.calleeSaved | calleeSavesThatMayBeTaken()) & ~(wanted | registersOfOperandsThatStay | patch->m_earlyClobbered | patch->m_lateClobbered | fixedResults);
                    for (Bank bank : { Bank::GP, Bank::FP }) {
                        const RegisterMask allocatableInBank = m_file.allocatableIn(bank);
                        if (!(registersOfOperandsThatStay & allocatableInBank))
                            continue;
                        unsigned numberTaken = bank == Bank::GP ? patch->numGPScratchRegisters : patch->numFPScratchRegisters;
                        if (kindOfResult == ValueRep::SomeEarlyRegister && bankOf(patch->type()) == bank)
                            ++numberTaken;
                        if (static_cast<unsigned>(std::popcount(allocatableInBank & registersLeft)) < numberTaken)
                            operandsStayWhereTheyLie = false;
                    }
                }

                if (operandsStayWhereTheyLie) {
                    locked |= registersOfOperandsThatStay;
                    if (traitsOfPatch & SomeOperandStaysInItsSpill) {
                        noteFrameUse();
                        notePatchWithOperandInFrame(patch);
                    }
                } else {
                    const ValueRep* const constraints = patch->m_constraints.begin();
                    Location* const placesOfOperands = locations + patch->resultConstraints.size();
                    const RegisterMask earlyClobbered = patch->m_earlyClobbered;
                    const unsigned endOfOperandsBroughtToRegisters = someOperandMayBeBrought ? std::min<unsigned>(patch->m_registersToBringOperandsTo.size(), numberOfChildren) : 0;
                    const Reg* const registersToBringOperandsTo = patch->m_registersToBringOperandsTo.begin();
                    writtenForOperandsToldElsewhere = 0;

                    auto placeOperandInGeneral = [&](ValueRep::Kind kind, Operation* value, Location& place) ALWAYS_INLINE_LAMBDA {
                        RegisterMask lockedBehindOperand = locked;
                        placeOperandOfPatchUnder(*this, patch, kind, value, place, lockedBehindOperand);
                        locked = lockedBehindOperand;
                    };
                    auto notePlaceOfOperand = [&](unsigned child, ValueRep::Kind kind, Location place) ALWAYS_INLINE_LAMBDA {
                        if (place.isSpill())
                            notePatchWithOperandInFrame(patch);
                        if (child < endOfOperandsBroughtToRegisters) {
                            Reg reg = registersToBringOperandsTo[child];
                            if (reg && !(place.isRegister() && place.reg() == reg))
                                writtenForOperandsToldElsewhere |= maskOf(reg);
                        }
                        if (patchOperandIsReadLate(kind) && place.isRegister())
                            heldLate |= maskOf(place.reg());
                    };

                    if (traitsOfPatch & SomeOperandTakesRegister) {
                        for (unsigned child = 0; child < numberOfChildren; ++child) {
                            const ValueRep::Kind kind = constraints[child].kind();
                            Operation* value = children[child];
                            if (kind == ValueRep::Register || !patchOperandTakesRegister(kind, value, false))
                                continue;
                            Location& place = placesOfOperands[child];
                            const RegisterMask held = stateOf(value).registers & ~earlyClobbered;
                            ASSERT(!(held & ~m_file.allocatableIn(bankOf(value->type()))));
                            if (kind == ValueRep::SomeRegister && held && !value->hasNoPosition()) {
                                Reg reg = lowestRegisterIn(held);
                                place = Location::reg(reg);
                                locked |= maskOf(reg);
                            } else {
                                placeOperandInGeneral(kind, value, place);
                                if (m_proc.failure()) [[unlikely]]
                                    return true;
                            }
                            notePlaceOfOperand(child, kind, place);
                        }
                    }
                    if (traitsOfPatch & SomeOperandMayLieAnywhere) {
                        for (unsigned child = 0; child < numberOfChildren; ++child) {
                            const ValueRep::Kind kind = constraints[child].kind();
                            Operation* value = children[child];
                            if (!patchOperandMayLieAnywhere(kind) || patchOperandTakesRegister(kind, value, false))
                                continue;
                            Location& place = placesOfOperands[child];
                            if (value->hasNoPosition()) {
                                place = Location::immediate();
                                notePlaceOfOperand(child, kind, place);
                                continue;
                            }
                            const RegisterMask held = stateOf(value).registers & ~earlyClobbered;
                            ASSERT(!(held & ~m_file.allocatableIn(bankOf(value->type()))));
                            if (held && kind != ValueRep::LateColdAny) {
                                Reg reg = lowestRegisterIn(held);
                                if (takesNothingBesideOperands || (locked & maskOf(reg))) {
                                    place = Location::reg(reg);
                                    locked |= maskOf(reg);
                                    notePlaceOfOperand(child, kind, place);
                                    continue;
                                }
                            }
                            if (!held && kind == ValueRep::ColdAny)
                                place = memoryHomeOfPatchOperand(*this, value);
                            else
                                placeOperandInGeneral(kind, value, place);
                            if (m_proc.failure()) [[unlikely]]
                                return true;
                            notePlaceOfOperand(child, kind, place);
                        }
                    }
                }

                if (someOperandMayBeBrought && patch->bringsOperandsToTheirRegisters())
                    patch->m_lateClobbered |= writtenForOperandsToldElsewhere & ~fixedResults;
            }

            const Position position = m_position;
            ValueState* const states = m_values.begin();
            const ValueFacts* const factsOfValues = m_valueFacts;
            Operation* const* const endOfChildren = children + numberOfChildren;
            for (Operation* const* child = children; child != endOfChildren; ++child) {
                Operation* value = *child;
                const uint32_t index = value->index();
                const ValueFacts& facts = factsOfValues[index];
                if (facts.has(ValueFlag::IsFused)) [[unlikely]]
                    passUsesOf(value, position);
                else
                    passUseReachedByWalk(value, states[index], facts, position);
            }
        }

        const RegisterMask registersToVacate = ((patch->m_earlyClobbered | patch->m_lateClobbered) & allocatable) | fixedResults;
        if (patchEndsBlock && (traitsOfPatch & RegistersAreLostBehindPatch)) [[unlikely]] {
            forEachRegister(m_bound, [&](Reg reg) {
                unbind(reg);
            });
        } else
            vacateRegistersForPatchInLine(*this, registersToVacate, m_position + 1, locked);

        const ValueFacts& factsOfResult = m_valueFacts[patch->index()];
        RegistersPatchTakesBesideOperands besideOperands;
        if (traitsOfPatch & PatchTakesRegistersBesideOperands) {
            if (patch->numGPScratchRegisters || patch->numFPScratchRegisters) {
                besideOperands = takeRegistersOfPatchBesideOperands(*this, patch, std::span<Location>(locations, patch->resultConstraints.size()), locked | registersToVacate);
                if (m_proc.failure()) [[unlikely]]
                    return true;
            } else {
                ASSERT(kindOfResult == ValueRep::SomeEarlyRegister);
                const Bank bank = bankOf(patch->type());
                const RegisterMask allowed = m_file.allocatableIn(bank) & ~(locked | registersToVacate) & patch->registersOfResults();
                Reg reg = takeRegisterForPatchResultWithFacts(*this, bank, allowed, patch, factsOfResult);
                if (!reg) [[unlikely]] {
                    m_proc.fail(FailureKind::OutOfRegisters, patch);
                    return true;
                }
                besideOperands.earlyResults = maskOf(reg);
            }
        }

        RegisterMask registerOfResult = 0;
        Location locationOfResult;
        bool needsPointerBesideResult = false;
        if (patch->resultConstraints.size()) {
            auto registersTakenBesideResult = [&]() ALWAYS_INLINE_LAMBDA -> RegisterMask {
                return besideOperands.gpScratch | besideOperands.fpScratch | besideOperands.earlyResults | heldLate | fixedResults;
            };
            Reg reg;
            if (kindOfResult == ValueRep::Register) {
                reg = patch->resultConstraints[0].reg();
                registerOfResult = fixedResults;
            } else {
                if (kindOfResult == ValueRep::SomeRegister) {
                    const Bank bank = bankOf(patch->type());
                    reg = takeRegisterForPatchResultWithFacts(*this, bank, m_file.allocatableIn(bank) & ~registersTakenBesideResult() & ~patch->m_lateClobbered & patch->registersOfResults(), patch, factsOfResult);
                } else if (besideOperands.earlyResults)
                    reg = lowestRegisterIn(besideOperands.earlyResults);
                if (!reg) [[unlikely]] {
                    m_proc.fail(FailureKind::OutOfRegisters, patch);
                    return true;
                }
                registerOfResult = maskOf(reg);
                noteRegisterTakenForPatch(*this, reg);
            }

            ValueState& stateOfResult = stateOf(patch);
            m_allocation.m_registerAtDefinition[patch->index()] = reg;
            stateOfResult.nextUse = factsOfResult.firstUse;
            locationOfResult = Location::reg(reg);
            locations[0] = locationOfResult;
            if (factsOfResult.lastUse != noPosition) {
                const uint16_t flagsOfResult = factsOfResult.flags;
                needsPointerBesideResult = flagsOfResult & bitOf(ValueFlag::NeedsPointerBesideIt);
                if (m_bound & registerOfResult)
                    evict(reg);
                bindFreeRegisterNotedForPatch(*this, reg, patch, stateOfResult);
                bool isLostAtNextCall = false;
                if (flagsOfResult & bitOf(ValueFlag::LivesAcrossCall)) {
                    const Bank bank = bankOf(patch->type());
                    isLostAtNextCall = !leavesValuesAcrossCallsToFramedPart() && !(registerOfResult & m_file.calleeSaved) && !hasFreeRegister(bank, m_file.calleeSavedIn(bank) & ~(registersTakenBesideResult() | registerOfResult)) && callStandsOnWayFromDefinitionToRead(*this, patch);
                }
                if ((flagsOfResult & (bitOf(ValueFlag::LivesIntoLandingPad) | bitOf(ValueFlag::LivesAcrossSuspension))) || isLostAtNextCall)
                    markSpilled(patch);
            }
        }

        const RegisterMask usedRegisters = m_bound | registerOfResult;
        ArenaVector<PatchAssignment>& assignments = m_allocation.m_patchAssignments;
        const uint32_t indexOfAssignment = assignments.size();
        new (NotNull, assignments.appendUninitialized(m_arena)) PatchAssignment { std::span<const Location>(locations, patch->resultConstraints.size() + patch->m_operands.size()), usedRegisters, besideOperands.gpScratch, besideOperands.fpScratch };

        const uint32_t indexOfStep = m_allocation.m_steps.size();
        Step step;
        step.kind = StepKind::Execute;
        step.patchAssignment = indexOfAssignment;
        step.destination = locationOfResult;
        step.operation = patch;
        appendStep(step);

        if (patchEndsBlock) {
            BlockSteps& steps = m_allocation.m_blocks[patch->owner()->index()];
            steps.end = indexOfStep;
            steps.terminator = indexOfStep;
            return true;
        }

        if (needsPointerBesideResult) [[unlikely]]
            appendStepsBehindPatchResult(*this, patch, locationOfResult);
        return true;
    };
    if (patch->effects.terminal ? allocatePlainPatch.operator()<true, true>() : patch->m_registersToBringOperandsTo.isEmpty() ? allocatePlainPatch.operator()<false, false>() : allocatePlainPatch.operator()<false, true>()) [[likely]]
        return;

    const bool isCheck = patch->opcode() == Check;
    const bool endsBlock = !isCheck && patch->effects.terminal;
    ASSERT(endsBlock == patch->endsBlock());
    const Position early = m_position;
    ASSERT(early == earlyPositionOfAllocatedPatch(*this, patch));
    const Position late = early + 1;
    const uint16_t bits = patch->m_bits;
    const Type type = patch->type();
    const bool onlyKeepsChildrenAlive = patch->onlyKeepsChildrenAlive;
    const unsigned firstChild = isCheck ? 1 : 0;
    const unsigned numberOfChildren = patch->m_operands.size();
    ASSERT(patch->m_constraints.size() == numberOfChildren);
    Operation* const* const children = patch->m_operands.begin();
    const ValueRep* const constraints = patch->m_constraints.begin();
    const unsigned endOfOperandsBroughtToRegisters = std::min<unsigned>(patch->m_registersToBringOperandsTo.size(), numberOfChildren);
    const Reg* const registersToBringOperandsTo = patch->m_registersToBringOperandsTo.begin();
    const unsigned numberOfResults = m_proc.resultCount(type);
    const RegisterMask allocatable = m_file.allocatableIn(Bank::GP) | m_file.allocatableIn(Bank::FP);
    const RegisterMask clobberedAsAppended = patch->earlyClobberedMask() | patch->lateClobberedMask();
    RegisterMask clobbered = clobberedAsAppended & allocatable;

    RegisterMask fixedResults = 0;
    bool hasEarlyResults = false;
    const std::span<const ValueRep> resultConstraints = patch->resultConstraints.span();
    for (unsigned index = 0; index < resultConstraints.size(); ++index) {
        const ValueRep& constraint = resultConstraints[index];
        if (constraint.isReg())
            fixedResults |= maskOf(constraint.reg());
        else if (constraint.kind() == ValueRep::SomeEarlyRegister && index < numberOfResults)
            hasEarlyResults = true;
    }

    if (fixedResults & ~allocatable) [[unlikely]] {
        m_proc.fail(FailureKind::OutOfRegisters, patch);
        return;
    }
    if (type.isTuple() && !extractsStandBehindPatch(*this, patch)) {
        m_proc.fail(FailureKind::MalformedProcedure, patch);
        return;
    }

    const bool followsFrameSetup = bits & PatchOperation::FollowsFrameSetup;
    const bool needsFrame = (bits & PatchOperation::NeedsFrameRecord) || !(bits & PatchOperation::RunsWithoutFrame);
    if (needsFrame)
        noteFrameUse();
    if (followsFrameSetup)
        m_allocation.m_holdsPatchThatFollowsFrameSetup = true;
    if (bits & PatchOperation::NeedsFrameRecord)
        m_allocation.m_holdsPatchThatCountsOnFrameRecord = true;
    if (needsFrame || followsFrameSetup || (endsBlock && !patch->owner()->numSuccessors() && !(bits & PatchOperation::LeavesByReturning)))
        m_allocation.m_holdsPatchThatCountsOnStackCheck = true;

    RegisterMask registersNamedForOperands = 0;
    bool someOperandIsFixed = false;
    bool someOperandTakesRegister = false;
    bool someOperandMayLieAnywhere = false;
    bool someOperandIsReadLate = false;
    for (unsigned child = firstChild; child < numberOfChildren; ++child) {
        const ValueRep& constraint = constraints[child];
        ValueRep::Kind kind = placedKindOfPatchOperandUnder(constraint.kind(), onlyKeepsChildrenAlive, isCheck);
        if (kind == ValueRep::Register) {
            registersNamedForOperands |= maskOf(constraint.reg());
            someOperandIsFixed = true;
            continue;
        }
        if (child < endOfOperandsBroughtToRegisters) {
            if (Reg reg = registersToBringOperandsTo[child])
                registersNamedForOperands |= maskOf(reg);
        }
        if (kind == ValueRep::StackArgument) {
            someOperandIsFixed = true;
            continue;
        }
        if (patchOperandTakesRegister(kind, children[child], onlyKeepsChildrenAlive))
            someOperandTakesRegister = true;
        else
            someOperandMayLieAnywhere = true;
        someOperandIsReadLate |= patchOperandIsReadLate(kind);
    }

    RegisterMask written = clobberedAsAppended;
    if (endsBlock && !patch->m_successorClobbers.isEmpty()) {
        const unsigned numberOfSuccessors = patch->owner()->numSuccessors();
        for (const auto& clobber : patch->m_successorClobbers) {
            if (clobber.successorIndex < numberOfSuccessors)
                written |= clobber.mask;
        }
    }
    written &= ~m_file.calleeSavedClobberedAtSuspensions;
    written |= fixedResults | registersNamedForOperands;
    written &= m_file.calleeSaved;
    if (written) {
        forEachRegister(written, [&](Reg reg) {
            noteCalleeSaveUse(reg);
        });
        if (isInRareRegion() && (written & ~m_calleeSavesTakenOutsideRareRegions))
            m_allocation.m_needsFrameOutsideRareRegions = true;
    }

    if (bits & PatchOperation::IsSuspension) [[unlikely]]
        prepareSuspension(patch);

    std::span<Location> locations = m_arena.createSpan<Location>(numberOfResults + numberOfChildren - firstChild);
    std::span<Location> locationsOfResults = locations.first(numberOfResults);
    std::span<Location> locationsOfOperands = locations.subspan(numberOfResults);
    Location* const placesOfOperands = locationsOfOperands.data();

    bool registersMatterBehind = true;
    if (endsBlock) {
        registersMatterBehind = registersMatterBehindPatch(patch);
        if (registersMatterBehind && !patch->owner()->upsilons().empty())
            keepValuesHandedOnBehindPatchInMemory(*this, patch->owner(), late);
    }

    RegisterMask locked = 0;
    if (someOperandIsFixed)
        placeFixedOperands(patch, locationsOfOperands, locked);

    const bool bringsOperandsToTheirRegisters = (bits & PatchOperation::BringsOperandsToTheirRegisters) && endOfOperandsBroughtToRegisters;
    if (bringsOperandsToTheirRegisters) {
        RegisterMask writtenForOperands = 0;
        for (unsigned child = firstChild; child < endOfOperandsBroughtToRegisters; ++child) {
            Reg reg = registersToBringOperandsTo[child];
            if (reg && !(stateOf(children[child]).registers & maskOf(reg)))
                writtenForOperands |= maskOf(reg);
        }
        patch->m_lateClobbered |= writtenForOperands & ~fixedResults;
        clobbered |= writtenForOperands & ~fixedResults & allocatable;
    }

    Location sources[maxLeaves];
    unsigned numberOfSources = 0;
    if (isCheck && !m_proc.failure())
        numberOfSources = placeLeavesOfCheckPredicate(*this, patch, sources, locked);

    const RegisterMask forbiddenToLateOperands = patch->m_earlyClobbered | patch->m_lateClobbered | fixedResults;
    const bool takesNothingEarlyBesideOperands = !patch->numGPScratchRegisters && !patch->numFPScratchRegisters && !hasEarlyResults;
    for (bool takesRegister : { true, false }) {
        if (!(takesRegister ? someOperandTakesRegister : someOperandMayLieAnywhere))
            continue;
        for (unsigned child = firstChild; child < numberOfChildren && !m_proc.failure(); ++child) {
            ValueRep::Kind kind = placedKindOfPatchOperandUnder(constraints[child].kind(), onlyKeepsChildrenAlive, isCheck);
            if (kind == ValueRep::Register || kind == ValueRep::StackArgument)
                continue;
            Operation* value = children[child];
            if (patchOperandTakesRegister(kind, value, onlyKeepsChildrenAlive) != takesRegister)
                continue;
            Location& place = placesOfOperands[child - firstChild];
            if (!takesRegister && !value->hasNoPosition()) {
                const RegisterMask forbidden = kind == ValueRep::LateColdAny ? forbiddenToLateOperands : patch->m_earlyClobbered;
                const RegisterMask held = stateOf(value).registers & m_file.allocatableIn(bankOf(value->type())) & ~forbidden;
                if (!held) {
                    if (kind != ValueRep::WarmAny) {
                        place = memoryHomeOfPatchOperandInLine(*this, value);
                        continue;
                    }
                } else if (kind != ValueRep::LateColdAny && takesNothingEarlyBesideOperands) {
                    const Reg reg = lowestRegisterIn(held);
                    place = Location::reg(reg);
                    locked |= maskOf(reg);
                    continue;
                }
            }
            placeOperandOfPatchUnder(*this, patch, kind, value, place, locked);
        }
    }
    if (m_proc.failure()) [[unlikely]]
        return;

    if (bringsOperandsToTheirRegisters) {
        RegisterMask writtenForOperands = 0;
        for (unsigned child = firstChild; child < endOfOperandsBroughtToRegisters; ++child) {
            Reg reg = registersToBringOperandsTo[child];
            Location location = placesOfOperands[child - firstChild];
            if (reg && !(location.isRegister() && location.reg() == reg))
                writtenForOperands |= maskOf(reg);
        }
        patch->m_lateClobbered |= writtenForOperands & ~fixedResults;
        clobbered |= writtenForOperands & ~fixedResults & allocatable;
    }

    RegisterMask heldLate = 0;
    for (unsigned index = 0; index < numberOfChildren - firstChild; ++index) {
        if (placesOfOperands[index].isSpill())
            notePatchWithOperandInFrame(patch);
    }
    if (someOperandIsReadLate) {
        for (unsigned child = firstChild; child < numberOfChildren; ++child) {
            Location location = placesOfOperands[child - firstChild];
            if (location.isRegister() && patchOperandIsReadLate(placedKindOfPatchOperandUnder(constraints[child].kind(), onlyKeepsChildrenAlive, isCheck)))
                heldLate |= maskOf(location.reg());
        }
    }

    if (numberOfChildren)
        passUsesOf(patch, early);

    if (!registersMatterBehind) {
        forEachRegister(m_bound, [&](Reg reg) {
            unbind(reg);
        });
    } else if (!isCheck && ((clobbered | fixedResults) & m_bound))
        vacateRegistersForPatch(*this, clobbered | fixedResults, late, locked);

    RegistersPatchTakesBesideOperands besideOperands;
    if (patch->numGPScratchRegisters || patch->numFPScratchRegisters || hasEarlyResults)
        besideOperands = takeRegistersOfPatchBesideOperands(*this, patch, locationsOfResults, locked | clobbered | fixedResults);
    if (m_proc.failure()) [[unlikely]]
        return;

    if (numberOfResults) {
        placeResultsOfPatch(*this, patch, m_allocation.m_registerAtDefinition, fixedResults, locationsOfResults, besideOperands.gpScratch | besideOperands.fpScratch | besideOperands.earlyResults | heldLate);
        if (m_proc.failure()) [[unlikely]]
            return;
    }

    RegisterMask usedRegisters = m_bound;
    for (Location location : locationsOfResults) {
        if (location.isRegister())
            usedRegisters |= maskOf(location.reg());
    }
    uint32_t indexOfAssignment = m_allocation.m_patchAssignments.size();
    m_allocation.m_patchAssignments.append(m_arena, PatchAssignment { locations, usedRegisters, besideOperands.gpScratch, besideOperands.fpScratch });

    uint32_t indexOfStep = m_allocation.m_steps.size();
    Step& step = appendStep(StepKind::Execute, patch);
    step.patchAssignment = indexOfAssignment;
    if (numberOfResults == 1)
        step.destination = locationsOfResults[0];
    for (unsigned index = 0; index < numberOfSources; ++index)
        step.sources[index] = sources[index];

    if (endsBlock) {
        BlockSteps& steps = m_allocation.m_blocks[patch->owner()->index()];
        steps.end = indexOfStep;
        steps.terminator = indexOfStep;
        return;
    }

    appendStepsBehindPatchResults(*this, patch, locationsOfResults);
}

void Allocator::placeFixedOperands(PatchOperation* patch, std::span<Location> locationsOfOperands, RegisterMask& locked)
{
    if (patch->onlyKeepsChildrenAlive)
        return;

    const unsigned firstChild = firstPlacedChildOfPatch(patch);
    const unsigned numberOfChildren = patch->m_operands.size();
    ASSERT(patch->m_constraints.size() == numberOfChildren);
    Operation* const* const children = patch->m_operands.begin();
    const ValueRep* const constraints = patch->m_constraints.begin();
    Location* const placesOfOperands = locationsOfOperands.data();
    const RegisterMask allocatable = m_file.allocatableIn(Bank::GP) | m_file.allocatableIn(Bank::FP);

    Operation* wantedIn[64];
    RegisterMask wanted = 0;
    bool hasFixedOperands = false;
    for (unsigned child = firstChild; child < numberOfChildren; ++child) {
        const ValueRep& constraint = constraints[child];
        if (!constraint.isReg()) {
            hasFixedOperands |= constraint.isStackArgument();
            continue;
        }
        hasFixedOperands = true;
        Operation* value = children[child];
        Reg reg = constraint.reg();
        if (value->hasNoPosition() && value->isArgumentReg(reg))
            continue;
        RegisterMask mask = maskOf(reg);
        if (!(mask & allocatable) || ((wanted & mask) && wantedIn[reg.index()] != value)) {
            m_proc.fail(FailureKind::OutOfRegisters, patch);
            return;
        }
        wantedIn[reg.index()] = value;
        wanted |= mask;
    }
    if (!hasFixedOperands)
        return;

    const Position early = m_position;
    ASSERT(early == earlyPositionOfAllocatedPatch(*this, patch));

    RegisterMask registersToClear = 0;
    forEachRegister(wanted & m_bound, [&](Reg reg) {
        Operation* holder = m_valueIn[reg.index()];
        if (holder != wantedIn[reg.index()] && !isReadOnlyByFixedPatchOperands(*this, patch, holder, early + 1))
            registersToClear |= maskOf(reg);
    });
    if (registersToClear) {
        RegisterMask unavailable = wanted | locked | patch->earlyClobberedMask() | patch->lateClobberedMask() | registersOfFixedPatchResults(patch);
        vacateRegistersForPatch(*this, registersToClear, std::max<Position>(early, 1) - 1, unavailable);
    }

    m_pendingMoves.clear();
    RegisterMask filled = 0;
    RegisterMask filledFromRegisters = 0;
    RegisterMask filledFromElsewhere = 0;
    RegisterMask registersRead = 0;
    uint8_t placeOfMoveInto[64];
    bool movesNeedResolver = false;
    for (unsigned child = firstChild; child < numberOfChildren; ++child) {
        const ValueRep& constraint = constraints[child];
        Operation* value = children[child];
        if (constraint.isReg()) {
            Reg reg = constraint.reg();
            RegisterMask mask = maskOf(reg);
            Location destination = Location::reg(reg);
            placesOfOperands[child - firstChild] = destination;
            bool isInPlace = !(wanted & mask) || (stateOf(value).registers & mask) || (filled & mask);
            filled |= mask;
            if (isInPlace)
                continue;

            Location source = sourceOfFixedPatchOperand(*this, value, wanted);
            if (source.isRegister()) {
                RegisterMask maskOfSource = maskOf(source.reg());
                movesNeedResolver |= !!(registersRead & maskOfSource);
                registersRead |= maskOfSource;
                filledFromRegisters |= mask;
            } else {
                movesNeedResolver |= !source;
                for (const PendingMove& earlier : m_pendingMoves)
                    movesNeedResolver |= earlier.source == source && (!source.isImmediate() || earlier.value == value);
                filledFromElsewhere |= mask;
            }
            placeOfMoveInto[reg.index()] = static_cast<uint8_t>(m_pendingMoves.size());
            m_pendingMoves.append(m_arena, PendingMove { value, destination, source, widthOf(value->type()) });
            continue;
        }
        if (!constraint.isStackArgument())
            continue;

        if (!patchStackWordLiesInOutgoingArea(m_proc, constraint)) {
            m_proc.fail(FailureKind::MalformedProcedure, patch);
            return;
        }
        Location destination = Location::outgoing(static_cast<uint32_t>(constraint.offsetFromSP()));
        placesOfOperands[child - firstChild] = destination;
        noteFrameUse();
        movesNeedResolver = true;
        m_pendingMoves.append(m_arena, PendingMove { value, destination, sourceOfFixedPatchOperand(*this, value, wanted), widthOf(value->type()) });
    }
    if (!m_pendingMoves.isEmpty()) {
        if (movesNeedResolver || ((filledFromRegisters | filledFromElsewhere) & registersRead))
            resolveParallelMoves(m_arena, m_pendingMoves.span(), m_allocation.m_steps);
        else {
            forEachRegister(filledFromRegisters, [&](Reg reg) {
                const PendingMove& move = m_pendingMoves[placeOfMoveInto[reg.index()]];
                appendMoveForPatchAllocation(*this, StepKind::Move, move.value, move.destination, move.source);
            });
            forEachRegister(filledFromElsewhere, [&](Reg reg) {
                const PendingMove& move = m_pendingMoves[placeOfMoveInto[reg.index()]];
                appendMoveForPatchAllocation(*this, move.source.isImmediate() ? StepKind::Materialize : StepKind::Reload, move.value, move.destination, move.source);
            });
        }
    }

    forEachRegister(wanted, [&](Reg reg) {
        Operation* value = wantedIn[reg.index()];
        Operation* holder = m_valueIn[reg.index()];
        if (holder == value)
            return;
        if (holder)
            unbind(reg);
        if (!value->hasNoPosition())
            bind(reg, value);
    });
    locked |= wanted;
}

void Allocator::vacate(RegisterMask clobbered, Position behind)
{
    vacateRegistersForPatch(*this, clobbered, behind, 0);
}

RegisterMask Allocator::takeScratchRegisters(Bank bank, unsigned count, RegisterMask& locked)
{
    return takeScratchRegistersInLine(*this, bank, count, locked);
}

bool Allocator::isReadOnSomeWayFromHeadOf(const Operation* value, const Block* block)
{
    if (m_liveness.of(value).has(ValueFlag::IsKeptForCollector))
        return true;
    bool numberingCostsNoMoreThanSearchesHave = !m_numbersOfDominatedBlocks.isEmpty() || m_livenessSearchBlocksVisited >= m_order.blocksInOrder().size();
    if (numberingCostsNoMoreThanSearchesHave && m_valueOfLastSearchForWays != value && dominatesSomeReadBelowDefinitionOf(block, value))
        return true;
    findBlocksThatLeadToReadsOf(value, firstBlockOnWaysFrom(block, value));
    return m_lastSearchThatReachedBlock[block->index()] == m_numberOfSearchesForWays;
}

bool Allocator::dominatesSomeReadBelowDefinitionOf(const Block* block, const Operation* value)
{
    if (block == value->owner() || !dominates(value->owner(), block))
        return false;
    std::span<Block* const> blocks = m_order.blocksInOrder();
    for (uint32_t use = m_liveness.of(value).firstUse; use; use = m_liveness.use(use).next) {
        auto behind = std::ranges::upper_bound(blocks, m_liveness.use(use).position, { }, [&](const Block* other) {
            return m_order.headOf(other);
        });
        if (dominates(block, *std::prev(behind)))
            return true;
    }
    return false;
}

uint32_t Allocator::firstBlockOnWaysFrom(const Block* block, const Operation* value)
{
    std::span<const Loop> loops = m_order.loops();
    if (!m_someLoopHasSideEntrance) {
        m_someLoopHasSideEntrance = std::ranges::any_of(loops, [](const Loop& loop) {
            return loop.hasSideEntrance;
        });
    }
    if (*m_someLoopHasSideEntrance || m_order.of(block).isRare() || m_order.of(value->owner()).isRare())
        return 0;
    uint32_t first = m_order.of(block).orderIndex;
    uint32_t definedIn = m_order.of(value->owner()).orderIndex;
    for (uint32_t loop = m_order.of(block).loop; loop != noLoop; loop = loops[loop].parent) {
        if (loops[loop].firstBlock <= definedIn && definedIn < loops[loop].endBlock)
            break;
        first = std::min(first, loops[loop].firstBlock);
    }
    return first;
}

void Allocator::findBlocksThatLeadToReadsOf(const Operation* value, uint32_t firstBlock)
{
    const uint32_t firstUse = m_liveness.of(value).firstUse;
    if (m_valueOfLastSearchForWays != value || firstBlock < m_firstBlockOfLastSearchForWays) {
        m_valueOfLastSearchForWays = value;
        m_firstBlockOfLastSearchForWays = firstBlock;
        if (m_lastSearchThatReachedBlock.isEmpty())
            m_lastSearchThatReachedBlock.fill(m_arena, m_order.mostBlocks(), 0);
        ++m_numberOfSearchesForWays;
        ArenaVector<const Block*>& reached = m_blocksReachedBySearchForWays;
        reached.clear();
        auto reach = [&](const Block* block) {
            uint32_t& last = m_lastSearchThatReachedBlock[block->index()];
            if (block == value->owner() || last == m_numberOfSearchesForWays || m_order.of(block).orderIndex < firstBlock)
                return;
            last = m_numberOfSearchesForWays;
            reached.append(m_arena, block);
        };
        std::span<Block* const> blocks = m_order.blocksInOrder();
        for (uint32_t use = firstUse; use; use = m_liveness.use(use).next) {
            auto behind = std::ranges::upper_bound(blocks, m_liveness.use(use).position, { }, [&](const Block* block) {
                return m_order.headOf(block);
            });
            reach(*std::prev(behind));
        }
        for (uint32_t index = 0; index < reached.size(); ++index) {
            for (const Block* predecessor : m_order.of(reached[index]).predecessors)
                reach(predecessor);
        }
        m_livenessSearchBlocksVisited += reached.size();
    }
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
