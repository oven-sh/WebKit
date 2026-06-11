/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#include "config.h"
#include "ConcurrentButterflyOperations.h"

#include "FrameTracers.h"
#include "JSCConfig.h"
#include "Options.h"
#include "ThrowScope.h"
#include "VM.h"
#include <atomic>
#include <limits>
#include <mutex>

#if OS(DARWIN)
#include <pthread.h>
#endif

// vmstate workstream (SPEC-vmstate section 6.7): sole provider of
// currentButterflyTID() and setVMLiteTIDTagHook(). Until it lands we run a
// main-thread-only interim shim (TID 0), the same pattern as
// SPEC-objectmodel section 9.1. THREADS-INTEGRATE(jit): delete the shim when
// VMLite.h is in-tree; INTEGRATE-jit.md records the swap.
#if __has_include("VMLite.h")
#include "VMLite.h"
#define JSC_JIT_HAS_VMLITE 1
#endif

// Object-model workstream (SPEC-objectmodel section 9): segmented-butterfly
// and shared-write helpers the R3 shims below forward to.
// THREADS-INTEGRATE(jit): forwarding bodies are filled in (Tasks 8-10) once
// runtime/ConcurrentButterfly.h lands; see the per-operation notes.
#if __has_include("ConcurrentButterfly.h")
#include "ConcurrentButterfly.h"
#define JSC_JIT_HAS_CONCURRENT_BUTTERFLY 1
#endif

