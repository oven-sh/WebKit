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

#pragma once

// VMLite — per-thread execution state for N threads sharing one logical VM
// (SPEC-vmstate §6, frozen layout §6.3).
//
// Phase A (this milestone): VMLites are CARRIERS ONLY (§6.1). Under the GIL,
// every JS-visible piece of execution state still lives in VM members under
// today's names; no interpreter/JIT/runtime path consults a VMLite. The
// carriers exist so threads can be tagged (tid), registered (VMLiteRegistry,
// VMLiteShared.h §6.5.1), TLS-installed (setCurrent, L4), and so the frozen
// VMLitePrimitives layout can be asserted layout-identical to the matching VM
// member block (§6.4 M6 equivalence asserts).
//
// Phase B (UNOWNED — SPEC-vmstate Dev 10; frozen contract only): a pinned
// register/TLS base makes `VM::field` accesses VMLitePrimitives-relative, and
// per-thread VMThreadContext/VMTraps land per §6.8. The layout below is the
// ABI Phase B consumes; freeze rules L1-L5 (§6.3) apply.

#include "DFGDoesGCCheck.h" // AB18-C per-lite DoesGC validation word (only forward-declares VM — no cycle with the "must not include VM.h" rule below).
#include "Interpreter.h" // JSOrWasmInstruction (interpreter/Interpreter.h:61); brings JSCJSValue.h (EncodedJSValue).
#include "JSExportMacros.h"
#include "VMExceptionScopeVerificationState.h" // Obligation 10: per-lite EXCEPTION_SCOPE_VERIFICATION state (debug-only L2 tail append below).
#include "VMThreadContext.h" // §A.2.1 per-lite traps/stack limits (brings VMTraps.h; VMLite is only forward-declared there — no cycle).
#include <atomic>
#include <memory>
#include <type_traits>
#include <wtf/BumpPointerAllocator.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafetyAnalysis.h>
#include <wtf/Vector.h>

namespace JSC {

class CallFrame;
class Exception;
class MicrotaskQueue;
class QueuedTask;
class RegExp;
class VM;
class VMEntryScope;
struct EntryFrame;
struct ScratchBuffer; // Defined in VM.h; VMLite.h must not include VM.h (VM.h includes us via M6).

namespace GCClient {
class Heap;
}

// 15-bit thread-ID payload of the tagged butterfly word (SPEC-objectmodel
// §9.1; THREAD.md "2^15 thread-ID space"). ConcurrentButterfly.h declares the
// identical alias — a type-alias redeclaration naming the same type is legal.
using ButterflyTID = uint16_t;

// =============================================================================
// VMLitePrimitives — frozen ABI artifact (SPEC-vmstate §6.3, Groups 1-3).
//
// Names/types EXACTLY mirror the current VM declarations (including m_
// prefixes) — the ABI is consumed via offsetof and .asm. This X-macro is
// authoritative: VM (§6.4 M6) expands the SAME macro to declare its Group 1-3
// member block, and per-field static_asserts (M6, in VM.h) pin
//     OBJECT_OFFSETOF(VM, field) - OBJECT_OFFSETOF(VM, topCallFrame)
//         == OBJECT_OFFSETOF(VMLitePrimitives, field)
// so the two layouts cannot drift.
//
// Freeze rules (§6.3): L1 — field order frozen; add/remove/reorder is a spec
// revision. L5 — no numeric offsets are frozen beyond "Group 1 pair at
// 0x00/0x08"; everything else goes through OBJECT_OFFSETOF; padding is the
// ABI's choice.
//
// Deliberately NOT in VMLitePrimitives (§6.3; M6 relocates these just outside
// the VM block, names/types/sites unchanged): m_terminationException
// (cross-thread by design), maybeReturnPC, topJSPIContext,
// m_currentSoftReservedZoneSize (interleaved in today's Group-3 range; the
// §6.4(2) span assert forces it out), m_executingRegExp.
// =============================================================================

#define FOR_EACH_VMLITE_PRIMITIVE_FIELD(v) \
    /* Group 1: pair; order+adjacency ABI (loadpairq/storepairq of */ \
    /* "VM::topCallFrame" in LowLevelInterpreter.asm; VM.h pair assert) */ \
    v(CallFrame*, topCallFrame) \
    v(EntryFrame*, topEntryFrame) \
    /* Group 2: exception/unwind state (VM.h:395,397,878-890) */ \
    v(Exception*, m_exception) \
    v(Exception*, m_lastException) \
    v(CallFrame*, callFrameForCatch) \
    v(void*, targetMachinePCForThrow) \
    v(JSOrWasmInstruction, targetInterpreterPCForThrow) \
    v(uintptr_t, targetInterpreterMetadataPCForThrow) \
    v(void*, targetMachinePCAfterCatch) \
    v(CallFrame*, newCallFrameReturnValue) \
    v(EncodedJSValue, encodedHostCallReturnValue) \
    v(uint32_t, targetTryDepthForThrow) \
    v(uint32_t, osrExitIndex) \
    v(unsigned, varargsLength) \
    v(void*, osrExitJumpDestination) \
    /* Group 3: VM stack bookkeeping (VM.h:1237-1240). NOT the limits */ \
    /* generated code checks — those are VMTraps::m_stack (§6.8); one */ \
    /* authority, no duplicate here. */ \
    v(void*, m_stackPointerAtVMEntry) \
    v(void*, m_stackLimit) \
    v(void*, m_lastStackTop)

struct VMLitePrimitives {
#define VMLITE_DECLARE_FIELD(type, name) type name { };
    FOR_EACH_VMLITE_PRIMITIVE_FIELD(VMLITE_DECLARE_FIELD)
#undef VMLITE_DECLARE_FIELD

