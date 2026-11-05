/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSweep.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTPlan.h"
#include "AOTStringTable.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint8_t laterBarrierCoversMarkOfSweep = static_cast<uint8_t>(SweepMark::LaterBarrierCovers);
static constexpr uint8_t ownerIsYoungMarkOfSweep = static_cast<uint8_t>(SweepMark::OwnerIsYoung);

// A callee checks the stack and polls where it sets up its frame, whatever its summary says of its body.
static constexpr OpcodeSet callsAmongSweptOperations = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        AccessClass accessClass = accessClassOf(opcode);
        if (accessClass == AccessClass::DirectCall || accessClass == AccessClass::IndirectCall)
            opcodes.add(opcode);
    }
    return opcodes;
}();

// The barrier of any other store remembers the stored element alone, so it stands for no other store, and its emitter omits it for no other barrier.
static constexpr OpcodeSet sweptStoresThatRememberOwner = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (runsOwnerBarrierOf(opcode) && remembersOwnerOf(opcode))
            opcodes.add(opcode);
    }
    return opcodes;
}();

constexpr OpcodeSet sweptOperationsWithOwnerBarriers = sweptStoresThatRememberOwner;

static constexpr OpcodeSet sweptConstantsThatMayExitSidewaysOrCollect = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        Effects bound = declaredBoundOf(opcode);
        if (bound.mayCollect || bound.mayExitSideways())
            opcodes.add(opcode);
    }
    return opcodes;
}();

static constexpr TerminatorKindSet sweptTerminatorsThatMayExitSidewaysOrCollect = [] {
    TerminatorKindSet kinds;
    for (TerminatorKind kind : allMembersOf<TerminatorKind>) {
        Effects bound = declaredBoundOf(kind);
        if (bound.mayCollect || bound.mayExitSideways())
            kinds.add(kind);
    }
    return kinds;
}();

// A constant lies in no block and has no traits: it is made anew where it is used.
static bool makingSweptConstantsMayExitSidewaysOrCollect(const Code& code, std::span<const ValueID> used)
{
    for (ValueID id : used) {
        if (!id)
            continue;
        const Operation& constant = code.at(id);
        if (constant.blockIndex != Operation::noBlock)
            continue;
        if (sweptConstantsThatMayExitSidewaysOrCollect.contains(constant.opcode) || makingSweptConstantsMayExitSidewaysOrCollect(code, constant.operands()))
            return true;
    }
    return false;
}

// An operation whose allocation fails throws when it returns, so whatever may collect may exit sideways as well.
static bool sweptOperationMayExitSidewaysOrCollect(const Code& code, const Operation& operation)
{
    EffectTraits traits = operation.traits();
    if (traits.mayExitSideways() || traits.mayCollect() || callsAmongSweptOperations.contains(operation.opcode))
        return true;
    // Its failing side does the generic operation.
    if (testsItsOwnGuess(operation))
        return true;
    // A store that adds a field cannot throw when its allocation fails. This poll throws for the stores in front of it.
    if (operation.opcode == Opcode::LoopHint && operation.throwsOwedError())
        return true;
    return makingSweptConstantsMayExitSidewaysOrCollect(code, operation.operands());
}

static bool sweptTerminatorMayThrowToHandler(const Code& code, const Terminator& terminator)
{
    if (sweptTerminatorsThatMayExitSidewaysOrCollect.contains(terminator.kind))
        return true;
    if (makingSweptConstantsMayExitSidewaysOrCollect(code, terminator.operands()))
        return true;
    for (uint32_t targetIndex = 0; targetIndex < terminator.numberOfEdges; ++targetIndex) {
        if (makingSweptConstantsMayExitSidewaysOrCollect(code, code.at(terminator.edge(targetIndex)).arguments()))
            return true;
    }
    return false;
}

