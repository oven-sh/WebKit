/*
 * Copyright (C) 2012-2018 Apple Inc. All rights reserved.
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

#include "IndexingHeader.h"
#include "IndexingType.h"
#include "PropertyStorage.h"
#include <wtf/Assertions.h>
#include <wtf/Atomics.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/ThreadSanitizerSupport.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class VM;
class CopyVisitor;
class GCDeferralContext;
struct ArrayStorage;

template <typename T>
struct ContiguousData {
    ContiguousData() = default;
    ContiguousData(T* data, size_t length)
        : m_data(data)
#if ASSERT_ENABLED
        , m_length(length)
#endif
    {
        UNUSED_PARAM(length);
    }

    struct Data {
        Data(T& location, IndexingType indexingMode)
            : m_data(location)
#if ASSERT_ENABLED
            , m_isWritable(!isCopyOnWrite(indexingMode))
#endif
        {
            UNUSED_PARAM(indexingMode);
        }

        explicit operator bool() const { return !!m_data.get(); }

        const T& operator=(const T& value)
        {
            ASSERT(m_isWritable);
            m_data = value;
            return value;
        }

        operator const T&() const { return m_data; }

        // WriteBarrier forwarded methods.

        void set(VM& vm, const JSCell* owner, const JSValue& value)
        {
            ASSERT(m_isWritable);
            m_data.set(vm, owner, value);
        }

        void setWithoutWriteBarrier(const JSValue& value)
        {
            ASSERT(m_isWritable);
            m_data.setWithoutWriteBarrier(value);
        }

        void setStartingValue(JSValue value)
        {
            m_data.setStartingValue(value);
        }

        void clear()
        {
            ASSERT(m_isWritable);
            m_data.clear();
        }

        JSValue get() const
        {
            return m_data.get();
        }


        T& m_data;
#if ASSERT_ENABLED
        bool m_isWritable;
#endif
    };

    const Data at(const JSCell* owner, size_t index) const;
    Data at(const JSCell* owner, size_t index);

    T& atUnsafe(size_t index) { ASSERT(index < m_length); return m_data[index]; }

    T* data() const { return m_data; }
#if ASSERT_ENABLED
    size_t length() const { return m_length; }
#endif

private:
    T* m_data { nullptr };
#if ASSERT_ENABLED
    size_t m_length { 0 };
#endif
};

using ContiguousDoubles = ContiguousData<double>;
using ContiguousJSValues = ContiguousData<WriteBarrier<Unknown>>;
using ConstContiguousDoubles = ContiguousData<const double>;
using ConstContiguousJSValues = ContiguousData<const WriteBarrier<Unknown>>;

class Butterfly {
    WTF_MAKE_NONCOPYABLE(Butterfly);
private:
    Butterfly() { } // Not instantiable.
public:
    
    static size_t totalSize(size_t preCapacity, size_t propertyCapacity, bool hasIndexingHeader, size_t indexingPayloadSizeInBytes)
    {
        ASSERT(!indexingPayloadSizeInBytes || hasIndexingHeader);
        ASSERT(sizeof(EncodedJSValue) == sizeof(IndexingHeader));
        return (preCapacity + propertyCapacity) * sizeof(EncodedJSValue) + (hasIndexingHeader ? sizeof(IndexingHeader) : 0) + indexingPayloadSizeInBytes;
    }

    static Butterfly* fromBase(void* base, size_t preCapacity, size_t propertyCapacity)
    {
        return reinterpret_cast<Butterfly*>(static_cast<EncodedJSValue*>(base) + preCapacity + propertyCapacity + 1);
    }
    
    ALWAYS_INLINE static unsigned availableContiguousVectorLength(size_t propertyCapacity, unsigned vectorLength);
    static unsigned availableContiguousVectorLength(Structure*, unsigned vectorLength);
    
    ALWAYS_INLINE static unsigned optimalContiguousVectorLength(size_t propertyCapacity, unsigned vectorLength);
    static unsigned optimalContiguousVectorLength(Structure*, unsigned vectorLength);
    
    // This method is here not just because it's handy, but to remind you that
    // the whole point of butterflies is to do evil pointer arithmetic.
    static Butterfly* fromPointer(char* ptr)
    {
        return reinterpret_cast<Butterfly*>(ptr);
    }
    
    char* pointer() { return reinterpret_cast<char*>(this); }
    
    static constexpr ptrdiff_t offsetOfIndexingHeader() { return IndexingHeader::offsetOfIndexingHeader(); }
    static constexpr ptrdiff_t offsetOfArrayBuffer() { return offsetOfIndexingHeader() + IndexingHeader::offsetOfArrayBuffer(); }
    static constexpr ptrdiff_t offsetOfPublicLength() { return offsetOfIndexingHeader() + IndexingHeader::offsetOfPublicLength(); }
    static constexpr ptrdiff_t offsetOfVectorLength() { return offsetOfIndexingHeader() + IndexingHeader::offsetOfVectorLength(); }

    static Butterfly* tryCreateUninitialized(VM&, JSObject* intendedOwner, size_t preCapacity, size_t propertyCapacity, bool hasIndexingHeader, size_t indexingPayloadSizeInBytes, GCDeferralContext* = nullptr);
    static Butterfly* createUninitialized(VM&, JSObject* intendedOwner, size_t preCapacity, size_t propertyCapacity, bool hasIndexingHeader, size_t indexingPayloadSizeInBytes);

    // FIXME: These return uninitialized indexed storage. Either their names should be updated to reflect this
    // and/or they should take some kind of initialization scope.
    static Butterfly* tryCreate(VM& vm, JSObject*, size_t preCapacity, size_t propertyCapacity, bool hasIndexingHeader, const IndexingHeader& indexingHeader, size_t indexingPayloadSizeInBytes);
    static Butterfly* create(VM&, JSObject* intendedOwner, size_t preCapacity, size_t propertyCapacity, bool hasIndexingHeader, const IndexingHeader&, size_t indexingPayloadSizeInBytes);
    static Butterfly* create(VM&, JSObject* intendedOwner, Structure*);
    
    IndexingHeader* indexingHeader() { return IndexingHeader::from(this); }
    const IndexingHeader* indexingHeader() const { return IndexingHeader::from(this); }
    PropertyStorage propertyStorage() { return indexingHeader()->propertyStorage(); }
    ConstPropertyStorage propertyStorage() const { return indexingHeader()->propertyStorage(); }
    
    // TSAN-TRIAGE §3.15: these delegate to the IndexingHeader accessors, which
    // are relaxed atomics (C4 stale-tolerant; flag-off codegen unchanged).
    uint32_t publicLength() const { return indexingHeader()->publicLength(); }
    uint32_t vectorLength() const { return indexingHeader()->vectorLength(); }
    void setPublicLength(uint32_t value) { indexingHeader()->setPublicLength(value); }
    void setVectorLength(uint32_t value) { indexingHeader()->setVectorLength(value); }

    // SPEC-objectmodel review round 2: monotone publicLength bump for SHARED
    // flat words (SW=1). Two racing dense growers each store their slot and
    // then read-then-plain-store the length; the loser's smaller store could
    // REGRESS publicLength and hide the winner's element (I21 "no lost
    // properties"; i03-t5-racing-growers part (a)). A CAS-max loop makes the
    // bump monotone. Owner-exclusive (t, 0) words keep the plain
    // setPublicLength store; deliberate truncation (setLength/shrink) also
    // stays a plain store - shrink-vs-grow is program-order racy by SAB
    // semantics.
    //
    // AB17f (I21 publication ordering): the successful CAS is a RELEASE so
    // the dense element store program-order before it (the
    // trySetIndexQuicklyConcurrent setWithoutWriteBarrier / raw double
    // store) cannot be reordered AFTER the length becomes visible — a
    // relaxed bump let a weak-memory reader observe the bumped length while
    // missing the element. Free on x86-64 (every atomic RMW is already a
    // full fence). KNOWN RESIDUAL (I21 publication, reader side): readers
    // (tryGetIndexQuicklyConcurrent and friends) load publicLength relaxed
    // with no acquire/dependency edge, so ARM64 load-load reordering can
    // still pair a fresh length with a stale (empty) element read; the
    // benign outcome is a spurious hole => generic-path fallback, same
    // class as the IT-8 reader-side residual (ScriptExecutable.cpp).
    // TSO-sound; ARM64 reader-side acquire is chartered with IT-8.
    void bumpPublicLengthToAtLeast(uint32_t newLength)
    {
        uint32_t* location = reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(this) + offsetOfPublicLength());
        uint32_t current = WTF::atomicLoad(location, std::memory_order_relaxed);
        while (current < newLength) {
            uint32_t observed = WTF::atomicCompareExchangeStrong(location, current, newLength, std::memory_order_release);
            if (observed == current)
                return;
            current = observed;
        }
    }

    template<typename T>
    T* indexingPayload() { return reinterpret_cast_ptr<T*>(this); }
    ArrayStorage* arrayStorage() { return indexingPayload<ArrayStorage>(); }
    ContiguousJSValues contiguousInt32() { return ContiguousJSValues(indexingPayload<WriteBarrier<Unknown>>(), vectorLength()); }
    ContiguousDoubles contiguousDouble() { return ContiguousDoubles(indexingPayload<double>(), vectorLength()); }
    ContiguousJSValues contiguous() { return ContiguousJSValues(indexingPayload<WriteBarrier<Unknown>>(), vectorLength()); }

    template<typename T>
    const T* indexingPayload() const { return reinterpret_cast_ptr<const T*>(this); }
    const ArrayStorage* arrayStorage() const { return indexingPayload<ArrayStorage>(); }
    ConstContiguousJSValues contiguousInt32() const { return ConstContiguousJSValues(indexingPayload<WriteBarrier<Unknown>>(), vectorLength()); }
    ConstContiguousDoubles contiguousDouble() const { return ConstContiguousDoubles(indexingPayload<double>(), vectorLength()); }
    ConstContiguousJSValues contiguous() const { return ConstContiguousJSValues(indexingPayload<WriteBarrier<Unknown>>(), vectorLength()); }
    
    static Butterfly* fromContiguous(WriteBarrier<Unknown>* contiguous)
    {
        return reinterpret_cast<Butterfly*>(contiguous);
    }
    static Butterfly* fromContiguous(double* contiguous)
    {
        return reinterpret_cast<Butterfly*>(contiguous);
    }
    
    static constexpr ptrdiff_t offsetOfPropertyStorage() { return -static_cast<ptrdiff_t>(sizeof(IndexingHeader)); }
    constexpr static int indexOfPropertyStorage()
    {
        ASSERT(sizeof(IndexingHeader) == sizeof(EncodedJSValue));
        return -1;
    }

    void* base(size_t preCapacity, size_t propertyCapacity) { return propertyStorage() - propertyCapacity - preCapacity; }
    void* base(Structure*);

    // FIXME: This returns uninitialized indexed storage. Either their names should be updated to reflect this
    // and/or they should take some kind of initialization scope.
    static Butterfly* createOrGrowArrayRight(
        Butterfly*, VM&, JSObject* intendedOwner, Structure* oldStructure,
        size_t propertyCapacity, bool hadIndexingHeader,
        size_t oldIndexingPayloadSizeInBytes, size_t newIndexingPayloadSizeInBytes); 

    // The butterfly reallocation methods perform the reallocation itself but do not change any
    // of the meta-data to reflect that the reallocation occurred. Note that this set of
    // methods is not exhaustive and is not intended to encapsulate all possible allocation
    // modes of butterflies - there are code paths that allocate butterflies by calling
    // directly into Heap::tryAllocateStorage.
    // FIXME: These return uninitialized indexed storage. Either their names should be updated to reflect this
    // and/or they should take some kind of initialization scope.
    static Butterfly* createOrGrowPropertyStorage(Butterfly*, VM&, JSObject* intendedOwner, Structure*, size_t oldPropertyCapacity, size_t newPropertyCapacity);
    Butterfly* growArrayRight(VM&, JSObject* intendedOwner, Structure* oldStructure, size_t propertyCapacity, bool hadIndexingHeader, size_t oldIndexingPayloadSizeInBytes, size_t newIndexingPayloadSizeInBytes); // Assumes that preCapacity is zero, and asserts as much.
    Butterfly* growArrayRight(VM&, JSObject* intendedOwner, Structure*, size_t newIndexingPayloadSizeInBytes);

    Butterfly* reallocArrayRightIfPossible(VM&, GCDeferralContext&, JSObject* intendedOwner, Structure* oldStructure, size_t propertyCapacity, bool hadIndexingHeader, size_t oldIndexingPayloadSizeInBytes, size_t newIndexingPayloadSizeInBytes); // Assumes that preCapacity is zero, and asserts as much.

    Butterfly* resizeArray(VM&, JSObject* intendedOwner, size_t propertyCapacity, bool oldHasIndexingHeader, size_t oldIndexingPayloadSizeInBytes, size_t newPreCapacity, bool newHasIndexingHeader, size_t newIndexingPayloadSizeInBytes);
    Butterfly* resizeArray(VM&, JSObject* intendedOwner, Structure*, size_t newPreCapacity, size_t newIndexingPayloadSizeInBytes); // Assumes that you're not changing whether or not the object has an indexing header.
    Butterfly* unshift(Structure*, size_t numberOfSlots);
    Butterfly* shift(Structure*, size_t numberOfSlots);

    // FIXME: This should either not be static or take a span.
    ALWAYS_INLINE static void clearRange(IndexingType, Butterfly*, unsigned start, unsigned end);
};

// ===== Segmented butterflies (SPEC-objectmodel §4.1; shared-memory threads) =====
//
// When an object stops being transition-thread-local (a foreign transition, or
// an owner transition with the shared-write bit set), its out-of-line storage
// is re-published as a SEGMENTED butterfly: the tagged butterfly word
// (TID == notTTLTID, SW = 1 - see ConcurrentButterfly.h) carries a
// ButterflySpine* instead of a Butterfly*. The spine is IMMUTABLE after
// publication (I6): growth allocates a replacement spine (copy + append);
// fragments never move and are never reused for a different role, so every
// racing access during a reshape lands on the same fragment memory.
//
// Flat aliasing (§4.2): flat->segmented conversion is zero-copy - the spine's
// fragment pointers point at 32-byte slices of the pre-existing flat
// butterfly B, so the §4.1 address equations below must reproduce every
// pre-existing slot's flat address (I8):
//
//   out-of-line k:   B - 16 - 8k   = fragment k/4, slot 3 - (k % 4)
//                                    (fragment j base = B - 40 - 32j; slots
//                                    ascend while flat out-of-line descends)
//   element i:       B + 8i        = fragment (i + 1)/4, slot (i + 1) % 4
//                                    (fragment f base = B - 8 + 32f; the +1
//                                    hides the IndexingHeader slot)
//   IndexingHeader:  [B - 8, B)    = indexed fragment 0 slot 0, frozen: live
//                                    publicLength stays in its low half (C4:
//                                    shared by every spine the object ever
//                                    publishes), the flat-era vectorLength in
//                                    its high half is frozen forever (I9b).
//
// ButterflyInlines.h provides the conversion-time aliasing helpers
// (aliased*ForConversion) and validateSpineAliasesFlatButterfly(), the
// slot-by-slot I8 debug check. Nothing here is reachable with useJSThreads
// off (I22): no spine is ever published, so these types compile dark.

static constexpr size_t butterflyFragmentSlots = 4;
static constexpr size_t butterflyFragmentBytes = 32;

struct ButterflyFragment {
    WriteBarrierBase<Unknown> slots[butterflyFragmentSlots]; // Mutable; never moves (§4.1).
};

static_assert(sizeof(ButterflyFragment) == butterflyFragmentBytes, "fragments are exactly 32 bytes");
static_assert(sizeof(WriteBarrierBase<Unknown>) == sizeof(EncodedJSValue), "fragment slots are one JSValue each");

// Index -> (fragment, slot) mapping (§4.1). k is the index into out-of-line
// storage (= offsetInOutOfLineStorage(PropertyOffset)); i is an array index.
constexpr unsigned butterflyOutOfLineIndexToFragment(unsigned k) { return k / butterflyFragmentSlots; }
constexpr unsigned butterflyOutOfLineIndexToSlot(unsigned k) { return (butterflyFragmentSlots - 1) - (k % butterflyFragmentSlots); } // Slots ascend, flat out-of-line descends.
constexpr unsigned butterflyIndexedIndexToFragment(unsigned i) { return (i + 1) / butterflyFragmentSlots; } // +1 = the IndexingHeader slot,
constexpr unsigned butterflyIndexedIndexToSlot(unsigned i) { return (i + 1) % butterflyFragmentSlots; } // hidden by the accessors.

// Compile-time I8 check: composing the (fragment, slot) mapping with the
// aliased fragment-base equations reproduces the flat address equations
// (out-of-line k at B - 16 - 8k; element i at B + 8i; header at B - 8) for
// every index. Offsets are relative to B.
constexpr bool butterflyAliasEquationsHold()
{
    for (unsigned k = 0; k < 4 * butterflyFragmentSlots; ++k) {
        ptrdiff_t fragmentBase = -40 - 32 * static_cast<ptrdiff_t>(butterflyOutOfLineIndexToFragment(k));
        if (fragmentBase + 8 * static_cast<ptrdiff_t>(butterflyOutOfLineIndexToSlot(k)) != -16 - 8 * static_cast<ptrdiff_t>(k))
            return false;
    }
    for (unsigned i = 0; i < 4 * butterflyFragmentSlots; ++i) {
        ptrdiff_t fragmentBase = -8 + 32 * static_cast<ptrdiff_t>(butterflyIndexedIndexToFragment(i));
        if (fragmentBase + 8 * static_cast<ptrdiff_t>(butterflyIndexedIndexToSlot(i)) != 8 * static_cast<ptrdiff_t>(i))
            return false;
    }
    // The IndexingHeader aliases indexed fragment 0 slot 0: base -8 + 32*0 + 8*0 == -8.
    return butterflyIndexedIndexToFragment(0) == 0 && butterflyIndexedIndexToSlot(0) == 1;
}
static_assert(butterflyAliasEquationsHold(), "I8: the §4.1 equations reproduce every flat slot address");

// V7 (TSAN): spine words are racy-but-safe by design (I6 fenced publish +
// address dependency). Express that contract as relaxed atomics on BOTH the
// publishing side (ConcurrentButterfly.cpp spine construction) and every
// reader, so the race detector sees the contract the hardware enforces.
// Relaxed 32/64-bit loads/stores compile to the same plain MOV/LDR/STR on
// x86-64/arm64, and segmented spines only exist flag-on (I22), so flag-off
// codegen is untouched. NOTE (review round 1): relaxed atomic pairs only
// exempt the annotated WORDS from reporting — they do NOT transfer TSAN
// vector clocks. The happens-before edge for everything plain-initialized
// before publication (fragment contents, spine fields) is established
// separately: the converter calls ButterflySpine::tsanPublish() after the
// last pre-publication store, and every consumer entry point
// (ConcurrentButterfly.cpp segmented* wrappers) calls tsanConsume(), which
// pair via TSAN_ANNOTATE_HAPPENS_BEFORE/AFTER on the spine address. Both are
// no-ops outside TSAN builds.
template<typename T>
ALWAYS_INLINE T butterflyConcurrentLoad(const T* location)
{
    return WTF::atomicLoad(const_cast<T*>(location), std::memory_order_relaxed);
}

template<typename T>
ALWAYS_INLINE void butterflyConcurrentStore(T* location, T value)
{
    WTF::atomicStore(location, value, std::memory_order_relaxed);
}

// THREADS/TSAN-gated bulk copy/zero of butterfly payload words. A fresh (or
// in-resize) butterfly's words can pair with STALE concurrent readers'/writers'
// atomics at GC-recycled aux addresses, and resize sources can be written
// atomically by racing element stores — both blessed by the object-model
// staleness rules, but the bulk memcpy/memset must be word-wise atomics to be
// defined and TSAN-visible. Production builds keep memcpy/memset. Regions must
// be 8-byte aligned multiples of 8.
ALWAYS_INLINE void butterflyConcurrentCopyWords(void* dst, const void* src, size_t bytes)
{
#if TSAN_ENABLED
    ASSERT(!(bytes % sizeof(uint64_t)));
    uint64_t* to = static_cast<uint64_t*>(dst);
    const uint64_t* from = static_cast<const uint64_t*>(src);
    for (size_t i = 0; i < bytes / sizeof(uint64_t); ++i)
        WTF::atomicStore(&to[i], WTF::atomicLoad(const_cast<uint64_t*>(&from[i]), std::memory_order_relaxed), std::memory_order_relaxed);
#else
    memcpy(dst, src, bytes);
#endif
}

ALWAYS_INLINE void butterflyConcurrentZeroWords(void* dst, size_t bytes)
{
#if TSAN_ENABLED
    ASSERT(!(bytes % sizeof(uint64_t)));
    uint64_t* to = static_cast<uint64_t*>(dst);
    for (size_t i = 0; i < bytes / sizeof(uint64_t); ++i)
        WTF::atomicStore(&to[i], static_cast<uint64_t>(0), std::memory_order_relaxed);
#else
    memset(dst, 0, bytes);
#endif
}

struct ButterflySpine {
    // IMMUTABLE after publication (I6): every field is written before the
    // spine is published (§4.2 step 5 / §4.3 step 5) and never again; growth
    // allocates a replacement spine. Readers therefore need no fences past
    // the address dependency on the tagged butterfly word (M1/I23).
    uint32_t outOfLineFragmentCount; // Left side.
    uint32_t indexedFragmentCount; // Right side; 0 iff the flat butterfly had no IndexingHeader (C2) - then there is no header fragment and the publicLength accessors RELEASE_ASSERT.
    uint32_t vectorLength; // Authoritative live vector length; immutable per spine (§4.1). The flat-era vectorLength (indexed fragment 0 slot 0, high half) is frozen forever (I9b).
    uint32_t spineEpoch; // Monotonic per object; debug only.
    void* aliasedAllocationBase; // Allocation base of the aliased flat butterfly; null if none.
    uint64_t aliasedAllocationSize; // 0 if none; GC marks the base every visit (§4.5/I7). BOTH copied VERBATIM to every replacement spine; immutable once set.
    // Followed in the same allocation by:
    //   ButterflyFragment* fragments[outOfLineFragmentCount + indexedFragmentCount];
    // out-of-line fragments first (§4.1).

    static constexpr size_t allocationSize(uint32_t totalFragmentCount)
    {
        return sizeof(ButterflySpine) + static_cast<size_t>(totalFragmentCount) * sizeof(ButterflyFragment*);
    }

    uint32_t totalFragmentCount() const { return outOfLineFragmentCount + indexedFragmentCount; }

    // V7 (TSAN): relaxed getters for accesses to a PUBLISHED spine that race
    // with another thread's view of the same words. Identical codegen to the
    // plain reads; debug ASSERTs use them too in case the TSAN rig builds
    // with ASSERT_ENABLED (review amendment E).
    // T2-segmented-accessors-inline: ALWAYS_INLINE — these and the slot
    // resolvers below are the hot spine→fragment→slot arithmetic; perf
    // showed them behind out-of-line frames at W>=2.
    ALWAYS_INLINE uint32_t outOfLineFragmentCountConcurrent() const { return butterflyConcurrentLoad(&outOfLineFragmentCount); }
    ALWAYS_INLINE uint32_t indexedFragmentCountConcurrent() const { return butterflyConcurrentLoad(&indexedFragmentCount); }
    ALWAYS_INLINE uint32_t vectorLengthConcurrent() const { return butterflyConcurrentLoad(&vectorLength); }
    ALWAYS_INLINE uint32_t totalFragmentCountConcurrent() const { return outOfLineFragmentCountConcurrent() + indexedFragmentCountConcurrent(); }

    // V7 (TSAN): the publish/consume happens-before pair. The constructing
    // thread calls tsanPublish() after the LAST pre-publication store to the
    // spine or its fresh fragments (and before the dcas that makes the spine
    // reachable); every reader entry point calls tsanConsume() on the spine
    // it loaded from the tagged word. This imports the publisher's vector
    // clock, so TSAN sees a happens-before edge covering ALL plain
    // pre-publication initialization reached through the spine — not just
    // the individually annotated words. No-ops outside TSAN.
    ALWAYS_INLINE void tsanPublish() const { TSAN_ANNOTATE_HAPPENS_BEFORE(this); }
    ALWAYS_INLINE void tsanConsume() const { TSAN_ANNOTATE_HAPPENS_AFTER(this); }

    ALWAYS_INLINE ButterflyFragment** fragments() { return reinterpret_cast<ButterflyFragment**>(this + 1); }
    ALWAYS_INLINE ButterflyFragment* const* fragments() const { return reinterpret_cast<ButterflyFragment* const*>(this + 1); }

    ALWAYS_INLINE ButterflyFragment* outOfLineFragment(unsigned fragmentIndex) const
    {
        ASSERT(fragmentIndex < outOfLineFragmentCountConcurrent());
        return butterflyConcurrentLoad(&fragments()[fragmentIndex]);
    }

    ALWAYS_INLINE ButterflyFragment* indexedFragment(unsigned fragmentIndex) const
    {
        ASSERT(fragmentIndex < indexedFragmentCountConcurrent());
        return butterflyConcurrentLoad(&fragments()[outOfLineFragmentCountConcurrent() + fragmentIndex]);
    }

    // Precondition (I33, out-of-line clause): outOfLineIndex <
    // butterflyFragmentSlots * outOfLineFragmentCount. Out of range means the
    // caller loaded a stale spine and must acquire-re-load the tagged word and
    // re-dispatch; the nullptr-returning wrappers in ConcurrentButterfly.h
    // (segmentedOutOfLineSlotIfWithinBounds) encode that protocol.
    ALWAYS_INLINE WriteBarrierBase<Unknown>* outOfLineSlot(unsigned outOfLineIndex) const
    {
        ASSERT(static_cast<uint64_t>(outOfLineIndex) < static_cast<uint64_t>(butterflyFragmentSlots) * outOfLineFragmentCountConcurrent()); // I33
        return &outOfLineFragment(butterflyOutOfLineIndexToFragment(outOfLineIndex))->slots[butterflyOutOfLineIndexToSlot(outOfLineIndex)];
    }

    // Precondition (C4): index < this spine's vectorLength. publicLength is
    // shared by every spine the object publishes, so it can exceed THIS
    // spine's vectorLength after a racing T2 grow; callers bound by
    // min(publicLength, vectorLength) of the SAME loaded spine (I33) and
    // re-dispatch beyond it. Never resolves to fragment 0 slot 0 (the frozen
    // IndexingHeader) by construction of the +1 mapping.
    ALWAYS_INLINE WriteBarrierBase<Unknown>* indexedSlot(unsigned index) const
    {
        ASSERT(index < vectorLengthConcurrent()); // C4
        unsigned fragmentIndex = butterflyIndexedIndexToFragment(index);
        unsigned slotIndex = butterflyIndexedIndexToSlot(index);
        ASSERT(fragmentIndex || slotIndex); // Never the frozen IndexingHeader slot.
        ASSERT(fragmentIndex < indexedFragmentCountConcurrent()); // C2 sizing covers vectorLength + 1 slots.
        return &indexedFragment(fragmentIndex)->slots[slotIndex];
    }

    // Live publicLength = indexed fragment 0 slot 0, LOW half (the flat
    // IndexingHeader's publicLength byte position); the high half is the
    // frozen flat-era vectorLength (I9b). Header-less spines have no header
    // fragment, so these RELEASE_ASSERT (C2). Plain 32-bit atomicity is all
    // C4 requires (SAB-granularity staleness is legal).
    ALWAYS_INLINE uint32_t publicLength() const
    {
        RELEASE_ASSERT(indexedFragmentCountConcurrent()); // C2
        return WTF::atomicLoad(headerSlotWord(0), std::memory_order_relaxed);
    }

    ALWAYS_INLINE void setPublicLength(uint32_t value)
    {
        RELEASE_ASSERT(indexedFragmentCountConcurrent()); // C2
        WTF::atomicStore(headerSlotWord(0), value, std::memory_order_relaxed);
    }

    // Review round 2: monotone grower-side bump (see Butterfly::
    // bumpPublicLengthToAtLeast). Segmented words are shared by definition
    // (notTTLTID), so EVERY segmented dense-store length bump must use this
    // instead of setPublicLength; plain setPublicLength remains for deliberate
    // truncation (shrink drivers) and world-stopped/pre-publication writers.
    // AB17f: successful CAS is a RELEASE so the fragment-slot store before it
    // is published with the length — see Butterfly::bumpPublicLengthToAtLeast
    // for the full I21 publication rationale and the recorded reader-side
    // (ARM64 acquire) KNOWN RESIDUAL.
    ALWAYS_INLINE void bumpPublicLengthToAtLeast(uint32_t newLength)
    {
        RELEASE_ASSERT(indexedFragmentCountConcurrent()); // C2
        uint32_t* location = headerSlotWord(0);
        uint32_t current = WTF::atomicLoad(location, std::memory_order_relaxed);
        while (current < newLength) {
            uint32_t observed = WTF::atomicCompareExchangeStrong(location, current, newLength, std::memory_order_release);
            if (observed == current)
                return;
            current = observed;
        }
    }

    uint32_t frozenFlatVectorLength() const // I9b: bounds tardy flat-side readers; unused while segmented.
    {
        RELEASE_ASSERT(indexedFragmentCountConcurrent()); // C2
        return WTF::atomicLoad(headerSlotWord(1), std::memory_order_relaxed);
    }

    // Structural debug check; the slot-by-slot I8 cross-check against an
    // aliased flat butterfly is validateSpineAliasesFlatButterfly()
    // (ButterflyInlines.h).
    void validateConsistency() const
    {
        if (!indexedFragmentCountConcurrent())
            ASSERT(!vectorLengthConcurrent()); // C2: no header => no indexed storage at all.
        else {
            // C2: indexedFragmentCount = (1 + flatVectorLength + 3) / 4 at conversion
            // (+1 = the header slot) and only grows with vectorLength afterwards. The
            // last fragment may cover past 8 * vectorLength but is never dereferenced
            // there (C4 first).
            ASSERT(static_cast<uint64_t>(indexedFragmentCountConcurrent()) * butterflyFragmentSlots >= static_cast<uint64_t>(vectorLengthConcurrent()) + 1);
        }
        ASSERT(butterflyConcurrentLoad(&aliasedAllocationBase) || !butterflyConcurrentLoad(&aliasedAllocationSize)); // §4.1: both unset together.
    }

private:
    ALWAYS_INLINE uint32_t* headerSlotWord(unsigned halfIndex) const
    {
        return reinterpret_cast<uint32_t*>(indexedFragment(0)->slots) + halfIndex;
    }
};

static_assert(sizeof(ButterflySpine) == 32, "spine header is exactly four 64-bit words; fragments() follows it directly");
static_assert(alignof(ButterflySpine) == 8, "fragment pointer array needs no padding after the spine header");
// The frozen flat IndexingHeader keeps publicLength in the low half and
// vectorLength in the high half of indexed fragment 0 slot 0 (§4.1).
static_assert(!IndexingHeader::offsetOfPublicLength(), "publicLength is the low half of the frozen header slot");
static_assert(IndexingHeader::offsetOfVectorLength() == sizeof(uint32_t), "flat-era vectorLength is the high half of the frozen header slot");

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
