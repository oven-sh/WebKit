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
#include "SharedHeapTestHarness.h"

#include "AllocatorInlines.h"
#include "CompleteSubspaceInlines.h"
#include "GCSafepointEpoch.h"
#include "GCThreadLocalCache.h"
#include "Heap.h"
#include "HeapClientSet.h"
#include "HeapInlines.h"
#include "LocalAllocatorInlines.h"
#include "MachineStackMarker.h"
#include "MarkedBlockInlines.h"
#include "MarkedSpace.h"
#include "Options.h"
#include "ReleaseHeapAccessScope.h"
#include <algorithm>
#include <atomic>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <iterator>
#include <new>
#include <wtf/DataLog.h>
#include <wtf/Seconds.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>

namespace JSC {

// FIXME(fix-shared-heap-ring-liveness-5): TEMPORARY round-5 diagnostic seam,
// defined in Heap.cpp next to sharedGCStackRootSnapshot() (deliberately no
// header change — strip both sides together). Heap::gatherStackRoots calls
// it at the end of every shared-mode gather with the gather's full
// conservative root set, world stopped for all clients. Atomic because the
// gather can run on a parallel marker helper created before the store.
extern std::atomic<void (*)(Heap&, HeapCell**, size_t)> g_sharedGCConservativeRootAuditHook;

namespace {

// §12.1: K raw WTF::Threads. Clamped so a misbehaving JS caller can't fork
// bomb; every scenario tolerates the clamp.
constexpr unsigned maxHarnessThreads = 16;

// Deterministic per-(thread, salt) fill pattern; never 0 so an uninitialized
// (or lost-handout-zeroed) cell can't verify by accident.
uint64_t mixPattern(unsigned threadIndex, uint64_t salt)
{
    uint64_t x = (static_cast<uint64_t>(threadIndex + 1) << 56) ^ (salt + 1) * 0x9E3779B97F4A7C15ULL;
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33;
    return x | 1;
}

// Fill/verify a scratch cell. Cells come from the server's auxiliarySpace
// (HeapCell::Auxiliary, DoesNotNeedDestruction): the GC never interprets
// their payload, so arbitrary byte patterns are safe; conservative scanning
// keeps stack-referenced cells alive across collections (I12).
constexpr size_t maxPatternWords = 32;

void fillCell(void* cell, size_t size, uint64_t pattern)
{
    auto* words = static_cast<uint64_t*>(cell);
    size_t wordCount = size / sizeof(uint64_t);
    size_t prefix = std::min(wordCount, maxPatternWords);
    for (size_t i = 0; i < prefix; ++i)
        words[i] = pattern + i;
    if (wordCount)
        words[wordCount - 1] = pattern ^ 0x5A5A5A5A5A5A5A5AULL;
}

// I1 detector: if two threads were ever handed the same cell (or a stolen
// block was allocatable in two directories, I8), one thread's pattern stomps
// the other's and this fails.
bool verifyCell(const void* cell, size_t size, uint64_t pattern)
{
    auto* words = static_cast<const uint64_t*>(cell);
    size_t wordCount = size / sizeof(uint64_t);
    size_t prefix = std::min(wordCount, maxPatternWords);
    for (size_t i = 0; i < prefix; ++i) {
        if (i == wordCount - 1)
            break; // Tail word holds the xor stamp.
        if (words[i] != pattern + i)
            return false;
    }
    if (wordCount && words[wordCount - 1] != (pattern ^ 0x5A5A5A5A5A5A5A5AULL))
        return false;
    return true;
}

// Stack-resident retention ring: the cells[] array lives in the harness
// thread's frame, so the §10.6 conservative scan (suspend-and-copy of every
// I4(b)-registered thread, I12) keeps every retained cell alive across
// conducted collections. Never heap-allocate this (a Vector's backing store
// is malloc memory and would NOT be scanned).
struct ScratchRing;

// FIXME(fix-shared-heap-corruption): TEMPORARY round-3 diagnostic — strip
// before declaring the gates green. Registry of every live stack ring.
// Mutators only append/remove under the lock at ring construction/
// destruction (never while parked), so the world-stopped hook below cannot
// deadlock against a stopped holder; ring CONTENTS are only mutated by the
// owning thread while it holds its client's heap access, so reading them
// world-stopped is race-free.
static Lock s_ringRegistryLock;
static Vector<ScratchRing*>& ringRegistry() WTF_REQUIRES_LOCK(s_ringRegistryLock)
{
    static NeverDestroyed<Vector<ScratchRing*>> registry;
    return registry.get();
}

struct ScratchRing {
    static constexpr unsigned capacity = 32;
    void* cells[capacity] = {};
    size_t sizes[capacity] = {};
    uint64_t patterns[capacity] = {};
    unsigned cursor { 0 };

    // FIXME(fix-shared-heap-ring-liveness-5): TEMPORARY round-5 diagnostic —
    // the owning thread, captured for the gather-time audit (see
    // auditRingsAgainstGatheredRoots). The ring is stack-resident in the
    // owner's frame, so the owner strictly outlives the ring and a raw
    // pointer is safe; written once at construction, read only while the
    // world is stopped for all clients.
    Thread* owner { nullptr };

    ScratchRing()
    {
        owner = &Thread::currentSingleton();
        Locker locker { s_ringRegistryLock };
        ringRegistry().append(this);
    }

    ~ScratchRing()
    {
        Locker locker { s_ringRegistryLock };
        ringRegistry().removeFirst(this);
    }

    // Verify-then-replace one slot; RELEASE_ASSERTs pattern integrity (I1).
    void step(void* newCell, size_t newSize, uint64_t newPattern)
    {
        unsigned slot = cursor++ % capacity;
        if (cells[slot])
            RELEASE_ASSERT(verifyCell(cells[slot], sizes[slot], patterns[slot]));
        cells[slot] = newCell;
        sizes[slot] = newSize;
        patterns[slot] = newPattern;
    }

    void verifyAll() const
    {
        for (unsigned i = 0; i < capacity; ++i) {
            if (cells[i])
                RELEASE_ASSERT(verifyCell(cells[i], sizes[i], patterns[i]));
        }
    }

