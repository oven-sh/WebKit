/*
 * Copyright (c) 2020 Apple Inc. All rights reserved.
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

#include "TestHarness.h"
#include <atomic>
#include "bmalloc_heap.h"
#include "bmalloc_heap_config.h"
#include "bmalloc_heap_innards.h"
#include "bmalloc_heap_ref.h"
#include "bmalloc_type.h"
#include <condition_variable>
#include <functional>
#include "iso_heap.h"
#include "iso_heap_config.h"
#include "iso_heap_ref.h"
#include <map>
#include <mutex>
#include "pas_heap.h"
#include "pas_heap_inlines.h"
#include "pas_primitive_heap_ref.h"
#include "pas_race_test_hooks.h"
#include "pas_segregated_heap_inlines.h"
#include "pas_segregated_size_directory.h"
#include "pas_segregated_size_directory_inlines.h"
#include "pas_thread_local_cache.h"
#include <set>
#include <thread>

using namespace std;

#if PAS_ENABLE_TESTING && PAS_ENABLE_ISO

namespace {

mutex globalLock;
condition_variable globalCond;
map<thread::id, set<pas_lock*>> locksHeldForThread;
map<pas_lock*, thread::id> threadHoldingLock;
map<pas_lock*, unsigned> locksUpForContention;
function<void(pas_race_test_hook_kind)> hookCallback;

void hookCallbackAdapter(pas_race_test_hook_kind kind)
{
    hookCallback(kind);
}

void willLockCallback(pas_lock* lock)
{
    lock_guard<mutex> locker(globalLock);
    locksUpForContention[lock]++;
    globalCond.notify_all();
}

void recordLockAcquisition(pas_lock* lock)
{
    thread::id myId = this_thread::get_id();

    locksHeldForThread[myId].insert(lock);
    threadHoldingLock[lock] = myId;
}

void didLockCallback(pas_lock* lock)
{
    lock_guard<mutex> locker(globalLock);
    
    auto iter = locksUpForContention.find(lock);
    PAS_ASSERT(iter != locksUpForContention.end());
    PAS_ASSERT(iter->second);
    if (!--iter->second)
        locksUpForContention.erase(iter);

    recordLockAcquisition(lock);
}

void didTryLockCallback(pas_lock* lock)
{
    lock_guard<mutex> locker(globalLock);
    recordLockAcquisition(lock);
}

void willUnlockCallback(pas_lock* lock)
{
    lock_guard<mutex> locker(globalLock);

    thread::id myId = this_thread::get_id();

    locksHeldForThread[myId].erase(lock);
    threadHoldingLock.erase(lock);
}

// Call holding the lock.
bool isHoldingContendedLocks()
{
    thread::id myId = this_thread::get_id();

    for (pas_lock* lock : locksHeldForThread[myId]) {
        if (locksUpForContention.count(lock))
            return true;
    }

    return false;
}

class InstallRaceHooks : public TestScope {
public:
    InstallRaceHooks()
        : TestScope(
            "install-race-hooks",
            [] () {
                pas_race_test_hook_callback_instance = hookCallbackAdapter;
                pas_race_test_will_lock_callback = willLockCallback;
                pas_race_test_did_lock_callback = didLockCallback;
                pas_race_test_did_try_lock_callback = didTryLockCallback;
                pas_race_test_will_unlock_callback = willUnlockCallback;
            })
    {
    }
};

void testLocalAllocatorStopRace(pas_race_test_hook_kind kindToStopOn)
{
    pas_scavenger_suspend();
    
    bool okToGetToHook = false;
    bool didGetToHook = false;
    bool hookShouldStop = false;
    bool hookDidStop = false;
    bool shrinkDidFinish = false;
    void* thePtr;
    
    hookCallback =
        [&] (pas_race_test_hook_kind kind) {
            if (kind != kindToStopOn)
                return;

            CHECK_EQUAL(locksHeldForThread[this_thread::get_id()].size(), 3);
            CHECK(okToGetToHook);

            unique_lock<mutex> locker(globalLock);
            didGetToHook = true;
            globalCond.notify_all();
            while (!hookShouldStop)
                globalCond.wait(locker);
            hookDidStop = true;
        };

    pas_heap_ref heap = ISO_HEAP_REF_INITIALIZER(sizeof(int));

    thread thread1 = thread(
        [&] () {
            void* ptr = iso_allocate(&heap);
            CHECK(ptr);
            CHECK(pas_segregated_view_is_exclusive(
                      pas_segregated_view_for_object(
                          reinterpret_cast<uintptr_t>(ptr),
                          &iso_heap_config)));
            iso_deallocate(ptr);
            okToGetToHook = true;
            thePtr = ptr;
            pas_thread_local_cache_shrink(pas_thread_local_cache_try_get(),
                                          pas_lock_is_not_held);
            shrinkDidFinish = true;
        });

    {
        unique_lock<mutex> locker(globalLock);
        while (!didGetToHook)
            globalCond.wait(locker);
        hookShouldStop = true;
        globalCond.notify_all();
    }

    void* ptr = iso_allocate(&heap);
    if (kindToStopOn == pas_race_test_hook_local_allocator_stop_before_unlock)
        CHECK_EQUAL(ptr, thePtr);
    thread1.join();
    CHECK(hookDidStop);
    CHECK(shrinkDidFinish);
}

void testLocalAllocatorStopRaceAgainstScavenge(pas_race_test_hook_kind kindToStopOn)
{
    pas_scavenger_suspend();
    
    bool okToGetToHook = false;
    bool didGetToHook = false;
    bool hookShouldStop = false;
    bool hookDidStop = false;
    bool shrinkDidFinish = false;
    void* thePtr;
    
    hookCallback =
        [&] (pas_race_test_hook_kind kind) {
            if (kind != kindToStopOn)
                return;

            CHECK_EQUAL(locksHeldForThread[this_thread::get_id()].size(), 3);
            CHECK(okToGetToHook);

            unique_lock<mutex> locker(globalLock);
            didGetToHook = true;
            globalCond.notify_all();
            while (!hookShouldStop && !isHoldingContendedLocks())
                globalCond.wait(locker);
            hookDidStop = true;
        };

    pas_heap_ref heap = ISO_HEAP_REF_INITIALIZER(sizeof(int));

    thread thread1 = thread(
        [&] () {
            void* ptr = iso_allocate(&heap);
            CHECK(ptr);
            CHECK(pas_segregated_view_is_exclusive(
                      pas_segregated_view_for_object(
                          reinterpret_cast<uintptr_t>(ptr),
                          &iso_heap_config)));
            iso_deallocate(ptr);
            okToGetToHook = true;
            thePtr = ptr;
            pas_thread_local_cache_shrink(pas_thread_local_cache_try_get(),
                                          pas_lock_is_not_held);
            shrinkDidFinish = true;
        });

    {
        unique_lock<mutex> locker(globalLock);
        while (!didGetToHook)
            globalCond.wait(locker);
        hookShouldStop = true;
        globalCond.notify_all();
    }

    pas_scavenger_decommit_free_memory();

    void* ptr = iso_allocate(&heap);
    if (kindToStopOn == pas_race_test_hook_local_allocator_stop_before_unlock)
        CHECK_EQUAL(ptr, thePtr);
    hookShouldStop = true;
    thread1.join();
    CHECK(hookDidStop);
    CHECK(shrinkDidFinish);
}

void testMediumDirectoryTornInsertRace()
{
    pas_scavenger_suspend();

    pas_primitive_heap_ref heap = ISO_PRIMITIVE_HEAP_REF_INITIALIZER;
    constexpr size_t kBigSize = 8192;
    constexpr size_t kNewSize = 1024;

    /* This is a bit of a hack: we want to catch a race on the medium-tuple
       insertion path, but in order to set up the race we need to go through
       the insertion path once without getting stopped by the hook.
       So we start with a no-op hook and only afterwards install the effectful one. */
    hookCallback = [] (pas_race_test_hook_kind) { };
    void* seedBig = iso_allocate_primitive(&heap, kBigSize);
    CHECK(seedBig);

    bool didGetToHook = false;
    bool hookShouldStop = false;
    bool hookDidStop = false;

    hookCallback =
        [&] (pas_race_test_hook_kind kind) {
            if (kind != pas_race_test_hook_medium_directory_after_directory_store)
                return;

            unique_lock<mutex> locker(globalLock);
            didGetToHook = true;
            globalCond.notify_all();
            while (!hookShouldStop && !isHoldingContendedLocks())
                globalCond.wait(locker);
            hookDidStop = true;
        };

    thread writer = thread(
        [&] () {
            /* Inserts a smaller medium tuple at slot 0, shifting the original
               tuple to slot 1.
               Once the hook fires, slot[0]'s directory ptr will have been overwritten,
               but its begin_index/end_index fields won't yet have been.
               If there is a race here, then the reader will see
               { new_directory_ptr, old_begin_index, old_end_index } */
            void* small = iso_allocate_primitive(&heap, kNewSize);
            CHECK(small);
        });

    {
        unique_lock<mutex> locker(globalLock);
        while (!didGetToHook)
            globalCond.wait(locker);
    }

    /* A NULL cached_index forces the lookup to skip the
       basic-size-directory path, which ensures we take the medium path.
       Since the slot-write hasn't finished yet, we should expect to see
       the original directory, with object_size == kBigSize.
       If we see a directory with kNewSize, then there is a race.
       If the lock-free medium path doesn't respect the seqlock, it will
       observe the torn slot we created above. */
    size_t bigIndex = pas_segregated_heap_index_for_size(kBigSize, iso_heap_config);
    pas_segregated_size_directory* observed =
        pas_segregated_heap_size_directory_for_index(
            &heap.base.heap->segregated_heap, bigIndex, NULL, &iso_heap_config,
            pas_lock_is_not_held);

    {
        unique_lock<mutex> locker(globalLock);
        hookShouldStop = true;
        globalCond.notify_all();
    }
    writer.join();

    CHECK(hookDidStop);
    CHECK(observed);
    CHECK_GREATER_EQUAL((size_t)observed->object_size, kBigSize);
}

} // anonymous namespace