    // Per-field offsets, VMLitePrimitives-relative (Phase B consumes; L5:
    // never frozen numerically, always via OBJECT_OFFSETOF).
#define VMLITE_DECLARE_FIELD_OFFSET(type, name) \
    static constexpr ptrdiff_t offsetOf_##name() { return OBJECT_OFFSETOF(VMLitePrimitives, name); }
    FOR_EACH_VMLITE_PRIMITIVE_FIELD(VMLITE_DECLARE_FIELD_OFFSET)
#undef VMLITE_DECLARE_FIELD_OFFSET
};

// L3 (§6.3): deliberately NO standard-layout assert — targetInterpreterPCForThrow
// is a Variant (rev 7). OBJECT_OFFSETOF stays valid via __builtin_offsetof
// (§0); the §6.4(2) per-field equivalence asserts in VM.h are the real
// layout contract.
static_assert(std::is_trivially_copyable_v<VMLitePrimitives>); // variant of trivials
static_assert(OBJECT_OFFSETOF(VMLitePrimitives, topCallFrame) == 0);
static_assert(OBJECT_OFFSETOF(VMLitePrimitives, topEntryFrame) == sizeof(void*),
    "pair-load contract");

// =============================================================================
// ScratchBufferRegistry — process-wide baked-scratch-buffer index space
// (SPEC-ungil §A.1.6, ANNEX A16, BINDING; UNGIL U-T1, dark until U-T4 emits
// against it).
//
// GIL-off, scratchBufferForSize ADDRESSES baked into DFG/FTL code would be
// shared by N threads. Instead, every baked site becomes
// `loadVMLite -> segment -> [index]`: codegen allocates a process-wide
// monotonic INDEX here (with an index->size map, never freed), and each
// VMLite holds an append-only segmented pointer table (lock-free reads,
// below). A buffer must exist at (lite, index) BEFORE the compiled code can
// run: VM::allocateBakedScratchBufferIndex() fans the install to the VM's
// registered lites, and carrier/spawn registration backfills
// (VMLite::backfillBakedScratchBuffers). Indices are process-wide but only
// the single m_gilOff VM (U0b/U0c) ever allocates them, so cross-VM index
// collision cannot arise in v1.
//
// Lock rank (§LK): m_lock sits OUTSIDE VMLiteRegistry::lock — the install
// nesting ScratchBufferRegistry -> VMLiteRegistry::lock ->
// VMLite::scratchBufferLock is LEGAL (§LK.6 re-rank; SUPERSESSION vs vmstate
// §6.5.1/§7 "registry lock is a leaf", both sides — recorded in
// INTEGRATE-ungil.md (i)).
// =============================================================================

class ScratchBufferRegistry {
    WTF_MAKE_NONCOPYABLE(ScratchBufferRegistry);
public:
    ScratchBufferRegistry() = default; // Construction reserved to singleton() (NeverDestroyed).
    JS_EXPORT_PRIVATE static ScratchBufferRegistry& singleton();

