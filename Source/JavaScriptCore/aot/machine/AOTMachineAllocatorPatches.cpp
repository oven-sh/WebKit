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

// What sank into a block reads at the head, all of it at one position, and is allocated with the walk ahead of the head. So what a later one reads there is live behind this one.
static Position earlyPositionOfAllocatedPatch(const Allocator& allocator, const PatchOperation* patch)
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

// `value` is null for a result that nothing names.
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

// Allocator::spillHoldsValueIn() the block of the walk, for as long as the walk is in it.
// allocateBlock() sets Allocator::m_isAheadOfFramedPart by that block. The framed part is found ahead of the walk, and no block leaves it. So where the flag is down, whatever has a spill lies in it.
static ALWAYS_INLINE bool spillHoldsValueWherePatchStands(const Allocator& allocator, const Operation* value)
{
    bool holdsValue = allocator.m_allocation.isSpilled(value) && (!allocator.m_isAheadOfFramedPart || allocator.spillHoldsValueIn(allocator.m_block, value));
    ASSERT(holdsValue == allocator.spillHoldsValueIn(allocator.m_block, value));
    return holdsValue;
}

static bool liesInMemoryForPatchAllocation(Allocator& allocator, const Operation* value)
{
    // What has lost its register ahead of the framed part is read from its spill: settleFramedPart() finds the read, and the frame is set up at the entry.
    if (allocator.m_allocation.isSpilled(value) && !allocator.stateOf(value).registers)
        return true;
    return spillHoldsValueWherePatchStands(allocator, value) || allocator.stateOf(value).saveInRareRegion != noSpill;
}

static Location memoryHomeOfPatchOperand(Allocator& allocator, Operation* value)
{
    // memoryLocationOf() asks the same first. noteFrameUse() may make a spill hold what it did not hold before, and never the other way round.
    if (spillHoldsValueWherePatchStands(allocator, value)) {
        allocator.noteFrameUse();
        return Location::spill(allocator.m_allocation.spillOf(value));
    }

    if (!liesInMemoryForPatchAllocation(allocator, value)) {
        if (allocator.isInRareRegion() && allocator.isDefinedOutsideRareRegion(value))
            allocator.saveInRareRegion(value, allocator.anyRegisterOf(value, allocator.stateOf(value).registers));
        else
            allocator.markSpilled(value);
    } else if (allocator.stateOf(value).registers) {
        // As in evict(): only a Save in this block is known to have run.
        SpillIndex save = allocator.stateOf(value).saveInRareRegion;
        if (allocator.m_allocation.spills()[save].begin < allocator.m_order.headOf(allocator.m_block))
            appendMoveForPatchAllocation(allocator, StepKind::Save, value, Location::spill(save), Location::reg(allocator.anyRegisterOf(value, allocator.stateOf(value).registers)));
    }
    allocator.noteFrameUse();
    return allocator.memoryLocationOf(value);
}

// `placedKind`: placedKindOfPatchOperand().
static ALWAYS_INLINE bool patchOperandTakesRegister(ValueRep::Kind placedKind, const Operation* value, bool onlyKeepsChildrenAlive)
{
    if (!patchOperandMayLieAnywhere(placedKind))
        return true;
    return value->hasNoPosition() && !value->isConstant() && !onlyKeepsChildrenAlive;
}

// The registers of the bank that the Patch takes beside those of its operands: the scratch registers and the results that share a register with no operand.
// An operand that is read late shares its register with no result at all. The one result in an outgoing word takes its register behind the Patch.
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

// `numberStillToLock`: registers that the operand will take and that `locked` does not name yet.
static bool patchKeepsRegistersBesideOperand(const Allocator& allocator, const PatchOperation* patch, Bank bank, RegisterMask locked, unsigned numberStillToLock, bool operandIsReadLate)
{
    unsigned numberTaken = numberStillToLock + numberOfRegistersPatchTakesBesideOperand(allocator.m_proc, patch, bank, operandIsReadLate);
    if (!numberTaken)
        return true;
    // The operand gives way before a rare region takes a callee-saved register that the code outside does not save.
    RegisterMask registers = allocator.m_file.allocatableIn(bank) & (~allocator.m_file.calleeSaved | allocator.calleeSavesThatMayBeTaken());
    RegisterMask unavailable = locked | patch->earlyClobberedMask() | patch->lateClobberedMask() | registersOfFixedPatchResults(patch);
    return static_cast<unsigned>(std::popcount(registers & ~unavailable)) >= numberTaken;
}

// Of a value in `reg` that is read behind the operation and lies in none but the registers that are vacated: `held`. It goes to a free register outside `unavailable`, else by evict().
// `spillHoldsValue`: spillHoldsValueWherePatchStands().
static NEVER_INLINE void moveValueOutOfRegistersVacatedForPatch(Allocator& allocator, Reg reg, Operation* value, RegisterMask held, RegisterMask unavailable, bool spillHoldsValue)
{
    Bank bank = bankOf(reg);
    RegisterMask allowed = allocator.m_file.allocatableIn(bank) & ~unavailable;
    // liesInMemoryForPatchAllocation(), of a value that lies in a register.
    ASSERT(held && held == allocator.stateOf(value).registers);
    bool staysReachable = value->hasNoPosition() || spillHoldsValue || allocator.stateOf(value).saveInRareRegion != noSpill;
    ASSERT(staysReachable == (value->hasNoPosition() || liesInMemoryForPatchAllocation(allocator, value)));
    Reg target;
    if (!staysReachable)
        allowed &= ~allocator.m_file.calleeSaved | allocator.calleeSavesWorthTakingFor(value, bank, allocator.m_block);
    if (!staysReachable && allowed && allocator.hasFreeRegister(bank, allowed))
        target = allocator.takeRegister(bank, allowed, hintOfPatchResultAmong(allocator.hintOf(value), allowed));
    if (!target) {
        allocator.evict(reg);
        return;
    }

    // takeRegister() has noted the use of a callee-saved register that it hands out, and bind() notes it once more.
    appendMoveForPatchAllocation(allocator, StepKind::Move, value, Location::reg(target), Location::reg(reg));
    allocator.bind(target, value);
    forEachRegister(held, [&](Reg holder) {
        allocator.unbind(holder);
    });
}