    void clear()
    {
        for (unsigned i = 0; i < capacity; ++i)
            cells[i] = nullptr;
    }
};

// FIXME(fix-shared-heap-ring-liveness-6): ASAN's use-after-return mode
// (default-on in Linux clang ASAN builds; only the Cocoa port passes
// -fsanitize-address-use-after-return=never, see OptionsCocoa.cmake)
// relocates ordinary locals of instrumented frames into a heap-backed "fake
// stack" frame that lies OUTSIDE Thread::stack() bounds, so the §10.6
// suspend-and-copy conservative scan never reads it. A plain
// `ScratchRing ring` is therefore NOT stack-resident under ASAN — exactly
// the round-5 audit verdict (owner registered in MachineThreads,
// ringWithinOwnerStackBounds = false on every flagged cell): the scan never
// had a chance, the cells end the conducted cycle with stale marks AND stale
// newlyAllocated bits, and the round-3 safepoint hook traps. Only a
// VARIABLE-size (dynamic) alloca is never relocated to the fake stack —
// round 6 showed empirically that a constant-size __builtin_alloca is NOT
// enough (see FIXME(fix-shared-heap-ring-liveness-8) below) — so
// placement-constructing the ring in dynamically sized __builtin_alloca
// storage guarantees real-stack residency in every build mode — keeping the
// corpus an honest exercise of the I12 STACK clause (no
// synthetic root source is added; the §10.6 scan itself must produce the
// cells). NOTE: never expand DECLARE_STACK_SCRATCH_RING inside a loop body —
// alloca storage is only reclaimed on function return.
#if ASAN_ENABLED && defined(__GNUC__)
struct ScratchRingDestroyer {
    ScratchRing* ring;
    ~ScratchRingDestroyer() { ring->~ScratchRing(); }
};
// FIXME(fix-shared-heap-ring-liveness-8): the round-6 alloca arm passed a
// COMPILE-TIME-CONSTANT size, and clang lowers a constant-size
// __builtin_alloca as a STATIC alloca (static because every DECLARE site is
// its scope's first declaration, placing the alloca in the entry block; a
// future mid-function expansion would invalidate this analysis) — which
// ASAN's use-after-return mode relocates onto the heap-backed fake stack
// exactly like an ordinary local (the round-6 verdict:
// ringWithinOwnerStackBounds stayed false). Only VARIABLE-SIZE allocas are
// lowered as true dynamic allocas, which ASAN instruments in place
// (__asan_alloca_poison) but never relocates, so the ring storage stays
// inside Thread::stack() bounds in every ASAN mode. The volatile read makes
// the size opaque to constant folding at any optimization level. The §10.6
// suspend-and-copy scan reads the region via SUPPRESS_ASAN copyMemory, so
// the surrounding alloca redzone poison is harmless.
#define DECLARE_STACK_SCRATCH_RING(name) \
    volatile size_t name##AllocaSize = sizeof(ScratchRing); \
    ScratchRing& name = *new (__builtin_alloca(name##AllocaSize)) ScratchRing; \
    ScratchRingDestroyer name##Destroyer { &name }
#else
#define DECLARE_STACK_SCRATCH_RING(name) ScratchRing name
#endif

void* allocateScratch(JSC::Heap& server, GCClient::Heap& client, size_t size)
{
    // §12.1 seam: client-TLC routing, no VM-coupled preludes; CIND/SINFAC ARE
    // called inside (so pure allocation loops poll the §10A stop protocol).
    return server.auxiliarySpace.allocateForClient(client, size, nullptr, AllocationFailureMode::ReturnNull);
}

void allocateBurst(JSC::Heap& server, GCClient::Heap& client, ScratchRing& ring, unsigned threadIndex, uint64_t salt, unsigned count, const size_t* sizes, unsigned sizeCount)
{
    for (unsigned i = 0; i < count; ++i) {
        size_t size = sizes[(threadIndex + i) % sizeCount];
        uint64_t pattern = mixPattern(threadIndex, salt * 1024 + i);
        void* cell = allocateScratch(server, client, size);
        if (!cell)
            continue; // ReturnNull: OOM-tolerant; the storm goes on.
        fillCell(cell, size, pattern);
        ring.step(cell, size, pattern);
    }
}

constexpr size_t smallSizes[] = { 16, 32, 48, 64, 96, 128, 176, 256, 504, 1024 };
constexpr unsigned smallSizeCount = std::size(smallSizes);

// > MarkedSpace::largeCutoff and > Options::preciseAllocationCutoff():
// guaranteed to take the §5.6 precise path (I16).
constexpr size_t preciseSizes[] = { 64 * 1024, 100 * 1024, 200 * 1024 };
constexpr unsigned preciseSizeCount = std::size(preciseSizes);

// Spin barrier whose waiters keep polling the §10A stop protocol: a waiter
// holds heap access, so it MUST keep servicing GSP (release/re-acquire in
// SINFAC) or it would deadlock a conductor's §10.4 access barrier.
struct PhaseBarrier {
    std::atomic<uint32_t> arrivals { 0 };
    std::atomic<uint32_t> generation { 0 };

    void arrive(JSC::Heap& server, uint32_t participants)
    {
        uint32_t generationBefore = generation.load();
        if (arrivals.fetch_add(1) + 1 == participants) {
            arrivals.store(0);
            generation.fetch_add(1);
            return;
        }
        while (generation.load() == generationBefore) {
            server.stopIfNecessaryForAllClients(); // I6: holds no lock while parked.
            Thread::yield();
        }
    }
};

// FIXME(fix-shared-heap-corruption): TEMPORARY round-3 diagnostic — strip
// before declaring the gates green. Runs inside
// Heap::runSafepointHooksAndReclaim() (world stopped, after the conducted
// drain's last endMarking, before any post-resume sweep can act on the
// cycle's liveness bits). Round-2's endMarking probe only checks cells that
// WERE gathered as conservative roots; this hook checks every cell every
// live harness ring ACTUALLY holds. A trap here with round-2 silent in the
// same run proves the §10.6 suspend-and-copy scan never produced the cell
// (stack-coverage miss — MachineThreads layer); silence here plus a later
// harness pattern assert proves a marked/newlyAllocated cell was
// subsequently handed out or freed (sweep/steal layer). Caveat (review): a
// trap with round-2 silent can also mean a cell wrongly freed by a sweep in
// an EARLIER stop — disambiguate via the logged marksStale/
// newlyAllocatedStale bits and the ASAN report, not the trap site alone.
static void verifyRingLivenessHook(JSC::Heap& heap)
{
    if (!heap.isSharedServer())
        return;
    HeapVersion markingVersion = heap.objectSpace().markingVersion();
    Locker locker { s_ringRegistryLock };
    for (ScratchRing* ring : ringRegistry()) {
        for (unsigned i = 0; i < ScratchRing::capacity; ++i) {
            void* pointer = ring->cells[i];
            if (!pointer)
                continue;
            auto* cell = static_cast<HeapCell*>(pointer);
            if (cell->isPreciseAllocation()) {
                if (!cell->preciseAllocation().isLive()) [[unlikely]] {
                    dataLogLn(
                        "SharedHeapTestHarness diagnostic (fix-shared-heap-corruption): ring-held precise cell ",
                        RawPointer(cell), " has no liveness at the stop-the-world safepoint.");
                    RELEASE_ASSERT_NOT_REACHED();
                }
                continue;
            }
            MarkedBlock& block = cell->markedBlock();
            bool marked = !block.areMarksStale(markingVersion) && block.isMarkedRaw(cell);
            bool newlyAllocated = !block.isNewlyAllocatedStale() && block.isNewlyAllocated(cell);
            if (!marked && !newlyAllocated) [[unlikely]] {
                dataLogLn(
                    "SharedHeapTestHarness diagnostic (fix-shared-heap-corruption): ring-held cell ",
                    RawPointer(cell), " in block ", RawPointer(&block),
                    " of directory ", RawPointer(block.handle().directory()),
                    " (cellSize = ", block.handle().directory()->cellSize(),
                    ") has no version-current liveness bit at the stop-the-world safepoint (marksStale = ",
                    block.areMarksStale(markingVersion),
                    ", newlyAllocatedStale = ", block.isNewlyAllocatedStale(), ")");
                RELEASE_ASSERT_NOT_REACHED();
            }
        }
    }
}

// FIXME(fix-shared-heap-ring-liveness-5): TEMPORARY round-5 diagnostic —
// strip before declaring the gates green. Review round 5 REJECTED landing an
// I12 m_currentBlock root walk before the producer-side question is
// answered: the round-3 trap (marksStale && newlyAllocatedStale) proves a
// ring-held, stack-resident cell ended a conducted cycle with no
// version-current liveness bit, but cannot say WHY the §10.6 suspend-and-
// copy scan never produced the pointer. ScratchRing::cells[] lives in an
// I4(b)-registered thread's frame, so a sound producer must yield every
// non-null ring cell as a conservative root each gather (the conservative
// filter accepts them: the step-5 flush stamps version-current newlyAllocated
// bits before gather, and MarkedBlock::Handle::isLive checks those first).
// This audit runs at the END of every shared-mode gatherStackRoots (world
// stopped for all clients, conductor context — ring contents and the
// registry are race-free to read, same argument as verifyRingLivenessHook)
// and, for each ring cell ABSENT from the just-gathered root set, logs the
// three discriminating facts the reviewers asked for:
//   (a) filter side — does the TinyBloomFilter rule the cell's block out, and
//       is the block even in the MarkedBlockSet? (true + missing-from-set =
//       block-publication race; ruled out + in-set = bloom false negative,
//       which must never happen — bloom filters have no false negatives —
//       so that combination indicts an unsynchronized filter read.)
//   (b) scan side — is the owning thread currently registered in
//       MachineThreads, and do WTF's stack bounds for it contain the ring?
//       (no/no = the §10.6 scan never had a chance: I4(b) registration or
//       suspend-and-copy coverage miss — the MachineThreads-layer verdict.)
//   (c) victim side — the cell's isLive()/mark/newlyAllocated bits at gather
//       time. (dead already = an EARLIER stop's sweep is the killer; alive
//       but missing = THIS gather's production failed.)
// It deliberately only LOGS — the gathered root set is not modified, no
// liveness is synthesized, so nothing is masked: the round-3 hook still owns
// the trap and one verify run yields trap + provenance together. Only after
// the producer-side verdict is in should the I12 m_currentBlock clause be
// implemented (and then with a retained cross-check that ring cells are ALSO
// produced by the §10.6 scan, so the corpus keeps exercising the I12 STACK
// clause instead of being satisfied by a redundant root source).
constexpr unsigned maxAuditLogLines = 32;

static void auditRingsAgainstGatheredRoots(JSC::Heap& heap, HeapCell** rootsBegin, size_t rootCount)
{
    if (!heap.isSharedServer())
        return;

    UncheckedKeyHashSet<void*> gathered;
    for (size_t i = 0; i < rootCount; ++i)
        gathered.add(rootsBegin[i]);

    HeapVersion markingVersion = heap.objectSpace().markingVersion();
    unsigned missing = 0;
    unsigned logged = 0;

    Locker locker { s_ringRegistryLock };
    for (ScratchRing* ring : ringRegistry()) {
        // (b) scan side, computed once per ring (the whole cells[] array
        // shares one owner and one frame).
        bool ownerRegistered = false;
        {
            MachineThreads& machineThreads = heap.machineThreads();
            Locker machineLocker { machineThreads.getLock() };
            for (auto& thread : machineThreads.threads(machineLocker)) {
                if (thread.ptr() == ring->owner) {
                    ownerRegistered = true;
                    break;
                }
            }
        }
        bool ringWithinOwnerStack = ring->owner && ring->owner->stack().contains(ring);

        for (unsigned i = 0; i < ScratchRing::capacity; ++i) {
            void* pointer = ring->cells[i];
            if (!pointer || gathered.contains(pointer))
                continue;
            ++missing;
            if (logged >= maxAuditLogLines)
                continue;
            ++logged;
            auto* cell = static_cast<HeapCell*>(pointer);
            if (cell->isPreciseAllocation()) {
                dataLogLn(
                    "SharedHeapTestHarness audit (fix-shared-heap-ring-liveness-5): ring-held PRECISE cell ",
                    RawPointer(cell), " absent from gathered conservative roots (isLive = ",
                    cell->preciseAllocation().isLive(),
                    ", ownerThread = ", RawPointer(ring->owner),
                    ", ownerRegisteredInMachineThreads = ", ownerRegistered,
                    ", ringWithinOwnerStackBounds = ", ringWithinOwnerStack, ")");
                continue;
            }
            MarkedBlock& block = cell->markedBlock();
            // (a) filter side.
            bool bloomRulesOut = heap.objectSpace().blocks().filter().ruleOut(reinterpret_cast<uintptr_t>(&block));
            bool inBlockSet = heap.objectSpace().blocks().set().contains(&block);
            // (c) victim side.
            bool live = block.handle().isLive(cell);
            dataLogLn(
                "SharedHeapTestHarness audit (fix-shared-heap-ring-liveness-5): ring-held cell ",
                RawPointer(cell), " absent from gathered conservative roots: block ", RawPointer(&block),
                " of directory ", RawPointer(block.handle().directory()),
                " (cellSize = ", block.handle().directory()->cellSize(),
                "), bloomFilterRulesOut = ", bloomRulesOut,
                ", inMarkedBlockSet = ", inBlockSet,
                ", isLive = ", live,
                ", marksStale = ", block.areMarksStale(markingVersion),
                ", isMarked = ", !block.areMarksStale(markingVersion) && block.isMarkedRaw(cell),
                ", newlyAllocatedStale = ", block.isNewlyAllocatedStale(),
                ", isNewlyAllocated = ", !block.isNewlyAllocatedStale() && block.isNewlyAllocated(cell),
                ", ownerThread = ", RawPointer(ring->owner),
                ", ownerRegisteredInMachineThreads = ", ownerRegistered,
                ", ringWithinOwnerStackBounds = ", ringWithinOwnerStack);
        }
    }
    if (missing) {
        dataLogLn(
            "SharedHeapTestHarness audit (fix-shared-heap-ring-liveness-5): ", missing,
            " ring cell(s) absent from this gather's conservative root set (", logged,
            " logged, cap ", maxAuditLogLines, "; gathered root count = ", rootCount, ").");
    }
}

// FIXME(fix-shared-heap-corruption): TEMPORARY round-3 diagnostic — the hook
// registry has no removal API and does NOT dedupe (same pattern as the
// epochReclaim hook), so install once per process via this single shared
// flag, referenced from every scenario entry path.
static std::atomic<bool> s_ringHookInstalled { false };

void installRingLivenessHookOnce(JSC::Heap& server)
{
    bool expected = false;
    if (s_ringHookInstalled.compare_exchange_strong(expected, true)) {
        // FIXME(fix-shared-heap-ring-liveness-5): TEMPORARY round-5 audit —
        // producer-side provenance for the round-3 trap. Release store: the
        // reader can be a parallel marker helper created before this store
        // (see the seam's comment in Heap.cpp).
        g_sharedGCConservativeRootAuditHook.store(auditRingsAgainstGatheredRoots, std::memory_order_release);
        server.addStopTheWorldSafepointHook(&verifyRingLivenessHook);
    }
}

// Spawns `threadCount` raw WTF::Threads, each with a stack-local standalone
// GCClient::Heap over `server` (markStandalone() + attachCurrentThread()),
// runs `body(client, threadIndex)`, detaches, and joins all. The caller must
// have released its own heap access first (ReleaseHeapAccessScope) — the
// joining thread parks for the whole storm.
template<typename Body>
void runStandaloneClientThreads(JSC::Heap& server, unsigned threadCount, const Body& body)
{
    // FIXME(fix-shared-heap-corruption): TEMPORARY round-3 diagnostic.
    installRingLivenessHookOnce(server);

    Vector<Ref<Thread>> threads;
    threads.reserveInitialCapacity(threadCount);
    for (unsigned threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        threads.append(Thread::create("JSC SharedHeapTest"_s, [&server, &body, threadIndex] {
            GCClient::Heap client(server); // §5.1 registration; may flip sticky ISS (§10B.4).
            client.markStandalone(); // §12.1: arms the vm() RELEASE_ASSERT (T9).
            client.attachCurrentThread(); // I4(a)-(c) + access.
            body(client, threadIndex);
            client.detachCurrentThread(); // RHA; localEpoch = MAX (§11).
        }));
    }
    for (auto& thread : threads)
        thread->waitForCompletion();
}

unsigned clampThreads(unsigned threadCount, unsigned minimum = 2)
{
    return std::min(std::max(threadCount, minimum), maxHarnessThreads);
}

bool requireSharedHeapOption(const char* scenario)
{
    if (Options::useSharedGCHeap())
        return true;
    dataLogLn("SharedHeapTestHarness: scenario \"", scenario, "\" requires --useSharedGCHeap=1.");
    return false;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// epochReclaim (T7): I11 unit test in the 1-client !ISS configuration.
// ---------------------------------------------------------------------------

// State for the epochReclaim scenario (T7). The safepoint hook registry has
// no removal API, so the hook installs once per process and records into
// these; they are only interpreted while the scenario runs on the (single)
// mutator thread.
static std::atomic<unsigned> s_epochItemsDestroyed { 0 };
static std::atomic<bool> s_epochHookInstalled { false };
static std::atomic<bool> s_epochHookSawStopWithoutLicense { false };
static std::atomic<bool> s_epochHookEverSawLicense { false };

void SharedHeapTestHarness::recordEpochHookObservation(JSC::Heap& heap)
{
    // Runs inside Heap::runSafepointHooksAndReclaim(), with the world stopped
    // — in the legacy protocol the conducted cycle's periphery suspension
    // (stopThePeriphery's suspendCompilerThreads) is still in effect here —
    // and strictly BEFORE the reclaimer bracket opens. I11: that conducted
    // cycle's own suspension must NOT license a bump.
    GCSafepointEpoch& epoch = heap.safepointEpoch();
    bool worldStopped = heap.isSharedServer()
        ? heap.worldIsStoppedForAllClients()
        : heap.worldIsStopped();
    bool licensed = epoch.reclaimLicensed();
    if (licensed)
        s_epochHookEverSawLicense.store(true, std::memory_order_relaxed);
    if (worldStopped && !licensed)
        s_epochHookSawStopWithoutLicense.store(true, std::memory_order_relaxed);
}

bool SharedHeapTestHarness::runEpochReclaimScenario(JSC::Heap& server, unsigned, unsigned iterations)
{
    // SPEC-heap.md T7: this unit test runs in the 1-client !ISS configuration
    // — collections take the legacy path, and reclamation happens at the
    // legacy runEndPhase site (§9 contract notes). Caller context: the main
    // VM's mutator thread, API lock held ($vm.sharedHeapTest).
    if (server.isSharedServer()) {
        dataLogLn("SharedHeapTestHarness: epochReclaim requires the 1-client !ISS configuration; run it before any scenario that attaches a second client.");
        return false;
    }

    GCSafepointEpoch& epoch = server.safepointEpoch();

    // Never licensed while the world is running.
    RELEASE_ASSERT(!epoch.reclaimLicensed());

    bool expected = false;
    if (s_epochHookInstalled.compare_exchange_strong(expected, true))
        server.addStopTheWorldSafepointHook(&SharedHeapTestHarness::recordEpochHookObservation);
    s_epochHookSawStopWithoutLicense.store(false, std::memory_order_relaxed);

    unsigned itemCount = std::max(1u, iterations);
    unsigned destroyedBefore = s_epochItemsDestroyed.load(std::memory_order_relaxed);
    uint64_t epochBefore = epoch.current();

    // retire() from the mutator: each item's destroy thunk bumps the counter.
    for (unsigned i = 0; i < itemCount; ++i) {
        epoch.retire(&s_epochItemsDestroyed, [](void* pointer) {
            static_cast<std::atomic<unsigned>*>(pointer)->fetch_add(1, std::memory_order_relaxed);
        });
    }
    RELEASE_ASSERT(s_epochItemsDestroyed.load(std::memory_order_relaxed) == destroyedBefore);

    // First legacy GC. The runEndPhase reclaim sequence stamps the (single)
    // client's local epoch to E = current and bumps: items retired at E
    // survive — a published local epoch must EXCEED the retire epoch
    // (I11(a)); the cycle that retired them never frees them.
    server.collectSync(CollectionScope::Full);
    uint64_t epochAfterFirst = epoch.current();
    RELEASE_ASSERT(epochAfterFirst >= epochBefore + 1); // Non-empty retired list => the bump happened.
    if (epochAfterFirst == epochBefore + 1) {
        // Exactly one safepoint crossed: nothing may have been freed.
        RELEASE_ASSERT(s_epochItemsDestroyed.load(std::memory_order_relaxed) == destroyedBefore);
    }

    // Second legacy GC: every client has published an epoch > the retire
    // epoch => freed at the legacy runEndPhase site (retire -> legacy GC ->
    // free).
    server.collectSync(CollectionScope::Full);
    RELEASE_ASSERT(epoch.current() >= epochBefore + 2);
    RELEASE_ASSERT(s_epochItemsDestroyed.load(std::memory_order_relaxed) == destroyedBefore + itemCount);

    // I11 negative: the hook observed a stopped world — with the conducted
    // cycle's own compiler-thread suspension in effect — that was NOT
    // licensed to bumpAndReclaim(); and the hook point (any point outside the
    // reclaimer bracket) is never licensed.
    RELEASE_ASSERT(s_epochHookSawStopWithoutLicense.load(std::memory_order_relaxed));
    RELEASE_ASSERT(!s_epochHookEverSawLicense.load(std::memory_order_relaxed));

    // World running again: license gone.
    RELEASE_ASSERT(!epoch.reclaimLicensed());
    return true;
}

// ---------------------------------------------------------------------------
// §12.1 multi-client scenarios (T10).
// ---------------------------------------------------------------------------

bool SharedHeapTestHarness::runAllocationStormScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations)
{
    // I1/I12: every thread keeps a stack-held, pattern-checked retention ring
    // while thread 0 conducts collections and the rest poll SINFAC. A lost or
    // doubled block handout corrupts a pattern and RELEASE_ASSERTs.
    threadCount = clampThreads(threadCount);
    unsigned perThread = std::min(std::max(iterations, 1u), 1u << 20);

    ReleaseHeapAccessScope releaseScope(server); // Caller parks in join below.
    runStandaloneClientThreads(server, threadCount, [&](GCClient::Heap& client, unsigned threadIndex) {
        DECLARE_STACK_SCRATCH_RING(ring);
        for (unsigned i = 0; i < perThread; ++i) {
            size_t size = smallSizes[(threadIndex + i) % smallSizeCount];
            uint64_t pattern = mixPattern(threadIndex, i);
            void* cell = allocateScratch(server, client, size);
            if (cell) {
                fillCell(cell, size, pattern);
                ring.step(cell, size, pattern);
            }
            if (!(i % 512) && i) {
                if (!threadIndex)
                    server.collectSyncAllClients((i % 2048) ? CollectionScope::Eden : CollectionScope::Full);
                else
                    server.stopIfNecessaryForAllClients();
                ring.verifyAll(); // Cells survive the stop intact (non-moving GC).
            }
        }
        ring.verifyAll();
    });
    return true;
}

bool SharedHeapTestHarness::runPreciseAllocationStormScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations)
{
    // I16: hammer the §5.6 precise-allocation registry (creation +
    // registration under MSPL) from N clients while full collections iterate
    // it world-stopped. Pattern checks catch a doubled cell; the I16 asserts
    // in MarkedSpace catch unlocked registration.
    threadCount = clampThreads(threadCount);
    unsigned perThread = std::min(std::max(iterations, 1u), 1u << 14);

    ReleaseHeapAccessScope releaseScope(server);
    runStandaloneClientThreads(server, threadCount, [&](GCClient::Heap& client, unsigned threadIndex) {
        DECLARE_STACK_SCRATCH_RING(ring);
        for (unsigned i = 0; i < perThread; ++i) {
            size_t size = preciseSizes[(threadIndex + i) % preciseSizeCount];
            uint64_t pattern = mixPattern(threadIndex, i ^ 0xBEEF);
            void* cell = allocateScratch(server, client, size);
            if (cell) {
                fillCell(cell, size, pattern);
                ring.step(cell, size, pattern);
            }
            // Mix in small allocations so block handout and precise
            // registration interleave under one MSPL.
            allocateBurst(server, client, ring, threadIndex, i, 4, smallSizes, smallSizeCount);
            if (!(i % 64) && i) {
                if (!threadIndex)
                    server.collectSyncAllClients((i % 256) ? CollectionScope::Eden : CollectionScope::Full);
                else
                    server.stopIfNecessaryForAllClients();
                ring.verifyAll();
            }
        }
        ring.verifyAll();
    });
    return true;
}

bool SharedHeapTestHarness::runStealRaceScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations)
{
    // I8: alternate dense bursts between two size classes of the same
    // subspace, with a full collection between phases. The collection empties
    // the previous phase's blocks; the next phase's directory then steals
    // them (findEmptyBlockToSteal -> sweep -> removeFromDirectory -> addBlock
    // under MSPL). A block allocatable in two directories corrupts patterns.
    threadCount = clampThreads(threadCount);
    unsigned rounds = std::min(std::max(iterations, 1u), 64u);

    ReleaseHeapAccessScope releaseScope(server);
    PhaseBarrier barrier;
    runStandaloneClientThreads(server, threadCount, [&](GCClient::Heap& client, unsigned threadIndex) {
        DECLARE_STACK_SCRATCH_RING(ring);
        for (unsigned round = 0; round < rounds; ++round) {
            size_t size = (round & 1) ? 512 : 32;
            // Dense burst, mostly unretained (the ring keeps only the last
            // 32) so the GC below leaves plenty of empty blocks to steal.
            for (unsigned i = 0; i < 512; ++i) {
                uint64_t pattern = mixPattern(threadIndex, (static_cast<uint64_t>(round) << 32) | i);
                void* cell = allocateScratch(server, client, size);
                if (cell) {
                    fillCell(cell, size, pattern);
                    ring.step(cell, size, pattern);
                }
            }
            ring.verifyAll();
            ring.clear(); // Drop refs: this phase's blocks may go empty.
            barrier.arrive(server, threadCount);
            if (!threadIndex)
                server.collectSyncAllClients(CollectionScope::Full);
            barrier.arrive(server, threadCount);
        }
    });
    return true;
}

bool SharedHeapTestHarness::runClientChurnVsGCScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations)
{
    // I13: HeapClientSet add/remove churn against conducted stop windows —
    // add/remove cannot complete between stop and resume; removal of a
    // stopped client defers to resume. Thread 0 holds a long-lived client
    // and pounds collections (also pinning size() >= 2 so ISS stays up for
    // the whole storm); the rest construct/attach/detach/destroy whole
    // clients in a loop.
    // FIXME(fix-shared-heap-corruption): TEMPORARY round-3 diagnostic — see
    // verifyRingLivenessHook(); heap-client-churn.js runs this scenario first,
    // before any runStandaloneClientThreads call, so install here too (the
    // shared flag makes this idempotent).
    installRingLivenessHookOnce(server);

    threadCount = clampThreads(threadCount);
    unsigned gcIterations = std::min(std::max(iterations, 1u), 64u);
    unsigned churnIterations = std::min(std::max(iterations, 1u), 256u);

    ReleaseHeapAccessScope releaseScope(server);
    Vector<Ref<Thread>> threads;
    threads.append(Thread::create("JSC SharedHeapTest"_s, [&] {
        GCClient::Heap client(server);
        client.markStandalone();
        client.attachCurrentThread();
        DECLARE_STACK_SCRATCH_RING(ring);
        for (unsigned i = 0; i < gcIterations; ++i) {
            allocateBurst(server, client, ring, 0, i, 64, smallSizes, smallSizeCount);
            server.collectSyncAllClients((i % 4) ? CollectionScope::Eden : CollectionScope::Full);
            ring.verifyAll();
        }
        client.detachCurrentThread();
    }));
    for (unsigned threadIndex = 1; threadIndex < threadCount; ++threadIndex) {
        threads.append(Thread::create("JSC SharedHeapTest"_s, [&server, threadIndex, churnIterations] {
            // Hoisted out of the loop (alloca storage lives until the lambda
            // returns); cleared per iteration to preserve the fresh-ring-per-
            // client shape.
            DECLARE_STACK_SCRATCH_RING(ring);
            for (unsigned i = 0; i < churnIterations; ++i) {
                GCClient::Heap client(server); // add() vs stop windows (I13).
                client.markStandalone();
                client.attachCurrentThread();
                allocateBurst(server, client, ring, threadIndex, i, 32, smallSizes, smallSizeCount);
                ring.verifyAll();
                ring.clear();
                client.detachCurrentThread();
            } // dtor: remove() of a (possibly stopped) client defers to resume (I13).
        }));
    }
    for (auto& thread : threads)
        thread->waitForCompletion();
    return true;
}

bool SharedHeapTestHarness::runStructureLockVsSTWScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations)
{
    // I14/L5 shape (standalone stand-in for vmstate's StructureAllocationLocker:
    // STW-forbidden scope + deferred allocation): inside the scope a thread
    // must not initiate/join/wait for STW — its allocations run under a
    // deferral so CIND never elects (the I14 debug counter at the
    // CSAC/RCAC/SINFAC/election entries asserts the discipline). Thread 0
    // keeps real stops coming.
    threadCount = clampThreads(threadCount);
    unsigned perThread = std::min(std::max(iterations, 1u), 1u << 14);

    ReleaseHeapAccessScope releaseScope(server);
    runStandaloneClientThreads(server, threadCount, [&](GCClient::Heap& client, unsigned threadIndex) {
        if (!threadIndex) {
            unsigned gcs = std::min(perThread, 32u);
            DECLARE_STACK_SCRATCH_RING(ring);
            for (unsigned i = 0; i < gcs; ++i) {
                allocateBurst(server, client, ring, threadIndex, i, 64, smallSizes, smallSizeCount);
                server.collectSyncAllClients((i % 4) ? CollectionScope::Eden : CollectionScope::Full);
                ring.verifyAll();
            }
            return;
        }
        DECLARE_STACK_SCRATCH_RING(ring);
        for (unsigned i = 0; i < perThread; ++i) {
            server.incrementSTWForbiddenScope(); // I14 (debug-only counter).
            server.incrementDeferralDepth(); // L5: allocations inside the scope defer GC (I17 slot).
            allocateBurst(server, client, ring, threadIndex, i, 4, smallSizes, smallSizeCount);
            server.decrementDeferralDepth(); // Plain decrement: no GC inside the forbidden scope.
            server.decrementSTWForbiddenScope();
            // Outside the scope: honor whatever was deferred / park for stops.
            server.stopIfNecessaryForAllClients();
        }
        ring.verifyAll();
    });
    return true;
}