// The block has just been reached, and none of its operations is known to be needed.
void Sweep::giveUpHandlerOf(BlockID id)
{
    Code& code = *m_code;
    if (!m_hasGivenUpHandlers) {
        m_hasGivenUpHandlers = true;
        m_handlersLookedAt.clearAll();
        m_handlersLookedAt.ensureSize(code.numberOfBlocks());
        m_handlersThatOnlyThrowOutOfUnit.clearAll();
        m_handlersThatOnlyThrowOutOfUnit.ensureSize(code.numberOfBlocks());
    }
    code.m_blocksWithoutThrows.quickSet(id.index());
    if (sweptTerminatorMayThrowToHandler(code, code.at(id).terminator))
        keepHandlerOf(id);
}

bool Sweep::handlerOnlyThrowsOutOfUnit(BlockID handler)
{
    const Code& code = *m_code;
    // A handler block that comes round to itself finds the answer no.
    if (m_handlersLookedAt.quickSet(handler.index()))
        return m_handlersThatOnlyThrowOutOfUnit.quickGet(handler.index());

    ValueID caught;
    Vector<EdgeID, 4> way;
    BlockID id = handler;
    for (;;) {
        const Block& block = code.at(id);
        if (block.subject)
            return false;
        if (block.handler && !handlerOnlyThrowsOutOfUnit(code.at(block.handler).to))
            return false;
        for (ValueID value = code.lastOperationOf(id); value; value = code.at(value).previous) {
            const Operation& operation = code.at(value);
            if (operation.opcode == Opcode::Parameter)
                continue;
            ++m_totals.operationsSwept;
            if (id == handler && catchOpcodes.contains(operation.opcode)) {
                if (caught)
                    return false;
                caught = value;
                continue;
            }
            // Whatever else stands here is read by nothing that stays: no block behind the handler block is reached but through these, and the last of them reads what was caught.
            if (isRoot(operation))
                return false;
        }
        if (block.terminator.kind != TerminatorKind::Jump)
            break;
        EdgeID next = block.terminator.next();
        // Numbers that rise come to an end.
        if (code.at(next).to <= id)
            return false;
        way.append(next);
        id = code.at(next).to;
    }
    if (!caught)
        return false;

    const Terminator& terminator = code.at(id).terminator;
    ValueID thrown;
    if (terminator.kind == TerminatorKind::Rethrow && code.at(caught).opcode == Opcode::CatchException)
        thrown = terminator.exceptionOperand();
    // A `catch` of the program that throws the value again begins another throw, whose frames whoever catches it outside may list.
    else if (terminator.kind == TerminatorKind::Throw && code.at(caught).opcode == Opcode::CatchValue && code.at(handler).role != BlockRole::CatchHandler)
        thrown = terminator.valueOperand();
    for (size_t index = way.size(); index-- && thrown;) {
        const Edge& edge = code.at(way[index]);
        std::span<const ValueID> parameters = code.at(edge.to).parameters();
        size_t position = std::ranges::find(parameters, thrown) - parameters.begin();
        if (position < parameters.size())
            thrown = position < edge.arguments().size() ? edge.arguments()[position] : ValueID();
    }
    if (thrown != caught)
        return false;

    m_handlersThatOnlyThrowOutOfUnit.quickSet(handler.index());
    return true;
}

void Sweep::keepHandlerOf(BlockID id)
{
    Code& code = *m_code;
    if (handlerOnlyThrowsOutOfUnit(code.at(code.at(id).handler).to))
        return;
    code.m_blocksWithoutThrows.quickClear(id.index());
    EdgeID handler = code.at(id).handler;
    BlockID to = code.at(handler).to;
    RELEASE_ASSERT(to);
    needArgumentsOn(handler);
    if (!code.isReached(to))
        m_blocksToReach.append(to);
}

// Its block has given its handler up.
void Sweep::keepHandlerIfThrows(const Operation& needed)
{
    if (sweptOperationMayExitSidewaysOrCollect(*m_code, needed))
        keepHandlerOf(needed.block());
}

