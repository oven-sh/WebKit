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
// Flag-off and GIL-on, every JS-visible piece of execution state lives in the
// VM members under today's names; the only lite traffic is the JSLock
// installing and restoring the thread's carrier (tid, registration in
// VMLiteRegistry, setCurrent). GIL-off (vm.gilOff()), the installed lite is
// the authority for the Group 1-3 primitives (VM::group3Primitives()), the
// per-thread traps and stack limits (threadContext), the scratch buffers, the
// default microtask queue and the per-thread debug state below: C++ reaches
// them through the VM's mode-split selectors, generated code through the
// loadVMLite emitters (jit/AssemblyHelpers) and the g_jscCurrentVMLite loads
// in LowLevelInterpreter.asm. The layout below is that ABI; freeze rules
// L1-L5 (§6.3) apply, and the VM's own Group 1-3 block is asserted
// layout-identical to VMLitePrimitives (§6.4 M6 equivalence asserts).

#include "DFGDoesGCCheck.h" // AB18-C per-lite DoesGC validation word (only forward-declares VM — no cycle with the "must not include VM.h" rule below).
#include "Interpreter.h" // JSOrWasmInstruction (interpreter/Interpreter.h:61); brings JSCJSValue.h (EncodedJSValue).
#include "JSExportMacros.h"
#include "VMExceptionScopeVerificationState.h" // Obligation 10: per-lite EXCEPTION_SCOPE_VERIFICATION state (debug-only L2 tail append below).
#include "VMThreadContext.h" // §A.2.1 per-lite traps/stack limits (brings VMTraps.h; VMLite is only forward-declared there — no cycle).
#include "WriteBarrier.h" // AB-17 sort-scratch reroute: Group-3 m_cachedSortScratch slot type (no VM.h dependency).
#include <atomic>
#include <memory>
#include <type_traits>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafetyAnalysis.h>
#include <wtf/Vector.h>

namespace JSC {

class Allocator;
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

class VMLite;

// =============================================================================
// g_jscCurrentVMLite — the SOLE per-thread "installed VMLite" word (L4 backing
// store; M2-alloc-tax-residual (a) collapses the prior dual-store).
//
// DEFINED in runtime/VM.cpp (full rationale block there). This was originally
// the JIT/LLInt-visible MIRROR of a file-static `t_currentVMLite` in
// VMLite.cpp; alloctax2 attribution showed the out-of-line
// VMLite::currentIfExists() (JS_EXPORT_PRIVATE, reading the file-static via a
// general-dynamic TLS resolution) cost +0.913G cyc on the W=1 GIL-off pc-loop
// alone — every VM::group3Primitives() / trapsForCurrentThread() call paid a
// real call + __tls_get_addr. Promoting the mirror to the authoritative store
// and reading it from an ALWAYS_INLINE header accessor lets the compiler emit
// a single `movq %fs:@TPOFF` (Linux IE-TLS) at every call site, and removes
// the dual-write in setCurrent.
//
// L4 (§6.3, frozen): "plain C++ thread_local, NOT pthread_getspecific; accessor
// SIGNATURES frozen; backing store replaceable" — this IS the sanctioned
// backing-store replacement; currentIfExists()/current()/setCurrent()
// signatures are unchanged.
//
// COHERENCE: VMLite::setCurrent (VMLite.cpp) is still the SOLE writer. The
// JIT and LLInt bake this symbol's ELF TPOFF directly; GIL-off is refused at
// option validation on platforms without that (Mach-O TLV has no constant
// offset), see Options.cpp. extern "C" linkage so the offlineasm/JIT emitter
// and this header agree on the unmangled name regardless of the enclosing
// namespace.
// =============================================================================
#if OS(LINUX)
extern "C" __attribute__((tls_model("initial-exec"))) thread_local VMLite* g_jscCurrentVMLite;
#else
extern "C" thread_local VMLite* g_jscCurrentVMLite;
#endif

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
    v(void*, m_lastStackTop) \
    /* AB-17 annex (SPEC-ungil-history) sort-scratch reroute: the DFG/FTL */ \
    /* ArraySortCompact/Commit JSCellButterfly scratch cache. The compiled */ \
    /* code BAKES this slot's address (DFGSpeculativeJIT.cpp compileArray- */ \
    /* SortCompact/Commit; FTL twins), so GIL-off it MUST be per-lite: N */ \
    /* threads sharing one VM-resident slot hand the same 16-slot scratch */ \
    /* to concurrent sorts (tagged-garbage reads / silent wrong results — */ \
    /* JSTests/threads/dw1-sort-comparator-osr.js). L1 spec-revision */ \
    /* append (the sanctioned varargsLength/5c0e51c precedent): tail of */ \
    /* Group 3, nothing above moves; VM.h span assert updated in lockstep. */ \
    /* GC: the VM-block copy keeps its visitAggregateImpl append; gilOff */ \
    /* lites' copies are appended via the registry walk next to it. Owner- */ \
    /* thread-only mutation (the thread's own compiled sort code), so no */ \
    /* atomicity; marker reads race the owner's plain JIT stores exactly */ \
    /* like today's single-mutator concurrent-marking reads of the VM slot. */ \
    v(WriteBarrier<JSCell>, m_cachedSortScratch)

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
// (SPEC-ungil §A.1.6, ANNEX A16).
//
// GIL-off, scratchBufferForSize ADDRESSES baked into DFG/FTL code would be
// shared by N threads (and a buffer taken from the compiling thread's lite
// would die with that thread). Instead, every baked site — DFG and FTL node
// spills, OSR exit and entry buffers, the exit thunks — becomes
// `loadVMLite -> segment -> [index]`: codegen obtains a process-wide INDEX
// here (with an index->size map), and each VMLite holds an append-only
// segmented pointer table (lock-free reads, below). Indices are keyed by
// power-of-two size class, one per class, never freed: scratch use never
// nests on a thread (a site fills the buffer and consumes it before any other
// site runs), so sites compiled at different times may share one buffer per
// lite exactly as GIL-on sites share the VM's, and the index space is bounded
// by the number of size classes no matter how many compilations occur. A
// buffer must exist at (lite, index) BEFORE the compiled code can run:
// VM::allocateBakedScratchBufferIndex() fans the install to the VM's
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

