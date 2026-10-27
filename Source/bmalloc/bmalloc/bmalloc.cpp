/*
 * Copyright (C) 2017-2025 Apple Inc. All rights reserved.
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

#include "bmalloc.h"

#include "Environment.h"
#include "ProcessCheck.h"
#include "SystemHeap.h"
#include "VMAllocate.h"
#include <algorithm>
#include <utility>
#include <atomic>

#if BOS(LINUX)
#include <sys/mman.h>
#endif

#if BENABLE(LIBPAS)
#include "bmalloc_heap_config.h"
#include "tagged_bmalloc_heap_utils.h"
#include "pas_page_sharing_pool.h"
#include "pas_platform.h"
#include "pas_probabilistic_guard_malloc_allocator.h"
#include "pas_scavenger.h"
#include "pas_thread_local_cache.h"
#include "pas_mte_config.h"
#endif

namespace bmalloc { namespace api {

#if BUSE(LIBPAS)

#if BHAVE(36BIT_ADDRESS) && !PAS_HAVE(36BIT_ADDRESS)
#error BHAVE(36BIT_ADDRESS) is true, but PAS_HAVE(36BIT_ADDRESS) is false. They should match.
#elif !BHAVE(36BIT_ADDRESS) && PAS_HAVE(36BIT_ADDRESS)
#error BHAVE(36BIT_ADDRESS) is false, but PAS_HAVE(36BIT_ADDRESS) is true. They should match.
#endif

namespace {
static const bmalloc_type primitiveGigacageType = BMALLOC_TYPE_INITIALIZER(1, 1, "Primitive Gigacage");
} // anonymous namespace

pas_primitive_heap_ref gigacageHeaps[static_cast<size_t>(Gigacage::NumberOfKinds)] = {
    BMALLOC_AUXILIARY_HEAP_REF_INITIALIZER(&primitiveGigacageType),
};
#endif

void* mallocOutOfLine(size_t size, CompactAllocationMode mode, HeapKind kind)
{
    return malloc(size, mode, kind);
}

void freeOutOfLine(void* object, HeapKind kind)
{
#if BUSE(LIBPAS)
    BUNUSED(kind);
    if (bmalloc_try_deallocate_casual(object))
        return;
    // We stuff this in the out-of-line path because we expect it to never be
    // hit in non-hardened WebContent processes, where we're most performance-
    // sensitive. In processes where MTE is enabled, we can absorb the cost.
    if (tagged_bmalloc_try_deallocate_inline_only(object))
        return;
    tagged_bmalloc_deallocate_casual(object);
#else
    free(object, kind);
#endif
}

void* tryReallocOutOfLine(void* object, size_t newSize)
{
#if BUSE(LIBPAS)
    // Even with MTE enabled, a non-compact object may ultimately be served by
    // either the bmalloc_heap or the tagged_bmalloc_heap: only objects that
    // are *actually* tagged will route to the latter.
    // As of time of writing the only non-compact objects which won't be tagged
    // are those above PAS_MAX_MTE_TAGGABLE_OBJECT_SIZE; these are served
    // from the bmalloc_heap like usual, and delegated to libmalloc instead.
    if (tagged_bmalloc_owns_object(object))
        return tagged_bmalloc_try_reallocate(object, newSize, pas_reallocate_free_if_successful);
    return bmalloc_try_reallocate_inline(object, newSize, pas_reallocate_free_if_successful);
#else
    return tryRealloc(object, newSize, CompactAllocationMode::NonCompact);
#endif
}

void* reallocOutOfLine(void* object, size_t newSize)
{
#if BUSE(LIBPAS)
    if (tagged_bmalloc_owns_object(object))
        return tagged_bmalloc_reallocate(object, newSize, pas_reallocate_free_if_successful);
    return bmalloc_reallocate_inline(object, newSize, pas_reallocate_free_if_successful);
#else
    return realloc(object, newSize, CompactAllocationMode::NonCompact);
#endif
}

void* tryLargeZeroedMemalignVirtual(size_t requiredAlignment, size_t requestedSize, CompactAllocationMode mode, HeapKind kind)
{
    RELEASE_BASSERT(isPowerOfTwo(requiredAlignment));

    size_t pageSize = vmPageSize();
    size_t alignment = roundUpToMultipleOf(pageSize, requiredAlignment);
    size_t size = roundUpToMultipleOf(pageSize, requestedSize);
    RELEASE_BASSERT(alignment >= requiredAlignment);
    RELEASE_BASSERT(size >= requestedSize);

    void* result;
    if (auto* systemHeap = SystemHeap::tryGetIfShouldSupplantBmalloc())
        result = systemHeap->memalignLarge(alignment, size);
    else
        result = tryMemalign(alignment, size, mode, kind);

    if (result)
        vmZeroAndPurge(result, size);

    return result;
}

void freeLargeVirtual(void* object, size_t size, HeapKind kind)
{
    if (auto* systemHeap = SystemHeap::tryGetIfShouldSupplantBmalloc()) {
        systemHeap->freeLarge(object);
        return;
    }
#if BUSE(LIBPAS)
    BUNUSED(size);
    BUNUSED(kind);
    bmalloc_deallocate_inline(object);
#elif BUSE(MIMALLOC)
    BUNUSED(size);
    BUNUSED(kind);
    mi_free(object);
#else
    BUNUSED(size);
    BUNUSED(kind);
    ::free(object);
#endif
}

#if BUSE(MIMALLOC)
#if BOS(LINUX)
static constexpr size_t sizeOfBallastPiece = 1024 * 1024;
static constexpr size_t maxNumberOfBallastPieces = 64;
static Mutex s_ballastLock;
static void* s_ballastPieces[maxNumberOfBallastPieces];
static std::atomic<size_t> s_numberOfBallastPieces { 0 };
static std::atomic<size_t> s_numberOfBallastPiecesWanted { 0 };
#endif
static std::atomic<bool> s_lacksPiecesOfBallast { false };
static std::atomic<AddressSpaceBallastHandler> s_ballastHandler { nullptr };
static std::atomic<AddressSpaceBallastKeeper> s_ballastKeeper { nullptr };
static thread_local unsigned t_numberOfRequestsWithBallast;
static thread_local bool t_hasUsedBallast;
static thread_local bool t_hasUsedWholeBallast;
static thread_local bool t_isExemptFromBallast;

void setAddressSpaceBallastHandler(AddressSpaceBallastHandler handler)
{
    s_ballastHandler.store(handler, std::memory_order_relaxed);
}

void setAddressSpaceBallastKeeper(AddressSpaceBallastKeeper keeper)
{
    s_ballastKeeper.store(keeper, std::memory_order_relaxed);
}

void exemptThisThreadFromAddressSpaceBallast()
{
    t_isExemptFromBallast = true;
}

bool hasWholeAddressSpaceBallast()
{
    return !s_lacksPiecesOfBallast.load(std::memory_order_relaxed);
}

size_t sizeOfAddressSpaceBallastKept()
{
#if BOS(LINUX)
    return s_numberOfBallastPieces.load(std::memory_order_relaxed) * sizeOfBallastPiece;
#else
    return 0;
#endif
}

static void beginRequestWithBallast()
{
    ++t_numberOfRequestsWithBallast;
}

static void endRequestWithBallast()
{
    if (--t_numberOfRequestsWithBallast || !t_hasUsedBallast)
        return;
    t_hasUsedBallast = false;
    bool wasWhole = std::exchange(t_hasUsedWholeBallast, false);
    if (AddressSpaceBallastHandler handler = s_ballastHandler.load(std::memory_order_relaxed))
        handler(wasWhole);
}

RequestWithBallast::RequestWithBallast()
{
    beginRequestWithBallast();
}

RequestWithBallast::~RequestWithBallast()
{
    endRequestWithBallast();
}

#if BUSE(EXTERNAL_MIMALLOC) && BOS(LINUX)
static bool tryReleasePieceOfAddressSpaceBallast()
{
    LockHolder locker(s_ballastLock);
    size_t numberOfPieces = s_numberOfBallastPieces.load(std::memory_order_relaxed);
    if (!numberOfPieces)
        return false;
    if (!s_lacksPiecesOfBallast.exchange(true, std::memory_order_relaxed))
        t_hasUsedWholeBallast = true;
    s_numberOfBallastPieces.store(--numberOfPieces, std::memory_order_relaxed);
    munmap(s_ballastPieces[numberOfPieces], sizeOfBallastPiece);
    return true;
}

static bool makeRoomWithBallast(size_t size, size_t attempt)
{
    if (!t_numberOfRequestsWithBallast)
        return false;
    // Nothing is let go of for a request that all the pieces together could not hold.
    if (size > (attempt + s_numberOfBallastPieces.load(std::memory_order_relaxed)) * sizeOfBallastPiece)
        return false;
    // Some of what the last piece left may still be free, and was refused to a request that did not say that it may not fail.
    if (!attempt && s_lacksPiecesOfBallast.load(std::memory_order_relaxed))
        return true;
    if (!tryReleasePieceOfAddressSpaceBallast())
        return false;
    t_hasUsedBallast = true;
    return true;
}

static bool mayTakeAddressSpace(size_t size)
{
    if (!s_lacksPiecesOfBallast.load(std::memory_order_relaxed) || t_numberOfRequestsWithBallast || t_isExemptFromBallast)
        return true;
    AddressSpaceBallastKeeper keeper = s_ballastKeeper.load(std::memory_order_relaxed);
    if (!keeper)
        return false;
    size_t addressSpaceLeft = keeper();
    if (!s_lacksPiecesOfBallast.load(std::memory_order_relaxed))
        return true;
    size_t wanted = s_numberOfBallastPiecesWanted.load(std::memory_order_relaxed);
    size_t kept = s_numberOfBallastPieces.load(std::memory_order_relaxed);
    return wanted <= kept || (addressSpaceLeft >= size && addressSpaceLeft - size >= (wanted - kept) * sizeOfBallastPiece);
}
#endif

bool RequestWithBallast::tryMakeRoom()
{
#if BUSE(EXTERNAL_MIMALLOC) && BOS(LINUX)
    if (!tryReleasePieceOfAddressSpaceBallast())
        return false;
    t_hasUsedBallast = true;
    return true;
#else
    return false;
#endif
}

#if BUSE(EXTERNAL_MIMALLOC)
// patches/mimalloc/remember-the-request-that-found-no-memory.patch, ask-who-may-take-address-space.patch and new-uses-address-space-kept-aside.patch in oven-sh/bun. The vendored mimalloc does not have them, so they are declared here and not in mimalloc.h.
#if !defined(MI_REMEMBERS_FAILED_REQUEST)
extern "C" {
typedef struct mi_failed_request_s {
    void* block;
    size_t size;
    size_t alignment;
} mi_failed_request_t;
bool mi_last_failed_request(mi_failed_request_t*) noexcept;
}
#endif
#if !defined(MI_ASKS_ADDRESS_SPACE_KEEPER)
extern "C" {
typedef struct mi_address_space_keeper_s {
    bool (*may_take)(size_t size);
    bool (*make_room)(size_t size, size_t attempt);
    void (*begin_request_that_cannot_fail)(void);
    void (*end_request_that_cannot_fail)(void);
} mi_address_space_keeper_t;
void mi_register_address_space_keeper(const mi_address_space_keeper_t*) noexcept;
}
#endif
#endif

void keepAddressSpaceBallast(size_t size, size_t addressSpaceLeft, size_t addressSpaceToLeaveFree)
{
#if BOS(LINUX)
    size_t wanted = std::min(size / sizeOfBallastPiece, maxNumberOfBallastPieces);
    LockHolder locker(s_ballastLock);
#if BUSE(EXTERNAL_MIMALLOC)
    static bool hasRegistered;
    if (!hasRegistered) {
        static constexpr mi_address_space_keeper_t keeper { mayTakeAddressSpace, makeRoomWithBallast, beginRequestWithBallast, endRequestWithBallast };
        mi_register_address_space_keeper(&keeper);
        hasRegistered = true;
    }
#endif
    s_numberOfBallastPiecesWanted.store(wanted, std::memory_order_relaxed);
    size_t numberOfPieces = s_numberOfBallastPieces.load(std::memory_order_relaxed);
    while (numberOfPieces < wanted) {
        if (addressSpaceLeft < addressSpaceToLeaveFree + sizeOfBallastPiece)
            break;
        void* piece = mmap(nullptr, sizeOfBallastPiece, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (piece == MAP_FAILED)
            break;
        addressSpaceLeft -= sizeOfBallastPiece;
        s_ballastPieces[numberOfPieces++] = piece;
    }
    s_numberOfBallastPieces.store(numberOfPieces, std::memory_order_relaxed);
    // A piece that could never be kept is not missing: nothing has used it.
    if (numberOfPieces >= wanted)
        s_lacksPiecesOfBallast.store(false, std::memory_order_relaxed);
#else
    BUNUSED(size);
    BUNUSED(addressSpaceLeft);
    BUNUSED(addressSpaceToLeaveFree);
#endif
}

template<typename Function>
static void* askAgainWithBallast(const Function& tryAllocate)
{
#if BUSE(EXTERNAL_MIMALLOC)
    mi_failed_request_t request;
    if (!mi_last_failed_request(&request))
        return nullptr;
    RequestWithBallast requestWithBallast;
    return tryAllocate(request);
#else
    BUNUSED(tryAllocate);
    return nullptr;
#endif
}

void* mallocAgainWithBallast()
{
    return askAgainWithBallast([](auto& request) { return mi_malloc(request.size); });
}

void* zeroedMallocAgainWithBallast()
{
    return askAgainWithBallast([](auto& request) { return mi_zalloc(request.size); });
}

void* memalignAgainWithBallast()
{
    return askAgainWithBallast([](auto& request) { return mi_malloc_aligned(request.size, request.alignment); });
}

void* zeroedMemalignAgainWithBallast()
{
    return askAgainWithBallast([](auto& request) { return mi_zalloc_aligned(request.size, request.alignment); });
}

void* reallocAgainWithBallast()
{
    return askAgainWithBallast([](auto& request) { return mi_realloc(request.block, request.size); });
}
#endif // BUSE(MIMALLOC)

void scavengeThisThread(bool force)
{
#if BENABLE(LIBPAS)
    BUNUSED(force);
    pas_thread_local_cache_shrink(pas_thread_local_cache_try_get(),
                                  pas_lock_is_not_held);
#elif BUSE(MIMALLOC)
    mi_theap_collect(mi_theap_get_default(), force);
#else
    BUNUSED(force);
#endif
}

#if BUSE(EXTERNAL_MIMALLOC)
// oven-sh/mimalloc's idle hook: collects the thread's heap, discards the free runs inside the pages
// it still uses, and hands the arena purge to mimalloc's scavenger thread. The vendored mimalloc
// does not have it, so it is declared here and not in mimalloc.h.
extern "C" void mi_on_thread_idle(void) noexcept;
#endif

void scavengeThisThreadOnIdle()
{
#if BUSE(EXTERNAL_MIMALLOC)
    mi_on_thread_idle();
#else
    // Not forced: that would also purge what every other thread just freed.
    scavengeThisThread(/* force */ false);