// No value goes to a register in `unavailable`: an operand that has been placed there is read only when the operation runs.
static ALWAYS_INLINE void vacateRegistersForPatchInLine(Allocator& allocator, RegisterMask registers, Position behind, RegisterMask unavailable)
{
    unavailable |= registers;
    for (RegisterMask remaining = registers & allocator.m_bound; remaining; remaining &= remaining - 1) {
        Reg reg = Allocator::lowestRegisterIn(remaining);
        Operation* value = allocator.m_valueIn[reg.index()];
        // What was moved out of another of the registers has given this one up too.
        if (!value)
            continue;
        RegisterMask held = allocator.stateOf(value).registers;
        if (!allocator.isLiveBehind(value, behind) || (held & ~registers)) {
            allocator.unbind(reg);
            continue;
        }
        // A number that is made again or that its spill holds stays reachable, so it gets no other register.
        // And evict() stores nothing for it, whether it finds it dead, held twice, made again or in memory: it frees the register.
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
        moveValueOutOfRegistersVacatedForPatch(allocator, reg, value, held, unavailable, spillHoldsValue);
    }
}

static NEVER_INLINE void vacateRegistersForPatch(Allocator& allocator, RegisterMask registers, Position behind, RegisterMask unavailable)
{
    vacateRegistersForPatchInLine(allocator, registers, behind, unavailable);
}

// functor(Operation* extract) for the Extracts that stand right behind the Patch: at the head of its first successor where it ends its block.
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

// The operation that later operations name for the result. Null for a member of a tuple that nothing extracts.
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

// An Extract is bound where its Patch is allocated, and a spilled one is stored where it stands itself.
// So nothing that takes a register may stand between the two, and a register holds one value: a member is extracted once.
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

static bool registersMatterBehindPatch(const PatchOperation* patch)
{
    if (!patch->endsBlock())
        return true;
    for (const Edge& edge : patch->owner()->successors()) {
        if (!edge.block()->isLandingPad())
            return true;
    }
    return false;
}

static bool isReadOnlyByFixedPatchOperands(const Allocator& allocator, const PatchOperation* patch, const Operation* value, Position late)
{
    if (patch->opcode() == Check || allocator.isLiveBehind(value, late))
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

static Location sourceOfFixedPatchOperand(Allocator& allocator, Operation* value, RegisterMask wanted)
{
    if (RegisterMask held = allocator.stateOf(value).registers)
        return Location::reg(allocator.anyRegisterOf(value, (held & ~wanted) ? (held & ~wanted) : held));
    if (value->hasNoPosition())
        return Location::immediate();
    allocator.noteFrameUse();
    return allocator.memoryLocationOf(value);
}

// A member that is read, lies in an outgoing word, and found every register given to another member.
static bool patchResultBorrowsRegister(const Allocator& allocator, const Operation* value, Location definedIn)
{
    return value->opcode() == Extract && definedIn.kind() == Location::Outgoing && allocator.m_liveness.of(value).lastUse != noPosition && !allocator.m_allocation.registerAtDefinitionOf(value);
}

// `borrowed`: for a member of which patchResultBorrowsRegister() holds.
static void appendStepsBehindPatchResult(Allocator& allocator, Operation* value, Location definedIn, Reg borrowed = Reg())
{
    const ValueFacts& facts = allocator.m_liveness.of(value);
    if (facts.lastUse == noPosition)
        return;
    // These steps stand behind the Patch, and so ahead of the Extract that defines a member.
    const bool isMember = value->opcode() == Extract;

    // The next call reuses the outgoing area.
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
        // A member with a register of its own is stored from it behind its Extract, where its spill begins. Any other result has no register where it is defined, so nobody else stores it.
        if (!isMember || borrowed)
            appendMoveForPatchAllocation(allocator, StepKind::Save, value, Location::spill(spill), Location::reg(reg));
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

// The members that borrow go first: the register that another member in an outgoing word is loaded into holds nothing until then.
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
    // The one result is the Patch itself, and only a member borrows. Nothing follows a result that nobody reads, nor one in a register that needs no pointer beside it.
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
    // A Check ends no block, and a Patch without a result leaves nothing to finish.
    if (last->opcode() != Patch || last->type() == Void)
        return;
    if (!last->endsBlock() || predecessor->successor(0).block() != block)
        return;
    finishResultsOfPatchThatEndedBlock(allocator, static_cast<PatchOperation*>(last), predecessor);
}

// Of a Patch that ends the block. The moves of the edge go by the registers as they are behind the Patch, and the child of an Upsilon counts as read where the Patch reads its operands.
// One that nothing reads behind the Patch loses its register to an operand, a clobber or a result without being moved. So it gets a place in memory while the register still holds it.
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

// `sources` has room for maxLeaves. How many it has filled.
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

// `placedKind`: placedKindOfPatchOperand().
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
        // A generator is told of a register, a word of the frame or a constant. An address that is made again is none of the last two.
        needsRegister = true;
    }

    Bank bank = bankOf(value->type());
    const RegisterMask allocatableInBank = allocator.m_file.allocatableIn(bank);
    RegisterMask forbidden = patch->earlyClobberedMask();
    if (isLate)
        forbidden |= patch->lateClobberedMask() | registersOfFixedPatchResults(patch);

    // An operand that may lie anywhere keeps or gets a register only while the Patch can still have what it takes beside its operands.
    if (Reg reg = allocator.anyRegisterOf(value, allocatableInBank & ~forbidden)) {
        bool isLockedAlready = !isLate && (locked & maskOf(reg));
        // prepareSuspension() has given no place in the frame to a root that lies in the register which the suspension keeps.
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
    // Of the results under SomeEarlyRegister.
    RegisterMask earlyResults { 0 };
};

// Of a Patch that takes a scratch register or has a result under SomeEarlyRegister. Such a result gets its location here. `strictlyLocked`: what none of them may be.
// Whoever calls asks for a failure behind it.
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

