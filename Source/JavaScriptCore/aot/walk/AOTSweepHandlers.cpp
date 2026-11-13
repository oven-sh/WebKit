/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSweep.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTPlan.h"
#include "AOTStringTable.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint8_t laterBarrierCoversMarkOfSweep = static_cast<uint8_t>(SweepMark::LaterBarrierCovers);
static constexpr uint8_t ownerIsYoungMarkOfSweep = static_cast<uint8_t>(SweepMark::OwnerIsYoung);

static constexpr OpcodeSet callsAmongSweptOperations = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        AccessClass accessClass = accessClassOf(opcode);
        if (accessClass == AccessClass::DirectCall || accessClass == AccessClass::IndirectCall)
            opcodes.add(opcode);
    }
    return opcodes;
}();

static constexpr OpcodeSet sweptStoresThatRememberOwner = sweptOperationsWithOwnerBarriers;

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

static ALWAYS_INLINE bool makingSweptConstantsMayExitSidewaysOrCollect(const Code&, std::span<const ValueID>);

static NEVER_INLINE bool makingOperandsOfSweptConstantMayExitSidewaysOrCollect(const Code& code, const Operation& constant)
{
    return makingSweptConstantsMayExitSidewaysOrCollect(code, constant.operands());
}

static ALWAYS_INLINE bool makingSweptConstantsMayExitSidewaysOrCollect(const Code& code, std::span<const ValueID> used)
{
    for (ValueID id : used) {
        if (!id)
            continue;
        const Operation& constant = code.at(id);
        if (constant.blockIndex != Operation::noBlock)
            continue;
        if (sweptConstantsThatMayExitSidewaysOrCollect.contains(constant.opcode) || (!constant.operands().empty() && makingOperandsOfSweptConstantMayExitSidewaysOrCollect(code, constant)))
            return true;
    }
    return false;
}

static bool sweptOperationMayExitSidewaysOrCollect(const Code& code, const Operation& operation)
{
    EffectTraits traits = operation.traits();
    if (traits.mayExitSideways() || traits.mayCollect() || callsAmongSweptOperations.contains(operation.opcode))
        return true;
    if (testsItsOwnGuess(operation))
        return true;
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

void Sweep::giveUpHandlerOf(BlockID id, const Block& block)
{
    Code& code = *m_code;
    m_states[id.index()] |= sweptBlockHasGivenUpHandler;
    code.m_blocksWithoutThrows.quickSet(id.index());
    if (sweptTerminatorMayThrowToHandler(code, block.terminator))
        keepHandlerOf(id);
}

bool Sweep::handlerOnlyThrowsOutOfUnit(BlockID handler)
{
    const Code& code = *m_code;
    RELEASE_ASSERT(handler);
    uint8_t& stateOfHandler = m_states[handler.index()];
    if (stateOfHandler & sweptHandlerWasLookedAt)
        return stateOfHandler & sweptHandlerOnlyThrowsOutOfUnit;
    stateOfHandler |= sweptHandlerWasLookedAt;

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
            if (isRoot(operation))
                return false;
        }
        if (block.terminator.kind != TerminatorKind::Jump)
            break;
        EdgeID next = block.terminator.next();
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

    stateOfHandler |= sweptHandlerOnlyThrowsOutOfUnit;
    return true;
}

void Sweep::keepHandlerOf(BlockID id)
{
    Code& code = *m_code;
    EdgeID handler = code.at(id).handler;
    BlockID to = code.at(handler).to;
    RELEASE_ASSERT(to);
    if (handlerOnlyThrowsOutOfUnit(to)) {
        m_states[id.index()] |= sweptBlockNeedsNoHandler;
        return;
    }
    m_states[id.index()] &= static_cast<uint8_t>(~sweptBlockHasGivenUpHandler);
    code.m_blocksWithoutThrows.quickClear(id.index());
    needArgumentsOn(handler);
    if (!(m_states[to.index()] & sweptBlockIsReached))
        m_blocksToReach.append(to);
}

