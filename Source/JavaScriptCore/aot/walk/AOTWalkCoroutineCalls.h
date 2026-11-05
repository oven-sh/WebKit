/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

extern const OpcodeSet opcodesThatBuildCoroutineOperationTakes;
bool buildCoroutineOperationOfItsOpcodes(Walker&, const BodyValue&);
void forgetAwaitOfCallThatMayBeCopied(Walker&);
ALWAYS_INLINE bool buildCoroutineOperation(Walker& walker, const BodyValue& value)
{
    if (walker.coroutineState().keptByCalls) [[unlikely]]
        forgetAwaitOfCallThatMayBeCopied(walker);
    return opcodesThatBuildCoroutineOperationTakes.contains(value.opcode()) && buildCoroutineOperationOfItsOpcodes(walker, value);
}

AOT_LINK_OF_WALK(answerFromStepWrittenByHand, opcodesThatAnswerFromStepWrittenByHandTakes)

std::optional<Vector<Reference, 8>> layOutEntryOperandsOfCall(Walker&, const BodyValue& call, FunctionID callee, CodeUnit entered);

FunctionID findCoroutineCalledBy(Walker&, const BodyValue& call);
Reference buildStartOfBodyOfCoroutine(Walker&, const BodyValue& call, FunctionID callee, std::span<const Reference> entryOperands, Reference driver);
ValueID findOnlyTakerBehindCasts(const BodyIndex&, ValueID);
bool takeWhatCallInCopyHasAwaited(Walker&, const BodyValue& await);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