    // Allocates the next monotonic index and records its size. Never freed,
    // never reused (A16: "monotonic indices + an index->size map, never
    // freed").
    JS_EXPORT_PRIVATE unsigned allocateIndex(size_t);
    JS_EXPORT_PRIVATE size_t sizeForIndex(unsigned) const;
    JS_EXPORT_PRIVATE unsigned indexCount() const;

private:
    mutable Lock m_lock; // §LK rank: outside VMLiteRegistry::lock (see class comment).
    Vector<size_t> m_sizes WTF_GUARDED_BY_LOCK(m_lock); // index -> size; append-only.
};

// =============================================================================
// VMLite — the per-thread carrier (SPEC-vmstate §6.3).
//
// Lifetime/registration (§6.5.1, VMLiteShared.h): registerLite(lite, vm) is
// the sole writer of `vm` (immutable after); a lite is unregistered before it
// is destroyed and before its thread's teardown setCurrent(nullptr); a VM
// must not die while a registered lite's `vm` points at it (§6.4.4 ~VM
// assert). The main thread's carrier is VM::m_mainVMLite (tid 0), installed/
// restored by JSLock mirroring the atom-table swap (§6.4.4 — M4 hunks in
// INTEGRATE-vmstate.md).
// =============================================================================

class VMLite {
    WTF_MAKE_NONCOPYABLE(VMLite);
    WTF_MAKE_TZONE_ALLOCATED(VMLite);
public:
    VMLitePrimitives primitives;  // FIRST member, offset 0 (asserted below). FROZEN (L1).
    uint16_t tid { 0 };           // ButterflyTID; 0 = main thread. §6.7: ThreadManager (api WS)
                                  // is the sole allocator; spawn writes it BEFORE setCurrent;
                                  // immutable while installed, so reads need no sync.
    VM* vm { nullptr };           // Set by VMLiteRegistry::registerLite(lite, vm) (§6.5.1, sole
                                  // writer); immutable after.
    // Group 4: regexp, lazy:
    RegExp* executingRegExp { nullptr };
    std::unique_ptr<BumpPointerAllocator> regExpAllocator;
    // Group 5: scratch buffers (§6.6; Phase A inert — reserved for the frozen
    // Phase-B contract: baked DFG/FTL scratch-buffer pointers become
    // VMLite-relative):
    Lock scratchBufferLock; // leaf rank (§7): fastMalloc only under it
    Vector<ScratchBuffer*> scratchBuffers;
    // Group 6: microtasks (§6.5), lazy (Phase A: exercised only by unit tests;
    // VM::queueMicrotask/drainMicrotasks are NOT rerouted):
    RefPtr<MicrotaskQueue> defaultMicrotaskQueue;

    // L2 (§6.3): new fields append after Group 6 only. Appended (task 7,
    // §6.6 plumbing): logically Group-5 state — L2 forbids inserting it next
    // to scratchBuffers above. Guarded by scratchBufferLock.
    size_t sizeOfLastScratchBuffer { 0 };

    // ---- UNGIL U-T1 L2 appends (SPEC-ungil; dark while every VM has
    // m_gilOff == 0). Append-only per L1/L2; nothing above moves. ----

    // §A.1.3 two-level discriminator, level 2: copied from vm->m_gilOff at
    // lite registration (every registration site copies it immediately after
    // VMLiteRegistry::registerLite, before the lite can be installed). LLInt
    // loads this byte when the process-level JSCConfig gilOffProcess byte
    // (level 1, U-T3) is set: 0 => VM Group-3 storage (a second, GIL-on VM's
    // protocol stays intact — U0b), 1 => lite storage.
    uint8_t gilOff { 0 };

