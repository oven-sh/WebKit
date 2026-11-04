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

    void barrierAfterStore(const ValueContext& store, CellPointer owner, Operand stored, const Type& storedType);
    void barrierAfterStores(const ValueContext& store, CellPointer owner, std::span<const ValueID> stored);
    // Of a RememberOwner. It runs whatever was stored, unless the sweep has found a later barrier that covers it or no store that waits for it.
    void barrierBehindStores(const ValueContext& barrier, ValueID owner);
    void elementBarrierAfterStore(const ValueContext& store, CellPointer array, std::span<const Boxed> stored);
    void barrier(const OperationContext&, CellPointer owner);
    OwnerBarrier barrierOfExtension(const ValueContext& store) const;
    // Behind the store of a cell into a word of the statics. `cardMention` mentions the card of that word: NumberSpace::CardOfVariable, or NumberSpace::CardOfWord through MentionList::cardOfWord().
    void markCard(uint32_t cardMention);
    Condition isBeyondBarrierThreshold(CellPointer);
    void fenceAfterInitialization();
    // For a fence behind which the code of `allocation` hands no cell on. It makes nothing where the sweep has left the fence to a later allocation: Sweep::lookForCoveredFence().
    void fenceAfterInitialization(const OperationContext& allocation);

private:
    enum class BarrierDecision : uint8_t { Emit, OwnerIsInFrame, OwnerCannotBeMarked, LaterBarrierCovers, StoresNoCell };

    BarrierDecision decide(const ValueContext& store, bool mayStoreCell) const;
    bool shouldEmitBarrier(const ValueContext& store, bool mayStoreCell);
    ValueID ownerOperandOf(const Operation& store) const;
    // The definition of the owner, behind any Narrowed, is an allocation in the frame, or the owner liesOnlyInFrames().
    bool ownerIsInFrame(const Operation& store) const;
    // An environment with PlannedLayout::liesOnlyInFrames, whichever function has made it.
    bool liesOnlyInFrames(ValueID owner) const;
    template<typename Functor> void forEachStoredOperand(const Operation& store, const Functor&) const;
    bool hasSingleStoredCell(const Operation& store) const;
    bool storesOnlyStringsOfVM(const Operation& store) const;
    void callBarrierStub(const OperationContext&, StubKey, std::initializer_list<LValue> operands, CallOptions = { });
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
