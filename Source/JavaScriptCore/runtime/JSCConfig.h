/*
 * Copyright (C) 2019-2025 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "Gate.h"
#include "OptionsList.h"
#include "SecureARM64EHashPins.h"
#include "StopTheWorldCallback.h"
#include <mutex>
#include <wtf/PtrTag.h>
#include <wtf/WTFConfig.h>

namespace JSC {

class ExecutableAllocator;
class FixedVMPoolExecutableAllocator;
class VM;

#if ENABLE(SEPARATED_WX_HEAP)
using JITWriteSeparateHeapsFunction = void (*)(off_t, const void*, size_t);
#endif

#define JSC_CONFIG_METHOD(method) \
    WTF_FUNCPTR_PTRAUTH_STR("JSCConfig." #method) method

struct Config {
    static Config& NODELETE singleton();

    static void disableFreezingForTesting() { g_wtfConfig.disableFreezingForTesting(); }
    JS_EXPORT_PRIVATE static void NODELETE enableRestrictedOptions();
    // UNGIL SPEC-ungil sec.A.1.3 level (i) latch (AB17g item 2, F1; closes
    // AB-1, U-T3 obligation 9b): derive the gilOffProcess byte from the
    // finalized Options exactly once. Callable from two sites: (1) the VM
    // ctor strictly BEFORE the m_gilOff designation (runtime/VM.cpp) — the
    // F1 ordering that lets VM::gilOffWithProcessGate() be a single
    // read-only-page byte test — and (2) Config::finalize() below (spent
    // call_once => no-op there). The store happens strictly BEFORE
    // WTF::Config::finalize() freezes the Config page (a second store --
    // even same-value -- would fault on the read-only page, hence the
    // call_once; threads returning from the call_once have a happens-before
    // edge to the store, and the freeze happens after it inside WTF's own
    // call_once, so no thread can reach the store post-freeze).
    // Reader-side visibility premise: every reader of this byte — the LLInt
    // asm dispatch arms AND every C++ gilOffWithProcessGate() caller — runs
    // on a thread whose VM*/VMLite reached it through a synchronized
    // publication (thread spawn, JSLock, registry lock) that happens-after
    // the first VM ctor's latch; a thread reading the byte without such a
    // chain would already be racing the whole VM. A refactor that lets
    // LLInt run before/outside VM entry breaks this visibility contract.
    // Ordering contract: the first call happens after Options::finalize()
    // (the JSC callers are the VM ctor's pre-designation latch and
    // Config::finalize(), both of which run strictly after
    // InitializeThreading's Options::finalize()), asserted below. The
    // derivation MUST stay identical to VM::isGILOffProcess()
    // (runtime/VM.cpp) and to the notifyOptionsChanged() RELEASE_ASSERT
    // latch (runtime/Options.cpp), which forbids the derivation from
    // flipping after options finalization.
    static void latchGILOffProcess()
    {
        static std::once_flag s_gilOffProcessLatchOnce;
        std::call_once(s_gilOffProcessLatchOnce, [] {
            Config& config = singleton();
            // Intentionally fail-stops callers that run before
            // Options::finalize().
            RELEASE_ASSERT(config.options.isFinalized);
            bool gilOffProcess = config.options.useJSThreads
                && !config.options.useThreadGIL
                && config.options.useVMLite
                && config.options.useSharedAtomStringTable
                && config.options.useSharedGCHeap;
            if (gilOffProcess) {
                // An embedder that froze the page directly via
                // WTF::Config::finalize() before constructing the first VM
                // would otherwise take a raw SIGSEGV on the store below;
                // fail-stop with a diagnosable crash site instead.
                RELEASE_ASSERT(!config.isPermanentlyFrozen());
                config.gilOffProcess = 1;
            }
        });
    }

    static void finalize()
    {
        // Latch is normally already spent (the VM ctor latches before its
        // m_gilOff designation); kept here so embedder/direct callers of
        // Config::finalize() still latch before the freeze.
        latchGILOffProcess();
        WTF::Config::finalize();
    }

    static void configureForTesting()
    {
        WTF::setPermissionsOfConfigPage();
        disableFreezingForTesting();
        enableRestrictedOptions();
    }

    bool isPermanentlyFrozen() { return g_wtfConfig.isPermanentlyFrozen; }

    // All the fields in this struct should be chosen such that their
    // initial value is 0 / null / falsy because Config is instantiated
    // as a global singleton.
    // FIXME: We should use a placement new constructor from JSC::initialize so we can use default initializers.

    bool restrictedOptionsEnabled;
    bool jitDisabled;
    bool vmCreationDisallowed;
    bool vmEntryDisallowed;

    bool useFastJITPermissions;

    // The following HasBeenCalled flags are for auditing call_once initialization functions.
    bool initializeHasBeenCalled;

    struct {
#if ASSERT_ENABLED
        bool canUseJITIsSet;
#endif
        bool canUseJIT;
    } vm;

#if CPU(ARM64E)
    bool canUseFPAC;
#endif
    ExecutableAllocator* executableAllocator;
    FixedVMPoolExecutableAllocator* fixedVMPoolExecutableAllocator;
    void* startExecutableMemory;
    void* endExecutableMemory;
    uintptr_t startOfFixedWritableMemoryPool;
    uintptr_t startOfStructureHeap;
    uintptr_t structureIDBase;
    uintptr_t sizeOfStructureHeap;
    void* defaultCallThunk;
    void* arityFixupThunk;