    // ANNEX EXIT1/A36 (r31/r32) carrier/lite state machine. EVERY transition
    // AND every read is under VMLiteRegistry::lock. LIVE -> TEARDOWN (owner's
    // TLS destructor, live path) | LIVE -> COLLECTED -> DETACHED (~VM walk);
    // TEARDOWN and DETACHED are terminal. U-T1 lands the storage + the LIVE/
    // TEARDOWN marks on the paths it owns; U-T6 owns the full teardown
    // protocol (COLLECTED/DETACHED, the EXIT1.9 fence, the deferred dtor).
    enum class State : uint8_t { Live, Teardown, Collected, Detached };
    State state { State::Live };

    // ANNEX A36 (r32): registration-time-fixed under the registry lock — set
    // iff WTF::isMainThread() at §F.1 first-entry registration; immutable
    // thereafter; read under the registry lock like the state byte. A static
    // structural fact ("this lite's owner thread runs no TLS destructors"),
    // NEVER a liveness probe.
    bool ownerHasNoTlsDtor { false };

    // ANNEX EXIT1/A36C: the per-thread GCClient this carrier enters with;
    // null => not-entered (conductor predicate, EXIT1.2). U-T1 stamps the
    // VM's single clientHeap; U-T6 replaces with per-thread clients.
    GCClient::Heap* clientHeap { nullptr };

    // §A.1.5 per-entry record: GIL-off, VM::entryScope/VM::isEntered() and
    // the entry-scope service bits live per-lite; VM-wide consumers iterate
    // the registry under its lock. Written by VMEntryScope::setUpSlow/
    // tearDownSlow on the owning thread.
    //
    // UNGIL review fix: atomic, and GIL-off the setUpSlow/tearDownSlow
    // stores additionally run under VMLiteRegistry::lock. The registry-lock
    // walks (anyOtherLiteOfVMEntered, fanOutTerminationToSiblingLites,
    // VM::isAnyThreadEntered) read OTHER threads' records under that lock;
    // a plain owner-thread store was (a) a data race on the pointer (TSAN
    // no-JIT gate breakage) and (b) unserialized against the TERM1.2
    // retire/leave-set decision — with the store under the lock, a
    // sibling's entered<->not-entered transition cannot interleave a
    // retire walk, so "no other lite entered" observed under the lock is a
    // real snapshot, not a stale read. Same-thread fast-path reads
    // (VMEntryScopeInlines ctor/dtor gates, VM::currentThreadEntryScope)
    // use relaxed loads of the own-thread record. GIL-on: the VM-member
    // shadow is the only record; this field stays null.
    std::atomic<VMEntryScope*> entryScope { nullptr };

    // §A.1.5 service bits, same packing as VM::EntryScopeServicesBits
    // (byte 0 = EntryScopeService, byte 1 = ConcurrentEntryScopeService) —
    // interpreted ONLY by VM's accessors (VM.h owns the packing). Atomic:
    // VM-wide fan-out (under the registry lock) writes other threads' lites
    // while their owners read/clear concurrently.
    std::atomic<uint16_t> entryScopeServicesRawBits { 0 };

    // ANNEX A16 segmented baked-scratch-buffer table: lock-free reads
    // (`loadVMLite -> segment -> [index]`, all tiers, U-T4); writes under
    // scratchBufferLock. Buffers installed here are ALSO appended to
    // `scratchBuffers` above (the repurposed Group-5 ownership list), which
    // backs the jit-R2 registry GC scan and teardown free.
    static constexpr size_t scratchSegmentShift = 6;
    static constexpr size_t scratchSegmentSize = static_cast<size_t>(1) << scratchSegmentShift; // 64 entries
    static constexpr size_t maxScratchSegments = 256; // 16384 baked indices
    std::atomic<std::atomic<ScratchBuffer*>*> scratchSegments[maxScratchSegments] { };