namespace {

struct BarrierOfSweptStore {
    // The root of the cell that is stored into. Null: the store waits for no later barrier and stands for no earlier store.
    ValueID owner;
    // Whatever the store meets at run time, it ends in the barrier that remembers the owner.
    bool alwaysRuns { false };
};

} // anonymous namespace

ValueID rootOfCellInSweptCode(const Code& code, ValueID cell)
{
    while (opcodesThatGiveSameCell.contains(code.at(cell).opcode)) {
        ASSERT(code.at(cell).operandUses(0).contains(OperandUse::Returns));
        cell = code.at(cell).operand(0);
    }
    return cell;
}

static bool sweptStoreMayAddField(const Operation& store)
{
    return store.opcode == Opcode::PutSlotOrAdd;
}

// `store` is one of sweptStoresThatRememberOwner and neither exits sideways nor collects.
// alwaysRuns claims no more than BarrierEmitter does: each reason for which barrierAfterStore() or the emitter of a slot store omits the barrier, or tests before it runs, is named here.
static BarrierOfSweptStore describeBarrierOfSweptStore(const Code& code, const TypeArena& types, const Operation& store)
{
    if (store.ownerCannotBeMarked())
        return { };

    std::optional<uint8_t> ownerIndex;
    bool hasOneOwner = true;
    bool everyStoredTypeIsKnown = true;
    bool storesCells = false;
    bool mayOmitBarrier = false;
    store.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (retainedInOf(store.opcode, operandIndex) != RetainedIn::Operand)
            return;
        std::optional<uint8_t> retainingIndex = retainingOperandOf(store.opcode, operandIndex);
        hasOneOwner &= retainingIndex && (!ownerIndex || ownerIndex == retainingIndex);
        ownerIndex = retainingIndex;

        const Operation& stored = code.at(operand);
        if (!stored.type) {
            everyStoredTypeIsKnown = false;
            return;
        }
        // The VM marks some of its strings whoever holds them, and a store of those alone runs no barrier: BarrierEmitter::storesOnlyStringsOfVM().
        bool isStringOfVM = stored.opcode == Opcode::String && hasCellOfVM(types.bytesOf(stored.string()));
        storesCells |= !isStringOfVM && types.at(stored.type).hasCells();
        // The word of a literal string is tested first, and names a cell only sometimes.
        mayOmitBarrier |= stored.representation == Representation::LiteralString;
        // The slot names the function already, so the store writes no cell.
        const Operation& made = code.at(rootOfCellInSweptCode(code, operand));
        mayOmitBarrier |= made.opcode == Opcode::NewClosure && made.closureForm() == ClosureForm::Deferred;
    });
    if (!ownerIndex || !hasOneOwner || !everyStoredTypeIsKnown)
        return { };
    // A store that makes room for its value stores the Extension in the owner, whatever the value is.
    if (!storesCells && !sweptStoreMayAddField(store))
        return { };

    ValueID ownerOperand = store.operand(*ownerIndex);
    if (!ownerOperand)
        return { };
    ValueID owner = rootOfCellInSweptCode(code, ownerOperand);
    // Two uses of one constant may make two cells.
    if (code.isConstant(owner))
        return { };
    // No store into a cell of the frame runs a barrier.
    const Operation& definition = code.at(owner);
    if (PayloadWord::storage(definition.opcode) && definition.storage() != AllocationStorage::Heap)
        return { };
    return { owner, storesCells && !mayOmitBarrier };
}