void Sweep::keepHandlerIfThrows(const Operation& needed)
{
    if (sweptOperationMayExitSidewaysOrCollect(*m_code, needed))
        keepHandlerOf(needed.block());
}

namespace {

struct BarrierOfSweptStore {
    ValueID owner;
    bool alwaysRuns { false };
};

} // anonymous namespace

static bool sweptStoreMayAddField(const Operation& store)
{
    return store.opcode == Opcode::PutSlotOrAdd;
}

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
        bool isStringOfVM = stored.opcode == Opcode::String && hasCellOfVM(types.bytesOf(stored.string()));
        storesCells |= !isStringOfVM && types.at(stored.type).hasCells();
        const Operation& made = code.at(underlyingCellOf(code, operand));
        mayOmitBarrier |= made.opcode == Opcode::NewClosure && made.closureForm() == ClosureForm::Deferred;
    });
    if (!ownerIndex || !hasOneOwner || !everyStoredTypeIsKnown)
        return { };
    if (!storesCells && !sweptStoreMayAddField(store))
        return { };

    ValueID ownerOperand = store.operand(*ownerIndex);
    if (!ownerOperand)
        return { };
    ValueID owner = underlyingCellOf(code, ownerOperand);
    if (code.isConstant(owner))
        return { };
    const Operation& definition = code.at(owner);
    if (PayloadWord::storage(definition.opcode) && definition.storage() != AllocationStorage::Heap)
        return { };
    return { owner, storesCells && !mayOmitBarrier };
}

static ValueID objectRequiredBySwept(const Code& code, const Operation& operation)
{
    if (operation.opcode != Opcode::RequireObjectCoercible || testsItsOwnGuess(operation) || callsAmongSweptOperations.contains(operation.opcode) || makingSweptConstantsMayExitSidewaysOrCollect(code, operation.operands()))
        return { };
    ValueID required = underlyingCellOf(code, operation.operand(OperandIndex::RequireObjectCoercible::value));
    return code.isConstant(required) ? ValueID() : required;
}

static bool sweptStoreStoresOnly(const Code& code, const Operation& store, ValueID value)
{
    if (store.opcode != Opcode::PutSlot)
        return false;
    bool storesOnlyValue = true;
    store.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (retainedInOf(store.opcode, operandIndex) == RetainedIn::Operand)
            storesOnlyValue &= underlyingCellOf(code, operand) == value;
    });
    return storesOnlyValue;
}

bool Sweep::beginLookingForCoveredBarriers()
{
    const Code& code = *m_code;
    if (!code.opcodesOfValues().intersects(sweptStoresThatRememberOwner))
        return false;

    m_coveredOwners.shrink(0);
    m_ownersCovered.clearAll();
    m_ownersCovered.ensureSize(code.numberOfValues());
    m_storeWhoseOwnerIsKnown = { };
    m_requiredObject = { };
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
    m_requiredObject = { };
}

void Sweep::beginCoverageAtEndOf(BlockID id)
{
    const Code& code = *m_code;
    ASSERT(m_coveredOwners.isEmpty());
    const Terminator& terminator = code.at(id).terminator;
    if (!terminator.numberOfEdges || m_coveredAtHeads.isEmpty())
        return;

    auto coveredAtHeadOfTarget = [&](uint32_t targetIndex) {
        uint32_t target = code.at(terminator.edge(targetIndex)).to.index();
        return m_coveredAtHeads.span().subspan(m_firstCoveredAtHead[target], m_numberCoveredAtHead[target]);
    };
    std::span<const ValueID> coveredAtHeadOfFirstTarget = coveredAtHeadOfTarget(0);
    if (coveredAtHeadOfFirstTarget.empty() || sweptTerminatorMayThrowToHandler(code, terminator))
        return;
    m_coveredOwners.append(coveredAtHeadOfFirstTarget);
    for (uint32_t targetIndex = 1; targetIndex < terminator.numberOfEdges && !m_coveredOwners.isEmpty(); ++targetIndex) {
        std::span<const ValueID> covered = coveredAtHeadOfTarget(targetIndex);
        for (ValueID owner : covered)
            m_ownersCovered.quickSet(owner.index());
        m_coveredOwners.removeAllMatching([&](ValueID owner) {
            return !m_ownersCovered.quickGet(owner.index());
        });
        for (ValueID owner : covered)
            m_ownersCovered.quickClear(owner.index());
    }
    for (ValueID owner : m_coveredOwners)
        m_ownersCovered.quickSet(owner.index());
}

