/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAsyncResumption.h"

#if ENABLE(AOT)

#include "AOTCallSiteCache.h"
#include "AOTFunctionDescription.h"
#include "AOTImage.h"
#include "AOTSavedFrame.h"
#include "AOTStackTrace.h"
#include "DeferGC.h"
#include "InternalFieldTuple.h"
#include "JSCInlines.h"
#include "JSPromise.h"
#include "Microtask.h"
#include <wtf/HashSet.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct SuspendedAsyncFunction {
    SavedFrame* coroutine { nullptr };
    JSPromise* promise { nullptr };
    JSCell* waiter { nullptr };
};

static ALWAYS_INLINE SavedFrame* tryGetSavedFrame(JSCell* cell)
{
    return cell->type() == ObjectType && cell->classInfo() == SavedFrame::info() ? uncheckedDowncast<SavedFrame>(cell) : nullptr;
}

static SuspendedAsyncFunction findSuspendedAsyncFunction(JSCell* cell)
{
    SavedFrame* saved = tryGetSavedFrame(cell);
    if (!saved || saved->kind() != CoroutineKind::AsyncFunction)
        return { };
    return { saved, saved->promise(), saved->waiter() };
}

static void appendFrames(VM& vm, const SuspendedAsyncFunction& suspended, Vector<StackFrame>& results)
{
    if (!suspended.coroutine)
        return;
    const Image& image = *Image::current();
    // The function goes on behind the call that suspended it, so its resume address lies in it.
    const CallSiteCache::Entry* entry = vm.aotState().callSiteCache->resolve(image, suspended.coroutine->resumeAddress());
    if (!entry || FunctionDescription(image.rootFunctionOf(entry->code.index())).codeType() == ModuleCode)
        return;
    ShownFrameFilter filter(true);
    forEachShownFrame(entry->code, entry->site.callSite, filter, [&](const TraceFrame& frame) {
        results.constructAndAppend(frame, true);
    });
}

bool isSuspendedAsyncFunction(JSCell* cell)
{
    return !!findSuspendedAsyncFunction(cell).coroutine;
}

JSPromise* asyncFunctionPromise(JSCell* coroutine)
{
    return findSuspendedAsyncFunction(coroutine).promise;
}

Vector<JSPromise*, 4> promisesSettledBy(JSCell* coroutine)
{
    AssertNoGC assertNoGC;
    Vector<JSPromise*, 4> promises;
    if (auto* tuple = coroutine ? dynamicDowncast<InternalFieldTuple>(coroutine) : nullptr) {
        JSValue context = tuple->getInternalField(0);
        coroutine = context && context.isCell() ? context.asCell() : nullptr;
    }
    if (!coroutine)
        return promises;
    Vector<JSCell*, 4> work { coroutine };
    UncheckedKeyHashSet<JSCell*> visited;
    for (size_t i = 0; i < work.size(); ++i) {
        JSCell* current = work[i];
        if (!visited.add(current).isNewEntry)
            continue;
        if (auto* promise = dynamicDowncast<JSPromise>(current))
            promises.append(promise);
        else if (JSPromise* own = asyncFunctionPromise(current))
            promises.append(own);
        else if (auto* saved = tryGetSavedFrame(current); saved && saved->waiter())
            work.append(saved->waiter());
    }
    return promises;
}

void appendSuspendedAsyncFunction(VM& vm, JSCell*, Vector<StackFrame>& results, JSCell* coroutine)
{
    appendFrames(vm, findSuspendedAsyncFunction(coroutine), results);
}

JSCell* appendDirectAsyncCallers(VM& vm, JSCell*, Vector<StackFrame>& results, JSCell* coroutine, size_t maxStackSize)
{
    constexpr uint16_t mask = JSPromise::stateMask | JSPromise::inlineReactionKindMask | JSPromise::inlineReactionMicrotaskMask;
    constexpr uint16_t isAwaitedByCompiledCode = static_cast<uint16_t>(JSPromise::Status::Pending)
        | static_cast<uint16_t>(JSPromise::InlineReactionKind::InternalMicrotask) << JSPromise::inlineReactionKindShift
        | static_cast<uint16_t>(InternalMicrotask::CompiledAsyncFunctionResume) << JSPromise::inlineReactionMicrotaskShift;
    SuspendedAsyncFunction current = findSuspendedAsyncFunction(coroutine);
    if (!current.coroutine)
        return coroutine;
    for (;;) {
        JSValue context = current.waiter;
        if (!current.waiter) {
            if (!current.promise)
                return nullptr;
            uint16_t flags = current.promise->flags();
            if ((flags & mask) != isAwaitedByCompiledCode || results.size() >= maxStackSize) {
                bool hasNoAwaiters = (flags & JSPromise::stateMask) != static_cast<uint16_t>(JSPromise::Status::Pending) || (!(flags & JSPromise::inlineReactionKindMask) && !current.promise->payloadCell());
                return hasNoAwaiters ? nullptr : current.coroutine;
            }
            context = current.promise->inlineReactionContext();
        } else if (results.size() >= maxStackSize)
            return nullptr;
        if (!context || !context.isCell())
            return current.coroutine;
        SuspendedAsyncFunction caller = findSuspendedAsyncFunction(context.asCell());
        if (!caller.coroutine)
            return current.coroutine;
        current = caller;
        appendFrames(vm, current, results);
        if (results.size() > maxStackSize)
            results.shrink(maxStackSize);
    }
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
