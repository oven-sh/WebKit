/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet opcodesThatAskWhatCallsOfCoroutinesKeep = opcodesThatStateTypes | OpcodeSet { Opcode::Await };
inline constexpr OpcodeSet opcodesThatBuildCoroutineOperationTakes = opcodesThatAskWhatCallsOfCoroutinesKeep | OpcodeSet { Opcode::Call, Opcode::CreatePromise, Opcode::ResolvePromise, Opcode::HasJSType, Opcode::ReadsIteratorOfListUnseen };
bool buildCoroutineOperationOfItsOpcodes(Walker&, const BodyValue&);
void forgetAwaitOfCallThatMayBeCopied(Walker&);
ALWAYS_INLINE bool buildCoroutineOperation(Walker& walker, const BodyValue& value)
{
    if (walker.coroutineState().keptByCalls) [[unlikely]]
        forgetAwaitOfCallThatMayBeCopied(walker);
    return opcodesThatBuildCoroutineOperationTakes.contains(value.opcode()) && buildCoroutineOperationOfItsOpcodes(walker, value);
}
template<Opcode known> ALWAYS_INLINE bool buildCoroutineOperation(Walker& walker, const BodyValueOf<known>& value)
{
    bool callsKeepSomething = !!walker.coroutineState().keptByCalls;
    if (callsKeepSomething) [[unlikely]]
        forgetAwaitOfCallThatMayBeCopied(walker);
    return opcodesThatBuildCoroutineOperationTakes.contains(known) && (callsKeepSomething || !opcodesThatAskWhatCallsOfCoroutinesKeep.contains(known)) && buildCoroutineOperationOfItsOpcodes(walker, value);
}

inline constexpr OpcodeSet opcodesThatAnswerFromStepWrittenByHandTakes { Opcode::PropGet, Opcode::TypeTest };
AOT_LINK_OF_WALK(answerFromStepWrittenByHand, opcodesThatAnswerFromStepWrittenByHandTakes)
inline constexpr OpcodeSet opcodesThatLeaveNextToStepTakes { Opcode::PropGet };
AOT_LINK_OF_WALK(leaveNextToStep, opcodesThatLeaveNextToStepTakes)
inline constexpr OpcodeSet opcodesThatCallStepsGeneratorTakes { Opcode::Call };
AOT_LINK_OF_WALK(callStepsGenerator, opcodesThatCallStepsGeneratorTakes)

std::optional<Vector<Reference, 8>> layOutEntryOperandsOfCall(Walker&, const BodyValue& call, FunctionID callee, CodeUnit entered);
bool awaitsInSomeIteration(const BodyIndex&, ValueID suspension);
std::optional<Vector<Reference, 8>> buildProtocolCallOperands(Walker&, FunctionID callee, CodeUnit entered, Reference thisValue, Reference closure, Origin);

FunctionID findCoroutineCalledBy(Walker&, const BodyValue& call);
uint32_t findEndOfAwaitedCall(Walker&, BodyBlock, ValueID call);
Reference buildStartOfBodyOfCoroutine(Walker&, const BodyValue& call, FunctionID callee, std::span<const Reference> entryOperands, Reference driver);
ValueID findOnlyTakerBehindCasts(const BodyIndex&, ValueID);
bool takeWhatCallInCopyHasAwaited(Walker&, const BodyValue& await);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
