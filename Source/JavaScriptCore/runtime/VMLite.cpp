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
#include "VMLite.h"

#include "Allocator.h"           // sizeof/triviality asserts on the TLC table element the emitters index.
#include "GCThreadLocalCache.h"  // table()/tableBound(): the source of the lite's tlcTable mirror.
#include "Heap.h"                // GCClient::Heap::currentThreadClient / threadLocalCache.
#include "MicrotaskQueue.h" // Complete type for ~RefPtr<MicrotaskQueue> in ~VMLite + create() (§6.5).
#include "VM.h"             // ScratchBuffer/VMMalloc (§6.6); currentThreadIsHoldingAPILock (I14).
#include "VMLiteInlines.h"  // isInstalledOnCurrentThread (I11 asserts below).
#include "VMLiteShared.h"   // VMLiteRegistry (debug registration asserts, I20).
#include <atomic>
#include <bit>
#include <mutex>
#include <wtf/Atomics.h> // crossModifyingCodeFence (ANNEX ISB1, U-T5).
#include <wtf/FastMalloc.h>
#include <wtf/HashSet.h> // K4 §VIII cross-thread-entry set (U-T8b).
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(VMLite);

// U-T8b forward declarations (definitions later in this TU / in
// JSGlobalObject.cpp; see the §K.3/LZ1 and K4 §VIII banners below).
void purgePerLiteRealmStateForLite(VMLite&); // Defined in JSGlobalObject.cpp (per-lite §K.1 realm duplicates).
void jsThreadsNoteCrossThreadEntry(VM&);

// L4 (frozen, SPEC-vmstate §6.3): plain C++ thread_local, NOT
// pthread_getspecific. The accessor signatures in VMLite.h are frozen; the
// backing store — g_jscCurrentVMLite, declared in VMLite.h and DEFINED in
// runtime/VM.cpp — is the L4-sanctioned replaceable part.
//
// M2-alloc-tax-residual (a): the prior file-static `t_currentVMLite` here is
// COLLAPSED onto g_jscCurrentVMLite (which had been its JIT-visible mirror).
// VMLite::setCurrent below remains the SOLE writer; currentIfExists()/
// current() are now ALWAYS_INLINE header reads of the same word, so C++ and
// generated code observe the identical slot by construction.
//
// Generated code reads g_jscCurrentVMLite only through its ELF initial-exec
// TPOFF (Linux x86-64/arm64), so the store below is the whole install
// protocol; GIL-off is refused at option validation on every other platform
// (Options.cpp), where the loadVMLite emitters fail-stop.
//
// Flag-off / GIL-on identity: the store is a plain TLS write on a path (lite
// install/uninstall) that flag-off code reaches only at JSLock acquire/
// release; no generated code reads the symbol except gilOff-mode compilations
// (§A.1.3 COMPILED-FOR-VM-mode rule), and the C++ hot readers are all behind
// gilOffWithProcessGate().

// §6.7 TID-tag hook (jit CS3/I19 provider). Null default — Phase-A standalone
// builds and flag-off runs never register one. Registration happens once at
// P5 init (jit task 1b); acquire/release keeps the registering thread's
// writes visible to hooks invoked on later-switching threads.
static std::atomic<void (*)(uint16_t)> s_vmLiteTIDTagHook { nullptr };

VMLite::VMLite() = default;

VMLite::~VMLite()
{
    // I20: no thread's TLS may ever point at a destroyed VMLite. We can only
    // check this thread's slot here; the registration assert below covers the
    // rest (an installed lite is always registered — setCurrent asserts it).
    ASSERT(g_jscCurrentVMLite != this);

    // UNGIL §K.1 ~VM/lite-teardown walk (U-T8b, K4 binding consequence 3):
    // EVERY lite teardown path — owner TLS destructor, EXIT1.9 walk-free,
    // deferred own-carrier detach, GIL-on ~VM — funnels through this dtor,
    // so freeing the per-lite §K.1 duplicates here makes the walk total: the
    // (global, lite)-keyed RegExpGlobalData/asyncContextData copies
    // (JSGlobalObject.cpp side table) die with their lite. Leaf-lock only;
    // entry destruction happens after release inside the callee. Safe for
    // never-entered/flag-off lites (empty-table scan).
    purgePerLiteRealmStateForLite(*this);

    // `vm` is never dereferenced here: a DETACHED carrier (and a live carrier
    // between its unregistration and this free) can be destroyed after ~VM
    // has returned, so the pointer may already dangle. VM-keyed bookkeeping
    // (the K4 §VIII cross-thread-entry note) is retired by ~VM itself.

    // §6.6: scratch buffers are VMMalloc'd raw blocks (mirrors ~VM,
    // VM.cpp:655-656). No lock: the lifetime contract (§6.5.1 — unregistered,
    // uninstalled) means no other thread can reach this carrier anymore.
    // Buffers installed at A16 baked indices are owned by this same list
    // (ensureScratchBufferAtIndex appends), so this loop frees them too.
    for (auto* scratchBuffer : scratchBuffers)
        VMMalloc::free(scratchBuffer);
    scratchBuffers.clear();

    // A16: free the segment arrays (the buffers they pointed at were freed
    // above via the ownership list).
    for (auto& segmentSlot : scratchSegments) {
        if (auto* segment = segmentSlot.load(std::memory_order_relaxed)) {
            segmentSlot.store(nullptr, std::memory_order_relaxed);
            fastFree(segment);
        }
    }
#if ASSERT_ENABLED
    {
        // Lifetime contract (§6.5.1): unregister BEFORE destroy. Leaf lock —
        // nothing else is acquired while it is held.
        auto& registry = VMLiteRegistry::singleton();
        Locker locker { registry.lock };
        ASSERT(!registry.lites.contains(this));
    }
    // Poison (I20 debug): a stale g_jscCurrentVMLite or VMLite* on another
    // thread that dereferences this carrier after destruction trips on
    // obviously-bad values instead of reading freed-but-plausible state.
    vm = reinterpret_cast<VM*>(static_cast<uintptr_t>(0xbbadbeef));
    tid = 0xffff; // Not a valid ButterflyTID payload (15-bit space).
    executingRegExp = reinterpret_cast<RegExp*>(static_cast<uintptr_t>(0xbbadbeef));
#endif
}

