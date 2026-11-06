/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTPass.h"

namespace JSC { namespace AOT { namespace Typed {

class IRBuilder;
class PassRunner;

void splitCoroutines(ProgramEditor&, const OptimizationInputs&, const PassRunner&);
// A body of a generator becomes two functions that keep what lives across a yield in the words of the SavedFrame, as JSValues: the body itself, which runs from the call to the first yield,
// and a function that the step stub enters with (frame, mode, sent) and that goes on behind any yield. A yield in either stores the words and returns, so no machine frame is saved or restored.
void moveGeneratorFramesToHeap(ProgramEditor&, const OptimizationInputs&, const PassRunner&);
// A value that lives across many suspensions of a body is stored in a word of a cell where it is defined and read from there where it is used.
// In the machine frame every suspension would save it, list it and restore it: n suspensions that m values live across cost n * m, in the build and in the run.
void keepValuesInHeapWords(ProgramEditor&, const OptimizationInputs&, const PassRunner&);

inline const CreatorRecord::Coroutine* coroutineOf(const Function& rampBodyOrWorker)
{
    return rampBodyOrWorker.creator.tryAs<CreatorRecord::Coroutine>();
}

inline const CreatorRecord::Coroutine& coroutineOfBody(const Function& body)
{
    RELEASE_ASSERT(body.role == FunctionRole::CoroutineBody);
    return body.creator.as<CreatorRecord::Coroutine>();
}

// The promise of an async function (undefined when the caller awaits the call itself), or the generator object.
inline ValueID driverOf(const Function& body)
{
    return body.at(BlockID(0u)).parameters[*coroutineOfBody(body).promiseParameter];
}

// Whether ResolveAwaitedCalls, as the function stands, makes this direct call of a ramp and the await of its result enter the callee without a promise.
bool willEnterAwaitedCalleeWithoutPromise(FunctionPassContext&, ValueID call);

// `await generator.next(value)` becomes the step that serves `for await`, where nothing but `value` and `done` of the result is read. It comes in front of PlaceTypeTests, which tests the result for what it guesses.
unsigned stepAwaitedAsyncGenerators(FunctionPassContext&);

// For an unsplit async function or async generator, or the body of one. The results are those of StepAsyncGenerator.
ValueID appendAsyncIteratorStep(IRBuilder&, ValueID iterator, ValueID sent, std::span<const ValueID> held);

// What CreateIterResultObject makes.
Value makeIteratorResultObject(const Program&, ValueID value, ValueID done, AllocationSiteID = { });

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