namespace JSC {

// ===========================================================================
// P5: per-thread butterfly TID tag storage (SPEC-jit R5; App. R5).
// ===========================================================================

#if OS(LINUX)
extern "C" __attribute__((tls_model("initial-exec"))) thread_local uint64_t g_jscButterflyTIDTag = 0;
#else
extern "C" thread_local uint64_t g_jscButterflyTIDTag = 0;
#endif

#if !defined(JSC_JIT_HAS_VMLITE) && !defined(JSC_JIT_HAS_CONCURRENT_BUTTERFLY)
// Interim shim until vmstate W3 lands: only the main thread runs JS, and the
// main thread's butterfly TID is 0 (SPEC-vmstate I18). When the OM workstream's
// ConcurrentButterfly.h is in-tree, ITS shim (OM section 9.1, same pattern)
// provides this symbol instead; ours must not redeclare it.
static ALWAYS_INLINE uint16_t currentButterflyTID() { return 0; }
#endif

// ===========================================================================
// Task 1b (SPEC-jit R5/P5/I19; annex App. R5): per-platform setup so JIT'd
// code can read the tag in ONE load off the thread register, with the offset
// (ELF) or TSD-slot offset (Darwin) baked as an immediate at emission.
// ===========================================================================

#if OS(LINUX) && (CPU(X86_64) || CPU(ARM64))
#define JSC_BUTTERFLY_TID_TAG_ELF_TLS 1

static ALWAYS_INLINE uintptr_t currentThreadPointer()
{
    uintptr_t threadPointer;
#if CPU(X86_64)
    // x86-64 ELF TLS ABI (glibc and musl): the TCB self-pointer lives at
    // %fs:0, so this single load yields the thread pointer the JIT's
    // fs-prefixed loads are relative to.
    asm volatile("movq %%fs:0, %0" : "=r"(threadPointer));
#elif CPU(ARM64)
    asm volatile("mrs %0, tpidr_el0" : "=r"(threadPointer));
#endif
    return threadPointer;
}

static intptr_t s_butterflyTIDTagELFTLSOffset;
static std::atomic<bool> s_butterflyTIDTagELFTLSOffsetComputed { false };
static std::once_flag s_butterflyTIDTagELFTLSOffsetOnce;

// App. R5: process init computes the thread-invariant initial-exec TLS-base
// offset; every subsequent thread startup recomputes it and RELEASE_ASSERTs
// constancy (the property the baked-immediate emitters rely on).
static void setUpButterflyTIDTagLoadForCurrentThread()
{
    intptr_t offset = static_cast<intptr_t>(
        reinterpret_cast<uintptr_t>(&g_jscButterflyTIDTag) - currentThreadPointer());
    // Both emitters encode the offset as a (sign-extended) disp32.
    RELEASE_ASSERT(offset == static_cast<intptr_t>(static_cast<int32_t>(offset)));
    std::call_once(s_butterflyTIDTagELFTLSOffsetOnce, [&] {
        s_butterflyTIDTagELFTLSOffset = offset;
        s_butterflyTIDTagELFTLSOffsetComputed.store(true, std::memory_order_release);
    });
    RELEASE_ASSERT(s_butterflyTIDTagELFTLSOffset == offset);
}

intptr_t butterflyTIDTagELFTLSOffset()
{
    RELEASE_ASSERT(s_butterflyTIDTagELFTLSOffsetComputed.load(std::memory_order_acquire));
    return s_butterflyTIDTagELFTLSOffset;
}

#elif OS(DARWIN)
#define JSC_BUTTERFLY_TID_TAG_DARWIN_TSD 1

static pthread_key_t s_butterflyTIDTagPThreadKey;
static std::atomic<bool> s_butterflyTIDTagPThreadKeyCreated { false };
static std::once_flag s_butterflyTIDTagPThreadKeyOnce;

// App. R5: Mach-O TLV has no constant offset and all reserved direct TSD keys
// are taken, so the JIT-visible copy of the tag lives in a dynamically
// created pthread key (TSD slots are uniform, so direct-offset reads via
// fastTLSOffsetForKey are valid for dynamic keys too).
static void ensureButterflyTIDTagPThreadKey()
{
    std::call_once(s_butterflyTIDTagPThreadKeyOnce, [] {
        int result = pthread_key_create(&s_butterflyTIDTagPThreadKey, nullptr);
        RELEASE_ASSERT(!result);
        RELEASE_ASSERT(static_cast<unsigned long>(s_butterflyTIDTagPThreadKey)
            <= std::numeric_limits<uint32_t>::max());
#if defined(JSC_CONFIG_HAS_BUTTERFLY_TID_TAG_TLS_KEY)
        // M4a JSCConfig slot for LLInt's register-form tls_loadp (App. R5).
        // The integrator guarantees first P5 init precedes Config::finalize
        // on Darwin (INTEGRATE-jit.md M4a note); otherwise this store would
        // hit a frozen page.
        g_jscConfig.butterflyTIDTagTLSKey = static_cast<uint32_t>(s_butterflyTIDTagPThreadKey);
#endif
        s_butterflyTIDTagPThreadKeyCreated.store(true, std::memory_order_release);
    });
}

uint32_t butterflyTIDTagTLSKey()
{
    RELEASE_ASSERT(s_butterflyTIDTagPThreadKeyCreated.load(std::memory_order_acquire));
    return static_cast<uint32_t>(s_butterflyTIDTagPThreadKey);
}

#else
// D8/App. R5: no supported JIT-visible TLS mechanism (Windows; Linux on
// other CPUs). Flag-on multi-thread operation is unsupported: P5 init
// RELEASE_ASSERTs at second-thread startup.
static std::atomic<uint32_t> s_butterflyTIDTagInitializedThreadCount { 0 };
#endif

// CS3 hook body: keeps g_jscButterflyTIDTag coherent across lazy VM-lite
// installs, multi-VM switches (vmstate section 6.4.4), and detaches (I19).
// Also the body run by initialize/clear below, so all writers agree.
static void updateButterflyTIDTag(uint16_t tid)
{
    uint64_t tag = static_cast<uint64_t>(tid) << 48;
    g_jscButterflyTIDTag = tag;
#if defined(JSC_BUTTERFLY_TID_TAG_DARWIN_TSD)
    // Keep the JIT-visible TSD copy in lockstep with the C++ thread_local
    // (App. R5). The hook can legally run on a thread before P5 init created
    // the key only in Phase-A standalone builds; the guard keeps that sound.
    if (s_butterflyTIDTagPThreadKeyCreated.load(std::memory_order_acquire)) {
        int result = pthread_setspecific(s_butterflyTIDTagPThreadKey,
            reinterpret_cast<void*>(static_cast<uintptr_t>(tag)));
        RELEASE_ASSERT(!result);
    }
#endif
}

void initializeButterflyTIDTagForCurrentThread()
{
#if defined(JSC_JIT_HAS_VMLITE)
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        // CS3: vmstate's VMLite::setCurrent invokes the hook post-TLS-write
        // with `lite ? lite->tid : 0`; registering it here (P5 init) makes the
        // direct spawn/detach calls below idempotent with the hook's.
        setVMLiteTIDTagHook(&updateButterflyTIDTag);
    });