// The collector looks at a remembered owner again as a whole. So the last barrier on a way on which nothing can exit sideways or collect does for every store into that owner on the way.
// An operation that may throw ends the way like one that may collect: behind the throw the later barrier never runs.
// False: the unit holds no store that could wait for another.
bool Sweep::beginLookingForCoveredBarriers()
{
    const Code& code = *m_code;
    if (!code.opcodesOfValues().intersects(sweptStoresThatRememberOwner))
        return false;

    m_coveredOwners.shrink(0);
    m_ownersCovered.clearAll();
    m_ownersCovered.ensureSize(code.numberOfValues());
    m_coveredAtHeads.shrink(0);
    m_storesThatAwaitBirth.shrink(0);
    m_birthsAwaited.clearAll();
    m_birthsAwaited.ensureSize(code.numberOfValues());
    m_firstCoveredAtHead.fill(0, code.numberOfBlocks());
    m_numberCoveredAtHead.fill(0, code.numberOfBlocks());
    return true;
}

void Sweep::strikeCoveredOwner(ValueID owner)
{
    if (!m_ownersCovered.quickGet(owner.index()))
        return;
    m_ownersCovered.quickClear(owner.index());
    m_coveredOwners.removeFirst(owner);
}

void Sweep::strikeCoveredOwners()
{
    for (ValueID owner : m_coveredOwners)
        m_ownersCovered.quickClear(owner.index());
    m_coveredOwners.shrink(0);
}

// What is covered at the head of every block that the terminator leads to. A block that has not been visited lies behind a back edge and counts as one that covers nothing.
// The handler edge needs no look: whatever may take it ends the way by itself.
void Sweep::beginCoverageAtEndOf(BlockID id)
{
    const Code& code = *m_code;
    ASSERT(m_coveredOwners.isEmpty());
    const Terminator& terminator = code.at(id).terminator;
    // Behind a terminator that leaves the unit no barrier of the unit runs.
    if (!terminator.numberOfEdges || m_coveredAtHeads.isEmpty())
        return;

    auto coveredAtHeadOfTarget = [&](uint32_t targetIndex) {
        uint32_t target = code.at(terminator.edge(targetIndex)).to.index();
        return m_coveredAtHeads.span().subspan(m_firstCoveredAtHead[target], m_numberCoveredAtHead[target]);
    };
    std::span<const ValueID> coveredAtHeadOfFirstTarget = coveredAtHeadOfTarget(0);
    if (coveredAtHeadOfFirstTarget.empty() || sweptTerminatorMayThrowToHandler(code, terminator))
        return;
    for (ValueID owner : coveredAtHeadOfFirstTarget) {
        bool isCoveredOnEveryWay = true;
        for (uint32_t targetIndex = 1; targetIndex < terminator.numberOfEdges && isCoveredOnEveryWay; ++targetIndex) {
            std::span<const ValueID> covered = coveredAtHeadOfTarget(targetIndex);
            isCoveredOnEveryWay = std::ranges::find(covered, owner) != covered.end();
        }
        if (!isCoveredOnEveryWay)
            continue;
        m_ownersCovered.quickSet(owner.index());
        m_coveredOwners.append(owner);
    }
}

// `operation` is needed. The operations of a block come last to first.
void Sweep::lookForCoveredBarrier(ValueID id, const Operation& operation)
{
    Code& code = *m_code;
    bool isStore = sweptStoresThatRememberOwner.contains(operation.opcode);
    if (!isStore && m_coveredOwners.isEmpty())
        return;
    if (sweptOperationMayExitSidewaysOrCollect(code, operation)) {
        strikeCoveredOwners();
        return;
    }
    // In front of its definition the value names the cell of another turn of a loop. A store may give a value too: PutInternalField.
    strikeCoveredOwner(id);
    if (!isStore)
        return;

    BarrierOfSweptStore barrier = describeBarrierOfSweptStore(code, m_inputs->types, operation);
    if (!barrier.owner)
        return;
    if (m_ownersCovered.quickGet(barrier.owner.index())) {
        code.m_marks[id.index()] |= laterBarrierCoversMarkOfSweep;
        return;
    }
    if (barrier.alwaysRuns) {
        m_ownersCovered.quickSet(barrier.owner.index());
        m_coveredOwners.append(barrier.owner);
    }
}