// VMLite::currentIfExists() / VMLite::current() are ALWAYS_INLINE in VMLite.h
// (M2-alloc-tax-residual (a)); no out-of-line bodies.

VMLite* VMLite::setCurrent(VMLite* lite)
{
    if (lite) {
        // I18: an installed carrier's tid is never notTTLTID (0x7fff — the
        // all-ones 15-bit TID is the segmented-butterfly sentinel,
        // ConcurrentButterfly.h; not includable here: it includes us).
        ASSERT(lite->tid != 0x7fff);
#if ASSERT_ENABLED
        {
            // I20: only live, registered lites may be installed (§6.5.1:
            // registerLite precedes setCurrent — VM ctor registers the main
            // carrier before JSLock installs it; api §5.2 spawn registers
            // before the first JSLockHolder). Leaf lock.
            auto& registry = VMLiteRegistry::singleton();
            Locker locker { registry.lock };
            ASSERT(registry.lites.contains(lite));
        }
#endif
    }

    // This store IS the slot the loadVMLite emitters read (ELF TPOFF), so
    // every install and uninstall (including null) goes through here.
    VMLite* previous = g_jscCurrentVMLite;
    g_jscCurrentVMLite = lite;

    // §6.7: invoke the TID-tag hook AFTER the TLS write, with the new tid (0
    // for uninstall) — §6.4.4 install/restore and multi-VM switches keep
    // g_jscButterflyTIDTag coherent (jit I19). Null hook => no-op.
    if (auto* hook = s_vmLiteTIDTagHook.load(std::memory_order_acquire))
        hook(lite ? lite->tid : 0);

    // UNGIL annex K4 §VIII (U-T8b): note the owning VM's FIRST cross-thread
    // entry — any gilOff install of a lite other than the VM's main carrier
    // (spawned thread or foreign embedder carrier). setCurrent is the single
    // choke point every install passes, so this cannot be bypassed. Cost:
    // one byte test flag-off/GIL-on (lite->gilOff == 0 => skipped entirely);
    // gilOff installs pay one relaxed single-slot compare once noted.
    //
    // MAIN-CARRIER KEY (GIL-removal review round 4, same re-key as
    // VM::queueMicrotask / perLiteRealmRoutingLite — AB-23): GIL-off,
    // m_mainVMLite is NEVER installed (A36 — the main thread gets a
    // per-(thread,VM) carrier too), so `lite != mainVMLite()` alone noted
    // the MAIN THREAD's own first install as a "cross-thread entry" and the
    // K4 §VIII.9 immutable-after-init asserts (setGlobalThis/setName) fired
    // on single-threaded gilOff boot. The main thread's carrier
    // (ownerHasNoTlsDtor, A36 r32) is the gilOff main carrier and does not
    // count; spawned lites and non-main embedder carriers do. (A VM used
    // ONLY from non-main threads over-notes on its own first install —
    // debug-only over-strictness, recorded under AB-23's residual.)
    if (lite && lite->gilOff && lite->vm && lite != lite->vm->mainVMLite() && !lite->ownerHasNoTlsDtor) [[unlikely]]
        jsThreadsNoteCrossThreadEntry(*lite->vm);

    return previous;
}

// ---- §6.5 Group 6: per-thread default microtask queue. GIL-off,
// VM::queueMicrotask / VM::drainMicrotasks and
// JSGlobalObject::queueMicrotask[Slow] route a spawned or non-main carrier's
// microtasks here; flag-off/GIL-on nothing routes here, byte-identically. -----

