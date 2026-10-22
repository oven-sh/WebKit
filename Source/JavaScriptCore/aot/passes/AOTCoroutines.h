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

inline const CreatorRecord::Coroutine* coroutineOf(const Function& rampBodyOrWorker)
{
    return rampBodyOrWorker.creator.tryAs<CreatorRecord::Coroutine>();
}

inline const CreatorRecord::Coroutine& coroutineOfBody(const Function& body)
{
    RELEASE_ASSERT(body.role == FunctionRole::CoroutineBody);
    return body.creator.as<CreatorRecord::Coroutine>();
}

// An async function that never suspends has a worker in place of a body: an ordinary function without a driver, which returns and throws what settles the promise.
inline bool isAsyncWorker(const Function& function)
{
    return function.role == FunctionRole::AsyncWorker;
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
ValueID appendAsyncIteratorStep(IRBuilder&, ValueID iterator, ValueID sent);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