void Sweep::lookForCoveredBarrier(ValueID id, const Operation& operation)
{
    Code& code = *m_code;
    bool isStore = sweptStoresThatRememberOwner.contains(operation.opcode);
    if (!isStore && m_coveredOwners.isEmpty())
        return;
    if (sweptOperationMayExitSidewaysOrCollect(code, operation)) {
        ValueID required = m_coveredOwners.isEmpty() ? ValueID() : objectRequiredBySwept(code, operation);
        if (!required || (m_requiredObject && m_requiredObject != required)) {
            strikeCoveredOwners();
            return;
        }
        m_requiredObject = required;
    }
    strikeCoveredOwner(id);
    if (!isStore)
        return;
    if (m_requiredObject && !sweptStoreStoresOnly(code, operation, m_requiredObject))
        strikeCoveredOwners();

    BarrierOfSweptStore barrier = describeBarrierOfSweptStore(code, m_inputs->types, operation);
    m_storeWhoseOwnerIsKnown = id;
    m_ownerOfThatStore = barrier.owner;
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
    if (layout.cellsAreHandedBack)
        return false;
    return !made.prototypeOperand() && layout.hasSlots();
}

static bool sweptOperationMayCollectOrRunOtherCode(const Code& code, const Operation& operation)
{
    EffectTraits traits = operation.traits();
    if (traits.mayCollect() || traits.mayRunOtherCode() || callsAmongSweptOperations.contains(operation.opcode))
        return true;
    if (testsItsOwnGuess(operation) || operation.opcode == Opcode::LoopHint)
        return true;
    return makingSweptConstantsMayExitSidewaysOrCollect(code, operation.operands());
}

static constexpr OperandUses usesThatLetSweptCellBeSeen { OperandUse::Retains, OperandUse::Calls, OperandUse::PassesToUserCode };
static constexpr OperandUses usesThatLetSweptCellBeSeenOrGiveAnotherName { OperandUse::Retains, OperandUse::Calls, OperandUse::PassesToUserCode, OperandUse::Returns };
static constexpr OperandUses usesThatLeaveSweptCellYoung {
    OperandUse::Inspects, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly, OperandUse::ReadsContents, OperandUse::WritesContents, OperandUse::RetainsContents,
    OperandUse::MovesElements, OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor,
    OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype, OperandUse::AppliesTransitively, OperandUse::AppliesToContents,
};
static_assert(everyOperandUseIsInExactlyOneOf({ usesThatLetSweptCellBeSeen, OperandUse::Returns, usesThatLeaveSweptCellYoung }));
static_assert(everyOperandUseIsInExactlyOneOf({ usesThatLetSweptCellBeSeenOrGiveAnotherName, usesThatLeaveSweptCellYoung }));

