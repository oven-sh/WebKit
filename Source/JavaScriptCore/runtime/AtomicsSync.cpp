/*
 * Copyright (C) 2026 Jordan Hall. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "AtomicsSync.h"

#include "BuiltinNames.h"
#include "JSArrayBuffer.h"
#include "Options.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSSharedStruct.h"
#include "ObjectConstructor.h"
#include "TypedArrayController.h"
#include "WaiterListManager.h"
#include <atomic>
#include <thread>
#include <wtf/MonotonicTime.h>

namespace JSC {

// Atomics.Mutex and Atomics.Condition (https://github.com/tc39/proposal-structs).
//
// A mutex or a condition is an ordinary, sealed object with a null prototype (a shared struct with no fields) that
// holds one private property: a SharedArrayBuffer of two int32s that is the whole of its state. Because the state
// lives in a SharedArrayBuffer, an embedder that already shares SharedArrayBuffers between agents (a structured clone
// passes it by reference) shares the mutex as well: it rebuilds the wrapper around the received buffer with
// createAtomicsMutex() / createAtomicsCondition().
//
// The mutex is the classic three-state futex lock in the first int32 (0 unlocked, 1 locked, 2 locked with waiters);
// the second int32 is the agent that holds it (IsLockedBy), 0 when nobody does, which is how a mutex that its own
// holder locks again is found out (a deadlock). The condition counts the waiters that have registered and not been
// claimed by a notify (second int32) and the notifications that were handed out and not yet taken (first int32, the
// word that parked waiters sleep on), so that notify(count) wakes exactly count of the waiters that were registered
// when it ran (see waitOnCondition and conditionNotify). Both park through WaiterListManager, the machinery behind Atomics.wait, so a thread that
// blocks here is woken by notify and can be interrupted by a VM termination request.

namespace {

enum class LockResult : uint8_t { Locked, Deadlock, TimedOut, Terminated, WaitNotAllowed };

JSC_DECLARE_HOST_FUNCTION(mutexCall);
JSC_DECLARE_HOST_FUNCTION(mutexConstruct);
JSC_DECLARE_HOST_FUNCTION(mutexLock);
JSC_DECLARE_HOST_FUNCTION(mutexLockIfAvailable);
JSC_DECLARE_HOST_FUNCTION(tokenUnlock);
JSC_DECLARE_HOST_FUNCTION(tokenCall);
JSC_DECLARE_HOST_FUNCTION(tokenConstruct);
JSC_DECLARE_HOST_FUNCTION(tokenDispose);
JSC_DECLARE_HOST_FUNCTION(tokenLocked);
JSC_DECLARE_HOST_FUNCTION(conditionCall);
JSC_DECLARE_HOST_FUNCTION(conditionConstruct);
JSC_DECLARE_HOST_FUNCTION(conditionWait);
JSC_DECLARE_HOST_FUNCTION(conditionWaitFor);
JSC_DECLARE_HOST_FUNCTION(conditionNotify);

// The timeout of lockIfAvailable and waitFor: it has to be a Number (nothing is converted). NaN and +Infinity wait
// forever, -Infinity and anything below 0 do not wait.
std::optional<Seconds> timeoutArgument(JSGlobalObject* globalObject, JSValue value, ThrowScope& scope)
{
    if (!value.isNumber()) {
        throwTypeError(globalObject, scope, "The timeout must be a Number"_s);
        return std::nullopt;
    }
    double milliseconds = value.asNumber();
    if (std::isnan(milliseconds) || milliseconds == std::numeric_limits<double>::infinity())
        return Seconds::infinity();
    return std::max(Seconds::fromMilliseconds(milliseconds), 0_s);
}

// Agents are told apart by a number that each thread takes once.
int32_t currentAgentId()
{
    static std::atomic<int32_t> nextAgentId { 1 };
    thread_local int32_t agentId = nextAgentId.fetch_add(1);
    return agentId;
}

bool canSuspend(VM& vm)
{
    return vm.m_typedArrayController->isAtomicsWaitAllowedOnCurrentThread();
}

// The word that is the state of a mutex or a condition, or null if `value` is not one.
// The state of both kinds is two int32s: the lock or sequence word and, for a mutex, the agent that holds it.
constexpr size_t stateByteLength = 2 * sizeof(int32_t);

int32_t* stateWord(VM& vm, JSValue value, const Identifier& brand)
{
    JSObject* object = value.getObject();
    if (!object)
        return nullptr;
    JSValue buffer = object->getDirect(vm, brand);
    if (!buffer || !buffer.isObject())
        return nullptr;
    auto* arrayBuffer = dynamicDowncast<JSArrayBuffer>(buffer.getObject());
    if (!arrayBuffer || !arrayBuffer->impl() || arrayBuffer->impl()->byteLength() < stateByteLength)
        return nullptr;
    return static_cast<int32_t*>(arrayBuffer->impl()->data());
}

int32_t* mutexWord(JSGlobalObject* globalObject, VM& vm, JSValue value, ThrowScope& scope)
{
    int32_t* word = stateWord(vm, value, vm.propertyNames->builtinNames().atomicsMutexStatePrivateName());
    if (!word)
        throwTypeError(globalObject, scope, "Expected an Atomics.Mutex"_s);
    return word;
}

int32_t* conditionWord(JSGlobalObject* globalObject, VM& vm, JSValue value, ThrowScope& scope)
{
    int32_t* word = stateWord(vm, value, vm.propertyNames->builtinNames().atomicsConditionStatePrivateName());
    if (!word)
        throwTypeError(globalObject, scope, "Expected an Atomics.Condition"_s);
    return word;
}

bool isHeldByCurrentAgent(int32_t* word)
{
    return std::atomic_ref<int32_t>(word[0]).load() != 0 && std::atomic_ref<int32_t>(word[1]).load() == currentAgentId();
}

LockResult acquired(int32_t* word)
{
    std::atomic_ref<int32_t>(word[1]).store(currentAgentId());
    return LockResult::Locked;
}

// LockMutex: a mutex that this agent holds is a deadlock whatever the timeout, and with a timeout of 0 a mutex that
// another agent holds is timed out without waiting.
LockResult lockWord(VM& vm, int32_t* word, Seconds timeout)
{
    std::atomic_ref<int32_t> state(word[0]);
    int32_t expected = 0;
    if (state.compare_exchange_strong(expected, 1))
        return acquired(word);
    if (isHeldByCurrentAgent(word))
        return LockResult::Deadlock;
    if (timeout == 0_s)
        return LockResult::TimedOut;

    MonotonicTime deadline = MonotonicTime::timePointFromNow(timeout);
    while (true) {
        // Announce a waiter. If the lock was free after all, this thread holds it now (as state 2, which only
        // costs the next unlock a notify).
        if (state.exchange(2) == 0)
            return acquired(word);

        Seconds remaining = Seconds::infinity();
        if (timeout != Seconds::infinity()) {
            remaining = deadline - MonotonicTime::now();
            if (remaining <= 0_s)
                return LockResult::TimedOut;
        }
        // Blocking is refused wherever Atomics.wait is, for the same reason.
        if (!canSuspend(vm))
            return LockResult::WaitNotAllowed;
        switch (WaiterListManager::singleton().waitSync(vm, word, 2, remaining)) {
        case WaiterListManager::WaitSyncResult::Terminated:
            return LockResult::Terminated;
        case WaiterListManager::WaitSyncResult::TimedOut: {
            int32_t last = 0;
            return state.compare_exchange_strong(last, 2) ? acquired(word) : LockResult::TimedOut;
        }
        case WaiterListManager::WaitSyncResult::OK:
        case WaiterListManager::WaitSyncResult::NotEqual:
            break;
        }
    }
}

void unlockWord(int32_t* word)
{
    std::atomic_ref<int32_t>(word[1]).store(0);
    std::atomic_ref<int32_t> state(word[0]);
    if (state.exchange(0) == 2)
        WaiterListManager::singleton().notifyWaiter(word, 1);
}

JSObject* createStateObject(JSGlobalObject* globalObject, const Identifier& brand, JSArrayBuffer* buffer)
{
    VM& vm = globalObject->vm();
    JSObject* object = constructEmptyObject(vm, globalObject->nullPrototypeObjectStructure());
    object->putDirect(vm, brand, buffer, 0);
    JSObject::preventExtensions(object, globalObject);
    return object;
}

// A mutex or condition is one object per state in a global object, so that the object that is stored in a shared
// struct or a SharedArray is the object that reading it gives back (see SharedStructWrapperCache). The state is
// identified by the address of its memory, which every agent sees at the same place.
JSObject* registerStateWrapper(JSGlobalObject* globalObject, JSArrayBuffer* state, JSObject* wrapper)
{
    if (state->impl())
        globalObject->sharedStructWrapperCache().add(globalObject->vm(), state->impl()->data(), wrapper);
    return wrapper;
}

JSObject* findStateWrapper(JSGlobalObject* globalObject, JSArrayBuffer* state)
{
    return state->impl() ? globalObject->sharedStructWrapperCache().find(state->impl()->data()) : nullptr;
}

JSArrayBuffer* createStateBuffer(JSGlobalObject* globalObject, ThrowScope& scope)
{
    VM& vm = globalObject->vm();
    RefPtr<ArrayBuffer> contents = ArrayBuffer::tryCreateShared(vm, 2, sizeof(int32_t), stateByteLength);
    if (!contents) {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    return JSArrayBuffer::create(vm, globalObject->arrayBufferStructure(ArrayBufferSharingMode::Shared), WTF::move(contents));
}

// The unlock token: an object that remembers its mutex until it is unlocked once.
JSObject* createToken(JSGlobalObject* globalObject, JSObject* mutex)
{
    VM& vm = globalObject->vm();
    // The prototype is kept under a private name on the global object, not looked up through the Atomics object that
    // a program can change.
    JSValue storedPrototype = globalObject->getDirect(vm, vm.propertyNames->builtinNames().atomicsTokenPrototypePrivateName());
    JSObject* tokenPrototype = storedPrototype ? storedPrototype.getObject() : nullptr;
    if (!tokenPrototype)
        tokenPrototype = globalObject->objectPrototype();
    JSObject* token = constructEmptyObject(vm, globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, tokenPrototype, JSFinalObject::defaultInlineCapacity));
    token->putDirect(vm, vm.propertyNames->builtinNames().atomicsMutexTokenPrivateName(), mutex, 0);
    return token;
}

Identifier tokenName(VM& vm)
{
    return vm.propertyNames->builtinNames().atomicsMutexTokenPrivateName();
}

// The mutex a token holds, null for an empty token. `isToken` says whether `value` is an UnlockToken at all.
JSObject* tokenMutex(VM& vm, JSValue value, bool& isToken)
{
    JSObject* object = value.getObject();
    JSValue held = object ? object->getDirect(vm, tokenName(vm)) : JSValue();
    isToken = !!held;
    return held && held.isObject() ? held.getObject() : nullptr;
}

// The optional token argument of lock and lockIfAvailable: undefined, or an empty UnlockToken to be filled.
bool optionalToken(JSGlobalObject* globalObject, VM& vm, JSValue argument, JSObject*& token, ThrowScope& scope)
{
    token = nullptr;
    if (argument.isUndefined())
        return true;
    bool isToken;
    JSObject* held = tokenMutex(vm, argument, isToken);
    if (!isToken) {
        throwTypeError(globalObject, scope, "Expected an Atomics.Mutex.UnlockToken"_s);
        return false;
    }
    if (held) {
        throwTypeError(globalObject, scope, "The Atomics.Mutex.UnlockToken is already locked"_s);
        return false;
    }
    token = argument.getObject();
    return true;
}

// What a successful lock returns: the caller's empty token, now holding the mutex, or a new token.
JSValue lockedToken(JSGlobalObject* globalObject, JSObject* mutex, JSObject* given)
{
    if (!given)
        return createToken(globalObject, mutex);
    VM& vm = globalObject->vm();
    given->putDirect(vm, tokenName(vm), mutex, 0);
    return given;
}

// The state word of the mutex that a non-empty token holds.
int32_t* tokenWord(JSGlobalObject* globalObject, VM& vm, JSValue value, ThrowScope& scope)
{
    bool isToken;
    JSObject* held = tokenMutex(vm, value, isToken);
    if (!isToken) {
        throwTypeError(globalObject, scope, "Expected an Atomics.Mutex.UnlockToken"_s);
        return nullptr;
    }
    if (!held) {
        throwTypeError(globalObject, scope, "The Atomics.Mutex.UnlockToken is not locked"_s);
        return nullptr;
    }
    return mutexWord(globalObject, vm, held, scope);
}

JSC_DEFINE_HOST_FUNCTION(mutexCall, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "Atomics.Mutex must be called with new"_s);
}

JSC_DEFINE_HOST_FUNCTION(mutexConstruct, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArrayBuffer* buffer = createStateBuffer(globalObject, scope);
    RETURN_IF_EXCEPTION(scope, { });
    JSObject* mutex = createStateObject(globalObject, vm.propertyNames->builtinNames().atomicsMutexStatePrivateName(), buffer);
    return JSValue::encode(registerStateWrapper(globalObject, buffer, mutex));
}

JSC_DEFINE_HOST_FUNCTION(mutexLock, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int32_t* word = mutexWord(globalObject, vm, callFrame->argument(0), scope);
    RETURN_IF_EXCEPTION(scope, { });
    JSObject* token;
    if (!optionalToken(globalObject, vm, callFrame->argument(1), token, scope))
        return { };
    if (!canSuspend(vm))
        return throwVMTypeError(globalObject, scope, "Atomics.Mutex.lock cannot block on the current thread."_s);

    switch (lockWord(vm, word, Seconds::infinity())) {
    case LockResult::Locked:
        return JSValue::encode(lockedToken(globalObject, callFrame->argument(0).getObject(), token));
    case LockResult::Deadlock:
        return throwVMTypeError(globalObject, scope, "Atomics.Mutex.lock: the current agent already holds the mutex"_s);
    case LockResult::TimedOut:
        return throwVMError(globalObject, scope, createError(globalObject, "Atomics.Mutex.lock timed out"_s));
    case LockResult::Terminated:
        return { };
    case LockResult::WaitNotAllowed:
        return throwVMTypeError(globalObject, scope, "Atomics.Mutex.lock cannot block on the current thread."_s);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_HOST_FUNCTION(mutexLockIfAvailable, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int32_t* word = mutexWord(globalObject, vm, callFrame->argument(0), scope);
    RETURN_IF_EXCEPTION(scope, { });
    JSObject* token;
    if (!optionalToken(globalObject, vm, callFrame->argument(2), token, scope))
        return { };
    auto timeout = timeoutArgument(globalObject, callFrame->argument(1), scope);
    if (!timeout)
        return { };
    // With a timeout of 0 this never waits, so it works in an agent that cannot suspend.
    if (*timeout != 0_s && !canSuspend(vm))
        return throwVMTypeError(globalObject, scope, "Atomics.Mutex.lockIfAvailable cannot block on the current thread."_s);

    switch (lockWord(vm, word, *timeout)) {
    case LockResult::Locked:
        return JSValue::encode(lockedToken(globalObject, callFrame->argument(0).getObject(), token));
    case LockResult::Deadlock:
        return throwVMTypeError(globalObject, scope, "Atomics.Mutex.lockIfAvailable: the current agent already holds the mutex"_s);
    case LockResult::TimedOut:
        return JSValue::encode(jsNull());
    case LockResult::Terminated:
        return { };
    case LockResult::WaitNotAllowed:
        return throwVMTypeError(globalObject, scope, "Atomics.Mutex.lockIfAvailable cannot block on the current thread."_s);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_HOST_FUNCTION(tokenCall, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "Atomics.Mutex.UnlockToken must be called with new"_s);
}

// new Atomics.Mutex.UnlockToken() is an empty token: it holds no mutex until it is passed to lock.
JSC_DEFINE_HOST_FUNCTION(tokenConstruct, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    JSValue storedPrototype = globalObject->getDirect(vm, vm.propertyNames->builtinNames().atomicsTokenPrototypePrivateName());
    JSObject* tokenPrototype = storedPrototype ? storedPrototype.getObject() : nullptr;
    if (!tokenPrototype)
        tokenPrototype = globalObject->objectPrototype();
    JSObject* token = constructEmptyObject(vm, globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, tokenPrototype, JSFinalObject::defaultInlineCapacity));
    token->putDirect(vm, tokenName(vm), jsNull(), 0);
    return JSValue::encode(token);
}

// Releases the mutex a token holds and empties the token. Answers whether it held one.
std::optional<bool> releaseToken(JSGlobalObject* globalObject, CallFrame* callFrame, ThrowScope& scope)
{
    VM& vm = globalObject->vm();
    bool isToken;
    JSObject* held = tokenMutex(vm, callFrame->thisValue(), isToken);
    if (!isToken) {
        throwTypeError(globalObject, scope, "Expected an Atomics.Mutex.UnlockToken"_s);
        return std::nullopt;
    }
    if (!held)
        return false;
    callFrame->thisValue().getObject()->putDirect(vm, tokenName(vm), jsNull(), 0);
    if (int32_t* word = stateWord(vm, held, vm.propertyNames->builtinNames().atomicsMutexStatePrivateName()))
        unlockWord(word);
    return true;
}

JSC_DEFINE_HOST_FUNCTION(tokenUnlock, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto released = releaseToken(globalObject, callFrame, scope);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(*released));
}

JSC_DEFINE_HOST_FUNCTION(tokenDispose, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    releaseToken(globalObject, callFrame, scope);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(tokenLocked, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isToken;
    JSObject* held = tokenMutex(vm, callFrame->thisValue(), isToken);
    if (!isToken)
        return throwVMTypeError(globalObject, scope, "Expected an Atomics.Mutex.UnlockToken"_s);
    return JSValue::encode(jsBoolean(!!held));
}

JSC_DEFINE_HOST_FUNCTION(conditionCall, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "Atomics.Condition must be called with new"_s);
}

JSC_DEFINE_HOST_FUNCTION(conditionConstruct, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArrayBuffer* buffer = createStateBuffer(globalObject, scope);
    RETURN_IF_EXCEPTION(scope, { });
    JSObject* condition = createStateObject(globalObject, vm.propertyNames->builtinNames().atomicsConditionStatePrivateName(), buffer);
    return JSValue::encode(registerStateWrapper(globalObject, buffer, condition));
}

// A notification that was handed out by a notify and has not been taken yet.
bool takeNotification(int32_t* condition)
{
    std::atomic_ref<int32_t> notifications(condition[0]);
    int32_t available = notifications.load();
    while (available > 0) {
        if (notifications.compare_exchange_weak(available, available - 1))
            return true;
    }
    return false;
}

// Registers as a waiter, releases the mutex, which the caller holds, parks until notified and takes the mutex again.
// Answers whether it was notified (as opposed to timing out). The waiter is registered, and so can be claimed by a
// notify, before the mutex is released: whoever holds the mutex after that and notifies counts it.
// If the mutex was released and not taken again (termination), the token no longer holds it: it is emptied, so that
// nothing can later unlock a mutex that another agent may own by then.
std::optional<bool> waitOnCondition(VM& vm, int32_t* condition, int32_t* mutex, Seconds timeout, JSObject* token)
{
    if (!canSuspend(vm))
        return std::nullopt;
    std::atomic_ref<int32_t> waiters(condition[1]);
    waiters.fetch_add(1);
    unlockWord(mutex);
    auto lostTheMutex = [&]() -> std::optional<bool> {
        token->putDirect(vm, tokenName(vm), jsNull(), 0);
        return std::nullopt;
    };

    MonotonicTime deadline = MonotonicTime::timePointFromNow(timeout);
    bool notified = false;
    bool terminated = false;
    while (true) {
        if (takeNotification(condition)) {
            notified = true;
            break;
        }
        Seconds remaining = Seconds::infinity();
        if (timeout != Seconds::infinity()) {
            remaining = deadline - MonotonicTime::now();
            if (remaining <= 0_s)
                break;
        }
        // Sleeps while no notification is available; a wake that someone else's notification took is not one for this waiter.
        if (WaiterListManager::singleton().waitSync(vm, condition, 0, remaining) == WaiterListManager::WaitSyncResult::Terminated) {
            terminated = true;
            break;
        }
    }
    if (!notified) {
        // Gives up the registration. If a notify has claimed this waiter already, its notification is on its way:
        // take it, so that it is not left for a later waiter.
        int32_t registered = waiters.load();
        bool unregistered = false;
        while (registered > 0) {
            if (waiters.compare_exchange_weak(registered, registered - 1)) {
                unregistered = true;
                break;
            }
        }
        if (!unregistered) {
            while (!takeNotification(condition))
                std::this_thread::yield();
            notified = true;
        }
    }
    if (terminated)
        return lostTheMutex();
    if (lockWord(vm, mutex, Seconds::infinity()) != LockResult::Locked)
        return lostTheMutex();
    return notified;
}

JSC_DEFINE_HOST_FUNCTION(conditionWait, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int32_t* condition = conditionWord(globalObject, vm, callFrame->argument(0), scope);
    RETURN_IF_EXCEPTION(scope, { });
    int32_t* mutex = tokenWord(globalObject, vm, callFrame->argument(1), scope);
    RETURN_IF_EXCEPTION(scope, { });

    auto notified = waitOnCondition(vm, condition, mutex, Seconds::infinity(), callFrame->argument(1).getObject());
    if (!notified) {
        if (!vm.hasPendingTerminationException())
            return throwVMTypeError(globalObject, scope, "Atomics.Condition.wait cannot be called from the current thread."_s);
        return { };
    }
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(conditionWaitFor, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int32_t* condition = conditionWord(globalObject, vm, callFrame->argument(0), scope);
    RETURN_IF_EXCEPTION(scope, { });
    int32_t* mutex = tokenWord(globalObject, vm, callFrame->argument(1), scope);
    RETURN_IF_EXCEPTION(scope, { });
    auto timeout = timeoutArgument(globalObject, callFrame->argument(2), scope);
    if (!timeout)
        return { };
    JSValue predicate = callFrame->argument(3);
    CallData callData;
    if (!predicate.isUndefined()) {
        callData = JSC::getCallData(predicate);
        if (callData.type == CallData::Type::None)
            return throwVMTypeError(globalObject, scope, "Atomics.Condition.waitFor: predicate must be a function"_s);
    }
    if (!canSuspend(vm))
        return throwVMTypeError(globalObject, scope, "Atomics.Condition.waitFor cannot be called from the current thread."_s);

    // The predicate is user code too: it may unlock the token and drop the last reference to the mutex. The state
    // memory is kept alive by this reference, and every wait first checks that the token still holds this mutex.
    bool isToken;
    JSObject* heldMutex = tokenMutex(vm, callFrame->argument(1), isToken);
    RefPtr<ArrayBuffer> keepMutexAlive = atomicsMutexState(vm, heldMutex)->impl();

    auto waitOnce = [&](Seconds remaining) -> std::optional<bool> {
        if (tokenMutex(vm, callFrame->argument(1), isToken) != heldMutex) {
            throwTypeError(globalObject, scope, "The Atomics.Mutex.UnlockToken was unlocked during waitFor"_s);
            return std::nullopt;
        }
        auto notified = waitOnCondition(vm, condition, mutex, remaining, callFrame->argument(1).getObject());
        if (!notified && !scope.exception() && !vm.hasPendingTermination())
            throwTypeError(globalObject, scope, "Atomics.Condition.waitFor cannot be called from the current thread."_s);
        return notified;
    };

    // The loop of the proposal: the predicate (if any) decides, then the time, and only then does the agent unlock
    // the mutex and park; with a timeout that has run out it returns false without unlocking.
    MonotonicTime deadline = MonotonicTime::timePointFromNow(*timeout);
    bool satisfied = false;
    while (true) {
        if (!predicate.isUndefined()) {
            MarkedArgumentBuffer noArguments;
            JSValue result = call(globalObject, predicate, callData, jsUndefined(), noArguments);
            RETURN_IF_EXCEPTION(scope, { });
            satisfied = result.toBoolean(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (satisfied)
            return JSValue::encode(jsBoolean(true));
        Seconds remaining = Seconds::infinity();
        if (*timeout != Seconds::infinity()) {
            remaining = deadline - MonotonicTime::now();
            if (remaining <= 0_s)
                return JSValue::encode(jsBoolean(false));
        }
        auto notified = waitOnce(remaining);
        RETURN_IF_EXCEPTION(scope, { });
        if (!notified)
            return { };
        satisfied = *notified;
    }
}

JSC_DEFINE_HOST_FUNCTION(conditionNotify, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int32_t* condition = conditionWord(globalObject, vm, callFrame->argument(0), scope);
    RETURN_IF_EXCEPTION(scope, { });

    // The count is an integral Number or +Infinity (the proposal says "or is not", which no value passes; "and" is meant).
    unsigned count = std::numeric_limits<unsigned>::max();
    JSValue countValue = callFrame->argument(1);
    if (!countValue.isUndefined()) {
        if (!countValue.isNumber())
            return throwVMTypeError(globalObject, scope, "Atomics.Condition.notify: the count must be a Number"_s);
        double requested = countValue.asNumber();
        if (requested != std::numeric_limits<double>::infinity()) {
            if (!std::isfinite(requested) || std::trunc(requested) != requested)
                return throwVMTypeError(globalObject, scope, "Atomics.Condition.notify: the count must be an integral Number or Infinity"_s);
            if (requested < 0)
                requested = 0;
            if (requested < std::numeric_limits<unsigned>::max())
                count = static_cast<unsigned>(requested);
        }
    }

    // Claims exactly count of the waiters that have registered (all of them for Infinity), then hands out that many
    // notifications and wakes the parked waiters to take them. Nothing is handed out for a count of 0.
    std::atomic_ref<int32_t> waiters(condition[1]);
    int32_t registered = waiters.load();
    unsigned claimed = 0;
    while (true) {
        unsigned n = registered > 0 ? std::min<unsigned>(count, registered) : 0;
        if (!n)
            return JSValue::encode(jsNumber(0));
        if (waiters.compare_exchange_weak(registered, registered - static_cast<int32_t>(n))) {
            claimed = n;
            break;
        }
    }
    std::atomic_ref<int32_t>(condition[0]).fetch_add(static_cast<int32_t>(claimed));
    WaiterListManager::singleton().notifyWaiter(condition, claimed);
    return JSValue::encode(jsNumber(claimed));
}

// A constructor without a prototype property: the instances of Atomics.Mutex and Atomics.Condition have a null
// prototype, as shared structs do.
JSFunction* makeConstructor(VM& vm, JSGlobalObject* globalObject, ASCIILiteral name, NativeFunction call, NativeFunction construct)
{
    return JSFunction::create(vm, globalObject, 0, String(name), call, ImplementationVisibility::Public, NoIntrinsic, construct);
}

void defineStatic(VM& vm, JSGlobalObject* globalObject, JSObject* target, ASCIILiteral name, unsigned length, NativeFunction function)
{
    target->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, name), length, function, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
}

} // namespace

void installAtomicsSync(VM& vm, JSGlobalObject* globalObject, JSObject* atomics)
{
    JSFunction* mutex = makeConstructor(vm, globalObject, "Mutex"_s, mutexCall, mutexConstruct);
    defineStatic(vm, globalObject, mutex, "lock"_s, 1, mutexLock);
    defineStatic(vm, globalObject, mutex, "lockIfAvailable"_s, 2, mutexLockIfAvailable);

    // The prototype of the unlock token has the properties that the proposal lists and no others.
    JSFunction* token = makeConstructor(vm, globalObject, "UnlockToken"_s, tokenCall, tokenConstruct);
    JSObject* tokenPrototype = constructEmptyObject(globalObject);
    token->putDirect(vm, vm.propertyNames->prototype, tokenPrototype, static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly));
    tokenPrototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "unlock"_s), 0, tokenUnlock, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    tokenPrototype->putDirect(vm, vm.propertyNames->disposeSymbol, JSFunction::create(vm, globalObject, 0, "[Symbol.dispose]"_s, tokenDispose, ImplementationVisibility::Public), static_cast<unsigned>(PropertyAttribute::DontEnum));
    tokenPrototype->putGetter(globalObject, Identifier::fromString(vm, "locked"_s),
        JSFunction::create(vm, globalObject, 0, "get locked"_s, tokenLocked, ImplementationVisibility::Public),
        static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::Accessor));
    mutex->putDirect(vm, Identifier::fromString(vm, "UnlockToken"_s), token, static_cast<unsigned>(PropertyAttribute::DontEnum));
    globalObject->putDirect(vm, vm.propertyNames->builtinNames().atomicsTokenPrototypePrivateName(), tokenPrototype, 0);

    JSFunction* condition = makeConstructor(vm, globalObject, "Condition"_s, conditionCall, conditionConstruct);
    defineStatic(vm, globalObject, condition, "wait"_s, 2, conditionWait);
    defineStatic(vm, globalObject, condition, "waitFor"_s, 3, conditionWaitFor);
    defineStatic(vm, globalObject, condition, "notify"_s, 1, conditionNotify);

    atomics->putDirect(vm, Identifier::fromString(vm, "Mutex"_s), mutex, static_cast<unsigned>(PropertyAttribute::DontEnum));
    atomics->putDirect(vm, Identifier::fromString(vm, "Condition"_s), condition, static_cast<unsigned>(PropertyAttribute::DontEnum));
}

// Whether a global object can have mutexes and conditions: reading one out of a shared struct must not run user code or
// throw, so nothing is looked up through the global object.
static bool atomicsInstalled(JSGlobalObject*)
{
    return Options::useStructs();
}

JSObject* createAtomicsMutex(JSGlobalObject* globalObject, JSArrayBuffer* state)
{
    VM& vm = globalObject->vm();
    // The same state is the same mutex object in one global object.
    if (JSObject* existing = findStateWrapper(globalObject, state)) {
        if (atomicsMutexState(vm, existing))
            return existing;
    }
    if (!atomicsInstalled(globalObject))
        return nullptr;
    return registerStateWrapper(globalObject, state, createStateObject(globalObject, vm.propertyNames->builtinNames().atomicsMutexStatePrivateName(), state));
}

JSObject* createAtomicsCondition(JSGlobalObject* globalObject, JSArrayBuffer* state)
{
    VM& vm = globalObject->vm();
    if (JSObject* existing = findStateWrapper(globalObject, state)) {
        if (atomicsConditionState(vm, existing))
            return existing;
    }
    if (!atomicsInstalled(globalObject))
        return nullptr;
    return registerStateWrapper(globalObject, state, createStateObject(globalObject, vm.propertyNames->builtinNames().atomicsConditionStatePrivateName(), state));
}

JSArrayBuffer* atomicsMutexState(VM& vm, JSValue value)
{
    JSObject* object = value.getObject();
    JSValue buffer = object ? object->getDirect(vm, vm.propertyNames->builtinNames().atomicsMutexStatePrivateName()) : JSValue();
    return buffer ? dynamicDowncast<JSArrayBuffer>(buffer.getObject()) : nullptr;
}

JSArrayBuffer* atomicsConditionState(VM& vm, JSValue value)
{
    JSObject* object = value.getObject();
    JSValue buffer = object ? object->getDirect(vm, vm.propertyNames->builtinNames().atomicsConditionStatePrivateName()) : JSValue();
    return buffer ? dynamicDowncast<JSArrayBuffer>(buffer.getObject()) : nullptr;
}

} // namespace JSC