#endif

#if defined(JSC_BUTTERFLY_TID_TAG_ELF_TLS)
    setUpButterflyTIDTagLoadForCurrentThread();
#elif defined(JSC_BUTTERFLY_TID_TAG_DARWIN_TSD)
    ensureButterflyTIDTagPThreadKey();
#else
    {
        static thread_local bool threadCounted = false;
        if (!threadCounted) {
            threadCounted = true;
            if (s_butterflyTIDTagInitializedThreadCount.fetch_add(1, std::memory_order_relaxed)) {
                // Second-thread startup on a platform with no JIT-visible TLS
                // mechanism: unsupported flag-on (D8).
                RELEASE_ASSERT(!Options::useJSThreads());
            }
        }
    }
#endif

    updateButterflyTIDTag(currentButterflyTID());
}

void clearButterflyTIDTagForCurrentThread()
{
    updateButterflyTIDTag(0);
}

void assertButterflyTIDTagCoherent()
{
    uint64_t expected = static_cast<uint64_t>(currentButterflyTID()) << 48;
    RELEASE_ASSERT(g_jscButterflyTIDTag == expected);
#if defined(JSC_BUTTERFLY_TID_TAG_ELF_TLS)
    // Verify the JIT's view: the same word, read the way generated code reads
    // it (thread pointer + baked offset).
    if (s_butterflyTIDTagELFTLSOffsetComputed.load(std::memory_order_acquire)) {
        uint64_t jitView = *reinterpret_cast<uint64_t*>(
            currentThreadPointer() + s_butterflyTIDTagELFTLSOffset);
        RELEASE_ASSERT(jitView == expected);
    }
#elif defined(JSC_BUTTERFLY_TID_TAG_DARWIN_TSD)
    if (s_butterflyTIDTagPThreadKeyCreated.load(std::memory_order_acquire)) {
        uint64_t jitView = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(pthread_getspecific(s_butterflyTIDTagPThreadKey)));
        RELEASE_ASSERT(jitView == expected);
    }
#endif
}

#if ENABLE(JIT)

// ===========================================================================
// R3 shims. Frozen call-side contracts (SPEC-jit section 5.5).
//
// Task 8 status: LLInt/Baseline emission routes every predicate failure to
// the op's EXISTING generic slow path (see the choke points in
// llint/LowLevelInterpreter64.asm and jit/CCallHelpers.cpp), so nothing
// references these shims yet - they are the DFG/FTL tail-call form
// (Tasks 9/10). Referencing the OM helpers (segmentedOutOfLineSlot /
// ensureSharedWriteBit / locked AS ops) from here today would be an
// undefined symbol: the OM header declares them but its .cpp definitions
// land with OM Tasks 4-8. Each body keeps the standard JIT-operation
// prologue so the forwarding fill is purely the marked line.
//
// Reviewers (R2-5, re-affirmed R3-8): the absence of regime-2 fast paths in
// EVERY tier is a recorded, tripwired scope reduction, not an oversight —
// see GIL-removal precondition 9 in docs/threads/INTEGRATE-jit.md (fill the
// seven bodies + wire slow-case routing + run the shared-and-reshaped bench
// leg before GIL removal); gilRemovalPreconditionsMet() is constexpr false
// until then, and these RELEASE_ASSERT_NOT_REACHED bodies are unreachable
// today (no emitter references them; all predicate failures route to
// existing generic paths).
// THREADS-INTEGRATE(jit)
// ===========================================================================

