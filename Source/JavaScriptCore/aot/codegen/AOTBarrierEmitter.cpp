/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBarrierEmitter.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTIRProgram.h"
#include "AOTInstance.h"
#include "AOTStringTable.h"
#include "AOTStubsBarriers.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeArena.h"
#include "CellState.h"
#include "JSCell.h"
#include "RememberedSlot.h"
#include "StoredCellLog.h"
#include <algorithm>

#if COMPILER(CLANG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

namespace JSC { namespace AOT { namespace Typed {

template<typename Functor>
void BarrierEmitter::forEachStoredOperand(const Operation& store, const Functor& functor) const
{
    store.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (retainedInOf(store.opcode, operandIndex) == RetainedIn::Operand)
            functor(operand);
    });
}

// The VM marks them whoever holds them. Such a store runs no barrier, so Sweep::lookForCoveredBarrier() must not let it cover an earlier one.
bool BarrierEmitter::storesOnlyStringsOfVM(const Operation& store) const
{
    if (!runsOwnerBarrierOf(store.opcode))
        return false;
    bool storesString = false;
    bool storesOtherCell = false;
    forEachStoredOperand(store, [&](ValueID operand) {
        if (!typeOf(operand).hasCells())
            return;
        const Operation& stored = operationAt(operand);
        bool isOfVM = stored.opcode == Opcode::String && hasCellOfVM(m_generator.inputs().types.bytesOf(stored.string()));
        storesString |= isOfVM;
        storesOtherCell |= !isOfVM;
    });
    return storesString && !storesOtherCell;
}

ValueID BarrierEmitter::ownerOperandOf(const Operation& store) const
{
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(store.opcode) + hasVariadicOperand(store.opcode);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        if (retainedInOf(store.opcode, operandIndex) == RetainedIn::Operand)
            return store.operand(*retainingOperandOf(store.opcode, operandIndex));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool BarrierEmitter::ownerIsInFrame(const Operation& store) const
{
    ValueID owner = ownerOperandOf(store);
    const Operation& definition = operationAt(definitionOf(owner));
    return Operation::opcodesWithStorage.contains(definition.opcode) && definition.storage() == AllocationStorage::Frame;
}

BarrierEmitter::BarrierDecision BarrierEmitter::decide(const ValueContext& store, bool mayStoreCell) const
{
    if (!mayStoreCell)
        return BarrierDecision::StoresNoCell;
    if (!runsOwnerBarrierOf(store.opcode()))
        return BarrierDecision::Emit;
    if (ownerIsInFrame(store.value()))
        return BarrierDecision::OwnerIsInFrame;
    if (m_generator.ownerIsYoung(store.id()))
        return BarrierDecision::OwnerCannotBeMarked;
    if (m_generator.laterBarrierCovers(store.id()))
        return BarrierDecision::LaterBarrierCovers;
    return BarrierDecision::Emit;
}

bool BarrierEmitter::shouldEmitBarrier(const ValueContext& store, bool mayStoreCell)
{
    m_owesFenceBehindStores = false;
    switch (decide(store, mayStoreCell)) {
    case BarrierDecision::Emit:
        return true;
    case BarrierDecision::OwnerCannotBeMarked:
        // While the collector marks, the barrier that is left out would have fenced. So the store stays in front of the store that hands the cell on, as the stores that fill a new cell do.
        mutatorFence();
        m_generator.noteThatOwnerIsRememberedBehind(store.id());
        return false;
    case BarrierDecision::OwnerIsInFrame:
        m_generator.noteThatOwnerIsRememberedBehind(store.id());
        return false;
    case BarrierDecision::StoresNoCell:
    case BarrierDecision::LaterBarrierCovers:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void BarrierEmitter::barrierAfterStore(const ValueContext& store, CellPointer owner, const Type& storedType)
{
    if (!shouldEmitBarrier(store, storedType.hasCells() && !storesOnlyStringsOfVM(store.value()))) {
        if (m_generator.ownerIsYoung(store.id()) && (AOT::hasFlag(m_generator.build().options().testing, Wire::TestingSwitch::CheckProofsAtRunTime) || AOT::hasFlag(m_generator.build().options().testing, Wire::TestingSwitch::CheckBoundsInEmittersOnly)))
            stopIfCellIsOldAndNotRemembered(store, owner);
        return;
    }
    barrier(store, owner);
    m_generator.noteThatOwnerIsRememberedBehind(store.id());
}

void BarrierEmitter::stopIfCellIsOldAndNotRemembered(const ValueContext& store, CellPointer owner)
{
    LValue state = m_out.load8ZeroExt32(m_out.address(m_heaps.collectorState, owner.value(), JSCell::cellStateOffset()));
    emitter<ThrowEmitter>().requireOrCrash(store, Condition(m_out.notEqual(state, m_out.constInt32(static_cast<int32_t>(CellState::PossiblyBlack)))), std::nullopt, ProofSource::RememberedFact);
}

void BarrierEmitter::callBarrierStub(const OperationContext& context, StubKey key, std::initializer_list<LValue> operands, CallOptions options)
{
    options.accessesCollectorState = true;
    callStub(context, key, operands, options);
}

void BarrierEmitter::barrier(const OperationContext& context, CellPointer owner)
{
    m_owesFenceBehindStores = false;
    LBasicBlock slowPath = newBlock();
    LBasicBlock continuation = newBlock();
    Condition isBeyondThreshold = isBeyondBarrierThreshold(owner);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(isBeyondThreshold.value(), normalEdge(continuation), rareEdge(slowPath));
    m_out.appendToArm(slowPath, fork);
    callBarrierStub(context, writeBarrier(), { owner.value() }, { .isOnRarePath = true });
    m_out.jump(continuation);
    m_out.appendToJoin(continuation, fork);
}

OwnerBarrier BarrierEmitter::barrierOfExtension(const ValueContext& store) const
{
    BarrierDecision decision = decide(store, true);
    return decision == BarrierDecision::Emit || decision == BarrierDecision::OwnerCannotBeMarked ? OwnerBarrier::Emitted : OwnerBarrier::Omitted;
}

void BarrierEmitter::markCard(uint32_t cardMention)
{
    m_out.store32As8(m_out.constInt32(RememberedSlotSet::writtenCard), instanceWord(m_heaps.cards, cardMention));
}

Condition BarrierEmitter::isBeyondBarrierThreshold(CellPointer owner)
{
    LValue threshold = m_out.load32(instanceField(m_heaps.collectorState, AOT_INSTANCE_OFFSET(barrierThreshold)));
    LValue state = m_out.load8ZeroExt32(m_out.address(m_heaps.collectorState, owner.value(), JSCell::cellStateOffset()));
    return Condition(m_out.above(state, threshold));
}

void BarrierEmitter::elementBarrierAfterStore(const ValueContext& store, CellPointer array, std::span<const Boxed> stored)
{
    // The stub remembers only the stored elements, so this barrier covers no earlier store into the array: the row of the operation must not say remembersOwner.
    RELEASE_ASSERT(!remembersOwnerOf(store.opcode()));
    if (!shouldEmitBarrier(store, true))
        return;
    LBasicBlock slowPath = newBlock();
    LBasicBlock continuation = newBlock();
    Condition isBeyondThreshold = isBeyondBarrierThreshold(array);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(isBeyondThreshold.value(), normalEdge(continuation), rareEdge(slowPath));
    m_out.appendToArm(slowPath, fork);
    for (Boxed value : stored) {
        LBasicBlock remembers = newBlock();
        LBasicBlock next = newBlock();
        LBasicBlock nothingMarks = newBlock();
        LBasicBlock isLong = newBlock();
        LBasicBlock isStoredCell = newBlock();
        m_out.branch(m_out.load8ZeroExt32(instanceField(m_heaps.collectorState, AOT_INSTANCE_OFFSET(mutatorShouldBeFenced))), rareEdge(remembers), normalEdge(nothingMarks));
        m_out.appendTo(nothingMarks);
        Word32 vectorLength = loadField<Word32>(loadField<Pointer>(array, m_heaps.JSArray_butterfly), m_heaps.Butterfly_vectorLength);
        m_out.branch(m_out.below(vectorLength.value(), m_out.constInt32(StoredCellLog::minVectorLengthOfOwner)), normalEdge(remembers), normalEdge(isLong));
        m_out.appendTo(isLong);
        m_out.branch(isCell(value).value(), normalEdge(isStoredCell), normalEdge(next));
        m_out.appendTo(isStoredCell);
        static_assert(!static_cast<unsigned>(CellState::PossiblyBlack));
        m_out.branch(m_out.load8ZeroExt32(m_out.address(m_heaps.collectorState, value.value(), JSCell::cellStateOffset())), normalEdge(remembers), normalEdge(next));
        m_out.appendTo(remembers);
        callBarrierStub(store, rememberElement(), { array.value(), value.value() }, { .isOnRarePath = true });
        m_out.jump(next);
        m_out.appendTo(next);
    }
    m_out.jump(continuation);
    m_out.appendToJoin(continuation, fork);
}

void BarrierEmitter::fenceAfterInitialization()
{
    mutatorFence();
}

void BarrierEmitter::fenceAfterInitialization(const OperationContext& allocation)
{
    m_owesFenceBehindStores = false;
    if (allocation.valueID() && m_generator.laterFenceCovers(allocation.valueID()))
        return;
    mutatorFence();
}

void BarrierEmitter::fenceAfterLateStores()
{
    if (AOT::hasFlag(m_generator.build().options().testing, Wire::TestingSwitch::SkipFencesAfterLateStores)) [[unlikely]]
        return;
    mutatorFence();
}

} } } // namespace JSC::AOT::Typed

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

#endif // ENABLE(AOT)