static constexpr OperandUses usesThatEndYouthOfOperandOf(Opcode opcode)
{
    return cellPreservingOpcodes.contains(opcode) ? usesThatLetSweptCellBeSeen : usesThatLetSweptCellBeSeenOrGiveAnotherName;
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

static BlockID soleJumpIntoSweptBlock(const Code& code, BlockID id)
{
    EdgeID only = code.at(id).firstEdgeIn();
    if (!only || code.at(only).nextInto() || !code.at(only).arguments().empty() || code.at(id).isHandlerBlock() || code.at(id).isEntry)
        return { };
    BlockID from = code.at(only).from;
    const Block& block = code.at(from);
    return block.terminator.kind == TerminatorKind::Jump && block.terminator.next() == only && !block.isHandlerBlock() ? from : BlockID();
}

static bool isSweptBlockReachedOnlyByJumps(const Code& code, BlockID from, BlockID id)
{
    static constexpr unsigned mostJumps = 8;
    for (unsigned jumps = 0; id && id != from && jumps < mostJumps; ++jumps)
        id = soleJumpIntoSweptBlock(code, id);
    return id == from;
}

void Sweep::awaitBirthsAheadOf(BlockID id)
{
    m_blockAwaitingAllocations = soleJumpIntoSweptBlock(*m_code, id);
    if (!m_blockAwaitingAllocations)
        forgetStoresThatAwaitBirth();
}

void Sweep::forgetStoresThatAwaitBirth()
{
    for (const StoreThatAwaitsBirth& waiting : m_storesThatAwaitBirth)
        m_birthsAwaited.quickClear(waiting.owner.index());
    m_storesThatAwaitBirth.shrink(0);
}

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
        if (mayCollect) {
            forgetStoresThatAwaitBirth();
            return;
        }
        OperandUses uses = usesThatEndYouthOfOperandOf(operation.opcode);
        operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (!operation.operandUses(operandIndex).containsAny(uses) || code.isConstant(operand))
                return;
            ValueID seen = underlyingCellOf(code, operand);
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
    ValueID owner = m_storeWhoseOwnerIsKnown == id ? m_ownerOfThatStore : describeBarrierOfSweptStore(code, m_inputs->types, operation).owner;
    if (!owner)
        return;
    const Operation& made = code.at(owner);
    if (!sweptAllocationLeavesYoungCell(m_inputs->plan, made) || !isSweptBlockReachedOnlyByJumps(code, made.block(), operation.block()))
        return;
    m_birthsAwaited.quickSet(owner.index());
    m_storesThatAwaitBirth.append({ id, owner });
}

void Sweep::endCoverageAtHeadOf(BlockID id)
{
    if (m_requiredObject)
        strikeCoveredOwners();
    for (ValueID parameter : m_code->at(id).parameters())
        strikeCoveredOwner(parameter);
    m_firstCoveredAtHead[id.index()] = static_cast<uint32_t>(m_coveredAtHeads.size());
    m_numberCoveredAtHead[id.index()] = static_cast<uint32_t>(m_coveredOwners.size());
    m_coveredAtHeads.appendVector(m_coveredOwners);
    strikeCoveredOwners();
}

static constexpr uint8_t laterFenceCoversMarkOfSweep = static_cast<uint8_t>(SweepMark::LaterFenceCovers);

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
    bool mayHandCellOn { false };
    bool alwaysFences { false };
    bool mayLeaveFenceToLaterOne { false };
};

} // anonymous namespace

// A fence keeps the stores that fill a new cell in front of the store that hands the cell on, which the collector may follow while the program runs. It keeps every earlier store of the thread there too.
static ALWAYS_INLINE FenceOfSweptOperation describeFenceOfSweptOperation(const Program& program, const Plan& plan, const Operation& operation)
{
    EffectTraits traits = operation.traits();
    if (!traits.isRemovableWhenUnused() || callsAmongSweptOperations.contains(operation.opcode) || testsItsOwnGuess(operation))
        return { .mayHandCellOn = true };

    switch (operation.opcode) {
    case Opcode::NewObject: {
        bool fences = newObjectFencesBeforeHandingCellOn(program, plan, operation);
        if (operation.prototypeOperand() || (!fences && operation.storage() == AllocationStorage::Heap))
            return { .mayHandCellOn = true, .alwaysFences = fences };
        return { .alwaysFences = fences, .mayLeaveFenceToLaterOne = fences };
    }
    case Opcode::NewClosure: {
        bool isMadeInHeap = operation.storage() == AllocationStorage::Heap && (operation.closureForm() == ClosureForm::Full || operation.closureForm() == ClosureForm::Slim);
        return { .alwaysFences = isMadeInHeap, .mayLeaveFenceToLaterOne = isMadeInHeap };
    }
    case Opcode::NewArray:
        return { .alwaysFences = newArrayFencesBeforeHandingCellOn(operation), .mayLeaveFenceToLaterOne = operation.storage() == AllocationStorage::Heap };
    case Opcode::CreateRest:
        return { };
    default: {
        bool allocatesOrWrites = traits.mayCollect() || (traits.mayTouchMemory() && !declaredBoundInTable(operation.opcode).writes.isEmpty());
        return { .mayHandCellOn = allocatesOrWrites && !sweptOperationHandsOnNoOperand(operation.opcode) };
    }
    }
}

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