bool SharedHeapTestHarness::runBlockedInNativeVsGCScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations)
{
    // §10A/F8: threads bracket simulated blocking native calls with RHA/AHA
    // while thread 0 conducts. Re-acquire hits the F8 Dekker pair: if a stop
    // is pending, the CAS-then-revert path must block until GSP clears (the
    // race amplifier's AHA hook widens exactly this window).
    threadCount = clampThreads(threadCount);
    unsigned perThread = std::min(std::max(iterations, 1u), 1u << 16);

    ReleaseHeapAccessScope releaseScope(server);
    runStandaloneClientThreads(server, threadCount, [&](GCClient::Heap& client, unsigned threadIndex) {
        DECLARE_STACK_SCRATCH_RING(ring);
        if (!threadIndex) {
            unsigned gcs = std::min(perThread, 48u);
            for (unsigned i = 0; i < gcs; ++i) {
                allocateBurst(server, client, ring, threadIndex, i, 32, smallSizes, smallSizeCount);
                server.collectSyncAllClients((i % 4) ? CollectionScope::Eden : CollectionScope::Full);
                ring.verifyAll();
            }
            return;
        }
        for (unsigned i = 0; i < perThread; ++i) {
            allocateBurst(server, client, ring, threadIndex, i, 16, smallSizes, smallSizeCount);
            client.releaseHeapAccess(); // RHA: signals a pending §10.4 barrier.
            if (!(i % 8))
                WTF::sleep(Seconds::fromMicroseconds(100)); // Park across a whole stop window sometimes.
            else
                Thread::yield();
            client.acquireHeapAccess(); // AHA/F8: mandatory revert if a stop pends.
            ring.verifyAll(); // Writes published by RHA stay intact across the stop (F6).
        }
    });
    return true;
}

