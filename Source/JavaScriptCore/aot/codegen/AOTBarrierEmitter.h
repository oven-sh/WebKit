/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

enum class OwnerBarrier : bool { Omitted, Emitted };

class BarrierEmitter final : public OperationEmitter {
public:
    explicit BarrierEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void barrierAfterStore(const ValueContext& store, CellPointer owner, const Type& storedType);
    void elementBarrierAfterStore(const ValueContext& store, CellPointer array, std::span<const Boxed> stored);
    void barrier(const OperationContext&, CellPointer owner);
    OwnerBarrier barrierOfExtension(const ValueContext& store) const;
    void stopIfCellIsOldAndNotRemembered(const ValueContext& store, CellPointer owner);
    void markCard(uint32_t cardMention);
    Condition isBeyondBarrierThreshold(CellPointer);
    void fenceAfterInitialization();
    void fenceAfterInitialization(const OperationContext& allocation);
    void fenceAfterLateStores();

private:
    enum class BarrierDecision : uint8_t { Emit, OwnerIsInFrame, OwnerCannotBeMarked, LaterBarrierCovers, StoresNoCell };

    BarrierDecision decide(const ValueContext& store, bool mayStoreCell) const;
    bool shouldEmitBarrier(const ValueContext& store, bool mayStoreCell);
    ValueID ownerOperandOf(const Operation& store) const;
    bool ownerIsInFrame(const Operation& store) const;
    template<typename Functor> void forEachStoredOperand(const Operation& store, const Functor&) const;
    bool storesOnlyStringsOfVM(const Operation& store) const;
    void callBarrierStub(const OperationContext&, StubKey, std::initializer_list<LValue> operands, CallOptions = { });
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