void Sweep::beginFenceCoverageAtEndOf(BlockID id)
{
    const Code& code = *m_code;
    const Terminator& terminator = code.at(id).terminator;
    bool laterFenceCovers = !!terminator.numberOfEdges;
    for (uint32_t targetIndex = 0; targetIndex < terminator.numberOfEdges && laterFenceCovers; ++targetIndex)
        laterFenceCovers = m_laterFenceCoversAtHeads.quickGet(code.at(terminator.edge(targetIndex)).to.index());
    m_laterFenceCovers = laterFenceCovers && !sweptTerminatorMayThrowToHandler(code, terminator);
}

void Sweep::lookForCoveredFence(ValueID id, const Operation& operation)
{
    if (!m_laterFenceCovers && !sweptAllocationsWithFences.contains(operation.opcode))
        return;
    if (m_looksForBirthsToMove && sweptAllocationsWithFences.contains(operation.opcode))
        noteIfBirthMayMove(id, operation);
    FenceOfSweptOperation fence = describeFenceOfSweptOperation(m_inputs->program, m_inputs->plan, operation);
    if (m_lendsClosures && operation.opcode == Opcode::NewClosure && noteIfClosureMayBeLent(id) && fence.alwaysFences)
        return;
    if (m_lendsCells && operation.opcode == Opcode::NewObject && noteIfCellMayBeLent(id) && fence.alwaysFences)
        return;
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
    const bool looksForCoveredBarriers = m_looksForCoveredBarriers;
    const bool looksForCoveredFences = m_looksForCoveredFences;
    for (BlockID id : m_reachedInPostorder) {
        if (looksForCoveredBarriers && !m_coveredAtHeads.isEmpty())
            beginCoverageAtEndOf(id);
        if (looksForCoveredFences) {
            if (m_laterFenceCoversAtSomeHead)
                beginFenceCoverageAtEndOf(id);
            else
                m_laterFenceCovers = false;
        }
        if (!m_storesThatAwaitBirth.isEmpty() && m_blockAwaitingAllocations != id)
            forgetStoresThatAwaitBirth();
        std::span<const ValueID> order = code.valuesOf(id);
        for (size_t index = order.size(); index--;) {
            ++m_totals.operationsSwept;
            const Operation& operation = code.at(order[index]);
            if (looksForCoveredBarriers) {
                bool hasOwnerBarrier = sweptOperationsWithOwnerBarriers.contains(operation.opcode);
                if (hasOwnerBarrier || !m_coveredOwners.isEmpty())
                    lookForCoveredBarrier(order[index], operation);
                if (hasOwnerBarrier || !m_storesThatAwaitBirth.isEmpty())
                    lookForBirthOfOwner(order[index], operation);
            }
            if (looksForCoveredFences && (m_laterFenceCovers || sweptAllocationsWithFences.contains(operation.opcode)))
                lookForCoveredFence(order[index], operation);
        }
        if (looksForCoveredBarriers && !m_coveredOwners.isEmpty())
            endCoverageAtHeadOf(id);
        if (!m_storesThatAwaitBirth.isEmpty())
            awaitBirthsAheadOf(id);
        if (looksForCoveredFences && m_laterFenceCovers)
            endFenceCoverageAtHeadOf(id);
    }
}

namespace {

class MarksOfSweepAskedForward {
public:
    MarksOfSweepAskedForward(const SweepInputs& inputs, Code& code)
        : m_inputs(inputs)
        , m_code(code)
    {
    }