JSC_DEFINE_JIT_OPERATION(operationSegmentedButterflyLoad, EncodedJSValue, (VM* vmPointer, JSObject* object, int32_t offset))
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    UNUSED_PARAM(object);
    UNUSED_PARAM(offset);
    // THREADS-INTEGRATE(jit): forward to OM segmentedOutOfLineSlot(spine,
    // offset)->get() through the object's tagged butterfly word (OM 9.3).
    RELEASE_ASSERT_NOT_REACHED();
    OPERATION_RETURN(scope, encodedJSUndefined());
}

JSC_DEFINE_JIT_OPERATION(operationSegmentedButterflyStore, void, (VM* vmPointer, JSObject* object, int32_t offset, EncodedJSValue encodedValue))
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    UNUSED_PARAM(object);
    UNUSED_PARAM(offset);
    UNUSED_PARAM(encodedValue);
    // THREADS-INTEGRATE(jit): forward to OM segmentedOutOfLineSlot(spine,
    // offset)->set(vm, object, value) (OM 9.3; barriered).
    RELEASE_ASSERT_NOT_REACHED();
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationSegmentedButterflyIndexedLoad, EncodedJSValue, (VM* vmPointer, JSObject* object, uint32_t index))
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    UNUSED_PARAM(object);
    UNUSED_PARAM(index);
    // THREADS-INTEGRATE(jit): forward to OM segmentedIndexedSlot(spine, index)
    // (OM 9.3; bounds pre-checked by the caller per C4).
    RELEASE_ASSERT_NOT_REACHED();
    OPERATION_RETURN(scope, encodedJSUndefined());
}

JSC_DEFINE_JIT_OPERATION(operationSegmentedButterflyIndexedStore, void, (VM* vmPointer, JSObject* object, uint32_t index, EncodedJSValue encodedValue))
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    UNUSED_PARAM(object);
    UNUSED_PARAM(index);
    UNUSED_PARAM(encodedValue);
    // THREADS-INTEGRATE(jit): forward to OM segmentedIndexedSlot(spine,
    // index)->set(vm, object, value) (OM 9.3).
    RELEASE_ASSERT_NOT_REACHED();
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationButterflyEnsureSharedWrite, void, (VM* vmPointer, JSObject* object))
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    UNUSED_PARAM(object);
    // THREADS-INTEGRATE(jit): forward to OM ensureSharedWriteBit(vm, object)
    // (OM 9.3: fire-then-DCAS, R-DOUBLE rebox, CoW materialize-first, PA
    // locked flip). Caller performs its own store afterwards (non-AS only).
    RELEASE_ASSERT_NOT_REACHED();
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationSharedArrayStorageLoad, EncodedJSValue, (VM* vmPointer, JSObject* object, uint32_t index))
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    UNUSED_PARAM(object);
    UNUSED_PARAM(index);
    // THREADS-INTEGRATE(jit): cell-locked ArrayStorage read (OM I31: flag-on,
    // EVERY AS access at any SW state is cell-locked on the runtime side; the
    // generated fast path only reaches here when its SW test demands it).
    RELEASE_ASSERT_NOT_REACHED();
    OPERATION_RETURN(scope, encodedJSUndefined());
}

JSC_DEFINE_JIT_OPERATION(operationSharedArrayStorageStore, void, (VM* vmPointer, JSObject* object, uint32_t index, EncodedJSValue encodedValue))
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    UNUSED_PARAM(object);
    UNUSED_PARAM(index);
    UNUSED_PARAM(encodedValue);
    // THREADS-INTEGRATE(jit): write predicate case (4), AS arm (SPEC-jit
    // section 5.5): fire F1, flip SW, and perform the write ITSELF under the
    // cell lock / OM section 4.6 per-event-STW regime - never
    // ensure-SW-then-store inline (I20/OM I31).
    RELEASE_ASSERT_NOT_REACHED();
    OPERATION_RETURN(scope);
}

#endif // ENABLE(JIT)

} // namespace JSC