// `locked`: the scratch registers, the registers of operands that are read late, and those of results that have their location already.
// `registersAtDefinitions`: what Allocation::registerAtDefinitionOf() reads. `fixedResults`: registersOfFixedPatchResults().
static NEVER_INLINE void placeResultsOfPatch(Allocator& allocator, PatchOperation* patch, ArenaVector<Reg>& registersAtDefinitions, RegisterMask fixedResults, std::span<Location> locationsOfResults, RegisterMask locked)
{
    const Type type = patch->type();
    const std::span<const ValueRep> constraints = patch->resultConstraints.span();
    RegisterMask taken = locked | fixedResults;
    RegisterMask given = 0;
    RegisterMask loadedBehindPatch = 0;
    for (unsigned index = 0; index < locationsOfResults.size(); ++index) {
        // placedConstraintOfPatchResult(). A constraint that names a register or a word is one that was set.
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
            // allocatePatch() has chosen it: `locked` does not name the operands that it has to differ from.
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
            // appendStepsBehindPatchResult() loads it. The register of a member is chosen here, so that it takes that of no other member: none of them can be stored ahead of its Extract.
            if (!isRead || value == patch)
                continue;
            // The load stands behind the Patch, so a register that is clobbered late serves. The others are left to the results that the Patch itself writes.
            RegisterMask clobberedLate = available & patch->lateClobberedMask();
            reg = takeRegisterForPatchResult(allocator, bank, clobberedLate ? clobberedLate : available, value);
            // With every register given away the member goes to its spill through the register of another, ahead of the load into that one.
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
        // While a callee-saved register is free, a result that is held to another register is left for the next call to move there.
        bool isLostAtNextCall = facts->has(ValueFlag::LivesAcrossCall) && !allocator.leavesValuesAcrossCallsToFramedPart() && !(maskOf(reg) & allocator.m_file.calleeSaved) && !allocator.hasFreeRegister(bank, allocator.m_file.calleeSavedIn(bank) & ~taken) && callStandsOnWayFromDefinitionToRead(allocator, value);
        if (facts->has(ValueFlag::LivesIntoLandingPad) || facts->has(ValueFlag::LivesAcrossSuspension) || isLostAtNextCall)
            allocator.markSpilled(value);
    }
}

// takeRegisterForPatchResult(), for whoever has Liveness::of() the result at hand.
static ALWAYS_INLINE Reg takeRegisterForPatchResultWithFacts(Allocator& allocator, Bank bank, RegisterMask allowed, const Operation* value, const ValueFacts& facts)
{
    if (!allowed || facts.has(ValueFlag::LivesAcrossCall))
        return takeRegisterForPatchResult(allocator, bank, allowed, value);
    return allocator.takeRegister(bank, allowed, hintOfPatchResultAmong(allocator.hintOf(value), allowed));
}