// The run time allocates nothing behind the cell of any of them, and hands it on new or remembered: newCellForCompiledCode().
static constexpr OpcodeSet sweptAllocationsThatLeaveYoungCell = opcodesWhere([](Opcode opcode) {
    return allocatesAfterLastSafepointOf(opcode) && interpretedOf(opcode) == Interpretation::Allocation && effectRuleOf(opcode) == EffectRule::Declared;
});

static bool sweptAllocationLeavesYoungCell(const Plan& plan, const Operation& made)
{
    if (!sweptAllocationsThatLeaveYoungCell.contains(made.opcode))
        return false;
    if (PayloadWord::storage(made.opcode) && made.storage() != AllocationStorage::Heap)
        return false;
    if (made.opcode != Opcode::NewObject)
        return true;
    const PlannedLayout& layout = plan.layout(made.layout());
    // An environment cell that was handed back is an old one, which the collector may have marked.
    if (layout.isEscapedOffset)
        return false;
    // The run time makes an object with a prototype of its own, and an ordinary object, in several steps.
    return !made.prototypeOperand() && layout.hasSlots();
}

// What only throws ends nothing: behind the throw the store does not run.
static bool sweptOperationMayCollectOrRunOtherCode(const Code& code, const Operation& operation)
{
    EffectTraits traits = operation.traits();
    if (traits.mayCollect() || traits.mayRunOtherCode() || callsAmongSweptOperations.contains(operation.opcode))
        return true;
    // Its failing side does the generic operation. A poll stops for the collector.
    if (testsItsOwnGuess(operation) || operation.opcode == Opcode::LoopHint)
        return true;
    return makingSweptConstantsMayExitSidewaysOrCollect(code, operation.operands());
}

// Whatever the effects say: an operation that writes nothing may retain. What hands its operand on under a name that rootOfCellInSweptCode() does not follow back counts as one that keeps it: whoever lays that name in memory lays the cell there.
static constexpr OperandUses usesThatLetSweptCellBeSeen { OperandUse::Retains, OperandUse::Calls, OperandUse::PassesToUserCode };
static constexpr OperandUses usesThatLetSweptCellBeSeenOrGiveAnotherName { OperandUse::Retains, OperandUse::Calls, OperandUse::PassesToUserCode, OperandUse::Returns };

static constexpr OperandUses usesThatEndYouthOfOperandOf(Opcode opcode)
{
    return opcodesThatGiveSameCell.contains(opcode) ? usesThatLetSweptCellBeSeen : usesThatLetSweptCellBeSeenOrGiveAnotherName;
}

static bool sweptOperationMayEndYouthOfOperand(Opcode opcode)
{
    OperandUses uses = usesThatEndYouthOfOperandOf(opcode);
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + hasVariadicOperand(opcode);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        if (operandInfoOf(opcode, operandIndex).uses.containsAny(uses))
            return true;
    }
    return false;
}

void Sweep::forgetStoresThatAwaitBirth()
{
    for (const StoreThatAwaitsBirth& waiting : m_storesThatAwaitBirth)
        m_birthsAwaited.quickClear(waiting.owner.index());
    m_storesThatAwaitBirth.shrink(0);
}

