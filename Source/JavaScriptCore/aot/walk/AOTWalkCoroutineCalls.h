/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

// THE LINK. walkThroughChain(), in front of buildFreshObject. It asks, each behind its own mechanisms, each false = nothing built:
// buildPromiseOperation() (2) > buildPromiseAllOfElements (3) > buildStepWrittenByHand (3) > buildCallOfGeneratorThatLoopAloneTakes() (4) > buildAwaitedCall (3) > buildRampAtCall (3).
// Its head stands in line (walk/AOTWalkLinks.h: a link that cannot fire is never entered). It is written out because the note of C3 ends with the operation that it was taken at, whichever comes next.
extern const OpcodeSet opcodesThatBuildCoroutineOperationTakes;
bool buildCoroutineOperationOfItsOpcodes(Walker&, const BodyValue&);
void forgetAwaitOfCallThatMayBeCopied(Walker&);
ALWAYS_INLINE bool buildCoroutineOperation(Walker& walker, const BodyValue& value)
{
    if (walker.coroutineState().keptByCalls) [[unlikely]]
        forgetAwaitOfCallThatMayBeCopied(walker);
    return opcodesThatBuildCoroutineOperationTakes.contains(value.opcode()) && buildCoroutineOperationOfItsOpcodes(walker, value);
}

// A door of AOTWalkCalls.cpp, behind callAccessor(): planOperandsInWalkCalls() and buildOperandInWalkCalls() over the Call as written.
// One reference for each entry parameter of the callee, in the forms in which `entered` takes them. Nothing: NOTHING WAS BUILT, the unit does not take the call. A list that is too short: the walk cannot go on.
std::optional<Vector<Reference, 8>> layOutEntryOperandsOfCall(Walker&, const BodyValue& call, FunctionID callee, CodeUnit entered);

// The one coroutine that the Call surely enters (findOnlyCallee()), with a body, one result and a CoroutineBody that exists; no body of a module. It builds nothing. Null: there is none.
FunctionID findCoroutineCalledBy(Walker&, const BodyValue& call);
// CallDirect role=CoroutineBody of the entry operands and the driver behind them, with Link's effects of the callee. It leaves arrivals. Null: the walk cannot go on.
Reference buildStartOfBodyOfCoroutine(Walker&, const BodyValue& call, FunctionID callee, std::span<const Reference> entryOperands, Reference driver);
// The one operation that takes the value, as it is or through opcodesThatStateTypes. Null: several take it, none, or a terminator or an edge does.
ValueID findOnlyTakerBehindCasts(const BodyIndex&, ValueID);
// For replaceSuspensionOfCoroutine(). A call in a copy (C3) has awaited what the Await takes: it is bound to its operand. False: nothing was done.
bool takeWhatCallInCopyHasAwaited(Walker&, const BodyValue& await);

// ---- C8. AOTWalkCoroutineGuards.cpp
// The ramp of the coroutine tests its parameters: the function has a CoroutineBody AND a SlowBody. CallingConventions::typeBehindGuard() then speaks of the CoroutineBody. False: one body.
bool rampTestsParametersOf(const CallingConventions&, FunctionID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