MicrotaskQueue& VMLite::ensureDefaultMicrotaskQueue()
{
    // I11: a per-thread facility is touched only by the thread the carrier is
    // installed on.
    ASSERT(isInstalledOnCurrentThread());
    // §6.5.1: registerLite ran (sole writer of `vm`) before this carrier could
    // be installed, so `vm` is non-null and immutable here.
    RELEASE_ASSERT(vm);
    // I14: the registration side effect below (MicrotaskQueue's constructor
    // appends to VM::m_microtaskQueues, M12-locked) plus everything a queue is
    // for requires the owner to hold this VM's JSLock.
    ASSERT(vm->currentThreadIsHoldingAPILock());

    if (!defaultMicrotaskQueue) [[unlikely]]
        defaultMicrotaskQueue = MicrotaskQueue::create(*vm);
    return *defaultMicrotaskQueue;
}

// ---- §6.6 Group 5: per-thread scratch buffers. GIL-off,
// VM::scratchBufferForSize dispatches here for the non-baked (C++ slow path)
// requests of the installed thread. ----------------------------------------
//
// ANNEX A16 NON-BAKED ARM: VMLite::scratchBufferForSize(size_t) is implemented
// over the segmented table by size-class index — NOT by transplanting
// VM::scratchBufferForSize's `scratchBuffers.last()` geometric series with a
// high-water size. That transplant is memory-UNSAFE here: `scratchBuffers` is
// the lite's buffer-OWNERSHIP list, and ensureScratchBufferAtIndex appends
// baked-index buffers to it too — including from OTHER threads via
// VM::allocateBakedScratchBufferIndex's install fan and the registration
// backfill — so `.last()` can be an arbitrarily small baked buffer that a
// high-water check never re-validates (undersized return => caller heap
// overflow). The ownership list must never be a lookup structure.
//
// Size classes are powers of two (class c serves sizes in (2^(c-1), 2^c],
// buffer size 2^c): at most one buffer per class per lite, total per-lite
// non-baked footprint <= 2x the largest request — the same geometric-series
// memory bound VM's policy targets. Each class is ONE process-wide
// ScratchBufferRegistry index (ScratchBufferRegistry::indexForSizeClass, the
// same index the baked arm bakes), so the per-lite storage is the ordinary
// A16 segmented table: registration backfill pre-installs the classes other
// lites already use, and the two-load read below serves repeats lock-free.

// ---- GIL-off inline-allocation addressing (SPEC-heap §5.3 / UNGIL §B.4).
// The inline-allocate emitters (AssemblyHelpers::emitLoadTLCAllocatorForSlot,
// FTL tlcAllocatorForSlot) read `lite->tlcTable[slot]` guarded by
// `slot < lite->tlcTableBound`; that pair is the {table, bound} of the
// GCThreadLocalCache of the client this thread allocates through, stamped on
// the installed lite by GCClient::Heap::setCurrentThreadClient (the same
// site that publishes the §10A.1 TLS client slot). Both words are written
// only by the owner thread and read only by code running on it, so plain
// loads observe program order; a slot past the bound or a null word is a
// branch to the slow path, never a wild load. ----

// Emission stride contract: table[slot] is one pointer-sized, trivially
// copyable word (Allocator wraps exactly one LocalAllocator*; a null word is
// the "no allocator yet" slow-path sentinel the emitted null-check tests).
static_assert(sizeof(Allocator) == sizeof(LocalAllocator*));
static_assert(sizeof(Allocator) == sizeof(void*));
static_assert(std::is_trivially_copyable_v<Allocator>);

// Per-call witness on this TU's gilOff slow path: the client generated code
// allocates into (lite->clientHeap, via the tlcTable/tlcTableBound mirror)
// must be the client the C++ slow paths resolve through the §10A.1 TLS slot,
// and the mirror must be that client's own {table, bound}. Every carrier
// install and A36C restore re-stamps both; a divergence is a bug in that
// protocol, so this is debug-only rather than a release assert.
static ALWAYS_INLINE void assertVMLiteClientCoherence(VMLite& lite)
{
#if ASSERT_ENABLED
    if (lite.gilOff && lite.clientHeap) {
        ASSERT(GCClient::Heap::currentThreadClient() == lite.clientHeap);
        ASSERT(lite.tlcTable == lite.clientHeap->threadLocalCache().table());
        ASSERT(lite.tlcTableBound == lite.clientHeap->threadLocalCache().tableBound());
    }
#else
    UNUSED_PARAM(lite);
#endif
}

ScratchBuffer* VMLite::scratchBufferForSize(size_t size)
{
    if (!size)
        return nullptr;

    ASSERT(isInstalledOnCurrentThread()); // I11.

    // This buffer is owned by the installed thread and freed with its lite,
    // so it can be handed out only to a mutator that consumes it on this
    // thread; code generation (including a synchronous compile on the mutator)
    // must bake a registry index instead (VM::allocateBakedScratchBufferIndex).
    RELEASE_ASSERT(!isCompilationThread());

    // Debug-only and gilOff-gated inside; flag-off never reaches this
    // function (VM::scratchBufferForSize dispatches here only under m_gilOff
    // with a same-VM installed lite).
    assertVMLiteClientCoherence(*this);

    size_t classSize;
    unsigned index = ScratchBufferRegistry::singleton().indexForSizeClass(size, classSize);

    // Fast path: the two-load lock-free read (repeat requests in this class,
    // or a class another lite already claimed that our registration backfill
    // / a racing install fan populated).
    if (ScratchBuffer* buffer = scratchBufferAtIndex(index))
        return buffer;

    // Slow path: idempotent install under scratchBufferLock; appends to the
    // `scratchBuffers` ownership list (GC scan + teardown free), exactly
    // like the baked arm.
    ensureScratchBufferAtIndex(index, classSize);
    ScratchBuffer* buffer = scratchBufferAtIndex(index);
    RELEASE_ASSERT(buffer);
    return buffer;
}