bool SharedHeapTestHarness::runSyncRequesterStormScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations)
{
    // §10.2: every thread is a sync requester. Elections serialize on GCL;
    // losers release access and wait on GEC (never m_threadCondition, never
    // spinning); the winner drains ALL granted tickets. Pure liveness +
    // pattern integrity.
    threadCount = clampThreads(threadCount);
    unsigned perThread = std::min(std::max(iterations, 1u), 24u);

    ReleaseHeapAccessScope releaseScope(server);
    runStandaloneClientThreads(server, threadCount, [&](GCClient::Heap& client, unsigned threadIndex) {
        DECLARE_STACK_SCRATCH_RING(ring);
        for (unsigned i = 0; i < perThread; ++i) {
            allocateBurst(server, client, ring, threadIndex, i, 32, smallSizes, smallSizeCount);
            server.collectSyncAllClients((i % 3) ? CollectionScope::Eden : CollectionScope::Full);
            ring.verifyAll();
        }
    });
    return true;
}

bool SharedHeapTestHarness::runNoEnteredVMsGCScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations)
{
    // §12.1: zero-entered-VMs stop path. Every mutator in the storm is a
    // standalone (VM-less) client and the main VM's client holds no access
    // (released by the scope below), so the §10.4 barrier and VMM
    // requestStopAll(GC) complete with no VM ever parking via traps.
    threadCount = clampThreads(threadCount);
    unsigned gcs = std::min(std::max(iterations, 1u), 16u);

    ReleaseHeapAccessScope releaseScope(server);
    runStandaloneClientThreads(server, threadCount, [&](GCClient::Heap& client, unsigned threadIndex) {
        DECLARE_STACK_SCRATCH_RING(ring);
        for (unsigned i = 0; i < gcs; ++i) {
            allocateBurst(server, client, ring, threadIndex, i, 128, smallSizes, smallSizeCount);
            if (!threadIndex)
                server.collectSyncAllClients(CollectionScope::Full);
            else
                server.stopIfNecessaryForAllClients();
            ring.verifyAll();
        }
    });
    RELEASE_ASSERT(!server.worldIsStoppedForAllClients());
    return true;
}