    static constexpr unsigned numSizeClasses = sizeof(size_t) * 8; // class c serves sizes in (2^(c-1), 2^c]

    // Returns the index of size's power-of-two size class, allocating it on
    // first use; classSize receives the buffer size recorded for that index.
    JS_EXPORT_PRIVATE unsigned indexForSizeClass(size_t size, size_t& classSize);
    JS_EXPORT_PRIVATE size_t sizeForIndex(unsigned) const;
    JS_EXPORT_PRIVATE unsigned indexCount() const;

private:
    mutable Lock m_lock; // §LK rank: outside VMLiteRegistry::lock (see class comment).
    Vector<size_t> m_sizes WTF_GUARDED_BY_LOCK(m_lock); // index -> size; append-only.
    uint64_t m_sizeClassAllocated WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    unsigned m_sizeClassIndices[numSizeClasses] WTF_GUARDED_BY_LOCK(m_lock) { };
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
    // Group 4: the RegExp this thread is matching. GIL-off,
    // Yarr::MatchingContextHolder sets and clears the installed lite's slot;
    // flag-off/GIL-on it uses VM::m_executingRegExp. Regexp bytecode
    // compilation keeps the VM's BumpPointerAllocator under its own lock
    // (RegExp.cpp), so there is no per-lite allocator.
    RegExp* executingRegExp { nullptr };
    // Group 5: scratch buffers (§6.6). GIL-off, VM::scratchBufferForSize
    // dispatches to scratchBufferForSize below and baked DFG/FTL sites index
    // scratchSegments; scratchBuffers is the ownership list behind both (GC
    // root scan + teardown free). Flag-off/GIL-on: VM::m_scratchBuffers.
    Lock scratchBufferLock; // leaf rank (§7): fastMalloc only under it
    Vector<ScratchBuffer*> scratchBuffers;
    // Group 6: microtasks (§6.5), lazy. GIL-off, VM::queueMicrotask and
    // JSGlobalObject::queueMicrotask route a spawned or non-main carrier's
    // enqueues here; the main thread's carrier keeps the VM default queue.
    // Flag-off/GIL-on nothing routes here.
    RefPtr<MicrotaskQueue> defaultMicrotaskQueue;

    // L2 (§6.3): new fields append after Group 6 only.

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

