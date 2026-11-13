/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTIRFunction.h"
#include "AOTThinSummary.h"
#include "JSCJSValueInlines.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfEvals()
{
    return { Opcode::DirectEval, Opcode::DeclareGlobal, Opcode::CheckEvaluation };
}

void StraightEmitter::generateEval(const BodyValue& value)
{
    if (value.opcode() == Opcode::DeclareGlobal) {
        callOperation(value, RuntimeOperation::DeclareGlobal, { StraightArgument::number(mentionOfName(value.key())), StraightArgument::word(static_cast<uint32_t>(value.step())), StraightArgument::word(value.isFunction()) });
        return;
    }
    if (value.opcode() == Opcode::CheckEvaluation) {
        callOperation(value, RuntimeOperation::CheckEvaluation, { }, { .calledBuiltin = value.ofFunction() ? Builtins::Function : Builtins::Eval });
        return;
    }
    if (value.opcode() != Opcode::DirectEval) {
        fail(value);
        return;
    }

    constexpr uint32_t numberOfOperandsInFrontOfEnvironments = 2;
    std::span<const ValueID> environments = value.environmentsOperands();
    storeInScratchArea(value.thisValueOperand(), 0);
    if (ValueID newTarget = value.newTargetOperand())
        storeInScratchArea(newTarget, 1);
    else
        jit().store64(CCallHelpers::TrustedImm64(JSValue::encode(JSValue())), wordOfScratchArea(1));
    for (size_t index = 0; index < environments.size(); ++index)
        storeInScratchArea(environments[index], static_cast<uint32_t>(numberOfOperandsInFrontOfEnvironments + index));

    uint32_t scopeAndModule[] { value.evalScope().index(), program().at(function()).module.index() };
    callOperation(value, RuntimeOperation::DirectEval, {
        value.textOperand(),
        StraightArgument::number(mentions().request(RequestKind::DirectEval, scopeAndModule)),
        StraightArgument::word(numberOfOperandsInFrontOfEnvironments + environments.size()),
        StraightArgument::addressInScratchArea(0),
    }, { .calledBuiltin = Builtins::Eval });
    define(value);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