#if ENABLE(SEPARATED_WX_HEAP)
    JITWriteSeparateHeapsFunction jitWriteSeparateHeaps;
#endif

    OptionsStorage options;

    // SPEC-jit M4a (Option 1 — the §5.4 LLInt gate byte is satisfied by the
    // Options::useJSThreads() backing byte already inside `options` above;
    // only the Darwin TLS-key slot is added):
    uint32_t butterflyTIDTagTLSKey; // Darwin only: pthread key for g_jscButterflyTIDTag (SPEC-jit App. R5)
#define JSC_CONFIG_HAS_BUTTERFLY_TID_TAG_TLS_KEY 1 // consumed by jit/ConcurrentButterflyOperations.cpp (Task 1b)

    // UNGIL SPEC-ungil §A.1.3 level (i) / U0c (closes AB-1): process-level
    // Group-3 storage discriminator byte for the LLInt. 0 => VM-block
    // Group-3 storage everywhere (flag-off and GIL-on processes; each LLInt
    // Group-3 site pays one not-taken byte test — the delta-(a) budget).
    // 1 => the LLInt additionally consults the CURRENT thread's
    // VMLite::gilOff byte (level (ii)) and, when that is also set, addresses
    // VMLitePrimitives storage instead of the VM block.
    //
    // Write contract (UNGIL review round, supersedes the earlier "U0C CAS
    // winner writes it from the VM ctor" sketch — that store races
    // Config::finalize() freezing the page; even a same-value store faults
    // once the page is read-only): OPTION-derived and latched exactly once
    // via latchGILOffProcess() above (call_once), derivation-identical to
    // VM::isGILOffProcess() (see the contract comment beside it in
    // runtime/VM.cpp). The latch runs in the VM ctor strictly BEFORE the
    // m_gilOff designation (AB17g item 2, F1) and again — as a spent no-op
    // — from Config::finalize(), strictly before WTF::Config::finalize()
    // freezes the page. Flag-off and GIL-on processes derive 0, so every
    // LLInt discriminator keeps VM-block storage authoritative there (pure
    // flag-off shape).
    uint8_t gilOffProcess;
#define JSC_CONFIG_HAS_GILOFF_PROCESS_BYTE 1 // consumed by llint/LowLevelInterpreter*.asm; written only by the latchGILOffProcess() call_once above

    using ShellTimeoutCheckCallback = void (*)(VM&);
    ShellTimeoutCheckCallback JSC_CONFIG_METHOD(shellTimeoutCheckCallback);

    StopTheWorldCallback JSC_CONFIG_METHOD(wasmDebuggerOnStop);
    using PostResumeCallback = void (*)();
    PostResumeCallback JSC_CONFIG_METHOD(wasmDebuggerOnResume);
    StopTheWorldCallback JSC_CONFIG_METHOD(memoryDebuggerStopTheWorld);
#if USE(BUN_JSC_ADDITIONS)
    StopTheWorldCallback JSC_CONFIG_METHOD(jsDebuggerStopTheWorld);
#endif

    static constexpr unsigned exceptionInstructionsSize = 64;
    struct {
        uint8_t exceptionInstructions[exceptionInstructionsSize];
        const void* gateMap[numberOfGates];
    } llint;

#if CPU(ARM64E) && ENABLE(PTRTAG_DEBUGGING)
    WTF::PtrTagLookup ptrTagLookupRecord;
#endif

#if CPU(ARM64E) && ENABLE(JIT)
    SecureARM64EHashPins arm64eHashPins;
#endif
};

constexpr size_t alignmentOfJSCConfig = std::alignment_of<JSC::Config>::value;

static_assert(WTF::offsetOfWTFConfigExtension + sizeof(JSC::Config) <= WTF::ConfigSizeToProtect);
static_assert(roundUpToMultipleOf<alignmentOfJSCConfig>(WTF::offsetOfWTFConfigExtension) == WTF::offsetOfWTFConfigExtension);

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

// Workaround to localize bounds safety warnings to this file.
// FIXME: Use real types to make materializing JSC::Config* bounds-safe and type-safe.
inline Config* addressOfJSCConfig() { return std::bit_cast<Config*>(&g_wtfConfig.spaceForExtensions); }

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#define g_jscConfig (*JSC::addressOfJSCConfig())

constexpr size_t offsetOfJSCConfigInitializeHasBeenCalled = offsetof(JSC::Config, initializeHasBeenCalled);
constexpr size_t offsetOfJSCConfigGateMap = offsetof(JSC::Config, llint.gateMap);
constexpr size_t offsetOfJSCConfigStructureIDBase = offsetof(JSC::Config, structureIDBase);
constexpr size_t offsetOfJSCConfigDefaultCallThunk = offsetof(JSC::Config, defaultCallThunk);

ALWAYS_INLINE PURE_FUNCTION uintptr_t startOfStructureHeap()
{
    return g_jscConfig.startOfStructureHeap;
}

ALWAYS_INLINE PURE_FUNCTION uintptr_t sizeOfStructureHeap()
{
    return g_jscConfig.sizeOfStructureHeap;
}

ALWAYS_INLINE PURE_FUNCTION uintptr_t structureIDBase()
{
    return g_jscConfig.structureIDBase;
}

} // namespace JSC
