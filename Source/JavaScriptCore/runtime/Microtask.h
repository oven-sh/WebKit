/*
 * Copyright (C) 2014 Apple Inc. All rights reserved.
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

#pragma once

#include <wtf/ObjectIdentifier.h>
#include <wtf/Platform.h>

namespace JSC {

enum class MicrotaskIdentifierType { };
using MicrotaskIdentifier = ObjectIdentifier<MicrotaskIdentifierType>;

enum class InternalMicrotask : uint8_t {
    None = 0,
    PromiseResolveThenableJobFast,
    PromiseResolveThenableJobWithInternalMicrotaskFast,

    PromiseResolveThenableJob,
    PromiseResolveThenableJobWithInternalMicrotask,

    PromiseResolveWithoutHandlerJob,
    PromiseFulfillWithoutHandlerJob,

    PromiseRaceResolveJob,
    PromiseAllResolveJob,
    PromiseAllSettledResolveJob,
    PromiseAnyResolveJob,
    PromiseFinallyReactionJob,
    PromiseFinallyAwaitJob,

    PromiseReactionJob,

    AsyncFunctionResume,
    AsyncFromSyncIteratorContinue,
    AsyncFromSyncIteratorDone,
    AsyncGeneratorYieldAwaited,
    AsyncGeneratorBodyCallNormal,
    AsyncGeneratorBodyCallReturn,
    AsyncGeneratorAwaitReturn,
    AsyncGeneratorDriverResume,

    InvokeFunctionJob,
    AsyncModuleExecutionResume,
    AsyncModuleExecutionDone,
    ModuleRegistryFetchSettled,
    ModuleRegistryModuleSettled,
    ModuleGraphLoadingError,
    ModuleLoadStep,
    ModuleLoadTopSettled,
    ModuleLoadTopRejected,
    ModuleLoadSpecifierTransform,
    ModuleLoadCombinedLoadSettled,
    ModuleLoadCombinedStateSettled,
    ModuleLoadLinkEvaluateSettled,
    ModuleLoadReturnRecord,
    ModuleLoadReturnModuleKey,
    ModuleLoadStoreError,
    DynamicImportLoadSettled,
    DynamicImportEvaluateSettled,
    DynamicImportDeferLoadSettled,
    DynamicImportDeferDependencySettled,
    ImportModuleNamespace,
#if ENABLE(WEBASSEMBLY)
    WebAssemblyCompileStreaming,
    WebAssemblyInstantiateStreaming,
#endif
    Opaque, // Dispatch must handle everything.
#if USE(BUN_JSC_ADDITIONS)
    BunPerformMicrotaskJob, // Bun's performMicrotask function with async context
    BunInvokeJobWithArguments, // Invoke job function with up to 4 arguments
#endif
};

#if USE(BUN_JSC_ADDITIONS)
constexpr unsigned maxMicrotaskArguments = 4;

// True for the contiguous block of module-loader pipeline tasks plus
// PromiseFulfillWithoutHandlerJob (used only by JSPromise::pipeFrom, which
// itself is called only by the loader). These are the reactions that
// VM::m_synchronousModuleQueue is allowed to divert; everything else
// (AsyncFunctionResume, AsyncGenerator*, user .then() handlers) must keep
// going through the global microtask queue so require(esm) doesn't observably
// reorder a user `await`/`.then()` relative to one queued before the require.
constexpr bool isModuleLoaderInternalMicrotask(InternalMicrotask task)
{
    if (task == InternalMicrotask::PromiseFulfillWithoutHandlerJob)
        return true;
    return static_cast<uint8_t>(task) >= static_cast<uint8_t>(InternalMicrotask::AsyncModuleExecutionResume)
        && static_cast<uint8_t>(task) <= static_cast<uint8_t>(InternalMicrotask::ImportModuleNamespace);
}
#else
constexpr unsigned maxMicrotaskArguments = 3;
#endif

// True for Promise.all/allSettled/any element jobs, whose reaction packs
// (globalContext cell, element index) instead of a single context cell.
constexpr bool promiseReactionPacksGlobalContextAndIndex(InternalMicrotask task)
{
    static_assert(static_cast<uint8_t>(InternalMicrotask::PromiseAllSettledResolveJob) == static_cast<uint8_t>(InternalMicrotask::PromiseAllResolveJob) + 1);
    static_assert(static_cast<uint8_t>(InternalMicrotask::PromiseAnyResolveJob) == static_cast<uint8_t>(InternalMicrotask::PromiseAllSettledResolveJob) + 1);
    return task >= InternalMicrotask::PromiseAllResolveJob && task <= InternalMicrotask::PromiseAnyResolveJob;
}

enum class QueuedTaskResult : uint8_t {
    Executed,
    Discard,
    Suspended,
};

} // namespace JSC
