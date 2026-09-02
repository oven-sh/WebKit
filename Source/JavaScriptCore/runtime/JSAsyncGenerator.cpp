/*
 * Copyright (C) 2019-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "JSAsyncGenerator.h"

#include "DeferGCInlines.h"
#include "JSAsyncFunctionGenerator.h"
#include "JSAsyncGeneratorInlines.h"
#include "JSCInlines.h"
#include "JSInternalFieldObjectImplInlines.h"
#include "JSPromise.h"
#include "JSPromiseReaction.h"
#include "ObjectConstructor.h"

namespace JSC {

const ClassInfo JSAsyncGenerator::s_info = { "AsyncGenerator"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSAsyncGenerator) };

JSAsyncGenerator* JSAsyncGenerator::create(VM& vm, Structure* structure)
{
    JSAsyncGenerator* generator = new (NotNull, allocateCell<JSAsyncGenerator>(vm)) JSAsyncGenerator(vm, structure);
    generator->finishCreation(vm);
    return generator;
}

JSAsyncGenerator* JSAsyncGenerator::createWithInitialValues(VM& vm, Structure* structure)
{
    JSAsyncGenerator* generator = new (NotNull, allocateCell<JSAsyncGenerator>(vm)) JSAsyncGenerator(vm, structure);
    generator->finishCreation(vm);
    return generator;
}

Structure* JSAsyncGenerator::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(JSAsyncGeneratorType, StructureFlags), info());
}

JSAsyncGenerator::JSAsyncGenerator(VM& vm, Structure* structure)
    : Base(vm, structure)
{
}

void JSAsyncGenerator::finishCreation(VM& vm)
{
    Base::finishCreation(vm);
    auto values = initialValues();
    ASSERT(values.size() == numberOfInternalFields);
    for (unsigned index = 0; index < values.size(); ++index)
        internalField(index).set(vm, this, values[index]);
}

template<typename Visitor>
void JSAsyncGenerator::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<JSAsyncGenerator>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
}

DEFINE_VISIT_CHILDREN(JSAsyncGenerator);

// GIL-off, several threads can call next(), return() and throw() on one generator. The spec's
// enqueue decides from the state alone whether to resume, so two threads could both see a
// suspended state and both resume the body, or an enqueue could see a running state just before
// the driver suspends and never be resumed.
//
// GIL-off, every queue edit, and every store of a settled state (suspended-start, suspended-yield
// or completed), happens under the cell lock. A thread drives the generator from the enqueue that
// finds it settled with an empty queue until it stores a settled state again, which it does only
// after finding the queue empty under the lock (retireIfQueueEmptyGILOff). So an enqueuer that
// finds the generator settled with an empty queue drives it, and any other enqueuer leaves its
// request to the driver, which finds it before it can retire. This is the spec's order, so a
// request made on the driver's own thread during a settlement is queued exactly as the spec says.
// The lock is never held across JS, a settlement or an allocation that can collect.
static bool isSettledState(int32_t state)
{
    return state == static_cast<int32_t>(JSAsyncGenerator::AsyncGeneratorState::Init)
        || state == static_cast<int32_t>(JSAsyncGenerator::AsyncGeneratorState::Completed)
        || JSAsyncGenerator::isSuspendedYieldState(state);
}

auto JSAsyncGenerator::enqueueGILOff(VM& vm, JSValue value, int32_t resumeMode, JSObject* settlementTarget) -> GILOffEnqueueAction
{
    ASSERT(vm.gilOff());
    DeferGC deferGC(vm); // enqueue can allocate a queue entry, and no collection may start under the cell lock.
    Locker locker { cellLock() };

    int32_t state = this->state();
    if (!isQueueEmpty() || !isSettledState(state)) {
        enqueue(vm, value, resumeMode, settlementTarget);
        return GILOffEnqueueAction::None;
    }

    bool isInit = state == static_cast<int32_t>(AsyncGeneratorState::Init);
    bool isCompleted = state == static_cast<int32_t>(AsyncGeneratorState::Completed);
    switch (static_cast<JSGenerator::ResumeMode>(resumeMode)) {
    case JSGenerator::ResumeMode::NormalMode:
        if (isCompleted)
            return GILOffEnqueueAction::SettleCompleted;
        enqueue(vm, value, resumeMode, settlementTarget);
        return GILOffEnqueueAction::Resume;
    case JSGenerator::ResumeMode::ReturnMode:
        enqueue(vm, value, resumeMode, settlementTarget);
        if (isInit || isCompleted) {
            setState(static_cast<int32_t>(AsyncGeneratorState::DrainingQueue));
            return GILOffEnqueueAction::AwaitReturn;
        }
        return GILOffEnqueueAction::Resume;
    case JSGenerator::ResumeMode::ThrowMode:
        if (isInit) {
            setState(static_cast<int32_t>(AsyncGeneratorState::Completed));
            return GILOffEnqueueAction::SettleCompleted;
        }
        if (isCompleted)
            return GILOffEnqueueAction::SettleCompleted;
        enqueue(vm, value, resumeMode, settlementTarget);
        return GILOffEnqueueAction::Resume;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSObject* JSAsyncGenerator::dequeueGILOff(VM& vm)
{
    ASSERT(vm.gilOff());
    Locker locker { cellLock() };
    return dequeue(vm);
}

// Called by the driver where the spec checks whether the queue is empty. Returns true, having
// stored settledState, if it is; the caller then no longer drives and must not touch the
// generator. Returns false if it is not, and the head request is stable: enqueuers only append.
bool JSAsyncGenerator::retireIfQueueEmptyGILOff(int32_t settledState)
{
    ASSERT(isSettledState(settledState));
    Locker locker { cellLock() };
    if (!isQueueEmpty())
        return false;
    setState(settledState);
    return true;
}

} // namespace JSC
