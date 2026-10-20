/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "HandlerInfo.h"
#include <wtf/BitVector.h>
#include <wtf/Box.h>
#include <wtf/HashMap.h>
#include <wtf/Vector.h>

namespace JSC {

namespace B3 {
class Variable;
}

namespace AOT { namespace Typed {

class ExceptionEmitter final : public OperationEmitter {
public:
    explicit ExceptionEmitter(CodeGenerator&);

    void generateCatchValue(const ValueContext&);
    void generateCatchException(const ValueContext&);

    void throwValue(const TerminatorContext&, Boxed);
    void rethrow(const TerminatorContext&, CellPointer exception);

private:
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
    void generateLandingPad(const Target& handler, LBasicBlock landingPad);
    HandlerType handlerTypeOf(BlockID handler) const;
    bool isEnteredByJump(BlockID handler) const { return m_handlersEnteredByJump.get(handler.index()); }
    B3::Variable* caughtVariableFor(BlockID handler);
    LValue getCaughtVariable(BlockID handler);
    void setCaughtVariable(BlockID handler, LValue);
    void jumpToHandler(const Target& handler, LValue caught);
    void throwByOperation(const TerminatorContext&, RuntimeOperation, Boxed thrown);
    LValue caughtWord(const ValueContext&);
    Boxed valueOfException(CellPointer exception);
    RegisterSet registersClobberedAtLandingPads() const;

    BitVector m_handlersEnteredByJump;
    BlockMap<B3::Variable*> m_caughtVariables;
    BlockMap<LValue> m_caughtWords;
    BlockMap<Vector<LandingPad, 2>> m_landingPads;
    Box<LandingPadsBySuccessor> m_landingPadsBySuccessor;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