void VMLite::clearScratchBuffers()
{
    ASSERT(isInstalledOnCurrentThread()); // I11.
    Locker locker { scratchBufferLock };
    for (auto* scratchBuffer : scratchBuffers)
        scratchBuffer->setActiveLength(0);
}

// §6.7: the sole definition of currentButterflyTID().
void setVMLiteTIDTagHook(void (*hook)(uint16_t))
{
    s_vmLiteTIDTagHook.store(hook, std::memory_order_release);
}

// ---- ANNEX A16 (UNGIL §A.1.6): process-wide baked-index registry + per-lite
// segmented table. GIL-off compilations (DFG/FTL node spills, OSR exit and
// entry buffers, the exit thunks) bake a registry index and load the buffer
// through the installed lite (loadVMLite -> scratchSegments -> [index]).
// VM::allocateBakedScratchBufferIndex fans the new index's buffer to the
// VM's registered lites, and every GIL-off registration backfills (the
// JSLock.cpp carrier registration and the ThreadObject.cpp spawn), so a
// buffer exists at (lite, index) before code baking that index can run on
// the thread. Flag-off and GIL-on compilations keep baking the addresses
// VM::scratchBufferForSize hands out. The VM-singular caches this annex once
// planned to duplicate per lite are handled elsewhere: MegamorphicCache is
// inert under useJSThreads, the HasOwnPropertyCache is bypassed GIL-off
// (ObjectPrototype.cpp) and RegExpGlobalData has a per-lite side table in
// JSGlobalObject.cpp. ----

ScratchBufferRegistry& ScratchBufferRegistry::singleton()
{
    static LazyNeverDestroyed<ScratchBufferRegistry> registry;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        registry.construct();
    });
    return registry;
}

unsigned ScratchBufferRegistry::indexForSizeClass(size_t size, size_t& classSize)
{
    // bit_ceil of a size above 2^63 is unrepresentable (UB); no plausible
    // scratch request approaches it, so fail-stop first.
    RELEASE_ASSERT(size && size <= (static_cast<size_t>(1) << (numSizeClasses - 1)));
    classSize = std::bit_ceil(size); // Smallest power of two >= size.
    unsigned sizeClass = std::countr_zero(classSize);
    static_assert(numSizeClasses <= VMLite::maxScratchSegments * VMLite::scratchSegmentSize);

    Locker locker { m_lock };
    if (!(m_sizeClassAllocated & (1ull << sizeClass))) {
        m_sizeClassIndices[sizeClass] = m_sizes.size();
        m_sizes.append(classSize);
        m_sizeClassAllocated |= 1ull << sizeClass;
    }
    return m_sizeClassIndices[sizeClass];
}

size_t ScratchBufferRegistry::sizeForIndex(unsigned index) const
{
    Locker locker { m_lock };
    return m_sizes[index];
}

unsigned ScratchBufferRegistry::indexCount() const
{
    Locker locker { m_lock };
    return m_sizes.size();
}

void VMLite::ensureScratchBufferAtIndex(unsigned index, size_t size)
{
    ASSERT(index < maxScratchSegments * scratchSegmentSize);
    // scratchBufferLock is acquired under VMLiteRegistry::lock by the
    // VM-side install fan — legal per the §LK.6 re-rank (see
    // ScratchBufferRegistry's class comment). fastMalloc/VMMalloc only under
    // it, as before.
    Locker locker { scratchBufferLock };

    auto& segmentSlot = scratchSegments[index >> scratchSegmentShift];
    auto* segment = segmentSlot.load(std::memory_order_relaxed);
    if (!segment) {
        segment = static_cast<std::atomic<ScratchBuffer*>*>(
            fastZeroedMalloc(scratchSegmentSize * sizeof(std::atomic<ScratchBuffer*>)));
        // Release-publish the zeroed segment for the lock-free readers.
        segmentSlot.store(segment, std::memory_order_release);
    }

    auto& entry = segment[index & (scratchSegmentSize - 1)];
    if (entry.load(std::memory_order_relaxed))
        return; // Idempotent: a racing fan/backfill already installed it.

    ScratchBuffer* buffer = ScratchBuffer::create(size);
    RELEASE_ASSERT(buffer);
    // Ownership list FIRST (the repurposed Group 5: backs the jit-R2 GC scan
    // and the dtor free above), then the release-publish readers load from.
    scratchBuffers.append(buffer);
    entry.store(buffer, std::memory_order_release);
}

void VMLite::backfillBakedScratchBuffers()
{
    auto& registry = ScratchBufferRegistry::singleton();
    unsigned count = registry.indexCount();
    for (unsigned index = 0; index < count; ++index)
        ensureScratchBufferAtIndex(index, registry.sizeForIndex(index));
}

