/*
 * Copyright (C) 2009-2023 Apple Inc. All rights reserved.
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
#include "ArrayBuffer.h"

#include "JSArrayBufferView.h"
#include "JSArrayBufferViewInlines.h"
#include "JSCellInlines.h"
#include "JSWebAssemblyInstance.h"
#include "WaiterListManager.h"
#include "WeakInlines.h"
#include <wtf/Atomics.h>
#include <wtf/FastMalloc.h>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/MathExtras.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/Scope.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Vector.h>

#if ENABLE(WEBASSEMBLY)
#include "WasmMemory.h"
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace ArrayBufferInternal {
static constexpr bool verbose = false;
}

// THREADS/TSAN-gated copy of buffer DATA lanes that may race other Threads'
// element accesses (SAB-granularity staleness is the blessed semantics; the
// racing access itself must be defined). Production builds keep memcpy.
static void copyDataLanesRacy(void* dst, const void* src, size_t size)
{
#if TSAN_ENABLED
    uint8_t* to = static_cast<uint8_t*>(dst);
    uint8_t* from = const_cast<uint8_t*>(static_cast<const uint8_t*>(src));
    for (size_t i = 0; i < size; ++i)
        WTF::atomicStore(&to[i], WTF::atomicLoad(&from[i], std::memory_order_relaxed), std::memory_order_relaxed);
#else
    memcpy(dst, src, size);
#endif
}

static void zeroFill(void* base, size_t size)
{
#if TSAN_ENABLED
    // THREADS/TSAN-gated (recorded per TSAN-TRIAGE §13.4 precedent): a resize's
    // zero-fill writes buffer DATA lanes that concurrent typed-array readers on
    // other Threads may race (SAB-granularity staleness is the blessed
    // semantics; the race itself must be defined). Byte-wise relaxed stores keep
    // the accesses TSAN-visible; production builds keep memset below.
    {
        uint8_t* bytes = static_cast<uint8_t*>(base);
        for (size_t i = 0; i < size; ++i)
            WTF::atomicStore(&bytes[i], static_cast<uint8_t>(0), std::memory_order_relaxed);
        return;
    }
#endif
    constexpr size_t largeZeroFillThreshold = 1 * MB;
    if (size >= largeZeroFillThreshold) {
        size_t pageSizeValue = WTF::pageSize();
        uintptr_t begin = reinterpret_cast<uintptr_t>(base);
        uintptr_t end = begin + size;
        uintptr_t pageAlignedBegin = roundUpToMultipleOf(pageSizeValue, begin);
        uintptr_t pageAlignedEnd = roundDownToMultipleOf(pageSizeValue, end);
        if (pageAlignedEnd > pageAlignedBegin) {
            if (begin != pageAlignedBegin)
                memset(base, 0, pageAlignedBegin - begin);
            OSAllocator::zeroFill(reinterpret_cast<void*>(pageAlignedBegin), pageAlignedEnd - pageAlignedBegin);
            if (end != pageAlignedEnd)
                memset(reinterpret_cast<void*>(pageAlignedEnd), 0, end - pageAlignedEnd);
        } else
            memset(base, 0, size);
    } else
        memset(base, 0, size);
}

Ref<SharedTask<void(void*)>> ArrayBuffer::primitiveGigacageDestructor()
{
    static LazyNeverDestroyed<Ref<SharedTask<void(void*)>>> destructor;
    static std::once_flag onceKey;
    std::call_once(onceKey, [&] {
        destructor.construct(createSharedTask<void(void*)>([] (void* p) { Gigacage::free(Gigacage::Primitive, p); }));
    });
    return destructor.get().copyRef();
}

template<typename Func>
static bool tryAllocate(VM* vm, const Func& allocate)
{
    unsigned numTries = 2;
    bool success = false;
    for (unsigned i = 0; i < numTries && !success; ++i) {
        switch (allocate()) {
        case BufferMemoryResult::Kind::Success:
            success = true;
            break;
        case BufferMemoryResult::Kind::SuccessAndNotifyMemoryPressure:
            if (vm)
                vm->heap.collectAsync(CollectionScope::Full);
            success = true;
            break;
        case BufferMemoryResult::Kind::SyncTryToReclaimMemory:
            if (i + 1 == numTries)
                break;
            if (vm)
                vm->heap.collectSync(CollectionScope::Full);
            break;
        }
    }
    return success;
}

static RefPtr<BufferMemoryHandle> tryAllocateResizableMemory(VM* vm, size_t sizeInBytes, size_t maxByteLength)
{
    // Make sure malloc actually allocates something, but not too much. We use null to mean that the buffer is detached.
    size_t initialBytes = roundUpToMultipleOf<PageCount::pageSize>(sizeInBytes);
    if (!initialBytes)
        initialBytes = PageCount::pageSize;
    size_t maximumBytes = roundUpToMultipleOf<PageCount::pageSize>(maxByteLength);
    if (!maximumBytes)
        maximumBytes = PageCount::pageSize;

    bool done = tryAllocate(vm,
        [&] () -> BufferMemoryResult::Kind {
            return BufferMemoryManager::singleton().tryAllocatePhysicalBytes(initialBytes);
        });
    if (!done)
        return nullptr;

    char* slowMemory = nullptr;
    tryAllocate(vm,
        [&] () -> BufferMemoryResult::Kind {
            auto result = BufferMemoryManager::singleton().tryAllocateGrowableBoundsCheckingMemory(maximumBytes);
            slowMemory = std::bit_cast<char*>(result.basePtr);
            return result.kind;
        });
    if (!slowMemory) {
        BufferMemoryManager::singleton().freePhysicalBytes(initialBytes);
        return nullptr;
    }

    constexpr bool readable = false;
    constexpr bool writable = false;
    OSAllocator::protect(slowMemory + initialBytes, maximumBytes - initialBytes, readable, writable);
    return adoptRef(*new BufferMemoryHandle(slowMemory, initialBytes, maximumBytes, PageCount::fromBytes(initialBytes), PageCount::fromBytes(maximumBytes), MemorySharingMode::Shared, MemoryMode::BoundsChecking));
}

// ===== SPEC-ungil §N.6 / annex N6: per-server ArrayBuffer mapping quarantine =====
//
// PRINCIPLE (annex N6, BINDING): every tier's TA/DataView fast path loads
// LENGTH, bounds-checks, then loads BASE; the reader's two loads carry no
// ordering, so store ordering alone cannot close a torn two-word read.
// INVARIANT: a racing reader must NEVER pair a passing length with an
// unmapped-or-short base — any observable base must point at a mapping that is
// mapped and sized >= every length still observable against it. Retirement of
// a mapping therefore requires that no pre-retirement length remain live,
// which heap §10 stop quiescence provides (no JS/JIT fast path straddles a
// stop). Mechanically: detach/transfer move the mapping's OWNERSHIP into a
// per-server quarantine list entry, shrink defers the tail-page
// protect/decommit into the same list, and a heap §10 stop (the
// Heap::addStopTheWorldSafepointHook adapter below — fires once per collection
// of that heap, in BOTH protocols, while the world is stopped) retires every
// entry enqueued before the stop.
//
// All arms are GIL-off only; GIL-on and flag-off paths are byte-identical to
// the landed code (nothing below runs unless gilOffThreadsProcess()).

namespace {

// gilOffProcess (UNGIL-HANDOUT §intro): OPTION-derived at Config finalization
// (useJSThreads && !useThreadGIL && the U0 trio) and immutable for the
// process. Until the canonical Config-derived predicate lands (U-T1 owner),
// derive the option component locally; Options are finalized before any VM
// exists, so this is stable at every reachable call site. Flag-off
// (useJSThreads=false) this is false and every caller takes the landed path.
static inline bool gilOffThreadsProcess()
{
    return Options::useJSThreads() && !Options::useThreadGIL();
}

// ===== GIL-off detached-flag side table (annex N6 arm 1) =====
//
// Annex N6 arm 1 requires a detached FLAG distinct from !m_data: GIL-off
// detach leaves the stale base word in place until the next heap §10 stop, so
// "!m_data" would report a detached buffer as live for the whole
// detach->stop window. The flag is spec'd as an ArrayBuffer member with
// isDetached() returning it, but ArrayBuffer.h is NOT in this slice's owned
// files; until the header lands the member + accessor, the flag lives in this
// side table, and every in-file N6 writer arm (detach / transferTo /
// shareWith / resize) consults it through isArrayBufferDetachedGILOff().
// Read-side isDetached() callers OUTSIDE this file still evaluate !m_data
// during the window. That is memory-safe — the length is already 0, so every
// guarded fast path bounds-fails, and the mapping stays alive until the stop —
// but it is NOT the spec'd predicate for those sites and is a recorded U-T13
// sign-off gap until the header change lands (see the task summary).
//
// The table doubles as the WRITER-WRITER arbiter the annex's single-writer
// torn-pair table otherwise lacks: marking is a test-and-set under the table
// lock, so of any number of racing detach()/transferTo() calls EXACTLY ONE
// moves ownership into the quarantine and fires notifyDetaching — no double
// enqueue, no double std::exchange of m_destructor. Entries carry a
// generation so a stop-time clear can never hit a recycled ArrayBuffer*
// (ABA): ~ArrayBuffer unregisters itself, and retirement clears the stale
// contents words only while {pointer, generation} still match.
//
// Lock rank: leaf. Acquired under BufferMemoryHandle::lock() (detach/resize)
// and under the quarantine retirement path; holders take no other lock.
struct GILOffDetachedBufferTable {
    Lock lock;
    uint64_t nextGeneration WTF_GUARDED_BY_LOCK(lock) { 1 };
    UncheckedKeyHashMap<ArrayBuffer*, uint64_t> map WTF_GUARDED_BY_LOCK(lock);
};

GILOffDetachedBufferTable& gilOffDetachedBufferTable()
{
    static NeverDestroyed<GILOffDetachedBufferTable> table;
    return table.get();
}

// Pending-entry count, so the (hot) ~ArrayBuffer path can skip the table lock
// when no GIL-off detach is in flight. Incremented under the table lock at
// mark time; a destructor that misses the increment can only belong to a
// buffer that was never marked (destroying a buffer concurrently with
// detaching it requires a happens-before edge from the detach to the final
// deref — the detacher holds a Ref across detach() — and that edge also
// publishes the increment). NOTE (updated, GIL-removal review round 3):
// cross-thread Ref/deref of ArrayBuffer itself is now SAFE —
// DeferrableRefCounted's count is atomic (wtf/DeferrableRefCounted.h;
// ArrayBuffer via GCIncomingRefCounted is its only user), closing the gap
// this comment used to record. This code still deliberately adds no new
// cross-thread ref/deref pair (see the quarantine-entry comment below).
std::atomic<uint64_t> s_gilOffDetachedPendingCount { 0 };

// Returns the (nonzero) generation if this call newly marked the buffer
// detached, 0 if a racing detach/transfer already won.
uint64_t tryMarkArrayBufferDetachedGILOff(ArrayBuffer* buffer)
{
    GILOffDetachedBufferTable& table = gilOffDetachedBufferTable();
    Locker locker { table.lock };
    auto result = table.map.add(buffer, table.nextGeneration);
    if (!result.isNewEntry)
        return 0;
    table.nextGeneration++;
    s_gilOffDetachedPendingCount.fetch_add(1, std::memory_order_release);
    return result.iterator->value;
}

} // anonymous namespace

// The GIL-off detached predicate for this file's writer arms. Pre-stop it is
// the table flag; post-stop (and GIL-on) the caller's !m_data check covers.
// Flag-off / GIL-on cost: one relaxed load of a never-incremented counter.
// Defined at namespace JSC scope (NOT in the anonymous namespace) because
// AtomicsObject.cpp and JSArrayBufferPrototype.cpp declare and call it
// cross-TU; the helpers it uses stay internal to this file.
bool isArrayBufferDetachedGILOff(ArrayBuffer* buffer)
{
    if (!s_gilOffDetachedPendingCount.load(std::memory_order_acquire))
        return false;
    GILOffDetachedBufferTable& table = gilOffDetachedBufferTable();
    Locker locker { table.lock };
    return table.map.contains(buffer);
}

namespace {

// ~ArrayBuffer hook: a buffer that dies between its detach and the stop must
// not be touched by the stop-time clear (the entry's quarantined contents are
// unaffected — the entry owns its own refs/ownership and still frees the
// mapping at the stop).
void unregisterDetachedArrayBufferGILOff(ArrayBuffer* buffer)
{
    if (!s_gilOffDetachedPendingCount.load(std::memory_order_acquire))
        return;
    GILOffDetachedBufferTable& table = gilOffDetachedBufferTable();
    Locker locker { table.lock };
    if (table.map.remove(buffer))
        s_gilOffDetachedPendingCount.fetch_sub(1, std::memory_order_release);
}

// ===== GIL-off stale wasm mapping keepalive (annex N6 arm 4, partial) =====
//
// Annex N6 arm 4: a no-reservation (BoundsChecking-without-VA) wasm grow
// RELOCATES, and must do so under a heap §10 stop with the OLD mapping
// quarantined to the NEXT stop. The stop conduction belongs to the wasm grow
// path (Wasm::Memory::grow's MemoryMode::BoundsChecking arm), which is NOT in
// this slice's owned files and does NOT yet conduct it — see the comment in
// ArrayBufferContents::refreshAfterWasmMemoryGrow. What THIS file can own is
// the quarantine half: the replaced handle is kept alive here and released
// only inside a stop, so a racing reader's torn {pre-grow length, pre-grow
// base} pair never dereferences an unmapped base. (The complementary torn
// pair {post-grow length, pre-grow base} is only excluded by the missing stop
// conduction — OPEN DEPENDENCY, blocks U-T13 sign-off.)
//
// Drained by arrayBufferQuarantineSafepointHook below, i.e. at any heap's
// stop; under gilOffProcess there is exactly one sticky server heap (annex
// U0C), so that is the right stop. If NO quarantine hook is registered yet
// (no detach/shrink has happened in the process), stale handles are retained
// until one is — a bounded keepalive, never an unsafety.
struct StaleWasmMappingList {
    Lock lock;
    Vector<Ref<BufferMemoryHandle>> handles WTF_GUARDED_BY_LOCK(lock);
};

StaleWasmMappingList& staleWasmMappings()
{
    static NeverDestroyed<StaleWasmMappingList> list;
    return list.get();
}

void quarantineStaleWasmMappingGILOff(Ref<BufferMemoryHandle>&& handle)
{
    StaleWasmMappingList& list = staleWasmMappings();
    Locker locker { list.lock };
    list.handles.append(WTF::move(handle));
}

// One quarantine entry. Exactly one of the two shapes is populated:
// - DETACH/TRANSFER (annex N6 arms 1-2): `contents` carries the detached
//   mapping's keepalive — m_data + m_destructor exclusively (moved out of the
//   source), m_shared/m_memoryHandle as CO-refs (both ThreadSafeRefCounted;
//   the source's own ref words are deliberately left intact, because
//   resize()/grow() copy those RefPtrs WITHOUT a lock and a detach-time
//   std::exchange would race them — the source's refs are dropped at the stop
//   instead, under quiescence, so the entry still bounds the mapping's
//   lifetime at "the next stop"). `clearBaseWordAtStop` clears the source's
//   stale words under quiescence — it captures a RAW pointer + generation and
//   re-validates against the detached-buffer table, NOT a Ref keepalive:
//   ArrayBuffer's refcount (DeferrableRefCounted) is a plain uint32_t, so a
//   detach-thread ref paired with a stop-conductor deref would be a new
//   cross-thread non-atomic refcount RMW. ~ArrayBuffer unregisters from the
//   table, which neuters the closure (generation mismatch / absent key).
// - SHRINK tail (annex N6 arm 3): {tailHandle, tailOffset, tailSize} names the
//   deferred page range [tailOffset, tailOffset + tailSize) of tailHandle;
//   retirement performs the protect + freePhysicalBytes + updateSize under
//   quiescence. INVARIANT: at most ONE tail entry per handle, and while
//   pending it always abuts the handle's current end:
//   tailOffset + tailSize == handle->size(). (Established by
//   deferShrinkTailGILOff's replace rule and consumeQuarantinedTailOnRegrow's
//   trim rule, both of which run under the handle's lock.)
struct ArrayBufferQuarantineEntry {
    WTF_MAKE_NONCOPYABLE(ArrayBufferQuarantineEntry);
public:
    ArrayBufferQuarantineEntry() = default;
    ArrayBufferQuarantineEntry(ArrayBufferQuarantineEntry&&) = default;
    ArrayBufferQuarantineEntry& operator=(ArrayBufferQuarantineEntry&&) = default;

    ArrayBufferContents contents;
    Function<void()> clearBaseWordAtStop;

    RefPtr<BufferMemoryHandle> tailHandle;
    size_t tailOffset { 0 };
    size_t tailSize { 0 };
};

struct ArrayBufferQuarantine {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(ArrayBufferQuarantine);
    WTF_MAKE_NONCOPYABLE(ArrayBufferQuarantine);
public:
    ArrayBufferQuarantine() = default;
    Lock lock;
    Vector<ArrayBufferQuarantineEntry> pending WTF_GUARDED_BY_LOCK(lock);
};

// PER-SERVER-HEAP, never process-global (annex N6 "per-server quarantine
// list"; same shape as ConcurrentButterfly's §6 epoch registry). Entries are
// never removed from the map — a destroyed Heap's quarantine is simply left
// drained in place (GIL-off the server heap is process-lifetime anyway,
// UNGIL-HANDOUT gilOffProcess note), and a recycled Heap* re-adopts the old,
// empty quarantine, which is sound: retirement is keyed by the collecting
// heap and drains everything.
struct ArrayBufferQuarantineRegistry {
    Lock lock;
    UncheckedKeyHashMap<JSC::Heap*, std::unique_ptr<ArrayBufferQuarantine>> quarantines WTF_GUARDED_BY_LOCK(lock);
    UncheckedKeyHashSet<JSC::Heap*> hookRegistered WTF_GUARDED_BY_LOCK(lock);
};

ArrayBufferQuarantineRegistry& arrayBufferQuarantineRegistry()
{
    static NeverDestroyed<ArrayBufferQuarantineRegistry> registry;
    return registry.get();
}

ArrayBufferQuarantine* existingArrayBufferQuarantine(JSC::Heap& heap)
{
    ArrayBufferQuarantineRegistry& registry = arrayBufferQuarantineRegistry();
    Locker locker { registry.lock };
    auto iterator = registry.quarantines.find(&heap);
    if (iterator == registry.quarantines.end())
        return nullptr;
    return iterator->value.get();
}

// Retires one entry, world-stopped. Quiescence (heap §10) guarantees no
// mutator holds a pre-retirement {length, base} pair across this point, and
// no mutator is parked while holding a BufferMemoryHandle lock with a pending
// tail entry for that handle (the resize paths only enqueue/trim tail entries
// in regions of the handle-locked section that contain no allocation or other
// stop point — see ArrayBuffer::resize below).
void retireArrayBufferQuarantineEntry(ArrayBufferQuarantineEntry& entry)
{
    // Arms 1-2: clear/poison the source's stale base word FIRST, then release
    // the mapping (the entry's ArrayBufferContents destructor / handle deref
    // run when the drained vector is destroyed by the hook).
    if (entry.clearBaseWordAtStop)
        entry.clearBaseWordAtStop();

    // Arm 3: deferred tail protect + decommit (+ updateSize), under the
    // handle's lock. No mutator can contend here beyond a momentary hold (see
    // above), so this never waits on a parked thread.
    if (entry.tailHandle && entry.tailSize) {
        Ref<BufferMemoryHandle> handle = *entry.tailHandle;
        Locker locker { handle->lock() };
        ASSERT(handle->size() == entry.tailOffset + entry.tailSize);
        BufferMemoryManager::singleton().freePhysicalBytes(entry.tailSize);
        void* memory = handle->memory();
        RELEASE_ASSERT(memory);
        uint8_t* startAddress = static_cast<uint8_t*>(memory) + entry.tailOffset;
        dataLogLnIf(ArrayBufferInternal::verbose, "Quarantine retiring memory's ", RawPointer(memory), " as none in range [", RawPointer(startAddress), ", ", RawPointer(startAddress + entry.tailSize), ")");
        constexpr bool readable = false;
        constexpr bool writable = false;
        OSAllocator::protect(startAddress, entry.tailSize, readable, writable);
        handle->updateSize(entry.tailOffset);
    }
}

// §10 hook adapter: runs once per collection of `heap`, world-stopped
// (Heap::runStopTheWorldSafepointHooks contract, heap §9). Every entry in the
// list was enqueued before this stop (mutators are quiesced), so drain-all is
// exactly "retire entries enqueued before the stop". The list is swapped out
// under its (leaf) lock and retired OUTSIDE it, so the lock never nests a
// BufferMemoryHandle lock under it (the resize paths nest the other way:
// handle lock -> quarantine lock).
void arrayBufferQuarantineSafepointHook(JSC::Heap& heap)
{
    // Annex N6 arm 4 (partial): release stale relocated wasm mappings under
    // quiescence. World-stopped, so no reader still holds a pre-stop
    // {length, base} pair against any of these handles.
    Vector<Ref<BufferMemoryHandle>> staleHandles;
    {
        StaleWasmMappingList& list = staleWasmMappings();
        Locker locker { list.lock };
        staleHandles = std::exchange(list.handles, { });
    }
    staleHandles.clear();

    ArrayBufferQuarantine* quarantine = existingArrayBufferQuarantine(heap);
    if (!quarantine)
        return;
    Vector<ArrayBufferQuarantineEntry> retired;
    {
        Locker locker { quarantine->lock };
        retired = std::exchange(quarantine->pending, { });
    }
    for (auto& entry : retired)
        retireArrayBufferQuarantineEntry(entry);
    // Destroying `retired` releases the quarantined mappings (contents
    // destructors / handle derefs), under quiescence.
}

ArrayBufferQuarantine& ensureArrayBufferQuarantine(JSC::Heap& heap)
{
    ArrayBufferQuarantine* result;
    bool needsHook = false;
    {
        ArrayBufferQuarantineRegistry& registry = arrayBufferQuarantineRegistry();
        Locker locker { registry.lock };
        auto addResult = registry.quarantines.ensure(&heap, [] {
            return makeUnique<ArrayBufferQuarantine>();
        });
        result = addResult.iterator->value.get();
        needsHook = registry.hookRegistered.add(&heap).isNewEntry;
    }
    // Heap::addStopTheWorldSafepointHook takes the heap's own hook lock, so it
    // is called OUTSIDE the registry lock (which must stay a leaf). A racing
    // first-enqueuer can momentarily see the quarantine before the hook is
    // appended; its entry simply waits for the collection AFTER the winner's
    // append completes — retirement timing is "some stop after enqueue", which
    // annex N6 permits (only the enqueued-before-the-stop direction binds).
    if (needsHook)
        heap.addStopTheWorldSafepointHook(&arrayBufferQuarantineSafepointHook);
    return *result;
}

// Arms 1-2 enqueue. Caller must be at a point where reporting extra memory is
// legal (it can trigger a synchronous collection, which may retire the entry
// immediately — correct: the enqueuer is the conductor and is past every use
// of the old {length, base} pair). Quarantine sizing (annex N6): entries are
// byte-accounted against heap extra memory so a detach/shrink storm pulls the
// next collection forward.
void enqueueArrayBufferQuarantineEntry(JSC::Heap& heap, ArrayBufferQuarantineEntry&& entry, size_t accountedBytes)
{
    ArrayBufferQuarantine& quarantine = ensureArrayBufferQuarantine(heap);
    {
        Locker locker { quarantine.lock };
        quarantine.pending.append(WTF::move(entry));
    }
    if (accountedBytes)
        heap.reportExtraMemoryAllocated(static_cast<JSCell*>(nullptr), accountedBytes);
}

// Arm 3 enqueue/replace, caller holds handle.lock(). Maintains the
// one-tail-entry-per-handle invariant. Returns the count of NEWLY quarantined
// bytes; the caller reports them after releasing the handle lock (reporting
// can conduct a collection, which must not happen under the handle lock with
// this entry pending — the hook would need the same lock).
size_t deferShrinkTailGILOff(JSC::Heap& heap, BufferMemoryHandle& handle, size_t desiredSize)
{
    ASSERT(desiredSize < handle.size());
    ArrayBufferQuarantine& quarantine = ensureArrayBufferQuarantine(heap);
    Locker locker { quarantine.lock };
    for (auto& entry : quarantine.pending) {
        if (entry.tailHandle.get() != &handle)
            continue;
        // Existing tail [tailOffset, handle.size()): extend downward. The new
        // desired size is never above the pending tail's start (the current
        // logical length sits at or below tailOffset, and we are shrinking).
        ASSERT(entry.tailOffset + entry.tailSize == handle.size());
        ASSERT(desiredSize <= entry.tailOffset);
        size_t newlyQuarantined = entry.tailOffset - desiredSize;
        entry.tailOffset = desiredSize;
        entry.tailSize = handle.size() - desiredSize;
        return newlyQuarantined;
    }
    ArrayBufferQuarantineEntry entry;
    entry.tailHandle = &handle;
    entry.tailOffset = desiredSize;
    entry.tailSize = handle.size() - desiredSize;
    size_t newlyQuarantined = entry.tailSize;
    quarantine.pending.append(WTF::move(entry));
    return newlyQuarantined;
}

// Re-grow before the stop consumes/cancels overlapping pending tail entries
// (annex N6 arm 3). Caller holds handle.lock(); pages in a pending tail are
// still committed (the protect/decommit is what was deferred), so the landed
// zeroFill semantics apply to any re-used range. MUST run before any
// allocation/GC-capable call in the grow path: once this thread can conduct
// (or wait on) a collection while holding the handle lock, no pending tail
// entry for this handle may exist (the hook retires tails under the same
// lock).
void consumeQuarantinedTailOnRegrow(JSC::Heap& heap, BufferMemoryHandle& handle, size_t newDesiredSize)
{
    ArrayBufferQuarantine* quarantine = existingArrayBufferQuarantine(heap);
    if (!quarantine)
        return;
    Locker locker { quarantine->lock };
    quarantine->pending.removeAllMatching([&](ArrayBufferQuarantineEntry& entry) {
        if (entry.tailHandle.get() != &handle)
            return false;
        size_t end = entry.tailOffset + entry.tailSize;
        if (newDesiredSize >= end)
            return true; // Fully consumed: every deferred page is in use again.
        if (newDesiredSize > entry.tailOffset) {
            // Partially consumed: keep deferring [newDesiredSize, end).
            entry.tailOffset = newDesiredSize;
            entry.tailSize = end - newDesiredSize;
        }
        return false;
    });
    // Over-accounted extra memory from consumed entries is left in place
    // (there is no un-report API); it only hastens the next collection.
}

} // anonymous namespace

ArrayBufferContents::ArrayBufferContents(void* data, size_t sizeInBytes, std::optional<size_t> maxByteLength, ArrayBufferDestructorFunction&& destructor)
    : m_data(data)
    , m_destructor(WTF::move(destructor))
    , m_maxByteLength(maxByteLength.value_or(sizeInBytes))
    , m_hasMaxByteLength(!!maxByteLength)
{
    // THREADS/TSAN: relaxed store (not member-init) so the ctor's write at a
    // GC/allocator-recycled address pairs cleanly with stale readers' atomics.
    WTF::atomicStore(&m_sizeInBytes, sizeInBytes, std::memory_order_relaxed);
    RELEASE_ASSERT(sizeInBytes <= MAX_ARRAY_BUFFER_SIZE);
}

ArrayBufferContents::ArrayBufferContents(std::span<const uint8_t> data, std::optional<size_t> maxByteLength, ArrayBufferDestructorFunction&& destructor)
    : ArrayBufferContents(const_cast<uint8_t*>(data.data()), data.size(), maxByteLength, WTF::move(destructor))
{
}

ArrayBufferContents::ArrayBufferContents(Ref<SharedArrayBufferContents>&& shared, bool forceFixedLengthIfWasm)
    : m_shared(WTF::move(shared))
    , m_memoryHandle(m_shared->memoryHandle())
{
    WTF::atomicStore(&m_sizeInBytes, m_shared->sizeInBytes(std::memory_order_seq_cst), std::memory_order_relaxed); // THREADS/TSAN: see first ctor.
    RELEASE_ASSERT(WTF::atomicLoad(&m_sizeInBytes, std::memory_order_relaxed) <= MAX_ARRAY_BUFFER_SIZE);
    bool adjustedForceFixedLengthIfWasm = forceFixedLengthIfWasm || !Options::useWasmMemoryToBufferAPIs();
    if (m_shared->mode() == SharedArrayBufferContents::Mode::WebAssembly && adjustedForceFixedLengthIfWasm) {
        m_hasMaxByteLength = false;
        m_maxByteLength = WTF::atomicLoad(&m_sizeInBytes, std::memory_order_relaxed);
    } else {
        m_hasMaxByteLength = !!m_shared->maxByteLength();
        m_maxByteLength = m_shared->maxByteLength().value_or(WTF::atomicLoad(&m_sizeInBytes, std::memory_order_relaxed));
    }
    // data() cannot destroy m_shared here so the code is safe as is so avoid
    // refing for performance reasons.
    SUPPRESS_UNCOUNTED_ARG m_data = DataType { m_shared->data() };
}

ArrayBufferContents::ArrayBufferContents(void* data, size_t sizeInBytes, size_t maxByteLength, Ref<BufferMemoryHandle>&& memoryHandle)
    : m_data(data)
    , m_memoryHandle(WTF::move(memoryHandle))
    , m_maxByteLength(maxByteLength)
    , m_hasMaxByteLength(true)
{
    WTF::atomicStore(&m_sizeInBytes, sizeInBytes, std::memory_order_relaxed); // THREADS/TSAN: see first ctor.
    RELEASE_ASSERT(sizeInBytes <= MAX_ARRAY_BUFFER_SIZE);
}

void ArrayBufferContents::tryAllocate(size_t numElements, unsigned elementByteSize, InitializationPolicy policy)
{
    CheckedSize sizeInBytes = numElements;
    sizeInBytes *= elementByteSize;
    if (sizeInBytes.hasOverflowed() || sizeInBytes.value() > MAX_ARRAY_BUFFER_SIZE) {
        reset();
        return;
    }

    size_t allocationSize = sizeInBytes.value();
    if (!allocationSize)
        allocationSize = 1; // Make sure malloc actually allocates something, but not too much. We use null to mean that the buffer is detached.

    void* data = nullptr;
    if (policy == InitializationPolicy::ZeroInitialize)
        data = Gigacage::tryZeroedMalloc(Gigacage::Primitive, allocationSize);
    else
        data = Gigacage::tryMalloc(Gigacage::Primitive, allocationSize);
    m_data = DataType(data);
    if (!data) {
        reset();
        return;
    }

    WTF::atomicStore(&m_sizeInBytes, sizeInBytes.value(), std::memory_order_relaxed);
    RELEASE_ASSERT(WTF::atomicLoad(&m_sizeInBytes, std::memory_order_relaxed) <= MAX_ARRAY_BUFFER_SIZE);
    m_maxByteLength = m_sizeInBytes;
    m_hasMaxByteLength = false;
    m_destructor = ArrayBuffer::primitiveGigacageDestructor();
}

void ArrayBufferContents::makeShared()
{
    m_shared = SharedArrayBufferContents::create(mutableSpan(), maxByteLength(), m_memoryHandle, WTF::move(m_destructor), SharedArrayBufferContents::Mode::Default);
    m_destructor = nullptr;
}

SharedArrayBufferContents::~SharedArrayBufferContents()
{
    WaiterListManager::singleton().unregister(std::bit_cast<uint8_t*>(data()), m_sizeInBytes.load(std::memory_order_relaxed)); // SharedArrayBufferContents::m_sizeInBytes is already std::atomic.
    if (RefPtr destructor = m_destructor) {
        // FIXME: we shouldn't use getUnsafe here https://bugs.webkit.org/show_bug.cgi?id=197698
        destructor->run(m_data.getUnsafe());
    }
}

void ArrayBufferContents::copyTo(ArrayBufferContents& other)
{
    ASSERT(!other.m_data);
    size_t selfSizeInBytes = WTF::atomicLoad(&m_sizeInBytes, std::memory_order_relaxed);
    other.tryAllocate(selfSizeInBytes, sizeof(char), ArrayBufferContents::InitializationPolicy::DontInitialize);
    if (!other.m_data)
        return;
    copyDataLanesRacy(other.data(), data(), selfSizeInBytes);
    WTF::atomicStore(&other.m_sizeInBytes, selfSizeInBytes, std::memory_order_relaxed);
    RELEASE_ASSERT(selfSizeInBytes <= MAX_ARRAY_BUFFER_SIZE);
    ASSERT(other.m_maxByteLength <= MAX_ARRAY_BUFFER_SIZE);
}

void ArrayBufferContents::shareWith(ArrayBufferContents& other)
{
    ASSERT(!other.m_data);
    ASSERT(m_shared);
    other.m_data = m_data;
    other.m_destructor = nullptr;
    other.m_shared = m_shared;
    other.m_memoryHandle = m_memoryHandle;
    WTF::atomicStore(&other.m_sizeInBytes, WTF::atomicLoad(&m_sizeInBytes, std::memory_order_relaxed), std::memory_order_relaxed);
    other.m_maxByteLength = m_maxByteLength;
    other.m_hasMaxByteLength = m_hasMaxByteLength;
    RELEASE_ASSERT(WTF::atomicLoad(&other.m_sizeInBytes, std::memory_order_relaxed) <= MAX_ARRAY_BUFFER_SIZE);
    ASSERT(other.m_maxByteLength <= MAX_ARRAY_BUFFER_SIZE);
}

Ref<ArrayBuffer> ArrayBuffer::create(size_t numElements, unsigned elementByteSize)
{
    auto buffer = tryCreate(numElements, elementByteSize);
    if (!buffer)
        CRASH();
    return buffer.releaseNonNull();
}

Ref<ArrayBuffer> ArrayBuffer::create(ArrayBuffer& other)
{
    return ArrayBuffer::create(other.span());
}

Ref<ArrayBuffer> ArrayBuffer::create(std::span<const uint8_t> span)
{
    auto buffer = tryCreate(span);
    if (!buffer)
        CRASH();
    return buffer.releaseNonNull();
}

Ref<ArrayBuffer> ArrayBuffer::create(ArrayBufferContents&& contents)
{
    return adoptRef(*new ArrayBuffer(WTF::move(contents)));
}

// FIXME: We cannot use this except if the memory comes from the cage.
// Current this is only used from:
// - JSGenericTypedArrayView<>::slowDownAndWasteMemory. But in that case, the memory should have already come
//   from the cage.
Ref<ArrayBuffer> ArrayBuffer::createAdopted(std::span<const uint8_t> data)
{
    ASSERT(!Gigacage::isEnabled() || (Gigacage::contains(data.data()) && Gigacage::contains(data.data() + data.size() - 1)));
    return createFromBytes(data, ArrayBuffer::primitiveGigacageDestructor());
}

// FIXME: We cannot use this except if the memory comes from the cage.
// Currently this is only used from:
// - The C API. We could support that by either having the system switch to a mode where typed arrays are no
//   longer caged, or we could introduce a new set of typed array types that are uncaged and get accessed
//   differently.
// - WebAssembly. Wasm should allocate from the cage.
Ref<ArrayBuffer> ArrayBuffer::createFromBytes(std::span<const uint8_t> data, ArrayBufferDestructorFunction&& destructor)
{
    if (data.data() && !Gigacage::isCaged(Gigacage::Primitive, data.data()))
        Gigacage::disablePrimitiveGigacage();
    
    ArrayBufferContents contents(data, std::nullopt, WTF::move(destructor));
    return create(WTF::move(contents));
}

Ref<ArrayBuffer> ArrayBuffer::createShared(Ref<SharedArrayBufferContents>&& shared, bool forceFixedLengthIfWasm)
{
    ArrayBufferContents contents(WTF::move(shared), forceFixedLengthIfWasm);
    return create(WTF::move(contents));
}

RefPtr<ArrayBuffer> ArrayBuffer::tryCreate(size_t numElements, unsigned elementByteSize, std::optional<size_t> maxByteLength)
{
    return tryCreate(numElements, elementByteSize, maxByteLength, ArrayBufferContents::InitializationPolicy::ZeroInitialize);
}

RefPtr<ArrayBuffer> ArrayBuffer::tryCreate(ArrayBuffer& other)
{
    return tryCreate(other.span());
}

RefPtr<ArrayBuffer> ArrayBuffer::tryCreate(std::span<const uint8_t> span)
{
    ArrayBufferContents contents;
    contents.tryAllocate(span.size(), 1, ArrayBufferContents::InitializationPolicy::DontInitialize);
    if (!contents.m_data)
        return nullptr;
    return createInternal(WTF::move(contents), span.data(), span.size());
}

Ref<ArrayBuffer> ArrayBuffer::createUninitialized(size_t numElements, unsigned elementByteSize)
{
    return create(numElements, elementByteSize, ArrayBufferContents::InitializationPolicy::DontInitialize);
}

RefPtr<ArrayBuffer> ArrayBuffer::tryCreateUninitialized(size_t numElements, unsigned elementByteSize)
{
    return tryCreate(numElements, elementByteSize, std::nullopt, ArrayBufferContents::InitializationPolicy::DontInitialize);
}

Ref<ArrayBuffer> ArrayBuffer::create(size_t numElements, unsigned elementByteSize, ArrayBufferContents::InitializationPolicy policy)
{
    auto buffer = tryCreate(numElements, elementByteSize, std::nullopt, policy);
    if (!buffer)
        CRASH();
    return buffer.releaseNonNull();
}

Ref<ArrayBuffer> ArrayBuffer::createInternal(ArrayBufferContents&& contents, const void* source, size_t byteLength)
{
    auto buffer = adoptRef(*new ArrayBuffer(WTF::move(contents)));
    if (byteLength) {
        ASSERT(source);
        memcpy(buffer->data(), source, byteLength);
    }
    return buffer;
}

RefPtr<ArrayBuffer> ArrayBuffer::tryCreate(size_t numElements, unsigned elementByteSize, std::optional<size_t> maxByteLength, ArrayBufferContents::InitializationPolicy policy)
{
    if (!maxByteLength) {
        ArrayBufferContents contents;
        contents.tryAllocate(numElements, elementByteSize, policy);
        if (!contents.m_data)
            return nullptr;
        return adoptRef(*new ArrayBuffer(WTF::move(contents)));
    }

    CheckedSize sizeInBytes = numElements;
    sizeInBytes *= elementByteSize;
    if (sizeInBytes.hasOverflowed() || sizeInBytes.value() > MAX_ARRAY_BUFFER_SIZE)
        return nullptr;

    if (sizeInBytes.value() > maxByteLength.value() || maxByteLength.value() > MAX_ARRAY_BUFFER_SIZE)
        return nullptr;

    auto handle = tryAllocateResizableMemory(nullptr, sizeInBytes.value(), maxByteLength.value());
    if (!handle)
        return nullptr;

    void* memory = handle->memory();
    ArrayBufferContents contents(memory, sizeInBytes.value(), maxByteLength.value(), handle.releaseNonNull());
    return create(WTF::move(contents));
}

ArrayBuffer::ArrayBuffer(ArrayBufferContents&& contents)
    : m_contents(WTF::move(contents))
{
}

size_t ArrayBuffer::clampValue(double x, size_t left, size_t right)
{
    ASSERT(left <= right);
    if (x < left)
        x = left;
    if (right < x)
        x = right;
    return x;
}

size_t ArrayBuffer::clampIndex(double index) const
{
    size_t currentLength = byteLength();
    if (index < 0)
        index = currentLength + index;
    return clampValue(index, 0, currentLength);
}

RefPtr<ArrayBuffer> ArrayBuffer::slice(double begin, double end) const
{
    return sliceWithClampedIndex(clampIndex(begin), clampIndex(end));
}

RefPtr<ArrayBuffer> ArrayBuffer::slice(double begin) const
{
    return sliceWithClampedIndex(clampIndex(begin), byteLength());
}

RefPtr<ArrayBuffer> ArrayBuffer::sliceWithClampedIndex(size_t begin, size_t end) const
{
    size_t size = begin <= end ? end - begin : 0;
    auto result = ArrayBuffer::tryCreate(span().subspan(begin, size));
    if (result)
        result->setSharingMode(sharingMode());
    return result;
}

void ArrayBuffer::makeShared()
{
    // GIL-off the side-table flag is the detached predicate during the
    // detach->stop window (annex N6 arm 1); !m_data alone would miss it.
    ASSERT(!isArrayBufferDetachedGILOff(this));
    m_contents.makeShared();
    m_locked = true;
    ASSERT(!isDetached());
}

void ArrayBuffer::makeWasmMemory()
{
    m_locked = true;
    m_isWasmMemory = true;
}

void ArrayBuffer::setAssociatedWasmMemory(Wasm::Memory* memory)
{
    // The pointer from a buffer to a memory is only required when the buffer is resizable non-shared,
    // to direct a grow request to the memory (see ArrayBuffer::resize). In other scenarios
    // the pointer is not necessary and we should not be setting it to anything but a nullptr.
    ASSERT(isWasmMemory() && (isResizableNonShared() || !memory));
#if ENABLE(WEBASSEMBLY)
    m_associatedWasmMemory = memory;
#else
    UNUSED_PARAM(memory);
#endif
}

void ArrayBuffer::refreshAfterWasmMemoryGrow(VM& vm, Wasm::Memory* memory)
{
    ASSERT(isWasmMemory());

    void* oldData = m_contents.data();
    m_contents.refreshAfterWasmMemoryGrow(memory);
    void* newData = m_contents.data();
    if (newData == oldData)
        return;

    // JSArrayBufferViews (typed arrays) effectively cache their buffer's data pointer.
    // GIL-off, another thread constructing a view on this buffer concurrently
    // appends to its incoming-reference storage under the owning set's lock
    // (Heap::addReference -> addIncomingReference: singleton->Vector repoint
    // or Vector realloc), so an unlocked walk here is the §3.27
    // realloc-vs-reader UAF. Snapshot under the lock, walk the snapshot (see
    // notifyDetaching for the leaf-rank/lifetime argument).
    auto incomingReferences = snapshotIncomingReferences(vm);
    for (JSCell* cell : incomingReferences) {
        auto* view = dynamicDowncast<JSArrayBufferView>(cell);
        if (view)
            view->refreshVector(newData);
    }
}

void ArrayBuffer::setSharingMode(ArrayBufferSharingMode newSharingMode)
{
    if (newSharingMode == sharingMode())
        return;
    RELEASE_ASSERT(!isShared()); // Cannot revert sharing.
    RELEASE_ASSERT(newSharingMode == ArrayBufferSharingMode::Shared);
    makeShared();
}

bool ArrayBuffer::shareWith(ArrayBufferContents& result)
{
    // The flag arm matters GIL-off only (annex N6 arm 1 leaves a stale base
    // word in place until the stop, so !m_data alone would miss a
    // detached-but-not-yet-retired buffer); GIL-on the side table is empty
    // and this is exactly the landed !m_data check.
    if (isDetached() || isArrayBufferDetachedGILOff(this) || !m_contents.m_data || !isShared()) {
        result.m_data = nullptr;
        return false;
    }

    m_contents.shareWith(result);
    return true;
}

bool ArrayBuffer::transferTo(VM& vm, ArrayBufferContents& result)
{
    Ref<ArrayBuffer> protect(*this);

    // GIL-off a detached buffer keeps a stale (quarantined) base word, so the
    // flag — not !m_data — is the detached predicate (annex N6 arm 1). GIL-on
    // the side table is empty and this is exactly the landed !m_data check.
    // The check-then-act window against a concurrent detach is closed for
    // OWNERSHIP by the flag test-and-set inside detach() (the copy below
    // reads a stale-but-safe mapping, and our subsequent detach(vm) no-ops if
    // it lost) — only the JS-visible outcome of the race is nondeterministic,
    // never the memory safety.
    if (isDetached() || isArrayBufferDetachedGILOff(this) || !m_contents.m_data) {
        result.m_data = nullptr;
        return false;
    }

    if (isShared()) {
        m_contents.shareWith(result);
        return true;
    }

    if (!isDetachable()) {
        m_contents.copyTo(result);
        if (!result.m_data)
            return false;
        return true;
    }

    if (gilOffThreadsProcess()) [[unlikely]] {
        // SPEC-ungil annex N6 arm 2 (TRANSFER, r14 F2), GIL-off only: the
        // detachable non-shared arm is COPY + DETACH. The transferee gets a
        // FRESH allocation (no live-transferee aliasing over a
        // quarantine-visible mapping), then the source runs arm 1 verbatim —
        // its contents, i.e. the original mapping, enter the quarantine
        // owning the free. Perf delta O(1) -> O(n), recorded and accepted v1.
        // One atomic length snapshot for both arms: a concurrent detach()
        // stores m_sizeInBytes atomically, so copyTo()'s plain read would be
        // a data race. The snapshot pairs safely with the base (the mapping
        // stays full-sized until the stop, annex N6 stale-but-safe row).
        size_t currentByteLength = WTF::atomicLoad(&m_contents.m_sizeInBytes, std::memory_order_relaxed);
        if (!m_contents.m_hasMaxByteLength) {
            // Source without maxByteLength: plain copy (mirrors
            // ArrayBufferContents::copyTo, with the snapshot above).
            result.tryAllocate(currentByteLength, sizeof(char), ArrayBufferContents::InitializationPolicy::DontInitialize);
            if (!result.m_data)
                return false;
            if (currentByteLength)
                copyDataLanesRacy(result.data(), m_contents.data(), currentByteLength);
            result.m_sizeInBytes = currentByteLength;
        } else {
            // Source WITH maxByteLength: copyTo is insufficient (it copies
            // only m_data/m_sizeInBytes, and ArrayBuffer.prototype.transfer's
            // resizable path resize()s the transferee afterwards). Allocate
            // via the tryAllocateResizableMemory shape with the SOURCE's
            // maxByteLength reservation, and stamp m_maxByteLength,
            // m_hasMaxByteLength, and the NEW m_memoryHandle onto the result
            // BEFORE the memcpy of byteLength() bytes. The post-transferTo
            // resize of the transferee is thread-local (the JSArrayBuffer
            // wrapper is created only afterwards — no concurrent reader).
            // Snapshot once (a concurrent detach() zeroes m_maxByteLength
            // atomically) and use the same value for reservation + stamp.
            size_t maxByteLength = WTF::atomicLoad(&m_contents.m_maxByteLength, std::memory_order_relaxed);
            auto handle = tryAllocateResizableMemory(&vm, currentByteLength, maxByteLength);
            if (!handle) {
                // OOM => transfer fails as the landed non-detachable arm does.
                result.m_data = nullptr;
                return false;
            }
            void* memory = handle->memory();
            result = ArrayBufferContents(memory, currentByteLength, maxByteLength, handle.releaseNonNull());
            if (currentByteLength)
                copyDataLanesRacy(result.data(), m_contents.data(), currentByteLength);
        }

        // The source runs arm 1 verbatim (quarantine owning the free,
        // length=0 seq_cst, detached flag, notifyDetaching as landed).
        detach(vm);
        return true;
    }

    result = m_contents.detach();
    notifyDetaching(vm);
    return true;
}

// We allow detaching wasm memory ArrayBuffers even though they are locked.
void ArrayBuffer::detach(VM& vm)
{
    if (gilOffThreadsProcess()) [[unlikely]] {
        // SPEC-ungil annex N6 arm 1 (DETACH-AND-FREE), GIL-off only:
        // publish length = 0 (seq_cst) plus a separate detached FLAG (the
        // side-table flag above — NOT !m_data); the base word is NOT cleared.
        // Ownership of the mapping moves INTO a per-server quarantine entry;
        // a heap §10 stop clears/poisons the stale base word under quiescence
        // and then releases the mapping. A racing reader can only observe
        // {0, *} (bounds-fails) or {oldLen, oldBase} (stale-but-safe: the
        // mapping stays mapped and full-sized until the stop).
        //
        // The flag is set BEFORE the zero length (the annex lists them
        // together, unordered for readers): the test-and-set is also the
        // writer-writer arbiter, and a reader that observes flag-before-zero
        // only fails MORE conservatively (slow paths re-check both).
        //
        // When the buffer is handle-backed, the flag + length publish runs
        // under the handle's lock. resizeGILOff re-checks the flag under the
        // same lock before committing or publishing, so an in-flight resize
        // can never resurrect a nonzero length on a detached buffer (and a
        // resize that already published simply happens-before this zero).
        // m_memoryHandle itself is read here WITHOUT a lock, which is sound
        // because GIL-off no path writes that word off-stop (this function no
        // longer nulls it — see below).
        size_t oldSizeInBytes = 0;
        size_t oldMaxByteLength = 0;
        uint64_t generation = 0;
        auto publishDetachGILOff = [&]() {
            generation = tryMarkArrayBufferDetachedGILOff(this);
            if (!generation)
                return;
            oldSizeInBytes = WTF::atomicLoad(&m_contents.m_sizeInBytes, std::memory_order_relaxed);
            oldMaxByteLength = m_contents.m_maxByteLength;
            WTF::atomicStore(&m_contents.m_sizeInBytes, static_cast<size_t>(0), std::memory_order_seq_cst);
            // Mirror the landed detach()'s observable state: m_maxByteLength
            // cleared, m_hasMaxByteLength kept. Atomic because the GIL-off
            // resize() entry block reads it outside the handle lock.
            WTF::atomicStore(&m_contents.m_maxByteLength, static_cast<size_t>(0), std::memory_order_relaxed);
        };
        RefPtr<BufferMemoryHandle> handle = m_contents.m_memoryHandle;
        if (handle) {
            Locker locker { handle->lock() };
            publishDetachGILOff();
        } else
            publishDetachGILOff();
        if (!generation) {
            // A racing detach()/transferTo() already won the flag: it (alone)
            // moved ownership into the quarantine and fired notifyDetaching.
            // Idempotent return — no double enqueue of the same mapping.
            return;
        }

        if (m_contents.m_data) {
            size_t retainedBytes = handle ? handle->size() : oldSizeInBytes;

            // Move/share the mapping's keepalive into the quarantine entry.
            // m_data + m_destructor move EXCLUSIVELY (only the flag winner
            // runs this, and nothing reads m_destructor concurrently);
            // m_shared/m_memoryHandle are taken as CO-refs and the source's
            // words are left intact, because resize()/grow() copy those
            // RefPtrs without a lock — the source's refs are dropped at the
            // stop, under quiescence, by the closure below. Either way the
            // mapping cannot be freed before the stop, and ~ArrayBufferContents
            // on this buffer frees nothing early (m_destructor is null, the
            // handle/shared refs are non-last while the entry lives).
            ArrayBufferQuarantineEntry entry;
            entry.contents.m_data = m_contents.m_data;
            entry.contents.m_destructor = std::exchange(m_contents.m_destructor, nullptr);
            entry.contents.m_shared = m_contents.m_shared;
            entry.contents.m_memoryHandle = m_contents.m_memoryHandle;
            WTF::atomicStore(&entry.contents.m_sizeInBytes, oldSizeInBytes, std::memory_order_relaxed);
            entry.contents.m_maxByteLength = oldMaxByteLength;
            entry.contents.m_hasMaxByteLength = m_contents.m_hasMaxByteLength;

            // At the stop: clear/poison the stale base word (and drop the
            // source's co-refs) before the mapping is released. Raw pointer +
            // generation, re-validated against the table under its lock — NOT
            // a Ref keepalive (ArrayBuffer's refcount is a plain uint32_t;
            // see the table comment). If this buffer died before the stop,
            // ~ArrayBuffer removed the table entry (or a recycled pointer got
            // a fresh generation) and the closure no-ops; the entry's own
            // refs/ownership still free the mapping at the stop.
            entry.clearBaseWordAtStop = [buffer = this, generation] {
                GILOffDetachedBufferTable& table = gilOffDetachedBufferTable();
                Locker locker { table.lock };
                auto iterator = table.map.find(buffer);
                if (iterator == table.map.end() || iterator->value != generation)
                    return;
                buffer->m_contents.m_data = nullptr;
                buffer->m_contents.m_shared = nullptr;
                buffer->m_contents.m_memoryHandle = nullptr;
                // Past this point !m_data is true, so the header's landed
                // isDetached() predicate takes over; drop the table entry.
                table.map.remove(iterator);
                s_gilOffDetachedPendingCount.fetch_sub(1, std::memory_order_release);
            };

            enqueueArrayBufferQuarantineEntry(vm.heap, WTF::move(entry), retainedBytes);
        }

        // notifyDetaching/neutering watchpoints fire as landed —
        // hoisted-vector code jettisons; the quarantine additionally covers
        // code that raced the jettison.
        notifyDetaching(vm);
        return;
    }

    auto unused = m_contents.detach();
    notifyDetaching(vm);
}

// GIL-off, this buffer's incoming-reference storage is concurrently mutated
// by other threads creating views on it (Heap::addReference under the
// GCIncomingRefCountedSet lock), so all reads of that storage must also be
// under that lock — the lock-free walk was the §3.27 r0 UAF signature
// (Vector realloc / singleton->Vector repoint vs reader). The set lock is a
// strict heap-rank leaf, so we must NOT call back into view code while
// holding it (detachFromArrayBuffer takes the view's cellLock); instead we
// snapshot the cell list under the lock and walk the snapshot. The snapshot
// stays valid for the walk: this mutator is between safepoints, so the
// stop-the-world collector cannot run, and incoming references are only
// removed at GC sweep / lastChanceToFinalize — exactly the lifetime argument
// the old unlocked walk already relied on.
Vector<JSCell*, 8> ArrayBuffer::snapshotIncomingReferences(VM& vm)
{
    Vector<JSCell*, 8> incomingReferences;
    {
        Locker locker { vm.heap.arrayBufferIncomingReferencesLock() };
        size_t count = numberOfIncomingReferences();
        incomingReferences.reserveInitialCapacity(count);
        for (size_t i = 0; i < count; ++i)
            incomingReferences.append(incomingReferenceAt(i));
    }
    return incomingReferences;
}

void ArrayBuffer::notifyDetaching(VM& vm)
{
    auto incomingReferences = snapshotIncomingReferences(vm);
    for (size_t i = incomingReferences.size(); i--;) {
        JSCell* cell = incomingReferences[i];
        if (JSArrayBufferView* view = dynamicDowncast<JSArrayBufferView>(cell))
            view->detachFromArrayBuffer();
    }
    m_detachingWatchpointSet.fireAll(vm, "Array buffer was detached");
}

// Wasm JS API redefines the abstract operation HostGrowSharedArrayBuffer as follows:
// https://webassembly.github.io/threads/js-api/index.html#abstract-operation-hostgrowsharedarraybuffer
Expected<int64_t, GrowFailReason> ArrayBuffer::grow(VM& vm, size_t newByteLength)
{
    auto shared = m_contents.m_shared;
    if (!shared) [[unlikely]]
        return makeUnexpected(GrowFailReason::GrowSharedUnavailable);
    const bool requirePageMultiple = isWasmMemory();
    auto result = shared->grow(vm, newByteLength, requirePageMultiple);
    if (result && result.value() > 0)
        vm.heap.reportExtraMemoryAllocated(static_cast<JSCell*>(nullptr), result.value());
    return result;
}

// SPEC-ungil annex N6 arms 3-4 (SHRINK / GROW of a resizable non-shared
// buffer), GIL-off only. Mirrors the landed ArrayBuffer::resize shape, with
// two deltas:
// - SHRINK (arm 3): under the handle lock compute desiredSize as landed, but
//   DO NOT protect/freePhysicalBytes/updateSize on the resizing thread — the
//   tail range is appended to the per-server quarantine as a page-range entry
//   and retired (protect + decommit + updateSize) at the next heap §10 stop
//   under quiescence. The length is published seq_cst. VA is already reserved
//   to maxByteLength (tryAllocateResizableMemory), so the deferral costs only
//   physical-page residency until the stop.
// - GROW (arm 4): the base is IMMUTABLE — in-place only via the reserved VA.
//   Pending tail entries are consumed/cancelled FIRST (before any
//   allocation/GC-capable call, so no collection can run under this handle
//   lock while a tail entry for it is pending), then pages are committed,
//   THEN the larger length is release-published — both torn pairs index the
//   one immutable mapping.
// `sizeInBytesSlot` is &m_contents.m_sizeInBytes (passed by the member, which
// has the friend access); `buffer` is used for its public data() only.
static Expected<int64_t, GrowFailReason> resizeGILOff(VM& vm, ArrayBuffer& buffer, BufferMemoryHandle& memoryHandle, size_t* sizeInBytesSlot, size_t maxByteLength, bool isWasmMemoryBuffer, size_t newByteLength)
{
    int64_t deltaByteLength = 0;
    size_t newlyQuarantinedBytes = 0;

    // §10.4 deadlock avoidance: tryAllocate may conduct a SYNCHRONOUS FULL
    // COLLECTION (SyncTryToReclaimMemory). GIL-off/shared-heap, collectSync's
    // access barrier waits for EVERY client to reach NoAccess — but a sibling
    // mutator blocked on THIS handle lock (a racing resize, or detach, which
    // publishes under it) parks inside WTF::Lock still holding heap access,
    // never polls traps, and wedges the barrier into the 30s stop watchdog.
    // So the handle lock is NEVER held across tryAllocate: physical bytes are
    // pre-allocated OUTSIDE the lock and the locked section re-validates from
    // scratch on every iteration (size/detach may have changed while
    // unlocked). Leftover preallocation is refunded on every exit.
    size_t preallocatedBytes = 0;
    auto refundPreallocation = WTF::makeScopeExit([&] {
        if (preallocatedBytes)
            BufferMemoryManager::singleton().freePhysicalBytes(preallocatedBytes);
    });

    for (;;) {
        size_t bytesNeeded = 0;
        {
            Locker locker { memoryHandle.lock() };

            // detach() publishes its flag + zero length under THIS lock, so this
            // re-check fully serializes resize-vs-detach: if the flag is visible
            // we fail before mutating anything (no resurrected nonzero length on
            // a detached buffer); if it is not, any concurrent detach is ordered
            // AFTER our publish below and its zero length wins. GIL-on a detached
            // buffer fails the caller's null-m_memoryHandle check instead, with
            // the same error.
            if (isArrayBufferDetachedGILOff(&buffer))
                return makeUnexpected(GrowFailReason::GrowSharedUnavailable);

            // Keep in mind that newByteLength may not be page-size-aligned.
            if (maxByteLength < newByteLength)
                return makeUnexpected(GrowFailReason::InvalidGrowSize);

            size_t sizeInBytes = WTF::atomicLoad(sizeInBytesSlot, std::memory_order_relaxed);
            deltaByteLength = static_cast<int64_t>(newByteLength) - static_cast<int64_t>(sizeInBytes);
#if ENABLE(WEBASSEMBLY)
            if (Options::useWasmMemoryToBufferAPIs()) {
                // The wasm isWasmMemory() delta<0 rejection stands (annex N6).
                if (isWasmMemoryBuffer && (deltaByteLength < 0 || deltaByteLength % PageCount::pageSize))
                    return makeUnexpected(GrowFailReason::InvalidGrowSize);
            }
#else
            UNUSED_PARAM(isWasmMemoryBuffer);
#endif
            if (!deltaByteLength)
                return 0;

            auto newPageCount = PageCount::fromBytesWithRoundUp(newByteLength);
            if (newPageCount.bytes() > MAX_ARRAY_BUFFER_SIZE)
                return makeUnexpected(GrowFailReason::WouldExceedMaximum);

            size_t desiredSize = newPageCount.bytes();
            RELEASE_ASSERT(desiredSize <= MAX_ARRAY_BUFFER_SIZE);

            if (deltaByteLength > 0) {
                // Arm 4 + the arm 3 re-grow rule: consume/cancel overlapping
                // pending tail entries BEFORE committing pages (the stop hook
                // takes the handle lock to retire tails; while we hold the
                // lock no hook can retire underneath us, and no GC-capable
                // call runs under this hold — allocation happens unlocked
                // below).
                consumeQuarantinedTailOnRegrow(vm.heap, memoryHandle, desiredSize);

                if (desiredSize > memoryHandle.size()) {
                    ASSERT(memoryHandle.maximum() >= newPageCount);
                    size_t bytesToAdd = desiredSize - memoryHandle.size();
                    ASSERT(bytesToAdd);
                    ASSERT(roundUpToMultipleOf<PageCount::pageSize>(bytesToAdd) == bytesToAdd);
                    if (preallocatedBytes < bytesToAdd) {
                        // Not enough pre-allocated: drop the lock, allocate,
                        // retake and re-validate everything.
                        bytesNeeded = bytesToAdd;
                    } else {
                        void* memory = memoryHandle.memory();
                        RELEASE_ASSERT(memory);

                        // The VA was pre-reserved to maxByteLength: commit in
                        // place, consuming the preallocated accounting.
                        preallocatedBytes -= bytesToAdd;
                        uint8_t* startAddress = static_cast<uint8_t*>(memory) + memoryHandle.size();
                        dataLogLnIf(ArrayBufferInternal::verbose, "Marking memory's ", RawPointer(memory), " as read+write in range [", RawPointer(startAddress), ", ", RawPointer(startAddress + bytesToAdd), ")");
                        constexpr bool readable = true;
                        constexpr bool writable = true;
                        OSAllocator::protect(startAddress, bytesToAdd, readable, writable);
                        memoryHandle.updateSize(desiredSize);
                    }
                }
                // else: re-grow within still-committed pages (a pending tail
                // was consumed/trimmed above) — pages still committed =>
                // zeroFill as landed, below.

                if (!bytesNeeded) {
                    // Commit the new pages, THEN release-publish the larger length.
                    zeroFill(static_cast<uint8_t*>(buffer.data()) + sizeInBytes, newByteLength - sizeInBytes);
                    WTF::atomicStore(sizeInBytesSlot, newByteLength, std::memory_order_release);
                    break;
                }
            } else {
                // Arm 3: defer the tail [desiredSize, handle size) to the next
                // stop; publish the smaller length seq_cst. Racing readers see
                // {oldLen, base} (stale-but-safe: tail still committed) or
                // {newLen, base} (in-bounds).
                if (desiredSize < memoryHandle.size())
                    newlyQuarantinedBytes = deferShrinkTailGILOff(vm.heap, memoryHandle, desiredSize);
                WTF::atomicStore(sizeInBytesSlot, newByteLength, std::memory_order_seq_cst);
                break;
            }
        }

        // UNLOCKED: top up the physical-bytes preallocation. May conduct a
        // synchronous collection — safe here, no handle lock held.
        ASSERT(bytesNeeded > preallocatedBytes);
        size_t topUp = bytesNeeded - preallocatedBytes;
        bool allocationSuccess = tryAllocate(&vm,
            [&] () -> BufferMemoryResult::Kind {
                return BufferMemoryManager::singleton().tryAllocatePhysicalBytes(topUp);
            });
        if (!allocationSuccess)
            return makeUnexpected(GrowFailReason::OutOfMemory);
        preallocatedBytes += topUp;
    }

    // Extra-memory accounting outside the handle lock (it may conduct a
    // collection, whose hook retires tail entries under the handle lock).
    if (deltaByteLength > 0)
        vm.heap.reportExtraMemoryAllocated(static_cast<JSCell*>(nullptr), deltaByteLength);
    else if (newlyQuarantinedBytes)
        vm.heap.reportExtraMemoryAllocated(static_cast<JSCell*>(nullptr), newlyQuarantinedBytes);

    return deltaByteLength;
}

// Wasm JS API redefines the abstract operation HostResizeArrayBuffer as follows:
// https://webassembly.github.io/threads/js-api/index.html#abstract-operation-hostresizearraybuffer
Expected<int64_t, GrowFailReason> ArrayBuffer::resize(VM& vm, size_t newByteLength)
{
    auto memoryHandle = m_contents.m_memoryHandle;
    if (!memoryHandle || m_contents.m_shared) [[unlikely]]
        return makeUnexpected(GrowFailReason::GrowSharedUnavailable);

    if (gilOffThreadsProcess()) [[unlikely]] {
        // Annex N6 arm 1: the flag — not !m_data — is the detached predicate.
        // This check is advisory (TOCTOU); the binding one is resizeGILOff's
        // re-check under the handle lock, which detach() publishes under.
        if (isArrayBufferDetachedGILOff(this))
            return makeUnexpected(GrowFailReason::GrowSharedUnavailable);
        // Atomic: a concurrent detach() zeroes m_maxByteLength with an
        // atomic store, and we are outside the handle lock here.
        size_t maxByteLength = WTF::atomicLoad(&m_contents.m_maxByteLength, std::memory_order_relaxed);
#if ENABLE(WEBASSEMBLY)
        // The associated-wasm-memory delegation (landed: a page-count-changing
        // resize of a buffer associated with a non-shared Wasm memory routes
        // through the memory, which calls back refreshAfterWasmMemoryGrow) is
        // decided here, OUTSIDE the handle lock: Wasm::Memory::grow takes the
        // handle's lock itself, and annex N6 arm 4 requires a relocating
        // BoundsChecking grow to run under a heap §10 stop — neither may
        // happen under our hold of the same lock. (The stop conduction on the
        // wasm side is an OPEN DEPENDENCY — see
        // ArrayBufferContents::refreshAfterWasmMemoryGrow.)
        if (Options::useWasmMemoryToBufferAPIs()) {
            if (maxByteLength < newByteLength)
                return makeUnexpected(GrowFailReason::InvalidGrowSize);
            size_t currentSizeInBytes = WTF::atomicLoad(&m_contents.m_sizeInBytes, std::memory_order_relaxed);
            int64_t deltaByteLength = static_cast<int64_t>(newByteLength) - static_cast<int64_t>(currentSizeInBytes);
            if (isWasmMemory() && (deltaByteLength < 0 || deltaByteLength % PageCount::pageSize))
                return makeUnexpected(GrowFailReason::InvalidGrowSize);
            if (deltaByteLength) {
                auto newPageCount = PageCount::fromBytesWithRoundUp(newByteLength);
                auto oldPageCount = PageCount::fromBytes(memoryHandle->size());
                if (newPageCount.bytes() > MAX_ARRAY_BUFFER_SIZE)
                    return makeUnexpected(GrowFailReason::WouldExceedMaximum);
                if (newPageCount != oldPageCount) {
                    if (RefPtr<Wasm::Memory> memory = m_associatedWasmMemory.get()) {
                        std::ignore = memory->grow(vm, PageCount(newPageCount.pageCount() - oldPageCount.pageCount()));
                        return deltaByteLength;
                    }
                }
            }
        }
#endif
        return resizeGILOff(vm, *this, *memoryHandle, &m_contents.m_sizeInBytes, maxByteLength, isWasmMemory(), newByteLength);
    }

    int64_t deltaByteLength = 0;
    {
        Locker { memoryHandle->lock() };

        // Keep in mind that newByteLength may not be page-size-aligned.
        if (m_contents.m_maxByteLength < newByteLength)
            return makeUnexpected(GrowFailReason::InvalidGrowSize);

        deltaByteLength = static_cast<int64_t>(newByteLength) - static_cast<int64_t>(WTF::atomicLoad(&m_contents.m_sizeInBytes, std::memory_order_relaxed));
#if ENABLE(WEBASSEMBLY)
        if (Options::useWasmMemoryToBufferAPIs()) {
            if (isWasmMemory() && (deltaByteLength < 0 || deltaByteLength % PageCount::pageSize))
                return makeUnexpected(GrowFailReason::InvalidGrowSize);
        }
#endif
        if (!deltaByteLength)
            return 0;

        auto newPageCount = PageCount::fromBytesWithRoundUp(newByteLength);
        auto oldPageCount = PageCount::fromBytes(memoryHandle->size()); // MemoryHandle's size is always page-size aligned.
        if (newPageCount.bytes() > MAX_ARRAY_BUFFER_SIZE)
            return makeUnexpected(GrowFailReason::WouldExceedMaximum);

        if (newPageCount != oldPageCount) {
            ASSERT(memoryHandle->maximum() >= newPageCount);

#if ENABLE(WEBASSEMBLY)
            if (Options::useWasmMemoryToBufferAPIs()) {
                // If this is currently associated with a Wasm memory, let the memory do the growing.
                // The memory will call back to our refreshAfterWasmMemoryGrow().
                RefPtr<Wasm::Memory> memory = m_associatedWasmMemory.get();
                if (memory) {
                    std::ignore = memory->grow(vm, PageCount(newPageCount.pageCount() - oldPageCount.pageCount()));
                    return deltaByteLength;
                }
            }
#endif
            size_t desiredSize = newPageCount.bytes();
            RELEASE_ASSERT(desiredSize <= MAX_ARRAY_BUFFER_SIZE);

            if (desiredSize > memoryHandle->size()) {
                size_t bytesToAdd = desiredSize - memoryHandle->size();
                ASSERT(bytesToAdd);
                ASSERT(roundUpToMultipleOf<PageCount::pageSize>(bytesToAdd) == bytesToAdd);
                bool allocationSuccess = tryAllocate(&vm,
                    [&] () -> BufferMemoryResult::Kind {
                        return BufferMemoryManager::singleton().tryAllocatePhysicalBytes(bytesToAdd);
                    });
                if (!allocationSuccess)
                    return makeUnexpected(GrowFailReason::OutOfMemory);

                void* memory = memoryHandle->memory();
                RELEASE_ASSERT(memory);

                // Signaling memory must have been pre-allocated virtually.
                uint8_t* startAddress = static_cast<uint8_t*>(memory) + memoryHandle->size();

                dataLogLnIf(ArrayBufferInternal::verbose, "Marking memory's ", RawPointer(memory), " as read+write in range [", RawPointer(startAddress), ", ", RawPointer(startAddress + bytesToAdd), ")");
                constexpr bool readable = true;
                constexpr bool writable = true;
                OSAllocator::protect(startAddress, bytesToAdd, readable, writable);
            } else {
                size_t bytesToSubtract = memoryHandle->size() - desiredSize;
                ASSERT(bytesToSubtract);
                ASSERT(roundUpToMultipleOf<PageCount::pageSize>(bytesToSubtract) == bytesToSubtract);
                BufferMemoryManager::singleton().freePhysicalBytes(bytesToSubtract);

                void* memory = memoryHandle->memory();
                RELEASE_ASSERT(memory);

                // Signaling memory must have been pre-allocated virtually.
                uint8_t* startAddress = static_cast<uint8_t*>(memory) + desiredSize;

                dataLogLnIf(ArrayBufferInternal::verbose, "Marking memory's ", RawPointer(memory), " as none in range [", RawPointer(startAddress), ", ", RawPointer(startAddress + bytesToSubtract), ")");
                constexpr bool readable = false;
                constexpr bool writable = false;
                OSAllocator::protect(startAddress, bytesToSubtract, readable, writable);
            }
            memoryHandle->updateSize(desiredSize);
        }

        size_t oldByteLength = WTF::atomicLoad(&m_contents.m_sizeInBytes, std::memory_order_relaxed);
        if (oldByteLength < newByteLength)
            zeroFill(std::bit_cast<uint8_t*>(data()) + oldByteLength, newByteLength - oldByteLength);

        WTF::atomicStore(&m_contents.m_sizeInBytes, newByteLength, std::memory_order_relaxed);
    }

    if (deltaByteLength > 0)
        vm.heap.reportExtraMemoryAllocated(static_cast<JSCell*>(nullptr), deltaByteLength);

    return deltaByteLength;
}

RefPtr<ArrayBuffer> ArrayBuffer::tryCreateShared(VM& vm, size_t numElements, unsigned elementByteSize, size_t maxByteLength)
{
    CheckedSize sizeInBytes = numElements;
    sizeInBytes *= elementByteSize;
    if (sizeInBytes.hasOverflowed() || sizeInBytes.value() > maxByteLength || maxByteLength > MAX_ARRAY_BUFFER_SIZE)
        return nullptr;

    auto handle = tryAllocateResizableMemory(&vm, sizeInBytes.value(), maxByteLength);
    if (!handle)
        return nullptr;

    auto* memory = static_cast<uint8_t*>(handle->memory());
    return createShared(SharedArrayBufferContents::create({ memory, sizeInBytes.value() }, maxByteLength, WTF::move(handle), nullptr, SharedArrayBufferContents::Mode::Default));
}

ArrayBuffer::~ArrayBuffer()
{
    // Annex N6 arm 1: a buffer that dies between its GIL-off detach and the
    // retiring stop must neuter its pending stop-time clear (the closure
    // re-validates {pointer, generation} against this table). The mapping
    // itself is unaffected — the quarantine entry owns/co-owns it and still
    // frees it at the stop. Cost when no detach is pending: one atomic load.
    if (gilOffThreadsProcess()) [[unlikely]]
        unregisterDetachedArrayBufferGILOff(this);
}

Expected<int64_t, GrowFailReason> SharedArrayBufferContents::grow(VM& vm, size_t newByteLength, bool requirePageMultiple)
{
    if (!m_hasMaxByteLength)
        return makeUnexpected(GrowFailReason::GrowSharedUnavailable);
    ASSERT(m_memoryHandle);
    return grow(Locker { m_memoryHandle->lock() }, vm, newByteLength, requirePageMultiple);
}

Expected<int64_t, GrowFailReason> SharedArrayBufferContents::grow(const AbstractLocker& locker, VM& vm, size_t newByteLength, bool requirePageMultiple)
{
    // Keep in mind that newByteLength may not be page-size-aligned. If the buffer is a Wasm memory, that is an error.
    size_t sizeInBytes = m_sizeInBytes.load(std::memory_order_seq_cst);
    if (sizeInBytes > newByteLength || m_maxByteLength < newByteLength)
        return makeUnexpected(GrowFailReason::InvalidGrowSize);

    int64_t deltaByteLength = newByteLength - sizeInBytes;

#if ENABLE(WEBASSEMBLY)
    if (Options::useWasmMemoryToBufferAPIs()) {
        if (requirePageMultiple && deltaByteLength % PageCount::pageSize)
            return makeUnexpected(GrowFailReason::InvalidGrowSize);
    }
#else
    UNUSED_PARAM(requirePageMultiple);
#endif

    if (!deltaByteLength)
        return 0;

    auto newPageCount = PageCount::fromBytesWithRoundUp(newByteLength);
    auto oldPageCount = PageCount::fromBytes(m_memoryHandle->size()); // MemoryHandle's size is always page-size aligned.
    if (newPageCount.bytes() > MAX_ARRAY_BUFFER_SIZE)
        return makeUnexpected(GrowFailReason::WouldExceedMaximum);

    RefPtr memoryHandle = m_memoryHandle;
    if (newPageCount != oldPageCount) {
        ASSERT(memoryHandle->maximum() >= newPageCount);
        size_t desiredSize = newPageCount.bytes();
        RELEASE_ASSERT(desiredSize <= MAX_ARRAY_BUFFER_SIZE);
        RELEASE_ASSERT(desiredSize > memoryHandle->size());

        size_t extraBytes = desiredSize - memoryHandle->size();
        RELEASE_ASSERT(extraBytes);
        bool allocationSuccess = tryAllocate(&vm,
            [&] () -> BufferMemoryResult::Kind {
                return BufferMemoryManager::singleton().tryAllocatePhysicalBytes(extraBytes);
            });
        if (!allocationSuccess)
            return makeUnexpected(GrowFailReason::OutOfMemory);

        void* memory = memoryHandle->memory();
        RELEASE_ASSERT(memory);

        // Signaling memory must have been pre-allocated virtually.
        uint8_t* startAddress = static_cast<uint8_t*>(memory) + memoryHandle->size();

        dataLogLnIf(ArrayBufferInternal::verbose, "Marking memory's ", RawPointer(memory), " as read+write in range [", RawPointer(startAddress), ", ", RawPointer(startAddress + extraBytes), ")");
        constexpr bool readable = true;
        constexpr bool writable = true;
        OSAllocator::protect(startAddress, extraBytes, readable, writable);
        memoryHandle->updateSize(desiredSize);
    }

    zeroFill(std::bit_cast<uint8_t*>(data()) + sizeInBytes, newByteLength - sizeInBytes);

    updateSize(newByteLength);

    UNUSED_PARAM(locker);
#if ENABLE(WEBASSEMBLY)
    // Update cache for instance
    for (Ref anchor : memoryHandle->anchors(locker)) {
        Locker locker { anchor->m_lock };
        if (JSWebAssemblyInstance* instance = anchor->instance())
            instance->updateCachedMemories();
    }
#endif
    return deltaByteLength;
}

ASCIILiteral errorMessageForTransfer(ArrayBuffer* buffer)
{
    ASSERT(buffer->isLocked());
    if (buffer->isShared())
        return "Cannot transfer a SharedArrayBuffer"_s;
    if (buffer->isWasmMemory())
        return "Cannot transfer a WebAssembly.Memory"_s;
    return "Cannot transfer an ArrayBuffer whose backing store has been accessed by the JavaScriptCore C API"_s;
}

std::optional<ArrayBufferContents> ArrayBufferContents::fromSpan(std::span<const uint8_t> data)
{
    void* buffer = Gigacage::tryMalloc(Gigacage::Primitive, data.size_bytes());
    if (!buffer)
        return std::nullopt;

    memcpy(buffer, data.data(), data.size_bytes());

    return ArrayBufferContents { buffer, data.size_bytes(), std::nullopt, ArrayBuffer::primitiveGigacageDestructor() };
}

void ArrayBufferContents::refreshAfterWasmMemoryGrow(Wasm::Memory* memory)
{
#if ENABLE(WEBASSEMBLY)
    ASSERT(isResizableNonShared());
    // If the memory is BoundChecking, the memory's handle is replaced with a different one when it grows.
    //
    // GIL-off (SPEC-ungil annex N6 arm 4): a Signaling/reserved-VA grow keeps
    // the base immutable (m_data unchanged here), so the only published
    // mutation is the larger length — committed pages happen-before it via
    // the release store below.
    //
    // A relocating BoundsChecking grow REPLACES the base word. Annex N6
    // requires that relocation to run under a heap §10 stop, with the old
    // mapping quarantined to the NEXT stop for captured/hoisted bases. The
    // stop conduction belongs to the wasm grow path (Wasm::Memory::grow's
    // MemoryMode::BoundsChecking arm) and is NOT YET ESTABLISHED there —
    // OPEN DEPENDENCY, blocks U-T13 sign-off: until it lands, a GIL-off
    // relocating grow racing a reader can pair a post-grow length with the
    // pre-grow base (the reader's two loads carry no ordering). What this
    // file CAN own is the quarantine half, below: the replaced handle is kept
    // alive until a stop, so the complementary torn pair
    // {pre-grow length, pre-grow base} never dereferences an unmapped base.
    if (gilOffThreadsProcess()) [[unlikely]] {
        RefPtr<BufferMemoryHandle> oldHandle = m_memoryHandle;
        m_memoryHandle = memory->handle();
        m_data = memory->basePointer();
        if (oldHandle && oldHandle != m_memoryHandle)
            quarantineStaleWasmMappingGILOff(oldHandle.releaseNonNull());
        WTF::atomicStore(&m_sizeInBytes, m_memoryHandle->size(), std::memory_order_release);
    } else {
        m_memoryHandle = memory->handle();
        m_data = memory->basePointer();
        WTF::atomicStore(&m_sizeInBytes, m_memoryHandle->size(), std::memory_order_relaxed);
    }
#else
    UNUSED_PARAM(memory);
#endif
}


} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