bool SharedHeapTestHarness::runAttachWithPendingTicketScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations)
{
    // §10B.4: (phase A) a legacy granted-unserved ticket exists when the
    // flipping attach starts; the attach quiescence loop must wait it out
    // while the CREATOR side has RELEASED heap access (review round 3: the
    // round-1 quiescence condition also requires hasAccessBit clear for a
    // non-API-lock flipper, so the legacy collector thread — not a
    // creator-side stopIfNecessary() spin with access retained — serves the
    // ticket; this is the cross-part ISS-flip liveness contract in action).
    // (Phase B) once ISS, clients attach while shared RCAC tickets are
    // granted-unserved.
    threadCount = clampThreads(threadCount);
    unsigned rounds = std::min(std::max(iterations, 1u), 8u);

    // Phase A. The caller still holds access HERE (the collectAsync/RCAC
    // precondition), so the ticket is enqueued in the caller-contract state.
    // If the server is already sticky-shared (scenario ordering), this
    // enqueues a shared ticket instead — the attach below then skips the
    // §10B.4 loop, which is the phase-B shape; both are valid coverage.
    server.collectAsync(GCRequest(CollectionScope::Full));

    // Review round 3: release the caller's access for the WHOLE of phases
    // A+B, BEFORE spawning the attacher. Since review round 1 the §10B.4
    // quiescence loop also requires "hasAccessBit clear OR the flipping
    // thread holds the main VM's API lock"; the attacher is a raw
    // WTF::Thread (no API lock), so the previous shape — the creator
    // spinning stopIfNecessary() with access retained — could never satisfy
    // the loop and deadlocked deterministically whenever the server was not
    // yet sticky-shared. With access released, the legacy collector thread
    // serves the pending granted-unserved ticket on its own, and the
    // attacher's timed quiescence loop then observes tickets-served +
    // collector-idle + hasAccessBit-clear and completes — still covering
    // the §10B.4 "attach waits out a pending legacy ticket" shape this
    // scenario exists for. (This is exactly the ReleaseHeapAccessScope
    // obligation the INTEGRATE-heap.md ISS-flip liveness contract imposes
    // on spawn paths.)
    ReleaseHeapAccessScope releaseScope(server);

    std::atomic<bool> attached { false };
    Ref<Thread> attacher = Thread::create("JSC SharedHeapTest"_s, [&] {
        GCClient::Heap client(server); // §10B.4: blocks until legacy-quiescent.
        client.markStandalone();
        client.attachCurrentThread();
        attached.store(true);
        DECLARE_STACK_SCRATCH_RING(ring);
        for (unsigned i = 0; i < rounds; ++i) {
            // Shared RCAC ticket with no waiting requester...
            server.requestCollectionAllClients(GCRequest(CollectionScope::Eden));
            allocateBurst(server, client, ring, 1, i, 64, smallSizes, smallSizeCount);
            // ...served by a mutator poll (tryConductSharedCollectionForPoll).
            server.stopIfNecessaryForAllClients();
        }
        server.collectSyncAllClients(CollectionScope::Eden); // Drains every granted ticket.
        ring.verifyAll();
        client.detachCurrentThread();
    });

    // Creator side: with access released there is no creator-side serving
    // obligation (the legacy collector thread owns the pending ticket); just
    // wait for the attach to complete.
    while (!attached.load())
        Thread::yield();

    // Phase B: more clients attach while the attacher keeps RCAC tickets
    // granted-unserved. Our access is still released (scope above), so its
    // conductions complete.
    runStandaloneClientThreads(server, threadCount - 1, [&](GCClient::Heap& client, unsigned threadIndex) {
        DECLARE_STACK_SCRATCH_RING(ring);
        for (unsigned i = 0; i < rounds; ++i) {
            server.requestCollectionAllClients(GCRequest(CollectionScope::Eden));
            allocateBurst(server, client, ring, threadIndex + 2, i, 64, smallSizes, smallSizeCount);
            server.stopIfNecessaryForAllClients();
        }
        server.collectSyncAllClients(CollectionScope::Eden);
        ring.verifyAll();
    });
    attacher->waitForCompletion();
    return true;
}