// ---- UNGIL §A.2.1 per-lite traps seam (U-T2; AB-17 item 2). ---------------
//
// The §A.2.1 contract appends (L2, after Group 6) `VMThreadContext
// threadContext` to VMLite, giving every thread its own VMTraps (trap word +
// StackManager stack limits) that generated code reaches via the chained
// offset lite->threadContext.traps().m_trapBits. The VMLite.h member append
// is LANDED (AB-17 item 1), and the return below is FLIPPED for gilOff lites:
//   - rule-3 fan-outs (VMTraps::fireTrapVMWide) and the token-acquisition OR
//     (orVMWideTrapBitsIntoLite) are pointer-identity-keyed, so they de-alias
//     automatically and now write each gilOff lite's OWN word;
//   - per-lite readers (D9 park-lite polls, the W1 captured-lite poll) now
//     observe their own per-thread word.
//
// Every lite of a gilOff VM has its gilOff byte set at registration (VM ctor,
// JSLock carrier registration, ThreadObject spawn), so for such a VM this
// never returns the VM word: generated-code soft-stack-limit reads, the C++
// softStackLimitForCurrentThread readers, the registration backfill and the
// park-site polls all go through the lite's own word, and nothing refuses a
// second concurrent entry.

VMTraps* perThreadTrapsIfExists(VMLite& lite)
{
    // Unregistered/poisoned lites carry no usable VM; callers only walk
    // registered lites (under the registry lock), so this is belt-and-
    // suspenders for pre-registration probes.
    if (!lite.vm)
        return nullptr;
    // §A.2.1 ACTIVE: gilOff lites carry their own trap word + StackManager.
    // GIL-on lites (a second GIL-on VM in a gilOffProcess — U0b) keep the
    // VM-word alias; the rule-3 fan-out and TERM1.2 interim branches key on
    // pointer identity, so they de-alias automatically per-lite.
    if (lite.gilOff)
        return &lite.threadContext.traps();
    return &lite.vm->traps();
}

// ---- UNGIL §A.3.2c (ANNEX ISB1, U-T5): stop-generation counter +
// per-thread context-sync on non-NVS JIT re-entry. ----
//
// ISB1.1 state: one process-wide seq_cst uint64 stop-generation counter.
// EVERY §A.3 conductor (and every heap §10 conductor that patched/jettisoned
// code — the cheap conservative form bumps for every conductor) increments it
// INSIDE the stop window, before resume. Both delivery sites are landed
// (U-T5): the §A.3 conductor in VMManager.cpp calls
// jsThreadsBumpStopGeneration() between the patcher-side
// crossModifyingCodeFence and the stop-word clear, and the §10 shared-GC
// conductor (Heap::conductSharedCollection, gilOff-process only) calls it
// between its crossModifyingCodeFence and the seq_cst GSP clear — the
// re-acquirer's seq_cst F8 GSP load carries the publishing edge there.
//
// ISB1.2 consumption: every transition into "may execute JIT code" that did
// NOT pass through an NVS exit — F8 AHA re-acquisition (including the
// §A.3.2b bit-already-clear path), §F token acquisition and ACT (both funnel
// through GCClient::Heap::acquireHeapAccess, which calls the sync below on
// its success path), the DAL2 dtor and the §F.5 LIFO restore (both re-enter
// through the same AHA) — loads the counter, compares the per-THREAD copy,
// and on mismatch executes a context-synchronizing instruction
// (WTF::crossModifyingCodeFence: ISB on arm64, a serializing instruction on
// x86-64) BEFORE any JIT-code entry, then stores the new value. NVS exit
// keeps the unconditional R1.d ISB and ALSO refreshes the copy
// (jsThreadsNVSExitInstructionSync, called by the notifyVMStop/ticket-park
// exits in VMManager.cpp).
//
// DEVIATION RECORDED (spec letter vs storage): ISB1.1 says "per-lite uint64
// copy (L2 append)". The L2 append lives in VMLite.h, which is OUTSIDE
// U-T5's writable file set, so the copy is a thread_local here instead. This
// is a strict refinement, not a weakening: the guarantee ISB1 carries is
// per-CPU-THREAD instruction-stream synchronization (an ISB synchronizes the
// executing PE, not a carrier), and a thread that has synced for generation
// G has synced for ALL its carriers at G — a per-lite copy would only force
// redundant extra ISBs on multi-carrier threads. Lift into VMLite.h (L2
// append + offsetOf accessor) if a JIT-inlined fast path ever wants it.
//
// ISB1.5 cost: GIL-on/flag-off zero (the counter never bumps — only gilOff
// §A.3 conductors call the bump — and the compare sits on gilOff-only
// paths). GIL-off steady state: one relaxed load + compare per access/token
// transition; the seq_cst bump is conductor-only. Visibility of the bump to
// re-acquirers needs no seq_cst load here: the bump is sequenced before the
// conductor's seq_cst stop-word CLEAR, and a re-acquirer only reaches JIT
// code after its seq_cst stop-word load observes that clear (the §A.3.2b
// gate), which carries the synchronizes-with edge.

