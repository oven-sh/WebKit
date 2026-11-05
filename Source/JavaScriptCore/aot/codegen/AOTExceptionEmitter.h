/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTVector.h"
#include "HandlerInfo.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

class ExceptionEmitter final : public OperationEmitter {
public:
    explicit ExceptionEmitter(CodeGenerator&);

    void generateCatchValue(const ValueContext&);
    void generateCatchException(const ValueContext&);

    void throwValue(const TerminatorContext&, Boxed);
    void rethrow(const TerminatorContext&, CellPointer exception);

private:
    friend class HintEmitter;
    friend class RuntimeCallEmitter;
    friend class ThrowEmitter;

    static bool unwindsWithinFunction(const OperationContext& context) { return context.handler(); }
    void attachLandingPad(const OperationContext&, Machine::PatchOperation*, uint32_t site, bool mayReturn);

    struct LandingPad {
        const Edge* handler;
        LBasicBlock block;
        uint32_t indexInRecords;
        uint32_t next;
        HandlerType type;
    };
    static constexpr uint32_t noLandingPad = UINT32_MAX;
    template<typename T> using BlockMap = std::span<T>;

    void forEachReachedBlock(NOESCAPE const Invocable<void(BlockID, const Block&)> auto& functor) const
    {
        uint32_t numberOfBlocks = m_code.numberOfBlocks();
        for (uint32_t index = 0; index < numberOfBlocks; ++index) {
            BlockID block(index);
            if (m_code.isReached(block))
                functor(block, m_code.at(block));
        }
    }

    BlockID handlerOf(BlockID) const;
    uint32_t findOrAddLandingPad(const Edge& handler);
    const LandingPad& landingPadFor(BlockID, const Edge& handler);
    HandlerType handlerTypeOf(BlockID handler) const;
    HandlerType handlerTypeByOperations(BlockID handler) const;
    bool isEnteredByJump(BlockID handler) const { return m_handlersEnteredByJump.get(handler.index()); }
    bool keepsException(BlockID handler) const { return m_cleanupsThatKeepException.get(handler.index()); }
    bool isCaughtByCleanup(ValueID) const;
    BlockID cleanupThatCaught(ValueID) const;
    LValue caughtPhiOf(BlockID handler);
    void passCaughtWord(BlockID handler, LValue);
    TypedPointer wordKeptBy(BlockID cleanup);
    void keep(BlockID cleanup, LValue);
    LValue keptBy(ValueID thrown);
    void jumpToHandler(const Edge& handler, LValue caught);
    void throwByOperation(const TerminatorContext&, RuntimeOperation, Boxed thrown);
    void throwAfterCleanup(const TerminatorContext&, Boxed thrown);
    void findValuesCaughtByCleanups(Vector<BlockID, 8>& blocksThatThrowIntoHandlers);
    void findCleanupsThatKeepException();
    LValue caughtWord(const ValueContext&);
    Boxed valueOfException(CellPointer exception);

    BitVector m_handlersEnteredByJump;
    BitVector m_cleanupsThatKeepException;
    BitVector m_valuesCaughtByCleanups;
    std::span<BlockID> m_cleanupsOfCaughtValues;
    BlockMap<LValue> m_caughtPhis;
    BlockMap<Machine::StackSlot*> m_keptWords;
    BlockMap<LValue> m_caughtWords;
    BlockMap<uint32_t> m_landingPadsOfBlocks;
    Vector<LandingPad, 4> m_landingPads;
    UncheckedKeyHashMap<uint64_t, uint32_t> m_firstLandingPadsByKey;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