bool SharedHeapTestHarness::runDeferralVsAllocationStormScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations)
{
    // I17: when ISS, DeferGC depth is per-client. Each storm thread brackets
    // its bursts in increment/decrementDeferralDepth (routed to ITS client's
    // counter via the §10A.1 TLS stamp); thread 0 keeps collecting — one
    // client's deferral must never defer another client's collection, and a
    // deferred client still parks for stops (SINFAC handles GSP before the
    // isDeferred() check).
    threadCount = clampThreads(threadCount);
    unsigned perThread = std::min(std::max(iterations, 1u), 1u << 16);

    ReleaseHeapAccessScope releaseScope(server);
    runStandaloneClientThreads(server, threadCount, [&](GCClient::Heap& client, unsigned threadIndex) {
        DECLARE_STACK_SCRATCH_RING(ring);
        if (!threadIndex) {
            unsigned gcs = std::min(perThread, 48u);
            for (unsigned i = 0; i < gcs; ++i) {
                allocateBurst(server, client, ring, threadIndex, i, 64, smallSizes, smallSizeCount);
                server.collectSyncAllClients((i % 4) ? CollectionScope::Eden : CollectionScope::Full);
                ring.verifyAll();
            }
            return;
        }
        for (unsigned i = 0; i < perThread; ++i) {
            server.incrementDeferralDepth(); // ISS: this client's slot (I17).
            allocateBurst(server, client, ring, threadIndex, i, 8, smallSizes, smallSizeCount);
            // Deferred clients still park for a pending stop (GSP handling in
            // SINFAC precedes the isDeferred() conduction skip).
            server.stopIfNecessaryForAllClients();
            server.decrementDeferralDepthAndGCIfNeeded();
        }
        ring.verifyAll();
    });
    return true;
}