// The collector marks a cell that it finds in the frame only where the program stops for it, and a cell that it finds in memory at any time. So of a cell that was made behind the last place where the program may stop,
// and that no memory and no unfollowed name holds, it has marked nothing: a store into it needs no barrier. dfg/DFGStoreBarrierInsertionPhase.cpp has the same two halves.
// Where the heap is full one collection may run behind the cell all the same. The cell is remembered then: newCellForCompiledCode().
// `operation` is needed. The operations of a block come last to first, so a store waits until the way comes to the allocation, and whatever ends the youth of the cell on the way there ends the wait.
// No further than the block: at a head several ways meet, and the way backward follows one.
void Sweep::lookForBirthOfOwner(ValueID id, const Operation& operation)
{
    Code& code = *m_code;
    bool mayCollect = sweptOperationMayCollectOrRunOtherCode(code, operation);
    if (!m_storesThatAwaitBirth.isEmpty()) {
        if (m_birthsAwaited.quickGet(id.index())) {
            m_birthsAwaited.quickClear(id.index());
            m_storesThatAwaitBirth.removeAllMatching([&](const StoreThatAwaitsBirth& waiting) {
                if (waiting.owner != id)
                    return false;
                code.m_marks[waiting.store.index()] |= ownerIsYoungMarkOfSweep;
                return true;
            });
        }
        // An allocation may collect in front of its cell, so no older cell is young behind it.
        if (mayCollect) {
            forgetStoresThatAwaitBirth();
            return;
        }
        OperandUses uses = usesThatEndYouthOfOperandOf(operation.opcode);
        operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (!operation.operandUses(operandIndex).containsAny(uses) || code.isConstant(operand))
                return;
            ValueID seen = rootOfCellInSweptCode(code, operand);
            if (!m_birthsAwaited.quickGet(seen.index()))
                return;
            m_birthsAwaited.quickClear(seen.index());
            m_storesThatAwaitBirth.removeAllMatching([&](const StoreThatAwaitsBirth& waiting) {
                return waiting.owner == seen;
            });
        });
    }
    if (mayCollect || !sweptStoresThatRememberOwner.contains(operation.opcode))
        return;
    ValueID owner = describeBarrierOfSweptStore(code, m_inputs->types, operation).owner;
    if (!owner)
        return;
    const Operation& made = code.at(owner);
    if (made.blockIndex != operation.blockIndex || !sweptAllocationLeavesYoungCell(m_inputs->plan, made))
        return;
    m_birthsAwaited.quickSet(owner.index());
    m_storesThatAwaitBirth.append({ id, owner });
}

void Sweep::endCoverageAtHeadOf(BlockID id)
{
    for (ValueID parameter : m_code->at(id).parameters())
        strikeCoveredOwner(parameter);
    m_firstCoveredAtHead[id.index()] = static_cast<uint32_t>(m_coveredAtHeads.size());
    m_numberCoveredAtHead[id.index()] = static_cast<uint32_t>(m_coveredOwners.size());
    m_coveredAtHeads.appendVector(m_coveredOwners);
    strikeCoveredOwners();
}

static constexpr uint8_t laterFenceCoversMarkOfSweep = static_cast<uint8_t>(SweepMark::LaterFenceCovers);

// The allocations whose code describeFenceOfSweptOperation() knows. Any other one keeps its fence and stands for no earlier one.
constexpr OpcodeSet sweptAllocationsWithFences { Opcode::NewObject, Opcode::NewClosure, Opcode::NewArray, Opcode::CreateRest };

// By the schema it writes the state of the allocator alone, and what it keeps of its operands it keeps in its result. So a cell that it hands on is one that it has made, and whoever makes a cell fences in front of handing it on.
// The state of the run time is no such heap.
static bool sweptOperationHandsOnNoOperand(Opcode opcode)
{
    static constexpr OperandUses usesThatHandNothingOn { OperandUse::Inspects, OperandUse::Returns, OperandUse::ReadsContents, OperandUse::Retains };
    if (!declaredBoundInTable(opcode).writes.containsOnly(AbstractHeapSet { AbstractHeap::AllocatorState }))
        return false;
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + hasVariadicOperand(opcode);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        const OperandInfo& info = operandInfoOf(opcode, operandIndex);
        if (!info.protocols.isEmpty() || !info.uses.containsOnly(usesThatHandNothingOn))
            return false;
        if (info.uses.contains(OperandUse::Retains) && info.retainedIn != RetainedIn::Result)
            return false;
    }
    return true;
}

