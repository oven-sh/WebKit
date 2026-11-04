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

    bool unwindsWithinFunction(const OperationContext& context) const { return context.handler(); }
    void attachLandingPad(const OperationContext&, Machine::PatchOperation*, uint32_t site, bool mayReturn);

    // Sites whose handler edges lead to one block with the same arguments share a pad.
    struct LandingPad {
        // One of those edges.
        const Edge* handler;
        // Null until a site asks for the pad.
        LBasicBlock block;
        // In ExceptionRecords::landingPads, whose entry is null until a site of the pad is encoded.
        uint32_t indexInRecords;
        // In m_landingPads: another pad under the same key of m_firstLandingPadsByKey.
        uint32_t next;
    };
    static constexpr uint32_t noLandingPad = UINT32_MAX;
    // By the index of the block of Code, in the arena of the procedure.
    template<typename T> using BlockMap = std::span<T>;

    void forEachReachedBlock(NOESCAPE const Invocable<void(BlockID, const Block&)> auto& functor) const
    {
        for (uint32_t index = 0; index < m_code.numberOfBlocks(); ++index) {
            BlockID block(index);
            if (m_code.isReached(block))
                functor(block, m_code.at(block));
        }
    }

    // The block that a throw in the block leads to.
    BlockID handlerOf(BlockID) const;
    uint32_t findOrAddLandingPad(const Edge& handler);
    // The pad that a throw in the block lands on. `handler` is the handler edge of the block.
    const LandingPad& landingPadFor(BlockID, const Edge& handler);
    HandlerType handlerTypeOf(BlockID handler) const;
    HandlerType handlerTypeByOperations(BlockID handler) const;
    bool isEnteredByJump(BlockID handler) const { return m_handlersEnteredByJump.get(handler.index()); }
    // Beside the value, such a cleanup takes what says where the value was thrown, and hands it to the Throw at its end.
    bool keepsException(BlockID handler) const { return m_cleanupsThatKeepException.get(handler.index()); }
    bool isCaughtByCleanup(ValueID) const;
    // No block for a value that no cleanup has caught, and for one that comes from one of several cleanups.
    BlockID cleanupThatCaught(ValueID) const;
    // The parameter of the handler that takes what was caught: a Phi at the head of its machine block. Each pad and each jump that leads there hands it a word.
    LValue caughtPhiOf(BlockID handler);
    // In the block that jumps to the handler: behind passHandlerArguments(), which may end the block it starts in.
    void passCaughtWord(BlockID handler, LValue);
    TypedPointer wordKeptBy(BlockID cleanup);
    void keep(BlockID cleanup, LValue);
    LValue keptBy(ValueID thrown);
    void jumpToHandler(const Edge& handler, LValue caught);
    void throwByOperation(const TerminatorContext&, RuntimeOperation, Boxed thrown);
    void throwAfterCleanup(const TerminatorContext&, Boxed thrown);
    void findValuesCaughtByCleanups();
    void findCleanupsThatKeepException();
    LValue caughtWord(const ValueContext&);
    Boxed valueOfException(CellPointer exception);

    BitVector m_handlersEnteredByJump;
    BitVector m_cleanupsThatKeepException;
    BitVector m_valuesCaughtByCleanups;
    // By the index of the value.
    std::span<BlockID> m_cleanupsOfCaughtValues;
    BlockMap<LValue> m_caughtPhis;
    BlockMap<Machine::StackSlot*> m_keptWords;
    BlockMap<LValue> m_caughtWords;
    // An index into m_landingPads, plus one. Zero until a site of the block asks for the pad.
    BlockMap<uint32_t> m_landingPadsOfBlocks;
    Vector<LandingPad, 4> m_landingPads;
    // By the block that the handler edge leads to and a hash of its arguments.
    UncheckedKeyHashMap<uint64_t, uint32_t> m_firstLandingPadsByKey;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