bool SharedHeapTestHarness::runDebuggerStopDuringSharedGCScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations)
{
    // §10C(b)/(c) C-level shape: a non-GC stop (the JSThreadsStopScope GCL
    // bracket — same bracket a debugger/JSThreads conductor holds, §13.10b)
    // is requested WHILE GC conductions are in flight. The scope ctor blocks
    // on GCL until the conductor finishes; GC requesters arriving while the
    // scope is held take the §10.2 GCL-busy timed-wait path. The
    // VMM-trap/park halves of §10C run with real VMs in the JS corpus
    // (JSTests/threads/heap-stop-interleavings.js).
    threadCount = clampThreads(threadCount, 3);
    unsigned rounds = std::min(std::max(iterations, 1u), 32u);

    ReleaseHeapAccessScope releaseScope(server);
    runStandaloneClientThreads(server, threadCount, [&](GCClient::Heap& client, unsigned threadIndex) {
        DECLARE_STACK_SCRATCH_RING(ring);
        if (!threadIndex) {
            for (unsigned i = 0; i < rounds; ++i) {
                allocateBurst(server, client, ring, threadIndex, i, 32, smallSizes, smallSizeCount);
                server.collectSyncAllClients(CollectionScope::Eden);
            }
            ring.verifyAll();
            return;
        }
        if (threadIndex == 1) {
            for (unsigned i = 0; i < rounds; ++i) {
                client.releaseHeapAccess(); // §9 pre: access released before bracketing.
                {
                    JSC::Heap::JSThreadsStopScope stopScope(server); // Blocks while a GC conductor holds GCL (§10C(b)).
                    Thread::yield();
                }
                client.acquireHeapAccess(); // F8.
            }
            return;
        }
        for (unsigned i = 0; i < rounds * 16; ++i) {
            allocateBurst(server, client, ring, threadIndex, i, 16, smallSizes, smallSizeCount);
            server.stopIfNecessaryForAllClients();
        }
        ring.verifyAll();
    });
    return true;
}

bool SharedHeapTestHarness::runGCDuringDebuggerParkScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations)
{
    // §10C(a)/(e) C-level shape: the GCL bracket is HELD (a "debugger park")
    // when GC requesters arrive. They must take the §10.2 GCL-busy rule —
    // release access, timed (<= 1ms) GEC waits, never spinning, never untimed
    // — and complete promptly after the scope exits. The barrier/park-hook
    // half of (a) (parked VMs holding access until willPark releases it) runs
    // with real VMs in the JS corpus.
    threadCount = clampThreads(threadCount, 3);
    unsigned rounds = std::min(std::max(iterations, 1u), 16u);

    ReleaseHeapAccessScope releaseScope(server);
    runStandaloneClientThreads(server, threadCount, [&](GCClient::Heap& client, unsigned threadIndex) {
        DECLARE_STACK_SCRATCH_RING(ring);
        if (!threadIndex) {
            for (unsigned i = 0; i < rounds; ++i) {
                client.releaseHeapAccess();
                {
                    JSC::Heap::JSThreadsStopScope stopScope(server);
                    // Hold the bracket long enough that requesters reliably
                    // hit the GCL-busy path.
                    WTF::sleep(Seconds::fromMilliseconds(1));
                }
                client.acquireHeapAccess();
            }
            return;
        }
        for (unsigned i = 0; i < rounds; ++i) {
            allocateBurst(server, client, ring, threadIndex, i, 32, smallSizes, smallSizeCount);
            server.collectSyncAllClients(CollectionScope::Eden); // GCL-busy => parked, timed waits (G13).
            ring.verifyAll();
        }
    });
    return true;
}