    // ---- UNGIL §A.2.1 (AB-17 item 1; L2 append — nothing above moves). ----
    // Per-thread VMThreadContext: own VMTraps trap word + StackManager stack
    // limits. Generated code reaches them via the chained offset
    // offsetOfThreadContext() + VMThreadContext::offsetOfTraps() (+
    // VMTraps::offsetOfSoftStackLimit() / offsetOfTrapsBits()) — §6.8.
    // Live only for gilOff lites (perThreadTrapsIfExists, VMLite.cpp);
    // GIL-on lites keep VM-word semantics (U0b second-VM intact).
    VMThreadContext threadContext;

#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    // ---- UNGIL obligation 10 (INTEGRATE-ungil.md; U-T8b; L2 tail append
    // AFTER threadContext — nothing above moves). Debug-only per-lite
    // EXCEPTION_SCOPE_VERIFICATION bookkeeping: the ExceptionScope chain
    // anchor + simulated-throw state. NOT part of the frozen
    // VMLitePrimitives ABI; no generated-code offset reaches it. Selected
    // ONLY through VM::exceptionScopeVerificationState() (the
    // group3Primitives()-style mode split): GIL-off lites use this copy, so
    // a spawned thread's scope chain can never link into the carrier's
    // stack (the deterministic GIL-off ExceptionScope::stackPosition()
    // stack-use-after-return; VMEntryScope.cpp status item (i)). GIL-on /
    // flag-off / second-VM U0b: this copy stays untouched (VM member is
    // authoritative).
    VMExceptionScopeVerificationState exceptionScopeVerificationState;
#endif

#if ENABLE(DFG_DOES_GC_VALIDATION)
    // ---- UNGIL AB18-C (L2 tail append AFTER exceptionScopeVerificationState
    // — nothing above moves). Per-mutator DoesGC validation word: GIL-off,
    // every DFG/FTL setDoesGCExpectation store and every verifyCanGC read
    // targets the CURRENT thread's slot (VM::doesGCCheckSlot() mode split;
    // emission: loadVMLite -> offsetOfDoesGC()). Default-constructs to
    // encode(true, Special::Uninitialized) (DoesGCCheck ctor), so a fresh
    // lite trivially passes until its first DFG/FTL store. Owner-thread-only
    // (written and read by the installing thread only), so plain stores —
    // including the emission-side two-store32 pair form — need no atomicity.
    // NOT part of the frozen VMLitePrimitives ABI. GIL-on / flag-off /
    // second-VM U0b: untouched (VM::m_doesGC is authoritative).
    DFG::DoesGCCheck doesGC;
    static constexpr ptrdiff_t offsetOfDoesGC() { return OBJECT_OFFSETOF(VMLite, doesGC); }
#else
    // Stub mirrors VM::addressOfDoesGC()'s !ENABLE arm so `if constexpr
    // (validateDFGDoesGC)` emission splits type-check in all configurations.
    static ptrdiff_t offsetOfDoesGC() { UNREACHABLE_FOR_PLATFORM(); return 0; }
#endif

    static constexpr ptrdiff_t offsetOfPrimitives() { return OBJECT_OFFSETOF(VMLite, primitives); }
    static constexpr ptrdiff_t offsetOfTID() { return OBJECT_OFFSETOF(VMLite, tid); }
    static constexpr ptrdiff_t offsetOfGilOff() { return OBJECT_OFFSETOF(VMLite, gilOff); } // LLInt level-2 byte (U-T3).
    static constexpr ptrdiff_t offsetOfScratchSegments() { return OBJECT_OFFSETOF(VMLite, scratchSegments); } // A16 emission (U-T4).
    static constexpr ptrdiff_t offsetOfThreadContext() { return OBJECT_OFFSETOF(VMLite, threadContext); } // §A.2.1 chained-offset emission (U-T3/U-T4).

    // TLS accessors (L4): backed by `thread_local VMLite* t_currentVMLite` in
    // VMLite.cpp (NOT pthread_getspecific). Signatures frozen; the
    // implementation is replaceable in Phase B (pinned register/TLS base).
    JS_EXPORT_PRIVATE static VMLite* currentIfExists();   // TLS read
    JS_EXPORT_PRIVATE static VMLite& current();           // asserts non-null
    // Installs `lite` (may be null = uninstall) and returns the previous
    // value. Debug (I18/I20): asserts lite->tid != notTTLTID and that the
    // lite is registered. After the TLS write it invokes the TID-tag hook
    // (below) with `lite ? lite->tid : 0` — including for null — so the JIT's
    // per-thread butterfly TID tag stays coherent across §6.4.4
    // install/restore and multi-VM switches (jit CS3/I19).
    JS_EXPORT_PRIVATE static VMLite* setCurrent(VMLite*); // returns previous