#endif // PAS_ENABLE_TESTING && PAS_ENABLE_ISO

#if PAS_ENABLE_BMALLOC

namespace {

/* The allocation slow path (pas_try_allocate_common_impl_slow) looks up the size directory of a size without
   the heap lock, then takes the lock and publishes the allocator index of that directory. The tests below run
   the two halves on one thread, with what another thread can do between them:

       stale thread:    looks up the directory of a size.
       evicting thread: makes the first request for that size class with a greater alignment. That evicts the
                        first thread's directory from the index and installs a new one.
       stale thread:    takes the heap lock and publishes.

   The stale thread must publish nothing for a directory that the heap no longer has at that index. */

enum IndexKind { SmallIndex, MediumIndex };

void checkIndexKind(pas_heap* heap, size_t size, IndexKind indexKind)
{
    bool isMedium = pas_segregated_heap_index_for_size(size, BMALLOC_HEAP_CONFIG)
        >= heap->segregated_heap.small_index_upper_bound;
    CHECK_EQUAL(isMedium, indexKind == MediumIndex);
}

pas_segregated_size_directory* lookUpDirectory(pas_heap* heap, size_t size, size_t alignment)
{
    return pas_heap_ensure_size_directory_for_size(
        heap, size, alignment, pas_force_size_lookup, BMALLOC_HEAP_CONFIG, nullptr, &bmalloc_allocator_counts);
}

pas_segregated_size_directory* currentDirectory(pas_heap* heap, size_t size)
{
    return pas_segregated_heap_size_directory_for_size(&heap->segregated_heap, size, BMALLOC_HEAP_CONFIG, nullptr);
}

unsigned publishedAllocatorIndex(pas_heap* heap, size_t size)
{
    return pas_segregated_heap_allocator_index_for_size(&heap->segregated_heap, size, BMALLOC_HEAP_CONFIG);
}

void publish(pas_segregated_size_directory* directory, size_t size)
{
    pas_segregated_size_directory_select_allocator(
        directory, size, pas_force_size_lookup, &bmalloc_heap_config, nullptr);
}

void checkAligned(void* ptr, size_t alignment)
{
    CHECK(ptr);
    CHECK(pas_is_aligned(reinterpret_cast<uintptr_t>(ptr), alignment));
}

void checkBothKindsOfRequest(size_t size, size_t alignment)
{
    for (unsigned i = 0; i < 64; ++i) {
        CHECK(bmalloc_allocate(size));
        checkAligned(bmalloc_allocate_with_alignment(size, alignment), alignment);
    }
}

/* A request of sizeOfSharedDirectory comes first, when that is not zero. `size` then shares its directory,
   which has less alignment than a directory made for `size` has. */
void testStaleDirectoryPublish(
    IndexKind indexKind, size_t size, size_t alignment, size_t sizeOfSharedDirectory, bool evictorPublishesFirst)
{
    pas_heap* heap = &bmalloc_common_primitive_heap;

    if (sizeOfSharedDirectory)
        CHECK(bmalloc_allocate(sizeOfSharedDirectory));
    CHECK(bmalloc_allocate(size));
    checkIndexKind(heap, size, indexKind);

    pas_segregated_size_directory* stale = lookUpDirectory(heap, size, 1);
    CHECK(stale);
    CHECK_LESS(pas_segregated_size_directory_alignment(stale), alignment);

    if (evictorPublishesFirst) {
        checkAligned(bmalloc_allocate_with_alignment(size, alignment), alignment);
        publish(stale, size);
    } else {
        pas_segregated_size_directory* evictor = lookUpDirectory(heap, size, alignment);
        CHECK(evictor);
        CHECK_NOT_EQUAL(evictor, stale);
        publish(stale, size);
        publish(evictor, size);
    }

    pas_segregated_size_directory* current = currentDirectory(heap, size);
    CHECK(current);
    CHECK_NOT_EQUAL(current, stale);
    CHECK_EQUAL(publishedAllocatorIndex(heap, size), current->allocator_index);

    checkBothKindsOfRequest(size, alignment);
}

/* The eviction also takes the directory away from the smaller sizes that shared it. Here the stale thread
   publishes for one of those. It has no directory until the next request for it. */
void testStaleDirectoryPublishForSmallerSize(
    IndexKind indexKind, size_t sizeOfSharedDirectory, size_t smallerSize, size_t size, size_t alignment,
    bool nextRequestIsOnAnotherThread)
{
    pas_heap* heap = &bmalloc_common_primitive_heap;

    CHECK(bmalloc_allocate(sizeOfSharedDirectory));
    CHECK(bmalloc_allocate(smallerSize));
    CHECK(bmalloc_allocate(size));
    checkIndexKind(heap, smallerSize, indexKind);

    pas_segregated_size_directory* stale = lookUpDirectory(heap, smallerSize, 1);
    CHECK(stale);
    CHECK_EQUAL(stale, lookUpDirectory(heap, size, 1));

    checkAligned(bmalloc_allocate_with_alignment(size, alignment), alignment);
    CHECK(!currentDirectory(heap, smallerSize));

    publish(stale, smallerSize);
    CHECK(!publishedAllocatorIndex(heap, smallerSize));

    if (nextRequestIsOnAnotherThread) {
        thread otherThread = thread([&] () { CHECK(bmalloc_allocate(smallerSize)); });
        otherThread.join();
    } else
        CHECK(bmalloc_allocate(smallerSize));

    pas_segregated_size_directory* current = currentDirectory(heap, smallerSize);
    CHECK(current);
    CHECK_EQUAL(publishedAllocatorIndex(heap, smallerSize), current->allocator_index);

    checkBothKindsOfRequest(size, alignment);
}

/* The evicted directory keeps the greater sizes that shared it. It still publishes for those. */
void testEvictedDirectoryPublishesForGreaterSize()
{
    static constexpr size_t sizeOfSharedDirectory = 1072;
    static constexpr size_t greaterSize = 1056;
    static constexpr size_t size = 1024;
    static constexpr size_t alignment = 1024;

    pas_heap* heap = &bmalloc_common_primitive_heap;

    CHECK(bmalloc_allocate(sizeOfSharedDirectory));
    CHECK(bmalloc_allocate(size));

    pas_segregated_size_directory* evicted = lookUpDirectory(heap, size, 1);
    CHECK(evicted);
    CHECK_EQUAL(evicted, currentDirectory(heap, greaterSize));
    CHECK(!publishedAllocatorIndex(heap, greaterSize));

    checkAligned(bmalloc_allocate_with_alignment(size, alignment), alignment);
    CHECK_NOT_EQUAL(currentDirectory(heap, size), evicted);
    CHECK_EQUAL(currentDirectory(heap, greaterSize), evicted);

    publish(evicted, greaterSize);
    CHECK_EQUAL(publishedAllocatorIndex(heap, greaterSize), evicted->allocator_index);
}

/* A heap with a heap ref keeps the allocator index of its first size class in the heap ref. */
void testStaleDirectoryPublishToHeapRef(size_t size, size_t alignment)
{
    pas_primitive_heap_ref* heapRef = &bmalloc_compact_primitive_heap_ref;

    CHECK(bmalloc_allocate_auxiliary(heapRef, size));
    pas_heap* heap = bmalloc_auxiliary_heap_ref_get_heap(heapRef);

    pas_segregated_size_directory* stale = pas_heap_ensure_size_directory_for_size(
        heap, size, 1, pas_avoid_size_lookup, BMALLOC_HEAP_CONFIG, &heapRef->cached_index,
        &bmalloc_allocator_counts);
    CHECK(stale);
    CHECK_LESS(pas_segregated_size_directory_alignment(stale), alignment);

    checkAligned(bmalloc_allocate_auxiliary_with_alignment(heapRef, size, alignment), alignment);
    unsigned allocatorIndexOfEvictor = heapRef->base.allocator_index;
    CHECK(allocatorIndexOfEvictor);
    CHECK_NOT_EQUAL(allocatorIndexOfEvictor, stale->allocator_index);

    pas_segregated_size_directory_select_allocator(
        stale, size, pas_avoid_size_lookup, &bmalloc_heap_config, &heapRef->cached_index);
    CHECK_EQUAL(heapRef->base.allocator_index, allocatorIndexOfEvictor);

    checkAligned(bmalloc_allocate_auxiliary_with_alignment(heapRef, size, alignment), alignment);
}

/* A typed heap finds the directory of its type size through the heap, not through a table. Here the first
   request is an array that is greater than the type size, so the type size shares the directory of the array.
   An aligned array between the two sizes then evicts that directory and clears the heap ref. */
void testStaleDirectoryPublishOfTypedHeap(
    size_t typeSize, size_t sizeOfSharedDirectory, size_t evictorSize, size_t evictorAlignment, bool hasSmallTable)
{
    bmalloc_type type = BMALLOC_TYPE_INITIALIZER(static_cast<unsigned>(typeSize), 1, "stale directory publish");
    pas_heap_ref heapRef = BMALLOC_HEAP_REF_INITIALIZER(&type);

    CHECK(bmalloc_iso_allocate_array_by_size(&heapRef, sizeOfSharedDirectory));
    CHECK(bmalloc_iso_allocate(&heapRef));
    pas_heap* heap = heapRef.heap;
    CHECK(heapRef.allocator_index);
    CHECK_EQUAL(!!heap->segregated_heap.small_index_upper_bound, hasSmallTable);

    pas_segregated_size_directory* stale = pas_heap_ensure_size_directory_for_size(
        heap, typeSize, 1, pas_avoid_size_lookup, BMALLOC_HEAP_CONFIG, nullptr, &bmalloc_allocator_counts);
    CHECK(stale);
    CHECK_EQUAL(stale->allocator_index, heapRef.allocator_index);

    checkAligned(
        bmalloc_iso_allocate_array_by_size_with_alignment(&heapRef, evictorSize, evictorAlignment), evictorAlignment);
    CHECK(!heapRef.allocator_index);

    pas_segregated_size_directory_select_allocator(
        stale, typeSize, pas_avoid_size_lookup, &bmalloc_heap_config, nullptr);
    CHECK(!heapRef.allocator_index);
    CHECK(!publishedAllocatorIndex(heap, typeSize));

    CHECK(bmalloc_iso_allocate(&heapRef));
    CHECK(heapRef.allocator_index);
    CHECK_NOT_EQUAL(heapRef.allocator_index, stale->allocator_index);
}

#if PAS_ENABLE_TESTING

atomic<bool> shouldParkStaleThread;
atomic<bool> didParkStaleThread;
atomic<bool> shouldResumeStaleThread;
thread_local bool isStaleThread;

void parkStaleThreadBeforeHeapLock(pas_lock* lock)
{
    if (!isStaleThread || lock != &pas_heap_lock)
        return;
    if (!shouldParkStaleThread.exchange(false))
        return;
    didParkStaleThread = true;
    while (!shouldResumeStaleThread)
        sched_yield();
}

/* The same race with two threads and the slow path as it is. The stale thread has looked up its directory
   and is about to take the heap lock to publish. The lock hook holds it there during the eviction. */
void testStaleDirectoryPublishOnAnotherThread()
{
    static constexpr size_t sizeOfSharedDirectory = 1072;
    static constexpr size_t smallerSize = 840;
    static constexpr size_t size = 1024;
    static constexpr size_t alignment = 1024;

    pas_heap* heap = &bmalloc_common_primitive_heap;

    /* The smaller size makes `size` share the directory too. Nothing is published for `size` yet. */
    CHECK(bmalloc_allocate(sizeOfSharedDirectory));
    CHECK(bmalloc_allocate(smallerSize));
    pas_segregated_size_directory* shared = currentDirectory(heap, size);
    CHECK(shared);
    CHECK_LESS(pas_segregated_size_directory_alignment(shared), alignment);
    CHECK(!publishedAllocatorIndex(heap, size));

    pas_race_test_will_lock_callback = parkStaleThreadBeforeHeapLock;

    void* objectOfStaleThread = nullptr;
    thread staleThread = thread([&] () {
        /* This gives the thread its thread local cache, so that the next heap lock is the one to publish. */
        CHECK(bmalloc_allocate(16));
        isStaleThread = true;
        shouldParkStaleThread = true;
        objectOfStaleThread = bmalloc_allocate(size);
        isStaleThread = false;
        CHECK(!shouldParkStaleThread);
        CHECK(objectOfStaleThread);
        CHECK(bmalloc_allocate(size));
    });

    while (!didParkStaleThread)
        sched_yield();

    checkAligned(bmalloc_allocate_with_alignment(size, alignment), alignment);
    pas_segregated_size_directory* current = currentDirectory(heap, size);
    CHECK(current);
    CHECK_NOT_EQUAL(current, shared);

    shouldResumeStaleThread = true;
    staleThread.join();
    pas_race_test_will_lock_callback = nullptr;

    /* The stale thread took its one object from the evicted directory and published nothing. */
    CHECK_EQUAL(
        pas_segregated_size_directory_for_object(
            reinterpret_cast<uintptr_t>(objectOfStaleThread), &bmalloc_heap_config),
        shared);
    CHECK_EQUAL(publishedAllocatorIndex(heap, size), current->allocator_index);

    checkBothKindsOfRequest(size, alignment);
}

#endif // PAS_ENABLE_TESTING

} // anonymous namespace