static std::atomic<uint64_t> s_jsThreadsStopGeneration { 1 };
static thread_local uint64_t t_jsThreadsStopGenerationSeen { 0 };

void jsThreadsBumpStopGeneration()
{
    s_jsThreadsStopGeneration.fetch_add(1, std::memory_order_seq_cst);
}

void jsThreadsSyncToStopGenerationBeforeJITEntry()
{
    uint64_t generation = s_jsThreadsStopGeneration.load(std::memory_order_relaxed); // ISB1.5: relaxed + compare.
    if (generation != t_jsThreadsStopGenerationSeen) [[unlikely]] {
        WTF::crossModifyingCodeFence(); // arm64 ISB / x86-64 serializing instruction, BEFORE any JIT entry.
        t_jsThreadsStopGenerationSeen = generation;
    }
}

void jsThreadsNVSExitInstructionSync()
{
    // R1.d: every mutator leaving an NVS park executes an ISB
    // unconditionally; ISB1.2: the NVS exit also refreshes the per-thread
    // copy. ORDER IS LOAD-BEARING: sample the generation BEFORE the fence
    // and record that pre-fence value. Recording a post-fence sample could
    // mark a bump that landed between the fence and the load as "synced"
    // although no ISB ran after its window's patch; with the pre-fence
    // sample any such bump stays unrecorded and the next JIT-entry compare
    // (jsThreadsSyncToStopGenerationBeforeJITEntry) issues the ISB. The
    // conductor's patch is sequenced before its bump, so an observed value
    // is always covered by the fence below.
    uint64_t generation = s_jsThreadsStopGeneration.load(std::memory_order_relaxed);
    WTF::crossModifyingCodeFence();
    t_jsThreadsStopGenerationSeen = generation;
}

// ===========================================================================
// UNGIL annex K4 §VIII (U-T8b) — the shared
// no-write-after-first-cross-thread-entry assert machinery.
//
// Every K4 §VIII immutable-after-init row (VM structure roots, sentinels,
// propertyNames, smallStrings, embedder hooks VIII.8, JSGlobalObject
// configuration VIII.9, ...) gets a debug assert that no write happens after
// the owning VM's FIRST cross-thread entry. "First cross-thread entry" is
// noted HERE, in VMLite::setCurrent — the single choke point every gilOff
// install passes — when a gilOff lite other than the VM's main carrier is
// installed (spawned threads AND foreign embedder carriers).
//
// The assert call is jsThreadsAssertNoWriteAfterFirstCrossThreadEntry(&vm):
// release builds and flag-off/GIL-on VMs are exact no-ops (the predicate is
// vm.m_gilOff). The "one shared macro" wrapper the annex names —
//   #define JSC_ASSERT_NO_WRITE_AFTER_FIRST_CROSS_THREAD_ENTRY(vm) \
//       jsThreadsAssertNoWriteAfterFirstCrossThreadEntry(vm)
// — must live in a header beside the VIII-row setters (VM.h /
// JSGlobalObject.h), both OUTSIDE this task's owned set: recorded OPEN for
// their owners. Wired NOW in owned code: JSGlobalObject::setGlobalThis
// (VIII.9, JSGlobalObject.cpp). VIII.8's VM.cpp setters (:1091-1094) are the
// VM.cpp owner's wiring row.
//
// §VIII.9 RE-SCOPE (A-t8assert, 2026-06-12; K4 row amended in the same
// change): the §VIII.9 invariant for JSGlobalObject configuration is
// per-GLOBAL — "immutable once THIS global is observable to other threads" —
// not per-VM. Keying solely on the VM's first cross-thread entry is
// over-coarse for the one setter with a legitimate post-entry init write:
// a spawned thread creating a brand-new JSGlobalObject
// ($vm.createGlobalObject -> finishCreation -> setGlobalThis) init-writes
// m_globalThis on a global nobody else can have observed yet (create()
// has not returned; the only writers of the slot are finishCreation and
// resetPrototype). The slot itself is the per-global init bit: m_globalThis
// is null EXACTLY during finishCreation's first write and non-null for
// every later write. jsThreadsAssertNoPostInitWriteAfterFirstCrossThreadEntry
// below takes that per-global init state: init writes (null slot) are
// permitted regardless of VM cross-thread history; POST-INIT rewrites
// (resetPrototype — the global may be published) keep the full fail-stop.
// Protective power is NOT weakened: every write the old key correctly
// forbade (post-publication cross-thread rewrite of a non-null slot) still
// fail-stops; only the false-positive (init write on an unpublished global
// after the VM went cross-thread) is removed. Setters with NO per-global
// init key (setName & the other VIII.9 embedder setters) keep the
// VM-keyed assert unchanged (debug-only over-strictness, AB-23-residual
// class).
// ===========================================================================

static Lock s_crossThreadEntryLock;
static std::atomic<VM*> s_lastNotedCrossThreadVM { nullptr }; // single-slot fast path (U0b: one gilOff VM)