    // H-VMLITE-TLCPTR (SPEC-heap §5.3/§B.4; GILOFF-TAX #1/#2): cached mirror of
    // this lite's GCThreadLocalCache flat table pointer + bound (table() /
    // tableBound()); this mirror is the only form generated code reads, one
    // lite-relative hop. Stamped at the §10A.1 client-slot
    // stamp (GCClient::Heap::setCurrentThreadClient — covers attach + the A36C
    // carrier swap; owner thread, I2), so per-tier inline-allocate emitters resolve
    // `tlcTable[tlcIndexBase_const + sizeClassIndex]` instead of baking the
    // null Allocator that allocatorForConcurrently returns under gilOff (the
    // IT-9 {} return) and falling to the lazy-slow-path generation thunk on
    // EVERY allocation (operationCompileFTLLazySlowPath: 46.6M GIL-off vs 53
    // GIL-on, intcs W=1). Same-thread program order (I11 + I2): the JIT reader
    // is the owner thread, so no cross-thread fences; bound is stamped LAST so
    // a bound-first reader never indexes past the published table (the table
    // is allocated at its lifetime capacity, §5.3). GIL-on/flag-off: never
    // read (every emitter is behind a vm.gilOff() codegen gate); zero-init.
    Allocator* tlcTable { nullptr };
    unsigned tlcTableBound { 0 };

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
    // stack (a shared chain was a deterministic stack-use-after-return in
    // ExceptionScope::stackPosition() on the spawned thread). GIL-on /
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

    // GIL-off, the soft reserved zone size is per thread like the soft stack
    // limit it feeds (VM::updateStackLimits): ErrorHandlingScope saves and
    // restores it around error creation on the overflowing thread only, so
    // two threads' scopes cannot interleave on a shared word. Stamped from the
    // VM word at registration (VMLiteRegistry::registerLite), owner-thread-only
    // afterwards (VM::currentSoftReservedZoneSizeSlot). GIL-on / flag-off:
    // never read (the VM word is authoritative).
    size_t softReservedZoneSize { 0 };

    // Open VM::DrainMicrotaskDelayScopes of this thread when its microtasks
    // live on defaultMicrotaskQueue above (GIL-off spawned threads and
    // non-main carriers): the scope defers only the owning thread's drains,
    // because nobody else can drain a per-lite queue and a deferred drain at
    // thread close would drop the tasks. Owner-thread-only (I11). The main
    // thread's carrier, GIL-on and flag-off count on the VM word.
    uint64_t drainMicrotaskDelayScopeCount { 0 };

    // B14 / MC-DOS S7 amendment note: a per-lite observedRetireEpoch slot was
    // proposed here as an "explicit per-LITE witness regardless of the
    // lite<->client mapping" for GCSafepointEpoch (SPEC-jit §4.4). Dropped at
    // adversarial review: bumpAndReclaim()'s min scan reads ONLY
    // GCClient::Heap::m_localEpoch (GCSafepointEpoch.cpp:148), so a per-lite
    // slot with no reader is dead code, and its in-stop stamping walk was a
    // new release-build lock-site (VMLiteRegistry::lock) inside the conductor
    // window for zero functional gain. The §4.4 soundness rests on U-T6
    // (per-thread clients: every JS-executing lite has its own GCClient::Heap
    // registered in clientSet()) — see epochHeapFor() in
    // RetiredJITArtifacts.cpp for the full argument and the standing
    // obligation on any future change that lets a lite execute without a
    // distinct registered client.

    static constexpr ptrdiff_t offsetOfPrimitives() { return OBJECT_OFFSETOF(VMLite, primitives); }
    static constexpr ptrdiff_t offsetOfTID() { return OBJECT_OFFSETOF(VMLite, tid); }
    static constexpr ptrdiff_t offsetOfGilOff() { return OBJECT_OFFSETOF(VMLite, gilOff); } // LLInt level-2 byte (U-T3).
    static constexpr ptrdiff_t offsetOfVM() { return OBJECT_OFFSETOF(VMLite, vm); } // Reader-side same-VM guard in the Group-3 host-call-return-value discriminators (LLIntThunks.cpp / LowLevelInterpreter64.asm).
    static constexpr ptrdiff_t offsetOfScratchSegments() { return OBJECT_OFFSETOF(VMLite, scratchSegments); } // A16 emission (U-T4).
    static constexpr ptrdiff_t offsetOfThreadContext() { return OBJECT_OFFSETOF(VMLite, threadContext); } // §A.2.1 chained-offset emission (U-T3/U-T4).
    static constexpr ptrdiff_t offsetOfTlcTable() { return OBJECT_OFFSETOF(VMLite, tlcTable); } // H-VMLITE-TLCPTR §B.4 inline-allocate emission.
    static constexpr ptrdiff_t offsetOfTlcTableBound() { return OBJECT_OFFSETOF(VMLite, tlcTableBound); }

