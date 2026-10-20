/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class BarrierEmitter final : public OperationEmitter {
public:
    explicit BarrierEmitter(CodeGenerator&);

    void barrierAfterStore(const ValueContext& store, CellPointer owner, Operand stored, const Type& storedType);
    void barrierAfterStores(const ValueContext& store, CellPointer owner, std::span<const ValueID> stored);
    void elementBarrierAfterStore(const ValueContext& store, CellPointer array, std::span<const Boxed> stored);
    void barrier(const OperationContext&, CellPointer owner);
    void markCardOfStatics(uint32_t offsetInStatics);
    Condition isBeyondBarrierThreshold(CellPointer);
    void fenceAfterInitialization();

    void validateAfterStore(const ValueContext& store);
    void validateBeforeCollection(const OperationContext&);

private:
    enum class BarrierDecision : uint8_t { Emit, OwnerIsInFrame, OwnerCannotBeMarked, LaterBarrierCovers, StoresNoCell };

    BarrierDecision decide(const ValueContext& store, bool mayStoreCell) const;
    bool shouldEmitBarrier(const ValueContext& store, bool mayStoreCell);
    ValueID ownerOperandOf(const Value& store) const;
    bool ownerIsOnlyInFrames(const Value& store) const;
    template<typename Functor> void forEachStoredOperand(const Value& store, const Functor&) const;
    bool hasSingleStoredCell(const Value& store) const;
    const Type* storedTypeOf(const Value& store) const;
    void callBarrierStub(const OperationContext&, StubKey, std::initializer_list<LValue> operands, CallOptions = { });

    TypedPointer pendingBarrierOwner();
    void didEmitBarrier(CellPointer owner);
    void failValidationIf(const OperationContext&, Condition, CellPointer owner);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
