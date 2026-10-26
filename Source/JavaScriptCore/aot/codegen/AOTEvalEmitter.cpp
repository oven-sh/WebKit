/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEvalEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

void EvalEmitter::generateDirectEval(const ValueContext& value)
{
    std::optional<Request> request = value->request();
    if (!request) {
        fail(value, InternalErrorKind::PlacedPayloadMissing);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    Vector<Boxed, 8> operands;
    operands.append(in<Boxed>(value->thisValueOperand()));
    operands.append(inOptional<Boxed>(value->newTargetOperand()).value_or(boxedConstant(JSValue())));
    for (ValueID environment : value->environmentsOperands())
        operands.append(in<Boxed>(environment));

    Boxed text = in<Boxed>(value->textOperand());
    Word32 count = word32(static_cast<int32_t>(operands.size()));
    Pointer buffer = argumentBuffer(operands.span());
    define(value, callOperation<Boxed>(value, RuntimeOperation::DirectEval, { text, word32(static_cast<int32_t>(request->value())), count, buffer }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
