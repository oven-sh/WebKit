/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBarrierEmitter.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTConversionEmitter.h"
#include "AOTIRProgram.h"
#include "AOTInstance.h"
#include "AOTStringTable.h"
#include "AOTStubsBarriers.h"
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

bool BarrierEmitter::hasSingleStoredCell(const Operation& store) const
{
    unsigned count = 0;
    forEachStoredOperand(store, [&](ValueID operand) {
        count += typeOf(operand).hasCells();
    });
    return count == 1;
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

bool BarrierEmitter::liesOnlyInFrames(ValueID owner) const
{
    LayoutID environment = typeOf(owner).environment;
    return environment && m_plan.layout(environment).liesOnlyInFrames;
}

bool BarrierEmitter::ownerIsInFrame(const Operation& store) const
{
    ValueID owner = ownerOperandOf(store);
    if (liesOnlyInFrames(owner))
        return true;
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
    if (store->ownerCannotBeMarked())
        return BarrierDecision::OwnerCannotBeMarked;
    if (m_generator.laterBarrierCovers(store.id()))
        return BarrierDecision::LaterBarrierCovers;
    return BarrierDecision::Emit;
}

bool BarrierEmitter::shouldEmitBarrier(const ValueContext& store, bool mayStoreCell)
{
    switch (decide(store, mayStoreCell)) {
    case BarrierDecision::Emit:
        return true;
    case BarrierDecision::OwnerIsInFrame:
    case BarrierDecision::OwnerCannotBeMarked:
    case BarrierDecision::LaterBarrierCovers:
    case BarrierDecision::StoresNoCell:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void BarrierEmitter::barrierAfterStore(const ValueContext& store, CellPointer owner, Operand stored, const Type& storedType)
{
    if (!shouldEmitBarrier(store, storedType.hasCells() && !storesOnlyStringsOfVM(store.value())))
        return;
    if (!storedType.hasLiteralStrings() || stored.representation != Representation::LiteralString || !hasSingleStoredCell(store.value())) {
        barrier(store, owner);
        return;
    }
    // A word that names no cell runs no barrier, so Sweep::lookForCoveredBarrier() must not let this store cover an earlier one either.
    LBasicBlock isCell = newBlock();
    LBasicBlock continuation = newBlock();
    Condition storesCell = emitter<ConversionEmitter>().isStringCell(stored.as<LiteralStringWord>());
    ScopeFork fork = m_out.forkScope();
    m_out.branch(storesCell.value(), rareEdge(isCell), normalEdge(continuation));
    m_out.appendToArm(isCell, fork);
    callBarrierStub(store, writeBarrier(), { owner.value() }, { .isOnRarePath = true });
    m_out.jump(continuation);
    m_out.appendToJoin(continuation, fork);
}

void BarrierEmitter::barrierAfterStores(const ValueContext& store, CellPointer owner, std::span<const ValueID> stored)
{
    bool mayStoreCell = std::ranges::any_of(stored, [&](ValueID value) {
        return typeOf(value).hasCells();
    });
    if (shouldEmitBarrier(store, mayStoreCell && !storesOnlyStringsOfVM(store.value())))
        barrier(store, owner);
}

void BarrierEmitter::callBarrierStub(const OperationContext& context, StubKey key, std::initializer_list<LValue> operands, CallOptions options)
{
    // The effects of a store name no heap that a barrier writes, and a call that writes nothing is dropped.
    options.accessesCollectorState = true;
    callStub(context, key, operands, options);
}

void BarrierEmitter::barrier(const OperationContext& context, CellPointer owner)
{
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

// The owner names its new Extension, which is a store like the one that needs the room.
// The stores in front of it are marked as covered by a later barrier: this one. The sweep marks it too, where a later barrier covers it or no store waits for it.
void BarrierEmitter::barrierBehindStores(const ValueContext& value, ValueID owner)
{
    const Operation& definition = operationAt(definitionOf(owner));
    bool ownerLiesInFrame = Operation::opcodesWithStorage.contains(definition.opcode) && definition.storage() != AllocationStorage::Heap;
    if (ownerLiesInFrame || liesOnlyInFrames(owner) || m_generator.laterBarrierCovers(value.id()))
        return;
    barrier(value, in<CellPointer>(owner));
}

OwnerBarrier BarrierEmitter::barrierOfExtension(const ValueContext& store) const
{
    return decide(store, true) == BarrierDecision::Emit ? OwnerBarrier::Emitted : OwnerBarrier::Omitted;
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
        // A long array that has been marked stays so, and every store into it comes here. The first questions of generateRememberElement() are asked in place, in its order:
        // where they end it with nothing to remember, nothing is called. Every other answer is left to the stub, which asks again.
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
    if (allocation.valueID() && m_generator.laterFenceCovers(allocation.valueID()))
        return;
    mutatorFence();
}

} } } // namespace JSC::AOT::Typed

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

#endif // ENABLE(AOT)