    String run()
    {
        orderBlocks();
        m_blocksWithPolls.ensureSize(m_code.numberOfBlocks());
        for (size_t place = m_blocksInPostorder.size(); place--;) {
            BlockID block = m_blocksInPostorder[place];
            if (String brokenRule = walk(block, m_awaitedAtHeads.take(block.index())); !brokenRule.isNull())
                return brokenRule;
        }
        return findWayRoundWithoutPoll();
    }

private:
    using StoresByOwner = HashMap<uint32_t, ValueID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

    struct Awaited {
        StoresByOwner storesByOwner;
        ValueID allocation;

        bool isEmpty() const { return storesByOwner.isEmpty() && !allocation; }
    };

    void orderBlocks()
    {
        struct Step {
            BlockID block;
            uint32_t nextTarget { 0 };
        };
        Vector<Step, 32> way;
        BitVector seen;
        seen.ensureSize(m_code.numberOfBlocks());
        m_placesInPostorder.fill(0, m_code.numberOfBlocks());
        for (uint32_t index = 0; index < m_code.numberOfBlocks(); ++index) {
            if (!m_code.isReached(BlockID(index)) || seen.quickSet(index))
                continue;
            way.append({ BlockID(index) });
            while (!way.isEmpty()) {
                Step& step = way.last();
                const Terminator& terminator = m_code.at(step.block).terminator;
                if (step.nextTarget >= terminator.numberOfEdges) {
                    m_placesInPostorder[step.block.index()] = static_cast<uint32_t>(m_blocksInPostorder.size());
                    m_blocksInPostorder.append(step.block);
                    way.removeLast();
                    continue;
                }
                BlockID target = m_code.at(terminator.edge(step.nextTarget++)).to;
                if (!seen.quickSet(target.index()))
                    way.append({ target });
            }
        }
    }

    String walk(BlockID block, Awaited&& awaited)
    {
        for (ValueID id : m_code.valuesOf(block)) {
            const Operation& operation = m_code.at(id);
            OptionSet<SweepMark> marks = m_code.marksOf(id);
            if (operation.opcode == Opcode::LoopHint)
                m_blocksWithPolls.quickSet(block.index());

            if (!awaited.storesByOwner.isEmpty() && ((sweptOperationMayExitSidewaysOrCollect(m_code, operation) && !storesOnlyRequiredObject(awaited, operation)) || awaited.storesByOwner.contains(id.index())))
                return sentenceOfBarrier(firstStoreOf(awaited));
            bool leavesBarrier = marks.contains(SweepMark::LaterBarrierCovers);
            if (leavesBarrier || (!awaited.storesByOwner.isEmpty() && sweptStoresThatRememberOwner.contains(operation.opcode))) {
                BarrierOfSweptStore barrier = describeBarrierOfSweptStore(m_code, m_inputs.types, operation);
                if (leavesBarrier && !barrier.owner)
                    return sentenceOfBarrier(id);
                if (leavesBarrier)
                    awaited.storesByOwner.add(barrier.owner.index(), id);
                else if (barrier.owner && barrier.alwaysRuns && awaited.storesByOwner.remove(barrier.owner.index()))
                    m_code.noteBarrierIsCountedOn(id);
            }

            if (marks.contains(SweepMark::LaterFenceCovers)) {
                if (!awaited.allocation)
                    awaited.allocation = id;
            } else if (awaited.allocation) {
                FenceOfSweptOperation fence = describeFenceOfSweptOperation(m_inputs.program, m_inputs.plan, operation);
                if (fence.alwaysFences)
                    awaited.allocation = { };
                else if (fence.mayHandCellOn)
                    return sentenceOfFence(awaited.allocation);
            }
        }
        if (awaited.isEmpty())
            return { };

        const Terminator& terminator = m_code.at(block).terminator;
        bool wayEnds = !terminator.numberOfEdges || sweptTerminatorMayThrowToHandler(m_code, terminator);
        for (uint32_t targetIndex = 0; targetIndex < terminator.numberOfEdges && !wayEnds; ++targetIndex) {
            BlockID target = m_code.at(terminator.edge(targetIndex)).to;
            if (m_placesInPostorder[target.index()] >= m_placesInPostorder[block.index()]) {
                wayEnds = true;
                break;
            }
            Awaited& atHead = m_awaitedAtHeads.add(target.index(), Awaited { }).iterator->value;
            for (auto& [owner, store] : awaited.storesByOwner)
                atHead.storesByOwner.add(owner, store);
            if (!atHead.allocation)
                atHead.allocation = awaited.allocation;
        }
        if (!wayEnds)
            return { };
        return awaited.storesByOwner.isEmpty() ? sentenceOfFence(awaited.allocation) : sentenceOfBarrier(firstStoreOf(awaited));
    }