// What placeFixedOperands() does once it knows which operand is wanted where, for a Patch that holds no operand to an outgoing word, while nothing is locked.
// `wantedIn`: by Reg::index(), for the registers in `wanted`. `registersToFill`: those of them that do not hold what is wanted there, each where the first operand that is held to it stands among the operands.
// A register that holds what is wanted there is left alone: it is neither cleared nor taken for a value that gives way, and nothing is moved to it or bound to it.
static NEVER_INLINE void bringOperandsOfPatchToNamedRegisters(Allocator& allocator, PatchOperation* patch, Operation* const* wantedIn, RegisterMask wanted, const uint8_t* registersToFill, unsigned numberOfRegistersToFill, ArenaVector<Step>& steps)
{
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

    if (numberOfRegistersToFill == 1) {
        // One move waits for no other. resolveParallelMoves() is left to deal with a move that has no source.
        const Reg reg = Reg::fromIndex(registersToFill[0]);
        Operation* value = wantedIn[reg.index()];
        if (!(allocator.stateOf(value).registers & maskOf(reg))) {
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
    } else {
        // The registers that the moves fill, by where the value comes from, and those that they read.
        RegisterMask filledFromRegisters = 0;
        RegisterMask filledFromElsewhere = 0;
        RegisterMask registersRead = 0;
        // By Reg::index() of the destination. Only the entries of the registers that the moves fill are set.
        alignas(Location) std::byte storageOfSources[64 * sizeof(Location)];
        Location* const sourceInto = reinterpret_cast<Location*>(storageOfSources);
        // A move has no source, or two of them read one source.
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

        // The order in which resolveParallelMoves() performs moves into registers no two of which read one source, by the destinations.
        // A move is ready once nothing is left to read its destination. Those out of registers go first, each kind in the order in which its moves became ready:
        // by destination for those that are ready from the start, then as the move that reads the destination is performed. What is left at the end goes round in cycles.
        const RegisterMask filled = filledFromRegisters | filledFromElsewhere;
        if (!movesNeedResolver && !(filled & registersRead)) {
            // No move reads a register that another fills, so all of them are ready from the start.
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
                // It sorts the moves by their destinations itself.
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
    }

    // What is done to one register leaves the others as they are, so each is unbound and bound in one go.
    for (unsigned place = 0; place < numberOfRegistersToFill; ++place) {
        Reg reg = Reg::fromIndex(registersToFill[place]);
        Operation* value = wantedIn[reg.index()];
        Operation* holder = allocator.m_valueIn[reg.index()];
        if (holder == value)
            continue;
        if (holder)
            allocator.unbind(reg);
        if (!value->hasNoPosition())
            allocator.bind(reg, value);
    }
}

void Allocator::allocatePatch(PatchOperation* patch)
{
    // Nearly every Patch is plain: it has at most one result, in a register, and each of its operands is held to a register by name, takes some register or lies where it lies.
    // For such a Patch this does what the rest of the function does for every Patch and Check: the same steps in the same order, less those that have nothing to do for it.
    // False: the Patch is not plain. Nothing is done by then but what the rest of the function does again to the same effect: flags that only rise, and memory of the arena that nobody names.
    auto allocatePlainPatch = [&]() ALWAYS_INLINE_LAMBDA -> bool {
        const uint16_t bits = patch->m_bits;
        const Type type = patch->type();
        const unsigned numberOfResults = patch->resultConstraints.size();
        if (patch->opcode() != Patch || type.isTuple() || numberOfResults != (type == Void ? 0u : 1u)
            || (bits & (PatchOperation::IsSuspension | PatchOperation::ReleasesDeadValues))
            || patch->onlyKeepsChildrenAlive || !patch->m_childrenLetGoByCallee.isEmpty()) [[unlikely]]
            return false;

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
            } else if (kindOfResult != ValueRep::SomeRegister && kindOfResult != ValueRep::SomeEarlyRegister) [[unlikely]]
                return false;
        }

        // allocateBlock() has set the position of the walk to the one at which the operation that it hands over reads.
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
        } else if (patch->effects.terminal && !patch->owner()->numSuccessors() && !(bits & PatchOperation::LeavesByReturning))
            m_allocation.m_holdsPatchThatCountsOnStackCheck = true;

        // Each location is written before the assignment names them.
        Location* locations = nullptr;
        if (const unsigned numberOfLocations = numberOfResults + patch->m_operands.size())
            locations = m_arena.allocateArray<Location>(numberOfLocations);

        // One look at the operands: which kinds there are, the registers that are named for them, and which operand is wanted in which register, as placeFixedOperands() finds it.
        // None of what it asks of the Patch changes while the Patch is allocated.
        // It also finds the place of each operand that stays where it lies: in a register, in its spill, or in the code as a constant. To find such a place does nothing, and no other operand is asked about.
        // So this is the place that the operand is given in its turn, unless a register changes hands before that. What follows from the places waits until it is known that they hold.
        enum : unsigned {
            SomeOperandTakesRegister = 1 << 0,
            SomeOperandMayLieAnywhere = 1 << 1,
            // The look has not found its place, or what was done since may have changed it.
            SomeOperandIsPlacedInItsTurn = 1 << 2,
            SomeOperandStaysInItsSpill = 1 << 3,
            // registersMatterBehindPatch() says no.
            RegistersAreLostBehindPatch = 1 << 4,
        };
        unsigned traitsOfPatch = 0;
        // With `wanted`: the registers that are named for operands.
        RegisterMask registersNamedBesideWanted = 0;
        RegisterMask writtenForOperands = 0;
        // By Reg::index(). Only the entries of the registers in `wanted` are set.
        Operation* wantedIn[64];
        RegisterMask wanted = 0;
        // Those of them that do not hold the operand that is wanted there, in the order of the operands.
        uint8_t registersToFill[64];
        unsigned numberOfRegistersToFill = 0;
        // Of the operands that stay where they lie: their registers, and the registers that the generator writes because it is told of another place.
        RegisterMask registersOfOperandsThatStay = 0;
        RegisterMask writtenForOperandsToldElsewhere = 0;
        if (const unsigned numberOfChildren = patch->m_operands.size()) {
            ASSERT(patch->m_constraints.size() == numberOfChildren);
            Operation* const* const children = patch->m_operands.begin();
            const ValueRep* const constraints = patch->m_constraints.begin();
            // PatchOperation::registerToBringOperandTo() names a register for no operand from this one on.
            const unsigned endOfOperandsBroughtToRegisters = std::min<unsigned>(patch->m_registersToBringOperandsTo.size(), numberOfChildren);
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
                    // This holds until a register changes hands: until an operand is brought to a register that it is held to.
                    if (!(registersOfValue & maskToBringTo))
                        writtenForOperands |= maskToBringTo;
                }

                // A value is bound to allocatable registers of its own bank alone, so none of its registers has to be asked whether it is one.
                const RegisterMask held = registersOfValue & ~earlyClobbered;
                ASSERT(!(held & ~m_file.allocatableIn(bankOf(value->type()))));
                Location& place = placesOfOperands[child];
                // The generator is told one place of an operand. Told any other than the register where the operand belongs, it writes that register.
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
                        // A spill that holds its value goes on holding it. memoryHomeOfPatchOperand() answers with it and notes the use of the frame.
                        traitsOfPatch |= SomeOperandStaysInItsSpill;
                        place = Location::spill(m_allocation.spillOf(value));
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

        // A suspension hands the registers that it does not keep back to whoever resumes. A function that names none of them has nothing of its caller's to save.
        RegisterMask written = patch->m_earlyClobbered | patch->m_lateClobbered;
        if (patch->effects.terminal && !patch->m_successorClobbers.isEmpty()) {
            // PatchOperation::lateClobberedOnSuccessor() of each successor: clobberLateOnSuccessor() keeps one entry for a successor.
            const unsigned numberOfSuccessors = patch->owner()->numSuccessors();
            for (const auto& clobber : patch->m_successorClobbers) {
                if (clobber.successorIndex < numberOfSuccessors)
                    written |= clobber.mask;
            }
        }
        written &= ~m_file.calleeSavedClobberedAtSuspensions;
        written |= fixedResults | wanted | registersNamedBesideWanted;
        written &= m_file.calleeSaved;
        if (written) [[unlikely]] {
            forEachRegister(written, [&](Reg reg) {
                noteCalleeSaveUse(reg);
            });
            // A frame that covers rare regions alone saves no register.
            if (isInRareRegion() && (written & ~m_calleeSavesTakenOutsideRareRegions))
                m_allocation.m_needsFrameOutsideRareRegions = true;
        }

        if (patch->effects.terminal) {
            if (!registersMatterBehindPatch(patch))
                traitsOfPatch |= RegistersAreLostBehindPatch;
            else if (!patch->owner()->upsilons().empty()) {
                keepValuesHandedOnBehindPatchInMemory(*this, patch->owner(), m_position + 1);
                if (m_proc.failure()) [[unlikely]]
                    return true;
                traitsOfPatch |= SomeOperandIsPlacedInItsTurn;
            }
        }

        // Where each register holds the operand that is wanted there, placeFixedOperands() finds nothing to clear, to move or to bind. No register has changed hands since the look at the operands.
        if (numberOfRegistersToFill) {
            bringOperandsOfPatchToNamedRegisters(*this, patch, wantedIn, wanted, registersToFill, numberOfRegistersToFill, m_allocation.m_steps);
            if (m_proc.failure()) [[unlikely]]
                return true;
        }
        RegisterMask locked = wanted;

        RegisterMask heldLate = 0;
        if (const unsigned numberOfChildren = patch->m_operands.size()) {
            Operation* const* const children = patch->m_operands.begin();

            // PatchOperation::leaveOperandsWhereTheyLie(). Whatever the generator writes over an operand that lies in its register is the operand.
            const bool bringsOperandsToTheirRegisters = patch->bringsOperandsToTheirRegisters();
            if (bringsOperandsToTheirRegisters) {
                // Registers have changed hands since the look at the operands.
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
                // Then patchKeepsRegistersBesideOperand() says yes to an operand that is not read late and takes no register that `locked` does not name.
                const bool takesNothingBesideOperands = !patch->numGPScratchRegisters && !patch->numFPScratchRegisters && kindOfResult != ValueRep::SomeEarlyRegister;

                bool operandsStayWhereTheyLie = !(traitsOfPatch & SomeOperandIsPlacedInItsTurn) && !numberOfRegistersToFill;
                if (operandsStayWhereTheyLie && !takesNothingBesideOperands && (traitsOfPatch & SomeOperandMayLieAnywhere)) {
                    // An operand that may lie anywhere keeps its register if the Patch can still have what it takes beside its operands with that register locked and those that were locked ahead of it.
                    // No more registers are left with all of them locked. So where the Patch can have its own even then, each operand is told yes in its turn.
                    for (Bank bank : { Bank::GP, Bank::FP }) {
                        if ((registersOfOperandsThatStay & m_file.allocatableIn(bank)) && !patchKeepsRegistersBesideOperand(*this, patch, bank, wanted | registersOfOperandsThatStay, 0, false))
                            operandsStayWhereTheyLie = false;
                    }
                }

                if (operandsStayWhereTheyLie) {
                    locked |= registersOfOperandsThatStay;
                    if (traitsOfPatch & SomeOperandStaysInItsSpill) {
                        noteFrameUse();
                        m_allocation.m_holdsPatchThatCountsOnFrameRecord = true;
                    }
                } else {
                    // Each operand is placed in its turn, whatever the look has found for it.
                    const ValueRep* const constraints = patch->m_constraints.begin();
                    Location* const placesOfOperands = locations + patch->resultConstraints.size();
                    const RegisterMask earlyClobbered = patch->m_earlyClobbered;
                    const unsigned endOfOperandsBroughtToRegisters = std::min<unsigned>(patch->m_registersToBringOperandsTo.size(), numberOfChildren);
                    const Reg* const registersToBringOperandsTo = patch->m_registersToBringOperandsTo.begin();
                    writtenForOperandsToldElsewhere = 0;

                    auto placeOperandInGeneral = [&](ValueRep::Kind kind, Operation* value, Location& place) ALWAYS_INLINE_LAMBDA {
                        RegisterMask lockedBehindOperand = locked;
                        placeOperandOfPatchUnder(*this, patch, kind, value, place, lockedBehindOperand);
                        locked = lockedBehindOperand;
                    };
                    // No operand that is held to a register by name lies in the frame, is brought anywhere or is read late.
                    auto notePlaceOfOperand = [&](unsigned child, ValueRep::Kind kind, Location place) ALWAYS_INLINE_LAMBDA {
                        if (place.isSpill())
                            m_allocation.m_holdsPatchThatCountsOnFrameRecord = true;
                        // The generator is told one place of an operand. Told any other than the register where the operand belongs, it writes that register, which is not locked then, whatever lay there.
                        if (child < endOfOperandsBroughtToRegisters) {
                            Reg reg = registersToBringOperandsTo[child];
                            if (reg && !(place.isRegister() && place.reg() == reg))
                                writtenForOperandsToldElsewhere |= maskOf(reg);
                        }
                        if (patchOperandIsReadLate(kind) && place.isRegister())
                            heldLate |= maskOf(place.reg());
                    };

                    // Those that take a register come first: `locked` then names all of their registers when an operand that may lie anywhere is asked whether it keeps its own.
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
                                // A constant.
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

                if (bringsOperandsToTheirRegisters)
                    patch->m_lateClobbered |= writtenForOperandsToldElsewhere & ~fixedResults;
            }

            // What passUsesOf() does for an operation whose leaves go by its children. No Patch works in memory or goes by overflow.
            Operation* const* const endOfChildren = children + numberOfChildren;
            for (Operation* const* child = children; child != endOfChildren; ++child) {
                Operation* value = *child;
                if (m_valueFacts[value->index()].has(ValueFlag::IsFused)) [[unlikely]]
                    passUsesOf(value, m_position);
                else
                    passUseReachedByWalk(value, m_position);
            }
        }

        // The Patch names what was added for its operands among the registers that it clobbers late.
        const RegisterMask registersToVacate = ((patch->m_earlyClobbered | patch->m_lateClobbered) & allocatable) | fixedResults;
        if (traitsOfPatch & RegistersAreLostBehindPatch) [[unlikely]] {
            // A landing pad is entered with every value in its slot, so no block takes the registers from here.
            forEachRegister(m_bound, [&](Reg reg) {
                unbind(reg);
            });
        } else
            vacateRegistersForPatchInLine(*this, registersToVacate, m_position + 1, locked);

        const ValueFacts& factsOfResult = m_valueFacts[patch->index()];
        RegistersPatchTakesBesideOperands besideOperands;
        if (patch->numGPScratchRegisters || patch->numFPScratchRegisters)
            besideOperands = takeRegistersOfPatchBesideOperands(*this, patch, std::span<Location>(locations, patch->resultConstraints.size()), locked | registersToVacate);
        else if (kindOfResult == ValueRep::SomeEarlyRegister) {
            // takeRegistersOfPatchBesideOperands(), where one register is all that is taken: there are enough if there is one that the result may have.
            const Bank bank = bankOf(patch->type());
            const RegisterMask allowed = m_file.allocatableIn(bank) & ~(locked | registersToVacate) & patch->registersOfResults();
            Reg reg = takeRegisterForPatchResultWithFacts(*this, bank, allowed, patch, factsOfResult);
            if (!reg) [[unlikely]] {
                m_proc.fail(FailureKind::OutOfRegisters, patch);
                return true;
            }
            besideOperands.earlyResults = maskOf(reg);
        }
        if (m_proc.failure()) [[unlikely]]
            return true;

        // placeResultsOfPatch(), for the one result.
        RegisterMask registerOfResult = 0;
        Location locationOfResult;
        bool needsPointerBesideResult = false;
        if (patch->resultConstraints.size()) {
            // Few results ask for these.
            auto registersTakenBesideResult = [&]() ALWAYS_INLINE_LAMBDA -> RegisterMask {
                return besideOperands.gpScratch | besideOperands.fpScratch | besideOperands.earlyResults | heldLate | fixedResults;
            };
            Reg reg;
            if (kindOfResult == ValueRep::Register)
                reg = patch->resultConstraints[0].reg();
            else if (kindOfResult == ValueRep::SomeRegister) {
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
            m_allocation.m_registerAtDefinition[patch->index()] = reg;
            stateOf(patch).nextUse = factsOfResult.firstUse;
            locationOfResult = Location::reg(reg);
            locations[0] = locationOfResult;
            if (factsOfResult.lastUse != noPosition) {
                const uint16_t flagsOfResult = factsOfResult.flags;
                needsPointerBesideResult = flagsOfResult & bitOf(ValueFlag::NeedsPointerBesideIt);
                if (m_bound & registerOfResult)
                    evict(reg);
                bind(reg, patch);
                // While a callee-saved register is free, a result that is held to another register is left for the next call to move there.
                bool isLostAtNextCall = false;
                if (flagsOfResult & bitOf(ValueFlag::LivesAcrossCall)) {
                    const Bank bank = bankOf(patch->type());
                    isLostAtNextCall = !leavesValuesAcrossCallsToFramedPart() && !(registerOfResult & m_file.calleeSaved) && !hasFreeRegister(bank, m_file.calleeSavedIn(bank) & ~(registersTakenBesideResult() | registerOfResult)) && callStandsOnWayFromDefinitionToRead(*this, patch);
                }
                if ((flagsOfResult & (bitOf(ValueFlag::LivesIntoLandingPad) | bitOf(ValueFlag::LivesAcrossSuspension))) || isLostAtNextCall)
                    markSpilled(patch);
            }
            if (m_proc.failure()) [[unlikely]]
                return true;
        }

        // Whatever is bound is taken to keep its value, also what could be made again.
        const RegisterMask usedRegisters = m_bound | registerOfResult;
        ArenaVector<PatchAssignment>& assignments = m_allocation.m_patchAssignments;
        const uint32_t indexOfAssignment = assignments.size();
        new (NotNull, assignments.appendUninitialized(m_arena)) PatchAssignment { std::span<const Location>(locations, patch->resultConstraints.size() + patch->m_operands.size()), usedRegisters, besideOperands.gpScratch, besideOperands.fpScratch };

        const uint32_t indexOfStep = m_allocation.m_steps.size();
        Step step;
        step.kind = StepKind::Execute;
        step.patchAssignment = indexOfAssignment;
        // No location where there is no result.
        step.destination = locationOfResult;
        step.operation = patch;
        appendStep(step);

        if (patch->effects.terminal) {
            BlockSteps& steps = m_allocation.m_blocks[patch->owner()->index()];
            steps.end = indexOfStep;
            steps.terminator = indexOfStep;
            return true;
        }

        // appendStepsBehindPatchResults(), for a result that lies in a register.
        if (needsPointerBesideResult) [[unlikely]]
            appendStepsBehindPatchResult(*this, patch, locationOfResult);
        return true;
    };
    if (allocatePlainPatch()) [[likely]]
        return;

    const bool isCheck = patch->opcode() == Check;
    // A Check ends no block.
    const bool endsBlock = !isCheck && patch->effects.terminal;
    ASSERT(endsBlock == patch->endsBlock());
    // allocateBlock() has set the position of the walk to the one at which the operation that it hands over reads.
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
    // PatchOperation::registerToBringOperandTo() names a register for no operand from this one on.
    const unsigned endOfOperandsBroughtToRegisters = std::min<unsigned>(patch->m_registersToBringOperandsTo.size(), numberOfChildren);
    const Reg* const registersToBringOperandsTo = patch->m_registersToBringOperandsTo.begin();
    const unsigned numberOfResults = m_proc.resultCount(type);
    const RegisterMask allocatable = m_file.allocatableIn(Bank::GP) | m_file.allocatableIn(Bank::FP);
    const RegisterMask clobberedAsAppended = patch->earlyClobberedMask() | patch->lateClobberedMask();
    RegisterMask clobbered = clobberedAsAppended & allocatable;

    // registersOfFixedPatchResults(), and whether placedConstraintOfPatchResult() says SomeEarlyRegister of a result.
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

    // One look at the operands says which of the steps below have anything to do. None of what it asks changes while the Patch is allocated.
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

    // A suspension hands the registers that it does not keep back to whoever resumes. A function that names none of them has nothing of its caller's to save.
    RegisterMask written = clobberedAsAppended;
    if (endsBlock && !patch->m_successorClobbers.isEmpty()) {
        // PatchOperation::lateClobberedOnSuccessor() of each successor: clobberLateOnSuccessor() keeps one entry for a successor.
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
        // A frame that covers rare regions alone saves no register.
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

    // PatchOperation::leaveOperandsWhereTheyLie(). Whatever the generator writes over an operand that lies in its register is the operand.
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

    // Those that take a register come first: `locked` then names all of their registers when an operand that may lie anywhere is asked whether it keeps its own.
    for (bool takesRegister : { true, false }) {
        if (!(takesRegister ? someOperandTakesRegister : someOperandMayLieAnywhere))
            continue;
        for (unsigned child = firstChild; child < numberOfChildren && !m_proc.failure(); ++child) {
            ValueRep::Kind kind = placedKindOfPatchOperandUnder(constraints[child].kind(), onlyKeepsChildrenAlive, isCheck);
            // placeFixedOperands() has placed it.
            if (kind == ValueRep::Register || kind == ValueRep::StackArgument)
                continue;
            if (patchOperandTakesRegister(kind, children[child], onlyKeepsChildrenAlive) == takesRegister)
                placeOperandOfPatchUnder(*this, patch, kind, children[child], placesOfOperands[child - firstChild], locked);
        }
    }
    if (m_proc.failure()) [[unlikely]]
        return;

    // The generator is told one place of an operand. Told any other than the register where the operand belongs, it writes that register, which is not locked then, whatever lay there.
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
            m_allocation.m_holdsPatchThatCountsOnFrameRecord = true;
    }
    if (someOperandIsReadLate) {
        for (unsigned child = firstChild; child < numberOfChildren; ++child) {
            Location location = placesOfOperands[child - firstChild];
            if (patchOperandIsReadLate(placedKindOfPatchOperandUnder(constraints[child].kind(), onlyKeepsChildrenAlive, isCheck)) && location.isRegister())
                heldLate |= maskOf(location.reg());
        }
    }

    bool releasesItsChild = false;
    if ((bits & PatchOperation::ReleasesDeadValues) && !endsBlock && numberOfChildren == firstChild + 1) {
        Operation* child = children[firstChild];
        if (!child->hasNoPosition() && !isLiveBehind(child, late) && m_liveness.of(child).lastUse <= early) {
            releasesItsChild = true;
            patch->m_registersToRelease = stateOf(child).registers & m_file.allocatableIn(Bank::GP);
        }
    }
    if (!patch->m_childrenLetGoByCallee.isEmpty())
        releaseChildrenLetGoByCallee(patch, locationsOfOperands, early, locked);

    // An operation without children has no leaf.
    if (numberOfChildren)
        passUsesOf(patch, early);

    if (!registersMatterBehind) {
        // A landing pad is entered with every value in its slot, so no block takes the registers from here.
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

    // Whatever is bound is taken to keep its value, also what could be made again.
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
    if (releasesItsChild)
        releaseDeadValue(patch);
}

void Allocator::placeFixedOperands(PatchOperation* patch, std::span<Location> locationsOfOperands, RegisterMask& locked)
{
    // Each of its operands may lie anywhere. Of any other Patch placedKindOfPatchOperand() says Register or StackArgument just where the constraint does.
    if (patch->onlyKeepsChildrenAlive)
        return;

    const unsigned firstChild = firstPlacedChildOfPatch(patch);
    const unsigned numberOfChildren = patch->m_operands.size();
    ASSERT(patch->m_constraints.size() == numberOfChildren);
    Operation* const* const children = patch->m_operands.begin();
    const ValueRep* const constraints = patch->m_constraints.begin();
    Location* const placesOfOperands = locationsOfOperands.data();
    const RegisterMask allocatable = m_file.allocatableIn(Bank::GP) | m_file.allocatableIn(Bank::FP);

    // By Reg::index(). Only the entries of the registers in `wanted` are set.
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

    // As in allocatePatch().
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
    // The registers that the moves fill, by where the value comes from, and those that they read.
    RegisterMask filledFromRegisters = 0;
    RegisterMask filledFromElsewhere = 0;
    RegisterMask registersRead = 0;
    // By Reg::index() of the destination: the place of the move in m_pendingMoves. Only the entries of the registers that the moves fill are set.
    uint8_t placeOfMoveInto[64];
    // A move has no register as its destination or no source, or two of them read one source.
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
            // What resolveParallelMoves() makes of moves into registers none of which writes what another reads and no two of which read one source:
            // one step for each, those out of registers first, and each kind in the order of the destinations.
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

    // What is done to one register leaves the others as they are, so each is unbound and bound in one go.
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

void Allocator::placeResults(PatchOperation* patch, std::span<Location> locationsOfResults, RegisterMask locked)
{
    placeResultsOfPatch(*this, patch, m_allocation.m_registerAtDefinition, registersOfFixedPatchResults(patch), locationsOfResults, locked);
}

RegisterMask Allocator::takeScratchRegisters(Bank bank, unsigned count, RegisterMask& locked)
{
    return takeScratchRegistersInLine(*this, bank, count, locked);
}

static void clearMemoryOfReleasedValue(Allocator& allocator, Operation* value)
{
    auto clear = [&](SpillIndex spill) {
        if (spill == noSpill)
            return;
        Step& step = allocator.appendStep(StepKind::ClearSpill, value);
        step.width = allocator.m_allocation.spills()[spill].width;
        step.destination = Location::spill(spill);
        allocator.noteFrameUse();
    };
    clear(allocator.m_allocation.spillOf(value));
    clear(allocator.stateOf(value).saveInRareRegion);
}

RegisterMask Allocator::clearPlacesOfValuesHandedTo(Operation* released, Position position)
{
    if (released->opcode() != Phi)
        return 0;

    // Behind a suspension the frame holds what the suspension kept, and a range that reached over one would make it keep the word.
    std::span<PatchOperation* const> suspensions = m_allocation.suspensions();
    const Position lastSuspension = suspensions.empty() ? 0 : m_order.latePositionOf(suspensions.back());

    RegisterMask freed = 0;
    ArenaVector<Operation*> phis;
    phis.append(m_arena, released);
    for (uint32_t index = 0; index < phis.size(); ++index) {
        for (const Block* predecessor : m_order.of(phis[index]->owner()).predecessors) {
            for (Operation* upsilon : predecessor->upsilons()) {
                Operation* handedOn = upsilon->child(0);
                if (upsilon->phi() != phis[index] || handedOn->hasNoPosition())
                    continue;
                // What comes round a loop is made further on.
                const ValueFacts& facts = m_liveness.of(handedOn);
                if (facts.definition == noPosition || facts.definition >= position || facts.lastUse == noPosition)
                    continue;
                SpillIndex spill = m_allocation.spillOf(handedOn);
                bool isReadFurtherOn = facts.lastUse >= position;
                if (isReadFurtherOn) {
                    RegisterMask held = stateOf(handedOn).registers & m_file.allocatableIn(Bank::GP);
                    if ((spill == noSpill && !held) || isReadOnSomeWayOn(handedOn, position))
                        continue;
                    if (held && !isHeldAtHeadThatWayLeadsBackTo(handedOn)) {
                        unbindAll(handedOn);
                        freed |= held;
                    }
                }
                if (handedOn->opcode() == Phi && std::ranges::find(phis.span(), handedOn) == phis.span().end())
                    phis.append(m_arena, handedOn);

                if (spill == noSpill)
                    continue;
                SpillFacts& place = m_allocation.m_spills[spill];
                if (place.end < lastSuspension)
                    continue;
                // The other arms read the spill up to the end of its range.
                if (!isReadFurtherOn) {
                    if (place.end >= position)
                        continue;
                    place.end = position;
                }
                Step& step = appendStep(StepKind::ClearSpill, handedOn);
                step.width = place.width;
                step.destination = Location::spill(spill);
                noteFrameUse();
            }
        }
    }
    return freed;
}

bool Allocator::isHeldAtHeadThatWayLeadsBackTo(const Operation* value) const
{
    ASSERT(m_valueOfLastSearchForWays == value);
    std::span<const Loop> loops = m_order.loops();
    for (uint32_t loop = m_order.of(m_block).loop; loop != noLoop && m_liveness.of(value).definition < loops[loop].begin; loop = loops[loop].parent) {
        const Block* head = m_order.blocksInOrder()[loops[loop].firstBlock];
        if (m_lastSearchThatReachedBlock[head->index()] == m_numberOfSearchesForWays)
            continue;
        for (const RegisterBinding& binding : m_heads[head->index()].bindings) {
            if (binding.value == value)
                return true;
        }
    }
    return false;
}

bool Allocator::isReadOnSomeWayOn(const Operation* value, Position position)
{
    if (m_order.of(m_block).isRare() || m_liveness.of(value).has(ValueFlag::IsKeptForCollector))
        return true;
    const uint32_t firstUse = m_liveness.of(value).firstUse;
    for (uint32_t use = firstUse; use; use = m_liveness.use(use).next) {
        Position read = m_liveness.use(use).position;
        if (read >= position && read < m_order.endOf(m_block))
            return true;
    }

    if (m_valueOfLastSearchForWays != value) {
        m_valueOfLastSearchForWays = value;
        if (m_lastSearchThatReachedBlock.isEmpty())
            m_lastSearchThatReachedBlock.fill(m_arena, m_proc.numberOfBlocks(), 0);
        ++m_numberOfSearchesForWays;
        // A way into the block of the definition leads past the definition.
        ArenaVector<const Block*> reached;
        auto reach = [&](const Block* block) {
            uint32_t& last = m_lastSearchThatReachedBlock[block->index()];
            if (block == value->owner() || last == m_numberOfSearchesForWays)
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
    }
    return std::ranges::any_of(m_block->successors(), [&](const Edge& successor) {
        return m_lastSearchThatReachedBlock[successor.block()->index()] == m_numberOfSearchesForWays;
    });
}

// allocatePatch() has put the registers that held the child into m_registersToRelease, ahead of the pass over the uses that freed them.
void Allocator::releaseDeadValue(PatchOperation* patch)
{
    patch->m_registersToRelease |= registersWhereReleasedValueMayHaveWaited();
    patch->m_registersToRelease &= ~m_bound;
    Operation* released = patch->child(firstPlacedChildOfPatch(patch));
    clearMemoryOfReleasedValue(*this, released);
    // The assignment of the Patch, which is the last one, counts them as in use: they were bound when it was made.
    RegisterMask freed = clearPlacesOfValuesHandedTo(released, earlyPositionOfAllocatedPatch(*this, patch) + 1);
    patch->m_registersToRelease |= freed;
    m_allocation.m_patchAssignments.last().usedRegisters &= ~freed;
}

void Allocator::releaseChildrenLetGoByCallee(PatchOperation* patch, std::span<const Location> locationsOfOperands, Position early, RegisterMask locked)
{
    ASSERT(patch->opcode() == Patch);

    // The generator reads such an operand where it lies, whether or not the child is marked: an operand that is appended behind the arguments may be the same value.
    ArenaVector<const Operation*> readFromMemory;
    for (unsigned child = 0; child < patch->numChildren(); ++child) {
        if (locationsOfOperands[child].kind() == Location::Spill)
            readFromMemory.append(m_arena, patch->child(child));
    }

    // The child of an Upsilon counts as read where a Patch that ends the block reads its operands, and the moves of the edge read it behind the Patch.
    auto isHandedOnByEdge = [&](const Operation* value) {
        if (!patch->endsBlock())
            return false;
        for (const Operation* upsilon : patch->owner()->upsilons()) {
            if (upsilon->child(0) == value)
                return true;
        }
        return false;
    };

    for (unsigned child : patch->childrenLetGoByCallee()) {
        Operation* value = patch->child(child);
        if (value->hasNoPosition() || isLiveBehind(value, early + 1) || m_liveness.of(value).lastUse > early || isHandedOnByEdge(value))
            continue;
        patch->m_registersToRelease |= stateOf(value).registers & m_file.allocatableIn(Bank::GP) & ~locked;
        if (std::ranges::find(readFromMemory.span(), value) == readFromMemory.span().end())
            clearMemoryOfReleasedValue(*this, value);
        patch->m_registersToRelease |= clearPlacesOfValuesHandedTo(value, early) & ~locked;
        patch->m_registersToRelease |= registersWhereReleasedValueMayHaveWaited() & ~m_bound & ~locked;
    }
}

RegisterMask Allocator::registersWhereReleasedValueMayHaveWaited() const
{
    if (m_isAheadOfFramedPart)
        return 0;
    return m_allocation.usedCalleeSaves() & m_file.allocatableIn(Bank::GP);
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
