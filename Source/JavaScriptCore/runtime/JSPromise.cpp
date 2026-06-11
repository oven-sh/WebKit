/*
 * Copyright (C) 2013-2021 Apple Inc. All rights reserved.
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
#include "JSPromise.h"

#include "BuiltinNames.h"
#include "DeferredWorkTimer.h"
#include "ErrorInstance.h"
#include "GlobalObjectMethodTable.h"
#include "JSCInlines.h"
#include "JSFunctionWithFields.h"
#include "JSMicrotask.h"
#include "JSPromiseCombinatorsContext.h"
#include "JSPromiseCombinatorsGlobalContext.h"
#include "JSPromiseConstructor.h"
#include "JSPromisePrototype.h"
#include "JSPromiseReaction.h"
#include "Microtask.h"
#include "ObjectConstructor.h"
#include "TopExceptionScope.h"
#include "VMInlines.h"
#if USE(BUN_JSC_ADDITIONS)
#include "InternalFieldTuple.h"
#endif

namespace JSC {

// =============================================================================
// UNGIL §E.1b (ANNEX E1B + r16 F3/SD15 + ANNEX ALS1; U-T9) — same-library
// seams, self-declared identically to their defining TUs (no header changes;
// the currentThreadHoldsEntryToken pattern, JSLock.cpp):
//
//  - notifyPromiseRejectionTrackerCrossThreadAware (VM.cpp, U-T8e): the SD15
//    invocation gate. Carrier / GIL-on / flag-off: invokes the methodTable
//    hook inline, bit-identical to the landed call sites. Spawned GIL-off:
//    appends a tracker record {promise Strong, operation} to the handoff
//    queue, flushed + EXECUTED at the §F.1 carrier drain points (a report
//    may arrive a drain late; never lost while the carrier drains — SD15).
//    This file's four tracker call sites are re-pointed at it (U-T9 closes
//    the U-T8e "call-site status" note).
//  - threadAsyncContextData (JSGlobalObject.cpp, U-T8b storage / U-T9
//    consumption, ALS1.3): the ALS cursor, rerouted PER-LITE GIL-off
//    ("current async context" is thread-local by definition); flag-off /
//    GIL-on / main carrier it returns the in-object m_asyncContextData
//    BYTE-IDENTICALLY. Capture stays PER-REACTION at registration time; the
//    captured tuple is an ordinary shared-heap cell carried BY THE JOB, so
//    SD10 thread-migrating continuations PRESERVE ALS (ALS1.1) — the §E.1b
//    enqueue edges (I11 own-queue / §E.4 ThreadTask append under inboxLock)
//    carry release/acquire, so the settling thread reads an initialized
//    tuple (ALS1.2). RESTORE-side reroute (JSMicrotask.cpp save/write/run/
//    write-back brackets; JSPromisePrototype.cpp then() fast-path capture)
//    is RECORDED OPEN — both TUs are outside U-T9's owned file set — so the
//    accessor's per-lite routing is GATED OFF at its definition
//    (perLiteAsyncContextCursorEnabled, JSGlobalObject.cpp): a capture-only
//    reroute would split the regime (spawned brackets writing the SHARED
//    cursor while captures read a never-bracket-written per-lite copy). The
//    capture sites in THIS file are re-pointed at the seam now; flipping the
//    one constant together with the restore-side slice lands ALS1.3 whole.
// =============================================================================
void notifyPromiseRejectionTrackerCrossThreadAware(JSGlobalObject*, JSPromise*, JSPromiseRejectionOperation);
#if USE(BUN_JSC_ADDITIONS)
InternalFieldTuple* threadAsyncContextData(JSGlobalObject*);
#endif

const ClassInfo JSPromise::s_info = { "Promise"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSPromise) };

JSPromise* JSPromise::create(VM& vm, Structure* structure)
{
    JSPromise* promise = new (NotNull, allocateCell<JSPromise>(vm)) JSPromise(vm, structure);
    promise->finishCreation(vm);
    return promise;
}

JSPromise* JSPromise::createWithInitialValues(VM& vm, Structure* structure)
{
    return create(vm, structure);
}

Structure* JSPromise::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(JSPromiseType, StructureFlags), info());
}

JSPromise::JSPromise(VM& vm, Structure* structure)
    : Base(vm, structure)
{
}

template<typename Visitor>
void JSPromise::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<JSPromise>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    if (JSCell* payload = thisObject->m_packed.pointer())
        visitor.appendUnbarriered(payload);
    visitor.append(thisObject->m_slot);
}

DEFINE_VISIT_CHILDREN(JSPromise);

JSValue JSPromise::createNewPromiseCapability(JSGlobalObject* globalObject, JSValue constructor)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto [promise, resolve, reject] = newPromiseCapability(globalObject, constructor);
    RETURN_IF_EXCEPTION(scope, { });
    return createPromiseCapability(vm, globalObject, promise, resolve, reject);
}

JSValue JSPromise::createPromiseCapability(VM& vm, JSGlobalObject* globalObject, JSObject* promise, JSObject* resolve, JSObject* reject)
{
    auto* capability = constructEmptyObject(vm, globalObject->promiseCapabilityObjectStructure());
    capability->putDirectOffset(vm, promiseCapabilityResolvePropertyOffset, resolve);
    capability->putDirectOffset(vm, promiseCapabilityRejectPropertyOffset, reject);
    capability->putDirectOffset(vm, promiseCapabilityPromisePropertyOffset, promise);
    return capability;
}

std::tuple<JSObject*, JSObject*, JSObject*> JSPromise::newPromiseCapability(JSGlobalObject* globalObject, JSValue constructor)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (constructor == globalObject->promiseConstructor()) {
        auto* promise = JSPromise::create(vm, globalObject->promiseStructure());
        auto [resolve, reject] = promise->createFirstResolvingFunctions(vm, globalObject);
        return { promise, resolve, reject };
    }

    auto* executor = JSFunctionWithFields::create(vm, globalObject, vm.promiseCapabilityExecutorExecutable(), 2, emptyString());
    executor->setField(vm, JSFunctionWithFields::Field::ExecutorResolve, jsUndefined());
    executor->setField(vm, JSFunctionWithFields::Field::ExecutorReject, jsUndefined());

    MarkedArgumentBuffer args;
    args.append(executor);
    ASSERT(!args.hasOverflowed());
    JSObject* newObject = construct(globalObject, constructor, args, "argument is not a constructor"_s);
    RETURN_IF_EXCEPTION(scope, { });

    JSValue resolve = executor->getField(JSFunctionWithFields::Field::ExecutorResolve);
    JSValue reject = executor->getField(JSFunctionWithFields::Field::ExecutorReject);
    if (!resolve.isCallable()) [[unlikely]] {
        throwTypeError(globalObject, scope, "executor did not take a resolve function"_s);
        return { };
    }

    if (!reject.isCallable()) [[unlikely]] {
        throwTypeError(globalObject, scope, "executor did not take a reject function"_s);
        return { };
    }

    return { newObject, asObject(resolve), asObject(reject) };
}

JSPromise::DeferredData JSPromise::createDeferredData(JSGlobalObject* globalObject, JSPromiseConstructor* promiseConstructor)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto [ promiseCapability, resolveCapability, rejectCapability ] = newPromiseCapability(globalObject, promiseConstructor);
    RETURN_IF_EXCEPTION(scope, { });
    auto* promise = dynamicDowncast<JSPromise>(promiseCapability);
    auto* resolve = dynamicDowncast<JSFunction>(resolveCapability);
    auto* reject  = dynamicDowncast<JSFunction>(rejectCapability);
    if (promise && resolve && reject)
        return DeferredData { promise, resolve, reject };

    throwTypeError(globalObject, scope, "constructor is producing a bad value"_s);
    return { };
}

JSPromise* JSPromise::resolvedPromise(JSGlobalObject* globalObject, JSValue value)
{
    return uncheckedDowncast<JSPromise>(promiseResolve(globalObject, globalObject->promiseConstructor(), value));
}

JSPromise* JSPromise::rejectedPromise(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    promise->reject(vm, value);
    return promise;
}

JSPromise* JSPromise::rejectedPromiseWithCaughtException(JSGlobalObject* globalObject, ThrowScope& scope)
{
    Exception* exception = scope.exception();
    ASSERT(exception);
    TRY_CLEAR_EXCEPTION(scope, nullptr);
    scope.release();
    return rejectedPromise(globalObject, exception->value());
}

// UNGIL ANNEX E1B (U-T9): GIL-off, JSPromise internal-state transitions run
// under the promise's JSCellLock (10a) — internal fields are NOT §9.5 slots.
// The first-resolving claim below is the settle-side entry: two concurrent
// resolvers race the claim under the cell lock; exactly one proceeds. NO GC
// allocation and no JS runs inside any cell-lock hold in this file (OM I20).
// GIL-on/flag-off: the landed unlocked claim, byte-identical.
//
// Audit note (the §E.1b.2 "every other promise internal-field writer" U-T9
// audit): PromiseOperations.js / PromiseConstructor.js contain no
// @putPromiseInternalField sites — the native-restructure paths in this file
// (claims, performPromiseThen* publish loops, fulfillPromise/rejectPromise
// extraction) plus the embedder-binding direct twiddles (JSC__JSPromise__*,
// which construct PRE-SHARING promises) are the complete writer set;
// non-promise internal-field types are §N (U-T13's twin-intrinsic work).

void JSPromise::resolve(JSGlobalObject* globalObject, VM& vm, JSValue value)
{
    ASSERT(!value.inherits<Exception>());
    if (vm.gilOff()) [[unlikely]] {
        {
            Locker locker { cellLock() };
            uint16_t currentFlags = flags();
            if (currentFlags & isFirstResolvingFunctionCalledFlag)
                return;
            setFlags(currentFlags | isFirstResolvingFunctionCalledFlag);
        }
        resolvePromise(globalObject, vm, value);
        return;
    }
    if (!isFirstResolvingFunctionCalled()) {
        setFlags(flags() | isFirstResolvingFunctionCalledFlag);
        resolvePromise(globalObject, vm, value);
    }
}

void JSPromise::reject(VM& vm, JSValue value)
{
    ASSERT(!value.inherits<Exception>());
    if (vm.gilOff()) [[unlikely]] {
        {
            Locker locker { cellLock() };
            uint16_t currentFlags = flags();
            if (currentFlags & isFirstResolvingFunctionCalledFlag)
                return;
            setFlags(currentFlags | isFirstResolvingFunctionCalledFlag);
        }
        rejectPromise(vm, value);
        return;
    }
    if (!isFirstResolvingFunctionCalled()) {
        setFlags(flags() | isFirstResolvingFunctionCalledFlag);
        rejectPromise(vm, value);
    }
}

void JSPromise::fulfill(VM& vm, JSValue value)
{
    ASSERT(!value.inherits<Exception>());
    if (vm.gilOff()) [[unlikely]] {
        {
            Locker locker { cellLock() };
            uint16_t currentFlags = flags();
            if (currentFlags & isFirstResolvingFunctionCalledFlag)
                return;
            setFlags(currentFlags | isFirstResolvingFunctionCalledFlag);
        }
        fulfillPromise(vm, value);
        return;
    }
    if (!isFirstResolvingFunctionCalled()) {
        setFlags(flags() | isFirstResolvingFunctionCalledFlag);
        fulfillPromise(vm, value);
    }
}

void JSPromise::pipeFrom(VM& vm, JSPromise* from)
{
    if (vm.gilOff()) [[unlikely]] {
        {
            Locker locker { cellLock() };
            uint16_t currentFlags = flags();
            if (currentFlags & isFirstResolvingFunctionCalledFlag)
                return;
            setFlags(currentFlags | isFirstResolvingFunctionCalledFlag);
        }
        from->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::PromiseFulfillWithoutHandlerJob, this, jsUndefined());
        return;
    }
    if (isFirstResolvingFunctionCalled())
        return;
    setFlags(flags() | isFirstResolvingFunctionCalledFlag);

    from->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::PromiseFulfillWithoutHandlerJob, this, jsUndefined());
}

void JSPromise::performPromiseThenExported(VM& vm, JSGlobalObject* globalObject, JSValue onFulfilled, JSValue onRejected, JSValue promiseOrCapability)
{
    return performPromiseThen(vm, globalObject, onFulfilled, onRejected, promiseOrCapability);
}

void JSPromise::rejectAsHandled(VM& vm, JSValue value)
{
    // Setting isHandledFlag before calling reject since this removes round-trip between JSC and PromiseRejectionTracker, and it does not show an user-observable behavior.
    if (vm.gilOff()) [[unlikely]] {
        // E1B: claim + markAsHandled in ONE cell-lock RMW (a lost claim must
        // not set isHandled on a promise someone else is settling).
        {
            Locker locker { cellLock() };
            uint16_t currentFlags = flags();
            if (currentFlags & isFirstResolvingFunctionCalledFlag)
                return;
            setFlags(currentFlags | isFirstResolvingFunctionCalledFlag | isHandledFlag);
        }
        rejectPromise(vm, value);
        return;
    }
    if (!isFirstResolvingFunctionCalled()) {
        markAsHandled();
        reject(vm, value);
    }
}

void JSPromise::reject(VM& vm, Exception* reason)
{
    reject(vm, reason->value());
}

void JSPromise::rejectAsHandled(VM& vm, Exception* reason)
{
    rejectAsHandled(vm, reason->value());
}

JSPromise* JSPromise::rejectWithCaughtException(VM& vm, ThrowScope& scope)
{
    Exception* exception = scope.exception();
    ASSERT(exception);
    TRY_CLEAR_EXCEPTION(scope, nullptr);
    scope.release();
    reject(vm, exception->value());
    return this;
}

void JSPromise::setInlineMicrotaskReaction(VM& vm, InternalMicrotask task, JSCell* cell, JSValue context)
{
    ASSERT(status() == Status::Pending);
    ASSERT(inlineReactionKind() == InlineReactionKind::None);
    ASSERT(!payloadCell());
    ASSERT(task != InternalMicrotask::None);
    // The inline reaction always implies markAsHandled; fold both into one flag update.
    uint16_t newFlags = flags()
        | isHandledFlag
        | (static_cast<uint16_t>(InlineReactionKind::InternalMicrotask) << inlineReactionKindShift)
        | (static_cast<uint16_t>(task) << inlineReactionMicrotaskShift);
    setSlot(vm, context);
    setPackedCell(vm, newFlags, cell);
}

void JSPromise::setInlineHandlerReaction(VM& vm, InlineReactionKind kind, JSPromise* resultPromise, JSValue handler)
{
    ASSERT(status() == Status::Pending);
    ASSERT(inlineReactionKind() == InlineReactionKind::None);
    ASSERT(!payloadCell());
    ASSERT(kind == InlineReactionKind::FulfillHandler || kind == InlineReactionKind::RejectHandler);
    ASSERT(resultPromise);
    uint16_t newFlags = flags()
        | isHandledFlag
        | (static_cast<uint16_t>(kind) << inlineReactionKindShift);
    setSlot(vm, handler);
    setPackedCell(vm, newFlags, resultPromise);
}

JSPromiseReaction* JSPromise::spillInlineReaction(VM& vm)
{
    auto kind = inlineReactionKind();
    ASSERT(kind != InlineReactionKind::None);
    JSSlimPromiseReaction* reaction = nullptr;
    switch (kind) {
    case InlineReactionKind::InternalMicrotask: {
        InternalMicrotask task = inlineReactionMicrotask();
        JSValue context = m_slot.get();
        JSCell* cell = payloadCell();
        reaction = JSSlimPromiseReaction::create(vm, cell ? JSValue(cell) : jsUndefined(), task, context, nullptr);
        break;
    }
    case InlineReactionKind::FulfillHandler:
    case InlineReactionKind::RejectHandler: {
        JSPromise* resultPromise = uncheckedDowncast<JSPromise>(payloadCell());
        JSValue handler = m_slot.get();
        bool isFulfill = kind == InlineReactionKind::FulfillHandler;
        reaction = JSSlimPromiseReaction::create(vm, resultPromise, handler, isFulfill, nullptr);
        break;
    }
    case InlineReactionKind::None:
        RELEASE_ASSERT_NOT_REACHED();
    }
    clearSlot();
    uint16_t newFlags = flags() & ~(inlineReactionKindMask | inlineReactionMicrotaskMask);
    setPackedCell(vm, newFlags, reaction);
    return reaction;
}

JSPromiseReaction* JSPromise::reactionHead(VM& vm)
{
    ASSERT(status() == Status::Pending);
    if (inlineReactionKind() != InlineReactionKind::None) [[unlikely]]
        return spillInlineReaction(vm);
    return uncheckedDowncast<JSPromiseReaction>(payloadCell());
}

JSValue JSPromise::asyncStackTraceContext() const
{
    if (status() != Status::Pending)
        return { };
    switch (inlineReactionKind()) {
    case InlineReactionKind::None: {
        auto* head = uncheckedDowncast<JSPromiseReaction>(payloadCell());
        return head ? JSPromiseReaction::tryGetContext(head) : JSValue();
    }
    case InlineReactionKind::InternalMicrotask: {
        if (promiseReactionPacksGlobalContextAndIndex(inlineReactionMicrotask())) {
            ASSERT(payloadCell());
            return JSValue(payloadCell());
        }
        return m_slot.get();
    }
    case InlineReactionKind::FulfillHandler:
    case InlineReactionKind::RejectHandler:
        return { };
    }
    return { };
}

void JSPromise::performPromiseThen(VM& vm, JSGlobalObject* globalObject, JSValue onFulfilled, JSValue onRejected, JSValue promiseOrCapability)
{
    bool fulfilledCallable = onFulfilled.isCallable();
    bool rejectedCallable = onRejected.isCallable();

#if USE(BUN_JSC_ADDITIONS)
    // Capture async context for promise reaction (ALS1: PER-REACTION, at
    // registration time, from the CURRENT — GIL-off per-lite — cursor).
    // Wrap in InternalFieldTuple: [userContext (undefined), asyncContext]
    JSValue context = jsUndefined();
    if (auto* asyncContextData = threadAsyncContextData(globalObject)) {
        JSValue asyncContext = asyncContextData->getInternalField(0);
        if (!asyncContext.isUndefined()) {
            auto* tuple = InternalFieldTuple::create(vm, globalObject->internalFieldTupleStructure());
            tuple->putInternalField(vm, 0, jsUndefined()); // userContext
            tuple->putInternalField(vm, 1, asyncContext);  // asyncContext
            context = tuple;
        }
    }
#endif

    if (vm.gilOff()) [[unlikely]] {
        // ANNEX E1B concurrent then()/resolve(): snapshot under the cell
        // lock, ALLOCATE OUTSIDE it (OM I20), re-check + publish under it;
        // a raced snapshot drops the allocation and retries. The inline-
        // reaction install fast path is NOT taken GIL-off (the heap reaction
        // is always built; a PRE-EXISTING inline reaction is spilled here,
        // outside the lock, and published together with the new head).
        while (true) {
            uint16_t snapshotFlags;
            JSCell* payload;
            JSValue slotValue;
            bool wonHandleClaim = false;
            {
                Locker locker { cellLock() };
                snapshotFlags = flags();
                payload = payloadCell();
                slotValue = m_slot.get();
                // SD15 MULTIPLICITY (U-T9): the unhandled observation and the
                // isHandled claim are ATOMIC under the cell lock — two threads
                // concurrently then()ing a rejected shared promise must emit
                // at most ONE Handle event (the annex relaxes tracker ORDERING
                // only, never per-promise multiplicity; a duplicate Handle
                // under-counts refcount-style trackers). Rejected is terminal,
                // so claiming here cannot leak into a Pending retry.
                if (static_cast<Status>(snapshotFlags & stateMask) == Status::Rejected && !(snapshotFlags & isHandledFlag)) {
                    setFlags(snapshotFlags | isHandledFlag); // markAsHandled, fused with the claim.
                    wonHandleClaim = true;
                }
            }
            auto snapshotStatus = static_cast<Status>(snapshotFlags & stateMask);
            if (snapshotStatus == Status::Rejected) {
                JSValue settled = slotValue; // settlementValue(), from the coherent locked snapshot.
                if (wonHandleClaim)
                    notifyPromiseRejectionTrackerCrossThreadAware(globalObject, this, JSPromiseRejectionOperation::Handle);
                if (rejectedCallable)
#if USE(BUN_JSC_ADDITIONS)
                    globalObject->queueMicrotask(vm, InternalMicrotask::PromiseReactionJob, static_cast<uint8_t>(Status::Rejected), promiseOrCapability, onRejected, settled, context);
#else
                    globalObject->queueMicrotask(vm, InternalMicrotask::PromiseReactionJob, static_cast<uint8_t>(Status::Rejected), promiseOrCapability, onRejected, settled);
#endif
                else
                    globalObject->queueMicrotask(vm, InternalMicrotask::PromiseResolveWithoutHandlerJob, static_cast<uint8_t>(Status::Rejected), promiseOrCapability, settled, jsUndefined());
                return;
            }
            if (snapshotStatus == Status::Fulfilled) {
                JSValue settled = slotValue;
                if (fulfilledCallable)
#if USE(BUN_JSC_ADDITIONS)
                    globalObject->queueMicrotask(vm, InternalMicrotask::PromiseReactionJob, static_cast<uint8_t>(Status::Fulfilled), promiseOrCapability, onFulfilled, settled, context);
#else
                    globalObject->queueMicrotask(vm, InternalMicrotask::PromiseReactionJob, static_cast<uint8_t>(Status::Fulfilled), promiseOrCapability, onFulfilled, settled);
#endif
                else
                    globalObject->queueMicrotask(vm, InternalMicrotask::PromiseResolveWithoutHandlerJob, static_cast<uint8_t>(Status::Fulfilled), promiseOrCapability, settled, jsUndefined());
                return;
            }

            // Pending: build the spill (if any) + the new head, outside the lock.
            auto snapshotKind = static_cast<InlineReactionKind>((snapshotFlags & inlineReactionKindMask) >> inlineReactionKindShift);
            JSPromiseReaction* existing = nullptr;
            switch (snapshotKind) {
            case InlineReactionKind::InternalMicrotask: {
                InternalMicrotask task = static_cast<InternalMicrotask>((snapshotFlags & inlineReactionMicrotaskMask) >> inlineReactionMicrotaskShift);
                existing = JSSlimPromiseReaction::create(vm, payload ? JSValue(payload) : jsUndefined(), task, slotValue, nullptr);
                break;
            }
            case InlineReactionKind::FulfillHandler:
            case InlineReactionKind::RejectHandler:
                existing = JSSlimPromiseReaction::create(vm, uncheckedDowncast<JSPromise>(payload), slotValue, snapshotKind == InlineReactionKind::FulfillHandler, nullptr);
                break;
            case InlineReactionKind::None:
                existing = uncheckedDowncast<JSPromiseReaction>(payload);
                break;
            }

            bool onlyFulfill = fulfilledCallable && !rejectedCallable;
            bool onlyReject = !fulfilledCallable && rejectedCallable;
            JSPromiseReaction* reaction;
#if USE(BUN_JSC_ADDITIONS)
            if (!context.isUndefined()) {
                reaction = JSFullPromiseReaction::create(vm, promiseOrCapability,
                    fulfilledCallable ? onFulfilled : jsUndefined(),
                    rejectedCallable ? onRejected : jsUndefined(),
                    context, existing);
            } else
#endif
            if (onlyFulfill)
                reaction = JSSlimPromiseReaction::create(vm, promiseOrCapability, onFulfilled, true, existing);
            else if (onlyReject)
                reaction = JSSlimPromiseReaction::create(vm, promiseOrCapability, onRejected, false, existing);
            else if (fulfilledCallable) {
                ASSERT(rejectedCallable);
                reaction = JSFullPromiseReaction::create(vm, promiseOrCapability, onFulfilled, onRejected, jsUndefined(), existing);
            } else
                reaction = JSSlimPromiseReaction::create(vm, promiseOrCapability, InternalMicrotask::PromiseResolveWithoutHandlerJob, jsUndefined(), existing);

            {
                Locker locker { cellLock() };
                if (flags() == snapshotFlags && payloadCell() == payload && m_slot.get() == slotValue) {
                    if (snapshotKind != InlineReactionKind::None)
                        clearSlot(); // the spilled inline reaction now owns its context/handler.
                    setPackedCell(vm, (snapshotFlags & ~(inlineReactionKindMask | inlineReactionMicrotaskMask)) | isHandledFlag, reaction);
                    return;
                }
            }
            // Raced (a concurrent then() or settle moved the state): the
            // dropped allocations are unreferenced garbage; retry against
            // the fresh snapshot.
        }
    }

    switch (status()) {
    case JSPromise::Status::Pending: {
        bool onlyFulfill = fulfilledCallable && !rejectedCallable;
        bool onlyReject = !fulfilledCallable && rejectedCallable;
#if USE(BUN_JSC_ADDITIONS)
        // Inline reactions cannot carry an async context, so fall through to the
        // heap-allocated JSFullPromiseReaction path when one is captured.
        if (context.isUndefined() && inlineReactionKind() == InlineReactionKind::None && !payloadCell()) {
#else
        if (inlineReactionKind() == InlineReactionKind::None && !payloadCell()) {
#endif
            if ((onlyFulfill || onlyReject) && promiseOrCapability.inherits<JSPromise>()) [[likely]] {
                auto* resultPromise = uncheckedDowncast<JSPromise>(promiseOrCapability);
                setInlineHandlerReaction(vm, onlyFulfill ? InlineReactionKind::FulfillHandler : InlineReactionKind::RejectHandler, resultPromise, onlyFulfill ? onFulfilled : onRejected);
                break;
            }
        }
        JSPromiseReaction* existing = reactionHead(vm);
        JSPromiseReaction* reaction;
#if USE(BUN_JSC_ADDITIONS)
        if (!context.isUndefined()) {
            // Carry async context via JSFullPromiseReaction; normalize non-callable sides
            // to jsUndefined() so dispatch can use a tag check instead of isCallable().
            reaction = JSFullPromiseReaction::create(vm, promiseOrCapability,
                fulfilledCallable ? onFulfilled : jsUndefined(),
                rejectedCallable ? onRejected : jsUndefined(),
                context, existing);
        } else
#endif
        if (onlyFulfill)
            reaction = JSSlimPromiseReaction::create(vm, promiseOrCapability, onFulfilled, true, existing);
        else if (onlyReject)
            reaction = JSSlimPromiseReaction::create(vm, promiseOrCapability, onRejected, false, existing);
        else if (fulfilledCallable) {
            ASSERT(rejectedCallable);
            reaction = JSFullPromiseReaction::create(vm, promiseOrCapability, onFulfilled, onRejected, jsUndefined(), existing);
        } else
            reaction = JSSlimPromiseReaction::create(vm, promiseOrCapability, InternalMicrotask::PromiseResolveWithoutHandlerJob, jsUndefined(), existing);
        setPackedCell(vm, flags() | isHandledFlag, reaction);
        break;
    }
    case JSPromise::Status::Rejected: {
        JSValue settled = settlementValue();
        if (!isHandled())
            notifyPromiseRejectionTrackerCrossThreadAware(globalObject, this, JSPromiseRejectionOperation::Handle);
        if (rejectedCallable)
#if USE(BUN_JSC_ADDITIONS)
            globalObject->queueMicrotask(vm, InternalMicrotask::PromiseReactionJob, static_cast<uint8_t>(Status::Rejected), promiseOrCapability, onRejected, settled, context);
#else
            globalObject->queueMicrotask(vm, InternalMicrotask::PromiseReactionJob, static_cast<uint8_t>(Status::Rejected), promiseOrCapability, onRejected, settled);
#endif
        else
            globalObject->queueMicrotask(vm, InternalMicrotask::PromiseResolveWithoutHandlerJob, static_cast<uint8_t>(Status::Rejected), promiseOrCapability, settled, jsUndefined());
        markAsHandled();
        break;
    }
    case JSPromise::Status::Fulfilled: {
        JSValue settled = settlementValue();
        if (fulfilledCallable)
#if USE(BUN_JSC_ADDITIONS)
            globalObject->queueMicrotask(vm, InternalMicrotask::PromiseReactionJob, static_cast<uint8_t>(Status::Fulfilled), promiseOrCapability, onFulfilled, settled, context);
#else
            globalObject->queueMicrotask(vm, InternalMicrotask::PromiseReactionJob, static_cast<uint8_t>(Status::Fulfilled), promiseOrCapability, onFulfilled, settled);
#endif
        else
            globalObject->queueMicrotask(vm, InternalMicrotask::PromiseResolveWithoutHandlerJob, static_cast<uint8_t>(Status::Fulfilled), promiseOrCapability, settled, jsUndefined());
        break;
    }
    }
}

#if USE(BUN_JSC_ADDITIONS)
void JSPromise::performPromiseThenWithContext(VM& vm, JSGlobalObject* globalObject, JSValue onFulfilled, JSValue onRejected, JSValue promiseOrCapability, JSValue userContext)
{
    bool fulfilledCallable = onFulfilled.isCallable();
    bool rejectedCallable = onRejected.isCallable();

    // Wrap userContext and asyncContext in InternalFieldTuple: [userContext, asyncContext]
    // (ALS1: registration-time capture from the — GIL-off per-lite — cursor.)
    JSValue context = userContext;
    if (auto* asyncContextData = threadAsyncContextData(globalObject)) {
        JSValue asyncContext = asyncContextData->getInternalField(0);
        // Always create a tuple if there's a user context or async context
        if (!userContext.isUndefinedOrNull() || !asyncContext.isUndefined()) {
            auto* tuple = InternalFieldTuple::create(vm, globalObject->internalFieldTupleStructure());
            tuple->putInternalField(vm, 0, userContext);   // userContext
            tuple->putInternalField(vm, 1, asyncContext);  // asyncContext
            context = tuple;
        }
    }

    if (vm.gilOff()) [[unlikely]] {
        // ANNEX E1B: same snapshot/alloc-outside/re-check loop as
        // performPromiseThen (Pending arm always heap-allocates here).
        while (true) {
            uint16_t snapshotFlags;
            JSCell* payload;
            JSValue slotValue;
            bool wonHandleClaim = false;
            {
                Locker locker { cellLock() };
                snapshotFlags = flags();
                payload = payloadCell();
                slotValue = m_slot.get();
                // SD15 multiplicity: atomic observe+claim — see performPromiseThen.
                if (static_cast<Status>(snapshotFlags & stateMask) == Status::Rejected && !(snapshotFlags & isHandledFlag)) {
                    setFlags(snapshotFlags | isHandledFlag);
                    wonHandleClaim = true;
                }
            }
            auto snapshotStatus = static_cast<Status>(snapshotFlags & stateMask);
            if (snapshotStatus == Status::Rejected) {
                JSValue settled = slotValue;
                if (wonHandleClaim)
                    notifyPromiseRejectionTrackerCrossThreadAware(globalObject, this, JSPromiseRejectionOperation::Handle);
                if (rejectedCallable)
                    globalObject->queueMicrotask(vm, InternalMicrotask::PromiseReactionJob, static_cast<uint8_t>(Status::Rejected), promiseOrCapability, onRejected, settled, context);
                else
                    globalObject->queueMicrotask(vm, InternalMicrotask::PromiseResolveWithoutHandlerJob, static_cast<uint8_t>(Status::Rejected), promiseOrCapability, settled, jsUndefined());
                return;
            }
            if (snapshotStatus == Status::Fulfilled) {
                JSValue settled = slotValue;
                if (fulfilledCallable)
                    globalObject->queueMicrotask(vm, InternalMicrotask::PromiseReactionJob, static_cast<uint8_t>(Status::Fulfilled), promiseOrCapability, onFulfilled, settled, context);
                else
                    globalObject->queueMicrotask(vm, InternalMicrotask::PromiseResolveWithoutHandlerJob, static_cast<uint8_t>(Status::Fulfilled), promiseOrCapability, settled, jsUndefined());
                return;
            }

            auto snapshotKind = static_cast<InlineReactionKind>((snapshotFlags & inlineReactionKindMask) >> inlineReactionKindShift);
            JSPromiseReaction* existing = nullptr;
            switch (snapshotKind) {
            case InlineReactionKind::InternalMicrotask: {
                InternalMicrotask task = static_cast<InternalMicrotask>((snapshotFlags & inlineReactionMicrotaskMask) >> inlineReactionMicrotaskShift);
                existing = JSSlimPromiseReaction::create(vm, payload ? JSValue(payload) : jsUndefined(), task, slotValue, nullptr);
                break;
            }
            case InlineReactionKind::FulfillHandler:
            case InlineReactionKind::RejectHandler:
                existing = JSSlimPromiseReaction::create(vm, uncheckedDowncast<JSPromise>(payload), slotValue, snapshotKind == InlineReactionKind::FulfillHandler, nullptr);
                break;
            case InlineReactionKind::None:
                existing = uncheckedDowncast<JSPromiseReaction>(payload);
                break;
            }

            auto* reaction = JSFullPromiseReaction::create(vm, promiseOrCapability,
                fulfilledCallable ? onFulfilled : jsUndefined(),
                rejectedCallable ? onRejected : jsUndefined(),
                context, existing);

            {
                Locker locker { cellLock() };
                if (flags() == snapshotFlags && payloadCell() == payload && m_slot.get() == slotValue) {
                    if (snapshotKind != InlineReactionKind::None)
                        clearSlot();
                    setPackedCell(vm, (snapshotFlags & ~(inlineReactionKindMask | inlineReactionMicrotaskMask)) | isHandledFlag, reaction);
                    return;
                }
            }
        }
    }

    switch (status()) {
    case JSPromise::Status::Pending: {
        JSPromiseReaction* existing = reactionHead(vm);
        auto* reaction = JSFullPromiseReaction::create(vm, promiseOrCapability,
            fulfilledCallable ? onFulfilled : jsUndefined(),
            rejectedCallable ? onRejected : jsUndefined(),
            context, existing);
        setPackedCell(vm, flags() | isHandledFlag, reaction);
        break;
    }
    case JSPromise::Status::Rejected: {
        JSValue settled = settlementValue();
        if (!isHandled())
            notifyPromiseRejectionTrackerCrossThreadAware(globalObject, this, JSPromiseRejectionOperation::Handle);
        if (rejectedCallable)
            globalObject->queueMicrotask(vm, InternalMicrotask::PromiseReactionJob, static_cast<uint8_t>(Status::Rejected), promiseOrCapability, onRejected, settled, context);
        else
            globalObject->queueMicrotask(vm, InternalMicrotask::PromiseResolveWithoutHandlerJob, static_cast<uint8_t>(Status::Rejected), promiseOrCapability, settled, jsUndefined());
        markAsHandled();
        break;
    }
    case JSPromise::Status::Fulfilled: {
        JSValue settled = settlementValue();
        if (fulfilledCallable)
            globalObject->queueMicrotask(vm, InternalMicrotask::PromiseReactionJob, static_cast<uint8_t>(Status::Fulfilled), promiseOrCapability, onFulfilled, settled, context);
        else
            globalObject->queueMicrotask(vm, InternalMicrotask::PromiseResolveWithoutHandlerJob, static_cast<uint8_t>(Status::Fulfilled), promiseOrCapability, settled, jsUndefined());
        break;
    }
    }
}
#endif

void JSPromise::performPromiseThenWithInternalMicrotask(VM& vm, InternalMicrotask task, JSCell* cell, JSValue context)
{
    JSValue cellValue = cell ? JSValue(cell) : jsUndefined();

    if (vm.gilOff()) [[unlikely]] {
        // ANNEX E1B: same snapshot/alloc-outside/re-check loop; the inline
        // microtask-reaction install fast path is not taken GIL-off.
        JSGlobalObject* globalObject = realm();
        while (true) {
            uint16_t snapshotFlags;
            JSCell* payload;
            JSValue slotValue;
            bool wonHandleClaim = false;
            {
                Locker locker { cellLock() };
                snapshotFlags = flags();
                payload = payloadCell();
                slotValue = m_slot.get();
                // SD15 multiplicity: atomic observe+claim — see performPromiseThen.
                if (static_cast<Status>(snapshotFlags & stateMask) == Status::Rejected && !(snapshotFlags & isHandledFlag)) {
                    setFlags(snapshotFlags | isHandledFlag);
                    wonHandleClaim = true;
                }
            }
            auto snapshotStatus = static_cast<Status>(snapshotFlags & stateMask);
            if (snapshotStatus != Status::Pending) {
                JSValue settled = slotValue;
                if (wonHandleClaim)
                    notifyPromiseRejectionTrackerCrossThreadAware(globalObject, this, JSPromiseRejectionOperation::Handle);
#if USE(BUN_JSC_ADDITIONS)
                if (vm.m_synchronousModuleQueue && isModuleLoaderInternalMicrotask(task)) [[unlikely]] {
                    vm.m_synchronousModuleQueue->tasks.append({ task, static_cast<uint8_t>(snapshotStatus), cellValue, settled, context });
                } else
#endif
                globalObject->queueMicrotask(vm, task, static_cast<uint8_t>(snapshotStatus), cellValue, settled, context);
                return;
            }

            auto snapshotKind = static_cast<InlineReactionKind>((snapshotFlags & inlineReactionKindMask) >> inlineReactionKindShift);
            JSPromiseReaction* existing = nullptr;
            switch (snapshotKind) {
            case InlineReactionKind::InternalMicrotask: {
                InternalMicrotask existingTask = static_cast<InternalMicrotask>((snapshotFlags & inlineReactionMicrotaskMask) >> inlineReactionMicrotaskShift);
                existing = JSSlimPromiseReaction::create(vm, payload ? JSValue(payload) : jsUndefined(), existingTask, slotValue, nullptr);
                break;
            }
            case InlineReactionKind::FulfillHandler:
            case InlineReactionKind::RejectHandler:
                existing = JSSlimPromiseReaction::create(vm, uncheckedDowncast<JSPromise>(payload), slotValue, snapshotKind == InlineReactionKind::FulfillHandler, nullptr);
                break;
            case InlineReactionKind::None:
                existing = uncheckedDowncast<JSPromiseReaction>(payload);
                break;
            }

            auto* reaction = JSSlimPromiseReaction::create(vm, cellValue, task, context, existing);
            {
                Locker locker { cellLock() };
                if (flags() == snapshotFlags && payloadCell() == payload && m_slot.get() == slotValue) {
                    if (snapshotKind != InlineReactionKind::None)
                        clearSlot();
                    setPackedCell(vm, (snapshotFlags & ~(inlineReactionKindMask | inlineReactionMicrotaskMask)) | isHandledFlag, reaction);
                    return;
                }
            }
        }
    }

    switch (status()) {
    case JSPromise::Status::Pending: {
        if (inlineReactionKind() == InlineReactionKind::None && !payloadCell()) [[likely]] {
            setInlineMicrotaskReaction(vm, task, cell, context);
            break;
        }
        JSPromiseReaction* existing = reactionHead(vm);
        auto* reaction = JSSlimPromiseReaction::create(vm, cellValue, task, context, existing);
        setPackedCell(vm, flags() | isHandledFlag, reaction);
        break;
    }
    case JSPromise::Status::Rejected: {
        JSGlobalObject* globalObject = realm();
        JSValue settled = settlementValue();
        if (!isHandled())
            notifyPromiseRejectionTrackerCrossThreadAware(globalObject, this, JSPromiseRejectionOperation::Handle);
#if USE(BUN_JSC_ADDITIONS)
        if (vm.m_synchronousModuleQueue && isModuleLoaderInternalMicrotask(task)) [[unlikely]] {
            markAsHandled();
            vm.m_synchronousModuleQueue->tasks.append({ task, static_cast<uint8_t>(Status::Rejected), cellValue, settled, context });
            break;
        }
#endif
        globalObject->queueMicrotask(vm, task, static_cast<uint8_t>(Status::Rejected), cellValue, settled, context);
        markAsHandled();
        break;
    }
    case JSPromise::Status::Fulfilled: {
        JSGlobalObject* globalObject = realm();
        JSValue settled = settlementValue();
#if USE(BUN_JSC_ADDITIONS)
        if (vm.m_synchronousModuleQueue && isModuleLoaderInternalMicrotask(task)) [[unlikely]] {
            vm.m_synchronousModuleQueue->tasks.append({ task, static_cast<uint8_t>(Status::Fulfilled), cellValue, settled, context });
            break;
        }
#endif
        globalObject->queueMicrotask(vm, task, static_cast<uint8_t>(Status::Fulfilled), cellValue, settled, context);
        break;
    }
    }
}

bool isDefinitelyNonThenable(JSObject* object, JSGlobalObject* globalObject)
{
    if (!globalObject->promiseThenWatchpointSet().isStillValid()) [[unlikely]]
        return false;

    auto* structure = object->structure();
    auto state = structure->definitelyNonThenableState();
    if (state == Structure::DefinitelyNonThenableState::NonThenable && structure->realm() == globalObject)
        return true;
    if (state == Structure::DefinitelyNonThenableState::MaybeThenable)
        return false;

    bool result = true;
    auto* current = structure;
    while (current) {
        if (current->hasSpecialProperties()
            || current->typeInfo().getOwnPropertySlotIsImpureForPropertyAbsence()
            || current->typeInfo().overridesGetPrototype()
            || !current->hasMonoProto()) {
            result = false;
            break;
        }
        current = current->storedPrototypeStructure();
    }

    // Dictionary structures are mutated in place when properties are added or removed,
    // so the cached state could become stale (e.g. caching NonThenable, then adding `then`).
    // Give up caching entirely for them; the per-call walk above remains correct because
    // `hasSpecialProperties` is updated in place even for dictionaries.
    if (state == Structure::DefinitelyNonThenableState::NotComputed && !structure->isDictionary()) [[unlikely]] {
        if (!result) {
            // Always safe: a stale `false` only loses the optimization, never miscompiles.
            structure->setDefinitelyNonThenableState(Structure::DefinitelyNonThenableState::MaybeThenable);
        } else {
            // A `true` result is cacheable only when the entire prototype chain stays
            // under the protection of promiseThenWatchpointSet, which watches `then`
            // absence on Object.prototype. That limits the cacheable chain to [self]
            // (null proto) or [self, Object.prototype]. Mark anything else Uncacheable
            // so subsequent calls skip this check and go straight to the walk.
            JSValue proto = structure->storedPrototype();
            if (!proto.isObject() || asObject(proto) == globalObject->objectPrototype())
                structure->setDefinitelyNonThenableState(Structure::DefinitelyNonThenableState::NonThenable);
            else
                structure->setDefinitelyNonThenableState(Structure::DefinitelyNonThenableState::Uncacheable);
        }
    }
    return result;
}

ALWAYS_INLINE void JSPromise::settleInlineInternalMicrotask(VM& vm, JSGlobalObject* globalObject, Status newStatus, JSValue argument, uint16_t flagsSnapshot)
{
    ASSERT((flagsSnapshot & inlineReactionKindMask) == (static_cast<uint16_t>(InlineReactionKind::InternalMicrotask) << inlineReactionKindShift));
    ASSERT(flagsSnapshot & isHandledFlag);
    InternalMicrotask task = static_cast<InternalMicrotask>((flagsSnapshot & inlineReactionMicrotaskMask) >> inlineReactionMicrotaskShift);
    JSValue context = m_slot.get();
    JSCell* cell = payloadCell();
    JSValue cellValue = cell ? JSValue(cell) : jsUndefined();
    uint16_t settledFlags = (flagsSnapshot & ~(inlineReactionKindMask | inlineReactionMicrotaskMask)) | static_cast<uint16_t>(newStatus);
    setSlot(vm, argument);
    setPackedCell(vm, settledFlags, nullptr);
#if USE(BUN_JSC_ADDITIONS)
    if (vm.m_synchronousModuleQueue && isModuleLoaderInternalMicrotask(task)) [[unlikely]] {
        vm.m_synchronousModuleQueue->tasks.append({ task, static_cast<uint8_t>(newStatus), cellValue, argument, context });
        return;
    }
#endif
    globalObject->queueMicrotask(vm, task, static_cast<uint8_t>(newStatus), cellValue, argument, context);
}

ALWAYS_INLINE void JSPromise::settleInlineHandler(VM& vm, JSGlobalObject* globalObject, Status newStatus, JSValue argument, uint16_t flagsSnapshot)
{
    ASSERT(flagsSnapshot & isHandledFlag);
    InlineReactionKind kind = static_cast<InlineReactionKind>((flagsSnapshot & inlineReactionKindMask) >> inlineReactionKindShift);
    ASSERT(kind == InlineReactionKind::FulfillHandler || kind == InlineReactionKind::RejectHandler);
    bool settledIsFulfilled = newStatus == Status::Fulfilled;
    bool handlerIsFulfill = kind == InlineReactionKind::FulfillHandler;
    JSPromise* resultPromise = uncheckedDowncast<JSPromise>(payloadCell());
    JSValue handler = m_slot.get();
    uint16_t settledFlags = (flagsSnapshot & ~(inlineReactionKindMask | inlineReactionMicrotaskMask)) | static_cast<uint16_t>(newStatus);
    setSlot(vm, argument);
    setPackedCell(vm, settledFlags, nullptr);
    if (settledIsFulfilled == handlerIsFulfill)
        globalObject->queueMicrotask(vm, InternalMicrotask::PromiseReactionJob, static_cast<uint8_t>(newStatus), resultPromise, handler, argument);
    else
        globalObject->queueMicrotask(vm, InternalMicrotask::PromiseResolveWithoutHandlerJob, static_cast<uint8_t>(newStatus), resultPromise, argument, jsUndefined());
}

void JSPromise::rejectPromise(VM& vm, JSValue argument)
{
    JSGlobalObject* globalObject = realm();
    if (vm.gilOff()) [[unlikely]] {
        // ANNEX E1B: swap status + extract the reaction state under the cell
        // lock; ALL enqueues (and the SD15 tracker event) run post-unlock.
        // A racer that already settled wins silently (the host-function
        // direct callers — resolvePromise/rejectPromise host fns — carry no
        // first-resolving claim, so non-Pending is reachable here GIL-off).
        uint16_t snapshotFlags;
        JSCell* payload;
        JSValue slotValue;
        {
            Locker locker { cellLock() };
            snapshotFlags = flags();
            if ((snapshotFlags & stateMask) != static_cast<uint16_t>(Status::Pending))
                return;
            payload = payloadCell();
            slotValue = m_slot.get();
            uint16_t settledFlags = (snapshotFlags & ~(inlineReactionKindMask | inlineReactionMicrotaskMask)) | static_cast<uint16_t>(Status::Rejected);
            setSlot(vm, argument);
            setPackedCell(vm, settledFlags, nullptr);
        }
        auto kind = static_cast<InlineReactionKind>((snapshotFlags & inlineReactionKindMask) >> inlineReactionKindShift);
        switch (kind) {
        case InlineReactionKind::InternalMicrotask: {
            ASSERT(snapshotFlags & isHandledFlag);
            InternalMicrotask task = static_cast<InternalMicrotask>((snapshotFlags & inlineReactionMicrotaskMask) >> inlineReactionMicrotaskShift);
            JSValue cellValue = payload ? JSValue(payload) : jsUndefined();
#if USE(BUN_JSC_ADDITIONS)
            if (vm.m_synchronousModuleQueue && isModuleLoaderInternalMicrotask(task)) [[unlikely]] {
                vm.m_synchronousModuleQueue->tasks.append({ task, static_cast<uint8_t>(Status::Rejected), cellValue, argument, slotValue });
                return;
            }
#endif
            globalObject->queueMicrotask(vm, task, static_cast<uint8_t>(Status::Rejected), cellValue, argument, slotValue);
            return;
        }
        case InlineReactionKind::FulfillHandler:
        case InlineReactionKind::RejectHandler: {
            ASSERT(snapshotFlags & isHandledFlag);
            JSPromise* resultPromise = uncheckedDowncast<JSPromise>(payload);
            if (kind == InlineReactionKind::RejectHandler)
                globalObject->queueMicrotask(vm, InternalMicrotask::PromiseReactionJob, static_cast<uint8_t>(Status::Rejected), resultPromise, slotValue, argument);
            else
                globalObject->queueMicrotask(vm, InternalMicrotask::PromiseResolveWithoutHandlerJob, static_cast<uint8_t>(Status::Rejected), resultPromise, argument, jsUndefined());
            return;
        }
        case InlineReactionKind::None: {
            if (!(snapshotFlags & isHandledFlag))
                notifyPromiseRejectionTrackerCrossThreadAware(globalObject, this, JSPromiseRejectionOperation::Reject);
            if (JSPromiseReaction* reactions = uncheckedDowncast<JSPromiseReaction>(payload))
                triggerPromiseReactions(vm, globalObject, Status::Rejected, reactions, argument);
            return;
        }
        }
        return;
    }

    ASSERT(status() == Status::Pending);
    uint16_t currentFlags = flags();
    auto kind = static_cast<InlineReactionKind>((currentFlags & inlineReactionKindMask) >> inlineReactionKindShift);
    switch (kind) {
    case InlineReactionKind::InternalMicrotask:
        return settleInlineInternalMicrotask(vm, globalObject, Status::Rejected, argument, currentFlags);

    case InlineReactionKind::FulfillHandler:
    case InlineReactionKind::RejectHandler:
        return settleInlineHandler(vm, globalObject, Status::Rejected, argument, currentFlags);

    case InlineReactionKind::None: {
        JSPromiseReaction* reactions = uncheckedDowncast<JSPromiseReaction>(payloadCell());
        uint16_t settledFlags = currentFlags | static_cast<uint16_t>(Status::Rejected);
        setSlot(vm, argument);
        setPackedCell(vm, settledFlags, nullptr);

        if (!isHandled())
            notifyPromiseRejectionTrackerCrossThreadAware(globalObject, this, JSPromiseRejectionOperation::Reject);

        if (!reactions)
            return;
        triggerPromiseReactions(vm, globalObject, Status::Rejected, reactions, argument);
        return;
    }
    }
}

void JSPromise::fulfillPromise(VM& vm, JSValue argument)
{
    JSGlobalObject* globalObjectForGILOff = realm();
    if (vm.gilOff()) [[unlikely]] {
        // ANNEX E1B — mirror of rejectPromise's locked extraction (no
        // tracker event on the fulfill edge).
        uint16_t snapshotFlags;
        JSCell* payload;
        JSValue slotValue;
        {
            Locker locker { cellLock() };
            snapshotFlags = flags();
            if ((snapshotFlags & stateMask) != static_cast<uint16_t>(Status::Pending))
                return;
            payload = payloadCell();
            slotValue = m_slot.get();
            uint16_t settledFlags = (snapshotFlags & ~(inlineReactionKindMask | inlineReactionMicrotaskMask)) | static_cast<uint16_t>(Status::Fulfilled);
            setSlot(vm, argument);
            setPackedCell(vm, settledFlags, nullptr);
        }
        auto snapshotKind = static_cast<InlineReactionKind>((snapshotFlags & inlineReactionKindMask) >> inlineReactionKindShift);
        switch (snapshotKind) {
        case InlineReactionKind::InternalMicrotask: {
            ASSERT(snapshotFlags & isHandledFlag);
            InternalMicrotask task = static_cast<InternalMicrotask>((snapshotFlags & inlineReactionMicrotaskMask) >> inlineReactionMicrotaskShift);
            JSValue cellValue = payload ? JSValue(payload) : jsUndefined();
#if USE(BUN_JSC_ADDITIONS)
            if (vm.m_synchronousModuleQueue && isModuleLoaderInternalMicrotask(task)) [[unlikely]] {
                vm.m_synchronousModuleQueue->tasks.append({ task, static_cast<uint8_t>(Status::Fulfilled), cellValue, argument, slotValue });
                return;
            }
#endif
            globalObjectForGILOff->queueMicrotask(vm, task, static_cast<uint8_t>(Status::Fulfilled), cellValue, argument, slotValue);
            return;
        }
        case InlineReactionKind::FulfillHandler:
        case InlineReactionKind::RejectHandler: {
            ASSERT(snapshotFlags & isHandledFlag);
            JSPromise* resultPromise = uncheckedDowncast<JSPromise>(payload);
            if (snapshotKind == InlineReactionKind::FulfillHandler)
                globalObjectForGILOff->queueMicrotask(vm, InternalMicrotask::PromiseReactionJob, static_cast<uint8_t>(Status::Fulfilled), resultPromise, slotValue, argument);
            else
                globalObjectForGILOff->queueMicrotask(vm, InternalMicrotask::PromiseResolveWithoutHandlerJob, static_cast<uint8_t>(Status::Fulfilled), resultPromise, argument, jsUndefined());
            return;
        }
        case InlineReactionKind::None: {
            if (JSPromiseReaction* reactions = uncheckedDowncast<JSPromiseReaction>(payload))
                triggerPromiseReactions(vm, globalObjectForGILOff, Status::Fulfilled, reactions, argument);
            return;
        }
        }
        return;
    }

    ASSERT(status() == Status::Pending);
    JSGlobalObject* globalObject = globalObjectForGILOff;
    uint16_t currentFlags = flags();
    auto kind = static_cast<InlineReactionKind>((currentFlags & inlineReactionKindMask) >> inlineReactionKindShift);
    switch (kind) {
    case InlineReactionKind::InternalMicrotask:
        return settleInlineInternalMicrotask(vm, globalObject, Status::Fulfilled, argument, currentFlags);

    case InlineReactionKind::FulfillHandler:
    case InlineReactionKind::RejectHandler:
        return settleInlineHandler(vm, globalObject, Status::Fulfilled, argument, currentFlags);

    case InlineReactionKind::None: {
        JSPromiseReaction* reactions = uncheckedDowncast<JSPromiseReaction>(payloadCell());
        uint16_t settledFlags = currentFlags | static_cast<uint16_t>(Status::Fulfilled);
        setSlot(vm, argument);
        setPackedCell(vm, settledFlags, nullptr);

        if (!reactions)
            return;
        triggerPromiseReactions(vm, globalObject, Status::Fulfilled, reactions, argument);
        return;
    }
    }
}

void JSPromise::resolvePromise(JSGlobalObject* globalObject, VM& vm, JSValue resolution)
{
    if (resolution == this) [[unlikely]] {
        Structure* errorStructure = globalObject->errorStructure(ErrorType::TypeError);
        auto* error = ErrorInstance::create(vm, errorStructure, "Cannot resolve a promise with itself"_s, jsUndefined(), nullptr, TypeNothing, ErrorType::TypeError, false);
        return rejectPromise(vm, error);
    }

    if (!resolution.isObject())
        return fulfillPromise(vm, resolution);

    auto* resolutionObject = asObject(resolution);
    if (resolutionObject->inherits<JSPromise>()) {
        auto* promise = uncheckedDowncast<JSPromise>(resolutionObject);
        if (promise->isThenFastAndNonObservable()) {
#if USE(BUN_JSC_ADDITIONS)
            // Capture async context for thenable resolution
            JSValue asyncContext = jsUndefined();
            if (auto* asyncContextData = threadAsyncContextData(globalObject)) // ALS1 (U-T9): per-lite cursor GIL-off; in-object member otherwise.
                asyncContext = asyncContextData->getInternalField(0);
            return promise->realm()->queueMicrotask(vm, InternalMicrotask::PromiseResolveThenableJobFast, 0, resolutionObject, this, asyncContext);
#else
            return promise->realm()->queueMicrotask(vm, InternalMicrotask::PromiseResolveThenableJobFast, 0, resolutionObject, this, jsUndefined());
#endif
        }
    }

    if (isDefinitelyNonThenable(resolutionObject, globalObject))
        return fulfillPromise(vm, resolution);

    JSValue then;
    JSValue error;
    {
        auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        then = resolutionObject->get(globalObject, vm.propertyNames->then);
        if (catchScope.exception()) [[unlikely]] {
            error = catchScope.exception()->value();
            if (!catchScope.clearExceptionExceptTermination()) [[unlikely]]
                return;
        }
    }
    if (error) [[unlikely]]
        return rejectPromise(vm, error);

    if (!then.isCallable()) [[likely]]
        return fulfillPromise(vm, resolutionObject);

#if USE(BUN_JSC_ADDITIONS)
    // Capture async context for thenable resolution
    JSValue asyncContext = jsUndefined();
    if (auto* asyncContextData = threadAsyncContextData(globalObject)) // ALS1 (U-T9): per-lite cursor GIL-off; in-object member otherwise.
        asyncContext = asyncContextData->getInternalField(0);
    return globalObject->queueMicrotask(vm, InternalMicrotask::PromiseResolveThenableJob, 0, resolutionObject, then, this, asyncContext);
#else
    return globalObject->queueMicrotask(vm, InternalMicrotask::PromiseResolveThenableJob, 0, resolutionObject, then, this);
#endif
}

JSC_DEFINE_HOST_FUNCTION(promiseResolvingFunctionResolve, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();

    auto* callee = uncheckedDowncast<JSFunctionWithFields>(callFrame->jsCallee());
    auto* other = dynamicDowncast<JSFunctionWithFields>(callee->getField(JSFunctionWithFields::Field::ResolvingOther));
    if (!other) [[unlikely]]
        return JSValue::encode(jsUndefined());

    callee->setField(vm, JSFunctionWithFields::Field::ResolvingOther, jsNull());
    other->setField(vm, JSFunctionWithFields::Field::ResolvingOther, jsNull());

    auto* promise = uncheckedDowncast<JSPromise>(callee->getField(JSFunctionWithFields::Field::ResolvingPromise));
    JSValue argument = callFrame->argument(0);

    promise->resolvePromise(globalObject, vm, argument);
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(promiseResolvingFunctionReject, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();

    auto* callee = uncheckedDowncast<JSFunctionWithFields>(callFrame->jsCallee());
    auto* other = dynamicDowncast<JSFunctionWithFields>(callee->getField(JSFunctionWithFields::Field::ResolvingOther));
    if (!other) [[unlikely]]
        return JSValue::encode(jsUndefined());

    callee->setField(vm, JSFunctionWithFields::Field::ResolvingOther, jsNull());
    other->setField(vm, JSFunctionWithFields::Field::ResolvingOther, jsNull());

    auto* promise = uncheckedDowncast<JSPromise>(callee->getField(JSFunctionWithFields::Field::ResolvingPromise));
    JSValue argument = callFrame->argument(0);

    promise->rejectPromise(vm, argument);
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(promiseFirstResolvingFunctionResolve, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto* callee = uncheckedDowncast<JSFunctionWithFields>(callFrame->jsCallee());
    auto* promise = uncheckedDowncast<JSPromise>(callee->getField(JSFunctionWithFields::Field::FirstResolvingPromise));
    JSValue argument = callFrame->argument(0);

    promise->resolve(globalObject, globalObject->vm(), argument);
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(promiseFirstResolvingFunctionReject, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto* callee = uncheckedDowncast<JSFunctionWithFields>(callFrame->jsCallee());
    auto* promise = uncheckedDowncast<JSPromise>(callee->getField(JSFunctionWithFields::Field::FirstResolvingPromise));
    JSValue argument = callFrame->argument(0);

    promise->reject(globalObject->vm(), argument);
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(promiseResolvingFunctionResolveWithInternalMicrotask, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();

    auto* callee = uncheckedDowncast<JSFunctionWithFields>(callFrame->jsCallee());
    auto* other = dynamicDowncast<JSFunctionWithFields>(callee->getField(JSFunctionWithFields::Field::ResolvingWithInternalMicrotaskOther));
    if (!other) [[unlikely]]
        return JSValue::encode(jsUndefined());

    callee->setField(vm, JSFunctionWithFields::Field::ResolvingWithInternalMicrotaskOther, jsNull());
    other->setField(vm, JSFunctionWithFields::Field::ResolvingWithInternalMicrotaskOther, jsNull());

    auto* context = uncheckedDowncast<JSPromiseCombinatorsGlobalContext>(callee->getField(JSFunctionWithFields::Field::ResolvingWithInternalMicrotaskContext));
    JSValue argument = callFrame->argument(0);
    JSValue onFulfilled = context->promise();
    JSPromise::resolveWithInternalMicrotask(globalObject, vm, argument, static_cast<InternalMicrotask>(onFulfilled.asInt32()), context->remainingElementsCount());
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(promiseResolvingFunctionRejectWithInternalMicrotask, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();

    auto* callee = uncheckedDowncast<JSFunctionWithFields>(callFrame->jsCallee());
    auto* other = dynamicDowncast<JSFunctionWithFields>(callee->getField(JSFunctionWithFields::Field::ResolvingWithInternalMicrotaskOther));
    if (!other) [[unlikely]]
        return JSValue::encode(jsUndefined());

    callee->setField(vm, JSFunctionWithFields::Field::ResolvingWithInternalMicrotaskOther, jsNull());
    other->setField(vm, JSFunctionWithFields::Field::ResolvingWithInternalMicrotaskOther, jsNull());

    auto* context = uncheckedDowncast<JSPromiseCombinatorsGlobalContext>(callee->getField(JSFunctionWithFields::Field::ResolvingWithInternalMicrotaskContext));
    JSValue argument = callFrame->argument(0);
    JSValue onFulfilled = context->promise();
    JSPromise::rejectWithInternalMicrotask(vm, globalObject, argument, static_cast<InternalMicrotask>(onFulfilled.asInt32()), context->remainingElementsCount());
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(promiseCapabilityExecutor, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto* callee = uncheckedDowncast<JSFunctionWithFields>(callFrame->jsCallee());
    JSValue resolve = callee->getField(JSFunctionWithFields::Field::ExecutorResolve);
    if (!resolve.isUndefined()) [[unlikely]]
        return throwVMTypeError(globalObject, scope, "resolve function is already set"_s);

    JSValue reject = callee->getField(JSFunctionWithFields::Field::ExecutorReject);
    if (!reject.isUndefined()) [[unlikely]]
        return throwVMTypeError(globalObject, scope, "reject function is already set"_s);

    callee->setField(vm, JSFunctionWithFields::Field::ExecutorResolve, callFrame->argument(0));
    callee->setField(vm, JSFunctionWithFields::Field::ExecutorReject, callFrame->argument(1));

    return JSValue::encode(jsUndefined());
}

std::tuple<JSFunction*, JSFunction*> JSPromise::createResolvingFunctions(VM& vm, JSGlobalObject* globalObject)
{
    auto* resolve = JSFunctionWithFields::create(vm, globalObject, vm.promiseResolvingFunctionResolveExecutable(), 1, nullString());
    auto* reject = JSFunctionWithFields::create(vm, globalObject, vm.promiseResolvingFunctionRejectExecutable(), 1, nullString());

    resolve->setField(vm, JSFunctionWithFields::Field::ResolvingPromise, this);
    resolve->setField(vm, JSFunctionWithFields::Field::ResolvingOther, reject);

    reject->setField(vm, JSFunctionWithFields::Field::ResolvingPromise, this);
    reject->setField(vm, JSFunctionWithFields::Field::ResolvingOther, resolve);

    return std::tuple { resolve, reject };
}

JSFunction* JSPromise::createFirstResolveFunction(VM& vm, JSGlobalObject* globalObject)
{
    auto* resolve = JSFunctionWithFields::create(vm, globalObject, vm.promiseFirstResolvingFunctionResolveExecutable(), 1, nullString());
    resolve->setField(vm, JSFunctionWithFields::Field::FirstResolvingPromise, this);
    return resolve;
}

JSFunction* JSPromise::createFirstRejectFunction(VM& vm, JSGlobalObject* globalObject)
{
    auto* reject = JSFunctionWithFields::create(vm, globalObject, vm.promiseFirstResolvingFunctionRejectExecutable(), 1, nullString());
    reject->setField(vm, JSFunctionWithFields::Field::FirstResolvingPromise, this);
    return reject;
}

std::tuple<JSFunction*, JSFunction*> JSPromise::createFirstResolvingFunctions(VM& vm, JSGlobalObject* globalObject)
{
    return std::tuple { createFirstResolveFunction(vm, globalObject), createFirstRejectFunction(vm, globalObject) };
}

std::tuple<JSFunction*, JSFunction*> JSPromise::createResolvingFunctionsWithInternalMicrotask(VM& vm, JSGlobalObject* globalObject, InternalMicrotask task, JSValue context)
{
    JSValue encodedTask = jsNumber(static_cast<int32_t>(task));

    auto* resolve = JSFunctionWithFields::create(vm, globalObject, vm.promiseResolvingFunctionResolveWithInternalMicrotaskExecutable(), 1, nullString());
    auto* reject = JSFunctionWithFields::create(vm, globalObject, vm.promiseResolvingFunctionRejectWithInternalMicrotaskExecutable(), 1, nullString());

    auto* all = JSPromiseCombinatorsGlobalContext::create(vm, encodedTask, encodedTask, context);

    resolve->setField(vm, JSFunctionWithFields::Field::ResolvingWithInternalMicrotaskContext, all);
    resolve->setField(vm, JSFunctionWithFields::Field::ResolvingWithInternalMicrotaskOther, reject);

    reject->setField(vm, JSFunctionWithFields::Field::ResolvingWithInternalMicrotaskContext, all);
    reject->setField(vm, JSFunctionWithFields::Field::ResolvingWithInternalMicrotaskOther, resolve);

    return std::tuple { resolve, reject };
}

void JSPromise::triggerPromiseReactions(VM& vm, JSGlobalObject* globalObject, Status status, JSPromiseReaction* head, JSValue argument)
{
    bool isResolved = status == JSPromise::Status::Fulfilled;

    auto queue = [&](JSPromiseReaction* reaction) ALWAYS_INLINE_LAMBDA {
        JSValue promise = reaction->promise();
        InternalMicrotask task = InternalMicrotask::PromiseReactionJob;
        JSValue handler;
        JSValue arg = argument;

        switch (reaction->type()) {
        case JSSlimPromiseReactionType: {
            auto* slimReaction = uncheckedDowncast<JSSlimPromiseReaction>(reaction);
            if (auto internalTask = slimReaction->internalMicrotask(); internalTask != InternalMicrotask::None) {
                task = internalTask;
                handler = argument;
                arg = slimReaction->handlerOrContext();
#if USE(BUN_JSC_ADDITIONS)
                if (vm.m_synchronousModuleQueue && isModuleLoaderInternalMicrotask(task)) [[unlikely]] {
                    vm.m_synchronousModuleQueue->tasks.append({ task, static_cast<uint8_t>(status), promise, handler, arg });
                    return;
                }
#endif
            } else if (slimReaction->isFulfillHandler() == isResolved)
                handler = slimReaction->handlerOrContext();
            else {
                task = InternalMicrotask::PromiseResolveWithoutHandlerJob;
                handler = argument;
                arg = jsUndefined();
            }
            break;
        }
        case JSFullPromiseReactionType: {
            auto* fullReaction = uncheckedDowncast<JSFullPromiseReaction>(reaction);
            handler = isResolved ? fullReaction->onFulfilled() : fullReaction->onRejected();
#if USE(BUN_JSC_ADDITIONS)
            // performPromiseThen normalizes non-callable sides to jsUndefined() when storing
            // an async context in a full reaction; cheap tag check instead of isCallable().
            if (handler.isUndefined()) {
                task = InternalMicrotask::PromiseResolveWithoutHandlerJob;
                handler = argument;
                arg = jsUndefined();
                break;
            }
            JSValue context = fullReaction->context();
            if (!context.isUndefinedOrNull()) {
                globalObject->queueMicrotask(vm, task, static_cast<uint8_t>(status), promise, handler, arg, context);
                return;
            }
#else
            ASSERT(fullReaction->context().isUndefinedOrNull());
#endif
            break;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }


        globalObject->queueMicrotask(vm, task, static_cast<uint8_t>(status), promise, handler, arg);
    };

    ASSERT(head);
    if (!head->next()) [[likely]] {
        queue(head);
        return;
    }

    // Reverse the order of singly-linked-list.
    JSPromiseReaction* previous = nullptr;
    {
        auto* current = head;
        while (current) {
            auto* next = current->next();
            current->setNext(vm, previous);
            previous = current;
            current = next;
        }
    }
    head = previous;

    auto* current = head;
    do {
        auto* next = current->next();
        queue(current);
        current = next;
    } while (current);
}

void JSPromise::resolveWithInternalMicrotaskForAsyncAwait(JSGlobalObject* globalObject, VM& vm, JSValue resolution, InternalMicrotask task, JSValue context)
{
#if USE(BUN_JSC_ADDITIONS)
    // Capture Bun's async context at the point of await and wrap it with the generator context.
    // This allows AsyncFunctionResume and related microtasks to restore the async context when
    // resuming the async function.
    JSValue wrappedContext = context;
    // ALS1 (U-T9): per-lite cursor GIL-off; in-object member otherwise.
    if (auto* asyncContextData = threadAsyncContextData(globalObject)) {
        JSValue asyncContext = asyncContextData->getInternalField(0);
        if (!asyncContext.isUndefined()) {
            auto* tuple = InternalFieldTuple::create(vm, globalObject->internalFieldTupleStructure(), context, asyncContext);
            wrappedContext = tuple;
        }
    }
#define BUN_CONTEXT wrappedContext
#else
#define BUN_CONTEXT context
#endif


    if (resolution.inherits<JSPromise>()) {
        auto* promise = uncheckedDowncast<JSPromise>(resolution);
        if (promise->realm() == globalObject && promiseSpeciesWatchpointIsValid(vm, promise)) [[likely]]
            return promise->performPromiseThenWithInternalMicrotask(vm, task, nullptr, BUN_CONTEXT);

        JSValue constructor;
        JSValue error;
        {
            auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
            constructor = promise->get(globalObject, vm.propertyNames->constructor);
            if (catchScope.exception()) [[unlikely]] {
                error = catchScope.exception()->value();
                if (!catchScope.clearExceptionExceptTermination()) [[unlikely]]
                    return;
            }
        }
        if (error) [[unlikely]] {
            std::array<JSValue, maxMicrotaskArguments> arguments { {
                jsUndefined(),
                error,
                BUN_CONTEXT,
            } };
            runInternalMicrotask(globalObject, vm, task, static_cast<uint8_t>(JSPromise::Status::Rejected), arguments);
            return;
        }

        if (constructor == globalObject->promiseConstructor())
            return promise->performPromiseThenWithInternalMicrotask(vm, task, nullptr, BUN_CONTEXT);
    }

    resolveWithInternalMicrotask(globalObject, vm, resolution, task, BUN_CONTEXT);

#undef BUN_CONTEXT
}

void JSPromise::resolveWithInternalMicrotask(JSGlobalObject* globalObject, VM& vm, JSValue resolution, InternalMicrotask task, JSValue context)
{
    if (!resolution.isObject())
        return fulfillWithInternalMicrotask(vm, globalObject, resolution, task, context);

    auto* resolutionObject = asObject(resolution);
    if (resolutionObject->inherits<JSPromise>()) {
        auto* promise = uncheckedDowncast<JSPromise>(resolutionObject);
        if (promise->realm() == globalObject && promise->isThenFastAndNonObservable())
            return globalObject->queueMicrotask(vm, InternalMicrotask::PromiseResolveThenableJobWithInternalMicrotaskFast, static_cast<uint8_t>(task), resolutionObject, context, jsUndefined());
    }

    if (isDefinitelyNonThenable(resolutionObject, globalObject))
        return fulfillWithInternalMicrotask(vm, globalObject, resolution, task, context);

    JSValue then;
    JSValue error;
    {
        auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        then = resolutionObject->get(globalObject, vm.propertyNames->then);
        if (catchScope.exception()) [[unlikely]] {
            error = catchScope.exception()->value();
            if (!catchScope.clearExceptionExceptTermination()) [[unlikely]]
                return;
        }
    }
    if (error) [[unlikely]]
        return rejectWithInternalMicrotask(vm, globalObject, error, task, context);

    if (!then.isCallable()) [[likely]]
        return fulfillWithInternalMicrotask(vm, globalObject, resolution, task, context);

    return globalObject->queueMicrotask(vm, InternalMicrotask::PromiseResolveThenableJobWithInternalMicrotask, static_cast<uint8_t>(task), resolutionObject, then, context);
}

void JSPromise::rejectWithInternalMicrotask(VM& vm, JSGlobalObject* globalObject, JSValue argument, InternalMicrotask task, JSValue context)
{
    globalObject->queueMicrotask(vm, task, static_cast<uint8_t>(Status::Rejected), jsUndefined(), argument, context);
}

void JSPromise::fulfillWithInternalMicrotask(VM& vm, JSGlobalObject* globalObject, JSValue argument, InternalMicrotask task, JSValue context)
{
    globalObject->queueMicrotask(vm, task, static_cast<uint8_t>(Status::Fulfilled), jsUndefined(), argument, context);
}

bool JSPromise::isThenFastAndNonObservable()
{
    JSGlobalObject* globalObject = this->realm();
    Structure* structure = this->structure();
    if (!globalObject->promiseThenWatchpointSet().isStillValid()) [[unlikely]]
        return false;

    if (structure == globalObject->promiseStructure())
        return true;

    if (getPrototypeDirect() != globalObject->promisePrototype())
        return false;

    VM& vm = globalObject->vm();
    if (getDirectOffset(vm, vm.propertyNames->then) != invalidOffset)
        return false;

    return true;
}

JSObject* promiseSpeciesConstructor(JSGlobalObject* globalObject, JSObject* thisObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (auto* promise = dynamicDowncast<JSPromise>(thisObject)) [[likely]] {
        if (promiseSpeciesWatchpointIsValid(vm, promise)) [[likely]]
            return globalObject->promiseConstructor();
    }

    JSValue constructor = thisObject->get(globalObject, vm.propertyNames->constructor);
    RETURN_IF_EXCEPTION(scope, { });

    if (constructor.isUndefined())
        return globalObject->promiseConstructor();

    if (!constructor.isObject()) [[unlikely]] {
        throwTypeError(globalObject, scope, "|this|.constructor is not an Object or undefined"_s);
        return { };
    }

    constructor = asObject(constructor)->get(globalObject, vm.propertyNames->speciesSymbol);
    RETURN_IF_EXCEPTION(scope, { });

    if (constructor.isUndefinedOrNull())
        return globalObject->promiseConstructor();

    if (constructor.isConstructor()) [[likely]]
        return asObject(constructor);

    throwTypeError(globalObject, scope, "|this|.constructor[Symbol.species] is not a constructor"_s);
    return { };
}

Structure* createPromiseCapabilityObjectStructure(VM& vm, JSGlobalObject& globalObject)
{
    Structure* structure = globalObject.structureCache().emptyObjectStructureForPrototype(&globalObject, globalObject.objectPrototype(), JSFinalObject::defaultInlineCapacity);
    PropertyOffset offset;
    structure = Structure::addPropertyTransition(vm, structure, vm.propertyNames->resolve, 0, offset);
    RELEASE_ASSERT(offset == promiseCapabilityResolvePropertyOffset);
    structure = Structure::addPropertyTransition(vm, structure, vm.propertyNames->reject, 0, offset);
    RELEASE_ASSERT(offset == promiseCapabilityRejectPropertyOffset);
    structure = Structure::addPropertyTransition(vm, structure, vm.propertyNames->promise, 0, offset);
    RELEASE_ASSERT(offset == promiseCapabilityPromisePropertyOffset);
    return structure;
}

JSObject* JSPromise::then(JSGlobalObject* globalObject, JSValue onFulfilled, JSValue onRejected)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* resultPromise;
    JSValue resultPromiseCapability;
    if (promiseSpeciesWatchpointIsValid(vm, this)) [[likely]] {
        resultPromise = JSPromise::create(vm, globalObject->promiseStructure());
        resultPromiseCapability = resultPromise;
    } else {
        auto* constructor = promiseSpeciesConstructor(globalObject, this);
        RETURN_IF_EXCEPTION(scope, { });

        auto [promise, resolve, reject] = JSPromise::newPromiseCapability(globalObject, constructor);
        RETURN_IF_EXCEPTION(scope, { });

        resultPromise = promise;
        resultPromiseCapability = JSPromise::createPromiseCapability(vm, globalObject, promise, resolve, reject);
    }

    scope.release();
    performPromiseThen(vm, globalObject, onFulfilled, onRejected, resultPromiseCapability);
    return resultPromise;
}

JSObject* JSPromise::promiseResolve(JSGlobalObject* globalObject, JSObject* constructor, JSValue argument)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (argument.inherits<JSPromise>()) {
        auto* promise = uncheckedDowncast<JSPromise>(argument);
        if (promiseSpeciesWatchpointIsValid(vm, promise)) [[likely]] {
            if (constructor == promise->realm()->promiseConstructor())
                return promise;
        } else {
            auto property = promise->get(globalObject, vm.propertyNames->constructor);
            RETURN_IF_EXCEPTION(scope, { });

            if (property == constructor)
                return promise;
        }
    }

    if (constructor == globalObject->promiseConstructor()) [[likely]] {
        JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
        scope.release();
        promise->resolve(globalObject, vm, argument);
        return promise;
    }

    auto [promise, resolve, reject] = newPromiseCapability(globalObject, constructor);
    RETURN_IF_EXCEPTION(scope, { });

    MarkedArgumentBuffer arguments;
    arguments.append(argument);
    ASSERT(!arguments.hasOverflowed());
    scope.release();
    call(globalObject, resolve, jsUndefined(), arguments, "resolve is not a function"_s);
    return promise;
}

JSObject* JSPromise::promiseReject(JSGlobalObject* globalObject, JSObject* constructor, JSValue argument)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (constructor == globalObject->promiseConstructor()) [[likely]] {
        JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
        promise->reject(vm, argument);
        return promise;
    }

    auto [promise, resolve, reject] = newPromiseCapability(globalObject, constructor);
    RETURN_IF_EXCEPTION(scope, { });

    MarkedArgumentBuffer arguments;
    arguments.append(argument);
    ASSERT(!arguments.hasOverflowed());
    scope.release();
    call(globalObject, reject, jsUndefined(), arguments, "reject is not a function"_s);
    return promise;
}

} // namespace JSC
