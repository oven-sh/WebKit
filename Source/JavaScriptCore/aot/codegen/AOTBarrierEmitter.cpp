/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBarrierEmitter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTConversionEmitter.h"
#include "AOTFrameAllocation.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTInstance.h"
#include "AOTRuntimeValidation.h"
#include "AOTStubsBarriers.h"
#include "CellState.h"
#include "JSCell.h"
#include "RememberedSlot.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

BarrierEmitter::BarrierEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

template<typename Functor>
void BarrierEmitter::forEachStoredOperand(const Value& store, const Functor& functor) const
{
    store.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (retainedInOf(store.opcode, operandIndex) == RetainedIn::Operand)
            functor(operand);
    });
}

bool BarrierEmitter::hasSingleStoredCell(const Value& store) const
{
    unsigned count = 0;
    forEachStoredOperand(store, [&](ValueID operand) {
        count += typeOf(operand).hasCells();
    });
    return count == 1;
}

ValueID BarrierEmitter::ownerOperandOf(const Value& store) const
{
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(store.opcode) + hasVariadicOperand(store.opcode);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        if (retainedInOf(store.opcode, operandIndex) == RetainedIn::Operand)
            return store.operand(*retainingOperandOf(store.opcode, operandIndex));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool BarrierEmitter::ownerIsOnlyInFrames(const Value& store) const
{
    ValueID owner = ownerOperandOf(store);
    if (isFrameCell(m_function, owner))
        return true;

    const FrameAllocation& frameAllocation = inputs().frameAllocation;
    const Type& type = typeOf(owner);
    if (type.environment)
        return frameAllocation.isOnlyInFrames(type.environment);
    return type.hasOnlyLayouts(m_program) && std::ranges::all_of(membersOf(m_program, type), [&](LayoutID layout) {
        return frameAllocation.isOnlyInFrames(layout);
    });
}

BarrierEmitter::BarrierDecision BarrierEmitter::decide(const ValueContext& store, bool mayStoreCell) const
{
    if (!mayStoreCell)
        return BarrierDecision::StoresNoCell;
    if (!runsOwnerBarrierOf(store.opcode()))
        return BarrierDecision::Emit;
    // Such a cell may have no header, so no code may read its cell state.
    if (ownerIsOnlyInFrames(store.value()))
        return BarrierDecision::OwnerIsInFrame;
    return BarrierDecision::Emit;
}

bool BarrierEmitter::shouldEmitBarrier(const ValueContext& store, bool mayStoreCell)
{
    Statistics& statistics = build().statistics();
    switch (decide(store, mayStoreCell)) {
    case BarrierDecision::Emit:
        statistics.count(StatisticID::BarriersEmitted);
        return true;
    case BarrierDecision::OwnerIsInFrame:
        statistics.count(StatisticID::BarriersOnUnmarkableOwners);
        return false;
    case BarrierDecision::StoresNoCell:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void BarrierEmitter::barrierAfterStore(const ValueContext& store, CellPointer owner, Operand stored, const Type& storedType)
{
    if (!shouldEmitBarrier(store, storedType.hasCells()))
        return;
    if (!storedType.hasLiteralStrings() || stored.representation != Representation::LiteralString || !hasSingleStoredCell(store.value())) {
        barrier(store, owner);
        return;
    }
    LBasicBlock isCell = newBlock();
    LBasicBlock continuation = newBlock();
    m_out.branch(emitter<ConversionEmitter>().isStringCell(stored.as<LiteralStringWord>()).value(), rareEdge(isCell), normalEdge(continuation));
    m_out.appendTo(isCell);
    callBarrierStub(store, writeBarrier(), { owner.value() }, { .isOnRarePath = true });
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void BarrierEmitter::barrierAfterStores(const ValueContext& store, CellPointer owner, std::span<const ValueID> stored)
{
    bool mayStoreCell = std::ranges::any_of(stored, [&](ValueID value) {
        return typeOf(value).hasCells();
    });
    if (shouldEmitBarrier(store, mayStoreCell))
        barrier(store, owner);
}

void BarrierEmitter::callBarrierStub(const OperationContext& context, StubKey key, std::initializer_list<LValue> operands, CallOptions options)
{
    // The effects of a store name no heap that a barrier writes, and B3 removes a call that writes nothing.
    options.accessesCollectorState = true;
    callStub(context, key, operands, options);
}

void BarrierEmitter::barrier(const OperationContext& context, CellPointer owner)
{
    if (context.shape() != CodeShape::Fast)
        callBarrierStub(context, writeBarrier(), { owner.value() });
    else {
        LBasicBlock slowPath = newBlock();
        LBasicBlock continuation = newBlock();
        m_out.branch(isBeyondBarrierThreshold(owner).value(), normalEdge(continuation), rareEdge(slowPath));
        m_out.appendTo(slowPath);
        callBarrierStub(context, writeBarrier(), { owner.value() }, { .isOnRarePath = true });
        m_out.jump(continuation);
        m_out.appendTo(continuation);
    }
}

// The owner names its new Extension, which is a store like the one that needs the room.
OwnerBarrier BarrierEmitter::barrierOfExtension(const ValueContext& store) const
{
    return decide(store, true) == BarrierDecision::Emit ? OwnerBarrier::Emitted : OwnerBarrier::Omitted;
}

void BarrierEmitter::markCardOfStatics(uint32_t offsetInStatics)
{
    InstanceSizes sizes = inputs().instanceSizes;
    ptrdiff_t offsetOfCardsFromStatics = InstanceLayout::offsetOfCards(sizes) - InstanceLayout::offsetOfStatics(sizes);
    ptrdiff_t card = offsetInStatics >> InstanceLayout::cardShift;
    m_out.store32As8(m_out.constInt32(RememberedSlotSet::writtenCard), m_out.address(m_heaps.cards, m_statics, offsetOfCardsFromStatics + card));
}

Condition BarrierEmitter::isBeyondBarrierThreshold(CellPointer owner)
{
    LValue threshold = m_out.load32(instanceField(m_heaps.collectorState, AOT_INSTANCE_OFFSET(barrierThreshold)));
    LValue state = m_out.load8ZeroExt32(m_out.address(m_heaps.collectorState, owner.value(), JSCell::cellStateOffset()));
    return Condition(m_out.above(state, threshold));
}

void BarrierEmitter::elementBarrierAfterStore(const ValueContext& store, CellPointer array, std::span<const Boxed> stored)
{
    if (!shouldEmitBarrier(store, true))
        return;
    LBasicBlock slowPath = newBlock();
    LBasicBlock continuation = newBlock();
    m_out.branch(isBeyondBarrierThreshold(array).value(), normalEdge(continuation), rareEdge(slowPath));
    m_out.appendTo(slowPath);
    for (Boxed value : stored)
        callBarrierStub(store, rememberElement(), { array.value(), value.value() }, { .isOnRarePath = true });
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void BarrierEmitter::fenceAfterInitialization()
{
    mutatorFence();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