namespace {

struct FenceOfSweptOperation {
    // It may hand a cell on to where the collector finds it, or leave the way for code that may.
    bool mayHandCellOn { false };
    // Whatever it meets at run time, it fences behind the stores that fill its new cell, and hands no cell on in front of that fence.
    bool alwaysFences { false };
    // It hands no cell on at all, and wherever its code fences in line its emitter asks for the mark: BarrierEmitter::fenceAfterInitialization(const OperationContext&).
    bool mayLeaveFenceToLaterOne { false };
};

} // anonymous namespace

// A fence keeps the stores that fill a new cell in front of the store that hands the cell on, which the collector may follow while the program runs. It keeps every earlier store of the thread there too.
// So the last of several allocations fences for all of them, as long as nothing in between hands a cell on.
// A store into a cell that is older than the allocation does: whatever may collect in between may have handed the older cell to the collector.
// What an allocation stores into its own new cell does not: that cell reaches the collector behind the later fence, or where the program stops for the collector, which orders all that stands in front.
// A constant, which is made where it is used, names no value of the unit: it hands on at most a cell that it has made and fenced itself.
// An allocation that throws for lack of memory makes the error behind the stores, and whoever makes a cell fences.
static FenceOfSweptOperation describeFenceOfSweptOperation(const Program& program, const Plan& plan, const Operation& operation)
{
    EffectTraits traits = operation.traits();
    if (!traits.isRemovableWhenUnused() || callsAmongSweptOperations.contains(operation.opcode) || testsItsOwnGuess(operation))
        return { .mayHandCellOn = true };

    switch (operation.opcode) {
    case Opcode::NewObject: {
        bool fences = newObjectFencesBeforeHandingCellOn(program, plan, operation);
        // The run time takes the prototype. A cell of the heap of which the emitter does not say so may be one that the collector knows. A cell of the frame is filled where the collector does not look.
        if (operation.prototypeOperand() || (!fences && operation.storage() == AllocationStorage::Heap))
            return { .mayHandCellOn = true, .alwaysFences = fences };
        return { .alwaysFences = fences, .mayLeaveFenceToLaterOne = fences };
    }
    case Opcode::NewClosure: {
        // The fence stands in line, in the stub, or in the operation behind the stub: ConstantEmitter::newFullClosure(), generateNewClosure(), generateNewSlimClosure(), operationAOTNewClosureSlow.
        // A singleton is made once, and a deferred closure is not made here.
        bool isMadeInHeap = operation.storage() == AllocationStorage::Heap && (operation.closureForm() == ClosureForm::Full || operation.closureForm() == ClosureForm::Slim);
        return { .alwaysFences = isMadeInHeap, .mayLeaveFenceToLaterOne = isMadeInHeap };
    }
    case Opcode::NewArray:
        return { .alwaysFences = newArrayFencesBeforeHandingCellOn(operation), .mayLeaveFenceToLaterOne = operation.storage() == AllocationStorage::Heap };
    case Opcode::CreateRest:
        return { .mayLeaveFenceToLaterOne = operation.storage() == AllocationStorage::Heap };
    default: {
        // What writes where no program sees may still write where the collector looks.
        bool allocatesOrWrites = traits.mayCollect() || (traits.mayTouchMemory() && !declaredBoundInTable(operation.opcode).writes.isEmpty());
        return { .mayHandCellOn = allocatesOrWrites && !sweptOperationHandsOnNoOperand(operation.opcode) };
    }
    }
}

// False: the unit holds no allocation whose fence could wait for another or stand for one.
bool Sweep::beginLookingForCoveredFences()
{
    const Code& code = *m_code;
    if (!code.opcodesOfValues().intersects(sweptAllocationsWithFences))
        return false;
    m_laterFenceCovers = false;
    m_laterFenceCoversAtSomeHead = false;
    m_laterFenceCoversAtHeads.clearAll();
    m_laterFenceCoversAtHeads.ensureSize(code.numberOfBlocks());
    return true;
}