    // TLS accessors (L4): backed by g_jscCurrentVMLite (see banner above the
    // class). Signatures frozen; the backing store is the L4-sanctioned
    // replaceable part. M2-alloc-tax-residual (a): ALWAYS_INLINE so the
    // ~7 VM.h hot selectors (group3Primitives / trapsForCurrentThread /
    // exceptionScopeVerificationState / currentThreadEntryScope / …) compile
    // to one IE-TLS load instead of an out-of-line call. Flag-off identity:
    // every hot caller is behind gilOffWithProcessGate(), so flag-off code
    // never reaches the read; the read itself is a plain TLS load with no
    // option dependency.
    ALWAYS_INLINE static VMLite* currentIfExists() { return g_jscCurrentVMLite; }
    ALWAYS_INLINE static VMLite& current() { ASSERT(g_jscCurrentVMLite); return *g_jscCurrentVMLite; }
    // Installs `lite` (may be null = uninstall) and returns the previous
    // value. Debug (I18/I20): asserts lite->tid != notTTLTID and that the
    // lite is registered. After the TLS write it invokes the TID-tag hook
    // (below) with `lite ? lite->tid : 0` — including for null — so the JIT's
    // per-thread butterfly TID tag stays coherent across §6.4.4
    // install/restore and multi-VM switches (jit CS3/I19).
    JS_EXPORT_PRIVATE static VMLite* setCurrent(VMLite*); // returns previous

    JS_EXPORT_PRIVATE VMLite();
    JS_EXPORT_PRIVATE ~VMLite(); // I20: asserts not installed here, not registered; poisons in debug.

    // Helper defined in VMLiteInlines.h (I11 owner asserts build on it).
    inline bool isInstalledOnCurrentThread() const;

    // ---- §6.5 Group 6: per-thread default microtask queue. GIL-off,
    // VM::queueMicrotask/drainMicrotasks and JSGlobalObject::queueMicrotask
    // route a spawned or non-main carrier's enqueues and drains here, while
    // the main thread's carrier keeps the VM default queue. Only the owning
    // thread enqueues or drains this queue (I11); a cross-thread enqueue goes
    // through MicrotaskQueue's foreign inbox, never here. ----
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

    // GIL-off Map and Set iteration (JSOrderedHashTableHelper): the entry the
    // last nextAndUpdateIterationEntry on this thread found, with its key and
    // value. With the GIL on, the table itself holds the entry, but a table is
    // shared by every thread that iterates it. The key and value are GC roots
    // (VM::visitAggregateImpl), because a delete on another thread can remove
    // them from the table while this thread still holds them here.
    uint32_t orderedHashTableIterationEntry { 0 };
    JSValue orderedHashTableIterationKey;
    JSValue orderedHashTableIterationValue;

    // GIL-off, a termination that only this thread services: the time limit
    // of a call that this thread made (VM::addTerminationDeadline) or
    // VM::notifyNeedTerminationForCurrentThread. It is set with this lite's
    // NeedTermination bit, under VMLiteRegistry::lock, and it routes the bit
    // when this thread services it (VMTraps::handleTraps).
    std::atomic<bool> targetedTerminationRequested { false };
    // Set when this thread services that termination. It stands in for the
    // VM's termination request on this thread (VM::hasTerminationRequest),
    // which handleTraps sets at the same point for a VM-wide termination.
    std::atomic<bool> targetedTerminationHandled { false };
};
static_assert(OBJECT_OFFSETOF(VMLite, primitives) == 0);

// =============================================================================
// §6.7 — currentButterflyTID()
//
// SOLE definition is in VMLite.cpp; runtime/ConcurrentButterfly.h and
// jit/ConcurrentButterflyOperations.cpp consume it through this header.
// Returns the installed carrier's tid, or 0 when no VMLite is installed (I18:
// main thread / embedder threads / GIL phase — a never-entering thread touches
// no JS objects, so 0 is unobservable there); never notTTLTID (0x7fff).
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

// Process-wide stop generation (VMLite.cpp). Every stop-the-world conductor
// that patches code bumps it after the patch; a mutator compares its
// per-thread copy before entering JIT code and issues a cross-modifying-code
// fence when it has fallen behind. The NVS exit variant fences
// unconditionally and refreshes the per-thread copy.
JS_EXPORT_PRIVATE void jsThreadsBumpStopGeneration();
JS_EXPORT_PRIVATE void jsThreadsSyncToStopGenerationBeforeJITEntry();
JS_EXPORT_PRIVATE void jsThreadsNVSExitInstructionSync();

} // namespace JSC