    JS_EXPORT_PRIVATE VMLite();
    JS_EXPORT_PRIVATE ~VMLite(); // I20: asserts not installed here, not registered; poisons in debug.

    // Helpers defined in VMLiteInlines.h (I11 owner asserts build on these).
    inline bool isInstalledOnCurrentThread() const;
    inline BumpPointerAllocator& ensureRegExpAllocator(); // Group 4 lazy

    // ---- §6.5 Group 6: per-thread default microtask queue (Phase A inert —
    // VM::queueMicrotask/drainMicrotasks are NOT rerouted; Phase B routes
    // enqueue/drain to the current thread's queue; cross-thread enqueue stays
    // out of scope).
    //
    // COVERAGE STATUS (honest gap, tracked): these facilities (and the §6.6
    // scratch-buffer ones below) are JS-UNREACHABLE in Phase A, so the
    // JSTests/threads/vmstate suite cannot execute them; the task-7 C++ test
    // obligations (owner-only enqueue/drain, lazy-creation idempotence,
    // drain-exactly-once, scratchBufferForSize(0)==nullptr, geometric-growth
    // reuse, dtor-frees-under-ASAN) are an UNMET PENDING item in
    // INTEGRATE-vmstate.md and MUST land before any Phase-B routing consults
    // a VMLite. Do not treat this block as gated by the existing suites.
    // HARD GATE (review round 3): until that test lands, ANY new caller of
    // these facilities in any workstream's diff is a blocker by construction
    // — see the PENDING entry in INTEGRATE-vmstate.md. ----
    //
    // Lazy creation. GC visibility (§6.5): the queue is built with
    // MicrotaskQueue::create(*vm), whose constructor appends it to
    // VM::m_microtaskQueues — the single registration list GC markers
    // traverse. Both the append (M12) and the markers' iteration (M11) take
    // VMLiteRegistry::singleton().lock once those hunks are applied, so lazy
    // creation on a spawned thread cannot corrupt LIST MEMBERSHIP under
    // concurrent marking. The registry lock deliberately does NOT cover queue
    // CONTENTS: inside the locked forEach the marker calls visitAggregate,
    // which walks the queue's Deque lock-free. That is sound for the same
    // reason it is sound today (MicrotaskQueue.cpp:158-160): deque contents
    // are only visited at collector phases that suspend the mutator
    // (FixPoint/Begin — the "Sh" constraint runs in the fixpoint, world
    // stopped). Under useJSThreads this invariant must mean ALL mutators
    // stopped — verified, not assumed: M11 rationale + cross-WS checklist
    // item 12 in INTEGRATE-vmstate.md make it an explicit integration gate.
    // I11/I14: caller must be the installed owner thread AND hold the VM's
    // JSLock (asserted).
    JS_EXPORT_PRIVATE MicrotaskQueue& ensureDefaultMicrotaskQueue();

    // I11 helpers (VMLiteInlines.h): the per-thread queue is enqueued/drained
    // only by its owner — both debug-assert isInstalledOnCurrentThread().
    inline void enqueueMicrotaskToDefaultQueue(QueuedTask&&);
    // Runs a full microtask checkpoint on the default queue (no-op if the
    // queue was never created). Requires the JSLock (it enters the VM).
    inline void drainDefaultMicrotaskQueue();

    // ---- §6.6 Group 5: per-thread scratch buffers. This signature is the
    // §6.6-reserved one and must not change; UNGIL §A.1.6 (annex A16) makes
    // it the GIL-off NON-BAKED scratch path: VM::scratchBufferForSize
    // dispatches here when vm.m_gilOff (size-class policy mirrors VM's,
    // including geometric growth).
    //
    // GC (UNGIL U-T1, supersedes the Phase-A caveat that used to live here):
    // VM::gatherScratchBufferRoots now ALSO walks the VMLiteRegistry and
    // scans every same-VM lite's `scratchBuffers` ownership list (jit R2),
    // so buffers handed out here MAY carry values needing marking, exactly
    // like VM's.
    JS_EXPORT_PRIVATE ScratchBuffer* scratchBufferForSize(size_t);
    JS_EXPORT_PRIVATE void clearScratchBuffers(); // setActiveLength(0) on all; owner-only.