// A block that has not been visited lies behind a back edge and counts as one that covers nothing. Behind a terminator that leaves the unit no fence of the unit runs.
void Sweep::beginFenceCoverageAtEndOf(BlockID id)
{
    const Code& code = *m_code;
    const Terminator& terminator = code.at(id).terminator;
    m_laterFenceCovers = !!terminator.numberOfEdges;
    for (uint32_t targetIndex = 0; targetIndex < terminator.numberOfEdges && m_laterFenceCovers; ++targetIndex)
        m_laterFenceCovers = m_laterFenceCoversAtHeads.quickGet(code.at(terminator.edge(targetIndex)).to.index());
    m_laterFenceCovers = m_laterFenceCovers && !sweptTerminatorMayThrowToHandler(code, terminator);
}

// `operation` is needed. The operations of a block come last to first.
void Sweep::lookForCoveredFence(ValueID id, const Operation& operation)
{
    if (!m_laterFenceCovers && !sweptAllocationsWithFences.contains(operation.opcode))
        return;
    FenceOfSweptOperation fence = describeFenceOfSweptOperation(m_inputs->program, m_inputs->plan, operation);
    if (m_laterFenceCovers && fence.mayLeaveFenceToLaterOne) {
        m_code->m_marks[id.index()] |= laterFenceCoversMarkOfSweep;
        return;
    }
    if (fence.alwaysFences)
        m_laterFenceCovers = true;
    else if (fence.mayHandCellOn)
        m_laterFenceCovers = false;
}

void Sweep::endFenceCoverageAtHeadOf(BlockID id)
{
    if (!m_laterFenceCovers)
        return;
    m_laterFenceCoversAtSomeHead = true;
    m_laterFenceCoversAtHeads.quickSet(id.index());
}

// An operation that is no store and ends no way is looked at for being an owner alone. A store into an owner needs the owner, and stands behind it: so the way has found the owner needed where it came by it.
bool Sweep::mayChangeWhatIsCovered(const Operation& operation) const
{
    if (m_looksForCoveredBarriers) {
        if (sweptStoresThatRememberOwner.contains(operation.opcode) || sweptOperationMayExitSidewaysOrCollect(*m_code, operation))
            return true;
        if (sweptOperationMayEndYouthOfOperand(operation.opcode) || operation.opcode == Opcode::LoopHint)
            return true;
    }
    if (!m_looksForCoveredFences)
        return false;
    return sweptAllocationsWithFences.contains(operation.opcode) || describeFenceOfSweptOperation(m_inputs->program, m_inputs->plan, operation).mayHandCellOn;
}

void Sweep::lookForCoverageAgain()
{
    Code& code = *m_code;
    for (uint8_t& marks : code.m_marks)
        marks &= static_cast<uint8_t>(~(laterBarrierCoversMarkOfSweep | laterFenceCoversMarkOfSweep | ownerIsYoungMarkOfSweep));
    m_looksForCoveredBarriers = beginLookingForCoveredBarriers();
    m_looksForCoveredFences = beginLookingForCoveredFences();
    for (BlockID id : m_reachedInPostorder) {
        if (m_looksForCoveredBarriers)
            beginCoverageAtEndOf(id);
        if (m_looksForCoveredFences)
            beginFenceCoverageAtEndOf(id);
        std::span<const ValueID> order = code.valuesOf(id);
        for (size_t index = order.size(); index--;) {
            ++m_totals.operationsSwept;
            const Operation& operation = code.at(order[index]);
            if (m_looksForCoveredBarriers) {
                lookForCoveredBarrier(order[index], operation);
                if (!m_storesThatAwaitBirth.isEmpty() || sweptOperationsWithOwnerBarriers.contains(operation.opcode))
                    lookForBirthOfOwner(order[index], operation);
            }
            if (m_looksForCoveredFences)
                lookForCoveredFence(order[index], operation);
        }
        if (m_looksForCoveredBarriers)
            endCoverageAtHeadOf(id);
        forgetStoresThatAwaitBirth();
        if (m_looksForCoveredFences)
            endFenceCoverageAtHeadOf(id);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
