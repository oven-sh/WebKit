/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEvalEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTIRFunction.h"
#include "AOTVector.h"
#include "JSCJSValueInlines.h"

namespace JSC { namespace AOT { namespace Typed {

void EvalEmitter::generateDirectEval(const ValueContext& value)
{
    Vector<Boxed, 8> operands;
    operands.append(in<Boxed>(value->thisValueOperand()));
    operands.append(inOptional<Boxed>(value->newTargetOperand()).value_or(boxedConstant(JSValue())));
    for (ValueID environment : value->environmentsOperands())
        operands.append(in<Boxed>(environment));

    uint32_t scopeAndModule[] { value->evalScope().index(), function().module.index() };

    Boxed text = in<Boxed>(value->textOperand());
    Word32 request = numberOf(m_mentions.request(RequestKind::DirectEval, scopeAndModule));
    Word32 count = word32(static_cast<int32_t>(operands.size()));
    Pointer buffer = argumentBuffer(operands.span());
    define(value, callOperation<Boxed>(value, RuntimeOperation::DirectEval, { text, request, count, buffer }, { .calledBuiltin = Builtins::Eval }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