static HashSet<VM*>& crossThreadEnteredVMs() WTF_REQUIRES_LOCK(s_crossThreadEntryLock)
{
    static NeverDestroyed<HashSet<VM*>> set;
    return set;
}

// Self-declarations (macro/header lift recorded OPEN above; JSGlobalObject.cpp
// self-declares the assert form identically).
void jsThreadsNoteCrossThreadEntry(VM&);
bool jsThreadsHasSeenCrossThreadEntry(VM&);
void jsThreadsForgetCrossThreadEntry(VM&);
void jsThreadsAssertNoWriteAfterFirstCrossThreadEntry(VM*);
void jsThreadsAssertNoPostInitWriteAfterFirstCrossThreadEntry(VM*, bool isPerGlobalInitWrite);

void jsThreadsNoteCrossThreadEntry(VM& vm)
{
    if (s_lastNotedCrossThreadVM.load(std::memory_order_relaxed) == &vm)
        return; // Already noted; install fast path.
    Locker locker { s_crossThreadEntryLock };
    crossThreadEnteredVMs().add(&vm);
    s_lastNotedCrossThreadVM.store(&vm, std::memory_order_relaxed);
}

bool jsThreadsHasSeenCrossThreadEntry(VM& vm)
{
    if (s_lastNotedCrossThreadVM.load(std::memory_order_relaxed) == &vm)
        return true;
    Locker locker { s_crossThreadEntryLock };
    return crossThreadEnteredVMs().contains(&vm);
}

// Bookkeeping at VM death (address reuse must not leave a stale positive);
// called from ~VM once every foreign lite of the VM has been unregistered.
// Lites never call this: a carrier can outlive its VM's destruction, so
// ~VMLite must not dereference its `vm`.
void jsThreadsForgetCrossThreadEntry(VM& vm)
{
    Locker locker { s_crossThreadEntryLock };
    crossThreadEnteredVMs().remove(&vm);
    VM* expected = &vm;
    s_lastNotedCrossThreadVM.compare_exchange_strong(expected, nullptr, std::memory_order_relaxed);
}

void jsThreadsAssertNoWriteAfterFirstCrossThreadEntry(VM* vm)
{
#if ASSERT_ENABLED
    if (vm && vm->gilOff())
        ASSERT(!jsThreadsHasSeenCrossThreadEntry(*vm));
#else
    UNUSED_PARAM(vm);
#endif
}

// §VIII.9 re-scope entry point (see the RE-SCOPE paragraph in the banner
// above): per-global init writes are permitted; post-init writes get the
// unchanged VM-keyed fail-stop. isPerGlobalInitWrite MUST be derived from
// per-global state that is true ONLY while the global is unpublishable
// (e.g. setGlobalThis passes !m_globalThis — null exactly during
// finishCreation's first write). Release builds and flag-off/GIL-on VMs
// remain exact no-ops via the wrapped predicate.
void jsThreadsAssertNoPostInitWriteAfterFirstCrossThreadEntry(VM* vm, bool isPerGlobalInitWrite)
{
    if (!isPerGlobalInitWrite)
        jsThreadsAssertNoWriteAfterFirstCrossThreadEntry(vm);
}