bool SharedHeapTestHarness::runJSThreadsStopVsGCRequesterScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations)
{
    // §10C(e)/G13 combined storm: stop-scope churn, sync GC requesters, and
    // allocators all at once. Liveness (no lost wakeup between GEC, GBC and
    // the GCL hand-offs) + pattern integrity. Driven from a real VM by the
    // JS corpus per §12.1 ("real VMs via $vm").
    threadCount = clampThreads(threadCount, 4);
    unsigned rounds = std::min(std::max(iterations, 1u), 24u);

    ReleaseHeapAccessScope releaseScope(server);
    runStandaloneClientThreads(server, threadCount, [&](GCClient::Heap& client, unsigned threadIndex) {
        DECLARE_STACK_SCRATCH_RING(ring);
        if (!threadIndex) {
            for (unsigned i = 0; i < rounds * 2; ++i) {
                client.releaseHeapAccess();
                {
                    JSC::Heap::JSThreadsStopScope stopScope(server);
                    if (i & 1)
                        WTF::sleep(Seconds::fromMicroseconds(200));
                    else
                        Thread::yield();
                }
                client.acquireHeapAccess();
            }
            return;
        }
        if (threadIndex <= 2) {
            for (unsigned i = 0; i < rounds; ++i) {
                allocateBurst(server, client, ring, threadIndex, i, 32, smallSizes, smallSizeCount);
                server.collectSyncAllClients((i % 3) ? CollectionScope::Eden : CollectionScope::Full);
                ring.verifyAll();
            }
            return;
        }
        for (unsigned i = 0; i < rounds * 32; ++i) {
            allocateBurst(server, client, ring, threadIndex, i, 16, smallSizes, smallSizeCount);
            server.stopIfNecessaryForAllClients();
        }
        ring.verifyAll();
    });
    return true;
}

// issRevertChurn epoch-drain counter (separate from the epochReclaim
// scenario's so the two stay independently checkable).
static std::atomic<unsigned> s_issRevertItemsDestroyed { 0 };

bool SharedHeapTestHarness::runIssRevertChurnScenario(JSC::Heap& server, unsigned, unsigned iterations)
{
    // §10D: a short-lived secondary client must not downgrade GC forever.
    // Each round: spawn one secondary client (flips sticky ISS), let it work
    // and die (remove() leaving size() == 1 arms m_issRevertPending), then
    // THIS thread — the main client's — polls SINFAC until the reversion
    // lands. Re-running the loop re-flips ISS (I13: same server may go shared
    // again). Items retired while shared drain at the legacy runEndPhase
    // site after the reversion (§10D "residual m_retired drains via §11's
    // legacy site").
    unsigned rounds = std::min(std::max(iterations, 1u), 16u);
    unsigned destroyedBefore = s_issRevertItemsDestroyed.load(std::memory_order_relaxed);
    unsigned retired = 0;

    for (unsigned round = 0; round < rounds; ++round) {
        {
            ReleaseHeapAccessScope releaseScope(server); // Park access across the join.
            Ref<Thread> secondary = Thread::create("JSC SharedHeapTest"_s, [&server, round] {
                GCClient::Heap client(server); // size() -> 2: sticky ISS (re-)flips (§10B.4).
                client.markStandalone();
                client.attachCurrentThread();
                RELEASE_ASSERT(server.isSharedServer());
                // Retire while shared; must survive until after the reversion's
                // legacy drain (or a conducted cycle's §10 step 7 — either way,
                // only once every client crossed a safepoint, I11).
                server.safepointEpoch().retire(&s_issRevertItemsDestroyed, [](void* pointer) {
                    static_cast<std::atomic<unsigned>*>(pointer)->fetch_add(1, std::memory_order_relaxed);
                });
                DECLARE_STACK_SCRATCH_RING(ring);
                allocateBurst(server, client, ring, 1, round, 256, smallSizes, smallSizeCount);
                ring.verifyAll();
                client.detachCurrentThread();
            }); // dtor: remove() leaves size() == 1, survivor = main client => m_issRevertPending.
            secondary->waitForCompletion();
        } // Scope dtor: forwarded AHA re-stamps the main client's owner + §10A.1 TLS.
        ++retired;

        // §10D: the reversion happens only at a MAIN-client CIND/SINFAC poll.
        // Bounded: quiescence is immediate here (no tickets in flight).
        unsigned spins = 0;
        while (server.isSharedServer()) {
            server.stopIfNecessaryForAllClients();
            Thread::yield();
            RELEASE_ASSERT(++spins < 1u << 24); // Liveness: reversion must land.
        }
    }

    // Post-reversion: the legacy protocol owns reclamation again. Two legacy
    // full collections stamp + bump past every retire epoch: all items drain.
    RELEASE_ASSERT(!server.isSharedServer());
    server.collectSync(CollectionScope::Full);
    server.collectSync(CollectionScope::Full);
    RELEASE_ASSERT(s_issRevertItemsDestroyed.load(std::memory_order_relaxed) == destroyedBefore + retired);
    return true;
}

bool SharedHeapTestHarness::run(JSC::Heap& server, const String& scenarioName, unsigned threadCount, unsigned iterations)
{
    // epochReclaim deliberately runs without the option/ISS (T7; the I10
    // legacy-reclamation exemption).
    if (scenarioName == "epochReclaim"_s)
        return runEpochReclaimScenario(server, threadCount, iterations);

    struct Scenario {
        ASCIILiteral name;
        bool (*function)(JSC::Heap&, unsigned, unsigned);
    };
    static constexpr Scenario scenarios[] = {
        { "allocationStorm"_s, runAllocationStormScenario },
        { "preciseAllocationStorm"_s, runPreciseAllocationStormScenario },
        { "stealRace"_s, runStealRaceScenario },
        { "clientChurnVsGC"_s, runClientChurnVsGCScenario },
        { "structureLockVsSTW"_s, runStructureLockVsSTWScenario },
        { "blockedInNativeVsGC"_s, runBlockedInNativeVsGCScenario },
        { "syncRequesterStorm"_s, runSyncRequesterStormScenario },
        { "noEnteredVMsGC"_s, runNoEnteredVMsGCScenario },
        { "attachWithPendingTicket"_s, runAttachWithPendingTicketScenario },
        { "deferralVsAllocationStorm"_s, runDeferralVsAllocationStormScenario },
        { "debuggerStopDuringSharedGC"_s, runDebuggerStopDuringSharedGCScenario },
        { "gcDuringDebuggerPark"_s, runGCDuringDebuggerParkScenario },
        { "jsThreadsStopVsGCRequester"_s, runJSThreadsStopVsGCRequesterScenario },
        { "issRevertChurn"_s, runIssRevertChurnScenario },
    };

    for (const Scenario& scenario : scenarios) {
        if (scenarioName == scenario.name) {
            if (!requireSharedHeapOption(scenario.name.characters()))
                return false;
            // FIXME(fix-shared-heap-corruption): TEMPORARY round-3 diagnostic
            // — install before dispatch so scenarios that spawn threads
            // without runStandaloneClientThreads (clientChurnVsGC,
            // issRevertChurn, attachWithPendingTicket's attacher) are covered
            // even when they run first in a corpus file.
            installRingLivenessHookOnce(server);
            return scenario.function(server, threadCount, iterations);
        }
    }

    dataLogLn("SharedHeapTestHarness: unknown scenario \"", scenarioName, "\".");
    return false;
}

} // namespace JSC