    bool storesOnlyRequiredObject(const Awaited& awaited, const Operation& operation) const
    {
        ValueID required = objectRequiredBySwept(m_code, operation);
        return required && std::ranges::all_of(awaited.storesByOwner.values(), [&](ValueID store) {
            return sweptStoreStoresOnly(m_code, m_code.at(store), required);
        });
    }

    static ValueID firstStoreOf(const Awaited& awaited)
    {
        ValueID first;
        for (ValueID store : awaited.storesByOwner.values()) {
            if (!first || store.index() < first.index())
                first = store;
        }
        return first;
    }

    String sentenceOfBarrier(ValueID store) const
    {
        return sentence(m_code.at(store).block(), store, "the barrier of a store is left to a later one, and on some way something collects or leaves, or the way ends, in front of a barrier of the same owner"_s);
    }

    String sentenceOfFence(ValueID allocation) const
    {
        return sentence(m_code.at(allocation).block(), allocation, "the fence of an allocation is left to a later one, and on some way a cell may be handed on, or the way ends, in front of a fence"_s);
    }

    bool holdsPoll(BlockID block) const { return m_blocksWithPolls.quickGet(block.index()); }

    String findWayRoundWithoutPoll()
    {
        enum class State : uint8_t { NotSeen, IsOnWay, IsDone };
        Vector<State> states;
        states.fill(State::NotSeen, m_code.numberOfBlocks());
        struct Step {
            BlockID block;
            uint32_t nextTarget { 0 };
        };
        Vector<Step, 32> way;
        for (uint32_t index = 0; index < m_code.numberOfBlocks(); ++index) {
            if (!m_code.isReached(BlockID(index)) || states[index] != State::NotSeen || holdsPoll(BlockID(index)))
                continue;
            states[index] = State::IsOnWay;
            way.append({ BlockID(index) });
            while (!way.isEmpty()) {
                Step& step = way.last();
                const Terminator& terminator = m_code.at(step.block).terminator;
                if (step.nextTarget >= terminator.numberOfEdges) {
                    states[step.block.index()] = State::IsDone;
                    way.removeLast();
                    continue;
                }
                BlockID target = m_code.at(terminator.edge(step.nextTarget++)).to;
                if (states[target.index()] == State::IsOnWay)
                    return makeString('f', m_code.function().index(), ' ', nameOf(m_code.role()), " b"_s, target.index(), ": a way leads round to the block and no poll is kept on it"_s);
                if (states[target.index()] != State::NotSeen || holdsPoll(target))
                    continue;
                states[target.index()] = State::IsOnWay;
                way.append({ target });
            }
        }
        return { };
    }

    String sentence(BlockID block, ValueID id, ASCIILiteral what) const
    {
        return makeString('f', m_code.function().index(), ' ', nameOf(m_code.role()), " b"_s, block.index(), " v"_s, id.index(), " ("_s, nameOf(m_code.at(id).opcode), "): "_s, what);
    }

    const SweepInputs& m_inputs;
    Code& m_code;
    Vector<BlockID> m_blocksInPostorder;
    Vector<uint32_t> m_placesInPostorder;
    BitVector m_blocksWithPolls;
    HashMap<uint32_t, Awaited, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_awaitedAtHeads;
};

} // anonymous namespace

String findMarkOfSweepThatNothingCovers(const SweepInputs& inputs, Code& code)
{
    return MarksOfSweepAskedForward(inputs, code).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