    // ---- ANNEX A16 baked-index table (UNGIL U-T1; emission lands at
    // U-T4a/U-T4b). ----

    // Lock-free read: `segment acquire-load -> entry acquire-load`. Null
    // until installed; compiled code never runs before the install/backfill
    // contract has populated its indices (A16).
    ScratchBuffer* scratchBufferAtIndex(unsigned index) const
    {
        ASSERT(index < maxScratchSegments * scratchSegmentSize);
        auto* segment = scratchSegments[index >> scratchSegmentShift].load(std::memory_order_acquire);
        if (!segment)
            return nullptr;
        return segment[index & (scratchSegmentSize - 1)].load(std::memory_order_acquire);
    }

    // Idempotent install: creates the buffer (of the registry-recorded size)
    // at `index` if absent, appending it to the `scratchBuffers` ownership
    // list. Takes scratchBufferLock; legal under VMLiteRegistry::lock
    // (§LK.6 re-rank — the VM-side install fan holds it).
    JS_EXPORT_PRIVATE void ensureScratchBufferAtIndex(unsigned index, size_t);

    // Registration backfill (A16: "a buffer exists at (lite, index) BEFORE
    // the code runs — install fans to the VM's lites; registration
    // backfills"). Called after VMLiteRegistry::registerLite at every
    // GIL-off registration site; idempotent vs a racing install fan.
    JS_EXPORT_PRIVATE void backfillBakedScratchBuffers();
};
static_assert(OBJECT_OFFSETOF(VMLite, primitives) == 0);

// =============================================================================
// §6.7 — currentButterflyTID()
//
// SOLE defining TU is VMLite.cpp (INTEGRATE verifies ODR; the
// ConcurrentButterfly.h §9.1 shim and the jit ConcurrentButterflyOperations.cpp
// shim are both #if !__has_include("VMLite.h")-guarded and compile away now
// that this header exists). Returns the installed carrier's tid, or 0 when no
// VMLite is installed (I18: main thread / embedder threads / GIL phase — a
// never-entering thread touches no JS objects, so 0 is unobservable there);
// never notTTLTID (0x7fff).
// =============================================================================

JS_EXPORT_PRIVATE ButterflyTID currentButterflyTID();

// TID-tag hook (§6.7; jit CS3/I19 provider). Null by default; jit task 1b
// (ConcurrentButterflyOperations.cpp, initializeButterflyTIDTagForCurrentThread)
// registers its P5 tag-update body so every lite switch keeps
// g_jscButterflyTIDTag coherent without a runtime/ -> jit/ include. A null
// hook is a no-op (Phase-A standalone builds). The hook runs AFTER the TLS
// write in setCurrent, with `lite ? lite->tid : 0` (including uninstalls);
// api §5.2's explicit P5/clear calls remain and are idempotent with it.
JS_EXPORT_PRIVATE void setVMLiteTIDTagHook(void (*)(uint16_t));

// Carrier-TID allocation hooks (UNGIL §A.3.6 / ANNEX A36; U-T1). GIL-off,
// every main/embedder carrier lite needs a TM-allocated unique nonzero TID
// from the SAME 2^15 space as spawned threads (I17 exhaustion accounting
// includes carriers; main/embedder ThreadState.tid STAYS 0 — the carrier TID
// is a separate allocation, r9 F4). ThreadManager (api workstream; IU row)
// registers the provider pair at initialization; runtime/ must not include
// ThreadManager.h here for the same layering reason as the TID-tag hook
// above. allocateCarrierTID() RELEASE_ASSERTs a provider is installed —
// reaching it without one is an integration bug (the GIL-off entry path is
// unreachable until the U-T6/U-T9 activation tasks, so dark builds never
// trip it).
JS_EXPORT_PRIVATE void setCarrierTIDHooks(uint16_t (*allocate)(), void (*release)(uint16_t));
JS_EXPORT_PRIVATE uint16_t allocateCarrierTID();
JS_EXPORT_PRIVATE void releaseCarrierTIDIfHooked(uint16_t);

} // namespace JSC