#endif
}

void scavenge()
{
#if BENABLE(LIBPAS)
    pas_scavenger_run_synchronously_now();
    scavengeThisThread();
#elif BUSE(MIMALLOC)
    mi_collect(/* force */ true);
#endif
    if (SystemHeap* systemHeap = SystemHeap::tryGetIfShouldSupplantBmalloc()) {
        systemHeap->scavenge();
        return;
    }
}

bool isEnabled(HeapKind)
{
    return !Environment::get()->shouldBmallocAllocateThroughSystemHeap();
}

#if BOS(DARWIN)
void setScavengerThreadQOSClass(qos_class_t overrideClass)
{
#if BENABLE(LIBPAS)
    pas_scavenger_set_requested_qos_class(overrideClass);
#else
    BUNUSED(overrideClass);
#endif
}
#endif

void commitAlignedPhysical(void* object, size_t size, HeapKind)
{
    vmValidatePhysical(object, size);
    vmAllocatePhysicalPages(object, size);
}

void decommitAlignedPhysical(void* object, size_t size, HeapKind)
{
    vmValidatePhysical(object, size);
    vmDeallocatePhysicalPages(object, size);
}

void enableMiniMode(bool forceMiniMode)
{
#if BENABLE(LIBPAS)
    if (!forceMiniMode && !shouldAllowMiniMode())
        return;

    // Speed up the scavenger.
    pas_scavenger_period_in_milliseconds = 5.;
    pas_scavenger_max_epoch_delta = 5ll * 1000ll * 1000ll;

    // Do eager scavenging anytime pages are allocated or committed.
    pas_physical_page_sharing_pool_balancing_enabled = true;
    pas_physical_page_sharing_pool_balancing_enabled_for_utility = true;

    pas_bmalloc_force_allocations_into_bitfit_heaps_where_available();
#else
    BUNUSED_PARAM(forceMiniMode);
#endif // BENABLE(LIBPAS)
}

void disableScavenger()
{
#if BENABLE(LIBPAS)
    pas_scavenger_suspend();
#endif
}

void forceEnablePGM(uint16_t guardMallocRate)
{
#if BUSE(LIBPAS)
    pas_probabilistic_guard_malloc_initialize_pgm_as_enabled(guardMallocRate);
#else
    BUNUSED_PARAM(guardMallocRate);
#endif
}

} } // namespace bmalloc::api