// ===========================================================================
// UNGIL U-T8b CONSUMPTION RECORD — annexes K4 + N7 (doc-of-record block;
// SPEC-ungil-audit-K4.md / -N7.md stay the BINDING tables).
//
// §F.2 CONSUMER-ROW CITATIONS (K4 binding consequence 1): every §F.2
// EXCLUSIVITY consumer must cite its K4 row. Rows landed by THIS task:
//   - K4 §0 U2 / AUD1.K2 (m_regExpGlobalData, SD19) + ALS1.3
//     (m_asyncContextData): per-lite side table + accessors + registry-walk
//     rooting + teardown purge — JSGlobalObject.cpp (threadRegExpGlobalData /
//     threadAsyncContextData / purgePerLiteRealmStateForLite).
//   - N7 RESOLVED-2 / AUD1.N2 (RegExp::m_ovector): per-thread match scratch
//     provider + gilOff aliasing fail-stops — RegExp.cpp.
//   - N7 RESOLVED-3 / AUD1.N3 (modified-arguments bitmap): release-CAS
//     publish + acquire readers — GenericArgumentsImplInlines.h.
//   - N7 RESOLVED-5 / AUD1.N4 (StructureRareData caches): §K.3 CAS-publish
//     of m_specialPropertyCache + Structure::m_lock'd entry installs —
//     StructureRareData.cpp.
//   - K4 §VIII: the no-write-after-first-cross-thread-entry machinery above
//     (noted in setCurrent; wired at JSGlobalObject::setGlobalThis AND
//     JSGlobalObject::setName — the two VIII.9 post-init setters in this
//     task's owned files; the remaining VIII.9 setters
//     (setEvalEnabled/setWebAssemblyEnabled/quirks/disabled-error messages,
//     JSGlobalObject.h:1249-1255) and the VIII.8 embedder-hook setters
//     (VM.h:1091-1094) live in unowned headers — recorded for their owning
//     slices).
//   - §K.3/LZ1/LZ2: the owner/waiter tables, cycle walk, bounded park and
//     scope-exit abandonment live in LazyPropertyInlines.h (keyed by
//     WTF::Thread*); this TU keeps no lazy-init state.
// Rows ruled by K4/N7 but whose code sites are OUTSIDE this task's owned
// files keep their owners (K4 §II per-lite VM caches = VM.h/VM.cpp rows;
// K4 §III leaf locks = their cache TUs; K4 §V main-only gating = option
// validation/U-T14; K4 §VI class-4 = U-T13; N7 RESOLVED-4 =
// ScopedArguments/ClonedArguments TUs; RESOLVED-6 Intl = IntlObject TUs).
//
// HARD U-T9 ENTRY GATES recorded by this task (NOT mere open items — each
// is a normative U-T8b clause whose code site is outside this file set; no
// landable-now alternative exists inside it; U-T9 MUST NOT start until the
// orchestrator charters each slice):
//   GATE-1 — N7 RESOLVED-1/AUD1.N1: AbstractModuleRecord::m_resolutionCache
//     cell lock: LANDED. cacheResolution()/tryGetCachedResolution() take the
//     record's cellLock(), the lock the sibling maps on the cell already use.
//   GATE-2 — N7 RESOLVED-2/AUD1.N2 routing half: LANDED. RegExp.h
//     ovectorSpan(VM&) gilOff-reroutes to the per-thread buffer (inline
//     definition RegExpInlines.h; consumers RegExpGlobalDataInlines.h /
//     RegExpMatchesArray.h / StringPrototypeInlines.h re-pointed;
//     offsetVectorSize() ruled no-reroute — size immutable post-publication,
//     RegExp.h comment). The RELEASE_ASSERTs in RegExp::match /
//     matchConcurrently are KEPT as routing invariants per the RegExp.cpp
//     banner; the SD19 regexp corpus arms can now run.
//   GATE-3 — §K.3/LZ1/LZ2 consuming slice: LANDED in LazyPropertyInlines.h
//     (LazyPropertyInternal::InitTables + callFunc: the initializing CAS
//     records the owner, foreign touchers park in bounded quanta, the
//     cycle walk returns null, abandonment runs at scope exit).
//
// WS1.4 HANDLE-CREATION LOCK-CONTEXT COLUMN (Weak/Strong construction sites
// in THIS task's owned files — the audit column; WS(i): no Weak construction
// under api rank-1..3 or §LK.7 leaves):
//   - VMLite.cpp / RegExp.cpp / RegExpCachedResult.h /
//     GenericArgumentsImplInlines.h / StructureRareData.cpp: ZERO Weak or
//     Strong constructions (verified: no Weak<>/Strong<>/JSWeakValue use).
//   - JSGlobalObject.cpp (this task's additions): ZERO Weak/Strong
//     constructions; the per-lite side table holds WriteBarriers (no
//     handles) and allocates cells OUTSIDE its leaf lock (WS1(i)-conforming
//     by construction).
// WS1.2 RE-SHAPES:
//   - RegExpCache::lookupOrCreate: DONE — the Weak<RegExp> is constructed
//     before Locker { m_lock } and weakAdd'd under it.
//   - ThreadManager::restrictObject: OPEN — makeAffinityEntry constructs the
//     entry's Weak<JSObject> under m_affinityLock (api rank 2) on both the
//     ensure and stale-replace arms; under the shared heap that construction
//     takes MutatorSlowPathLocker (WeakSetInlines.h), the nesting WS(i)
//     forbids. Re-shape: build the entry before the lock, move/replace
//     under it, destroy a stale entry after release.
// WS1.5 churn corpus (restrict/collect + regexp-cache churn, TSAN): JSTests
// is outside this file set — recorded for the orchestrator with the WS1.2
// re-shapes (they gate together).
// ===========================================================================

// ---- UNGIL §A.3.6/ANNEX A36 carrier-TID hooks (U-T1). ----

static std::atomic<uint16_t (*)()> s_allocateCarrierTIDHook { nullptr };
static std::atomic<void (*)(uint16_t)> s_releaseCarrierTIDHook { nullptr };

void setCarrierTIDHooks(uint16_t (*allocate)(), void (*release)(uint16_t))
{
    s_allocateCarrierTIDHook.store(allocate, std::memory_order_release);
    s_releaseCarrierTIDHook.store(release, std::memory_order_release);
}

uint16_t allocateCarrierTID()
{
    auto* hook = s_allocateCarrierTIDHook.load(std::memory_order_acquire);
    RELEASE_ASSERT_WITH_MESSAGE(hook,
        "GIL-off carrier registration requires the ThreadManager carrier-TID provider (UNGIL annex A36; INTEGRATE-ungil.md)");
    uint16_t tid = hook();
    RELEASE_ASSERT(tid && tid != 0x7fff); // never tag 0 or notTTLTID (A36/TTL).
    return tid;
}

void releaseCarrierTIDIfHooked(uint16_t tid)
{
    if (auto* hook = s_releaseCarrierTIDHook.load(std::memory_order_acquire))
        hook(tid);
}

} // namespace JSC
