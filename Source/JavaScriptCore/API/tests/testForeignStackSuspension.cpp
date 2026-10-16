/*
 * Copyright (C) 2026 Peter Steinberger. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND ITS CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR ITS
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#if !ENABLE(SIGNAL_BASED_VM_TRAPS)
#error This regression requires signal-based VM traps.
#endif

#include "APICast.h"
#include "Exception.h"
#include "GCRequest.h"
#include "Heap.h"
#include "InitializeThreading.h"
#include "JSCellInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "JSLock.h"
#include "JSObjectInlines.h"
#include "JavaScript.h"
#include "Options.h"
#include "ThrowScope.h"
#include "VM.h"
#include "VMInlines.h"
#include <atomic>
#include <cstdio>
#include <signal.h>
#include <sys/mman.h>
#include <thread>
#include <ucontext.h>
#include <unistd.h>
#include <wtf/MainThread.h>
#include <wtf/ThreadMessage.h>
#include <wtf/WTFConfig.h>

#if ASAN_ENABLED
#include <sanitizer/common_interface_defs.h>
#endif
#if TSAN_ENABLED
#include <sanitizer/tsan_interface.h>
#endif

using JSC::CollectionScope;
using JSC::JSLockHolder;
using JSC::Options;
using JSC::Sync;
using JSC::VM;

namespace {
struct State {
    std::atomic<bool> entered { false };
    std::atomic<bool> release { false };
    std::atomic<bool> returned { false };
    std::atomic<bool> finish { false };
    std::atomic<bool> terminated { false };
    std::atomic<VM*> vm { nullptr };
    ucontext_t original;
    ucontext_t alternate;
#if ASAN_ENABLED
    void* originalFakeStack { nullptr };
    const void* originalStackBottom { nullptr };
    size_t originalStackSize { 0 };
#endif
#if TSAN_ENABLED
    void* originalFiber { nullptr };
    void* alternateFiber { nullptr };
#endif
};
thread_local State* activeState;
struct sigaction previousSuspendAction;
std::atomic<unsigned> suspensionSignals { 0 };
static_assert(std::atomic<unsigned>::is_always_lock_free);

void observeSuspendSignal(int signal, siginfo_t* info, void* context)
{
    if (activeState && activeState->entered.load(std::memory_order_relaxed))
        suspensionSignals.fetch_add(1, std::memory_order_relaxed);
    previousSuspendAction.sa_sigaction(signal, info, context);
}

void waitFor(const std::atomic<bool>& flag)
{
    while (!flag.load(std::memory_order_acquire))
        std::this_thread::yield();
}

void foreignMain()
{
    auto& state = *activeState;
#if ASAN_ENABLED
    __sanitizer_finish_switch_fiber(nullptr, &state.originalStackBottom, &state.originalStackSize);
#endif
    RELEASE_ASSERT(!Thread::currentSingleton().stack().contains(currentStackPointer()));
    state.entered.store(true, std::memory_order_release);
    waitFor(state.release);
#if ASAN_ENABLED
    __sanitizer_start_switch_fiber(nullptr, state.originalStackBottom, state.originalStackSize);
#endif
#if TSAN_ENABLED
    __tsan_switch_to_fiber(state.originalFiber, 0);
#endif
    RELEASE_ASSERT(!swapcontext(&state.alternate, &state.original));
    RELEASE_ASSERT_NOT_REACHED();
}

void runOnForeignStack(State& state)
{
    constexpr size_t stackSize = 1024 * 1024;
    size_t guardSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto* mapping = static_cast<char*>(mmap(nullptr, stackSize + guardSize * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    RELEASE_ASSERT(mapping != MAP_FAILED);
    RELEASE_ASSERT(!mprotect(mapping + guardSize, stackSize, PROT_READ | PROT_WRITE));
    activeState = &state;
    Thread::currentSingleton();
    RELEASE_ASSERT(!getcontext(&state.alternate));
    state.alternate.uc_stack.ss_sp = mapping + guardSize;
    state.alternate.uc_stack.ss_size = stackSize;
    state.alternate.uc_link = nullptr;
    makecontext(&state.alternate, foreignMain, 0);
#if TSAN_ENABLED
    state.originalFiber = __tsan_get_current_fiber();
    state.alternateFiber = __tsan_create_fiber(0);
#endif
#if ASAN_ENABLED
    __sanitizer_start_switch_fiber(&state.originalFakeStack, mapping + guardSize, stackSize);
#endif
#if TSAN_ENABLED
    __tsan_switch_to_fiber(state.alternateFiber, 0);
#endif
    RELEASE_ASSERT(!swapcontext(&state.original, &state.alternate));
#if ASAN_ENABLED
    __sanitizer_finish_switch_fiber(state.originalFakeStack, nullptr, nullptr);
#endif
#if TSAN_ENABLED
    __tsan_destroy_fiber(state.alternateFiber);
#endif
    RELEASE_ASSERT(!munmap(mapping, stackSize + guardSize * 2));
    state.returned.store(true, std::memory_order_release);
    activeState = nullptr;
}

JSC_DECLARE_HOST_FUNCTION(park);
JSC_DEFINE_HOST_FUNCTION(park, (JSC::JSGlobalObject* globalObject, JSC::CallFrame*))
{
    auto& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A host function retains the executing VM's ownership throughout the
    // native call, as Bun's N-API bridge does.
    RELEASE_ASSERT(vm.currentThreadIsHoldingAPILock());
    auto& state = *activeState;
    runOnForeignStack(state);
    RELEASE_ASSERT(vm.traps().needHandling(JSC::VMTraps::NeedTermination));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSC::JSValue::encode(JSC::jsUndefined()));
}

JSValueRef evaluate(JSGlobalContextRef context, const char* text, JSValueRef* exception)
{
    JSStringRef script = JSStringCreateWithUTF8CString(text);
    JSValueRef result = JSEvaluateScript(context, script, nullptr, nullptr, 1, exception);
    JSStringRelease(script);
    return result;
}

#if !defined(TEST_FOREIGN_STACK_BLOCKING_CONTROL)
void testMessageRefusal()
{
    State state;
    auto worker = Thread::create("Foreign stack test"_s, [&] {
        runOnForeignStack(state);
        waitFor(state.finish);
    });
    waitFor(state.entered);
    bool called = false;
    {
        ThreadSuspendLocker locker;
        auto result = WTF::trySendMessage(locker, worker.get(), [&](PlatformRegisters&) { called = true; });
        RELEASE_ASSERT(result == WTF::MessageStatus::TemporarilyUnavailable);
        RELEASE_ASSERT(!called);
    }
    RELEASE_ASSERT(!state.returned.load());
    state.release.store(true, std::memory_order_release);
    waitFor(state.returned);
    {
        ThreadSuspendLocker locker;
        auto result = WTF::trySendMessage(locker, worker.get(), [&](PlatformRegisters&) { called = true; });
        RELEASE_ASSERT(result == WTF::MessageStatus::MessageRan && called);
        // A refused attempt must not leave an outstanding suspension count.
        RELEASE_ASSERT(worker->suspend(locker));
        RELEASE_ASSERT(worker->suspend(locker));
        worker->resume(locker);
        worker->resume(locker);
    }
    state.finish.store(true, std::memory_order_release);
    worker->waitForCompletion();
    std::puts("PASS temporary refusal, later delivery, nested suspension and resume");
}
#endif

void testTerminationAndGC()
{
    State state;
    JSGlobalContextRef parent = JSGlobalContextCreate(nullptr);
    JSValueRef exception = nullptr;
    JSValueRef root = evaluate(parent, "({value: 240, child: {value: 5718}})", &exception);
    RELEASE_ASSERT(root && !exception);
    JSValueProtect(parent, root);
    auto worker = Thread::create("Foreign stack VM"_s, [&] {
        JSGlobalContextRef context = JSGlobalContextCreate(nullptr);
        auto& vm = toJS(context)->vm();
        activeState = &state;
        {
            JSLockHolder lock(vm);
            vm.ensureTerminationException();
            auto* globalObject = toJS(context);
            auto name = JSC::Identifier::fromString(vm, "park"_s);
            auto* function = JSC::JSFunction::create(vm, globalObject, 0, name.string(), park, JSC::ImplementationVisibility::Public);
            globalObject->putDirect(vm, name, function);
        }
        state.vm.store(&vm, std::memory_order_release);
        JSValueRef thrown = nullptr;
        JSValueRef result = evaluate(context, "park(); while (true) {}", &thrown);
        {
            JSLockHolder lock(vm);
            state.terminated.store(!result && thrown && toJS(toJS(context), thrown) == vm.terminationException()->value(), std::memory_order_release);
            JSGlobalContextRelease(context);
        }
    });
    waitFor(state.entered);
    unsigned signalsBefore = suspensionSignals.load();
    state.vm.load(std::memory_order_acquire)->notifyNeedTermination();
    while (suspensionSignals.load() == signalsBefore)
        std::this_thread::yield();
    {
        // The release flag is deliberately written only after the parent GC.
        // An unbounded trap-sender retry holds the lock this collection needs.
        JSLockHolder lock(toJS(parent)->vm());
        toJS(parent)->vm().heap.collectNow(Sync, CollectionScope::Full);
    }
    RELEASE_ASSERT(!state.returned.load());
    JSStringRef key = JSStringCreateWithUTF8CString("value");
    JSValueRef value = JSObjectGetProperty(parent, JSValueToObject(parent, root, nullptr), key, nullptr);
    RELEASE_ASSERT(JSValueToNumber(parent, value, nullptr) == 240);
    JSStringRelease(key);
    state.release.store(true, std::memory_order_release);
    worker->waitForCompletion();
    RELEASE_ASSERT(state.returned.load() && state.terminated.load());
    JSValueUnprotect(parent, root);
    {
        JSLockHolder lock(toJS(parent)->vm());
        JSGlobalContextRelease(parent);
    }
    std::puts("PASS parent full GC before native release, rooted object retained, pending termination delivered");
}
}

int main()
{
    alarm(30);
    WTF::initializeMainThread();
    JSC::initialize();
    RELEASE_ASSERT(Options::setOptions("usePollingTraps=false"));
    RELEASE_ASSERT(!sigaction(g_wtfConfig.sigThreadSuspendResume, nullptr, &previousSuspendAction));
    RELEASE_ASSERT(previousSuspendAction.sa_flags & SA_SIGINFO);
    auto action = previousSuspendAction;
    action.sa_sigaction = observeSuspendSignal;
    RELEASE_ASSERT(!sigaction(g_wtfConfig.sigThreadSuspendResume, &action, nullptr));
#if !defined(TEST_FOREIGN_STACK_BLOCKING_CONTROL)
    testMessageRefusal();
#endif
    testTerminationAndGC();
    RELEASE_ASSERT(!sigaction(g_wtfConfig.sigThreadSuspendResume, &previousSuspendAction, nullptr));
    alarm(0);
    return 0;
}
