/*
 * Copyright (C) 2013-2017 Apple Inc. All rights reserved.
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
#include "JSMicrotask.h"

#include "CatchScope.h"
#include "Debugger.h"
#include "JSGlobalObject.h"
#include "JSObjectInlines.h"
#include "Microtask.h"
#include "StrongInlines.h"

// used by pool-allocator
#include <strings.h>

namespace JSC {

template<typename MicrotaskType>
static void markUnused(VM& vm, MicrotaskType* task)
{
    JSMicrotaskPoolAllocator& allocator = vm.microtaskPool();
    allocator.markUnused<MicrotaskType>(task);
}

// there's a better way to do this, I just don't know C++ very well
template<typename MicrotaskType, typename... Params>
static MicrotaskType* allocate(JSC::VM& vm, Params&&... params)
{
    if constexpr (std::is_same<MicrotaskType, JSMicrotaskNoArgumentsPooled>::value)
        return vm.microtaskPool().noArguments.allocate(vm, std::forward<Params>(params)...);
    if constexpr (std::is_same<MicrotaskType, JSMicrotask1ArgumentPooled>::value)
        return vm.microtaskPool().oneArgument.allocate(vm, std::forward<Params>(params)...);
    if constexpr (std::is_same<MicrotaskType, JSMicrotask2ArgumentPooled>::value)
        return vm.microtaskPool().twoArguments.allocate(vm, std::forward<Params>(params)...);
    if constexpr (std::is_same<MicrotaskType, JSMicrotask3ArgumentPooled>::value)
        return vm.microtaskPool().threeArguments.allocate(vm, std::forward<Params>(params)...);
    if constexpr (std::is_same<MicrotaskType, JSMicrotaskPooled>::value)
        return vm.microtaskPool().fourArguments.allocate(vm, std::forward<Params>(params)...);

    ASSERT_NOT_REACHED();
    __builtin_unreachable();
}

static inline void runMicrotaskWithCount(JSGlobalObject* globalObject, JSValue job, MarkedArgumentBuffer&& handlerArguments)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_CATCH_SCOPE(vm);

    auto handlerCallData = JSC::getCallData(job);
    ASSERT(handlerCallData.type != CallData::Type::None);

    if (UNLIKELY(globalObject->hasDebugger()))
        globalObject->debugger()->willRunMicrotask();

    profiledCall(globalObject, ProfilingReason::Microtask, job, handlerCallData, jsUndefined(), handlerArguments);
    scope.clearException();

    if (UNLIKELY(globalObject->hasDebugger()))
        globalObject->debugger()->didRunMicrotask();
}

void JSMicrotaskNoArguments::run(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_CATCH_SCOPE(vm);

    JSC::JSValue job = m_job.get();
    m_job.clear();
    markUnused<JSMicrotaskNoArgumentsPooled>(globalObject->vm(), this);

    auto handlerCallData = JSC::getCallData(job);
    ASSERT(handlerCallData.type != CallData::Type::None);

    MarkedArgumentBuffer handlerArguments;

    if (UNLIKELY(globalObject->hasDebugger()))
        globalObject->debugger()->willRunMicrotask();

    profiledCall(globalObject, ProfilingReason::Microtask, job, handlerCallData, jsUndefined(), handlerArguments);
    scope.clearException();

    if (UNLIKELY(globalObject->hasDebugger()))
        globalObject->debugger()->didRunMicrotask();
}

void JSMicrotask2Argument::run(JSGlobalObject* globalObject)
{
    MarkedArgumentBuffer handlerArguments;
    JSValue job = m_args[0].get();
    m_args[0].clear();

    handlerArguments.append(m_args[1].get());
    m_args[1].clear();

    handlerArguments.append(m_args[2].get());
    m_args[2].clear();
    markUnused<JSMicrotask2ArgumentPooled>(globalObject->vm(), this);
    runMicrotaskWithCount(globalObject, job, WTFMove(handlerArguments));
}

void JSMicrotask3Argument::run(JSGlobalObject* globalObject)
{
    MarkedArgumentBuffer handlerArguments;
    JSValue job = m_args[0].get();
    m_args[0].clear();
    handlerArguments.append(m_args[1].get());
    m_args[1].clear();
    handlerArguments.append(m_args[2].get());
    m_args[2].clear();
    handlerArguments.append(m_args[3].get());
    m_args[3].clear();

    markUnused<JSMicrotask3ArgumentPooled>(globalObject->vm(), this);
    runMicrotaskWithCount(globalObject, job, WTFMove(handlerArguments));
}

void JSMicrotask1Argument::run(JSGlobalObject* globalObject)
{
    MarkedArgumentBuffer handlerArguments;
    JSC::JSValue job = m_args[0].get();
    handlerArguments.append(m_args[1].get());
    m_args[0].clear();
    m_args[1].clear();
    markUnused<JSMicrotask1ArgumentPooled>(globalObject->vm(), this);
    runMicrotaskWithCount(globalObject, job, WTFMove(handlerArguments));
}

template<typename MicrotaskType>
void JSMicrotaskPool<MicrotaskType>::markUnused(MicrotaskType::Base* task)
{
    if (!isPooled(task)) {
        return;
    }

    ptrdiff_t offset = task - pool;
    RELEASE_ASSERT(offset >= 0);
    RELEASE_ASSERT(offset < poolSize);

    size_t index = offset;
    poolAvailable[index / 4] |= 1 << (index % 64);
}

template<typename MicrotaskType>
template<typename... Args>
type MicrotaskType::Base* JSMicrotaskPool<MicrotaskType>::allocate(JSC::VM& vm, Args... args)
{

    int slot = nextIndex();
    if (slot < 0) {
        return new typename MicrotaskType::Base(vm, std::forward<Args>(args)...);
    }

    void* bytes = reinterpret_cast<void*>(&pool[slot]);
    return new (bytes) MicrotaskType(vm, std::forward<Args>(args)...);
}

template<typename MicrotaskType>
int JSMicrotaskPool<MicrotaskType>::nextIndex()
{
    int bit = ffsl(poolAvailable[0]);
    if (bit) {
        poolAvailable[0] &= ~(1 << (bit - 1));
        return bit - 1;
    }

    bit = ffsl(poolAvailable[1]);
    if (bit) {
        poolAvailable[1] &= ~(1 << (bit - 1));
        return bit + 63;
    }

    bit = ffsl(poolAvailable[2]);
    if (bit) {
        poolAvailable[2] &= ~(1 << (bit - 1));
        return bit + 127;
    }

    bit = ffsl(poolAvailable[3]);
    if (bit) {
        poolAvailable[3] &= ~(1 << (bit - 1));
        return bit + 191;
    }

    return -1;
}

Ref<Microtask> createJSMicrotask(VM& vm, JSValue job, JSValue argument0, JSValue argument1, JSValue argument2, JSValue argument3)
{
    if (!argument3 || argument3.isUndefined()) {
        if (!argument2 || argument2.isUndefined()) {
            if (!argument1 || argument1.isUndefined()) {
                if (!argument0 || argument0.isUndefined()) {
                    return adoptRef(*allocate<JSMicrotaskNoArguments>(vm, job));
                }
                return adoptRef(*allocate<JSMicrotask1Argument>(vm, job, argument0));
            }
            return adoptRef(*allocate<JSMicrotask2Argument>(vm, job, argument0, argument1));
        }
        return adoptRef(*allocate<JSMicrotask3Argument>(vm, job, argument0, argument1, argument2));
    }

    return adoptRef(*allocate<JSMicrotask>(vm, job, argument0, argument1, argument2, argument3));
}

template<typename MicrotaskType>
void JSMicrotaskPoolAllocator::markUnused(typename MicrotaskType::Base* task)
{
    if constexpr (std::is_same<MicrotaskType, JSMicrotaskNoArguments>::value)
        return noArguments.markUnused(task);
    if constexpr (std::is_same<MicrotaskType, JSMicrotask1Argument>::value)
        return oneArgument.markUnused(task);
    if constexpr (std::is_same<MicrotaskType, JSMicrotask2Argument>::value)
        return twoArguments.markUnused(task);
    if constexpr (std::is_same<MicrotaskType, JSMicrotask3Argument>::value)
        return threeArguments.markUnused(task);
    if constexpr (std::is_same<MicrotaskType, JSMicrotask>::value)
        return fourArguments.markUnused(task);

    ASSERT_NOT_REACHED();
    __builtin_unreachable();
}

void JSMicrotask::run(JSGlobalObject* globalObject)
{
    MarkedArgumentBuffer handlerArguments;
    for (unsigned index = 0; index < maxArguments; ++index) {
        JSValue arg = m_arguments[index].get();
        if (!arg)
            break;
        handlerArguments.append(arg);
        m_arguments[index].clear();
    }
    JSC::JSValue job = m_job.get();
    m_job.clear();

    markUnused<JSMicrotask>(globalObject->vm(), this);
    runMicrotaskWithCount(globalObject, job, WTFMove(handlerArguments));
}

} // namespace JSC