#endif // PAS_ENABLE_BMALLOC

void addRaceTests()
{
#if PAS_ENABLE_BMALLOC
    /* These replay their race on one thread and use no hook, so they are added before the hooks are installed. */
    ADD_TEST(testStaleDirectoryPublish(SmallIndex, 1024, 1024, 1072, true));
    ADD_TEST(testStaleDirectoryPublish(SmallIndex, 1024, 1024, 1072, false));
    ADD_TEST(testStaleDirectoryPublish(SmallIndex, 1280, 256, 0, true));
    ADD_TEST(testStaleDirectoryPublish(SmallIndex, 1280, 256, 0, false));
    ADD_TEST(testStaleDirectoryPublish(MediumIndex, 10240, 1024, 10752, true));
    ADD_TEST(testStaleDirectoryPublish(MediumIndex, 10240, 1024, 10752, false));
    ADD_TEST(testStaleDirectoryPublishForSmallerSize(SmallIndex, 1072, 840, 1024, 1024, true));
    ADD_TEST(testStaleDirectoryPublishForSmallerSize(SmallIndex, 1072, 840, 1024, 1024, false));
    ADD_TEST(testStaleDirectoryPublishForSmallerSize(MediumIndex, 10752, 10100, 10240, 1024, true));
    ADD_TEST(testStaleDirectoryPublishForSmallerSize(MediumIndex, 10752, 10100, 10240, 1024, false));
    ADD_TEST(testEvictedDirectoryPublishesForGreaterSize());
    ADD_TEST(testStaleDirectoryPublishToHeapRef(1280, 256));
    ADD_TEST(testStaleDirectoryPublishOfTypedHeap(384, 432, 416, 32, true));
    ADD_TEST(testStaleDirectoryPublishOfTypedHeap(512, 592, 576, 64, false));
#if PAS_ENABLE_TESTING
    ADD_TEST(testStaleDirectoryPublishOnAnotherThread());
#endif // PAS_ENABLE_TESTING
#endif // PAS_ENABLE_BMALLOC

#if PAS_ENABLE_TESTING && PAS_ENABLE_ISO
    InstallRaceHooks installRaceHooks;

    {
        DisableBitfit disableBitfit;
        ForceTLAs forceTLAs;

        ADD_TEST(testLocalAllocatorStopRace(pas_race_test_hook_local_allocator_stop_before_did_stop_allocating));
        ADD_TEST(testLocalAllocatorStopRace(pas_race_test_hook_local_allocator_stop_before_unlock));
        ADD_TEST(testLocalAllocatorStopRaceAgainstScavenge(pas_race_test_hook_local_allocator_stop_before_did_stop_allocating));
        ADD_TEST(testLocalAllocatorStopRaceAgainstScavenge(pas_race_test_hook_local_allocator_stop_before_unlock));

        ADD_TEST(testMediumDirectoryTornInsertRace());
    }
#endif // PAS_ENABLE_TESTING && PAS_ENABLE_ISO
}

