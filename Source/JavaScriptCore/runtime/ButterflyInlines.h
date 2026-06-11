/*
 * Copyright (C) 2012-2021 Apple Inc. All rights reserved.
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

#include "ArrayStorageInlines.h"
#include "Butterfly.h"
#include "ButterflyInlinesLight.h"
#include "ConcurrentButterfly.h"
#include "HeapCellInlines.h"
#include "JSObject.h"
#include "Structure.h"
#include "VM.h"
#include <JavaScriptCore/GCMemoryOperations.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

inline size_t NODELETE nextLength(size_t length)
{
    return length + length / 2;
}

ALWAYS_INLINE unsigned Butterfly::availableContiguousVectorLength(size_t propertyCapacity, unsigned vectorLength)
{
    size_t cellSize = totalSize(0, propertyCapacity, true, sizeof(EncodedJSValue) * vectorLength);
    cellSize = MarkedSpace::optimalSizeFor(cellSize);
    vectorLength = (cellSize - totalSize(0, propertyCapacity, true, 0)) / sizeof(EncodedJSValue);
    return vectorLength;
}

ALWAYS_INLINE unsigned Butterfly::availableContiguousVectorLength(Structure* structure, unsigned vectorLength)
{
    return availableContiguousVectorLength(structure ? structure->outOfLineCapacity() : 0, vectorLength);
}

ALWAYS_INLINE unsigned Butterfly::optimalContiguousVectorLength(size_t propertyCapacity, unsigned vectorLength)
{
    if (!vectorLength)
        vectorLength = BASE_CONTIGUOUS_VECTOR_LEN_EMPTY;
    else
        vectorLength = std::max(BASE_CONTIGUOUS_VECTOR_LEN, vectorLength);
    return availableContiguousVectorLength(propertyCapacity, vectorLength);
}

ALWAYS_INLINE unsigned Butterfly::optimalContiguousVectorLength(Structure* structure, unsigned vectorLength)
{
    return optimalContiguousVectorLength(structure ? structure->outOfLineCapacity() : 0, vectorLength);
}

inline Butterfly* Butterfly::tryCreateUninitialized(VM& vm, JSObject*, size_t preCapacity, size_t propertyCapacity, bool hasIndexingHeader, size_t indexingPayloadSizeInBytes, GCDeferralContext* deferralContext)
{
    size_t size = totalSize(preCapacity, propertyCapacity, hasIndexingHeader, indexingPayloadSizeInBytes);
    void* base = vm.auxiliarySpace().allocate(vm, size, deferralContext, AllocationFailureMode::ReturnNull);
    if (!base) [[unlikely]]
        return nullptr;

    Butterfly* result = fromBase(base, preCapacity, propertyCapacity);

    return result;
}

inline Butterfly* Butterfly::createUninitialized(VM& vm, JSObject*, size_t preCapacity, size_t propertyCapacity, bool hasIndexingHeader, size_t indexingPayloadSizeInBytes)
{
    size_t size = totalSize(preCapacity, propertyCapacity, hasIndexingHeader, indexingPayloadSizeInBytes);
    void* base = vm.auxiliarySpace().allocate(vm, size, nullptr, AllocationFailureMode::Assert);
    Butterfly* result = fromBase(base, preCapacity, propertyCapacity);

    return result;
}

inline Butterfly* Butterfly::tryCreate(VM& vm, JSObject*, size_t preCapacity, size_t propertyCapacity, bool hasIndexingHeader, const IndexingHeader& indexingHeader, size_t indexingPayloadSizeInBytes)
{
    size_t size = totalSize(preCapacity, propertyCapacity, hasIndexingHeader, indexingPayloadSizeInBytes);
    void* base = vm.auxiliarySpace().allocate(vm, size, nullptr, AllocationFailureMode::ReturnNull);
    if (!base)
        return nullptr;
    Butterfly* result = fromBase(base, preCapacity, propertyCapacity);
    if (hasIndexingHeader) {
#if USE(JSVALUE64)
        // TSAN-TRIAGE §3.15 (butterfly-words): a fresh butterfly is
        // pre-publication, but auxiliary memory is recycled and the spec's
        // stale-tolerant readers (M5/C4 re-dispatch paths) may still issue
        // relaxed loads through a stale pointer into this allocation. Make
        // the header init a single relaxed 64-bit store so that pairing is
        // defined; codegen-identical to the plain copy, flag-off unchanged.
        static_assert(sizeof(IndexingHeader) == sizeof(uint64_t));
        butterflyConcurrentStore(std::bit_cast<uint64_t*>(result->indexingHeader()), std::bit_cast<uint64_t>(indexingHeader));
#else
        *result->indexingHeader() = indexingHeader;
#endif
    }
    // Use memcpy since this butterfly is not tied to any object yet.
    memset(result->propertyStorage() - propertyCapacity, 0, propertyCapacity * sizeof(EncodedJSValue));
    return result;
}

inline Butterfly* Butterfly::create(VM& vm, JSObject* intendedOwner, size_t preCapacity, size_t propertyCapacity, bool hasIndexingHeader, const IndexingHeader& indexingHeader, size_t indexingPayloadSizeInBytes)
{
    Butterfly* result = tryCreate(vm, intendedOwner, preCapacity, propertyCapacity, hasIndexingHeader, indexingHeader, indexingPayloadSizeInBytes);

    RELEASE_ASSERT(result);
    return result;
}

inline Butterfly* Butterfly::create(VM& vm, JSObject* intendedOwner, Structure* structure)
{
    return create(
        vm, intendedOwner, 0, structure->outOfLineCapacity(),
        structure->hasIndexingHeader(intendedOwner), IndexingHeader(), 0);
}

inline void* Butterfly::base(Structure* structure)
{
    return base(indexingHeader()->preCapacity(structure), structure->outOfLineCapacity());
}

inline Butterfly* Butterfly::createOrGrowPropertyStorage(
    Butterfly* oldButterfly, VM& vm, JSObject* intendedOwner, Structure* structure, size_t oldPropertyCapacity, size_t newPropertyCapacity)
{
    RELEASE_ASSERT(newPropertyCapacity > oldPropertyCapacity);
    if (!oldButterfly)
        return create(vm, intendedOwner, 0, newPropertyCapacity, false, IndexingHeader(), 0);

    size_t preCapacity = oldButterfly->indexingHeader()->preCapacity(structure);
    size_t indexingPayloadSizeInBytes = oldButterfly->indexingHeader()->indexingPayloadSizeInBytes(structure);
    bool hasIndexingHeader = structure->hasIndexingHeader(intendedOwner);
    Butterfly* result = createUninitialized(vm, intendedOwner, preCapacity, newPropertyCapacity, hasIndexingHeader, indexingPayloadSizeInBytes);
    // The fresh butterfly is not tied to any object yet, but TSAN pairs these
    // words with stale atomics at recycled aux addresses; word-wise racy copy.
    butterflyConcurrentCopyWords(
        result->propertyStorage() - oldPropertyCapacity,
        oldButterfly->propertyStorage() - oldPropertyCapacity,
        totalSize(0, oldPropertyCapacity, hasIndexingHeader, indexingPayloadSizeInBytes));
    butterflyConcurrentZeroWords(result->propertyStorage() - newPropertyCapacity, (newPropertyCapacity - oldPropertyCapacity) * sizeof(EncodedJSValue));
    return result;
}

inline Butterfly* Butterfly::createOrGrowArrayRight(
    Butterfly* oldButterfly, VM& vm, JSObject* intendedOwner, Structure* oldStructure,
    size_t propertyCapacity, bool hadIndexingHeader, size_t oldIndexingPayloadSizeInBytes,
    size_t newIndexingPayloadSizeInBytes)
{
    if (!oldButterfly) {
        return create(
            vm, intendedOwner, 0, propertyCapacity, true, IndexingHeader(),
            newIndexingPayloadSizeInBytes);
    }
    return oldButterfly->growArrayRight(
        vm, intendedOwner, oldStructure, propertyCapacity, hadIndexingHeader,
        oldIndexingPayloadSizeInBytes, newIndexingPayloadSizeInBytes);
}

inline Butterfly* Butterfly::growArrayRight(
    VM& vm, JSObject* intendedOwner, Structure* oldStructure, size_t propertyCapacity,
    bool hadIndexingHeader, size_t oldIndexingPayloadSizeInBytes,
    size_t newIndexingPayloadSizeInBytes)
{
    ASSERT_UNUSED(oldStructure, !indexingHeader()->preCapacity(oldStructure));
    ASSERT_UNUSED(intendedOwner, hadIndexingHeader == oldStructure->hasIndexingHeader(intendedOwner));
    void* theBase = base(0, propertyCapacity);
    size_t oldSize = totalSize(0, propertyCapacity, hadIndexingHeader, oldIndexingPayloadSizeInBytes);
    size_t newSize = totalSize(0, propertyCapacity, true, newIndexingPayloadSizeInBytes);
    void* newBase = vm.auxiliarySpace().allocate(vm, newSize, nullptr, AllocationFailureMode::ReturnNull);
    if (!newBase)
        return nullptr;
    // Use memcpy since this butterfly is not tied to any object yet.
    memcpy(static_cast<JSValue*>(newBase), static_cast<JSValue*>(theBase), oldSize);
    return fromBase(newBase, 0, propertyCapacity);
}

inline Butterfly* Butterfly::growArrayRight(
    VM& vm, JSObject* intendedOwner, Structure* oldStructure,
    size_t newIndexingPayloadSizeInBytes)
{
    return growArrayRight(
        vm, intendedOwner, oldStructure, oldStructure->outOfLineCapacity(),
        oldStructure->hasIndexingHeader(intendedOwner),
        indexingHeader()->indexingPayloadSizeInBytes(oldStructure),
        newIndexingPayloadSizeInBytes);
}

inline Butterfly* Butterfly::reallocArrayRightIfPossible(
    VM& vm, GCDeferralContext& deferralContext, JSObject* intendedOwner, Structure* oldStructure, size_t propertyCapacity,
    bool hadIndexingHeader, size_t oldIndexingPayloadSizeInBytes,
    size_t newIndexingPayloadSizeInBytes)
{
    ASSERT_UNUSED(oldStructure, !indexingHeader()->preCapacity(oldStructure));
    ASSERT_UNUSED(intendedOwner, hadIndexingHeader == oldStructure->hasIndexingHeader(intendedOwner));

    void* theBase = base(0, propertyCapacity);
    size_t oldSize = totalSize(0, propertyCapacity, hadIndexingHeader, oldIndexingPayloadSizeInBytes);
    size_t newSize = totalSize(0, propertyCapacity, true, newIndexingPayloadSizeInBytes);
    ASSERT(newSize >= oldSize);

    // We can eagerly destroy butterfly backed by PreciseAllocation if (1) concurrent collector is not active and (2) the butterfly does not contain any property storage.
    // This is because during deallocation concurrent collector can access butterfly and DFG concurrent compilers accesses properties.
    // Objects with no properties are common in arrays, and we are focusing on very large array crafted by repeating Array#push, so... that's fine!
    bool canRealloc = !propertyCapacity && !vm.heap.mutatorShouldBeFenced() && std::bit_cast<HeapCell*>(theBase)->isPreciseAllocation();
    if (canRealloc) {
        void* newBase = vm.auxiliarySpace().reallocatePreciseAllocationNonVirtual(vm, std::bit_cast<HeapCell*>(theBase), newSize, &deferralContext, AllocationFailureMode::ReturnNull);
        if (!newBase)
            return nullptr;
        return fromBase(newBase, 0, propertyCapacity);
    }

    void* newBase = vm.auxiliarySpace().allocate(vm, newSize, &deferralContext, AllocationFailureMode::ReturnNull);
    if (!newBase)
        return nullptr;
    // Use memcpy since this butterfly is not tied to any object yet.
    memcpy(static_cast<JSValue*>(newBase), static_cast<JSValue*>(theBase), oldSize);
    return fromBase(newBase, 0, propertyCapacity);
}

inline Butterfly* Butterfly::resizeArray(
    VM& vm, JSObject* intendedOwner, size_t propertyCapacity, bool oldHasIndexingHeader,
    size_t oldIndexingPayloadSizeInBytes, size_t newPreCapacity, bool newHasIndexingHeader,
    size_t newIndexingPayloadSizeInBytes)
{
    Butterfly* result = createUninitialized(vm, intendedOwner, newPreCapacity, propertyCapacity, newHasIndexingHeader, newIndexingPayloadSizeInBytes);
    // FIXME: This could be made much more efficient if we used the property size,
    // not the capacity.
    void* to = result->propertyStorage() - propertyCapacity;
    void* from = propertyStorage() - propertyCapacity;
    size_t size = std::min(
        totalSize(0, propertyCapacity, oldHasIndexingHeader, oldIndexingPayloadSizeInBytes),
        totalSize(0, propertyCapacity, newHasIndexingHeader, newIndexingPayloadSizeInBytes));
    // Use memcpy since this butterfly is not tied to any object yet.
    memcpy(static_cast<JSValue*>(to), static_cast<JSValue*>(from), size);
    return result;
}

inline Butterfly* Butterfly::resizeArray(
    VM& vm, JSObject* intendedOwner, Structure* structure, size_t newPreCapacity,
    size_t newIndexingPayloadSizeInBytes)
{
    bool hasIndexingHeader = structure->hasIndexingHeader(intendedOwner);
    return resizeArray(
        vm, intendedOwner, structure->outOfLineCapacity(), hasIndexingHeader,
        indexingHeader()->indexingPayloadSizeInBytes(structure), newPreCapacity,
        hasIndexingHeader, newIndexingPayloadSizeInBytes);
}

inline Butterfly* Butterfly::unshift(Structure* structure, size_t numberOfSlots)
{
    ASSERT(hasAnyArrayStorage(structure->indexingType()));
    ASSERT(numberOfSlots <= indexingHeader()->preCapacity(structure));
    unsigned propertyCapacity = structure->outOfLineCapacity();
    // FIXME: It would probably be wise to rewrite this as a loop since (1) we know in which
    // direction we're moving memory so we don't need the extra check of memmove and (2) we're
    // moving a small amount of memory in the common case so the throughput of memmove won't
    // amortize the overhead of calling it. And no, we cannot rely on the C++ compiler to
    // inline memmove (particularly since the size argument is likely to be variable), nor can
    // we rely on the compiler to recognize the ordering of the pointer arguments (since
    // propertyCapacity is variable and could cause wrap-around as far as the compiler knows).
    gcSafeMemmove(
        propertyStorage() - numberOfSlots - propertyCapacity,
        propertyStorage() - propertyCapacity,
        sizeof(EncodedJSValue) * propertyCapacity + sizeof(IndexingHeader) + ArrayStorage::sizeFor(0));
    return IndexingHeader::fromEndOf(propertyStorage() - numberOfSlots)->butterfly();
}

inline Butterfly* Butterfly::shift(Structure* structure, size_t numberOfSlots)
{
    ASSERT(hasAnyArrayStorage(structure->indexingType()));
    unsigned propertyCapacity = structure->outOfLineCapacity();
    // FIXME: See comment in unshift(), above.
    gcSafeMemmove(
        propertyStorage() - propertyCapacity + numberOfSlots,
        propertyStorage() - propertyCapacity,
        sizeof(EncodedJSValue) * propertyCapacity + sizeof(IndexingHeader) + ArrayStorage::sizeFor(0));
    return IndexingHeader::fromEndOf(propertyStorage() + numberOfSlots)->butterfly();
}

ALWAYS_INLINE void Butterfly::clearRange(IndexingType indexingType, Butterfly* butterfly, unsigned start, unsigned end)
{
    ASSERT(end >= start);

    if (size_t remaining = end - start) {
        // 32-bit, non-Darwin builds don't use `remaining`.
        UNUSED_VARIABLE(remaining);
        if (hasDouble(indexingType)) {
#if OS(DARWIN)
            constexpr double pattern = PNaN;
            memset_pattern8(static_cast<void*>(butterfly->contiguous().data() + start), &pattern, sizeof(double) * remaining);
#else
            for (unsigned i = start; i < end; ++i)
                butterfly->contiguousDouble().atUnsafe(i) = PNaN;
#endif
        } else {
#if USE(JSVALUE64)
            memset(static_cast<void*>(butterfly->contiguous().data() + start), 0, sizeof(JSValue) * remaining);
#else
            for (unsigned i = start; i < end; ++i)
                butterfly->contiguous().atUnsafe(i).clear();
#endif
        }
    }
}

// ===== SPEC-objectmodel §4.1/§4.2: flat -> segmented aliasing equations =====
//
// Flat->segmented conversion (§4.2) is zero-copy: the new spine's fragment
// pointers are computed from the existing flat butterfly B with the equations
// below, so every pre-existing slot keeps its flat address (I8). These
// helpers are consumed by convertToSegmentedButterfly (ConcurrentButterfly.cpp,
// objectmodel Task 5); the §9.3 exported accessors (segmentedOutOfLineSlot &
// friends) are one-line wrappers over the ButterflySpine members in
// Butterfly.h. Nothing here runs with useJSThreads off (I22).

// Out-of-line fragment j covers out-of-line indices 4j..4j+3; its base is
// B - 40 - 32j (slots ascend in memory, flat out-of-line slots descend).
ALWAYS_INLINE ButterflyFragment* aliasedOutOfLineFragmentForConversion(Butterfly* flat, unsigned fragmentIndex)
{
    return reinterpret_cast<ButterflyFragment*>(flat->pointer() - 40 - butterflyFragmentBytes * static_cast<size_t>(fragmentIndex));
}

// Indexed fragment f base is B - 8 + 32f; fragment 0 slot 0 aliases the flat
// IndexingHeader [B - 8, B), frozen (§4.1).
ALWAYS_INLINE ButterflyFragment* aliasedIndexedFragmentForConversion(Butterfly* flat, unsigned fragmentIndex)
{
    return reinterpret_cast<ButterflyFragment*>(flat->pointer() - 8 + butterflyFragmentBytes * static_cast<size_t>(fragmentIndex));
}

// C1: out-of-line capacity is a multiple of 4 at conversion (GT#4: initial
// capacity 4, growth x2), so flat out-of-line storage splits into whole
// 32-byte fragments. RELEASE_ASSERT per spec §4.1.
ALWAYS_INLINE uint32_t aliasedOutOfLineFragmentCountForConversion(size_t outOfLineCapacity)
{
    RELEASE_ASSERT(!(outOfLineCapacity % butterflyFragmentSlots)); // C1
    return static_cast<uint32_t>(outOfLineCapacity / butterflyFragmentSlots);
}

// C2: indexed fragments only if the flat butterfly HAS an IndexingHeader
// (Butterfly::totalSize); header-less => 0, no header fragment. Else
// (1 + flatVectorLength + 3) / 4 - the +1 is the header slot. The last
// fragment may cover past B + 8 * vectorLength; it is never dereferenced
// there (C4 bounds first).
ALWAYS_INLINE uint32_t aliasedIndexedFragmentCountForConversion(bool hasIndexingHeader, uint32_t flatVectorLength)
{
    if (!hasIndexingHeader)
        return 0;
    return static_cast<uint32_t>((1 + static_cast<size_t>(flatVectorLength) + (butterflyFragmentSlots - 1)) / butterflyFragmentSlots);
}

// C3: preCapacity != 0 only for ArrayStorage shapes, which never segment
// (I31), so conversions always see preCapacity == 0 (RELEASE_ASSERT; a
// violation is a logic error, §4.2 step 3). The aliased allocation base is
// B - 8 * (propertyCapacity + 1) in BOTH header cases - for header-less
// butterflies B points one (unallocated) slot past the allocation's end, so
// the base equation is unchanged and the size below excludes that slot.
ALWAYS_INLINE void* aliasedAllocationBaseForConversion(Butterfly* flat, size_t preCapacity, size_t propertyCapacity)
{
    RELEASE_ASSERT(!preCapacity); // C3
    return flat->pointer() - sizeof(EncodedJSValue) * (propertyCapacity + 1);
}

// The recorded size covers exactly the flat allocation (GC re-marks
// [base, base + size) every visit - §4.5/I7): header-less it is
// 8 * propertyCapacity (C3), with a header it additionally covers the header
// slot and the indexing payload. Butterfly::totalSize computes both cases.
ALWAYS_INLINE uint64_t aliasedAllocationSizeForConversion(size_t propertyCapacity, bool hasIndexingHeader, size_t indexingPayloadSizeInBytes)
{
    ASSERT(!indexingPayloadSizeInBytes || hasIndexingHeader);
    return Butterfly::totalSize(0, propertyCapacity, hasIndexingHeader, indexingPayloadSizeInBytes);
}

// I8 debug check: called by §4.2 conversion (under the cell lock, after
// step-3 re-validation, before step-5 publication) on a freshly built spine
// whose every fragment aliases `flat`. Verifies slot by slot that the §4.1
// equations reproduce the flat addresses (out-of-line k at B - 16 - 8k;
// element i at B + 8i; header at B - 8). C1/C3 are RELEASE_ASSERTed
// unconditionally; the per-slot sweep runs when asserts are enabled or
// Options::verifyConcurrentButterfly() is set.
inline void validateSpineAliasesFlatButterfly(const ButterflySpine* spine, Butterfly* flat, size_t preCapacity, size_t outOfLineCapacity, bool hasIndexingHeader)
{
    RELEASE_ASSERT(!(outOfLineCapacity % butterflyFragmentSlots)); // C1
    RELEASE_ASSERT(!preCapacity); // C3
    if (!ASSERT_ENABLED && !verifyConcurrentButterflyEnabled())
        return;

    spine->validateConsistency();
    RELEASE_ASSERT(spine->outOfLineFragmentCount == aliasedOutOfLineFragmentCountForConversion(outOfLineCapacity));
    uint32_t flatVectorLength = hasIndexingHeader ? flat->vectorLength() : 0;
    RELEASE_ASSERT(spine->indexedFragmentCount == aliasedIndexedFragmentCountForConversion(hasIndexingHeader, flatVectorLength));
    RELEASE_ASSERT(spine->vectorLength == flatVectorLength); // Conversion-time live VL == flat VL (§4.2 step 3 re-reads it under the lock).
    RELEASE_ASSERT(spine->aliasedAllocationBase == aliasedAllocationBaseForConversion(flat, preCapacity, outOfLineCapacity));

    char* base = flat->pointer();

    // I8: out-of-line index k lives at B - 16 - 8k.
    for (size_t k = 0; k < outOfLineCapacity; ++k)
        RELEASE_ASSERT(reinterpret_cast<char*>(spine->outOfLineSlot(static_cast<unsigned>(k))) == base - 16 - sizeof(EncodedJSValue) * k);

    if (hasIndexingHeader) {
        // I8: the frozen IndexingHeader is indexed fragment 0 slot 0, [B - 8, B).
        // C4 holds by ADDRESS identity (next line + the Butterfly.h half-layout
        // static_asserts); no value compare — the live publicLength may be
        // advanced concurrently by lock-free in-bounds growers
        // (Butterfly::bumpPublicLengthToAtLeast) even under the cell lock, so
        // two reads of it need not agree (TOCTOU, not a race signal). The old
        // compare was also a TSAN-visible data race: flat->publicLength() is a
        // plain (non-atomic) load racing the grower's CAS. The retained I9b
        // compare is a same-address double read too (high half of B - 8) but
        // is sound: flat vectorLength is immutable flag-on and the grower CAS
        // is a 32-bit low-half-only RMW that cannot tear the high half.
        RELEASE_ASSERT(reinterpret_cast<char*>(&spine->indexedFragment(0)->slots[0]) == base - 8);
        RELEASE_ASSERT(spine->frozenFlatVectorLength() == flatVectorLength); // I9b: high half frozen.

        // I8: element i lives at B + 8i.
        for (uint32_t i = 0; i < flatVectorLength; ++i)
            RELEASE_ASSERT(reinterpret_cast<char*>(spine->indexedSlot(i)) == base + sizeof(EncodedJSValue) * static_cast<size_t>(i));
    }
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
