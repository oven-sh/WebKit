/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTVector.h"
#include "HandlerInfo.h"
#include <wtf/BitVector.h>
#include <wtf/Box.h>
#include <wtf/HashMap.h>

namespace JSC {

namespace B3 {
class Variable;
namespace Air {
class StackSlot;
}
}

namespace AOT { namespace Typed {

class ExceptionEmitter final : public OperationEmitter {
public:
    explicit ExceptionEmitter(CodeGenerator&);

    void generateCatchValue(const ValueContext&);
    void generateCatchException(const ValueContext&);

    void throwValue(const TerminatorContext&, Boxed);
    void rethrow(const TerminatorContext&, CellPointer exception);

    // The code that follows is what the call returns to. The block that is answered is entered with the same registers, and the result is what the callee could not return.
    LBasicBlock attachLandingOfMisfit(B3::PatchpointValue*, uint32_t site);

private:
    friend class HintEmitter;
    friend class RuntimeCallEmitter;
    friend class ThrowEmitter;

    bool unwindsWithinFunction(const OperationContext& context) const { return context.handler(); }
    void attachLandingPad(const OperationContext&, B3::PatchpointValue*, uint32_t site, bool mayReturn);

    struct LandingPad {
        Arguments arguments;
        LBasicBlock block;
    };
    template<typename T> using BlockMap = UncheckedKeyHashMap<BlockID, T>;
    using LandingPadsBySuccessor = UncheckedKeyHashMap<CCallHelpers::Label*, uint32_t>;

    LBasicBlock landingPadFor(const Target& handler);
    HandlerType handlerTypeOf(BlockID handler) const;
    HandlerType handlerTypeByOperations(BlockID handler) const;
    bool isEnteredByJump(BlockID handler) const { return m_handlersEnteredByJump.get(handler.index()); }
    // Beside the value, such a cleanup takes what says where the value was thrown, and hands it to the Throw at its end.
    bool keepsException(BlockID handler) const { return m_cleanupsThatKeepException.get(handler.index()); }
    B3::Variable* caughtVariableFor(BlockID handler);
    void setCaughtVariable(BlockID handler, LValue);
    TypedPointer wordKeptBy(BlockID cleanup);
    void keep(BlockID cleanup, LValue);
    LValue keptBy(ValueID thrown);
    void jumpToHandler(const Target& handler, LValue caught);
    void throwByOperation(const TerminatorContext&, RuntimeOperation, Boxed thrown);
    void throwAfterCleanup(const TerminatorContext&, Boxed thrown);
    void findValuesCaughtByCleanups();
    void findCleanupsThatKeepException();
    LValue caughtWord(const ValueContext&);
    Boxed valueOfException(CellPointer exception);

    BitVector m_handlersEnteredByJump;
    BitVector m_cleanupsThatKeepException;
    // No block for a value that comes from one of several cleanups.
    UncheckedKeyHashMap<ValueID, BlockID> m_valuesCaughtByCleanups;
    BlockMap<B3::Variable*> m_caughtVariables;
    BlockMap<B3::Air::StackSlot*> m_keptWords;
    BlockMap<LValue> m_caughtWords;
    BlockMap<Vector<LandingPad, 2>> m_landingPads;
    Box<LandingPadsBySuccessor> m_landingPadsBySuccessor;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
